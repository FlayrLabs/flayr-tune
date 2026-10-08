#pragma once

// Flayr Tune DSP engine. Framework-free so it can be unit tested offline.
//
// Pipeline:
//   1. YIN pitch detection (FFT accelerated) on a mono sum, every `hop` samples.
//   2. Correction model at hop rate: scale/MIDI target selection with hysteresis,
//      natural-vibrato rescaling, Expression capture zone, retune smoothing with
//      Humanize, synthetic vibrato, transpose. Produces a pitch ratio per hop.
//   3. TD-PSOLA resynthesis per channel. Grains are always read over +/- one input
//      period, so formants stay put (formant preservation) unless the grain is
//      resampled by the throat factor.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

namespace tune
{
constexpr double kPi = 3.14159265358979323846;

inline int nextPow2 (int v)
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

//==============================================================================
inline constexpr std::array<const char*, 13> kScaleNames {
    "Chromatic", "Major", "Natural Minor", "Harmonic Minor", "Melodic Minor",
    "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian",
    "Major Pentatonic", "Minor Pentatonic", "Blues"
};

inline constexpr std::array<const char*, 12> kNoteNames {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

inline uint16_t maskOf (std::initializer_list<int> steps)
{
    uint16_t m = 0;
    for (int s : steps)
        m = (uint16_t) (m | (1u << s));
    return m;
}

// Bit i = i semitones above the key.
inline uint16_t scaleIntervals (int scale)
{
    switch (scale)
    {
        case 1:  return maskOf ({ 0, 2, 4, 5, 7, 9, 11 });
        case 2:  return maskOf ({ 0, 2, 3, 5, 7, 8, 10 });
        case 3:  return maskOf ({ 0, 2, 3, 5, 7, 8, 11 });
        case 4:  return maskOf ({ 0, 2, 3, 5, 7, 9, 11 });
        case 5:  return maskOf ({ 0, 2, 3, 5, 7, 9, 10 });
        case 6:  return maskOf ({ 0, 1, 3, 5, 7, 8, 10 });
        case 7:  return maskOf ({ 0, 2, 4, 6, 7, 9, 11 });
        case 8:  return maskOf ({ 0, 2, 4, 5, 7, 9, 10 });
        case 9:  return maskOf ({ 0, 1, 3, 5, 6, 8, 10 });
        case 10: return maskOf ({ 0, 2, 4, 7, 9 });
        case 11: return maskOf ({ 0, 3, 5, 7, 10 });
        case 12: return maskOf ({ 0, 3, 5, 6, 7, 10 });
        default: return 0x0FFF;
    }
}

// Absolute pitch-class mask (bit 0 = C) for a key + scale.
inline uint16_t scalePitchClasses (int key, int scale)
{
    const uint16_t rel = scaleIntervals (scale);
    uint16_t abs = 0;
    for (int i = 0; i < 12; ++i)
        if ((rel >> i) & 1)
            abs = (uint16_t) (abs | (1u << ((i + key) % 12)));
    return abs;
}

// Krumhansl-Schmuckler key estimate from a pitch-class duration histogram.
// Returns false when there is too little material to judge.
inline bool estimateKey (const float* hist, int& key, bool& minor)
{
    static constexpr double major[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static constexpr double minorP[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

    double total = 0.0;
    for (int i = 0; i < 12; ++i)
        total += hist[i];
    if (total < 0.5)
        return false;

    auto corr = [&] (const double* profile, int root)
    {
        double mx = 0.0, my = 0.0;
        for (int i = 0; i < 12; ++i) { mx += hist[(i + root) % 12]; my += profile[i]; }
        mx /= 12.0; my /= 12.0;
        double sxy = 0.0, sxx = 0.0, syy = 0.0;
        for (int i = 0; i < 12; ++i)
        {
            const double dx = hist[(i + root) % 12] - mx, dy = profile[i] - my;
            sxy += dx * dy; sxx += dx * dx; syy += dy * dy;
        }
        return sxx > 0.0 ? sxy / std::sqrt (sxx * syy) : 0.0;
    };

    double best = -2.0;
    for (int root = 0; root < 12; ++root)
    {
        for (int m = 0; m < 2; ++m)
        {
            const double r = corr (m ? minorP : major, root);
            if (r > best) { best = r; key = root; minor = m == 1; }
        }
    }
    return true;
}

//==============================================================================
struct Params
{
    float retuneMs = 20.0f;          // 0..400
    float humanize = 0.0f;           // 0..1
    float expression = 0.0f;         // 0..1
    float naturalVibratoDb = 0.0f;   // -12..+12
    int key = 0;
    int scale = 0;
    uint16_t removedMask = 0;        // pitch classes excluded from correction
    bool midiTarget = false;
    float transpose = 0.0f;          // semitones
    float concertA = 440.0f;
    bool formantPreserve = true;
    float throat = 1.0f;             // throat length factor; >1 = longer = darker
    float tracking = 0.5f;           // 0 = strict, 1 = relaxed
    int inputType = 1;               // 0 soprano, 1 alto/tenor, 2 low male, 3 instrument
    float vibRateHz = 5.5f;
    float vibDepthCents = 0.0f;
    float vibDelayMs = 400.0f;
    float mix = 1.0f;
    float outputGain = 1.0f;
    bool liveMode = false;           // low-latency delay-line shifter instead of PSOLA
};

struct DisplayPoint
{
    float inNote = 0.0f;
    float outNote = 0.0f;
    float targetNote = -1.0f;
    uint8_t voiced = 0;
};

//==============================================================================
class Fft
{
public:
    void init (int size)
    {
        n = size;
        int bits = 0;
        while ((1 << bits) < n)
            ++bits;
        rev.assign ((size_t) n, 0);
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if ((i >> b) & 1)
                    r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
        tw.resize ((size_t) n / 2);
        for (int k = 0; k < n / 2; ++k)
            tw[(size_t) k] = std::polar (1.0f, (float) (-2.0 * kPi * k / n));
    }

    void run (std::complex<float>* a, bool inverse) const
    {
        for (int i = 0; i < n; ++i)
            if (i < rev[(size_t) i])
                std::swap (a[i], a[rev[(size_t) i]]);

        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2;
            const int step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < half; ++j)
                {
                    auto w = tw[(size_t) (j * step)];
                    if (inverse)
                        w = std::conj (w);
                    const auto u = a[i + j];
                    const auto v = a[i + j + half] * w;
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
        }

        if (inverse)
        {
            const float s = 1.0f / (float) n;
            for (int i = 0; i < n; ++i)
                a[i] *= s;
        }
    }

private:
    int n = 0;
    std::vector<int> rev;
    std::vector<std::complex<float>> tw;
};

//==============================================================================
// YIN with the difference function computed through FFT cross-correlation.
class PitchDetector
{
public:
    void prepare (int integrationWindow)
    {
        wi = integrationWindow;
        fft.init (2 * wi);
        a.assign ((size_t) (2 * wi), {});
        b.assign ((size_t) (2 * wi), {});
        cmnd.assign ((size_t) (2 * wi), 1.0f);
        energy.assign ((size_t) (2 * wi + 1), 0.0);
    }

    // Samples needed for a given integration length and longest lag.
    static int segmentLength (int intLen, int maxTau) { return intLen + maxTau + 2; }

    // x holds segmentLength (intLen, maxTau) samples, which must fit in 2 * wi.
    // Returns the period in samples, or 0 when unvoiced.
    float detect (const float* x, int intLen, int minTau, int maxTau, float threshold)
    {
        const int n = 2 * wi;
        intLen = std::clamp (intLen, 16, wi);
        maxTau = std::min (maxTau, n - intLen - 2);
        minTau = std::max (2, minTau);
        if (minTau >= maxTau)
            return 0.0f;
        const int len = segmentLength (intLen, maxTau);

        for (int i = 0; i < n; ++i)
        {
            a[(size_t) i] = { i < intLen ? x[i] : 0.0f, 0.0f };
            b[(size_t) i] = { i < len ? x[i] : 0.0f, 0.0f };
        }
        fft.run (a.data(), false);
        fft.run (b.data(), false);
        for (int i = 0; i < n; ++i)
            a[(size_t) i] = std::conj (a[(size_t) i]) * b[(size_t) i];
        fft.run (a.data(), true); // a[tau].real() = sum_j x[j] * x[j + tau]

        energy[0] = 0.0;
        for (int i = 0; i < len; ++i)
            energy[(size_t) i + 1] = energy[(size_t) i] + (double) x[i] * x[i];

        const double e0 = energy[(size_t) intLen];
        if (e0 < 1e-10 * intLen)
            return 0.0f;

        cmnd[0] = 1.0f;
        double running = 0.0;
        for (int tau = 1; tau <= maxTau + 1; ++tau)
        {
            const double et = energy[(size_t) (tau + intLen)] - energy[(size_t) tau];
            const double dt = std::max (0.0, e0 + et - 2.0 * (double) a[(size_t) tau].real());
            running += dt;
            cmnd[(size_t) tau] = running > 0.0 ? (float) (dt * tau / running) : 1.0f;
        }

        int best = -1;
        for (int tau = minTau; tau <= maxTau; ++tau)
        {
            if (cmnd[(size_t) tau] < threshold)
            {
                while (tau + 1 <= maxTau && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau])
                    ++tau;
                best = tau;
                break;
            }
        }
        if (best < 0)
            return 0.0f;

        const float s0 = cmnd[(size_t) best - 1];
        const float s1 = cmnd[(size_t) best];
        const float s2 = cmnd[(size_t) best + 1];
        const float denom = s0 - 2.0f * s1 + s2;
        float shift = std::abs (denom) > 1e-9f ? 0.5f * (s0 - s2) / denom : 0.0f;
        shift = std::clamp (shift, -1.0f, 1.0f);
        return (float) best + shift;
    }

private:
    int wi = 0;
    Fft fft;
    std::vector<std::complex<float>> a, b;
    std::vector<float> cmnd;
    std::vector<double> energy;
};

//==============================================================================
// Low-latency formant handling for Live mode. LPC splits the voice into a spectral
// envelope (the formants) and a spectrally flat residual. The residual is what gets
// pitch shifted; re-applying the envelope afterwards keeps the formants in place, and
// re-applying an envelope measured on a time-stretched copy moves them (Throat).
// Lattice filters stay stable while their coefficients glide between updates.
class LpcFormant
{
public:
    static constexpr int kMaxOrder = 48;
    using Coeffs = std::array<float, kMaxOrder + 1>; // index 1..order

    // `historyLength` must exceed the longest live read delay.
    void prepare (double sampleRate, int historyLength)
    {
        sr = sampleRate;
        histLen = nextPow2 (historyLength);
        histMask = histLen - 1;
        histK.assign ((size_t) (histLen * (kMaxOrder + 1)), 0.0f);
        histKW.assign ((size_t) (histLen * (kMaxOrder + 1)), 0.0f);
        histGain.assign ((size_t) histLen, 1.0f);
        order = std::clamp ((int) std::lround (sr / 2400.0), 16, kMaxOrder);
        win = (int) std::lround (0.021 * sr);
        window.resize ((size_t) win);
        for (int i = 0; i < win; ++i)
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * (i + 0.5) / win));
        buf.assign ((size_t) win, 0.0f);
        setSmoothing (250.0);
        smooth = (float) (1.0 - std::exp (-1.0 / (0.002 * sr)));
        reset();
    }

