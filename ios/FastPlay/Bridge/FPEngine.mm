// FastPlay's engine for the iPhone app: see FPEngine.h. This is also where the
// core's calls to "the UI" (app_ui.h) arrive, which on the desktop is the main window.

#import "FPEngine.h"

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

#include "accessibility.h"
#include "app_ui.h"
#include "audio.h"
#include "commands.h"
#include "database.h"
#include "effects.h"
#include "globals.h"
#include "paths.h"
#include "platform.h"
#include "player.h"
#include "playlist_io.h"
#include "scheduler.h"
#include "settings.h"
#include "system_keys.h"
#include "utils.h"
#include "version.h"

#include <algorithm>
#include <string>
#include <vector>

// speech_ios.mm: what FastPlay says is dropped while this is set
void SetSpeechMuted(bool muted);
bool IsSpeechMuted();

NSNotificationName const FPEngineStateDidChangeNotification = @"FPEngineStateDidChange";
NSNotificationName const FPEngineMessageNotification = @"FPEngineMessage";
NSString* const FPEngineMessageTitleKey = @"title";
NSString* const FPEngineMessageTextKey = @"text";

namespace {

// The seek unit that is "1 chapter" (settings.cpp)
const int kChapterSeekIndex = 12;
// Effects are numbered for the app: the four stream effects, then the DSP effects.
const NSInteger kDspEffectBase = 100;
// Rewind or fast forward held, in jump seeking: the first jump, then repeats
const NSTimeInterval kMediaSeekDelay = 0.4;
const NSTimeInterval kMediaSeekRepeat = 0.2;
// A setting of the iPhone app alone, so in its own defaults rather than FastPlay.ini
NSString* const kMixWithOthersKey = @"MixWithOthers";

// The audio session as a player's, mixing with other apps' sound or taking over
// from it. (miniaudio made it a playback session when the engine started.)
void ApplyAudioSessionOptions() {
    const bool mix = [NSUserDefaults.standardUserDefaults boolForKey:kMixWithOthersKey];
    [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback
                                   withOptions:mix ? AVAudioSessionCategoryOptionMixWithOthers : 0
                                         error:nil];
}

NSString* ToNS(const std::wstring& text) {
    return [NSString stringWithUTF8String:WideToUtf8(text).c_str()] ?: @"";
}

NSString* ToNS(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

std::wstring ToWide(NSString* text) {
    return Utf8ToWide(text.UTF8String ?: "");
}

bool g_stateChangePending = false;

void PublishNowPlaying() {
	if (!audio::IsLoaded()) {
		ClearNowPlaying();
		return;
	}
	UpdateNowPlaying(ToWide(FPEngine.shared.title), audio::Length(), audio::Position(), IsPlaying());
}

// Tell the app that something changed, once however many times it is asked for
// before the main thread gets to it.
void PostStateChange() {
    if (g_stateChangePending) return;
    g_stateChangePending = true;
    dispatch_async(dispatch_get_main_queue(), ^{
        g_stateChangePending = false;
		// Publish transport changes without waiting for the position timer, which
		// may stop running once a paused app is suspended in the background.
		PublishNowPlaying();
        [NSNotificationCenter.defaultCenter postNotificationName:FPEngineStateDidChangeNotification object:nil];
    });
}

// The app's container moves when the app is restored or reinstalled, and the
// settings hold full paths (the playlist, remembered positions, recent files).
// When it has moved, the old container's path in the settings becomes the new.
void MoveSettingsToThisContainer() {
    NSString* home = NSHomeDirectory();
    NSString* dataDir = ToNS(GetDataDirectory());
    NSString* marker = [dataDir stringByAppendingPathComponent:@"container.txt"];
    NSString* before = [NSString stringWithContentsOfFile:marker encoding:NSUTF8StringEncoding error:nil];
    if ([before isEqualToString:home]) return;
    if (before.length > 0) {
        NSString* ini = [dataDir stringByAppendingPathComponent:@"FastPlay.ini"];
        NSString* text = [NSString stringWithContentsOfFile:ini encoding:NSUTF8StringEncoding error:nil];
        if (text) {
            text = [text stringByReplacingOccurrencesOfString:before withString:home];
            [text writeToFile:ini atomically:YES encoding:NSUTF8StringEncoding error:nil];
        }
    }
    [home writeToFile:marker atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

}  // namespace

// ---------------------------------------------------------------------------
// The small objects handed to the app
// ---------------------------------------------------------------------------

@interface FPParam ()
- (instancetype)initWithIdentifier:(NSInteger)identifier name:(NSString*)name text:(NSString*)text;
@end

@implementation FPParam
- (instancetype)initWithIdentifier:(NSInteger)identifier name:(NSString*)name text:(NSString*)text {
    if ((self = [super init])) {
        _identifier = identifier;
        _name = [name copy];
        _text = [text copy];
    }
    return self;
}
@end

@interface FPEffect ()
- (instancetype)initWithIdentifier:(NSInteger)identifier name:(NSString*)name enabled:(BOOL)enabled;
@end

@implementation FPEffect
- (instancetype)initWithIdentifier:(NSInteger)identifier name:(NSString*)name enabled:(BOOL)enabled {
    if ((self = [super init])) {
        _identifier = identifier;
        _name = [name copy];
        _enabled = enabled;
    }
    return self;
}
@end

@interface FPSeekUnit ()
- (instancetype)initWithIdentifier:(NSInteger)identifier name:(NSString*)name enabled:(BOOL)enabled;
@end

@implementation FPSeekUnit
- (instancetype)initWithIdentifier:(NSInteger)identifier name:(NSString*)name enabled:(BOOL)enabled {
    if ((self = [super init])) {
        _identifier = identifier;
        _name = [name copy];
        _enabled = enabled;
    }
    return self;
}
@end

// ---------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------

@interface FPEngine ()
- (void)runCommand:(int)command param:(int)param;
- (void)mediaSeek:(int)direction pressed:(bool)pressed;
- (void)startScheduleDurationTimer:(int)ms;
- (void)stopScheduleDurationTimer;
@end

@implementation FPEngine {
    BOOL _started;
    NSTimer* _tickTimer;
    NSTimer* _scheduleTimer;
    NSTimer* _durationTimer;
    NSTimer* _mediaSeekTimer;
    int _mediaSeekDirection;
    BOOL _interruptedWhilePlaying;
}

+ (FPEngine*)shared {
    static FPEngine* engine;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        engine = [FPEngine new];
    });
    return engine;
}

- (BOOL)start {
    if (_started) return YES;
    srand(static_cast<unsigned>(time(nullptr)));  // so the shuffle order differs between runs
    PlatformStartup();
    MoveSettingsToThisContainer();
    LoadSettings();
    // Recordings go in a folder of FastPlay's own files (set each time: the app's
    // container, and so the folder's full path, can move)
    g_recordPath = GetUserMusicDir() + L"/Recordings";
    // On a phone, choosing a file plays on through its folder unless told otherwise
    // (the desktop's default is the file alone); set once, then the user's to change
    if (![NSUserDefaults.standardUserDefaults boolForKey:@"PhoneDefaultsApplied"]) {
        g_loadFolder = true;
        SaveSettings();
        [NSUserDefaults.standardUserDefaults setBool:YES forKey:@"PhoneDefaultsApplied"];
    }
    if (!InitAudio()) return NO;
    ApplyAudioSessionOptions();
    // For trying things on a phone that should stay quiet: the real audio device,
    // with the sound muted
    if (getenv("FASTPLAY_MUTED")) {
        g_muted = true;
        UpdateOutputGain();
    }
    InitDatabase();
    InitEffects();
    LoadDSPSettings();
    InitSpeech();
    _started = YES;

    // The lock screen, Control Center, headphone buttons and keyboards' media keys
    StartMediaKeys(
        [](int command) { [FPEngine.shared runCommand:command param:0]; },
        [](int direction, bool pressed) { [FPEngine.shared mediaSeek:direction pressed:pressed]; });

    // Now Playing's position, and the app's own time display, twice a second
    _tickTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer*) {
        [self tick];
    }];
    _scheduleTimer = [NSTimer scheduledTimerWithTimeInterval:60 repeats:YES block:^(NSTimer*) {
        CheckScheduledEvents();
    }];

    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    [center addObserver:self selector:@selector(audioInterrupted:)
                   name:AVAudioSessionInterruptionNotification object:nil];
    [center addObserver:self selector:@selector(audioRouteChanged:)
                   name:AVAudioSessionRouteChangeNotification object:nil];
    // Another app taking the sound stops the output device, and nothing starts
    // it again: Play does, with the session made active first. Coming back to
    // the app does too, so the device is there before anything is pressed.
    audio::SetBeforeDeviceStart([]() { [AVAudioSession.sharedInstance setActive:YES error:nil]; });
    [center addObserver:self selector:@selector(appBecameActive:)
                   name:UIApplicationDidBecomeActiveNotification object:nil];

    // What was playing last time, where it was
    if (g_playlist.empty()) LoadPlaybackState();
    PostStateChange();
    return YES;
}

