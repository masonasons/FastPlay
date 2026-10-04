// The media keys, headphone and lock screen controls, and what Control Center shows
// under Now Playing, through MPRemoteCommandCenter and MPNowPlayingInfoCenter: the
// same on macOS and iOS.

#include "system_keys.h"
#include "commands.h"
#include "utils.h"

#import <Foundation/Foundation.h>
#import <MediaPlayer/MediaPlayer.h>

#include <chrono>
#include <cmath>

namespace {

void (*g_mediaHandler)(int) = nullptr;
void (*g_seekHandler)(int, bool) = nullptr;
NSMutableArray* g_mediaTargets = nil;  // (command, target) pairs to remove on stop

void AddMediaTarget(MPRemoteCommand* command, int commandId) {
    id target = [command addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (g_mediaHandler) g_mediaHandler(commandId);
        });
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    command.enabled = YES;
    [g_mediaTargets addObject:@[command, target]];
}

// Seeking held down: the rewind and fast forward keys, and next or previous held.
// macOS says when it starts and when it ends.
void AddSeekTarget(MPRemoteCommand* command, int direction) {
    id target = [command addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* event) {
        if (![event isKindOfClass:[MPSeekCommandEvent class]]) return MPRemoteCommandHandlerStatusCommandFailed;
        const bool pressed = static_cast<MPSeekCommandEvent*>(event).type == MPSeekCommandEventTypeBeginSeeking;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (g_seekHandler) g_seekHandler(direction, pressed);
        });
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    command.enabled = YES;
    [g_mediaTargets addObject:@[command, target]];
}

// What Control Center was last told, to update it only when something changes.
struct NowPlayingState {
    std::wstring title;
    double duration = -1;
    double position = 0;
    bool playing = false;
    std::chrono::steady_clock::time_point when;
    bool valid = false;
};
NowPlayingState g_nowPlaying;

}  // namespace

void StartMediaKeys(void (*handler)(int commandId), void (*seekHandler)(int direction, bool pressed)) {
    g_mediaHandler = handler;
    g_seekHandler = seekHandler;
    if (g_mediaTargets) return;
    g_mediaTargets = [NSMutableArray array];
    MPRemoteCommandCenter* center = [MPRemoteCommandCenter sharedCommandCenter];
    AddMediaTarget(center.togglePlayPauseCommand, IDM_PLAY_PLAYPAUSE);
    AddMediaTarget(center.playCommand, IDM_PLAY_PLAY);
    AddMediaTarget(center.pauseCommand, IDM_PLAY_PAUSE);
    AddMediaTarget(center.stopCommand, IDM_PLAY_STOP);
    AddMediaTarget(center.nextTrackCommand, IDM_PLAY_NEXT);
    AddMediaTarget(center.previousTrackCommand, IDM_PLAY_PREV);
    AddSeekTarget(center.seekBackwardCommand, -1);
    AddSeekTarget(center.seekForwardCommand, 1);
}

void StopMediaKeys() {
    g_mediaHandler = nullptr;
    g_seekHandler = nullptr;
    for (NSArray* pair in g_mediaTargets) {
        MPRemoteCommand* command = pair[0];
        [command removeTarget:pair[1]];
    }
    g_mediaTargets = nil;
    ClearNowPlaying();
}

void UpdateNowPlaying(const std::wstring& title, double duration, double position, bool playing) {
    auto now = std::chrono::steady_clock::now();
    NowPlayingState& last = g_nowPlaying;
    if (last.valid && last.title == title && last.playing == playing &&
        std::fabs(last.duration - duration) < 0.5) {
        // Control Center moves the position on by itself; only a seek needs telling.
        double expected = last.position;
        if (last.playing) expected += std::chrono::duration<double>(now - last.when).count();
        if (std::fabs(expected - position) < 2.0) return;
    }
    last.title = title;
    last.duration = duration;
    last.position = position;
    last.playing = playing;
    last.when = now;
    last.valid = true;

    NSMutableDictionary* info = [NSMutableDictionary dictionary];
    NSString* name = [NSString stringWithUTF8String:WideToUtf8(title).c_str()];
    info[MPMediaItemPropertyTitle] = name ? name : @"";
    if (duration > 0) info[MPMediaItemPropertyPlaybackDuration] = @(duration);
    info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(position);
    info[MPNowPlayingInfoPropertyPlaybackRate] = @(playing ? 1.0 : 0.0);

    MPNowPlayingInfoCenter* center = [MPNowPlayingInfoCenter defaultCenter];
    center.nowPlayingInfo = info;
    center.playbackState = playing ? MPNowPlayingPlaybackStatePlaying : MPNowPlayingPlaybackStatePaused;
}

void ClearNowPlaying() {
    g_nowPlaying = NowPlayingState();
    MPNowPlayingInfoCenter* center = [MPNowPlayingInfoCenter defaultCenter];
    center.nowPlayingInfo = nil;
    center.playbackState = MPNowPlayingPlaybackStateStopped;
}
