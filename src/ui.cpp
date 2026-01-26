#include "ui.h"
#include "globals.h"
#include "utils.h"
#include "player.h"
#include "settings.h"
#include "hotkeys.h"
#include "tray.h"
#include "accessibility.h"
#include "effects.h"
#include "tempo_processor.h"
#include "convolution.h"
#include "database.h"
#include "resource.h"
#include <commdlg.h>
#include <commctrl.h>
#include <shlobj.h>
#include <wininet.h>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <sstream>
#include <iomanip>

// External globals (defined in globals.cpp)
extern HWND g_hwnd;
extern HWND g_statusBar;
extern HSTREAM g_fxStream;
extern std::vector<std::wstring> g_playlist;
extern int g_currentTrack;
extern float g_volume;
extern bool g_isLoading;
extern bool g_isBusy;
extern int g_selectedDevice;
extern bool g_allowAmplify;
extern bool g_rememberState;
extern int g_rememberPosMinutes;
extern bool g_bringToFront;
extern bool g_loadFolder;
extern const int g_posThresholds[];
extern const int g_posThresholdCount;
extern bool g_seekEnabled[];
extern int g_currentSeekIndex;
extern const SeekAmount g_seekAmounts[];
extern const int g_seekAmountCount;
extern const FileAssoc g_fileAssocs[];
extern const int g_fileAssocCount;
extern const HotkeyAction g_hotkeyActions[];
extern const int g_hotkeyActionCount;
extern std::vector<GlobalHotkey> g_hotkeys;
extern int g_nextHotkeyId;
extern bool g_hotkeysEnabled;
extern std::wstring g_configPath;
extern bool g_effectEnabled[];
extern int g_currentEffectIndex;
extern int g_bufferSize;
extern int g_updatePeriod;
extern const int g_bufferSizes[];
extern const int g_bufferSizeCount;
extern const int g_updatePeriods[];
extern const int g_updatePeriodCount;
extern int g_tempoAlgorithm;
extern std::wstring g_ytdlpPath;
extern std::wstring g_ytApiKey;
extern bool g_isRecording;

// Scheduled event duration tracking
static ScheduleStopAction g_pendingStopAction = ScheduleStopAction::StopBoth;
static bool g_schedulerMuted = false;  // Track if we muted for scheduled recording

// External functions
extern std::wstring GetFileName(const std::wstring& path);
extern std::wstring FormatTime(double seconds);
extern void SaveSettings();
extern void SaveHotkeys();
extern bool ReinitBass(int device);
extern void SetVolume(float vol);
extern void RegisterGlobalHotkeys();
extern void UnregisterGlobalHotkeys();
extern void PlayTrack(int index, bool autoPlay);
extern void SaveFilePosition(const std::wstring& filePath);
extern double LoadFilePosition(const std::wstring& filePath);
extern INT_PTR CALLBACK HotkeyDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern std::wstring FormatHotkey(UINT modifiers, UINT vk);

// Constants
static const wchar_t* APP_NAME = L"FastPlay";

// Update window title (optionally shows track name)
void UpdateWindowTitle() {
    std::wstring title = APP_NAME;

    if (g_showTitleInWindow && g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
        title += L" - ";
        // Try to get metadata (artist - title) first
        std::wstring tagTitle = GetTagTitle();
        if (!tagTitle.empty() && tagTitle != L"No title" && tagTitle != L"Nothing playing") {
            title += tagTitle;
        } else {
            // Fall back to filename
            title += GetFileName(g_playlist[g_currentTrack]);
        }
    }

    SetWindowTextW(g_hwnd, title.c_str());
}

// Update status bar with position, volume, state
void UpdateStatusBar() {
    if (!g_statusBar || g_isLoading || g_isBusy) return;

    // Position part
    std::wstring posText = L"--:-- / --:--";
    std::wstring stateText;

    if (g_fxStream) {
        // Use tempo processor to get position and length (supports both SoundTouch and Rubber Band)
        TempoProcessor* processor = GetTempoProcessor();
        if (processor && processor->IsActive()) {
            double pos = processor->GetPosition();
            double len = processor->GetLength();
            if (len > 0) {
                posText = FormatTime(pos) + L" / " + FormatTime(len);
            }
        }

        DWORD state = BASS_ChannelIsActive(g_fxStream);
        switch (state) {
            case BASS_ACTIVE_PLAYING: stateText = L"Playing"; break;
            case BASS_ACTIVE_PAUSED:  stateText = L"Paused"; break;
            case BASS_ACTIVE_STOPPED: stateText = L"Stopped"; break;
            default: stateText = L""; break;
        }

        // Add recording indicator
        if (g_isRecording) {
            if (!stateText.empty()) stateText += L" | ";
            stateText += L"REC";
        }
    }

    SendMessageW(g_statusBar, SB_SETTEXTW, SB_PART_POSITION, reinterpret_cast<LPARAM>(posText.c_str()));

    // Volume part
    wchar_t volBuf[32];
    swprintf(volBuf, 32, L"Vol: %d%%", static_cast<int>(g_volume * 100 + 0.5f));
    SendMessageW(g_statusBar, SB_SETTEXTW, SB_PART_VOLUME, reinterpret_cast<LPARAM>(volBuf));

    SendMessageW(g_statusBar, SB_SETTEXTW, SB_PART_STATE, reinterpret_cast<LPARAM>(stateText.c_str()));
}

// Create status bar
void CreateStatusBar(HWND hwnd, HINSTANCE hInstance) {
    g_statusBar = CreateWindowExW(
        0,
        STATUSCLASSNAMEW,
        nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0,
        hwnd,
        nullptr,
        hInstance,
        nullptr
    );

    if (g_statusBar) {
        // Set up parts: position (200px), volume (100px), state (rest)
        int parts[SB_PART_COUNT] = {200, 300, -1};
        SendMessageW(g_statusBar, SB_SETPARTS, SB_PART_COUNT, reinterpret_cast<LPARAM>(parts));
    }
}

// Check if a file extension is a supported audio format
bool IsSupportedAudioExt(const std::wstring& ext) {
    static const wchar_t* exts[] = {
        L".mp3", L".wav", L".ogg", L".flac", L".m4a", L".wma", L".aac",
        L".opus", L".aiff", L".ape", L".wv", L".mid", L".midi", L".dff", L".dsf"
    };
    std::wstring lowerExt = ext;
    for (auto& c : lowerExt) c = towlower(c);
    for (const auto& e : exts) {
        if (lowerExt == e) return true;
    }
    return false;
}

// Expand a single file to all audio files in its folder
// Returns the index of the original file in the expanded list
int ExpandFileToFolder(const std::wstring& filePath, std::vector<std::wstring>& outFiles) {
    outFiles.clear();

    // Get directory and filename
    size_t lastSlash = filePath.find_last_of(L"\\/");
    if (lastSlash == std::wstring::npos) {
        outFiles.push_back(filePath);
        return 0;
    }

    std::wstring dir = filePath.substr(0, lastSlash + 1);
    std::wstring targetFile = filePath.substr(lastSlash + 1);

    // Find all audio files in the directory
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        outFiles.push_back(filePath);
        return 0;
    }

    std::vector<std::wstring> files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::wstring name = fd.cFileName;
            size_t dotPos = name.find_last_of(L'.');
            if (dotPos != std::wstring::npos) {
                std::wstring ext = name.substr(dotPos);
                if (IsSupportedAudioExt(ext)) {
                    files.push_back(dir + name);
                }
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    // Find the index of the original file
    int targetIndex = 0;
    for (size_t i = 0; i < files.size(); i++) {
        if (_wcsicmp(GetFileName(files[i]).c_str(), targetFile.c_str()) == 0) {
            targetIndex = static_cast<int>(i);
            break;
        }
    }

    outFiles = std::move(files);
    return targetIndex;
}

// Get executable path
std::wstring GetExePath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

// Check if file extension is associated with FastPlay
bool IsExtensionAssociated(const wchar_t* ext) {
    wchar_t keyPath[256];
    swprintf(keyPath, 256, L"Software\\Classes\\%s", ext);

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t value[256] = {0};
    DWORD size = sizeof(value);
    DWORD type;
    bool associated = false;

    if (RegQueryValueExW(hKey, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(value), &size) == ERROR_SUCCESS) {
        associated = (wcscmp(value, L"FastPlay.AudioFile") == 0);
    }
    RegCloseKey(hKey);
    return associated;
}

// Set or remove file association
void SetFileAssociation(const wchar_t* ext, bool associate) {
    wchar_t extKeyPath[256];
    swprintf(extKeyPath, 256, L"Software\\Classes\\%s", ext);

    if (associate) {
        // Create extension key pointing to our ProgId
        HKEY hKey;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, extKeyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            const wchar_t* progId = L"FastPlay.AudioFile";
            RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(progId), static_cast<DWORD>((wcslen(progId) + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }

        // Create ProgId with shell\open\command
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\FastPlay.AudioFile", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            const wchar_t* desc = L"FastPlay Audio File";
            RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(desc), static_cast<DWORD>((wcslen(desc) + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }

        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\FastPlay.AudioFile\\shell\\open\\command", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            std::wstring cmd = L"\"" + GetExePath() + L"\" \"%1\"";
            RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), static_cast<DWORD>((cmd.length() + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }
    } else {
        // Remove association by deleting the extension key
        RegDeleteKeyW(HKEY_CURRENT_USER, extKeyPath);
    }

    // Notify shell of change
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// Show file open dialog
void ShowOpenDialog() {
    // Buffer for multiple file selection
    wchar_t szFile[32768] = {0};

    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = L"All Supported\0*.mp3;*.wav;*.ogg;*.flac;*.m4a;*.wma;*.aac;*.opus;*.aiff;*.ape;*.wv;*.mid;*.midi;*.dff;*.dsf;*.alac;*.m3u;*.m3u8;*.pls\0"
                      L"Audio Files\0*.mp3;*.wav;*.ogg;*.flac;*.m4a;*.wma;*.aac;*.opus;*.aiff;*.ape;*.wv;*.mid;*.midi;*.dff;*.dsf;*.alac\0"
                      L"Playlists\0*.m3u;*.m3u8;*.pls\0"
                      L"All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    if (GetOpenFileNameW(&ofn)) {
        g_playlist.clear();
        g_currentTrack = -1;

        // Check if multiple files selected
        wchar_t* p = szFile;
        std::wstring dir = p;
        p += wcslen(p) + 1;

        int startIndex = 0;
        if (*p == 0) {
            // Single file selected
            // Check if it's a playlist file
            if (IsPlaylistFile(dir)) {
                g_playlist = ParsePlaylist(dir);
            } else if (g_loadFolder) {
                // Expand to folder if option enabled
                startIndex = ExpandFileToFolder(dir, g_playlist);
            } else {
                g_playlist.push_back(dir);
            }
        } else {
            // Multiple files - first string is directory
            while (*p) {
                std::wstring fullPath = dir + L"\\" + p;
                // Check if it's a playlist file
                if (IsPlaylistFile(fullPath)) {
                    auto entries = ParsePlaylist(fullPath);
                    g_playlist.insert(g_playlist.end(), entries.begin(), entries.end());
                } else {
                    g_playlist.push_back(fullPath);
                }
                p += wcslen(p) + 1;
            }
        }

        // Play selected file
        if (!g_playlist.empty()) {
            PlayTrack(startIndex);
        }
    }
}

// Recursively add audio files from a folder
static void AddFilesFromFolderRecursive(const std::wstring& folder, std::vector<std::wstring>& files, int depth) {
    // Limit recursion depth to prevent stack overflow
    if (depth > 32) return;

    WIN32_FIND_DATAW fd;
    std::wstring searchPath = folder + L"\\*";
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);

    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        // Skip reparse points (junctions, symlinks) to avoid infinite loops
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        std::wstring fullPath = folder + L"\\" + fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Recurse into subdirectory
            AddFilesFromFolderRecursive(fullPath, files, depth + 1);
        } else {
            // Check if it's a supported audio file
            size_t dotPos = fullPath.rfind(L'.');
            if (dotPos != std::wstring::npos) {
                std::wstring ext = fullPath.substr(dotPos);
                if (IsSupportedAudioExt(ext)) {
                    files.push_back(fullPath);
                }
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

static void AddFilesFromFolder(const std::wstring& folder, std::vector<std::wstring>& files) {
    AddFilesFromFolderRecursive(folder, files, 0);
}

// Show folder browser dialog and add all audio files
void ShowAddFolderDialog() {
    BROWSEINFOW bi = {0};
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = L"Select folder to add to playlist";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t folderPath[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, folderPath)) {
            // Collect all audio files recursively
            std::vector<std::wstring> newFiles;
            AddFilesFromFolder(folderPath, newFiles);

            // Sort files alphabetically
            std::sort(newFiles.begin(), newFiles.end());

            if (!newFiles.empty()) {
                // Replace playlist with new files
                g_playlist.clear();
                g_currentTrack = -1;
                for (const auto& file : newFiles) {
                    g_playlist.push_back(file);
                }

                // Start playing from the beginning
                PlayTrack(0);

                Speak(std::to_string(newFiles.size()) + " files loaded");
            } else {
                Speak("No audio files found");
            }
        }
        CoTaskMemFree(pidl);
    }
}

// Get files from clipboard (supports files, folders, and text URLs/paths)
static std::vector<std::wstring> GetFilesFromClipboard() {
    std::vector<std::wstring> files;

    if (!OpenClipboard(nullptr)) return files;

    // First try file drop format
    HANDLE hData = GetClipboardData(CF_HDROP);
    if (hData) {
        HDROP hDrop = static_cast<HDROP>(hData);
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);

        for (UINT i = 0; i < count; i++) {
            // Get required buffer size first
            UINT pathLen = DragQueryFileW(hDrop, i, nullptr, 0);
            if (pathLen > 0 && pathLen < 32768) {
                std::wstring path(pathLen + 1, L'\0');
                if (DragQueryFileW(hDrop, i, &path[0], pathLen + 1)) {
                    // Remove null terminator from string
                    path.resize(wcslen(path.c_str()));

                    DWORD attrs = GetFileAttributesW(path.c_str());
                    if (attrs != INVALID_FILE_ATTRIBUTES) {
                        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
                            // Recursively add folder contents
                            AddFilesFromFolder(path, files);
                        } else {
                            // Check if it's a supported audio file
                            size_t dotPos = path.rfind(L'.');
                            if (dotPos != std::wstring::npos) {
                                std::wstring ext = path.substr(dotPos);
                                if (IsSupportedAudioExt(ext)) {
                                    files.push_back(path);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // If no files from drop, try text format (URLs or file paths)
    if (files.empty()) {
        HANDLE hText = GetClipboardData(CF_UNICODETEXT);
        if (hText) {
            const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(hText));
            if (text) {
                std::wstring clipText = text;
                GlobalUnlock(hText);

                // Split by newlines and process each line
                size_t start = 0;
                while (start < clipText.length()) {
                    size_t end = clipText.find_first_of(L"\r\n", start);
                    if (end == std::wstring::npos) end = clipText.length();

                    std::wstring line = clipText.substr(start, end - start);
                    // Trim whitespace
                    while (!line.empty() && (line[0] == L' ' || line[0] == L'\t')) line.erase(0, 1);
                    while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) line.pop_back();

                    if (!line.empty()) {
                        // Check if it's a URL
                        if (line.find(L"http://") == 0 || line.find(L"https://") == 0 ||
                            line.find(L"mms://") == 0 || line.find(L"rtsp://") == 0) {
                            files.push_back(line);
                        } else {
                            // Check if it's a valid file/folder path
                            DWORD attrs = GetFileAttributesW(line.c_str());
                            if (attrs != INVALID_FILE_ATTRIBUTES) {
                                if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
                                    AddFilesFromFolder(line, files);
                                } else {
                                    std::wstring ext = line;
                                    size_t dotPos = ext.rfind(L'.');
                                    if (dotPos != std::wstring::npos) {
                                        ext = ext.substr(dotPos);
                                        if (IsSupportedAudioExt(ext)) {
                                            files.push_back(line);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    start = end + 1;
                    // Skip consecutive newlines
                    while (start < clipText.length() && (clipText[start] == L'\r' || clipText[start] == L'\n')) start++;
                }
            }
        }
    }

    CloseClipboard();
    return files;
}

// Helper to rebuild playlist listbox
static void RebuildPlaylistList(HWND hList, int selectIndex = -1) {
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_playlist.size(); i++) {
        std::wstring filename = g_playlist[i];
        size_t pos = filename.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            filename = filename.substr(pos + 1);
        }
        wchar_t buf[512];
        swprintf(buf, 512, L"%d. %s", (int)(i + 1), filename.c_str());
        SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }
    if (selectIndex >= 0 && selectIndex < (int)g_playlist.size()) {
        SendMessageW(hList, LB_SETCURSEL, selectIndex, 0);
    }
}

// Subclassed listbox data for playlist manager
static WNDPROC g_playlistOrigProc = nullptr;
static HWND g_playlistDlg = nullptr;

// Helper to get selected indices from multi-select listbox
static std::vector<int> GetSelectedIndices(HWND hwnd) {
    std::vector<int> indices;
    int count = (int)SendMessageW(hwnd, LB_GETSELCOUNT, 0, 0);
    if (count > 0) {
        indices.resize(count);
        SendMessageW(hwnd, LB_GETSELITEMS, count, reinterpret_cast<LPARAM>(indices.data()));
    }
    return indices;
}

// Subclassed listbox procedure for playlist manager
static LRESULT CALLBACK PlaylistListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        int sel = (int)SendMessageW(hwnd, LB_GETCURSEL, 0, 0);
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (wParam == VK_ESCAPE) {
            EndDialog(g_playlistDlg, IDCANCEL);
            return 0;
        }

        if (wParam == VK_RETURN && sel >= 0 && sel < (int)g_playlist.size()) {
            PlayTrack(sel);
            EndDialog(g_playlistDlg, IDOK);
            return 0;
        }

        if (wParam == VK_DELETE) {
            std::vector<int> selected = GetSelectedIndices(hwnd);
            if (!selected.empty()) {
                // Remove from end to preserve indices
                for (int i = (int)selected.size() - 1; i >= 0; i--) {
                    int idx = selected[i];
                    if (idx >= 0 && idx < (int)g_playlist.size()) {
                        g_playlist.erase(g_playlist.begin() + idx);
                        if (g_currentTrack > idx) {
                            g_currentTrack--;
                        } else if (g_currentTrack == idx) {
                            g_currentTrack = -1;
                        }
                    }
                }
                int newSel = selected[0];
                if (newSel >= (int)g_playlist.size()) newSel = (int)g_playlist.size() - 1;
                RebuildPlaylistList(hwnd, newSel);
                Speak(std::to_string(selected.size()) + " removed");
                return 0;
            }
        }

        // Ctrl+A: Select all
        if (ctrl && wParam == 'A') {
            SendMessageW(hwnd, LB_SETSEL, TRUE, -1);
            return 0;
        }

        // Ctrl+V: Paste
        if (ctrl && wParam == 'V') {
            try {
                std::vector<std::wstring> newFiles = GetFilesFromClipboard();
                if (!newFiles.empty()) {
                    int insertPos = (sel >= 0 && sel < (int)g_playlist.size()) ? sel + 1 : (int)g_playlist.size();
                    for (size_t i = 0; i < newFiles.size(); i++) {
                        g_playlist.insert(g_playlist.begin() + insertPos + i, newFiles[i]);
                    }
                    if (g_currentTrack >= insertPos) {
                        g_currentTrack += (int)newFiles.size();
                    }
                    RebuildPlaylistList(hwnd, insertPos);
                    Speak(std::to_string(newFiles.size()) + " files pasted");
                }
            } catch (...) {
                // Silently ignore clipboard errors
            }
            return 0;
        }
    }

    if (msg == WM_SYSKEYDOWN) {
        std::vector<int> selected = GetSelectedIndices(hwnd);
        if (selected.empty()) return CallWindowProcW(g_playlistOrigProc, hwnd, msg, wParam, lParam);

        // Alt+Up: Move selected items up
        if (wParam == VK_UP && selected[0] > 0) {
            // Move items up one by one from the top
            for (int idx : selected) {
                std::swap(g_playlist[idx], g_playlist[idx - 1]);
                if (g_currentTrack == idx) g_currentTrack--;
                else if (g_currentTrack == idx - 1) g_currentTrack++;
            }
            // Rebuild and reselect
            RebuildPlaylistList(hwnd, selected[0] - 1);
            // Reselect all moved items
            for (int idx : selected) {
                SendMessageW(hwnd, LB_SETSEL, TRUE, idx - 1);
            }
            return 0;
        }

        // Alt+Down: Move selected items down
        int lastIdx = selected[selected.size() - 1];
        if (wParam == VK_DOWN && lastIdx < (int)g_playlist.size() - 1) {
            // Move items down one by one from the bottom
            for (int i = (int)selected.size() - 1; i >= 0; i--) {
                int idx = selected[i];
                std::swap(g_playlist[idx], g_playlist[idx + 1]);
                if (g_currentTrack == idx) g_currentTrack++;
                else if (g_currentTrack == idx + 1) g_currentTrack--;
            }
            // Rebuild and reselect
            RebuildPlaylistList(hwnd, selected[0] + 1);
            // Reselect all moved items
            for (int idx : selected) {
                SendMessageW(hwnd, LB_SETSEL, TRUE, idx + 1);
            }
            return 0;
        }
    }

    return CallWindowProcW(g_playlistOrigProc, hwnd, msg, wParam, lParam);
}

// Playlist dialog procedure
static INT_PTR CALLBACK PlaylistDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hList = nullptr;

    switch (msg) {
        case WM_INITDIALOG: {
            g_playlistDlg = hwnd;
            hList = GetDlgItem(hwnd, IDC_PLAYLIST_LIST);

            // Subclass the listbox
            g_playlistOrigProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PlaylistListProc)));

            RebuildPlaylistList(hList, g_currentTrack);
            SetFocus(hList);
            return FALSE;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            // Handle double-click on listbox
            if (LOWORD(wParam) == IDC_PLAYLIST_LIST && HIWORD(wParam) == LBN_DBLCLK) {
                int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)g_playlist.size()) {
                    PlayTrack(sel);
                    EndDialog(hwnd, IDOK);
                }
                return TRUE;
            }
            break;

        case WM_DESTROY:
            // Restore original listbox procedure
            if (g_playlistOrigProc && hList) {
                SetWindowLongPtrW(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_playlistOrigProc));
                g_playlistOrigProc = nullptr;
            }
            g_playlistDlg = nullptr;
            break;

        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

// Show playlist manager dialog
void ShowPlaylistDialog() {
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_PLAYLIST), g_hwnd, PlaylistDlgProc);
}

