#include "updater.h"
#include "version.h"
#include "globals.h"
#include "accessibility.h"
#include "resource.h"
#include <winhttp.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <regex>

#pragma comment(lib, "winhttp.lib")

// Simple JSON value extraction (no external library needed)
static std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";

    size_t colonPos = json.find(':', keyPos + searchKey.length());
    if (colonPos == std::string::npos) return "";

    size_t startQuote = json.find('"', colonPos + 1);
    if (startQuote == std::string::npos) return "";

    size_t endQuote = startQuote + 1;
    while (endQuote < json.length()) {
        if (json[endQuote] == '"' && json[endQuote - 1] != '\\') break;
        endQuote++;
    }

    return json.substr(startQuote + 1, endQuote - startQuote - 1);
}

// Extract first object from JSON array
static std::string ExtractFirstArrayObject(const std::string& json) {
    size_t start = json.find('[');
    if (start == std::string::npos) return "";

    size_t objStart = json.find('{', start);
    if (objStart == std::string::npos) return "";

    int depth = 1;
    size_t objEnd = objStart + 1;
    while (objEnd < json.length() && depth > 0) {
        if (json[objEnd] == '{') depth++;
        else if (json[objEnd] == '}') depth--;
        objEnd++;
    }

    return json.substr(objStart, objEnd - objStart);
}

// Convert string to lowercase
static std::string ToLower(const std::string& str) {
    std::string result = str;
    for (char& c : result) {
        c = (char)tolower((unsigned char)c);
    }
    return result;
}

// Asset URLs for Windows
struct WindowsAssets {
    std::string zipUrl;
    std::string installerUrl;
};

// Find Windows assets in release (both zip and installer)
static WindowsAssets FindWindowsAssets(const std::string& releaseJson) {
    WindowsAssets assets;

    // Look for assets array
    size_t assetsPos = releaseJson.find("\"assets\"");
    if (assetsPos == std::string::npos) return assets;

    size_t arrayStart = releaseJson.find('[', assetsPos);
    if (arrayStart == std::string::npos) return assets;

    // Find matching bracket
    int depth = 1;
    size_t arrayEnd = arrayStart + 1;
    while (arrayEnd < releaseJson.length() && depth > 0) {
        if (releaseJson[arrayEnd] == '[') depth++;
        else if (releaseJson[arrayEnd] == ']') depth--;
        arrayEnd++;
    }

    std::string assetsArray = releaseJson.substr(arrayStart, arrayEnd - arrayStart);

    std::string fallbackZipUrl;

    // Find each asset object
    size_t pos = 0;
    while (pos < assetsArray.length()) {
        size_t objStart = assetsArray.find('{', pos);
        if (objStart == std::string::npos) break;

        int objDepth = 1;
        size_t objEnd = objStart + 1;
        while (objEnd < assetsArray.length() && objDepth > 0) {
            if (assetsArray[objEnd] == '{') objDepth++;
            else if (assetsArray[objEnd] == '}') objDepth--;
            objEnd++;
        }

        std::string asset = assetsArray.substr(objStart, objEnd - objStart);
        std::string name = ExtractJsonString(asset, "name");
        std::string nameLower = ToLower(name);
        std::string url = ExtractJsonString(asset, "browser_download_url");

        // Skip non-Windows platforms
        if (nameLower.find("linux") != std::string::npos ||
            nameLower.find("macos") != std::string::npos ||
            nameLower.find("darwin") != std::string::npos ||
            nameLower.find("mac-") != std::string::npos ||
            nameLower.find("-mac") != std::string::npos) {
            pos = objEnd;
            continue;
        }

        // Check for installer exe (Setup.exe, Installer.exe, etc.)
        if ((nameLower.find("setup") != std::string::npos ||
             nameLower.find("installer") != std::string::npos) &&
            nameLower.find(".exe") != std::string::npos) {
            assets.installerUrl = url;
        }
        // Check for zip file
        else if (nameLower.find(".zip") != std::string::npos) {
            // Prefer Windows-specific zip
            if (nameLower.find("windows") != std::string::npos ||
                nameLower.find("win64") != std::string::npos ||
                nameLower.find("win32") != std::string::npos ||
                nameLower.find("win-") != std::string::npos ||
                nameLower.find("-win") != std::string::npos) {
                assets.zipUrl = url;
            } else if (fallbackZipUrl.empty()) {
                fallbackZipUrl = url;
            }
        }

        pos = objEnd;
    }

    // Use fallback zip if no Windows-specific one found
    if (assets.zipUrl.empty() && !fallbackZipUrl.empty()) {
        assets.zipUrl = fallbackZipUrl;
    }

    return assets;
}

