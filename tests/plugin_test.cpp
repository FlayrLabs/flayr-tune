// Checks the plug-in wrapper against hostile project state, MIDI and host input.
// Built with the plug-in sources (see CMakeLists.txt); run by ctest.

#include "PluginProcessor.h"
#include "state_differential.h"

#include <cstdio>
#include <cstring>
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
        // Found by fuzz_state: JUCE stepped over the terminating null after a trailing
        // "&#x" and kept parsing past the end of the buffer (a heap over-read).
        expectIgnored (blobFromBytes ("<e>&#x"), "trailing '&#x' refused (fuzz: heap over-read in JUCE)");
        expectIgnored (blobFromBytes (small + "&#x4"), "trailing '&#x4' refused");
        expectIgnored (blobFromBytes ("<FlayrTune><PARAM id=\"retune\" value=\"&#x"), "'&#x' at the end of an attribute refused");
        expectIgnored (blobFromBytes ("<FlayrTune><PARAM id=\"retune\" value=\"&#x35\"/></FlayrTune>"),
                       "unterminated character reference refused");
        expectIgnored (blobFromBytes ("<FlayrTune><PARAM id=\"retune\" value=\"&#;\"/></FlayrTune>"),
                       "empty character reference refused");
        for (const char* text : { "<e>&#x", "<e>&#x4", "<e a=\"&#x", "<e>&#xZ</e>" })
            juce::parseXML (juce::String (text)); // under ASan: patched JUCE stops at the end
        check (true, "patched JUCE parser stays inside the buffer");
        // Found by fuzz_state: JUCE's number parser overflowed an int on a huge exponent
        // (undefined behaviour; patches/juce-double-exponent-overflow.patch).
        for (const char* v : { "1e99999999999999999999", "-1e-99999999999999999999", "5e+2147483648" })
        {
            auto huge = blobFromBytes (std::string ("<FlayrTune><PARAM id=\"retune\" value=\"") + v + "\"/></FlayrTune>");
            p.setStateInformation (huge.getData(), (int) huge.getSize());
            check (std::isfinite (value (p, "retune")), std::string ("value ") + v + " loads as a finite number");
        }
        setValue (p, "retune", 123.0f);
        // Found by fuzz_state (timeout): for an '&' that isn't a known entity JUCE scans the
        // rest of the text for ';', so megabytes of '&' took quadratic time (a host hang).
        {
            const auto start = juce::Time::getMillisecondCounterHiRes();
            expectIgnored (blobFromBytes ("<FlayrTune>" + std::string (4 * 1024 * 1024, '&') + ";</FlayrTune>"),
                           "4 MB of '&' refused");
            expectIgnored (blobFromBytes ("<FlayrTune><PARAM id=\"retune\" value=\"&foo;\"/></FlayrTune>"),
                           "unknown named entity refused");
            check (juce::Time::getMillisecondCounterHiRes() - start < 2000.0, "and refused quickly");
        }
        auto named = blobFromBytes ("<FlayrTune><PARAM id=\"retune\" value=\"5\" x=\"&amp;&lt;&gt;&quot;&apos;\"/></FlayrTune>");
        p.setStateInformation (named.getData(), (int) named.getSize());
        check (std::abs (value (p, "retune") - 5.0f) < 0.01f, "the five XML entities still load");
        setValue (p, "retune", 123.0f);
        auto refs = blobFromBytes ("<FlayrTune><PARAM id=\"&#114;etune\" value=\"&#x35;\"/></FlayrTune>");
        p.setStateInformation (refs.getData(), (int) refs.getSize());
        check (std::abs (value (p, "retune") - 5.0f) < 0.01f, "complete character references still load");
        setValue (p, "retune", 123.0f);
        juce::MemoryBlock huge (17 * 1024 * 1024, true);
        expectIgnored (huge, "17 MB blob refused");
        expectIgnored (blobFromBytes ("<FlayrTune>" + param + std::string ((size_t) FlayrTuneProcessor::kMaxStateBytes, ' ')
                                      + "</FlayrTune>"),
                       "state over the cap refused");

        // Found by review: attribute names "base64:<x>" are decoded by ValueTree::fromXml,
        // which allocated whatever size the text claimed (std::bad_alloc escaped for -1).
        for (const char* claim : { "-1.AAAA", "2000000000.A", "99999999999999999999.A" })
            expectIgnored (blobFromBytes (std::string ("<FlayrTune><PARAM id=\"retune\" value=\"5\" base64:x=\"") + claim
                                          + "\"/></FlayrTune>"),
                           std::string ("base64: attribute claiming ") + claim + " refused");
        expectIgnored (blobFromBytes ("<FlayrTune><PARAM id=\"retune\" value=\"5\"/><a:b/></FlayrTune>"), "any ':' refused");

        // Found by review: quadratic time inside JUCE (a String append per entity, a global
        // pool insert per attribute name). The caps keep the worst accepted input fast.
        auto timed = [&p] (const std::string& xml)
        {
            auto b = blobFromBytes (xml);
            const auto start = juce::Time::getMillisecondCounterHiRes();
            p.setStateInformation (b.getData(), (int) b.getSize());
            return juce::Time::getMillisecondCounterHiRes() - start;
        };
        auto amps = [] (int n) { return "<FlayrTune><PARAM id=\"retune\" value=\"5\" z=\"" + repeated ("&amp;", n) + "\"/></FlayrTune>"; };
        auto names = [] (int n)
        {
            std::string s = "<FlayrTune><PARAM id=\"retune\" value=\"5\"";
            for (int i = 0; i < n; ++i)
                s += " q" + std::to_string (i) + "=''";
            return s + "/></FlayrTune>";
        };
        expectIgnored (blobFromBytes (amps (257)), "257 '&' refused");
        expectIgnored (blobFromBytes (names (511)), "513 '=' refused");
        double worst = 0.0;
        worst = std::max (worst, timed (amps (900000))); // was 174 s for 3.3M
        worst = std::max (worst, timed (names (400000))); // was 155 s for 1.3M
        setValue (p, "retune", 123.0f);
        worst = std::max (worst, timed (amps (256)));
        check (std::abs (value (p, "retune") - 5.0f) < 0.01f, "256 entities still load");
        setValue (p, "retune", 123.0f);
        worst = std::max (worst, timed (names (510)));
        check (std::abs (value (p, "retune") - 5.0f) < 0.01f, "510 extra attributes still load");
        setValue (p, "retune", 123.0f);
        // The largest text an accepted state can hold, as one attribute value.
        worst = std::max (worst, timed ("<FlayrTune><PARAM id=\"retune\" value=\"5\" z=\""
                                        + std::string ((size_t) FlayrTuneProcessor::kMaxStateBytes - 200, 'x') + "\"/></FlayrTune>"));
        check (std::abs (value (p, "retune") - 5.0f) < 0.01f, "a 5 MB attribute still loads");
        setValue (p, "retune", 123.0f);
        std::printf ("  slowest of these: %.0f ms\n", worst);
        check (worst < 2000.0, "every one finishes within 2 s");

        // patches/juce-xml-depth-limit.patch: JUCE's parser itself refuses deep nesting.
        {
            auto nested = [] (int levels) { return juce::String (repeated ("<a>", levels) + repeated ("</a>", levels)); };
            check (juce::parseXML (nested (64)) != nullptr, "JUCE parses 64 nested levels");
            check (juce::parseXML (nested (65)) == nullptr && juce::parseXML (nested (100000)) == nullptr,
                   "and refuses 65 or 100,000 (no recursion past the limit)");
        }

        // The JUCE fix itself, for every caller (ValueTree::fromXml, the LV2 wrapper, ...).
        {
            juce::MemoryBlock m;
            check (! m.fromBase64Encoding ("-1.AAAA") && ! m.fromBase64Encoding ("2000000000.A")
                       && ! m.fromBase64Encoding ("99999999999999999999.A") && ! m.fromBase64Encoding ("x1.AAAA")
                       && m.getSize() < 16,
                   "patched fromBase64Encoding refuses impossible byte counts");
            juce::MemoryBlock original ("Flayr Tune", 10), decoded;
            check (decoded.fromBase64Encoding (original.toBase64Encoding()) && decoded == original,
                   "and still decodes what it encodes");
            juce::XmlElement e ("PARAM");
            e.setAttribute ("base64:x", "2000000000.A");
            const auto t = juce::ValueTree::fromXml (e);
            check (t.isValid(), "ValueTree::fromXml with a lying base64: attribute: no huge allocation");
        }

        // Found by review: "/>" in text lowered the nesting estimate without closing
        // anything; the tag cap is what bounds nesting, and loading keeps nothing nested.
        {
            const std::string deep = "<FlayrTune>" + param + repeated ("/>", 120) + repeated ("<a>", 120) + repeated ("</a>", 120)
                                     + "</FlayrTune>";
            auto b = blobFromBytes (deep);
            p.setStateInformation (b.getData(), (int) b.getSize());
            juce::MemoryBlock saved;
            p.getStateInformation (saved);
            auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
            check (xml != nullptr && xml->getChildByName ("a") == nullptr, "120 hidden levels: harmless, not saved back");
            setValue (p, "retune", 123.0f);
        }

        p.setStateInformation (good.getData(), (int) good.getSize());
        check (std::abs (value (p, "retune") - 5.0f) < 0.01f, "a well-formed state still loads");
    }

    {
        // Found by fuzz_state (save -> load -> save oracle): parameters missing from a
        // project kept this instance's values and were then saved without a value.
        std::puts ("Loading replaces the whole state");
        FlayrTuneProcessor p;
        setValue (p, "retune", 123.0f);
        setValue (p, "mix", 40.0f);
        setValue (p, "live", 1.0f);
        auto partial = blobFromText ("<FlayrTune><PARAM id=\"graph\" value=\"1\"/><PARAM id=\"key\"/></FlayrTune>");
        p.setStateInformation (partial.getData(), (int) partial.getSize());
        check (value (p, "retune") == 20.0f && value (p, "mix") == 100.0f && value (p, "live") == 0.0f
                   && value (p, "graph") == 1.0f && value (p, "key") == 0.0f,
               "parameters the project doesn't mention go to their defaults");
        juce::MemoryBlock saved, again;
        p.getStateInformation (saved);
        auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
        bool allValued = xml != nullptr;
        for (auto* param : p.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (param))
                allValued &= xml != nullptr && xml->getChildByAttribute ("id", r->paramID) != nullptr
                             && xml->getChildByAttribute ("id", r->paramID)->hasAttribute ("value");
        check (allValued, "every parameter is saved with a value");
        FlayrTuneProcessor q;
        setValue (q, "retune", 300.0f);
        q.setStateInformation (saved.getData(), (int) saved.getSize());
        q.getStateInformation (again);
        check (again == saved, "save -> load -> save is stable, whatever the loading instance held");

        // Found by fuzz_state: '/>' inside attributes hid real nesting from the pre-scan; the
        // junk was kept, saved back properly nested (deeper than the scan allows), and the
        // project then refused to load after its first save. Unknown content is now dropped.
        auto junk = blobFromText ("<FlayrTune x=\"1\"><PARAM id=\"retune\" value=\"7\"/><P d=\"\"><a x=\"\"><a x=\"\">"
                                  "<a x=\"/>\"><a><a x=\"\"><a x=\"\"><a></a></a></a></a></a></a></a></P></FlayrTune>");
        FlayrTuneProcessor j;
        j.setStateInformation (junk.getData(), (int) junk.getSize());
        juce::MemoryBlock junkSaved;
        j.getStateInformation (junkSaved);
        FlayrTuneProcessor k;
        k.setStateInformation (junkSaved.getData(), (int) junkSaved.getSize());
        auto junkXml = juce::AudioProcessor::getXmlFromBinary (junkSaved.getData(), (int) junkSaved.getSize());
        bool onlyParams = junkXml != nullptr && junkXml->getNumAttributes() == 0;
        for (auto* e : junkXml->getChildIterator())
            onlyParams &= e->hasTagName ("PARAM") && e->getNumChildElements() == 0;
        check (value (j, "retune") == 7.0f && value (k, "retune") == 7.0f, "a project with junk nesting reloads after saving");
        check (onlyParams, "only our parameters are saved back");
    }

    {
        // Found by fuzz_state: JUCE's base64 decoder allocates the byte count the text
        // declares before reading it, so a tiny project could ask for 2 GB, or for SIZE_MAX
        // (std::bad_alloc thrown into the host).
        std::puts ("PitchTrack base64 with a lying byte count");
        for (const char* data : { "-1.AAAA", "2000000000.AAAA", "99999999999999999999.AAAA", "8.AAAA", "1x.AAAA", ".AAAA" })
        {
            FlayrTuneProcessor p;
            p.track.setInput (3, 60.0f);
            auto blob = blobFromText (juce::String ("<FlayrTune><PARAM id=\"retune\" value=\"5\"/><PitchTrack data=\"") + data
                                      + "\"/></FlayrTune>");
            p.setStateInformation (blob.getData(), (int) blob.getSize());
            check (std::abs (value (p, "retune") - 5.0f) < 0.01f && p.track.getExtent() == 0,
                   std::string ("\"") + data + "\": parameters load, track empty, nothing huge allocated");
        }
    }

    {
        std::puts ("A full 30-minute PitchTrack round-trips under the size cap");
        FlayrTuneProcessor a;
        // Worst case for the size cap: every float a random bit pattern within 0..127 (as
        // close to incompressible as decodeTrack accepts) and random edit modes.
        std::mt19937 rng (7);
        auto randomNote = [&rng]
        {
            for (;;)
            {
                const uint32_t bits = rng() & 0x7fffffffu;
                float f;
                std::memcpy (&f, &bits, sizeof (f));
                if (f <= 127.0f)
                    return f;
            }
        };
        for (int i = 0; i < tune::PitchTrack::kCapacity; ++i)
        {
            a.track.setInput (i, randomNote());
            a.track.setTarget (i, randomNote(), (rng() & 1) ? tune::PitchTrack::kExact : tune::PitchTrack::kNote);
        }
        setValue (a, "retune", 77.0f);
        juce::MemoryBlock state;
        a.getStateInformation (state);
        std::printf ("  worst-case saved state: %.2f MB (cap %.2f MB)\n", (double) state.getSize() / (1024.0 * 1024.0),
                     FlayrTuneProcessor::kMaxStateBytes / (1024.0 * 1024.0));
        check (state.getSize() < (size_t) FlayrTuneProcessor::kMaxStateBytes * 9 / 10, "worst-case state fits the cap with 10% to spare");

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

        // Found by fuzz_host: finite but absurd values (1e30 in a choice parameter was cast
        // to int, undefined behaviour) and NaN pushed through the host API (JUCE then cast
        // NaN to int while writing the state).
        // (Not transpose: PSOLA moves a pure test tone up onto its own spectral envelope,
        // which is silent above the tone, so the check below would fail for a good reason.)
        for (auto* id : { "key", "scale", "inputType", "natvib", "concertA", "vibRate", "vibDepth", "humanize" })
            p.apvts.getRawParameterValue (id)->store (1e30f);
        const float peak3 = play (p, 1.0, finite);
        check (finite && peak3 > 0.05f, "1e30 in parameters: output finite and audible");
        p.apvts.getParameter ("retune")->setValueNotifyingHost (NAN);
        juce::MemoryBlock saved;
        p.getStateInformation (saved);
        auto savedXml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
        check (savedXml != nullptr && ! savedXml->toString().containsIgnoreCase ("nan"), "a NaN parameter is saved as its default");
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
        std::puts ("Bypass keeps the reported latency");
        FlayrTuneProcessor p;
        p.prepareToPlay (48000.0, 256);
        const int latency = p.getLatencySamples();
        juce::MidiBuffer m;
        int heardAt = -1;
        for (int block = 0; block < 64 && heardAt < 0; ++block)
        {
            juce::AudioBuffer<float> b (2, 256);
            b.clear();
            if (block == 0)
            {
                b.setSample (0, 0, 1.0f);
                b.setSample (1, 0, 1.0f);
            }
            p.processBlockBypassed (b, m);
            for (int i = 0; i < 256; ++i)
                if (b.getSample (0, i) > 0.5f && b.getSample (1, i) > 0.5f)
                    heardAt = block * 256 + i;
        }
        std::printf ("  latency %d, impulse out at %d\n", latency, heardAt);
        check (latency > 0 && heardAt == latency, "bypassed audio is delayed by exactly the reported latency");
        p.prepareToPlay (0.0, 256); // unsupported rate: latency 0, plain pass-through
        juce::AudioBuffer<float> b (2, 16);
        b.clear();
        b.setSample (0, 3, 1.0f);
        p.processBlockBypassed (b, m);
        check (b.getSample (0, 3) == 1.0f, "no latency: bypass passes audio straight through");
    }

    {
        // Differential: whatever the pre-scan accepts must be within its limits as JUCE's
        // XmlDocument and ValueTree::fromXml actually parse it (elements, depth, attributes,
        // names, bytes), for tricky inputs and for seeded mutants of them.
        std::puts ("Pre-scan vs JUCE's parser (differential)");
        const std::string p = "<PARAM id=\"retune\" value=\"5\"/>";
        const std::vector<std::string> tricky {
            "<FlayrTune>" + p + "</FlayrTune>",
            "<FlayrTune>&AMP;&Lt;&GT;&QUOT;&aPoS;" + p + "</FlayrTune>",          // entity case variants
            "<FlayrTune>&#X3C;a&#x3e;&#60;b&#62;" + p + "</FlayrTune>",          // references encoding '<' '>'
            "<FlayrTune a=\"&#58;&#x3A;\">&#58;" + p + "</FlayrTune>",          // references encoding ':'
            "<FlayrTune><PARAM id=\"retune\" value=\"5\" base64&#58;x=\"-1.AAAA\"/></FlayrTune>",
            "<FlayrTune><PARAM id=\"retune\" value=\"5\" base64:x=\"2000000000.A\"/></FlayrTune>",
            "<FlayrTune><x:PARAM id=\"retune\" value=\"5\"/></FlayrTune>",
            "\xEF\xBB\xBF<FlayrTune>" + p + "</FlayrTune>",                      // UTF-8 BOM
            "\xFE\xFF<FlayrTune>" + p + "</FlayrTune>",                           // UTF-16 BOM
            "  \r\n\t<?xml version=\"1.0\"?><FlayrTune>" + p + "</FlayrTune>",  // whitespace before <?xml
            "<?xml version=\"1.0\"?>\r\n<FlayrTune\r\n\tx\r=\n'1'\t>" + p + "</FlayrTune\n>",
            "<FlayrTune>\r\n<PARAM\tid\n=\r\"retune\"\tvalue='5'\n/>\r\n</FlayrTune>",
            "<FlayrTune " + std::string (200, ' ') + "a=\"1\"" + std::string (200, '\t') + ">" + p + "</FlayrTune>",
            "<FlayrTune>" + p + "</FlayrTune>" + std::string (1, '\0') + repeated ("<a>", 300),
            "<FlayrTune>" + repeated ("<a x='/>'>", 100) + repeated ("</a>", 100) + "</FlayrTune>",
            "<FlayrTune>" + repeated ("/>", 200) + repeated ("<a>", 200) + repeated ("</a>", 200) + "</FlayrTune>",
            "<FlayrTune>" + repeated ("<a/>", 255) + "</FlayrTune>",
            "<FlayrTune>" + repeated ("<a b='' c=''/>", 170) + "</FlayrTune>",
            "<FlayrTune>" + repeated ("&#x10FFFF;", 250) + "</FlayrTune>",
            "<FlayrTune><![CDATA[<a><b>]]>" + p + "</FlayrTune>",
            "<FlayrTune><!-- <a> -->" + p + "</FlayrTune>",
            "<FlayrTune a='<' b=\"<\" c='&lt;'>" + p + "</FlayrTune>",
            "<FlayrTune>" + p + "<PARAM id='x' value='" + std::string (4 * 1024 * 1024, 'v') + "'/></FlayrTune>",
        };
        std::vector<juce::MemoryBlock> blobs;
        for (const auto& t : tricky)
        {
            blobs.push_back (blobFromBytes (t));
            blobs.push_back (blobFromBytes (t, (int) t.size() / 2));       // declared length shorter than the data
            auto longer = blobFromBytes (t);
            static_cast<uint32_t*> (longer.getData())[1] = (uint32_t) t.size() + 1; // covers the terminating NUL
            blobs.push_back (longer);
        }
        int accepted = 0, problems = 0;
        double slowest = 0.0;
        auto examine = [&] (const juce::MemoryBlock& b, const std::string& what)
        {
            if (! FlayrTuneProcessor::isAcceptableState (b.getData(), (int) b.getSize()))
                return;
            ++accepted;
            const auto parsed = differential::parse (b.getData(), (int) b.getSize());
            slowest = std::max (slowest, parsed.parseMs);
            const auto bad = differential::limitsViolated (parsed);
            if (! bad.empty() && problems++ < 5)
                std::printf ("  accepted but %s: %s\n", bad.c_str(), what.c_str());
        };
        for (size_t i = 0; i < blobs.size(); ++i)
            examine (blobs[i], "tricky #" + std::to_string (i / 3));

        // Seeded mutants: splice tokens that change how JUCE reads the text.
        const std::vector<std::string> tokens { "<", ">", "/>", "</", "<a>", "</a>", "'", "\"", "=", " ", "\t", "\r\n", "&amp;",
                                                "&AMP;", "&#60;", "&#x3C;", "&#58;", ":", "base64:", "<?", "?>", "<!", "\0",
                                                "\xC0\xBC", "x=''", "<a x='/>'>", "&#", ";" };
        std::mt19937 rng (20261014);
        for (int n = 0; n < 40000; ++n)
        {
            std::string t = tricky[rng() % (tricky.size() - 1)]; // not the 4 MB one
            for (int k = 1 + (int) (rng() % 6); k > 0; --k)
            {
                const auto at = t.empty() ? 0 : rng() % (t.size() + 1);
                switch (rng() % 3)
                {
                    case 0: t.insert (at, tokens[rng() % tokens.size()]); break;
                    case 1: if (at < t.size()) t.erase (at, 1 + rng() % 8); break;
                    default: if (at < t.size()) t[at] = tokens[rng() % tokens.size()][0]; break;
                }
            }
            examine (blobFromBytes (t), "mutant " + std::to_string (n));
        }
        std::printf ("  %zu tricky blobs and 40000 mutants: %d accepted, slowest parse %.1f ms\n", blobs.size(), accepted, slowest);
        check (problems == 0, "everything accepted is within the limits as JUCE parses it");
        check (accepted > 100, "and the comparison actually ran on accepted input");
    }

    {
        // Found by review: a host buffer with 0 channels was dereferenced as channel 0.
        std::puts ("Buffers with no channels or samples");
        FlayrTuneProcessor p;
        p.prepareToPlay (48000.0, 512);
        juce::MidiBuffer m;
        juce::AudioBuffer<float> none (0, 64), empty (2, 0), one (1, 64);
        p.processBlock (none, m);
        p.processBlock (empty, m);
        p.processBlock (one, m); // fewer channels than the stereo buses
        check (true, "0-channel, 0-sample and 1-of-2-channel buffers: no crash");
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
       #if ! JUCE_WINDOWS
        juce::ChildProcess child;
        check (! child.start ("/bin/echo hello"), "no process launching (ChildProcess::start fails)");
       #endif
    }

    std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
