#pragma once
#ifndef FASTPLAY_SETTINGS_H
#define FASTPLAY_SETTINGS_H

#include <string>

// Config path initialization
void InitConfigPath();

// Settings load/save
void LoadSettings();
void SaveSettings();
void LoadDSPSettings();  // Call after InitEffects()

// Playback state persistence
void SavePlaybackState();
void LoadPlaybackState();

// Per-file position tracking
void SaveFilePosition(const std::wstring& filePath);
double LoadFilePosition(const std::wstring& filePath);

// Seek amount cycling
void CycleSeekAmount(int direction);
double GetCurrentSeekAmount();
bool IsSeekAmountAvailable(int index);
void SpeakSeekAmount();

#endif // FASTPLAY_SETTINGS_H
