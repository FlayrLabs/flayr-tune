// Shared helpers for the fuzz targets (FLAYR_TUNE_BUILD_FUZZERS; see tests/fuzz/README.md).
#pragma once

#include "PluginProcessor.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fuzz
{
// Reads typed values off the front of the fuzz input; returns zeros once it runs dry,
// so every input, however short, is a valid program.
struct Reader
{
    const uint8_t* p;
    size_t n;

    bool empty() const { return n == 0; }

    template <typename T>
    T get()
    {
        T v {};
        const size_t k = std::min (sizeof (T), n);
        std::memcpy (&v, p, k);
        p += k;
        n -= k;
        return v;
    }
    uint8_t u8() { return get<uint8_t>(); }
    uint16_t u16() { return get<uint16_t>(); }
    uint32_t u32() { return get<uint32_t>(); }
    bool flag() { return (u8() & 1) != 0; }

    // A double that is usually "interesting" (edge values) and sometimes raw bits.
    double number (std::initializer_list<double> table)
    {
        const auto sel = u8();
        if (sel < 160)
            return table.begin()[sel % table.size()];
        return get<double>();
    }

    juce::MemoryBlock rest()
    {
        juce::MemoryBlock b (p, n);
        p += n;
        n = 0;
        return b;
    }
};

// The layout AudioProcessor::copyXmlToBinary writes: magic, length, text, terminating zero.
inline juce::MemoryBlock wrapXml (const void* text, size_t size)
{
    juce::MemoryBlock b;
    juce::MemoryOutputStream out (b, false);
    out.writeInt (0x21324356);
    out.writeInt ((int) size);
    out.write (text, size);
    out.writeByte (0);
    out.flush();
    return b;
}

inline juce::String gzipBase64 (const void* data, size_t size)
{
    juce::MemoryOutputStream packed;
    {
        juce::GZIPCompressorOutputStream gz (packed, 6);
        gz.write (data, size);
    }
    return packed.getMemoryBlock().toBase64Encoding();
}

// Our own state with a PitchTrack whose (base64) data attribute is `trackData`.
inline juce::MemoryBlock stateWithTrack (const juce::String& trackData)
{
    const juce::String xml = "<FlayrTune><PARAM id=\"graph\" value=\"1.0\"/><PARAM id=\"retune\" value=\"0.0\"/>"
                             "<PitchTrack data=\"" + trackData + "\"/></FlayrTune>";
    return wrapXml (xml.toRawUTF8(), xml.getNumBytesAsUTF8());
}

[[noreturn]] inline void fail (const char* what)
{
    std::fprintf (stderr, "\n==FUZZ== invariant violated: %s\n", what);
    std::abort();
}

inline void require (bool ok, const char* what)
{
    if (! ok)
        fail (what);
}

// After loading any state: every parameter finite and in range.
inline void checkParameters (FlayrTuneProcessor& p)
{
    for (auto* param : p.getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (param))
        {
            const float v = p.apvts.getRawParameterValue (r->paramID)->load();
            const auto& range = r->getNormalisableRange();
            require (std::isfinite (v), "parameter not finite");
            require (v >= range.start - 1e-3f && v <= range.end + 1e-3f, "parameter out of range");
            const float norm = r->getValue();
            require (std::isfinite (norm) && norm >= 0.0f && norm <= 1.0f, "normalised parameter out of 0..1");
        }
}

// After anything at all: the track and the playhead the GUI reads are sane.
inline void checkTrack (FlayrTuneProcessor& p)
{
    const int extent = p.track.getExtent();
    require (extent >= 0 && extent <= tune::PitchTrack::kCapacity, "track extent out of range");
    for (int i = 0; i < extent; ++i)
    {
        const float in = p.track.inputAt (i), tg = p.track.targetAt (i);
        const auto m = p.track.modeAt (i);
        require (! std::isfinite (in) || (in >= 0.0f && in <= 127.0f), "track input outside MIDI range");
        require (! std::isfinite (tg) || (tg >= 0.0f && tg <= 127.0f), "track target outside MIDI range");
        require (m == tune::PitchTrack::kNone || m == tune::PitchTrack::kExact || m == tune::PitchTrack::kNote,
                 "unknown track mode");
        require (m == tune::PitchTrack::kNone || std::isfinite (tg), "edit without a target");
    }
    const double head = p.playheadSeconds.load();
    require (std::isfinite (head) && head >= 0.0 && head <= FlayrTuneProcessor::kMaxPlayheadSeconds, "playhead out of range");
}

inline void checkProcessor (FlayrTuneProcessor& p)
{
    checkParameters (p);
    checkTrack (p);
}

inline void checkAudio (const juce::AudioBuffer<float>& buf)
{
    for (int c = 0; c < buf.getNumChannels(); ++c)
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            const float v = buf.getSample (c, i);
            require (std::isfinite (v), "non-finite output sample");
            require (std::abs (v) <= tune::Engine::kOutputLimit, "output above the hard limit");
        }
}

// Never torn down: as a function-local static it would be destroyed after JUCE's own
// statics at exit (a double free in DeletedAtShutdown), and fuzzers just exit anyway.
inline void initJuce()
{
    static auto* init = new juce::ScopedJuceInitialiser_GUI();
    (void) init;
}
} // namespace fuzz
