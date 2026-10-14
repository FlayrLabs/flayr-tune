// A small coverage-guided mutation fuzzer for toolchains without libFuzzer (Apple clang).
// It drives the same LLVMFuzzerTestOneInput entry points and understands a subset of
// libFuzzer's command line, so the two are interchangeable:
//
//   fuzz_state [-runs=N] [-max_total_time=S] [-seed=N] [-max_len=N] [-timeout=S]
//              [-dict=FILE] [-artifact_prefix=DIR/] [-print_every=S] CORPUS_DIR [SEED_DIR...]
//   fuzz_state crash-file ...          # re-run inputs once (reproduce a crash)
//
// Coverage comes from -fsanitize-coverage=inline-8bit-counters: an input that reaches an
// edge, or an edge hit-count bucket, no earlier input reached is kept and written to CORPUS_DIR. Crashes, sanitizer reports,
// invariant failures (abort) and timeouts save the input as <prefix>crash-<hash> or
// <prefix>timeout-<hash>, exactly like libFuzzer. Runs are deterministic for a given seed.

#include <sanitizer/common_interface_defs.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size);

//==============================================================================
// Coverage: -fsanitize-coverage=inline-8bit-counters gives every edge a byte counter that
// the instrumented code bumps inline (no callback, so the DSP loops stay fast). After each
// run the counters are folded into AFL-style hit-count buckets; an input that lights a
// bucket no earlier input lit is new coverage. Then the counters are cleared.
struct CounterRange { uint8_t* start; uint8_t* stop; size_t offset; };
static std::vector<CounterRange> counterRanges;
static std::vector<uint8_t> seenBuckets;
static size_t totalEdges = 0, newEdges = 0;

extern "C" void __sanitizer_cov_8bit_counters_init (uint8_t* start, uint8_t* stop)
{
    if (start == stop)
        return;
    for (const auto& r : counterRanges)
        if (r.start == start)
            return;
    counterRanges.push_back ({ start, stop, totalEdges });
    totalEdges += (size_t) (stop - start);
    seenBuckets.resize (totalEdges, 0);
}

static uint8_t bucket (uint8_t c)
{
    return c >= 128 ? 128 : c >= 32 ? 64 : c >= 16 ? 32 : c >= 8 ? 16 : c >= 4 ? 8 : c == 3 ? 4 : c == 2 ? 2 : 1;
}

// Returns the number of (edge, bucket) pairs never seen before, and clears the counters.
static size_t collectCoverage()
{
    size_t fresh = 0;
    for (const auto& r : counterRanges)
    {
        const size_t n = (size_t) (r.stop - r.start);
        size_t i = 0;
        for (; i < n; ++i)
        {
            if ((i & 7) == 0 && i + 8 <= n)
            {
                uint64_t word;
                std::memcpy (&word, r.start + i, 8);
                if (word == 0) { i += 7; continue; }
            }
            if (const uint8_t c = r.start[i])
            {
                const uint8_t b = bucket (c);
                auto& seen = seenBuckets[r.offset + i];
                if ((seen & b) == 0)
                {
                    if (seen == 0)
                        ++newEdges;
                    seen |= b;
                    ++fresh;
                }
                r.start[i] = 0;
            }
        }
    }
    return fresh;
}

//==============================================================================
using Bytes = std::vector<uint8_t>;

static const Bytes* current = nullptr; // the input being run, for the crash handlers
static std::string artifactPrefix;
static char crashPath[4096], timeoutPath[4096];

static uint64_t fnv (const Bytes& b)
{
    uint64_t h = 1469598103934665603ull;
    for (auto c : b)
        h = (h ^ c) * 1099511628211ull;
    return h;
}

static std::string hex (uint64_t v)
{
    char s[17];
    std::snprintf (s, sizeof (s), "%016llx", (unsigned long long) v);
    return s;
}

static void writeFile (const std::string& path, const Bytes& b)
{
    std::ofstream f (path, std::ios::binary);
    f.write ((const char*) b.data(), (std::streamsize) b.size());
}

static bool readFile (const std::string& path, Bytes& out)
{
    std::ifstream f (path, std::ios::binary);
    if (! f)
        return false;
    out.assign (std::istreambuf_iterator<char> (f), {});
    return true;
}