    void reset()
    {
        kTarget.fill (0.0f); kWarpTarget.fill (0.0f); kCur.fill (0.0f); kWarpCur.fill (0.0f);
        for (auto& st : anaState) st.fill (0.0f);
        for (auto& st : synState) st.fill (0.0f);
        gainTarget = gainCur = 1.0f;
        std::fill (histK.begin(), histK.end(), 0.0f);
        std::fill (histKW.begin(), histKW.end(), 0.0f);
        std::fill (histGain.begin(), histGain.end(), 1.0f);
    }

    // Gaussian lag window = spectral blur of `hz`. It has to span about one harmonic
    // spacing, or the envelope locks onto individual harmonics and drags the original
    // pitch back in when it is re-applied.
    void setSmoothing (double hz)
    {
        if (std::abs (hz - smoothingHz) < 1.0)
            return;
        smoothingHz = hz;
        for (int k = 0; k <= kMaxOrder; ++k)
        {
            const double w = 2.0 * kPi * hz * k / sr;
            lagWin[(size_t) k] = std::exp (-0.5 * w * w);
        }
    }

    // Re-estimate from the newest `win` samples. `formantScale` > 1 raises formants.
    template <typename Reader>
    void update (Reader&& at, double newest, double formantScale)
    {
        double e1 = 0.0;
        for (int i = 0; i < win; ++i)
            buf[(size_t) i] = window[(size_t) i] * at (newest - (win - 1 - i));
        if (! levinson (kTarget, e1))
            return;

        if (std::abs (formantScale - 1.0) < 1e-3)
        {
            kWarpTarget = kTarget;
            gainTarget = 1.0f;
            return;
        }
        // Reading the past faster than real time stretches its spectrum by formantScale.
        double e2 = 0.0;
        for (int i = 0; i < win; ++i)
            buf[(size_t) i] = window[(size_t) i] * at (newest - (win - 1 - i) * formantScale);
        if (levinson (kWarpTarget, e2))
            gainTarget = (float) std::clamp (std::sqrt (e2 / std::max (e1, 1e-12)), 0.25, 4.0);
    }