- (void)saveState {
    if (!_started) return;
    SaveCurrentPosition();
    SavePlaybackState();
    SaveSettings();
}

- (void)tick {
	if (!audio::IsLoaded()) return;
	PublishNowPlaying();
}

// A phone call or an alarm takes the sound: paused until it is given back.
- (void)audioInterrupted:(NSNotification*)note {
    NSUInteger type = [note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
    if (type == AVAudioSessionInterruptionTypeBegan) {
        _interruptedWhilePlaying = IsPlaying();
        if (_interruptedWhilePlaying) Pause();
    } else {
        NSUInteger options = [note.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
        if (_interruptedWhilePlaying && (options & AVAudioSessionInterruptionOptionShouldResume)) Play();
        _interruptedWhilePlaying = NO;
    }
}

// Back in the app. Only while nothing else is sounding: starting the device
// takes the sound, which is for Play to do, not for a look at the app.
- (void)appBecameActive:(NSNotification*)note {
	if (IsPlaying() && !AVAudioSession.sharedInstance.isOtherAudioPlaying) audio::EnsureDeviceRunning();
}

// Headphones unplugged or a speaker switched off: pause, rather than carry on aloud.
- (void)audioRouteChanged:(NSNotification*)note {
    NSUInteger reason = [note.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
    if (reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable && IsPlaying()) {
        dispatch_async(dispatch_get_main_queue(), ^{
            Pause();
        });
    }
}

// ---- Files ----------------------------------------------------------------

- (NSString*)documentsPath {
    return ToNS(GetUserMusicDir());
}

- (BOOL)isPlayableFile:(NSString*)path {
    std::wstring wide = ToWide(path);
    if (IsPlaylistFile(wide)) return YES;
    size_t dot = wide.find_last_of(L'.');
    return dot != std::wstring::npos && IsSupportedAudioExt(wide.substr(dot));
}

- (void)startPlaylistAt:(int)index {
    g_currentTrack = -1;
    if (g_playlist.empty()) {
        Speak("Nothing to play");
        return;
    }
    PlayTrack(std::clamp(index, 0, static_cast<int>(g_playlist.size()) - 1));
    PostStateChange();
}

- (void)playFile:(NSString*)path {
    std::wstring wide = ToWide(path);
    int start = 0;
    if (IsPlaylistFile(wide)) {
        g_playlist = ParsePlaylist(wide);
    } else if (g_loadFolder) {
        start = ExpandFileToFolder(wide, g_playlist);  // with the rest of its folder
    } else {
        g_playlist.assign(1, wide);
    }
    [self startPlaylistAt:start];
}

- (void)playFolder:(NSString*)path {
    std::vector<std::wstring> files;
    AddFilesFromFolder(ToWide(path), files);
    g_playlist = files;
    [self startPlaylistAt:0];
}

- (void)playURL:(NSString*)url name:(NSString*)name {
    std::wstring wide = ToWide(url);
    if (name.length > 0) SetTrackName(wide, ToWide(name));
    g_playlist.assign(1, wide);
    [self startPlaylistAt:0];
}

- (void)playURLs:(NSArray<NSString*>*)urls names:(NSArray<NSString*>*)names startingAt:(NSInteger)index {
    g_playlist.clear();
    for (NSUInteger i = 0; i < urls.count; i++) {
        std::wstring wide = ToWide(urls[i]);
        if (i < names.count && names[i].length > 0) SetTrackName(wide, ToWide(names[i]));
        g_playlist.push_back(wide);
    }
    [self startPlaylistAt:static_cast<int>(index)];
}

// ---- The playlist -----------------------------------------------------------

- (NSInteger)trackCount {
    return static_cast<NSInteger>(g_playlist.size());
}

- (NSInteger)currentTrack {
    return g_currentTrack;
}

- (NSString*)trackNameAtIndex:(NSInteger)index {
    if (index < 0 || index >= static_cast<NSInteger>(g_playlist.size())) return @"";
    return ToNS(GetTrackName(g_playlist[static_cast<size_t>(index)]));
}

- (NSString*)trackPathAtIndex:(NSInteger)index {
    if (index < 0 || index >= static_cast<NSInteger>(g_playlist.size())) return @"";
    return ToNS(g_playlist[static_cast<size_t>(index)]);
}

- (void)playTrackAtIndex:(NSInteger)index {
    if (index < 0 || index >= static_cast<NSInteger>(g_playlist.size())) return;
    PlayTrack(static_cast<int>(index));
    PostStateChange();
}

// ---- Playback ---------------------------------------------------------------

- (void)playPause { PlayPause(); PostStateChange(); }
- (void)play { Play(); PostStateChange(); }
- (void)pause { Pause(); PostStateChange(); }
- (void)stop { Stop(); PostStateChange(); }
- (void)nextTrack { NextTrack(); PostStateChange(); }
- (void)previousTrack { PrevTrack(); PostStateChange(); }

- (BOOL)loaded { return audio::IsLoaded(); }
- (BOOL)playing { return IsPlaying(); }
- (BOOL)live { return audio::IsLoaded() && audio::IsLive(); }
- (double)position { return audio::IsLoaded() ? audio::Position() : 0; }
- (double)length { return audio::IsLoaded() ? audio::Length() : 0; }

- (void)seekTo:(double)seconds {
    SeekToPosition(seconds);
    PostStateChange();
}

- (NSString*)title {
    if (g_currentTrack < 0 || g_currentTrack >= static_cast<int>(g_playlist.size())) return @"";
    // The tags' title; a file without one goes by its name
    std::wstring title = GetTagTitle();
    if (title.empty() || title == L"No title" || title == L"Nothing playing") {
        title = GetTrackName(g_playlist[static_cast<size_t>(g_currentTrack)]);
    }
    return ToNS(title);
}

- (NSString*)artist {
    if (!audio::IsLoaded()) return @"";
    std::wstring artist = GetTagArtist();
    return artist == L"No artist" ? @"" : ToNS(artist);
}

- (NSString*)statusText {
    if (!audio::IsLoaded()) return @"";
    std::wstring text;
    switch (audio::GetState()) {
        case audio::State::Playing: text = L"Playing"; break;
        case audio::State::Paused: text = L"Paused"; break;
        case audio::State::Stopped: text = L"Stopped"; break;
        default: break;
    }
    if (int bitrate = GetCurrentBitrate()) {
        if (!text.empty()) text += L" | ";
        text += (IsCurrentVbr() ? L"~" : L"") + std::to_wstring(bitrate) + L" kbps" + (IsCurrentVbr() ? L" VBR" : L"");
    }
    if (g_isRecording) text += L" | REC";
    return ToNS(text);
}

+ (NSString*)formatTime:(double)seconds {
    return ToNS(FormatTime(seconds));
}

// ---- Seeking ----------------------------------------------------------------

// As the desktop's Left and Right do (MainFrame::SeekBackOrForward)
- (void)seekStep:(NSInteger)direction {
    const int dir = direction < 0 ? -1 : 1;
    if (g_currentSeekIndex == kChapterSeekIndex) {
        if (!g_chapters.empty()) {
            if (dir < 0) SeekToPrevChapter(); else SeekToNextChapter();
        }
    } else if (g_seekAmounts[g_currentSeekIndex].isTrack && g_playlist.size() <= 1) {
        // Track seeking with a single track: the first enabled time amount instead
        for (int i = 0; i < g_seekAmountCount; i++) {
            if (g_seekEnabled[i] && !g_seekAmounts[i].isTrack) {
                Seek(dir * g_seekAmounts[i].value);
                break;
            }
        }
    } else if (g_seekAmounts[g_currentSeekIndex].isTrack) {
        SeekTracks(dir * static_cast<int>(g_seekAmounts[g_currentSeekIndex].value));
    } else {
        Seek(dir * GetCurrentSeekAmount());
    }
    PostStateChange();
}

- (void)changeSeekUnit:(NSInteger)direction {
    const int dir = direction < 0 ? -1 : 1;
    if (IsScrubSeekMode()) ChangeScrubSpeed(dir); else CycleSeekAmount(dir);
    PostStateChange();
}

- (NSString*)seekUnitText {
    if (IsScrubSeekMode()) {
        int speed = g_seekMode == SEEK_MODE_TAPE ? g_tapeSpeed : g_springSpeed;
        return [NSString stringWithFormat:@"%d times", speed];
    }
    if (g_currentSeekIndex == kChapterSeekIndex) return @"1 chapter";
    if (g_currentSeekIndex >= 0 && g_currentSeekIndex < g_seekAmountCount) {
        return ToNS(std::string(g_seekAmounts[g_currentSeekIndex].label));
    }
    return @"";
}

- (FPSeekMode)seekMode {
    return static_cast<FPSeekMode>(g_seekMode);
}

- (NSString*)seekModeName {
    switch (g_seekMode) {
        case SEEK_MODE_SPRING: return @"Spring";
        case SEEK_MODE_TAPE: return @"Tape";
        default: return @"Jump";
    }
}

- (void)setSeekMode:(FPSeekMode)mode {
    if (g_seekMode == static_cast<int>(mode)) return;
    StopScrubbing();
    g_seekMode = std::clamp(static_cast<int>(mode), 0, static_cast<int>(SEEK_MODE_COUNT) - 1);
    PostStateChange();
}

- (void)cycleSeekMode {
    CycleSeekMode();
    PostStateChange();
}

// ---- Recording --------------------------------------------------------------

- (void)toggleRecording {
    ToggleRecording();
    PostStateChange();
}

- (BOOL)recording { return g_isRecording; }

- (NSInteger)recordingFormat { return g_recordFormat; }
- (void)setRecordingFormat:(NSInteger)format {
    g_recordFormat = static_cast<int>(std::clamp<NSInteger>(format, 0, 3));
    SaveSettings();
}

- (NSInteger)recordingBitrate { return g_recordBitrate; }
- (void)setRecordingBitrate:(NSInteger)bitrate {
    g_recordBitrate = static_cast<int>(bitrate);
    SaveSettings();
}

- (BOOL)recordEffects { return g_recordEffects; }
- (void)setRecordEffects:(BOOL)on {
    g_recordEffects = on;
    audio::SetTapBeforeEffects(!g_recordEffects);
    SaveSettings();
}

- (BOOL)scrubSeekMode {
    return IsScrubSeekMode();
}

- (void)startScrubbing:(NSInteger)direction {
    StartScrubbing(direction < 0 ? -1 : 1);
}

- (void)stopScrubbing {
    StopScrubbing();
    PostStateChange();
}

// ---- The effect controls ----------------------------------------------------

- (void)cycleParam:(NSInteger)direction {
    CycleParam(direction < 0 ? -1 : 1);
    PostStateChange();
}

- (void)adjustParam:(NSInteger)direction {
    AdjustCurrentParam(direction < 0 ? -1 : 1);
    PostStateChange();
}

- (void)resetParam {
    ResetCurrentParam();
    PostStateChange();
}

- (NSString*)paramName {
    if (GetAvailableParamCount() == 0) return @"";
    return ToNS(std::string(GetParamName(GetCurrentParam())));
}

- (NSString*)paramText {
    if (GetAvailableParamCount() == 0) return @"";
    return ToNS(DescribeParam(GetCurrentParam()));
}

- (NSArray<FPParam*>*)params {
    NSMutableArray<FPParam*>* list = [NSMutableArray array];
    for (ParamId id : GetAvailableParamIds()) {
        [list addObject:[[FPParam alloc] initWithIdentifier:static_cast<NSInteger>(id)
                                                       name:ToNS(std::string(GetParamName(id)))
                                                       text:ToNS(DescribeParam(id))]];
    }
    return list;
}

- (NSInteger)currentParam {
    return static_cast<NSInteger>(GetCurrentParam());
}

- (void)selectParam:(NSInteger)identifier {
    SetCurrentParam(static_cast<ParamId>(identifier));
    PostStateChange();
}

// ---- Settings ---------------------------------------------------------------

- (NSArray<FPEffect*>*)effects {
    static NSString* const streamNames[] = {@"Volume", @"Pitch", @"Tempo", @"Rate"};
    static NSString* const dspNames[] = {@"Reverb", @"Echo", @"Equalizer", @"Compressor", @"Stereo Width",
                                         @"Center Cancel", @"Convolution Reverb", @"3D Audio", @"Normalizer"};
    NSMutableArray<FPEffect*>* list = [NSMutableArray array];
    for (int i = 0; i < static_cast<int>(StreamEffect::COUNT); i++) {
        [list addObject:[[FPEffect alloc] initWithIdentifier:i name:streamNames[i] enabled:IsStreamEffectEnabled(i)]];
    }
    for (int i = 0; i < static_cast<int>(DSPEffectType::COUNT); i++) {
        DSPEffectType type = static_cast<DSPEffectType>(i);
        // Reverb is on when one of its algorithms is chosen
        bool on = type == DSPEffectType::Reverb ? g_reverbAlgorithm != 0 : IsDSPEffectEnabled(type);
        [list addObject:[[FPEffect alloc] initWithIdentifier:kDspEffectBase + i name:dspNames[i] enabled:on]];
    }
    return list;
}

- (void)setEffect:(NSInteger)identifier enabled:(BOOL)enabled {
    if (identifier < kDspEffectBase) {
        int index = static_cast<int>(identifier);
        if (index < 0 || index >= static_cast<int>(StreamEffect::COUNT)) return;
        if (IsStreamEffectEnabled(index) != static_cast<bool>(enabled)) ToggleStreamEffect(index);
    } else {
        int index = static_cast<int>(identifier - kDspEffectBase);
        if (index < 0 || index >= static_cast<int>(DSPEffectType::COUNT)) return;
        DSPEffectType type = static_cast<DSPEffectType>(index);
        if (type == DSPEffectType::Reverb) {
            if ((g_reverbAlgorithm != 0) != static_cast<bool>(enabled)) SetReverbAlgorithm(enabled ? 1 : 0);
        } else {
            EnableDSPEffect(type, enabled);
        }
    }
    SaveSettings();
    PostStateChange();
}

- (NSArray<FPSeekUnit*>*)seekUnits {
    NSMutableArray<FPSeekUnit*>* list = [NSMutableArray array];
    for (int i = 0; i < g_seekAmountCount; i++) {
        [list addObject:[[FPSeekUnit alloc] initWithIdentifier:i
                                                          name:ToNS(std::string(g_seekAmounts[i].label))
                                                       enabled:g_seekEnabled[i]]];
    }
    [list addObject:[[FPSeekUnit alloc] initWithIdentifier:kChapterSeekIndex name:@"1 chapter"
                                                   enabled:g_chapterSeekEnabled]];
    return list;
}

- (void)setSeekUnit:(NSInteger)identifier enabled:(BOOL)enabled {
    if (identifier == kChapterSeekIndex) {
        g_chapterSeekEnabled = enabled;
    } else if (identifier >= 0 && identifier < g_seekAmountCount) {
        g_seekEnabled[identifier] = enabled;
    }
    // The unit in use may just have been turned off: onto one that is on
    if (!IsSeekAmountAvailable(g_currentSeekIndex)) {
        for (int i = 0; i <= kChapterSeekIndex; i++) {
            if (IsSeekAmountAvailable(i)) {
                g_currentSeekIndex = i;
                break;
            }
        }
    }
    SaveSettings();
    PostStateChange();
}

- (BOOL)shuffle { return g_shuffle; }
- (void)setShuffle:(BOOL)shuffle {
    g_shuffle = shuffle;
    if (g_shuffle) ResetShuffleOrder();
    SaveSettings();
}

- (NSInteger)repeatMode { return g_repeatMode; }
- (void)setRepeatMode:(NSInteger)mode {
    g_repeatMode = static_cast<int>(std::clamp<NSInteger>(mode, 0, 2));
    SaveSettings();
}

- (BOOL)autoAdvance { return g_autoAdvance; }
- (void)setAutoAdvance:(BOOL)on { g_autoAdvance = on; SaveSettings(); }

- (BOOL)smoothSeeking { return g_smoothSeek; }
- (void)setSmoothSeeking:(BOOL)on {
    g_smoothSeek = on;
    audio::SetSmoothTransitions(g_smoothSeek);
    SaveSettings();
}

- (BOOL)announceTrackChanges { return g_speechTrackChange; }
- (void)setAnnounceTrackChanges:(BOOL)on { g_speechTrackChange = on; SaveSettings(); }

- (BOOL)announceEffectValues { return g_speechEffect; }
- (void)setAnnounceEffectValues:(BOOL)on { g_speechEffect = on; SaveSettings(); }

- (BOOL)rememberPlayback { return g_rememberState; }
- (void)setRememberPlayback:(BOOL)on { g_rememberState = on; SaveSettings(); }

- (NSInteger)tempoAlgorithm { return g_tempoAlgorithm; }
- (void)setTempoAlgorithm:(NSInteger)algorithm {
    g_tempoAlgorithm = algorithm == 1 ? 1 : 2;  // takes effect with the next track
    SaveSettings();
}

- (BOOL)mixWithOthers {
    return [NSUserDefaults.standardUserDefaults boolForKey:kMixWithOthersKey];
}

- (void)setMixWithOthers:(BOOL)mix {
    [NSUserDefaults.standardUserDefaults setBool:mix forKey:kMixWithOthersKey];
    ApplyAudioSessionOptions();
}

- (NSString*)engineInfo {
    return ToNS(GetAudioEngineInfo());
}

- (NSString*)version {
    return @APP_VERSION;
}

// ---- Speech -----------------------------------------------------------------

- (void)speak:(NSString*)text {
    Speak(text.UTF8String ?: "");
}

- (BOOL)speechMuted { return IsSpeechMuted(); }
- (void)setSpeechMuted:(BOOL)muted { SetSpeechMuted(muted); }

// ---- What the core asks of "the main window" ----------------------------------

// A command, as the desktop's menu and hotkeys run them: the ones that make sense
// without windows of their own.
- (void)runCommand:(int)command param:(int)param {
    switch (command) {
        case IDM_PLAY_PLAYPAUSE: PlayPause(); break;
        case IDM_PLAY_PLAY: Play(); break;
        case IDM_PLAY_PAUSE: Pause(); break;
        case IDM_PLAY_STOP: Stop(); break;
        case IDM_PLAY_PREV: PrevTrack(); break;
        case IDM_PLAY_NEXT: NextTrack(param == 0); break;  // 1: load without playing
        case IDM_PLAY_SEEKBACK: [self seekStep:-1]; break;
        case IDM_PLAY_SEEKFWD: [self seekStep:1]; break;
        case IDM_PLAY_BEGINNING: SeekToPosition(0); break;
        case IDM_SEEK_DECREASE: [self changeSeekUnit:-1]; break;
        case IDM_SEEK_INCREASE: [self changeSeekUnit:1]; break;
        case IDM_SEEK_MODE: CycleSeekMode(); break;
        case IDM_PLAY_GOLIVE: GoLive(); break;
        case IDM_PLAY_MUTE: ToggleMute(); break;
        case IDM_PLAY_REPEAT_TOGGLE: ToggleRepeatMode(); break;
        case IDM_EFFECT_PREV: CycleParam(-1); break;
        case IDM_EFFECT_NEXT: CycleParam(1); break;
        case IDM_EFFECT_UP: AdjustCurrentParam(1); break;
        case IDM_EFFECT_DOWN: AdjustCurrentParam(-1); break;
        case IDM_EFFECT_RESET: ResetCurrentParam(); break;
        case IDM_RECORD_TOGGLE: ToggleRecording(); break;
        default: break;
    }
    PostStateChange();
}

// Rewind or fast forward (or next or previous held down), from the lock screen,
// headphones or a keyboard: as a seek key held does on the desktop.
- (void)mediaSeek:(int)direction pressed:(bool)pressed {
    if (!pressed) {
        if (_mediaSeekDirection == 0) return;
        _mediaSeekDirection = 0;
        [_mediaSeekTimer invalidate];
        _mediaSeekTimer = nil;
        StopScrubbing();
        PostStateChange();
        return;
    }
    _mediaSeekDirection = direction;
    if (IsScrubSeekMode()) {
        StartScrubbing(direction);
        return;
    }
    [self seekStep:direction];
    [_mediaSeekTimer invalidate];
    _mediaSeekTimer = [NSTimer scheduledTimerWithTimeInterval:kMediaSeekRepeat repeats:YES block:^(NSTimer*) {
        if (self->_mediaSeekDirection != 0) [self seekStep:self->_mediaSeekDirection];
    }];
    _mediaSeekTimer.fireDate = [NSDate dateWithTimeIntervalSinceNow:kMediaSeekDelay];
}

- (void)startScheduleDurationTimer:(int)ms {
    [_durationTimer invalidate];
    _durationTimer = [NSTimer scheduledTimerWithTimeInterval:ms / 1000.0 repeats:NO block:^(NSTimer*) {
        HandleScheduledDurationEnd();
    }];
}

- (void)stopScheduleDurationTimer {
    [_durationTimer invalidate];
    _durationTimer = nil;
}

@end

// ---------------------------------------------------------------------------
// app_ui.h: how code outside the UI reaches it
// ---------------------------------------------------------------------------

void RunOnUiThread(std::function<void()> fn) {
    auto shared = std::make_shared<std::function<void()>>(std::move(fn));
    dispatch_async(dispatch_get_main_queue(), ^{
        (*shared)();
    });
}

void PostCommand(int commandId, int param) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [FPEngine.shared runCommand:commandId param:param];
    });
}

void ShowMessage(const std::wstring& text, const std::wstring& title, MessageIcon) {
    NSDictionary* info = @{FPEngineMessageTitleKey: ToNS(title), FPEngineMessageTextKey: ToNS(text)};
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter postNotificationName:FPEngineMessageNotification object:nil userInfo:info];
    });
}

// Nothing in the core asks questions; an app with no modal loop could only say no.
bool AskYesNo(const std::wstring&, const std::wstring&) {
    return false;
}

void* GetMainWindowHandle() {
    return nullptr;
}

void CloseMainWindow() {
    // An iPhone app is not closed by itself.
}

void UpdateWindowTitle() {
    PostStateChange();
}

void UpdateStatusBar() {
    PostStateChange();
}

void StartScheduleDurationTimer(int ms) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [FPEngine.shared startScheduleDurationTimer:ms];
    });
}

void StopScheduleDurationTimer() {
    dispatch_async(dispatch_get_main_queue(), ^{
        [FPEngine.shared stopScheduleDurationTimer];
    });
}

void NotifyPlaylistTrackChanged() {
    PostStateChange();
}
