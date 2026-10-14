// Checks the plug-in wrapper against hostile project state, MIDI and host input.
// Built with the plug-in sources (see CMakeLists.txt); run by ctest.

#include "PluginProcessor.h"

#include <cstdio>
#include <random>
#include <string>

static int failures = 0;

static void check (bool ok, const std::string& what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (! ok)
        ++failures;
}

// The layout AudioProcessor::copyXmlToBinary writes: magic, length, text, terminating zero.
// `declaredLength` < 0 means the real length.
static juce::MemoryBlock blobFromBytes (const std::string& bytes, int declaredLength = -1)
{
    juce::MemoryBlock b;
    juce::MemoryOutputStream out (b, false);
    out.writeInt (0x21324356);
    out.writeInt (declaredLength < 0 ? (int) bytes.size() : declaredLength);
    out.write (bytes.data(), bytes.size());
    out.writeByte (0);
    out.flush();
    return b;
}

static juce::MemoryBlock blobFromText (const juce::String& text) { return blobFromBytes (text.toStdString()); }

static std::string repeated (const std::string& s, int n)
{
    std::string r;
    r.reserve (s.size() * (size_t) n);
    for (int i = 0; i < n; ++i)
        r += s;
    return r;
}

static float value (FlayrTuneProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }

static void setValue (FlayrTuneProcessor& p, const char* id, float v)
{
    auto* param = p.apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (v));
}

// Runs `seconds` of a 220 Hz tone through processBlock; returns the peak and whether all was finite.
static float play (FlayrTuneProcessor& p, double seconds, bool& finite, juce::MidiBuffer* midi = nullptr)
{
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer none;
    double phase = 0.0;
    float peak = 0.0f;
    finite = true;
    for (int block = 0; block < (int) (seconds * 48000.0 / 512); ++block)
    {
        for (int i = 0; i < 512; ++i)
        {
            phase += 2.0 * juce::MathConstants<double>::pi * 220.0 / 48000.0;
            buf.setSample (0, i, 0.3f * (float) std::sin (phase));
            buf.setSample (1, i, buf.getSample (0, i));
        }
        p.processBlock (buf, midi != nullptr && block == 0 ? *midi : none);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 512; ++i)
            {
                const float v = buf.getSample (c, i);
                finite &= std::isfinite (v);
                if (std::isfinite (v) && block > (int) (seconds * 48000.0 / 512) / 2)
                    peak = std::max (peak, std::abs (v));
            }
    }
    return peak;
}

