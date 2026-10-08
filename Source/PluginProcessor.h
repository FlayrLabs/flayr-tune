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

private:
    tune::Params readParams() const;
    void timerCallback() override; // keeps the host's latency in step with Live mode

    // Cached so the audio thread never looks parameters up by name.
    struct Raw
    {
        std::atomic<float>* retune; std::atomic<float>* humanize; std::atomic<float>* flex;
        std::atomic<float>* natvib; std::atomic<float>* key; std::atomic<float>* scale;
        std::atomic<float>* inputType; std::atomic<float>* formant; std::atomic<float>* throat;
        std::atomic<float>* transpose; std::atomic<float>* concertA; std::atomic<float>* tracking;
        std::atomic<float>* vibRate; std::atomic<float>* vibDepth; std::atomic<float>* vibDelay;
        std::atomic<float>* mix; std::atomic<float>* output; std::atomic<float>* midiTarget;
        std::atomic<float>* live; std::atomic<float>* graph;
        std::array<std::atomic<float>*, 12> removed;
    } raw;

    std::array<bool, 128> heldNotes {};
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlayrTuneProcessor)
};
