#include "ui/main_frame.h"
#include "ui/tray_icon.h"
#include "ui/ui_common.h"
#include "ui/dialogs.h"

#include "globals.h"
#include "commands.h"
#include "app_ui.h"
#include "player.h"
#include "settings.h"
#include "hotkeys.h"
#include "accessibility.h"
#include "effects.h"
#include "database.h"
#include "youtube.h"
#include "library.h"
#include "download_manager.h"
#include "updater.h"
#include "playlist_io.h"
#include "scheduler.h"
#include "audio.h"
#include "utils.h"
#include "keycodes.h"
#include "system_keys.h"
#ifdef __WXOSX__
#include "platform.h"
#endif

#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <chrono>
#include <utility>

static MainFrame* g_mainFrame = nullptr;

MainFrame* GetMainFrame() {
    return g_mainFrame;
}

// How often the status bar is refreshed, and how often schedules are checked.
static const int kTitleIntervalMs = UPDATE_INTERVAL;
static const int kSchedulerIntervalMs = 60000;

// Rewind or fast forward held in jump seeking: the first jump, then one every so
// often, as a key held repeats.
static const int kMediaSeekDelayMs = 400;
static const int kMediaSeekRepeatMs = 200;

// Media keys are registered with ids of their own, clear of the user's hotkeys.
static const int kHotkeyMediaPlayPause = 0x7F00;
static const int kHotkeyMediaStop = 0x7F01;
static const int kHotkeyMediaPrev = 0x7F02;
static const int kHotkeyMediaNext = 0x7F03;

#ifdef __WXOSX__
namespace {

// macOS delivers key presses to the focused view, and a frame with only a menu bar
// and status bar has none: only keys that are also menu shortcuts would work. This
// empty window fills the frame and holds the focus, so every key reaches the
// frame's accelerator table (wxWidgets looks it up from the focused window).
class KeyTarget : public wxWindow {
public:
    explicit KeyTarget(wxWindow* parent)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS) {}
    bool AcceptsFocus() const override { return true; }
    bool AcceptsFocusFromKeyboard() const override { return true; }
};

}  // namespace
#endif

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, APP_NAME, wxDefaultPosition, wxSize(500, 150)),
      m_titleTimer(), m_schedulerTimer(), m_batchTimer(), m_durationTimer() {
    g_mainFrame = this;
    SetIcon(GetAppIcon());

    BuildMenuBar();
    BuildAccelerators();

    // Position (200 px), volume (100 px), state (the rest).
    CreateStatusBar(SB_PART_COUNT);
    const int widths[SB_PART_COUNT] = {200, 100, -1};
    GetStatusBar()->SetStatusWidths(SB_PART_COUNT, widths);

#ifdef __WXOSX__
    (new KeyTarget(this))->SetFocus();
    StartShortcutMonitor(
        this,
        [](unsigned modifiers, unsigned vk) { return g_mainFrame != nullptr && g_mainFrame->RunShortcut(modifiers, vk); },
        [](unsigned modifiers, unsigned vk, bool down, bool repeat) {
            return g_mainFrame != nullptr && g_mainFrame->HandleScrubKey(modifiers, vk, down, repeat);
        });
#endif

    Bind(wxEVT_MENU, &MainFrame::OnMenu, this);
    Bind(wxEVT_MENU_OPEN, &MainFrame::OnMenuOpen, this);
    Bind(wxEVT_ICONIZE, &MainFrame::OnIconize, this);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);
    // An arrow held for scrubbing that comes up in another window: stop anyway
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
#ifdef __WXMSW__
        // The arrows held on the keyboard are watched only while the window is in front
        if (event.GetActive()) {
            StartWatchingArrows();
        } else {
            m_scrubArrowPoll.Stop();
            StopWatchingArrows();
        }
#endif
        if (!event.GetActive() && m_scrubKey) {
            m_scrubKey = 0;
            StopScrubbing();
        }
        event.Skip();
    });
#ifdef __WXMSW__
    Bind(wxEVT_HOTKEY, &MainFrame::OnHotkey, this);
#endif

    m_titleTimer.Bind(wxEVT_TIMER, [](wxTimerEvent&) { UpdateStatusBar(); });
    m_schedulerTimer.Bind(wxEVT_TIMER, [](wxTimerEvent&) { CheckScheduledEvents(); });
    m_batchTimer.Bind(wxEVT_TIMER, &MainFrame::OnBatchTimer, this);
    m_durationTimer.Bind(wxEVT_TIMER, [](wxTimerEvent&) { HandleScheduledDurationEnd(); });
    m_mediaSeekRepeat.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (m_mediaSeekDirection == 0 || IsScrubSeekMode()) return;
        SeekBackOrForward(m_mediaSeekDirection);
        if (m_mediaSeekRepeat.IsOneShot()) m_mediaSeekRepeat.Start(kMediaSeekRepeatMs);
    });
#ifdef __WXMSW__
    m_scrubHotkeyPoll.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (!(::GetAsyncKeyState(static_cast<int>(m_scrubHotkeyVk)) & 0x8000)) StopHotkeyScrub();
    });
    m_scrubArrowPoll.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (m_scrubKey && ArrowHeld(m_scrubKey)) return;
        m_scrubArrowPoll.Stop();
        if (!m_scrubKey) return;
        m_scrubKey = 0;
        StopScrubbing();
    });
#endif
}

MainFrame::~MainFrame() {
#ifdef __WXMSW__
    StopWatchingArrows();
    if (m_nativeAccel) ::DestroyAcceleratorTable(static_cast<HACCEL>(m_nativeAccel));
#endif
#ifdef __WXOSX__
    StopShortcutMonitor();
#endif
    if (g_mainFrame == this) g_mainFrame = nullptr;
}

bool MainFrame::Initialize() {
    if (!InitAudio()) {
        return false;
    }

    InitDatabase();
    StartLibrary();  // brought up to date in the background
    InitEffects();
    LoadDSPSettings();
    InitSpeech();
    RegisterGlobalHotkeys();

    m_shuffleItem->Check(g_shuffle);

    m_titleTimer.Start(kTitleIntervalMs);
    m_schedulerTimer.Start(kSchedulerIntervalMs);
    g_startupTime = TickCountMs();

    if (!g_playlist.empty()) {
        int startIndex = 0;
        if (g_playlist.size() == 1 && (g_loadFolder || IsCdTrack(g_playlist[0]))) {
            std::wstring singleFile = g_playlist[0];
            startIndex = ExpandFileToFolder(singleFile, g_playlist);
        }
        PlayTrack(startIndex);
    }

    UpdateStatus();

    // Check for updates on startup (runs in a background thread)
    CheckForUpdatesOnStartup();
    return true;
}

// ---------------------------------------------------------------------------
// Menu bar and keyboard shortcuts
// ---------------------------------------------------------------------------

