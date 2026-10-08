#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

// Graph mode editor: the captured pitch along the song timeline with drawing tools.
// Edits are written straight into the processor's PitchTrack, which the audio thread
// reads on playback.
class GraphEditor : public juce::Component
{
public:
    explicit GraphEditor (FlayrTuneProcessor&);

    void refresh(); // called from the editor's timer
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scale) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    enum class Tool { draw, line, note, erase };

    struct UndoEntry
    {
        int start = 0;
        std::vector<float> target;
        std::vector<uint8_t> mode;
    };

    juce::Rectangle<float> canvas() const;
    float xFor (double seconds) const;
    float yFor (float note) const;
    double secondsAt (float x) const;
    float noteAt (float y) const;

    void pushUndo (int start, int end);
    void undo();
    void writeRange (int a, int b, float noteA, float noteB, uint8_t mode);
    void snapVisible();
    void clearVisible();
    void setTool (Tool t);

    FlayrTuneProcessor& proc;
    tune::PitchTrack& track;

    juce::TextButton trackBtn { "TRACK" }, drawBtn { "DRAW" }, lineBtn { "LINE" }, noteBtn { "NOTE" },
        eraseBtn { "ERASE" }, snapBtn { "SNAP TO SCALE" }, undoBtn { "UNDO" }, clearBtn { "CLEAR VIEW" },
        followBtn { "FOLLOW" };

    Tool tool = Tool::draw;
    double viewStart = 0.0, viewLength = 8.0;
    float viewLow = 48.0f, viewHigh = 72.0f;

    bool dragging = false, userMovedPitchView = false, pitchViewPlaced = false;
    juce::Point<float> downPos, lastPos;
    std::vector<UndoEntry> undoStack;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphEditor)
};
