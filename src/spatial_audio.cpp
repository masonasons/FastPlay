#include "spatial_audio.h"
#include "effects.h"
#include "speakers/engine/engine.h"
#include "speakers/model/system.h"
#include "speakers/presets.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#define FASTPLAY_X86_FTZ 1
#endif

using namespace fastplay::audio;

static constexpr float RAD2DEG = 180.0f / 3.14159265358979f;
// Output level of each mode, so 3D Audio plays about as loud as it did with its
// previous renderer (measured on noise: about 1.3 dB binaural, 1.8 dB in 5.1).
static constexpr float BINAURAL_LEVEL = 1.16f;
static constexpr float SURROUND_LEVEL = 1.23f;
static constexpr float DEG2RAD = 3.14159265358979f / 180.0f;

static SpatialAudio* g_spatialAudio = nullptr;

SpatialAudio* GetSpatialAudio() {
    if (!g_spatialAudio) g_spatialAudio = new SpatialAudio();
    return g_spatialAudio;
}

void FreeSpatialAudio() {
    if (g_spatialAudio) { g_spatialAudio->Shutdown(); delete g_spatialAudio; g_spatialAudio = nullptr; }
}

SpatialAudio::SpatialAudio()
    : m_upmix(static_cast<size_t>(MAX_SPEAKERS) * FRAME_SIZE),
      m_outL(FRAME_SIZE), m_outR(FRAME_SIZE),
      m_queueL(MAX_QUEUE), m_queueR(MAX_QUEUE),
      m_engine(std::make_unique<speakers::Engine>()),
      m_system(std::make_unique<speakers::SpeakerSystem>()),
      m_speakerDry(static_cast<size_t>(SPEAKER_CHUNK) * 2),
      m_speakerOld(static_cast<size_t>(SPEAKER_CHUNK) * 2) {
    m_renderer.init();
    for (auto& voice : m_voices) voice.init();
    m_voiceInput.allocate(FRAME_SIZE);
}

SpatialAudio::~SpatialAudio() {
    Shutdown();
}

void SpatialAudio::ResetVoices() {
    for (auto& voice : m_voices) voice.reset();
}

void SpatialAudio::SetMode(SpatialMode mode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_mode != mode) {
        // The speakers change meaning; start them fresh.
        ResetVoices();
        m_carryCount = 0;
        m_queueCount = 0;
        if (mode == SpatialMode::Speakers) {
            m_engine->Reset();
            m_oldEngine.reset();
        }
        m_mode = mode;
    }
}

bool SpatialAudio::Initialize(int sampleRate) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastError.clear();

    // Building the filters resamples the whole set, so keep them while the rate stays the same.
    if (!m_hrtf.ready() || m_hrtf.sample_rate() != sampleRate) {
        if (!m_hrtf.init(hrtf_builtin_set(), sampleRate)) {
            m_lastError = L"The HRTF could not be prepared for this sample rate.";
            m_initialized = false;
            return false;
        }
    }
    m_sampleRate = sampleRate;
    ResetVoices();

    m_engine->Init(static_cast<float>(sampleRate), SPEAKER_CHUNK);
    m_engine->SetHrtf(&m_hrtf);
    RebuildSpeakers(false);
    // A new track: nothing of the last one may ring on into it.
    m_engine->Reset();
    m_oldEngine.reset();
    m_clearTails = false;

    m_carryCount = 0;

    // Pre-fill output queue with FRAME_SIZE silence to cover initial deficit
    std::memset(m_queueL.data(), 0, FRAME_SIZE * sizeof(float));
    std::memset(m_queueR.data(), 0, FRAME_SIZE * sizeof(float));
    m_queueCount = FRAME_SIZE;

    m_initialized = true;
    return true;
}

void SpatialAudio::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_initialized = false;
    m_carryCount = 0;
    m_queueCount = 0;
    m_convBuf.clear();
    m_convBuf.shrink_to_fit();
}

float* SpatialAudio::GetConversionBuffer(int samples) {
    if (samples > static_cast<int>(m_convBuf.size())) {
        m_convBuf.resize(samples);
    }
    return m_convBuf.data();
}

