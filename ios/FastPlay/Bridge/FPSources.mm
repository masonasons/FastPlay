// Internet radio and podcasts for the iPhone app: see FPSources.h.

#import "FPSources.h"

#include "database.h"
#include "globals.h"
#include "player.h"
#include "playlist_io.h"
#include "podcast.h"
#include "radio.h"
#include "utils.h"

#include <string>
#include <vector>

namespace {

NSString* ToNS(const std::wstring& text) {
    return [NSString stringWithUTF8String:WideToUtf8(text).c_str()] ?: @"";
}

std::wstring ToWide(NSString* text) {
    return Utf8ToWide(text.UTF8String ?: "");
}

// Network lookups, off the main thread
dispatch_queue_t LookupQueue() {
    return dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
}

}  // namespace

@implementation FPRadioStation
- (instancetype)initWithStation:(const RadioStation&)station {
    if ((self = [super init])) {
        _identifier = station.id;
        _name = [ToNS(station.name) copy];
        _url = [ToNS(station.url) copy];
    }
    return self;
}
@end

@implementation FPRadioResult {
@public
    RadioSearchResult _result;
}
- (instancetype)initWithResult:(const RadioSearchResult&)result {
    if ((self = [super init])) {
        _result = result;
        _name = [ToNS(result.name) copy];
        NSMutableArray<NSString*>* parts = [NSMutableArray array];
        if (!result.country.empty()) [parts addObject:ToNS(result.country)];
        if (!result.codec.empty()) [parts addObject:ToNS(result.codec)];
        if (result.bitrate > 0) [parts addObject:[NSString stringWithFormat:@"%d kbps", result.bitrate]];
        _detail = [[parts componentsJoinedByString:@", "] copy];
    }
    return self;
}
@end

@implementation FPPodcast
- (instancetype)initWithSubscription:(const PodcastSubscription&)subscription {
    if ((self = [super init])) {
        _identifier = subscription.id;
        _name = [ToNS(subscription.name) copy];
        _feedURL = [ToNS(subscription.feedUrl) copy];
    }
    return self;
}
@end

@implementation FPPodcastResult
- (instancetype)initWithResult:(const PodcastSearchResult&)result {
    if ((self = [super init])) {
        _name = [ToNS(result.name) copy];
        _author = [ToNS(result.artistName) copy];
        _feedURL = [ToNS(result.feedUrl) copy];
    }
    return self;
}
@end

@implementation FPEpisode
- (instancetype)initWithEpisode:(const PodcastEpisode&)episode {
    if ((self = [super init])) {
        _title = [ToNS(episode.title) copy];
        _date = [ToNS(episode.pubDate) copy];
        _summary = [ToNS(CleanPodcastDescription(episode.description)) copy];
        _audioURL = [ToNS(episode.audioUrl) copy];
        _duration = episode.durationSeconds;
    }
    return self;
}
@end

@implementation FPEngine (Sources)

// ---- Radio ------------------------------------------------------------------

- (NSArray<FPRadioStation*>*)radioFavorites {
    NSMutableArray<FPRadioStation*>* list = [NSMutableArray array];
    for (const RadioStation& station : GetRadioFavorites()) {
        [list addObject:[[FPRadioStation alloc] initWithStation:station]];
    }
    return list;
}

- (BOOL)addRadioFavoriteNamed:(NSString*)name url:(NSString*)url {
    std::wstring address = ToWide(url);
    for (const RadioStation& station : GetRadioFavorites()) {
        if (station.url == address) return NO;
    }
    return AddRadioStation(ToWide(name), address) >= 0;
}

- (void)removeRadioFavorite:(NSInteger)identifier {
    RemoveRadioStation(static_cast<int>(identifier));
}

- (void)searchRadio:(NSString*)query
          directory:(FPRadioDirectory)directory
         completion:(void (^)(NSArray<FPRadioResult*>*))completion {
    std::wstring text = ToWide(query);
    dispatch_async(LookupQueue(), ^{
        std::vector<RadioSearchResult> results;
        switch (directory) {
            case FPRadioDirectoryTuneIn: SearchTuneIn(text, results); break;
            case FPRadioDirectoryIHeartRadio: SearchIHeartRadio(text, results); break;
            default: SearchRadioBrowser(text, L"", results); break;
        }
        NSMutableArray<FPRadioResult*>* list = [NSMutableArray array];
        for (const RadioSearchResult& result : results) {
            [list addObject:[[FPRadioResult alloc] initWithResult:result]];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(list);
        });
    });
}

- (void)resolveRadioResult:(FPRadioResult*)result completion:(void (^)(NSString*))completion {
    RadioSearchResult found = result->_result;
    dispatch_async(LookupQueue(), ^{
        std::wstring url = ResolveRadioStreamUrl(found);
        NSString* address = url.empty() ? nil : ToNS(url);
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(address);
        });
    });
}

