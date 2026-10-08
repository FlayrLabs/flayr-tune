#include "PluginEditor.h"
#include "GraphEditor.h"

namespace ui
{
static juce::Font font (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

static juce::String noteName (int midi)
{
    return juce::String (tune::kNoteNames[(size_t) (((midi % 12) + 12) % 12)]) + juce::String (midi / 12 - 1);
}

//==============================================================================
LookAndFeel::LookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, text);
    setColour (juce::ComboBox::backgroundColourId, panel);
    setColour (juce::ComboBox::outlineColourId, line);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::arrowColourId, accent);
    setColour (juce::PopupMenu::backgroundColourId, panel);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.25f));
    setColour (juce::TextButton::buttonColourId, panel);
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOffId, dim);
    setColour (juce::TextButton::textColourOnId, bg);
}

void LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                    float start, float end, juce::Slider& s)
{
    const auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
    const float r = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
    const auto c = b.getCentre();
    const float ang = start + pos * (end - start);
    const float thick = juce::jmax (3.0f, r * 0.09f);
    const bool bipolar = s.getProperties().contains ("bipolar");
    const float from = bipolar ? (start + end) * 0.5f : start;

    juce::Path track;
    track.addCentredArc (c.x, c.y, r - thick, r - thick, 0.0f, start, end, true);
    g.setColour (line);
    g.strokePath (track, { thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });

    juce::Path value;
    value.addCentredArc (c.x, c.y, r - thick, r - thick, 0.0f, juce::jmin (from, ang), juce::jmax (from, ang), true);
    g.setGradientFill (juce::ColourGradient (accent, c.x - r, c.y, accent2, c.x + r, c.y, false));
    g.strokePath (value, { thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });

    const float kr = r - thick * 2.6f;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a4252), c.x, c.y - kr,
                                             juce::Colour (0xff171a21), c.x, c.y + kr, false));
    g.fillEllipse (c.x - kr, c.y - kr, kr * 2.0f, kr * 2.0f);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawEllipse (c.x - kr, c.y - kr, kr * 2.0f, kr * 2.0f, 1.0f);

    juce::Path pointer;
    pointer.addRoundedRectangle (-1.5f, -kr + 3.0f, 3.0f, kr * 0.42f, 1.5f);
    g.setColour (text);
    g.fillPath (pointer, juce::AffineTransform::rotation (ang).translated (c.x, c.y));
}

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = b.getToggleState();
    const float alpha = b.isEnabled() ? 1.0f : 0.3f;
    g.setColour ((on ? accent : (over ? line.brighter (0.2f) : panel)).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour ((on ? accent.brighter (0.3f) : line).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (r, 5.0f, 1.0f);
}

//==============================================================================
void PitchGraph::push (const tune::DisplayPoint& p)
{
    hist[(size_t) head] = p;
    head = (head + 1) % (int) hist.size();
    if (! p.voiced)
        return;
    // Jump straight to the singer's range on the first note, then only move the view
    // when the voice leaves its middle, so the grid stays still while someone sings.
    if (! hasCentre)
    {
        centre = targetCentre = std::round (p.outNote);
        hasCentre = true;
    }
    else if (std::abs (p.outNote - targetCentre) > 5.0f)
    {
        targetCentre = std::round (p.outNote);
    }
}

void PitchGraph::tick()
{
    centre += 0.2f * (targetCentre - centre);
    if (std::abs (targetCentre - centre) < 0.01f)
        centre = targetCentre;
}

void PitchGraph::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (b, 8.0f);

    const float span = 8.0f;
    const float lo = centre - span, hi = centre + span;
    const float plotX = b.getX() + 44.0f, plotW = b.getWidth() - 52.0f;
    auto yOf = [&] (float note) { return b.getBottom() - 8.0f - (note - lo) / (hi - lo) * (b.getHeight() - 16.0f); };

    g.setFont (font (11.0f));
    for (int n = (int) lo; n <= (int) hi; ++n)
    {
        const int pc = ((n % 12) + 12) % 12;
        const bool on = (mask >> pc) & 1;
        const float y = yOf ((float) n);
        g.setColour (on ? line.brighter (0.35f) : line.withAlpha (0.45f));
        g.drawHorizontalLine ((int) y, plotX, plotX + plotW);
        g.setColour (on ? text.withAlpha (0.85f) : dim.withAlpha (0.5f));
        g.drawText (noteName (n), juce::Rectangle<float> (b.getX() + 4.0f, y - 7.0f, 36.0f, 14.0f), juce::Justification::centredRight);
    }

    const int n = (int) hist.size();
    auto drawTrace = [&] (bool output, juce::Colour colour, float width)
    {
        juce::Path path;
        bool drawing = false;
        for (int i = 0; i < n; ++i)
        {
            const auto& p = hist[(size_t) ((head + i) % n)];
            const float v = output ? p.outNote : p.inNote;
            if (! p.voiced || v < lo - 2 || v > hi + 2)
            {
                drawing = false;
                continue;
            }
            const float x = plotX + plotW * (float) i / (float) (n - 1);
            const float y = juce::jlimit (b.getY(), b.getBottom(), yOf (v));
            if (drawing)
                path.lineTo (x, y);
            else
                path.startNewSubPath (x, y);
            drawing = true;
        }
        g.setColour (colour);
        g.strokePath (path, { width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });
    };
    drawTrace (false, dim.withAlpha (0.7f), 1.4f);
    drawTrace (true, accent, 2.4f);

    g.setFont (font (11.0f, true));
    g.setColour (dim);
    g.drawText ("INPUT", juce::Rectangle<float> (plotX + plotW - 150.0f, b.getY() + 6.0f, 60.0f, 14.0f), juce::Justification::centredRight);
    g.setColour (accent);
    g.drawText ("CORRECTED", juce::Rectangle<float> (plotX + plotW - 86.0f, b.getY() + 6.0f, 80.0f, 14.0f), juce::Justification::centredRight);
}