// HTTP GET request using WinHTTP
static std::string HttpGet(const std::wstring& host, const std::wstring& path, bool https = true) {
    std::string result;

    HINTERNET hSession = WinHttpOpen(L"FastPlay/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession) return "";

    // Enable TLS 1.2 (required for GitHub API)
    DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
        https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);

    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        https ? WINHTTP_FLAG_SECURE : 0);

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Add GitHub API headers
    WinHttpAddRequestHeaders(hRequest,
        L"Accept: application/vnd.github.v3+json\r\nUser-Agent: FastPlay/1.0",
        -1, WINHTTP_ADDREQ_FLAG_ADD);

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {

        DWORD bytesAvailable;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            std::vector<char> buffer(bytesAvailable + 1);
            DWORD bytesRead;
            if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
                buffer[bytesRead] = 0;
                result += buffer.data();
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return result;
}

// Download file with progress callback
static bool HttpDownload(const std::string& url, const std::wstring& destPath,
                         DownloadProgressCallback progressCallback) {
    // Parse URL
    std::wstring wurl(url.begin(), url.end());
    URL_COMPONENTS urlComp = {0};
    urlComp.dwStructSize = sizeof(urlComp);

    wchar_t hostName[256] = {0};
    wchar_t urlPath[2048] = {0};
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &urlComp)) {
        return false;
    }

    bool https = (urlComp.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hSession = WinHttpOpen(L"FastPlay/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, hostName,
        urlComp.nPort, 0);

    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        https ? WINHTTP_FLAG_SECURE : 0);

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    bool success = false;

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {

        // Check for redirect
        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);

        if (statusCode >= 300 && statusCode < 400) {
            // Handle redirect
            wchar_t redirectUrl[2048] = {0};
            DWORD redirectUrlSize = sizeof(redirectUrl);
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                    WINHTTP_HEADER_NAME_BY_INDEX, redirectUrl, &redirectUrlSize, WINHTTP_NO_HEADER_INDEX)) {
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);

                // Convert to narrow string and recurse
                std::wstring wRedirect(redirectUrl);
                std::string redirect(wRedirect.begin(), wRedirect.end());
                return HttpDownload(redirect, destPath, progressCallback);
            }
        }

        // Get content length
        DWORD contentLength = 0;
        DWORD contentLengthSize = sizeof(contentLength);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &contentLengthSize, WINHTTP_NO_HEADER_INDEX);

        // Open output file
        std::ofstream outFile(destPath, std::ios::binary);
        if (!outFile) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        size_t totalDownloaded = 0;
        DWORD bytesAvailable;
        std::vector<char> buffer(65536); // 64KB buffer

        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            DWORD toRead = (bytesAvailable < (DWORD)buffer.size()) ? bytesAvailable : (DWORD)buffer.size();
            DWORD bytesRead;
            if (WinHttpReadData(hRequest, buffer.data(), toRead, &bytesRead)) {
                outFile.write(buffer.data(), bytesRead);
                totalDownloaded += bytesRead;

                if (progressCallback) {
                    if (!progressCallback(totalDownloaded, contentLength)) {
                        // Cancelled
                        outFile.close();
                        DeleteFileW(destPath.c_str());
                        WinHttpCloseHandle(hRequest);
                        WinHttpCloseHandle(hConnect);
                        WinHttpCloseHandle(hSession);
                        return false;
                    }
                }
            }
        }

        outFile.close();
        success = (totalDownloaded > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return success;
}

// Get path to downloaded update zip
static std::wstring GetUpdateZipPath() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    return std::wstring(tempPath) + L"FastPlay-update.zip";
}

// Get path to app directory
static std::wstring GetAppDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring appPath(path);
    size_t lastSlash = appPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        return appPath.substr(0, lastSlash);
    }
    return L".";
}

// Check if app was installed (vs portable) by looking for installed.txt marker
bool IsInstalledMode() {
    std::wstring appDir = GetAppDirectory();
    std::wstring markerPath = appDir + L"\\installed.txt";
    DWORD attrs = GetFileAttributesW(markerPath.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES);
}

