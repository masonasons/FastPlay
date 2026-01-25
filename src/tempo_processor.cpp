#include "tempo_processor.h"
#include "globals.h"
#include "bass_fx.h"
#include <cmath>
#include <memory>
#include <vector>
#include <mutex>
#include <deque>

// Include Speedy/Sonic
#ifdef USE_SPEEDY
extern "C" {
#include "sonic2.h"
}
#endif

// Include Rubber Band if available
#ifdef USE_RUBBERBAND
#ifndef RUBBERBAND_STATIC
#define RUBBERBAND_STATIC
#endif
#include "rubberband/RubberBandStretcher.h"
using namespace RubberBand;
#endif

// Global algorithm preference
static TempoAlgorithm g_algorithm = TempoAlgorithm::SoundTouch;
static std::unique_ptr<TempoProcessor> g_tempoProcessor;

// Algorithm metadata
const char* GetAlgorithmName(TempoAlgorithm algo) {
    switch (algo) {
        case TempoAlgorithm::SoundTouch: return "SoundTouch (BASS_FX)";
        case TempoAlgorithm::RubberBandR2: return "Rubber Band R2 (Faster)";
        case TempoAlgorithm::RubberBandR3: return "Rubber Band R3 (Finer)";
        case TempoAlgorithm::Speedy: return "Speedy (Google)";
        default: return "Unknown";
    }
}

const char* GetAlgorithmDescription(TempoAlgorithm algo) {
    switch (algo) {
        case TempoAlgorithm::SoundTouch:
            return "Fast processing, good for speech and general use";
        case TempoAlgorithm::RubberBandR2:
            return "Balanced quality/performance, good transient handling";
        case TempoAlgorithm::RubberBandR3:
            return "Highest quality, best for music (uses more CPU)";
        case TempoAlgorithm::Speedy:
            return "Nonlinear speech speedup, preserves consonants";
        default:
            return "";
    }
}

// ============================================================================
// SoundTouch (BASS_FX) Implementation
// ============================================================================
class SoundTouchProcessor : public TempoProcessor {
private:
    HSTREAM m_sourceStream = 0;
    HSTREAM m_fxStream = 0;
    float m_sampleRate = 44100.0f;
    float m_tempo = 0.0f;   // percentage
    float m_pitch = 0.0f;   // semitones
    float m_rate = 1.0f;    // multiplier

public:
    ~SoundTouchProcessor() override {
        Shutdown();
    }

    HSTREAM Initialize(HSTREAM sourceStream, float sampleRate) override {
        m_sourceStream = sourceStream;
        m_sampleRate = sampleRate;

        // Create tempo stream wrapping the source (use float for DSP effects)
        m_fxStream = BASS_FX_TempoCreate(sourceStream, BASS_FX_FREESOURCE | BASS_SAMPLE_FLOAT);
        if (!m_fxStream) {
            return 0;
        }

        // Apply SoundTouch algorithm settings
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_USE_AA_FILTER, g_stAntiAliasFilter ? 1.0f : 0.0f);
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_AA_FILTER_LENGTH, static_cast<float>(g_stAAFilterLength));
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_USE_QUICKALGO, g_stQuickAlgorithm ? 1.0f : 0.0f);
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_SEQUENCE_MS, static_cast<float>(g_stSequenceMs));
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_SEEKWINDOW_MS, static_cast<float>(g_stSeekWindowMs));
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_OVERLAP_MS, static_cast<float>(g_stOverlapMs));
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_PREVENT_CLICK, g_stPreventClick ? 1.0f : 0.0f);

        // Apply current tempo/pitch/rate settings
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO, m_tempo);
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_PITCH, m_pitch);
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_FREQ, m_sampleRate * m_rate);

        return m_fxStream;
    }

    void Shutdown() override {
        // BASS_FX_FREESOURCE flag means freeing fxStream frees source too
        if (m_fxStream) {
            BASS_StreamFree(m_fxStream);
            m_fxStream = 0;
            m_sourceStream = 0;
        }
    }

    void SetTempo(float tempoPercent) override {
        m_tempo = tempoPercent;
        if (m_fxStream) {
            BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO, m_tempo);
        }
    }

    void SetPitch(float semitones) override {
        m_pitch = semitones;
        if (m_fxStream) {
            BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_PITCH, m_pitch);
        }
    }

    void SetRate(float rate) override {
        m_rate = rate;
        if (m_fxStream) {
            BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_FREQ, m_sampleRate * m_rate);
        }
    }

    float GetTempo() const override { return m_tempo; }
    float GetPitch() const override { return m_pitch; }
    float GetRate() const override { return m_rate; }
    bool IsActive() const override { return m_fxStream != 0; }
    TempoAlgorithm GetAlgorithm() const override { return TempoAlgorithm::SoundTouch; }

    double GetLength() const override {
        if (!m_fxStream) return 0.0;
        QWORD bytes = BASS_ChannelGetLength(m_fxStream, BASS_POS_BYTE);
        if (bytes == (QWORD)-1) return 0.0;
        return BASS_ChannelBytes2Seconds(m_fxStream, bytes);
    }

    double GetPosition() const override {
        if (!m_fxStream) return 0.0;
        QWORD bytes = BASS_ChannelGetPosition(m_fxStream, BASS_POS_BYTE);
        if (bytes == (QWORD)-1) return 0.0;
        return BASS_ChannelBytes2Seconds(m_fxStream, bytes);
    }

    void SetPosition(double seconds) override {
        if (!m_fxStream) return;
        QWORD bytes = BASS_ChannelSeconds2Bytes(m_fxStream, seconds);
        BASS_ChannelSetPosition(m_fxStream, bytes, BASS_POS_BYTE | BASS_POS_FLUSH);
    }

    HSTREAM GetSourceStream() const override {
        return m_sourceStream;
    }
};

