// FastPlay's options by name, for the settings screens: the iPhone's FPSettings.mm.
// The same options as the desktop's Options window keeps, saved in the same
// settings file. Setting one applies it as the desktop's OK button does, and saves.

#include "android_jni.h"

#include "audio.h"
#include "convolution.h"
#include "effects.h"
#include "globals.h"
#include "paths.h"
#include "player.h"
#include "settings.h"
#include "utils.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <string>

using namespace fp;

#define JNI(ret, name) extern "C" JNIEXPORT ret JNICALL Java_me_masonasons_fastplay_engine_Native_##name

namespace {

struct Option {
    std::function<double()> get;
    std::function<void(double)> set;
};

template <class T>
Option Plain(T& value, std::function<void()> apply = nullptr) {
    return {[&value]() { return static_cast<double>(value); },
            [&value, apply](double v) {
                value = static_cast<T>(v);
                if (apply) apply();
            }};
}

Option Flag(bool& value, std::function<void()> apply = nullptr) {
    return {[&value]() { return value ? 1.0 : 0.0; },
            [&value, apply](double v) {
                value = v != 0;
                if (apply) apply();
            }};
}

Option Clamped(float& value, float low, float high) {
    return {[&value]() { return static_cast<double>(value); },
            [&value, low, high](double v) { value = std::clamp(static_cast<float>(v), low, high); }};
}

// A time spring or tape seeking takes, in milliseconds
Option ScrubTime(int& value) {
    return {[&value]() { return static_cast<double>(value); },
            [&value](double v) { value = std::clamp(static_cast<int>(v), kScrubTimeMinMs, kScrubTimeMaxMs); }};
}

// Every option the settings screens show, as the desktop's Options window has them.
const std::map<std::string, Option>& Options() {
    static const std::map<std::string, Option> options = {
        // Playback
        {"allowAmplify", Flag(g_allowAmplify, []() {
             if (!g_allowAmplify && g_volume > MAX_VOLUME_NORMAL) SetVolume(MAX_VOLUME_NORMAL);
         })},
        {"rememberState", Flag(g_rememberState)},
        {"rememberPosMinutes", Plain(g_rememberPosMinutes)},
        {"loadFolder", Flag(g_loadFolder)},
        {"autoAdvance", Flag(g_autoAdvance)},
        {"shuffle", Flag(g_shuffle, []() {
             if (g_shuffle) ResetShuffleOrder();
         })},
        {"repeatMode", Plain(g_repeatMode)},
        {"rewindOnPauseMs", {[]() { return static_cast<double>(g_rewindOnPauseMs); },
                             [](double v) { g_rewindOnPauseMs = std::max(0, static_cast<int>(v)); }}},
        {"volumeStepPercent", {[]() { return static_cast<double>(static_cast<int>(g_volumeStep * 100 + 0.5f)); },
                               [](double v) { g_volumeStep = static_cast<float>(v) / 100.0f; }}},
        {"replayGainMode", Plain(g_replayGainMode, []() { RefreshReplayGain(); })},
        {"replayGainPreamp", Plain(g_replayGainPreamp, []() { RefreshReplayGain(); })},
        {"replayGainPreventClip", Flag(g_replayGainPreventClip, []() { RefreshReplayGain(); })},
        // Recording
        {"recordFormat", {[]() { return static_cast<double>(g_recordFormat); },
                          [](double v) { g_recordFormat = std::clamp(static_cast<int>(v), 0, 3); }}},
        {"recordBitrate", Plain(g_recordBitrate)},
        {"recordEffects", Flag(g_recordEffects, []() { audio::SetTapBeforeEffects(!g_recordEffects); })},
        // Speech
        {"speechTrackChange", Flag(g_speechTrackChange)},
        {"speechVolume", Flag(g_speechVolume)},
        {"speechEffect", Flag(g_speechEffect)},
        // Seeking
        {"springSpeed", Plain(g_springSpeed)},
        {"springWindUp", Flag(g_springWindUp)},
        {"springDoublingMs", ScrubTime(g_springDoublingMs)},
        {"tapeSpeed", Plain(g_tapeSpeed)},
        {"tapeSpinUp", Flag(g_tapeSpinUp)},
        {"tapeSpinUpMs", ScrubTime(g_tapeSpinUpMs)},
        {"tapeSpinDown", Flag(g_tapeSpinDown)},
        {"tapeSpinDownMs", ScrubTime(g_tapeSpinDownMs)},
        {"tapeStopOnPause", Flag(g_tapeStopOnPause)},
        {"tapeStopOnStop", Flag(g_tapeStopOnStop)},
        {"tapeStopMs", ScrubTime(g_tapeStopMs)},
        {"tapeStartOnPlay", Flag(g_tapeStartOnPlay)},
        {"tapeStartMs", ScrubTime(g_tapeStartMs)},
        // Effects
        {"rateStepMode", Plain(g_rateStepMode)},
        {"wrapEffects", Flag(g_wrapEffects)},
        {"reverbAlgorithm", {[]() { return static_cast<double>(g_reverbAlgorithm); },
                             [](double v) { SetReverbAlgorithm(std::clamp(static_cast<int>(v), 0, 2)); }}},
        // Advanced
        {"bufferSize", {[]() { return static_cast<double>(g_bufferSize); },
                        [](double v) {
                            if (static_cast<int>(v) == g_bufferSize) return;
                            // The output buffer is made with the device: open it again
                            g_bufferSize = static_cast<int>(v);
                            SwitchAudioDevice(g_selectedDevice);
                        }}},
        {"tempoAlgorithm", {[]() { return static_cast<double>(g_tempoAlgorithm); },
                            [](double v) { g_tempoAlgorithm = static_cast<int>(v) == 1 ? 1 : 2; }}},
        {"eqBassFreq", Clamped(g_eqBassFreq, 20, 500)},
        {"eqMidFreq", Clamped(g_eqMidFreq, 200, 5000)},
        {"eqTrebleFreq", Clamped(g_eqTrebleFreq, 2000, 20000)},
        {"smoothSeek", Flag(g_smoothSeek, []() { audio::SetSmoothTransitions(g_smoothSeek); })},
        {"liveRewind", Flag(g_liveRewind, []() { ApplyLiveRewindSetting(); })},
        {"liveRewindMinutes", Plain(g_liveRewindMinutes, []() { ApplyLiveRewindSetting(); })},
        // Speedy
        {"speedyNonlinear", Flag(g_speedyNonlinear)},
        // Signalsmith
        {"ssPreset", {[]() { return static_cast<double>(g_ssPreset); },
                      [](double v) { g_ssPreset = std::clamp(static_cast<int>(v), 0, 1); }}},
        {"ssTonalityLimit", {[]() { return static_cast<double>(g_ssTonalityLimit); },
                             [](double v) { g_ssTonalityLimit = std::clamp(static_cast<int>(v), 0, 20000); }}},
        // MIDI
        {"midiMaxVoices", {[]() { return static_cast<double>(g_midiMaxVoices); },
                           [](double v) {
                               g_midiMaxVoices = std::clamp(static_cast<int>(v), 1, 1000);
                               ApplyMidiSettings();
                           }}},
        {"midiSincInterp", Flag(g_midiSincInterp, []() { ApplyMidiSettings(); })},
    };
    return options;
}

std::wstring* FileOption(const std::string& name) {
    if (name == "convolutionIR") return &g_convolutionIRPath;
    if (name == "midiSoundFont") return &g_midiSoundFont;
    return nullptr;
}

void ApplyFileOption(const std::string& name) {
    if (name == "convolutionIR") {
        if (ConvolutionReverb* reverb = GetConvolutionReverb()) {
            if (!g_convolutionIRPath.empty()) reverb->LoadIR(g_convolutionIRPath.c_str());
        }
    } else if (name == "midiSoundFont") {
        ApplyMidiSettings();
    }
}

void StateChanged() {
    JNIEnv* env = Env();
    env->CallStaticVoidMethod(Callbacks(), CallbackMethod("stateChanged", "()V"));
    CheckException(env, "stateChanged");
}

}  // namespace

