#pragma once
#ifndef FASTPLAY_SYSTEM_KEYS_H
#define FASTPLAY_SYSTEM_KEYS_H

// System-wide keys on macOS (src/platform/system_keys_mac.mm): global hotkeys, and the
// media keys and Now Playing controls. On Windows wxWidgets registers the hotkeys and
// media keys itself; there, the arrows are watched for scrubbing (below).

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

// The position slider on the lock screen and in Control Center: `handler` runs on
// the UI thread with the time in seconds to go to. Offered only while what is
// playing has a length (not a live stream). Without a handler the slider is shown
// but does nothing.
void SetMediaPositionHandler(void (*handler)(double seconds));

// Replace previous/next with skip buttons, using the player's seek commands.
// Off by default. `interval` is the displayed time step in seconds, or 0 for a
// chapter/track step, which has no fixed duration. Call on the UI thread.
void SetMediaSeekControls(bool enabled, double interval);

// What Control Center shows under Now Playing. macOS sends the media keys to the app
// that last reported something playing.
void UpdateNowPlaying(const std::wstring& title, double duration, double position, bool playing);
void ClearNowPlaying();

#endif

#ifdef _WIN32

// The arrow keys as the keyboard has them (src/platform/held_arrows_windows.cpp).
// A screen reader such as JAWS takes the arrows and passes one held on as a press
// and a release for every repeat, so the window's own key messages cannot tell it
// is still held. A low-level keyboard hook sees the keyboard's own presses ahead
// of it. Watched while FastPlay's window is in front.
void StartWatchingArrows();
void StopWatchingArrows();
// True while `vk` (VK_LEFT or VK_RIGHT) is held down on the keyboard, as far as
// the hook has seen.
bool ArrowHeld(unsigned vk);

#endif

#endif // FASTPLAY_SYSTEM_KEYS_H