// ============================================================================
// Rubber Band Implementation - Push-based stream approach
// ============================================================================
#ifdef USE_RUBBERBAND

class RubberBandProcessor : public TempoProcessor {
private:
    HSTREAM m_sourceStream = 0;
    HSTREAM m_outputStream = 0;
    std::unique_ptr<RubberBandStretcher> m_stretcher;
    float m_sampleRate = 44100.0f;
    int m_channels = 2;
    float m_tempo = 0.0f;   // percentage
    float m_pitch = 0.0f;   // semitones
    float m_rate = 1.0f;    // multiplier
    TempoAlgorithm m_algorithm;
    bool m_sourceEnded = false;

    // Thread safety for parameter changes
    mutable std::mutex m_mutex;

    // Buffers
    std::vector<float> m_decodeBuffer;      // For reading from source
    std::vector<float*> m_deinterleavedIn;  // Pointers to deinterleaved input
    std::vector<float*> m_deinterleavedOut; // Pointers to deinterleaved output
    std::vector<std::vector<float>> m_channelIn;  // Per-channel input buffers
    std::vector<std::vector<float>> m_channelOut; // Per-channel output buffers
    std::deque<float> m_outputQueue;        // Interleaved output buffer

    static constexpr size_t DECODE_BLOCK_SIZE = 2048;  // Samples per channel
    static constexpr size_t MAX_OUTPUT_QUEUE = 65536;  // Max buffered samples

    // Convert tempo percentage to time ratio
    static double TempoToTimeRatio(float tempoPercent, float rate) {
        double speedMultiplier = (100.0 + tempoPercent) / 100.0 * rate;
        if (speedMultiplier < 0.1) speedMultiplier = 0.1;
        if (speedMultiplier > 10.0) speedMultiplier = 10.0;
        return 1.0 / speedMultiplier;
    }

    // Convert semitones to pitch scale
    static double SemitonesToPitchScale(float semitones) {
        return pow(2.0, semitones / 12.0);
    }

