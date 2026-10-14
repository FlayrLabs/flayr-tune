#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
struct Preset
{
    const char* name;
    float retune, humanize, flex, natVib, formantShift, transpose, vibDepth;
    bool formant;
};

const std::array<Preset, 8> kPresets { {
    { "Natural Vocal",       45.0f, 40.0f, 30.0f,  0.0f,  0.0f,   0.0f,  0.0f, true  },
    { "Pop Polish",          15.0f, 25.0f, 10.0f,  0.0f,  0.0f,   0.0f,  0.0f, true  },
    { "Hard Tune",            0.0f,  0.0f,  0.0f,  0.0f,  0.0f,   0.0f,  0.0f, true  },
    { "Subtle Pitch Fix",   120.0f, 60.0f, 60.0f,  0.0f,  0.0f,   0.0f,  0.0f, true  },
    { "Lush Vibrato",        30.0f, 30.0f, 20.0f,  6.0f,  0.0f,   0.0f, 20.0f, true  },
    { "Darker Voice",        30.0f, 30.0f, 20.0f,  0.0f, -3.2f,   0.0f,  0.0f, true  },
    { "Octave Down Monster",  0.0f,  0.0f,  0.0f,  0.0f, -4.5f, -12.0f,  0.0f, true  },
    { "Chipmunk",            10.0f,  0.0f,  0.0f,  0.0f,  0.0f,  12.0f,  0.0f, false },
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
            get ("inputType"), get ("formant"), get ("formantShift"), get ("transpose"), get ("concertA"),
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
                          const char* unit, float skewCentre = NAN)
    {
        NormalisableRange<float> range (lo, hi, step);
        if (std::isfinite (skewCentre)) // only Retune Speed and Vibrato Onset are skewed
            range.setSkewForCentre (skewCentre);
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, range, def,
                                                            AudioParameterFloatAttributes().withLabel (unit)));
    };

    addFloat ("retune",    "Retune Speed",    0.0f,  400.0f, 0.1f,  20.0f, "ms", 60.0f);
    addFloat ("humanize",  "Humanize",        0.0f,  100.0f, 0.1f,   0.0f, "%");
    addFloat ("flex",      "Expression",      0.0f,  100.0f, 0.1f,   0.0f, "%");
    addFloat ("natvib",    "Natural Vibrato", -12.0f, 12.0f, 0.1f,   0.0f, "dB");
    addFloat ("formantShift", "Formant Shift", -6.0f,  6.0f, 0.1f,   0.0f, "st");
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
    // A non-finite value (setStateInformation filters them, but a host could still
    // inject one) falls back to the parameter's default from createLayout().
    auto get = [] (const std::atomic<float>* v, float fallback)
    {
        const float x = v->load();
        return std::isfinite (x) ? x : fallback;
    };
    auto choice = [&get] (const std::atomic<float>* v, int fallback, int count)
    {
        return juce::jlimit (0, count - 1, (int) get (v, (float) fallback));
    };

    tune::Params p;
    p.retuneMs = get (raw.retune, 20.0f);
    p.humanize = get (raw.humanize, 0.0f) / 100.0f;
    p.expression = get (raw.flex, 0.0f) / 100.0f;
    p.naturalVibratoDb = get (raw.natvib, 0.0f);
    p.key = choice (raw.key, 0, (int) tune::kNoteNames.size());
    p.scale = choice (raw.scale, 0, (int) tune::kScaleNames.size());
    p.inputType = choice (raw.inputType, 1, 4);
    p.formantPreserve = get (raw.formant, 1.0f) > 0.5f;
    // The engine models a vocal-tract length: +12 st of formant shift = half the length.
    p.throat = std::pow (2.0f, -get (raw.formantShift, 0.0f) / 12.0f);
    p.transpose = get (raw.transpose, 0.0f);
    p.concertA = get (raw.concertA, 440.0f);
    p.tracking = get (raw.tracking, 50.0f) / 100.0f;
    p.vibRateHz = get (raw.vibRate, 5.5f);
    p.vibDepthCents = get (raw.vibDepth, 0.0f);
    p.vibDelayMs = get (raw.vibDelay, 400.0f);
    p.mix = get (raw.mix, 100.0f) / 100.0f;
    p.outputGain = juce::Decibels::decibelsToGain (get (raw.output, 0.0f));
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
    set ("formantShift", pr.formantShift);
    set ("transpose", pr.transpose);
    set ("vibDepth", pr.vibDepth);
    set ("formant", pr.formant ? 1.0f : 0.0f);
}

namespace
{
const juce::Identifier kTrackTag ("PitchTrack");

// Project files can come from anyone. JUCE's XML parser expands <!DOCTYPE>/<!ENTITY>
// definitions and recurses once per nesting level, so a crafted blob can overflow the
// stack (crashing the host) before we ever see the tree. Our own state is flat
// (FlayrTune > PARAM / PitchTrack) and never contains "<!", so anything else is refused
// before it reaches the parser.
constexpr uint32_t kXmlMagic = 0x21324356;       // AudioProcessor::copyXmlToBinary header
constexpr int kMaxStateBytes = 16 * 1024 * 1024; // a full 30-minute PitchTrack: ~3.1 MB, ~4.4 MB if incompressible
constexpr int kMaxXmlDepth = 8;

bool isSafeStateBlob (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 9 || sizeInBytes > kMaxStateBytes)
        return false;
    const auto* bytes = static_cast<const uint8_t*> (data);
    if (juce::ByteOrder::littleEndianInt (bytes) != kXmlMagic)
        return false;
    const auto length = (size_t) juce::ByteOrder::littleEndianInt (bytes + 4);
    if (length == 0 || length > (size_t) sizeInBytes - 8)
        return false;

