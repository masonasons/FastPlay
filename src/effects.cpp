#include "effects.h"
#include "globals.h"
#include "accessibility.h"
#include "resource.h"
#include "bass_fx.h"
#include "tempo_processor.h"
#include "center_cancel.h"
#include "convolution.h"
#include <cstdio>
#include <vector>
#include <cmath>

// Clamp helper
template<typename T> T clamp_val(T val, T minVal, T maxVal) {
    if (val < minVal) return minVal;
    if (val > maxVal) return maxVal;
    return val;
}

// Parameter definitions
static const ParamDef g_paramDefs[] = {
    // Stream effects (DSPEffectType cast to -1 means not a DSP effect)
    {ParamId::Volume,      "Volume",       "%",          0.0f,   4.0f,   0.02f, 1.0f,  (DSPEffectType)-1},
    {ParamId::Pitch,       "Pitch",        " semitones", -12.0f, 12.0f,  1.0f,  0.0f,  (DSPEffectType)-1},
    {ParamId::Tempo,       "Tempo",        "%",          -50.0f, 100.0f, 5.0f,  0.0f,  (DSPEffectType)-1},
    {ParamId::Rate,        "Rate",         "x",          0.5f,   2.0f,   0.01f, 1.0f,  (DSPEffectType)-1},
    // Freeverb parameters (algorithm 1)
    {ParamId::ReverbMix,   "Reverb Mix",   "%",          0.0f,   100.0f, 5.0f,  30.0f, DSPEffectType::Reverb},
    {ParamId::ReverbRoom,  "Reverb Room",  "%",          0.0f,   100.0f, 5.0f,  50.0f, DSPEffectType::Reverb},
    {ParamId::ReverbDamp,  "Reverb Damp",  "%",          0.0f,   100.0f, 5.0f,  50.0f, DSPEffectType::Reverb},
    // DX8 Reverb parameters (algorithm 2)
    {ParamId::DX8ReverbTime,    "DX8 Reverb Time",   "ms",  1.0f,   3000.0f, 100.0f, 1000.0f, DSPEffectType::Reverb},
    {ParamId::DX8ReverbHFRatio, "DX8 HF Ratio",      "",    0.001f, 0.999f,  0.1f,   0.5f,    DSPEffectType::Reverb},
    {ParamId::DX8ReverbMix,     "DX8 Reverb Mix",    "dB",  -96.0f, 0.0f,    3.0f,   -10.0f,  DSPEffectType::Reverb},
    // I3DL2 Reverb parameters (algorithm 3)
    {ParamId::I3DL2Room,       "I3DL2 Room",        "mB",  -10000.0f, 0.0f,    500.0f, -1000.0f, DSPEffectType::Reverb},
    {ParamId::I3DL2DecayTime,  "I3DL2 Decay",       "s",   0.1f,      20.0f,   0.5f,   1.49f,    DSPEffectType::Reverb},
    {ParamId::I3DL2Diffusion,  "I3DL2 Diffusion",   "%",   0.0f,      100.0f,  5.0f,   100.0f,   DSPEffectType::Reverb},
    {ParamId::I3DL2Density,    "I3DL2 Density",     "%",   0.0f,      100.0f,  5.0f,   100.0f,   DSPEffectType::Reverb},
    // Echo parameters
    {ParamId::EchoDelay,   "Echo Delay",   "ms",         10.0f,  2000.0f, 50.0f, 300.0f, DSPEffectType::Echo},
    {ParamId::EchoFeedback,"Echo Feedback","%",          0.0f,   90.0f,  5.0f,  40.0f, DSPEffectType::Echo},
    {ParamId::EchoMix,     "Echo Mix",     "%",          0.0f,   100.0f, 5.0f,  30.0f, DSPEffectType::Echo},
    // EQ parameters (in dB)
    {ParamId::EQPreamp,    "EQ Preamp",    "dB",         -15.0f, 0.0f,   1.0f,  0.0f,  DSPEffectType::EQ},
    {ParamId::EQBass,      "EQ Bass",      "dB",         -15.0f, 15.0f,  1.0f,  0.0f,  DSPEffectType::EQ},
    {ParamId::EQMid,       "EQ Mid",       "dB",         -15.0f, 15.0f,  1.0f,  0.0f,  DSPEffectType::EQ},
    {ParamId::EQTreble,    "EQ Treble",    "dB",         -15.0f, 15.0f,  1.0f,  0.0f,  DSPEffectType::EQ},
    // Compressor parameters
    {ParamId::CompThreshold, "Comp Threshold", "dB",     -60.0f, 0.0f,   3.0f,  -20.0f, DSPEffectType::Compressor},
    {ParamId::CompRatio,     "Comp Ratio",     ":1",     1.0f,   20.0f,  1.0f,  4.0f,   DSPEffectType::Compressor},
    {ParamId::CompAttack,    "Comp Attack",    "ms",     0.01f,  500.0f, 10.0f, 20.0f,  DSPEffectType::Compressor},
    {ParamId::CompRelease,   "Comp Release",   "ms",     10.0f,  2000.0f,50.0f, 200.0f, DSPEffectType::Compressor},
    {ParamId::CompGain,      "Comp Gain",      "dB",     -20.0f, 20.0f,  1.0f,  0.0f,   DSPEffectType::Compressor},
    // Stereo width parameter (0% = mono, 100% = normal, 200% = extra wide)
    {ParamId::StereoWidth,   "Stereo Width",   "%",      0.0f,   200.0f, 10.0f, 100.0f, DSPEffectType::StereoWidth},
    // Center cancel parameter (-100% = extract center, 0% = off, +100% = cancel center)
    {ParamId::CenterCancel,  "Center Cancel",  "%",      -100.0f, 100.0f, 10.0f, 0.0f, DSPEffectType::CenterCancel},
    // Convolution reverb parameters
    {ParamId::ConvolutionMix,  "Conv Mix",   "%",      0.0f, 100.0f, 5.0f, 50.0f, DSPEffectType::Convolution},
    {ParamId::ConvolutionGain, "Conv Gain",  "dB",    -20.0f, 20.0f, 1.0f, 0.0f, DSPEffectType::Convolution},
};
static const int g_paramDefCount = sizeof(g_paramDefs) / sizeof(g_paramDefs[0]);

