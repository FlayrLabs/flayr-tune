// Fuzz target: setStateInformation, the whole path a project file takes into the plug-in
// (binary header, XML pre-scan, JUCE's parser, ValueTree, the 1.0-1.2 throat migration,
// non-finite filtering, PitchTrack base64 + gzip + decode), then save/load round trip
// and a few blocks of Graph-mode audio over whatever track was decoded.
//
// The first input byte picks how the rest is used, so mutations reach every layer
// instead of dying at the header, the base64 or the gzip checksum:
//   0  raw blob, exactly as a host hands it over
//   1  XML text; the binary header is added
//   2  raw PitchTrack payload (int n, n inputs, n targets, n modes); gzipped, base64'd
//      and placed in an otherwise valid state
//   3  a gzip stream; base64'd and placed in the state (fuzzes the decompressor)

#include "fuzz_common.h"
#include "../state_differential.h"

extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size)
{
    fuzz::initJuce();
    fuzz::Reader in { data, size };
    const int mode = in.u8() % 4;
    auto rest = in.rest();

    // (Moved, not copied: JUCE's MemoryBlock copy-assignment calls memcpy with a null
    // pointer for an empty block, which UBSan reports on glibc.)
    juce::MemoryBlock blob;
    switch (mode)
    {
        case 0:  blob = std::move (rest); break;
        case 1:  blob = fuzz::wrapXml (rest.getData(), rest.getSize()); break;
        case 2:  blob = fuzz::stateWithTrack (fuzz::gzipBase64 (rest.getData(), rest.getSize())); break;
        default: blob = fuzz::stateWithTrack (rest.toBase64Encoding()); break;
    }

    // Two long-lived processors (building one per input would dominate the run time).
    // Each input starts from the default state, so a crash reproduces from its input alone.
    static FlayrTuneProcessor* const p = new FlayrTuneProcessor();
    static FlayrTuneProcessor* const q = new FlayrTuneProcessor();
    static const juce::MemoryBlock defaults = []
    {
        juce::MemoryBlock b;
        FlayrTuneProcessor().getStateInformation (b);
        return b;
    }();
    p->setStateInformation (defaults.getData(), (int) defaults.getSize());
    p->trackArmed = false;

    p->setStateInformation (blob.getData(), (int) blob.getSize());
    fuzz::checkProcessor (*p);

    // Differential oracle: if the pre-scan accepted it, JUCE's parse must be within limits.
    const auto problem = differential::check (blob.getData(), (int) blob.getSize());
    if (! problem.empty())
    {
        std::fprintf (stderr, "pre-scan accepted, but JUCE parsed: %s\n", problem.c_str());
        fuzz::fail ("pre-scan and JUCE's parser disagree");
    }

    // Whatever we accepted must save and load back to the same state, in an instance that
    // held something else before: a project that loads once but not after saving, or comes
    // back different, loses the user's work. Values may differ by float rounding only.
    juce::MemoryBlock saved;
    p->getStateInformation (saved);
    fuzz::require (saved.getSize() > 0, "getStateInformation wrote nothing");
    q->apvts.getParameter ("retune")->setValueNotifyingHost (0.123f); // proves the load happened
    q->track.setInput (7, 1.0f);
    q->setStateInformation (saved.getData(), (int) saved.getSize());
    for (auto* param : p->getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (param))
        {
            const auto& range = r->getNormalisableRange();
            const float a = p->apvts.getRawParameterValue (r->paramID)->load();
            const float b = q->apvts.getRawParameterValue (r->paramID)->load();
            if (std::abs (a - b) > 1e-5f * (range.end - range.start))
            {
                std::fprintf (stderr, "%s: %.9g before saving, %.9g after loading\n", r->paramID.toRawUTF8(), a, b);
                fuzz::fail ("a parameter changed across save -> load");
            }
        }
    auto same = [] (float x, float y) { return std::isnan (x) ? std::isnan (y) : x == y; };
    fuzz::require (p->track.getExtent() == q->track.getExtent(), "track extent changed across save -> load");
    for (int i = 0; i < p->track.getExtent(); ++i)
        fuzz::require (same (p->track.inputAt (i), q->track.inputAt (i)) && same (p->track.targetAt (i), q->track.targetAt (i))
                           && p->track.modeAt (i) == q->track.modeAt (i),
                       "track changed across save -> load");

    // Run the decoded track through the engine: transport playing mid-track, Graph on.
    if (p->track.getExtent() == 0)
        return 0;
    p->prepareToPlay (48000.0, 256);
    struct Head : juce::AudioPlayHead
    {
        juce::int64 pos = 0;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setIsPlaying (true);
            info.setTimeInSamples (pos);
            return info;
        }
    } head;
    head.pos = (juce::int64) (tune::PitchTrack::secondsAt (p->track.getExtent() / 2) * 48000.0); // mid-track
    p->setPlayHead (&head);
    p->apvts.getParameter ("graph")->setValueNotifyingHost (1.0f);
    juce::AudioBuffer<float> buf (2, 256);
    juce::MidiBuffer midi;
    for (int block = 0; block < 4; ++block)
    {
        for (int i = 0; i < 256; ++i)
            for (int c = 0; c < 2; ++c)
                buf.setSample (c, i, 0.3f * std::sin (0.0288f * (float) (block * 256 + i)));
        p->processBlock (buf, midi);
        fuzz::checkAudio (buf);
        head.pos += 256;
    }
    p->setPlayHead (nullptr);
    fuzz::checkProcessor (*p);
    return 0;
}