- (NSInteger)importRadioFavoritesFromFile:(NSString*)path skipped:(NSInteger*)skipped {
    RadioImportResult result = ImportRadioFavorites(ToWide(path));
    if (skipped) *skipped = result.skipped;
    return result.imported;
}

- (BOOL)exportRadioFavoritesToFile:(NSString*)path {
    return ExportRadioFavorites(ToWide(path), GetRadioFavorites());
}

// ---- Podcasts ---------------------------------------------------------------

- (NSInteger)importPodcastsFromOPML:(NSString*)path skipped:(NSInteger*)skipped {
    std::vector<OpmlFeed> feeds = ParseOpmlFile(ToWide(path));
    if (skipped) *skipped = 0;
    if (feeds.empty()) return -1;
    std::vector<PodcastSubscription> existing = GetPodcastSubscriptions();
    NSInteger added = 0;
    for (const OpmlFeed& feed : feeds) {
        bool have = false;
        for (const PodcastSubscription& subscription : existing) {
            if (subscription.feedUrl == feed.feedUrl) have = true;
        }
        if (have || feed.feedUrl.empty()) {
            if (skipped) (*skipped)++;
            continue;
        }
        if (AddPodcastSubscription(feed.title.empty() ? feed.feedUrl : feed.title, feed.feedUrl) >= 0) {
            added++;
            PodcastSubscription now;
            now.feedUrl = feed.feedUrl;
            existing.push_back(now);  // a feed listed twice in the file is added once
        }
    }
    return added;
}

- (BOOL)exportPodcastsToOPML:(NSString*)path {
    return ExportOpmlFile(ToWide(path), GetPodcastSubscriptions());
}

- (NSArray<FPPodcast*>*)podcasts {
    NSMutableArray<FPPodcast*>* list = [NSMutableArray array];
    for (const PodcastSubscription& subscription : GetPodcastSubscriptions()) {
        [list addObject:[[FPPodcast alloc] initWithSubscription:subscription]];
    }
    return list;
}

- (BOOL)isSubscribedToFeed:(NSString*)feedURL {
    std::wstring feed = ToWide(feedURL);
    for (const PodcastSubscription& subscription : GetPodcastSubscriptions()) {
        if (subscription.feedUrl == feed) return YES;
    }
    return NO;
}

- (void)subscribeToPodcastNamed:(NSString*)name feedURL:(NSString*)feedURL {
    if ([self isSubscribedToFeed:feedURL]) return;
    AddPodcastSubscription(ToWide(name), ToWide(feedURL));
}

- (void)removePodcast:(NSInteger)identifier {
    RemovePodcastSubscription(static_cast<int>(identifier));
}

- (void)searchPodcasts:(NSString*)query completion:(void (^)(NSArray<FPPodcastResult*>*))completion {
    std::wstring text = ToWide(query);
    dispatch_async(LookupQueue(), ^{
        std::vector<PodcastSearchResult> results;
        SearchItunesPodcasts(text, results);
        NSMutableArray<FPPodcastResult*>* list = [NSMutableArray array];
        for (const PodcastSearchResult& result : results) {
            if (!result.feedUrl.empty()) [list addObject:[[FPPodcastResult alloc] initWithResult:result]];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(list);
        });
    });
}

- (void)loadEpisodesOfFeed:(NSString*)feedURL
                completion:(void (^)(NSString*, NSArray<FPEpisode*>*, NSString*))completion {
    std::wstring feed = ToWide(feedURL);
    // A protected feed's credentials, if it is one subscribed to
    std::wstring username, password;
    for (const PodcastSubscription& subscription : GetPodcastSubscriptions()) {
        if (subscription.feedUrl == feed) {
            username = subscription.username;
            password = subscription.password;
        }
    }
    dispatch_async(LookupQueue(), ^{
        std::wstring title;
        std::vector<PodcastEpisode> episodes;
        PodcastFetchDiag diag;
        bool ok = ParsePodcastFeed(feed, title, episodes, username, password, &diag);
        NSMutableArray<FPEpisode*>* list = [NSMutableArray array];
        for (const PodcastEpisode& episode : episodes) {
            [list addObject:[[FPEpisode alloc] initWithEpisode:episode]];
        }
        NSString* error = ok ? nil : ToNS(BuildPodcastDiagMessage(feed, diag));
        NSString* name = ToNS(title);
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(name, list, error);
        });
    });
}

- (void)playEpisodes:(NSArray<FPEpisode*>*)episodes startingAt:(NSInteger)index {
    if (episodes.count == 0) return;
    g_playlist.clear();
    for (FPEpisode* episode in episodes) {
        std::wstring url = ToWide(episode.audioURL);
        SetTrackName(url, ToWide(episode.title));  // an episode's address says nothing to a listener
        g_playlist.push_back(url);
    }
    g_currentTrack = -1;
    PlayTrack(static_cast<int>(MAX(0, MIN(index, static_cast<NSInteger>(episodes.count) - 1))));
}

@end
