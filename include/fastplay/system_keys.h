#pragma once
#ifndef FASTPLAY_SYSTEM_KEYS_H
#define FASTPLAY_SYSTEM_KEYS_H

// System-wide keys on macOS (src/platform/system_keys_mac.mm): global hotkeys, and the
// media keys and Now Playing controls. On Windows wxWidgets registers the hotkeys and
// media keys itself.

#include <string>

#ifdef __APPLE__

// The Windows virtual key code (keycodes.h) for a macOS key code (kVK_*), or 0.
unsigned MacKeyCodeToVirtualKey(unsigned macKeyCode);

// Global hotkeys, given as MOD_* flags and a virtual key code. MOD_CONTROL is
// Command, MOD_ALT is Option and MOD_WIN is Control. The handler runs on the UI
// thread with the hotkey's id, when it is pressed and when it is let go.
void SetSystemHotkeyHandler(void (*handler)(int id, bool pressed));
bool RegisterSystemHotkey(int id, unsigned modifiers, unsigned vk);
void UnregisterSystemHotkey(int id);

// The media keys, headphone buttons and Control Center's Now Playing controls. The
// handler runs on the UI thread with the IDM_* command to run. `seekHandler` gets
// the rewind and fast forward keys (and next or previous held down): direction -1
// or 1, pressed when the key goes down and not when it comes up.
void StartMediaKeys(void (*handler)(int commandId), void (*seekHandler)(int direction, bool pressed));
void StopMediaKeys();

// Replace previous/next with skip buttons, using the player's seek commands.
// Off by default. `interval` is the displayed time step in seconds, or 0 for a
// chapter/track step, which has no fixed duration. Call on the UI thread.
void SetMediaSeekControls(bool enabled, double interval);

// What Control Center shows under Now Playing. macOS sends the media keys to the app
// that last reported something playing.
void UpdateNowPlaying(const std::wstring& title, double duration, double position, bool playing);
void ClearNowPlaying();

#endif

#endif // FASTPLAY_SYSTEM_KEYS_H
