// Renders a mono float WAV through the engine in several settings and reports how
// in-tune the result is (share of voiced frames within 10 cents of a semitone).
// Build: clang++ -std=c++17 -O2 -ISource tools/render.cpp -o render
// Usage: render in.wav out_prefix

#include "TuneEngine.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using namespace tune;

static bool readWav (const std::string& path, std::vector<float>& x, double& sr)
{
    std::ifstream f (path, std::ios::binary);
    char riff[12];
    if (! f.read (riff, 12) || std::memcmp (riff, "RIFF", 4) != 0)
        return false;
    uint16_t fmt = 0, chans = 0, bits = 0;
    uint32_t rate = 0;
    char id[4];
    uint32_t size = 0;
    while (f.read (id, 4) && f.read ((char*) &size, 4))
    {
        if (std::memcmp (id, "fmt ", 4) == 0)
        {
            std::vector<char> b (size);
            f.read (b.data(), size);
            std::memcpy (&fmt, b.data(), 2);
            std::memcpy (&chans, b.data() + 2, 2);
            std::memcpy (&rate, b.data() + 4, 4);
            std::memcpy (&bits, b.data() + 14, 2);
        }
        else if (std::memcmp (id, "data", 4) == 0)
        {
            if (bits != 32 || chans != 1)
                return false;
            x.resize (size / 4);
            f.read ((char*) x.data(), size);
            sr = rate;
            return true;
        }
        else
        {
            f.seekg (size + (size & 1), std::ios::cur);
        }
    }
    return false;
}

static void writeWav (const std::string& path, const std::vector<float>& x, double sr)
{
    std::ofstream f (path, std::ios::binary);
    const uint32_t dataBytes = (uint32_t) (x.size() * 4), rate = (uint32_t) sr, byteRate = rate * 4;
    const uint32_t riffSize = 36 + dataBytes, fmtSize = 16;
    const uint16_t fmt = 3, chans = 1, align = 4, bits = 32;
    f.write ("RIFF", 4); f.write ((const char*) &riffSize, 4); f.write ("WAVE", 4);
    f.write ("fmt ", 4); f.write ((const char*) &fmtSize, 4); f.write ((const char*) &fmt, 2);
    f.write ((const char*) &chans, 2); f.write ((const char*) &rate, 4); f.write ((const char*) &byteRate, 4);
    f.write ((const char*) &align, 2); f.write ((const char*) &bits, 2);
    f.write ("data", 4); f.write ((const char*) &dataBytes, 4);
    f.write ((const char*) x.data(), dataBytes);
}

struct Stats { double inTune = 0, medianDev = 0; int frames = 0; };

static Stats tuneStats (const std::vector<float>& y, double sr)
{
    PitchDetector d;
    const int wi = nextPow2 ((int) (sr / 60.0) + 2);
    d.prepare (wi);
    std::vector<double> devs;
    for (size_t s = 0; s + 2 * (size_t) wi < y.size(); s += 256)
    {
        double e = 0;
        for (int i = 0; i < 2 * wi; ++i) e += (double) y[s + (size_t) i] * y[s + (size_t) i];
        if (std::sqrt (e / (2 * wi)) < 0.01)
            continue;
        const float p = d.detect (y.data() + s, wi, (int) (sr / 900), (int) (sr / 65), 0.12f);
        if (p <= 0)
            continue;
        const double note = 69 + 12 * std::log2 ((sr / p) / 440.0);
        devs.push_back (std::abs (note - std::round (note)) * 100.0);
    }
    Stats st;
    st.frames = (int) devs.size();
    if (devs.empty())
        return st;
    int good = 0;
    for (double v : devs) good += v <= 10.0;
    st.inTune = 100.0 * good / (double) devs.size();
    std::sort (devs.begin(), devs.end());
    st.medianDev = devs[devs.size() / 2];
    return st;
}

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: render in.wav out_prefix\n");
        return 2;
    }
    std::vector<float> x;
    double sr = 0;
    if (! readWav (argv[1], x, sr))
    {
        std::fprintf (stderr, "need a mono 32-bit float WAV\n");
        return 1;
    }
    const std::string prefix = argv[2];

    struct Setting { const char* name; Params p; };
    Params hard; hard.retuneMs = 0;
    Params natural; natural.retuneMs = 45; natural.humanize = 0.4f; natural.expression = 0.3f;
    Params liveHard = hard; liveHard.liveMode = true;
    Params liveNatural = natural; liveNatural.liveMode = true;
    Params deep = hard; deep.transpose = -5; deep.throat = 1.15f;
    Params liveDeep = deep; liveDeep.liveMode = true;
    const Setting settings[] = { { "studio_hard", hard }, { "studio_natural", natural },
                                 { "live_hard", liveHard }, { "live_natural", liveNatural },
                                 { "studio_fourth_down", deep }, { "live_fourth_down", liveDeep } };

    const auto in = tuneStats (x, sr);
    std::printf ("%-20s in-tune %5.1f%%  median dev %4.1f c  (%d frames)\n", "input", in.inTune, in.medianDev, in.frames);

    for (const auto& s : settings)
    {
        Engine e;
        e.prepare (sr, 1);
        e.setParams (s.p);
        std::vector<float> y = x;
        y.resize (x.size() + (size_t) e.getLatencySamples(), 0.0f);
        for (size_t pos = 0; pos < y.size(); pos += 512)
        {
            float* ch[1] = { y.data() + pos };
            e.process (ch, 1, (int) std::min<size_t> (512, y.size() - pos));
        }
        const int lat = e.getLatencySamples (s.p.liveMode);
        y.erase (y.begin(), y.begin() + lat);
        float peak = 0;
        bool finite = true;
        for (float v : y) { finite &= std::isfinite (v); peak = std::max (peak, std::abs (v)); }
        const auto st = tuneStats (y, sr);
        std::printf ("%-20s in-tune %5.1f%%  median dev %4.1f c  peak %.2f%s\n", s.name, st.inTune, st.medianDev, peak,
                     finite ? "" : "  NON-FINITE!");
        writeWav (prefix + "_" + s.name + ".wav", y, sr);
    }
    return 0;
}
