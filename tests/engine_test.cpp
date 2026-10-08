// Offline checks for the tuning engine: feed synthetic voices, measure the output pitch.
// Build: clang++ -std=c++17 -O2 -I../Source engine_test.cpp -o engine_test

#include "TuneEngine.h"

#include <cstdio>
#include <functional>
#include <string>

using namespace tune;

static int failures = 0;

static void check (bool ok, const std::string& what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (! ok)
        ++failures;
}

// Harmonic-rich "voice" with a pitch curve in Hz over time.
static std::vector<float> voice (double sr, double seconds, std::function<double (double)> f0)
{
    std::vector<float> x ((size_t) (sr * seconds));
    double phase = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) i / sr;
        phase += 2.0 * kPi * f0 (t) / sr;
        double v = 0.0;
        for (int k = 1; k <= 12; ++k)
            v += std::sin (k * phase) / k * (k == 3 ? 1.4 : 1.0);
        x[i] = (float) (0.25 * v);
    }
    return x;
}

// A vowel-like voice: harmonics shaped by fixed formants (700 / 1200 / 2600 Hz),
// so the spectral envelope stays put whatever the pitch.
static std::vector<float> vowel (double sr, double seconds, std::function<double (double)> f0)
{
    auto env = [] (double f)
    {
        auto g = [f] (double c, double bw) { return std::exp (-0.5 * std::pow ((f - c) / bw, 2)); };
        return g (700, 130) + 0.6 * g (1200, 150) + 0.3 * g (2600, 250) + 0.02;
    };
    std::vector<float> x ((size_t) (sr * seconds));
    double phase = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double hz = f0 ((double) i / sr);
        phase += 2.0 * kPi * hz / sr;
        double v = 0.0;
        for (int k = 1; k * hz < 6000.0; ++k)
            v += env (k * hz) * std::sin (k * phase);
        x[i] = (float) (0.2 * v);
    }
    return x;
}

// Spectral centroid of 150..4000 Hz averaged over frames: tracks where the formants sit.
static double centroid (const std::vector<float>& y, double sr, double fromSec)
{
    const int n = 4096;
    Fft fft;
    fft.init (n);
    std::vector<std::complex<float>> buf ((size_t) n);
    double num = 0, den = 0;
    for (size_t s = (size_t) (fromSec * sr); s + n < y.size(); s += n / 2)
    {
        for (int i = 0; i < n; ++i)
            buf[(size_t) i] = { y[s + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2 * kPi * i / n)), 0.0f };
        fft.run (buf.data(), false);
        for (int b = 1; b < n / 2; ++b)
        {
            const double hz = b * sr / n;
            if (hz < 150 || hz > 4000)
                continue;
            const double mag = std::abs (buf[(size_t) b]);
            num += hz * mag;
            den += mag;
        }
    }
    return den > 0 ? num / den : 0;
}

static std::vector<float> run (Engine& e, std::vector<float> x, int block = 256)
{
    float* ch[1];
    for (size_t pos = 0; pos < x.size(); pos += (size_t) block)
    {
        ch[0] = x.data() + pos;
        e.process (ch, 1, (int) std::min<size_t> ((size_t) block, x.size() - pos));
    }
    return x;
}

// Per-window pitch of the signal, in cents relative to refHz.
static std::vector<double> centsTrack (const std::vector<float>& y, double sr, double refHz, double fromSec)
{
    PitchDetector d;
    const int wi = 1024;
    d.prepare (wi);
    std::vector<double> out;
    for (size_t s = (size_t) (fromSec * sr); s + 2 * wi < y.size(); s += 512)
    {
        const float p = d.detect (y.data() + s, wi, (int) (sr / 1500), (int) (sr / 55), 0.15f);
        if (p > 0)
            out.push_back (1200.0 * std::log2 ((sr / p) / refHz));
    }
    return out;
}

