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

// Dialogs
void ShowOpenDialog();
void ShowAddFolderDialog();
void ShowPlaylistDialog();
void ShowOpenURLDialog();
void ShowJumpToTimeDialog();
void ShowOptionsDialog();
void ShowBookmarksDialog();
void ShowRadioDialog();
void ShowSchedulerDialog();
void CheckScheduledEvents();
void CalculateNextScheduleTime(int id, int64_t lastRun, ScheduleRepeat repeat);
void HandleScheduledDurationEnd();
INT_PTR CALLBACK OptionsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void ShowTabControls(HWND hwnd, int tab);
void ShowTagDialog(const wchar_t* title, const std::wstring& text);

#endif // FASTPLAY_UI_H
