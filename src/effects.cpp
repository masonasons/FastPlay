#include "effects.h"
#include "effect_chain.h"
#include "ini.h"
#include "globals.h"
#include "accessibility.h"
#include "app_ui.h"
#include "audio.h"
#include "audio/basic_effects.h"
#include "player.h"
#include "center_cancel.h"
#include "convolution.h"
#include "reverb/reverb.h"
#include "reverb/efx_reverb.h"
#include "spatial_audio.h"
#include "speakers/presets.h"
#include <cwchar>
#include <cstdio>
#include <vector>
#include <cmath>
#include <mutex>
#include <algorithm>

// Clamp helper
template<typename T> T clamp_val(T val, T minVal, T maxVal) {
    if (val < minVal) return minVal;
    if (val > maxVal) return maxVal;
    return val;
}

// The effects themselves are an EffectChain (effect_chain.h) on FastPlay's
// player. This is FastPlay's side of them: the effect slider and what it says,
// presets, and the stream controls (volume, pitch, tempo, rate), which are the
// player's.
static audio::EffectChain& Chain() {
    // Never destroyed: the app ends with it in place
    static audio::EffectChain* chain = [] {
        auto* c = new audio::EffectChain();
        c->UseSpatialAudio(GetSpatialAudio());
        c->UseConvolution(GetConvolutionReverb());
        c->Attach(&audio::DefaultPlayer());
        return c;
    }();
    return *chain;
}

static const std::vector<ParamDef>& Defs() { return audio::ParamDefs(); }

// Current parameter index for cycling
static int g_currentParamIndex = 0;

// High cut at its maximum means off.
static const float kReverbHighCutOff = 20000.0f;

// A choice parameter's value by name ("Cathedral")
static std::string ChoiceName(ParamId id, float value) {
    std::vector<std::string> names = audio::ParamChoices(id);
    if (names.empty()) return std::string();
    int i = clamp_val(static_cast<int>(std::lround(value)), 0, static_cast<int>(names.size()) - 1);
    return names[static_cast<size_t>(i)];
}

// Initialize parameter values to defaults
bool InitEffects() {
    // The chain starts at the defaults; the stream controls are loaded before this
    Chain().SetEqFrequencies(g_eqBassFreq, g_eqMidFreq, g_eqTrebleFreq);
    return true;
}

void FreeEffects() {
    RemoveDSPEffects();
    Chain().UseSpatialAudio(nullptr);  // about to go
    FreeCenterCancelProcessor();
    FreeSpatialAudio();
}

// Helper to check if a reverb param matches current algorithm
static bool IsReverbParamForCurrentAlgorithm(ParamId id) {
    switch (id) {
        // Simple reverb params (algorithm 1)
        case ParamId::ReverbPreset:
        case ParamId::ReverbMix:
        case ParamId::ReverbRoom:
        case ParamId::ReverbDamp:
        case ParamId::ReverbWidth:
        case ParamId::ReverbPreDelay:
        case ParamId::ReverbLowCut:
        case ParamId::ReverbHighCut:
            return g_reverbAlgorithm == 1;
        // Advanced reverb params (algorithm 2)
        case ParamId::AdvReverbPreset:
        case ParamId::AdvReverbMix:
        case ParamId::AdvReverbDecay:
        case ParamId::AdvReverbHFRatio:
        case ParamId::AdvReverbDensity:
        case ParamId::AdvReverbDiffusion:
        case ParamId::AdvReverbReflections:
        case ParamId::AdvReverbLate:
        case ParamId::AdvReverbReflDelay:
        case ParamId::AdvReverbLateDelay:
            return g_reverbAlgorithm == 2;
        default:
            return true;  // Not a reverb param
    }
}

// Build list of available parameters based on enabled stream effects and DSP effects
// The room preset 3D Mode is on (0 and up), or -1 for Binaural and 5.1.
static int CurrentRoomPreset() {
    int mode = static_cast<int>(Chain().Get(ParamId::SpatialMode) + 0.5f);
    return mode >= 2 ? mode - 2 : -1;
}