// DSP effect handles
static HFX g_hfxReverb = 0;
static HFX g_hfxEcho = 0;
static HFX g_hfxEQPreamp = 0;
static HFX g_hfxEQBass = 0;
static HFX g_hfxEQMid = 0;
static HFX g_hfxEQTreble = 0;
static HFX g_hfxCompressor = 0;
static HDSP g_hdspStereoWidth = 0;  // Custom DSP for stereo width
static HDSP g_hdspCenterCancel = 0; // Custom DSP for center cancel/extract
static HDSP g_hdspConvolution = 0;  // Custom DSP for convolution reverb
static HDSP g_hdspVolume = 0;       // Custom DSP for volume (runs LAST, after encoder)

// DSP effect enabled states
static bool g_dspEnabled[(int)DSPEffectType::COUNT] = {false, false, false, false, false, false, false};

// Parameter values
static float g_paramValues[(int)ParamId::COUNT];

// Current parameter index for cycling
static int g_currentParamIndex = 0;

// Initialize parameter values to defaults
bool InitEffects() {
    for (int i = 0; i < g_paramDefCount; i++) {
        g_paramValues[(int)g_paramDefs[i].id] = g_paramDefs[i].defaultValue;
    }
    // Note: g_tempo, g_pitch, g_rate are loaded from settings in LoadSettings()
    // Only set defaults if they haven't been loaded yet (all zero means uninitialized)
    // Actually, these are loaded before InitEffects, so don't overwrite them
    return true;
}

void FreeEffects() {
    RemoveDSPEffects();
    FreeCenterCancelProcessor();
}

// Helper to check if a reverb param matches current algorithm
static bool IsReverbParamForCurrentAlgorithm(ParamId id) {
    switch (id) {
        // Freeverb params (algorithm 1)
        case ParamId::ReverbMix:
        case ParamId::ReverbRoom:
        case ParamId::ReverbDamp:
            return g_reverbAlgorithm == 1;
        // DX8 Reverb params (algorithm 2)
        case ParamId::DX8ReverbTime:
        case ParamId::DX8ReverbHFRatio:
        case ParamId::DX8ReverbMix:
            return g_reverbAlgorithm == 2;
        // I3DL2 Reverb params (algorithm 3)
        case ParamId::I3DL2Room:
        case ParamId::I3DL2DecayTime:
        case ParamId::I3DL2Diffusion:
        case ParamId::I3DL2Density:
            return g_reverbAlgorithm == 3;
        default:
            return true;  // Not a reverb param
    }
}

// Build list of available parameters based on enabled stream effects and DSP effects
static std::vector<ParamId> GetAvailableParams() {
    std::vector<ParamId> params;

    for (int i = 0; i < g_paramDefCount; i++) {
        const ParamDef& def = g_paramDefs[i];

        // Check if this is a stream effect parameter
        if ((int)def.dspEffect == -1) {
            // Stream effect - check if enabled in g_effectEnabled
            int effectIdx = (int)def.id;
            if (effectIdx < 4 && g_effectEnabled[effectIdx]) {
                params.push_back(def.id);
            }
        } else if (def.dspEffect == DSPEffectType::Reverb) {
            // Reverb parameter - check algorithm and matching params
            if (g_reverbAlgorithm > 0 && IsReverbParamForCurrentAlgorithm(def.id)) {
                params.push_back(def.id);
            }
        } else {
            // Other DSP effect parameter - check if the DSP effect is enabled
            if (g_dspEnabled[(int)def.dspEffect]) {
                params.push_back(def.id);
            }
        }
    }

    return params;
}

int GetAvailableParamCount() {
    return (int)GetAvailableParams().size();
}

// Toggle stream effect (Volume=0, Pitch=1, Tempo=2, Rate=3)
void ToggleStreamEffect(int effectIndex) {
    if (effectIndex < 0 || effectIndex >= 4) return;

    g_effectEnabled[effectIndex] = !g_effectEnabled[effectIndex];

    const char* names[] = {"Volume", "Pitch", "Tempo", "Rate"};
    std::string msg = std::string(names[effectIndex]) +
                      (g_effectEnabled[effectIndex] ? " enabled" : " disabled");
    Speak(msg);
}

bool IsStreamEffectEnabled(int effectIndex) {
    if (effectIndex < 0 || effectIndex >= 4) return false;
    return g_effectEnabled[effectIndex];
}

// Toggle DSP effect
void ToggleDSPEffect(DSPEffectType type) {
    if ((int)type < 0 || (int)type >= (int)DSPEffectType::COUNT) return;

    // Reverb uses algorithm selection, not simple toggle
    if (type == DSPEffectType::Reverb) {
        // Cycle through reverb algorithms: Off -> Freeverb -> DX8 -> I3DL2 -> Off
        int newAlgo = (g_reverbAlgorithm + 1) % 4;
        SetReverbAlgorithm(newAlgo);
        const char* algoNames[] = {"Off", "Freeverb", "DX8 Reverb", "I3DL2 Reverb"};
        Speak(std::string("Reverb: ") + algoNames[newAlgo]);
        return;
    }

    bool newState = !g_dspEnabled[(int)type];
    EnableDSPEffect(type, newState);

    const char* names[] = {"Reverb", "Echo", "EQ", "Compressor", "Stereo Width", "Center Cancel"};
    std::string msg = std::string(names[(int)type]) +
                      (newState ? " enabled" : " disabled");
    Speak(msg);
}