    // Process more audio from source through Rubber Band
    bool ProcessMoreAudio() {
        if (m_sourceEnded || !m_stretcher) return false;

        // Decode a block from source
        DWORD bytesNeeded = DECODE_BLOCK_SIZE * m_channels * sizeof(float);
        m_decodeBuffer.resize(DECODE_BLOCK_SIZE * m_channels);

        DWORD bytesRead = BASS_ChannelGetData(m_sourceStream, m_decodeBuffer.data(),
            bytesNeeded | BASS_DATA_FLOAT);

        if (bytesRead == (DWORD)-1 || bytesRead == 0) {
            // End of stream or error
            m_sourceEnded = true;
            // Process final block
            if (m_stretcher) {
                // Send empty final block
                for (int ch = 0; ch < m_channels; ch++) {
                    m_channelIn[ch].assign(1, 0.0f);
                    m_deinterleavedIn[ch] = m_channelIn[ch].data();
                }
                m_stretcher->process(m_deinterleavedIn.data(), 0, true);
            }
            return false;
        }

        size_t samplesDecoded = bytesRead / sizeof(float) / m_channels;

        // Deinterleave
        for (int ch = 0; ch < m_channels; ch++) {
            m_channelIn[ch].resize(samplesDecoded);
            for (size_t i = 0; i < samplesDecoded; i++) {
                m_channelIn[ch][i] = m_decodeBuffer[i * m_channels + ch];
            }
            m_deinterleavedIn[ch] = m_channelIn[ch].data();
        }

        // Process through Rubber Band
        m_stretcher->process(m_deinterleavedIn.data(), samplesDecoded, false);

        // Retrieve any available output
        int available = m_stretcher->available();
        if (available > 0) {
            // Ensure output buffers are large enough
            for (int ch = 0; ch < m_channels; ch++) {
                m_channelOut[ch].resize(available);
                m_deinterleavedOut[ch] = m_channelOut[ch].data();
            }

            size_t retrieved = m_stretcher->retrieve(m_deinterleavedOut.data(), available);

            // Interleave into output queue
            for (size_t i = 0; i < retrieved; i++) {
                for (int ch = 0; ch < m_channels; ch++) {
                    m_outputQueue.push_back(m_channelOut[ch][i]);
                }
            }
        }

        return true;
    }

    // BASS stream callback
    static DWORD CALLBACK StreamProc(HSTREAM handle, void* buffer, DWORD length, void* user) {
        RubberBandProcessor* proc = static_cast<RubberBandProcessor*>(user);
        if (!proc) return BASS_STREAMPROC_END;

        std::lock_guard<std::mutex> lock(proc->m_mutex);

        float* outBuf = static_cast<float*>(buffer);
        DWORD samplesNeeded = length / sizeof(float);
        DWORD samplesWritten = 0;

        // Fill from output queue, processing more if needed
        while (samplesWritten < samplesNeeded) {
            // Check if we have data in queue
            if (!proc->m_outputQueue.empty()) {
                size_t canCopy = std::min((size_t)(samplesNeeded - samplesWritten),
                                         proc->m_outputQueue.size());
                for (size_t i = 0; i < canCopy; i++) {
                    outBuf[samplesWritten++] = proc->m_outputQueue.front();
                    proc->m_outputQueue.pop_front();
                }
            } else if (!proc->m_sourceEnded) {
                // Need more data - process from source
                if (!proc->ProcessMoreAudio()) {
                    // Source ended, check for remaining output
                    int remaining = proc->m_stretcher ? proc->m_stretcher->available() : 0;
                    if (remaining > 0) {
                        for (int ch = 0; ch < proc->m_channels; ch++) {
                            proc->m_channelOut[ch].resize(remaining);
                            proc->m_deinterleavedOut[ch] = proc->m_channelOut[ch].data();
                        }
                        size_t retrieved = proc->m_stretcher->retrieve(
                            proc->m_deinterleavedOut.data(), remaining);
                        for (size_t i = 0; i < retrieved; i++) {
                            for (int ch = 0; ch < proc->m_channels; ch++) {
                                proc->m_outputQueue.push_back(proc->m_channelOut[ch][i]);
                            }
                        }
                    }
                    if (proc->m_outputQueue.empty()) {
                        break;  // Truly done
                    }
                }
            } else {
                // Source ended and queue empty
                break;
            }
        }

        // Zero any remaining samples if we couldn't fill the buffer
        while (samplesWritten < samplesNeeded) {
            outBuf[samplesWritten++] = 0.0f;
        }

        if (proc->m_sourceEnded && proc->m_outputQueue.empty()) {
            return samplesWritten * sizeof(float) | BASS_STREAMPROC_END;
        }

        return samplesWritten * sizeof(float);
    }

public:
    explicit RubberBandProcessor(TempoAlgorithm algorithm)
        : m_algorithm(algorithm) {}

    ~RubberBandProcessor() override {
        Shutdown();
    }