void MainFrame::BuildMenuBar() {
    auto* file = new wxMenu();
    file->Append(IDM_FILE_OPEN, "&Open...\tCtrl+O");
    file->Append(IDM_FILE_ADD_FOLDER, "Add &Folder...\tCtrl+Shift+O");
    file->Append(IDM_FILE_PLAYLIST, "&Playlist...\tCtrl+P");
    file->Append(IDM_FILE_OPEN_URL, "Open &URL...\tCtrl+U");
    file->Append(IDM_FILE_YOUTUBE, "&YouTube...\tCtrl+Y");
    file->Append(IDM_FILE_LIBRARY, "&Library...\tCtrl+L");
    file->Append(IDM_FILE_RADIO, "&Radio...\tCtrl+R");
    file->Append(IDM_FILE_ADD_TO_FAVORITES, "&Add Stream to Favorites...\tCtrl+D");
    file->Append(IDM_FILE_PODCAST, "&Podcasts...\tCtrl+Shift+P");
    file->Append(IDM_FILE_SCHEDULE, "&Schedule...\tCtrl+S");
    file->Append(IDM_VIEW_SONG_HISTORY, "Song &History...\tCtrl+Shift+H");
    file->AppendSeparator();
    m_recentMenu = new wxMenu();
    file->AppendSubMenu(m_recentMenu, "Recent &Files");
    RebuildRecentFilesMenu();
    file->AppendSeparator();
#ifdef __WXOSX__
    // Cmd+H is the system's Hide; the menu bar icon stands in for the tray.
    file->Append(IDM_FILE_HIDE_TRAY, "&Hide to Menu Bar");
    // wxWidgets moves these two into the application menu, where macOS keeps them.
    file->Append(wxID_PREFERENCES, "&Settings...\tCtrl+,");
    file->Append(wxID_EXIT, "&Quit FastPlay\tCtrl+Q");
#else
    file->Append(IDM_FILE_HIDE_TRAY, "&Hide to Tray\tCtrl+H");
    file->AppendSeparator();
    file->Append(IDM_TOOLS_OPTIONS, "O&ptions...\tCtrl+,");
    file->AppendSeparator();
    file->Append(IDM_FILE_EXIT, "E&xit\tAlt+F4");
#endif

    auto* play = new wxMenu();
    play->Append(IDM_PLAY_PLAY, "&Play\tX");
    play->Append(IDM_PLAY_PAUSE, "Pa&use\tC");
    play->Append(IDM_PLAY_PLAYPAUSE, "Play/Pause\tSpace");
    play->Append(IDM_PLAY_STOP, "&Stop\tV");
    play->AppendSeparator();
    play->Append(IDM_PLAY_PREV, "P&revious\tZ");
    play->Append(IDM_PLAY_NEXT, "&Next\tB");
    play->AppendSeparator();
    m_shuffleItem = play->AppendCheckItem(IDM_PLAY_SHUFFLE, "S&huffle\tH");
    play->Append(IDM_PLAY_REPEAT_TOGGLE, "R&epeat\tE");
    play->AppendSeparator();
    play->Append(IDM_PLAY_SEEKBACK, "Seek &Backward\tLeft");
    play->Append(IDM_PLAY_SEEKFWD, "Seek &Forward\tRight");
    play->Append(IDM_SEEK_MODE, "Next Seek &Mode\t/");
    play->Append(IDM_PLAY_BEGINNING, "&Beginning\tHome");
    play->Append(IDM_PLAY_JUMPTOTIME, "&Jump to Time...\tJ");
    play->Append(IDM_PLAY_RANDOM, "Jump to Ran&dom Point");
    play->Append(IDM_PLAY_REVERSE, "Re&verse\tShift+V");
    play->Append(IDM_PLAY_GOLIVE, "Go to &Live\tL");
    play->AppendSeparator();
#ifdef __WXOSX__
    // On macOS a menu shortcut is a real key, and plain Up/Down belong to the
    // effect controls; volume is Cmd+Up/Down.
    play->Append(IDM_PLAY_VOLUP, "Volume &Up\tCtrl+Up");
    play->Append(IDM_PLAY_VOLDOWN, "Volume &Down\tCtrl+Down");
#else
    play->Append(IDM_PLAY_VOLUP, "Volume &Up\tUp");
    play->Append(IDM_PLAY_VOLDOWN, "Volume &Down\tDown");
#endif
    play->AppendSeparator();
    play->Append(IDM_PLAY_ELAPSED, "Speak &Elapsed\tCtrl+Shift+E");
    play->Append(IDM_PLAY_REMAINING, "Speak &Remaining\tCtrl+Shift+R");
    play->Append(IDM_PLAY_TOTAL, "Speak &Total\tCtrl+Shift+T");

    auto* help = new wxMenu();
    help->Append(IDM_HELP_README, "&Readme");
    help->Append(IDM_HELP_UPDATES, "Check for &Updates...");
    help->AppendSeparator();
    help->Append(IDM_HELP_PLUGINS, "&Audio Engine...");

    auto* bar = new wxMenuBar();
    bar->Append(file, "&File");
    bar->Append(play, "&Playback");
    bar->Append(help, "&Help");
    SetMenuBar(bar);
}

#ifndef __WXMSW__
// The wxWidgets key code for a hotkey's Windows virtual key code (keycodes.h), or 0.
static int WxKeyFromVirtualKey(unsigned vk) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return static_cast<int>(vk);
    if (vk >= VK_F1 && vk <= VK_F24) return WXK_F1 + static_cast<int>(vk - VK_F1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return WXK_NUMPAD0 + static_cast<int>(vk - VK_NUMPAD0);
    switch (vk) {
        case VK_BACK: return WXK_BACK;
        case VK_TAB: return WXK_TAB;
        case VK_RETURN: return WXK_RETURN;
        case VK_ESCAPE: return WXK_ESCAPE;
        case VK_SPACE: return WXK_SPACE;
        case VK_PRIOR: return WXK_PAGEUP;
        case VK_NEXT: return WXK_PAGEDOWN;
        case VK_END: return WXK_END;
        case VK_HOME: return WXK_HOME;
        case VK_LEFT: return WXK_LEFT;
        case VK_UP: return WXK_UP;
        case VK_RIGHT: return WXK_RIGHT;
        case VK_DOWN: return WXK_DOWN;
        case VK_INSERT: return WXK_INSERT;
        case VK_DELETE: return WXK_DELETE;
        case VK_MULTIPLY: return WXK_NUMPAD_MULTIPLY;
        case VK_ADD: return WXK_NUMPAD_ADD;
        case VK_SUBTRACT: return WXK_NUMPAD_SUBTRACT;
        case VK_DECIMAL: return WXK_NUMPAD_DECIMAL;
        case VK_DIVIDE: return WXK_NUMPAD_DIVIDE;
        case VK_OEM_1: return ';';
        case VK_OEM_PLUS: return '=';
        case VK_OEM_COMMA: return ',';
        case VK_OEM_MINUS: return '-';
        case VK_OEM_PERIOD: return '.';
        case VK_OEM_2: return '/';
        case VK_OEM_3: return '`';
        case VK_OEM_4: return '[';
        case VK_OEM_5: return '\\';
        case VK_OEM_6: return ']';
        case VK_OEM_7: return '\'';
    }
    return 0;
}
#endif

