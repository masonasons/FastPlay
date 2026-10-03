#pragma once
#ifndef FASTPLAY_PLAYLIST_IO_H
#define FASTPLAY_PLAYLIST_IO_H

// Local files and playlist files: which extensions FastPlay plays, expanding a file
// or folder into a list of them, and reading .m3u / .m3u8 / .pls playlists.

#include <map>
#include <string>
#include <vector>

bool IsSupportedAudioExt(const std::wstring& ext);

// Replace outFiles with every supported file in filePath's folder, and return the
// index of filePath among them.
int ExpandFileToFolder(const std::wstring& filePath, std::vector<std::wstring>& outFiles);

// Append every supported file under folder (recursively) to files.
void AddFilesFromFolder(const std::wstring& folder, std::vector<std::wstring>& files);

bool IsPlaylistFile(const std::wstring& path);
std::vector<std::wstring> ParsePlaylist(const std::wstring& playlistPath);

// Names for playlist entries whose path says nothing to a listener (a YouTube
// stream's address, say), and the name to show or say for any entry: its given
// name, otherwise its file name.
void SetTrackName(const std::wstring& path, const std::wstring& name);
std::wstring GetTrackName(const std::wstring& path);

// Metadata supplied by a service when the audio itself has no usable tags.
// Kept by playback path so stopping, replaying and switching devices retain it.
struct TrackMetadata {
	std::map<std::string, std::string> tags;  // common tag names, UTF-8 values
	std::wstring sourceUrl;
	double duration = 0;
	int bitrate = 0;  // kbps
};
void SetTrackMetadata(const std::wstring& path, const TrackMetadata& metadata);
TrackMetadata GetTrackMetadata(const std::wstring& path);

// A line of a playlist file: UTF-8 if it is valid UTF-8, otherwise the system's
// legacy code page (Latin-1 outside Windows).
std::wstring PlaylistLineToWide(const char* line);

#endif // FASTPLAY_PLAYLIST_IO_H