    const auto* text = reinterpret_cast<const char*> (bytes + 8);
    int depth = 0;
    for (size_t i = 0; i < length; ++i)
    {
        if (text[i] == 0)
            return false;
        if (text[i] != '<')
            continue;
        if (i + 1 >= length || text[i + 1] == '!') // DOCTYPE, ENTITY, comments, CDATA
            return false;
        if (text[i + 1] == '?') // the <?xml ...?> declaration
        {
            size_t j = i + 2;
            while (j + 1 < length && ! (text[j] == '?' && text[j + 1] == '>'))
                ++j;
            if (j + 1 >= length)
                return false;
            i = j + 1;
            continue;
        }

        // Walk to the end of the tag, skipping quoted attribute values.
        char quote = 0;
        size_t j = i + 1;
        for (; j < length; ++j)
        {
            const char c = text[j];
            if (c == 0)
                return false;
            if (quote != 0)
                quote = c == quote ? 0 : quote;
            else if (c == '"' || c == '\'')
                quote = c;
            else if (c == '>')
                break;
            else if (c == '<')
                return false;
        }
        if (j >= length)
            return false;

        if (text[i + 1] == '/')
        {
            if (--depth < 0)
                return false;
        }
        else if (text[j - 1] != '/' && ++depth > kMaxXmlDepth)
        {
            return false;
        }
        i = j;
    }
    return depth == 0;
}

// A finite number, or `fallback` for NaN, Inf and anything a float can't hold.
double finiteOr (const juce::var& v, double fallback)
{
    const double d = (double) v;
    return std::isfinite (d) && std::isfinite ((float) d) ? d : fallback;
}

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

    // Project files can come from anyone, so never inflate more than a full track can hold
    // (a small gzip "bomb" would otherwise expand until the host runs out of memory).
    constexpr size_t maxBytes = 4 + (size_t) tune::PitchTrack::kCapacity * 9;
    juce::MemoryBlock rawBlock;
    if (gz.readIntoMemoryBlock (rawBlock, (juce::pointer_sized_int) maxBytes + 1) > maxBytes)
        return;
    juce::MemoryInputStream in (rawBlock, false);
    const int n = in.readInt();
    if (n <= 0 || n > tune::PitchTrack::kCapacity || (size_t) n * 9 + 4 > rawBlock.getSize())
        return;
    // Only MIDI-range notes and known edit types get through.
    auto note = [] (float v) { return std::isfinite (v) && v >= 0.0f && v <= 127.0f ? v : NAN; };
    for (int i = 0; i < n; ++i) t.setInput (i, note (in.readFloat()));
    std::vector<float> targets ((size_t) n);
    for (auto& v : targets) v = note (in.readFloat());
    for (int i = 0; i < n; ++i)
    {
        const auto mode = (uint8_t) in.readByte();
        const bool known = mode == tune::PitchTrack::kExact || mode == tune::PitchTrack::kNote;
        t.setTarget (i, known ? targets[(size_t) i] : NAN, known ? mode : (uint8_t) tune::PitchTrack::kNone);
    }
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
    if (! isSafeStateBlob (data, sizeInBytes))
        return;
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);

    // Projects from 1.0-1.2 stored "throat" (70-140 % tract length); convert to semitones.
    const auto oldThroat = tree.getChildWithProperty ("id", "throat");
    if (oldThroat.isValid() && ! tree.getChildWithProperty ("id", "formantShift").isValid())
    {
        const double length = juce::jlimit (0.5, 2.0, finiteOr (oldThroat.getProperty ("value", 100.0), 100.0) / 100.0);
        juce::ValueTree shift ("PARAM");
        shift.setProperty ("id", "formantShift", nullptr);
        shift.setProperty ("value", juce::jlimit (-6.0, 6.0, -12.0 * std::log2 (length)), nullptr);
        tree.appendChild (shift, nullptr);
    }
    tree.removeChild (oldThroat, nullptr);
    const auto trackTree = tree.getChildWithName (kTrackTag);
    if (trackTree.isValid())
        decodeTrack (track, trackTree.getProperty ("data").toString());
    else
        track.clearAll();
    tree.removeChild (trackTree, nullptr);

    // "nan" or "inf" survive jlimit and would stick in the engine, so they become defaults.
    for (auto* param : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
        {
            auto child = tree.getChildWithProperty ("id", ranged->paramID);
            const double fallback = ranged->convertFrom0to1 (ranged->getDefaultValue());
            if (child.isValid() && child.hasProperty ("value"))
                child.setProperty ("value", finiteOr (child.getProperty ("value"), fallback), nullptr);
        }
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