// The keyboard shortcuts. This table is what the keys do; the shortcut text in the
// menus is only a label. On Windows the frame's own table is consulted before the
// menu bar's, so "Volume Up\tUp" in the menu does not take Up away from the effect
// controls; on macOS the keys are caught before the menu bar sees them (RunShortcut).
void MainFrame::BuildAccelerators() {
    const int N = wxACCEL_NORMAL, C = wxACCEL_CTRL, S = wxACCEL_SHIFT;
    std::vector<wxAcceleratorEntry> e = {
        {C, 'O', IDM_FILE_OPEN},
        {C | S, 'O', IDM_FILE_ADD_FOLDER},
        {C, 'P', IDM_FILE_PLAYLIST},
        {C, 'U', IDM_FILE_OPEN_URL},
        {C, 'Y', IDM_FILE_YOUTUBE},
        {C, 'L', IDM_FILE_LIBRARY},
        {C, 'R', IDM_FILE_RADIO},
        {C, 'D', IDM_FILE_ADD_TO_FAVORITES},
        {C | S, 'P', IDM_FILE_PODCAST},
        {C, 'S', IDM_FILE_SCHEDULE},
#ifndef __WXOSX__
        {C, 'H', IDM_FILE_HIDE_TRAY},
#endif
        {C | S, 'H', IDM_VIEW_SONG_HISTORY},
        {C, ',', IDM_TOOLS_OPTIONS},
        {N, WXK_SPACE, IDM_PLAY_PLAYPAUSE},
        {N, WXK_LEFT, IDM_PLAY_SEEKBACK},
        {N, WXK_RIGHT, IDM_PLAY_SEEKFWD},
        {N, WXK_UP, IDM_EFFECT_UP},
        {N, WXK_DOWN, IDM_EFFECT_DOWN},
        {C, WXK_UP, IDM_PLAY_VOLUP},
        {C, WXK_DOWN, IDM_PLAY_VOLDOWN},
        {N, ',', IDM_SEEK_DECREASE},
        {N, '.', IDM_SEEK_INCREASE},
        {N, '/', IDM_SEEK_MODE},
        {C | S, 'E', IDM_PLAY_ELAPSED},
        {C | S, 'R', IDM_PLAY_REMAINING},
        {C | S, 'T', IDM_PLAY_TOTAL},
        {C | S, 'N', IDM_TOGGLE_NORMALIZER},
        // Winamp-style shortcuts
        {N, 'Z', IDM_PLAY_PREV},
        {N, 'X', IDM_PLAY_PLAY},
        {N, 'C', IDM_PLAY_PAUSE},
        {N, 'V', IDM_PLAY_STOP},
        {S, 'V', IDM_PLAY_REVERSE},
        {N, 'B', IDM_PLAY_NEXT},
        {N, 'H', IDM_PLAY_SHUFFLE},
        {N, 'E', IDM_PLAY_REPEAT_TOGGLE},
        {N, 'P', IDM_EFFECT_PRESETS},
        {N, WXK_HOME, IDM_PLAY_BEGINNING},
        {N, 'J', IDM_PLAY_JUMPTOTIME},
        {N, 'L', IDM_PLAY_GOLIVE},
        // Bookmarks
        {N, 'M', IDM_BOOKMARK_ADD},
        {C, 'M', IDM_BOOKMARK_LIST},
        // Recording
        {N, 'R', IDM_RECORD_TOGGLE},
        // Mute (recording still works)
        {N, 'U', IDM_PLAY_MUTE},
        // Effect controls ([ and ] to cycle, Up/Down to adjust, Backspace to reset)
        {N, '[', IDM_EFFECT_PREV},
        {N, ']', IDM_EFFECT_NEXT},
        {N, WXK_BACK, IDM_EFFECT_RESET},
        {C, WXK_HOME, IDM_EFFECT_MIN},
        {C, WXK_END, IDM_EFFECT_MAX},
        // Audio device selection
        {N, 'A', IDM_SHOW_AUDIO_DEVICES},
        // Effect toggles (Ctrl+1 .. Ctrl+=)
        {C, '1', IDM_TOGGLE_VOLUME},
        {C, '2', IDM_TOGGLE_PITCH},
        {C, '3', IDM_TOGGLE_TEMPO},
        {C, '4', IDM_TOGGLE_RATE},
        {C, '5', IDM_TOGGLE_REVERB},
        {C, '6', IDM_TOGGLE_ECHO},
        {C, '7', IDM_TOGGLE_EQ},
        {C, '8', IDM_TOGGLE_COMPRESSOR},
        {C, '9', IDM_TOGGLE_STEREOWIDTH},
        {C, '0', IDM_TOGGLE_CENTERCANCEL},
        {C, '-', IDM_TOGGLE_CONVOLUTION},
        {C, '=', IDM_TOGGLE_SPATIAL},
        // Tag reading (1-0 keys)
        {N, '1', IDM_READ_TAG_TITLE},
        {N, '2', IDM_READ_TAG_ARTIST},
        {N, '3', IDM_READ_TAG_ALBUM},
        {N, '4', IDM_READ_TAG_YEAR},
        {N, '5', IDM_READ_TAG_TRACK},
        {N, '6', IDM_READ_TAG_GENRE},
        {N, '7', IDM_READ_TAG_COMMENT},
        {N, '8', IDM_READ_TAG_BITRATE},
        {N, '9', IDM_READ_TAG_DURATION},
        {N, '0', IDM_READ_TAG_FILENAME},
        // View tags in dialog (Shift+1-0)
        {S, '1', IDM_VIEW_TAG_TITLE},
        {S, '2', IDM_VIEW_TAG_ARTIST},
        {S, '3', IDM_VIEW_TAG_ALBUM},
        {S, '4', IDM_VIEW_TAG_YEAR},
        {S, '5', IDM_VIEW_TAG_TRACK},
        {S, '6', IDM_VIEW_TAG_GENRE},
        {S, '7', IDM_VIEW_TAG_COMMENT},
        {S, '8', IDM_VIEW_TAG_BITRATE},
        {S, '9', IDM_VIEW_TAG_DURATION},
        {S, '0', IDM_VIEW_TAG_FILENAME},
    };
#ifdef __WXMSW__
    // The same table with Windows virtual key codes. wxWidgets would turn '[' and the
    // other punctuation into whatever key types that character on the current layout
    // (on a German keyboard '[' is AltGr+8); the old table named the physical keys.
    std::vector<ACCEL> native;
    // The user's local hotkeys first: of two entries for one key, the first wins.
    // They are stored as virtual key codes already.
    for (const auto& hk : g_hotkeys) {
        if (hk.global) continue;
        ACCEL a = {};
        a.fVirt = FVIRTKEY;
        if (hk.modifiers & MOD_CONTROL) a.fVirt |= FCONTROL;
        if (hk.modifiers & MOD_SHIFT) a.fVirt |= FSHIFT;
        if (hk.modifiers & MOD_ALT) a.fVirt |= FALT;
        a.key = static_cast<WORD>(hk.vk);
        a.cmd = static_cast<WORD>(g_hotkeyActions[hk.actionIdx].commandId);
        native.push_back(a);
    }
    for (const auto& entry : e) {
        ACCEL a = {};
        a.fVirt = FVIRTKEY;
        if (entry.GetFlags() & wxACCEL_CTRL) a.fVirt |= FCONTROL;
        if (entry.GetFlags() & wxACCEL_SHIFT) a.fVirt |= FSHIFT;
        if (entry.GetFlags() & wxACCEL_ALT) a.fVirt |= FALT;
        switch (entry.GetKeyCode()) {
            case WXK_SPACE: a.key = VK_SPACE; break;
            case WXK_LEFT: a.key = VK_LEFT; break;
            case WXK_RIGHT: a.key = VK_RIGHT; break;
            case WXK_UP: a.key = VK_UP; break;
            case WXK_DOWN: a.key = VK_DOWN; break;
            case WXK_HOME: a.key = VK_HOME; break;
            case WXK_END: a.key = VK_END; break;
            case WXK_BACK: a.key = VK_BACK; break;
            case ',': a.key = VK_OEM_COMMA; break;
            case '.': a.key = VK_OEM_PERIOD; break;
            case '/': a.key = VK_OEM_2; break;
            case '[': a.key = VK_OEM_4; break;
            case ']': a.key = VK_OEM_6; break;
            case '-': a.key = VK_OEM_MINUS; break;
            case '=': a.key = VK_OEM_PLUS; break;
            default: a.key = static_cast<WORD>(entry.GetKeyCode()); break;  // letters and digits
        }
        a.cmd = static_cast<WORD>(entry.GetCommand());
        native.push_back(a);
    }
    if (m_nativeAccel) ::DestroyAcceleratorTable(static_cast<HACCEL>(m_nativeAccel));
    m_nativeAccel = ::CreateAcceleratorTableW(native.data(), static_cast<int>(native.size()));
#else
    // The user's local hotkeys first, so they win over FastPlay's own keys
    std::vector<wxAcceleratorEntry> all;
    for (const auto& hk : g_hotkeys) {
        int key = hk.global ? 0 : WxKeyFromVirtualKey(hk.vk);
        if (key == 0) continue;
        int flags = wxACCEL_NORMAL;
        if (hk.modifiers & MOD_CONTROL) flags |= wxACCEL_CTRL;  // Command on macOS
        if (hk.modifiers & MOD_SHIFT) flags |= wxACCEL_SHIFT;
        if (hk.modifiers & MOD_ALT) flags |= wxACCEL_ALT;
        if (hk.modifiers & MOD_WIN) flags |= wxACCEL_RAW_CTRL;  // Control on macOS
        all.emplace_back(flags, key, g_hotkeyActions[hk.actionIdx].commandId);
    }
    all.insert(all.end(), e.begin(), e.end());
    SetAcceleratorTable(wxAcceleratorTable(static_cast<int>(all.size()), all.data()));
    m_shortcuts = all;
#endif
}

