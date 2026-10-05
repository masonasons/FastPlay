#pragma once
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../core/dsp.h"
#include "modes.h"
#include "reverb.h"
#include "voice.h"
#include "../model/system.h"
#include "spatial/hrtf.h"

namespace speakers {

// The renderer. Takes the stereo signal you are playing and the speaker system
// you have built, and produces what your two ears hear standing where you are
// standing.
//
//   every speaker   crossover, driver, amplifier, then placed in the room
//        |
//        +--> the reverberation send, scaled by distance
//                  |
//                  +--> the tank
//        |
//   sum -> cabin gain -> standing waves -> what you would feel -> limiter
//
// Cabin gain is there because small sealed spaces pressurise rather than
// radiate, which is the whole reason a car has the bass it does.
//
// Threading: Prepare() rebuilds and may allocate, so it belongs on the control
// thread and is only needed when the system actually changes. SetListener()
// and Render() do not allocate; Render() is safe to call from an audio
// callback.

class Engine {
public:
    void Init(float sampleRate, int maxBlockFrames = 2048);

    // Hear the speakers and their reflections through this measured head (the
    // HRTF Binaural mode uses) rather than the ear model. Before Prepare(); the
    // database must outlive the engine. With it the engine works in blocks of
    // fastplay::audio::kHrtfBlock, so RenderInterleaved() is that many frames late.
    void SetHrtf(const fastplay::audio::HrtfDatabase *db);

    // Rebuilds the voices and room tuning from the system. Call it
    // when SpeakerSystem::Revision() has moved.
    void Prepare(const SpeakerSystem &system);

    // Everything that can change without rebuilding a filter: per speaker
    // levels, mute and solo, polarity, alignment delay, where speakers are,
    // and the system wide settings. Does not allocate, and every change it
    // makes is ramped, so it is safe to call on every keypress -- which is
    // what stops a volume key from clicking.
    //
    // Adding or removing a speaker, changing the room, the crossover, or a
    // speaker's own specification all need Prepare() instead.
    void UpdateLevels(const SpeakerSystem &system);

    // Cheap updates that need no rebuild.
    void SetListener(const Listener &listener) { m_listener = listener; }

    // Whether your head is outside the space rather than in it. Standing in a
    // car park while a car goes past is not the same as sitting in it: you are
    // not inside the pressure vessel, so there is no cabin gain and no
    // standing wave to sit in, and everything the speakers do reaches you
    // through steel, glass and upholstery, which is why a car going past is
    // mostly bass.
    bool Outside() const;

    // Renders `frames` frames. Outputs are overwritten, not accumulated.
    void Render(const float *inL, const float *inR, int frames, float *outL, float *outR);
    // The same, for interleaved stereo in and out.
    void RenderInterleaved(const float *in, float *out, int frames);

    void Reset();

    float SampleRate() const { return m_sampleRate; }
    int VoiceCount() const { return (int)m_voices.size(); }

    // ---- diagnostics, for the interface to speak -------------------------
    // Peak output of the last render, in dBFS. Above 0 the limiter is working.
    float OutputPeakDb() const;
    // How hard speaker `index` was driven past clean, 0 being clean.
    float VoiceOverdrive(int index) const;
    // How far speaker `index` moved its cone, as a fraction of its linear
    // travel. Past one it is out beyond Xmax, which is audible long before it
    // is damaging, and is the thing that makes a big note sound big.
    float VoiceExcursion(int index) const;
    // And how much output it has lost to the voice coil being hot, in dB.
    float VoiceCompressionDb(int index) const;
    const std::string &VoiceLabel(int index) const;

private:
    void ApplyCabinGain(float *buffer, dsp::Biquad &filter, int frames);

    float m_sampleRate = 48000.0f;
    int m_maxBlock = 2048;

    RoomSpec m_room;
    SystemSettings m_settings;
    Listener m_listener;

    std::vector<Voice> m_voices;
    std::vector<std::string> m_voiceLabels;
    Reverb m_reverb;
    RoomModes m_roomModes;
    // Where the bass is coming from, for working out how hard each standing
    // wave is driven. Below a couple of hundred hertz that is the subs, and
    // there is usually one of them.
    Vec3 m_modeSource;

    // Scratch, sized once in Init().
    std::vector<float> m_reverbSend;
    std::vector<float> m_voiceMono;   // what one speaker put into the room
    std::vector<float> m_inL, m_inR, m_outL, m_outR; // for RenderInterleaved
    // The bass control, and the signal through it; moved a little each block
    // towards what it is set to, so turning it does not click
    dsp::Biquad m_bassL, m_bassR;
    std::vector<float> m_toneL, m_toneR;
    float m_bassDb = 0.0f;

