// Standalone Apple remote-control regression test, with an in-memory command
// center so it never changes the user's Now Playing session. Run on macOS:
// clang++ -std=c++17 -fobjc-arc -Iinclude/fastplay tests/media_seek_controls_test.mm \
//   src/platform/now_playing_apple.mm src/utils.cpp -framework Foundation \
//   -framework MediaPlayer -Wl,-dead_strip -o /tmp/media_seek_controls_test
// /tmp/media_seek_controls_test

#include "commands.h"
#include "system_keys.h"

#import <Foundation/Foundation.h>
#import <MediaPlayer/MediaPlayer.h>
#import <objc/runtime.h>

#include <iostream>
#include <stdexcept>
#include <vector>

typedef MPRemoteCommandHandlerStatus (^CommandHandler)(MPRemoteCommandEvent*);

@interface TestCommand : NSObject
@property (nonatomic) BOOL enabled;
@property (nonatomic, copy) NSArray<NSNumber*>* preferredIntervals;
@property (nonatomic, strong) NSMutableArray* handlers;
- (id)addTargetWithHandler:(CommandHandler)handler;
- (void)removeTarget:(id)target;
- (void)send;
@end

@implementation TestCommand
- (instancetype)init {
	if ((self = [super init])) _handlers = [NSMutableArray array];
	return self;
}
- (id)addTargetWithHandler:(CommandHandler)handler {
	id target = [handler copy];
	[_handlers addObject:target];
	return target;
}
- (void)removeTarget:(id)target { [_handlers removeObjectIdenticalTo:target]; }
- (void)send {
	// Also allows testing track events from accessories despite a disabled UI button.
	for (CommandHandler handler in _handlers) handler(nil);
	[NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
}
@end

@interface TestCommandCenter : NSObject
@property (nonatomic, strong) TestCommand* togglePlayPauseCommand;
@property (nonatomic, strong) TestCommand* playCommand;
@property (nonatomic, strong) TestCommand* pauseCommand;
@property (nonatomic, strong) TestCommand* stopCommand;
@property (nonatomic, strong) TestCommand* nextTrackCommand;
@property (nonatomic, strong) TestCommand* previousTrackCommand;
@property (nonatomic, strong) TestCommand* seekBackwardCommand;
@property (nonatomic, strong) TestCommand* seekForwardCommand;
@property (nonatomic, strong) TestCommand* skipBackwardCommand;
@property (nonatomic, strong) TestCommand* skipForwardCommand;
@property (nonatomic, readonly) NSArray<TestCommand*>* commands;
@end

@implementation TestCommandCenter
- (instancetype)init {
	if ((self = [super init])) {
		_togglePlayPauseCommand = [TestCommand new];
		_playCommand = [TestCommand new];
		_pauseCommand = [TestCommand new];
		_stopCommand = [TestCommand new];
		_nextTrackCommand = [TestCommand new];
		_previousTrackCommand = [TestCommand new];
		_seekBackwardCommand = [TestCommand new];
		_seekForwardCommand = [TestCommand new];
		_skipBackwardCommand = [TestCommand new];
		_skipForwardCommand = [TestCommand new];
	}
	return self;
}
- (NSArray<TestCommand*>*)commands {
	return @[_togglePlayPauseCommand, _playCommand, _pauseCommand, _stopCommand,
		_nextTrackCommand, _previousTrackCommand, _seekBackwardCommand, _seekForwardCommand,
		_skipBackwardCommand, _skipForwardCommand];
}
@end

@interface TestNowPlayingCenter : NSObject
@property (nonatomic, copy) NSDictionary* nowPlayingInfo;
@property (nonatomic) MPNowPlayingPlaybackState playbackState;
@end
@implementation TestNowPlayingCenter
@end

namespace {
std::vector<int> received;

void Check(bool ok, const char* message) {
	if (!ok) throw std::runtime_error(message);
}

void CheckCommand(TestCommand* command, int expected) {
	received.clear();
	[command send];
	Check(received == std::vector<int>{expected}, "Remote button sent the wrong command or sent it more than once");
}
}

int main() {
	@autoreleasepool {
		TestCommandCenter* center = [TestCommandCenter new];
		TestNowPlayingCenter* nowPlaying = [TestNowPlayingCenter new];
		Method remoteMethod = class_getClassMethod(MPRemoteCommandCenter.class, @selector(sharedCommandCenter));
		Method infoMethod = class_getClassMethod(MPNowPlayingInfoCenter.class, @selector(defaultCenter));
		IMP remoteMock = imp_implementationWithBlock(^id(id) { return center; });
		IMP infoMock = imp_implementationWithBlock(^id(id) { return nowPlaying; });
		IMP oldRemote = method_setImplementation(remoteMethod, remoteMock);
		IMP oldInfo = method_setImplementation(infoMethod, infoMock);
		int result = 0;
		try {
			auto handler = [](int command) { received.push_back(command); };
			StartMediaKeys(handler, nullptr);
			Check(center.nextTrackCommand.enabled && center.previousTrackCommand.enabled, "Track controls must be the default");
			Check(!center.skipForwardCommand.enabled && !center.skipBackwardCommand.enabled, "Skip controls must default to off");
			CheckCommand(center.nextTrackCommand, IDM_PLAY_NEXT);
			CheckCommand(center.previousTrackCommand, IDM_PLAY_PREV);

			SetMediaSeekControls(true, 5);
			Check(!center.nextTrackCommand.enabled && !center.previousTrackCommand.enabled, "Skip mode must hide track controls");
			Check(center.skipForwardCommand.enabled && center.skipBackwardCommand.enabled, "Skip mode must enable both seek buttons");
			Check([center.skipForwardCommand.preferredIntervals isEqual:@[@5]] &&
				[center.skipBackwardCommand.preferredIntervals isEqual:@[@5]], "Both buttons must advertise the player's interval");
			CheckCommand(center.skipForwardCommand, IDM_PLAY_SEEKFWD);
			CheckCommand(center.skipBackwardCommand, IDM_PLAY_SEEKBACK);
			CheckCommand(center.nextTrackCommand, IDM_PLAY_SEEKFWD);
			CheckCommand(center.previousTrackCommand, IDM_PLAY_SEEKBACK);
			CheckCommand(center.togglePlayPauseCommand, IDM_PLAY_PLAYPAUSE);
			Check(center.seekForwardCommand.enabled && center.seekBackwardCommand.enabled, "Held seeking must remain available");

			SetMediaSeekControls(true, 30);
			Check([center.skipForwardCommand.preferredIntervals isEqual:@[@30]] &&
				[center.skipBackwardCommand.preferredIntervals isEqual:@[@30]], "Changing Seek By must update both intervals");
			SetMediaSeekControls(true, 0);
			Check(center.skipForwardCommand.preferredIntervals.count == 0 &&
				center.skipBackwardCommand.preferredIntervals.count == 0, "Track/chapter steps must not advertise seconds");
			CheckCommand(center.skipForwardCommand, IDM_PLAY_SEEKFWD);

			SetMediaSeekControls(false, 10);
			Check(center.nextTrackCommand.enabled && !center.skipForwardCommand.enabled, "Turning off must restore track buttons");
			CheckCommand(center.nextTrackCommand, IDM_PLAY_NEXT);
			CheckCommand(center.previousTrackCommand, IDM_PLAY_PREV);
			SetMediaSeekControls(true, 10);
			StopMediaKeys();
			for (TestCommand* command in center.commands) {
				Check(!command.enabled && command.handlers.count == 0, "Stopping must remove and disable every command");
			}
			StartMediaKeys(handler, nullptr);
			Check(center.nextTrackCommand.enabled && !center.skipForwardCommand.enabled, "Restart must reset skip mode");
			CheckCommand(center.nextTrackCommand, IDM_PLAY_NEXT);
			SetMediaSeekControls(true, 10);
			CheckCommand(center.skipForwardCommand, IDM_PLAY_SEEKFWD);
			std::cout << "Media seek control regression checks passed\n";
		} catch (const std::exception& error) {
			std::cerr << error.what() << '\n';
			result = 1;
		}
		StopMediaKeys();
		method_setImplementation(remoteMethod, oldRemote);
		method_setImplementation(infoMethod, oldInfo);
		imp_removeBlock(remoteMock);
		imp_removeBlock(infoMock);
		return result;
	}
}