// Set reverb algorithm (0=Off, 1=Freeverb, 2=DX8, 3=I3DL2)
void SetReverbAlgorithm(int algorithm) {
    if (algorithm < 0 || algorithm > 3) return;

    // Remove existing reverb effect if any
    if (g_hfxReverb && g_fxStream) {
        BASS_ChannelRemoveFX(g_fxStream, g_hfxReverb);
        g_hfxReverb = 0;
    }

    g_reverbAlgorithm = algorithm;

    // Apply new reverb if enabled and stream exists
    if (algorithm > 0 && g_fxStream) {
        ApplyDSPEffects();
    }
}

// Enable or disable a DSP effect
void EnableDSPEffect(DSPEffectType type, bool enable) {
    if ((int)type < 0 || (int)type >= (int)DSPEffectType::COUNT) return;

    bool wasEnabled = g_dspEnabled[(int)type];
    g_dspEnabled[(int)type] = enable;

    // If stream exists, apply/remove effect immediately
    if (g_fxStream) {
        if (enable && !wasEnabled) {
            ApplyDSPEffects();
        } else if (!enable && wasEnabled) {
            // Remove just this effect
            switch (type) {
                case DSPEffectType::Reverb:
                    if (g_hfxReverb) { BASS_ChannelRemoveFX(g_fxStream, g_hfxReverb); g_hfxReverb = 0; }
                    break;
                case DSPEffectType::Echo:
                    if (g_hfxEcho) { BASS_ChannelRemoveFX(g_fxStream, g_hfxEcho); g_hfxEcho = 0; }
                    break;
                case DSPEffectType::EQ:
                    if (g_hfxEQPreamp) { BASS_ChannelRemoveFX(g_fxStream, g_hfxEQPreamp); g_hfxEQPreamp = 0; }
                    if (g_hfxEQBass) { BASS_ChannelRemoveFX(g_fxStream, g_hfxEQBass); g_hfxEQBass = 0; }
                    if (g_hfxEQMid) { BASS_ChannelRemoveFX(g_fxStream, g_hfxEQMid); g_hfxEQMid = 0; }
                    if (g_hfxEQTreble) { BASS_ChannelRemoveFX(g_fxStream, g_hfxEQTreble); g_hfxEQTreble = 0; }
                    break;
                case DSPEffectType::Compressor:
                    if (g_hfxCompressor) { BASS_ChannelRemoveFX(g_fxStream, g_hfxCompressor); g_hfxCompressor = 0; }
                    break;
                case DSPEffectType::StereoWidth:
                    if (g_hdspStereoWidth) { BASS_ChannelRemoveDSP(g_fxStream, g_hdspStereoWidth); g_hdspStereoWidth = 0; }
                    break;
                case DSPEffectType::CenterCancel:
                    if (g_hdspCenterCancel) { BASS_ChannelRemoveDSP(g_fxStream, g_hdspCenterCancel); g_hdspCenterCancel = 0; }
                    break;
                case DSPEffectType::Convolution:
                    if (g_hdspConvolution) { BASS_ChannelRemoveDSP(g_fxStream, g_hdspConvolution); g_hdspConvolution = 0; }
                    break;
                default:
                    break;
            }
        }
    }
}

// Stereo width DSP callback - uses Mid/Side processing
// Width 0% = mono, 100% = normal stereo, 200% = extra wide
static void CALLBACK StereoWidthDSPProc(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    // Get channel info to check format
    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(channel, &info)) return;

    // Only process stereo content
    if (info.chans != 2) return;

    // Get width value (0-200, where 100 is normal)
    float width = g_paramValues[(int)ParamId::StereoWidth] / 100.0f;

    // Check if float format (BASS_FX tempo streams use float)
    if (info.flags & BASS_SAMPLE_FLOAT) {
        float* samples = static_cast<float*>(buffer);
        DWORD frameCount = length / (sizeof(float) * 2);  // 2 channels

        for (DWORD i = 0; i < frameCount; i++) {
            float left = samples[i * 2];
            float right = samples[i * 2 + 1];

            // Convert to Mid/Side
            float mid = (left + right) * 0.5f;
            float side = (left - right) * 0.5f;

            // Apply width to side signal
            side *= width;

            // Convert back to Left/Right
            samples[i * 2] = mid + side;
            samples[i * 2 + 1] = mid - side;
        }
    } else {
        // 16-bit format
        short* samples = static_cast<short*>(buffer);
        DWORD frameCount = length / (sizeof(short) * 2);  // 2 channels

        for (DWORD i = 0; i < frameCount; i++) {
            float left = samples[i * 2] / 32768.0f;
            float right = samples[i * 2 + 1] / 32768.0f;

            // Convert to Mid/Side
            float mid = (left + right) * 0.5f;
            float side = (left - right) * 0.5f;

            // Apply width to side signal
            side *= width;

            // Convert back to Left/Right and clamp
            float outL = mid + side;
            float outR = mid - side;
            if (outL > 1.0f) outL = 1.0f; else if (outL < -1.0f) outL = -1.0f;
            if (outR > 1.0f) outR = 1.0f; else if (outR < -1.0f) outR = -1.0f;

            samples[i * 2] = static_cast<short>(outL * 32767.0f);
            samples[i * 2 + 1] = static_cast<short>(outR * 32767.0f);
        }
    }
}