    // Cabin gain: a low shelf on what you hear.
    dsp::Biquad m_cabinL, m_cabinR;
    bool m_cabinActive = false;

    // The measured response of the space, on the ear path only.
    struct ShapeStage {
        dsp::Biquad l, r;
    };
    std::vector<ShapeStage> m_shape;

    // Heard from outside: what the shell does to everything on its way out.
    dsp::Cascade m_shellL, m_shellR;
    float m_shellGain = 1.0f;

    // A limiter that holds a ceiling by gain rather than by shaping.
    //
    // It looks ahead: the output is a millisecond and a half late, and the
    // gain for each sample is already down by the time a peak arrives, ramped
    // over that time rather than snapped. Reacting to a peak as it came meant
    // the first half cycle of every bass note got past before the gain had
    // fallen. The caller says how much gain each sample can stand; the gain
    // applied is the average of the least needed around each of the next few
    // samples, so it slides down over the whole look-ahead and comes back up
    // slowly.
    struct Limiter {
        void Init(int aheadSamples, float releasePerSample);
        void Reset();
        // Stores `l`/`r`, records that the sample can stand a gain of `need`
        // (1 being none), and returns the sample from the look-ahead ago.
        inline void Process(float l, float r, float need, float &outL, float &outR) {
            int p = pos;
            float delayedL = bufL[(size_t)p], delayedR = bufR[(size_t)p];
            float target = (float)(sum / ahead);
            needs[(size_t)p] = need;
            float least = 1.0f;
            for (float n : needs) least = std::min(least, n);
            sum += least - hold[(size_t)p];
            hold[(size_t)p] = least;
            bufL[(size_t)p] = l;
            bufR[(size_t)p] = r;
            pos = p + 1 == ahead ? 0 : p + 1;
            gain = target < gain ? target : gain + (target - gain) * release;
            outL = delayedL * gain;
            outR = delayedR * gain;
        }
        float gain = 1.0f;
        float release = 0.0f;
        int ahead = 1;
        int pos = 0;
        std::vector<float> bufL, bufR;   // the input, delayed
        std::vector<float> needs;        // the gain each of those samples needs
        std::vector<float> hold;         // the least needed near each one
        double sum = 0.0;                // of hold
    };

    // What an ear would make of a system this loud, put back.
    //
    // Three shelves reproduce the gap between how an ear hears bass when
    // something is genuinely loud and how it hears the same recording at
    // headphone level, fitted to the measured equal-loudness contours. How
    // much of it applies depends on whether the system can produce bass at
    // all and whether there is any in the signal, so a set of doors on their
    // own is left alone and subs make the notes swell.
    static constexpr int kFeelStages = 3;
    dsp::Biquad m_feelL[kFeelStages], m_feelR[kFeelStages];
    dsp::Biquad m_bassSense;      // what counts as bass, for deciding there is some
    dsp::OnePole m_bassLevel;     // how much of it there is, slowly
    float m_feelTiltDb = 0.0f;    // the whole correction, at the bottom
    float m_bassAuthority = 0.0f; // how much of it this system has earned
    float m_feelBlend = -1.0f;    // what the shelves are currently set to
    float m_feelDb = 0.0f;        // how much correction is currently asked for
    float m_feelDuckExtra = 0.0f; // extra allowance earned by being driven hard

    // The output stage: the bass is held to what fits beside the rest, then
    // the whole is held to the ceiling. See the notes in engine.cpp.
    float m_makeup = 1.0f;
    dsp::Cascade m_splitLowL, m_splitLowR, m_splitHighL, m_splitHighR;
    std::vector<float> m_highL, m_highR;       // the top band, delayed to match
    int m_highPos = 0;
    Limiter m_bassLimit;                       // the bass, to what is left beside the rest
    Limiter m_limit;                           // everything, to the ceiling

    float m_peak = 0.0f;
    std::string m_emptyLabel;

    // Through the HRTF: the renderer the voices mix into each block, and the
    // block being gathered (in) and the one being given out (a block behind)
    const fastplay::audio::HrtfDatabase *m_hrtf = nullptr;
    fastplay::audio::HrtfRenderer m_hrtfRenderer;
    std::vector<float> m_blockInL, m_blockInR, m_blockOutL, m_blockOutR;
    int m_blockFill = 0;
};

}  // namespace speakers
