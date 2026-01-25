#include "youtube.h"
#include "globals.h"
#include "utils.h"
#include "player.h"
#include "accessibility.h"
#include "resource.h"
#include <wininet.h>
#include <shlwapi.h>
#include <regex>
#include <sstream>

#pragma comment(lib, "wininet.lib")

// Dialog state
static HWND g_ytDialog = nullptr;
static std::vector<YouTubeResult> g_ytResults;
static std::wstring g_ytNextPageToken;
static std::wstring g_ytCurrentQuery;
static bool g_ytIsPlaylistView = false;
static std::wstring g_ytCurrentPlaylistId;

// Forward declarations
static bool SearchWithAPI(const std::wstring& query, std::vector<YouTubeResult>& results,
                          std::wstring& nextPageToken, const std::wstring& pageToken);
static bool SearchWithYtdlp(const std::wstring& query, std::vector<YouTubeResult>& results);
static std::wstring RunYtdlp(const std::wstring& args);
static std::wstring UrlEncode(const std::wstring& str);
static std::wstring HttpGet(const std::wstring& url);
static std::wstring ParseJsonString(const std::wstring& json, const std::wstring& key);
static std::vector<std::wstring> ParseJsonArray(const std::wstring& json, const std::wstring& arrayKey);

// Check if yt-dlp is available
static bool IsYtdlpAvailable() {
    if (g_ytdlpPath.empty()) return false;
    return PathFileExistsW(g_ytdlpPath.c_str()) != FALSE;
}

// Check if API key is available
static bool HasApiKey() {
    return !g_ytApiKey.empty();
}

// URL encode a string
static std::wstring UrlEncode(const std::wstring& str) {
    std::string utf8 = WideToUtf8(str);
    std::wostringstream encoded;
    for (unsigned char c : utf8) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << static_cast<wchar_t>(c);
        } else {
            encoded << L'%' << std::hex << std::uppercase
                    << ((c >> 4) & 0xF) << (c & 0xF);
        }
    }
    return encoded.str();
}