// Helper to show/hide tab controls
void ShowTabControls(HWND hwnd, int tab) {
    // Tab indices: 0=Playback, 1=Recording, 2=Speech, 3=Movement, 4=File Types, 5=Global Hotkeys,
    //              6=Effects, 7=Advanced, 8=YouTube, 9=SoundTouch, 10=Rubber Band, 11=Speedy, 12=MIDI

    // Playback tab controls (tab 0)
    int playbackCtrls[] = {IDC_SOUNDCARD, IDC_ALLOW_AMPLIFY, IDC_REMEMBER_STATE, IDC_REMEMBER_POS, IDC_BRING_TO_FRONT, IDC_LOAD_FOLDER, IDC_MINIMIZE_TO_TRAY, IDC_VOLUME_STEP, IDC_SHOW_TITLE, IDC_AUTO_ADVANCE};
    // Recording tab controls (tab 1)
    int recordingCtrls[] = {IDC_REC_PATH, IDC_REC_BROWSE, IDC_REC_TEMPLATE, IDC_REC_FORMAT, IDC_REC_BITRATE};
    // Speech tab controls (tab 2)
    int speechCtrls[] = {IDC_SPEECH_TRACKCHANGE, IDC_SPEECH_VOLUME, IDC_SPEECH_EFFECT};
    // Movement tab controls (tab 3)
    int movementCtrls[] = {IDC_SEEK_1S, IDC_SEEK_5S, IDC_SEEK_10S, IDC_SEEK_30S, IDC_SEEK_1M, IDC_SEEK_5M, IDC_SEEK_10M,
                           IDC_SEEK_1T, IDC_SEEK_5T, IDC_SEEK_10T, IDC_CHAPTER_SEEK};
    // File Types tab controls (tab 4)
    int fileTypeCtrls[] = {IDC_ASSOC_MP3, IDC_ASSOC_WAV, IDC_ASSOC_OGG, IDC_ASSOC_FLAC, IDC_ASSOC_M4A, IDC_ASSOC_WMA,
                           IDC_ASSOC_AAC, IDC_ASSOC_OPUS, IDC_ASSOC_AIFF, IDC_ASSOC_APE, IDC_ASSOC_WV,
                           IDC_ASSOC_M3U, IDC_ASSOC_M3U8, IDC_ASSOC_PLS, IDC_ASSOC_MID, IDC_ASSOC_MIDI};
    // Global Hotkeys tab controls (tab 5)
    int hotkeyCtrls[] = {IDC_HOTKEY_ENABLED, IDC_HOTKEY_LIST, IDC_HOTKEY_ADD, IDC_HOTKEY_EDIT, IDC_HOTKEY_REMOVE};
    // Effects tab controls (tab 6)
    int effectCtrls[] = {IDC_EFFECT_VOLUME, IDC_EFFECT_PITCH, IDC_EFFECT_TEMPO, IDC_EFFECT_RATE, IDC_RATE_STEP_MODE,
                         IDC_DSP_REVERB, IDC_DSP_ECHO, IDC_DSP_EQ, IDC_DSP_COMPRESSOR, IDC_DSP_STEREOWIDTH,
                         IDC_DSP_CENTERCANCEL, IDC_DSP_CONVOLUTION, IDC_CONV_IR, IDC_CONV_BROWSE};
    // Advanced tab controls (tab 7)
    int advancedCtrls[] = {IDC_BUFFER_SIZE, IDC_UPDATE_PERIOD, IDC_TEMPO_ALGORITHM,
                           IDC_EQ_BASS_FREQ, IDC_EQ_MID_FREQ, IDC_EQ_TREBLE_FREQ,
                           IDC_LEGACY_VOLUME};
    // YouTube tab controls (tab 8)
    int youtubeCtrls[] = {IDC_YTDLP_PATH, IDC_YTDLP_BROWSE, IDC_YT_APIKEY};
    // SoundTouch tab controls (tab 9)
    int soundtouchCtrls[] = {IDC_ST_AA_FILTER, IDC_ST_AA_LENGTH, IDC_ST_QUICK_ALGO, IDC_ST_SEQUENCE,
                             IDC_ST_SEEKWINDOW, IDC_ST_OVERLAP, IDC_ST_PREVENT_CLICK, IDC_ST_ALGORITHM};
    // Rubber Band tab controls (tab 10)
    int rubberbandCtrls[] = {IDC_RB_FORMANT, IDC_RB_PITCH_MODE, IDC_RB_WINDOW, IDC_RB_TRANSIENTS,
                             IDC_RB_DETECTOR, IDC_RB_CHANNELS, IDC_RB_PHASE, IDC_RB_SMOOTHING};
    // Speedy tab controls (tab 11)
    int speedyCtrls[] = {IDC_SPEEDY_NONLINEAR};
    // MIDI tab controls (tab 12)
    int midiCtrls[] = {IDC_MIDI_SOUNDFONT, IDC_MIDI_SF_BROWSE, IDC_MIDI_VOICES, IDC_MIDI_SINC};

    // Show/hide playback controls
    for (int id : playbackCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 0 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide recording controls
    for (int id : recordingCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 1 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide speech controls
    for (int id : speechCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 2 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide movement controls
    for (int id : movementCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 3 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide file type controls
    for (int id : fileTypeCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 4 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide hotkey controls
    for (int id : hotkeyCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 5 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide effect controls
    for (int id : effectCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 6 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide advanced controls
    for (int id : advancedCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 7 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide YouTube controls
    for (int id : youtubeCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 8 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide SoundTouch controls
    for (int id : soundtouchCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 9 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide Rubber Band controls
    for (int id : rubberbandCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 10 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide Speedy controls
    for (int id : speedyCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 11 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide MIDI controls
    for (int id : midiCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 12 ? SW_SHOW : SW_HIDE);
    }
}

// Options dialog procedure
INT_PTR CALLBACK OptionsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            // Initialize tab control
            HWND hTab = GetDlgItem(hwnd, IDC_TAB);
            TCITEMW tie = {0};
            tie.mask = TCIF_TEXT;
            tie.pszText = const_cast<LPWSTR>(L"Playback");
            TabCtrl_InsertItem(hTab, 0, &tie);
            tie.pszText = const_cast<LPWSTR>(L"Recording");
            TabCtrl_InsertItem(hTab, 1, &tie);
            tie.pszText = const_cast<LPWSTR>(L"Speech");
            TabCtrl_InsertItem(hTab, 2, &tie);
            tie.pszText = const_cast<LPWSTR>(L"Movement");
            TabCtrl_InsertItem(hTab, 3, &tie);
            tie.pszText = const_cast<LPWSTR>(L"File Types");
            TabCtrl_InsertItem(hTab, 4, &tie);
            tie.pszText = const_cast<LPWSTR>(L"Global Hotkeys");
            TabCtrl_InsertItem(hTab, 5, &tie);
            tie.pszText = const_cast<LPWSTR>(L"Effects");
            TabCtrl_InsertItem(hTab, 6, &tie);
            tie.pszText = const_cast<LPWSTR>(L"Advanced");
            TabCtrl_InsertItem(hTab, 7, &tie);
            tie.pszText = const_cast<LPWSTR>(L"YouTube");
            TabCtrl_InsertItem(hTab, 8, &tie);
            tie.pszText = const_cast<LPWSTR>(L"SoundTouch");
            TabCtrl_InsertItem(hTab, 9, &tie);
            tie.pszText = const_cast<LPWSTR>(L"Rubber Band");
            TabCtrl_InsertItem(hTab, 10, &tie);
            tie.pszText = const_cast<LPWSTR>(L"Speedy");
            TabCtrl_InsertItem(hTab, 11, &tie);
            tie.pszText = const_cast<LPWSTR>(L"MIDI");
            TabCtrl_InsertItem(hTab, 12, &tie);

            // Populate hotkey list and set enabled checkbox
            CheckDlgButton(hwnd, IDC_HOTKEY_ENABLED, g_hotkeysEnabled ? BST_CHECKED : BST_UNCHECKED);
            HWND hList = GetDlgItem(hwnd, IDC_HOTKEY_LIST);
            for (const auto& hk : g_hotkeys) {
                std::wstring item = FormatHotkey(hk.modifiers, hk.vk) + L" - " + g_hotkeyActions[hk.actionIdx].name;
                SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
            }

            // Populate sound card combo box
            HWND hCombo = GetDlgItem(hwnd, IDC_SOUNDCARD);
            BASS_DEVICEINFO info;
            int currentIndex = 0;

            for (int i = 1; BASS_GetDeviceInfo(i, &info); i++) {
                if (info.flags & BASS_DEVICE_ENABLED) {
                    // Convert device name to wide string
                    int len = MultiByteToWideChar(CP_ACP, 0, info.name, -1, nullptr, 0);
                    std::wstring wideName(len, 0);
                    MultiByteToWideChar(CP_ACP, 0, info.name, -1, &wideName[0], len);

                    int idx = static_cast<int>(SendMessageW(hCombo, CB_ADDSTRING, 0,
                        reinterpret_cast<LPARAM>(wideName.c_str())));
                    SendMessageW(hCombo, CB_SETITEMDATA, idx, i);

                    if (i == g_selectedDevice || (g_selectedDevice == -1 && (info.flags & BASS_DEVICE_DEFAULT))) {
                        currentIndex = idx;
                    }
                }
            }
            SendMessageW(hCombo, CB_SETCURSEL, currentIndex, 0);

            // Set amplify checkbox
            CheckDlgButton(hwnd, IDC_ALLOW_AMPLIFY, g_allowAmplify ? BST_CHECKED : BST_UNCHECKED);

            // Set remember playback state checkbox
            CheckDlgButton(hwnd, IDC_REMEMBER_STATE, g_rememberState ? BST_CHECKED : BST_UNCHECKED);

            // Set bring to front checkbox
            CheckDlgButton(hwnd, IDC_BRING_TO_FRONT, g_bringToFront ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_LOAD_FOLDER, g_loadFolder ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_MINIMIZE_TO_TRAY, g_minimizeToTray ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SHOW_TITLE, g_showTitleInWindow ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_AUTO_ADVANCE, g_autoAdvance ? BST_CHECKED : BST_UNCHECKED);

            // Populate volume step combo box
            {
                HWND hVolStepCombo = GetDlgItem(hwnd, IDC_VOLUME_STEP);
                const wchar_t* stepLabels[] = {L"1%", L"2%", L"5%", L"10%", L"15%", L"20%", L"25%"};
                const int stepValues[] = {1, 2, 5, 10, 15, 20, 25};
                int stepIndex = 1;  // Default to 2%
                for (int i = 0; i < 7; i++) {
                    SendMessageW(hVolStepCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(stepLabels[i]));
                    if (static_cast<int>(g_volumeStep * 100 + 0.5f) == stepValues[i]) {
                        stepIndex = i;
                    }
                }
                SendMessageW(hVolStepCombo, CB_SETCURSEL, stepIndex, 0);
            }

            // Populate remember position combo box
            HWND hPosCombo = GetDlgItem(hwnd, IDC_REMEMBER_POS);
            const wchar_t* posLabels[] = {L"Off", L"5 minutes", L"10 minutes", L"20 minutes", L"30 minutes", L"45 minutes", L"60 minutes"};
            int posIndex = 0;
            for (int i = 0; i < g_posThresholdCount; i++) {
                SendMessageW(hPosCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(posLabels[i]));
                if (g_posThresholds[i] == g_rememberPosMinutes) {
                    posIndex = i;
                }
            }
            SendMessageW(hPosCombo, CB_SETCURSEL, posIndex, 0);

            // Set seek amount checkboxes
            CheckDlgButton(hwnd, IDC_SEEK_1S, g_seekEnabled[0] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_5S, g_seekEnabled[1] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_10S, g_seekEnabled[2] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_30S, g_seekEnabled[3] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_1M, g_seekEnabled[4] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_5M, g_seekEnabled[5] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_10M, g_seekEnabled[6] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_1T, g_seekEnabled[7] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_5T, g_seekEnabled[8] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_10T, g_seekEnabled[9] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHAPTER_SEEK, g_chapterSeekEnabled ? BST_CHECKED : BST_UNCHECKED);

            // Set file association checkboxes based on current registry state
            for (int i = 0; i < g_fileAssocCount; i++) {
                CheckDlgButton(hwnd, g_fileAssocs[i].ctrlId,
                    IsExtensionAssociated(g_fileAssocs[i].ext) ? BST_CHECKED : BST_UNCHECKED);
            }

            // Set effect checkboxes
            CheckDlgButton(hwnd, IDC_EFFECT_VOLUME, g_effectEnabled[0] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_EFFECT_PITCH, g_effectEnabled[1] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_EFFECT_TEMPO, g_effectEnabled[2] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_EFFECT_RATE, g_effectEnabled[3] ? BST_CHECKED : BST_UNCHECKED);

            // Set rate step mode combobox
            {
                HWND hRateStepCombo = GetDlgItem(hwnd, IDC_RATE_STEP_MODE);
                SendMessageW(hRateStepCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"0.01x"));
                SendMessageW(hRateStepCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Semitone"));
                SendMessageW(hRateStepCombo, CB_SETCURSEL, g_rateStepMode, 0);
            }

            // Set reverb algorithm combobox
            {
                HWND hReverbCombo = GetDlgItem(hwnd, IDC_DSP_REVERB);
                SendMessageW(hReverbCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Off"));
                SendMessageW(hReverbCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Freeverb (Musical)"));
                SendMessageW(hReverbCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"DX8 (DirectX)"));
                SendMessageW(hReverbCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"I3DL2 (Environmental)"));
                SendMessageW(hReverbCombo, CB_SETCURSEL, g_reverbAlgorithm, 0);
            }

            // Set DSP effect checkboxes
            CheckDlgButton(hwnd, IDC_DSP_ECHO, IsDSPEffectEnabled(DSPEffectType::Echo) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_DSP_EQ, IsDSPEffectEnabled(DSPEffectType::EQ) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_DSP_COMPRESSOR, IsDSPEffectEnabled(DSPEffectType::Compressor) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_DSP_STEREOWIDTH, IsDSPEffectEnabled(DSPEffectType::StereoWidth) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_DSP_CENTERCANCEL, IsDSPEffectEnabled(DSPEffectType::CenterCancel) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_DSP_CONVOLUTION, IsDSPEffectEnabled(DSPEffectType::Convolution) ? BST_CHECKED : BST_UNCHECKED);

            // Display current IR file path (just filename)
            if (!g_convolutionIRPath.empty()) {
                std::wstring filename = g_convolutionIRPath;
                size_t pos = filename.find_last_of(L"\\/");
                if (pos != std::wstring::npos) {
                    filename = filename.substr(pos + 1);
                }
                SetDlgItemTextW(hwnd, IDC_CONV_IR, filename.c_str());
            }

            // Populate buffer size combo box
            {
                HWND hBufferCombo = GetDlgItem(hwnd, IDC_BUFFER_SIZE);
                int bufferIndex = 3;  // Default to 500ms
                for (int i = 0; i < g_bufferSizeCount; i++) {
                    wchar_t label[32];
                    swprintf(label, 32, L"%d ms", g_bufferSizes[i]);
                    SendMessageW(hBufferCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
                    if (g_bufferSizes[i] == g_bufferSize) {
                        bufferIndex = i;
                    }
                }
                SendMessageW(hBufferCombo, CB_SETCURSEL, bufferIndex, 0);
            }

            // Populate update period combo box
            {
                HWND hUpdateCombo = GetDlgItem(hwnd, IDC_UPDATE_PERIOD);
                int updateIndex = 4;  // Default to 100ms
                for (int i = 0; i < g_updatePeriodCount; i++) {
                    wchar_t label[32];
                    swprintf(label, 32, L"%d ms", g_updatePeriods[i]);
                    SendMessageW(hUpdateCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
                    if (g_updatePeriods[i] == g_updatePeriod) {
                        updateIndex = i;
                    }
                }
                SendMessageW(hUpdateCombo, CB_SETCURSEL, updateIndex, 0);
            }

            // Populate tempo algorithm combo box
            {
                HWND hAlgoCombo = GetDlgItem(hwnd, IDC_TEMPO_ALGORITHM);
                SendMessageW(hAlgoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"SoundTouch (BASS_FX) - Fast, good for speech"));
#ifdef USE_RUBBERBAND
                SendMessageW(hAlgoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Rubber Band R2 (Faster) - Balanced quality"));
                SendMessageW(hAlgoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Rubber Band R3 (Finer) - Highest quality"));
#else
                SendMessageW(hAlgoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Rubber Band R2 (coming soon)"));
                SendMessageW(hAlgoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Rubber Band R3 (coming soon)"));
#endif
#ifdef USE_SPEEDY
                SendMessageW(hAlgoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Speedy (Google) - Nonlinear speech speedup"));
#else
                SendMessageW(hAlgoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Speedy (coming soon)"));
#endif
                SendMessageW(hAlgoCombo, CB_SETCURSEL, g_tempoAlgorithm, 0);
            }

            // Initialize EQ frequency edit controls
            {
                wchar_t buf[32];
                swprintf(buf, 32, L"%.0f", g_eqBassFreq);
                SetDlgItemTextW(hwnd, IDC_EQ_BASS_FREQ, buf);
                swprintf(buf, 32, L"%.0f", g_eqMidFreq);
                SetDlgItemTextW(hwnd, IDC_EQ_MID_FREQ, buf);
                swprintf(buf, 32, L"%.0f", g_eqTrebleFreq);
                SetDlgItemTextW(hwnd, IDC_EQ_TREBLE_FREQ, buf);
            }

            // Initialize legacy volume checkbox
            CheckDlgButton(hwnd, IDC_LEGACY_VOLUME, g_legacyVolume ? BST_CHECKED : BST_UNCHECKED);

            // Initialize YouTube tab
            SetDlgItemTextW(hwnd, IDC_YTDLP_PATH, g_ytdlpPath.c_str());
            SetDlgItemTextW(hwnd, IDC_YT_APIKEY, g_ytApiKey.c_str());

            // Initialize Recording tab
            {
                // Set default recording path to user's Music folder if not set
                if (g_recordPath.empty()) {
                    wchar_t musicPath[MAX_PATH];
                    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYMUSIC, nullptr, 0, musicPath))) {
                        g_recordPath = musicPath;
                    }
                }
                SetDlgItemTextW(hwnd, IDC_REC_PATH, g_recordPath.c_str());
                SetDlgItemTextW(hwnd, IDC_REC_TEMPLATE, g_recordTemplate.c_str());

                // Format combo: WAV, MP3, OGG, FLAC
                HWND hFormatCombo = GetDlgItem(hwnd, IDC_REC_FORMAT);
                SendMessageW(hFormatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WAV (lossless)"));
                SendMessageW(hFormatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MP3"));
                SendMessageW(hFormatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"OGG Vorbis"));
                SendMessageW(hFormatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"FLAC (lossless)"));
                SendMessageW(hFormatCombo, CB_SETCURSEL, g_recordFormat, 0);

                // Bitrate combo (for MP3/OGG)
                HWND hBitrateCombo = GetDlgItem(hwnd, IDC_REC_BITRATE);
                int bitrates[] = {128, 160, 192, 224, 256, 320};
                int bitrateIndex = 2;  // Default to 192
                for (int i = 0; i < 6; i++) {
                    wchar_t buf[16];
                    swprintf(buf, 16, L"%d kbps", bitrates[i]);
                    SendMessageW(hBitrateCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
                    if (bitrates[i] == g_recordBitrate) {
                        bitrateIndex = i;
                    }
                }
                SendMessageW(hBitrateCombo, CB_SETCURSEL, bitrateIndex, 0);

                // Enable bitrate only for lossy formats (MP3=1, OGG=2)
                BOOL enableBitrate = (g_recordFormat == 1 || g_recordFormat == 2);
                EnableWindow(hBitrateCombo, enableBitrate);
            }

            // Initialize Speech tab
            CheckDlgButton(hwnd, IDC_SPEECH_TRACKCHANGE, g_speechTrackChange ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SPEECH_VOLUME, g_speechVolume ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SPEECH_EFFECT, g_speechEffect ? BST_CHECKED : BST_UNCHECKED);

            // Initialize SoundTouch tab
            {
                CheckDlgButton(hwnd, IDC_ST_AA_FILTER, g_stAntiAliasFilter ? BST_CHECKED : BST_UNCHECKED);
                CheckDlgButton(hwnd, IDC_ST_QUICK_ALGO, g_stQuickAlgorithm ? BST_CHECKED : BST_UNCHECKED);
                CheckDlgButton(hwnd, IDC_ST_PREVENT_CLICK, g_stPreventClick ? BST_CHECKED : BST_UNCHECKED);

                // AA filter length combo (8, 16, 32, 64, 128)
                HWND hAALen = GetDlgItem(hwnd, IDC_ST_AA_LENGTH);
                int aaLengths[] = {8, 16, 32, 64, 128};
                int aaIndex = 2;  // Default to 32
                for (int i = 0; i < 5; i++) {
                    wchar_t buf[16];
                    swprintf(buf, 16, L"%d", aaLengths[i]);
                    SendMessageW(hAALen, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
                    if (aaLengths[i] == g_stAAFilterLength) aaIndex = i;
                }
                SendMessageW(hAALen, CB_SETCURSEL, aaIndex, 0);

                // Interpolation algorithm combo
                HWND hAlgo = GetDlgItem(hwnd, IDC_ST_ALGORITHM);
                SendMessageW(hAlgo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Linear"));
                SendMessageW(hAlgo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Cubic"));
                SendMessageW(hAlgo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Shannon"));
                SendMessageW(hAlgo, CB_SETCURSEL, g_stAlgorithm, 0);

                // Sequence, seek window, overlap edit controls
                wchar_t buf[16];
                swprintf(buf, 16, L"%d", g_stSequenceMs);
                SetDlgItemTextW(hwnd, IDC_ST_SEQUENCE, buf);
                swprintf(buf, 16, L"%d", g_stSeekWindowMs);
                SetDlgItemTextW(hwnd, IDC_ST_SEEKWINDOW, buf);
                swprintf(buf, 16, L"%d", g_stOverlapMs);
                SetDlgItemTextW(hwnd, IDC_ST_OVERLAP, buf);
            }

            // Initialize Rubber Band tab
            {
                CheckDlgButton(hwnd, IDC_RB_FORMANT, g_rbFormantPreserved ? BST_CHECKED : BST_UNCHECKED);
                CheckDlgButton(hwnd, IDC_RB_SMOOTHING, g_rbSmoothing ? BST_CHECKED : BST_UNCHECKED);

                // Pitch mode combo
                HWND hPitch = GetDlgItem(hwnd, IDC_RB_PITCH_MODE);
                SendMessageW(hPitch, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"High Speed"));
                SendMessageW(hPitch, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"High Quality"));
                SendMessageW(hPitch, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"High Consistency"));
                SendMessageW(hPitch, CB_SETCURSEL, g_rbPitchMode, 0);

                // Window size combo
                HWND hWindow = GetDlgItem(hwnd, IDC_RB_WINDOW);
                SendMessageW(hWindow, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Standard"));
                SendMessageW(hWindow, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Short"));
                SendMessageW(hWindow, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Long"));
                SendMessageW(hWindow, CB_SETCURSEL, g_rbWindowSize, 0);

                // Transients combo (R2 only)
                HWND hTrans = GetDlgItem(hwnd, IDC_RB_TRANSIENTS);
                SendMessageW(hTrans, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Crisp"));
                SendMessageW(hTrans, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Mixed"));
                SendMessageW(hTrans, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Smooth"));
                SendMessageW(hTrans, CB_SETCURSEL, g_rbTransients, 0);

                // Detector combo (R2 only)
                HWND hDetect = GetDlgItem(hwnd, IDC_RB_DETECTOR);
                SendMessageW(hDetect, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Compound"));
                SendMessageW(hDetect, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Percussive"));
                SendMessageW(hDetect, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Soft"));
                SendMessageW(hDetect, CB_SETCURSEL, g_rbDetector, 0);

                // Channels combo
                HWND hChan = GetDlgItem(hwnd, IDC_RB_CHANNELS);
                SendMessageW(hChan, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Apart"));
                SendMessageW(hChan, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Together"));
                SendMessageW(hChan, CB_SETCURSEL, g_rbChannels, 0);

                // Phase combo (R2 only)
                HWND hPhase = GetDlgItem(hwnd, IDC_RB_PHASE);
                SendMessageW(hPhase, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Laminar"));
                SendMessageW(hPhase, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Independent"));
                SendMessageW(hPhase, CB_SETCURSEL, g_rbPhase, 0);
            }

            // Initialize Speedy tab
            {
                CheckDlgButton(hwnd, IDC_SPEEDY_NONLINEAR, g_speedyNonlinear ? BST_CHECKED : BST_UNCHECKED);
            }

            // Initialize MIDI tab controls
            {
                SetDlgItemTextW(hwnd, IDC_MIDI_SOUNDFONT, g_midiSoundFont.c_str());

                wchar_t buf[32];
                swprintf(buf, 32, L"%d", g_midiMaxVoices);
                SetDlgItemTextW(hwnd, IDC_MIDI_VOICES, buf);

                CheckDlgButton(hwnd, IDC_MIDI_SINC, g_midiSincInterp ? BST_CHECKED : BST_UNCHECKED);
            }

            // Show only playback tab controls initially
            ShowTabControls(hwnd, 0);

            return TRUE;
        }

        case WM_NOTIFY: {
            NMHDR* pnmh = reinterpret_cast<NMHDR*>(lParam);
            if (pnmh->idFrom == IDC_TAB && pnmh->code == TCN_SELCHANGE) {
                int tab = TabCtrl_GetCurSel(GetDlgItem(hwnd, IDC_TAB));
                ShowTabControls(hwnd, tab);
            }
            break;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    // Get selected device
                    HWND hCombo = GetDlgItem(hwnd, IDC_SOUNDCARD);
                    int sel = static_cast<int>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0));
                    int newDevice = static_cast<int>(SendMessageW(hCombo, CB_GETITEMDATA, sel, 0));

                    // Get amplify setting
                    bool newAmplify = (IsDlgButtonChecked(hwnd, IDC_ALLOW_AMPLIFY) == BST_CHECKED);

                    // Get remember playback state setting
                    g_rememberState = (IsDlgButtonChecked(hwnd, IDC_REMEMBER_STATE) == BST_CHECKED);

                    // Get bring to front setting
                    g_bringToFront = (IsDlgButtonChecked(hwnd, IDC_BRING_TO_FRONT) == BST_CHECKED);
                    g_loadFolder = (IsDlgButtonChecked(hwnd, IDC_LOAD_FOLDER) == BST_CHECKED);
                    g_minimizeToTray = (IsDlgButtonChecked(hwnd, IDC_MINIMIZE_TO_TRAY) == BST_CHECKED);
                    g_showTitleInWindow = (IsDlgButtonChecked(hwnd, IDC_SHOW_TITLE) == BST_CHECKED);
                    g_autoAdvance = (IsDlgButtonChecked(hwnd, IDC_AUTO_ADVANCE) == BST_CHECKED);
                    UpdateWindowTitle();  // Apply immediately

                    // Get volume step setting
                    {
                        HWND hVolStepCombo = GetDlgItem(hwnd, IDC_VOLUME_STEP);
                        int volStepSel = static_cast<int>(SendMessageW(hVolStepCombo, CB_GETCURSEL, 0, 0));
                        const int stepValues[] = {1, 2, 5, 10, 15, 20, 25};
                        if (volStepSel >= 0 && volStepSel < 7) {
                            g_volumeStep = stepValues[volStepSel] / 100.0f;
                        }
                    }

                    // Get remember position threshold
                    HWND hPosCombo = GetDlgItem(hwnd, IDC_REMEMBER_POS);
                    int posSel = static_cast<int>(SendMessageW(hPosCombo, CB_GETCURSEL, 0, 0));
                    if (posSel >= 0 && posSel < g_posThresholdCount) {
                        g_rememberPosMinutes = g_posThresholds[posSel];
                    }

                    // Apply device change if needed
                    if (newDevice != g_selectedDevice) {
                        ReinitBass(newDevice);
                    }

                    // Apply amplify setting
                    g_allowAmplify = newAmplify;

                    // Clamp volume if amplify was disabled
                    if (!g_allowAmplify && g_volume > MAX_VOLUME_NORMAL) {
                        SetVolume(MAX_VOLUME_NORMAL);
                    }

                    // Get seek amount checkboxes
                    g_seekEnabled[0] = (IsDlgButtonChecked(hwnd, IDC_SEEK_1S) == BST_CHECKED);
                    g_seekEnabled[1] = (IsDlgButtonChecked(hwnd, IDC_SEEK_5S) == BST_CHECKED);
                    g_seekEnabled[2] = (IsDlgButtonChecked(hwnd, IDC_SEEK_10S) == BST_CHECKED);
                    g_seekEnabled[3] = (IsDlgButtonChecked(hwnd, IDC_SEEK_30S) == BST_CHECKED);
                    g_seekEnabled[4] = (IsDlgButtonChecked(hwnd, IDC_SEEK_1M) == BST_CHECKED);
                    g_seekEnabled[5] = (IsDlgButtonChecked(hwnd, IDC_SEEK_5M) == BST_CHECKED);
                    g_seekEnabled[6] = (IsDlgButtonChecked(hwnd, IDC_SEEK_10M) == BST_CHECKED);
                    g_seekEnabled[7] = (IsDlgButtonChecked(hwnd, IDC_SEEK_1T) == BST_CHECKED);
                    g_seekEnabled[8] = (IsDlgButtonChecked(hwnd, IDC_SEEK_5T) == BST_CHECKED);
                    g_seekEnabled[9] = (IsDlgButtonChecked(hwnd, IDC_SEEK_10T) == BST_CHECKED);
                    g_chapterSeekEnabled = (IsDlgButtonChecked(hwnd, IDC_CHAPTER_SEEK) == BST_CHECKED);

                    // Validate current seek index - ensure it points to an enabled amount
                    if (!g_seekEnabled[g_currentSeekIndex]) {
                        for (int i = 0; i < g_seekAmountCount; i++) {
                            if (g_seekEnabled[i]) {
                                g_currentSeekIndex = i;
                                break;
                            }
                        }
                    }

                    // Update file associations
                    for (int i = 0; i < g_fileAssocCount; i++) {
                        bool checked = (IsDlgButtonChecked(hwnd, g_fileAssocs[i].ctrlId) == BST_CHECKED);
                        bool current = IsExtensionAssociated(g_fileAssocs[i].ext);
                        if (checked != current) {
                            SetFileAssociation(g_fileAssocs[i].ext, checked);
                        }
                    }

                    // Get effect checkboxes
                    g_effectEnabled[0] = (IsDlgButtonChecked(hwnd, IDC_EFFECT_VOLUME) == BST_CHECKED);
                    g_effectEnabled[1] = (IsDlgButtonChecked(hwnd, IDC_EFFECT_PITCH) == BST_CHECKED);
                    g_effectEnabled[2] = (IsDlgButtonChecked(hwnd, IDC_EFFECT_TEMPO) == BST_CHECKED);
                    g_effectEnabled[3] = (IsDlgButtonChecked(hwnd, IDC_EFFECT_RATE) == BST_CHECKED);

                    // Get rate step mode
                    {
                        HWND hRateStepCombo = GetDlgItem(hwnd, IDC_RATE_STEP_MODE);
                        int rateStepSel = static_cast<int>(SendMessageW(hRateStepCombo, CB_GETCURSEL, 0, 0));
                        if (rateStepSel >= 0 && rateStepSel <= 1) {
                            g_rateStepMode = rateStepSel;
                        }
                    }

                    // Validate current effect index - ensure it points to an enabled effect
                    if (!g_effectEnabled[g_currentEffectIndex]) {
                        for (int i = 0; i < 4; i++) {
                            if (g_effectEnabled[i]) {
                                g_currentEffectIndex = i;
                                break;
                            }
                        }
                    }

                    // Get reverb algorithm from combobox
                    {
                        HWND hReverbCombo = GetDlgItem(hwnd, IDC_DSP_REVERB);
                        int reverbSel = static_cast<int>(SendMessageW(hReverbCombo, CB_GETCURSEL, 0, 0));
                        if (reverbSel >= 0 && reverbSel <= 3) {
                            SetReverbAlgorithm(reverbSel);
                        }
                    }

                    // Get DSP effect checkboxes and enable/disable effects
                    EnableDSPEffect(DSPEffectType::Echo, IsDlgButtonChecked(hwnd, IDC_DSP_ECHO) == BST_CHECKED);
                    EnableDSPEffect(DSPEffectType::EQ, IsDlgButtonChecked(hwnd, IDC_DSP_EQ) == BST_CHECKED);
                    EnableDSPEffect(DSPEffectType::Compressor, IsDlgButtonChecked(hwnd, IDC_DSP_COMPRESSOR) == BST_CHECKED);
                    EnableDSPEffect(DSPEffectType::StereoWidth, IsDlgButtonChecked(hwnd, IDC_DSP_STEREOWIDTH) == BST_CHECKED);
                    EnableDSPEffect(DSPEffectType::CenterCancel, IsDlgButtonChecked(hwnd, IDC_DSP_CENTERCANCEL) == BST_CHECKED);
                    EnableDSPEffect(DSPEffectType::Convolution, IsDlgButtonChecked(hwnd, IDC_DSP_CONVOLUTION) == BST_CHECKED);

                    // Get buffer settings
                    {
                        HWND hBufferCombo = GetDlgItem(hwnd, IDC_BUFFER_SIZE);
                        int bufferSel = static_cast<int>(SendMessageW(hBufferCombo, CB_GETCURSEL, 0, 0));
                        if (bufferSel >= 0 && bufferSel < g_bufferSizeCount) {
                            g_bufferSize = g_bufferSizes[bufferSel];
                            BASS_SetConfig(BASS_CONFIG_BUFFER, g_bufferSize);
                        }

                        HWND hUpdateCombo = GetDlgItem(hwnd, IDC_UPDATE_PERIOD);
                        int updateSel = static_cast<int>(SendMessageW(hUpdateCombo, CB_GETCURSEL, 0, 0));
                        if (updateSel >= 0 && updateSel < g_updatePeriodCount) {
                            g_updatePeriod = g_updatePeriods[updateSel];
                            BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, g_updatePeriod);
                        }

                        HWND hAlgoCombo = GetDlgItem(hwnd, IDC_TEMPO_ALGORITHM);
                        int algoSel = static_cast<int>(SendMessageW(hAlgoCombo, CB_GETCURSEL, 0, 0));
                        if (algoSel >= 0 && algoSel <= 3) {
                            g_tempoAlgorithm = algoSel;
                        }

                        // Get EQ frequencies
                        wchar_t freqBuf[32];
                        GetDlgItemTextW(hwnd, IDC_EQ_BASS_FREQ, freqBuf, 32);
                        float bassFreq = static_cast<float>(_wtof(freqBuf));
                        if (bassFreq >= 20.0f && bassFreq <= 500.0f) g_eqBassFreq = bassFreq;

                        GetDlgItemTextW(hwnd, IDC_EQ_MID_FREQ, freqBuf, 32);
                        float midFreq = static_cast<float>(_wtof(freqBuf));
                        if (midFreq >= 200.0f && midFreq <= 5000.0f) g_eqMidFreq = midFreq;

                        GetDlgItemTextW(hwnd, IDC_EQ_TREBLE_FREQ, freqBuf, 32);
                        float trebleFreq = static_cast<float>(_wtof(freqBuf));
                        if (trebleFreq >= 2000.0f && trebleFreq <= 20000.0f) g_eqTrebleFreq = trebleFreq;

                        // Get legacy volume setting
                        bool wasLegacy = g_legacyVolume;
                        g_legacyVolume = (IsDlgButtonChecked(hwnd, IDC_LEGACY_VOLUME) == BST_CHECKED);

                        // Handle mode switch
                        if (wasLegacy != g_legacyVolume && g_fxStream) {
                            if (g_legacyVolume) {
                                // Switching TO legacy: apply volume via BASS_ATTRIB_VOL
                                float curvedVolume = g_muted ? 0.0f : (g_volume * g_volume);
                                BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, curvedVolume);
                            } else {
                                // Switching FROM legacy: reset BASS_ATTRIB_VOL to 1.0 so DSP works
                                BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, 1.0f);
                                // Ensure volume DSP is set up
                                ApplyDSPEffects();
                            }
                        }
                    }

                    // Get YouTube settings
                    {
                        wchar_t buf[512];
                        GetDlgItemTextW(hwnd, IDC_YTDLP_PATH, buf, 512);
                        g_ytdlpPath = buf;
                        GetDlgItemTextW(hwnd, IDC_YT_APIKEY, buf, 512);
                        g_ytApiKey = buf;
                    }

                    // Get Recording settings
                    {
                        wchar_t buf[512];
                        GetDlgItemTextW(hwnd, IDC_REC_PATH, buf, 512);
                        g_recordPath = buf;
                        GetDlgItemTextW(hwnd, IDC_REC_TEMPLATE, buf, 512);
                        g_recordTemplate = buf;

                        HWND hFormatCombo = GetDlgItem(hwnd, IDC_REC_FORMAT);
                        int formatSel = static_cast<int>(SendMessageW(hFormatCombo, CB_GETCURSEL, 0, 0));
                        if (formatSel >= 0 && formatSel <= 3) g_recordFormat = formatSel;

                        HWND hBitrateCombo = GetDlgItem(hwnd, IDC_REC_BITRATE);
                        int bitrateSel = static_cast<int>(SendMessageW(hBitrateCombo, CB_GETCURSEL, 0, 0));
                        int bitrates[] = {128, 160, 192, 224, 256, 320};
                        if (bitrateSel >= 0 && bitrateSel < 6) g_recordBitrate = bitrates[bitrateSel];
                    }

                    // Get Speech settings
                    g_speechTrackChange = (IsDlgButtonChecked(hwnd, IDC_SPEECH_TRACKCHANGE) == BST_CHECKED);
                    g_speechVolume = (IsDlgButtonChecked(hwnd, IDC_SPEECH_VOLUME) == BST_CHECKED);
                    g_speechEffect = (IsDlgButtonChecked(hwnd, IDC_SPEECH_EFFECT) == BST_CHECKED);

                    // Get SoundTouch settings
                    {
                        g_stAntiAliasFilter = (IsDlgButtonChecked(hwnd, IDC_ST_AA_FILTER) == BST_CHECKED);
                        g_stQuickAlgorithm = (IsDlgButtonChecked(hwnd, IDC_ST_QUICK_ALGO) == BST_CHECKED);
                        g_stPreventClick = (IsDlgButtonChecked(hwnd, IDC_ST_PREVENT_CLICK) == BST_CHECKED);

                        wchar_t buf[32];
                        GetDlgItemTextW(hwnd, IDC_ST_AA_LENGTH, buf, 32);
                        int aaLen = _wtoi(buf);
                        if (aaLen >= 8 && aaLen <= 128) g_stAAFilterLength = aaLen;

                        GetDlgItemTextW(hwnd, IDC_ST_SEQUENCE, buf, 32);
                        int seq = _wtoi(buf);
                        if (seq >= 0 && seq <= 200) g_stSequenceMs = seq;

                        GetDlgItemTextW(hwnd, IDC_ST_SEEKWINDOW, buf, 32);
                        int seek = _wtoi(buf);
                        if (seek >= 0 && seek <= 100) g_stSeekWindowMs = seek;

                        GetDlgItemTextW(hwnd, IDC_ST_OVERLAP, buf, 32);
                        int overlap = _wtoi(buf);
                        if (overlap >= 0 && overlap <= 50) g_stOverlapMs = overlap;

                        HWND hAlgoCombo = GetDlgItem(hwnd, IDC_ST_ALGORITHM);
                        int algoSel = static_cast<int>(SendMessageW(hAlgoCombo, CB_GETCURSEL, 0, 0));
                        if (algoSel >= 0 && algoSel <= 2) g_stAlgorithm = algoSel;
                    }

                    // Get Rubber Band settings
                    {
                        g_rbFormantPreserved = (IsDlgButtonChecked(hwnd, IDC_RB_FORMANT) == BST_CHECKED);
                        g_rbSmoothing = (IsDlgButtonChecked(hwnd, IDC_RB_SMOOTHING) == BST_CHECKED);

                        HWND hPitchCombo = GetDlgItem(hwnd, IDC_RB_PITCH_MODE);
                        int pitchSel = static_cast<int>(SendMessageW(hPitchCombo, CB_GETCURSEL, 0, 0));
                        if (pitchSel >= 0 && pitchSel <= 2) g_rbPitchMode = pitchSel;

                        HWND hWindowCombo = GetDlgItem(hwnd, IDC_RB_WINDOW);
                        int windowSel = static_cast<int>(SendMessageW(hWindowCombo, CB_GETCURSEL, 0, 0));
                        if (windowSel >= 0 && windowSel <= 2) g_rbWindowSize = windowSel;

                        HWND hTransCombo = GetDlgItem(hwnd, IDC_RB_TRANSIENTS);
                        int transSel = static_cast<int>(SendMessageW(hTransCombo, CB_GETCURSEL, 0, 0));
                        if (transSel >= 0 && transSel <= 2) g_rbTransients = transSel;

                        HWND hDetectorCombo = GetDlgItem(hwnd, IDC_RB_DETECTOR);
                        int detectorSel = static_cast<int>(SendMessageW(hDetectorCombo, CB_GETCURSEL, 0, 0));
                        if (detectorSel >= 0 && detectorSel <= 2) g_rbDetector = detectorSel;

                        HWND hChannelsCombo = GetDlgItem(hwnd, IDC_RB_CHANNELS);
                        int channelsSel = static_cast<int>(SendMessageW(hChannelsCombo, CB_GETCURSEL, 0, 0));
                        if (channelsSel >= 0 && channelsSel <= 1) g_rbChannels = channelsSel;

                        HWND hPhaseCombo = GetDlgItem(hwnd, IDC_RB_PHASE);
                        int phaseSel = static_cast<int>(SendMessageW(hPhaseCombo, CB_GETCURSEL, 0, 0));
                        if (phaseSel >= 0 && phaseSel <= 1) g_rbPhase = phaseSel;
                    }

                    // Get Speedy settings
                    {
                        g_speedyNonlinear = (IsDlgButtonChecked(hwnd, IDC_SPEEDY_NONLINEAR) == BST_CHECKED);
                    }

                    // Get MIDI settings
                    {
                        wchar_t buf[MAX_PATH];
                        GetDlgItemTextW(hwnd, IDC_MIDI_SOUNDFONT, buf, MAX_PATH);
                        g_midiSoundFont = buf;

                        wchar_t voicesBuf[32];
                        GetDlgItemTextW(hwnd, IDC_MIDI_VOICES, voicesBuf, 32);
                        int voices = _wtoi(voicesBuf);
                        if (voices >= 1 && voices <= 1000) g_midiMaxVoices = voices;

                        g_midiSincInterp = (IsDlgButtonChecked(hwnd, IDC_MIDI_SINC) == BST_CHECKED);
                    }

                    // Save settings
                    SaveSettings();

                    EndDialog(hwnd, IDOK);
                    return TRUE;
                }

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;

                case IDC_REC_BROWSE: {
                    // Browse for recording output folder
                    BROWSEINFOW bi = {0};
                    bi.hwndOwner = hwnd;
                    bi.lpszTitle = L"Select recording output folder";
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                    if (pidl) {
                        wchar_t folderPath[MAX_PATH];
                        if (SHGetPathFromIDListW(pidl, folderPath)) {
                            SetDlgItemTextW(hwnd, IDC_REC_PATH, folderPath);
                        }
                        CoTaskMemFree(pidl);
                    }
                    return TRUE;
                }

                case IDC_REC_FORMAT:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        // Enable bitrate only for lossy formats (MP3=1, OGG=2)
                        int format = static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_REC_FORMAT), CB_GETCURSEL, 0, 0));
                        BOOL enableBitrate = (format == 1 || format == 2);
                        EnableWindow(GetDlgItem(hwnd, IDC_REC_BITRATE), enableBitrate);
                    }
                    return TRUE;

                case IDC_YTDLP_BROWSE: {
                    wchar_t filePath[MAX_PATH] = {0};
                    OPENFILENAMEW ofn = {sizeof(ofn)};
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = L"Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
                    ofn.lpstrFile = filePath;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrTitle = L"Select yt-dlp executable";
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn)) {
                        SetDlgItemTextW(hwnd, IDC_YTDLP_PATH, filePath);
                    }
                    return TRUE;
                }

                case IDC_MIDI_SF_BROWSE: {
                    wchar_t filePath[MAX_PATH] = {0};
                    OPENFILENAMEW ofn = {sizeof(ofn)};
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = L"SoundFont Files (*.sf2;*.sf3;*.sfz)\0*.sf2;*.sf3;*.sfz\0All Files (*.*)\0*.*\0";
                    ofn.lpstrFile = filePath;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrTitle = L"Select SoundFont file";
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn)) {
                        SetDlgItemTextW(hwnd, IDC_MIDI_SOUNDFONT, filePath);
                    }
                    return TRUE;
                }

                case IDC_CONV_BROWSE: {
                    wchar_t filePath[MAX_PATH] = {0};
                    OPENFILENAMEW ofn = {sizeof(ofn)};
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = L"IR Files (*.wav;*.flac;*.ogg;*.mp3)\0*.wav;*.flac;*.ogg;*.mp3\0"
                                      L"WAV Files (*.wav)\0*.wav\0"
                                      L"FLAC Files (*.flac)\0*.flac\0"
                                      L"All Files (*.*)\0*.*\0";
                    ofn.lpstrFile = filePath;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrTitle = L"Select Impulse Response file";
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn)) {
                        g_convolutionIRPath = filePath;
                        // Display just the filename
                        std::wstring filename = filePath;
                        size_t pos = filename.find_last_of(L"\\/");
                        if (pos != std::wstring::npos) {
                            filename = filename.substr(pos + 1);
                        }
                        SetDlgItemTextW(hwnd, IDC_CONV_IR, filename.c_str());
                        // Load the IR file
                        ConvolutionReverb* conv = GetConvolutionReverb();
                        if (conv) {
                            conv->LoadIR(filePath);
                        }
                    }
                    return TRUE;
                }

                case IDC_HOTKEY_ADD: {
                    HotkeyDlgData data = {0, 0, 0, false};
                    if (DialogBoxParamW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_HOTKEY), hwnd, HotkeyDlgProc, reinterpret_cast<LPARAM>(&data)) == IDOK) {
                        // Add new hotkey
                        GlobalHotkey hk;
                        hk.id = g_nextHotkeyId++;
                        hk.modifiers = data.modifiers;
                        hk.vk = data.vk;
                        hk.actionIdx = data.actionIdx;
                        g_hotkeys.push_back(hk);

                        // Update list
                        HWND hList = GetDlgItem(hwnd, IDC_HOTKEY_LIST);
                        std::wstring item = FormatHotkey(hk.modifiers, hk.vk) + L" - " + g_hotkeyActions[hk.actionIdx].name;
                        SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));

                        // Re-register hotkeys
                        UnregisterGlobalHotkeys();
                        RegisterGlobalHotkeys();
                        SaveHotkeys();
                    }
                    return TRUE;
                }

                case IDC_HOTKEY_EDIT: {
                    HWND hList = GetDlgItem(hwnd, IDC_HOTKEY_LIST);
                    int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < static_cast<int>(g_hotkeys.size())) {
                        HotkeyDlgData data;
                        data.actionIdx = g_hotkeys[sel].actionIdx;
                        data.modifiers = g_hotkeys[sel].modifiers;
                        data.vk = g_hotkeys[sel].vk;
                        data.isEdit = true;

                        if (DialogBoxParamW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_HOTKEY), hwnd, HotkeyDlgProc, reinterpret_cast<LPARAM>(&data)) == IDOK) {
                            // Update hotkey
                            UnregisterHotKey(g_hwnd, g_hotkeys[sel].id);
                            g_hotkeys[sel].modifiers = data.modifiers;
                            g_hotkeys[sel].vk = data.vk;
                            g_hotkeys[sel].actionIdx = data.actionIdx;
                            RegisterHotKey(g_hwnd, g_hotkeys[sel].id, g_hotkeys[sel].modifiers, g_hotkeys[sel].vk);

                            // Update list item
                            SendMessageW(hList, LB_DELETESTRING, sel, 0);
                            std::wstring item = FormatHotkey(g_hotkeys[sel].modifiers, g_hotkeys[sel].vk) + L" - " + g_hotkeyActions[g_hotkeys[sel].actionIdx].name;
                            SendMessageW(hList, LB_INSERTSTRING, sel, reinterpret_cast<LPARAM>(item.c_str()));
                            SendMessageW(hList, LB_SETCURSEL, sel, 0);

                            SaveHotkeys();
                        }
                    }
                    return TRUE;
                }

                case IDC_HOTKEY_REMOVE: {
                    HWND hList = GetDlgItem(hwnd, IDC_HOTKEY_LIST);
                    int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < static_cast<int>(g_hotkeys.size())) {
                        // Unregister and remove
                        UnregisterHotKey(g_hwnd, g_hotkeys[sel].id);
                        g_hotkeys.erase(g_hotkeys.begin() + sel);
                        SendMessageW(hList, LB_DELETESTRING, sel, 0);

                        // Select next item or previous
                        if (sel >= static_cast<int>(g_hotkeys.size())) {
                            sel = static_cast<int>(g_hotkeys.size()) - 1;
                        }
                        if (sel >= 0) {
                            SendMessageW(hList, LB_SETCURSEL, sel, 0);
                        }

                        SaveHotkeys();
                    }
                    return TRUE;
                }

                case IDC_HOTKEY_ENABLED: {
                    bool newEnabled = (IsDlgButtonChecked(hwnd, IDC_HOTKEY_ENABLED) == BST_CHECKED);
                    if (newEnabled != g_hotkeysEnabled) {
                        g_hotkeysEnabled = newEnabled;
                        if (g_hotkeysEnabled) {
                            RegisterGlobalHotkeys();
                        } else {
                            UnregisterGlobalHotkeys();
                        }
                        SaveHotkeys();
                    }
                    return TRUE;
                }
            }
            break;
    }

    return FALSE;
}

// Show options dialog
void ShowOptionsDialog() {
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_OPTIONS), g_hwnd, OptionsDlgProc);
}

// URL dialog procedure
static std::wstring g_urlResult;

INT_PTR CALLBACK URLDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            // Check clipboard for URL
            if (OpenClipboard(hwnd)) {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    wchar_t* clipText = static_cast<wchar_t*>(GlobalLock(hData));
                    if (clipText) {
                        // Check if clipboard contains a URL
                        if (_wcsnicmp(clipText, L"http://", 7) == 0 ||
                            _wcsnicmp(clipText, L"https://", 8) == 0) {
                            SetDlgItemTextW(hwnd, IDC_URL_EDIT, clipText);
                            // Select all text
                            SendDlgItemMessageW(hwnd, IDC_URL_EDIT, EM_SETSEL, 0, -1);
                        }
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
            }
            SetFocus(GetDlgItem(hwnd, IDC_URL_EDIT));
            return FALSE;  // Don't set default focus

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    wchar_t url[2048] = {0};
                    GetDlgItemTextW(hwnd, IDC_URL_EDIT, url, 2048);
                    g_urlResult = url;
                    EndDialog(hwnd, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    g_urlResult.clear();
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Check if file is a playlist
bool IsPlaylistFile(const std::wstring& path) {
    size_t dotPos = path.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return false;
    std::wstring ext = path.substr(dotPos);
    for (auto& c : ext) c = towlower(c);
    return (ext == L".m3u" || ext == L".m3u8" || ext == L".pls");
}

// Parse M3U playlist file
static std::vector<std::wstring> ParseM3U(const std::wstring& playlistPath) {
    std::vector<std::wstring> entries;

    // Get directory of playlist for relative paths
    std::wstring baseDir;
    size_t lastSlash = playlistPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        baseDir = playlistPath.substr(0, lastSlash + 1);
    }

    // Read file as binary first to detect BOM
    FILE* f = _wfopen(playlistPath.c_str(), L"rb");
    if (!f) return entries;

    // Check for UTF-8 BOM
    unsigned char bom[3] = {0};
    fread(bom, 1, 3, f);
    bool isUtf8 = (bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF);
    if (!isUtf8) {
        fseek(f, 0, SEEK_SET);  // No BOM, rewind
    }

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        // Trim whitespace
        char* start = line;
        while (*start && (*start == ' ' || *start == '\t')) start++;
        size_t slen = strlen(start);
        if (slen == 0) continue;
        char* end = start + slen - 1;
        while (end >= start && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t')) {
            *end-- = '\0';
        }

        // Skip empty lines and comments
        if (*start == '\0' || *start == '#') continue;

        // Convert to wide string (try UTF-8 first, then ACP)
        std::wstring entry;
        int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, start, -1, nullptr, 0);
        if (len > 0) {
            entry.resize(len);
            MultiByteToWideChar(CP_UTF8, 0, start, -1, &entry[0], len);
        } else {
            len = MultiByteToWideChar(CP_ACP, 0, start, -1, nullptr, 0);
            if (len <= 0) continue;
            entry.resize(len);
            MultiByteToWideChar(CP_ACP, 0, start, -1, &entry[0], len);
        }
        // Remove null terminator from string
        if (!entry.empty() && entry.back() == L'\0') {
            entry.pop_back();
        }

        if (entry.empty()) continue;

        // Build full path
        std::wstring fullPath;
        if (_wcsnicmp(entry.c_str(), L"http://", 7) == 0 ||
            _wcsnicmp(entry.c_str(), L"https://", 8) == 0 ||
            _wcsnicmp(entry.c_str(), L"ftp://", 6) == 0 ||
            (entry.length() > 2 && entry[1] == L':')) {
            fullPath = entry;
        } else {
            // Relative path - prepend base directory
            fullPath = baseDir + entry;
        }

        // Check if it's a folder and expand it
        DWORD attrs = GetFileAttributesW(fullPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            // Recursively add folder contents
            AddFilesFromFolder(fullPath, entries);
        } else {
            entries.push_back(fullPath);
        }
    }
    fclose(f);
    return entries;
}

