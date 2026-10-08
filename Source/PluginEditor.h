#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"

class GraphEditor;

namespace ui
{
const juce::Colour bg       { 0xff12151b };
const juce::Colour panel    { 0xff1a1e26 };
const juce::Colour line     { 0xff2a303c };
const juce::Colour text     { 0xffd8dde6 };
const juce::Colour dim      { 0xff7d8696 };
const juce::Colour accent   { 0xff35e0c4 };
const juce::Colour accent2  { 0xffff4f8b };
const juce::Colour removed  { 0xffe0454f };

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float start, float end, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
};

class PitchGraph : public juce::Component
{
public:
    void push (const tune::DisplayPoint& p);
    void tick(); // once per GUI frame: eases the view toward its target range
    void setMask (uint16_t m) { mask = m; }
    void paint (juce::Graphics&) override;

private:
    std::array<tune::DisplayPoint, 900> hist {};
    int head = 0;
    float centre = 60.0f, targetCentre = 60.0f;
    bool hasCentre = false;
    uint16_t mask = 0x0FFF;
};

class NoteStrip : public juce::Component
{
public:
    explicit NoteStrip (FlayrTuneProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void setState (uint16_t scale, uint16_t active, int targetPc) { scaleMask = scale; activeMask = active; target = targetPc; }

private:
    juce::Rectangle<float> keyRect (int pc) const;
    static bool isBlack (int pc) { return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }
    int keyAt (juce::Point<float>) const;

    FlayrTuneProcessor& proc;
    uint16_t scaleMask = 0x0FFF, activeMask = 0x0FFF;
    int target = -1;
};
} // namespace ui

class FlayrTuneEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit FlayrTuneEditor (FlayrTuneProcessor&);
    ~FlayrTuneEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh() { timerCallback(); }

private:
    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    Knob& addKnob (const juce::String& id, const juce::String& name, const juce::String& suffix);
    void addCombo (juce::ComboBox&, const juce::String& id, std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>&);
    void timerCallback() override;
    void finishKeyLearn();
    void setParam (const juce::String& id, float value);

    FlayrTuneProcessor& proc;
    ui::LookAndFeel lnf;

    ui::PitchGraph graph;
    ui::NoteStrip notes;
    juce::Label noteReadout, centsReadout;

    juce::ComboBox keyBox, scaleBox, inputBox, presetBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> keyAtt, scaleAtt, inputAtt;
    juce::TextButton formantBtn { "FORMANT" }, midiBtn { "MIDI NOTES" }, liveBtn { "LIVE" }, learnBtn { "LEARN KEY" },
        graphBtn { "GRAPH" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> formantAtt, midiAtt, liveAtt, graphAtt;
    std::unique_ptr<GraphEditor> graphEditor;
    juce::Label latencyLabel;

    std::vector<std::unique_ptr<Knob>> knobs;
    Knob* retune = nullptr;
    Knob* humanize = nullptr;
    Knob* flex = nullptr;

    std::vector<Knob*> voiceRow, vibRow, outRow;

    float lastIn = 0.0f, lastTarget = -1.0f;
    bool lastVoiced = false;
    double liveDelayMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlayrTuneEditor)
};