// Simple HTTP GET request
static std::wstring HttpGet(const std::wstring& url) {
    std::wstring result;
    HINTERNET hInternet = InternetOpenW(L"FastPlay/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet) return result;

    HINTERNET hConnect = InternetOpenUrlW(hInternet, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
    if (hConnect) {
        char buffer[4096];
        DWORD bytesRead;
        std::string response;
        while (InternetReadFile(hConnect, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            response += buffer;
        }
        // Convert UTF-8 to wide string
        int len = MultiByteToWideChar(CP_UTF8, 0, response.c_str(), -1, nullptr, 0);
        if (len > 0) {
            result.resize(len - 1);
            MultiByteToWideChar(CP_UTF8, 0, response.c_str(), -1, &result[0], len);
        }
        InternetCloseHandle(hConnect);
    }
    InternetCloseHandle(hInternet);
    return result;
}

// Simple JSON string value parser (not a full JSON parser)
static std::wstring ParseJsonString(const std::wstring& json, const std::wstring& key) {
    std::wstring searchKey = L"\"" + key + L"\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::wstring::npos) return L"";

    size_t colonPos = json.find(L':', keyPos + searchKey.length());
    if (colonPos == std::wstring::npos) return L"";

    size_t startQuote = json.find(L'"', colonPos + 1);
    if (startQuote == std::wstring::npos) return L"";

    size_t endQuote = startQuote + 1;
    while (endQuote < json.length()) {
        if (json[endQuote] == L'"' && json[endQuote - 1] != L'\\') break;
        endQuote++;
    }

    std::wstring value = json.substr(startQuote + 1, endQuote - startQuote - 1);
    // Unescape basic sequences
    size_t pos = 0;
    while ((pos = value.find(L"\\n", pos)) != std::wstring::npos) {
        value.replace(pos, 2, L" ");
    }
    pos = 0;
    while ((pos = value.find(L"\\\"", pos)) != std::wstring::npos) {
        value.replace(pos, 2, L"\"");
    }
    return value;
}

// Run yt-dlp and capture output
static std::wstring RunYtdlp(const std::wstring& args) {
    if (!IsYtdlpAvailable()) return L"";

    std::wstring cmdLine = L"\"" + g_ytdlpPath + L"\" " + args;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return L"";

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return L"";
    }

    CloseHandle(hWritePipe);

    std::string output;
    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        output += buffer;
    }

    CloseHandle(hReadPipe);
    WaitForSingleObject(pi.hProcess, 30000);  // 30 second timeout
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Convert UTF-8 to wide string
    int len = MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring result(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, &result[0], len);
    return result;
}

// Search using YouTube Data API
static bool SearchWithAPI(const std::wstring& query, std::vector<YouTubeResult>& results,
                          std::wstring& nextPageToken, const std::wstring& pageToken) {
    if (!HasApiKey()) return false;

    std::wstring url = L"https://www.googleapis.com/youtube/v3/search?part=snippet&type=video&maxResults=25&q=";
    url += UrlEncode(query);
    url += L"&key=" + g_ytApiKey;
    if (!pageToken.empty()) {
        url += L"&pageToken=" + pageToken;
    }

    std::wstring response = HttpGet(url);
    if (response.empty()) return false;

    // Parse results (simple parsing, not full JSON)
    nextPageToken = ParseJsonString(response, L"nextPageToken");

    // Find items array and parse each item
    size_t itemsPos = response.find(L"\"items\"");
    if (itemsPos == std::wstring::npos) return false;

    size_t searchStart = itemsPos;
    while ((searchStart = response.find(L"\"videoId\"", searchStart)) != std::wstring::npos) {
        YouTubeResult result;
        result.videoId = ParseJsonString(response.substr(searchStart, 500), L"videoId");

        // Find the snippet for this item
        size_t snippetPos = response.rfind(L"\"snippet\"", searchStart);
        if (snippetPos != std::wstring::npos && snippetPos > itemsPos) {
            std::wstring snippet = response.substr(snippetPos, searchStart - snippetPos + 1000);
            result.title = ParseJsonString(snippet, L"title");
            result.channel = ParseJsonString(snippet, L"channelTitle");
        }

        if (!result.videoId.empty() && !result.title.empty()) {
            results.push_back(result);
        }
        searchStart += 10;
    }

    return !results.empty();
}

// Search using yt-dlp
static bool SearchWithYtdlp(const std::wstring& query, std::vector<YouTubeResult>& results) {
    // Use yt-dlp to search YouTube
    std::wstring args = L"--flat-playlist --dump-json \"ytsearch25:" + query + L"\"";
    std::wstring output = RunYtdlp(args);
    if (output.empty()) return false;

    // Parse JSON lines (each line is a video)
    std::wistringstream iss(output);
    std::wstring line;
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] != L'{') continue;

        YouTubeResult result;
        result.videoId = ParseJsonString(line, L"id");
        result.title = ParseJsonString(line, L"title");
        result.channel = ParseJsonString(line, L"channel");
        result.duration = ParseJsonString(line, L"duration_string");

        if (!result.videoId.empty() && !result.title.empty()) {
            results.push_back(result);
        }
    }

    return !results.empty();
}

// Public search function
bool YouTubeSearch(const std::wstring& query, std::vector<YouTubeResult>& results,
                   std::wstring& nextPageToken, const std::wstring& pageToken) {
    results.clear();
    nextPageToken.clear();

    // Try API first if available
    if (HasApiKey() && SearchWithAPI(query, results, nextPageToken, pageToken)) {
        return true;
    }

    // Fall back to yt-dlp (only for first page, no pagination support)
    if (pageToken.empty() && IsYtdlpAvailable()) {
        return SearchWithYtdlp(query, results);
    }

    return false;
}

// Get playlist contents
bool YouTubeGetPlaylistContents(const std::wstring& playlistId, std::vector<YouTubeResult>& results,
                                std::wstring& nextPageToken, const std::wstring& pageToken) {
    results.clear();
    nextPageToken.clear();

    if (!IsYtdlpAvailable()) return false;

    std::wstring url = L"https://www.youtube.com/playlist?list=" + playlistId;
    std::wstring args = L"--flat-playlist --dump-json \"" + url + L"\"";
    std::wstring output = RunYtdlp(args);
    if (output.empty()) return false;

    std::wistringstream iss(output);
    std::wstring line;
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] != L'{') continue;

        YouTubeResult result;
        result.videoId = ParseJsonString(line, L"id");
        result.title = ParseJsonString(line, L"title");
        result.channel = ParseJsonString(line, L"channel");
        result.duration = ParseJsonString(line, L"duration_string");

        if (!result.videoId.empty() && !result.title.empty()) {
            results.push_back(result);
        }
    }

    return !results.empty();
}