// Paths are prepared before each run: the handlers only call open/write/_exit.
static void prepareArtifactPaths (const Bytes& b)
{
    const auto h = hex (fnv (b));
    std::snprintf (crashPath, sizeof (crashPath), "%scrash-%s", artifactPrefix.c_str(), h.c_str());
    std::snprintf (timeoutPath, sizeof (timeoutPath), "%stimeout-%s", artifactPrefix.c_str(), h.c_str());
}

static void dumpCurrent (const char* path)
{
    if (current == nullptr)
        return;
    const int fd = open (path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
    {
        const ssize_t w = write (fd, current->data(), current->size());
        (void) w;
        close (fd);
    }
    const char msg[] = "\n==FUZZ== input saved to ";
    ssize_t w = write (2, msg, sizeof (msg) - 1);
    w = write (2, path, strlen (path));
    w = write (2, "\n", 1);
    (void) w;
}

static void onDeath() { dumpCurrent (crashPath); }

static void onSignal (int sig)
{
    if (sig == SIGALRM)
    {
        const char msg[] = "\n==FUZZ== timeout\n";
        ssize_t w = write (2, msg, sizeof (msg) - 1);
        (void) w;
        dumpCurrent (timeoutPath);
        _exit (70);
    }
    dumpCurrent (crashPath);
    signal (sig, SIG_DFL);
    raise (sig);
}

static unsigned timeoutSec = 20;

static size_t runOne (const Bytes& b)
{
    current = &b;
    prepareArtifactPaths (b);
    collectCoverage(); // drop whatever ran between inputs
    alarm (timeoutSec);
    LLVMFuzzerTestOneInput (b.data(), b.size());
    alarm (0);
    current = nullptr;
    return collectCoverage();
}

//==============================================================================
// libFuzzer dictionary format: one "token" per line, with \\, \" and \xNN escapes.
static std::vector<Bytes> loadDictionary (const std::string& path)
{
    std::vector<Bytes> dict;
    std::ifstream f (path);
    std::string line;
    while (std::getline (f, line))
    {
        const auto q = line.find ('"');
        const auto e = line.rfind ('"');
        if (line.empty() || line[0] == '#' || q == std::string::npos || e <= q)
            continue;
        Bytes tok;
        for (size_t i = q + 1; i < e; ++i)
        {
            if (line[i] == '\\' && i + 1 < e)
            {
                if (line[i + 1] == 'x' && i + 3 < e)
                {
                    tok.push_back ((uint8_t) std::stoi (line.substr (i + 2, 2), nullptr, 16));
                    i += 3;
                }
                else
                    tok.push_back ((uint8_t) line[++i]);
            }
            else
                tok.push_back ((uint8_t) line[i]);
        }
        if (! tok.empty())
            dict.push_back (tok);
    }
    return dict;
}

struct Mutator
{
    std::mt19937_64 rng;
    std::vector<Bytes> dict;
    size_t maxLen = 65536;

    size_t below (size_t n) { return n == 0 ? 0 : (size_t) (rng() % n); }

    void mutate (Bytes& b, const std::vector<Bytes>& corpus)
    {
        const int rounds = 1 + (int) below (8);
        for (int r = 0; r < rounds; ++r)
            once (b, corpus);
        if (b.size() > maxLen)
            b.resize (maxLen);
    }

    void once (Bytes& b, const std::vector<Bytes>& corpus)
    {
        static const uint8_t interesting8[] = { 0, 1, 0x7f, 0x80, 0xff, '<', '>', '/', '"', '=', '!', '?', '&',
                                                ';', ' ', '0', '9', '-', '.', 'e' };
        static const uint64_t interesting64[] = { 0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff,
                                                  0x80000000, 0xffffffff, 0x21324356, 360000, 359999, 360001,
                                                  0x7ff8000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
                                                  0x7fc00000, 0x7f800000, 0xff800000, 0x8000000000000000ull };
        static const char* numbers[] = { "nan", "-nan", "inf", "-inf", "1e300", "-1e300", "1e39", "3.4e38", "-0",
                                         "0", "1", "100", "127", "128", "-1", "0x7fffffff", "4294967296",
                                         "2147483647", "-2147483648", "1e-45", "9999999999999999999" };
        const auto pos = [&] { return below (b.size() + 1); };

        switch (below (14))
        {
            case 0: if (! b.empty()) b[below (b.size())] ^= (uint8_t) (1u << below (8)); break;
            case 1: if (! b.empty()) b[below (b.size())] = interesting8[below (sizeof (interesting8))]; break;
            case 2: if (! b.empty()) b[below (b.size())] = (uint8_t) rng(); break;
            case 3: { const auto at = pos(); const auto n = 1 + below (8); for (size_t i = 0; i < n; ++i) b.insert (b.begin() + (long) at, (uint8_t) rng()); break; }
            case 4: if (b.size() > 1) { const auto at = below (b.size()); const auto n = 1 + below (std::min<size_t> (b.size() - at, 64)); b.erase (b.begin() + (long) at, b.begin() + (long) (at + n)); } break;
            case 5: case 6: if (! dict.empty()) { const auto& t = dict[below (dict.size())]; const auto at = pos(); if (below (2) || b.size() < t.size()) b.insert (b.begin() + (long) at, t.begin(), t.end()); else std::copy (t.begin(), t.end(), b.begin() + (long) std::min (at, b.size() - t.size())); } break;
            case 7: if (! corpus.empty()) { const auto& o = corpus[below (corpus.size())]; if (! o.empty()) { const auto from = below (o.size()); const auto n = 1 + below (std::min<size_t> (o.size() - from, 256)); b.insert (b.begin() + (long) pos(), o.begin() + (long) from, o.begin() + (long) (from + n)); } } break;
            case 8: if (! corpus.empty() && ! b.empty()) { const auto& o = corpus[below (corpus.size())]; const auto cut = below (b.size()); b.resize (cut); if (! o.empty()) { const auto from = below (o.size()); b.insert (b.end(), o.begin() + (long) from, o.end()); } } break;
            case 9: if (b.size() >= 4) { const auto w = (size_t) 1 << below (4); if (b.size() >= w) { const auto at = below (b.size() - w + 1); uint64_t v = 0; std::memcpy (&v, &b[at], w); v += (uint64_t) ((int64_t) below (71) - 35); std::memcpy (&b[at], &v, w); } } break;
            case 10: { const auto w = (size_t) 1 << (1 + below (3)); const uint64_t v = interesting64[below (sizeof (interesting64) / 8)]; if (b.size() >= w && below (2)) std::memcpy (&b[below (b.size() - w + 1)], &v, w); else { const auto at = pos(); b.insert (b.begin() + (long) at, (const uint8_t*) &v, (const uint8_t*) &v + w); } break; }
            case 11: if (! b.empty()) { const auto from = below (b.size()); const auto n = 1 + below (std::min<size_t> (b.size() - from, 32)); const Bytes chunk (b.begin() + (long) from, b.begin() + (long) (from + n)); const auto times = 1 + below (below (2) ? 8 : 512); const auto at = pos(); for (size_t i = 0; i < times && b.size() + chunk.size() <= maxLen; ++i) b.insert (b.begin() + (long) at, chunk.begin(), chunk.end()); } break;
            case 12: { // replace a run of digits with an interesting number
                if (b.empty()) break;
                size_t at = below (b.size()), tries = 0;
                while (tries++ < b.size() && ! (b[at] >= '0' && b[at] <= '9')) at = (at + 1) % b.size();
                if (! (b[at] >= '0' && b[at] <= '9')) break;
                size_t end = at;
                while (end < b.size() && ((b[end] >= '0' && b[end] <= '9') || b[end] == '.' || b[end] == '-')) ++end;
                const std::string s = numbers[below (sizeof (numbers) / sizeof (*numbers))];
                b.erase (b.begin() + (long) at, b.begin() + (long) end);
                b.insert (b.begin() + (long) at, s.begin(), s.end());
                break;
            }
            default: if (b.size() > 2) { const auto a = below (b.size()), c = below (b.size()); std::swap (b[a], b[c]); } break;
        }
    }
};

static std::vector<std::string> listFiles (const std::string& dir)
{
    std::vector<std::string> out;
    if (auto* d = opendir (dir.c_str()))
    {
        while (auto* e = readdir (d))
            if (e->d_name[0] != '.')
                out.push_back (dir + "/" + e->d_name);
        closedir (d);
    }
    std::sort (out.begin(), out.end()); // deterministic order
    return out;
}

static bool isDir (const std::string& p)
{
    struct stat st {};
    return stat (p.c_str(), &st) == 0 && S_ISDIR (st.st_mode);
}

int main (int argc, char** argv)
{
    long long runs = -1;
    double maxTime = 0, printEvery = 10;
    uint64_t seed = 1;
    Mutator m;
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto val = [&a] { return a.substr (a.find ('=') + 1); };
        if (a.rfind ("-runs=", 0) == 0) runs = std::stoll (val());
        else if (a.rfind ("-max_total_time=", 0) == 0) maxTime = std::stod (val());
        else if (a.rfind ("-seed=", 0) == 0) seed = std::stoull (val());
        else if (a.rfind ("-max_len=", 0) == 0) m.maxLen = std::stoull (val());
        else if (a.rfind ("-timeout=", 0) == 0) timeoutSec = (unsigned) std::stoul (val());
        else if (a.rfind ("-dict=", 0) == 0) { auto d = loadDictionary (val()); m.dict.insert (m.dict.end(), d.begin(), d.end()); }
        else if (a.rfind ("-artifact_prefix=", 0) == 0) artifactPrefix = val();
        else if (a.rfind ("-print_every=", 0) == 0) printEvery = std::stod (val());
        else if (a[0] == '-') { std::fprintf (stderr, "unknown option %s\n", a.c_str()); return 2; }
        else paths.push_back (a);
    }

    __sanitizer_set_death_callback (onDeath);
    for (int sig : { SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGALRM })
        signal (sig, onSignal);

    // Files on the command line: reproduce mode.
    if (! paths.empty() && ! isDir (paths[0]))
    {
        for (const auto& p : paths)
        {
            Bytes b;
            if (! readFile (p, b)) { std::fprintf (stderr, "cannot read %s\n", p.c_str()); return 2; }
            std::fprintf (stderr, "Running %s (%zu bytes)\n", p.c_str(), b.size());
            runOne (b);
        }
        std::fprintf (stderr, "Executed %zu inputs, no crash\n", paths.size());
        return 0;
    }

    m.rng.seed (seed);
    const std::string outDir = paths.empty() ? std::string() : paths[0];
    std::vector<Bytes> corpus;
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&start] { return std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count(); };

    // Every seed runs once, whatever its size; the ones that add coverage stay.
    size_t seeds = 0;
    for (const auto& dir : paths)
        for (const auto& f : listFiles (dir))
        {
            Bytes b;
            if (! readFile (f, b))
                continue;
            ++seeds;
            if (runOne (b) > 0 || corpus.empty())
            {
                if (b.size() > m.maxLen)
                    b.resize (m.maxLen);
                corpus.push_back (std::move (b));
            }
        }
    if (corpus.empty())
        corpus.push_back ({});
    std::fprintf (stderr, "#seeds %zu kept %zu, edges %zu/%zu, %.1f s\n", seeds, corpus.size(), newEdges, totalEdges, elapsed());

    long long execs = 0;
    double nextPrint = printEvery;
    Bytes b;
    while ((runs < 0 || execs < runs) && (maxTime <= 0 || elapsed() < maxTime))
    {
        // Favour recent finds: they sit at the frontier of coverage.
        const size_t n = corpus.size();
        const size_t pick = m.below (2) ? n - 1 - m.below (std::min<size_t> (n, 16)) : m.below (n);
        b = corpus[pick];
        m.mutate (b, corpus);
        const auto fresh = runOne (b);
        ++execs;
        if (fresh > 0)
        {
            if (! outDir.empty())
                writeFile (outDir + "/" + hex (fnv (b)), b);
            corpus.push_back (b);
        }
        if (elapsed() >= nextPrint)
        {
            std::fprintf (stderr, "#%lld  %.0f s  %.0f exec/s  corpus %zu  edges %zu/%zu\n", execs, elapsed(),
                          (double) execs / elapsed(), corpus.size(), newEdges, totalEdges);
            nextPrint += printEvery;
        }
    }
    std::fprintf (stderr, "Done: %lld runs in %.0f s (%.0f exec/s), corpus %zu, edges %zu/%zu, no crash\n", execs,
                  elapsed(), (double) execs / std::max (elapsed(), 1e-9), corpus.size(), newEdges, totalEdges);
    return 0;
}
