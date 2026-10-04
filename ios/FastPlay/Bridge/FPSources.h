// Internet radio and podcasts, from FastPlay's engine: the saved stations and
// subscriptions (the same database as on the desktop) and the directory searches.
// The lookups go over the network on a background queue; every completion block
// is called on the main thread.

#import <Foundation/Foundation.h>

#import "FPEngine.h"

NS_ASSUME_NONNULL_BEGIN

// A saved radio station.
@interface FPRadioStation : NSObject
@property (nonatomic, readonly) NSInteger identifier;
@property (nonatomic, readonly, copy) NSString* name;
@property (nonatomic, readonly, copy) NSString* url;
@end

// A station found in one of the directories.
@interface FPRadioResult : NSObject
@property (nonatomic, readonly, copy) NSString* name;
// "Germany, MP3, 128 kbps", as much of it as the directory gave
@property (nonatomic, readonly, copy) NSString* detail;
@end

typedef NS_ENUM(NSInteger, FPRadioDirectory) {
    FPRadioDirectoryRadioBrowser = 0,
    FPRadioDirectoryTuneIn = 1,
    FPRadioDirectoryIHeartRadio = 2,
};

// A podcast subscribed to.
@interface FPPodcast : NSObject
@property (nonatomic, readonly) NSInteger identifier;
@property (nonatomic, readonly, copy) NSString* name;
@property (nonatomic, readonly, copy) NSString* feedURL;
@end

// A podcast found in the directory.
@interface FPPodcastResult : NSObject
@property (nonatomic, readonly, copy) NSString* name;
@property (nonatomic, readonly, copy) NSString* author;
@property (nonatomic, readonly, copy) NSString* feedURL;
@end

@interface FPEpisode : NSObject
@property (nonatomic, readonly, copy) NSString* title;
@property (nonatomic, readonly, copy) NSString* date;
@property (nonatomic, readonly, copy) NSString* summary;
@property (nonatomic, readonly, copy) NSString* audioURL;
@property (nonatomic, readonly) NSInteger duration;  // seconds, 0 if the feed does not say
@end

@interface FPEngine (Sources)

// ---- Radio ------------------------------------------------------------------
@property (nonatomic, readonly, copy) NSArray<FPRadioStation*>* radioFavorites;
// False if the address is already saved.
- (BOOL)addRadioFavoriteNamed:(NSString*)name url:(NSString*)url;
- (void)removeRadioFavorite:(NSInteger)identifier;
- (void)searchRadio:(NSString*)query
          directory:(FPRadioDirectory)directory
         completion:(void (^)(NSArray<FPRadioResult*>* results))completion;
// The address a found station plays from, or nil if it could not be found.
- (void)resolveRadioResult:(FPRadioResult*)result completion:(void (^)(NSString* _Nullable url))completion
    NS_SWIFT_NAME(resolveRadioResult(_:completion:));

// The stations of an M3U, M3U8 or PLS playlist file, added to the saved ones
// (those already saved are passed over). `skipped` gets how many were.
- (NSInteger)importRadioFavoritesFromFile:(NSString*)path skipped:(nullable NSInteger*)skipped
    NS_SWIFT_NAME(importRadioFavorites(fromFile:skipped:));
// The saved stations, written as an M3U file. False if it could not be written.
- (BOOL)exportRadioFavoritesToFile:(NSString*)path NS_SWIFT_NAME(exportRadioFavorites(toFile:));

// ---- Podcasts ---------------------------------------------------------------
@property (nonatomic, readonly, copy) NSArray<FPPodcast*>* podcasts;
// The feeds of an OPML file, subscribed to (those already subscribed to are passed
// over). Returns how many were added, or -1 if the file has no feeds in it.
- (NSInteger)importPodcastsFromOPML:(NSString*)path skipped:(nullable NSInteger*)skipped
    NS_SWIFT_NAME(importPodcasts(fromOPML:skipped:));
// The subscriptions, written as an OPML file. False if it could not be written.
- (BOOL)exportPodcastsToOPML:(NSString*)path NS_SWIFT_NAME(exportPodcasts(toOPML:));
- (BOOL)isSubscribedToFeed:(NSString*)feedURL;
- (void)subscribeToPodcastNamed:(NSString*)name feedURL:(NSString*)feedURL
    NS_SWIFT_NAME(subscribeToPodcast(named:feedURL:));
- (void)removePodcast:(NSInteger)identifier;
- (void)searchPodcasts:(NSString*)query completion:(void (^)(NSArray<FPPodcastResult*>* results))completion;
// A feed's episodes, newest first as the feed has them. On failure `episodes` is
// empty and `error` says what went wrong.
- (void)loadEpisodesOfFeed:(NSString*)feedURL
                completion:(void (^)(NSString* title, NSArray<FPEpisode*>* episodes, NSString* _Nullable error))completion;
// Plays the episodes as a playlist, starting with the one at `index`.
- (void)playEpisodes:(NSArray<FPEpisode*>*)episodes startingAt:(NSInteger)index
    NS_SWIFT_NAME(playEpisodes(_:startingAt:));

@end

NS_ASSUME_NONNULL_END
