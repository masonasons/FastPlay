#include "spatial.h"

#include <algorithm>
#include <cmath>

namespace speakers {

namespace {

// How sharp the pinna notch is, and how deep.
// How much later than the direct sound a reflection has to arrive before
// it counts. Everything closer is faded out, because at that spacing it
// is not heard as a reflection -- it just comb filters the direct sound.
constexpr float kReflectionFadeMs = 2.0f;

constexpr float kPinnaQ = 4.0f;
constexpr float kPinnaDepthDb = -9.0f;

constexpr float kSpeedOfSound = 343.0f;
constexpr float kHeadRadius = 0.0875f; // metres, the usual spherical head
constexpr float kPi = 3.14159265358979f;
constexpr float kDegToRad = kPi / 180.0f;

// Half a second of delay line: enough for the longest first order reflection
// in the hall (about seventy metres) plus any alignment delay on top.
constexpr int kDelayCapacity = 24576;

// Distance at which a source is at unity gain. Closer than this it stops
// getting louder, which keeps a speaker you walk into from deafening you.
constexpr float kRefDistance = 1.0f;
constexpr float kMinDistance = 0.25f;
// Where the head starts to cast a shadow: a wavelength a few head widths long.
constexpr float kHeadShadowHz = 500.0f;

struct HeadFrame {
    Vec3 forward, right;
};

HeadFrame MakeHeadFrame(float yawDeg) {
    float yaw = yawDeg * kDegToRad;
    HeadFrame h;
    h.forward = {std::sin(yaw), std::cos(yaw), 0.0f};
    h.right = {std::cos(yaw), -std::sin(yaw), 0.0f};
    return h;
}

// Constant power pan from a signed left/right coordinate in the head frame.
void PanFromAzimuth(float azimuth, float &gainL, float &gainR) {
    // Map -pi..pi onto a pan position that keeps the rear hemisphere panned
    // the same way as the front: you cannot tell front from back by level.
    float pan = dsp::Clampf(std::sin(azimuth), -1.0f, 1.0f);
    float angle = (pan + 1.0f) * 0.25f * kPi; // 0..pi/2
    gainL = std::cos(angle);
    gainR = std::sin(angle);
}

} // namespace

void Spatializer::Init(float sampleRate) {
    m_sampleRate = sampleRate;
    m_line.Init(kDelayCapacity);

    // Twenty milliseconds of smoothing on the delays: fast enough to track a
    // walk, slow enough that the interpolation never clicks.
    m_delayL.SetTime(sampleRate, 20.0f);
    m_delayR.SetTime(sampleRate, 20.0f);
    m_gainL.SetTime(sampleRate, 15.0f);
    m_gainR.SetTime(sampleRate, 15.0f);
    m_shelfL.SetTime(sampleRate, 15.0f);
    m_shelfR.SetTime(sampleRate, 15.0f);
    m_splitL.SetCutoff(sampleRate, kHeadShadowHz);
    m_splitR.SetCutoff(sampleRate, kHeadShadowHz);
    m_delayC.SetTime(sampleRate, 20.0f);
    m_gainC.SetTime(sampleRate, 15.0f);
    for (auto &t : m_taps) {
        t.gain.SetTime(sampleRate, 30.0f);
        t.delay.SetTime(sampleRate, 40.0f);
        t.gainL.SetTime(sampleRate, 30.0f);
        t.gainR.SetTime(sampleRate, 30.0f);
    }
    Reset();
}

void Spatializer::Reset() {
    m_line.Reset();
    m_directivity.Reset();
    m_air.Reset();
    m_bassHold.Reset();
    m_shadowL.Reset();
    m_shadowR.Reset();
    m_splitL.Reset();
    m_splitR.Reset();
    m_shelfL.Snap(1.0f);
    m_shelfR.Snap(1.0f);
    m_reflectionToneL.Reset();
    m_reflectionToneR.Reset();
    m_pinnaL.Reset();
    m_pinnaR.Reset();
    m_delayL.Snap(0.0f);
    m_delayR.Snap(0.0f);
    m_gainL.Snap(0.0f);
    m_gainR.Snap(0.0f);
    m_delayC.Snap(0.0f);
    m_gainC.Snap(0.0f);
    if (m_hrtf) {
        m_hrtfDirect.reset();
        for (auto &state : m_hrtfTaps) state.reset();
    }
    for (auto &t : m_taps) {
        t.gain.Snap(0.0f);
        t.tone.Reset();
        t.delay.Snap(0.0f);
        t.gainL.Snap(0.0f);
        t.gainR.Snap(0.0f);
    }
    m_directivityActive = false;
    m_settle = true;
}

void Spatializer::UpdateReflection(int index, Vec3 imagePos, const Listener &listener,
                                   float wallGain, float directDistance) {
    Tap &tap = m_taps[index];
    Vec3 rel = imagePos - listener.position;
    float dist = std::max(Length(rel), kMinDistance);

    HeadFrame head = MakeHeadFrame(listener.yawDeg);
    float x = Dot(rel, head.right);
    float y = Dot(rel, head.forward);
    float azimuth = std::atan2(x, y);

    float gL = 0.0f, gR = 0.0f;
    PanFromAzimuth(azimuth, gL, gR);

    // One bounce. Deliberately well below what the inverse square law alone
    // would give: a real surface scatters most of what it does not absorb, so
    // only a fraction comes back as a clean specular reflection. Six taps at
    // the level the geometry suggests would be louder than the direct sound,
    // which is audibly wrong.
    float gain = wallGain * (kRefDistance / dist) * 0.24f;

    // A reflection that arrives almost on top of the direct sound is the one
    // thing that ruins being able to tell where a speaker is. Inside a car the
    // walls are centimetres away, so the mirror images land a fraction of a
    // millisecond late -- far too close to be heard as reflections, and close
    // enough to comb filter the direct sound and smear the cues that place it.
    //
    // Fading them in over the first couple of milliseconds of extra path takes
    // that away and leaves genuine room reflections untouched. It is honest
    // too: a surface that close is part of the near field, and a real cabin is
    // upholstery and glass at angles, not a rigid rectangular box.
    float extraMs = std::max(0.0f, (dist - directDistance) / kSpeedOfSound * 1000.0f);
    float fade = dsp::Clampf(extraMs / kReflectionFadeMs, 0.0f, 1.0f);
    gain *= fade * fade * (3.0f - 2.0f * fade);
    tap.delay.target = (dist / kSpeedOfSound) * m_sampleRate;
    tap.gainL.target = gL * gain;
    tap.gainR.target = gR * gain;
    // Through the HRTF: the level, and where it comes from
    tap.gain.target = gain;
    if (m_hrtf) {
        float elevation = std::asin(dsp::Clampf(rel.z / dist, -1.0f, 1.0f));
        m_lookTaps[index] = m_hrtf->lookup(azimuth / kDegToRad, elevation / kDegToRad);
    }
}

void Spatializer::Update(const RoomSpec &room, const Listener &listener, Vec3 sourcePos, Vec3 aim,
                         float beamingHz, float extraDelayMs, bool reflectionsEnabled) {
    Vec3 rel = sourcePos - listener.position;
    float dist = std::max(Length(rel), kMinDistance);
    m_distance = dist;

    HeadFrame head = MakeHeadFrame(listener.yawDeg);
    float x = Dot(rel, head.right); // positive to your right
    float z = rel.z;

    float elevation = std::asin(dsp::Clampf(z / dist, -1.0f, 1.0f));

    // Through the HRTF: one path to the middle of the head, the measured head
    // supplying the difference between the ears, from this direction
    m_delayC.target = std::max(0.0f, (dist / kSpeedOfSound + extraDelayMs * 0.001f) * m_sampleRate);
    m_gainC.target = kRefDistance / dist;
    if (m_hrtf) {
        float y = Dot(rel, head.forward);
        m_lookDirect = m_hrtf->lookup(std::atan2(x, y) / kDegToRad, elevation / kDegToRad);
    }

    // ---- how far it is from each ear, separately -------------------------
    //
    // Not one distance for the head with a time difference bolted on. Your two
    // ears are in different places, so a source half a metre away is measurably
    // further from one than the other, and that difference is most of what
    // tells you where a nearby speaker is. Sharing a single distance between
    // the ears throws it away and leaves a door speaker sounding like it is
    // everywhere -- which is exactly what it sounded like.
    Vec3 earOffset = head.right * kHeadRadius;
    float distL = std::max(Length(sourcePos - (listener.position - earOffset)), kMinDistance);
    float distR = std::max(Length(sourcePos - (listener.position + earOffset)), kMinDistance);

    // How much of the head is in the way of each ear, 0 to 1. This is already
    // the direction cosine along the axis through the ears, so it accounts for
    // elevation on its own -- multiplying by the cosine of the elevation again
    // double counts it, and quietly cancels most of the cue.
    float unitX = dsp::Clampf(x / dist, -1.0f, 1.0f);
    float shadowL = dsp::Clampf(unitX, 0.0f, 1.0f);
    float shadowR = dsp::Clampf(-unitX, 0.0f, 1.0f);

    // Sound reaching the far ear has to bend around the head, which is further
    // than the straight line to it. Adding that makes the straight line
    // distances agree with the textbook interaural delay at the side and with
    // simple geometry up close, without either being special cased.
    constexpr float kDiffraction = kPi * 0.5f - 1.0f;
    float pathL = distL + kHeadRadius * shadowL * kDiffraction;
    float pathR = distR + kHeadRadius * shadowR * kDiffraction;

    m_delayL.target =
        std::max(0.0f, (pathL / kSpeedOfSound + extraDelayMs * 0.001f) * m_sampleRate);
    m_delayR.target =
        std::max(0.0f, (pathR / kSpeedOfSound + extraDelayMs * 0.001f) * m_sampleRate);

    // ---- level and shadow ------------------------------------------------
    // The head's shadow is a shelf, not a volume control: above a few hundred
    // hertz the far ear is quieter, below it a wavelength several heads long
    // simply bends round. Cutting the whole band made a sub in one corner of a
    // room five decibels louder in one ear than the other, which no bass does.
    m_gainL.target = kRefDistance / distL;
    m_gainR.target = kRefDistance / distR;
    m_shelfL.target = 1.0f - 0.55f * shadowL;
    m_shelfR.target = 1.0f - 0.55f * shadowR;

    // And the far ear loses the top end, hard: a head is a good obstacle by
    // the time the wavelength is shorter than it is.
    m_shadowL.SetCutoff(m_sampleRate, 20000.0f * std::exp(-3.2f * shadowL));
    m_shadowR.SetCutoff(m_sampleRate, 20000.0f * std::exp(-3.2f * shadowR));

    // Pinna notch, the one cue that separates above from below. It climbs in
    // frequency as the source rises.
    float elevNorm = dsp::Clampf(elevation / (kPi * 0.5f), -1.0f, 1.0f);
    // Sharp, not broad. A real pinna notch is a narrow null cut by the ridges
    // of one particular ear -- it moves with elevation, which is what makes it
    // a cue for height. Spread over an octave and applied to both ears at once
    // it stops being a cue at all and becomes a tone control, taking four
    // decibels out of everything from four kilohertz up and leaving the whole
    // thing dull.
    float notchHz = dsp::Clampf(7000.0f * (1.0f + 0.55f * elevNorm), 3000.0f, 12000.0f);
    m_pinnaL.SetPeaking(m_sampleRate, notchHz, kPinnaQ, kPinnaDepthDb);
    m_pinnaR.SetPeaking(m_sampleRate, notchHz, kPinnaQ, kPinnaDepthDb);

    // ---- off axis and air -------------------------------------------------
    m_directivityActive = beamingHz > 0.0f;
    if (m_directivityActive) {
        Vec3 toListener = Normalized(rel * -1.0f);
        float cosOff = dsp::Clampf(Dot(Normalized(aim), toListener), -1.0f, 1.0f);
        float offAxis = std::acos(cosOff) / kPi; // 0 on axis, 1 straight behind
        // Up to 14 dB off the top end when you are behind a speaker. Small
        // cones stay wide, which is why a tweeter is still audible off axis
        // and a fifteen is not.
        m_directivity.SetHighShelf(m_sampleRate, dsp::Clampf(beamingHz, 200.0f, 12000.0f), 0.7f,
                                   -14.0f * offAxis * offAxis);
    }
    // Air absorption. Small over a car, real across a hall.
    m_air.SetCutoff(m_sampleRate, 20000.0f * std::exp(-dist * 0.045f));

    // And the bass that the distance law should not have taken. Indoors, below
    // the frequency where the room stops behaving like open air, the level is
    // much the same wherever you stand -- so give back what the inverse square
    // law just removed, up to a point. Walk away from a car and you lose the
    // kick from the doors long before you lose the subwoofer, which is the
    // whole shape of hearing a system from across a car park.
    //
    // Symmetrically: it takes back off things that are closer than the
    // reference just as it gives to things further away. Only lifting would
    // leave a sub across the car boosted while a door speaker at half a metre
    // kept the extra bass the distance law handed it, which is not uniform
    // pressure, it is a thumb on the scale.
    m_bassHoldActive = room.kind != RoomKind::Outdoor;
    if (m_bassHoldActive) {
        float restore = dsp::Clampf(20.0f * std::log10(dist / kRefDistance), -8.0f, 14.0f);
        // In a room the reflections and the modes carry half of this already:
        // they are how the pressure holds up, and the full amount on top of
        // them left every home preset ten decibels heavy below 250 Hz.
        if (room.kind != RoomKind::Vehicle) restore *= 0.5f;
        float corner = dsp::Clampf(room.cabinGainHz * 2.5f, 70.0f, 320.0f);
        m_bassHold.SetLowShelf(m_sampleRate, corner, 0.7f, restore);
    }

    // ---- first order reflections ------------------------------------------
    m_reflectionsOn = reflectionsEnabled && room.kind != RoomKind::Outdoor;
    if (m_reflectionsOn) {
        float wallGain = std::sqrt(std::max(0.0f, 1.0f - room.absorption));
        // A deader room takes more of the top end out of every bounce.
        float toneHz = dsp::Clampf(11000.0f * (1.0f - room.absorption * 1.6f), 1200.0f, 11000.0f);
        m_reflectionToneL.SetCutoff(m_sampleRate, toneHz);
        m_reflectionToneR.SetCutoff(m_sampleRate, toneHz);
        for (auto &t : m_taps) t.tone.SetCutoff(m_sampleRate, toneHz);
        float hx = room.width * 0.5f, hy = room.depth * 0.5f;
        Vec3 p = sourcePos;
        // Mirror the source through each surface in turn.
        UpdateReflection(0, {-hx * 2.0f - p.x, p.y, p.z}, listener, wallGain, dist);
        UpdateReflection(1, {hx * 2.0f - p.x, p.y, p.z}, listener, wallGain, dist);
        UpdateReflection(2, {p.x, -hy * 2.0f - p.y, p.z}, listener, wallGain, dist);
        UpdateReflection(3, {p.x, hy * 2.0f - p.y, p.z}, listener, wallGain, dist);
        UpdateReflection(4, {p.x, p.y, -p.z}, listener, wallGain, dist);
        UpdateReflection(5, {p.x, p.y, room.height * 2.0f - p.z}, listener, wallGain, dist);
    } else {
        for (auto &t : m_taps) {
            t.gainL.target = 0.0f;
            t.gainR.target = 0.0f;
            t.gain.target = 0.0f;
        }
    }

    // Straight after a reset there is nothing to glide from. The smoothing is
    // for moving while the music plays; sliding a delay up from zero at the
    // start of a track, or after a seek, bends the pitch for a tenth of a
    // second (a quarter of one for the reflections). The gains still fade in.
    if (m_settle) {
        m_settle = false;
        m_delayL.Snap(m_delayL.target);
        m_delayR.Snap(m_delayR.target);
        m_delayC.Snap(m_delayC.target);
        for (auto &t : m_taps) t.delay.Snap(t.delay.target);
    }
}

void Spatializer::SetHrtf(const fastplay::audio::HrtfDatabase *db, fastplay::audio::HrtfRenderer *renderer) {
    if (db && db != m_hrtf) {
        // Made ready here, off the audio thread
        m_hrtfDirect.init();
        for (auto &state : m_hrtfTaps) state.init();
    }
    m_hrtf = db;
    m_renderer = renderer;
}

void Spatializer::Process(const float *mono, int frames, float *outL, float *outR) {
    if (m_hrtf && m_renderer && frames == fastplay::audio::kHrtfBlock) {
        for (int i = 0; i < frames; ++i) {
            float x = mono[i];
            if (m_directivityActive) x = m_directivity.Process(x);
            x = m_air.Process(x);
            if (m_bassHoldActive) x = m_bassHold.Process(x);
            m_line.Write(x);
            m_blockDirect[i] = m_line.Read(m_delayC.Next()) * m_gainC.Next();
            // The ear model's own, kept moving so that leaving the HRTF does not jump
            m_delayL.Next();
            m_delayR.Next();
            m_gainL.Next();
            m_gainR.Next();
            m_shelfL.Next();
            m_shelfR.Next();
            for (int k = 0; k < kReflections; ++k) {
                Tap &t = m_taps[k];
                float e = m_line.Read(t.delay.Next()) * t.gain.Next();
                t.gainL.Next();
                t.gainR.Next();
                m_blockTaps[k][i] = t.tone.Process(e);
            }
        }
        m_renderer->render_voice(m_hrtfDirect, m_blockDirect, *m_hrtf, nullptr, m_lookDirect);
        if (m_reflectionsOn) {
            for (int k = 0; k < kReflections; ++k) {
                m_renderer->render_voice(m_hrtfTaps[k], m_blockTaps[k], *m_hrtf, nullptr, m_lookTaps[k]);
            }
        }
        (void)outL;
        (void)outR;
        return;
    }
    for (int i = 0; i < frames; ++i) {
        float x = mono[i];
        if (m_directivityActive) x = m_directivity.Process(x);
        x = m_air.Process(x);
        if (m_bassHoldActive) x = m_bassHold.Process(x);
        m_line.Write(x);

        float dl = m_delayL.Next();
        float dr = m_delayR.Next();
        float l = m_line.Read(dl);
        float r = m_line.Read(dr);

        l = m_shadowL.Process(l);
        r = m_shadowR.Process(r);
        float underL = m_splitL.Process(l), underR = m_splitR.Process(r);
        l = (underL + (l - underL) * m_shelfL.Next()) * m_gainL.Next();
        r = (underR + (r - underR) * m_shelfR.Next()) * m_gainR.Next();
        l = m_pinnaL.Process(l);
        r = m_pinnaR.Process(r);

        if (m_reflectionsOn) {
            float reflectedL = 0.0f, reflectedR = 0.0f;
            for (auto &t : m_taps) {
                float e = m_line.Read(t.delay.Next());
                reflectedL += e * t.gainL.Next();
                reflectedR += e * t.gainR.Next();
            }
            l += m_reflectionToneL.Process(reflectedL);
            r += m_reflectionToneR.Process(reflectedR);
        } else {
            // Keep the smoothers moving so that switching reflections back on
            // does not jump.
            for (auto &t : m_taps) {
                t.delay.Next();
                t.gainL.Next();
                t.gainR.Next();
            }
        }

        outL[i] += l;
        outR[i] += r;
    }
}

}  // namespace speakers
