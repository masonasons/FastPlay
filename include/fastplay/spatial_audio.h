#pragma once
#ifndef FASTPLAY_SPATIAL_AUDIO_H
#define FASTPLAY_SPATIAL_AUDIO_H

#include "types.h"
#include "spatial/hrtf.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace speakers {
class Engine;
class SpeakerSystem;
}

// 3D Audio, for headphones, in two kinds of mode:
// - Binaural and 5.1: the stereo signal played through virtual speakers around
//   the listener and rendered with an HRTF. Binaural uses two speakers; 5.1
//   upmixes to five (or six, with the rear center speaker).
// - Room presets (SpatialMode::Speakers): real speakers simulated in a room --
//   a car, a living room, a PA in a hall -- each with its drivers, crossover,
//   amplifier and placement, heard from a seat in it (src/speakers).
class SpatialAudio {
public:
    static constexpr int FRAME_SIZE = fastplay::audio::kHrtfBlock;
    static constexpr int MAX_QUEUE = 16384;  // Must handle largest BASS callback (~500ms @ 48kHz = ~24000 frames)
    static constexpr int MAX_SPEAKERS = 6;

    SpatialAudio();
    ~SpatialAudio();

    bool Initialize(int sampleRate);
    void Shutdown();
    // Interleaved stereo float; blend is the share of the 3D signal (0..1).
    void Process(float* buffer, int frameCount, float blend);
    bool IsInitialized() const { return m_initialized; }

    void SetMode(SpatialMode mode);
    SpatialMode GetMode() const { return m_mode; }
    void SetRearCenter(bool enabled) { m_rearCenter = enabled; }
    bool GetRearCenter() const { return m_rearCenter; }

    // Room presets: which one (an index into speakers/presets.h), its
    // subwoofers and crossover.
    void SetRoomPreset(int preset);
    void SetSubwoofer(bool on);
    void SetCrossover(float hz);
    // How much bass, in dB: the subs' level where there are subs, the bass
    // control on the amplifier where there are not.
    void SetBass(float db);
    void SetConeNoise(float amount);  // 0 to 10, 1 being as measured

    // Where you are: Binaural and 5.1's speaker spread (degrees from the front),
    // which way you face (degrees), and where you sit (in a room, tenths of a
    // metre from the seat). Read as each block is rendered.
    void SetWidth(float degrees) { m_width = degrees; }
    void SetRotation(float degrees) { m_rotation = degrees; }
    void SetListenerOffset(float x, float y, float z) {
        m_listenerX = x;
        m_listenerY = y;
        m_listenerZ = z;
    }

    // The music jumped (a seek): drop what the rooms were still sounding of the
    // old position -- reverberation, delay lines -- rather than play it over
    // the new one. Safe from any thread; the audio thread acts on it.
    void ClearTails() { m_clearTails = true; }

    const wchar_t* GetLastError() const { return m_lastError.c_str(); }

    // Scratch space for converting 16 bit audio to float before Process().
    float* GetConversionBuffer(int samples);

private:
    void ProcessBinauralFrame(float* frameL, float* frameR);
    void ProcessSurroundFrame(float* frameL, float* frameR);
    // Render one virtual speaker at angleDeg (clockwise from straight ahead, on a
    // unit circle around the origin) into the current block, heard from the
    // listener position.
    void RenderSpeaker(int speaker, const float* signal, float gain, float angleDeg,
                       float lx, float ly, float lz);
    void ResetVoices();

    // Room presets. The first two need m_mutex held.
    // After the preset or anything structural changed. `crossfade`: into a new
    // room built beside the one playing, rather than rebuilding it in place,
    // which would cut off everything it was sounding at once (a loud click).
    void RebuildSpeakers(bool crossfade);
    void ApplySpeakerLevels();   // after a level changed; never rebuilds a filter
    void ProcessSpeakers(float* buffer, int frameCount, float blend);
    void UpdateListener();

    fastplay::audio::HrtfDatabase m_hrtf;
    fastplay::audio::HrtfRenderer m_renderer;
    fastplay::audio::HrtfVoiceState m_voices[MAX_SPEAKERS];
    fastplay::audio::AlignedFloats m_voiceInput;  // one speaker's gain-scaled block

    // Surround upmix: FL, FR, C, SL, SR, RC, one block each
    std::vector<float> m_upmix;
    std::vector<float> m_outL, m_outR;

    // Int16-to-float conversion buffer
    std::vector<float> m_convBuf;

    // Input carry (remainder from previous callback, < FRAME_SIZE samples)
    float m_carryL[FRAME_SIZE * 2];  // Extra margin for safety
    float m_carryR[FRAME_SIZE * 2];
    int m_carryCount = 0;

    // Output queue (pre-filled to absorb deficit)
    std::vector<float> m_queueL, m_queueR;
    int m_queueCount = 0;

    int m_sampleRate = 0;
    bool m_initialized = false;
    SpatialMode m_mode = SpatialMode::Binaural;
    bool m_rearCenter = true;
    std::wstring m_lastError;
    std::mutex m_mutex;

    // Room presets
    static constexpr int SPEAKER_CHUNK = 2048;
    std::unique_ptr<speakers::Engine> m_engine;
    // The room the music is faded out of after a change (over SPEAKER_CROSSFADE
    // frames, then rendered on until its tail has died away), and one done with
    // (freed off the audio thread, at the next change)
    std::unique_ptr<speakers::Engine> m_oldEngine, m_retiredEngine;
    static constexpr int SPEAKER_CROSSFADE = 1024;
    int m_crossfadeDone = 0;
    std::vector<float> m_speakerOld;  // the old room's output, one chunk
    std::unique_ptr<speakers::SpeakerSystem> m_system;
    int m_preset = 0;
    bool m_subOn = true;
    float m_crossoverHz = 80.0f;
    float m_bassDb = 0.0f;
    float m_coneNoise = 1.0f;
    std::vector<float> m_speakerDry;  // one chunk, for blending
    std::atomic<bool> m_clearTails{false};
    std::atomic<float> m_width{45.0f};
    std::atomic<float> m_rotation{0.0f};
    std::atomic<float> m_listenerX{0.0f}, m_listenerY{0.0f}, m_listenerZ{0.0f};
};

SpatialAudio* GetSpatialAudio();
void FreeSpatialAudio();

#endif