#ifdef __WXOSX__
bool MainFrame::RunShortcut(unsigned modifiers, unsigned vk) {
    int key = WxKeyFromVirtualKey(vk);
    if (key == 0) return false;
    int flags = wxACCEL_NORMAL;
    if (modifiers & MOD_CONTROL) flags |= wxACCEL_CTRL;
    if (modifiers & MOD_SHIFT) flags |= wxACCEL_SHIFT;
    if (modifiers & MOD_ALT) flags |= wxACCEL_ALT;
    if (modifiers & MOD_WIN) flags |= wxACCEL_RAW_CTRL;
    for (const auto& entry : m_shortcuts) {
        if (entry.GetFlags() == flags && entry.GetKeyCode() == key) {
            RunCommand(entry.GetCommand(), 0);
            return true;
        }
    }
    return false;
}
#endif

#ifdef __WXMSW__
bool MainFrame::MSWTranslateMessage(WXMSG* msg) {
    // The arrows going down and coming up, for scrubbing, ahead of the shortcuts
    // (which only see them go down)
    const MSG* m = static_cast<const MSG*>(msg);
    if ((m->message == WM_KEYDOWN || m->message == WM_KEYUP) && (m->wParam == VK_LEFT || m->wParam == VK_RIGHT)) {
        unsigned modifiers = 0;
        if (::GetKeyState(VK_CONTROL) < 0) modifiers |= MOD_CONTROL;
        if (::GetKeyState(VK_SHIFT) < 0) modifiers |= MOD_SHIFT;
        if (::GetKeyState(VK_MENU) < 0) modifiers |= MOD_ALT;
        if (::GetKeyState(VK_LWIN) < 0 || ::GetKeyState(VK_RWIN) < 0) modifiers |= MOD_WIN;
        const bool repeat = (m->lParam & (1 << 30)) != 0;
        if (HandleScrubKey(modifiers, static_cast<unsigned>(m->wParam), m->message == WM_KEYDOWN, repeat)) return true;
    }
    // Checked before the menu bar's shortcuts, so "Volume Up\tUp" in the menu is
    // only a label and Up still adjusts the current effect.
    if (m_nativeAccel && ::TranslateAcceleratorW(static_cast<HWND>(GetHWND()), static_cast<HACCEL>(m_nativeAccel), static_cast<MSG*>(msg))) {
        return true;
    }
    return wxFrame::MSWTranslateMessage(msg);
}

WXLRESULT MainFrame::MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) {
    // An app command from a key pressed in FastPlay's window. Used here, it goes
    // no further, so the shell does not pass it back below as well.
    if (message == WM_APPCOMMAND && RunAppCommand(GET_APPCOMMAND_LPARAM(lParam))) return TRUE;
    // One pressed elsewhere that the window it went to did not use
    if (m_shellHookMessage && message == m_shellHookMessage && wParam == HSHELL_APPCOMMAND) {
        RunAppCommand(GET_APPCOMMAND_LPARAM(lParam));
        return 0;
    }
    return wxFrame::MSWWindowProc(message, wParam, lParam);
}

bool MainFrame::RunAppCommand(int command) {
    // The rewind and fast forward keys have no key codes of their own, so they come
    // as app commands rather than hotkeys, and without word of the key coming up.
    switch (command) {
        case APPCOMMAND_MEDIA_REWIND: PostCommand(IDM_PLAY_SEEKBACK); return true;
        case APPCOMMAND_MEDIA_FAST_FORWARD: PostCommand(IDM_PLAY_SEEKFWD); return true;
    }
    return false;
}
#endif

void MainFrame::MediaSeek(int direction, bool pressed) {
    if (!pressed) {
        if (m_mediaSeekDirection == 0) return;
        m_mediaSeekDirection = 0;
        m_mediaSeekRepeat.Stop();
        StopScrubbing();
        return;
    }
    m_mediaSeekDirection = direction;
    if (IsScrubSeekMode()) {
        StartScrubbing(direction);
    } else {
        SeekBackOrForward(direction);
        m_mediaSeekRepeat.StartOnce(kMediaSeekDelayMs);
    }
}

void MainFrame::RebuildRecentFilesMenu() {
    while (m_recentMenu->GetMenuItemCount() > 0) {
        m_recentMenu->Destroy(m_recentMenu->FindItemByPosition(0));
    }

    if (g_recentFiles.empty()) {
        m_recentMenu->Append(wxID_ANY, "(Empty)")->Enable(false);
        return;
    }
    for (size_t i = 0; i < g_recentFiles.size(); i++) {
        // Just the file name, with a number prefix for keyboard access
        std::wstring display = GetFileName(g_recentFiles[i]);
        wxString text = wxString::Format("&%d %s", static_cast<int>((i + 1) % 10), WX(display));
        m_recentMenu->Append(IDM_FILE_RECENT_BASE + static_cast<int>(i), text);
    }
}

void MainFrame::OnMenuOpen(wxMenuEvent& event) {
    // The recent files list changes as files are played; refresh it whenever a menu opens.
    if (!event.IsPopup()) {
        RebuildRecentFilesMenu();
    }
    event.Skip();
}