//==============================================================================
juce::Rectangle<float> NoteStrip::keyRect (int pc) const
{
    static constexpr int whiteIndex[12] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 };
    const auto b = getLocalBounds().toFloat();
    const float ww = b.getWidth() / 7.0f;
    if (! isBlack (pc))
        return { b.getX() + ww * (float) whiteIndex[pc], b.getY(), ww, b.getHeight() };
    const float bw = ww * 0.6f;
    return { b.getX() + ww * (float) (whiteIndex[pc] + 1) - bw * 0.5f, b.getY(), bw, b.getHeight() * 0.6f };
}

int NoteStrip::keyAt (juce::Point<float> p) const
{
    for (int pc = 0; pc < 12; ++pc)
        if (isBlack (pc) && keyRect (pc).contains (p))
            return pc;
    for (int pc = 0; pc < 12; ++pc)
        if (! isBlack (pc) && keyRect (pc).contains (p))
            return pc;
    return -1;
}

void NoteStrip::paint (juce::Graphics& g)
{
    auto drawKey = [&] (int pc)
    {
        auto r = keyRect (pc).reduced (1.5f);
        const bool inScale = (scaleMask >> pc) & 1;
        const bool isRemoved = proc.apvts.getRawParameterValue (FlayrTuneProcessor::removeId (pc))->load() > 0.5f;
        const bool active = (activeMask >> pc) & 1;
        const bool isTarget = pc == target;

        juce::Colour fill = isBlack (pc) ? juce::Colour (0xff20242d) : juce::Colour (0xff2b313c);
        if (active)
            fill = accent.withAlpha (isBlack (pc) ? 0.45f : 0.30f).overlaidWith (juce::Colours::transparentBlack);
        if (isTarget && active)
            fill = accent;
        g.setColour (fill);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (bg);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);

        const auto label = r.removeFromBottom (18.0f);
        g.setFont (font (11.0f, true));
        g.setColour (isTarget && active ? bg : (inScale ? text : dim.withAlpha (0.5f)));
        g.drawText (tune::kNoteNames[(size_t) pc], label, juce::Justification::centred);

        if (isRemoved && inScale)
        {
            const auto c = r.withTrimmedBottom (4.0f).getCentre();
            const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.18f;
            g.setColour (removed);
            g.drawLine (c.x - s, c.y - s, c.x + s, c.y + s, 2.5f);
            g.drawLine (c.x - s, c.y + s, c.x + s, c.y - s, 2.5f);
        }
    };
    for (int pc = 0; pc < 12; ++pc)
        if (! isBlack (pc))
            drawKey (pc);
    for (int pc = 0; pc < 12; ++pc)
        if (isBlack (pc))
            drawKey (pc);
}

