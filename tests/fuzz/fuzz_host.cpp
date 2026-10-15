// Fuzz target: everything a host feeds the plug-in while it runs. The input is a little
// program: each opcode byte picks an action and the following bytes its arguments.
//   sample rate and block size changes (incl. 0, negative, NaN, Inf, absurd values),
//   mono/stereo layouts, audio blocks of any size with NaN/Inf/denormal/huge samples,
//   raw MIDI bytes at any position, playhead positions (samples, seconds, both, none,
//   non-finite), parameter changes (incl. out-of-range and non-finite normalised values,
//   and NaN stored straight into the parameter atomics), program changes, Graph capture
//   and apply, and save/load mid-stream.
// After every block: output finite and within the hard limit; track and playhead sane.

#include "fuzz_common.h"

namespace
{
struct Head : juce::AudioPlayHead
{
    juce::Optional<juce::int64> samples;
    juce::Optional<double> secs;
    bool playing = false, present = true;

    juce::Optional<PositionInfo> getPosition() const override
    {
        if (! present)
            return {};
        PositionInfo info;
        info.setIsPlaying (playing);
        info.setTimeInSamples (samples);
        info.setTimeInSeconds (secs);
        return info;
    }
};

constexpr int kMaxBlock = 4096;
constexpr int kSampleBudget = 1 << 13; // per input: enough for several analyses, still ~20+ inputs/s under ASan
} // namespace

extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size)
{
    fuzz::initJuce();
    fuzz::Reader in { data, size };

    auto p = std::make_unique<FlayrTuneProcessor>();
    Head head;
    p->setPlayHead (&head);

    const auto params = p->getParameters();
    int budget = kSampleBudget;
    double phase = 0.0;
    int prepares = 0;

    if (in.flag())
    {
        auto layout = p->getBusesLayout();
        const auto set = juce::AudioChannelSet::mono();
        layout.inputBuses.getReference (0) = set;
        layout.outputBuses.getReference (0) = set;
        p->setBusesLayout (layout);
    }

    for (int op = 0; op < 64 && ! in.empty() && budget > 0; ++op)
    {
        switch (in.u8() % 10)
        {
            case 0: // prepareToPlay
            {
                const double rate = in.number ({ 44100.0, 48000.0, 96000.0, 192000.0, 8000.0, 7999.0, 768000.0, 768001.0,
                                                 22050.0, 0.0, -1.0, -48000.0, (double) NAN, (double) INFINITY,
                                                 -(double) INFINITY, 1e-300, 1e9, 6.4e10, 11025.0, 384000.0 });
                const int block = (int) in.u16() - 1000;
                // prepareToPlay at 768 kHz allocates and clears tens of MB; a few per input
                // cover rate changes without making every input slow.
                if (prepares++ < 3)
                    p->prepareToPlay (rate, block);
                break;
            }

            case 1: case 2: case 3: // process a block
            {
                const int n = std::min ({ (int) (in.u16() % (kMaxBlock + 1)), budget });
                budget -= std::max (n, 1);
                // As a host does: one buffer channel per bus channel.
                const int chans = std::max (p->getTotalNumInputChannels(), p->getTotalNumOutputChannels());
                juce::AudioBuffer<float> buf (chans, n);
                const auto fill = in.u8();
                for (int i = 0; i < n; ++i)
                {
                    phase += 0.03 + 0.001 * (fill & 15);
                    float v = 0.3f * (float) std::sin (phase);
                    switch (fill >> 5)
                    {
                        case 1: if (i == n / 2) v = NAN; break;
                        case 2: if (i == n / 3) v = (i & 1) ? INFINITY : -INFINITY; break;
                        case 3: v *= 1e30f; break;
                        case 4: v *= 1e-40f; break; // denormals
                        case 5: v = in.empty() ? v : in.get<float>(); break; // raw fuzz samples
                        case 6: v = 0.0f; break;
                        default: break;
                    }
                    for (int c = 0; c < chans; ++c)
                        buf.setSample (c, i, v);
                }

                juce::MidiBuffer midi;
                for (int e = in.u8() % 6; e > 0; --e)
                {
                    uint8_t bytes[3] = { in.u8(), in.u8(), in.u8() };
                    if ((bytes[0] & 0x80) == 0)
                        bytes[0] |= 0x80;
                    const int len = 1 + in.u8() % 3;
                    midi.addEvent (bytes, len, n > 0 ? (int) (in.u16() % (uint16_t) n) : 0);
                }

                const auto pos = in.u8();
                head.present = (pos & 1) == 0;
                head.playing = (pos & 2) != 0;
                head.samples = {};
                head.secs = {};
                if (pos & 4)
                    head.samples = (pos & 16) ? (juce::int64) in.get<uint64_t>()
                                              : (juce::int64) (in.u32() % (48000u * 600u));
                if (pos & 8)
                    head.secs = in.number ({ 0.0, 1.0, 59.5, 1800.0, -1.0, 1e7, 1e300, -1e300,
                                             (double) NAN, (double) INFINITY, -(double) INFINITY });

                p->processBlock (buf, midi);
                fuzz::checkAudio (buf);
                fuzz::checkTrack (*p);
                break;
            }

            case 4: // a parameter change through the host API
            {
                auto* param = params[(int) (in.u8() % (uint8_t) params.size())];
                const auto sel = in.u8();
                float v = (float) in.u16() / 65535.0f;
                if (sel > 230) v = -0.5f;
                else if (sel > 220) v = 1.5f;
                else if (sel > 215) v = NAN;
                else if (sel > 210) v = INFINITY;
                param->setValueNotifyingHost (v);
                break;
            }

            case 5: // a buggy host or corrupt automation writes straight into a parameter
            {
                auto* param = dynamic_cast<juce::RangedAudioParameter*> (params[(int) (in.u8() % (uint8_t) params.size())]);
                const float bad[] = { NAN, INFINITY, -INFINITY, 1e30f, -1e30f, 1e-40f };
                if (param != nullptr)
                    p->apvts.getRawParameterValue (param->paramID)->store (bad[in.u8() % 6]);
                break;
            }

            case 6:
                p->setCurrentProgram ((int) in.u32());
                break;

            case 7: // Graph: arm capture, toggle apply, clear edits
            {
                const auto g = in.u8();
                p->trackArmed.store ((g & 1) != 0);
                p->apvts.getParameter ("graph")->setValueNotifyingHost ((g & 2) ? 1.0f : 0.0f);
                if (g & 4)
                    p->track.clearEdits();
                if (g & 8) // an edit as the Graph editor makes one (any index, a MIDI note, a known mode)
                    // Mostly near the start: an edit at minute 29 makes every later save
                    // encode a 30-minute track, which would make the input slow to run.
                    p->track.setTarget ((g & 16) ? (int) (in.u32() % (uint32_t) (tune::PitchTrack::kCapacity + 10)) - 5
                                                 : (int) (in.u16() % 4000),
                                        (float) (in.u8() % 128), (uint8_t) (1 + in.u8() % 2));
                break;
            }

            case 8: // save and reload mid-stream
            {
                juce::MemoryBlock state;
                p->getStateInformation (state);
                p->setStateInformation (state.getData(), (int) state.getSize());
                fuzz::checkTrack (*p);
                break;
            }

            default:
                p->releaseResources();
                p->reset();
                break;
        }
    }

    p->setPlayHead (nullptr);
    return 0;
}
