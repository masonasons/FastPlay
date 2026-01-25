#pragma once
#ifndef FASTPLAY_TRAY_H
#define FASTPLAY_TRAY_H

#include <windows.h>

// Tray icon management
void CreateTrayIcon(HWND hwnd);
void RemoveTrayIcon();
void ShowTrayMenu(HWND hwnd);

// Window visibility
void HideToTray(HWND hwnd);
void RestoreFromTray(HWND hwnd);
void ToggleWindow(HWND hwnd);

#endif // FASTPLAY_TRAY_H