void NoteStrip::mouseDown (const juce::MouseEvent& e)
{
    const int pc = keyAt (e.position);
    if (pc < 0)
        return;
    if (auto* p = proc.apvts.getParameter (FlayrTuneProcessor::removeId (pc)))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.0f : 1.0f);
        p->endChangeGesture();
    }
    repaint();
}
} // namespace ui

//==============================================================================
FlayrTuneEditor::FlayrTuneEditor (FlayrTuneProcessor& p)
    : AudioProcessorEditor (&p), proc (p), notes (p)
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (graph);
    addAndMakeVisible (notes);

    for (auto* l : { &noteReadout, &centsReadout })
    {
        l->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (*l);
    }
    noteReadout.setFont (ui::font (34.0f, true));
    centsReadout.setFont (ui::font (14.0f, true));

    addCombo (keyBox, "key", keyAtt);
    addCombo (scaleBox, "scale", scaleAtt);
    addCombo (inputBox, "inputType", inputAtt);

    for (int i = 0; i < proc.getNumPrograms(); ++i)
        presetBox.addItem (proc.getProgramName (i), i + 1);
    presetBox.setTextWhenNothingSelected ("Presets");
    presetBox.onChange = [this] {
        if (presetBox.getSelectedId() > 0)
            proc.setCurrentProgram (presetBox.getSelectedId() - 1);
    };
    addAndMakeVisible (presetBox);

    for (auto* b : { &formantBtn, &midiBtn, &liveBtn, &learnBtn, &graphBtn })
    {
        b->setClickingTogglesState (true);
        addAndMakeVisible (*b);
    }
    liveBtn.setTooltip ("Live mode: near-zero latency (about 1.5 to 10 ms, depending on the voice) for tracking and "
                        "performing. Studio has the best quality on big shifts");
    liveAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "live", liveBtn);
    graphBtn.setTooltip ("Graph mode: capture the vocal along the song, then draw exact pitch, lines or notes");
    graphAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "graph", graphBtn);
    graphEditor = std::make_unique<GraphEditor> (proc);
    addChildComponent (*graphEditor);
    learnBtn.setTooltip ("Play or sing the part, then click again: sets Key and Scale from what it heard");
    learnBtn.onClick = [this] {
        if (learnBtn.getToggleState())
            proc.engine.beginKeyLearn();
        else
            finishKeyLearn();
    };
    latencyLabel.setJustificationType (juce::Justification::centred);
    latencyLabel.setFont (ui::font (11.0f, true));
    latencyLabel.setColour (juce::Label::textColourId, ui::dim);
    addAndMakeVisible (latencyLabel);
    formantBtn.setTooltip ("Preserve formants so shifted notes keep the singer's natural tone");
    midiBtn.setTooltip ("Use notes from the side-chain MIDI input as targets");
    formantAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "formant", formantBtn);
    midiAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "midiTarget", midiBtn);

    retune = &addKnob ("retune", "RETUNE SPEED", " ms");
    humanize = &addKnob ("humanize", "HUMANIZE", " %");
    flex = &addKnob ("flex", "EXPRESSION", " %");

    auto* formantShift = &addKnob ("formantShift", "FORMANT SHIFT", " st");
    formantShift->slider.setTooltip ("Moves the voice's formants: up = brighter, smaller voice; down = deeper, bigger voice");
    voiceRow = { formantShift, &addKnob ("transpose", "TRANSPOSE", " st"),
                 &addKnob ("concertA", "CONCERT A", " Hz"), &addKnob ("tracking", "TRACKING", " %") };
    vibRow = { &addKnob ("natvib", "NATURAL VIB", " dB"), &addKnob ("vibRate", "VIB RATE", " Hz"),
               &addKnob ("vibDepth", "VIB DEPTH", " c"), &addKnob ("vibDelay", "VIB ONSET", " ms") };
    outRow = { &addKnob ("mix", "MIX", " %"), &addKnob ("output", "OUTPUT", " dB") };

    for (auto* k : { voiceRow[0], voiceRow[1], vibRow[0], outRow[1] })
        k->slider.getProperties().set ("bipolar", true);

    retune->slider.setTooltip ("How fast notes are pulled to pitch. 0 = hard tune effect");
    humanize->slider.setTooltip ("Slower correction on sustained notes so long notes keep their natural drift");
    flex->slider.setTooltip ("Only correct notes that are already close to the target; leaves scoops and bends alone");

    setSize (1200, 640);
    // Readings queued while the window was closed are stale; start from live data.
    for (tune::DisplayPoint stale; proc.engine.popDisplay (stale);)
    {
    }
    startTimerHz (40);
}