// Center cancel/extract DSP callback - FFT-based spectral processing
// -100% = extract center (isolate vocals), 0% = no effect, +100% = cancel center (remove vocals)
static void CALLBACK CenterCancelDSPProc(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    // Get channel info to check format
    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(channel, &info)) return;

    // Only process stereo content
    if (info.chans != 2) return;

    // Get center cancel value (-100 to +100, where 0 is no effect)
    float amount = g_paramValues[(int)ParamId::CenterCancel] / 100.0f;

    // Get or initialize the processor
    CenterCancelProcessor* processor = GetCenterCancelProcessor();
    if (!processor) {
        InitCenterCancelProcessor((int)info.freq);
        processor = GetCenterCancelProcessor();
    }
    if (!processor || !processor->IsInitialized()) return;

    // Update the amount
    processor->SetAmount(amount);

    // If amount is 0, processor will passthrough
    if (info.flags & BASS_SAMPLE_FLOAT) {
        float* samples = static_cast<float*>(buffer);
        int frameCount = length / (sizeof(float) * 2);
        int outputFrames = 0;

        // Process in-place
        std::vector<float> tempOut(frameCount * 2);
        processor->ProcessFloat(samples, frameCount, tempOut.data(), outputFrames);

        // Copy output back (outputFrames should equal frameCount for steady-state)
        for (int i = 0; i < outputFrames * 2; i++) {
            samples[i] = tempOut[i];
        }
    } else {
        // 16-bit format
        short* samples = static_cast<short*>(buffer);
        int frameCount = length / (sizeof(short) * 2);
        int outputFrames = 0;

        std::vector<short> tempOut(frameCount * 2);
        processor->ProcessInt16(samples, frameCount, tempOut.data(), outputFrames);

        for (int i = 0; i < outputFrames * 2; i++) {
            samples[i] = tempOut[i];
        }
    }
}

// Convolution reverb DSP callback
static void CALLBACK ConvolutionDSPProc(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(channel, &info)) return;

    // Only process stereo content
    if (info.chans != 2) return;

    ConvolutionReverb* conv = GetConvolutionReverb();
    if (!conv) return;

    // Initialize if IR is loaded but not yet initialized
    if (conv->IsLoaded() && !conv->IsInitialized()) {
        conv->Init((int)info.freq);
    }

    conv->SetMix(g_paramValues[(int)ParamId::ConvolutionMix]);
    conv->SetGain(g_paramValues[(int)ParamId::ConvolutionGain]);

    // Handle both float and 16-bit formats
    if (info.flags & BASS_SAMPLE_FLOAT) {
        float* samples = static_cast<float*>(buffer);
        int frameCount = length / (sizeof(float) * 2);
        conv->Process(samples, frameCount);
    } else {
        // Convert 16-bit to float, process, convert back
        short* samples = static_cast<short*>(buffer);
        int frameCount = length / (sizeof(short) * 2);

        std::vector<float> floatBuf(frameCount * 2);
        for (int i = 0; i < frameCount * 2; i++) {
            floatBuf[i] = samples[i] / 32768.0f;
        }

        conv->Process(floatBuf.data(), frameCount);

        for (int i = 0; i < frameCount * 2; i++) {
            float val = floatBuf[i];
            if (val > 1.0f) val = 1.0f;
            if (val < -1.0f) val = -1.0f;
            samples[i] = static_cast<short>(val * 32767.0f);
        }
    }
}

// Volume DSP - runs LAST (very low priority) so encoder captures full volume
// This allows recording at full volume while playback respects g_volume/g_muted
// Only used when legacy volume mode is disabled
static void CALLBACK VolumeDSPProc(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    (void)handle; (void)channel; (void)user;

    // Skip if using legacy volume (handled by BASS_ATTRIB_VOL instead)
    if (g_legacyVolume) return;

    float volume = g_muted ? 0.0f : g_volume;
    if (volume == 1.0f) return; // No processing needed at full volume

    // Apply perceptual volume curve (quadratic) to match BASS_ATTRIB_VOL behavior
    // This makes lower volumes feel more gradual and natural
    float curvedVolume = volume * volume;

    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(channel, &info)) return;

    if (info.flags & BASS_SAMPLE_FLOAT) {
        float* samples = static_cast<float*>(buffer);
        int sampleCount = length / sizeof(float);
        for (int i = 0; i < sampleCount; i++) {
            samples[i] *= curvedVolume;
        }
    } else {
        short* samples = static_cast<short*>(buffer);
        int sampleCount = length / sizeof(short);
        for (int i = 0; i < sampleCount; i++) {
            samples[i] = static_cast<short>(samples[i] * curvedVolume);
        }
    }
}

bool IsDSPEffectEnabled(DSPEffectType type) {
    if ((int)type < 0 || (int)type >= (int)DSPEffectType::COUNT) return false;
    // Reverb uses g_reverbAlgorithm instead of g_dspEnabled
    if (type == DSPEffectType::Reverb) {
        return g_reverbAlgorithm > 0;
    }
    return g_dspEnabled[(int)type];
}

