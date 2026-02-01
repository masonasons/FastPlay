#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include "globals.h"
#include "player.h"
#include "settings.h"
#include "hotkeys.h"
#include "tray.h"
#include "accessibility.h"
#include "ui.h"
#include "effects.h"
#include "database.h"
#include "youtube.h"
#include "download_manager.h"
#include "updater.h"
#include "resource.h"
#include <utility>  // for std::pair

#pragma comment(lib, "bass.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

// Forward declarations
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void ParseCommandLine();

// Declarations from ui.cpp
int ExpandFileToFolder(const std::wstring& filePath, std::vector<std::wstring>& outFiles);

// Parse command line arguments
void ParseCommandLine() {
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argv) {
        for (int i = 1; i < argc; i++) {
            if (GetFileAttributesW(argv[i]) != INVALID_FILE_ATTRIBUTES) {
                std::wstring path = argv[i];
                if (IsPlaylistFile(path)) {
                    // Parse playlist and add its contents
                    auto entries = ParsePlaylist(path);
                    g_playlist.insert(g_playlist.end(), entries.begin(), entries.end());
                } else {
                    g_playlist.push_back(path);
                }
            }
        }
        LocalFree(argv);
    }
}

// Window procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            HINSTANCE hInstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

            INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES};
            InitCommonControlsEx(&icc);

            CreateStatusBar(hwnd, hInstance);

            if (!InitBass(hwnd)) {
                return -1;
            }

            InitDatabase();
            InitEffects();
            LoadDSPSettings();
            InitSpeech(hwnd);
            RegisterGlobalHotkeys();

            // Set initial menu check states
            CheckMenuItem(GetMenu(hwnd), IDM_PLAY_SHUFFLE, g_shuffle ? MF_CHECKED : MF_UNCHECKED);

            SetTimer(hwnd, IDT_UPDATE_TITLE, UPDATE_INTERVAL, nullptr);
            SetTimer(hwnd, IDT_SCHEDULER, 60000, nullptr);  // Check schedules every minute
            g_startupTime = GetTickCount();

            if (!g_playlist.empty()) {
                int startIndex = 0;
                if (g_loadFolder && g_playlist.size() == 1) {
                    std::wstring singleFile = g_playlist[0];
                    startIndex = ExpandFileToFolder(singleFile, g_playlist);
                }
                PlayTrack(startIndex);
            }

            UpdateStatusBar();

            // Check for updates on startup (runs in background thread)
            CheckForUpdatesOnStartup();

            return 0;
        }

        case WM_SIZE:
            if (g_statusBar) {
                SendMessageW(g_statusBar, WM_SIZE, 0, 0);
            }
            if (wParam == SIZE_MINIMIZED && g_minimizeToTray) {
                HideToTray(hwnd);
                return 0;
            }
            return 0;

        case WM_TIMER:
            if (wParam == IDT_UPDATE_TITLE) {
                UpdateStatusBar();
            } else if (wParam == IDT_BATCH_FILES) {
                KillTimer(hwnd, IDT_BATCH_FILES);
                if (!g_pendingFiles.empty()) {
                    int startIndex = 0;
                    if (g_loadFolder && g_pendingFiles.size() == 1) {
                        startIndex = ExpandFileToFolder(g_pendingFiles[0], g_playlist);
                        g_pendingFiles.clear();
                    } else {
                        g_playlist = std::move(g_pendingFiles);
                        g_pendingFiles.clear();
                    }
                    PlayTrack(startIndex);
                    if (g_bringToFront) {
                        if (!IsWindowVisible(hwnd)) {
                            RestoreFromTray(hwnd);
                        } else {
                            SetForegroundWindow(hwnd);
                            if (IsIconic(hwnd)) {
                                ShowWindow(hwnd, SW_RESTORE);
                            }
                        }
                    }
                }
            } else if (wParam == IDT_SCHEDULER) {
                CheckScheduledEvents();
            } else if (wParam == IDT_SCHED_DURATION) {
                KillTimer(hwnd, IDT_SCHED_DURATION);
                HandleScheduledDurationEnd();
            }
            return 0;

        case WM_SPEAK:
            DoSpeak();
            return 0;

        case WM_META_CHANGED:
            AnnounceStreamMetadata();
            UpdateWindowTitle();
            return 0;

        case WM_USER + 200: {
            // Update check result
            auto* data = reinterpret_cast<std::pair<UpdateInfo, bool>*>(lParam);
            if (data) {
                HandleUpdateCheckResult(hwnd, &data->first, data->second);
                delete data;
            }
            return 0;
        }

        case WM_USER + 201:
            // Apply downloaded update
            ApplyUpdate();
            return 0;

        case WM_HOTKEY: {
            int hotkeyId = static_cast<int>(wParam);
            for (const auto& hk : g_hotkeys) {
                if (hk.id == hotkeyId) {
                    PostMessage(hwnd, WM_COMMAND, g_hotkeyActions[hk.actionIdx].commandId, 0);
                    break;
                }
            }
            return 0;
        }

        case WM_TRAYICON:
            if (lParam == WM_LBUTTONDBLCLK) {
                RestoreFromTray(hwnd);
            } else if (lParam == WM_RBUTTONUP) {
                ShowTrayMenu(hwnd);
            }
            return 0;

        case WM_COPYDATA: {
            COPYDATASTRUCT* cds = reinterpret_cast<COPYDATASTRUCT*>(lParam);
            if (cds && (cds->dwData == 1 || cds->dwData == 2) && cds->lpData) {
                const wchar_t* filePath = static_cast<const wchar_t*>(cds->lpData);
                if (GetFileAttributesW(filePath) != INVALID_FILE_ATTRIBUTES) {
                    DWORD elapsed = GetTickCount() - g_startupTime;
                    std::wstring path = filePath;
                    if (elapsed < BATCH_DELAY && !g_playlist.empty()) {
                        if (IsPlaylistFile(path)) {
                            auto entries = ParsePlaylist(path);
                            g_playlist.insert(g_playlist.end(), entries.begin(), entries.end());
                        } else {
                            g_playlist.push_back(path);
                        }
                    } else {
                        if (IsPlaylistFile(path)) {
                            auto entries = ParsePlaylist(path);
                            g_pendingFiles.insert(g_pendingFiles.end(), entries.begin(), entries.end());
                        } else {
                            g_pendingFiles.push_back(path);
                        }
                        SetTimer(hwnd, IDT_BATCH_FILES, BATCH_DELAY, nullptr);
                    }
                }
            }
            return TRUE;
        }

        case WM_INITMENUPOPUP:
            // Update recent files menu when File menu is opened
            if (HIWORD(lParam) == FALSE) {  // Not a system menu
                HMENU hMenu = GetMenu(hwnd);
                if (hMenu) {
                    UpdateRecentFilesMenu(hMenu);
                }
            }
            break;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDM_FILE_OPEN:
                    ShowOpenDialog();
                    break;
                case IDM_FILE_ADD_FOLDER:
                    ShowAddFolderDialog();
                    break;
                case IDM_FILE_PLAYLIST:
                    ShowPlaylistDialog();
                    break;
                case IDM_FILE_OPEN_URL:
                    ShowOpenURLDialog();
                    break;
                case IDM_FILE_YOUTUBE:
                    ShowYouTubeDialog(hwnd);
                    break;
                case IDM_FILE_RADIO:
                    ShowRadioDialog();
                    break;
                case IDM_FILE_SCHEDULE:
                    ShowSchedulerDialog();
                    break;
                case IDM_FILE_PODCAST:
                    ShowPodcastDialog();
                    break;
                case IDM_FILE_EXIT:
                    PostQuitMessage(0);
                    break;
                case IDM_FILE_HIDE_TRAY:
                    HideToTray(hwnd);
                    break;
                case IDM_TOOLS_OPTIONS:
                    ShowOptionsDialog();
                    break;
                case IDM_HELP_PLUGINS:
                    MessageBoxW(hwnd, GetLoadedPluginsInfo().c_str(), L"Loaded Plugins", MB_OK | MB_ICONINFORMATION);
                    break;
                case IDM_HELP_UPDATES:
                    ShowCheckForUpdatesDialog(hwnd, false);
                    break;
                case IDM_BOOKMARK_ADD:
                    if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
                        double pos = GetCurrentPosition();
                        int id = AddBookmark(g_playlist[g_currentTrack], pos);
                        if (id >= 0) {
                            Speak("Bookmark added");
                        }
                    }
                    break;
                case IDM_BOOKMARK_LIST:
                    ShowBookmarksDialog();
                    break;
                case IDM_PLAY_PLAYPAUSE:
                    PlayPause();
                    break;
                case IDM_PLAY_PLAY:
                    Play();
                    break;
                case IDM_PLAY_PAUSE:
                    Pause();
                    break;
                case IDM_PLAY_STOP:
                    Stop();
                    break;
                case IDM_PLAY_PREV:
                    PrevTrack();
                    break;
                case IDM_PLAY_NEXT:
                    // lParam==1 means don't auto-play (e.g., when auto-advance is disabled)
                    NextTrack(lParam == 0);
                    break;
                case IDM_PLAY_SHUFFLE:
                    g_shuffle = !g_shuffle;
                    Speak(g_shuffle ? "Shuffle on" : "Shuffle off");
                    CheckMenuItem(GetMenu(hwnd), IDM_PLAY_SHUFFLE, g_shuffle ? MF_CHECKED : MF_UNCHECKED);
                    SaveSettings();
                    break;
                case IDM_PLAY_BEGINNING:
                    SeekToPosition(0);
                    break;
                case IDM_PLAY_JUMPTOTIME:
                    ShowJumpToTimeDialog();
                    break;
                case IDM_PLAY_SEEKBACK:
                    if (g_currentSeekIndex == 12) {
                        // Chapter seeking
                        if (!g_chapters.empty()) {
                            SeekToPrevChapter();
                        }
                    } else if (g_seekAmounts[g_currentSeekIndex].isTrack && g_playlist.size() <= 1) {
                        for (int i = 0; i < g_seekAmountCount; i++) {
                            if (g_seekEnabled[i] && !g_seekAmounts[i].isTrack) {
                                Seek(-g_seekAmounts[i].value);
                                break;
                            }
                        }
                    } else if (g_seekAmounts[g_currentSeekIndex].isTrack) {
                        SeekTracks(-static_cast<int>(g_seekAmounts[g_currentSeekIndex].value));
                    } else {
                        Seek(-GetCurrentSeekAmount());
                    }
                    break;
                case IDM_PLAY_SEEKFWD:
                    if (g_currentSeekIndex == 12) {
                        // Chapter seeking
                        if (!g_chapters.empty()) {
                            SeekToNextChapter();
                        }
                    } else if (g_seekAmounts[g_currentSeekIndex].isTrack && g_playlist.size() <= 1) {
                        for (int i = 0; i < g_seekAmountCount; i++) {
                            if (g_seekEnabled[i] && !g_seekAmounts[i].isTrack) {
                                Seek(g_seekAmounts[i].value);
                                break;
                            }
                        }
                    } else if (g_seekAmounts[g_currentSeekIndex].isTrack) {
                        SeekTracks(static_cast<int>(g_seekAmounts[g_currentSeekIndex].value));
                    } else {
                        Seek(GetCurrentSeekAmount());
                    }
                    break;
                case IDM_SEEK_DECREASE:
                    CycleSeekAmount(-1);
                    break;
                case IDM_SEEK_INCREASE:
                    CycleSeekAmount(1);
                    break;
                case IDM_PLAY_VOLUP:
                    SetVolume(g_volume + g_volumeStep);
                    break;
                case IDM_PLAY_VOLDOWN:
                    SetVolume(g_volume - g_volumeStep);
                    break;
                case IDM_PLAY_MUTE:
                    ToggleMute();
                    break;
                case IDM_PLAY_ELAPSED:
                    SpeakElapsed();
                    break;
                case IDM_PLAY_REMAINING:
                    SpeakRemaining();
                    break;
                case IDM_PLAY_TOTAL:
                    SpeakTotal();
                    break;
                case IDM_PLAY_NOWPLAYING:
                    SpeakTagTitle();
                    break;
                case IDM_TOGGLE_WINDOW:
                    ToggleWindow(hwnd);
                    break;
                case IDM_TRAY_RESTORE:
                    RestoreFromTray(hwnd);
                    break;
                case IDM_TRAY_EXIT:
                    DestroyWindow(hwnd);
                    break;
                // Effect controls
                case IDM_EFFECT_PREV:
                    CycleEffect(-1);
                    break;
                case IDM_EFFECT_NEXT:
                    CycleEffect(1);
                    break;
                case IDM_EFFECT_UP:
                    AdjustCurrentEffect(1);
                    break;
                case IDM_EFFECT_DOWN:
                    AdjustCurrentEffect(-1);
                    break;
                case IDM_EFFECT_RESET:
                    ResetCurrentParam();
                    break;
                case IDM_EFFECT_MIN:
                    SetCurrentParamToMin();
                    break;
                case IDM_EFFECT_MAX:
                    SetCurrentParamToMax();
                    break;
                // Effect toggles
                case IDM_TOGGLE_VOLUME:
                    ToggleStreamEffect(0);
                    break;
                case IDM_TOGGLE_PITCH:
                    ToggleStreamEffect(1);
                    break;
                case IDM_TOGGLE_TEMPO:
                    ToggleStreamEffect(2);
                    break;
                case IDM_TOGGLE_RATE:
                    ToggleStreamEffect(3);
                    break;
                case IDM_TOGGLE_REVERB:
                    ToggleDSPEffect(DSPEffectType::Reverb);
                    break;
                case IDM_TOGGLE_ECHO:
                    ToggleDSPEffect(DSPEffectType::Echo);
                    break;
                case IDM_TOGGLE_EQ:
                    ToggleDSPEffect(DSPEffectType::EQ);
                    break;
                case IDM_TOGGLE_COMPRESSOR:
                    ToggleDSPEffect(DSPEffectType::Compressor);
                    break;
                case IDM_TOGGLE_STEREOWIDTH:
                    ToggleDSPEffect(DSPEffectType::StereoWidth);
                    break;
                case IDM_TOGGLE_CENTERCANCEL:
                    ToggleDSPEffect(DSPEffectType::CenterCancel);
                    break;
                // Speak seek amount
                case IDM_SPEAK_SEEK:
                    SpeakSeekAmount();
                    break;
                // Tag reading (1-0 keys)
                case IDM_READ_TAG_TITLE:
                    SpeakTagTitle();
                    break;
                case IDM_READ_TAG_ARTIST:
                    SpeakTagArtist();
                    break;
                case IDM_READ_TAG_ALBUM:
                    SpeakTagAlbum();
                    break;
                case IDM_READ_TAG_YEAR:
                    SpeakTagYear();
                    break;
                case IDM_READ_TAG_TRACK:
                    SpeakTagTrack();
                    break;
                case IDM_READ_TAG_GENRE:
                    SpeakTagGenre();
                    break;
                case IDM_READ_TAG_COMMENT:
                    SpeakTagComment();
                    break;
                case IDM_READ_TAG_BITRATE:
                    SpeakTagBitrate();
                    break;
                case IDM_READ_TAG_DURATION:
                    SpeakTagDuration();
                    break;
                case IDM_READ_TAG_FILENAME:
                    SpeakTagFilename();
                    break;
                // View tags in dialog (Shift+1-0)
                case IDM_VIEW_TAG_TITLE:
                    ShowTagDialog(L"Title", GetTagTitle());
                    break;
                case IDM_VIEW_TAG_ARTIST:
                    ShowTagDialog(L"Artist", GetTagArtist());
                    break;
                case IDM_VIEW_TAG_ALBUM:
                    ShowTagDialog(L"Album", GetTagAlbum());
                    break;
                case IDM_VIEW_TAG_YEAR:
                    ShowTagDialog(L"Year", GetTagYear());
                    break;
                case IDM_VIEW_TAG_TRACK:
                    ShowTagDialog(L"Track", GetTagTrack());
                    break;
                case IDM_VIEW_TAG_GENRE:
                    ShowTagDialog(L"Genre", GetTagGenre());
                    break;
                case IDM_VIEW_TAG_COMMENT:
                    ShowTagDialog(L"Comment", GetTagComment());
                    break;
                case IDM_VIEW_TAG_BITRATE:
                    ShowTagDialog(L"Bitrate", GetTagBitrate());
                    break;
                case IDM_VIEW_TAG_DURATION:
                    ShowTagDialog(L"Duration", GetTagDuration());
                    break;
                case IDM_VIEW_TAG_FILENAME:
                    ShowTagDialog(L"Filename", GetTagFilename());
                    break;
                case IDM_RECORD_TOGGLE:
                    ToggleRecording();
                    break;
                case IDM_SHOW_AUDIO_DEVICES:
                    ShowAudioDeviceMenu(hwnd);
                    break;
                default:
                    // Handle audio device selection (dynamic menu IDs)
                    {
                        WORD cmdId = LOWORD(wParam);
                        if (cmdId >= IDM_AUDIO_DEVICE_BASE && cmdId < IDM_AUDIO_DEVICE_BASE + 100) {
                            int deviceIndex = cmdId - IDM_AUDIO_DEVICE_BASE;
                            SelectAudioDevice(deviceIndex);
                        }
                        // Handle recent file selection
                        else if (cmdId >= IDM_FILE_RECENT_BASE && cmdId < IDM_FILE_RECENT_BASE + MAX_RECENT_FILES) {
                            size_t idx = cmdId - IDM_FILE_RECENT_BASE;
                            if (idx < g_recentFiles.size()) {
                                g_playlist.clear();
                                g_playlist.push_back(g_recentFiles[idx]);
                                g_currentTrack = -1;
                                PlayTrack(0);
                            }
                        }
                    }
                    break;
            }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, IDT_UPDATE_TITLE);
            KillTimer(hwnd, IDT_SCHEDULER);
            KillTimer(hwnd, IDT_SCHED_DURATION);
            RemoveTrayIcon();
            UnregisterGlobalHotkeys();
            StopRecording();  // Stop recording on exit
            if (g_fxStream && g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
                SaveFilePosition(g_playlist[g_currentTrack]);
            }
            SavePlaybackState();
            SaveSettings();
            YouTubeCleanup();  // Clean up temp files
            CloseDatabase();
            FreeBass();
            FreeSpeech();
            PostQuitMessage(0);
            return 0;
    }

    // Download completion from DownloadManager (posted to main window)
    if (msg == WM_DOWNLOAD_COMPLETE) {
        int id = static_cast<int>(wParam);
        bool success = (lParam != 0);
        DownloadManager::Instance().ProcessCompletion(id, success);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Entry point
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    // Set DLL search path to lib subfolder (must be before any DLL loads)
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
        wcscat_s(exePath, MAX_PATH, L"lib");
        SetDllDirectoryW(exePath);
    }

    // Build config path early to read multi-instance setting
    wchar_t configPath[MAX_PATH];
    GetModuleFileNameW(nullptr, configPath, MAX_PATH);
    wchar_t* configSlash = wcsrchr(configPath, L'\\');
    if (configSlash) {
        *(configSlash + 1) = L'\0';
        wcscat_s(configPath, MAX_PATH, L"FastPlay.ini");
    }

    // Check if multiple instances are allowed
    bool allowMultiple = GetPrivateProfileIntW(L"Playback", L"AllowMultipleInstances", 0, configPath) != 0;

    // Check if we have file arguments
    bool hasFileArgs = false;
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; i++) {
            if (GetFileAttributesW(argv[i]) != INVALID_FILE_ATTRIBUTES) {
                hasFileArgs = true;
                break;
            }
        }
    }

    // Single instance logic:
    // - If multiple instances NOT allowed: always use single instance
    // - If multiple instances allowed AND has file args: send to existing instance
    // - If multiple instances allowed AND no file args: start new instance
    bool useSingleInstance = !allowMultiple || hasFileArgs;

    HANDLE hMutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS && useSingleInstance) {
        HWND existingWnd = FindWindowW(WINDOW_CLASS, nullptr);
        if (existingWnd) {
            if (argv && hasFileArgs) {
                bool firstFile = true;
                for (int i = 1; i < argc; i++) {
                    if (GetFileAttributesW(argv[i]) != INVALID_FILE_ATTRIBUTES) {
                        COPYDATASTRUCT cds;
                        cds.dwData = firstFile ? 1 : 2;
                        cds.cbData = static_cast<DWORD>((wcslen(argv[i]) + 1) * sizeof(wchar_t));
                        cds.lpData = argv[i];
                        SendMessageW(existingWnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
                        firstFile = false;
                    }
                }
            }
            if (argv) LocalFree(argv);
            CloseHandle(hMutex);
            return 0;
        }
    }
    if (argv) LocalFree(argv);

    LoadSettings();
    LoadHotkeys();
    YouTubeCleanup();  // Clean up any leftover temp files from previous sessions
    ParseCommandLine();

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDM_MAIN_MENU);
    wc.lpszClassName = WINDOW_CLASS;
    wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register window class.", APP_NAME, MB_ICONERROR);
        return 1;
    }

    HACCEL hAccel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDA_ACCEL));

    HWND hwnd = CreateWindowExW(
        0,
        WINDOW_CLASS,
        APP_NAME,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        500, 150,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create window.", APP_NAME, MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    if (g_playlist.empty()) {
        LoadPlaybackState();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // Handle modeless YouTube dialog
        HWND ytDlg = GetYouTubeDialog();
        if (ytDlg && IsDialogMessageW(ytDlg, &msg)) {
            continue;  // Message was handled by the dialog
        }

        // Only process accelerators if YouTube dialog doesn't have focus
        bool ytHasFocus = ytDlg && (GetForegroundWindow() == ytDlg || IsChild(ytDlg, GetFocus()));
        if (ytHasFocus || !TranslateAcceleratorW(hwnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return static_cast<int>(msg.wParam);
}