    HSTREAM Initialize(HSTREAM sourceStream, float sampleRate) override {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_sourceStream = sourceStream;
        m_sampleRate = sampleRate;
        m_sourceEnded = false;
        m_outputQueue.clear();

        // Get channel info
        BASS_CHANNELINFO info;
        if (!BASS_ChannelGetInfo(sourceStream, &info)) {
            return 0;
        }
        m_channels = info.chans;

        // Initialize buffers
        m_channelIn.resize(m_channels);
        m_channelOut.resize(m_channels);
        m_deinterleavedIn.resize(m_channels);
        m_deinterleavedOut.resize(m_channels);

        // Set up Rubber Band options
        RubberBandStretcher::Options options =
            RubberBandStretcher::OptionProcessRealTime |
            RubberBandStretcher::OptionThreadingNever;

        // Select engine based on algorithm
        if (m_algorithm == TempoAlgorithm::RubberBandR3) {
            options |= RubberBandStretcher::OptionEngineFiner;
        } else {
            options |= RubberBandStretcher::OptionEngineFaster;
        }

        // Apply user-configured Rubber Band settings
        // Formant preservation
        if (g_rbFormantPreserved) {
            options |= RubberBandStretcher::OptionFormantPreserved;
        }

        // Pitch mode (for R3 these must be set at construction)
        switch (g_rbPitchMode) {
            case 0: options |= RubberBandStretcher::OptionPitchHighSpeed; break;
            case 1: options |= RubberBandStretcher::OptionPitchHighQuality; break;
            case 2: options |= RubberBandStretcher::OptionPitchHighConsistency; break;
        }

        // Window size
        switch (g_rbWindowSize) {
            case 0: options |= RubberBandStretcher::OptionWindowStandard; break;
            case 1: options |= RubberBandStretcher::OptionWindowShort; break;
            case 2: options |= RubberBandStretcher::OptionWindowLong; break;
        }

        // Channel handling
        if (g_rbChannels == 1) {
            options |= RubberBandStretcher::OptionChannelsTogether;
        }

        // R2-only options (only apply for R2 engine)
        if (m_algorithm == TempoAlgorithm::RubberBandR2) {
            // Transients
            switch (g_rbTransients) {
                case 0: options |= RubberBandStretcher::OptionTransientsCrisp; break;
                case 1: options |= RubberBandStretcher::OptionTransientsMixed; break;
                case 2: options |= RubberBandStretcher::OptionTransientsSmooth; break;
            }

            // Detector
            switch (g_rbDetector) {
                case 0: options |= RubberBandStretcher::OptionDetectorCompound; break;
                case 1: options |= RubberBandStretcher::OptionDetectorPercussive; break;
                case 2: options |= RubberBandStretcher::OptionDetectorSoft; break;
            }

            // Phase
            if (g_rbPhase == 1) {
                options |= RubberBandStretcher::OptionPhaseIndependent;
            }

            // Smoothing
            if (g_rbSmoothing) {
                options |= RubberBandStretcher::OptionSmoothingOn;
            }
        }

        // Create the stretcher
        m_stretcher = std::make_unique<RubberBandStretcher>(
            static_cast<size_t>(m_sampleRate),
            static_cast<size_t>(m_channels),
            options
        );

        // Configure for current settings
        UpdateStretcherParams();

        // Set max process size
        m_stretcher->setMaxProcessSize(DECODE_BLOCK_SIZE);

        // Pre-fill with silence to handle latency
        size_t startPad = m_stretcher->getPreferredStartPad();
        if (startPad > 0) {
            std::vector<float> silence(startPad, 0.0f);
            for (int ch = 0; ch < m_channels; ch++) {
                m_channelIn[ch] = silence;
                m_deinterleavedIn[ch] = m_channelIn[ch].data();
            }
            m_stretcher->process(m_deinterleavedIn.data(), startPad, false);
        }

        // Create output stream with same format as source (no DECODE flag - this is the playback stream)
        m_outputStream = BASS_StreamCreate(
            static_cast<DWORD>(m_sampleRate),
            m_channels,
            BASS_SAMPLE_FLOAT,
            StreamProc,
            this
        );

        if (!m_outputStream) {
            m_stretcher.reset();
            return 0;
        }

        return m_outputStream;
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_outputStream) {
            BASS_StreamFree(m_outputStream);
            m_outputStream = 0;
        }
        // Note: don't free m_sourceStream - it's owned by caller
        m_stretcher.reset();
        m_sourceStream = 0;
        m_outputQueue.clear();
    }

    void SetTempo(float tempoPercent) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tempo = tempoPercent;
        UpdateStretcherParams();
    }

    void SetPitch(float semitones) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pitch = semitones;
        UpdateStretcherParams();
    }

    void SetRate(float rate) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_rate = rate;
        UpdateStretcherParams();
    }

    float GetTempo() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tempo;
    }
    float GetPitch() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pitch;
    }
    float GetRate() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_rate;
    }
    bool IsActive() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stretcher != nullptr;
    }
    TempoAlgorithm GetAlgorithm() const override { return m_algorithm; }

    double GetLength() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sourceStream) return 0.0;
        QWORD bytes = BASS_ChannelGetLength(m_sourceStream, BASS_POS_BYTE);
        if (bytes == (QWORD)-1) return 0.0;
        return BASS_ChannelBytes2Seconds(m_sourceStream, bytes);
    }

    double GetPosition() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sourceStream) return 0.0;
        QWORD bytes = BASS_ChannelGetPosition(m_sourceStream, BASS_POS_BYTE);
        if (bytes == (QWORD)-1) return 0.0;
        return BASS_ChannelBytes2Seconds(m_sourceStream, bytes);
    }

    void SetPosition(double seconds) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sourceStream || !m_stretcher) return;

        // Seek the source stream with flush for immediate response
        QWORD bytes = BASS_ChannelSeconds2Bytes(m_sourceStream, seconds);
        BASS_ChannelSetPosition(m_sourceStream, bytes, BASS_POS_BYTE | BASS_POS_FLUSH);

        // Reset Rubber Band state and clear buffers
        m_stretcher->reset();
        m_outputQueue.clear();
        m_sourceEnded = false;

        // Re-apply settings after reset
        double timeRatio = TempoToTimeRatio(m_tempo, m_rate);
        double pitchScale = SemitonesToPitchScale(m_pitch);
        m_stretcher->setTimeRatio(timeRatio);
        m_stretcher->setPitchScale(pitchScale);
    }

    HSTREAM GetSourceStream() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sourceStream;
    }

