#pragma once
#ifndef FASTPLAY_PLAYER_H
#define FASTPLAY_PLAYER_H

#include <windows.h>
#include <string>
#include "bass.h"

// BASS initialization
bool InitBass(HWND hwnd);
void FreeBass();
void LoadBassPlugins();
std::wstring GetLoadedPluginsInfo();

// Playback control
bool LoadFile(const wchar_t* path);
bool LoadURL(const wchar_t* url);
bool IsURL(const wchar_t* path);
void PlayPause();
void Play();
void Pause();
void Stop();
void FreeCurrentStream();

// Seeking
void Seek(double seconds);
void SeekTracks(int tracks);
void SeekToPosition(double seconds);
double GetCurrentPosition();

// Chapter support
void ParseChapters(HSTREAM stream);
bool SeekToNextChapter();
bool SeekToPrevChapter();
int GetCurrentChapterIndex();

// Volume
void SetVolume(float vol);
void ToggleMute();

// Track navigation
void NextTrack(bool autoPlay = true);
void PrevTrack();
void PlayTrack(int index, bool autoPlay = true);

// Track end callback
void CALLBACK OnTrackEnd(HSYNC handle, DWORD channel, DWORD data, void* user);

// Stream metadata change callback (for internet radio)
void CALLBACK OnMetaChange(HSYNC handle, DWORD channel, DWORD data, void* user);
void AnnounceStreamMetadata();

// Device management
bool ReinitBass(int device);
int FindDeviceByName(const std::wstring& name);
std::wstring GetDeviceName(int device);
void ShowAudioDeviceMenu(HWND hwnd);
void SelectAudioDevice(int deviceIndex);

// Speak functions
void SpeakElapsed();
void SpeakRemaining();
void SpeakTotal();

// Tag reading functions (speak ID3/metadata tags)
void SpeakTagTitle();
void SpeakTagArtist();
void SpeakTagAlbum();
void SpeakTagYear();
void SpeakTagTrack();
void SpeakTagGenre();
void SpeakTagComment();
void SpeakTagBitrate();
int GetCurrentBitrate();  // Returns current stream bitrate in kbps, or 0 if unavailable
void SpeakTagDuration();
void SpeakTagFilename();

// Tag retrieval functions (return tag text for display)
std::wstring GetTagTitle();
std::wstring GetTagArtist();
std::wstring GetTagAlbum();
std::wstring GetTagYear();
std::wstring GetTagTrack();
std::wstring GetTagGenre();
std::wstring GetTagComment();
std::wstring GetTagBitrate();
std::wstring GetTagDuration();
std::wstring GetTagFilename();

// Recording functions
void ToggleRecording();
void StopRecording();

#endif // FASTPLAY_PLAYER_H
