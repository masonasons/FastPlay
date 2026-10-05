#pragma once
#include "../core/dsp.h"
#include "../core/vec3.h"
#include "../model/room.h"
#include "../model/system.h"
#include "spatial/hrtf.h"

namespace speakers {

// Turns one mono sound source at a point in the room into what your two ears
// hear from it. Every speaker owns one of these, and so does every rattling
// object.
//
// The chain, in order:
//
//   directivity   the cone narrows off axis, so you lose the top end unless
//                 the speaker is pointing at you
//   air           distance eats high frequencies, over metres not centimetres
//   delay line    one shared line, read at several points:
//                   - once per ear for the direct sound
//                   - once per first order reflection off each of the six
//                     surfaces, at that image source's longer path
//   per ear       interaural delay and level, plus the head shadow: the far
//                 ear hears a duller, quieter copy a fraction of a millisecond
//                 later, which is what puts the sound outside your head
//   elevation     a moving notch standing in for the pinna, so up and down are
//                 not identical
//
// Reading the direct sound out of a delay line at its true time of flight is
// what makes time alignment mean something: two speakers at different
// distances comb filter exactly as they do in a real car.

class Spatializer {
public:
    void Init(float sampleRate);
    void Reset();

    // Recomputes every target: where the source is relative to your head, how
    // far the reflections travel, how much of the top end survives. Cheap
    // enough to call once per block; everything it sets is smoothed towards,
    // never jumped to.
    //
    // `extraDelayMs` is the speaker's own alignment delay. `beamingHz` is the
    // frequency above which this source starts to narrow (0 to skip it), and
    // `aim` is the direction it faces.
    void Update(const RoomSpec &room, const Listener &listener, Vec3 sourcePos, Vec3 aim,
                float beamingHz, float extraDelayMs, bool reflectionsEnabled);

    // Adds this source's contribution to the output buffers. `mono` is the
    // source signal; `outL`/`outR` are accumulated into, not overwritten.
    void Process(const float *mono, int frames, float *outL, float *outR);

    // Distance from the listener as of the last Update(), in metres.
    float Distance() const { return m_distance; }

    // Hears through a measured head (an HRTF, the one Binaural mode uses) rather
    // than the ear model: the direct sound and each of the six reflections from
    // the direction it arrives from, timing, level and colouring between the ears
    // as a real head has them -- which is what puts a speaker out in the room
    // rather than inside your head. Null for the ear model. With it, Process()
    // takes blocks of fastplay::audio::kHrtfBlock, and the engine's renderer
    // mixes what it adds once each block's voices are in.
    void SetHrtf(const fastplay::audio::HrtfDatabase *db, fastplay::audio::HrtfRenderer *renderer);

private:
    static constexpr int kReflections = 6; // four walls, floor, ceiling

    struct Tap {
        dsp::Smoother delay;  // in samples
        dsp::Smoother gain;   // through the HRTF: one level, the direction does the rest
        dsp::OnePole tone;    // and its own surface colouring
        dsp::Smoother gainL;
        dsp::Smoother gainR;
    };

    void UpdateReflection(int index, Vec3 imagePos, const Listener &listener, float wallGain,
                          float directDistance);

    float m_sampleRate = 48000.0f;
    dsp::DelayLine m_line;

    // Pre-delay colouring, applied to the mono signal once rather than per ear.
    dsp::Biquad m_directivity;   // high shelf cut when you are off axis
    dsp::OnePole m_air;          // air absorption over distance
    // Bass does not obey the inverse square law indoors. Below the frequency
    // where a room stops behaving like open air, the space pressurises instead
    // of radiating, and the level down there is much the same wherever you
    // stand -- while everything above it falls away with distance as usual.
    // Without this, walking away from a system just turns it down evenly,
    // when what should happen is that the punch goes and the bass stays.
    dsp::Biquad m_bassHold;
    bool m_bassHoldActive = false;
    bool m_directivityActive = false;

    // Direct path, per ear.
    dsp::Smoother m_delayL, m_delayR;
    dsp::Smoother m_gainL, m_gainR;
    dsp::OnePole m_shadowL, m_shadowR;   // the far ear goes dull
    // And quieter, but only above the frequency where a head is big enough to
    // be in the way. Below it the wave bends round the head as if it were not
    // there, so a sub in one corner is as loud in both ears.
    dsp::OnePole m_splitL, m_splitR;     // what passes under the head
    dsp::Smoother m_shelfL, m_shelfR;    // how much of the rest arrives
    dsp::Biquad m_pinnaL, m_pinnaR;      // elevation cue

    Tap m_taps[kReflections];
    // Surfaces are not mirrors: they scatter, and they absorb far more at the
    // top end than the broadband figure suggests. Without this the reflections
    // arrive as six bright discrete slaps, which is what a room emphatically
    // does not sound like.
    dsp::OnePole m_reflectionToneL, m_reflectionToneR;
    bool m_reflectionsOn = false;
    // Set by Reset(): the next Update() starts the delays at their targets
    // rather than gliding up from nothing, which would be a pitch bend.
    bool m_settle = false;
    float m_distance = 1.0f;

    // Through the HRTF
    const fastplay::audio::HrtfDatabase *m_hrtf = nullptr;
    fastplay::audio::HrtfRenderer *m_renderer = nullptr;
    fastplay::audio::HrtfVoiceState m_hrtfDirect;
    fastplay::audio::HrtfVoiceState m_hrtfTaps[kReflections];
    fastplay::audio::HrtfDatabase::Lookup m_lookDirect;
    fastplay::audio::HrtfDatabase::Lookup m_lookTaps[kReflections];
    dsp::Smoother m_delayC, m_gainC;  // the direct sound, to the middle of the head
    float m_blockDirect[fastplay::audio::kHrtfBlock] = {};
    float m_blockTaps[kReflections][fastplay::audio::kHrtfBlock] = {};
};

}  // namespace speakers
