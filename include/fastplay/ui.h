#pragma once
#ifndef FASTPLAY_UI_H
#define FASTPLAY_UI_H

#include <windows.h>
#include <string>
#include <vector>
#include "database.h"

// Playlist file handling
bool IsPlaylistFile(const std::wstring& path);
std::vector<std::wstring> ParsePlaylist(const std::wstring& playlistPath);

// Status bar
void CreateStatusBar(HWND hwnd, HINSTANCE hInstance);
void UpdateStatusBar();
void UpdateWindowTitle();

// File associations
void RegisterAllFileTypes();
void UnregisterAllFileTypes();

// Dialogs
void ShowOpenDialog();
void ShowAddFolderDialog();
void ShowPlaylistDialog();
void ShowOpenURLDialog();
void ShowJumpToTimeDialog();
void ShowEffectPresetsMenu(HWND hwnd);
void ShowSaveEffectPresetDialog();
void ShowOptionsDialog();
void ShowBookmarksDialog();
void ShowSongHistoryDialog();
void ShowRadioDialog();
void AddCurrentStreamToFavorites();
void ShowSchedulerDialog();
void ShowPodcastDialog();
void CheckScheduledEvents();
void CalculateNextScheduleTime(int id, int64_t lastRun, ScheduleRepeat repeat);
void HandleScheduledDurationEnd();
INT_PTR CALLBACK OptionsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void ShowTabControls(HWND hwnd, int tab);
void ShowTagDialog(const wchar_t* title, const std::wstring& text);
void NotifyPlaylistTrackChanged();

#endif // FASTPLAY_UI_H
