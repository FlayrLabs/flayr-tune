#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
struct Preset
{
    const char* name;
    float retune, humanize, flex, natVib, throat, transpose, vibDepth;
    bool formant;
};

const std::array<Preset, 8> kPresets { {
    { "Natural Vocal",       45.0f, 40.0f, 30.0f,  0.0f, 100.0f,   0.0f,  0.0f, true  },
    { "Pop Polish",          15.0f, 25.0f, 10.0f,  0.0f, 100.0f,   0.0f,  0.0f, true  },
    { "Hard Tune",            0.0f,  0.0f,  0.0f,  0.0f, 100.0f,   0.0f,  0.0f, true  },
    { "Subtle Pitch Fix",   120.0f, 60.0f, 60.0f,  0.0f, 100.0f,   0.0f,  0.0f, true  },
    { "Lush Vibrato",        30.0f, 30.0f, 20.0f,  6.0f, 100.0f,   0.0f, 20.0f, true  },
    { "Darker Voice",        30.0f, 30.0f, 20.0f,  0.0f, 120.0f,   0.0f,  0.0f, true  },
    { "Octave Down Monster",  0.0f,  0.0f,  0.0f,  0.0f, 130.0f, -12.0f,  0.0f, true  },
    { "Chipmunk",            10.0f,  0.0f,  0.0f,  0.0f, 100.0f,  12.0f,  0.0f, false },
} };
} // namespace

FlayrTuneProcessor::FlayrTuneProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "FlayrTune", createLayout())
{
    auto get = [this] (const juce::String& id) { return apvts.getRawParameterValue (id); };
    raw = { get ("retune"), get ("humanize"), get ("flex"), get ("natvib"), get ("key"), get ("scale"),
            get ("inputType"), get ("formant"), get ("throat"), get ("transpose"), get ("concertA"),
            get ("tracking"), get ("vibRate"), get ("vibDepth"), get ("vibDelay"), get ("mix"),
            get ("output"), get ("midiTarget"), get ("live"), get ("graph"), {} };
    engine.setTrack (&track);
    for (int pc = 0; pc < 12; ++pc)
        raw.removed[(size_t) pc] = get (removeId (pc));

    startTimerHz (10);
}

void FlayrTuneProcessor::timerCallback()
{
    const int wanted = engine.getLatencySamples (raw.live->load() > 0.5f);
    if (wanted != getLatencySamples())
        setLatencySamples (wanted);
}

juce::AudioProcessorValueTreeState::ParameterLayout FlayrTuneProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    auto addFloat = [&p] (const char* id, const char* name, float lo, float hi, float step, float def,
                          const char* unit, float skewCentre = -1.0f)
    {
        NormalisableRange<float> range (lo, hi, step);
        if (skewCentre > lo)
            range.setSkewForCentre (skewCentre);
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, range, def,
                                                            AudioParameterFloatAttributes().withLabel (unit)));
    };

    addFloat ("retune",    "Retune Speed",    0.0f,  400.0f, 0.1f,  20.0f, "ms", 60.0f);
    addFloat ("humanize",  "Humanize",        0.0f,  100.0f, 0.1f,   0.0f, "%");
    addFloat ("flex",      "Expression",      0.0f,  100.0f, 0.1f,   0.0f, "%");
    addFloat ("natvib",    "Natural Vibrato", -12.0f, 12.0f, 0.1f,   0.0f, "dB");
    addFloat ("throat",    "Throat Length",   70.0f, 140.0f, 0.1f, 100.0f, "%");
    addFloat ("concertA",  "Concert A",      430.0f, 450.0f, 0.1f, 440.0f, "Hz");
    addFloat ("tracking",  "Tracking",        0.0f,  100.0f, 0.1f,  50.0f, "%");
    addFloat ("vibRate",   "Vibrato Rate",    1.0f,   10.0f, 0.01f,  5.5f, "Hz");
    addFloat ("vibDepth",  "Vibrato Depth",   0.0f,  100.0f, 0.1f,   0.0f, "cents");
    addFloat ("vibDelay",  "Vibrato Onset",   0.0f, 2000.0f, 1.0f, 400.0f, "ms", 500.0f);
    addFloat ("mix",       "Mix",             0.0f,  100.0f, 0.1f, 100.0f, "%");
    addFloat ("output",    "Output",        -24.0f,   12.0f, 0.1f,   0.0f, "dB");

    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "transpose", 1 }, "Transpose", -12, 12, 0,
                                                      AudioParameterIntAttributes().withLabel ("st")));

    StringArray keys, scales;
    for (auto* n : tune::kNoteNames) keys.add (n);
    for (auto* s : tune::kScaleNames) scales.add (s);
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "key", 1 }, "Key", keys, 0));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "scale", 1 }, "Scale", scales, 0));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "inputType", 1 }, "Input Type",
                                                         StringArray { "Soprano", "Alto/Tenor", "Low Male", "Instrument" }, 1));

    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "formant", 1 }, "Formant", true));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "midiTarget", 1 }, "MIDI Target Notes", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "live", 1 }, "Live Mode", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "graph", 1 }, "Graph Mode", false));

    for (int pc = 0; pc < 12; ++pc)
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { removeId (pc), 1 },
                                                           String ("Remove ") + tune::kNoteNames[(size_t) pc], false));

    return { p.begin(), p.end() };
}