// Whether a 3D Audio parameter means anything in the current 3D mode: width and
// the rear speaker belong to the virtual speakers of Binaural and 5.1, the
// subwoofer controls to room presets that have subs.
static bool IsSpatialParamInUse(ParamId id) {
    int preset = CurrentRoomPreset();
    switch (id) {
        case ParamId::SpatialWidth:
        case ParamId::SpatialRearCenter:
            return preset < 0;
        case ParamId::SpatialConeNoise:
        case ParamId::SpatialBass:
            return preset >= 0;
        case ParamId::SpatialSub:
        case ParamId::SpatialCrossover:
            return preset >= 0 && speakers::RoomPresetHasSub(preset);
        default:
            return true;
    }
}

static std::vector<ParamId> GetAvailableParams() {
    std::vector<ParamId> params;

    for (const ParamDef& def : Defs()) {

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
            if (Chain().Enabled(def.dspEffect) &&
                (def.dspEffect != DSPEffectType::SpatialAudio || IsSpatialParamInUse(def.id))) {
                params.push_back(def.id);
            }
        }
    }

    return params;
}

int GetAvailableParamCount() {
    return (int)GetAvailableParams().size();
}

std::vector<ParamId> GetAvailableParamIds() {
    return GetAvailableParams();
}

ParamId GetCurrentParam() {
    return (ParamId)g_currentParamIndex;
}

void SetCurrentParam(ParamId id) {
    if (GetParamDef(id)) g_currentParamIndex = (int)id;
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
        // Cycle through reverb algorithms: Off -> Simple -> Advanced -> Off
        int newAlgo = (g_reverbAlgorithm + 1) % (int)ReverbAlgorithm::COUNT;
        SetReverbAlgorithm(newAlgo);
        const char* algoNames[] = {"Off", "Simple", "Advanced"};
        Speak(std::string("Reverb: ") + algoNames[newAlgo]);
        return;
    }

    bool newState = !Chain().Enabled(type);
    EnableDSPEffect(type, newState);

    const char* names[] = {"Reverb", "Echo", "EQ", "Compressor", "Stereo Width", "Center Cancel", "Convolution", "3D Audio",
                           "Normalizer"};
    std::string msg = std::string(names[(int)type]) +
                      (newState ? " enabled" : " disabled");
    Speak(msg);
}

// Set reverb algorithm (0=Off, 1=Simple, 2=Advanced)
void SetReverbAlgorithm(int algorithm) {
    if (algorithm < 0 || algorithm >= (int)ReverbAlgorithm::COUNT) return;
    g_reverbAlgorithm = algorithm;
    Chain().SetReverbAlgorithm(algorithm);  // in at once if something is playing
}

// Enable or disable a DSP effect
void EnableDSPEffect(DSPEffectType type, bool enable) {
    if ((int)type < 0 || (int)type >= (int)DSPEffectType::COUNT) return;
    Chain().Enable(type, enable);
    // 3D audio that would not start (no HRTF at this rate) is turned off again
    if (enable && type == DSPEffectType::SpatialAudio && audio::IsLoaded() && !Chain().Enabled(type)) {
        const wchar_t* err = Chain().Spatial()->GetLastError();
        if (err && err[0]) ShowMessage(err, L"3D Audio Error", MessageIcon::Error);
    }
}

bool IsDSPEffectEnabled(DSPEffectType type) {
    if ((int)type < 0 || (int)type >= (int)DSPEffectType::COUNT) return false;
    return Chain().Enabled(type);
}

// Put the enabled effects into the player's chain, fresh, for what was just loaded.
void ApplyDSPEffects() {
    if (!audio::IsLoaded()) return;
    // What Options may have changed since
    Chain().SetEqFrequencies(g_eqBassFreq, g_eqMidFreq, g_eqTrebleFreq);
    if (Chain().ReverbAlgorithm() != g_reverbAlgorithm) Chain().SetReverbAlgorithm(g_reverbAlgorithm);
    std::wstring error;
    if (!Chain().Apply(&error) && !error.empty()) ShowMessage(error, L"3D Audio Error", MessageIcon::Error);
}

void RemoveDSPEffects() { Chain().Remove(); }