private:
    void UpdateStretcherParams() {
        // Note: caller must hold m_mutex
        if (!m_stretcher) return;

        double timeRatio = TempoToTimeRatio(m_tempo, m_rate);
        double pitchScale = SemitonesToPitchScale(m_pitch);

        m_stretcher->setTimeRatio(timeRatio);
        m_stretcher->setPitchScale(pitchScale);
    }
};

#endif // USE_RUBBERBAND

// ============================================================================
// Speedy (Google) Implementation - Push-based stream approach
// ============================================================================
#ifdef USE_SPEEDY

class SpeedyProcessor : public TempoProcessor {
private:
    HSTREAM m_sourceStream = 0;
    HSTREAM m_outputStream = 0;
    sonicStream m_sonicStream = nullptr;
    float m_sampleRate = 44100.0f;
    int m_channels = 2;
    float m_tempo = 0.0f;   // percentage
    float m_pitch = 0.0f;   // semitones
    float m_rate = 1.0f;    // multiplier
    bool m_sourceEnded = false;
    bool m_nonlinearEnabled = true;  // Use Speedy's nonlinear speedup

    mutable std::mutex m_mutex;

    // Buffers
    std::vector<float> m_decodeBuffer;
    std::deque<float> m_outputQueue;

    static constexpr size_t DECODE_BLOCK_SIZE = 2048;
    static constexpr size_t MAX_OUTPUT_QUEUE = 65536;

    // Convert tempo percentage to speed multiplier
    float TempoToSpeed() const {
        float speed = (100.0f + m_tempo) / 100.0f * m_rate;
        if (speed < 0.1f) speed = 0.1f;
        if (speed > 6.0f) speed = 6.0f;  // Speedy supports up to 6X
        return speed;
    }

    // Convert semitones to pitch multiplier
    float SemitonesToPitch() const {
        return powf(2.0f, m_pitch / 12.0f);
    }

