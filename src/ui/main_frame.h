#pragma once
#ifndef FASTPLAY_MAIN_FRAME_H
#define FASTPLAY_MAIN_FRAME_H

#include <wx/wx.h>
#include <memory>
#include <string>
#include <vector>

class TrayIcon;

// The main window: menu bar, keyboard shortcuts, status bar, tray icon and global
// hotkeys. Every command, from whichever of those it comes, runs through RunCommand.
class MainFrame : public wxFrame {
public:
    MainFrame();
    ~MainFrame() override;

    // Start audio, the database, effects, speech and hotkeys, and play any files
    // FastPlay was started with. False if audio could not start (already reported).
    bool Initialize();

    // Run an IDM_* command. `param` as in PostCommand().
    void RunCommand(int id, int param = 0);

    void UpdateTitle();
    void UpdateStatus();

    // Tray
    void HideToTray();
    void RestoreFromTray();
    void ToggleWindow();

    // Global hotkeys (media keys and the user's own).
    void RegisterGlobalHotkeys();
    void UnregisterGlobalHotkeys();

    // The main window's keyboard shortcuts, with the user's local hotkeys ahead of
    // FastPlay's own; built again when the hotkeys change.
    void BuildAccelerators();

    // A file handed over by another FastPlay started with it (e.g. from Explorer).
    void ReceiveFile(const std::wstring& path);

    void StartScheduleDurationTimer(int ms);
    void StopScheduleDurationTimer();

    // Popup menus opened from the keyboard.
    void ShowEffectPresetsMenu();
    void ShowAudioDeviceMenu();

private:
    void BuildMenuBar();
    void RebuildRecentFilesMenu();
    void OnMenu(wxCommandEvent& event);
    void OnMenuOpen(wxMenuEvent& event);
    void OnIconize(wxIconizeEvent& event);
    void OnClose(wxCloseEvent& event);
    void OnHotkey(wxKeyEvent& event);
    // A global hotkey pressed, or on macOS, let go of (`pressed` false)
    void RunHotkey(int id, bool pressed = true);
    // The global seek hotkeys scrub while held too (in spring and tape seeking)
    void StopHotkeyScrub();
    int m_scrubHotkey = 0;  // the hotkey held for scrubbing, if any
#ifdef __WXMSW__
    // Windows only says when a hotkey goes down (and repeats): its key is watched
    // to see it come up
    unsigned m_scrubHotkeyVk = 0;
    wxTimer m_scrubHotkeyPoll;
#endif
    // The rewind and fast forward media keys. Held, they seek as a seek key held
    // does: scrubbing in spring and tape seeking, jumps repeating in jump seeking.
    // Windows only says such a key was pressed, so there each press is one seek.
    void MediaSeek(int direction, bool pressed);
    int m_mediaSeekDirection = 0;  // the media seek key held, if any
    wxTimer m_mediaSeekRepeat;     // repeats the jumps while it is held
#ifdef __WXMSW__
    WXLRESULT MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) override;
    // An APPCOMMAND_* from the keyboard; true if FastPlay used it
    bool RunAppCommand(int command);
    // The shell's message for app commands no other window used, so the keys
    // work with FastPlay in the background
    unsigned m_shellHookMessage = 0;
#endif
    void OnBatchTimer(wxTimerEvent& event);
    void SeekBackOrForward(int direction);
    // Spring and tape seeking: Left or Right held scrubs, from the key going down to
    // it coming up. True if the key was used for that.
    bool HandleScrubKey(unsigned modifiers, unsigned vk, bool down, bool repeat);
    unsigned m_scrubKey = 0;  // the arrow held for scrubbing, if any
#ifdef __WXMSW__
    // Its coming up on the keyboard, watched once a screen reader has passed on a
    // release while it is still held
    wxTimer m_scrubArrowPoll;
#endif
#ifdef __WXMSW__
    // The keyboard shortcuts as a native accelerator table, so punctuation keys
    // are matched by their key position (VK_OEM_*) on every keyboard layout.
    bool MSWTranslateMessage(WXMSG* msg) override;
    void* m_nativeAccel = nullptr;
#else
    // The keyboard shortcuts, for the keys caught before macOS dispatches them
    // (see menu_keys_mac.mm). True if the key ran a command.
    bool RunShortcut(unsigned modifiers, unsigned vk);
    std::vector<wxAcceleratorEntry> m_shortcuts;
#endif

    wxMenu* m_recentMenu = nullptr;
    wxMenuItem* m_shuffleItem = nullptr;
    wxTimer m_titleTimer;
    wxTimer m_schedulerTimer;
    wxTimer m_batchTimer;
    wxTimer m_durationTimer;
    std::unique_ptr<TrayIcon> m_tray;
    bool m_hotkeysRegistered = false;
    std::wstring m_titleChapter;  // the chapter the window title names, to see it change
#ifdef __WXOSX__
    std::wstring m_nowPlayingTitle;  // for Control Center's Now Playing
#endif
};

#endif // FASTPLAY_MAIN_FRAME_H
