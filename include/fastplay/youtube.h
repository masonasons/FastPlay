#pragma once
#ifndef FASTPLAY_YOUTUBE_H
#define FASTPLAY_YOUTUBE_H

#include <windows.h>
#include <string>
#include <vector>

// YouTube search result
struct YouTubeResult {
    std::wstring videoId;
    std::wstring title;
    std::wstring channel;
    std::wstring duration;    // Human-readable duration
    std::wstring uploadDate;  // Human-readable upload date
    bool isPlaylist = false;
    bool isChannel = false;
};

// Search YouTube using API or yt-dlp fallback
// Returns results, nextPageToken is set if more results available
bool YouTubeSearch(const std::wstring& query, std::vector<YouTubeResult>& results,
                   std::wstring& nextPageToken, const std::wstring& pageToken = L"");

// Get contents of a playlist or channel
bool YouTubeGetPlaylistContents(const std::wstring& playlistId, std::vector<YouTubeResult>& results,
                                std::wstring& nextPageToken, const std::wstring& pageToken = L"");

// Get audio stream URL for a video using yt-dlp
bool YouTubeGetStreamURL(const std::wstring& videoId, std::wstring& streamUrl);

// Download YouTube audio to temp file (more reliable than streaming)
bool YouTubeDownloadAudio(const std::wstring& videoId, std::wstring& filePath);

// Start streaming - downloads and returns path when complete (blocking)
bool YouTubeStartStream(const std::wstring& videoId, std::wstring& filePath);

// Async download functions
bool YouTubeStartDownload(const std::wstring& videoId);  // Start download, returns immediately
bool YouTubeIsDownloadComplete();                         // Check if download finished
bool YouTubeGetDownloadResult(std::wstring& filePath);   // Get result after completion

// Async search functions
bool YouTubeStartSearch(const std::wstring& query, bool isPlaylist, const std::wstring& playlistId,
                        const std::wstring& pageToken, bool isLoadMore);  // Start search, returns immediately
bool YouTubeIsSearchComplete();                                            // Check if search finished
bool YouTubeGetSearchResult(std::vector<YouTubeResult>& results, std::wstring& nextPageToken);  // Get results
bool YouTubeWasLoadMore();                                                 // Check if last search was "load more"

// Clean up temp files (call on startup and exit)
void YouTubeCleanup();

// Check if input looks like a YouTube URL
bool IsYouTubeURL(const std::wstring& input);

// Parse YouTube URL to extract video/playlist/channel ID
bool ParseYouTubeURL(const std::wstring& url, std::wstring& id, bool& isPlaylist, bool& isChannel);

// Show YouTube search dialog (modeless)
void ShowYouTubeDialog(HWND parent);

// Get YouTube dialog handle (for message loop)
HWND GetYouTubeDialog();

// YouTube dialog procedure
INT_PTR CALLBACK YouTubeDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

#endif // FASTPLAY_YOUTUBE_H