    // Process more audio from source through Speedy
    bool ProcessMoreAudio() {
        if (m_sourceEnded || !m_sonicStream) return false;

        // Decode a block from source
        DWORD bytesNeeded = DECODE_BLOCK_SIZE * m_channels * sizeof(float);
        m_decodeBuffer.resize(DECODE_BLOCK_SIZE * m_channels);

        DWORD bytesRead = BASS_ChannelGetData(m_sourceStream, m_decodeBuffer.data(),
            bytesNeeded | BASS_DATA_FLOAT);

        if (bytesRead == (DWORD)-1 || bytesRead == 0) {
            m_sourceEnded = true;
            sonicFlushStream(m_sonicStream);
            // Read any remaining output
            std::vector<float> tempOut(4096 * m_channels);
            int samplesRead;
            while ((samplesRead = sonicReadFloatFromStream(m_sonicStream, tempOut.data(), 4096)) > 0) {
                for (int i = 0; i < samplesRead * m_channels; i++) {
                    m_outputQueue.push_back(tempOut[i]);
                }
            }
            return false;
        }

        int samplesDecoded = bytesRead / sizeof(float) / m_channels;

        // Write to Speedy
        sonicWriteFloatToStream(m_sonicStream, m_decodeBuffer.data(), samplesDecoded);

        // Read processed output
        std::vector<float> tempOut(4096 * m_channels);
        int samplesRead;
        while ((samplesRead = sonicReadFloatFromStream(m_sonicStream, tempOut.data(), 4096)) > 0) {
            for (int i = 0; i < samplesRead * m_channels; i++) {
                m_outputQueue.push_back(tempOut[i]);
            }
        }

        return true;
    }

    // BASS stream callback
    static DWORD CALLBACK StreamProc(HSTREAM handle, void* buffer, DWORD length, void* user) {
        SpeedyProcessor* proc = static_cast<SpeedyProcessor*>(user);
        if (!proc) return BASS_STREAMPROC_END;

        std::lock_guard<std::mutex> lock(proc->m_mutex);

        float* outBuf = static_cast<float*>(buffer);
        DWORD samplesNeeded = length / sizeof(float);
        DWORD samplesWritten = 0;

        while (samplesWritten < samplesNeeded) {
            if (!proc->m_outputQueue.empty()) {
                size_t canCopy = std::min((size_t)(samplesNeeded - samplesWritten),
                                         proc->m_outputQueue.size());
                for (size_t i = 0; i < canCopy; i++) {
                    outBuf[samplesWritten++] = proc->m_outputQueue.front();
                    proc->m_outputQueue.pop_front();
                }
            } else if (!proc->m_sourceEnded) {
                if (!proc->ProcessMoreAudio()) {
                    if (proc->m_outputQueue.empty()) break;
                }
            } else {
                break;
            }
        }

        while (samplesWritten < samplesNeeded) {
            outBuf[samplesWritten++] = 0.0f;
        }

        if (proc->m_sourceEnded && proc->m_outputQueue.empty()) {
            return samplesWritten * sizeof(float) | BASS_STREAMPROC_END;
        }

        return samplesWritten * sizeof(float);
    }

public:
    SpeedyProcessor() = default;

    ~SpeedyProcessor() override {
        Shutdown();
    }