// Pitch track restricted to [fromSec, toSec).
static std::vector<double> centsRange (const std::vector<float>& y, double sr, double refHz, double fromSec, double toSec)
{
    const auto end = std::min (y.size(), (size_t) (toSec * sr));
    return centsTrack (std::vector<float> (y.begin(), y.begin() + (long) end), sr, refHz, fromSec);
}

// Runs with the host transport playing from t = 0 so Graph mode can capture/apply.
static std::vector<float> runPlaying (Engine& e, std::vector<float> x, bool capture, bool apply, int block = 256)
{
    float* ch[1];
    for (size_t pos = 0; pos < x.size(); pos += (size_t) block)
    {
        e.setTransport (true, (double) pos, capture, apply);
        ch[0] = x.data() + pos;
        e.process (ch, 1, (int) std::min<size_t> ((size_t) block, x.size() - pos));
    }
    return x;
}

static double median (std::vector<double> v)
{
    if (v.empty())
        return 1e9;
    std::sort (v.begin(), v.end());
    return v[v.size() / 2];
}

static double stddev (const std::vector<double>& v)
{
    if (v.size() < 2)
        return 0;
    double m = 0;
    for (double x : v) m += x;
    m /= (double) v.size();
    double s = 0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt (s / (double) v.size());
}

static Engine& fresh (Engine& e, const Params& p, double sr = 48000.0)
{
    e.prepare (sr, 1);
    e.setParams (p);
    return e;
}

