#pragma once
#ifndef FASTPLAY_YOUTUBE_H
#define FASTPLAY_YOUTUBE_H

// YouTube: search (the YouTube Data API when there is a key, yt-dlp otherwise),
// playlist and channel listings, favorites' latest uploads, and getting a video
// ready to play. Everything here blocks, often for seconds: call it from a worker
// thread.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "playlist_io.h"

enum class YouTubeKind { Video, Channel, Playlist };

// A search result or listing entry
struct YouTubeResult {
    YouTubeKind kind = YouTubeKind::Video;
    std::wstring id;          // video, channel (UC...) or playlist ID
    std::wstring title;
    std::wstring channel;     // the channel a video or playlist belongs to
    std::wstring channelId;   // its ID, when known
    std::wstring duration;    // Human-readable duration ("live" for a live stream)
};

// A video ready to play: a stream address, or a downloaded audio file when there
// is nothing to stream.
struct YouTubeMedia {
	std::wstring url;
	std::wstring file;
	std::wstring title;
	std::wstring channel;
	TrackMetadata metadata;
};

// A channel or playlist, as identified from its URL
struct YouTubeListInfo {
    YouTubeKind kind = YouTubeKind::Channel;
    std::wstring id;
    std::wstring name;
    std::wstring channel;  // for a playlist: the channel that made it
};

// Short progress messages worth saying while something slow happens (a first-time
// download of the YouTube tools, say). Called on the worker thread.
using YouTubeStatus = std::function<void(const std::wstring& message)>;

// Search YouTube, a page at a time: pass the nextPageToken from the last page as
// pageToken for the next one (empty when there are no more). `error` explains a
// failure.
bool YouTubeSearch(const std::wstring& query, std::vector<YouTubeResult>& results,
                   std::wstring& nextPageToken, const std::wstring& pageToken,
                   std::wstring& error, const YouTubeStatus& status = nullptr);

// The videos of a playlist or channel page (a YouTube URL), a page at a time.
bool YouTubeGetListContents(const std::wstring& listUrl, std::vector<YouTubeResult>& results,
                            std::wstring& nextPageToken, const std::wstring& pageToken,
                            std::wstring& error, const YouTubeStatus& status = nullptr);

// Get a video ready to play: its audio stream, or where YouTube offers none, its
// audio downloaded (kept for a week). Managed mode downloads yt-dlp and deno on
// first use; installed mode uses the tools selected in Settings > YouTube.
bool YouTubePrepare(const std::wstring& videoId, YouTubeMedia& media, std::wstring& error,
                    const YouTubeStatus& status = nullptr);

// What channel or playlist a URL is
bool YouTubeIdentify(const std::wstring& url, YouTubeListInfo& info, std::wstring& error,
                     const YouTubeStatus& status = nullptr);

// The channel a video belongs to (as a YouTubeListInfo of kind Channel)
bool YouTubeVideoChannel(const std::wstring& videoId, YouTubeListInfo& info, std::wstring& error,
                         const YouTubeStatus& status = nullptr);

// When the newest video of a channel or playlist was published (Unix time), from
// YouTube's feed for it. False if the feed could not be read.
bool YouTubeLatestUpload(YouTubeKind kind, const std::wstring& id, int64_t& published);

// A channel's or playlist's feed: its name (and a playlist's channel), when its
// newest video was published, and that video's title. False if the feed could not
// be read.
bool YouTubeReadFeed(YouTubeKind kind, const std::wstring& id, YouTubeListInfo& info, int64_t& published,
                     std::wstring* newestTitle = nullptr);

// A channel or playlist from a line of text, as in a list of channels to import: a
// channel or playlist URL, an @handle or a channel ID. Also gives the newest
// upload's time when the feed has it (0 otherwise).
bool YouTubeResolveFavorite(const std::wstring& text, YouTubeListInfo& info, int64_t& published,
                            std::wstring& error, const YouTubeStatus& status = nullptr);

// The URL listing a channel's videos or a playlist's, by ID
std::wstring YouTubeListUrl(YouTubeKind kind, const std::wstring& id);

// A cookies.txt (Netscape format, as browser extensions export it) for yt-dlp to
// use, for videos YouTube only shows to a signed-in account. Importing copies it
// into FastPlay's data folder.
// Gets yt-dlp and deno ready (downloading or updating them), so the next video
// starts sooner. Blocks.
void YouTubePrepareTools(const YouTubeStatus& status = nullptr);

bool YouTubeImportCookies(const std::wstring& path, std::wstring& error);
void YouTubeRemoveCookies();
bool YouTubeHasCookies();

// Download a video (or a whole playlist) to keep, as Options > YouTube Downloads
// says: audio or video, format, naming and extras. Downloads are queued and run
// one at a time in the background; FastPlay says when each starts, finishes or
// fails. Call on the UI thread.
void YouTubeDownload(const std::wstring& url, const std::wstring& title);

// Where downloads go: the folder chosen in Options, or FastPlay in the
// Downloads folder.
std::wstring YouTubeDownloadFolder();

// Remove downloaded videos not played for a week (call on startup and exit)
void YouTubeCleanup();

// Check if input looks like a YouTube URL
bool IsYouTubeURL(const std::wstring& input);

// Parse YouTube URL to extract video/playlist/channel ID
bool ParseYouTubeURL(const std::wstring& url, std::wstring& id, bool& isPlaylist, bool& isChannel);

// The URL listing a playlist's videos, and a channel's (from ParseYouTubeURL's ID,
// which for a channel may be a @handle)
std::wstring YouTubePlaylistUrl(const std::wstring& playlistId);
std::wstring YouTubeChannelUrl(const std::wstring& channelId);

#endif // FASTPLAY_YOUTUBE_H