void MainFrame::OnMenu(wxCommandEvent& event) {
    int id = event.GetId();
#ifdef __WXOSX__
    // The application menu's Settings and Quit (see BuildMenuBar)
    if (id == wxID_PREFERENCES) id = IDM_TOOLS_OPTIONS;
    else if (id == wxID_EXIT) id = IDM_FILE_EXIT;
#endif
    RunCommand(id, 0);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void MainFrame::SeekBackOrForward(int direction) {
    if (g_currentSeekIndex == 12) {
        // Chapter seeking
        if (!g_chapters.empty()) {
            if (direction < 0) SeekToPrevChapter(); else SeekToNextChapter();
        }
    } else if (g_seekAmounts[g_currentSeekIndex].isTrack && g_playlist.size() <= 1) {
        // Track seeking with a single track: use the first enabled time amount instead
        for (int i = 0; i < g_seekAmountCount; i++) {
            if (g_seekEnabled[i] && !g_seekAmounts[i].isTrack) {
                Seek(direction * g_seekAmounts[i].value);
                break;
            }
        }
    } else if (g_seekAmounts[g_currentSeekIndex].isTrack) {
        SeekTracks(direction * static_cast<int>(g_seekAmounts[g_currentSeekIndex].value));
    } else {
        Seek(direction * GetCurrentSeekAmount());
    }
}

bool MainFrame::HandleScrubKey(unsigned modifiers, unsigned vk, bool down, bool repeat) {
    if (vk != VK_LEFT && vk != VK_RIGHT) return false;
    if (!down) {
        if (vk != m_scrubKey) return false;
#ifdef __WXMSW__
        // Come up only as a screen reader passes it on, after each repeat: still held
        // on the keyboard, so scrubbing goes on until it comes up there
        if (ArrowHeld(vk)) {
            if (!m_scrubArrowPoll.IsRunning()) m_scrubArrowPoll.Start(15);
            return true;
        }
        m_scrubArrowPoll.Stop();
#endif
        m_scrubKey = 0;
        StopScrubbing();
        return true;
    }
    if (modifiers != 0 || !IsScrubSeekMode()) return false;
    // A hotkey of the user's own on the key keeps it
    for (const auto& hk : g_hotkeys) {
        if (!hk.global && hk.modifiers == 0 && hk.vk == vk) return false;
    }
    // Held, the key repeats: only its first press starts (or turns) scrubbing. (A
    // screen reader passes the repeats on as presses, while it is held on the keyboard.)
    bool held = repeat;
#ifdef __WXMSW__
    if (ArrowHeld(vk)) held = true;
#endif
    if (!held || vk != m_scrubKey) {
        m_scrubKey = vk;
        StartScrubbing(vk == VK_LEFT ? -1 : 1);
    }
    return true;
}

void MainFrame::RunCommand(int id, int param) {
    switch (id) {
        case IDM_FILE_OPEN: ShowOpenDialog(); break;
        case IDM_FILE_ADD_FOLDER: ShowAddFolderDialog(); break;
        case IDM_FILE_PLAYLIST: ShowPlaylistDialog(); break;
        case IDM_FILE_OPEN_URL: ShowOpenURLDialog(); break;
        case IDM_FILE_YOUTUBE: ShowYouTubeDialog(); break;
        case IDM_FILE_LIBRARY: ShowLibraryDialog(); break;
        case IDM_FILE_RADIO: ShowRadioDialog(); break;
        case IDM_FILE_ADD_TO_FAVORITES: AddCurrentStreamToFavorites(); break;
        case IDM_FILE_SCHEDULE: ShowSchedulerDialog(); break;
        case IDM_FILE_PODCAST: ShowPodcastDialog(); break;
        case IDM_FILE_EXIT: Close(); break;
        case IDM_FILE_HIDE_TRAY: HideToTray(); break;
        case IDM_TOOLS_OPTIONS: ShowOptionsDialog(); break;
        case IDM_HELP_PLUGINS:
            wxMessageBox(WX(GetAudioEngineInfo()), "Audio Engine", wxOK | wxICON_INFORMATION, this);
            break;
        case IDM_HELP_UPDATES: ShowCheckForUpdatesDialog(false); break;
        case IDM_HELP_README: {
#ifdef __WXOSX__
            // Inside the app bundle, in Contents/Resources/docs.
            wxFileName readme(wxStandardPaths::Get().GetResourcesDir(), "readme.txt");
            readme.AppendDir("docs");
            const char* missing = "Could not open readme.txt.";
#else
            wxFileName readme(wxStandardPaths::Get().GetExecutablePath());
            readme.AppendDir("docs");
            readme.SetFullName("readme.txt");
            const char* missing = "Could not open readme.txt. Make sure the docs folder is present alongside FastPlay.exe.";
#endif
            if (!readme.FileExists() || !wxLaunchDefaultApplication(readme.GetFullPath())) {
                wxMessageBox(missing, "Readme", wxOK | wxICON_WARNING, this);
            }
            break;
        }
        case IDM_BOOKMARK_ADD:
            if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
                double pos = GetCurrentPosition();
                if (AddBookmark(g_playlist[g_currentTrack], pos) >= 0) {
                    Speak("Bookmark added");
                }
            }
            break;
        case IDM_BOOKMARK_LIST: ShowBookmarksDialog(); break;
        case IDM_VIEW_SONG_HISTORY: ShowSongHistoryDialog(); break;
        case IDM_PLAY_PLAYPAUSE: PlayPause(); break;
        case IDM_PLAY_PLAY: Play(); break;
        case IDM_PLAY_PAUSE: Pause(); break;
        case IDM_PLAY_STOP: Stop(); break;
        case IDM_PLAY_PREV: PrevTrack(); break;
        case IDM_PLAY_NEXT:
            // param 1 means load without playing (auto-advance disabled)
            NextTrack(param == 0);
            break;
        case IDM_PLAY_SHUFFLE:
            g_shuffle = !g_shuffle;
            if (g_shuffle) ResetShuffleOrder();  // fresh order each time shuffle is enabled
            Speak(g_shuffle ? "Shuffle on" : "Shuffle off");
            m_shuffleItem->Check(g_shuffle);
            SaveSettings();
            break;
        case IDM_PLAY_REPEAT_TOGGLE: ToggleRepeatMode(); break;
        case IDM_EFFECT_PRESETS: ShowEffectPresetsMenu(); break;
        case IDM_PLAY_BEGINNING: SeekToPosition(0); break;
        case IDM_PLAY_JUMPTOTIME: ShowJumpToTimeDialog(); break;
        case IDM_PLAY_SEEKBACK: SeekBackOrForward(-1); break;
        case IDM_PLAY_SEEKFWD: SeekBackOrForward(1); break;
        // In spring and tape seeking, the speed; otherwise the seek unit
        case IDM_SEEK_DECREASE: IsScrubSeekMode() ? ChangeScrubSpeed(-1) : CycleSeekAmount(-1); break;
        case IDM_SEEK_INCREASE: IsScrubSeekMode() ? ChangeScrubSpeed(1) : CycleSeekAmount(1); break;
        case IDM_SEEK_MODE: CycleSeekMode(); break;
        case IDM_PLAY_GOLIVE: GoLive(); break;
        case IDM_PLAY_RANDOM: JumpToRandomPoint(); break;
        case IDM_PLAY_REVERSE: ToggleReverse(); break;
        case IDM_PLAY_VOLUP: SetVolume(g_volume + g_volumeStep); break;
        case IDM_PLAY_VOLDOWN: SetVolume(g_volume - g_volumeStep); break;
        case IDM_PLAY_MUTE: ToggleMute(); break;
        case IDM_PLAY_ELAPSED: SpeakElapsed(); break;
        case IDM_PLAY_REMAINING: SpeakRemaining(); break;
        case IDM_PLAY_TOTAL: SpeakTotal(); break;
        case IDM_PLAY_NOWPLAYING: SpeakTagTitle(); break;
        case IDM_TOGGLE_WINDOW: ToggleWindow(); break;
        case IDM_TRAY_RESTORE: RestoreFromTray(); break;
        case IDM_TRAY_EXIT: Close(); break;
        // Effect controls
        case IDM_EFFECT_PREV: CycleEffect(-1); break;
        case IDM_EFFECT_NEXT: CycleEffect(1); break;
        case IDM_EFFECT_UP: AdjustCurrentEffect(1); break;
        case IDM_EFFECT_DOWN: AdjustCurrentEffect(-1); break;
        case IDM_EFFECT_RESET: ResetCurrentParam(); break;
        case IDM_EFFECT_MIN: SetCurrentParamToMin(); break;
        case IDM_EFFECT_MAX: SetCurrentParamToMax(); break;
        // Effect toggles
        case IDM_TOGGLE_VOLUME: ToggleStreamEffect(0); break;
        case IDM_TOGGLE_PITCH: ToggleStreamEffect(1); break;
        case IDM_TOGGLE_TEMPO: ToggleStreamEffect(2); break;
        case IDM_TOGGLE_RATE: ToggleStreamEffect(3); break;
        case IDM_TOGGLE_REVERB: ToggleDSPEffect(DSPEffectType::Reverb); break;
        case IDM_TOGGLE_ECHO: ToggleDSPEffect(DSPEffectType::Echo); break;
        case IDM_TOGGLE_EQ: ToggleDSPEffect(DSPEffectType::EQ); break;
        case IDM_TOGGLE_COMPRESSOR: ToggleDSPEffect(DSPEffectType::Compressor); break;
        case IDM_TOGGLE_NORMALIZER: ToggleDSPEffect(DSPEffectType::Normalizer); break;
        case IDM_TOGGLE_STEREOWIDTH: ToggleDSPEffect(DSPEffectType::StereoWidth); break;
        case IDM_TOGGLE_CENTERCANCEL: ToggleDSPEffect(DSPEffectType::CenterCancel); break;
        case IDM_TOGGLE_CONVOLUTION: ToggleDSPEffect(DSPEffectType::Convolution); break;
        case IDM_TOGGLE_SPATIAL: ToggleDSPEffect(DSPEffectType::SpatialAudio); break;
        case IDM_SPEAK_SEEK: SpeakSeekMode(); break;
        // Tag reading (1-0 keys)
        case IDM_READ_TAG_TITLE: SpeakTagTitle(); break;
        case IDM_READ_TAG_ARTIST: SpeakTagArtist(); break;
        case IDM_READ_TAG_ALBUM: SpeakTagAlbum(); break;
        case IDM_READ_TAG_YEAR: SpeakTagYear(); break;
        case IDM_READ_TAG_TRACK: SpeakTagTrack(); break;
        case IDM_READ_TAG_GENRE: SpeakTagGenre(); break;
        case IDM_READ_TAG_COMMENT: SpeakTagComment(); break;
        case IDM_READ_TAG_BITRATE: SpeakTagBitrate(); break;
        case IDM_READ_TAG_DURATION: SpeakTagDuration(); break;
        case IDM_READ_TAG_FILENAME: SpeakTagFilename(); break;
        // View tags in dialog (Shift+1-0)
        case IDM_VIEW_TAG_TITLE: ShowTagDialog(L"Title", GetTagTitle()); break;
        case IDM_VIEW_TAG_ARTIST: ShowTagDialog(L"Artist", GetTagArtist()); break;
        case IDM_VIEW_TAG_ALBUM: ShowTagDialog(L"Album", GetTagAlbum()); break;
        case IDM_VIEW_TAG_YEAR: ShowTagDialog(L"Year", GetTagYear()); break;
        case IDM_VIEW_TAG_TRACK: ShowTagDialog(L"Track", GetTagTrack()); break;
        case IDM_VIEW_TAG_GENRE: ShowTagDialog(L"Genre", GetTagGenre()); break;
        case IDM_VIEW_TAG_COMMENT: ShowTagDialog(L"Comment", GetTagComment()); break;
        case IDM_VIEW_TAG_BITRATE: ShowTagDialog(L"Bitrate", GetTagBitrate()); break;
        case IDM_VIEW_TAG_DURATION: ShowTagDialog(L"Duration", GetTagDuration()); break;
        case IDM_VIEW_TAG_FILENAME: ShowTagDialog(L"Filename", GetTagFilename()); break;
        case IDM_RECORD_TOGGLE: ToggleRecording(); break;
        case IDM_SHOW_AUDIO_DEVICES: ShowAudioDeviceMenu(); break;
        default:
            if (id >= IDM_AUDIO_DEVICE_BASE && id < IDM_AUDIO_DEVICE_BASE + 100) {
                SelectAudioDevice(id - IDM_AUDIO_DEVICE_BASE);
            } else if (id >= IDM_FILE_RECENT_BASE && id < IDM_FILE_RECENT_BASE + MAX_RECENT_FILES) {
                size_t idx = id - IDM_FILE_RECENT_BASE;
                if (idx < g_recentFiles.size()) {
                    g_playlist.clear();
                    g_playlist.push_back(g_recentFiles[idx]);
                    g_currentTrack = -1;
                    PlayTrack(0);
                }
            } else if (id >= IDM_PRESET_BASE && id < IDM_PRESET_BASE + 100) {
                auto names = GetEffectPresetNames();
                size_t idx = id - IDM_PRESET_BASE;
                if (idx < names.size() && LoadEffectPreset(names[idx])) {
                    SpeakW(L"Loaded preset " + names[idx]);
                }
            } else if (id >= IDM_PRESET_DELETE_BASE && id < IDM_PRESET_DELETE_BASE + 100) {
                auto names = GetEffectPresetNames();
                size_t idx = id - IDM_PRESET_DELETE_BASE;
                if (idx < names.size()) {
                    std::wstring n = names[idx];
                    if (DeleteEffectPreset(n)) {
                        SpeakW(L"Deleted preset " + n);
                    }
                }
            } else if (id == IDM_PRESET_SAVE_NEW) {
                ShowSaveEffectPresetDialog();
            }
            break;
    }
}