    // Once per sample (absolute index n), before the per-channel filters. The coefficients
    // used at n are recorded so colour() can re-apply exactly the envelope that was
    // removed from the sample it is reading, not today's.
    void tick (int64_t n)
    {
        const auto slot = (size_t) (n & histMask) * (kMaxOrder + 1);
        for (int m = 1; m <= order; ++m)
        {
            kCur[(size_t) m] += smooth * (kTarget[(size_t) m] - kCur[(size_t) m]);
            kWarpCur[(size_t) m] += smooth * (kWarpTarget[(size_t) m] - kWarpCur[(size_t) m]);
            histK[slot + (size_t) m] = kCur[(size_t) m];
            histKW[slot + (size_t) m] = kWarpCur[(size_t) m];
        }
        gainCur += smooth * (gainTarget - gainCur);
        histGain[(size_t) (n & histMask)] = gainCur;
    }

    // FIR lattice A(z): voice -> flat residual.
    float whiten (int c, float x)
    {
        auto& st = anaState[(size_t) c];
        float f = x, bNew = x;
        for (int m = 1; m <= order; ++m)
        {
            const float bOld = st[(size_t) m - 1];
            const float k = kCur[(size_t) m];
            const float fNext = f + k * bOld;
            const float bNext = bOld + k * f;
            st[(size_t) m - 1] = bNew;
            bNew = bNext;
            f = fNext;
        }
        return f;
    }

    // IIR lattice 1/A(z) with the (possibly warped) envelope that was in force when the
    // residual at `sourceIndex` was made: residual -> voice.
    float colour (int c, float e, int64_t sourceIndex)
    {
        const auto at = (size_t) (std::max<int64_t> (0, sourceIndex) & histMask);
        const float* k = &histKW[at * (kMaxOrder + 1)];
        auto& st = synState[(size_t) c];
        float f = e;
        for (int m = order; m >= 1; --m)
        {
            f -= k[m] * st[(size_t) m - 1];
            st[(size_t) m] = st[(size_t) m - 1] + k[m] * f;
        }
        st[0] = f;
        return f * histGain[at];
    }

private:
    bool levinson (Coeffs& kOut, double& err)
    {
        std::array<double, kMaxOrder + 1> r {}, a {}, prev {};
        for (int k = 0; k <= order; ++k)
        {
            double sum = 0.0;
            for (int i = k; i < win; ++i)
                sum += (double) buf[(size_t) i] * buf[(size_t) (i - k)];
            r[(size_t) k] = sum * lagWin[(size_t) k];
        }
        if (r[0] < 1e-9)
            return false;
        r[0] *= 1.0001; // -40 dB noise floor keeps the recursion well conditioned

        a[0] = 1.0;
        err = r[0];
        for (int m = 1; m <= order; ++m)
        {
            double acc = r[(size_t) m];
            for (int i = 1; i < m; ++i)
                acc += a[(size_t) i] * r[(size_t) (m - i)];
            const double km = std::clamp (-acc / err, -0.995, 0.995);
            prev = a;
            for (int i = 1; i < m; ++i)
                a[(size_t) i] = prev[(size_t) i] + km * prev[(size_t) (m - i)];
            a[(size_t) m] = km;
            err *= (1.0 - km * km);
            kOut[(size_t) m] = (float) km;
        }
        return true;
    }

    double sr = 48000.0, smoothingHz = 0.0;
    int order = 20, win = 1008;
    std::vector<float> window, buf;
    std::array<double, kMaxOrder + 1> lagWin {};
    Coeffs kTarget {}, kWarpTarget {}, kCur {}, kWarpCur {};
    int histLen = 0;
    int64_t histMask = 0;
    std::vector<float> histK, histKW, histGain;
    std::array<Coeffs, 2> anaState {}, synState {};
    float gainTarget = 1.0f, gainCur = 1.0f, smooth = 0.01f;
};

//==============================================================================
// Graph mode data: pitch along the host timeline on a fixed 5 ms grid. Preallocated and
// lock-free; the audio thread writes captured pitch and reads edits, the GUI the reverse.
// NaN means "nothing here".
class PitchTrack
{
public:
    static constexpr double kGridSec = 0.005;
    static constexpr int kCapacity = 30 * 60 * 200; // 30 minutes
    enum Mode : uint8_t { kNone = 0, kExact = 1, kNote = 2 };