// Get path to downloaded installer
static std::wstring GetUpdateInstallerPath() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    return std::wstring(tempPath) + L"FastPlay-Setup.exe";
}

UpdateInfo CheckForUpdates() {
    UpdateInfo info = {false, "", "", "", "", "", ""};

    // Fetch releases from GitHub API
    std::string response = HttpGet(L"api.github.com", L"/repos/masonasons/FastPlay/releases");

    if (response.empty()) {
        info.errorMessage = "Failed to connect to GitHub. Please check your internet connection.";
        return info;
    }

    // Get first (latest) release
    std::string release = ExtractFirstArrayObject(response);
    if (release.empty()) {
        info.errorMessage = "No releases found.";
        return info;
    }

    // Extract version info from release body
    std::string body = ExtractJsonString(release, "body");
    std::string tagName = ExtractJsonString(release, "tag_name");

    // Look for commit SHA in body: "Automated build from commit XXXX"
    std::regex commitRegex("commit ([a-f0-9]+)");
    std::smatch commitMatch;
    if (std::regex_search(body, commitMatch, commitRegex)) {
        info.latestCommit = commitMatch[1].str();
    }

    // Look for version in body: "**Version:** X.Y.Z"
    std::regex versionRegex("\\*\\*Version:\\*\\* ([0-9.]+)");
    std::smatch versionMatch;
    if (std::regex_search(body, versionMatch, versionRegex)) {
        info.latestVersion = versionMatch[1].str();
    } else {
        info.latestVersion = tagName;
    }

    // Find Windows assets (both zip and installer)
    WindowsAssets assets = FindWindowsAssets(release);
    if (assets.zipUrl.empty() && assets.installerUrl.empty()) {
        info.errorMessage = "No Windows download available for this release.";
        return info;
    }

    info.downloadUrl = assets.zipUrl;
    info.installerUrl = assets.installerUrl;
    info.releaseNotes = body;

    // Compare versions
    std::string localCommit = BUILD_COMMIT;
    std::string localVersion = APP_VERSION;

    // If we have commit info, compare commits
    if (!info.latestCommit.empty() && !localCommit.empty()) {
        // Compare first 7 characters of commit SHA
        std::string latestShort = info.latestCommit.substr(0, 7);
        std::string localShort = localCommit.substr(0, 7);
        info.available = (latestShort != localShort);
    } else {
        // Fall back to version comparison
        info.available = (info.latestVersion != localVersion);
    }

    return info;
}

// Track whether we're updating with installer or zip
static bool g_updateWithInstaller = false;

bool DownloadUpdate(const std::string& url, DownloadProgressCallback progressCallback) {
    // Determine destination based on file type
    std::wstring destPath;
    std::string urlLower = ToLower(url);
    if ((urlLower.find("setup") != std::string::npos || urlLower.find("installer") != std::string::npos) &&
        urlLower.find(".exe") != std::string::npos) {
        destPath = GetUpdateInstallerPath();
        g_updateWithInstaller = true;
    } else {
        destPath = GetUpdateZipPath();
        g_updateWithInstaller = false;
    }
    return HttpDownload(url, destPath, progressCallback);
}