void SpatialAudio::RenderSpeaker(int speaker, const float* signal, float gain, float angleDeg,
                                 float lx, float ly, float lz) {
    // Speaker on a unit circle around the origin (x right, y up, z behind), seen
    // from the listener position.
    float rad = angleDeg * DEG2RAD;
    float dx = std::sin(rad) - lx;
    float dy = -ly;
    float dz = -std::cos(rad) - lz;
    float azimuth = 0.0f, elevation = 0.0f;
    float horizontal = std::sqrt(dx * dx + dz * dz);
    if (horizontal > 0.0001f || std::fabs(dy) > 0.0001f) {
        azimuth = std::atan2(dx, -dz) * RAD2DEG;          // clockwise from straight ahead
        elevation = std::atan2(dy, horizontal) * RAD2DEG;  // up is positive
    }

    float* in = m_voiceInput.data();
    for (int i = 0; i < FRAME_SIZE; i++) in[i] = signal[i] * gain;
    m_renderer.render_voice(m_voices[speaker], in, m_hrtf, nullptr, m_hrtf.lookup(azimuth, elevation), 1.0f);
}

// Process a single FRAME_SIZE chunk in binaural (2-speaker) mode
void SpatialAudio::ProcessBinauralFrame(float* frameL, float* frameR) {
    float width = GetParamValue(ParamId::SpatialWidth);
    float rotation = GetParamValue(ParamId::SpatialRotation);
    float lx = GetParamValue(ParamId::SpatialX);
    float ly = GetParamValue(ParamId::SpatialY);
    float lz = GetParamValue(ParamId::SpatialZ);

    // Left and right channels as two speakers, summed at equal power
    m_renderer.begin_block();
    RenderSpeaker(0, frameL, 0.707f * BINAURAL_LEVEL, -width - rotation, lx, ly, lz);
    RenderSpeaker(1, frameR, 0.707f * BINAURAL_LEVEL, width - rotation, lx, ly, lz);

    std::fill(m_outL.begin(), m_outL.end(), 0.0f);
    std::fill(m_outR.begin(), m_outR.end(), 0.0f);
    m_renderer.end_block(m_outL.data(), m_outR.data());
    std::memcpy(frameL, m_outL.data(), FRAME_SIZE * sizeof(float));
    std::memcpy(frameR, m_outR.data(), FRAME_SIZE * sizeof(float));
}

// Process a single FRAME_SIZE chunk in virtual surround mode
void SpatialAudio::ProcessSurroundFrame(float* frameL, float* frameR) {
    float width = GetParamValue(ParamId::SpatialWidth);
    float rotation = GetParamValue(ParamId::SpatialRotation);
    float lx = GetParamValue(ParamId::SpatialX);
    float ly = GetParamValue(ParamId::SpatialY);
    float lz = GetParamValue(ParamId::SpatialZ);

    // Width controls the front speaker angle; surround placement depends on RC
    float frontAngle = width;
    float surroundAngle;
    if (m_rearCenter) {
        // With RC: surrounds go far back (135-172°) since RC fills dead center
        surroundAngle = 180.0f - (width * 0.5f);
    } else {
        // Without RC: surrounds stay to the sides (90-120°), standard 5.1 layout
        // This creates a clear side-surround without rear fill
        surroundAngle = 90.0f + (width * 0.33f);
    }

    // Upmix stereo to 6 channels
    float* fl = m_upmix.data();
    float* fr = fl + FRAME_SIZE;
    float* center = fl + 2 * FRAME_SIZE;
    float* sl = fl + 3 * FRAME_SIZE;
    float* sr = fl + 4 * FRAME_SIZE;
    float* rc = fl + 5 * FRAME_SIZE;
    for (int i = 0; i < FRAME_SIZE; i++) {
        float l = frameL[i];
        float r = frameR[i];
        float mid = (l + r) * 0.5f;
        float side = (l - r) * 0.5f;

        fl[i] = l;
        fr[i] = r;
        center[i] = mid * 0.6f;
        sl[i] = l * 0.5f + side * 1.0f;
        sr[i] = r * 0.5f - side * 1.0f;
        rc[i] = mid * 0.9f;
    }

    struct SpeakerInfo {
        const float* signal;
        float angle;
        float gain;
    };
    const SpeakerInfo speakers[] = {
        { fl,     -frontAngle    + rotation, 0.9f },
        { fr,      frontAngle    + rotation, 0.9f },
        { center,  0.0f          + rotation, 0.7f },
        { sl,     -surroundAngle + rotation, 1.0f },
        { sr,      surroundAngle + rotation, 1.0f },
        { rc,      180.0f        + rotation, 1.0f },
    };
    int speakerCount = m_rearCenter ? 6 : 5;

    // The output normalisation is folded into each speaker's gain.
    float norm = (m_rearCenter ? 0.33f : 0.38f) * SURROUND_LEVEL;
    m_renderer.begin_block();
    for (int s = 0; s < speakerCount; s++) {
        RenderSpeaker(s, speakers[s].signal, speakers[s].gain * norm, speakers[s].angle, lx, ly, lz);
    }

    std::fill(m_outL.begin(), m_outL.end(), 0.0f);
    std::fill(m_outR.begin(), m_outR.end(), 0.0f);
    m_renderer.end_block(m_outL.data(), m_outR.data());
    std::memcpy(frameL, m_outL.data(), FRAME_SIZE * sizeof(float));
    std::memcpy(frameR, m_outR.data(), FRAME_SIZE * sizeof(float));
}