FlayrTuneEditor::~FlayrTuneEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

FlayrTuneEditor::Knob& FlayrTuneEditor::addKnob (const juce::String& id, const juce::String& name, const juce::String& suffix)
{
    auto k = std::make_unique<Knob>();
    k->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 18);
    k->slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    k->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k->slider);
    k->slider.setTextValueSuffix (suffix);
    k->label.setText (name, juce::dontSendNotification);
    k->label.setJustificationType (juce::Justification::centred);
    k->label.setFont (ui::font (11.0f, true));
    k->label.setColour (juce::Label::textColourId, ui::dim);
    addAndMakeVisible (k->slider);
    addAndMakeVisible (k->label);
    knobs.push_back (std::move (k));
    return *knobs.back();
}

void FlayrTuneEditor::addCombo (juce::ComboBox& box, const juce::String& id,
                                std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>& att)
{
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (id)))
        box.addItemList (choice->choices, 1);
    att = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, id, box);
    addAndMakeVisible (box);
}

void FlayrTuneEditor::timerCallback()
{
    graph.tick();
    tune::DisplayPoint p;
    bool any = false;
    while (proc.engine.popDisplay (p))
    {
        graph.push (p);
        lastVoiced = p.voiced;
        if (p.voiced)
        {
            lastIn = p.inNote;
            lastTarget = p.targetNote;
        }
        any = true;
    }

    const auto mask = proc.engine.getActiveMask();
    const int key = (int) proc.apvts.getRawParameterValue ("key")->load();
    const int scale = (int) proc.apvts.getRawParameterValue ("scale")->load();
    const int targetPc = lastVoiced && lastTarget >= 0 ? ((int) lastTarget) % 12 : -1;
    graph.setMask (mask);
    notes.setState (tune::scalePitchClasses (key, scale), mask, targetPc);

    if (lastVoiced && lastTarget >= 0)
    {
        noteReadout.setText (ui::noteName ((int) lastTarget), juce::dontSendNotification);
        const int c = (int) std::lround ((lastIn - lastTarget) * 100.0f);
        centsReadout.setText ((c > 0 ? "+" : "") + juce::String (c) + " cents in", juce::dontSendNotification);
        centsReadout.setColour (juce::Label::textColourId, std::abs (c) <= 10 ? ui::accent : ui::accent2);
    }
    else
    {
        noteReadout.setText ("--", juce::dontSendNotification);
        centsReadout.setText (lastVoiced ? "no target" : "listening", juce::dontSendNotification);
        centsReadout.setColour (juce::Label::textColourId, ui::dim);
    }

    const bool graphOn = proc.apvts.getRawParameterValue ("graph")->load() > 0.5f;
    if (graphEditor->isVisible() != graphOn)
    {
        graphEditor->setVisible (graphOn);
        graph.setVisible (! graphOn);
        notes.setVisible (! graphOn);
    }
    if (graphOn)
        graphEditor->refresh();

    const bool live = proc.apvts.getRawParameterValue ("live")->load() > 0.5f;
    if (live)
    {
        const double now = 1000.0 * proc.engine.getLiveDelaySamples() / proc.engine.getSampleRate();
        liveDelayMs = std::isfinite (liveDelayMs) ? liveDelayMs + 0.15 * (now - liveDelayMs) : now;
        latencyLabel.setText ("LIVE  " + juce::String (liveDelayMs, 1) + " ms", juce::dontSendNotification);
        latencyLabel.setColour (juce::Label::textColourId, ui::accent);
    }
    else
    {
        const double ms = 1000.0 * proc.engine.getLatencySamples (false) / proc.engine.getSampleRate();
        latencyLabel.setText ("STUDIO  " + juce::String (juce::roundToInt (ms)) + " ms (compensated)", juce::dontSendNotification);
        latencyLabel.setColour (juce::Label::textColourId, ui::dim);
    }

    if (learnBtn.getToggleState())
    {
        const auto hist = proc.engine.getKeyHistogram();
        int key = 0;
        bool minor = false;
        learnBtn.setButtonText (tune::estimateKey (hist.data(), key, minor)
                                    ? juce::String (tune::kNoteNames[(size_t) key]) + (minor ? " MIN?" : " MAJ?")
                                    : juce::String ("LISTENING"));
    }

    if (any)
        graph.repaint();
    notes.repaint();
}

