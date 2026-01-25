#pragma once
#ifndef FASTPLAY_HOTKEYS_H
#define FASTPLAY_HOTKEYS_H

#include <windows.h>
#include <string>

// Hotkey registration
void RegisterGlobalHotkeys();
void UnregisterGlobalHotkeys();

// Hotkey persistence
void LoadHotkeys();
void SaveHotkeys();

// Hotkey formatting
std::wstring FormatHotkey(UINT modifiers, UINT vk);

// Hotkey dialog
INT_PTR CALLBACK HotkeyDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

#endif // FASTPLAY_HOTKEYS_H
