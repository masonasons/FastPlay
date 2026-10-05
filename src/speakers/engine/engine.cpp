#include "engine.h"

#include "../core/loudness.h"

#include <algorithm>
#include <cmath>

namespace speakers {

namespace {

// How often the geometry is recomputed. Short enough that walking and turning
// track smoothly, long enough that the trigonometry costs nothing.
constexpr int kGeometryBlock = 128;
// Which is also the HRTF's block, so that each geometry block is one HRTF block.
static_assert(kGeometryBlock == fastplay::audio::kHrtfBlock, "the HRTF needs whole blocks");
// Below this a measured cabin's response is its pressure gain, which the
// character knob leaves alone; above it, the colour the knob is for.
constexpr float kCabinPressureHz = 150.0f;
// The bass control: a shelf below about where a kick drum's weight is, moved
// at most this far each block (128 frames, so 15 dB takes about a fifth of a
// second) so that turning it is smooth.
constexpr float kBassHz = 100.0f;
constexpr float kBassStepDb = 0.25f;

// What an ear does with a system this loud, put back.
//
// The normal equal-loudness-level contours are not parallel: the bass end of
// a quiet one sits far above the bass end of a loud one. A car at full tilt
// and the same recording at headphone level are therefore not the same
// balance, and the difference is worth about sixteen decibels at the bottom.
//
// Three low shelves reproduce that difference to within a tenth of a decibel
// across the whole band, fitted to ISO 226:2003 rather than drawn by eye. The
// shape of the difference turns out not to depend on how big the difference
// is -- within one percent from a fifteen phon gap to a forty phon one -- so
// one set of shelves scaled by a single number covers every case.
struct FeelShelf {
    float hz;
    float q;
    float share; // of the whole tilt
};
const FeelShelf kFeel[3] = {{64.3f, 0.383f, 0.8267f},
                            {179.0f, 0.581f, 0.1592f},
                            {467.1f, 0.797f, 0.1555f}};

// Full scale out of the engine stands for this much sound pressure from the
// system being simulated, and for this much from the headphones it is
// actually coming out of. The gap between them is what the ear is missing.
constexpr float kSystemFullScaleSpl = 124.0f;
constexpr float kHeadphoneFullScaleSpl = 94.0f;

// Bass has to actually be present before there is anything to put back. This
// is where "present" is measured, and how much of the band counts as full.
constexpr float kBassSenseHz = 90.0f;
constexpr float kBassSenseFull = 0.16f;

// How high the correction may push the output when there was room going
// spare.
constexpr float kFeelCeiling = 0.89f;
// And the furthest it may pull the rest of the music down to make room. Past
// this it would be ducking the track every time a note landed, which is worse
// than not having the bass.
constexpr float kFeelMaxDuckDb = 5.0f;
// And how much further it may go when the system is being driven hard. A car
// played that loud really does swamp the midrange -- bass masks upwards, and
// the louder the bass the further up it reaches -- so past a certain point
// letting it take more is the honest answer rather than a trick.
constexpr float kFeelDuckPerDb = 0.35f;
constexpr float kFeelDuckExtraMax = 5.0f;
constexpr float kFeelDriveReference = -4.0f;
// How fast the amount of correction follows the music, per block: quick to
// back off when a passage gets loud, unhurried coming back.
constexpr float kFeelRisePerBlock = 0.10f;
constexpr float kFeelFallPerBlock = 0.45f; // RMS of the low band, as a fraction of full scale

// What a closed car does to the sound getting out of it.
//
// Glass and steel are heavy and the seats are soft, so the bottom end walks
// straight through and nothing else does. A quarter of a kilohertz is about
// where it gives up, and the whole thing arrives a good deal quieter.
constexpr float kShellHz = 260.0f;
constexpr int kShellOrder = 4;
constexpr float kShellDb = -11.0f;

// The output stage.
//
// A soft clipper shapes every sample on its own, which on bass heavy material
// means the woofer modulates everything else: the bass survives and the
// midrange gets eaten, several decibels of it, every time a note lands. That
// is the sound of a blown speaker, not a loud one.
//
// Holding the ceiling by turning the whole thing down instead -- fast enough
// to catch a note, slow enough not to breathe -- leaves the balance alone but
// has the same shape: everything the room does to the bottom end (the cabin
// pressure, the hump, what an ear would make of it) puts the bass on the
// ceiling, and the music that reads as loud, above a couple of hundred hertz,
// comes out several decibels quieter than it went in, ducked whenever a note
// lands. Quiet and pumping at once.
//
// So the bass is held on its own. The output is split at kSplitHz, and the
// bottom band is limited to what fits beside the top band under the ceiling:
// a note that would not fit turns the bass down, never the rest. The whole is
// then held to the ceiling, which after that only ever has the top band to
// catch, and rarely. With the bass no longer arriving several decibels over,
// the whole thing is turned up by kMakeupDb so the top band comes out about
// as loud as it went in.
//
// The shaper stays behind it all, doing nothing until something gets past,
// so a system built to be far too loud still distorts rather than wrapping
// round into noise. "Behind it" means above the ceiling: with the shaper's
// knee below the ceiling, everything the limiter let through between the two
// was bent too, audible as a crackle on the loud notes.
constexpr float kLimitCeiling = 0.89f;
constexpr float kShaperKnee = 0.9f;
constexpr float kLimitLookAheadSeconds = 0.0015f;
// The bass comes back unhurried, a rebalance rather than a pump; the limiter
// behind it has only the odd peak of the top band to catch, and lets go
// quickly so that it is not heard breathing.
constexpr float kBassReleaseSeconds = 0.25f;
constexpr float kLimitReleaseSeconds = 0.06f;
// Where the bass ends: above the cabin's hump, below the body of a voice.
constexpr float kSplitHz = 300.0f;
constexpr int kSplitOrder = 4;   // Linkwitz-Riley, so the two bands sum flat
constexpr float kMakeupDb = 3.0f;
// And the most the bass may be turned down for it. Past this the top band is
// over the ceiling on its own, and the limiter behind takes the rest.
constexpr float kBassLimitFloor = 0.1f;

inline float OutputLimit(float x) {
    float a = std::fabs(x);
    if (a <= kShaperKnee) return x;
    float sign = x < 0.0f ? -1.0f : 1.0f;
    float room = 1.0f - kShaperKnee;
    return sign * (kShaperKnee + room * std::tanh((a - kShaperKnee) / room));
}

// How far past its nominal level the system is being asked to run, turned
// into how much of the midrange the bass is allowed to take with it.
float FeelDuckExtra(const SystemSettings &settings) {
    float over = settings.masterGainDb + settings.driveDb - kFeelDriveReference;
    return dsp::Clampf(over * kFeelDuckPerDb, 0.0f, kFeelDuckExtraMax);
}

bool HasAudibleSub(const SpeakerSystem &system) {
    for (const auto &s : system.Speakers())
        if (s.IsSub() && system.IsAudible(s)) return true;
    return false;
}

// The settings the engine works to: the bass setting made into the subs'
// level where there are subs (the shelf then stays flat), and room made for a
// boost. Turning the bass up takes part of it off everything else, as an
// equaliser's preamp does. Music sits at full scale, and with a system's bass
// already near the ceiling, a boost on top had nowhere to go but into the
// limiter, which took most of it straight back: +10 dB of bass came out as
// three. Taking part of the boost off everything lets the bass actually rise
// against the rest.
constexpr float kBassBoostHeadroom = 0.6f;
constexpr float kSubBoostHeadroom = 0.4f;

SystemSettings EngineSettings(const SpeakerSystem &system) {
    SystemSettings settings = system.Settings();
    float boost = std::max(0.0f, settings.bassDb);
    if (HasAudibleSub(system)) {
        settings.subGainDb += settings.bassDb;
        settings.bassDb = 0.0f;
        settings.masterGainDb -= kSubBoostHeadroom * boost;
    } else {
        settings.masterGainDb -= kBassBoostHeadroom * boost;
    }
    return settings;
}

float BassAuthority(const SpeakerSystem &system) {
    float best = 0.0f;
    for (const auto &s : system.Speakers()) {
        if (!system.IsAudible(s)) continue;
        if (s.IsSub()) return 1.0f;
        float corner = s.spec.lowCornerHz > 0.0f ? s.spec.lowCornerHz : 200.0f;
        best = std::max(best, dsp::Clampf((100.0f - corner) / 55.0f, 0.0f, 1.0f) * 0.6f);
    }
    return best;
}

} // namespace

bool Engine::Outside() const {
    // Outside the walls, with a little slack so that standing against one is
    // still inside. Only enclosed spaces have an outside worth modelling.
    if (m_room.kind == RoomKind::Outdoor) return false;
    const Vec3 &p = m_listener.position;
    float margin = 0.05f;
    return std::fabs(p.x) > m_room.width * 0.5f + margin ||
           std::fabs(p.y) > m_room.depth * 0.5f + margin || p.z < -margin ||
           p.z > m_room.height + margin;
}

void Engine::Init(float sampleRate, int maxBlockFrames) {
    m_sampleRate = sampleRate;
    m_maxBlock = std::max(64, maxBlockFrames);
    m_reverbSend.assign((size_t)m_maxBlock, 0.0f);
    m_voiceMono.assign((size_t)m_maxBlock, 0.0f);
    m_inL.assign((size_t)m_maxBlock, 0.0f);
    m_inR.assign((size_t)m_maxBlock, 0.0f);
    m_outL.assign((size_t)m_maxBlock, 0.0f);
    m_outR.assign((size_t)m_maxBlock, 0.0f);
    m_reverb.Init(sampleRate);
    m_roomModes.Init(sampleRate);
    // Slow: this follows how hard the system is running, not the music.
    m_shellL.SetButterworthLowpass(sampleRate, kShellHz, kShellOrder);
    m_shellR.SetButterworthLowpass(sampleRate, kShellHz, kShellOrder);
    m_shellGain = dsp::DbToGain(kShellDb);
    m_bassSense.SetLowpass(sampleRate, kBassSenseHz, 0.707f);
    m_bassLevel.SetCutoff(sampleRate, 3.0f);
    m_makeup = dsp::DbToGain(kMakeupDb);
    m_splitLowL.SetLinkwitzRileyLowpass(sampleRate, kSplitHz, kSplitOrder);
    m_splitLowR.SetLinkwitzRileyLowpass(sampleRate, kSplitHz, kSplitOrder);
    m_splitHighL.SetLinkwitzRileyHighpass(sampleRate, kSplitHz, kSplitOrder);
    m_splitHighR.SetLinkwitzRileyHighpass(sampleRate, kSplitHz, kSplitOrder);
    int ahead = std::max(1, std::min(1024, (int)std::lround(kLimitLookAheadSeconds * sampleRate)));
    m_highL.assign((size_t)ahead, 0.0f);
    m_highR.assign((size_t)ahead, 0.0f);
    m_highPos = 0;
    m_bassLimit.Init(ahead, 1.0f - std::exp(-1.0f / (kBassReleaseSeconds * sampleRate)));
    m_limit.Init(ahead, 1.0f - std::exp(-1.0f / (kLimitReleaseSeconds * sampleRate)));
    m_room.Derive();
    m_toneL.assign((size_t)m_maxBlock, 0.0f);
    m_toneR.assign((size_t)m_maxBlock, 0.0f);
    m_hrtfRenderer.init();
    const size_t block = fastplay::audio::kHrtfBlock;
    m_blockInL.assign(block, 0.0f);
    m_blockInR.assign(block, 0.0f);
    m_blockOutL.assign(block, 0.0f);
    m_blockOutR.assign(block, 0.0f);
    m_blockFill = 0;
}

void Engine::SetHrtf(const fastplay::audio::HrtfDatabase *db) {
    m_hrtf = db && db->ready() ? db : nullptr;
    for (auto &v : m_voices) v.SetHrtf(m_hrtf, &m_hrtfRenderer);
}

void Engine::Limiter::Init(int aheadSamples, float releasePerSample) {
    ahead = std::max(1, aheadSamples);
    release = releasePerSample;
    bufL.assign((size_t)ahead, 0.0f);
    bufR.assign((size_t)ahead, 0.0f);
    needs.assign((size_t)ahead, 1.0f);
    hold.assign((size_t)ahead, 1.0f);
    Reset();
}

void Engine::Limiter::Reset() {
    gain = 1.0f;
    pos = 0;
    std::fill(bufL.begin(), bufL.end(), 0.0f);
    std::fill(bufR.begin(), bufR.end(), 0.0f);
    std::fill(needs.begin(), needs.end(), 1.0f);
    std::fill(hold.begin(), hold.end(), 1.0f);
    sum = ahead;
}

void Engine::Prepare(const SpeakerSystem &system) {
    m_room = system.Room();
    m_settings = EngineSettings(system);
    m_listener = system.GetListener();

    bool hasSub = HasAudibleSub(system);

    m_bassAuthority = BassAuthority(system);
    m_feelDuckExtra = FeelDuckExtra(m_settings);
    // The whole correction, measured at the bottom of the band: what the ear
    // is missing when a system this loud arrives through headphones.
    m_feelTiltDb = loudness::TiltDb(20.0f, kHeadphoneFullScaleSpl, kSystemFullScaleSpl);

    // ---- voices ----------------------------------------------------------
    const auto &speakers = system.Speakers();
    if (m_voices.size() != speakers.size()) {
        m_voices.resize(speakers.size());
        for (auto &v : m_voices) v.Init(m_sampleRate);
    }
    for (auto &v : m_voices) v.SetHrtf(m_hrtf, &m_hrtfRenderer);
    m_voiceLabels.resize(speakers.size());
    for (size_t i = 0; i < speakers.size(); ++i) {
        m_voices[i].Configure(speakers[i], m_settings, hasSub);
        m_voices[i].SetActive(system.IsAudible(speakers[i]));
        m_voiceLabels[i] = speakers[i].Label(m_room);
    }

    // ---- the room --------------------------------------------------------
    m_reverb.Configure(m_room);
    m_roomModes.Configure(m_room);

    // Standing waves are driven by whatever is making the bass, which is the
    // subwoofers if there are any and everything otherwise.
    Vec3 sum{};
    int counted = 0;
    for (const auto &s : system.Speakers()) {
        if (!system.IsAudible(s)) continue;
        if (hasSub && !s.IsSub()) continue;
        sum = sum + s.position;
        ++counted;
    }
    m_modeSource = counted > 0 ? sum * (1.0f / (float)counted) : Vec3{0.0f, 0.0f, 0.0f};

    m_cabinActive = m_settings.cabinGainEnabled && m_room.cabinGainDb > 0.1f &&
                    m_room.cabinGainHz > 1.0f;
    if (m_cabinActive) {
        // A shelf rather than a boost at a frequency: below the corner the
        // whole band lifts together, which is what pressurising a sealed space
        // actually does.
        float hz = m_room.cabinGainHz;
        float db = m_room.cabinGainDb;
        m_cabinL.SetLowShelf(m_sampleRate, hz, 0.6f, db);
        m_cabinR.SetLowShelf(m_sampleRate, hz, 0.6f, db);
    }

    // What the space does to the rest of the bottom end on the way to your
    // ears, where a room has been measured and has a shape to apply.
    m_shape.clear();
    float character = dsp::Clampf(m_settings.cabinCharacter, 0.0f, 1.5f);
    for (const auto &band : m_room.cabinShape) {
        if (band.hz <= 1.0f || (band.hz >= kCabinPressureHz && band.db * character == 0.0f)) continue;
        ShapeStage stage;
        float db = band.hz < kCabinPressureHz ? band.db : band.db * character;
        if (band.peak) {
            stage.l.SetPeaking(m_sampleRate, band.hz, band.q, db);
            stage.r.SetPeaking(m_sampleRate, band.hz, band.q, db);
        } else {
            stage.l.SetLowShelf(m_sampleRate, band.hz, band.q, db);
            stage.r.SetLowShelf(m_sampleRate, band.hz, band.q, db);
        }
        m_shape.push_back(stage);
    }
}

void Engine::UpdateLevels(const SpeakerSystem &system) {
    const auto &speakers = system.Speakers();
    // A speaker added or removed since the last Prepare() means the voices no
    // longer line up with the system, and only a rebuild can fix that.
    if (speakers.size() != m_voices.size()) return;

    m_settings = EngineSettings(system);
    m_bassAuthority = BassAuthority(system);
    m_feelDuckExtra = FeelDuckExtra(m_settings);
    for (size_t i = 0; i < speakers.size(); ++i) {
        m_voices[i].SetLevels(speakers[i], m_settings);
        m_voices[i].SetActive(system.IsAudible(speakers[i]));
    }
}

void Engine::Reset() {
    for (auto &v : m_voices) v.Reset();
    m_reverb.Reset();
    m_roomModes.Reset();
    m_bassSense.Reset();
    m_bassLevel.Reset();
    m_feelDb = 0.0f;
    m_splitLowL.Reset();
    m_splitLowR.Reset();
    m_splitHighL.Reset();
    m_splitHighR.Reset();
    std::fill(m_highL.begin(), m_highL.end(), 0.0f);
    std::fill(m_highR.begin(), m_highR.end(), 0.0f);
    m_highPos = 0;
    m_bassLimit.Reset();
    m_limit.Reset();
    m_bassL.Reset();
    m_bassR.Reset();
    std::fill(m_blockOutL.begin(), m_blockOutL.end(), 0.0f);
    std::fill(m_blockOutR.begin(), m_blockOutR.end(), 0.0f);
    m_blockFill = 0;
    m_feelBlend = -1.0f;
    for (int i = 0; i < kFeelStages; ++i) {
        m_feelL[i].Reset();
        m_feelR[i].Reset();
    }
    m_cabinL.Reset();
    m_cabinR.Reset();
    for (auto &stage : m_shape) {
        stage.l.Reset();
        stage.r.Reset();
    }
    m_peak = 0.0f;
}

void Engine::ApplyCabinGain(float *buffer, dsp::Biquad &filter, int frames) {
    if (!m_cabinActive) return;
    for (int i = 0; i < frames; ++i) buffer[i] = filter.Process(buffer[i]);
}

void Engine::Render(const float *inL, const float *inR, int frames, float *outL, float *outR) {
    std::fill(outL, outL + frames, 0.0f);
    std::fill(outR, outR + frames, 0.0f);
    m_peak = 0.0f;

    bool roomOn = m_settings.roomEnabled;
    const bool outside = Outside();

    int done = 0;
    while (done < frames) {
        int n = std::min(frames - done, std::min(kGeometryBlock, m_maxBlock));

        std::fill(m_reverbSend.begin(), m_reverbSend.begin() + n, 0.0f);

        // ---- the bass control, before any of the speakers ------------------
        const float *blockL = inL + done, *blockR = inR + done;
        const float bassTarget = dsp::Clampf(m_settings.bassDb, -15.0f, 15.0f);
        if (m_bassDb != bassTarget) {
            m_bassDb += dsp::Clampf(bassTarget - m_bassDb, -kBassStepDb, kBassStepDb);
            m_bassL.SetLowShelf(m_sampleRate, kBassHz, 0.7f, m_bassDb);
            m_bassR.SetLowShelf(m_sampleRate, kBassHz, 0.7f, m_bassDb);
        }
        if (m_bassDb != 0.0f) {
            for (int i = 0; i < n; ++i) {
                m_toneL[(size_t)i] = m_bassL.Process(blockL[i]);
                m_toneR[(size_t)i] = m_bassR.Process(blockR[i]);
            }
            blockL = m_toneL.data();
            blockR = m_toneR.data();
        }
        const bool hrtf = m_hrtf && n == fastplay::audio::kHrtfBlock;
        if (hrtf) m_hrtfRenderer.begin_block();

        // ---- speakers ----------------------------------------------------
        for (size_t vi = 0; vi < m_voices.size(); ++vi) {
            Voice &v = m_voices[vi];
            v.Update(m_room, m_listener, roomOn);
            v.Process(blockL, blockR, n, outL + done, outR + done, m_voiceMono.data());

            float send = roomOn ? m_reverb.SendFor(v.Distance()) : 0.0f;
            for (int i = 0; i < n; ++i) m_reverbSend[(size_t)i] += m_voiceMono[(size_t)i] * send;
        }
        // What the speakers and their reflections sound like at your ears
        if (hrtf) m_hrtfRenderer.end_block(outL + done, outR + done);

        // ---- the room's own tail ------------------------------------------
        if (roomOn && !outside) m_reverb.Process(m_reverbSend.data(), n, outL + done, outR + done);

        // ---- what the space does on the way to your ears -------------------
        // The response measured from a speaker to a seat: the door loading it,
        // the cabin it crosses.
        if (m_settings.cabinGainEnabled && !outside) {
            for (auto &stage : m_shape) {
                for (int i = 0; i < n; ++i) {
                    outL[done + i] = stage.l.Process(outL[done + i]);
                    outR[done + i] = stage.r.Process(outR[done + i]);
                }
            }
        }

        // ---- and out through the shell ---------------------------------
        // Everything the speakers do has to get past the doors to reach you,
        // which is why a car going past is bass and not much else.
        if (outside) {
            for (int i = 0; i < n; ++i) {
                outL[done + i] = m_shellL.Process(outL[done + i]) * m_shellGain;
                outR[done + i] = m_shellR.Process(outR[done + i]) * m_shellGain;
            }
        }

        // ---- and the room as a pressure vessel ----------------------------
        // Both of these are the room acting on everything at once rather than
        // on any one speaker, so they go here, on the sum. They are applied
        // per chunk so that they stay in step with the geometry above.
        // A room with a measured shape has its whole response in that shape
        // already, pressure gain included, so the shelf would be counted
        // twice.
        if (m_shape.empty() && !outside) {
            ApplyCabinGain(outL + done, m_cabinL, n);
            ApplyCabinGain(outR + done, m_cabinR, n);
        }
        if (roomOn && !outside) {
            m_roomModes.Update(m_room, m_listener.position, m_modeSource);
            m_roomModes.Process(outL + done, outR + done, n);
        }

        done += n;
    }

    // ---- what an ear would make of a system this loud ---------------------
    //
    // The contours say an ear hearing a car through headphones is short of
    // about sixteen decibels at the bottom. There is rarely sixteen decibels
    // of room left to give it, and asking for it anyway only hands the whole
    // problem to the limiter, which turns the music down every time a note
    // lands -- the bass survives and everything else gets eaten.
    //
    // So only as much is asked for as there is room for, plus a little over,
    // which the bass limiter below holds to what fits. A quiet passage gets
    // the whole correction and swells; a loud one gets a tilt, bass against
    // the rest, and stays clean.
    //
    // It runs only while there is bass present and something able to have
    // produced it: doors on their own never lost any bottom end.
    if (m_settings.bassFeel > 0.0f && m_bassAuthority > 0.0f) {
        float present = 0.0f, bare = 0.0f;
        for (int i = 0; i < frames; ++i) {
            float low = m_bassSense.Process(0.5f * (outL[i] + outR[i]));
            present = m_bassLevel.Process(std::fabs(low));
            bare = std::max(bare, std::max(std::fabs(outL[i]), std::fabs(outR[i])));
        }
        float wanted = m_feelTiltDb * dsp::Clampf(present / kBassSenseFull, 0.0f, 1.0f) *
                       m_bassAuthority * dsp::Clampf(m_settings.bassFeel, 0.0f, 2.0f);

        // What there is room for: the headroom going spare, and no more than
        // kFeelMaxDuckDb over that, which the bass limiter takes back off the
        // bass alone.
        float headroom = dsp::GainToDb(kFeelCeiling / std::max(bare, 1e-6f));
        float duck = kFeelMaxDuckDb + m_feelDuckExtra;
        float allowed = std::min(wanted, std::max(headroom, 0.0f) + duck);

        // Eased rather than jumped, so the shelves do not zip as the music
        // moves. Backing off is quicker than coming on.
        float rate = allowed < m_feelDb ? kFeelFallPerBlock : kFeelRisePerBlock;
        m_feelDb += (allowed - m_feelDb) * rate;

        float blend = m_feelDb / std::max(m_feelTiltDb, 0.01f);
        if (std::fabs(blend - m_feelBlend) > 0.005f) {
            m_feelBlend = blend;
            for (int i = 0; i < kFeelStages; ++i) {
                float db = m_feelTiltDb * kFeel[i].share * blend;
                m_feelL[i].SetLowShelf(m_sampleRate, kFeel[i].hz, kFeel[i].q, db);
                m_feelR[i].SetLowShelf(m_sampleRate, kFeel[i].hz, kFeel[i].q, db);
            }
        }
        // Always run, even with next to nothing asked of them: shelves stopped
        // while the bass was away kept what they held from then, and came back
        // with it as a step when it returned -- a loud click, seconds after.
        {
            for (int i = 0; i < frames; ++i) {
                float l = outL[i], r = outR[i];
                for (int stage = 0; stage < kFeelStages; ++stage) {
                    l = m_feelL[stage].Process(l);
                    r = m_feelR[stage].Process(r);
                }
                outL[i] = l;
                outR[i] = r;
            }
        }
    }

    // ---- the output stage ------------------------------------------------
    // The master level was applied by the speakers themselves. What is left
    // is the makeup, then holding the bass to what fits beside the rest, then
    // holding the whole to the ceiling; see the notes at the top.
    const int ahead = m_limit.ahead;
    for (int i = 0; i < frames; ++i) {
        float l = outL[i] * m_makeup, r = outR[i] * m_makeup;
        float lowL = m_splitLowL.Process(l), lowR = m_splitLowR.Process(r);
        float highL = m_splitHighL.Process(l), highR = m_splitHighR.Process(r);

        // The bass may have what the top band leaves under the ceiling.
        float low = std::max(std::fabs(lowL), std::fabs(lowR));
        float high = std::max(std::fabs(highL), std::fabs(highR));
        float need = 1.0f;
        if (low + high > kLimitCeiling && low > 1e-6f) {
            need = std::max((kLimitCeiling - high) / low, kBassLimitFloor);
        }
        float heldL, heldR;
        m_bassLimit.Process(lowL, lowR, need, heldL, heldR);

        // The top band, delayed by as much as the bass was.
        int p = m_highPos;
        float lateL = m_highL[(size_t)p], lateR = m_highR[(size_t)p];
        m_highL[(size_t)p] = highL;
        m_highR[(size_t)p] = highR;
        m_highPos = p + 1 == ahead ? 0 : p + 1;

        l = heldL + lateL;
        r = heldR + lateR;
        float a = std::max(std::fabs(l), std::fabs(r));
        m_peak = std::max(m_peak, a);
        m_limit.Process(l, r, a > kLimitCeiling ? kLimitCeiling / a : 1.0f, l, r);
        outL[i] = OutputLimit(l);
        outR[i] = OutputLimit(r);
    }
}

void Engine::RenderInterleaved(const float *in, float *out, int frames) {
    if (m_hrtf) {
        // A block at a time, as the HRTF works: what comes in fills the next
        // block while the last one's result goes out, a block late. `in` and
        // `out` may be the same buffer, so each piece is read before it is written.
        const int block = fastplay::audio::kHrtfBlock;
        int done = 0;
        while (done < frames) {
            int take = std::min(frames - done, block - m_blockFill);
            for (int i = 0; i < take; ++i) {
                const size_t at = (size_t)(done + i) * 2;
                const size_t slot = (size_t)(m_blockFill + i);
                const float l = in[at], r = in[at + 1];
                m_blockInL[slot] = l;
                m_blockInR[slot] = r;
                out[at] = m_blockOutL[slot];
                out[at + 1] = m_blockOutR[slot];
            }
            m_blockFill += take;
            done += take;
            if (m_blockFill == block) {
                Render(m_blockInL.data(), m_blockInR.data(), block, m_blockOutL.data(), m_blockOutR.data());
                m_blockFill = 0;
            }
        }
        return;
    }
    // In pieces through the scratch set up in Init(), so nothing is allocated
    // and any length can be rendered. `in` and `out` may be the same buffer.
    int done = 0;
    while (done < frames) {
        int n = std::min(frames - done, m_maxBlock);
        for (int i = 0; i < n; ++i) {
            m_inL[(size_t)i] = in[(size_t)(done + i) * 2];
            m_inR[(size_t)i] = in[(size_t)(done + i) * 2 + 1];
        }
        Render(m_inL.data(), m_inR.data(), n, m_outL.data(), m_outR.data());
        for (int i = 0; i < n; ++i) {
            out[(size_t)(done + i) * 2] = m_outL[(size_t)i];
            out[(size_t)(done + i) * 2 + 1] = m_outR[(size_t)i];
        }
        done += n;
    }
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

float Engine::OutputPeakDb() const { return dsp::GainToDb(m_peak); }

float Engine::VoiceOverdrive(int index) const {
    if (index < 0 || index >= (int)m_voices.size()) return 0.0f;
    return m_voices[(size_t)index].Overdrive();
}

float Engine::VoiceExcursion(int index) const {
    if (index < 0 || index >= (int)m_voices.size()) return 0.0f;
    return m_voices[(size_t)index].Excursion();
}

float Engine::VoiceCompressionDb(int index) const {
    if (index < 0 || index >= (int)m_voices.size()) return 0.0f;
    return m_voices[(size_t)index].CompressionDb();
}

const std::string &Engine::VoiceLabel(int index) const {
    if (index < 0 || index >= (int)m_voiceLabels.size()) return m_emptyLabel;
    return m_voiceLabels[(size_t)index];
}

}  // namespace speakers