// Parse PLS playlist file
static std::vector<std::wstring> ParsePLS(const std::wstring& playlistPath) {
    std::vector<std::wstring> entries;

    // Get directory of playlist for relative paths
    std::wstring baseDir;
    size_t lastSlash = playlistPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        baseDir = playlistPath.substr(0, lastSlash + 1);
    }

    // Read entries using GetPrivateProfileString
    for (int i = 1; i <= 1000; i++) {  // Reasonable max
        wchar_t key[32];
        swprintf(key, 32, L"File%d", i);
        wchar_t value[4096] = {0};
        GetPrivateProfileStringW(L"playlist", key, L"", value, 4096, playlistPath.c_str());
        if (value[0] == L'\0') break;

        std::wstring entry = value;
        std::wstring fullPath;
        // Check if it's a URL or absolute path
        if (_wcsnicmp(entry.c_str(), L"http://", 7) == 0 ||
            _wcsnicmp(entry.c_str(), L"https://", 8) == 0 ||
            _wcsnicmp(entry.c_str(), L"ftp://", 6) == 0 ||
            (entry.length() > 2 && entry[1] == L':')) {
            fullPath = entry;
        } else {
            // Relative path - prepend base directory
            fullPath = baseDir + entry;
        }

        // Check if it's a folder and expand it
        DWORD attrs = GetFileAttributesW(fullPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            // Recursively add folder contents
            AddFilesFromFolder(fullPath, entries);
        } else {
            entries.push_back(fullPath);
        }
    }
    return entries;
}