void ApplyUpdate() {
    // Debug: show what we're about to do
    std::wstring debugMsg = L"ApplyUpdate called.\n";
    debugMsg += L"Mode: ";
    debugMsg += g_updateWithInstaller ? L"Installer" : L"Portable (zip)";
    debugMsg += L"\nPath: ";
    debugMsg += g_updateWithInstaller ? GetUpdateInstallerPath() : GetUpdateZipPath();
    MessageBoxW(g_hwnd, debugMsg.c_str(), L"Update Debug", MB_OK);

    if (g_updateWithInstaller) {
        // Run installer in silent mode
        std::wstring installerPath = GetUpdateInstallerPath();

        // Verify installer exists
        if (GetFileAttributesW(installerPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            MessageBoxW(g_hwnd, L"Update file not found. The download may have failed.",
                L"Update Error", MB_OK | MB_ICONERROR);
            return;
        }

        // Run installer with /SILENT flag (Inno Setup)
        // /SILENT shows progress but no prompts
        // /VERYSILENT hides everything
        HINSTANCE result = ShellExecuteW(NULL, L"open", installerPath.c_str(), L"/SILENT", NULL, SW_SHOWNORMAL);
        if (reinterpret_cast<intptr_t>(result) <= 32) {
            MessageBoxW(g_hwnd, L"Failed to launch installer.",
                L"Update Error", MB_OK | MB_ICONERROR);
            return;
        }
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    } else {
        // Portable mode: extract zip with batch script
        std::wstring appDir = GetAppDirectory();
        std::wstring zipPath = GetUpdateZipPath();
        std::wstring batchPath = appDir + L"\\update.bat";
        std::wstring extractDir = appDir + L"\\update_temp";

        // Verify zip exists
        if (GetFileAttributesW(zipPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            MessageBoxW(g_hwnd, L"Update file not found. The download may have failed.",
                L"Update Error", MB_OK | MB_ICONERROR);
            return;
        }

        // Get exe name
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        std::wstring exeName(exePath);
        size_t lastSlash = exeName.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            exeName = exeName.substr(lastSlash + 1);
        }

        // Create batch script
        std::ofstream batch(batchPath);
        batch << "@echo off\r\n";
        batch << "echo Updating FastPlay...\r\n";
        batch << "timeout /t 2 /nobreak > nul\r\n";
        batch << "powershell -Command \"Expand-Archive -Path '" << std::string(zipPath.begin(), zipPath.end()) << "' -DestinationPath '" << std::string(extractDir.begin(), extractDir.end()) << "' -Force\"\r\n";
        batch << "xcopy /s /y /q \"" << std::string(extractDir.begin(), extractDir.end()) << "\\*\" \"" << std::string(appDir.begin(), appDir.end()) << "\\\"\r\n";
        batch << "rmdir /s /q \"" << std::string(extractDir.begin(), extractDir.end()) << "\"\r\n";
        batch << "del \"" << std::string(zipPath.begin(), zipPath.end()) << "\"\r\n";
        batch << "start \"\" \"" << std::string(appDir.begin(), appDir.end()) << "\\" << std::string(exeName.begin(), exeName.end()) << "\"\r\n";
        batch << "del \"%~f0\"\r\n";
        batch.close();

        // Run batch script and exit
        HINSTANCE result = ShellExecuteW(NULL, L"open", batchPath.c_str(), NULL, appDir.c_str(), SW_HIDE);
        if (reinterpret_cast<intptr_t>(result) <= 32) {
            MessageBoxW(g_hwnd, L"Failed to launch update script.",
                L"Update Error", MB_OK | MB_ICONERROR);
            return;
        }
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
}

// Progress dialog data
struct ProgressDialogData {
    HWND hwndDialog;
    HWND hwndProgress;
    HWND hwndText;
    bool cancelled;
    size_t totalBytes;
    size_t downloadedBytes;
};

static ProgressDialogData* g_progressData = nullptr;

static INT_PTR CALLBACK ProgressDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            g_progressData->hwndDialog = hwnd;
            g_progressData->hwndProgress = GetDlgItem(hwnd, IDC_PROGRESS_BAR);
            g_progressData->hwndText = GetDlgItem(hwnd, IDC_PROGRESS_TEXT);
            SendMessageW(g_progressData->hwndProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            return TRUE;

        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL) {
                g_progressData->cancelled = true;
                return TRUE;
            }
            break;

        case WM_USER + 100: // Update progress
            if (g_progressData) {
                int percent = (g_progressData->totalBytes > 0)
                    ? (int)((g_progressData->downloadedBytes * 100) / g_progressData->totalBytes)
                    : 0;
                SendMessageW(g_progressData->hwndProgress, PBM_SETPOS, percent, 0);

                wchar_t text[256];
                double downloadedMB = g_progressData->downloadedBytes / (1024.0 * 1024.0);
                double totalMB = g_progressData->totalBytes / (1024.0 * 1024.0);
                swprintf(text, 256, L"Downloading: %.1f MB / %.1f MB (%d%%)",
                    downloadedMB, totalMB, percent);
                SetWindowTextW(g_progressData->hwndText, text);
            }
            return TRUE;

        case WM_USER + 101: // Download complete
            DestroyWindow(hwnd);
            return TRUE;

        case WM_USER + 102: // Download failed
            DestroyWindow(hwnd);
            return TRUE;
    }
    return FALSE;
}

void ShowCheckForUpdatesDialog(HWND hwndParent, bool silent) {
    // Run check in background thread
    std::thread([hwndParent, silent]() {
        UpdateInfo info = CheckForUpdates();

        // Post result to UI thread
        PostMessageW(hwndParent, WM_USER + 200, 0,
            reinterpret_cast<LPARAM>(new std::pair<UpdateInfo, bool>(info, silent)));
    }).detach();
}