void SpatialAudio::Process(float* buffer, int frameCount, float blend) {
    if (!m_initialized || frameCount <= 0) return;
    // Waits for a change in progress (a millisecond or two) rather than skipping
    // the block: a skipped block went out unprocessed, a burst of the dry sound in
    // the middle of the room's, which is a loud click.
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) return;

    if (m_mode == SpatialMode::Speakers) {
        ProcessSpeakers(buffer, frameCount, blend);
        return;
    }

    int newPos = 0;

    while (true) {
        int available = m_carryCount + (frameCount - newPos);
        if (available < FRAME_SIZE) break;
        if (m_queueCount + FRAME_SIZE > MAX_QUEUE) break;

        // Fill one frame of L and R from carry then input
        float fL[FRAME_SIZE], fR[FRAME_SIZE];
        int filled = 0;

        int fromCarry = m_carryCount;
        if (fromCarry > FRAME_SIZE) fromCarry = FRAME_SIZE;
        for (int i = 0; i < fromCarry; i++) {
            fL[i] = m_carryL[i];
            fR[i] = m_carryR[i];
        }
        filled = fromCarry;

        m_carryCount -= fromCarry;
        if (m_carryCount > 0) {
            std::memmove(m_carryL, m_carryL + fromCarry, m_carryCount * sizeof(float));
            std::memmove(m_carryR, m_carryR + fromCarry, m_carryCount * sizeof(float));
        }

        int fromInput = FRAME_SIZE - filled;
        for (int i = 0; i < fromInput; i++) {
            fL[filled + i] = buffer[(newPos + i) * 2];
            fR[filled + i] = buffer[(newPos + i) * 2 + 1];
        }
        newPos += fromInput;

        // Process frame based on mode
        if (m_mode == SpatialMode::Surround51) {
            ProcessSurroundFrame(fL, fR);
        } else {
            ProcessBinauralFrame(fL, fR);
        }

        // Append to output queue
        for (int i = 0; i < FRAME_SIZE; i++) {
            m_queueL[m_queueCount + i] = fL[i];
            m_queueR[m_queueCount + i] = fR[i];
        }
        m_queueCount += FRAME_SIZE;
    }

    // Save remaining new input as carry (should be < FRAME_SIZE if queue had room)
    int remaining = frameCount - newPos;
    if (remaining > FRAME_SIZE) remaining = FRAME_SIZE;  // clamp to prevent overrun
    if (remaining > 0) {
        for (int i = 0; i < remaining; i++) {
            m_carryL[m_carryCount + i] = buffer[(newPos + i) * 2];
            m_carryR[m_carryCount + i] = buffer[(newPos + i) * 2 + 1];
        }
        m_carryCount += remaining;
    }

    // Write from output queue to buffer
    bool doBlend = (blend < 1.0f);
    float wet = blend, dry = 1.0f - blend;

    int toWrite = frameCount;
    if (toWrite > m_queueCount) toWrite = m_queueCount;

    for (int i = 0; i < toWrite; i++) {
        if (doBlend) {
            buffer[i * 2]     = buffer[i * 2]     * dry + m_queueL[i] * wet;
            buffer[i * 2 + 1] = buffer[i * 2 + 1] * dry + m_queueR[i] * wet;
        } else {
            buffer[i * 2]     = m_queueL[i];
            buffer[i * 2 + 1] = m_queueR[i];
        }
    }

    if (toWrite > 0 && toWrite < m_queueCount) {
        std::memmove(m_queueL.data(), m_queueL.data() + toWrite, (m_queueCount - toWrite) * sizeof(float));
        std::memmove(m_queueR.data(), m_queueR.data() + toWrite, (m_queueCount - toWrite) * sizeof(float));
    }
    m_queueCount -= toWrite;
}

