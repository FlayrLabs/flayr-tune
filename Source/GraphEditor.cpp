#include "GraphEditor.h"
#include "PluginEditor.h"

namespace
{
juce::Font font (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

juce::String noteName (int midi)
{
    return juce::String (tune::kNoteNames[(size_t) (((midi % 12) + 12) % 12)]) + juce::String (midi / 12 - 1);
}

constexpr int kToolbarHeight = 34;
constexpr float kLabelWidth = 40.0f;
constexpr size_t kMaxUndo = 40;
} // namespace

GraphEditor::GraphEditor (FlayrTuneProcessor& p) : proc (p), track (p.track)
{
    setWantsKeyboardFocus (true);

    for (auto* b : { &trackBtn, &drawBtn, &lineBtn, &noteBtn, &eraseBtn, &snapBtn, &undoBtn, &clearBtn, &followBtn })
        addAndMakeVisible (*b);

    trackBtn.setClickingTogglesState (true);
    trackBtn.setTooltip ("Capture the sung pitch while the song plays");
    trackBtn.onClick = [this] { proc.trackArmed.store (trackBtn.getToggleState()); };
    trackBtn.setToggleState (proc.trackArmed.load(), juce::dontSendNotification);

    followBtn.setClickingTogglesState (true);
    followBtn.setToggleState (true, juce::dontSendNotification);

    drawBtn.setTooltip ("Freehand: the output follows exactly what you draw");
    lineBtn.setTooltip ("Straight line between two points (slides, held notes)");
    noteBtn.setTooltip ("Drag a note onto a semitone; the singer's vibrato is kept");
    eraseBtn.setTooltip ("Remove edits; erased spots go back to automatic correction");
    snapBtn.setTooltip ("Turn everything visible into notes on the current scale (vibrato kept)");
    clearBtn.setTooltip ("Remove every edit in view");

    drawBtn.onClick = [this] { setTool (Tool::draw); };
    lineBtn.onClick = [this] { setTool (Tool::line); };
    noteBtn.onClick = [this] { setTool (Tool::note); };
    eraseBtn.onClick = [this] { setTool (Tool::erase); };
    snapBtn.onClick = [this] { snapVisible(); };
    undoBtn.onClick = [this] { undo(); };
    clearBtn.onClick = [this] { clearVisible(); };
    setTool (Tool::draw);
}

void GraphEditor::setTool (Tool t)
{
    tool = t;
    drawBtn.setToggleState (t == Tool::draw, juce::dontSendNotification);
    lineBtn.setToggleState (t == Tool::line, juce::dontSendNotification);
    noteBtn.setToggleState (t == Tool::note, juce::dontSendNotification);
    eraseBtn.setToggleState (t == Tool::erase, juce::dontSendNotification);
    setMouseCursor (t == Tool::erase ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
}

void GraphEditor::resized()
{
    auto bar = getLocalBounds().removeFromTop (kToolbarHeight).reduced (6, 4);
    trackBtn.setBounds (bar.removeFromLeft (76));
    bar.removeFromLeft (14);
    for (auto* b : { &drawBtn, &lineBtn, &noteBtn, &eraseBtn })
    {
        b->setBounds (bar.removeFromLeft (64));
        bar.removeFromLeft (4);
    }
    followBtn.setBounds (bar.removeFromRight (74));
    bar.removeFromRight (8);
    clearBtn.setBounds (bar.removeFromRight (96));
    bar.removeFromRight (4);
    undoBtn.setBounds (bar.removeFromRight (60));
    bar.removeFromRight (4);
    snapBtn.setBounds (bar.removeFromRight (118));
}

juce::Rectangle<float> GraphEditor::canvas() const
{
    return getLocalBounds().toFloat().withTrimmedTop ((float) kToolbarHeight).withTrimmedLeft (kLabelWidth).reduced (0.0f, 6.0f);
}

float GraphEditor::xFor (double seconds) const
{
    const auto c = canvas();
    return c.getX() + (float) ((seconds - viewStart) / viewLength) * c.getWidth();
}

float GraphEditor::yFor (float note) const
{
    const auto c = canvas();
    return c.getBottom() - (note - viewLow) / (viewHigh - viewLow) * c.getHeight();
}

double GraphEditor::secondsAt (float x) const
{
    const auto c = canvas();
    return viewStart + (double) ((x - c.getX()) / c.getWidth()) * viewLength;
}

float GraphEditor::noteAt (float y) const
{
    const auto c = canvas();
    return viewLow + (c.getBottom() - y) / c.getHeight() * (viewHigh - viewLow);
}

//==============================================================================
void GraphEditor::refresh()
{
    const double head = proc.playheadSeconds.load();

    // Until the user scrolls the pitch axis, keep the visible singing centred.
    if (! userMovedPitchView && ! dragging)
    {
        std::vector<float> seen;
        const int a = juce::jmax (0, tune::PitchTrack::indexFor (viewStart));
        const int b = juce::jmin (tune::PitchTrack::kCapacity - 1, tune::PitchTrack::indexFor (viewStart + viewLength));
        for (int i = a; i <= b; i += 4)
            if (std::isfinite (track.inputAt (i)))
                seen.push_back (track.inputAt (i));
        if (! seen.empty())
        {
            std::nth_element (seen.begin(), seen.begin() + (long) seen.size() / 2, seen.end());
            const float span = viewHigh - viewLow;
            viewLow = std::round (seen[seen.size() / 2] - span * 0.5f);
            viewHigh = viewLow + span;
        }
    }
    if (followBtn.getToggleState() && proc.hostPlaying.load() && ! dragging)
    {
        // Keep the playhead about a third in, and the singing in view.
        if (head < viewStart + viewLength * 0.1 || head > viewStart + viewLength * 0.7)
            viewStart = juce::jmax (0.0, head - viewLength * 0.3);
        const float now = track.inputAt (tune::PitchTrack::indexFor (head));
        if (std::isfinite (now) && (now < viewLow + 2.0f || now > viewHigh - 2.0f))
        {
            const float span = viewHigh - viewLow;
            viewLow = std::round (now - span * 0.5f);
            viewHigh = viewLow + span;
        }
    }
    repaint();
}

void GraphEditor::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (ui::panel);
    g.fillRoundedRectangle (bounds, 8.0f);
    const auto c = canvas();

    // Semitone grid, scale notes brighter.
    const auto mask = proc.engine.getActiveMask();
    g.setFont (font (11.0f));
    for (int n = (int) std::floor (viewLow); n <= (int) std::ceil (viewHigh); ++n)
    {
        const float y = yFor ((float) n);
        if (y < c.getY() - 1 || y > c.getBottom() + 1)
            continue;
        const bool on = (mask >> (((n % 12) + 12) % 12)) & 1;
        g.setColour (on ? ui::line.brighter (0.35f) : ui::line.withAlpha (0.4f));
        g.drawHorizontalLine ((int) y, c.getX(), c.getRight());
        g.setColour (on ? ui::text.withAlpha (0.85f) : ui::dim.withAlpha (0.45f));
        g.drawText (noteName (n), juce::Rectangle<float> (bounds.getX() + 2.0f, y - 7.0f, kLabelWidth - 6.0f, 14.0f),
                    juce::Justification::centredRight);
    }

    // Seconds grid.
    const double step = viewLength > 30 ? 5.0 : viewLength > 12 ? 2.0 : viewLength > 4 ? 1.0 : 0.25;
    g.setFont (font (10.0f));
    for (double t = std::ceil (viewStart / step) * step; t < viewStart + viewLength; t += step)
    {
        const float x = xFor (t);
        g.setColour (ui::line.withAlpha (0.6f));
        g.drawVerticalLine ((int) x, c.getY(), c.getBottom());
        g.setColour (ui::dim);
        const int m = (int) (t / 60.0);
        g.drawText (juce::String::formatted ("%d:%05.2f", m, t - m * 60.0),
                    juce::Rectangle<float> (x + 3.0f, c.getBottom() - 14.0f, 60.0f, 12.0f), juce::Justification::left);
    }

    const juce::Graphics::ScopedSaveState saveState (g);
    g.reduceClipRegion (c.toNearestInt());

    const int i0 = juce::jmax (0, tune::PitchTrack::indexFor (viewStart) - 1);
    const int i1 = juce::jmin (tune::PitchTrack::kCapacity - 1, tune::PitchTrack::indexFor (viewStart + viewLength) + 1);
    const int stride = juce::jmax (1, (i1 - i0) / juce::jmax (1, (int) c.getWidth()));

    auto trace = [&] (auto valueAt, juce::Colour colour, float width)
    {
        juce::Path path;
        bool drawing = false;
        for (int i = i0; i <= i1; i += stride)
        {
            const float v = valueAt (i);
            if (! std::isfinite (v))
            {
                drawing = false;
                continue;
            }
            const float x = xFor (tune::PitchTrack::secondsAt (i)), y = yFor (v);
            drawing ? path.lineTo (x, y) : path.startNewSubPath (x, y);
            drawing = true;
        }
        g.setColour (colour);
        g.strokePath (path, { width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });
    };

    // Captured input.
    trace ([this] (int i) { return track.inputAt (i); }, ui::dim.withAlpha (0.8f), 1.5f);

    // Note edits: a bar on the note, plus what will be heard (note + the singer's vibrato).
    for (int i = i0; i <= i1;)
    {
        if (track.modeAt (i) != tune::PitchTrack::kNote)
        {
            ++i;
            continue;
        }
        const float n = track.targetAt (i);
        int j = i;
        while (j <= i1 && track.modeAt (j) == tune::PitchTrack::kNote && track.targetAt (j) == n)
            ++j;
        const float x0 = xFor (tune::PitchTrack::secondsAt (i)), x1 = xFor (tune::PitchTrack::secondsAt (j));
        const float h = juce::jmax (6.0f, c.getHeight() / (viewHigh - viewLow) * 0.7f);
        g.setColour (ui::accent.withAlpha (0.22f));
        g.fillRoundedRectangle (x0, yFor (n) - h * 0.5f, juce::jmax (2.0f, x1 - x0), h, 4.0f);
        g.setColour (ui::accent.withAlpha (0.7f));
        g.drawRoundedRectangle (x0, yFor (n) - h * 0.5f, juce::jmax (2.0f, x1 - x0), h, 4.0f, 1.0f);
        i = j;
    }

    // Exact edits.
    trace ([this] (int i) {
        return track.modeAt (i) == tune::PitchTrack::kExact ? track.targetAt (i) : NAN;
    }, ui::accent, 2.4f);

    // Preview of the line / note being dragged.
    if (dragging && (tool == Tool::line || tool == Tool::note))
    {
        g.setColour (ui::accent2);
        const float yA = tool == Tool::note ? yFor (std::round (noteAt (downPos.y))) : downPos.y;
        const float yB = tool == Tool::note ? yA : lastPos.y;
        g.drawLine (downPos.x, yA, lastPos.x, yB, 2.4f);
    }

    // Playhead.
    const float px = xFor (proc.playheadSeconds.load());
    g.setColour (ui::text.withAlpha (proc.hostPlaying.load() ? 0.9f : 0.4f));
    g.drawVerticalLine ((int) px, c.getY(), c.getBottom());

    if (track.getExtent() == 0)
    {
        g.setColour (ui::dim);
        g.setFont (font (14.0f, true));
        g.drawText ("Press TRACK and play the song to capture the vocal's pitch", c, juce::Justification::centred);
    }
}

//==============================================================================
void GraphEditor::pushUndo (int start, int end)
{
    start = juce::jmax (0, start);
    end = juce::jmin (tune::PitchTrack::kCapacity, end);
    if (end <= start)
        return;
    UndoEntry u;
    u.start = start;
    u.target.resize ((size_t) (end - start));
    u.mode.resize ((size_t) (end - start));
    for (int i = start; i < end; ++i)
    {
        u.target[(size_t) (i - start)] = track.targetAt (i);
        u.mode[(size_t) (i - start)] = track.modeAt (i);
    }
    undoStack.push_back (std::move (u));
    if (undoStack.size() > kMaxUndo)
        undoStack.erase (undoStack.begin());
}

void GraphEditor::undo()
{
    if (undoStack.empty())
        return;
    const auto u = std::move (undoStack.back());
    undoStack.pop_back();
    for (size_t k = 0; k < u.target.size(); ++k)
        track.setTarget (u.start + (int) k, u.target[k], u.mode[k]);
    repaint();
}

void GraphEditor::writeRange (int a, int b, float noteA, float noteB, uint8_t mode)
{
    if (a > b)
    {
        std::swap (a, b);
        std::swap (noteA, noteB);
    }
    for (int i = a; i <= b; ++i)
    {
        const float t = b == a ? 0.0f : (float) (i - a) / (float) (b - a);
        track.setTarget (i, mode == tune::PitchTrack::kNone ? NAN : noteA + t * (noteB - noteA), mode);
    }
}

void GraphEditor::snapVisible()
{
    const int a = juce::jmax (0, tune::PitchTrack::indexFor (viewStart));
    const int b = juce::jmin (tune::PitchTrack::kCapacity - 1, tune::PitchTrack::indexFor (viewStart + viewLength));
    pushUndo (a, b + 1);
    const auto mask = proc.engine.getActiveMask();
    if (mask == 0)
        return;

    // Each voiced phrase becomes notes: median over +/-40 ms (ignores vibrato and blips),
    // then the nearest scale note, held until the voice settles on another one.
    constexpr int halfWin = 8;
    int current = -1;
    for (int i = a; i <= b; ++i)
    {
        if (! std::isfinite (track.inputAt (i)))
        {
            current = -1;
            continue;
        }
        std::vector<float> w;
        for (int k = i - halfWin; k <= i + halfWin; ++k)
            if (std::isfinite (track.inputAt (k)))
                w.push_back (track.inputAt (k));
        std::nth_element (w.begin(), w.begin() + (long) w.size() / 2, w.end());
        const float centre = w[w.size() / 2];

        int best = -1;
        float bestDist = 1e9f;
        for (int k = (int) std::round (centre) - 6; k <= (int) std::round (centre) + 6; ++k)
            if (((mask >> (((k % 12) + 12) % 12)) & 1) && std::abs ((float) k - centre) < bestDist)
            {
                bestDist = std::abs ((float) k - centre);
                best = k;
            }
        if (current < 0 || std::abs ((float) current - centre) > bestDist + 0.25f)
            current = best;
        track.setTarget (i, (float) current, tune::PitchTrack::kNote);
    }
    repaint();
}

void GraphEditor::clearVisible()
{
    const int a = juce::jmax (0, tune::PitchTrack::indexFor (viewStart));
    const int b = juce::jmin (tune::PitchTrack::kCapacity - 1, tune::PitchTrack::indexFor (viewStart + viewLength));
    pushUndo (a, b + 1);
    writeRange (a, b, NAN, NAN, tune::PitchTrack::kNone);
    repaint();
}

//==============================================================================
void GraphEditor::mouseDown (const juce::MouseEvent& e)
{
    if (! canvas().contains (e.position))
        return;
    grabKeyboardFocus();
    dragging = true;
    downPos = lastPos = e.position;
    pushUndo (tune::PitchTrack::indexFor (viewStart) - 2, tune::PitchTrack::indexFor (viewStart + viewLength) + 3);

    if (tool == Tool::draw || tool == Tool::erase)
        mouseDrag (e);
}

void GraphEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;
    const auto pos = e.position.withX (juce::jlimit (canvas().getX(), canvas().getRight(), e.position.x));
    const int a = tune::PitchTrack::indexFor (secondsAt (lastPos.x));
    const int b = tune::PitchTrack::indexFor (secondsAt (pos.x));