// ---------------------------------------------------------------------------
// Popup menus
// ---------------------------------------------------------------------------

void MainFrame::ShowEffectPresetsMenu() {
    wxMenu menu;
    auto names = GetEffectPresetNames();
    if (names.empty()) {
        menu.Append(wxID_ANY, "(No presets saved)")->Enable(false);
    } else {
        for (size_t i = 0; i < names.size() && i < 100; i++) {
            menu.Append(IDM_PRESET_BASE + static_cast<int>(i), WX(names[i]));
        }
        menu.AppendSeparator();
        auto* deleteMenu = new wxMenu();
        for (size_t i = 0; i < names.size() && i < 100; i++) {
            deleteMenu->Append(IDM_PRESET_DELETE_BASE + static_cast<int>(i), WX(names[i]));
        }
        menu.AppendSubMenu(deleteMenu, "&Delete preset");
    }
    menu.AppendSeparator();
    menu.Append(IDM_PRESET_SAVE_NEW, "&Save current as new preset...");

    // At the mouse if it is over the window, otherwise near the top left corner.
    wxPoint pt = ScreenToClient(wxGetMousePosition());
    if (!GetClientRect().Contains(pt)) {
        pt = wxPoint(20, 20);
    }
    PopupMenu(&menu, pt);
}

void MainFrame::ShowAudioDeviceMenu() {
    std::vector<AudioDeviceInfo> devices = GetAudioDevices();
    if (devices.empty()) {
        Speak("No audio devices found");
        return;
    }

    wxMenu menu;
    for (const auto& dev : devices) {
        wxMenuItem* item = menu.AppendCheckItem(IDM_AUDIO_DEVICE_BASE + dev.index, WX(dev.name));
        item->Check(dev.current);
    }

    // A global hotkey can open this while the window is hidden.
    bool wasHidden = !IsShown();
    if (wasHidden) Show();
    Raise();

    int cmd = GetPopupMenuSelectionFromUser(menu, ScreenToClient(wxGetMousePosition()));
    if (cmd >= IDM_AUDIO_DEVICE_BASE && cmd < IDM_AUDIO_DEVICE_BASE + 100) {
        SelectAudioDevice(cmd - IDM_AUDIO_DEVICE_BASE);
    }

    if (wasHidden) Hide();
}