// ---------------------------------------------------------------------------
// Room presets
// ---------------------------------------------------------------------------

void SpatialAudio::SetRoomPreset(int preset) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (preset == m_preset) return;
    m_preset = preset;
    RebuildSpeakers(true);  // a different room, faded into: the last one's tail does not carry over
}

void SpatialAudio::SetSubwoofer(bool on) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (on == m_subOn) return;
    m_subOn = on;
    // Structural: with no sub playing, the other speakers keep their bottom end.
    RebuildSpeakers(true);
}

void SpatialAudio::SetSubLevel(float db) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_subDb = db;
    ApplySpeakerLevels();
}

void SpatialAudio::SetCrossover(float hz) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (hz == m_crossoverHz) return;
    m_crossoverHz = hz;
    RebuildSpeakers(true);  // new crossover filters
}

void SpatialAudio::SetBassFeel(float amount) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_bassFeel = amount;
    ApplySpeakerLevels();
}

void SpatialAudio::SetConeNoise(float amount) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_coneNoise = amount;
    ApplySpeakerLevels();
}

void SpatialAudio::RebuildSpeakers(bool crossfade) {
    m_retiredEngine.reset();
    speakers::BuildRoomPreset(m_preset, *m_system);
    speakers::SystemSettings& settings = m_system->Settings();
    settings.crossoverHz = m_crossoverHz;
    settings.subGainDb = m_subDb;
    settings.bassFeel = m_bassFeel;
    settings.coneNoise = m_coneNoise;
    for (auto& speaker : m_system->Speakers()) {
        if (speaker.IsSub()) speaker.muted = !m_subOn;
    }
    if (m_sampleRate <= 0) return;
    if (crossfade && m_initialized && m_mode == SpatialMode::Speakers) {
        // Built beside the one playing, which fades out as this fades in. (A
        // change during a fade lets the oldest go at once: by then it is quiet.)
        auto fresh = std::make_unique<speakers::Engine>();
        fresh->Init(static_cast<float>(m_sampleRate), SPEAKER_CHUNK);
        fresh->SetHrtf(&m_hrtf);
        fresh->Prepare(*m_system);
        m_retiredEngine = std::move(m_oldEngine);
        m_oldEngine = std::move(m_engine);
        m_engine = std::move(fresh);
        m_crossfadeDone = 0;
    } else {
        m_engine->Prepare(*m_system);
    }
    UpdateListener();
}

void SpatialAudio::ApplySpeakerLevels() {
    speakers::SystemSettings& settings = m_system->Settings();
    settings.subGainDb = m_subDb;
    settings.bassFeel = m_bassFeel;
    settings.coneNoise = m_coneNoise;
    if (m_sampleRate > 0) m_engine->UpdateLevels(*m_system);
}

