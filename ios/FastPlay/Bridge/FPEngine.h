// FastPlay's engine, as the iPhone app's view controllers use it: the C++ core
// (player, effects, settings, database) behind an Objective-C face. Everything here
// is for the main thread.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Posted on the main thread when what is playing, its state or the playlist changes.
extern NSNotificationName const FPEngineStateDidChangeNotification;
// The engine has something to tell the user: userInfo has FPEngineMessageTitleKey
// and FPEngineMessageTextKey.
extern NSNotificationName const FPEngineMessageNotification;
extern NSString* const FPEngineMessageTitleKey;
extern NSString* const FPEngineMessageTextKey;

// How the seek controls move through the audio (the desktop's slash key).
typedef NS_ENUM(NSInteger, FPSeekMode) {
    FPSeekModeJump = 0,    // by the seek unit
    FPSeekModeSpring = 1,  // held: faster the longer it goes, pitch kept
    FPSeekModeTape = 2,    // held: like a tape spooling
};

// A parameter the effect controls can be on: "Tempo", "Echo Delay"...
@interface FPParam : NSObject
@property (nonatomic, readonly) NSInteger identifier;
@property (nonatomic, readonly, copy) NSString* name;
// It and its value as FastPlay says them: "Tempo 5%"
@property (nonatomic, readonly, copy) NSString* text;
@end

// An effect that is on or off. Its parameters are among the effect controls'
// while it is on.
@interface FPEffect : NSObject
@property (nonatomic, readonly) NSInteger identifier;
@property (nonatomic, readonly, copy) NSString* name;
@property (nonatomic, readonly) BOOL enabled;
@end

// A unit the seek controls can move by: "5 seconds", "1 track"...
@interface FPSeekUnit : NSObject
@property (nonatomic, readonly) NSInteger identifier;
@property (nonatomic, readonly, copy) NSString* name;
@property (nonatomic, readonly) BOOL enabled;
@end

@interface FPEngine : NSObject

@property (class, nonatomic, readonly) FPEngine* shared;

// Loads the settings and starts the audio engine. False if audio could not start.
- (BOOL)start;
// Saves the settings, the playlist and where playback is (going to the background).
- (void)saveState;

// ---- Files ----------------------------------------------------------------
// The app's own folder, the one the Files app shows.
@property (nonatomic, readonly, copy) NSString* documentsPath;
- (BOOL)isPlayableFile:(NSString*)path;
// Plays a file, with the other files of its folder as the playlist.
- (void)playFile:(NSString*)path;
// Plays everything in a folder and the folders inside it.
- (void)playFolder:(NSString*)path;
// Plays an address (a stream, a file on a server). `name` is what to call it.
- (void)playURL:(NSString*)url name:(nullable NSString*)name;
// Plays several addresses as a playlist, each under its name, from the one at `index`.
- (void)playURLs:(NSArray<NSString*>*)urls names:(NSArray<NSString*>*)names startingAt:(NSInteger)index
    NS_SWIFT_NAME(playURLs(_:names:startingAt:));

// ---- The playlist -----------------------------------------------------------
@property (nonatomic, readonly) NSInteger trackCount;
@property (nonatomic, readonly) NSInteger currentTrack;  // -1: none
- (NSString*)trackNameAtIndex:(NSInteger)index;
- (void)playTrackAtIndex:(NSInteger)index;

// ---- Playback ---------------------------------------------------------------
- (void)playPause;
- (void)play;
- (void)pause;
- (void)stop;
- (void)nextTrack;
- (void)previousTrack;
@property (nonatomic, readonly) BOOL loaded;
@property (nonatomic, readonly) BOOL playing;
@property (nonatomic, readonly) BOOL live;
@property (nonatomic, readonly) double position;  // seconds
@property (nonatomic, readonly) double length;    // 0 when unknown
- (void)seekTo:(double)seconds;
// What is playing, or "" with nothing loaded.
@property (nonatomic, readonly, copy) NSString* title;
@property (nonatomic, readonly, copy) NSString* artist;
// "Playing | 128 kbps"
@property (nonatomic, readonly, copy) NSString* statusText;
+ (NSString*)formatTime:(double)seconds;