// Get stream URL for a video
bool YouTubeGetStreamURL(const std::wstring& videoId, std::wstring& streamUrl) {
    if (!IsYtdlpAvailable()) return false;

    // Get best audio format URL
    std::wstring url = L"https://www.youtube.com/watch?v=" + videoId;
    std::wstring args = L"-f bestaudio --get-url \"" + url + L"\"";
    std::wstring output = RunYtdlp(args);

    // Trim whitespace
    size_t start = output.find_first_not_of(L" \t\r\n");
    size_t end = output.find_last_not_of(L" \t\r\n");
    if (start != std::wstring::npos && end != std::wstring::npos) {
        streamUrl = output.substr(start, end - start + 1);
        return !streamUrl.empty() && streamUrl.find(L"http") == 0;
    }

    return false;
}

// Check if input is a YouTube URL
bool IsYouTubeURL(const std::wstring& input) {
    return input.find(L"youtube.com") != std::wstring::npos ||
           input.find(L"youtu.be") != std::wstring::npos;
}

// Parse YouTube URL
bool ParseYouTubeURL(const std::wstring& url, std::wstring& id, bool& isPlaylist, bool& isChannel) {
    isPlaylist = false;
    isChannel = false;
    id.clear();

    // Check for playlist
    size_t listPos = url.find(L"list=");
    if (listPos != std::wstring::npos) {
        size_t start = listPos + 5;
        size_t end = url.find_first_of(L"&# ", start);
        id = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        isPlaylist = true;
        return !id.empty();
    }

    // Check for channel
    if (url.find(L"/channel/") != std::wstring::npos || url.find(L"/@") != std::wstring::npos) {
        isChannel = true;
        // Extract channel ID or handle
        size_t pos = url.find(L"/channel/");
        if (pos != std::wstring::npos) {
            pos += 9;
        } else {
            pos = url.find(L"/@");
            if (pos != std::wstring::npos) pos += 2;
        }
        if (pos != std::wstring::npos) {
            size_t end = url.find_first_of(L"/?# ", pos);
            id = url.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
            return !id.empty();
        }
    }

    // Check for video ID
    size_t vPos = url.find(L"v=");
    if (vPos != std::wstring::npos) {
        size_t start = vPos + 2;
        size_t end = url.find_first_of(L"&# ", start);
        id = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        return !id.empty();
    }

    // youtu.be format
    size_t bePos = url.find(L"youtu.be/");
    if (bePos != std::wstring::npos) {
        size_t start = bePos + 9;
        size_t end = url.find_first_of(L"?# ", start);
        id = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        return !id.empty();
    }

    return false;
}

// Update results list in dialog
static void UpdateResultsList(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_YT_RESULTS);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);

    for (const auto& result : g_ytResults) {
        std::wstring display = result.title;
        if (!result.channel.empty()) {
            display += L" - " + result.channel;
        }
        if (!result.duration.empty()) {
            display += L" [" + result.duration + L"]";
        }
        SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
    }

    // Update load more button visibility
    HWND hLoadMore = GetDlgItem(hwnd, IDC_YT_LOADMORE);
    EnableWindow(hLoadMore, !g_ytNextPageToken.empty());
}

// Perform search
static void DoSearch(HWND hwnd) {
    wchar_t query[512];
    GetDlgItemTextW(hwnd, IDC_YT_SEARCH, query, 512);

    if (wcslen(query) == 0) return;

    g_ytCurrentQuery = query;
    g_ytResults.clear();
    g_ytNextPageToken.clear();
    g_ytIsPlaylistView = false;

    // Check if it's a YouTube URL
    if (IsYouTubeURL(query)) {
        std::wstring id;
        bool isPlaylist, isChannel;
        if (ParseYouTubeURL(query, id, isPlaylist, isChannel)) {
            if (isPlaylist) {
                g_ytIsPlaylistView = true;
                g_ytCurrentPlaylistId = id;
                YouTubeGetPlaylistContents(id, g_ytResults, g_ytNextPageToken, L"");
                UpdateResultsList(hwnd);
                Speak("Playlist loaded");
                return;
            } else if (!isPlaylist && !isChannel) {
                // Single video - try to play it directly
                std::wstring streamUrl;
                Speak("Loading video");
                if (YouTubeGetStreamURL(id, streamUrl)) {
                    LoadURL(streamUrl.c_str());
                    Speak("Playing");
                } else {
                    Speak("Failed to get stream URL");
                }
                return;
            }
        }
    }

    // Regular search
    Speak("Searching");
    if (YouTubeSearch(query, g_ytResults, g_ytNextPageToken, L"")) {
        UpdateResultsList(hwnd);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d results", static_cast<int>(g_ytResults.size()));
        Speak(buf);
    } else {
        Speak("No results or search failed");
    }
}

