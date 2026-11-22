// Graph mode under simulated hosts (issue #11): drives the real processor through
// processBlock the way each host does, then checks what the Graph editor draws, i.e. the
// captured pitch line (PitchTrack::inputAt over the timeline) and the playhead.
// The Audacity 4.0.2 behaviour is from its sources: au3-vst3/VST3Wrapper.cpp (kPlaying, no
// projectTimeSamples, a 64-sample silent block and setActive(true) at each start) and
// au3-audio-unit/AudioUnitInstance.cpp (no host callbacks, mSampleTime from 0 per start).
// Built with the plug-in sources (see CMakeLists.txt); run by ctest.

#include "PluginProcessor.h"

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

static void check (bool ok, const std::string& what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (! ok)
        ++failures;
}

enum class Host
{
    audacityVst3, // isPlaying, position fields present but always 0, no tempo or ppq
    audacityAu,   // JUCE's AU wrapper with no host callbacks: not playing, sample clock from 0
    noPlayhead,   // no playhead at all
    daw,          // Logic / Live: playing, position and ppq move with the audio
    stopped       // transport stopped at 2 s, audio still flowing (e.g. monitoring)
};

struct SimHead : juce::AudioPlayHead
{
    Host host = Host::daw;
    juce::int64 rendered = 0; // samples since playback started
    juce::int64 start = 0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        switch (host)
        {
            case Host::audacityVst3:
                info.setIsPlaying (true);
                info.setTimeInSamples (juce::int64 (0));
                info.setTimeInSeconds (0.0);
                break;
            case Host::audacityAu:
                info.setIsPlaying (false);
                info.setTimeInSamples (rendered);
                info.setTimeInSeconds ((double) rendered / 48000.0);
                break;
            case Host::daw:
                info.setIsPlaying (true);
                info.setBpm (120.0);
                info.setTimeInSamples (start + rendered);
                info.setTimeInSeconds ((double) (start + rendered) / 48000.0);
                info.setPpqPosition ((double) (start + rendered) / 48000.0 * 2.0);
                break;
            case Host::stopped:
                info.setIsPlaying (false);
                info.setBpm (120.0);
                info.setTimeInSamples (start);
                info.setTimeInSeconds ((double) start / 48000.0);
                info.setPpqPosition ((double) start / 48000.0 * 2.0);
                break;
            case Host::noPlayhead:
                break;
        }
        return info;
    }
};

static constexpr double kRate = 48000.0, kSeconds = 3.0, kF0 = 196.0, kF1 = 392.0; // G3 -> G4
static double glideNote (double t) // MIDI note sung t seconds after playback start
{
    t = juce::jlimit (0.0, kSeconds, t);
    return 69.0 + 12.0 * std::log2 (kF0 * std::pow (kF1 / kF0, t / kSeconds) / 440.0);
}

// One playback pass of the glide in uneven blocks (as Audacity sends them, incl. empty calls).
static void playPass (FlayrTuneProcessor& p, SimHead& head)
{
    static const int sizes[] = { 512, 441, 1024, 37, 0, 300, 777, 128, 1, 1000 };
    juce::AudioBuffer<float> buf (2, 1024);
    juce::MidiBuffer none;
    double phase = 0.0;
    head.rendered = 0;
    if (head.host == Host::audacityVst3)
    {
        // VST3Wrapper::Initialize sends a 64-sample silent block with kPlaying set first.
        buf.setSize (2, 64, false, false, true);
        buf.clear();
        p.processBlock (buf, none);
    }
    const auto total = (juce::int64) (kSeconds * kRate);
    for (int b = 0; head.rendered < total; ++b)
    {
        const int n = (int) juce::jmin ((juce::int64) sizes[b % 10], total - head.rendered);
        buf.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) (head.rendered + i) / kRate;
            phase += juce::MathConstants<double>::twoPi * kF0 * std::pow (kF1 / kF0, t / kSeconds) / kRate;
            buf.setSample (0, i, 0.3f * (float) std::sin (phase));
            buf.setSample (1, i, buf.getSample (0, i));
        }
        p.processBlock (buf, none);
        head.rendered += n;
    }
}