void CheckForUpdatesOnStartup() {
    if (!g_checkForUpdates) return;

    // Delay 3 seconds to let UI initialize
    std::thread([]() {
        Sleep(3000);
        if (g_hwnd) {
            ShowCheckForUpdatesDialog(g_hwnd, true);
        }
    }).detach();
}

// Handle update check result in main window (call from WndProc)
void HandleUpdateCheckResult(HWND hwnd, UpdateInfo* info, bool silent) {
    if (!info->errorMessage.empty()) {
        if (!silent) {
            MessageBoxA(hwnd, info->errorMessage.c_str(), "Check for Updates", MB_OK | MB_ICONERROR);
        }
        return;
    }

    if (!info->available) {
        if (!silent) {
            Speak("No updates available. You are running the latest version.");
            MessageBoxA(hwnd, "No updates available. You are running the latest version.",
                "Check for Updates", MB_OK | MB_ICONINFORMATION);
        }
        return;
    }

    // Update available - ask user
    std::string message = "A new version of FastPlay is available!\n\n";
    message += "Current version: " + std::string(APP_VERSION);
    if (strlen(BUILD_COMMIT) > 0) {
        message += " (" + std::string(BUILD_COMMIT).substr(0, 7) + ")";
    }
    message += "\nLatest version: " + info->latestVersion;
    if (!info->latestCommit.empty()) {
        message += " (" + info->latestCommit.substr(0, 7) + ")";
    }
    message += "\n\nDo you want to download and install the update?";

    Speak("Update available. " + info->latestVersion);

    if (MessageBoxA(hwnd, message.c_str(), "Update Available", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        // Create progress dialog dynamically
        HWND hwndProgress = CreateDialogW(GetModuleHandle(NULL),
            MAKEINTRESOURCEW(IDD_PROGRESS), hwnd, ProgressDlgProc);

        if (!hwndProgress) {
            // Fallback: create simple dialog
            MessageBoxA(hwnd, "Starting download...", "Update", MB_OK);
        }

        ProgressDialogData progressData = {0};
        progressData.cancelled = false;
        g_progressData = &progressData;

        if (hwndProgress) {
            ShowWindow(hwndProgress, SW_SHOW);
        }

        // Choose download URL: use installer if installed, otherwise use zip
        std::string downloadUrl;
        if (IsInstalledMode() && !info->installerUrl.empty()) {
            downloadUrl = info->installerUrl;
        } else if (!info->downloadUrl.empty()) {
            downloadUrl = info->downloadUrl;
        } else if (!info->installerUrl.empty()) {
            // Fallback to installer if no zip available
            downloadUrl = info->installerUrl;
        }

        std::thread([hwnd, hwndProgress, downloadUrl]() {
            bool wasCancelled = false;
            bool success = DownloadUpdate(downloadUrl, [hwndProgress, &wasCancelled](size_t downloaded, size_t total) {
                if (g_progressData) {
                    g_progressData->downloadedBytes = downloaded;
                    g_progressData->totalBytes = total;
                    if (hwndProgress) {
                        PostMessageW(hwndProgress, WM_USER + 100, 0, 0);
                    }
                    wasCancelled = g_progressData->cancelled;
                    return !wasCancelled;
                }
                return true;
            });

            // Capture cancelled state before destroying dialog (which nulls g_progressData)
            if (g_progressData) {
                wasCancelled = g_progressData->cancelled;
            }

            if (hwndProgress) {
                PostMessageW(hwndProgress, success ? WM_USER + 101 : WM_USER + 102, 0, 0);
            }

            // Small delay to let dialog close before showing results
            Sleep(100);

            if (success && !wasCancelled) {
                // Apply update
                PostMessageW(hwnd, WM_USER + 201, 0, 0);
            } else if (!success && !wasCancelled) {
                MessageBoxA(hwnd, "Failed to download update.", "Error", MB_OK | MB_ICONERROR);
            }
        }).detach();

        if (hwndProgress) {
            // Run dialog message loop
            MSG msg;
            while (GetMessageW(&msg, NULL, 0, 0)) {
                if (!IsDialogMessageW(hwndProgress, &msg)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                if (!IsWindow(hwndProgress)) break;
            }
        }

        g_progressData = nullptr;
    }
}
