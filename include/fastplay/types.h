#pragma once
#ifndef FASTPLAY_TYPES_H
#define FASTPLAY_TYPES_H

#include <windows.h>
#include <functional>
#include <string>

// File association info
struct FileAssoc {
    const wchar_t* ext;
    const wchar_t* desc;
    int ctrlId;
};

// Seek amount definition
struct SeekAmount {
    double value;       // seconds or track count
    const char* label;
    int ctrlId;
    bool isTrack;       // true if track-based navigation
};

// Global hotkey action
struct HotkeyAction {
    int commandId;
    const wchar_t* name;
};

// Global hotkey storage
struct GlobalHotkey {
    int id;         // Unique ID for RegisterHotKey
    UINT modifiers; // MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN
    UINT vk;        // Virtual key code
    int actionIdx;  // Index into g_hotkeyActions
};

// Hotkey dialog data
struct HotkeyDlgData {
    UINT modifiers;
    UINT vk;
    int actionIdx;
    bool isEdit;
};

// Stream effect types (tempo stream attributes)
enum class StreamEffect {
    Volume,
    Pitch,
    Tempo,
    Rate,
    COUNT
};

// DSP effect types (BASS_FX DSP effects + custom)
enum class DSPEffectType {
    Reverb,
    Echo,
    EQ,
    Compressor,
    StereoWidth,
    CenterCancel,  // Center channel canceler/extractor (vocal removal/isolation)
    Convolution,   // Convolution reverb using impulse response
    COUNT
};

// Reverb algorithm types
enum class ReverbAlgorithm {
    Off,
    Freeverb,   // BASS_FX_BFX_FREEVERB - Musical, simple
    DX8,        // BASS_FX_DX8_REVERB - DirectX standard
    I3DL2,      // BASS_FX_DX8_I3DL2REVERB - Environmental/3D
    COUNT
};

// All adjustable parameters (stream + DSP)
enum class ParamId {
    // Stream effects
    Volume,
    Pitch,
    Tempo,
    Rate,
    // Freeverb parameters
    ReverbMix,
    ReverbRoom,
    ReverbDamp,
    // DX8 Reverb parameters
    DX8ReverbTime,
    DX8ReverbHFRatio,
    DX8ReverbMix,
    // I3DL2 Reverb parameters
    I3DL2Room,
    I3DL2DecayTime,
    I3DL2Diffusion,
    I3DL2Density,
    // Echo parameters
    EchoDelay,
    EchoFeedback,
    EchoMix,
    // EQ parameters
    EQPreamp,
    EQBass,
    EQMid,
    EQTreble,
    // Compressor parameters
    CompThreshold,
    CompRatio,
    CompAttack,
    CompRelease,
    CompGain,
    // Stereo width parameter
    StereoWidth,
    // Center cancel parameter (-100% extract to +100% cancel)
    CenterCancel,
    // Convolution reverb parameters
    ConvolutionMix,
    ConvolutionGain,
    COUNT
};

// Parameter definition
struct ParamDef {
    ParamId id;
    const char* name;
    const char* unit;
    float minValue;
    float maxValue;
    float step;
    float defaultValue;
    DSPEffectType dspEffect;  // Which DSP effect this belongs to (or -1 for stream effects)
};

// Legacy EffectType for backwards compatibility
enum class EffectType {
    Volume,
    Pitch,
    Tempo,
    Rate
};

// Legacy EffectParam for backwards compatibility
struct EffectParam {
    EffectType type;
    float minValue;
    float maxValue;
    float step;
    float defaultValue;
};

#endif // FASTPLAY_TYPES_H
