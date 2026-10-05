#pragma once
#include "../core/dsp.h"
#include "spatial.h"
#include "../model/system.h"

namespace speakers {

// One placed speaker, from the signal fed to it to the sound arriving at your
// ears. The order is the order the real thing is in:
//
//   feed        which channel this speaker is wired to
//   crossover   the system split: subs take the bottom, mains take the rest.
//               With no sub in the system there is nothing to split, and the
//               mains run full range -- which is exactly why a car with no sub
//               sounds the way it does
//   amplifier   gain, then a limiter that is clean until it is not. Drive it
//               hard and you hear the amplifier, not the speaker
//   driver      what this cone can actually reproduce: a high pass at its
//               corner with the enclosure's slope, the resonant lift a leaky
//               door or a vented box adds just above it, and a low pass where
//               the cone gives up. It comes after the amplifier because it is
//               mechanical, which is why a clipped subwoofer sounds flabby
//               instead of bright
//   output      scaled by what this speaker can actually produce, so a fifteen
//               inch horn is genuinely louder than a dash speaker for the same
//               input
//
// After that the Spatializer places it in the room.

class Voice {
public:
    void Init(float sampleRate);
    // Hears through a measured head (see Spatializer::SetHrtf); null for the ear model.
    void SetHrtf(const fastplay::audio::HrtfDatabase *db, fastplay::audio::HrtfRenderer *renderer) {
        m_spatial.SetHrtf(db, renderer);
        m_spatialTweeter.SetHrtf(db, renderer);
    }

    // Rebuilds every filter for this speaker. Control thread only.
    void Configure(const PlacedSpeaker &speaker, const SystemSettings &settings,
                   bool systemHasSub);

    // Everything that can change without rebuilding a filter: levels,
    // polarity, alignment delay and where the speaker is. Turning a speaker
    // down should not reset its filters, so this is the path the interface
    // uses for anything but a structural change.
    void SetLevels(const PlacedSpeaker &speaker, const SystemSettings &settings);

    // Recomputes where this speaker is relative to your head. Per block.
    void Update(const RoomSpec &room, const Listener &listener, bool reflectionsEnabled);

    // Renders this speaker. `outL`/`outR` are accumulated into; `monoOut` is
    // overwritten with what this speaker put into the room, before any of it
    // was placed. The engine routes that onward, into the reverberation.
    void Process(const float *inL, const float *inR, int frames, float *outL, float *outR,
                 float *monoOut);

    // Mute and solo, without rebuilding anything: the change is ramped, so
    // muting a speaker mid-track does not click.
    void SetActive(bool active) { m_active.target = active ? 1.0f : 0.0f; }

    void Reset();

    float Distance() const { return m_spatial.Distance(); }
    // How far the cone went over the last block, as a fraction of its linear
    // travel. Past one it is out past Xmax and audibly so.
    float Excursion() const { return m_excursion; }
    // How much the voice coil having got hot has taken off the output, in dB.
    float CompressionDb() const { return m_compressionDb; }
    // Peak amount by which the amplifier was driven past clean over the last
    // block: 0 is clean, 1 and above is audibly clipping.
    float Overdrive() const { return m_overdrive; }

private:
    float m_sampleRate = 48000.0f;

    ChannelFeed m_feed = ChannelFeed::Mono;
    bool m_crossoverActive = false;
    dsp::Cascade m_crossover;      // low pass for a sub, high pass for a main
    dsp::Cascade m_driverHp;       // the bottom of what the cone can do
    dsp::Biquad m_driverResonance; // the lift a vented or unboxed driver adds
    dsp::Cascade m_driverLp;       // the top of what the cone can do

    // Drive carries the polarity in its sign, so flipping polarity fades
    // through zero instead of stepping, and is smoothed so that riding a
    // volume key does not zipper.
    dsp::Smoother m_driveGain;   // into the amplifier, where clipping happens
    float m_outputGain = 1.0f;   // out of it, from sensitivity and power
    // The master level, applied here rather than to the finished mix. It has
    // to act on what the speakers put into the room, not on what comes out of
    // it, or the bass bus never sees it -- and then turning the volume down
    // would leave everything rattling exactly as hard as before.
    dsp::Smoother m_postGain;
    float m_overdrive = 0.0f;
    dsp::Smoother m_active;

    Vec3 m_position;
    Vec3 m_aim{0.0f, 1.0f, 0.0f};
    float m_beamingHz = 0.0f;
    float m_delayMs = 0.0f;

    // ---- what the cone can physically do ----------------------------------
    // Displacement per volt is flat below the box corner and falls twelve dB
    // an octave above it, which is only another way of saying the cone has to
    // move four times as far for the same loudness an octave down. Everything
    // that makes a hard driven speaker sound hard driven follows from that,
    // and none of it is expressible as a frequency response.
    dsp::Biquad m_excursionLp;
    dsp::Biquad m_excursionHp; // a port unloads the cone at tuning, and only there
    bool m_ported = false;
    bool m_limitsEnabled = true;
    float m_excursionScale = 0.0f; // displacement signal -> fractions of Xmax
    float m_blDroop = 0.0f;        // how fast the motor gives up as it leaves the gap
    float m_stiffening = 0.0f;     // how fast the suspension runs out of travel
    float m_asymmetry = 0.0f;      // a motor is not symmetric about its rest position
    float m_excursion = 0.0f;

    // The voice coil getting hot, which takes seconds and comes back slower.
    dsp::Envelope m_thermal;
    float m_compressionDb = 0.0f;

    // Air going through a port fast enough that you hear it going.
    dsp::Biquad m_portBand;
    dsp::Biquad m_chuffColour;
    dsp::Noise m_chuffNoise{0x5bd1e995u};

    // The cone itself, heard rather than the air in front of it.
    dsp::Biquad m_air;   // a tweeter's last octave, which a cone model loses
    bool m_airActive = false;
    dsp::Biquad m_coneHp, m_coneLp;
    dsp::Noise m_coneNoise{0x2545f491u};
    float m_coneLevel = 0.0f;

    // What a subwoofer amplifier does to the peaks, and the phase knob on the
    // front of it.
    dsp::Envelope m_subComp;
    float m_subCompAmount = 0.0f;
    dsp::Biquad m_phaseAllpass;
    bool m_phaseActive = false;

    Spatializer m_spatial;

    // A component set radiates its top end from somewhere else entirely, so it
    // gets a second place in the room and a band split to feed it. You locate
    // a speaker by its treble, so a door woofer whose tweeter is up on the
    // pillar belongs at the pillar as far as your ears are concerned.
    bool m_separateTweeter = false;
    Vec3 m_tweeterPosition;
    dsp::Cascade m_bodyLow, m_tweeterHigh;
    Spatializer m_spatialTweeter;
};

}  // namespace speakers
