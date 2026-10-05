#include "voice.h"

#include <algorithm>
#include <cmath>

namespace speakers {

namespace {

// Output is scaled so that a speaker capable of this peak SPL sits at unity.
// It sets where the master volume lands, and it is what makes a 15 inch PA top
// come out of the mix an order of magnitude ahead of a dash speaker. Chosen so
// that a normal system with a sub, at the default master, peaks a few dB below
// full scale once cabin gain has been added -- leaving a big build room to be
// genuinely loud and a factory system room to be genuinely quiet.
constexpr float kReferenceSplDb = 124.0f;

// Where a component set hands over from the door to the pillar. Above this the
// sound comes from the tweeter, and so does most of your sense of where the
// speaker is.
constexpr float kTweeterCrossoverHz = 3200.0f;

// Air, for turning cone movement into sound pressure.
constexpr float kAirDensity = 1.2f; // kg/m3 at 20 C

// How much output a voice coil at its rated power gives up to heat. Three and
// a half dB is the usual figure for a car driver run hard, and it arrives over
// seconds rather than milliseconds -- which is why a system that was loud at
// the start of a track is quieter by the end of one.
constexpr float kThermalSag = 1.0f;

// Where a port stops moving air and starts whistling with it. In fractions of
// the cone's linear travel, measured in the band the port is working in.
constexpr float kChuffThreshold = 0.55f;
constexpr float kChuffLevel = 0.30f;

// The cone heard as an object rather than as a source of air.
//
// Measured at the open boot of a car with a twelve in it: with the sub
// working, everything from a few hundred hertz to eleven kilohertz comes up
// six to nine decibels, broadband and nearly level, and from the driver's
// seat with the boot shut it is gone. It is the surround and the spider and
// the coil, and it is loudest where the cone is moving fastest.
//
// It follows the note rather than the envelope of the note: the modulation
// sits at the note's own frequency, -2.6 dB, and at twice it, -4.2 dB, with
// almost nothing anywhere else. A half wave rectifier gives the first and a
// full wave one the second, and these are the weights that land on both.
constexpr float kConeHalfWave = 1.00f;
constexpr float kConeFullWave = 0.48f;
constexpr float kConeLowHz = 200.0f;  // below this it would just be distortion
constexpr float kConeHighHz = 6500.0f;
constexpr float kConeLevel = 0.19f;
constexpr float kConeThreshold = 0.10f; // a cone barely moving makes no noise

// A subwoofer amplifier's own compression, and its phase knob.
constexpr float kSubCompAttackMs = 5.0f;
constexpr float kSubCompReleaseMs = 140.0f;
// Where it starts working, and how hard it leans at full. Four to one is a
// bass amplifier holding on rather than a limiter slamming shut.
constexpr float kSubCompThresholdDb = -8.0f;
constexpr float kSubCompMaxRatio = 4.0f;

// A tweeter's last octave.
//
// The driver model rolls a dome off like a cone, on the same second order
// slope, and a real one does not: a tweeter is still level where this had it
// several decibels down. Against the recordings the renderer was three to four
// decibels short from four kilohertz up, and this is where that was coming
// from. Only for drivers that have a tweeter -- a midbass has nothing up there
// to lift.
// Where a one inch dome starts to narrow: c / (pi * 25 mm).
constexpr float kDomeBeamingHz = 4400.0f;

constexpr float kAirHz = 5500.0f;
constexpr float kAirDb = 3.5f;
// And more of it the smaller the driver. A three and a half inch coaxial is
// nearly all tweeter -- the cone hands over low and contributes almost nothing
// above it -- so its top end is a larger share of what it makes than a six and
// a half's is. Taking one figure for both left dash speakers sounding like
// small door speakers instead of like tweeters on a stalk.
constexpr float kAirSmallDb = 4.0f;
constexpr float kAirReferenceInches = 6.5f;
constexpr float kAirSmallSpan = 3.0f;

// An amplifier that is exactly linear until it runs out of rails, and then
// rounds over rather than squaring off. Clean below 1.0, hard ceiling at 2.0.
inline float AmpLimit(float x) {
    float a = std::fabs(x);
    if (a <= 1.0f) return x;
    float sign = x < 0.0f ? -1.0f : 1.0f;
    return sign * (1.0f + std::tanh(a - 1.0f));
}

} // namespace

void Voice::Init(float sampleRate) {
    m_sampleRate = sampleRate;
    m_spatial.Init(sampleRate);
    m_spatialTweeter.Init(sampleRate);
    m_active.SetTime(sampleRate, 12.0f);
    m_active.Snap(1.0f);
    m_driveGain.SetTime(sampleRate, 15.0f);
    m_driveGain.Snap(1.0f);
    m_postGain.SetTime(sampleRate, 15.0f);
    m_postGain.Snap(1.0f);
    Reset();
}

void Voice::Reset() {
    m_excursionLp.Reset();
    m_excursionHp.Reset();
    m_portBand.Reset();
    m_chuffColour.Reset();
    m_thermal.Reset();
    m_air.Reset();
    m_subComp.Reset();
    m_coneHp.Reset();
    m_coneLp.Reset();
    m_phaseAllpass.Reset();
    m_excursion = 0.0f;
    m_compressionDb = 0.0f;
    m_crossover.Reset();
    m_driverHp.Reset();
    m_driverResonance.Reset();
    m_driverLp.Reset();
    m_bodyLow.Reset();
    m_tweeterHigh.Reset();
    m_spatial.Reset();
    m_spatialTweeter.Reset();
    m_overdrive = 0.0f;
}

void Voice::Configure(const PlacedSpeaker &speaker, const SystemSettings &settings,
                      bool systemHasSub) {
    const SpeakerSpec &spec = speaker.spec;
    m_feed = speaker.feed;
    m_postGain.target = dsp::DbToGain(settings.masterGainDb + settings.levelTrimDb);

    m_position = speaker.position;
    m_aim = DirectionFromAngles(speaker.aimYawDeg, speaker.aimPitchDeg);
    // What actually radiates the top end decides how wide the top end is.
    //
    // A coaxial's treble comes off a one inch dome, not off the cone it is
    // bolted through, and a dome that size stays wide to four or five
    // kilohertz. Taking the cone's figure for the whole driver made small
    // coaxials the darkest thing in the catalogue off axis -- a dash pair came
    // out eleven decibels duller at 7 kHz than a six and a half in the door,
    // which is backwards. A component set already has its tweeter rendered
    // separately and wide, so this is only for the ones that do not.
    m_beamingHz = spec.beamingHz;
    bool ownTweeter = spec.kind == SpeakerKind::Coaxial || spec.kind == SpeakerKind::Center;
    if (ownTweeter && !speaker.separateTweeter)
        m_beamingHz = std::max(m_beamingHz, kDomeBeamingHz);
    m_delayMs = speaker.delayMs;

    // ---- system crossover ------------------------------------------------
    int order = settings.crossoverOrder <= 2 ? 2 : (settings.crossoverOrder >= 8 ? 8 : 4);
    float xoverHz = dsp::Clampf(settings.crossoverHz, 20.0f, 400.0f);
    m_crossover.Clear();
    if (speaker.IsSub()) {
        // A subwoofer is low passed whether or not anything else is present.
        m_crossover.SetLinkwitzRileyLowpass(m_sampleRate, xoverHz, order);
        m_crossoverActive = true;
    } else if (systemHasSub) {
        m_crossover.SetLinkwitzRileyHighpass(m_sampleRate, xoverHz, order);
        m_crossoverActive = true;
    } else {
        // Nothing to hand the bottom end to, so this speaker keeps it, and
        // whatever its own corner does not survive is simply lost.
        m_crossoverActive = false;
    }

    // ---- the driver itself -----------------------------------------------
    float lowHz = dsp::Clampf(spec.lowCornerHz, 10.0f, 2000.0f);
    int lowOrder = spec.lowOrder >= 4 ? 4 : 2;
    m_driverHp.SetButterworthHighpass(m_sampleRate, lowHz, lowOrder);

    // A box with a Q above 0.707 is not flat: it humps just above its corner.
    // That hump is the boom of a door speaker with no enclosure behind it.
    float excessQ = std::max(0.0f, spec.lowQ - 0.707f);
    float resonanceDb = dsp::Clampf(excessQ * 14.0f, 0.0f, 6.0f);
    m_driverResonance.SetPeaking(m_sampleRate, lowHz * 1.35f, 1.1f, resonanceDb);

    float topHz = dsp::Clampf(spec.topHz, 200.0f, m_sampleRate * 0.45f);
    m_driverLp.SetButterworthLowpass(m_sampleRate, topHz, spec.topOrder >= 4 ? 4 : 2);
    m_airActive = spec.kind == SpeakerKind::Coaxial ||
                  spec.kind == SpeakerKind::Component ||
                  spec.kind == SpeakerKind::Center;
    if (m_airActive) {
        float small = dsp::Clampf((kAirReferenceInches - spec.coneInches) / kAirSmallSpan, 0.0f,
                                  1.0f);
        m_air.SetHighShelf(m_sampleRate, std::min(kAirHz, m_sampleRate * 0.25f), 0.7f,
                           kAirDb + kAirSmallDb * small);
    }

    // ---- where the top end comes from ------------------------------------
    m_separateTweeter = speaker.separateTweeter;
    m_tweeterPosition = speaker.tweeterPosition;
    m_bodyLow.Clear();
    m_tweeterHigh.Clear();
    if (m_separateTweeter) {
        m_bodyLow.SetLinkwitzRileyLowpass(m_sampleRate, kTweeterCrossoverHz, 4);
        m_tweeterHigh.SetLinkwitzRileyHighpass(m_sampleRate, kTweeterCrossoverHz, 4);
    }

    // ---- what the cone can physically do ---------------------------------
    m_limitsEnabled = settings.driverLimitsEnabled;
    m_ported = (spec.enclosure == Enclosure::Ported);
    m_excursionLp.SetLowpass(m_sampleRate, lowHz, spec.lowQ > 0.0f ? spec.lowQ : 0.707f);
    if (m_ported) {
        float fb = dsp::Clampf(spec.portTuningHz > 0.0f ? spec.portTuningHz : lowHz, 10.0f,
                               200.0f);
        // Right at tuning the box holds the cone still and the port does the
        // work, so the cone's travel has a notch there and climbs steeply
        // below it, where the port has stopped loading it at all.
        m_excursionHp.SetHighpass(m_sampleRate, fb, 0.9f);
        m_portBand.SetBandpass(m_sampleRate, fb, 1.1f);
        m_chuffColour.SetBandpass(m_sampleRate, 700.0f, 0.8f);
    }

    // How far this cone would have to move at its corner to make the loudest
    // sound it claims, against how far it can actually move. A piston in half
    // space gives p = rho * Sd * w^2 * x / (2 pi r), so the displacement that
    // the rated SPL demands is that rearranged. Above one, the cone runs out
    // before the amplifier does -- which is the usual case for a subwoofer at
    // the bottom of its range and almost never the case in the midrange.
    {
        float pMax = 20.0e-6f * std::pow(10.0f, spec.MaxSplDb() / 20.0f);
        float sd = std::max(spec.sdCm2, 1.0f) * 1.0e-4f;
        float xmax = std::max(spec.xmaxMm, 0.2f) * 1.0e-3f;
        float w = 2.0f * dsp::kPi * lowHz;
        float needed = pMax * 2.0f * dsp::kPi / (kAirDensity * sd * w * w);
        m_excursionScale = dsp::Clampf(needed / xmax, 0.05f, 8.0f);
    }
    // The cone's own noise. Every driver makes some; a subwoofer at full
    // travel makes enough to hear from across a car park.
    m_coneHp.SetHighpass(m_sampleRate, kConeLowHz, 0.707f);
    m_coneLp.SetLowpass(m_sampleRate, std::min(kConeHighHz, m_sampleRate * 0.45f), 0.707f);
    m_coneLevel = kConeLevel * dsp::Clampf(settings.coneNoise, 0.0f, 10.0f);

    // The subwoofer bus gets an amplifier with its own idea of how loud is
    // loud, and a phase knob. Neither belongs on a door speaker.
    m_subCompAmount = spec.kind == SpeakerKind::Subwoofer
                          ? dsp::Clampf(settings.subCompression, 0.0f, 1.0f)
                          : 0.0f;
    m_subComp.SetTimes(m_sampleRate, kSubCompAttackMs, kSubCompReleaseMs);

    // A phase control is an allpass, not a delay: it turns the signal by a
    // fixed angle at the frequency it is set for rather than moving it in
    // time. Aimed at the crossover, which is where a sub and the doors have
    // to agree. At ninety degrees the corner sits on the crossover itself.
    float phase = dsp::Clampf(speaker.phaseDeg, 0.0f, 180.0f);
    m_phaseActive = spec.kind == SpeakerKind::Subwoofer && phase > 0.5f;
    if (m_phaseActive) {
        float aim = settings.crossoverHz > 20.0f ? settings.crossoverHz : 80.0f;
        float half = phase * 0.5f * dsp::kPi / 180.0f;
        float corner = dsp::Clampf(aim / std::max(std::tan(half), 1e-3f), 5.0f,
                                   m_sampleRate * 0.45f);
        m_phaseAllpass.SetAllpass(m_sampleRate, corner, 0.707f);
    }

    m_blDroop = spec.kind == SpeakerKind::Subwoofer ? 0.40f : 0.30f;
    m_stiffening = 0.30f;
    m_asymmetry = 0.12f;
    m_thermal.SetTimes(m_sampleRate, 3000.0f, 12000.0f);

    // ---- amplifier and output level --------------------------------------
    m_outputGain = dsp::DbToGain(spec.MaxSplDb() - kReferenceSplDb);
    SetLevels(speaker, settings);
    // A rebuild is not a fade.
    m_driveGain.Snap(m_driveGain.target);
    m_postGain.Snap(m_postGain.target);
}

void Voice::SetLevels(const PlacedSpeaker &speaker, const SystemSettings &settings) {
    float gain = dsp::DbToGain(settings.driveDb + speaker.gainDb);
    m_driveGain.target = speaker.invertPolarity ? -gain : gain;

    // Subwoofer level belongs after the amplifier, not before it. Folding it
    // into the drive meant asking for more bass also asked for more clipping,
    // so the bottom end got dirtier exactly as you tried to make it bigger. A
    // real system gives the subs their own clean amplifier.
    float busGain = speaker.IsSub() ? settings.subGainDb + settings.subTrimDb : 0.0f;
    m_postGain.target = dsp::DbToGain(settings.masterGainDb + settings.levelTrimDb + busGain);
    // Only a level, so it can change without rebuilding anything.
    m_coneLevel = kConeLevel * dsp::Clampf(settings.coneNoise, 0.0f, 10.0f);

    m_position = speaker.position;
    m_aim = DirectionFromAngles(speaker.aimYawDeg, speaker.aimPitchDeg);
    m_delayMs = speaker.delayMs;
}

void Voice::Update(const RoomSpec &room, const Listener &listener, bool reflectionsEnabled) {
    m_spatial.Update(room, listener, m_position, m_aim, m_beamingHz, m_delayMs,
                     reflectionsEnabled);
    if (m_separateTweeter) {
        // A tweeter is small, so it stays wide off axis; and it points into
        // the cabin rather than wherever the woofer happens to face.
        m_spatialTweeter.Update(room, listener, m_tweeterPosition, {0.0f, 1.0f, 0.0f}, 0.0f,
                                m_delayMs, reflectionsEnabled);
    }
}

void Voice::Process(const float *inL, const float *inR, int frames, float *outL, float *outR,
                    float *monoOut) {
    constexpr int kMaxBlock = 1024;
    float body[kMaxBlock];
    float treble[kMaxBlock];

    int done = 0;
    float overdrive = 0.0f;
    float excursionPeak = 0.0f;
    float sagLowest = 1.0f;
    while (done < frames) {
        int n = std::min(frames - done, kMaxBlock);
        for (int i = 0; i < n; ++i) {
            int k = done + i;
            float x;
            switch (m_feed) {
            case ChannelFeed::Left: x = inL[k]; break;
            case ChannelFeed::Right: x = inR[k]; break;
            default: x = (inL[k] + inR[k]) * 0.5f; break;
            }

            // The crossover is electronic and comes before the amplifier;
            // the driver is mechanical and comes after it. That order is what
            // makes a clipping subwoofer sound flabby rather than bright: the
            // harmonics the amplifier generates are then thrown away by a cone
            // that cannot reproduce them.
            if (m_crossoverActive) x = m_crossover.Process(x);
            if (m_phaseActive) x = m_phaseAllpass.Process(x);

            // The amplifier leaning on the peaks before it clips them. Gentle,
            // slow to let go, and only on the bass bus.
            if (m_subCompAmount > 0.0f) {
                float env = m_subComp.Process(x * m_driveGain.target);
                float level = dsp::GainToDb(std::max(env, 1e-6f));
                if (level > kSubCompThresholdDb) {
                    float ratio = 1.0f + (kSubCompMaxRatio - 1.0f) * m_subCompAmount;
                    float over = level - kSubCompThresholdDb;
                    x *= dsp::DbToGain(-over * (1.0f - 1.0f / ratio));
                }
            }

            float driven = x * m_driveGain.Next();
            float coneDrive = 0.0f;
            float over = std::fabs(driven) - 1.0f;
            if (over > overdrive) overdrive = over;
            x = AmpLimit(driven);

            if (m_limitsEnabled) {
                // Where the cone is, worked out from the voltage driving it.
                // Feed forward rather than solved: the stiffening genuinely
                // reduces the travel that caused it, but taking that back
                // round the loop buys very little and costs stability.
                float e = m_excursionLp.Process(x);
                if (m_ported) e = m_excursionHp.Process(e);
                float u = e * m_excursionScale;
                float au = std::fabs(u);
                if (au > excursionPeak) excursionPeak = au;

                // The coil leaving the gap and the suspension running out of
                // room. Both take force away the further out the cone is, and
                // being even in x they fold energy up into odd harmonics --
                // which is what a cone at its limit actually sounds like. The
                // asymmetry is the second harmonic: a motor is not the same
                // in both directions about its rest position.
                float ua = u + m_asymmetry * u * au;
                float bl = 1.0f / (1.0f + m_blDroop * ua * ua);
                float km = 1.0f / (1.0f + m_stiffening * au * au * au * au);
                x *= bl * km;

                // And the coil getting hot. Seconds, not milliseconds.
                float sag = 1.0f / (1.0f + kThermalSag * m_thermal.Process(driven * driven));
                if (sag < sagLowest) sagLowest = sag;
                x *= sag;

                // How hard the cone is working, kept for the noise it makes.
                // That is added after the driver, not here: it is the cone
                // itself being heard, and a subwoofer's own response would
                // throw all of it away above eight hundred hertz.
                coneDrive = kConeHalfWave * std::max(0.0f, u) + kConeFullWave * au;

                // Air through the port, fast enough to hear it going.
                if (m_ported) {
                    float over = std::fabs(m_portBand.Process(u)) - kChuffThreshold;
                    if (over > 0.0f)
                        x += m_chuffColour.Process(m_chuffNoise.Next()) * over * kChuffLevel;
                }
            }

            x = m_driverHp.Process(x);
            x = m_driverResonance.Process(x);
            x = m_driverLp.Process(x);
            if (m_airActive) x = m_air.Process(x);

            // The cone heard as a thing rather than as a source of air:
            // loudest where it is moving fastest, following the note itself
            // rather than the shape of the phrase. It radiates off the cone
            // surface, so it is not subject to the driver's own passband.
            if (m_coneLevel > 0.0f) {
                float drive = coneDrive - kConeThreshold;
                if (drive > 0.0f)
                    x += m_coneLp.Process(m_coneHp.Process(m_coneNoise.Next())) * drive *
                         m_coneLevel;
            }

            x *= m_outputGain * m_postGain.Next() * m_active.Next();

            if (m_separateTweeter) {
                body[i] = m_bodyLow.Process(x);
                treble[i] = m_tweeterHigh.Process(x);
            } else {
                body[i] = x;
            }
            monoOut[k] = x;
        }
        m_spatial.Process(body, n, outL + done, outR + done);
        if (m_separateTweeter) m_spatialTweeter.Process(treble, n, outL + done, outR + done);
        done += n;
    }
    m_overdrive = std::max(0.0f, overdrive);
    m_excursion = excursionPeak;
    m_compressionDb = dsp::GainToDb(sagLowest);
}

}  // namespace speakers
