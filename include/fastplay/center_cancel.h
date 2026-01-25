#pragma once
#ifndef FASTPLAY_CENTER_CANCEL_H
#define FASTPLAY_CENTER_CANCEL_H

#include <complex>
#include <vector>

// FFT-based center channel canceler/extractor
// Uses spectral processing to identify and remove/isolate center-panned content

class CenterCancelProcessor {
public:
    CenterCancelProcessor();
    ~CenterCancelProcessor();

    // Initialize with sample rate and FFT size
    bool Init(int sampleRate, int fftSize = 4096);
    void Reset();

    // Set the cancel amount (-1.0 = extract center, 0.0 = off, 1.0 = cancel center)
    void SetAmount(float amount) { m_amount = amount; }
    float GetAmount() const { return m_amount; }

    // Process stereo float samples (interleaved L/R)
    // Returns number of output samples available
    void ProcessFloat(float* input, int inputFrames, float* output, int& outputFrames);

    // Process stereo 16-bit samples (interleaved L/R)
    void ProcessInt16(short* input, int inputFrames, short* output, int& outputFrames);

    bool IsInitialized() const { return m_initialized; }

private:
    // FFT functions
    void FFT(std::complex<float>* data, int n, bool inverse);
    void ApplyWindow(float* data, int n);

    // Process one FFT frame
    void ProcessFrame();

    bool m_initialized;
    int m_sampleRate;
    int m_fftSize;
    int m_hopSize;      // Overlap amount (fftSize / 4 for 75% overlap)
    float m_amount;     // -1 to 1

    // Buffers
    std::vector<float> m_inputBufferL;
    std::vector<float> m_inputBufferR;
    std::vector<float> m_outputBufferL;
    std::vector<float> m_outputBufferR;
    std::vector<float> m_window;

    // FFT work buffers
    std::vector<std::complex<float>> m_fftL;
    std::vector<std::complex<float>> m_fftR;

    int m_inputPos;     // Current position in input buffer
    int m_outputPos;    // Current read position in output buffer
    int m_outputAvail;  // Samples available in output buffer
};

// Global processor instance
CenterCancelProcessor* GetCenterCancelProcessor();
void InitCenterCancelProcessor(int sampleRate);
void FreeCenterCancelProcessor();

#endif // FASTPLAY_CENTER_CANCEL_H