// ---------------------------------------------------------------------------
// Title and status bar
// ---------------------------------------------------------------------------

void MainFrame::UpdateTitle() {
    std::wstring title = APP_NAME;

    bool haveTrack = g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size());
#ifdef __WXOSX__
    bool wantTrackTitle = haveTrack;  // Control Center shows it too
#else
    bool wantTrackTitle = haveTrack && g_showTitleInWindow;
#endif
    std::wstring trackTitle;
    if (wantTrackTitle) {
        // Try to get metadata (artist - title) first
        std::wstring tagTitle = GetTagTitle();
        if (!tagTitle.empty() && tagTitle != L"No title" && tagTitle != L"Nothing playing") {
            trackTitle = tagTitle;
        } else {
            // Fall back to filename
            trackTitle = GetTrackName(g_playlist[g_currentTrack]);
        }
    }
    if (g_showTitleInWindow && haveTrack) {
        title += L" - " + trackTitle;
    }
    // The chapter playing, kept up to date as playback moves through them (UpdateStatus)
    m_titleChapter = g_showChapterInTitle && haveTrack && audio::IsLoaded() ? GetCurrentChapterTitle() : L"";
    if (!m_titleChapter.empty()) {
        title += L" - " + m_titleChapter;
    }
#ifdef __WXOSX__
    m_nowPlayingTitle = trackTitle;
#endif

    SetTitle(WX(title));
}

void MainFrame::UpdateStatus() {
    wxStatusBar* bar = GetStatusBar();
    if (!bar || g_isLoading || g_isBusy) return;

    // Position part
    std::wstring posText = L"--:-- / --:--";
    std::wstring stateText;

    if (audio::IsLoaded()) {
        double pos = audio::Position(), len = audio::Length();
        if (len > 0) {
            posText = FormatTime(pos) + L" / " + FormatTime(len);
        }
        if (g_showChapterInTitle && GetCurrentChapterTitle() != m_titleChapter) {
            UpdateTitle();
        }
#ifdef __WXOSX__
        UpdateNowPlaying(m_nowPlayingTitle, len, pos, IsPlaying());
#endif

        switch (audio::GetState()) {
            case audio::State::Playing: stateText = audio::IsReverse() ? L"Playing in reverse" : L"Playing"; break;
            case audio::State::Paused:  stateText = L"Paused"; break;
            case audio::State::Stopped: stateText = L"Stopped"; break;
            default: stateText = L""; break;
        }

        // Add bitrate if available
        int bitrate = GetCurrentBitrate();
        if (bitrate > 0) {
            if (!stateText.empty()) stateText += L" | ";
            if (IsCurrentVbr()) {
                stateText += L"~" + std::to_wstring(bitrate) + L" kbps VBR";
            } else {
                stateText += std::to_wstring(bitrate) + L" kbps";
            }
        }

        // Add recording indicator
        if (g_isRecording) {
            if (!stateText.empty()) stateText += L" | ";
            stateText += L"REC";
        }
    }

    // The bar is refreshed four times a second; only touch parts whose text changed.
    auto set = [bar](int part, const wxString& text) {
        if (bar->GetStatusText(part) != text) bar->SetStatusText(text, part);
    };
    set(SB_PART_POSITION, WX(posText));
    set(SB_PART_VOLUME, wxString::Format("Vol: %d%%", static_cast<int>(g_volume * 100 + 0.5f)));
    set(SB_PART_STATE, WX(stateText));
}

// ---------------------------------------------------------------------------
// Tray
// ---------------------------------------------------------------------------

void MainFrame::HideToTray() {
    if (!m_tray) {
        m_tray = std::make_unique<TrayIcon>(this);
    }
    Hide();
}

void MainFrame::RestoreFromTray() {
    Show();
    if (IsIconized()) {
        Iconize(false);
    }
    Raise();
}

void MainFrame::ToggleWindow() {
    if (IsShown()) {
        HideToTray();
    } else {
        RestoreFromTray();
    }
}

void MainFrame::OnIconize(wxIconizeEvent& event) {
    if (event.IsIconized() && g_minimizeToTray) {
        HideToTray();
        return;
    }
    event.Skip();
}

// ---------------------------------------------------------------------------
// Global hotkeys
// ---------------------------------------------------------------------------

void MainFrame::RegisterGlobalHotkeys() {
#ifdef __WXMSW__
    // Always register the media keys (no modifiers needed)
    RegisterHotKey(kHotkeyMediaPlayPause, 0, VK_MEDIA_PLAY_PAUSE);
    RegisterHotKey(kHotkeyMediaStop, 0, VK_MEDIA_STOP);
    RegisterHotKey(kHotkeyMediaPrev, 0, VK_MEDIA_PREV_TRACK);
    RegisterHotKey(kHotkeyMediaNext, 0, VK_MEDIA_NEXT_TRACK);

    // The user's hotkeys are stored as Windows modifier flags and virtual key codes.
    if (g_hotkeysEnabled) {
        for (const auto& hk : g_hotkeys) {
            if (!hk.global) continue;  // in the main window's key table instead
            int mods = 0;
            if (hk.modifiers & MOD_ALT) mods |= wxMOD_ALT;
            if (hk.modifiers & MOD_CONTROL) mods |= wxMOD_CONTROL;
            if (hk.modifiers & MOD_SHIFT) mods |= wxMOD_SHIFT;
            if (hk.modifiers & MOD_WIN) mods |= wxMOD_WIN;
            RegisterHotKey(hk.id, mods, static_cast<int>(hk.vk));
        }
    }
    // Rewind and fast forward, from the shell when another window has no use for them
    m_shellHookMessage = ::RegisterWindowMessageW(L"SHELLHOOK");
    ::RegisterShellHookWindow(static_cast<HWND>(GetHWND()));
    m_hotkeysRegistered = true;
#elif defined(__WXOSX__)
    // The media keys arrive through the system's Now Playing controls.
    StartMediaKeys([](int commandId) { PostCommand(commandId); },
                   [](int direction, bool pressed) {
                       if (MainFrame* frame = GetMainFrame()) frame->MediaSeek(direction, pressed);
                   });
    SetMediaPositionHandler([](double seconds) {
        SeekToPosition(seconds);
        UpdateStatusBar();
    });
    SetSystemHotkeyHandler([](int id, bool pressed) {
        if (MainFrame* frame = GetMainFrame()) frame->RunHotkey(id, pressed);
    });
    if (g_hotkeysEnabled) {
        for (const auto& hk : g_hotkeys) {
            if (hk.global) RegisterSystemHotkey(hk.id, hk.modifiers, hk.vk);
        }
    }
    m_hotkeysRegistered = true;
#endif
}

void MainFrame::UnregisterGlobalHotkeys() {
#ifdef __WXMSW__
    if (!m_hotkeysRegistered) return;
    UnregisterHotKey(kHotkeyMediaPlayPause);
    UnregisterHotKey(kHotkeyMediaStop);
    UnregisterHotKey(kHotkeyMediaPrev);
    UnregisterHotKey(kHotkeyMediaNext);
    for (const auto& hk : g_hotkeys) {
        UnregisterHotKey(hk.id);
    }
    ::DeregisterShellHookWindow(static_cast<HWND>(GetHWND()));
    m_hotkeysRegistered = false;
#elif defined(__WXOSX__)
    if (!m_hotkeysRegistered) return;
    for (const auto& hk : g_hotkeys) {
        UnregisterSystemHotkey(hk.id);
    }
    m_hotkeysRegistered = false;
#endif
}