// Apply DSP effects to current stream
void ApplyDSPEffects() {
    if (!g_fxStream) return;

    // Reverb (based on selected algorithm)
    if (g_reverbAlgorithm > 0 && !g_hfxReverb) {
        switch (g_reverbAlgorithm) {
            case 1:  // Freeverb
                g_hfxReverb = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_FREEVERB, 0);
                if (g_hfxReverb) {
                    BASS_BFX_FREEVERB reverb;
                    reverb.fDryMix = 1.0f - (g_paramValues[(int)ParamId::ReverbMix] / 100.0f);
                    reverb.fWetMix = g_paramValues[(int)ParamId::ReverbMix] / 100.0f * 3.0f;
                    reverb.fRoomSize = g_paramValues[(int)ParamId::ReverbRoom] / 100.0f;
                    reverb.fDamp = g_paramValues[(int)ParamId::ReverbDamp] / 100.0f;
                    reverb.fWidth = 1.0f;
                    reverb.lMode = 0;
                    reverb.lChannel = BASS_BFX_CHANALL;
                    BASS_FXSetParameters(g_hfxReverb, &reverb);
                }
                break;
            case 2:  // DX8 Reverb
                g_hfxReverb = BASS_ChannelSetFX(g_fxStream, BASS_FX_DX8_REVERB, 0);
                if (g_hfxReverb) {
                    BASS_DX8_REVERB reverb;
                    reverb.fInGain = 0.0f;  // No input gain reduction
                    reverb.fReverbMix = g_paramValues[(int)ParamId::DX8ReverbMix];
                    reverb.fReverbTime = g_paramValues[(int)ParamId::DX8ReverbTime];
                    reverb.fHighFreqRTRatio = g_paramValues[(int)ParamId::DX8ReverbHFRatio];
                    BASS_FXSetParameters(g_hfxReverb, &reverb);
                }
                break;
            case 3:  // I3DL2 Reverb
                g_hfxReverb = BASS_ChannelSetFX(g_fxStream, BASS_FX_DX8_I3DL2REVERB, 0);
                if (g_hfxReverb) {
                    BASS_DX8_I3DL2REVERB reverb;
                    reverb.lRoom = static_cast<int>(g_paramValues[(int)ParamId::I3DL2Room]);
                    reverb.lRoomHF = 0;
                    reverb.flRoomRolloffFactor = 0.0f;
                    reverb.flDecayTime = g_paramValues[(int)ParamId::I3DL2DecayTime];
                    reverb.flDecayHFRatio = 0.83f;
                    reverb.lReflections = -2602;
                    reverb.flReflectionsDelay = 0.007f;
                    reverb.lReverb = 200;
                    reverb.flReverbDelay = 0.011f;
                    reverb.flDiffusion = g_paramValues[(int)ParamId::I3DL2Diffusion];
                    reverb.flDensity = g_paramValues[(int)ParamId::I3DL2Density];
                    reverb.flHFReference = 5000.0f;
                    BASS_FXSetParameters(g_hfxReverb, &reverb);
                }
                break;
        }
    }

    // Echo
    if (g_dspEnabled[(int)DSPEffectType::Echo] && !g_hfxEcho) {
        g_hfxEcho = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_ECHO4, 0);
        if (g_hfxEcho) {
            BASS_BFX_ECHO4 echo;
            echo.fDryMix = 1.0f - (g_paramValues[(int)ParamId::EchoMix] / 100.0f);
            echo.fWetMix = g_paramValues[(int)ParamId::EchoMix] / 100.0f;
            echo.fFeedback = g_paramValues[(int)ParamId::EchoFeedback] / 100.0f;
            echo.fDelay = g_paramValues[(int)ParamId::EchoDelay] / 1000.0f;  // Convert ms to seconds
            echo.bStereo = TRUE;
            echo.lChannel = BASS_BFX_CHANALL;
            BASS_FXSetParameters(g_hfxEcho, &echo);
        }
    }

    // EQ (3-band using peaking EQ)
    if (g_dspEnabled[(int)DSPEffectType::EQ]) {
        // Preamp (gain reduction to prevent clipping)
        if (!g_hfxEQPreamp) {
            g_hfxEQPreamp = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_VOLUME, 0);
            if (g_hfxEQPreamp) {
                BASS_BFX_VOLUME vol = {0};
                vol.lChannel = BASS_BFX_CHANALL;
                // Convert dB to linear: linear = 10^(dB/20)
                vol.fVolume = powf(10.0f, g_paramValues[(int)ParamId::EQPreamp] / 20.0f);
                BASS_FXSetParameters(g_hfxEQPreamp, &vol);
            }
        }
        // Bass (60 Hz)
        if (!g_hfxEQBass) {
            g_hfxEQBass = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_PEAKEQ, 0);
            if (g_hfxEQBass) {
                BASS_BFX_PEAKEQ eq = {0};
                eq.lBand = 0;
                eq.fBandwidth = 2.5f;  // Octaves
                eq.fQ = 0.0f;
                eq.fCenter = g_eqBassFreq;  // Bass center frequency
                eq.fGain = g_paramValues[(int)ParamId::EQBass];
                eq.lChannel = BASS_BFX_CHANALL;
                BASS_FXSetParameters(g_hfxEQBass, &eq);
            }
        }
        // Mid (1000 Hz)
        if (!g_hfxEQMid) {
            g_hfxEQMid = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_PEAKEQ, 0);
            if (g_hfxEQMid) {
                BASS_BFX_PEAKEQ eq = {0};
                eq.lBand = 0;
                eq.fBandwidth = 2.5f;  // Octaves
                eq.fQ = 0.0f;
                eq.fCenter = g_eqMidFreq;   // Mid center frequency
                eq.fGain = g_paramValues[(int)ParamId::EQMid];
                eq.lChannel = BASS_BFX_CHANALL;
                BASS_FXSetParameters(g_hfxEQMid, &eq);
            }
        }
        // Treble (8000 Hz)
        if (!g_hfxEQTreble) {
            g_hfxEQTreble = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_PEAKEQ, 0);
            if (g_hfxEQTreble) {
                BASS_BFX_PEAKEQ eq = {0};
                eq.lBand = 0;
                eq.fBandwidth = 2.5f;  // Octaves
                eq.fQ = 0.0f;
                eq.fCenter = g_eqTrebleFreq; // Treble center frequency
                eq.fGain = g_paramValues[(int)ParamId::EQTreble];
                eq.lChannel = BASS_BFX_CHANALL;
                BASS_FXSetParameters(g_hfxEQTreble, &eq);
            }
        }
    }

    // Compressor
    if (g_dspEnabled[(int)DSPEffectType::Compressor] && !g_hfxCompressor) {
        g_hfxCompressor = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_COMPRESSOR2, 0);
        if (g_hfxCompressor) {
            BASS_BFX_COMPRESSOR2 comp = {0};
            comp.fGain = g_paramValues[(int)ParamId::CompGain];
            comp.fThreshold = g_paramValues[(int)ParamId::CompThreshold];
            comp.fRatio = g_paramValues[(int)ParamId::CompRatio];
            comp.fAttack = g_paramValues[(int)ParamId::CompAttack];
            comp.fRelease = g_paramValues[(int)ParamId::CompRelease];
            comp.lChannel = BASS_BFX_CHANALL;
            BASS_FXSetParameters(g_hfxCompressor, &comp);
        }
    }

    // Stereo Width (custom DSP)
    if (g_dspEnabled[(int)DSPEffectType::StereoWidth] && !g_hdspStereoWidth) {
        g_hdspStereoWidth = BASS_ChannelSetDSP(g_fxStream, StereoWidthDSPProc, nullptr, 0);
    }

    // Center Cancel/Extract (custom DSP)
    if (g_dspEnabled[(int)DSPEffectType::CenterCancel] && !g_hdspCenterCancel) {
        g_hdspCenterCancel = BASS_ChannelSetDSP(g_fxStream, CenterCancelDSPProc, nullptr, 0);
    }

    // Convolution Reverb (custom DSP)
    if (g_dspEnabled[(int)DSPEffectType::Convolution] && !g_hdspConvolution) {
        ConvolutionReverb* conv = GetConvolutionReverb();
        if (conv && conv->IsLoaded()) {
            BASS_CHANNELINFO info;
            if (BASS_ChannelGetInfo(g_fxStream, &info)) {
                conv->Init((int)info.freq);
            }
        }
        g_hdspConvolution = BASS_ChannelSetDSP(g_fxStream, ConvolutionDSPProc, nullptr, 0);
    }

    // Legacy volume mode - apply volume directly to stream attribute
    // This must be done every time a new stream is created
    if (g_legacyVolume) {
        float curvedVolume = g_muted ? 0.0f : (g_volume * g_volume);
        BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, curvedVolume);
    }

    // Volume DSP - added with very low priority so it runs LAST (after encoder)
    // This ensures encoders capture full-volume audio, while playback is adjusted
    // Only used when legacy volume mode is disabled
    // Priority -2000000000 ensures it runs after encoder (priority 0)
    if (!g_legacyVolume && !g_hdspVolume) {
        g_hdspVolume = BASS_ChannelSetDSP(g_fxStream, VolumeDSPProc, nullptr, -2000000000);
    }
}

