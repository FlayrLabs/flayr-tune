// Renders the editor to a PNG with a simulated vocal flowing through it (UI check without a DAW).
#include "PluginProcessor.h"
#include "PluginEditor.h"

// Pretends the host is playing from 0 so Graph mode can capture.
struct FakeHead : juce::AudioPlayHead
{
    juce::int64 sample = 0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (true);
        p.setTimeInSamples (sample);
        return p;
    }
};

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    FlayrTuneProcessor proc;
    proc.apvts.getParameter ("scale")->setValueNotifyingHost (proc.apvts.getParameter ("scale")->convertTo0to1 (1));
    proc.apvts.getParameter ("retune")->setValueNotifyingHost (proc.apvts.getParameter ("retune")->convertTo0to1 (25));
    proc.apvts.getParameter ("humanize")->setValueNotifyingHost (0.35f);
    proc.apvts.getParameter (FlayrTuneProcessor::removeId (11))->setValueNotifyingHost (1.0f);
    const juce::String mode = argc > 2 ? argv[2] : "";
    if (mode == "live")
        proc.apvts.getParameter ("live")->setValueNotifyingHost (1.0f);
    FakeHead head;
    const bool graphMode = mode == "graph";
    if (graphMode)
    {
        proc.apvts.getParameter ("graph")->setValueNotifyingHost (1.0f);
        proc.trackArmed.store (true);
        proc.setPlayHead (&head);
    }
    proc.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());

    // A melody sung slightly off pitch with vibrato and scoops.
    const double sr = 48000.0;
    const int melody[] = { 60, 64, 67, 65, 64, 62, 60, 67 };
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    double phase = 0.0, t = 0.0;
    for (int block = 0; block < (int) (sr * (graphMode ? 4.8 : 2.4) / 512); ++block)
    {
        for (int i = 0; i < 512; ++i, t += 1.0 / sr)
        {
            const int idx = (int) (t / 0.3) % 8;
            const double into = std::fmod (t, 0.3);
            const double scoop = into < 0.06 ? -0.8 * (1.0 - into / 0.06) : 0.0;
            const double note = melody[idx] + 0.25 * std::sin (idx * 1.7) + scoop + 0.3 * std::sin (2 * juce::MathConstants<double>::pi * 5.5 * t);
            phase += 2 * juce::MathConstants<double>::pi * 440.0 * std::pow (2.0, (note - 69) / 12.0) / sr;
            float v = 0; for (int k = 1; k < 10; ++k) v += (float) (std::sin (k * phase) / k);
            buf.setSample (0, i, 0.2f * v); buf.setSample (1, i, 0.2f * v);
        }
        proc.processBlock (buf, midi);
        head.sample += 512;
        if (block % 4 == 0) static_cast<FlayrTuneEditor*> (ed.get())->refresh();
    }
    if (graphMode)
    {
        // Example edits: notes over the 2nd-4th notes, a drawn slide into the last one.
        const int pattern[] = { 64, 67, 65 };
        for (int k = 0; k < 3; ++k)
            for (int i = tune::PitchTrack::indexFor (0.3 * (k + 1) + 0.02); i < tune::PitchTrack::indexFor (0.3 * (k + 2) - 0.02); ++i)
                proc.track.setTarget (i, (float) pattern[k], tune::PitchTrack::kNote);
        for (int i = tune::PitchTrack::indexFor (2.1); i < tune::PitchTrack::indexFor (2.4); ++i)
            proc.track.setTarget (i, 60.0f + 7.0f * (float) (tune::PitchTrack::secondsAt (i) - 2.1) / 0.3f, tune::PitchTrack::kExact);
    }
    if (graphMode)
    {
        // Save / reload round trip: graph data must come back exactly.
        juce::MemoryBlock state;
        proc.getStateInformation (state);
        FlayrTuneProcessor copy;
        copy.setStateInformation (state.getData(), (int) state.getSize());
        int mismatches = 0, edits = 0;
        for (int i = 0; i < proc.track.getExtent(); ++i)
        {
            const float a = proc.track.inputAt (i), b = copy.track.inputAt (i);
            const float ta = proc.track.targetAt (i), tb = copy.track.targetAt (i);
            if (std::isfinite (ta)) ++edits;
            if ((std::isfinite (a) != std::isfinite (b)) || (std::isfinite (a) && a != b)
                || (std::isfinite (ta) != std::isfinite (tb)) || (std::isfinite (ta) && ta != tb)
                || proc.track.modeAt (i) != copy.track.modeAt (i))
                ++mismatches;
        }
        std::printf ("state round trip: extent %d -> %d, %d edits, %d mismatches, %d bytes\n", proc.track.getExtent(),
                     copy.track.getExtent(), edits, mismatches, (int) state.getSize());
    }
    static_cast<FlayrTuneEditor*> (ed.get())->refresh();
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
    juce::File out (argc > 1 ? argv[1] : "snapshot.png");
    out.deleteFile();
    juce::FileOutputStream os (out);
    juce::PNGImageFormat().writeImageToStream (img, os);
    return 0;
}
