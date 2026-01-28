#include "download_manager.h"
#include <wininet.h>

#pragma comment(lib, "wininet.lib")

// Thread parameters
struct DownloadThreadParams {
    int id;
    std::wstring url;
    std::wstring destPath;
    HWND hwndNotify;
};

// Download thread function
static DWORD WINAPI DownloadThread(LPVOID lpParam) {
    DownloadThreadParams* params = static_cast<DownloadThreadParams*>(lpParam);
    if (!params) return 1;

    bool success = false;

    // Create directory if needed
    std::wstring dir = params->destPath;
    size_t lastSlash = dir.rfind(L'\\');
    if (lastSlash != std::wstring::npos) {
        dir = dir.substr(0, lastSlash);
        CreateDirectoryW(dir.c_str(), nullptr);
    }

    HINTERNET hInternet = InternetOpenW(L"FastPlay/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (hInternet) {
        DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
        HINTERNET hUrl = InternetOpenUrlW(hInternet, params->url.c_str(), nullptr, 0, flags, 0);
        if (hUrl) {
            FILE* file = _wfopen(params->destPath.c_str(), L"wb");
            if (file) {
                char buffer[8192];
                DWORD bytesRead;
                while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
                    fwrite(buffer, 1, bytesRead, file);
                }
                fclose(file);
                success = true;
            }
            InternetCloseHandle(hUrl);
        }
        InternetCloseHandle(hInternet);
    }

    // Notify completion via window message
    if (params->hwndNotify) {
        PostMessageW(params->hwndNotify, WM_DOWNLOAD_COMPLETE, params->id, success ? 1 : 0);
    }

    delete params;
    return success ? 0 : 1;
}

DownloadManager& DownloadManager::Instance() {
    static DownloadManager instance;
    return instance;
}

DownloadManager::DownloadManager() {
    InitializeCriticalSection(&m_cs);
}

DownloadManager::~DownloadManager() {
    CancelAll();
    DeleteCriticalSection(&m_cs);
}

void DownloadManager::Enqueue(const std::wstring& url, const std::wstring& destPath, const std::wstring& title) {
    EnterCriticalSection(&m_cs);

    // Check if already queued or downloading
    for (const auto& item : m_queue) {
        if (item.url == url) {
            LeaveCriticalSection(&m_cs);
            return;
        }
    }
    for (const auto& pair : m_active) {
        if (pair.second.url == url) {
            LeaveCriticalSection(&m_cs);
            return;
        }
    }

    // Check if file already exists
    if (GetFileAttributesW(destPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        LeaveCriticalSection(&m_cs);
        return;
    }

    DownloadItem item;
    item.id = m_nextId++;
    item.url = url;
    item.destPath = destPath;
    item.title = title;
    item.thread = nullptr;

    m_queue.push_back(item);
    LeaveCriticalSection(&m_cs);

    if (onQueueChanged) onQueueChanged();
    ProcessQueue();
}

void DownloadManager::EnqueueMultiple(const std::vector<std::tuple<std::wstring, std::wstring, std::wstring>>& items) {
    EnterCriticalSection(&m_cs);

    int addedCount = 0;
    for (const auto& tuple : items) {
        const std::wstring& url = std::get<0>(tuple);
        const std::wstring& destPath = std::get<1>(tuple);
        const std::wstring& title = std::get<2>(tuple);

        // Check if already queued or downloading
        bool exists = false;
        for (const auto& item : m_queue) {
            if (item.url == url) { exists = true; break; }
        }
        for (const auto& pair : m_active) {
            if (pair.second.url == url) { exists = true; break; }
        }
        if (exists) continue;

        // Check if file already exists
        if (GetFileAttributesW(destPath.c_str()) != INVALID_FILE_ATTRIBUTES) continue;

        DownloadItem item;
        item.id = m_nextId++;
        item.url = url;
        item.destPath = destPath;
        item.title = title;
        item.thread = nullptr;

        m_queue.push_back(item);
        addedCount++;
    }

    LeaveCriticalSection(&m_cs);

    if (addedCount > 0) {
        if (onQueueChanged) onQueueChanged();
        ProcessQueue();
    }
}

void DownloadManager::CancelAll() {
    EnterCriticalSection(&m_cs);

    // Cancel active downloads
    for (auto& pair : m_active) {
        if (pair.second.thread) {
            TerminateThread(pair.second.thread, 1);
            CloseHandle(pair.second.thread);
        }
    }
    m_active.clear();

    // Clear queue
    m_queue.clear();

    LeaveCriticalSection(&m_cs);

    if (onQueueChanged) onQueueChanged();
}

int DownloadManager::PendingCount() const {
    return static_cast<int>(m_queue.size() + m_active.size());
}

int DownloadManager::ActiveCount() const {
    return static_cast<int>(m_active.size());
}

int DownloadManager::QueuedCount() const {
    return static_cast<int>(m_queue.size());
}

void DownloadManager::ProcessQueue() {
    EnterCriticalSection(&m_cs);

    while (static_cast<int>(m_active.size()) < m_maxConcurrent && !m_queue.empty()) {
        DownloadItem item = m_queue.front();
        m_queue.erase(m_queue.begin());

        StartDownload(item);
        m_active[item.id] = item;
    }

    LeaveCriticalSection(&m_cs);
}

void DownloadManager::StartDownload(DownloadItem& item) {
    DownloadThreadParams* params = new DownloadThreadParams();
    params->id = item.id;
    params->url = item.url;
    params->destPath = item.destPath;
    params->hwndNotify = m_hwndNotify;

    item.thread = CreateThread(nullptr, 0, DownloadThread, params, 0, nullptr);
    if (!item.thread) {
        delete params;
        // Notify failure
        if (m_hwndNotify) {
            PostMessageW(m_hwndNotify, WM_DOWNLOAD_COMPLETE, item.id, 0);
        }
    }
}

void DownloadManager::ProcessCompletion(int id, bool success) {
    std::wstring title;

    EnterCriticalSection(&m_cs);

    auto it = m_active.find(id);
    if (it != m_active.end()) {
        title = it->second.title;
        if (it->second.thread) {
            CloseHandle(it->second.thread);
        }
        m_active.erase(it);
    }

    bool allDone = m_active.empty() && m_queue.empty();

    LeaveCriticalSection(&m_cs);

    // Fire callbacks
    if (onDownloadComplete && !title.empty()) {
        onDownloadComplete(title, success);
    }

    if (onQueueChanged) {
        onQueueChanged();
    }

    // Process next in queue
    ProcessQueue();

    // Check if all done
    if (allDone && onAllComplete) {
        onAllComplete();
    }
}