struct FakeHead : juce::AudioPlayHead
{
    juce::Optional<juce::int64> samples;
    juce::Optional<double> secs;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (true);
        info.setTimeInSamples (samples);
        info.setTimeInSeconds (secs);
        return info;
    }
};

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); // CI shows the last test reached, even on a crash
    juce::ScopedJuceInitialiser_GUI juceInit;

    {
        std::puts ("Hostile XML in project state is refused before parsing");
        FlayrTuneProcessor p;
        p.prepareToPlay (48000.0, 512);
        setValue (p, "retune", 123.0f);

        auto expectIgnored = [&p] (const juce::MemoryBlock& b, const std::string& what)
        {
            p.setStateInformation (b.getData(), (int) b.getSize());
            check (std::abs (value (p, "retune") - 123.0f) < 0.01f, what);
        };

        // Self-referencing entity: JUCE expands it recursively until the stack overflows.
        const auto pad = juce::String::repeatedString ("x", 64);
        expectIgnored (blobFromText ("<!DOCTYPE FlayrTune [ <!ENTITY a \"&" + pad + "a;\"> ]><FlayrTune x=\"&a;\">"
                                     "<PARAM id=\"retune\" value=\"5\"/></FlayrTune>"),
                       "recursive <!ENTITY> refused, no crash");
        expectIgnored (blobFromText ("<FlayrTune><!-- hi --><PARAM id=\"retune\" value=\"5\"/></FlayrTune>"),
                       "comments refused");

        // A million levels of nesting: JUCE recurses once per level.
        constexpr int depth = 1000000;
        juce::String deep;
        deep.preallocateBytes ((size_t) depth * 7 + 64);
        deep << "<FlayrTune>" << juce::String::repeatedString ("<a>", depth)
             << juce::String::repeatedString ("</a>", depth) << "</FlayrTune>";
        expectIgnored (blobFromText (deep), "1,000,000-deep nesting refused, no crash");
        expectIgnored (blobFromText ("<FlayrTune><a><b><c><d><e><f><g><h><PARAM id=\"retune\" value=\"5\"/>"
                                     "</h></g></f></e></d></c></b></a></FlayrTune>"),
                       "nesting deeper than 8 refused");

        auto good = blobFromText ("<FlayrTune><PARAM id=\"retune\" value=\"5\"/></FlayrTune>");
        auto badMagic = good;
        static_cast<char*> (badMagic.getData())[0] ^= 1;
        expectIgnored (badMagic, "wrong header magic refused");
        auto badLength = good;
        static_cast<uint32_t*> (badLength.getData())[1] = 0x7fffffff;
        expectIgnored (badLength, "length beyond the data refused");
        auto expectNoCrash = [&p] (const juce::MemoryBlock& b, const std::string& what)
        {
            p.setStateInformation (b.getData(), (int) b.getSize());
            check (true, what); // reaching this line is the test
            setValue (p, "retune", 123.0f);
        };
        expectNoCrash (blobFromText ("<FlayrTune><PARAM id=\"retune\" value=\"5\"/>"), "unbalanced tags: no crash");

        // The scan never interprets quotes, so these must not fool it into missing depth.
        const std::string param = "<PARAM id=\"retune\" value=\"5\"/>";
        expectIgnored (blobFromBytes ("<FlayrTune>" + param + "<PARAM id=\"x\" value=\"<<<<<<<<<<\"/></FlayrTune>"),
                       "'<' inside attribute values counts as nesting");
        expectNoCrash (blobFromBytes ("<FlayrTune><PARAM id=\"retune\" value=\"5\" x=\"a>b>c\"/></FlayrTune>"),
                       "'>' inside an attribute: no crash");
        expectIgnored (blobFromBytes ("<FlayrTune><PARAM id=\"retune value=\"5\"/>" + repeated ("<a>", 100000)
                                      + repeated ("</a>", 100000) + "</FlayrTune>"),
                       "unterminated quote before deep nesting refused");
        expectIgnored (blobFromBytes ("<FlayrTune>" + repeated ("<a x=\"/>/>/>/>/>/>/>/>/>/>\">", 100000)
                                      + repeated ("</a>", 100000) + "</FlayrTune>"),
                       "'/>' inside attribute strings can't hide 100,000 levels (tag cap)");
        // Under the tag cap, "/>" in attributes can hide depth from the estimate, but JUCE
        // then recurses at most 256 levels, which is harmless.
        expectNoCrash (blobFromBytes ("<FlayrTune>" + repeated ("<a x=\"/>/>\">", 200) + repeated ("</a>", 200)
                                      + "</FlayrTune>"),
                       "hidden depth bounded by the tag cap: no crash");
        expectIgnored (blobFromBytes ("<FlayrTune>" + param + "</FlayrTune>" + std::string (1, '\0')
                                      + repeated ("<a>", 100000) + repeated ("</a>", 100000)),
                       "NUL inside the declared length refused");
        const std::string small = "<FlayrTune>" + param + "</FlayrTune>";
        auto trailing = blobFromBytes (small + repeated ("<a>", 1000000), (int) small.size());
        p.setStateInformation (trailing.getData(), (int) trailing.getSize());
        check (std::abs (value (p, "retune") - 5.0f) < 0.01f, "nesting past the declared length is never parsed");
        setValue (p, "retune", 123.0f);
        expectIgnored (blobFromBytes ("\xEF\xBB\xBF" + small), "UTF-8 BOM refused (state is plain ASCII)");
        expectIgnored (blobFromBytes (std::string ("<\0F\0l\0a\0y\0r\0T\0u\0n\0e\0>\0", 22)), "UTF-16 refused");
        expectIgnored (blobFromBytes ("<FlayrTune>" + repeated ("\xC0\xBC" "a>", 100000) + "</FlayrTune>"),
                       "overlong UTF-8 '<' refused");
        expectIgnored (blobFromBytes ("<FlayrTune><?pi x?>" + param + "</FlayrTune>"), "'<?' after the declaration refused");
        expectIgnored (blobFromBytes ("<?xml version=\"1.0\"" + small), "unterminated declaration refused");
        expectIgnored (blobFromBytes ("<?xml version=\"1.0\" \xC0\xBE<!DOCTYPE x>?>" + small),
                       "non-ASCII inside the declaration refused");
        auto declared = blobFromBytes ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>" + small);
        p.setStateInformation (declared.getData(), (int) declared.getSize());
        check (std::abs (value (p, "retune") - 5.0f) < 0.01f, "a leading <?xml?> declaration is accepted");
        setValue (p, "retune", 123.0f);
        juce::MemoryBlock huge (17 * 1024 * 1024, true);
        expectIgnored (huge, "17 MB blob refused");

        p.setStateInformation (good.getData(), (int) good.getSize());
        check (std::abs (value (p, "retune") - 5.0f) < 0.01f, "a well-formed state still loads");
    }

    {
        std::puts ("A full 30-minute PitchTrack round-trips under the size cap");
        FlayrTuneProcessor a;
        std::mt19937 rng (7);
        std::uniform_real_distribution<float> note (36.0f, 84.0f);
        for (int i = 0; i < tune::PitchTrack::kCapacity; ++i)
        {
            a.track.setInput (i, note (rng));
            a.track.setTarget (i, note (rng), (i & 1) ? tune::PitchTrack::kExact : tune::PitchTrack::kNote);
        }
        setValue (a, "retune", 77.0f);
        juce::MemoryBlock state;
        a.getStateInformation (state);
        std::printf ("  saved state: %.2f MB\n", (double) state.getSize() / (1024.0 * 1024.0));
        check (state.getSize() < 8u * 1024 * 1024, "worst-case state leaves 2x headroom under the 16 MB cap");

        FlayrTuneProcessor b;
        b.setStateInformation (state.getData(), (int) state.getSize());
        const int last = tune::PitchTrack::kCapacity - 1;
        check (b.track.getExtent() == tune::PitchTrack::kCapacity
                   && b.track.inputAt (last) == a.track.inputAt (last)
                   && b.track.targetAt (last) == a.track.targetAt (last)
                   && std::abs (value (b, "retune") - 77.0f) < 0.01f,
               "track and parameters restored");
    }

    {
        std::puts ("NaN / Inf in saved parameters becomes the default");
        FlayrTuneProcessor src;
        juce::MemoryBlock state;
        src.getStateInformation (state);
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        for (auto* e : xml->getChildIterator())
        {
            const auto id = e->getStringAttribute ("id");
            if (id == "retune" || id == "mix" || id == "key" || id == "scale" || id == "inputType")
                e->setAttribute ("value", "nan");
            else if (id == "output" || id == "concertA")
                e->setAttribute ("value", "inf");
            else if (id == "transpose")
                e->setAttribute ("value", "-1e300");
        }
        // A 1.2 project: legacy "throat" instead of "formantShift".
        xml->removeChildElement (xml->getChildByAttribute ("id", "formantShift"), true);
        auto* throat = xml->createNewChildElement ("PARAM");
        throat->setAttribute ("id", "throat");
        throat->setAttribute ("value", "nan");
        juce::MemoryBlock evil;
        juce::AudioProcessor::copyXmlToBinary (*xml, evil);

        FlayrTuneProcessor p;
        p.prepareToPlay (48000.0, 512);
        setValue (p, "mix", 40.0f);
        p.setStateInformation (evil.getData(), (int) evil.getSize());
        bool allFinite = true;
        for (auto* param : p.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (param))
                allFinite &= std::isfinite (p.apvts.getRawParameterValue (r->paramID)->load());
        check (allFinite, "every parameter is finite after loading");
        check (value (p, "retune") == 20.0f && value (p, "mix") == 100.0f && value (p, "inputType") == 1.0f
                   && std::abs (value (p, "formantShift")) < 1e-4f,
               "non-finite values replaced by defaults (throat migration too)");
        bool finite;
        const float peak = play (p, 2.0, finite);
        std::printf ("  output peak %.3f\n", peak);
        check (finite && peak > 0.05f, "output is finite and audible");

        // And if a host injects NaN straight into a parameter, the audio still survives.
        p.apvts.getRawParameterValue ("mix")->store (NAN);
        p.apvts.getRawParameterValue ("key")->store (NAN);
        p.apvts.getRawParameterValue ("output")->store (INFINITY);
        const float peak2 = play (p, 2.0, finite);
        check (finite && peak2 > 0.05f, "NaN injected at runtime: output finite and audible");
    }

    {
        std::puts ("MIDI note numbers outside 0..127 are ignored");
        FlayrTuneProcessor p;
        p.prepareToPlay (48000.0, 512);
        p.setCurrentProgram (0);
        setValue (p, "midiTarget", 1.0f);
        juce::MidiBuffer midi;
        for (int n = 128; n < 256; ++n)
            midi.addEvent (juce::MidiMessage (0x90, n, 100), 0); // raw bytes, as AU / LV2 deliver them
        bool finite;
        play (p, 0.2, finite, &midi);
        check (p.getCurrentProgram() == 0, "notes 128..255 wrote nothing past the held-note table");
        check (p.engine.getActiveMask() == 0x0FFF, "no phantom MIDI target");
        juce::MidiBuffer off;
        for (int n = 128; n < 256; ++n)
            off.addEvent (juce::MidiMessage (0x80, n, 0), 0);
        play (p, 0.1, finite, &off);
        check (p.getCurrentProgram() == 0, "note-offs 128..255 ignored too");
    }

    {
        std::puts ("Absurd host positions keep the playhead finite and bounded");
        FlayrTuneProcessor p;
        p.prepareToPlay (48000.0, 512);
        FakeHead head;
        p.setPlayHead (&head);
        bool finite;
        auto playhead = [&p] { return p.playheadSeconds.load(); };
        head.samples = std::numeric_limits<juce::int64>::max();
        play (p, 0.05, finite);
        check (playhead() <= FlayrTuneProcessor::kMaxPlayheadSeconds, "int64 max samples");
        head.samples = {};
        head.secs = 1e300;
        play (p, 0.05, finite);
        check (std::isfinite (playhead()) && playhead() <= FlayrTuneProcessor::kMaxPlayheadSeconds, "1e300 seconds");
        head.secs = (double) NAN;
        play (p, 0.05, finite);
        check (playhead() == 0.0 && finite, "NaN seconds");
        p.setPlayHead (nullptr);
    }

    {
        std::puts ("Presets and unsupported rates");
        FlayrTuneProcessor p;
        p.setCurrentProgram (2); // Hard Tune
        check (value (p, "retune") == 0.0f && p.getCurrentProgram() == 2, "preset applies through cached parameters");
        p.setCurrentProgram (7); // Chipmunk
        check (value (p, "transpose") == 12.0f && value (p, "formant") == 0.0f, "second preset applies");

        for (double rate : { 0.0, -1.0, (double) NAN, 1e9 })
        {
            p.prepareToPlay (rate, 512);
            bool finite;
            const float peak = play (p, 0.2, finite);
            check (finite && peak == 0.0f && p.getLatencySamples() == 0,
                   "prepareToPlay (" + std::to_string (rate) + ") -> silence");
        }
        p.prepareToPlay (48000.0, 512);
        bool finite;
        check (play (p, 1.0, finite) > 0.05f && finite, "back to normal at 48 kHz");
    }

    {
        std::puts ("No networking (FLAYR_NO_NETWORK): the JUCE API links but fails at once");
       #if ! FLAYR_NO_NETWORK
        check (false, "built without FLAYR_NO_NETWORK");
       #endif
        // Port 9 / a reserved address: nothing would answer even if the stubs were missing.
        juce::WebInputStream web (juce::URL ("http://192.0.2.1:9/"), false);
        check (! web.connect (nullptr) && web.isError() && web.read (nullptr, 0) <= 0, "WebInputStream never connects");
        check (juce::URL ("http://192.0.2.1:9/").createInputStream (juce::URL::InputStreamOptions (
                   juce::URL::ParameterHandling::inAddress).withConnectionTimeoutMs (100)) == nullptr,
               "URL::createInputStream returns nothing");
        juce::StreamingSocket tcp;
        check (! tcp.connect ("192.0.2.1", 9, 100) && ! tcp.createListener (0) && ! tcp.bindToPort (0), "no TCP socket");
        juce::DatagramSocket udp;
        check (! udp.bindToPort (0) && udp.write ("192.0.2.1", 9, "x", 1) < 0, "no UDP socket");
        check (juce::IPAddress::getAllAddresses().isEmpty() && juce::MACAddress::getAllAddresses().isEmpty(),
               "no interface enumeration");
    }

    std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