const ParamDef* GetParamDef(ParamId id) { return audio::FindParamDef(id); }

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
    return Chain().Get(id);
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

    // The stream controls are the player's; the effects' are the chain's
    switch (id) {
        case ParamId::Volume:
            g_volume = value;
            UpdateOutputGain();
            break;
        case ParamId::Pitch:
            g_pitch = value;
            audio::SetPitch(g_pitch);
            break;
        case ParamId::Tempo:
            g_tempo = value;
            audio::SetTempo(g_tempo);  // live streams keep their speed
            break;
        case ParamId::Rate:
            g_rate = value;
            audio::SetRate(g_rate);  // speed and pitch together; not for live streams
            break;
        default:
            break;
    }
    Chain().Set(id, value);
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

    // 3D Rotation, Mode, and Rear Speaker wrap around instead of clamping
    // so the user can cycle through modes or rotate continuously.
    if (id == ParamId::SpatialRotation) {
        // Angular: ±180° meet, so full range = max - min
        float range = def->maxValue - def->minValue;
        while (newVal > def->maxValue) newVal -= range;
        while (newVal < def->minValue) newVal += range;
    } else if (id == ParamId::SpatialMode || id == ParamId::SpatialRearCenter || id == ParamId::SpatialSub ||
               id == ParamId::ReverbPreset || id == ParamId::AdvReverbPreset) {
        // Discrete choice: add step so past-max wraps to min
        float range = def->maxValue - def->minValue + def->step;
        while (newVal > def->maxValue) newVal -= range;
        while (newVal < def->minValue) newVal += range;
    } else {
        newVal = clamp_val(newVal, def->minValue, maxVal);
    }

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
    std::string text = DescribeParam((ParamId)g_currentParamIndex);
    if (!text.empty()) Speak(text);
}