// Remove all DSP effects from stream
void RemoveDSPEffects() {
    if (g_hfxReverb) { if (g_fxStream) BASS_ChannelRemoveFX(g_fxStream, g_hfxReverb); g_hfxReverb = 0; }
    if (g_hfxEcho) { if (g_fxStream) BASS_ChannelRemoveFX(g_fxStream, g_hfxEcho); g_hfxEcho = 0; }
    if (g_hfxEQPreamp) { if (g_fxStream) BASS_ChannelRemoveFX(g_fxStream, g_hfxEQPreamp); g_hfxEQPreamp = 0; }
    if (g_hfxEQBass) { if (g_fxStream) BASS_ChannelRemoveFX(g_fxStream, g_hfxEQBass); g_hfxEQBass = 0; }
    if (g_hfxEQMid) { if (g_fxStream) BASS_ChannelRemoveFX(g_fxStream, g_hfxEQMid); g_hfxEQMid = 0; }
    if (g_hfxEQTreble) { if (g_fxStream) BASS_ChannelRemoveFX(g_fxStream, g_hfxEQTreble); g_hfxEQTreble = 0; }
    if (g_hfxCompressor) { if (g_fxStream) BASS_ChannelRemoveFX(g_fxStream, g_hfxCompressor); g_hfxCompressor = 0; }
    if (g_hdspStereoWidth) { if (g_fxStream) BASS_ChannelRemoveDSP(g_fxStream, g_hdspStereoWidth); g_hdspStereoWidth = 0; }
    if (g_hdspCenterCancel) { if (g_fxStream) BASS_ChannelRemoveDSP(g_fxStream, g_hdspCenterCancel); g_hdspCenterCancel = 0; }
    if (g_hdspConvolution) { if (g_fxStream) BASS_ChannelRemoveDSP(g_fxStream, g_hdspConvolution); g_hdspConvolution = 0; }
    if (g_hdspVolume) { if (g_fxStream) BASS_ChannelRemoveDSP(g_fxStream, g_hdspVolume); g_hdspVolume = 0; }
}

// Get parameter definition
const ParamDef* GetParamDef(ParamId id) {
    for (int i = 0; i < g_paramDefCount; i++) {
        if (g_paramDefs[i].id == id) return &g_paramDefs[i];
    }
    return nullptr;
}

float GetParamValue(ParamId id) {
    // For stream effects, return the actual global values
    switch (id) {
        case ParamId::Volume: return g_volume;
        case ParamId::Pitch: return g_pitch;
        case ParamId::Tempo: return g_tempo;
        case ParamId::Rate: return g_rate;
        default: break;
    }
    // For DSP effect parameters, use the stored values
    if ((int)id < 0 || (int)id >= (int)ParamId::COUNT) return 0.0f;
    return g_paramValues[(int)id];
}

const char* GetParamName(ParamId id) {
    const ParamDef* def = GetParamDef(id);
    return def ? def->name : "Unknown";
}

const char* GetParamUnit(ParamId id) {
    const ParamDef* def = GetParamDef(id);
    return def ? def->unit : "";
}