struct Drawn
{
    int points = 0, misplaced = 0, backwards = 0;
    double first = -1.0, last = -1.0;
};

// What GraphEditor's "captured input" trace walks: every finite inputAt() on the timeline.
static Drawn drawn (const FlayrTuneProcessor& p, double startSec)
{
    Drawn d;
    float prev = NAN;
    for (int i = 0; i < p.track.getExtent(); ++i)
    {
        const float v = p.track.inputAt (i);
        if (! std::isfinite (v))
            continue;
        const double t = tune::PitchTrack::secondsAt (i);
        ++d.points;
        if (d.first < 0.0)
            d.first = t;
        d.last = t;
        if (std::abs (v - glideNote (t - startSec)) > 0.5)
            ++d.misplaced;
        if (std::isfinite (prev) && v < prev - 0.25f)
            ++d.backwards;
        prev = v;
    }
    return d;
}

// reprepare: the host re-activates the plug-in (prepareToPlay) at each playback start, as
// Audacity 4's VST3Wrapper::Initialize does (setActive(true)); its AU host only resets.
static void run (const char* name, Host host, juce::int64 start, bool expectLine, int passes = 1, bool reprepare = true)
{
    std::printf ("%s\n", name);
    FlayrTuneProcessor p;
    p.apvts.getParameter ("graph")->setValueNotifyingHost (1.0f);
    p.trackArmed.store (true);
    SimHead head;
    head.host = host;
    head.start = start;
    p.setPlayHead (host == Host::noPlayhead ? nullptr : &head);
    for (int pass = 0; pass < passes; ++pass)
    {
        if (pass == 0 || reprepare)
            p.prepareToPlay (kRate, 1024);
        else
            p.reset();
        playPass (p, head);
        if (reprepare)
            p.releaseResources();
    }

    const double startSec = (double) start / kRate;
    const auto d = drawn (p, startSec);
    const int expected = (int) (kSeconds / tune::PitchTrack::kGridSec);
    std::printf ("  drawn %d points from %.3f s to %.3f s, %d off the glide, %d backwards; extent %.3f s, "
                 "playhead %.3f s, rolling %d, estimated %d\n",
                 d.points, d.first, d.last, d.misplaced, d.backwards,
                 tune::PitchTrack::secondsAt (p.track.getExtent()), p.playheadSeconds.load(),
                 (int) p.hostPlaying.load(), (int) p.timelineEstimated.load());
    if (expectLine)
    {
        check (d.points > expected * 9 / 10, "the pitch line covers the pass");
        check (d.first >= startSec - 0.01 && d.first < startSec + 0.1 && d.last > startSec + kSeconds - 0.1
                   && d.last < startSec + kSeconds + 0.01,
               "it sits where playback started and spans the pass");
        check (d.backwards == 0 && d.misplaced < d.points / 50, "time-ordered: the rising glide rises, in place");
        check (p.hostPlaying.load() && std::abs (p.playheadSeconds.load() - (startSec + kSeconds)) < 0.05,
               "playhead follows the audio");
        check (p.timelineEstimated.load() == (host != Host::daw), "the estimated-timeline notice shows only without a host position");
    }
    else
    {
        check (d.points == 0 && p.track.getExtent() == 0 && ! p.hostPlaying.load(), "nothing drawn while stopped");
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    run ("Audacity 4 VST3: playing, position stuck at 0", Host::audacityVst3, 0, true);
    run ("Audacity 4 VST3: second playback pass restarts at 0", Host::audacityVst3, 0, true, 2);
    run ("Audacity 4 AU: no transport, sample clock from 0", Host::audacityAu, 0, true);
    run ("Audacity 4 AU: second pass, clock restarts at 0, reset() only", Host::audacityAu, 0, true, 2, false);
    run ("Audacity 4 AU: no playhead at all", Host::noPlayhead, 0, true);
    run ("Logic / Live: playing from 4 s", Host::daw, (juce::int64) (4 * kRate), true);
    run ("Stopped at 2 s, audio flowing", Host::stopped, (juce::int64) (2 * kRate), false);

    std::printf ("graph_host_test: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