std::string DescribeParam(ParamId id) {
    const ParamDef* def = GetParamDef(id);
    if (!def) return std::string();

    float val = GetParamValue(id);
    char buf[64];

    // Format based on parameter type
    if (id == ParamId::SpatialMode) {
        int preset = CurrentRoomPreset();
        snprintf(buf, sizeof(buf), "3D Mode: %s",
                 preset >= 0 ? speakers::RoomPresetName(preset) : val >= 0.5f ? "5.1 Surround" : "Binaural");
    } else if (id == ParamId::SpatialRearCenter) {
        snprintf(buf, sizeof(buf), "3D Rear Speaker: %s", val >= 0.5f ? "On" : "Off");
    } else if (id == ParamId::SpatialSub) {
        snprintf(buf, sizeof(buf), "3D Subwoofer: %s", val >= 0.5f ? "On" : "Off");
    } else if (id == ParamId::SpatialBass) {
        snprintf(buf, sizeof(buf), "%s %+.0f%s", def->name, val, def->unit);
    } else if ((id == ParamId::SpatialX || id == ParamId::SpatialY || id == ParamId::SpatialZ) &&
               CurrentRoomPreset() >= 0) {
        // In a room the listener moves in tenths of a metre from the seat
        snprintf(buf, sizeof(buf), "%s %.1f metres", def->name, val * 0.1f);
    } else if (id == ParamId::ReverbPreset) {
        snprintf(buf, sizeof(buf), "%s: %s", def->name, ChoiceName(id, val).c_str());
    } else if (id == ParamId::AdvReverbPreset) {
        snprintf(buf, sizeof(buf), "%s: %s", def->name, ChoiceName(id, val).c_str());
    } else if ((id == ParamId::ReverbLowCut && val <= 0.0f) ||
               (id == ParamId::ReverbHighCut && val >= kReverbHighCutOff)) {
        snprintf(buf, sizeof(buf), "%s Off", def->name);
    } else if (id == ParamId::AdvReverbDecay) {
        snprintf(buf, sizeof(buf), "%s %.1f%s", def->name, val, def->unit);
    } else if (id == ParamId::AdvReverbHFRatio) {
        snprintf(buf, sizeof(buf), "%s %.2f%s", def->name, val, def->unit);
    } else if (id == ParamId::AdvReverbReflections || id == ParamId::AdvReverbLate) {
        snprintf(buf, sizeof(buf), "%s %+.0f%s", def->name, val, def->unit);
    } else if (id == ParamId::Volume) {
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

    return buf;
}

void ResetEffects() {
    for (const ParamDef& def : Defs()) SetParamValue(def.id, def.defaultValue);
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

// ---------------------------------------------------------------------------
// Effect presets: save/load/delete named sets of all enabled effects + params.
// Stored in FastPlay.ini under [Presets] (index) and [Preset_<name>] (values).
// ---------------------------------------------------------------------------

static std::wstring PresetSectionName(const std::wstring& name) {
    return L"Preset_" + name;
}

// A parameter's entry in a preset: by its stable name, which never moves.
static std::wstring PresetParamKey(ParamId id) {
    std::string key = audio::ParamKey(id);
    return L"Param." + std::wstring(key.begin(), key.end());
}

// Presets saved before the names were by ParamId's number, as it was then: the
// 3D sub level (54, now 3D Bass) and bass feel (56, gone) sat among the 3D
// settings, and everything after them a place or two later.
static int LegacyPresetNumber(ParamId id) {
    static_assert((int)ParamId::SpatialSub == 53 && (int)ParamId::NormRelease == 60,
                  "the numbers this maps from have moved");
    switch (id) {
        case ParamId::SpatialBass: return 54;
        case ParamId::SpatialCrossover: return 55;
        case ParamId::SpatialConeNoise: return 57;
        case ParamId::NormTarget: return 58;
        case ParamId::NormLookahead: return 59;
        case ParamId::NormMaxGain: return 60;
        case ParamId::NormRelease: return 61;
        default: return (int)id;
    }
}

static float IniGetFloatPreset(const wchar_t* section, const wchar_t* key, float defaultVal) {
    wchar_t buf[64] = {0};
    IniGetString(section, key, L"", buf, 64, g_configPath.c_str());
    if (buf[0] == L'\0') return defaultVal;
    return (float)std::wcstod(buf, nullptr);
}

std::vector<std::wstring> GetEffectPresetNames() {
    std::vector<std::wstring> names;
    int count = IniGetInt(L"Presets", L"Count", 0, g_configPath.c_str());
    for (int i = 0; i < count; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Name%d", i);
        wchar_t buf[128] = {0};
        IniGetString(L"Presets", key, L"", buf, 128, g_configPath.c_str());
        if (buf[0] != L'\0') names.push_back(buf);
    }
    return names;
}

static void WritePresetNameList(const std::vector<std::wstring>& names) {
    // Clear old name entries first
    int oldCount = IniGetInt(L"Presets", L"Count", 0, g_configPath.c_str());
    for (int i = 0; i < oldCount; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Name%d", i);
        IniWriteString(L"Presets", key, nullptr, g_configPath.c_str());
    }
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", (int)names.size());
    IniWriteString(L"Presets", L"Count", buf, g_configPath.c_str());
    for (size_t i = 0; i < names.size(); i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Name%zu", i);
        IniWriteString(L"Presets", key, names[i].c_str(), g_configPath.c_str());
    }
}

bool SaveEffectPreset(const std::wstring& name) {
    if (name.empty()) return false;

    std::wstring section = PresetSectionName(name);
    wchar_t buf[64];

    // Stream effect enabled flags
    for (int i = 0; i < 4; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"StreamEnabled%d", i);
        IniWriteString(section.c_str(), key, g_effectEnabled[i] ? L"1" : L"0", g_configPath.c_str());
    }

    // Stream effect values (pitch, tempo, rate — volume intentionally excluded)
    swprintf(buf, 64, L"%.4f", g_pitch);
    IniWriteString(section.c_str(), L"Pitch", buf, g_configPath.c_str());
    swprintf(buf, 64, L"%.4f", g_tempo);
    IniWriteString(section.c_str(), L"Tempo", buf, g_configPath.c_str());
    swprintf(buf, 64, L"%.4f", g_rate);
    IniWriteString(section.c_str(), L"Rate", buf, g_configPath.c_str());

    // Reverb algorithm
    swprintf(buf, 64, L"%d", g_reverbAlgorithm);
    IniWriteString(section.c_str(), L"ReverbAlgorithm", buf, g_configPath.c_str());

    // DSP effect enabled flags
    for (int i = 0; i < (int)DSPEffectType::COUNT; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"DSPEnabled%d", i);
        IniWriteString(section.c_str(), key,
            Chain().Enabled((DSPEffectType)i) ? L"1" : L"0", g_configPath.c_str());
    }

    // All param values (DSP params plus stream effect params)
    for (const ParamDef& def : Defs()) {
        // Skip volume - presets shouldn't hijack playback volume
        if (def.id == ParamId::Volume) continue;
        swprintf(buf, 64, L"%.6f", Chain().Get(def.id));
        IniWriteString(section.c_str(), PresetParamKey(def.id).c_str(), buf, g_configPath.c_str());
    }

    // Add to name list if not already present
    auto names = GetEffectPresetNames();
    bool found = false;
    for (auto& n : names) { if (n == name) { found = true; break; } }
    if (!found) {
        names.push_back(name);
        WritePresetNameList(names);
    }
    return true;
}