// ---- Seeking, as the desktop's arrows, comma and period do --------------------
// One step back (-1) or forward (1) by the seek unit.
- (void)seekStep:(NSInteger)direction;
// The next or previous seek unit; in spring and tape seeking, the speed.
- (void)changeSeekUnit:(NSInteger)direction;
// "5 seconds", "1 track", "1 chapter"; in spring and tape seeking, "8 times".
@property (nonatomic, readonly, copy) NSString* seekUnitText;
@property (nonatomic) FPSeekMode seekMode;
@property (nonatomic, readonly, copy) NSString* seekModeName;
- (void)cycleSeekMode;
// Spring and tape seeking: scrub from now until stopScrubbing.
@property (nonatomic, readonly) BOOL scrubSeekMode;
- (void)startScrubbing:(NSInteger)direction;
- (void)stopScrubbing;

// ---- Recording --------------------------------------------------------------
// Records what is playing, as it is heard, into the Recordings folder of
// FastPlay's files, until toggled again. Says so, and why not if it cannot.
- (void)toggleRecording;
@property (nonatomic, readonly) BOOL recording;
@property (nonatomic) NSInteger recordingFormat;   // 0 WAV, 1 MP3, 2 Ogg Vorbis, 3 FLAC
@property (nonatomic) NSInteger recordingBitrate;  // kbps, for MP3 and Ogg Vorbis
@property (nonatomic) BOOL recordEffects;          // with the effects, or the sound before them

// ---- The effect controls, as the desktop's brackets and up and down ------------
- (void)cycleParam:(NSInteger)direction;
- (void)adjustParam:(NSInteger)direction;
- (void)resetParam;
@property (nonatomic, readonly, copy) NSString* paramName;
@property (nonatomic, readonly, copy) NSString* paramText;  // "Tempo 5%"
@property (nonatomic, readonly, copy) NSArray<FPParam*>* params;
@property (nonatomic, readonly) NSInteger currentParam;
- (void)selectParam:(NSInteger)identifier;

// ---- Settings ---------------------------------------------------------------
@property (nonatomic, readonly, copy) NSArray<FPEffect*>* effects;
- (void)setEffect:(NSInteger)identifier enabled:(BOOL)enabled;
@property (nonatomic, readonly, copy) NSArray<FPSeekUnit*>* seekUnits;
- (void)setSeekUnit:(NSInteger)identifier enabled:(BOOL)enabled;
@property (nonatomic) BOOL shuffle;
@property (nonatomic) NSInteger repeatMode;  // 0 off, 1 one, 2 all
@property (nonatomic) BOOL autoAdvance;
@property (nonatomic) BOOL smoothSeeking;
@property (nonatomic) BOOL announceTrackChanges;
@property (nonatomic) BOOL announceEffectValues;
@property (nonatomic) BOOL rememberPlayback;   // the playlist and position, next time
@property (nonatomic) NSInteger tempoAlgorithm;  // 1 Speedy (speech), 2 Signalsmith (music)
// Plays alongside other apps' sound instead of stopping it. Off unless turned on.
// While on, iOS does not treat FastPlay as the app that is playing, so the lock
// screen and headphone controls go to the other app.
@property (nonatomic) BOOL mixWithOthers;
@property (nonatomic, readonly, copy) NSString* engineInfo;
@property (nonatomic, readonly, copy) NSString* version;

// ---- Speech -----------------------------------------------------------------
// Through VoiceOver, when it is running.
- (void)speak:(NSString*)text;
// While set, what the engine would say is dropped: for a control VoiceOver reads
// the new value of by itself, which would otherwise be said twice.
@property (nonatomic) BOOL speechMuted;

@end

NS_ASSUME_NONNULL_END