    HSTREAM Initialize(HSTREAM sourceStream, float sampleRate) override {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_sourceStream = sourceStream;
        m_sampleRate = sampleRate;
        m_sourceEnded = false;
        m_outputQueue.clear();
        m_nonlinearEnabled = g_speedyNonlinear;  // Use global setting

        // Get channel info
        BASS_CHANNELINFO info;
        if (!BASS_ChannelGetInfo(sourceStream, &info)) {
            return 0;
        }
        m_channels = info.chans;

        // Create Speedy/Sonic stream
        m_sonicStream = sonicCreateStream(static_cast<int>(m_sampleRate), m_channels);
        if (!m_sonicStream) {
            return 0;
        }

        // Enable Speedy's nonlinear speedup for speech
        if (m_nonlinearEnabled) {
            sonicEnableNonlinearSpeedup(m_sonicStream, 1.0f);
        }

        // Apply current settings
        UpdateSonicParams();

        // Create output stream
        m_outputStream = BASS_StreamCreate(
            static_cast<DWORD>(m_sampleRate),
            m_channels,
            BASS_SAMPLE_FLOAT,
            StreamProc,
            this
        );

        if (!m_outputStream) {
            sonicDestroyStream(m_sonicStream);
            m_sonicStream = nullptr;
            return 0;
        }

        return m_outputStream;
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_outputStream) {
            BASS_StreamFree(m_outputStream);
            m_outputStream = 0;
        }
        if (m_sonicStream) {
            sonicDestroyStream(m_sonicStream);
            m_sonicStream = nullptr;
        }
        m_sourceStream = 0;
        m_outputQueue.clear();
    }

    void SetTempo(float tempoPercent) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tempo = tempoPercent;
        UpdateSonicParams();
    }

    void SetPitch(float semitones) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pitch = semitones;
        UpdateSonicParams();
    }

    void SetRate(float rate) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_rate = rate;
        UpdateSonicParams();
    }

    float GetTempo() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tempo;
    }
    float GetPitch() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pitch;
    }
    float GetRate() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_rate;
    }
    bool IsActive() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sonicStream != nullptr;
    }
    TempoAlgorithm GetAlgorithm() const override { return TempoAlgorithm::Speedy; }

    double GetLength() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sourceStream) return 0.0;
        QWORD bytes = BASS_ChannelGetLength(m_sourceStream, BASS_POS_BYTE);
        if (bytes == (QWORD)-1) return 0.0;
        return BASS_ChannelBytes2Seconds(m_sourceStream, bytes);
    }

    double GetPosition() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sourceStream) return 0.0;
        QWORD bytes = BASS_ChannelGetPosition(m_sourceStream, BASS_POS_BYTE);
        if (bytes == (QWORD)-1) return 0.0;
        return BASS_ChannelBytes2Seconds(m_sourceStream, bytes);
    }

    void SetPosition(double seconds) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sourceStream || !m_sonicStream) return;

        QWORD bytes = BASS_ChannelSeconds2Bytes(m_sourceStream, seconds);
        BASS_ChannelSetPosition(m_sourceStream, bytes, BASS_POS_BYTE | BASS_POS_FLUSH);

        // Recreate sonic stream to reset state
        sonicDestroyStream(m_sonicStream);
        m_sonicStream = sonicCreateStream(static_cast<int>(m_sampleRate), m_channels);
        if (m_sonicStream && m_nonlinearEnabled) {
            sonicEnableNonlinearSpeedup(m_sonicStream, 1.0f);
        }
        UpdateSonicParams();
        m_outputQueue.clear();
        m_sourceEnded = false;
    }

    HSTREAM GetSourceStream() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sourceStream;
    }

private:
    void UpdateSonicParams() {
        if (!m_sonicStream) return;

        float speed = TempoToSpeed();
        float pitch = SemitonesToPitch();

        sonicSetSpeed(m_sonicStream, speed);
        // sonicSetPitch is not wrapped by sonic2.h, use internal function
        sonicIntSetPitch(m_sonicStream, pitch);
    }
};

#endif // USE_SPEEDY

// ============================================================================
// Factory and Global Management
// ============================================================================
TempoProcessor* CreateTempoProcessor(TempoAlgorithm algorithm) {
    switch (algorithm) {
        case TempoAlgorithm::SoundTouch:
            return new SoundTouchProcessor();
#ifdef USE_RUBBERBAND
        case TempoAlgorithm::RubberBandR2:
        case TempoAlgorithm::RubberBandR3:
            return new RubberBandProcessor(algorithm);
#endif
#ifdef USE_SPEEDY
        case TempoAlgorithm::Speedy:
            return new SpeedyProcessor();
#endif
        default:
            // Fall back to SoundTouch if algorithm not available
            return new SoundTouchProcessor();
    }
}

TempoAlgorithm GetCurrentAlgorithm() {
    return g_algorithm;
}

void SetCurrentAlgorithm(TempoAlgorithm algorithm) {
#ifndef USE_RUBBERBAND
    // If Rubber Band not available, force SoundTouch
    if (algorithm != TempoAlgorithm::SoundTouch) {
        algorithm = TempoAlgorithm::SoundTouch;
    }
#endif
    g_algorithm = algorithm;
}

void InitTempoProcessor() {
    if (!g_tempoProcessor) {
        g_tempoProcessor.reset(CreateTempoProcessor(g_algorithm));
    }
}

void FreeTempoProcessor() {
    if (g_tempoProcessor) {
        g_tempoProcessor->Shutdown();
        g_tempoProcessor.reset();
    }
}

TempoProcessor* GetTempoProcessor() {
    if (!g_tempoProcessor) {
        InitTempoProcessor();
    }
    return g_tempoProcessor.get();
}
