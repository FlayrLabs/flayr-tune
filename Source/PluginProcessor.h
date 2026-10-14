#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "TuneEngine.h"

class FlayrTuneProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    FlayrTuneProcessor();
    ~FlayrTuneProcessor() override { stopTimer(); }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    // Bypass keeps the reported latency, so bypassed audio stays in time with the rest of the mix.
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static juce::String removeId (int pitchClass) { return "rm" + juce::String (pitchClass); }

    juce::AudioProcessorValueTreeState apvts;
    tune::Engine engine;

    // Graph mode: pitch timeline, the TRACK switch, and the host position for the GUI.
    tune::PitchTrack track;
    std::atomic<bool> trackArmed { false };
    std::atomic<double> playheadSeconds { 0.0 };
    std::atomic<bool> hostPlaying { false };
    static constexpr double kMaxPlayheadSeconds = 1.0e7;
    // Largest saved state we accept. A full 30-minute PitchTrack of random notes saves to
    // 3.7 MB (tests/plugin_test.cpp measures it); even incompressible data would be 4.3 MB.
    static constexpr int kMaxStateBytes = 5 * 1024 * 1024;
    // Limits of the pre-scan setStateInformation runs before JUCE parses anything (see
    // isSafeStateBlob in PluginProcessor.cpp), exposed for the differential tests.
    static constexpr int kMaxXmlTags = 256, kMaxXmlAttributes = 512, kMaxXmlEntities = 256;
    static bool isAcceptableState (const void* data, int sizeInBytes);

private:
    tune::Params readParams() const;
    void loadState (const void* data, int sizeInBytes); // setStateInformation without the catch-all
    void timerCallback() override; // keeps the host's latency in step with Live mode

    // Cached so the audio thread never looks parameters up by name.
    struct Raw
    {
        std::atomic<float>* retune; std::atomic<float>* humanize; std::atomic<float>* flex;
        std::atomic<float>* natvib; std::atomic<float>* key; std::atomic<float>* scale;
        std::atomic<float>* inputType; std::atomic<float>* formant; std::atomic<float>* formantShift;
        std::atomic<float>* transpose; std::atomic<float>* concertA; std::atomic<float>* tracking;
        std::atomic<float>* vibRate; std::atomic<float>* vibDepth; std::atomic<float>* vibDelay;
        std::atomic<float>* mix; std::atomic<float>* output; std::atomic<float>* midiTarget;
        std::atomic<float>* live; std::atomic<float>* graph;
        std::array<std::atomic<float>*, 12> removed;
    } raw;

    // Parameters a preset sets, looked up once so setCurrentProgram() doesn't allocate.
    std::array<juce::RangedAudioParameter*, 8> presetTargets {};

    std::array<bool, 128> heldNotes {};
    juce::AudioBuffer<float> bypassDelay; // ring buffer, power-of-two length
    int bypassPos = 0;
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlayrTuneProcessor)
};