    if (tool == Tool::draw)
        writeRange (a, b, noteAt (lastPos.y), noteAt (pos.y), tune::PitchTrack::kExact);
    else if (tool == Tool::erase)
        writeRange (a, b, NAN, NAN, tune::PitchTrack::kNone);

    lastPos = pos; // draw/erase continue from here; line/note use it as the preview end
    repaint();
}

void GraphEditor::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;
    const int a = tune::PitchTrack::indexFor (secondsAt (downPos.x));
    const int b = tune::PitchTrack::indexFor (secondsAt (lastPos.x));
    if (tool == Tool::line)
        writeRange (a, b, noteAt (downPos.y), noteAt (lastPos.y), tune::PitchTrack::kExact);
    else if (tool == Tool::note)
    {
        const float n = std::round (noteAt (downPos.y));
        writeRange (a, b, n, n, tune::PitchTrack::kNote);
    }
    dragging = false;
    repaint();
}

void GraphEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        mouseMagnify (e, w.deltaY > 0 ? 1.15f : 1.0f / 1.15f);
        return;
    }
    const float dx = e.mods.isShiftDown() ? w.deltaY : w.deltaX;
    const float dy = e.mods.isShiftDown() ? 0.0f : w.deltaY;
    viewStart = juce::jmax (0.0, viewStart - dx * viewLength * 0.5);
    const float shift = dy * (viewHigh - viewLow) * 0.5f;
    viewLow = juce::jlimit (12.0f, 100.0f, viewLow + shift);
    viewHigh = viewLow + 24.0f;
    if (std::abs (dy) > 0.0f)
        userMovedPitchView = true;
    if (std::abs (dx) > 0.0f)
        followBtn.setToggleState (false, juce::dontSendNotification);
    repaint();
}

void GraphEditor::mouseMagnify (const juce::MouseEvent& e, float scale)
{
    const double anchor = secondsAt (e.position.x);
    const double newLength = juce::jlimit (0.5, 120.0, viewLength / scale);
    viewStart = juce::jmax (0.0, anchor - (anchor - viewStart) * newLength / viewLength);
    viewLength = newLength;
    repaint();
}

bool GraphEditor::keyPressed (const juce::KeyPress& k)
{
    if (k.getModifiers().isCommandDown() && k.getKeyCode() == 'Z')
    {
        undo();
        return true;
    }
    return false;
}