bool FlayrTuneProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void FlayrTuneProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate, juce::jmax (1, getTotalNumInputChannels()));
    setLatencySamples (engine.getLatencySamples (raw.live->load() > 0.5f));
    heldNotes.fill (false);
}

tune::Params FlayrTuneProcessor::readParams() const
{
    tune::Params p;
    p.retuneMs = raw.retune->load();
    p.humanize = raw.humanize->load() / 100.0f;
    p.expression = raw.flex->load() / 100.0f;
    p.naturalVibratoDb = raw.natvib->load();
    p.key = (int) raw.key->load();
    p.scale = (int) raw.scale->load();
    p.inputType = (int) raw.inputType->load();
    p.formantPreserve = raw.formant->load() > 0.5f;
    p.throat = raw.throat->load() / 100.0f;
    p.transpose = raw.transpose->load();
    p.concertA = raw.concertA->load();
    p.tracking = raw.tracking->load() / 100.0f;
    p.vibRateHz = raw.vibRate->load();
    p.vibDepthCents = raw.vibDepth->load();
    p.vibDelayMs = raw.vibDelay->load();
    p.mix = raw.mix->load() / 100.0f;
    p.outputGain = juce::Decibels::decibelsToGain (raw.output->load());
    p.midiTarget = raw.midiTarget->load() > 0.5f;
    p.liveMode = raw.live->load() > 0.5f;
    for (int pc = 0; pc < 12; ++pc)
        if (raw.removed[(size_t) pc]->load() > 0.5f)
            p.removedMask = (uint16_t) (p.removedMask | (1u << pc));
    return p;
}

void FlayrTuneProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (msg.isNoteOn())
            heldNotes[(size_t) msg.getNoteNumber()] = true;
        else if (msg.isNoteOff())
            heldNotes[(size_t) msg.getNoteNumber()] = false;
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            heldNotes.fill (false);
    }

    uint16_t midiMask = 0;
    for (int n = 0; n < 128; ++n)
        if (heldNotes[(size_t) n])
            midiMask = (uint16_t) (midiMask | (1u << (n % 12)));

    engine.setMidiMask (midiMask);
    engine.setParams (readParams());

    bool playing = false;
    double hostSample = 0.0;
    if (auto* head = getPlayHead())
        if (const auto pos = head->getPosition())
        {
            playing = pos->getIsPlaying();
            if (const auto t = pos->getTimeInSamples())
                hostSample = (double) *t;
            else if (const auto secs = pos->getTimeInSeconds())
                hostSample = *secs * getSampleRate();
        }
    hostPlaying.store (playing, std::memory_order_relaxed);
    playheadSeconds.store (hostSample / engine.getSampleRate(), std::memory_order_relaxed);
    engine.setTransport (playing, hostSample, trackArmed.load (std::memory_order_relaxed),
                         raw.graph->load() > 0.5f);

    const int numIn = getTotalNumInputChannels();
    for (int c = numIn; c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    engine.process (buffer.getArrayOfWritePointers(), juce::jmin (numIn, buffer.getNumChannels()), buffer.getNumSamples());
}