int main()
{
    const double sr = 48000.0;
    const double a3 = 220.0;
    const auto cents = [] (double c) { return std::pow (2.0, c / 1200.0); };

    {
        std::puts ("Latency");
        Engine e;
        fresh (e, {});
        std::printf ("  latency = %d samples (%.1f ms @48k)\n", e.getLatencySamples(), 1000.0 * e.getLatencySamples() / sr);
        check (e.getLatencySamples() < 0.07 * sr, "latency under 70 ms");
    }

    {
        std::puts ("In-tune input passes through unchanged");
        Params p; p.retuneMs = 0;
        Engine e;
        auto x = voice (sr, 2.0, [&] (double) { return a3; });
        auto y = run (fresh (e, p), x);
        // PSOLA may sit a whole period away from the nominal delay; align on the best lag.
        const int L0 = e.getLatencySamples();
        double snr = -1e9;
        for (int L = L0 - 300; L <= L0 + 300; ++L)
        {
            double err = 0, sig = 0;
            for (size_t i = (size_t) sr; i < x.size(); ++i)
            {
                err += std::pow (y[i] - x[i - (size_t) L], 2);
                sig += std::pow (x[i - (size_t) L], 2);
            }
            snr = std::max (snr, 10 * std::log10 (sig / std::max (err, 1e-20)));
        }
        std::printf ("  SNR vs best-aligned input = %.1f dB\n", snr);
        check (snr > 25, "near-transparent when already in tune");
    }

    {
        std::puts ("Hard tune: 35 cents sharp -> A3");
        Params p; p.retuneMs = 0;
        Engine e;
        auto y = run (fresh (e, p), voice (sr, 2.0, [&] (double) { return a3 * cents (35); }));
        const double m = median (centsTrack (y, sr, a3, 0.5));
        std::printf ("  output median = %+.1f cents\n", m);
        check (std::abs (m) < 4, "corrected to within 4 cents");
    }

    {
        std::puts ("Scale snap: C major, input 190 Hz -> G3 (196 Hz)");
        Params p; p.retuneMs = 0; p.key = 0; p.scale = 1;
        Engine e;
        auto y = run (fresh (e, p), voice (sr, 2.0, [] (double) { return 190.0; }));
        const double m = median (centsTrack (y, sr, 196.0, 0.5));
        std::printf ("  output median vs G3 = %+.1f cents\n", m);
        check (std::abs (m) < 5, "snapped to G");
    }

    {
        std::puts ("Removed note: C major minus G, input 190 Hz -> F3 (174.6 Hz)");
        Params p; p.retuneMs = 0; p.key = 0; p.scale = 1; p.removedMask = 1u << 7;
        Engine e;
        auto y = run (fresh (e, p), voice (sr, 2.0, [] (double) { return 190.0; }));
        const double m = median (centsTrack (y, sr, 174.614, 0.5));
        std::printf ("  output median vs F3 = %+.1f cents\n", m);
        check (std::abs (m) < 5, "skips the removed G");
    }

    {
        std::puts ("MIDI target: only E held, input A3 -> E3");
        Params p; p.retuneMs = 0; p.midiTarget = true;
        Engine e;
        fresh (e, p).setMidiMask (1u << 4);
        auto y = run (e, voice (sr, 2.0, [&] (double) { return a3; }));
        const double m = median (centsTrack (y, sr, 164.814, 0.5));
        std::printf ("  output median vs E3 = %+.1f cents\n", m);
        check (std::abs (m) < 5, "follows the MIDI note");
    }

    {
        std::puts ("Transpose +12 with formant preservation");
        Params p; p.retuneMs = 0; p.transpose = 12;
        Engine e;
        auto y = run (fresh (e, p), voice (sr, 2.0, [&] (double) { return a3; }));
        const double m = median (centsTrack (y, sr, 440.0, 0.5));
        std::printf ("  output median vs A4 = %+.1f cents\n", m);
        check (std::abs (m) < 5, "octave up");
    }

    {
        std::puts ("Transpose -7 (fifth down)");
        Params p; p.retuneMs = 0; p.transpose = -7;
        Engine e;
        auto y = run (fresh (e, p), voice (sr, 2.0, [&] (double) { return a3; }));
        const double m = median (centsTrack (y, sr, 146.832, 0.5));
        std::printf ("  output median vs D3 = %+.1f cents\n", m);
        check (std::abs (m) < 5, "fifth down");
    }

    const auto vibrato = [&] (double t) { return a3 * cents (40.0 * std::sin (2 * kPi * 5.5 * t)); };
    double vibIn = 0;
    {
        Engine e;
        Params p; p.scale = 0; p.retuneMs = 400; p.mix = 0; // dry reference
        vibIn = stddev (centsTrack (run (fresh (e, p), voice (sr, 3.0, vibrato)), sr, a3, 0.5));
    }
    {
        std::puts ("Retune speed vs vibrato (input vibrato +/-40 cents)");
        Engine e1, e2;
        Params fast; fast.retuneMs = 0;
        Params slow; slow.retuneMs = 300;
        const double sFast = stddev (centsTrack (run (fresh (e1, fast), voice (sr, 3.0, vibrato)), sr, a3, 0.5));
        const double sSlow = stddev (centsTrack (run (fresh (e2, slow), voice (sr, 3.0, vibrato)), sr, a3, 0.5));
        std::printf ("  vibrato stddev: input %.1f, retune 0 -> %.1f, retune 300 -> %.1f cents\n", vibIn, sFast, sSlow);
        check (sFast < 0.3 * vibIn, "fast retune flattens vibrato");
        check (sSlow > 0.7 * vibIn, "slow retune keeps vibrato");
    }

    {
        std::puts ("Natural Vibrato +6 dB / -12 dB at slow retune");
        Engine e1, e2;
        Params up; up.retuneMs = 300; up.naturalVibratoDb = 6;
        Params dn; dn.retuneMs = 300; dn.naturalVibratoDb = -12;
        const double sUp = stddev (centsTrack (run (fresh (e1, up), voice (sr, 3.0, vibrato)), sr, a3, 0.5));
        const double sDn = stddev (centsTrack (run (fresh (e2, dn), voice (sr, 3.0, vibrato)), sr, a3, 0.5));
        std::printf ("  vibrato stddev: +6 dB -> %.1f, -12 dB -> %.1f (input %.1f)\n", sUp, sDn, vibIn);
        check (sUp > 1.5 * vibIn, "deepens vibrato");
        check (sDn < 0.5 * vibIn, "reduces vibrato");
    }

    {
        std::puts ("Expression 100%: 40 cents off is left alone, 3 cents off is pulled in");
        Engine e1, e2;
        Params p; p.retuneMs = 0; p.expression = 1.0f;
        const double far = median (centsTrack (run (fresh (e1, p), voice (sr, 2.0, [&] (double) { return a3 * cents (40); })), sr, a3, 0.5));
        const double near = median (centsTrack (run (fresh (e2, p), voice (sr, 2.0, [&] (double) { return a3 * cents (3); })), sr, a3, 0.5));
        std::printf ("  40c input -> %+.1f, 3c input -> %+.1f cents\n", far, near);
        check (far > 30, "expressive deviation preserved");
        check (std::abs (near) < 1.5, "near-target note corrected");
    }

    {
        std::puts ("Humanize: sustained note settles slower than a short one");
        // Input jumps from 30 cents sharp A3 to 30 cents sharp B3 at 1.0 s and stays.
        auto curve = [&] (double t) { return (t < 1.0 ? a3 : 246.942) * cents (30); };
        Engine e1, e2;
        Params p0; p0.retuneMs = 20; p0.humanize = 0;
        Params p1 = p0; p1.humanize = 1;
        auto t0 = centsTrack (run (fresh (e1, p0), voice (sr, 3.0, curve)), sr, 246.942, 2.0);
        auto t1 = centsTrack (run (fresh (e2, p1), voice (sr, 3.0, curve)), sr, 246.942, 2.0);
        // Both land on B3 eventually; humanize must still correct sustained notes.
        std::printf ("  late-sustain median: humanize 0 -> %+.1f, humanize 100 -> %+.1f cents\n", median (t0), median (t1));
        check (std::abs (median (t0)) < 4 && std::abs (median (t1)) < 12, "sustained notes still corrected");
    }

    {
        std::puts ("Stability: silence, noise, sweep");
        Engine e;
        fresh (e, {});
        std::vector<float> x ((size_t) sr * 2, 0.0f);
        uint32_t seed = 1;
        for (size_t i = sr / 2; i < sr; ++i) { seed = seed * 1664525u + 1013904223u; x[i] = ((seed >> 9) / 8388608.0f - 1.0f) * 0.5f; }
        auto sweep = voice (sr, 1.0, [] (double t) { return 80.0 * std::pow (10.0, t); });
        std::copy (sweep.begin(), sweep.end(), x.begin() + (long) sr);
        auto y = run (e, x, 37);
        bool finite = true; float peak = 0;
        for (float v : y) { finite &= std::isfinite (v); peak = std::max (peak, std::abs (v)); }
        float silent = 0;
        for (size_t i = 0; i < sr / 4; ++i) silent = std::max (silent, std::abs (y[i]));
        std::printf ("  peak %.3f, leading silence peak %.6f\n", peak, silent);
        check (finite, "no NaN/Inf");
        check (peak < 2.0f, "no blow-ups");
        check (silent < 1e-6f, "silence stays silent");
    }

    {
        std::puts ("Sample rates 44.1k / 96k hard tune");
        for (double rate : { 44100.0, 96000.0 })
        {
            Engine e;
            Params p; p.retuneMs = 0;
            auto y = run (fresh (e, p, rate), voice (rate, 2.0, [&] (double) { return a3 * cents (-30); }));
            const double m = median (centsTrack (y, rate, a3, 0.5));
            std::printf ("  %.0f Hz: %+.1f cents, latency %d\n", rate, m, e.getLatencySamples());
            check (std::abs (m) < 5, "corrects at " + std::to_string ((int) rate));
        }
    }

    {
        std::puts ("LIVE: in-tune voice, measured delay");
        for (double f0 : { 110.0, 220.0, 440.0 })
        {
            Params p; p.retuneMs = 0; p.liveMode = true;
            Engine e;
            auto x = voice (sr, 2.0, [&] (double) { return f0; });
            auto y = run (fresh (e, p), x);
            double bestSnr = -1e9;
            for (int L = 0; L <= 1200; ++L)
            {
                double err = 0, sig = 0;
                for (size_t i = (size_t) sr; i < (size_t) sr + 9600; ++i)
                {
                    err += std::pow (y[i] - x[i - (size_t) L], 2);
                    sig += std::pow (x[i - (size_t) L], 2);
                }
                bestSnr = std::max (bestSnr, 10 * std::log10 (sig / std::max (err, 1e-20)));
            }
            const double delayMs = 1000.0 * e.getLiveDelaySamples() / sr;
            std::printf ("  %3.0f Hz: delay %.2f ms (floor %.2f ms), SNR %.1f dB\n", f0, delayMs,
                         1000.0 * e.getLatencySamples (true) / sr, bestSnr);
            check (delayMs < 1.5 + 1000.0 / f0, "live delay <= floor + one period at " + std::to_string ((int) f0) + " Hz");
            check (bestSnr > 25, "live is clean when already in tune");
        }
    }

    {
        std::puts ("LIVE: hard tune, scale snap, transpose, vibrato");
        auto liveRun = [&] (Params p, std::function<double (double)> f0, double ref, double from = 0.5)
        {
            p.liveMode = true;
            Engine e;
            return centsTrack (run (fresh (e, p), voice (sr, 2.5, f0)), sr, ref, from);
        };
        Params hard; hard.retuneMs = 0;
        const double m1 = median (liveRun (hard, [&] (double) { return a3 * cents (35); }, a3));
        Params major = hard; major.scale = 1;
        const double m2 = median (liveRun (major, [] (double) { return 190.0; }, 196.0));
        Params up = hard; up.transpose = 12;
        const double m3 = median (liveRun (up, [&] (double) { return a3; }, 440.0));
        Params down = hard; down.transpose = -12;
        const double m4 = median (liveRun (down, [&] (double) { return a3; }, 110.0));
        const double sv = stddev (liveRun (hard, vibrato, a3));
        std::printf ("  +35c -> %+.1f, 190Hz -> G %+.1f, +12 -> %+.1f, -12 -> %+.1f cents, vibrato stddev %.1f\n", m1, m2, m3, m4, sv);
        check (std::abs (m1) < 4, "live hard tune");
        check (std::abs (m2) < 5, "live scale snap");
        check (std::abs (m3) < 5, "live octave up");
        check (std::abs (m4) < 5, "live octave down");
        check (sv < 0.3 * vibIn, "live flattens vibrato at retune 0");
    }

    {
        std::puts ("LIVE <-> STUDIO switching mid-stream stays stable");
        Engine e;
        Params p; p.retuneMs = 10;
        fresh (e, p);
        auto x = voice (sr, 4.0, vibrato);
        float peak = 0; bool finite = true;
        for (size_t pos = 0; pos < x.size(); pos += 512)
        {
            p.liveMode = ((pos / 24000) % 2) == 1; // flip every 0.5 s
            e.setParams (p);
            float* ch[1] = { x.data() + pos };
            e.process (ch, 1, (int) std::min<size_t> (512, x.size() - pos));
        }
        for (float v : x) { finite &= std::isfinite (v); peak = std::max (peak, std::abs (v)); }
        std::printf ("  peak %.3f\n", peak);
        check (finite && peak < 2.0f, "no blow-ups while switching");
    }

    {
        std::puts ("Learn Key");
        auto learn = [&] (std::vector<int> melody, int& key, bool& minor)
        {
            Engine e;
            Params p; p.retuneMs = 0;
            fresh (e, p).beginKeyLearn();
            auto f = [&] (double t) { return 440.0 * std::pow (2.0, (melody[(size_t) (t / 0.25) % melody.size()] - 69) / 12.0); };
            run (e, voice (sr, 0.25 * (double) melody.size() * 2, f));
            const auto h = e.getKeyHistogram();
            return estimateKey (h.data(), key, minor);
        };
        int key = -1; bool minor = false;
        // A natural minor phrase, resting on A, C, E
        const bool ok1 = learn ({ 57, 60, 64, 62, 60, 59, 57, 64, 65, 64, 62, 60, 59, 57, 57, 52 }, key, minor);
        std::printf ("  A minor melody -> %s %s\n", kNoteNames[(size_t) std::max (0, key)], minor ? "minor" : "major");
        check (ok1 && key == 9 && minor, "detects A minor");
        // E major phrase
        const bool ok2 = learn ({ 64, 68, 71, 69, 68, 66, 64, 71, 73, 71, 68, 66, 63, 64, 64, 59 }, key, minor);
        std::printf ("  E major melody -> %s %s\n", kNoteNames[(size_t) std::max (0, key)], minor ? "minor" : "major");
        check (ok2 && key == 4 && ! minor, "detects E major");
    }

    {
        std::puts ("Formants: +5 semitones on a vowel (centroid relative to input)");
        auto f150 = [] (double) { return 150.0; };
        const auto x = vowel (sr, 2.5, f150);
        const double c0 = centroid (x, sr, 0.5);
        auto shifted = [&] (bool live, bool formant, float throat, float transpose)
        {
            Params p; p.retuneMs = 0; p.liveMode = live; p.formantPreserve = formant; p.throat = throat; p.transpose = transpose;
            Engine e;
            return run (fresh (e, p), x);
        };
        const auto liveOn = shifted (true, true, 1.0f, 5);
        const double rStudio = centroid (shifted (false, true, 1.0f, 5), sr, 0.5) / c0;
        const double rLiveOff = centroid (shifted (true, false, 1.0f, 5), sr, 0.5) / c0;
        const double rLiveOn = centroid (liveOn, sr, 0.5) / c0;
        const double rThroat = centroid (shifted (true, true, 1.25f, 0), sr, 0.5) / c0;
        // 150 Hz is D3 + 37 cents; hard tune snaps to D3 (146.83 Hz) before transposing.
        const double pitch = median (centsTrack (liveOn, sr, 146.832 * std::pow (2.0, 5.0 / 12.0), 0.5));
        std::printf ("  studio formant %.3f, live off %.3f, live formant %.3f, live throat 125%% %.3f; live formant pitch %+.1f c\n",
                     rStudio, rLiveOff, rLiveOn, rThroat, pitch);
        check (rLiveOff > 1.15, "without formant, live moves formants up with pitch");
        check (std::abs (rLiveOn - 1.0) < 0.08, "live formant keeps formants in place");
        check (rThroat < 0.92, "live throat lowers formants");
        check (std::abs (pitch) < 5, "live formant still lands the pitch");
    }

    {
        std::puts ("Formants: high voice (300 Hz) +5 semitones");
        const auto x = vowel (sr, 2.5, [] (double) { return 293.665; }); // D4, in tune
        const double c0 = centroid (x, sr, 0.5);
        Params p; p.retuneMs = 0; p.liveMode = true; p.transpose = 5;
        Engine e1, e2;
        const double rOn = centroid (run (fresh (e1, p), x), sr, 0.5) / c0;
        p.formantPreserve = false;
        const double rOff = centroid (run (fresh (e2, p), x), sr, 0.5) / c0;
        std::printf ("  live formant %.3f, live off %.3f\n", rOn, rOff);
        check (std::abs (rOn - 1.0) < 0.08 && rOff > 1.15, "high voice formants held");
    }

    {
        std::puts ("Formants: live with formant on is clean when already in tune");
        Params p; p.retuneMs = 0; p.liveMode = true; p.formantPreserve = true;
        Engine e;
        auto x = vowel (sr, 2.0, [] (double) { return 220.0; });
        auto y = run (fresh (e, p), x);
        double bestSnr = -1e9;
        for (int L = 0; L <= 1200; ++L)
        {
            double err = 0, sig = 0;
            for (size_t i = (size_t) sr; i < (size_t) sr + 9600; ++i)
            {
                err += std::pow (y[i] - x[i - (size_t) L], 2);
                sig += std::pow (x[i - (size_t) L], 2);
            }
            bestSnr = std::max (bestSnr, 10 * std::log10 (sig / std::max (err, 1e-20)));
        }
        std::printf ("  SNR %.1f dB\n", bestSnr);
        check (bestSnr > 20, "near-transparent through the LPC path");
    }

    {
        std::puts ("Graph: capture lines up with the host timeline");
        auto track = std::make_unique<PitchTrack>();
        Engine e;
        Params p; p.retuneMs = 0;
        fresh (e, p).setTrack (track.get());
        runPlaying (e, voice (sr, 2.0, [&] (double t) { return t < 1.0 ? a3 : 261.626; }), true, false);
        const float before = track->inputAt (PitchTrack::indexFor (0.6));
        const float after = track->inputAt (PitchTrack::indexFor (1.4));
        int change = -1;
        for (int i = PitchTrack::indexFor (0.8); i < PitchTrack::indexFor (1.2); ++i)
            if (std::isfinite (track->inputAt (i)) && track->inputAt (i) > 58.5f) { change = i; break; }
        const double changeMs = change < 0 ? 1e9 : (PitchTrack::secondsAt (change) - 1.0) * 1000.0;
        std::printf ("  captured %.2f then %.2f, note change seen at %+.1f ms\n", before, after, changeMs);
        check (std::abs (before - 57.0f) < 0.05f && std::abs (after - 60.0f) < 0.05f, "captured pitch is right");
        check (std::abs (changeMs) < 30.0, "captured timing within 30 ms");
    }

    for (bool live : { false, true })
    {
        std::printf ("Graph: drawn D4 over 0.8-1.6 s on an A3 voice (%s)\n", live ? "live" : "studio");
        auto track = std::make_unique<PitchTrack>();
        for (int i = PitchTrack::indexFor (0.8); i < PitchTrack::indexFor (1.6); ++i)
            track->setTarget (i, 62.0f, PitchTrack::kExact);
        Engine e;
        Params p; p.retuneMs = 0; p.liveMode = live;
        fresh (e, p).setTrack (track.get());
        auto y = runPlaying (e, voice (sr, 2.2, [&] (double) { return a3; }), false, true);
        const double lat = e.getLatencySamples (live) / sr;
        const double inEdit = median (centsRange (y, sr, 293.665, 1.0 + lat, 1.35 + lat));
        const double outside = median (centsRange (y, sr, a3, 1.75 + lat, 2.15));
        std::printf ("  inside edit vs D4 %+.1f c, after edit vs A3 %+.1f c\n", inEdit, outside);
        check (std::abs (inEdit) < 5, "follows the drawn pitch");
        check (std::abs (outside) < 5, "auto mode resumes after the edit");
    }

    {
        std::puts ("Graph: Note edit keeps the singer's vibrato");
        auto track = std::make_unique<PitchTrack>();
        for (int i = PitchTrack::indexFor (0.3); i < PitchTrack::indexFor (3.0); ++i)
            track->setTarget (i, 60.0f, PitchTrack::kNote);
        Engine e;
        Params p; p.retuneMs = 0;
        fresh (e, p).setTrack (track.get());
        auto y = runPlaying (e, voice (sr, 3.0, vibrato), false, true);
        const auto c = centsRange (y, sr, 261.626, 0.8, 2.9);
        std::printf ("  centre %+.1f c vs C4, vibrato stddev %.1f (input %.1f)\n", median (c), stddev (c), vibIn);
        check (std::abs (median (c)) < 8, "moved to the drawn note");
        check (stddev (c) > 0.6 * vibIn, "vibrato kept");
    }

    std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
