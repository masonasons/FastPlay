// Updates, shared by every system: finding the latest build on GitHub and
// downloading it. Installing it (ApplyUpdate) is per system, in
// src/platform/updater_*.cpp.

#include "updater.h"
#include "updater_internal.h"
#include "version.h"
#include "globals.h"
#include "http.h"
#include "app_ui.h"
#include "utils.h"

#include <chrono>
#include <regex>
#include <thread>

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
// Convert string to lowercase
static std::string ToLower(const std::string& str) {
    std::string result = str;
    for (char& c : result) {
        c = (char)tolower((unsigned char)c);
    }
    return result;
}

static bool Contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

// Download URLs for this system
struct ReleaseAssets {
    std::string zipUrl;
    std::string installerUrl;
};

// Find this system's downloads in a release (a zip, and on Windows the installer)
static ReleaseAssets FindAssets(const std::string& releaseJson) {
    ReleaseAssets assets;

    size_t assetsPos = releaseJson.find("\"assets\"");
    if (assetsPos == std::string::npos) return assets;

    size_t arrayStart = releaseJson.find('[', assetsPos);
    if (arrayStart == std::string::npos) return assets;

    int depth = 1;
    size_t arrayEnd = arrayStart + 1;
    while (arrayEnd < releaseJson.length() && depth > 0) {
        if (releaseJson[arrayEnd] == '[') depth++;
        else if (releaseJson[arrayEnd] == ']') depth--;
        arrayEnd++;
    }

    std::string assetsArray = releaseJson.substr(arrayStart, arrayEnd - arrayStart);
    std::string fallbackZipUrl;

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
        pos = objEnd;

        bool isMac = Contains(nameLower, "macos") || Contains(nameLower, "darwin") ||
                     Contains(nameLower, "mac-") || Contains(nameLower, "-mac");
        bool isLinux = Contains(nameLower, "linux");
#ifdef __APPLE__
        // The macOS app, zipped
        if (isMac && Contains(nameLower, ".zip")) assets.zipUrl = url;
#else
        // Skip non-Windows platforms
        if (isMac || isLinux) continue;

        // Installer exe (Setup.exe, Installer.exe, etc.)
        if ((Contains(nameLower, "setup") || Contains(nameLower, "installer")) && Contains(nameLower, ".exe")) {
            assets.installerUrl = url;
        }
        // Zip file
        else if (Contains(nameLower, ".zip")) {
            if (Contains(nameLower, "windows") || Contains(nameLower, "win64") || Contains(nameLower, "win32") ||
                Contains(nameLower, "win-") || Contains(nameLower, "-win")) {
                assets.zipUrl = url;
            } else if (fallbackZipUrl.empty()) {
                fallbackZipUrl = url;
            }
        }
#endif
    }

    if (assets.zipUrl.empty() && !fallbackZipUrl.empty()) {
        assets.zipUrl = fallbackZipUrl;
    }

    return assets;
}

UpdateInfo CheckForUpdates() {
    UpdateInfo info = {false, "", "", "", "", "", ""};

    // Fetch releases from GitHub API
    HttpOptions options;
    options.headers = {L"Accept: application/vnd.github.v3+json"};
    HttpResult result = HttpGet(Utf8ToWide(GITHUB_API_URL), options);
    const std::string& response = result.body;

    if (result.completed && result.status == 404) {
        info.errorMessage = "No FastPlay release found.";
        return info;
    }
    if (!result.completed || result.status != 200 || response.empty()) {
        info.errorMessage = "Failed to connect to GitHub. Please check your internet connection.";
        return info;
    }

    // The release itself (one object, not a list)
    const std::string& release = response;

    std::string body = ExtractJsonString(release, "body");
    std::string tagName = ExtractJsonString(release, "tag_name");

    // Look for commit SHA in body: "commit XXXX"
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

    ReleaseAssets assets = FindAssets(release);
    if (assets.zipUrl.empty() && assets.installerUrl.empty()) {
#ifdef __APPLE__
        info.errorMessage = "No macOS download available for this release.";
#else
        info.errorMessage = "No Windows download available for this release.";
#endif
        return info;
    }

    info.downloadUrl = assets.zipUrl;
    info.installerUrl = assets.installerUrl;
    info.releaseNotes = body;

    // Compare commits if both are available, otherwise fall back to version strings
    std::string localCommit = BUILD_COMMIT;
    std::string localVersion = APP_VERSION;

    if (!info.latestCommit.empty() && !localCommit.empty()) {
        std::string latestShort = info.latestCommit.substr(0, 7);
        std::string localShort = localCommit.substr(0, 7);
        info.available = (latestShort != localShort);
    } else {
        info.available = (info.latestVersion != localVersion);
    }

    return info;
}

// Track whether we're updating with installer or zip
static bool g_updateWithInstaller = false;

bool UpdateWithInstaller() {
    return g_updateWithInstaller;
}

bool DownloadUpdate(const std::string& url, DownloadProgressCallback progressCallback) {
    std::string urlLower = ToLower(url);
    g_updateWithInstaller = (Contains(urlLower, "setup") || Contains(urlLower, "installer")) && Contains(urlLower, ".exe");
    std::wstring destPath = g_updateWithInstaller ? UpdateInstallerPath() : UpdateZipPath();

    HttpOptions options;
    options.saveTo = destPath;
    options.timeoutMs = 60000;
    if (progressCallback) {
        options.progress = [&progressCallback](uint64_t received, uint64_t total) {
            return progressCallback(static_cast<size_t>(received), static_cast<size_t>(total));
        };
    }
    HttpResult result = HttpGet(Utf8ToWide(url), options);
    return result.completed && !result.cancelled && result.status == 200 && result.bytesReceived > 0;
}

// Check for updates on startup: after a short delay, a silent check that only
// speaks up when an update is available.
void CheckForUpdatesOnStartup() {
    if (!g_checkForUpdates) return;

    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        RunOnUiThread([]() { ShowCheckForUpdatesDialog(true); });
    }).detach();
}
