// Fuzz target: the VST3 wrapper's state path. JUCE's wrapper splits the component state
// into the plug-in's part and its own "JUCEPrivateData" trailer and reads the Bypass flag
// from the trailer before Tune's setStateInformation sees anything. This runs the exact
// code the wrapper runs (juce::detail::VST3PrivateState, patches/juce-vst3-private-state.patch)
// and then Tune's setStateInformation on what it hands over.
//
// The first input byte picks the layout of the rest:
//   0  raw component state, exactly as a host hands it over
//   1  [u16 split][u8 size mode][8 bytes] plug-in state | private data, with a trailer
//      whose declared size is right, off by a little, or arbitrary

#include "fuzz_common.h"

#include <juce_audio_plugin_client/detail/juce_VST3PrivateState.h>

extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size)
{
    fuzz::initJuce();
    fuzz::Reader in { data, size };
    const int mode = in.u8() % 2;

    juce::MemoryBlock state;
    if (mode == 0)
    {
        state = in.rest();
    }
    else
    {
        const auto cut = in.u16();
        const auto sizeMode = in.u8();
        const auto rawSize = in.get<int64_t>();
        auto rest = in.rest();
        const size_t pluginBytes = std::min ((size_t) cut, rest.getSize());
        const size_t privateBytes = rest.getSize() - pluginBytes;
        int64_t declared = (int64_t) privateBytes;
        switch (sizeMode % 4)
        {
            case 1: declared += (int64_t) (sizeMode >> 2) - 32; break;
            case 2: declared = rawSize; break;
            default: break;
        }
        juce::MemoryOutputStream out;
        out.write (rest.getData(), pluginBytes);
        out.writeInt64 (0);
        out.write (static_cast<const char*> (rest.getData()) + pluginBytes, privateBytes);
        out.writeInt64 (declared);
        out << juce::detail::VST3PrivateState::identifier;
        state = out.getMemoryBlock();
    }

    static FlayrTuneProcessor* const p = new FlayrTuneProcessor();

    // What JuceVST3Component::setStateInformation does with it.
    const auto split = juce::detail::VST3PrivateState::split (state.getData(), state.getSize());
    if (! split.valid)
        return 0;
    fuzz::require (split.pluginStateSize <= state.getSize(), "plug-in part larger than the state");
    if (split.privateSize > 0)
    {
        const auto* begin = static_cast<const char*> (state.getData());
        fuzz::require (split.privateData >= begin && split.privateData + split.privateSize <= begin + state.getSize(),
                       "private data outside the state");
        (void) juce::detail::VST3PrivateState::readBypass (split.privateData, split.privateSize);
    }
    if (split.pluginStateSize > 0 && split.pluginStateSize <= (size_t) std::numeric_limits<int>::max())
        p->setStateInformation (state.getData(), (int) split.pluginStateSize);
    fuzz::checkParameters (*p);
    fuzz::checkTrack (*p);
    return 0;
}