// A numeric or yes/no option (yes is 1).
JNI(jdouble, numberSetting)(JNIEnv* env, jclass, jstring name) {
    auto it = Options().find(ToUtf8(env, name));
    return it == Options().end() ? 0 : it->second.get();
}

JNI(void, setNumberSetting)(JNIEnv* env, jclass, jstring name, jdouble value) {
    auto it = Options().find(ToUtf8(env, name));
    if (it == Options().end()) return;
    it->second.set(value);
    SaveSettings();
    StateChanged();
}

// An option that is a file ("convolutionIR", "midiSoundFont"): the file's name, or
// "" if none is chosen.
JNI(jstring, fileSetting)(JNIEnv* env, jclass, jstring name) {
    std::wstring* path = FileOption(ToUtf8(env, name));
    if (!path || path->empty()) return ToJava(env, std::wstring());
    return ToJava(env, std::filesystem::path(*path).filename().wstring());
}

// Chooses the file at `path` (copied into FastPlay's own data, so it stays), or
// with null, none. False if the file could not be copied.
JNI(jboolean, setFileSetting)(JNIEnv* env, jclass, jstring name, jstring path) {
    const std::string option = ToUtf8(env, name);
    std::wstring* value = FileOption(option);
    if (!value) return JNI_FALSE;
    const std::wstring from = ToWide(env, path);
    if (from.empty()) {
        value->clear();
        ApplyFileOption(option);
        SaveSettings();
        return JNI_TRUE;
    }
    // Kept with FastPlay's own data, under the name it came with, so it is there
    // however the file it was picked from fares
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path folder = fs::path(GetDataDirectory()) / option;
    fs::remove_all(folder, ec);  // the one chosen before
    if (!fs::create_directories(folder, ec) && ec) return JNI_FALSE;
    const fs::path kept = folder / fs::path(from).filename();
    if (!fs::copy_file(fs::path(from), kept, fs::copy_options::overwrite_existing, ec)) return JNI_FALSE;
    *value = kept.wstring();
    ApplyFileOption(option);
    SaveSettings();
    return JNI_TRUE;
}
