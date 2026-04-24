#pragma once
#ifndef FASTPLAY_UPDATER_H
#define FASTPLAY_UPDATER_H

#include <windows.h>
#include <string>
#include <functional>

// Update check result
struct UpdateInfo {
    bool available;
    std::string latestVersion;
    std::string latestCommit;
    std::string downloadUrl;        // URL for portable zip
    std::string installerUrl;       // URL for installer exe
    std::string releaseNotes;
    std::string errorMessage;
};

// Check if app was installed (vs portable)
bool IsInstalledMode();

// Progress callback: (bytesDownloaded, totalBytes) -> bool (return false to cancel)
using DownloadProgressCallback = std::function<bool(size_t, size_t)>;

// Check for updates (runs synchronously, call from background thread)
UpdateInfo CheckForUpdates();

// Download and apply update (runs synchronously, call from background thread)
// Returns true if update was downloaded and is ready to apply
bool DownloadUpdate(const std::string& url, DownloadProgressCallback progressCallback);

// Apply the downloaded update (creates batch script and exits app)
void ApplyUpdate();

// Show check for updates dialog (can be called from UI thread)
void ShowCheckForUpdatesDialog(HWND hwndParent, bool silent = false);

// Check for updates on startup (runs in background thread)
void CheckForUpdatesOnStartup();

// Handle update check result (called from main window proc)
void HandleUpdateCheckResult(HWND hwnd, UpdateInfo* info, bool silent);

#endif // FASTPLAY_UPDATER_H