// Parse playlist file (M3U or PLS)
std::vector<std::wstring> ParsePlaylist(const std::wstring& playlistPath) {
    size_t dotPos = playlistPath.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return {};

    std::wstring ext = playlistPath.substr(dotPos);
    for (auto& c : ext) c = towlower(c);

    if (ext == L".pls") {
        return ParsePLS(playlistPath);
    } else {
        return ParseM3U(playlistPath);
    }
}

// Show open URL dialog
void ShowOpenURLDialog() {
    g_urlResult.clear();
    if (DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_URL), g_hwnd, URLDlgProc) == IDOK) {
        if (!g_urlResult.empty()) {
            // Add URL to playlist and play
            g_playlist.clear();
            g_playlist.push_back(g_urlResult);
            g_currentTrack = -1;
            PlayTrack(0);
        }
    }
}

// Jump to time dialog

static double g_jumpTimeResult = -1;

// Parse time string (mm:ss or hh:mm:ss) to seconds
static double ParseTimeString(const wchar_t* str) {
    int h = 0, m = 0, s = 0;

    // Try hh:mm:ss format
    if (swscanf(str, L"%d:%d:%d", &h, &m, &s) == 3) {
        return h * 3600.0 + m * 60.0 + s;
    }
    // Try mm:ss format
    if (swscanf(str, L"%d:%d", &m, &s) == 2) {
        return m * 60.0 + s;
    }
    // Try just seconds
    double secs = 0;
    if (swscanf(str, L"%lf", &secs) == 1) {
        return secs;
    }
    return -1;
}

// Format seconds to time string (mm:ss or hh:mm:ss)
static std::wstring FormatTimeForEdit(double seconds) {
    if (seconds < 0) seconds = 0;
    int h = static_cast<int>(seconds) / 3600;
    int m = (static_cast<int>(seconds) % 3600) / 60;
    int s = static_cast<int>(seconds) % 60;

    wchar_t buf[32];
    if (h > 0) {
        swprintf(buf, 32, L"%d:%02d:%02d", h, m, s);
    } else {
        swprintf(buf, 32, L"%d:%02d", m, s);
    }
    return buf;
}

static INT_PTR CALLBACK JumpToTimeDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            // Prefill with current position
            double pos = GetCurrentPosition();
            std::wstring timeStr = FormatTimeForEdit(pos);
            SetDlgItemTextW(hwnd, IDC_JUMPTIME_EDIT, timeStr.c_str());
            // Select all text
            SendDlgItemMessageW(hwnd, IDC_JUMPTIME_EDIT, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(hwnd, IDC_JUMPTIME_EDIT));
            return FALSE;  // We set focus manually
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    wchar_t buf[64];
                    GetDlgItemTextW(hwnd, IDC_JUMPTIME_EDIT, buf, 64);
                    g_jumpTimeResult = ParseTimeString(buf);
                    EndDialog(hwnd, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

void ShowJumpToTimeDialog() {
    g_jumpTimeResult = -1;
    if (DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_JUMPTOTIME), g_hwnd, JumpToTimeDlgProc) == IDOK) {
        if (g_jumpTimeResult >= 0) {
            SeekToPosition(g_jumpTimeResult);
        }
    }
}

// Bookmarks dialog

static std::vector<Bookmark> g_allBookmarks;       // All bookmarks from database
static std::vector<Bookmark> g_dialogBookmarks;    // Filtered bookmarks shown in list
static std::wstring g_currentFilePath;             // Current file path for filtering
static HWND g_bookmarksDlg = nullptr;              // Dialog handle for subclass access

// Jump to a bookmark (load file if needed and seek to position)
static void JumpToBookmark(const Bookmark& bm) {
    // Check if the file is in the current playlist
    int trackIndex = -1;
    for (size_t i = 0; i < g_playlist.size(); i++) {
        if (_wcsicmp(g_playlist[i].c_str(), bm.filePath.c_str()) == 0) {
            trackIndex = static_cast<int>(i);
            break;
        }
    }

    if (trackIndex >= 0) {
        // File is in playlist, switch to it if not current
        if (trackIndex != g_currentTrack) {
            PlayTrack(trackIndex);
        }
    } else {
        // Load the file
        g_playlist.clear();
        g_playlist.push_back(bm.filePath);
        g_currentTrack = -1;
        PlayTrack(0);
    }

    // Seek to position (give a moment for file to load if needed)
    SeekToPosition(bm.position);
}

// Refresh the bookmark listbox based on current filter
static void RefreshBookmarkList(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_BOOKMARK_LIST);
    HWND hFilter = GetDlgItem(hwnd, IDC_BOOKMARK_FILTER);

    // Get filter selection (0 = current file, 1 = all)
    int filterSel = static_cast<int>(SendMessageW(hFilter, CB_GETCURSEL, 0, 0));
    bool showAll = (filterSel == 1);

    // Clear and repopulate
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    g_dialogBookmarks.clear();

    for (const auto& bm : g_allBookmarks) {
        if (showAll || _wcsicmp(bm.filePath.c_str(), g_currentFilePath.c_str()) == 0) {
            g_dialogBookmarks.push_back(bm);
            SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(bm.displayName.c_str()));
        }
    }

    // Select first item if any
    if (!g_dialogBookmarks.empty()) {
        SendMessageW(hList, LB_SETCURSEL, 0, 0);
    }
}

// Subclass procedure for the bookmark listbox to handle Enter, Delete, and Escape keys
static WNDPROC g_origListProc = nullptr;

static LRESULT CALLBACK BookmarkListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            // Enter - jump to bookmark
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_dialogBookmarks.size())) {
                JumpToBookmark(g_dialogBookmarks[sel]);
                EndDialog(GetParent(hwnd), IDOK);
            }
            return 0;
        } else if (wParam == VK_DELETE) {
            // Delete - remove bookmark
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_dialogBookmarks.size())) {
                int bookmarkId = g_dialogBookmarks[sel].id;
                RemoveBookmark(bookmarkId);

                // Remove from both lists
                g_dialogBookmarks.erase(g_dialogBookmarks.begin() + sel);
                for (auto it = g_allBookmarks.begin(); it != g_allBookmarks.end(); ++it) {
                    if (it->id == bookmarkId) {
                        g_allBookmarks.erase(it);
                        break;
                    }
                }

                SendMessageW(hwnd, LB_DELETESTRING, sel, 0);

                // Select next item
                int count = static_cast<int>(SendMessageW(hwnd, LB_GETCOUNT, 0, 0));
                if (count > 0) {
                    if (sel >= count) sel = count - 1;
                    SendMessageW(hwnd, LB_SETCURSEL, sel, 0);
                }

                Speak("Bookmark removed");
            }
            return 0;
        } else if (wParam == VK_ESCAPE) {
            // Escape - close dialog
            EndDialog(GetParent(hwnd), IDCANCEL);
            return 0;
        }
    } else if (msg == WM_GETDLGCODE) {
        // Only capture Enter/Escape, let Tab pass through for dialog navigation
        MSG* pmsg = reinterpret_cast<MSG*>(lParam);
        if (pmsg && (pmsg->wParam == VK_RETURN || pmsg->wParam == VK_ESCAPE)) {
            return DLGC_WANTMESSAGE;
        }
        return CallWindowProcW(g_origListProc, hwnd, msg, wParam, lParam);
    }
    return CallWindowProcW(g_origListProc, hwnd, msg, wParam, lParam);
}

// Bookmarks dialog procedure
static INT_PTR CALLBACK BookmarksDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            g_bookmarksDlg = hwnd;

            // Get current file path
            if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
                g_currentFilePath = g_playlist[g_currentTrack];
            } else {
                g_currentFilePath.clear();
            }

            // Load all bookmarks
            g_allBookmarks = GetAllBookmarks();

            // Setup filter combobox
            HWND hFilter = GetDlgItem(hwnd, IDC_BOOKMARK_FILTER);
            SendMessageW(hFilter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Current file"));
            SendMessageW(hFilter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All bookmarks"));

            // Default to current file if playing, otherwise all bookmarks
            SendMessageW(hFilter, CB_SETCURSEL, g_currentFilePath.empty() ? 1 : 0, 0);

            // Subclass the listbox to handle Enter, Delete, and Escape
            HWND hList = GetDlgItem(hwnd, IDC_BOOKMARK_LIST);
            g_origListProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hList, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(BookmarkListSubclassProc)));

            // Populate listbox
            RefreshBookmarkList(hwnd);

            SetFocus(hList);
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_BOOKMARK_FILTER:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        RefreshBookmarkList(hwnd);
                        SetFocus(GetDlgItem(hwnd, IDC_BOOKMARK_LIST));
                        return TRUE;
                    }
                    break;

                case IDC_BOOKMARK_LIST:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        // Double-click to jump to bookmark
                        HWND hList = GetDlgItem(hwnd, IDC_BOOKMARK_LIST);
                        int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_dialogBookmarks.size())) {
                            JumpToBookmark(g_dialogBookmarks[sel]);
                            EndDialog(hwnd, IDOK);
                        }
                        return TRUE;
                    }
                    break;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_DESTROY: {
            // Restore original listbox proc
            HWND hList = GetDlgItem(hwnd, IDC_BOOKMARK_LIST);
            if (g_origListProc) {
                SetWindowLongPtrW(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origListProc));
                g_origListProc = nullptr;
            }
            g_bookmarksDlg = nullptr;
            break;
        }
    }
    return FALSE;
}

// Show bookmarks dialog
void ShowBookmarksDialog() {
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_BOOKMARKS), g_hwnd, BookmarksDlgProc);
}

// ============================================================================
// Radio Dialog
// ============================================================================

static std::vector<RadioStation> g_radioStations;

// Refresh radio list from database
static void RefreshRadioList(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_RADIO_LIST);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);

    g_radioStations = GetRadioFavorites();

    for (const auto& station : g_radioStations) {
        SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(station.name.c_str()));
    }
}