bool LoadEffectPreset(const std::wstring& name) {
    if (name.empty()) return false;
    std::wstring section = PresetSectionName(name);

    // Quick existence check
    wchar_t test[8] = {0};
    IniGetString(section.c_str(), L"Pitch", L"__MISSING__", test, 8, g_configPath.c_str());
    if (wcscmp(test, L"__MISSING__") == 0) return false;

    // Stream effect values
    wchar_t buf[64] = {0};
    IniGetString(section.c_str(), L"Pitch", L"0", buf, 64, g_configPath.c_str());
    g_pitch = (float)std::wcstod(buf, nullptr);
    IniGetString(section.c_str(), L"Tempo", L"0", buf, 64, g_configPath.c_str());
    g_tempo = (float)std::wcstod(buf, nullptr);
    IniGetString(section.c_str(), L"Rate", L"1", buf, 64, g_configPath.c_str());
    g_rate = (float)std::wcstod(buf, nullptr);

    // Stream effect enabled flags
    for (int i = 0; i < 4; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"StreamEnabled%d", i);
        g_effectEnabled[i] = IniGetInt(section.c_str(), key,
            g_effectEnabled[i] ? 1 : 0, g_configPath.c_str()) != 0;
    }

    // Reverb algorithm
    int ra = IniGetInt(section.c_str(), L"ReverbAlgorithm", g_reverbAlgorithm, g_configPath.c_str());
    if (ra < 0) ra = 0;
    if (ra >= (int)ReverbAlgorithm::COUNT) ra = (int)ReverbAlgorithm::Advanced;  // old DX8 / I3DL2
    SetReverbAlgorithm(ra);

    // DSP effect enabled flags (apply via EnableDSPEffect so handlers hook up properly)
    for (int i = 0; i < (int)DSPEffectType::COUNT; i++) {
        if ((DSPEffectType)i == DSPEffectType::Reverb) continue;  // controlled by algorithm
        wchar_t key[32];
        swprintf(key, 32, L"DSPEnabled%d", i);
        bool en = IniGetInt(section.c_str(), key,
            Chain().Enabled((DSPEffectType)i) ? 1 : 0, g_configPath.c_str()) != 0;
        EnableDSPEffect((DSPEffectType)i, en);
    }

    // All param values (via SetParamValue so effects update live)
    for (const ParamDef& def : Defs()) {
        if (def.id == ParamId::Volume) continue;
        // By name, or in a preset from before the names, by its old number
        wchar_t legacy[64];
        swprintf(legacy, 64, L"Param%d", LegacyPresetNumber(def.id));
        float val = IniGetFloatPreset(section.c_str(), legacy, Chain().Get(def.id));
        val = IniGetFloatPreset(section.c_str(), PresetParamKey(def.id).c_str(), val);
        SetParamValue(def.id, val);
    }

    return true;
}

bool DeleteEffectPreset(const std::wstring& name) {
    if (name.empty()) return false;
    std::wstring section = PresetSectionName(name);
    // Wipe the preset's entire section
    IniWriteString(section.c_str(), nullptr, nullptr, g_configPath.c_str());

    auto names = GetEffectPresetNames();
    bool found = false;
    for (auto it = names.begin(); it != names.end(); ) {
        if (*it == name) { it = names.erase(it); found = true; } else { ++it; }
    }
    if (found) WritePresetNameList(names);
    return found;
}
