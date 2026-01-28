#pragma once
#ifndef FASTPLAY_DOWNLOAD_MANAGER_H
#define FASTPLAY_DOWNLOAD_MANAGER_H

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <functional>

struct DownloadItem {
    int id;
    std::wstring url;
    std::wstring destPath;
    std::wstring title;
    HANDLE thread;
};

class DownloadManager {
public:
    static DownloadManager& Instance();

    // Callbacks
    std::function<void(const std::wstring& title, bool success)> onDownloadComplete;
    std::function<void()> onAllComplete;
    std::function<void()> onQueueChanged;

    // Queue management
    void Enqueue(const std::wstring& url, const std::wstring& destPath, const std::wstring& title);
    void EnqueueMultiple(const std::vector<std::tuple<std::wstring, std::wstring, std::wstring>>& items);
    void CancelAll();

    // Status
    int PendingCount() const;
    int ActiveCount() const;
    int QueuedCount() const;

    // Called when a download completes (internal use)
    void NotifyComplete(int id, bool success);

    // Set notification window for thread-safe callbacks
    void SetNotifyWindow(HWND hwnd) { m_hwndNotify = hwnd; }
    HWND GetNotifyWindow() const { return m_hwndNotify; }

    // Process a completion message from the notification window
    void ProcessCompletion(int id, bool success);

private:
    DownloadManager();
    ~DownloadManager();
    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;

    void ProcessQueue();
    void StartDownload(DownloadItem& item);

    std::vector<DownloadItem> m_queue;
    std::map<int, DownloadItem> m_active;
    int m_nextId = 1;
    int m_maxConcurrent = 3;
    HWND m_hwndNotify = nullptr;
    CRITICAL_SECTION m_cs;

    // Batch tracking for speech feedback
    int m_batchTotal = 0;
    int m_batchSuccess = 0;
    int m_batchFailed = 0;
};

// Custom message for download completion
#define WM_DOWNLOAD_COMPLETE (WM_USER + 101)

#endif // FASTPLAY_DOWNLOAD_MANAGER_H