// Add station dialog proc
static INT_PTR CALLBACK RadioAddDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            // If a URL was passed, pre-fill the URL field
            const wchar_t* prefilledUrl = reinterpret_cast<const wchar_t*>(lParam);
            if (prefilledUrl && wcslen(prefilledUrl) > 0) {
                SetDlgItemTextW(hwnd, IDC_RADIO_URL, prefilledUrl);
            }
            SetFocus(GetDlgItem(hwnd, IDC_RADIO_NAME));
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    wchar_t name[256] = {0};
                    wchar_t url[512] = {0};
                    GetDlgItemTextW(hwnd, IDC_RADIO_NAME, name, 256);
                    GetDlgItemTextW(hwnd, IDC_RADIO_URL, url, 512);

                    // Validate
                    if (wcslen(name) == 0) {
                        MessageBoxW(hwnd, L"Please enter a station name.", L"Add Station", MB_ICONWARNING);
                        SetFocus(GetDlgItem(hwnd, IDC_RADIO_NAME));
                        return TRUE;
                    }
                    if (wcslen(url) == 0) {
                        MessageBoxW(hwnd, L"Please enter a stream URL.", L"Add Station", MB_ICONWARNING);
                        SetFocus(GetDlgItem(hwnd, IDC_RADIO_URL));
                        return TRUE;
                    }

                    // Add to database
                    int id = AddRadioStation(name, url);
                    if (id >= 0) {
                        EndDialog(hwnd, IDOK);
                    } else {
                        MessageBoxW(hwnd, L"Failed to add station.", L"Add Station", MB_ICONERROR);
                    }
                    return TRUE;
                }

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Radio search results
struct RadioSearchResult {
    std::wstring name;
    std::wstring url;        // Direct URL for RadioBrowser, playlist URL for TuneIn, empty for iHeart
    std::wstring stationId;  // Station ID for iHeartRadio
    std::wstring country;
    std::wstring codec;
    int bitrate;
    int source;              // 0=RadioBrowser, 1=TuneIn, 2=iHeartRadio
};
static std::vector<RadioSearchResult> g_radioSearchResults;
static int g_radioSearchSource = 0;  // Track which source was last used

// HTTP GET request (reused from youtube.cpp pattern)
static std::wstring RadioHttpGet(const std::wstring& url, const wchar_t* extraHeaders = nullptr) {
    std::wstring result;
    HINTERNET hInternet = InternetOpenW(L"FastPlay/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet) return result;

    // Use INTERNET_FLAG_SECURE for HTTPS
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if (_wcsnicmp(url.c_str(), L"https://", 8) == 0) {
        flags |= INTERNET_FLAG_SECURE;
    }

    // Build headers
    std::wstring headers = L"Accept: */*\r\n";
    if (extraHeaders) {
        headers += extraHeaders;
    }

    HINTERNET hConnect = InternetOpenUrlW(hInternet, url.c_str(), headers.c_str(),
                                          static_cast<DWORD>(headers.length()), flags, 0);
    if (hConnect) {
        char buffer[4096];
        DWORD bytesRead;
        std::string response;
        while (InternetReadFile(hConnect, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
            response.append(buffer, bytesRead);
        }
        InternetCloseHandle(hConnect);

        // Convert UTF-8 to wide string
        if (!response.empty()) {
            int len = MultiByteToWideChar(CP_UTF8, 0, response.c_str(), -1, nullptr, 0);
            if (len > 0) {
                result.resize(len);
                MultiByteToWideChar(CP_UTF8, 0, response.c_str(), -1, &result[0], len);
            }
        }
    }
    InternetCloseHandle(hInternet);
    return result;
}

// URL encode a string
static std::wstring RadioUrlEncode(const std::wstring& str) {
    std::wstring result;
    for (wchar_t c : str) {
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
            (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' || c == L'.' || c == L'~') {
            result += c;
        } else if (c == L' ') {
            result += L'+';
        } else {
            // Convert to UTF-8 and percent-encode
            char utf8[8];
            int len = WideCharToMultiByte(CP_UTF8, 0, &c, 1, utf8, sizeof(utf8), nullptr, nullptr);
            for (int i = 0; i < len; i++) {
                wchar_t hex[4];
                swprintf(hex, 4, L"%%%02X", (unsigned char)utf8[i]);
                result += hex;
            }
        }
    }
    return result;
}

// Helper to extract JSON string value
static std::wstring ExtractJsonString(const std::wstring& obj, const std::wstring& key) {
    std::wstring search = L"\"" + key + L"\":\"";
    size_t start = obj.find(search);
    if (start == std::wstring::npos) return L"";
    start += search.length();
    size_t end = start;
    while (end < obj.length()) {
        if (obj[end] == L'"' && (end == start || obj[end-1] != L'\\')) break;
        end++;
    }
    return obj.substr(start, end - start);
}

// Helper to extract JSON int value
static int ExtractJsonInt(const std::wstring& obj, const std::wstring& key) {
    std::wstring search = L"\"" + key + L"\":";
    size_t start = obj.find(search);
    if (start == std::wstring::npos) return 0;
    start += search.length();
    while (start < obj.length() && (obj[start] == L' ' || obj[start] == L'\t')) start++;
    return _wtoi(obj.c_str() + start);
}

// Helper to extract JSON value as string (handles both "key":"value" and "key":123)
static std::wstring ExtractJsonValue(const std::wstring& obj, const std::wstring& key) {
    std::wstring search = L"\"" + key + L"\":";
    size_t start = obj.find(search);
    if (start == std::wstring::npos) return L"";
    start += search.length();
    while (start < obj.length() && (obj[start] == L' ' || obj[start] == L'\t')) start++;
    if (start >= obj.length()) return L"";

    // Check if it's a quoted string
    if (obj[start] == L'"') {
        start++;
        size_t end = start;
        while (end < obj.length()) {
            if (obj[end] == L'"' && (end == start || obj[end-1] != L'\\')) break;
            end++;
        }
        return obj.substr(start, end - start);
    }

    // It's a number or other unquoted value
    size_t end = start;
    while (end < obj.length() && obj[end] != L',' && obj[end] != L'}' && obj[end] != L']') {
        end++;
    }
    return obj.substr(start, end - start);
}

// Search RadioBrowser API
static bool SearchRadioBrowser(const std::wstring& query, std::vector<RadioSearchResult>& results) {
    results.clear();

    // RadioBrowser API endpoint - use search endpoint with name parameter
    std::wstring url = L"https://de1.api.radio-browser.info/json/stations/search?name=" + RadioUrlEncode(query) + L"&limit=50&hidebroken=true";
    std::wstring json = RadioHttpGet(url);
    if (json.empty()) return false;

    // Simple JSON array parser - find each object by tracking brace depth
    size_t pos = 0;
    while ((pos = json.find(L'{', pos)) != std::wstring::npos) {
        // Find matching closing brace by tracking depth
        int depth = 1;
        size_t endPos = pos + 1;
        bool inString = false;
        while (endPos < json.length() && depth > 0) {
            wchar_t c = json[endPos];
            if (c == L'"' && (endPos == 0 || json[endPos-1] != L'\\')) {
                inString = !inString;
            } else if (!inString) {
                if (c == L'{') depth++;
                else if (c == L'}') depth--;
            }
            endPos++;
        }
        if (depth != 0) break;

        std::wstring obj = json.substr(pos, endPos - pos);
        RadioSearchResult result;
        result.source = 0;  // RadioBrowser

        result.name = ExtractJsonString(obj, L"name");
        result.url = ExtractJsonString(obj, L"url_resolved");
        if (result.url.empty()) result.url = ExtractJsonString(obj, L"url");
        result.country = ExtractJsonString(obj, L"country");
        result.codec = ExtractJsonString(obj, L"codec");
        result.bitrate = ExtractJsonInt(obj, L"bitrate");

        // Only add if we have a name and URL
        if (!result.name.empty() && !result.url.empty()) {
            results.push_back(result);
        }

        pos = endPos;
    }

    return !results.empty();
}

// Parse playlist content (M3U or PLS) to extract stream URL
static std::wstring ParsePlaylistContent(const std::string& content) {
    // Make lowercase copy for case-insensitive searching
    std::string lower = content;
    for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    // Check if it's a PLS file (case-insensitive)
    if (lower.find("[playlist]") != std::string::npos) {
        // Look for File1= (case-insensitive search, then extract from original)
        size_t filePos = lower.find("file1=");
        if (filePos != std::string::npos) {
            filePos += 6;
            size_t endPos = content.find_first_of("\r\n", filePos);
            if (endPos == std::string::npos) endPos = content.length();
            std::string url = content.substr(filePos, endPos - filePos);
            // Trim whitespace
            while (!url.empty() && (url.back() == ' ' || url.back() == '\t')) url.pop_back();
            while (!url.empty() && (url.front() == ' ' || url.front() == '\t')) url.erase(0, 1);
            if (!url.empty()) return Utf8ToWide(url);
        }
    }

    // Check if it's an M3U file
    // Look for first non-comment, non-empty line that starts with http
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        // Trim
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();

        if (line.empty() || line[0] == '#') continue;
        if (line.find("http") == 0) {
            return Utf8ToWide(line);
        }
    }

    // Maybe the content itself is a redirect URL
    if (content.find("http") == 0) {
        size_t endPos = content.find_first_of("\r\n \t");
        if (endPos == std::string::npos) endPos = content.length();
        return Utf8ToWide(content.substr(0, endPos));
    }

    return L"";
}

// Check if URL looks like a playlist file
static bool IsPlaylistUrl(const std::wstring& url) {
    std::wstring lower = url;
    for (auto& c : lower) c = towlower(c);

    // Check extension (before any query string)
    size_t queryPos = lower.find(L'?');
    std::wstring path = (queryPos != std::wstring::npos) ? lower.substr(0, queryPos) : lower;

    return path.length() > 4 && (
        path.substr(path.length() - 4) == L".m3u" ||
        path.substr(path.length() - 4) == L".pls" ||
        (path.length() > 5 && path.substr(path.length() - 5) == L".m3u8")
    );
}

// Resolve a TuneIn playlist URL to get the actual stream URL
static std::wstring ResolveTuneInUrl(const std::wstring& playlistUrl) {
    std::wstring currentUrl = playlistUrl;

    // Follow up to 3 levels of playlist redirects
    for (int i = 0; i < 3; i++) {
        std::wstring content = RadioHttpGet(currentUrl);
        if (content.empty()) return L"";

        // Convert to narrow string for parsing
        std::string narrow = WideToUtf8(content);

        // Parse the playlist content
        std::wstring streamUrl = ParsePlaylistContent(narrow);
        if (streamUrl.empty()) return L"";

        // If the result is another playlist, fetch and parse it
        if (IsPlaylistUrl(streamUrl)) {
            currentUrl = streamUrl;
            continue;
        }

        // Found a direct stream URL
        return streamUrl;
    }

    // If we've followed too many redirects, return the last URL we got
    return currentUrl;
}

// Search TuneIn API (returns OPML/XML)
static bool SearchTuneIn(const std::wstring& query, std::vector<RadioSearchResult>& results) {
    results.clear();

    // TuneIn search endpoint
    std::wstring url = L"http://opml.radiotime.com/Search.ashx?query=" + RadioUrlEncode(query);
    std::wstring xml = RadioHttpGet(url);
    if (xml.empty()) return false;

    // Parse OPML - look for <outline> elements with type="audio"
    size_t pos = 0;
    while ((pos = xml.find(L"<outline", pos)) != std::wstring::npos) {
        size_t endPos = xml.find(L"/>", pos);
        if (endPos == std::wstring::npos) {
            endPos = xml.find(L"</outline>", pos);
            if (endPos == std::wstring::npos) break;
        }

        std::wstring elem = xml.substr(pos, endPos - pos + 2);

        // Check if it's an audio type (station)
        if (elem.find(L"type=\"audio\"") != std::wstring::npos) {
            RadioSearchResult result;
            result.source = 1;  // TuneIn
            result.bitrate = 0;

            // Extract attributes
            auto extractAttr = [&elem](const std::wstring& attr) -> std::wstring {
                std::wstring search = attr + L"=\"";
                size_t start = elem.find(search);
                if (start == std::wstring::npos) return L"";
                start += search.length();
                size_t end = elem.find(L'"', start);
                if (end == std::wstring::npos) return L"";
                return elem.substr(start, end - start);
            };

            result.name = extractAttr(L"text");
            result.url = extractAttr(L"URL");  // Store TuneIn URL, resolve later
            std::wstring subtext = extractAttr(L"subtext");

            // subtext often contains location info
            if (!subtext.empty()) {
                result.country = subtext;
            }

            // Bitrate might be in bitrate attribute
            std::wstring bitrateStr = extractAttr(L"bitrate");
            if (!bitrateStr.empty()) {
                result.bitrate = _wtoi(bitrateStr.c_str());
            }

            // Decode HTML entities in name
            size_t ampPos;
            while ((ampPos = result.name.find(L"&amp;")) != std::wstring::npos) {
                result.name.replace(ampPos, 5, L"&");
            }
            while ((ampPos = result.name.find(L"&apos;")) != std::wstring::npos) {
                result.name.replace(ampPos, 6, L"'");
            }
            while ((ampPos = result.name.find(L"&quot;")) != std::wstring::npos) {
                result.name.replace(ampPos, 6, L"\"");
            }

            // Only add if we have a name and URL
            if (!result.name.empty() && !result.url.empty()) {
                results.push_back(result);
            }
        }

        pos = endPos + 1;
    }

    return !results.empty();
}

// Get stream URL for an iHeartRadio station by ID
static std::wstring GetIHeartStreamUrl(const std::wstring& stationId) {
    // Use the live station API to get stream URLs
    std::wstring url = L"https://api.iheart.com/api/v2/content/liveStations/" + stationId;
    std::wstring json = RadioHttpGet(url, L"Accept: application/json\r\n");
    if (json.empty()) return L"";

    // Look for streams section
    size_t streamsPos = json.find(L"\"streams\"");
    if (streamsPos == std::wstring::npos) return L"";

    std::wstring streamsSection = json.substr(streamsPos);

    // Try different stream types in order of preference
    std::wstring streamUrl = ExtractJsonString(streamsSection, L"shoutcast_stream");
    if (streamUrl.empty()) {
        streamUrl = ExtractJsonString(streamsSection, L"secure_shoutcast_stream");
    }
    if (streamUrl.empty()) {
        streamUrl = ExtractJsonString(streamsSection, L"pls_stream");
    }
    if (streamUrl.empty()) {
        streamUrl = ExtractJsonString(streamsSection, L"hls_stream");
    }

    return streamUrl;
}

// Search iHeartRadio API
static bool SearchIHeartRadio(const std::wstring& query, std::vector<RadioSearchResult>& results) {
    results.clear();

    // Try the v2 live stations search endpoint
    std::wstring url = L"https://api.iheart.com/api/v2/content/liveStations?countryCode=US&limit=20&q=" + RadioUrlEncode(query);
    std::wstring json = RadioHttpGet(url, L"Accept: application/json\r\n");

    // If v2 fails, try v3
    if (json.empty() || json.find(L"\"hits\"") == std::wstring::npos) {
        url = L"https://api.iheart.com/api/v3/search/all?keywords=" + RadioUrlEncode(query) +
              L"&startIndex=0&maxRows=20";
        json = RadioHttpGet(url, L"Accept: application/json\r\n");
    }

    if (json.empty()) return false;

    // Find the array containing stations - try multiple possible locations
    size_t arrayStart = std::wstring::npos;

    // Try "hits" array first (v2 response)
    size_t hitsPos = json.find(L"\"hits\"");
    if (hitsPos != std::wstring::npos) {
        arrayStart = json.find(L'[', hitsPos);
    }

    // Try "stations" -> "results" (v3 response)
    if (arrayStart == std::wstring::npos) {
        size_t stationsPos = json.find(L"\"stations\"");
        if (stationsPos != std::wstring::npos) {
            size_t resultsPos = json.find(L"\"results\"", stationsPos);
            if (resultsPos != std::wstring::npos) {
                arrayStart = json.find(L'[', resultsPos);
            }
        }
    }

    // Try just finding first array after "stations"
    if (arrayStart == std::wstring::npos) {
        size_t stationsPos = json.find(L"\"stations\"");
        if (stationsPos != std::wstring::npos) {
            arrayStart = json.find(L'[', stationsPos);
        }
    }

    if (arrayStart == std::wstring::npos) return false;

    // Find array end
    size_t arrayEnd = arrayStart + 1;
    int depth = 1;
    bool inString = false;
    while (arrayEnd < json.length() && depth > 0) {
        wchar_t c = json[arrayEnd];
        if (c == L'"' && (arrayEnd == 0 || json[arrayEnd-1] != L'\\')) {
            inString = !inString;
        } else if (!inString) {
            if (c == L'[') depth++;
            else if (c == L']') depth--;
        }
        arrayEnd++;
    }

    std::wstring stationsArray = json.substr(arrayStart, arrayEnd - arrayStart);

    // Parse each station object
    size_t pos = 0;
    while ((pos = stationsArray.find(L'{', pos)) != std::wstring::npos) {
        // Find matching closing brace
        int objDepth = 1;
        size_t endPos = pos + 1;
        bool objInString = false;
        while (endPos < stationsArray.length() && objDepth > 0) {
            wchar_t c = stationsArray[endPos];
            if (c == L'"' && (endPos == 0 || stationsArray[endPos-1] != L'\\')) {
                objInString = !objInString;
            } else if (!objInString) {
                if (c == L'{') objDepth++;
                else if (c == L'}') objDepth--;
            }
            endPos++;
        }
        if (objDepth != 0) break;

        std::wstring obj = stationsArray.substr(pos, endPos - pos);
        RadioSearchResult result;
        result.source = 2;  // iHeartRadio
        result.bitrate = 0;

        result.name = ExtractJsonString(obj, L"name");
        if (result.name.empty()) {
            result.name = ExtractJsonString(obj, L"description");
        }

        result.stationId = ExtractJsonValue(obj, L"id");
        result.country = ExtractJsonString(obj, L"city");
        std::wstring state = ExtractJsonString(obj, L"state");
        if (!state.empty()) {
            if (!result.country.empty()) result.country += L", ";
            result.country += state;
        }

        // Get call letters for display
        std::wstring callLetters = ExtractJsonString(obj, L"callLetters");
        if (!callLetters.empty() && result.name.find(callLetters) == std::wstring::npos) {
            result.name = callLetters + L" - " + result.name;
        }

        // Only add if we have a name and station ID (URL resolved later)
        if (!result.name.empty() && !result.stationId.empty()) {
            results.push_back(result);
        }

        pos = endPos;
    }

    return !results.empty();
}

// Resolve stream URL for a radio search result (called when playing/adding)
static std::wstring ResolveRadioStreamUrl(const RadioSearchResult& result) {
    std::wstring url;

    if (result.source == 0) {
        // RadioBrowser - URL is already resolved
        url = result.url;
    } else if (result.source == 1) {
        // TuneIn - resolve playlist URL to get actual stream
        url = ResolveTuneInUrl(result.url);
    } else if (result.source == 2) {
        // iHeartRadio - get stream URL from station ID
        url = GetIHeartStreamUrl(result.stationId);
    } else {
        url = result.url;
    }

    // Safety check: if the resolved URL is still a playlist, try to resolve it
    if (!url.empty() && IsPlaylistUrl(url)) {
        std::wstring resolved = ResolveTuneInUrl(url);
        if (!resolved.empty()) {
            url = resolved;
        }
    }

    return url;
}

// Show/hide radio dialog controls based on tab
static void UpdateRadioTabVisibility(HWND hwnd, int tab) {
    // Favorites tab controls (tab 0)
    int favCtrls[] = {IDC_RADIO_LIST, IDC_RADIO_ADD, IDC_RADIO_IMPORT};
    // Search tab controls (tab 1)
    int searchCtrls[] = {IDC_RADIO_SEARCH_SOURCE, IDC_RADIO_SEARCH_EDIT, IDC_RADIO_SEARCH_BTN,
                         IDC_RADIO_SEARCH_LIST, IDC_RADIO_SEARCH_ADD};

    for (int id : favCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 0 ? SW_SHOW : SW_HIDE);
    }
    for (int id : searchCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 1 ? SW_SHOW : SW_HIDE);
    }

    // Also hide/show static text labels (they have -1 IDs, so we position them)
}

// Radio list subclass proc for keyboard handling
static WNDPROC g_origRadioListProc = nullptr;
static WNDPROC g_origRadioSearchListProc = nullptr;
static WNDPROC g_origRadioSearchEditProc = nullptr;

// Search edit subclass proc - handle Enter to trigger search
static LRESULT CALLBACK RadioSearchEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        // Trigger search button click
        SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(IDC_RADIO_SEARCH_BTN, BN_CLICKED), 0);
        return 0;
    } else if (msg == WM_GETDLGCODE) {
        MSG* pmsg = reinterpret_cast<MSG*>(lParam);
        if (pmsg && pmsg->wParam == VK_RETURN) {
            return DLGC_WANTMESSAGE;
        }
        return CallWindowProcW(g_origRadioSearchEditProc, hwnd, msg, wParam, lParam);
    }
    return CallWindowProcW(g_origRadioSearchEditProc, hwnd, msg, wParam, lParam);
}

// Search list subclass proc
static LRESULT CALLBACK RadioSearchListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            // Play selected station
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_radioSearchResults.size())) {
                // Resolve the stream URL
                SetCursor(LoadCursor(nullptr, IDC_WAIT));
                std::wstring streamUrl = ResolveRadioStreamUrl(g_radioSearchResults[sel]);
                SetCursor(LoadCursor(nullptr, IDC_ARROW));
                if (!streamUrl.empty()) {
                    g_playlist.clear();
                    g_playlist.push_back(streamUrl);
                    PlayTrack(0);
                } else {
                    Speak("Could not get stream URL");
                }
            }
            return 0;
        } else if (wParam == VK_ESCAPE) {
            SendMessageW(GetParent(hwnd), WM_COMMAND, IDCANCEL, 0);
            return 0;
        }
    } else if (msg == WM_GETDLGCODE) {
        MSG* pmsg = reinterpret_cast<MSG*>(lParam);
        if (pmsg && (pmsg->wParam == VK_RETURN || pmsg->wParam == VK_ESCAPE)) {
            return DLGC_WANTMESSAGE;
        }
        return CallWindowProcW(g_origRadioSearchListProc, hwnd, msg, wParam, lParam);
    }
    return CallWindowProcW(g_origRadioSearchListProc, hwnd, msg, wParam, lParam);
}