    PitchTrack()
        : input (new std::atomic<float>[kCapacity]),
          target (new std::atomic<float>[kCapacity]),
          mode (new std::atomic<uint8_t>[kCapacity])
    {
        clearAll();
    }

    static int indexFor (double seconds) { return (int) std::lround (seconds / kGridSec); }
    static double secondsAt (int index) { return index * kGridSec; }
    static bool valid (int i) { return i >= 0 && i < kCapacity; }

    float inputAt (int i) const { return valid (i) ? input[i].load (std::memory_order_relaxed) : NAN; }
    float targetAt (int i) const { return valid (i) ? target[i].load (std::memory_order_relaxed) : NAN; }
    uint8_t modeAt (int i) const { return valid (i) ? mode[i].load (std::memory_order_relaxed) : (uint8_t) kNone; }
    int getExtent() const { return extent.load (std::memory_order_relaxed); }

    void setInput (int i, float note)
    {
        if (! valid (i))
            return;
        input[i].store (note, std::memory_order_relaxed);
        if (i >= extent.load (std::memory_order_relaxed))
            extent.store (i + 1, std::memory_order_relaxed);
    }

    void setTarget (int i, float note, uint8_t m)
    {
        if (! valid (i))
            return;
        target[i].store (note, std::memory_order_relaxed);
        mode[i].store (std::isfinite (note) ? m : (uint8_t) kNone, std::memory_order_relaxed);
        if (std::isfinite (note) && i >= extent.load (std::memory_order_relaxed))
            extent.store (i + 1, std::memory_order_relaxed);
    }

    void clearEdits()
    {
        for (int i = 0; i < kCapacity; ++i)
            setTarget (i, NAN, kNone);
    }

    void clearAll()
    {
        for (int i = 0; i < kCapacity; ++i)
        {
            input[i].store (NAN, std::memory_order_relaxed);
            target[i].store (NAN, std::memory_order_relaxed);
            mode[i].store (kNone, std::memory_order_relaxed);
        }
        extent.store (0);
    }

private:
    std::unique_ptr<std::atomic<float>[]> input, target;
    std::unique_ptr<std::atomic<uint8_t>[]> mode;
    std::atomic<int> extent { 0 };
};