// Where you are in the room: the preset's seat, moved by the 3D Listener
// settings (tenths of a metre across, forward and up), and turned by 3D Rotation.
void SpatialAudio::UpdateListener() {
    const speakers::RoomSpec& room = m_system->Room();
    speakers::Listener listener;
    float halfWidth = std::max(0.0f, room.width * 0.5f - room.wallMargin);
    float halfDepth = std::max(0.0f, room.depth * 0.5f - room.wallMargin);
    listener.position.x = std::clamp(room.defaultListener.x + GetParamValue(ParamId::SpatialX) * 0.1f,
                                     -halfWidth, halfWidth);
    listener.position.y = std::clamp(room.defaultListener.y + GetParamValue(ParamId::SpatialY) * 0.1f,
                                     -halfDepth, halfDepth);
    listener.position.z = std::clamp(room.listenerHeight + GetParamValue(ParamId::SpatialZ) * 0.1f, 0.3f,
                                     std::max(0.3f, room.height - 0.1f));
    listener.yawDeg = room.defaultYawDeg + GetParamValue(ParamId::SpatialRotation);
    m_engine->SetListener(listener);
}

void SpatialAudio::ProcessSpeakers(float* buffer, int frameCount, float blend) {
#ifdef FASTPLAY_X86_FTZ
    // Reverb tails and filters decaying into silence reach denormal numbers,
    // which x86 processors handle many times slower. Flush them to zero while
    // the room renders (ARM has no such penalty).
    struct FlushDenormals {
        unsigned saved = _mm_getcsr();
        FlushDenormals() { _mm_setcsr(saved | 0x8040); }  // FTZ and DAZ
        ~FlushDenormals() { _mm_setcsr(saved); }
    } flush;
#endif
    if (m_clearTails.exchange(false)) {
        m_engine->Reset();
        m_oldEngine.reset();
    }
    UpdateListener();
    if (blend >= 1.0f && !m_oldEngine) {
        m_engine->RenderInterleaved(buffer, buffer, frameCount);
        return;
    }
    float wet = blend, dry = 1.0f - blend;
    for (int done = 0; done < frameCount; done += SPEAKER_CHUNK) {
        int n = std::min(SPEAKER_CHUNK, frameCount - done);
        float* chunk = buffer + static_cast<size_t>(done) * 2;
        std::memcpy(m_speakerDry.data(), chunk, static_cast<size_t>(n) * 2 * sizeof(float));
        if (!m_oldEngine) {
            m_engine->RenderInterleaved(chunk, chunk, n);
        } else {
            // A change of room: the music is faded out of the old one and into the
            // new, rather than the two rooms' sound being swapped. Each then does
            // what a room does with a sound starting and stopping: the new one's
            // arrives faded in however far its speakers are from the seat, and the
            // old one's rings on and dies away.
            float peak = 0.0f;
            for (int i = 0; i < n; i++) {
                float t = std::min(1.0f, static_cast<float>(m_crossfadeDone + i) / SPEAKER_CROSSFADE);
                for (int c = 0; c < 2; c++) {
                    chunk[i * 2 + c] = m_speakerDry[i * 2 + c] * t;
                    m_speakerOld[i * 2 + c] = m_speakerDry[i * 2 + c] * (1.0f - t);
                }
            }
            m_engine->RenderInterleaved(chunk, chunk, n);
            m_oldEngine->RenderInterleaved(m_speakerOld.data(), m_speakerOld.data(), n);
            for (int i = 0; i < n * 2; i++) {
                chunk[i] += m_speakerOld[i];
                peak = std::max(peak, std::fabs(m_speakerOld[i]));
            }
            m_crossfadeDone += n;
            // Until its tail has died away (or a few seconds, whatever it holds)
            const int longest = m_sampleRate * 4;
            if (m_crossfadeDone >= longest || (m_crossfadeDone > SPEAKER_CROSSFADE && peak < 1e-4f)) {
                m_retiredEngine = std::move(m_oldEngine);
            }
        }
        if (blend < 1.0f) {
            for (int i = 0; i < n * 2; i++) chunk[i] = m_speakerDry[i] * dry + chunk[i] * wet;
        }
    }
}