//==============================================================================
int FlayrTuneProcessor::getNumPrograms() { return (int) kPresets.size(); }

const juce::String FlayrTuneProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, (int) kPresets.size()) ? kPresets[(size_t) index].name : "";
}

void FlayrTuneProcessor::setCurrentProgram (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) kPresets.size()))
        return;
    currentProgram = index;
    const auto& pr = kPresets[(size_t) index];

    auto set = [this] (const char* id, float value)
    {
        if (auto* param = apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (value));
    };
    set ("retune", pr.retune);
    set ("humanize", pr.humanize);
    set ("flex", pr.flex);
    set ("natvib", pr.natVib);
    set ("throat", pr.throat);
    set ("transpose", pr.transpose);
    set ("vibDepth", pr.vibDepth);
    set ("formant", pr.formant ? 1.0f : 0.0f);
}

namespace
{
const juce::Identifier kTrackTag ("PitchTrack");

// Captured pitch + edits up to the track's extent, gzipped, so they save with the project.
juce::String encodeTrack (const tune::PitchTrack& t)
{
    const int n = t.getExtent();
    juce::MemoryOutputStream raw;
    raw.writeInt (n);
    for (int i = 0; i < n; ++i) raw.writeFloat (t.inputAt (i));
    for (int i = 0; i < n; ++i) raw.writeFloat (t.targetAt (i));
    for (int i = 0; i < n; ++i) raw.writeByte ((char) t.modeAt (i));

    juce::MemoryOutputStream packed;
    {
        juce::GZIPCompressorOutputStream gz (packed, 6);
        gz.write (raw.getData(), raw.getDataSize());
    }
    return packed.getMemoryBlock().toBase64Encoding();
}

void decodeTrack (tune::PitchTrack& t, const juce::String& data)
{
    t.clearAll();
    juce::MemoryBlock packed;
    if (! packed.fromBase64Encoding (data))
        return;
    juce::MemoryInputStream src (packed, false);
    juce::GZIPDecompressorInputStream gz (src);
    juce::MemoryBlock rawBlock;
    gz.readIntoMemoryBlock (rawBlock);
    juce::MemoryInputStream in (rawBlock, false);
    const int n = in.readInt();
    if (n <= 0 || n > tune::PitchTrack::kCapacity || (size_t) n * 9 + 4 > rawBlock.getSize())
        return;
    for (int i = 0; i < n; ++i) t.setInput (i, in.readFloat());
    std::vector<float> targets ((size_t) n);
    for (auto& v : targets) v = in.readFloat();
    for (int i = 0; i < n; ++i) t.setTarget (i, targets[(size_t) i], (uint8_t) in.readByte());
}
} // namespace

void FlayrTuneProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.removeChild (state.getChildWithName (kTrackTag), nullptr);
    if (track.getExtent() > 0)
    {
        juce::ValueTree t (kTrackTag);
        t.setProperty ("data", encodeTrack (track), nullptr);
        state.appendChild (t, nullptr);
    }
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void FlayrTuneProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);
    const auto trackTree = tree.getChildWithName (kTrackTag);
    if (trackTree.isValid())
        decodeTrack (track, trackTree.getProperty ("data").toString());
    else
        track.clearAll();
    tree.removeChild (trackTree, nullptr);
    apvts.replaceState (tree);

    // replaceState skips parameters whose stored value didn't change, which can leave
    // a bool at an in-between normalised value; push every saved value explicitly.
    for (auto* param : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
        {
            const auto child = tree.getChildWithProperty ("id", ranged->paramID);
            if (child.isValid() && child.hasProperty ("value"))
                ranged->setValueNotifyingHost (ranged->convertTo0to1 ((float) child.getProperty ("value")));
        }
}

juce::AudioProcessorEditor* FlayrTuneProcessor::createEditor() { return new FlayrTuneEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new FlayrTuneProcessor(); }