// Load more results
static void DoLoadMore(HWND hwnd) {
    if (g_ytNextPageToken.empty()) return;

    std::vector<YouTubeResult> moreResults;
    std::wstring newToken;

    Speak("Loading more");
    if (g_ytIsPlaylistView) {
        YouTubeGetPlaylistContents(g_ytCurrentPlaylistId, moreResults, newToken, g_ytNextPageToken);
    } else {
        YouTubeSearch(g_ytCurrentQuery, moreResults, newToken, g_ytNextPageToken);
    }

    g_ytNextPageToken = newToken;
    for (const auto& r : moreResults) {
        g_ytResults.push_back(r);
    }
    UpdateResultsList(hwnd);

    char buf[64];
    snprintf(buf, sizeof(buf), "%d more loaded", static_cast<int>(moreResults.size()));
    Speak(buf);
}

// Play selected result
static void PlaySelected(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_YT_RESULTS);
    int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_ytResults.size())) return;

    const YouTubeResult& result = g_ytResults[sel];
    std::wstring streamUrl;

    Speak("Loading");
    if (YouTubeGetStreamURL(result.videoId, streamUrl)) {
        LoadURL(streamUrl.c_str());
        Speak("Playing");
    } else {
        Speak("Failed to get stream URL");
    }
}

// Subclassed edit proc to handle Enter key
static WNDPROC g_origSearchProc = nullptr;
static LRESULT CALLBACK SearchEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        DoSearch(GetParent(hwnd));
        return 0;
    }
    return CallWindowProcW(g_origSearchProc, hwnd, msg, wParam, lParam);
}

// Subclassed listbox proc to handle Enter key
static WNDPROC g_origListProc = nullptr;
static LRESULT CALLBACK ResultsListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        PlaySelected(GetParent(hwnd));
        return 0;
    }
    return CallWindowProcW(g_origListProc, hwnd, msg, wParam, lParam);
}

// Dialog procedure
INT_PTR CALLBACK YouTubeDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            g_ytDialog = hwnd;
            g_ytResults.clear();
            g_ytNextPageToken.clear();
            // Subclass edit and listbox to handle Enter key
            g_origSearchProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                GetDlgItem(hwnd, IDC_YT_SEARCH), GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(SearchEditProc)));
            g_origListProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                GetDlgItem(hwnd, IDC_YT_RESULTS), GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(ResultsListProc)));
            SetFocus(GetDlgItem(hwnd, IDC_YT_SEARCH));
            return FALSE;  // We set focus manually

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_YT_RESULTS:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        PlaySelected(hwnd);
                    }
                    break;

                case IDC_YT_LOADMORE:
                    DoLoadMore(hwnd);
                    break;

                case IDCANCEL:
                    DestroyWindow(hwnd);
                    g_ytDialog = nullptr;
                    return TRUE;
            }
            break;

        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            // Resize controls
            SetWindowPos(GetDlgItem(hwnd, IDC_YT_SEARCH), nullptr, 7, 22, width - 14, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_YT_RESULTS), nullptr, 7, 54, width - 14, height - 90, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_YT_LOADMORE), nullptr, 7, height - 30, 60, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDCANCEL), nullptr, width - 57, height - 30, 50, 14, SWP_NOZORDER);
            InvalidateRect(hwnd, nullptr, TRUE);
            return TRUE;
        }

        case WM_DESTROY:
            g_ytDialog = nullptr;
            break;
    }
    return FALSE;
}

// Show YouTube dialog
void ShowYouTubeDialog(HWND parent) {
    if (g_ytDialog) {
        // Already open, bring to front
        SetForegroundWindow(g_ytDialog);
        return;
    }

    // Check if yt-dlp is available
    if (!IsYtdlpAvailable()) {
        MessageBoxW(parent, L"yt-dlp is not configured. Please set the yt-dlp path in Options > YouTube tab.",
                    L"YouTube", MB_ICONWARNING);
        return;
    }

    g_ytDialog = CreateDialogW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_YOUTUBE),
                               parent, YouTubeDlgProc);
    if (g_ytDialog) {
        ShowWindow(g_ytDialog, SW_SHOW);
    }
}

// Get YouTube dialog handle
HWND GetYouTubeDialog() {
    return g_ytDialog;
}

// Cleanup temporary files and resources
void YouTubeCleanup() {
    g_ytResults.clear();
    g_ytNextPageToken.clear();
    g_ytCurrentQuery.clear();
    g_ytCurrentPlaylistId.clear();
}