//==============================================================================
class Engine
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr double kMinF0 = 60.0;

    void prepare (double sampleRate, int numChannels)
    {
        sr = sampleRate;
        chans = std::clamp (numChannels, 1, kMaxChannels);
        pMax = sr / kMinF0;
        wi = nextPow2 ((int) std::ceil (pMax) + 2);
        detector.prepare (wi);
        hop = std::max (64, nextPow2 ((int) (sr / 400.0)));
        lookBehind = (int) std::ceil (1.5 * wi) + 2;
        latency = lookBehind + (int) std::ceil (1.5 * pMax) + 4;
        ringSize = nextPow2 (latency + 4 * wi + 4096);
        ringMask = ringSize - 1;
        unvoicedPeriod = sr * 0.005;
        liveFadeLen = std::max (16, (int) std::lround (0.0012 * sr));
        liveMinDelay = liveFadeLen + 8;

        for (int c = 0; c < kMaxChannels; ++c)
        {
            in[(size_t) c].assign ((size_t) ringSize, 0.0f);
            acc[(size_t) c].assign ((size_t) ringSize, 0.0f);
            residual[(size_t) c].assign ((size_t) ringSize, 0.0f);
        }
        lpc.prepare (sr, liveMinDelay + (int) std::ceil (pMax) * 2 + 4 * liveFadeLen + 64);
        mono.assign ((size_t) ringSize, 0.0f);
        wsum.assign ((size_t) ringSize, 0.0f);
        segment.assign ((size_t) (2 * wi), 0.0f);
        reset();
    }

    void reset()
    {
        for (int c = 0; c < kMaxChannels; ++c)
        {
            std::fill (in[(size_t) c].begin(), in[(size_t) c].end(), 0.0f);
            std::fill (acc[(size_t) c].begin(), acc[(size_t) c].end(), 0.0f);
            std::fill (residual[(size_t) c].begin(), residual[(size_t) c].end(), 0.0f);
        }
        lpc.reset();
        liveFormantBlend = params.formantPreserve ? 1.0f : 0.0f;
        std::fill (mono.begin(), mono.end(), 0.0f);
        std::fill (wsum.begin(), wsum.end(), 0.0f);
        written = 0;
        hopCounter = 0;
        synPos = anaPos = 0.0;
        tlCount = tlHead = 0;
        wasVoiced = false;
        corr = center = lastNote = vibCenter1 = vibCenter2 = slopeSm = 0.0f;
        trackedPeriod = 0.0;
        target = -1;
        heldSec = 0.0;
        vibPhase = 0.0;
        glitchHeld = false;
        liveRead = -(double) liveMinDelay;
        liveReadB = 0.0;
        liveFading = false;
        liveFadePos = 0;
        liveRatio = liveRatioSm = 1.0;
        livePeriod = unvoicedPeriod;
        modeBlend = params.liveMode ? 1.0f : 0.0f;
    }

    int getLatencySamples() const { return latency; }
    double getSampleRate() const { return sr; }
    // Live mode reports its floor; the actual delay floats up to one pitch period above it.
    int getLatencySamples (bool live) const { return live ? liveMinDelay : latency; }

    // Key learning: the GUI starts/stops, the audio thread accumulates.
    void beginKeyLearn() { keyClearRequested.store (true); keyLearning.store (true); }
    void endKeyLearn() { keyLearning.store (false); }
    bool isLearningKey() const { return keyLearning.load(); }
    // Actual live-mode delay right now (it floats within one pitch period above the floor).
    double getLiveDelaySamples() const { return liveDelayNow.load (std::memory_order_relaxed); }
    std::array<float, 12> getKeyHistogram() const
    {
        std::array<float, 12> h {};
        for (size_t i = 0; i < 12; ++i)
            h[i] = keyHist[i].load (std::memory_order_relaxed);
        return h;
    }
    void setParams (const Params& p) { params = p; }

    // Graph mode. Call setTransport once per block, before process().
    void setTrack (PitchTrack* t) { track = t; }
    void setTransport (bool playing, double hostSampleAtBlockStart, bool capture, bool apply)
    {
        hostPlaying = playing;
        hostAtBlock = hostSampleAtBlockStart;
        graphCapture = capture;
        graphApply = apply;
    }
    void setMidiMask (uint16_t m) { midiMask = m; }
    uint16_t getActiveMask() const { return activeMask.load (std::memory_order_relaxed); }

    // Single consumer (GUI thread).
    bool popDisplay (DisplayPoint& p)
    {
        const auto r = dispRead.load (std::memory_order_relaxed);
        if (r == dispWrite.load (std::memory_order_acquire))
            return false;
        p = display[r % display.size()];
        dispRead.store (r + 1, std::memory_order_release);
        return true;
    }

    void process (float* const* io, int numCh, int numSamples)
    {
        numCh = std::clamp (numCh, 1, chans);
        writtenAtBlock = written;
        const float mix = std::clamp (params.mix, 0.0f, 1.0f);
        const float gain = params.outputGain;
        const float blendTarget = params.liveMode ? 1.0f : 0.0f;
        const float blendStep = 1.0f / (float) (0.02 * sr);
        const double ratioSmooth = 1.0 - std::exp (-1.0 / (0.001 * sr));
        const float guardCoef = (float) (1.0 - std::exp (-1.0 / (0.003 * sr)));

        for (int s = 0; s < numSamples; ++s)
        {
            const auto idx = (size_t) (written & ringMask);
            float m = 0.0f;
            lpc.tick (written);
            for (int c = 0; c < numCh; ++c)
            {
                in[(size_t) c][idx] = io[c][s];
                residual[(size_t) c][idx] = lpc.whiten (c, io[c][s]);
                m += io[c][s];
            }
            mono[idx] = m / (float) numCh;
            ++written;

            if (++hopCounter >= currentHop())
            {
                hopCounter = 0;
                analyse();
            }

            // Studio path: PSOLA grains, read out `latency` samples behind.
            while (synPos + lookBehind <= (double) (written - 1))
                addGrain (numCh);

            const int64_t r = written - 1 - latency;
            const auto ri = (size_t) (r & ringMask);
            const float norm = 1.0f / std::max (wsum[ri], 0.25f);

            // Live path: pitch-synchronous delay line a few ms behind the input.
            liveRatioSm += ratioSmooth * (liveRatio - liveRatioSm);
            updateLiveHeads();
            const float fadeGain = liveFading ? (float) liveFadePos / (float) liveFadeLen : 0.0f;
            const int64_t liveDryPos = written - 1 - liveMinDelay;
            liveFormantBlend += (params.formantPreserve ? blendStep : -blendStep);
            liveFormantBlend = std::clamp (liveFormantBlend, 0.0f, 1.0f);

            if (modeBlend != blendTarget)
                modeBlend = blendTarget > modeBlend ? std::min (blendTarget, modeBlend + blendStep)
                                                    : std::max (blendTarget, modeBlend - blendStep);

            for (int c = 0; c < numCh; ++c)
            {
                float studioWet = 0.0f, studioDry = 0.0f;
                if (r >= 0)
                {
                    studioWet = acc[(size_t) c][ri] * norm;
                    studioDry = in[(size_t) c][ri];
                    acc[(size_t) c][ri] = 0.0f;
                }

                float liveWet = readAt (in[(size_t) c], liveRead);
                float liveRes = readAt (residual[(size_t) c], liveRead);
                if (liveFading)
                {
                    liveWet += fadeGain * (readAt (in[(size_t) c], liveReadB) - liveWet);
                    liveRes += fadeGain * (readAt (residual[(size_t) c], liveReadB) - liveRes);
                }
                // The synthesis filter always runs so its state is warm when Formant flips on.
                float liveFormant = lpc.colour (c, liveRes, (int64_t) liveRead);

                // Guard: the formant path may never run much hotter than the plain live path.
                auto& g = formantGuard[(size_t) c];
                g.raw += guardCoef * (liveWet * liveWet - g.raw);
                g.formant += guardCoef * (liveFormant * liveFormant - g.formant);
                const float limit = 2.0f * g.raw + 1e-9f; // +3 dB headroom
                if (g.formant > limit)
                    liveFormant *= std::sqrt (limit / g.formant);
                if (! std::isfinite (liveFormant))
                {
                    liveFormant = liveWet;
                    lpc.reset();
                }
                liveWet += liveFormantBlend * (liveFormant - liveWet);
                const float liveDry = liveDryPos >= 0 ? in[(size_t) c][(size_t) (liveDryPos & ringMask)] : 0.0f;

                const float wet = studioWet + modeBlend * (liveWet - studioWet);
                const float dry = studioDry + modeBlend * (liveDry - studioDry);
                io[c][s] = gain * (mix * wet + (1.0f - mix) * dry);
            }
            if (r >= 0)
                wsum[ri] = 0.0f;

            liveRead += liveRatioSm;
            if (liveFading)
            {
                liveReadB += liveRatioSm;
                if (++liveFadePos >= liveFadeLen)
                {
                    liveRead = liveReadB;
                    liveFading = false;
                }
            }
        }
    }