// Edit station dialog data
static std::wstring g_editStationName;
static std::wstring g_editStationUrl;
static INT_PTR CALLBACK EditStationDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            SetWindowTextW(hwnd, L"Edit Station");
            SetDlgItemTextW(hwnd, IDC_RADIO_NAME, g_editStationName.c_str());
            SetDlgItemTextW(hwnd, IDC_RADIO_URL, g_editStationUrl.c_str());
            // Select all text in name field
            SendDlgItemMessageW(hwnd, IDC_RADIO_NAME, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(hwnd, IDC_RADIO_NAME));
            return FALSE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                wchar_t buf[4096];
                GetDlgItemTextW(hwnd, IDC_RADIO_NAME, buf, 512);
                g_editStationName = buf;
                GetDlgItemTextW(hwnd, IDC_RADIO_URL, buf, 4096);
                g_editStationUrl = buf;
                EndDialog(hwnd, IDOK);
                return TRUE;
            } else if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

static LRESULT CALLBACK RadioListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            // Play selected station
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_radioStations.size())) {
                g_playlist.clear();
                g_playlist.push_back(g_radioStations[sel].url);
                PlayTrack(0);
            }
            return 0;
        } else if (wParam == VK_F2) {
            // Edit selected station
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_radioStations.size())) {
                g_editStationName = g_radioStations[sel].name;
                g_editStationUrl = g_radioStations[sel].url;
                if (DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_RADIO_ADD),
                               GetParent(hwnd), EditStationDlgProc) == IDOK) {
                    // Trim whitespace from name
                    while (!g_editStationName.empty() &&
                           (g_editStationName.front() == L' ' || g_editStationName.front() == L'\t')) {
                        g_editStationName.erase(0, 1);
                    }
                    while (!g_editStationName.empty() &&
                           (g_editStationName.back() == L' ' || g_editStationName.back() == L'\t')) {
                        g_editStationName.pop_back();
                    }
                    // Trim whitespace from URL
                    while (!g_editStationUrl.empty() &&
                           (g_editStationUrl.front() == L' ' || g_editStationUrl.front() == L'\t')) {
                        g_editStationUrl.erase(0, 1);
                    }
                    while (!g_editStationUrl.empty() &&
                           (g_editStationUrl.back() == L' ' || g_editStationUrl.back() == L'\t')) {
                        g_editStationUrl.pop_back();
                    }
                    if (!g_editStationName.empty() && !g_editStationUrl.empty()) {
                        if (UpdateRadioStation(g_radioStations[sel].id, g_editStationName, g_editStationUrl)) {
                            Speak("Station updated");
                            RefreshRadioList(GetParent(hwnd));
                            SendMessageW(hwnd, LB_SETCURSEL, sel, 0);
                        }
                    }
                }
            }
            return 0;
        } else if (wParam == VK_DELETE) {
            // Remove selected station
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_radioStations.size())) {
                if (RemoveRadioStation(g_radioStations[sel].id)) {
                    Speak("Station removed");
                    RefreshRadioList(GetParent(hwnd));
                    // Select next item or previous if at end
                    int count = static_cast<int>(SendMessageW(hwnd, LB_GETCOUNT, 0, 0));
                    if (count > 0) {
                        if (sel >= count) sel = count - 1;
                        SendMessageW(hwnd, LB_SETCURSEL, sel, 0);
                    }
                }
            }
            return 0;
        } else if (wParam == VK_ESCAPE) {
            // Close dialog
            SendMessageW(GetParent(hwnd), WM_COMMAND, IDCANCEL, 0);
            return 0;
        }
    } else if (msg == WM_GETDLGCODE) {
        // Capture Enter/Escape/Delete/F2, let Tab pass through
        MSG* pmsg = reinterpret_cast<MSG*>(lParam);
        if (pmsg && (pmsg->wParam == VK_RETURN || pmsg->wParam == VK_ESCAPE ||
                     pmsg->wParam == VK_DELETE || pmsg->wParam == VK_F2)) {
            return DLGC_WANTMESSAGE;
        }
        return CallWindowProcW(g_origRadioListProc, hwnd, msg, wParam, lParam);
    }
    return CallWindowProcW(g_origRadioListProc, hwnd, msg, wParam, lParam);
}

// Radio dialog proc
static INT_PTR CALLBACK RadioDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            // Initialize tab control
            HWND hTab = GetDlgItem(hwnd, IDC_RADIO_TAB);
            TCITEMW tie = {0};
            tie.mask = TCIF_TEXT;
            tie.pszText = const_cast<LPWSTR>(L"Favorites");
            SendMessageW(hTab, TCM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&tie));
            tie.pszText = const_cast<LPWSTR>(L"Search");
            SendMessageW(hTab, TCM_INSERTITEMW, 1, reinterpret_cast<LPARAM>(&tie));

            // Subclass the favorites listbox for keyboard handling
            HWND hList = GetDlgItem(hwnd, IDC_RADIO_LIST);
            g_origRadioListProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RadioListSubclassProc)));

            // Subclass the search listbox for keyboard handling
            HWND hSearchList = GetDlgItem(hwnd, IDC_RADIO_SEARCH_LIST);
            g_origRadioSearchListProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hSearchList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RadioSearchListSubclassProc)));

            // Subclass the search edit for Enter key handling
            HWND hSearchEdit = GetDlgItem(hwnd, IDC_RADIO_SEARCH_EDIT);
            g_origRadioSearchEditProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hSearchEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RadioSearchEditSubclassProc)));

            // Initialize search source combo
            HWND hSource = GetDlgItem(hwnd, IDC_RADIO_SEARCH_SOURCE);
            SendMessageW(hSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"RadioBrowser"));
            SendMessageW(hSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"TuneIn"));
            SendMessageW(hSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"iHeartRadio"));
            SendMessageW(hSource, CB_SETCURSEL, 0, 0);

            // Load favorites
            RefreshRadioList(hwnd);

            // Show favorites tab, hide search tab
            UpdateRadioTabVisibility(hwnd, 0);

            // Focus on list
            SetFocus(hList);
            if (SendMessageW(hList, LB_GETCOUNT, 0, 0) > 0) {
                SendMessageW(hList, LB_SETCURSEL, 0, 0);
            }
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_RADIO_ADD: {
                    // Check if currently playing a URL stream
                    const wchar_t* currentUrl = nullptr;
                    if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
                        const std::wstring& path = g_playlist[g_currentTrack];
                        if (IsURL(path.c_str())) {
                            currentUrl = path.c_str();
                        }
                    }
                    if (DialogBoxParamW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_RADIO_ADD),
                                        hwnd, RadioAddDlgProc, reinterpret_cast<LPARAM>(currentUrl)) == IDOK) {
                        RefreshRadioList(hwnd);
                        Speak("Station added");
                    }
                    return TRUE;
                }

                case IDC_RADIO_IMPORT: {
                    // Open file dialog for playlist files
                    wchar_t szFile[32768] = {0};
                    OPENFILENAMEW ofn = {0};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
                    ofn.lpstrFilter = L"Playlist Files\0*.m3u;*.m3u8;*.pls\0"
                                      L"M3U Playlists\0*.m3u;*.m3u8\0"
                                      L"PLS Playlists\0*.pls\0"
                                      L"All Files (*.*)\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

                    if (GetOpenFileNameW(&ofn)) {
                        std::wstring playlistPath = szFile;
                        int imported = 0;

                        // Determine file type and parse
                        std::wstring ext = playlistPath.substr(playlistPath.find_last_of(L'.'));
                        for (auto& c : ext) c = towlower(c);

                        if (ext == L".pls") {
                            // Parse PLS - has Title entries
                            for (int i = 1; i <= 1000; i++) {
                                wchar_t fileKey[32], titleKey[32];
                                swprintf(fileKey, 32, L"File%d", i);
                                swprintf(titleKey, 32, L"Title%d", i);

                                wchar_t url[4096] = {0}, title[512] = {0};
                                GetPrivateProfileStringW(L"playlist", fileKey, L"", url, 4096, playlistPath.c_str());
                                if (url[0] == L'\0') break;

                                // Only import URLs (not local files)
                                if (_wcsnicmp(url, L"http://", 7) != 0 && _wcsnicmp(url, L"https://", 8) != 0) {
                                    continue;
                                }

                                GetPrivateProfileStringW(L"playlist", titleKey, L"", title, 512, playlistPath.c_str());
                                std::wstring name = title[0] ? title : url;

                                if (AddRadioStation(name, url) >= 0) {
                                    imported++;
                                }
                            }
                        } else {
                            // Parse M3U/M3U8
                            FILE* f = _wfopen(playlistPath.c_str(), L"rb");
                            if (f) {
                                // Check for UTF-8 BOM
                                unsigned char bom[3] = {0};
                                fread(bom, 1, 3, f);
                                if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)) {
                                    fseek(f, 0, SEEK_SET);
                                }

                                char line[4096];
                                std::wstring pendingName;

                                while (fgets(line, sizeof(line), f)) {
                                    // Trim whitespace
                                    char* start = line;
                                    while (*start && (*start == ' ' || *start == '\t')) start++;
                                    size_t slen = strlen(start);
                                    if (slen == 0) continue;
                                    char* end = start + slen - 1;
                                    while (end >= start && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t')) {
                                        *end-- = '\0';
                                    }
                                    if (*start == '\0') continue;

                                    // Convert to wide string
                                    std::wstring wline;
                                    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, start, -1, nullptr, 0);
                                    if (len > 0) {
                                        wline.resize(len);
                                        MultiByteToWideChar(CP_UTF8, 0, start, -1, &wline[0], len);
                                    } else {
                                        len = MultiByteToWideChar(CP_ACP, 0, start, -1, nullptr, 0);
                                        if (len <= 0) continue;
                                        wline.resize(len);
                                        MultiByteToWideChar(CP_ACP, 0, start, -1, &wline[0], len);
                                    }
                                    if (!wline.empty() && wline.back() == L'\0') wline.pop_back();
                                    if (wline.empty()) continue;

                                    // Check for #EXTINF line (contains station name)
                                    if (_wcsnicmp(wline.c_str(), L"#EXTINF:", 8) == 0) {
                                        // Format: #EXTINF:duration,Station Name
                                        const wchar_t* comma = wcschr(wline.c_str() + 8, L',');
                                        if (comma) {
                                            // Skip comma and any leading whitespace
                                            const wchar_t* nameStart = comma + 1;
                                            while (*nameStart == L' ' || *nameStart == L'\t') nameStart++;
                                            pendingName = nameStart;
                                        }
                                        continue;
                                    }

                                    // Skip other comments
                                    if (wline[0] == L'#') continue;

                                    // This should be a URL
                                    if (_wcsnicmp(wline.c_str(), L"http://", 7) == 0 ||
                                        _wcsnicmp(wline.c_str(), L"https://", 8) == 0) {
                                        std::wstring name = pendingName.empty() ? wline : pendingName;
                                        if (AddRadioStation(name, wline) >= 0) {
                                            imported++;
                                        }
                                    }
                                    pendingName.clear();
                                }
                                fclose(f);
                            }
                        }

                        if (imported > 0) {
                            RefreshRadioList(hwnd);
                            wchar_t msg[64];
                            swprintf(msg, 64, L"Imported %d stations", imported);
                            Speak(WideToUtf8(msg).c_str());
                        } else {
                            Speak("No stations found to import");
                        }
                    }
                    return TRUE;
                }

                case IDC_RADIO_LIST:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        // Double-click to play
                        HWND hList = GetDlgItem(hwnd, IDC_RADIO_LIST);
                        int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_radioStations.size())) {
                            g_playlist.clear();
                            g_playlist.push_back(g_radioStations[sel].url);
                            PlayTrack(0);
                        }
                    }
                    return TRUE;

                case IDC_RADIO_SEARCH_BTN: {
                    // Get search query
                    wchar_t query[256] = {0};
                    GetDlgItemTextW(hwnd, IDC_RADIO_SEARCH_EDIT, query, 256);
                    if (wcslen(query) == 0) {
                        Speak("Enter a search term");
                        return TRUE;
                    }

                    // Clear results
                    HWND hSearchList = GetDlgItem(hwnd, IDC_RADIO_SEARCH_LIST);
                    SendMessageW(hSearchList, LB_RESETCONTENT, 0, 0);
                    g_radioSearchResults.clear();

                    // Show searching message
                    Speak("Searching");
                    SetCursor(LoadCursor(nullptr, IDC_WAIT));

                    // Search based on selected source
                    int source = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_RADIO_SEARCH_SOURCE, CB_GETCURSEL, 0, 0));
                    bool found = false;

                    if (source == 0) {  // RadioBrowser
                        found = SearchRadioBrowser(query, g_radioSearchResults);
                    } else if (source == 1) {  // TuneIn
                        found = SearchTuneIn(query, g_radioSearchResults);
                    } else if (source == 2) {  // iHeartRadio
                        found = SearchIHeartRadio(query, g_radioSearchResults);
                    }

                    SetCursor(LoadCursor(nullptr, IDC_ARROW));

                    if (found) {
                        // Populate list
                        for (const auto& r : g_radioSearchResults) {
                            std::wstring display = r.name;
                            if (!r.country.empty()) {
                                display += L" (" + r.country + L")";
                            }
                            if (r.bitrate > 0) {
                                wchar_t br[32];
                                swprintf(br, 32, L" [%dk]", r.bitrate);
                                display += br;
                            }
                            SendMessageW(hSearchList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
                        }

                        wchar_t msg[64];
                        swprintf(msg, 64, L"Found %d stations", static_cast<int>(g_radioSearchResults.size()));
                        Speak(WideToUtf8(msg).c_str());

                        // Select first item
                        if (SendMessageW(hSearchList, LB_GETCOUNT, 0, 0) > 0) {
                            SendMessageW(hSearchList, LB_SETCURSEL, 0, 0);
                            SetFocus(hSearchList);
                        }
                    } else {
                        Speak("No stations found");
                    }
                    return TRUE;
                }

                case IDC_RADIO_SEARCH_ADD: {
                    // Add selected search result to favorites
                    HWND hSearchList = GetDlgItem(hwnd, IDC_RADIO_SEARCH_LIST);
                    int sel = static_cast<int>(SendMessageW(hSearchList, LB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < static_cast<int>(g_radioSearchResults.size())) {
                        const auto& r = g_radioSearchResults[sel];
                        // Resolve the stream URL
                        SetCursor(LoadCursor(nullptr, IDC_WAIT));
                        std::wstring streamUrl = ResolveRadioStreamUrl(r);
                        SetCursor(LoadCursor(nullptr, IDC_ARROW));
                        if (!streamUrl.empty()) {
                            if (AddRadioStation(r.name, streamUrl) >= 0) {
                                Speak("Added to favorites");
                            } else {
                                Speak("Failed to add station");
                            }
                        } else {
                            Speak("Could not get stream URL");
                        }
                    } else {
                        Speak("Select a station first");
                    }
                    return TRUE;
                }

                case IDC_RADIO_SEARCH_LIST:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        // Double-click to play
                        HWND hSearchList = GetDlgItem(hwnd, IDC_RADIO_SEARCH_LIST);
                        int sel = static_cast<int>(SendMessageW(hSearchList, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_radioSearchResults.size())) {
                            // Resolve the stream URL
                            SetCursor(LoadCursor(nullptr, IDC_WAIT));
                            std::wstring streamUrl = ResolveRadioStreamUrl(g_radioSearchResults[sel]);
                            SetCursor(LoadCursor(nullptr, IDC_ARROW));
                            if (!streamUrl.empty()) {
                                g_playlist.clear();
                                g_playlist.push_back(streamUrl);
                                PlayTrack(0);
                            } else {
                                Speak("Could not get stream URL");
                            }
                        }
                    }
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_NOTIFY: {
            NMHDR* nmhdr = reinterpret_cast<NMHDR*>(lParam);
            if (nmhdr->idFrom == IDC_RADIO_TAB && nmhdr->code == TCN_SELCHANGE) {
                HWND hTab = GetDlgItem(hwnd, IDC_RADIO_TAB);
                int tab = static_cast<int>(SendMessageW(hTab, TCM_GETCURSEL, 0, 0));
                UpdateRadioTabVisibility(hwnd, tab);
                // Don't change focus - let user navigate naturally
            }
            break;
        }

        case WM_SIZE: {
            // Resize list and reposition buttons
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            // Tab control
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_TAB), nullptr, 7, 7, w - 14, h - 42, SWP_NOZORDER);

            // Favorites tab controls
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_LIST), nullptr, 14, 28, w - 28, h - 92, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_ADD), nullptr, w - 174, h - 54, 50, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_IMPORT), nullptr, w - 120, h - 54, 50, 14, SWP_NOZORDER);

            // Search tab controls
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_SEARCH_EDIT), nullptr, 142, 28, w - 210, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_SEARCH_BTN), nullptr, w - 64, 27, 50, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_SEARCH_LIST), nullptr, 14, 48, w - 28, h - 112, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_SEARCH_ADD), nullptr, w - 84, h - 54, 70, 14, SWP_NOZORDER);

            // Close button (common)
            SetWindowPos(GetDlgItem(hwnd, IDCANCEL), nullptr, w - 64, h - 22, 50, 14, SWP_NOZORDER);
            return TRUE;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 300;
            mmi->ptMinTrackSize.y = 200;
            return TRUE;
        }
    }
    return FALSE;
}

// Show radio dialog
void ShowRadioDialog() {
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_RADIO), g_hwnd, RadioDlgProc);
}

// ========== Podcast Dialog ==========

// Podcast search result (from iTunes API)
struct PodcastSearchResult {
    std::wstring name;
    std::wstring feedUrl;
    std::wstring imageUrl;
    std::wstring artistName;
};

// Podcast dialog state
static std::vector<PodcastSubscription> g_podcastSubs;
static std::vector<PodcastEpisode> g_podcastEpisodes;
static std::vector<PodcastSearchResult> g_podcastSearchResults;
static int g_currentPodcastId = -1;