void MainFrame::OnHotkey(wxKeyEvent& event) {
    RunHotkey(event.GetId());
}

void MainFrame::RunHotkey(int id, bool pressed) {
    if (!pressed) {
        if (id == m_scrubHotkey) StopHotkeyScrub();
        return;
    }
    switch (id) {
        case kHotkeyMediaPlayPause: PostCommand(IDM_PLAY_PLAYPAUSE); return;
        case kHotkeyMediaStop: PostCommand(IDM_PLAY_STOP); return;
        case kHotkeyMediaPrev: PostCommand(IDM_PLAY_PREV); return;
        case kHotkeyMediaNext: PostCommand(IDM_PLAY_NEXT); return;
    }
    for (const auto& hk : g_hotkeys) {
        if (hk.id != id) continue;
        const int command = g_hotkeyActions[hk.actionIdx].commandId;
        if ((command == IDM_PLAY_SEEKBACK || command == IDM_PLAY_SEEKFWD) && IsScrubSeekMode()) {
            if (id == m_scrubHotkey) return;  // held: the key repeating
            m_scrubHotkey = id;
            StartScrubbing(command == IDM_PLAY_SEEKBACK ? -1 : 1);
#ifdef __WXMSW__
            m_scrubHotkeyVk = hk.vk;
            m_scrubHotkeyPoll.Start(15);
#endif
            return;
        }
        PostCommand(command);
        break;
    }
}

void MainFrame::StopHotkeyScrub() {
    if (!m_scrubHotkey) return;
    m_scrubHotkey = 0;
#ifdef __WXMSW__
    m_scrubHotkeyPoll.Stop();
#endif
    StopScrubbing();
}

// ---------------------------------------------------------------------------
// Files from another FastPlay
// ---------------------------------------------------------------------------

static void AddPathTo(std::vector<std::wstring>& list, const std::wstring& path) {
    if (IsPlaylistFile(path)) {
        auto entries = ParsePlaylist(path);
        list.insert(list.end(), entries.begin(), entries.end());
    } else {
        list.push_back(path);
    }
}

void MainFrame::ReceiveFile(const std::wstring& path) {
    if (!wxFileName::Exists(WX(path))) return;

    // Files arriving just after startup belong to the batch the program was started
    // with (Explorer starts one FastPlay per selected file and they hand over here).
    uint32_t elapsed = TickCountMs() - g_startupTime;
    if (!g_disableBatchDelay && elapsed < BATCH_DELAY && !g_playlist.empty()) {
        AddPathTo(g_playlist, path);
    } else {
        AddPathTo(g_pendingFiles, path);
        // Wait for the rest of the batch; each new file restarts the wait.
        m_batchTimer.StartOnce(g_disableBatchDelay ? 1 : BATCH_DELAY);
    }
}

void MainFrame::OnBatchTimer(wxTimerEvent&) {
    if (g_pendingFiles.empty()) return;

    int startIndex = 0;
    if (g_pendingFiles.size() == 1 && (g_loadFolder || IsCdTrack(g_pendingFiles[0]))) {
        startIndex = ExpandFileToFolder(g_pendingFiles[0], g_playlist);
        g_pendingFiles.clear();
    } else {
        g_playlist = std::move(g_pendingFiles);
        g_pendingFiles.clear();
    }
    PlayTrack(startIndex);
    if (g_bringToFront) {
        if (!IsShown()) {
            RestoreFromTray();
        } else {
            if (IsIconized()) Iconize(false);
            Raise();
        }
    }
}

// ---------------------------------------------------------------------------
// Scheduled event duration
// ---------------------------------------------------------------------------

void MainFrame::StartScheduleDurationTimer(int ms) {
    m_durationTimer.StartOnce(ms);
}

void MainFrame::StopScheduleDurationTimer() {
    m_durationTimer.Stop();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void MainFrame::OnClose(wxCloseEvent&) {
    m_titleTimer.Stop();
    m_schedulerTimer.Stop();
    m_durationTimer.Stop();
    m_batchTimer.Stop();
    if (m_tray) {
        // A tray menu command may be what closed us, and the tray icon's popup menu is
        // still on the stack then: remove the icon now, delete the object later.
        m_tray->RemoveIcon();
        m_tray.release()->Destroy();
    }
    UnregisterGlobalHotkeys();
    StopYouTubeAutoRefresh();
#ifdef __WXOSX__
    StopMediaKeys();  // and leave Control Center's Now Playing
#endif
    StopRecording();  // Stop recording on exit
    SaveCurrentPosition();
    SavePlaybackState();
    SaveSettings();
    YouTubeCleanup();  // Clean up temp files
    StopLibrary();
    CloseDatabase();
    FreeAudio();
    FreeSpeech();
    Destroy();
}

// ---------------------------------------------------------------------------
// app_ui.h: the bridge for code outside the UI
// ---------------------------------------------------------------------------

void PostCommand(int commandId, int param) {
    RunOnUiThread([commandId, param]() {
        if (MainFrame* frame = GetMainFrame()) frame->RunCommand(commandId, param);
    });
}

void RunOnUiThread(std::function<void()> fn) {
    if (wxTheApp) {
        wxTheApp->CallAfter(std::move(fn));
    }
}

static long IconStyle(MessageIcon icon) {
    switch (icon) {
        case MessageIcon::Warning: return wxICON_WARNING;
        case MessageIcon::Error: return wxICON_ERROR;
        case MessageIcon::Question: return wxICON_QUESTION;
        default: return wxICON_INFORMATION;
    }
}

void ShowMessage(const std::wstring& text, const std::wstring& title, MessageIcon icon) {
    if (!wxIsMainThread()) {
        RunOnUiThread([text, title, icon]() { ShowMessage(text, title, icon); });
        return;
    }
    wxMessageBox(WX(text), WX(title), wxOK | IconStyle(icon), GetActiveOwner());
}

bool AskYesNo(const std::wstring& text, const std::wstring& title) {
    return wxMessageBox(WX(text), WX(title), wxYES_NO | wxICON_QUESTION, GetActiveOwner()) == wxYES;
}

void* GetMainWindowHandle() {
    MainFrame* frame = GetMainFrame();
    return frame ? frame->GetHandle() : nullptr;
}

void CloseMainWindow() {
    RunOnUiThread([]() {
        if (MainFrame* frame = GetMainFrame()) frame->Close();
    });
}

void UpdateWindowTitle() {
    if (!wxIsMainThread()) {
        RunOnUiThread([]() { UpdateWindowTitle(); });
        return;
    }
    if (MainFrame* frame = GetMainFrame()) frame->UpdateTitle();
}

void UpdateStatusBar() {
    if (!wxIsMainThread()) {
        RunOnUiThread([]() { UpdateStatusBar(); });
        return;
    }
    if (MainFrame* frame = GetMainFrame()) frame->UpdateStatus();
}

void StartScheduleDurationTimer(int ms) {
    RunOnUiThread([ms]() {
        if (MainFrame* frame = GetMainFrame()) frame->StartScheduleDurationTimer(ms);
    });
}

void StopScheduleDurationTimer() {
    RunOnUiThread([]() {
        if (MainFrame* frame = GetMainFrame()) frame->StopScheduleDurationTimer();
    });
}