private:
    struct TimelineEntry
    {
        double time = 0.0;
        double period = 0.0;
        double ratio = 1.0;
        double formant = 1.0;
        bool voiced = false;
    };

    //==========================================================================
    void analyse()
    {
        float minF = 80.0f, maxF = 900.0f;
        switch (params.inputType)
        {
            case 0:  minF = 130.0f; maxF = 1100.0f; break;
            case 2:  minF = 60.0f;  maxF = 500.0f;  break;
            case 3:  minF = 60.0f;  maxF = 1600.0f; break;
            default: break;
        }
        const int minTau = (int) std::floor (sr / maxF);
        int maxTau = std::min ((int) std::ceil (sr / minF), (int) std::floor (pMax));

        // Live mode listens to the shortest window that still spans the longest period
        // it searches. While a voice is tracked, that range follows the voice (1.5 of its
        // periods), so high voices get short windows whatever the Input Type says.
        // Studio uses the full window for stability.
        if (params.liveMode && wasVoiced && trackedPeriod > 0.0)
            maxTau = std::clamp ((int) std::ceil (1.5 * trackedPeriod), minTau + 8, maxTau);
        const int intLen = params.liveMode ? std::max (maxTau, 256) : wi;
        const int n = PitchDetector::segmentLength (intLen, maxTau);
        const int64_t start = written - n;
        double energySum = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const int64_t p = start + i;
            const float v = p >= 0 ? mono[(size_t) (p & ringMask)] : 0.0f;
            segment[(size_t) i] = v;
            energySum += (double) v * v;
        }
        const double rms = std::sqrt (energySum / n);
        const float threshold = 0.05f + 0.30f * std::clamp (params.tracking, 0.0f, 1.0f);

        float period = 0.0f;
        if (rms > 0.0018) // about -55 dBFS
            period = detector.detect (segment.data(), intLen, minTau, maxTau, threshold);

        TimelineEntry e;
        e.time = (double) written - 0.625 * n;
        liveAnalysisLagSec = 0.5 * n / sr;
        correct (period, e);
        pushTimeline (e);

        if (e.voiced)
            trackedPeriod = trackedPeriod > 0.0 && wasVoiced ? trackedPeriod + 0.3 * (period - trackedPeriod) : period;
        else
            trackedPeriod = 0.0;

        liveRatio = e.voiced ? e.ratio : 1.0;
        livePeriod = e.voiced ? e.period : unvoicedPeriod;

        const double throatScale = std::clamp (1.0 / std::max (0.1f, params.throat), 0.667, 1.5);
        if (e.voiced)
            lpc.setSmoothing (std::clamp (1.7 * sr / e.period, 150.0, 450.0));
        lpc.update ([this] (double pos) { return readAt (mono, pos); }, (double) (written - 1), throatScale);
    }

    void correct (float period, TimelineEntry& e)
    {
        const double hopSec = currentHop() / sr;
        const auto& p = params;

        uint16_t mask = (uint16_t) (scalePitchClasses (p.key, p.scale) & ~p.removedMask & 0x0FFF);
        if (p.midiTarget && midiMask != 0)
            mask = midiMask;
        activeMask.store (mask, std::memory_order_relaxed);

        // Where on the host timeline this analysis sits (Graph mode).
        int graphIdx = -1;
        if (track != nullptr && hostPlaying)
        {
            const double hostSample = hostAtBlock + (e.time - (double) writtenAtBlock);
            const int i = PitchTrack::indexFor (hostSample / sr);
            if (PitchTrack::valid (i))
                graphIdx = i;
        }

        if (period <= 0.0f)
        {
            if (graphIdx >= 0 && graphCapture)
                track->setInput (graphIdx, NAN);
            e.voiced = false;
            e.period = unvoicedPeriod;
            wasVoiced = false;
            pushDisplay ({ 0.0f, 0.0f, -1.0f, 0 });
            return;
        }

        const float ref = std::max (1.0f, p.concertA);
        float note = 69.0f + 12.0f * std::log2 ((float) (sr / period) / ref);

        // Reject single-hop octave glitches.
        if (wasVoiced && std::abs (note - lastNote) > 7.0f && ! glitchHeld)
        {
            note = lastNote;
            glitchHeld = true;
        }
        else
        {
            glitchHeld = false;
        }

        // Live mode corrects audio that is newer than the analysis window, so extrapolate
        // the pitch trend across that gap (about half the window).
        const float measured = note;
        if (graphIdx >= 0 && graphCapture)
            track->setInput (graphIdx, measured);
        // Only small, smooth motion (vibrato, drift) is extrapolated; a note jump would
        // overshoot, so prediction is dropped for it.
        if (p.liveMode && wasVoiced && ! glitchHeld && std::abs (measured - lastNote) < 0.4f)
        {
            const float slope = (float) ((measured - lastNote) / hopSec);
            slopeSm += 0.5f * (slope - slopeSm);
            note += std::clamp (slopeSm * (float) liveAnalysisLagSec, -0.35f, 0.35f);
        }
        else
        {
            slopeSm = 0.0f;
        }

        if (! wasVoiced)
        {
            center = vibCenter1 = vibCenter2 = note;
            corr = 0.0f;
            heldSec = 0.0;
            target = -1;
        }
        else
        {
            // 3 Hz one-pole tracks the note centre for target selection.
            const float aC = 1.0f - (float) std::exp (-2.0 * kPi * 3.0 * hopSec);
            if (std::abs (note - center) > 1.5f)
                center = note - std::copysign (1.5f, note - center);
            center += aC * (note - center);

            // Steeper 2 Hz two-pole for vibrato separation (passes ~12% at 5.5 Hz).
            const float aV = 1.0f - (float) std::exp (-2.0 * kPi * 2.0 * hopSec);
            if (std::abs (note - vibCenter2) > 1.5f)
                vibCenter1 = vibCenter2 = note - std::copysign (1.5f, note - vibCenter2);
            vibCenter1 += aV * (note - vibCenter1);
            vibCenter2 += aV * (vibCenter1 - vibCenter2);
        }
        if (keyClearRequested.exchange (false))
            for (auto& h : keyHist)
                h.store (0.0f, std::memory_order_relaxed);
        if (keyLearning.load (std::memory_order_relaxed))
        {
            // Weight by how settled the note is on a semitone, so slides count little.
            const float nearest = std::round (center);
            const float w = std::max (0.0f, 1.0f - 2.0f * std::abs (center - nearest));
            auto& h = keyHist[(size_t) ((((int) nearest % 12) + 12) % 12)];
            h.store (h.load (std::memory_order_relaxed) + w * (float) hopSec, std::memory_order_relaxed);
        }

        const float vibGain = std::pow (10.0f, p.naturalVibratoDb / 20.0f);
        const float inMod = vibCenter2 + vibGain * (note - vibCenter2);

        float graphTarget = NAN;
        uint8_t graphMode = PitchTrack::kNone;
        if (graphIdx >= 0 && graphApply)
        {
            graphTarget = track->targetAt (graphIdx);
            graphMode = track->modeAt (graphIdx);
        }

        // Target selection on the vibrato-free centre, with hysteresis.
        float desired = 0.0f;
        const bool graphActive = std::isfinite (graphTarget);
        if (graphActive)
        {
            // Graph edit: the drawn curve is the output. Note edits keep the singer's vibrato.
            const float drawn = graphMode == PitchTrack::kNote ? graphTarget + (note - vibCenter2) : graphTarget;
            const int t = (int) std::lround (graphTarget);
            if (t != target)
                heldSec = 0.0;
            target = t;
            desired = drawn - inMod;
        }
        else if (mask != 0)
        {
            // Select on the smoothed centre so vibrato never flips targets, but once the
            // voice is clearly away from the current target (a real note change) use the
            // raw pitch so the new note is caught without the smoother's lag.
            const bool jumped = target >= 0 && std::abs (note - (float) target) > 1.4f;
            if (jumped)
                center = note; // re-seat the smoother so the next hop doesn't flip back
            const float sel = center;
            const int base = (int) std::lround (sel);
            int cand = -1;
            float candDist = 1e9f;
            for (int k = base - 6; k <= base + 6; ++k)
            {
                const int pc = ((k % 12) + 12) % 12;
                if (((mask >> pc) & 1) && std::abs ((float) k - sel) < candDist)
                {
                    candDist = std::abs ((float) k - sel);
                    cand = k;
                }
            }

            const bool targetValid = target >= 0 && ((mask >> (target % 12)) & 1);
            if (! targetValid || std::abs ((float) target - sel) > candDist + 0.2f)
            {
                if (cand != target)
                    heldSec = 0.0;
                target = cand;
            }

            const float dev = (float) target - inMod;

            // Expression: only pull notes that are already close to the target.
            float w = 1.0f;
            if (p.expression > 0.001f)
            {
                const float outer = 0.05f + (1.0f - p.expression) * 1.0f;
                const float inner = outer * 0.4f;
                const float ad = std::abs (dev);
                w = ad <= inner ? 1.0f : ad >= outer ? 0.0f : 1.0f - (ad - inner) / (outer - inner);
            }
            desired = w * dev;
        }
        else
        {
            target = -1;
        }

        // Retune speed: time constant ~ a third of the knob's time-to-target.
        // Humanize lengthens it on sustained notes so they keep their drift.
        double tauMs = p.retuneMs / 3.0;
        if (p.humanize > 0.0f)
        {
            const double t = std::clamp ((heldSec - 0.12) / 0.38, 0.0, 1.0);
            const double sustain = t * t * (3.0 - 2.0 * t);
            tauMs = tauMs * (1.0 + 6.0 * p.humanize * sustain) + p.humanize * sustain * 40.0;
        }
        if (graphActive)
            tauMs = 3.0; // follow drawn curves closely
        const float alpha = tauMs < 0.1 ? 1.0f : (float) (1.0 - std::exp (-hopSec / (tauMs / 1000.0)));
        corr += alpha * (desired - corr);

        float out = inMod + corr;

        if (p.vibDepthCents > 0.0f)
        {
            const double env = std::clamp ((heldSec - p.vibDelayMs / 1000.0) / 0.3, 0.0, 1.0);
            out += (float) (p.vibDepthCents / 100.0 * env * std::sin (vibPhase));
            vibPhase += 2.0 * kPi * p.vibRateHz * hopSec;
            if (vibPhase > 2.0 * kPi)
                vibPhase -= 2.0 * kPi;
        }
        else
        {
            vibPhase = 0.0;
        }

        out += p.transpose;

        const double ratio = std::clamp (std::pow (2.0, (out - note) / 12.0), 0.5, 2.0);
        double f = p.formantPreserve ? 1.0 / std::max (0.1f, p.throat) : ratio;
        f = std::max (0.667, std::min ({ f, 1.5, 1.6 * ratio }));

        e.voiced = true;
        e.period = period;
        e.ratio = ratio;
        e.formant = f;

        wasVoiced = true;
        lastNote = measured;
        heldSec += hopSec;
        pushDisplay ({ measured, out - (note - measured), (float) target, 1 });
    }

    //==========================================================================
    void pushTimeline (const TimelineEntry& e)
    {
        timeline[(size_t) tlHead] = e;
        tlHead = (tlHead + 1) % (int) timeline.size();
        tlCount = std::min (tlCount + 1, (int) timeline.size());
    }

    const TimelineEntry& tlAt (int age) const // 0 = newest
    {
        const int n = (int) timeline.size();
        return timeline[(size_t) ((tlHead - 1 - age + 2 * n) % n)];
    }

    TimelineEntry lookup (double t) const
    {
        if (tlCount == 0)
        {
            TimelineEntry e;
            e.period = unvoicedPeriod;
            return e;
        }
        for (int age = 0; age < tlCount; ++age)
        {
            const auto& e0 = tlAt (age);
            if (e0.time > t)
                continue;
            if (age == 0)
                return e0;
            const auto& e1 = tlAt (age - 1);
            if (e0.voiced != e1.voiced)
                return (t - e0.time) < (e1.time - t) ? e0 : e1;
            const double fr = (t - e0.time) / std::max (1e-9, e1.time - e0.time);
            TimelineEntry r = e0;
            r.period = e0.period + fr * (e1.period - e0.period);
            r.ratio = e0.ratio + fr * (e1.ratio - e0.ratio);
            r.formant = e0.formant + fr * (e1.formant - e0.formant);
            r.time = t;
            return r;
        }
        return tlAt (tlCount - 1);
    }

    void addGrain (int numCh)
    {
        const auto e = lookup (synPos);
        double pa = unvoicedPeriod, ratio = 1.0, f = 1.0;
        if (! e.voiced)
        {
            anaPos = synPos;
        }
        else
        {
            pa = e.period;
            ratio = e.ratio;
            f = e.formant;
            while (anaPos < synPos - 0.5 * pa)
                anaPos += pa;
            while (anaPos > synPos + 0.5 * pa)
                anaPos -= pa;
        }

        const double ls = pa / f;
        const int64_t k0 = (int64_t) std::ceil (synPos - ls);
        const int64_t k1 = (int64_t) std::floor (synPos + ls);
        for (int64_t k = std::max<int64_t> (0, k0); k <= k1; ++k)
        {
            const double i = (double) k - synPos;
            const double ip = anaPos + i * f;
            if (ip < 0.0)
                continue;
            const float w = (float) (0.5 * (1.0 + std::cos (kPi * i / ls)));
            const auto i0 = (int64_t) std::floor (ip);
            const float fr = (float) (ip - (double) i0);
            const auto a0 = (size_t) (i0 & ringMask);
            const auto a1 = (size_t) ((i0 + 1) & ringMask);
            const auto ko = (size_t) (k & ringMask);
            for (int c = 0; c < numCh; ++c)
            {
                const auto& src = in[(size_t) c];
                acc[(size_t) c][ko] += w * (src[a0] + fr * (src[a1] - src[a0]));
            }
            wsum[ko] += w;
        }

        synPos += pa / ratio;
        anaPos += pa;
    }

    // Live mode analyses twice as often so corrections land sooner.
    int currentHop() const { return params.liveMode ? std::max (32, hop / 2) : hop; }

    float readAt (const std::vector<float>& src, double pos) const
    {
        if (pos < 0.0)
            return 0.0f;
        const auto i0 = (int64_t) pos;
        const float fr = (float) (pos - (double) i0);
        const float a = src[(size_t) (i0 & ringMask)];
        const float b = src[(size_t) ((i0 + 1) & ringMask)];
        return a + fr * (b - a);
    }

    // Keeps the live read head between liveMinDelay and one period above it by
    // splicing whole periods (seamless for periodic signals) with a short crossfade.
    void updateLiveHeads()
    {
        if (liveFading)
            return;
        const double newest = (double) (written - 1);
        const double delay = newest - liveRead;
        liveDelayNow.store (delay, std::memory_order_relaxed);
        const double period = std::max (16.0, livePeriod);
        if (delay < liveMinDelay)
        {
            const double periods = std::ceil ((liveMinDelay - delay) / period);
            liveReadB = liveRead - periods * period;
        }
        else if (delay > liveMinDelay + period + 2.0)
        {
            const double periods = std::floor ((delay - liveMinDelay) / period);
            liveReadB = liveRead + periods * period;
        }
        else
        {
            return;
        }
        liveFading = true;
        liveFadePos = 0;
    }

    void pushDisplay (const DisplayPoint& p)
    {
        const auto w = dispWrite.load (std::memory_order_relaxed);
        if (w - dispRead.load (std::memory_order_acquire) >= display.size())
            return;
        display[w % display.size()] = p;
        dispWrite.store (w + 1, std::memory_order_release);
    }

    //==========================================================================
    Params params;
    PitchDetector detector;
    double sr = 48000.0, pMax = 800.0, unvoicedPeriod = 240.0;
    int chans = 2, wi = 1024, hop = 128, lookBehind = 0, latency = 0;
    int ringSize = 0;
    int64_t ringMask = 0;
    int64_t written = 0;
    int hopCounter = 0;

    std::array<std::vector<float>, kMaxChannels> in, acc, residual;
    LpcFormant lpc;
    float liveFormantBlend = 1.0f;
    struct Guard { float raw = 0.0f, formant = 0.0f; };
    std::array<Guard, kMaxChannels> formantGuard {};
    std::vector<float> mono, wsum, segment;

    std::array<TimelineEntry, 64> timeline {};
    int tlHead = 0, tlCount = 0;
    double synPos = 0.0, anaPos = 0.0;

    bool wasVoiced = false, glitchHeld = false;
    float corr = 0.0f, center = 0.0f, lastNote = 0.0f;
    float vibCenter1 = 0.0f, vibCenter2 = 0.0f, slopeSm = 0.0f;
    double liveAnalysisLagSec = 0.0, trackedPeriod = 0.0;
    int target = -1;
    double heldSec = 0.0, vibPhase = 0.0;
    uint16_t midiMask = 0;
    std::atomic<uint16_t> activeMask { 0x0FFF };

    // Live mode
    int liveFadeLen = 58, liveMinDelay = 66, liveFadePos = 0;
    double liveRead = 0.0, liveReadB = 0.0, liveRatio = 1.0, liveRatioSm = 1.0, livePeriod = 240.0;
    bool liveFading = false;
    float modeBlend = 0.0f;

    std::array<std::atomic<float>, 12> keyHist {};
    std::atomic<bool> keyLearning { false }, keyClearRequested { false };
    std::atomic<double> liveDelayNow { 0.0 };

    PitchTrack* track = nullptr;
    bool hostPlaying = false, graphCapture = false, graphApply = false;
    double hostAtBlock = 0.0;
    int64_t writtenAtBlock = 0;

    std::array<DisplayPoint, 2048> display {};
    std::atomic<size_t> dispWrite { 0 }, dispRead { 0 };
};

} // namespace tune