void SetParamValue(ParamId id, float value) {
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // For Volume, respect the allow amplify setting
    float maxVal = def->maxValue;
    if (id == ParamId::Volume) {
        maxVal = g_allowAmplify ? MAX_VOLUME_AMPLIFY : MAX_VOLUME_NORMAL;
    }

    value = clamp_val(value, def->minValue, maxVal);
    g_paramValues[(int)id] = value;

    // Apply the change
    switch (id) {
        case ParamId::Volume:
            g_volume = value;
            // In legacy mode, apply via BASS_ATTRIB_VOL
            // In normal mode, volume DSP automatically uses updated g_volume
            if (g_legacyVolume && g_fxStream) {
                float curvedVolume = g_muted ? 0.0f : (g_volume * g_volume);
                BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, curvedVolume);
            }
            break;
        case ParamId::Pitch:
            g_pitch = value;
            if (g_fxStream) {
                TempoProcessor* processor = GetTempoProcessor();
                if (processor && processor->IsActive()) {
                    processor->SetPitch(g_pitch);
                }
            }
            break;
        case ParamId::Tempo:
            g_tempo = value;
            // Skip applying tempo to live streams (not supported)
            if (g_fxStream && !g_isLiveStream) {
                TempoProcessor* processor = GetTempoProcessor();
                if (processor && processor->IsActive()) {
                    processor->SetTempo(g_tempo);
                }
            }
            break;
        case ParamId::Rate:
            g_rate = value;
            // Skip applying rate to live streams (not supported)
            if (g_fxStream && !g_isLiveStream) {
                // Use native BASS frequency attribute (changes speed and pitch together)
                BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_FREQ, g_originalFreq * g_rate);
            }
            break;
        // Freeverb parameters
        case ParamId::ReverbMix:
        case ParamId::ReverbRoom:
        case ParamId::ReverbDamp:
            if (g_hfxReverb && g_reverbAlgorithm == 1) {
                BASS_BFX_FREEVERB reverb;
                BASS_FXGetParameters(g_hfxReverb, &reverb);
                reverb.fDryMix = 1.0f - (g_paramValues[(int)ParamId::ReverbMix] / 100.0f);
                reverb.fWetMix = g_paramValues[(int)ParamId::ReverbMix] / 100.0f * 3.0f;
                reverb.fRoomSize = g_paramValues[(int)ParamId::ReverbRoom] / 100.0f;
                reverb.fDamp = g_paramValues[(int)ParamId::ReverbDamp] / 100.0f;
                BASS_FXSetParameters(g_hfxReverb, &reverb);
            }
            break;
        // DX8 Reverb parameters
        case ParamId::DX8ReverbTime:
        case ParamId::DX8ReverbHFRatio:
        case ParamId::DX8ReverbMix:
            if (g_hfxReverb && g_reverbAlgorithm == 2) {
                BASS_DX8_REVERB reverb;
                BASS_FXGetParameters(g_hfxReverb, &reverb);
                reverb.fReverbMix = g_paramValues[(int)ParamId::DX8ReverbMix];
                reverb.fReverbTime = g_paramValues[(int)ParamId::DX8ReverbTime];
                reverb.fHighFreqRTRatio = g_paramValues[(int)ParamId::DX8ReverbHFRatio];
                BASS_FXSetParameters(g_hfxReverb, &reverb);
            }
            break;
        // I3DL2 Reverb parameters
        case ParamId::I3DL2Room:
        case ParamId::I3DL2DecayTime:
        case ParamId::I3DL2Diffusion:
        case ParamId::I3DL2Density:
            if (g_hfxReverb && g_reverbAlgorithm == 3) {
                BASS_DX8_I3DL2REVERB reverb;
                BASS_FXGetParameters(g_hfxReverb, &reverb);
                reverb.lRoom = static_cast<int>(g_paramValues[(int)ParamId::I3DL2Room]);
                reverb.flDecayTime = g_paramValues[(int)ParamId::I3DL2DecayTime];
                reverb.flDiffusion = g_paramValues[(int)ParamId::I3DL2Diffusion];
                reverb.flDensity = g_paramValues[(int)ParamId::I3DL2Density];
                BASS_FXSetParameters(g_hfxReverb, &reverb);
            }
            break;
        case ParamId::EchoDelay:
        case ParamId::EchoFeedback:
        case ParamId::EchoMix:
            if (g_hfxEcho) {
                BASS_BFX_ECHO4 echo;
                BASS_FXGetParameters(g_hfxEcho, &echo);
                echo.fDryMix = 1.0f - (g_paramValues[(int)ParamId::EchoMix] / 100.0f);
                echo.fWetMix = g_paramValues[(int)ParamId::EchoMix] / 100.0f;
                echo.fFeedback = g_paramValues[(int)ParamId::EchoFeedback] / 100.0f;
                echo.fDelay = g_paramValues[(int)ParamId::EchoDelay] / 1000.0f;
                BASS_FXSetParameters(g_hfxEcho, &echo);
            }
            break;
        case ParamId::EQPreamp:
            if (g_hfxEQPreamp) {
                BASS_BFX_VOLUME vol = {0};
                vol.lChannel = BASS_BFX_CHANALL;
                vol.fVolume = powf(10.0f, value / 20.0f);
                BASS_FXSetParameters(g_hfxEQPreamp, &vol);
            }
            break;
        case ParamId::EQBass:
            if (g_hfxEQBass) {
                BASS_BFX_PEAKEQ eq = {0};
                eq.lBand = 0;
                BASS_FXGetParameters(g_hfxEQBass, &eq);
                eq.fGain = value;
                BASS_FXSetParameters(g_hfxEQBass, &eq);
            }
            break;
        case ParamId::EQMid:
            if (g_hfxEQMid) {
                BASS_BFX_PEAKEQ eq = {0};
                eq.lBand = 0;
                BASS_FXGetParameters(g_hfxEQMid, &eq);
                eq.fGain = value;
                BASS_FXSetParameters(g_hfxEQMid, &eq);
            }
            break;
        case ParamId::EQTreble:
            if (g_hfxEQTreble) {
                BASS_BFX_PEAKEQ eq = {0};
                eq.lBand = 0;
                BASS_FXGetParameters(g_hfxEQTreble, &eq);
                eq.fGain = value;
                BASS_FXSetParameters(g_hfxEQTreble, &eq);
            }
            break;
        case ParamId::CompThreshold:
        case ParamId::CompRatio:
        case ParamId::CompAttack:
        case ParamId::CompRelease:
        case ParamId::CompGain:
            if (g_hfxCompressor) {
                BASS_BFX_COMPRESSOR2 comp = {0};
                BASS_FXGetParameters(g_hfxCompressor, &comp);
                comp.fThreshold = g_paramValues[(int)ParamId::CompThreshold];
                comp.fRatio = g_paramValues[(int)ParamId::CompRatio];
                comp.fAttack = g_paramValues[(int)ParamId::CompAttack];
                comp.fRelease = g_paramValues[(int)ParamId::CompRelease];
                comp.fGain = g_paramValues[(int)ParamId::CompGain];
                BASS_FXSetParameters(g_hfxCompressor, &comp);
            }
            break;
        default:
            break;
    }
}

