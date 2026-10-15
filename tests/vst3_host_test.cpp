// Loads the built Flayr Tune VST3 like a DAW does and feeds the VST3 wrapper hostile
// component state: the wrapper reads its own "JUCEPrivateData" trailer before the
// plug-in's setStateInformation runs (patches/juce-vst3-private-state.patch).
//   vst3_host_test "<path>/Flayr Tune.vst3"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdio>
#include <string>

using namespace juce;

static int failures = 0;

static void check (bool ok, const std::string& what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (! ok)
        ++failures;
}

// The JUCE VST3 host wraps the component state as base64 text in an <IComponent> element.
struct HostState
{
    std::unique_ptr<XmlElement> xml;

    MemoryBlock component() const
    {
        MemoryBlock b;
        b.fromBase64Encoding (xml->getChildByName ("IComponent")->getAllSubText());
        return b;
    }

    MemoryBlock withComponent (const MemoryBlock& comp) const
    {
        XmlElement copy (*xml);
        copy.getChildByName ("IComponent")->deleteAllTextElements();
        copy.getChildByName ("IComponent")->addTextElement (comp.toBase64Encoding());
        MemoryBlock out;
        AudioProcessor::copyXmlToBinary (copy, out);
        return out;
    }
};

static HostState stateOf (AudioPluginInstance& p)
{
    MemoryBlock b;
    p.getStateInformation (b);
    return { AudioProcessor::getXmlFromBinary (b.getData(), (int) b.getSize()) };
}

// Our own state (copyXmlToBinary layout) with the given Retune Speed.
static MemoryBlock pluginState (double retune)
{
    XmlElement x ("FlayrTune");
    auto* e = x.createNewChildElement ("PARAM");
    e->setAttribute ("id", "retune");
    e->setAttribute ("value", retune);
    MemoryBlock b;
    AudioProcessor::copyXmlToBinary (x, b);
    return b;
}

static double retuneIn (AudioPluginInstance& p)
{
    const auto comp = stateOf (p).component();
    const auto* bytes = static_cast<const uint8*> (comp.getData());
    if (comp.getSize() < 9)
        return -1;
    const auto len = (int) ByteOrder::littleEndianInt (bytes + 4);
    auto xml = AudioProcessor::getXmlFromBinary (comp.getData(), juce::jmin ((int) comp.getSize(), len + 9));
    if (xml == nullptr)
        return -1;
    auto* e = xml->getChildByAttribute ("id", "retune");
    return e != nullptr ? e->getDoubleAttribute ("value", -1) : -1;
}

// [plug-in state][int64 0][private][int64 declared size]"JUCEPrivateData"
static MemoryBlock withTrailer (const MemoryBlock& plugin, const MemoryBlock& privateData, int64 declaredSize)
{
    MemoryOutputStream c;
    c.write (plugin.getData(), plugin.getSize());
    c.writeInt64 (0);
    c.write (privateData.getData(), privateData.getSize());
    c.writeInt64 (declaredSize);
    c << "JUCEPrivateData";
    return c.getMemoryBlock();
}

static MemoryBlock privateTree (bool bypass)
{
    ValueTree t ("JUCEPrivateData");
    t.setProperty ("Bypass", bypass, nullptr);
    MemoryOutputStream o;
    t.writeToStream (o);
    return o.getMemoryBlock();
}

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    if (argc != 2)
    {
        std::fprintf (stderr, "usage: %s <path to Flayr Tune.vst3>\n", argv[0]);
        return 2;
    }
    ScopedJuceInitialiser_GUI juceInit;

    VST3PluginFormat format;
    OwnedArray<PluginDescription> types;
    format.findAllTypesForFile (types, argv[1]);
    if (types.isEmpty())
    {
        std::printf ("no plug-in found in %s\n", argv[1]);
        return 1;
    }
    String error;
    auto plugin = format.createInstanceFromDescription (*types[0], 48000.0, 512, error);
    if (plugin == nullptr)
    {
        std::printf ("could not load the plug-in: %s\n", error.toRawUTF8());
        return 1;
    }

    const auto host = stateOf (*plugin);
    check (host.xml != nullptr && host.xml->getChildByName ("IComponent") != nullptr, "host state has an IComponent");
    auto load = [&] (const MemoryBlock& comp)
    {
        const auto blob = host.withComponent (comp);
        plugin->setStateInformation (blob.getData(), (int) blob.getSize());
    };

    std::puts ("A genuine state with the wrapper's trailer");
    load (withTrailer (pluginState (77.0), privateTree (false), (int64) privateTree (false).getSize()));
    check (std::abs (retuneIn (*plugin) - 77.0) < 0.01, "plug-in state restored through the wrapper");

    std::puts ("Bypass flag in the private data");
    auto* bypass = plugin->getBypassParameter();
    check (bypass != nullptr, "the wrapper exposes a bypass parameter (so it reads the private data)");
    load (withTrailer (pluginState (60.0), privateTree (true), (int64) privateTree (true).getSize()));
    check (bypass == nullptr || bypass->getValue() > 0.5f, "Bypass = true restored");
    load (withTrailer (pluginState (60.0), privateTree (false), (int64) privateTree (false).getSize()));
    check (bypass == nullptr || bypass->getValue() < 0.5f, "Bypass = false restored");

    std::puts ("Declared private size larger than the data (wrapped to a wild read before)");
    for (int64 size : { (int64) 0x40000000, (int64) -1, (int64) 0x7fffffffffffffff, (int64) 2000 })
    {
        load (withTrailer (pluginState (11.0), {}, size));
        check (std::abs (retuneIn (*plugin) - 60.0) < 0.01, "size " + std::to_string (size) + ": refused, nothing loaded");
    }

    std::puts ("Deeply nested private ValueTree (overflowed the stack before)");
    {
        MemoryOutputStream deep;
        for (int i = 0; i < 400000; ++i)
        {
            deep.write ("a", 2);
            deep.writeCompressedInt (0);
            deep.writeCompressedInt (1);
        }
        load (withTrailer (pluginState (33.0), deep.getMemoryBlock(), (int64) deep.getDataSize()));
        check (std::abs (retuneIn (*plugin) - 33.0) < 0.01, "400,000 levels: ignored, plug-in state still loads");

        MemoryOutputStream huge;
        huge.write ("JUCEPrivateData", 16);
        huge.writeCompressedInt (0);
        huge.writeCompressedInt (0x7fffffff);
        load (withTrailer (pluginState (34.0), huge.getMemoryBlock(), (int64) huge.getDataSize()));
        check (std::abs (retuneIn (*plugin) - 34.0) < 0.01, "2^31 children claimed: ignored");
    }

    std::puts ("Truncated and tiny states");
    for (size_t n : { (size_t) 1, (size_t) 15, (size_t) 23, (size_t) 24 })
    {
        const auto full = withTrailer (pluginState (35.0), privateTree (false), (int64) privateTree (false).getSize());
        MemoryBlock tail (static_cast<const char*> (full.getData()) + full.getSize() - n, n);
        load (tail);
        check (true, "last " + std::to_string (n) + " bytes only: no crash");
    }

    plugin.reset();
    std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
