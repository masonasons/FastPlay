#pragma once
#ifndef FASTPLAY_CONVOLUTION_H
#define FASTPLAY_CONVOLUTION_H

#include <vector>
#include <complex>
#include <string>

// Partitioned convolution reverb using overlap-save method
class ConvolutionReverb {
public:
    ConvolutionReverb();
    ~ConvolutionReverb();

    // Load impulse response from WAV file
    bool LoadIR(const wchar_t* path);

    // Initialize with sample rate (call after loading IR)
    bool Init(int sampleRate);

    // Reset internal buffers
    void Reset();

    // Process audio (stereo interleaved)
    void Process(float* buffer, int frames);

    // Parameters
    void SetMix(float mix) { m_mix = mix; }  // 0-100%
    float GetMix() const { return m_mix; }

    void SetGain(float gain) { m_gain = gain; }  // dB
    float GetGain() const { return m_gain; }

    bool IsLoaded() const { return m_irLoaded; }
    bool IsInitialized() const { return m_initialized; }
    const std::wstring& GetIRPath() const { return m_irPath; }

    // Get IR info
    int GetIRSampleRate() const { return m_irSampleRate; }
    int GetIRChannels() const { return m_irChannels; }
    float GetIRLengthMs() const;

private:
    // FFT helpers
    void FFT(std::complex<float>* data, int n, bool inverse);
    void ProcessPartition(int partitionIdx);

    // Resample IR if needed
    void ResampleIR(int targetRate);

    bool m_initialized;
    bool m_irLoaded;
    std::wstring m_irPath;

    int m_sampleRate;
    int m_irSampleRate;
    int m_irChannels;
    int m_irSamples;

    // FFT parameters
    int m_fftSize;        // FFT size (block size * 2)
    int m_blockSize;      // Processing block size
    int m_numPartitions;  // Number of IR partitions

    // IR in frequency domain (partitioned)
    // Each partition is an FFT of a block of the IR
    std::vector<std::vector<std::complex<float>>> m_irSpectrumL;
    std::vector<std::vector<std::complex<float>>> m_irSpectrumR;

    // Input buffer (overlap-save)
    std::vector<float> m_inputBufferL;
    std::vector<float> m_inputBufferR;
    int m_inputPos;

    // FFT buffers
    std::vector<std::complex<float>> m_fftBuffer;

    // Frequency-domain delay line for partitioned convolution
    std::vector<std::vector<std::complex<float>>> m_fdlL;  // Frequency delay line left
    std::vector<std::vector<std::complex<float>>> m_fdlR;  // Frequency delay line right
    int m_fdlPos;

    // Output accumulator
    std::vector<float> m_outputL;
    std::vector<float> m_outputR;

    // Parameters
    float m_mix;   // 0-100%
    float m_gain;  // dB
};

// Global instance management
ConvolutionReverb* GetConvolutionReverb();
void InitConvolutionReverb(int sampleRate);
void FreeConvolutionReverb();

#endif // FASTPLAY_CONVOLUTION_H