// HTTP GET for podcast operations
static std::wstring PodcastHttpGet(const std::wstring& url) {
    std::wstring result;

    // Parse URL to get host and path
    std::wstring host, path;
    bool secure = false;

    if (url.find(L"https://") == 0) {
        secure = true;
        size_t hostStart = 8;
        size_t pathStart = url.find(L'/', hostStart);
        if (pathStart == std::wstring::npos) {
            host = url.substr(hostStart);
            path = L"/";
        } else {
            host = url.substr(hostStart, pathStart - hostStart);
            path = url.substr(pathStart);
        }
    } else if (url.find(L"http://") == 0) {
        size_t hostStart = 7;
        size_t pathStart = url.find(L'/', hostStart);
        if (pathStart == std::wstring::npos) {
            host = url.substr(hostStart);
            path = L"/";
        } else {
            host = url.substr(hostStart, pathStart - hostStart);
            path = url.substr(pathStart);
        }
    } else {
        return result;
    }

    HINTERNET hInternet = InternetOpenW(L"FastPlay/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet) return result;

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if (secure) flags |= INTERNET_FLAG_SECURE;

    HINTERNET hConnect = InternetConnectW(hInternet, host.c_str(),
                                          secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT,
                                          nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (hConnect) {
        HINTERNET hRequest = HttpOpenRequestW(hConnect, L"GET", path.c_str(), nullptr, nullptr, nullptr, flags, 0);
        if (hRequest) {
            if (HttpSendRequestW(hRequest, nullptr, 0, nullptr, 0)) {
                char buffer[4096];
                DWORD bytesRead;
                std::string response;
                while (InternetReadFile(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
                    response.append(buffer, bytesRead);
                }
                result = Utf8ToWide(response);
            }
            InternetCloseHandle(hRequest);
        }
        InternetCloseHandle(hConnect);
    }
    InternetCloseHandle(hInternet);

    return result;
}

// URL encode for podcast searches
static std::wstring PodcastUrlEncode(const std::wstring& str) {
    std::string utf8 = WideToUtf8(str);
    std::wostringstream encoded;
    for (unsigned char c : utf8) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << static_cast<wchar_t>(c);
        } else if (c == ' ') {
            encoded << L'+';
        } else {
            encoded << L'%' << std::hex << std::uppercase << std::setw(2) << std::setfill(L'0') << static_cast<int>(c);
        }
    }
    return encoded.str();
}

// Extract text content from an XML element
static std::wstring ExtractXmlContent(const std::wstring& xml, const std::wstring& tagName) {
    std::wstring startTag = L"<" + tagName;
    std::wstring endTag = L"</" + tagName + L">";

    size_t start = xml.find(startTag);
    if (start == std::wstring::npos) return L"";

    // Find the end of the opening tag
    size_t tagEnd = xml.find(L'>', start);
    if (tagEnd == std::wstring::npos) return L"";

    // Check for CDATA
    size_t contentStart = tagEnd + 1;
    size_t end = xml.find(endTag, contentStart);
    if (end == std::wstring::npos) return L"";

    std::wstring content = xml.substr(contentStart, end - contentStart);

    // Strip CDATA wrapper if present (may have leading whitespace)
    size_t cdataStart = content.find(L"<![CDATA[");
    if (cdataStart != std::wstring::npos) {
        size_t cdataEnd = content.rfind(L"]]>");
        if (cdataEnd != std::wstring::npos && cdataEnd > cdataStart) {
            content = content.substr(cdataStart + 9, cdataEnd - cdataStart - 9);
        }
    }

    // Basic HTML entity decode
    size_t pos = 0;
    while ((pos = content.find(L"&amp;", pos)) != std::wstring::npos) {
        content.replace(pos, 5, L"&");
    }
    pos = 0;
    while ((pos = content.find(L"&lt;", pos)) != std::wstring::npos) {
        content.replace(pos, 4, L"<");
    }
    pos = 0;
    while ((pos = content.find(L"&gt;", pos)) != std::wstring::npos) {
        content.replace(pos, 4, L">");
    }
    pos = 0;
    while ((pos = content.find(L"&quot;", pos)) != std::wstring::npos) {
        content.replace(pos, 6, L"\"");
    }
    pos = 0;
    while ((pos = content.find(L"&apos;", pos)) != std::wstring::npos) {
        content.replace(pos, 6, L"'");
    }

    return content;
}

// Extract enclosure URL from an item
static std::wstring ExtractEnclosureUrl(const std::wstring& item) {
    size_t encPos = item.find(L"<enclosure");
    if (encPos == std::wstring::npos) return L"";

    size_t urlStart = item.find(L"url=\"", encPos);
    if (urlStart == std::wstring::npos) {
        urlStart = item.find(L"url='", encPos);
        if (urlStart == std::wstring::npos) return L"";
        urlStart += 5;
        size_t urlEnd = item.find(L"'", urlStart);
        if (urlEnd == std::wstring::npos) return L"";
        return item.substr(urlStart, urlEnd - urlStart);
    }
    urlStart += 5;
    size_t urlEnd = item.find(L"\"", urlStart);
    if (urlEnd == std::wstring::npos) return L"";
    return item.substr(urlStart, urlEnd - urlStart);
}

// Parse iTunes duration (HH:MM:SS or MM:SS or seconds)
static int ParseDuration(const std::wstring& duration) {
    if (duration.empty()) return 0;

    // Check if it's just seconds
    bool hasColon = duration.find(L':') != std::wstring::npos;
    if (!hasColon) {
        return _wtoi(duration.c_str());
    }

    // Parse HH:MM:SS or MM:SS
    int h = 0, m = 0, s = 0;
    if (swscanf(duration.c_str(), L"%d:%d:%d", &h, &m, &s) == 3) {
        return h * 3600 + m * 60 + s;
    } else if (swscanf(duration.c_str(), L"%d:%d", &m, &s) == 2) {
        return m * 60 + s;
    }
    return 0;
}

// Parse RSS feed and extract episodes
static bool ParsePodcastFeed(const std::wstring& feedUrl, std::wstring& outTitle,
                             std::vector<PodcastEpisode>& episodes) {
    episodes.clear();

    std::wstring xml = PodcastHttpGet(feedUrl);
    if (xml.empty()) return false;

    // Extract channel title
    size_t channelStart = xml.find(L"<channel");
    if (channelStart != std::wstring::npos) {
        size_t channelEnd = xml.find(L"</channel>", channelStart);
        if (channelEnd != std::wstring::npos) {
            std::wstring channel = xml.substr(channelStart, channelEnd - channelStart);
            // Get title before first <item>
            size_t firstItem = channel.find(L"<item");
            if (firstItem != std::wstring::npos) {
                std::wstring header = channel.substr(0, firstItem);
                outTitle = ExtractXmlContent(header, L"title");
            }
        }
    }

    // Find all <item> elements
    size_t pos = 0;
    while ((pos = xml.find(L"<item", pos)) != std::wstring::npos) {
        size_t itemEnd = xml.find(L"</item>", pos);
        if (itemEnd == std::wstring::npos) break;

        std::wstring item = xml.substr(pos, itemEnd - pos + 7);
        PodcastEpisode ep;

        ep.title = ExtractXmlContent(item, L"title");
        ep.description = ExtractXmlContent(item, L"description");
        ep.pubDate = ExtractXmlContent(item, L"pubDate");
        ep.guid = ExtractXmlContent(item, L"guid");
        ep.audioUrl = ExtractEnclosureUrl(item);

        // Try to get duration from itunes:duration
        std::wstring durationStr = ExtractXmlContent(item, L"itunes:duration");
        ep.durationSeconds = ParseDuration(durationStr);

        if (!ep.audioUrl.empty() && !ep.title.empty()) {
            episodes.push_back(ep);
        }

        pos = itemEnd;
    }

    return !episodes.empty();
}

// Search iTunes podcast directory
static bool SearchItunesPodcasts(const std::wstring& query, std::vector<PodcastSearchResult>& results) {
    results.clear();

    std::wstring url = L"https://itunes.apple.com/search?term=" +
                       PodcastUrlEncode(query) + L"&media=podcast&limit=25";

    std::wstring json = PodcastHttpGet(url);
    if (json.empty()) return false;

    // Parse JSON results - look for each result object
    size_t pos = 0;
    while ((pos = json.find(L"\"collectionName\"", pos)) != std::wstring::npos) {
        PodcastSearchResult r;

        // Extract collectionName
        size_t valueStart = json.find(L":", pos);
        if (valueStart != std::wstring::npos) {
            size_t strStart = json.find(L"\"", valueStart + 1);
            if (strStart != std::wstring::npos) {
                strStart++;
                size_t strEnd = json.find(L"\"", strStart);
                if (strEnd != std::wstring::npos) {
                    r.name = json.substr(strStart, strEnd - strStart);
                }
            }
        }

        // Find feedUrl in the same object (search backwards and forwards)
        size_t searchStart = (pos > 500) ? pos - 500 : 0;
        size_t searchEnd = pos + 1000;
        if (searchEnd > json.length()) searchEnd = json.length();
        std::wstring context = json.substr(searchStart, searchEnd - searchStart);

        size_t feedPos = context.find(L"\"feedUrl\"");
        if (feedPos != std::wstring::npos) {
            size_t fvalueStart = context.find(L":", feedPos);
            if (fvalueStart != std::wstring::npos) {
                size_t fstrStart = context.find(L"\"", fvalueStart + 1);
                if (fstrStart != std::wstring::npos) {
                    fstrStart++;
                    size_t fstrEnd = context.find(L"\"", fstrStart);
                    if (fstrEnd != std::wstring::npos) {
                        r.feedUrl = context.substr(fstrStart, fstrEnd - fstrStart);
                    }
                }
            }
        }

        // Extract artistName
        size_t artistPos = context.find(L"\"artistName\"");
        if (artistPos != std::wstring::npos) {
            size_t avalueStart = context.find(L":", artistPos);
            if (avalueStart != std::wstring::npos) {
                size_t astrStart = context.find(L"\"", avalueStart + 1);
                if (astrStart != std::wstring::npos) {
                    astrStart++;
                    size_t astrEnd = context.find(L"\"", astrStart);
                    if (astrEnd != std::wstring::npos) {
                        r.artistName = context.substr(astrStart, astrEnd - astrStart);
                    }
                }
            }
        }

        if (!r.name.empty() && !r.feedUrl.empty()) {
            results.push_back(r);
        }

        pos++;
    }

    return !results.empty();
}

// Refresh podcast subscriptions list
static void RefreshPodcastSubsList(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_PODCAST_SUBS_LIST);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);

    g_podcastSubs = GetPodcastSubscriptions();
    // Sort alphabetically by name (case-insensitive)
    std::sort(g_podcastSubs.begin(), g_podcastSubs.end(),
              [](const PodcastSubscription& a, const PodcastSubscription& b) {
                  return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
              });
    for (const auto& sub : g_podcastSubs) {
        SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sub.name.c_str()));
    }
}

// Subclass proc for podcast description edit - prevents text selection
static WNDPROC g_origDescProc = nullptr;
static LRESULT CALLBACK PodcastDescSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    LRESULT result = CallWindowProcW(g_origDescProc, hwnd, msg, wParam, lParam);
    if (msg == WM_SETFOCUS || msg == WM_SETTEXT) {
        // Deselect any text after focus or text change
        SendMessageW(hwnd, EM_SETSEL, 0, 0);
    }
    return result;
}

// Load episodes for a subscription
static void LoadPodcastEpisodes(HWND hwnd, const std::wstring& feedUrl) {
    HWND hList = GetDlgItem(hwnd, IDC_PODCAST_EPISODES);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    g_podcastEpisodes.clear();
    SetDlgItemTextW(hwnd, IDC_PODCAST_EP_DESC, L"");

    Speak("Loading episodes");

    std::wstring title;
    if (ParsePodcastFeed(feedUrl, title, g_podcastEpisodes)) {
        for (const auto& ep : g_podcastEpisodes) {
            std::wstring display = ep.title;
            if (!ep.pubDate.empty()) {
                // Truncate pub date to just the date part
                size_t commaPos = ep.pubDate.find(L',');
                if (commaPos != std::wstring::npos && commaPos + 12 < ep.pubDate.length()) {
                    display += L" (" + ep.pubDate.substr(commaPos + 2, 11) + L")";
                }
            }
            if (!ep.description.empty()) {
                // Clean up description - remove HTML tags and limit length
                std::wstring desc = ep.description;
                // Remove HTML tags
                size_t pos;
                while ((pos = desc.find(L'<')) != std::wstring::npos) {
                    size_t endPos = desc.find(L'>', pos);
                    if (endPos != std::wstring::npos) {
                        desc.erase(pos, endPos - pos + 1);
                    } else {
                        break;
                    }
                }
                // Replace &nbsp; and other entities
                while ((pos = desc.find(L"&nbsp;")) != std::wstring::npos) {
                    desc.replace(pos, 6, L" ");
                }
                while ((pos = desc.find(L"&amp;")) != std::wstring::npos) {
                    desc.replace(pos, 5, L"&");
                }
                while ((pos = desc.find(L"&quot;")) != std::wstring::npos) {
                    desc.replace(pos, 6, L"\"");
                }
                while ((pos = desc.find(L"&apos;")) != std::wstring::npos) {
                    desc.replace(pos, 6, L"'");
                }
                while ((pos = desc.find(L"&lt;")) != std::wstring::npos) {
                    desc.replace(pos, 4, L"<");
                }
                while ((pos = desc.find(L"&gt;")) != std::wstring::npos) {
                    desc.replace(pos, 4, L">");
                }
                // Trim whitespace and collapse multiple spaces
                while (!desc.empty() && (desc[0] == L' ' || desc[0] == L'\n' || desc[0] == L'\r' || desc[0] == L'\t')) {
                    desc.erase(0, 1);
                }
                // Truncate to reasonable length
                if (desc.length() > 150) {
                    desc = desc.substr(0, 147) + L"...";
                }
                if (!desc.empty()) {
                    display += L" - " + desc;
                }
            }
            SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
        }

        // Select first episode
        if (!g_podcastEpisodes.empty()) {
            SendMessageW(hList, LB_SETCURSEL, 0, 0);
            // Trigger description update by simulating selection change
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_PODCAST_EPISODES, LBN_SELCHANGE),
                         reinterpret_cast<LPARAM>(hList));
        }

        char buf[64];
        snprintf(buf, sizeof(buf), "%d episodes", static_cast<int>(g_podcastEpisodes.size()));
        Speak(buf);
    } else {
        Speak("Failed to load episodes");
    }
}

// Update visibility of podcast tab controls
static void UpdatePodcastTabVisibility(HWND hwnd, int tab) {
    // Subscriptions tab controls (tab 0)
    int subsCtrls[] = {IDC_PODCAST_SUBS_LIST, IDC_PODCAST_EPISODES, IDC_PODCAST_EP_DESC,
                       IDC_PODCAST_REFRESH,
                       IDC_PODCAST_SUBS_LABEL, IDC_PODCAST_EP_LABEL, IDC_PODCAST_SUBS_HELP};
    // Search tab controls (tab 1)
    int searchCtrls[] = {IDC_PODCAST_SEARCH_EDIT, IDC_PODCAST_SEARCH_BTN,
                         IDC_PODCAST_SEARCH_LIST, IDC_PODCAST_SUBSCRIBE, IDC_PODCAST_ADD_URL,
                         IDC_PODCAST_SEARCH_LABEL, IDC_PODCAST_SEARCH_HELP};

    for (int id : subsCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 0 ? SW_SHOW : SW_HIDE);
    }
    for (int id : searchCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 1 ? SW_SHOW : SW_HIDE);
    }
}

// Add podcast dialog procedure
static INT_PTR CALLBACK PodcastAddDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static std::wstring* pName = nullptr;
    static std::wstring* pUrl = nullptr;

    switch (msg) {
        case WM_INITDIALOG:
            pName = reinterpret_cast<std::wstring*>(lParam);
            pUrl = pName + 1;
            SetDlgItemTextW(hwnd, IDC_PODCAST_NAME, pName->c_str());
            SetDlgItemTextW(hwnd, IDC_PODCAST_FEED_URL, pUrl->c_str());
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    wchar_t name[256], url[512];
                    GetDlgItemTextW(hwnd, IDC_PODCAST_NAME, name, 256);
                    GetDlgItemTextW(hwnd, IDC_PODCAST_FEED_URL, url, 512);

                    // Trim whitespace
                    std::wstring n = name, u = url;
                    while (!n.empty() && (n.front() == L' ' || n.front() == L'\t')) n.erase(0, 1);
                    while (!n.empty() && (n.back() == L' ' || n.back() == L'\t')) n.pop_back();
                    while (!u.empty() && (u.front() == L' ' || u.front() == L'\t')) u.erase(0, 1);
                    while (!u.empty() && (u.back() == L' ' || u.back() == L'\t')) u.pop_back();

                    if (n.empty() || u.empty()) {
                        MessageBoxW(hwnd, L"Please enter both a name and feed URL.", L"Add Podcast", MB_ICONWARNING);
                        return TRUE;
                    }

                    *pName = n;
                    *pUrl = u;
                    EndDialog(hwnd, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Podcast dialog procedure
static INT_PTR CALLBACK PodcastDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            // Initialize tab control
            HWND hTab = GetDlgItem(hwnd, IDC_PODCAST_TAB);
            TCITEMW tie = {0};
            tie.mask = TCIF_TEXT;
            tie.pszText = const_cast<LPWSTR>(L"Subscriptions");
            SendMessageW(hTab, TCM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&tie));
            tie.pszText = const_cast<LPWSTR>(L"Search");
            SendMessageW(hTab, TCM_INSERTITEMW, 1, reinterpret_cast<LPARAM>(&tie));

            // Subclass description edit to prevent text selection
            HWND hDesc = GetDlgItem(hwnd, IDC_PODCAST_EP_DESC);
            g_origDescProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hDesc, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PodcastDescSubclassProc)));

            // Load subscriptions
            RefreshPodcastSubsList(hwnd);
            g_podcastEpisodes.clear();
            g_podcastSearchResults.clear();
            g_currentPodcastId = -1;

            UpdatePodcastTabVisibility(hwnd, 0);
            SetFocus(GetDlgItem(hwnd, IDC_PODCAST_SUBS_LIST));
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    // Handle Enter key based on focus
                    HWND hFocus = GetFocus();
                    if (hFocus == GetDlgItem(hwnd, IDC_PODCAST_SUBS_LIST)) {
                        // Load episodes for selected subscription
                        int sel = static_cast<int>(SendMessageW(hFocus, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_podcastSubs.size())) {
                            g_currentPodcastId = g_podcastSubs[sel].id;
                            LoadPodcastEpisodes(hwnd, g_podcastSubs[sel].feedUrl);
                            SetFocus(GetDlgItem(hwnd, IDC_PODCAST_EPISODES));
                        }
                    } else if (hFocus == GetDlgItem(hwnd, IDC_PODCAST_EPISODES)) {
                        // Play selected episode
                        int sel = static_cast<int>(SendMessageW(hFocus, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_podcastEpisodes.size())) {
                            g_playlist.clear();
                            g_playlist.push_back(g_podcastEpisodes[sel].audioUrl);
                            PlayTrack(0, true);
                            Speak("Playing");
                        }
                    } else if (hFocus == GetDlgItem(hwnd, IDC_PODCAST_SEARCH_EDIT)) {
                        // Trigger search
                        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_PODCAST_SEARCH_BTN, BN_CLICKED), 0);
                    } else if (hFocus == GetDlgItem(hwnd, IDC_PODCAST_SEARCH_LIST)) {
                        // Preview/play first episode of selected podcast
                        int sel = static_cast<int>(SendMessageW(hFocus, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_podcastSearchResults.size())) {
                            std::vector<PodcastEpisode> eps;
                            std::wstring title;
                            Speak("Loading preview");
                            if (ParsePodcastFeed(g_podcastSearchResults[sel].feedUrl, title, eps) && !eps.empty()) {
                                g_playlist.clear();
                                g_playlist.push_back(eps[0].audioUrl);
                                PlayTrack(0, true);
                                Speak("Playing");
                            } else {
                                Speak("No episodes found");
                            }
                        }
                    }
                    return TRUE;
                }

                case IDC_PODCAST_REFRESH: {
                    int sel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_PODCAST_SUBS_LIST), LB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < static_cast<int>(g_podcastSubs.size())) {
                        LoadPodcastEpisodes(hwnd, g_podcastSubs[sel].feedUrl);
                        UpdatePodcastLastUpdated(g_podcastSubs[sel].id);
                    }
                    return TRUE;
                }

                case IDC_PODCAST_SEARCH_BTN: {
                    wchar_t query[256];
                    GetDlgItemTextW(hwnd, IDC_PODCAST_SEARCH_EDIT, query, 256);
                    if (wcslen(query) == 0) return TRUE;

                    Speak("Searching");
                    HWND hList = GetDlgItem(hwnd, IDC_PODCAST_SEARCH_LIST);
                    SendMessageW(hList, LB_RESETCONTENT, 0, 0);

                    if (SearchItunesPodcasts(query, g_podcastSearchResults)) {
                        for (const auto& r : g_podcastSearchResults) {
                            std::wstring display = r.name;
                            if (!r.artistName.empty()) {
                                display += L" - " + r.artistName;
                            }
                            SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
                        }
                        SendMessageW(hList, LB_SETCURSEL, 0, 0);
                        SetFocus(hList);
                        char buf[64];
                        snprintf(buf, sizeof(buf), "%d results", static_cast<int>(g_podcastSearchResults.size()));
                        Speak(buf);
                    } else {
                        Speak("No results");
                    }
                    return TRUE;
                }

                case IDC_PODCAST_SUBSCRIBE: {
                    int sel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_PODCAST_SEARCH_LIST), LB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < static_cast<int>(g_podcastSearchResults.size())) {
                        const auto& r = g_podcastSearchResults[sel];
                        if (AddPodcastSubscription(r.name, r.feedUrl, r.imageUrl) > 0) {
                            RefreshPodcastSubsList(hwnd);
                            Speak("Subscribed");
                        } else {
                            Speak("Already subscribed or failed");
                        }
                    } else {
                        Speak("Select a podcast first");
                    }
                    return TRUE;
                }

                case IDC_PODCAST_ADD_URL: {
                    // Use URL dialog to get feed URL, then fetch name from feed
                    wchar_t urlBuf[2048] = {0};
                    if (DialogBoxParamW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_URL),
                                        hwnd, [](HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) -> INT_PTR {
                        static wchar_t* pUrl = nullptr;
                        switch (msg) {
                            case WM_INITDIALOG:
                                pUrl = reinterpret_cast<wchar_t*>(lParam);
                                SetWindowTextW(hDlg, L"Add Podcast Feed");
                                SetDlgItemTextW(hDlg, -1, L"Enter podcast feed URL:");
                                return TRUE;
                            case WM_COMMAND:
                                if (LOWORD(wParam) == IDOK) {
                                    GetDlgItemTextW(hDlg, IDC_URL_EDIT, pUrl, 2048);
                                    EndDialog(hDlg, IDOK);
                                    return TRUE;
                                } else if (LOWORD(wParam) == IDCANCEL) {
                                    EndDialog(hDlg, IDCANCEL);
                                    return TRUE;
                                }
                                break;
                        }
                        return FALSE;
                    }, reinterpret_cast<LPARAM>(urlBuf)) == IDOK && urlBuf[0]) {
                        Speak("Fetching feed");
                        std::wstring feedUrl = urlBuf;
                        std::wstring title;
                        std::vector<PodcastEpisode> eps;
                        if (ParsePodcastFeed(feedUrl, title, eps)) {
                            if (title.empty()) title = L"Unknown Podcast";
                            if (AddPodcastSubscription(title, feedUrl) > 0) {
                                RefreshPodcastSubsList(hwnd);
                                Speak("Podcast added");
                            } else {
                                Speak("Already subscribed or failed");
                            }
                        } else {
                            Speak("Failed to fetch feed");
                        }
                    }
                    return TRUE;
                }

                case IDC_PODCAST_SUBS_LIST:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        // Load episodes on double-click
                        int sel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_PODCAST_SUBS_LIST), LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_podcastSubs.size())) {
                            g_currentPodcastId = g_podcastSubs[sel].id;
                            LoadPodcastEpisodes(hwnd, g_podcastSubs[sel].feedUrl);
                            SetFocus(GetDlgItem(hwnd, IDC_PODCAST_EPISODES));
                        }
                    }
                    break;

                case IDC_PODCAST_EPISODES:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        // Play episode on double-click
                        int sel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_PODCAST_EPISODES), LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_podcastEpisodes.size())) {
                            g_playlist.clear();
                            g_playlist.push_back(g_podcastEpisodes[sel].audioUrl);
                            PlayTrack(0, true);
                            Speak("Playing");
                        }
                    } else if (HIWORD(wParam) == LBN_SELCHANGE) {
                        // Update description field
                        int sel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_PODCAST_EPISODES), LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_podcastEpisodes.size())) {
                            std::wstring desc = g_podcastEpisodes[sel].description;
                            // Clean up description - remove HTML tags
                            size_t pos;
                            while ((pos = desc.find(L'<')) != std::wstring::npos) {
                                size_t endPos = desc.find(L'>', pos);
                                if (endPos != std::wstring::npos) {
                                    desc.erase(pos, endPos - pos + 1);
                                } else {
                                    break;
                                }
                            }
                            // Decode HTML entities
                            while ((pos = desc.find(L"&nbsp;")) != std::wstring::npos) desc.replace(pos, 6, L" ");
                            while ((pos = desc.find(L"&amp;")) != std::wstring::npos) desc.replace(pos, 5, L"&");
                            while ((pos = desc.find(L"&quot;")) != std::wstring::npos) desc.replace(pos, 6, L"\"");
                            while ((pos = desc.find(L"&apos;")) != std::wstring::npos) desc.replace(pos, 6, L"'");
                            while ((pos = desc.find(L"&lt;")) != std::wstring::npos) desc.replace(pos, 4, L"<");
                            while ((pos = desc.find(L"&gt;")) != std::wstring::npos) desc.replace(pos, 4, L">");
                            while ((pos = desc.find(L"&#39;")) != std::wstring::npos) desc.replace(pos, 5, L"'");
                            // Convert standalone \n to \r\n for edit control (skip existing \r\n)
                            pos = 0;
                            while ((pos = desc.find(L'\n', pos)) != std::wstring::npos) {
                                if (pos == 0 || desc[pos - 1] != L'\r') {
                                    desc.insert(pos, 1, L'\r');
                                    pos += 2;
                                } else {
                                    pos++;
                                }
                            }
                            SetDlgItemTextW(hwnd, IDC_PODCAST_EP_DESC, desc.c_str());
                            // Scroll to top (deselect handled by subclass)
                            SendDlgItemMessageW(hwnd, IDC_PODCAST_EP_DESC, WM_VSCROLL, SB_TOP, 0);
                        } else {
                            SetDlgItemTextW(hwnd, IDC_PODCAST_EP_DESC, L"");
                        }
                    }
                    break;

                case IDC_PODCAST_SEARCH_LIST:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        // Subscribe on double-click
                        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_PODCAST_SUBSCRIBE, BN_CLICKED), 0);
                    }
                    break;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_NOTIFY: {
            NMHDR* pnmh = reinterpret_cast<NMHDR*>(lParam);
            if (pnmh->idFrom == IDC_PODCAST_TAB && pnmh->code == TCN_SELCHANGE) {
                int tab = static_cast<int>(SendMessageW(pnmh->hwndFrom, TCM_GETCURSEL, 0, 0));
                UpdatePodcastTabVisibility(hwnd, tab);
            }
            break;
        }

        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);

            // Resize tab control
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_TAB), nullptr, 7, 7, width - 14, height - 42, SWP_NOZORDER);

            // Resize subscriptions list (left side)
            int subsWidth = (width - 28) / 3;
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_SUBS_LIST), nullptr, 14, 40, subsWidth, height - 90, SWP_NOZORDER);

            // Resize episodes list (right side, top portion)
            int epsX = 14 + subsWidth + 8;
            int epsWidth = width - epsX - 14;
            int epsHeight = (height - 90) * 55 / 100;  // 55% for episodes list
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_EPISODES), nullptr, epsX, 40, epsWidth, epsHeight, SWP_NOZORDER);

            // Description field below episodes list
            int descY = 40 + epsHeight + 4;
            int descHeight = height - 90 - epsHeight - 4;
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_EP_DESC), nullptr, epsX, descY, epsWidth, descHeight, SWP_NOZORDER);

            // Reposition buttons
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_ADD_FEED), nullptr, width - 130, height - 46, 60, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_REFRESH), nullptr, width - 64, height - 46, 50, 14, SWP_NOZORDER);

            // Search tab controls
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_SEARCH_EDIT), nullptr, 72, 28, width - 140, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_SEARCH_BTN), nullptr, width - 64, 27, 50, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_SEARCH_LIST), nullptr, 14, 48, width - 28, height - 120, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_SUBSCRIBE), nullptr, width - 130, height - 66, 55, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_ADD_URL), nullptr, width - 70, height - 66, 55, 14, SWP_NOZORDER);

            // Close button
            SetWindowPos(GetDlgItem(hwnd, IDCANCEL), nullptr, width - 64, height - 28, 50, 14, SWP_NOZORDER);

            InvalidateRect(hwnd, nullptr, TRUE);
            return TRUE;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 400;
            mmi->ptMinTrackSize.y = 250;
            return 0;
        }
    }
    return FALSE;
}

