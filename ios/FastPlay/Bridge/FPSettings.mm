// FastPlay's options by name: see FPSettings.h.

#import "FPSettings.h"

#include "audio.h"
#include "convolution.h"
#include "effects.h"
#include "globals.h"
#include "paths.h"
#include "player.h"
#include "settings.h"
#include "utils.h"

#include <algorithm>
#include <functional>
#include <map>
#include <string>

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
        // Effects
        {"rateStepMode", Plain(g_rateStepMode)},
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

std::wstring* FileOption(NSString* name) {
    if ([name isEqualToString:@"convolutionIR"]) return &g_convolutionIRPath;
    if ([name isEqualToString:@"midiSoundFont"]) return &g_midiSoundFont;
    return nullptr;
}

void ApplyFileOption(NSString* name) {
    if ([name isEqualToString:@"convolutionIR"]) {
        if (ConvolutionReverb* reverb = GetConvolutionReverb()) {
            if (!g_convolutionIRPath.empty()) reverb->LoadIR(g_convolutionIRPath.c_str());
        }
    } else if ([name isEqualToString:@"midiSoundFont"]) {
        ApplyMidiSettings();
    }
}

}  // namespace

@implementation FPEngine (Settings)

- (double)numberForSetting:(NSString*)name {
    auto it = Options().find(name.UTF8String ?: "");
    return it == Options().end() ? 0 : it->second.get();
}

- (void)setNumber:(double)value forSetting:(NSString*)name {
    auto it = Options().find(name.UTF8String ?: "");
    if (it == Options().end()) return;
    it->second.set(value);
    SaveSettings();
    [NSNotificationCenter.defaultCenter postNotificationName:FPEngineStateDidChangeNotification object:nil];
}

- (NSString*)fileNameForSetting:(NSString*)name {
    std::wstring* path = FileOption(name);
    if (!path || path->empty()) return @"";
    NSString* full = [NSString stringWithUTF8String:WideToUtf8(*path).c_str()];
    return full.lastPathComponent ?: @"";
}

- (BOOL)setFile:(NSString*)path forSetting:(NSString*)name {
    std::wstring* option = FileOption(name);
    if (!option) return NO;
    if (path.length == 0) {
        option->clear();
        ApplyFileOption(name);
        SaveSettings();
        return YES;
    }
    // Kept with FastPlay's own data, under the name it came with, so it is there
    // however the file it was picked from fares
    NSString* folder = [[NSString stringWithUTF8String:WideToUtf8(GetDataDirectory()).c_str()]
        stringByAppendingPathComponent:name];
    NSFileManager* manager = NSFileManager.defaultManager;
    [manager removeItemAtPath:folder error:nil];  // the one chosen before
    if (![manager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil]) return NO;
    NSString* kept = [folder stringByAppendingPathComponent:path.lastPathComponent];
    if (![manager copyItemAtPath:path toPath:kept error:nil]) return NO;
    *option = Utf8ToWide(kept.UTF8String);
    ApplyFileOption(name);
    SaveSettings();
    return YES;
}

@end