void CycleParam(int direction) {
    std::vector<ParamId> params = GetAvailableParams();

    if (params.empty()) {
        Speak("No parameters available");
        return;
    }

    // Find current param in available list
    int currentIdx = -1;
    for (int i = 0; i < (int)params.size(); i++) {
        if ((int)params[i] == g_currentParamIndex) {
            currentIdx = i;
            break;
        }
    }

    // If current not found, start at beginning
    if (currentIdx < 0) {
        currentIdx = 0;
    } else {
        currentIdx += direction;
        // Clamp to bounds (no wrapping)
        if (currentIdx < 0) currentIdx = 0;
        if (currentIdx >= (int)params.size()) currentIdx = (int)params.size() - 1;
    }

    g_currentParamIndex = (int)params[currentIdx];
    AnnounceCurrentParam();
}

void AdjustCurrentParam(int direction) {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // Block tempo and rate adjustments for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak("Not available for live streams");
        return;
    }

    // For Volume, respect the allow amplify setting and use g_volumeStep
    float maxVal = def->maxValue;
    float step = def->step;
    if (id == ParamId::Volume) {
        maxVal = g_allowAmplify ? MAX_VOLUME_AMPLIFY : MAX_VOLUME_NORMAL;
        step = g_volumeStep;  // Use configurable volume step
    }

    float currentVal = GetParamValue(id);
    float newVal;

    // Handle Rate with semitone stepping if enabled
    if (id == ParamId::Rate && g_rateStepMode == 1) {
        // Semitone ratio = 2^(1/12) ≈ 1.0594630943592953
        const float semitoneRatio = 1.0594630943592953f;
        if (direction > 0) {
            newVal = currentVal * semitoneRatio;
        } else {
            newVal = currentVal / semitoneRatio;
        }
    } else {
        newVal = currentVal + (direction * step);
    }
    newVal = clamp_val(newVal, def->minValue, maxVal);

    SetParamValue(id, newVal);
    AnnounceCurrentParam();
}

void ResetCurrentParam() {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // Block tempo and rate reset for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak("Not available for live streams");
        return;
    }

    SetParamValue(id, def->defaultValue);
    AnnounceCurrentParam();
}

void SetCurrentParamToMin() {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // Block tempo and rate min for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak("Not available for live streams");
        return;
    }

    SetParamValue(id, def->minValue);
    AnnounceCurrentParam();
}

void SetCurrentParamToMax() {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // Block tempo and rate max for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak("Not available for live streams");
        return;
    }

    // Use maxValue, but respect g_allowAmplify for volume
    float maxVal = def->maxValue;
    if (id == ParamId::Volume && !g_allowAmplify && maxVal > 1.0f) {
        maxVal = 1.0f;
    }

    SetParamValue(id, maxVal);
    AnnounceCurrentParam();
}

void AnnounceCurrentParam() {
    if (!g_speechEffect) return;

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    float val = GetParamValue(id);
    char buf[64];

    // Format based on parameter type
    if (id == ParamId::Volume) {
        snprintf(buf, sizeof(buf), "%s %d%s", def->name, (int)(val * 100 + 0.5f), def->unit);
    } else if (id == ParamId::Rate) {
        snprintf(buf, sizeof(buf), "%s %.2f%s", def->name, val, def->unit);
    } else if (id == ParamId::Pitch || id == ParamId::EQBass || id == ParamId::EQMid || id == ParamId::EQTreble) {
        snprintf(buf, sizeof(buf), "%s %+.0f%s", def->name, val, def->unit);
    } else if (id == ParamId::EchoDelay) {
        snprintf(buf, sizeof(buf), "%s %.0f%s", def->name, val, def->unit);
    } else {
        snprintf(buf, sizeof(buf), "%s %.0f%s", def->name, val, def->unit);
    }

    Speak(buf);
}

void ResetEffects() {
    for (int i = 0; i < g_paramDefCount; i++) {
        SetParamValue(g_paramDefs[i].id, g_paramDefs[i].defaultValue);
    }
}

// Legacy compatibility functions
float GetEffectValue(EffectType type) {
    switch (type) {
        case EffectType::Volume: return g_volume;
        case EffectType::Pitch: return g_pitch;
        case EffectType::Tempo: return g_tempo;
        case EffectType::Rate: return g_rate;
        default: return 0.0f;
    }
}

const char* GetEffectName(EffectType type) {
    switch (type) {
        case EffectType::Volume: return "Volume";
        case EffectType::Pitch: return "Pitch";
        case EffectType::Tempo: return "Tempo";
        case EffectType::Rate: return "Rate";
        default: return "Unknown";
    }
}

const char* GetEffectUnit(EffectType type) {
    switch (type) {
        case EffectType::Volume: return "%";
        case EffectType::Pitch: return " semitones";
        case EffectType::Tempo: return "%";
        case EffectType::Rate: return "x";
        default: return "";
    }
}

void SetEffectValue(EffectType type, float value) {
    switch (type) {
        case EffectType::Volume: SetParamValue(ParamId::Volume, value); break;
        case EffectType::Pitch: SetParamValue(ParamId::Pitch, value); break;
        case EffectType::Tempo: SetParamValue(ParamId::Tempo, value); break;
        case EffectType::Rate: SetParamValue(ParamId::Rate, value); break;
        default: break;
    }
}

void CycleEffect(int direction) {
    CycleParam(direction);
}

void AdjustCurrentEffect(int direction) {
    AdjustCurrentParam(direction);
}