// Show podcast dialog
void ShowPodcastDialog() {
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_PODCAST), g_hwnd, PodcastDlgProc);
}

// ========== Scheduler Dialog ==========

static std::vector<ScheduledEvent> g_schedEvents;

static void RefreshScheduleList(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_SCHED_LIST);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);

    g_schedEvents = GetAllScheduledEvents();
    for (const auto& ev : g_schedEvents) {
        SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ev.displayName.c_str()));
    }
}

// Calculate next schedule time for repeating events
void CalculateNextScheduleTime(int id, int64_t lastRun, ScheduleRepeat repeat) {
    if (repeat == ScheduleRepeat::None) {
        // One-time event, just disable it
        UpdateScheduledEventEnabled(id, false);
        return;
    }

    time_t now = time(nullptr);
    struct tm tm;
    localtime_s(&tm, &now);

    // Start from the current scheduled time and add appropriate interval
    int64_t nextTime = lastRun;

    switch (repeat) {
        case ScheduleRepeat::Daily:
            nextTime += 24 * 60 * 60;  // Add 1 day
            break;

        case ScheduleRepeat::Weekly:
            nextTime += 7 * 24 * 60 * 60;  // Add 1 week
            break;

        case ScheduleRepeat::Weekdays: {
            // Find next weekday
            time_t t = static_cast<time_t>(nextTime);
            do {
                t += 24 * 60 * 60;
                localtime_s(&tm, &t);
            } while (tm.tm_wday == 0 || tm.tm_wday == 6);  // Skip Sun=0, Sat=6
            nextTime = static_cast<int64_t>(t);
            break;
        }

        case ScheduleRepeat::Weekends: {
            // Find next weekend day
            time_t t = static_cast<time_t>(nextTime);
            do {
                t += 24 * 60 * 60;
                localtime_s(&tm, &t);
            } while (tm.tm_wday != 0 && tm.tm_wday != 6);  // Find Sun=0 or Sat=6
            nextTime = static_cast<int64_t>(t);
            break;
        }

        case ScheduleRepeat::Monthly: {
            // Same day next month
            time_t t = static_cast<time_t>(nextTime);
            localtime_s(&tm, &t);
            tm.tm_mon += 1;
            if (tm.tm_mon > 11) {
                tm.tm_mon = 0;
                tm.tm_year += 1;
            }
            nextTime = static_cast<int64_t>(mktime(&tm));
            break;
        }

        default:
            break;
    }

    UpdateScheduledEventTime(id, nextTime);
}

// Handle scheduled duration timer expiry
void HandleScheduledDurationEnd() {
    // Restore mute state if we muted for scheduled recording
    if (g_schedulerMuted) {
        g_muted = false;
        g_schedulerMuted = false;
    }

    switch (g_pendingStopAction) {
        case ScheduleStopAction::StopBoth:
            Stop();
            if (g_isRecording) {
                StopRecording();
            }
            Speak("Scheduled event ended");
            break;
        case ScheduleStopAction::StopPlayback:
            Stop();
            Speak("Scheduled playback ended");
            break;
        case ScheduleStopAction::StopRecording:
            if (g_isRecording) {
                StopRecording();
                Speak("Scheduled recording ended");
            }
            break;
    }
}

// Check for and execute scheduled events
void CheckScheduledEvents() {
    std::vector<ScheduledEvent> pending = GetPendingScheduledEvents();

    for (const auto& ev : pending) {
        // Mark as run immediately to prevent re-triggering
        int64_t now = static_cast<int64_t>(time(nullptr));
        UpdateScheduledEventLastRun(ev.id, now);

        // Execute the action
        bool shouldPlay = (ev.action == ScheduleAction::Playback || ev.action == ScheduleAction::Both);
        bool shouldRecord = (ev.action == ScheduleAction::Recording || ev.action == ScheduleAction::Both);

        // Load the source
        if (shouldPlay || shouldRecord) {
            g_playlist.clear();
            g_playlist.push_back(ev.sourcePath);

            // For Recording-only mode, mute playback so user doesn't hear it
            // but the stream still plays (required for encoder to capture audio)
            if (shouldRecord && !shouldPlay) {
                g_muted = true;
                g_schedulerMuted = true;
            }

            if (shouldRecord && !g_isRecording) {
                // Start recording before playback
                ToggleRecording();
            }

            // Always play the track - recording requires audio to flow through the stream
            PlayTrack(0);

            // Set up duration timer if specified
            if (ev.duration > 0) {
                g_pendingStopAction = ev.stopAction;
                // Set timer for duration (minutes to milliseconds)
                SetTimer(g_hwnd, IDT_SCHED_DURATION, ev.duration * 60 * 1000, nullptr);
            }
        }

        // Speak announcement
        std::wstring msg = L"Scheduled event: " + ev.name;
        Speak(WideToUtf8(msg).c_str());

        // Calculate next run time for repeating events
        CalculateNextScheduleTime(ev.id, ev.scheduledTime, ev.repeat);
    }
}

// Scheduler list subclass proc
static WNDPROC g_origSchedListProc = nullptr;

static LRESULT CALLBACK SchedListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            // Toggle enabled state
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_schedEvents.size())) {
                bool newState = !g_schedEvents[sel].enabled;
                if (UpdateScheduledEventEnabled(g_schedEvents[sel].id, newState)) {
                    Speak(newState ? "Enabled" : "Disabled");
                    RefreshScheduleList(GetParent(hwnd));
                    SendMessageW(hwnd, LB_SETCURSEL, sel, 0);
                }
            }
            return 0;
        } else if (wParam == VK_DELETE) {
            // Remove selected event
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_schedEvents.size())) {
                if (RemoveScheduledEvent(g_schedEvents[sel].id)) {
                    Speak("Schedule removed");
                    RefreshScheduleList(GetParent(hwnd));
                    int count = static_cast<int>(SendMessageW(hwnd, LB_GETCOUNT, 0, 0));
                    if (count > 0) {
                        if (sel >= count) sel = count - 1;
                        SendMessageW(hwnd, LB_SETCURSEL, sel, 0);
                    }
                }
            }
            return 0;
        } else if (wParam == VK_ESCAPE) {
            SendMessageW(GetParent(hwnd), WM_COMMAND, IDCANCEL, 0);
            return 0;
        }
    } else if (msg == WM_GETDLGCODE) {
        MSG* pmsg = reinterpret_cast<MSG*>(lParam);
        if (pmsg && (pmsg->wParam == VK_RETURN || pmsg->wParam == VK_ESCAPE || pmsg->wParam == VK_DELETE)) {
            return DLGC_WANTMESSAGE;
        }
        return CallWindowProcW(g_origSchedListProc, hwnd, msg, wParam, lParam);
    }
    return CallWindowProcW(g_origSchedListProc, hwnd, msg, wParam, lParam);
}

// Show/hide file vs radio controls based on source selection
static void UpdateSchedSourceControls(HWND hwnd) {
    HWND hSource = GetDlgItem(hwnd, IDC_SCHED_SOURCE);
    int sel = static_cast<int>(SendMessageW(hSource, CB_GETCURSEL, 0, 0));
    bool isFile = (sel == 0);

    // Show/hide file controls
    ShowWindow(GetDlgItem(hwnd, IDC_SCHED_FILE), isFile ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_SCHED_BROWSE), isFile ? SW_SHOW : SW_HIDE);

    // Show/hide radio control
    ShowWindow(GetDlgItem(hwnd, IDC_SCHED_RADIO), isFile ? SW_HIDE : SW_SHOW);
}

// Add schedule dialog proc
static INT_PTR CALLBACK SchedAddDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            // Action combo
            HWND hAction = GetDlgItem(hwnd, IDC_SCHED_ACTION);
            SendMessageW(hAction, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Playback"));
            SendMessageW(hAction, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Recording"));
            SendMessageW(hAction, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Both"));
            SendMessageW(hAction, CB_SETCURSEL, 0, 0);

            // Source combo
            HWND hSource = GetDlgItem(hwnd, IDC_SCHED_SOURCE);
            SendMessageW(hSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"File"));
            SendMessageW(hSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Radio"));
            SendMessageW(hSource, CB_SETCURSEL, 0, 0);

            // Radio stations combo
            HWND hRadio = GetDlgItem(hwnd, IDC_SCHED_RADIO);
            std::vector<RadioStation> stations = GetRadioFavorites();
            for (const auto& rs : stations) {
                int idx = static_cast<int>(SendMessageW(hRadio, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(rs.name.c_str())));
                SendMessageW(hRadio, CB_SETITEMDATA, idx, rs.id);
            }
            if (!stations.empty()) {
                SendMessageW(hRadio, CB_SETCURSEL, 0, 0);
            }

            // Repeat combo
            HWND hRepeat = GetDlgItem(hwnd, IDC_SCHED_REPEAT);
            SendMessageW(hRepeat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Once"));
            SendMessageW(hRepeat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Daily"));
            SendMessageW(hRepeat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Weekly"));
            SendMessageW(hRepeat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Weekdays"));
            SendMessageW(hRepeat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Weekends"));
            SendMessageW(hRepeat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Monthly"));
            SendMessageW(hRepeat, CB_SETCURSEL, 0, 0);

            // Enabled checkbox - default on
            CheckDlgButton(hwnd, IDC_SCHED_ENABLED, BST_CHECKED);

            // Duration - default 0 (no limit)
            SetDlgItemTextW(hwnd, IDC_SCHED_DURATION, L"0");

            // Stop action combo (only relevant when action is "Both")
            HWND hStop = GetDlgItem(hwnd, IDC_SCHED_STOP);
            SendMessageW(hStop, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Both"));
            SendMessageW(hStop, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Playback only"));
            SendMessageW(hStop, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Recording only"));
            SendMessageW(hStop, CB_SETCURSEL, 0, 0);

            // Set default date/time to now + 1 hour
            SYSTEMTIME st;
            GetLocalTime(&st);
            // Add 1 hour
            FILETIME ft;
            SystemTimeToFileTime(&st, &ft);
            ULARGE_INTEGER uli;
            uli.LowPart = ft.dwLowDateTime;
            uli.HighPart = ft.dwHighDateTime;
            uli.QuadPart += 36000000000ULL;  // 1 hour in 100ns units
            ft.dwLowDateTime = uli.LowPart;
            ft.dwHighDateTime = uli.HighPart;
            FileTimeToSystemTime(&ft, &st);

            SendDlgItemMessageW(hwnd, IDC_SCHED_DATE, DTM_SETSYSTEMTIME, GDT_VALID,
                reinterpret_cast<LPARAM>(&st));
            SendDlgItemMessageW(hwnd, IDC_SCHED_TIME, DTM_SETSYSTEMTIME, GDT_VALID,
                reinterpret_cast<LPARAM>(&st));

            // If currently playing, prefill file
            if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
                SetDlgItemTextW(hwnd, IDC_SCHED_FILE, g_playlist[g_currentTrack].c_str());
            }

            UpdateSchedSourceControls(hwnd);
            SetFocus(GetDlgItem(hwnd, IDC_SCHED_NAME));
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_SCHED_SOURCE:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        UpdateSchedSourceControls(hwnd);
                    }
                    return TRUE;

                case IDC_SCHED_BROWSE: {
                    wchar_t filePath[MAX_PATH] = {0};
                    // Get current value
                    GetDlgItemTextW(hwnd, IDC_SCHED_FILE, filePath, MAX_PATH);

                    OPENFILENAMEW ofn = {sizeof(ofn)};
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = L"All Supported\0*.mp3;*.wav;*.ogg;*.flac;*.m4a;*.wma;*.aac;*.opus;*.aiff;*.ape;*.wv;*.mid;*.midi\0"
                                      L"All Files (*.*)\0*.*\0";
                    ofn.lpstrFile = filePath;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrTitle = L"Select Audio File";
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn)) {
                        SetDlgItemTextW(hwnd, IDC_SCHED_FILE, filePath);
                    }
                    return TRUE;
                }

                case IDOK: {
                    wchar_t name[256] = {0};
                    GetDlgItemTextW(hwnd, IDC_SCHED_NAME, name, 256);

                    if (wcslen(name) == 0) {
                        MessageBoxW(hwnd, L"Please enter a name.", L"Add Schedule", MB_ICONWARNING);
                        SetFocus(GetDlgItem(hwnd, IDC_SCHED_NAME));
                        return TRUE;
                    }

                    ScheduleAction action = static_cast<ScheduleAction>(
                        SendDlgItemMessageW(hwnd, IDC_SCHED_ACTION, CB_GETCURSEL, 0, 0));

                    int sourceIdx = static_cast<int>(
                        SendDlgItemMessageW(hwnd, IDC_SCHED_SOURCE, CB_GETCURSEL, 0, 0));
                    ScheduleSource sourceType = static_cast<ScheduleSource>(sourceIdx);

                    std::wstring sourcePath;
                    int radioStationId = 0;

                    if (sourceType == ScheduleSource::File) {
                        wchar_t filePath[MAX_PATH] = {0};
                        GetDlgItemTextW(hwnd, IDC_SCHED_FILE, filePath, MAX_PATH);
                        if (wcslen(filePath) == 0) {
                            MessageBoxW(hwnd, L"Please select a file.", L"Add Schedule", MB_ICONWARNING);
                            SetFocus(GetDlgItem(hwnd, IDC_SCHED_FILE));
                            return TRUE;
                        }
                        sourcePath = filePath;
                    } else {
                        int sel = static_cast<int>(
                            SendDlgItemMessageW(hwnd, IDC_SCHED_RADIO, CB_GETCURSEL, 0, 0));
                        if (sel < 0) {
                            MessageBoxW(hwnd, L"Please select a radio station.", L"Add Schedule", MB_ICONWARNING);
                            SetFocus(GetDlgItem(hwnd, IDC_SCHED_RADIO));
                            return TRUE;
                        }
                        radioStationId = static_cast<int>(
                            SendDlgItemMessageW(hwnd, IDC_SCHED_RADIO, CB_GETITEMDATA, sel, 0));
                        // Get the URL from the radio stations
                        std::vector<RadioStation> stations = GetRadioFavorites();
                        for (const auto& rs : stations) {
                            if (rs.id == radioStationId) {
                                sourcePath = rs.url;
                                break;
                            }
                        }
                    }

                    // Get date and time
                    SYSTEMTIME stDate = {0}, stTime = {0};
                    SendDlgItemMessageW(hwnd, IDC_SCHED_DATE, DTM_GETSYSTEMTIME, 0,
                        reinterpret_cast<LPARAM>(&stDate));
                    SendDlgItemMessageW(hwnd, IDC_SCHED_TIME, DTM_GETSYSTEMTIME, 0,
                        reinterpret_cast<LPARAM>(&stTime));

                    // Combine date and time
                    SYSTEMTIME st = stDate;
                    st.wHour = stTime.wHour;
                    st.wMinute = stTime.wMinute;
                    st.wSecond = 0;
                    st.wMilliseconds = 0;

                    // Convert to timestamp
                    struct tm tm = {0};
                    tm.tm_year = st.wYear - 1900;
                    tm.tm_mon = st.wMonth - 1;
                    tm.tm_mday = st.wDay;
                    tm.tm_hour = st.wHour;
                    tm.tm_min = st.wMinute;
                    tm.tm_sec = 0;
                    tm.tm_isdst = -1;
                    int64_t scheduledTime = static_cast<int64_t>(mktime(&tm));

                    ScheduleRepeat repeat = static_cast<ScheduleRepeat>(
                        SendDlgItemMessageW(hwnd, IDC_SCHED_REPEAT, CB_GETCURSEL, 0, 0));

                    bool enabled = IsDlgButtonChecked(hwnd, IDC_SCHED_ENABLED) == BST_CHECKED;

                    // Get duration
                    int duration = GetDlgItemInt(hwnd, IDC_SCHED_DURATION, nullptr, FALSE);
                    if (duration < 0) duration = 0;

                    // Get stop action
                    ScheduleStopAction stopAction = static_cast<ScheduleStopAction>(
                        SendDlgItemMessageW(hwnd, IDC_SCHED_STOP, CB_GETCURSEL, 0, 0));

                    int id = AddScheduledEvent(name, action, sourceType, sourcePath,
                                               radioStationId, scheduledTime, repeat, enabled,
                                               duration, stopAction);
                    if (id >= 0) {
                        EndDialog(hwnd, IDOK);
                    } else {
                        MessageBoxW(hwnd, L"Failed to add scheduled event.", L"Add Schedule", MB_ICONERROR);
                    }
                    return TRUE;
                }

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Scheduler dialog proc
static INT_PTR CALLBACK SchedulerDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            // Subclass the listbox
            HWND hList = GetDlgItem(hwnd, IDC_SCHED_LIST);
            g_origSchedListProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SchedListSubclassProc)));

            RefreshScheduleList(hwnd);

            SetFocus(hList);
            if (SendMessageW(hList, LB_GETCOUNT, 0, 0) > 0) {
                SendMessageW(hList, LB_SETCURSEL, 0, 0);
            }
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_SCHED_ADD:
                    if (DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_SCHED_ADD),
                                   hwnd, SchedAddDlgProc) == IDOK) {
                        RefreshScheduleList(hwnd);
                        Speak("Schedule added");
                    }
                    return TRUE;

                case IDC_SCHED_LIST:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        // Double-click to toggle
                        HWND hList = GetDlgItem(hwnd, IDC_SCHED_LIST);
                        int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_schedEvents.size())) {
                            bool newState = !g_schedEvents[sel].enabled;
                            if (UpdateScheduledEventEnabled(g_schedEvents[sel].id, newState)) {
                                Speak(newState ? "Enabled" : "Disabled");
                                RefreshScheduleList(hwnd);
                                SendMessageW(hList, LB_SETCURSEL, sel, 0);
                            }
                        }
                    }
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_SIZE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            SetWindowPos(GetDlgItem(hwnd, IDC_SCHED_LIST), nullptr, 7, 20, w - 14, h - 60, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_SCHED_ADD), nullptr, 7, h - 32, 50, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDCANCEL), nullptr, w - 64, h - 22, 50, 14, SWP_NOZORDER);
            return TRUE;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 300;
            mmi->ptMinTrackSize.y = 200;
            return TRUE;
        }
    }
    return FALSE;
}

// Show scheduler dialog
void ShowSchedulerDialog() {
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_SCHEDULER), g_hwnd, SchedulerDlgProc);
}

// Tag view dialog data
static std::wstring g_tagDialogText;
static const wchar_t* g_tagDialogTitle;

// Tag view dialog procedure
static INT_PTR CALLBACK TagViewDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            SetWindowTextW(hwnd, g_tagDialogTitle);
            SetDlgItemTextW(hwnd, IDC_TAG_TEXT, g_tagDialogText.c_str());
            // Select all text for easy copying
            SendDlgItemMessageW(hwnd, IDC_TAG_TEXT, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(hwnd, IDC_TAG_TEXT));
            return FALSE;  // Don't set default focus

        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                EndDialog(hwnd, LOWORD(wParam));
                return TRUE;
            }
            break;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

// Show tag in a dialog
void ShowTagDialog(const wchar_t* title, const std::wstring& text) {
    g_tagDialogTitle = title;
    g_tagDialogText = text;
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_TAG_VIEW), g_hwnd, TagViewDlgProc);
}