void FlayrTuneEditor::setParam (const juce::String& id, float value)
{
    if (auto* param = proc.apvts.getParameter (id))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (value));
        param->endChangeGesture();
    }
}

void FlayrTuneEditor::finishKeyLearn()
{
    proc.engine.endKeyLearn();
    const auto hist = proc.engine.getKeyHistogram();
    int key = 0;
    bool minor = false;
    if (tune::estimateKey (hist.data(), key, minor))
    {
        setParam ("key", (float) key);
        setParam ("scale", minor ? 2.0f : 1.0f);
    }
    learnBtn.setButtonText ("LEARN KEY");
}

void FlayrTuneEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::bg);

    auto header = getLocalBounds().removeFromTop (56).toFloat();
    g.setColour (ui::panel);
    g.fillRect (header);
    g.setColour (ui::line);
    g.drawHorizontalLine (56, 0.0f, (float) getWidth());

    g.setFont (ui::font (22.0f, true));
    g.setGradientFill (juce::ColourGradient (ui::accent, 20.0f, 0.0f, ui::accent2, 200.0f, 0.0f, false));
    g.drawText ("FLAYR TUNE", juce::Rectangle<float> (20.0f, 0.0f, 200.0f, 56.0f), juce::Justification::centredLeft);

    auto section = [&g] (const juce::String& title, juce::Rectangle<int> r)
    {
        g.setColour (ui::panel);
        g.fillRoundedRectangle (r.toFloat(), 8.0f);
        g.setColour (ui::dim);
        g.setFont (ui::font (10.0f, true));
        g.drawText (title, r.reduced (12, 6).removeFromTop (12), juce::Justification::topLeft);
    };
    section ("VOICE", { 20, 478, 480, 146 });
    section ("VIBRATO", { 510, 478, 480, 146 });
    section ("OUTPUT", { 1000, 478, 180, 146 });
}

void FlayrTuneEditor::resized()
{
    // Header
    auto header = getLocalBounds().removeFromTop (56).reduced (12, 12);
    header.removeFromLeft (172);
    midiBtn.setBounds (header.removeFromRight (100));
    header.removeFromRight (8);
    formantBtn.setBounds (header.removeFromRight (90));
    header.removeFromRight (8);
    liveBtn.setBounds (header.removeFromRight (64));
    header.removeFromRight (8);
    graphBtn.setBounds (header.removeFromRight (72));
    header.removeFromRight (16);
    presetBox.setBounds (header.removeFromRight (150));
    header.removeFromRight (8);
    inputBox.setBounds (header.removeFromRight (110));
    header.removeFromRight (8);
    learnBtn.setBounds (header.removeFromRight (100));
    header.removeFromRight (8);
    scaleBox.setBounds (header.removeFromRight (150));
    header.removeFromRight (8);
    keyBox.setBounds (header.removeFromRight (64));

    graph.setBounds (20, 72, 910, 290);
    notes.setBounds (20, 374, 910, 92);
    graphEditor->setBounds (20, 72, 910, 394);

    auto right = juce::Rectangle<int> (946, 72, 234, 394);
    latencyLabel.setBounds (right.removeFromTop (16));
    noteReadout.setBounds (right.removeFromTop (40));
    centsReadout.setBounds (right.removeFromTop (20));
    auto place = [] (Knob* k, juce::Rectangle<int> r)
    {
        k->label.setBounds (r.removeFromTop (16));
        k->slider.setBounds (r);
    };
    place (retune, right.removeFromTop (190).reduced (10, 4));
    auto small = right.reduced (0, 6);
    place (humanize, small.removeFromLeft (small.getWidth() / 2).reduced (6, 0));
    place (flex, small.reduced (6, 0));

    auto row = [&place] (const std::vector<Knob*>& ks, juce::Rectangle<int> r)
    {
        r = r.reduced (6, 0).withTrimmedTop (20).withTrimmedBottom (6);
        const int w = r.getWidth() / (int) ks.size();
        for (auto* k : ks)
            place (k, r.removeFromLeft (w).reduced (2, 0));
    };
    row (voiceRow, { 20, 478, 480, 146 });
    row (vibRow, { 510, 478, 480, 146 });
    row (outRow, { 1000, 478, 180, 146 });
}
