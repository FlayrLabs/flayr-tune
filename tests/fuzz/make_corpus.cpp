// Writes the seed corpora for the fuzz targets:
//   fuzz_make_corpus <state dir> <host dir>
// State seeds are real states from getStateInformation (default, every preset, small and
// full-length PitchTracks, a 1.0-1.2 "throat" project) plus the hostile blobs from the
// security fixes (entity expansion, deep nesting, NUL / BOM / UTF-16 tricks, NaN values,
// gzip bombs, bad track headers), each prefixed with the fuzz_state mode byte.
// Host seeds are small programs in fuzz_host's opcode format.

#include "fuzz_common.h"

#include <random>

namespace
{
juce::File stateDir, hostDir;
int counter = 0;

void save (const juce::File& dir, const juce::String& name, uint8_t mode, const void* data, size_t size)
{
    juce::MemoryBlock b;
    b.append (&mode, 1);
    b.append (data, size);
    dir.getChildFile (juce::String (counter++).paddedLeft ('0', 3) + "-" + name).replaceWithData (b.getData(), b.getSize());
}

void saveBlob (const juce::String& name, const juce::MemoryBlock& blob) { save (stateDir, name, 0, blob.getData(), blob.getSize()); }
void saveXml (const juce::String& name, const std::string& xml) { saveBlob (name, fuzz::wrapXml (xml.data(), xml.size())); }

std::string repeated (const std::string& s, int n)
{
    std::string r;
    for (int i = 0; i < n; ++i)
        r += s;
    return r;
}

juce::MemoryBlock stateOf (FlayrTuneProcessor& p)
{
    juce::MemoryBlock b;
    p.getStateInformation (b);
    return b;
}

juce::MemoryBlock trackPayload (int n, int declared, float in, float tg, uint8_t mode)
{
    juce::MemoryOutputStream raw;
    raw.writeInt (declared);
    for (int i = 0; i < n; ++i) raw.writeFloat (in + (float) (i % 12));
    for (int i = 0; i < n; ++i) raw.writeFloat (tg);
    for (int i = 0; i < n; ++i) raw.writeByte ((char) mode);
    return raw.getMemoryBlock();
}

// fuzz_host programs.
struct Program
{
    juce::MemoryOutputStream out;
    Program (bool mono) { out.writeByte (mono ? 1 : 0); }
    Program& prepare (uint8_t rateSel, uint16_t block) { out.writeByte (0); out.writeByte ((char) rateSel); out.writeShort ((short) block); return *this; }
    Program& block (uint16_t n, uint8_t fill, uint8_t posFlags, uint32_t samples = 0, std::initializer_list<std::array<uint8_t, 3>> midi = {})
    {
        out.writeByte (1);
        out.writeShort ((short) n);
        out.writeByte ((char) fill);
        out.writeByte ((char) midi.size());
        for (auto& e : midi)
        {
            out.write (e.data(), 3);
            out.writeByte (2); // length 3
            if (n > 0)
                out.writeShort (0);
        }
        out.writeByte ((char) posFlags);
        if (posFlags & 4)
            out.writeInt ((int) samples);
        return *this;
    }
    Program& param (uint8_t index, uint16_t value) { out.writeByte (4); out.writeByte ((char) index); out.writeByte (0); out.writeShort ((short) value); return *this; }
    Program& rawBad (uint8_t index, uint8_t which) { out.writeByte (5); out.writeByte ((char) index); out.writeByte ((char) which); return *this; }
    Program& program (uint32_t index) { out.writeByte (6); out.writeInt ((int) index); return *this; }
    Program& graph (uint8_t flags) { out.writeByte (7); out.writeByte ((char) flags); return *this; }
    Program& saveLoad() { out.writeByte (8); return *this; }
    Program& release() { out.writeByte (9); return *this; }
    void write (const juce::String& name) { auto b = out.getMemoryBlock(); hostDir.getChildFile (juce::String (counter++).paddedLeft ('0', 3) + "-" + name).replaceWithData (b.getData(), b.getSize()); }
};
} // namespace

