#pragma once
#ifndef FASTPLAY_TYPES_H
#define FASTPLAY_TYPES_H

#include <functional>
#include <string>

// File association info
struct FileAssoc {
    const wchar_t* ext;
    const wchar_t* desc;
};

// Seek amount definition
struct SeekAmount {
    double value;       // seconds or track count
    const char* label;
    bool isTrack;       // true if track-based navigation
};

// Global hotkey action
struct HotkeyAction {
    int commandId;
    const wchar_t* name;
    const wchar_t* key;  // what saved hotkeys call it: never changes
};

// A hotkey of the user's: global (works whichever program has the focus) or local
// (works in FastPlay's main window, ahead of its own shortcuts)
struct GlobalHotkey {
    int id;         // Unique ID for RegisterHotKey
    unsigned modifiers; // MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN
    unsigned vk;        // Virtual key code
    int actionIdx;  // Index into g_hotkeyActions
    bool global = true;
};

// Hotkey dialog data
struct HotkeyDlgData {
    unsigned modifiers;
    unsigned vk;
    int actionIdx;
    bool isEdit;
    bool global = true;
};

// A folder in the library (Options > Library)
struct LibraryFolder {
    std::wstring path;
    bool tagged = true;  // its songs are in the songs, artists and albums views, not only folders
};

// Stream effect types (tempo stream attributes)
// A chapter of the file playing: where it starts and its title (may be empty).
struct Chapter {
    double position;        // Position in seconds
    std::wstring name;      // Chapter name
};

// How tempo and pitch are changed. The numbers are what the settings store
// (1 and 2; 0 was SoundTouch, which is gone and now reads as Signalsmith).
enum class TempoAlgorithm {
    Speedy = 1,       // Google's Speedy: nonlinear speedup, for speech
    Signalsmith = 2,  // Signalsmith Stretch: high quality, for music
};

enum class StreamEffect {
    Volume,
    Pitch,
    Tempo,
    Rate,
    COUNT
};

// Spatial audio mode
enum class SpatialMode {
    Binaural,     // Stereo HRTF for headphones (2 virtual speakers)
    Surround51,   // 5.1 virtual surround (5 virtual speakers rendered binaurally)
    Speakers,     // a room of simulated speakers (src/speakers/presets.h), heard from a seat in it
    COUNT
};

// DSP effect types (the effects in the audio engine's chain)
enum class DSPEffectType {
    Reverb,
    Echo,
    EQ,
    Compressor,
    StereoWidth,
    CenterCancel,  // Center channel canceler/extractor (vocal removal/isolation)
    Convolution,   // Convolution reverb using impulse response
    SpatialAudio,  // 3D audio via HRTF/binaural rendering
    Normalizer,    // a steady level, with lookahead (last in the chain)
    COUNT
};

// Reverb algorithm types (see src/reverb)
enum class ReverbAlgorithm {
    Off,
    Simple,     // 16 line FDN reverb: room size, damping, width
    Advanced,   // EFX model reverb: the OpenAL EFX / EAX parameter set and environments
    COUNT
};

// All adjustable parameters (stream + DSP)
// The numeric values are stored in effect presets (Param<n>), so existing ones must not change:
// retired parameters leave gaps and new ones go at the end.
enum class ParamId {
    // Stream effects
    Volume,
    Pitch,
    Tempo,
    Rate,
    // Simple reverb parameters (the rest are after SpatialZ)
    ReverbMix,
    ReverbRoom,
    ReverbDamp,
    // 7 .. 13 were the DX8 and I3DL2 reverb parameters
    // Echo parameters
    EchoDelay = 14,
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
    // 3D audio parameters
    SpatialBlend,
    SpatialWidth,
    SpatialRotation,
    SpatialMode,        // 0=Binaural, 1=5.1 Surround, 2 and up = the room presets
    SpatialRearCenter,  // 0=Off, 1=On (5.1 only)
    SpatialX,           // Listener X position
    SpatialY,           // Listener Y position
    SpatialZ,           // Listener Z position
    // Simple reverb parameters (continued)
    ReverbPreset,       // index into the simple reverb's rooms
    ReverbWidth,
    ReverbPreDelay,
    ReverbLowCut,       // 0 = off
    ReverbHighCut,      // maximum = off
    // Advanced reverb parameters
    AdvReverbPreset,    // index into the EFX environments
    AdvReverbMix,
    AdvReverbDecay,
    AdvReverbHFRatio,
    AdvReverbDensity,
    AdvReverbDiffusion,
    AdvReverbReflections,   // dB
    AdvReverbLate,          // dB
    AdvReverbReflDelay,     // ms
    AdvReverbLateDelay,     // ms
    // 3D audio room presets (listed with the other 3D parameters)
    SpatialSub,         // 0=Off, 1=On (presets with subwoofers)
    SpatialSubLevel,    // dB
    SpatialCrossover,   // Hz
    SpatialBassFeel,    // %
    SpatialConeNoise,   // %
    SpatialBass,        // dB
    // Normalizer parameters
    NormTarget,         // dBFS
    NormLookahead,      // ms
    NormMaxGain,        // dB
    NormRelease,        // ms
    COUNT
};

// How YouTube videos are downloaded (Options > YouTube Downloads)
struct YouTubeDownloadSettings {
    std::wstring folder;        // empty: FastPlay in the Downloads folder
    int type = 0;               // 0 audio only, 1 video
    int audioFormat = 0;        // 0 M4A (AAC), 1 as YouTube has it, 2 MP3, 3 Opus, 4 FLAC, 5 WAV
    int audioQuality = 0;       // 0 best, 1 320, 2 256, 3 192, 4 128 kbps (when converting)
    int videoQuality = 0;       // 0 best, 1 2160p, 2 1440p, 3 1080p, 4 720p, 5 480p, 6 360p
    int videoContainer = 0;     // 0 MP4, 1 MKV, 2 WebM
    int videoCodec = 0;         // 0 any, 1 H.264, 2 VP9, 3 AV1
    int naming = 0;             // 0 title, 1 title [id], 2 channel - title, 3 date - title
    bool addMetadata = false;
    bool embedThumbnail = false;
    bool writeThumbnail = false;
    bool writeDescription = false;
    bool writeSubtitles = false;
    bool embedSubtitles = false;
    bool channelFolder = false; // a folder per channel
    std::wstring extraOptions;  // more yt-dlp options, as typed on a command line
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
