#pragma once
#ifndef FASTPLAY_SPATIAL_AUDIO_H
#define FASTPLAY_SPATIAL_AUDIO_H

#ifdef USE_STEAM_AUDIO

#include <phonon.h>
#include <windows.h>
#include "types.h"

class SpatialAudio {
public:
    static constexpr int FRAME_SIZE = 128;
    static constexpr int MAX_QUEUE = 16384;  // Must handle largest BASS callback (~500ms @ 48kHz = ~24000 frames)

    SpatialAudio();
    ~SpatialAudio();

    bool Initialize(int sampleRate);
    void Shutdown();
    void Process(float* buffer, int frameCount, float blend);
    bool IsInitialized() const { return m_initialized; }

    void SetMode(SpatialMode mode);
    SpatialMode GetMode() const { return m_mode; }

    void SetRearCenter(bool enabled) { m_rearCenter = enabled; }
    bool GetRearCenter() const { return m_rearCenter; }

    const wchar_t* GetLastError() const { return m_lastError; }
    volatile int m_debugStep;  // For crash diagnostics
    void SetLastError(const wchar_t* msg) { wcscpy_s(m_lastError, msg); }
    float* GetConversionBuffer(int samples);

private:
    void ProcessBinaural(float* buffer, int frameCount, float blend);
    void ProcessSurround51(float* buffer, int frameCount, float blend);

    void ProcessBinauralFrame(float* frameL, float* frameR);
    void ProcessSurroundFrame(float* frameL, float* frameR);

    IPLContext m_context;
    IPLHRTF m_hrtf;

    // Binaural mode: 2 effects (L, R)
    IPLBinauralEffect m_effectL;
    IPLBinauralEffect m_effectR;

    // Surround mode: 6 effects (FL, FR, C, SL, SR, RC)
    IPLBinauralEffect m_effectFL;
    IPLBinauralEffect m_effectFR;
    IPLBinauralEffect m_effectC;
    IPLBinauralEffect m_effectSL;
    IPLBinauralEffect m_effectSR;
    IPLBinauralEffect m_effectRC;  // Rear center (180°)

    // Per-frame working buffers (FRAME_SIZE each, heap-allocated)
    float* m_mono;
    float* m_tmpL;
    float* m_tmpR;
    float* m_savL;
    float* m_savR;

    // Surround upmix buffers (FRAME_SIZE each, heap-allocated)
    float* m_upmix;   // 6 * FRAME_SIZE: FL, FR, C, SL, SR, RC interleaved
    float* m_outAccL;  // Accumulation buffer for surround output L
    float* m_outAccR;  // Accumulation buffer for surround output R

    // Int16-to-float conversion buffer (heap-allocated)
    float* m_convBuf;
    int m_convBufSize;

    // Input carry (remainder from previous callback, < FRAME_SIZE samples)
    float m_carryL[FRAME_SIZE * 2];  // Extra margin for safety
    float m_carryR[FRAME_SIZE * 2];
    int m_carryCount;

    // Output queue (pre-filled to absorb deficit)
    float m_queueL[MAX_QUEUE];
    float m_queueR[MAX_QUEUE];
    int m_queueCount;

    int m_sampleRate;
    bool m_initialized;
    SpatialMode m_mode;
    bool m_rearCenter;
    wchar_t m_lastError[256];
    CRITICAL_SECTION m_cs;
};

SpatialAudio* GetSpatialAudio();
void FreeSpatialAudio();

#endif
#endif