int main (int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf (stderr, "usage: %s <state corpus dir> <host corpus dir>\n", argv[0]);
        return 2;
    }
    fuzz::initJuce();
    stateDir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    hostDir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]);
    stateDir.createDirectory();
    hostDir.createDirectory();

    // Real states.
    {
        FlayrTuneProcessor p;
        saveBlob ("default", stateOf (p));
        const auto xml = juce::AudioProcessor::getXmlFromBinary (stateOf (p).getData(), (int) stateOf (p).getSize())->toString().toStdString();
        save (stateDir, "default-xml", 1, xml.data(), xml.size());
        for (int i = 0; i < p.getNumPrograms(); ++i)
        {
            p.setCurrentProgram (i);
            saveBlob ("preset-" + juce::String (i), stateOf (p));
        }
    }
    {
        FlayrTuneProcessor p;
        std::mt19937 rng (3);
        std::uniform_real_distribution<float> note (40.0f, 80.0f);
        for (int i = 0; i < 400; ++i)
        {
            p.track.setInput (i, note (rng));
            if (i > 100 && i < 300)
                p.track.setTarget (i, (float) (60 + (i / 40) % 5), (i / 50) % 2 ? tune::PitchTrack::kNote : tune::PitchTrack::kExact);
        }
        p.apvts.getParameter ("graph")->setValueNotifyingHost (1.0f);
        saveBlob ("track-400", stateOf (p));
        for (int i = 0; i < tune::PitchTrack::kCapacity; ++i)
        {
            p.track.setInput (i, note (rng));
            p.track.setTarget (i, note (rng), (i & 1) ? tune::PitchTrack::kExact : tune::PitchTrack::kNote);
        }
        saveBlob ("track-full-30min", stateOf (p));
    }
    {
        // A 1.0-1.2 project: "throat" instead of "formantShift".
        FlayrTuneProcessor p;
        auto xml = juce::AudioProcessor::getXmlFromBinary (stateOf (p).getData(), (int) stateOf (p).getSize());
        xml->removeChildElement (xml->getChildByAttribute ("id", "formantShift"), true);
        auto* throat = xml->createNewChildElement ("PARAM");
        throat->setAttribute ("id", "throat");
        throat->setAttribute ("value", 85.0);
        juce::MemoryBlock b;
        juce::AudioProcessor::copyXmlToBinary (*xml, b);
        saveBlob ("throat-1.2", b);
        const auto text = xml->toString().toStdString();
        save (stateDir, "throat-1.2-xml", 1, text.data(), text.size());
        for (auto* e : xml->getChildIterator())
            e->setAttribute ("value", e->getStringAttribute ("id") == "throat" ? "nan" : "inf");
        juce::AudioProcessor::copyXmlToBinary (*xml, b);
        saveBlob ("throat-nan-values-inf", b);
    }

    // Hostile blobs from the security fixes (see tests/plugin_test.cpp).
    const std::string param = "<PARAM id=\"retune\" value=\"5\"/>";
    const std::string small = "<FlayrTune>" + param + "</FlayrTune>";
    saveXml ("poc-entity", "<!DOCTYPE FlayrTune [ <!ENTITY a \"&" + std::string (64, 'x') + "a;\"> ]><FlayrTune x=\"&a;\">" + param + "</FlayrTune>");
    saveXml ("poc-deep-100k", "<FlayrTune>" + repeated ("<a>", 100000) + repeated ("</a>", 100000) + "</FlayrTune>");
    saveXml ("poc-depth-9", "<FlayrTune><a><b><c><d><e><f><g><h>" + param + "</h></g></f></e></d></c></b></a></FlayrTune>");
    saveXml ("poc-attr-lt", "<FlayrTune>" + param + "<PARAM id=\"x\" value=\"<<<<<<<<<<\"/></FlayrTune>");
    saveXml ("poc-attr-slash", "<FlayrTune>" + repeated ("<a x=\"/>/>\">", 200) + repeated ("</a>", 200) + "</FlayrTune>");
    saveXml ("poc-unterminated-quote", "<FlayrTune><PARAM id=\"retune value=\"5\"/>" + repeated ("<a>", 300) + "</FlayrTune>");
    saveXml ("poc-nul", small + std::string (1, '\0') + repeated ("<a>", 50));
    saveXml ("poc-bom", "\xEF\xBB\xBF" + small);
    saveXml ("poc-utf16", std::string ("<\0F\0l\0a\0y\0r\0T\0u\0n\0e\0>\0", 22));
    saveXml ("poc-overlong-utf8", "<FlayrTune>" + repeated ("\xC0\xBC" "a>", 100) + "</FlayrTune>");
    saveXml ("poc-pi", "<FlayrTune><?pi x?>" + param + "</FlayrTune>");
    saveXml ("poc-declaration", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" + small);
    saveXml ("poc-entities", "<FlayrTune><PARAM id=\"&#114;etune\" value=\"&#53;&#x30;\"/><PARAM id=\"x\" value=\"&lt;&gt;&amp;&quot;&apos;&#200;&#x1F600;\"/></FlayrTune>");
    saveXml ("poc-unbalanced", "<FlayrTune>" + param);
    saveXml ("poc-values", "<FlayrTune><PARAM id=\"retune\" value=\"nan\"/><PARAM id=\"mix\" value=\"-inf\"/><PARAM id=\"key\" value=\"1e300\"/>"
                           "<PARAM id=\"scale\" value=\"-2147483648\"/><PARAM id=\"transpose\" value=\"3.5e38\"/><PARAM id=\"graph\" value=\"0.5\"/>"
                           "<PARAM id=\"retune\" value=\"7\"/><PARAM value=\"1\"/><PARAM id=\"output\"/></FlayrTune>");
    {
        auto good = fuzz::wrapXml (small.data(), small.size());
        auto bad = good;
        static_cast<uint8_t*> (bad.getData())[0] ^= 1;
        saveBlob ("poc-bad-magic", bad);
        bad = good;
        static_cast<uint32_t*> (bad.getData())[1] = 0x7fffffff;
        saveBlob ("poc-bad-length", bad);
        auto trailing = fuzz::wrapXml ((small + repeated ("<a>", 2000)).data(), small.size() + 6000);
        static_cast<uint32_t*> (trailing.getData())[1] = (uint32_t) small.size();
        saveBlob ("poc-trailing-past-length", trailing);
    }

    // PitchTrack payloads (mode 2: raw, gzipped by the harness; mode 3: gzip stream).
    auto saveTrack = [] (const juce::String& name, const juce::MemoryBlock& raw)
    {
        save (stateDir, name, 2, raw.getData(), raw.getSize());
    };
    saveTrack ("track-raw-10", trackPayload (10, 10, 60.0f, 62.0f, 1));
    saveTrack ("track-raw-modes", trackPayload (20, 20, 50.0f, 55.0f, 2));
    saveTrack ("track-raw-bad-mode", trackPayload (20, 20, 50.0f, 55.0f, 7));
    saveTrack ("track-raw-nan-notes", trackPayload (8, 8, NAN, INFINITY, 1));
    saveTrack ("track-raw-out-of-range", trackPayload (8, 8, -5.0f, 300.0f, 1));
    saveTrack ("track-raw-n-too-big", trackPayload (8, tune::PitchTrack::kCapacity + 1, 60.0f, 60.0f, 1));
    saveTrack ("track-raw-n-negative", trackPayload (8, -1, 60.0f, 60.0f, 1));
    saveTrack ("track-raw-n-short", trackPayload (8, 100, 60.0f, 60.0f, 1));
    {
        auto raw = trackPayload (16, 16, 60.0f, 64.0f, 1);
        juce::MemoryOutputStream packed;
        {
            juce::GZIPCompressorOutputStream gz (packed, 6);
            gz.write (raw.getData(), raw.getSize());
        }
        save (stateDir, "track-gzip-16", 3, packed.getData(), packed.getDataSize());
        juce::MemoryOutputStream bomb;
        {
            juce::GZIPCompressorOutputStream gz (bomb, 9);
            juce::MemoryBlock zeros (1 << 20, true);
            for (int i = 0; i < 8; ++i)
                gz.write (zeros.getData(), zeros.getSize());
        }
        save (stateDir, "track-gzip-bomb-8mb", 3, bomb.getData(), bomb.getDataSize());
    }

    // Host programs. Rate table indices: 0 = 44.1k, 1 = 48k, 4 = 8k, 6 = 768k, 9 = 0, 12 = NaN.
    const std::array<uint8_t, 3> noteOnA { 0x90, 69, 100 }, noteOnHigh { 0x90, 200, 100 }, allOff { 0xB0, 123, 0 };
    {
        Program p (false);
        p.prepare (1, 512);
        for (int i = 0; i < 20; ++i)
            p.block (512, 0, 0);
        p.write ("stereo-48k-plain");
    }
    {
        Program p (false);
        p.prepare (1, 512).param (17, 65535); // MIDI target on
        p.block (512, 1, 0, 0, { noteOnA, noteOnHigh }).block (512, 0, 0).block (512, 0, 0, 0, { allOff }).block (512, 0, 0);
        p.write ("midi-target");
    }
    {
        Program p (false);
        p.prepare (1, 256).graph (1);
        for (int i = 0; i < 16; ++i)
            p.block (256, 0, 2 | 4, (uint32_t) (i * 256));
        p.graph (2 | 8);
        p.block (256, 0, 2 | 4, 0).saveLoad().block (256, 0, 2 | 4, 4096);
        p.write ("graph-capture-apply");
    }
    {
        Program p (true);
        p.prepare (0, 128).block (128, 0, 0).prepare (6, 4096).block (4096, 0, 0).prepare (9, 64).block (64, 0, 0)
            .prepare (12, 64).block (64, 0, 0).prepare (4, 1).block (1, 0, 0).block (0, 0, 0);
        p.write ("mono-rate-changes");
    }
    {
        Program p (false);
        p.prepare (1, 512).rawBad (0, 0).rawBad (15, 1).program (2).program (0xffffffff).block (512, 2 << 5, 0)
            .block (512, 3 << 5, 0).block (512, 4 << 5, 0).release().block (512, 0, 0);
        p.write ("nan-params-and-samples");
    }
    {
        Program p (false);
        p.prepare (1, 512).param (18, 65535); // live mode
        for (int i = 0; i < 8; ++i)
            p.block (512, 0, 0).param (18, (uint16_t) ((i & 1) ? 65535 : 0));
        p.write ("live-toggle");
    }

    std::printf ("wrote %d seeds to %s and %s\n", counter, stateDir.getFullPathName().toRawUTF8(), hostDir.getFullPathName().toRawUTF8());
    return 0;
}
