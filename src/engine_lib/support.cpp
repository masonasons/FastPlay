// What the engine's sources expect of the app around them, provided by the
// library instead: the "UI thread" its notifications are posted to (here, the
// library's event thread), the settings it reads, and where things are kept.

#include "support.h"

#include "paths.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// The event thread
// ---------------------------------------------------------------------------

namespace {

struct EventThread {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> queue;
    std::thread thread;
    bool running = false;

    void Run() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            wake.wait(lock, [this]() { return !queue.empty() || !running; });
            if (queue.empty() && !running) return;
            std::function<void()> fn = std::move(queue.front());
            queue.pop_front();
            lock.unlock();
            fn();
            lock.lock();
        }
    }
};

EventThread& Events() {
    static EventThread events;
    return events;
}

std::mutex g_dataMutex;
std::wstring g_dataDir;

}  // namespace

namespace fpe {

void StartEventThread() {
    EventThread& e = Events();
    std::lock_guard<std::mutex> lock(e.mutex);
    if (e.running) return;
    e.running = true;
    e.thread = std::thread([&e]() { e.Run(); });
}

void StopEventThread() {
    EventThread& e = Events();
    {
        std::lock_guard<std::mutex> lock(e.mutex);
        if (!e.running) return;
        e.running = false;
    }
    e.wake.notify_all();
    // Stopping from an event itself (fpe_shutdown() in a callback) cannot wait for itself
    if (e.thread.get_id() == std::this_thread::get_id()) {
        e.thread.detach();
    } else if (e.thread.joinable()) {
        e.thread.join();
    }
}

void Post(std::function<void()> fn) {
    EventThread& e = Events();
    {
        std::lock_guard<std::mutex> lock(e.mutex);
        if (!e.running) return;
        e.queue.push_back(std::move(fn));
    }
    e.wake.notify_one();
}

void Flush() {
    EventThread& e = Events();
    {
        std::lock_guard<std::mutex> lock(e.mutex);
        if (!e.running || e.thread.get_id() == std::this_thread::get_id()) return;
    }
    std::mutex doneMutex;
    std::condition_variable doneWake;
    bool done = false;
    Post([&]() {
        std::lock_guard<std::mutex> lock(doneMutex);
        done = true;
        doneWake.notify_all();
    });
    std::unique_lock<std::mutex> lock(doneMutex);
    // (bounded, in case the thread was stopped with it still queued)
    doneWake.wait_for(lock, std::chrono::seconds(10), [&]() { return done; });
}

void SetDataDirectory(const std::wstring& dir) {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    g_dataDir = dir;
    if (!g_dataDir.empty() && g_dataDir.back() != L'/' && g_dataDir.back() != L'\\') g_dataDir += kPathSeparator;
}

}  // namespace fpe

// The engine posts its notifications (the end of a track, a new stream title) here
void RunOnUiThread(std::function<void()> fn) { fpe::Post(std::move(fn)); }

// ---------------------------------------------------------------------------
// Settings the engine reads, at FastPlay's defaults
// ---------------------------------------------------------------------------

bool g_speedyNonlinear = true;
int g_ssPreset = 0;
int g_ssTonalityLimit = 0;
std::wstring g_midiSoundFont;  // none: MIDI files use the system's synth, where there is one
int g_midiMaxVoices = 128;
bool g_midiSincInterp = false;
std::wstring g_ytApiKey;       // none: YouTube searches go through yt-dlp

// ---------------------------------------------------------------------------
// Where things are
// ---------------------------------------------------------------------------

#ifdef _WIN32
const wchar_t kPathSeparator = L'\\';
#else
const wchar_t kPathSeparator = L'/';
#endif

std::wstring GetDataDirectory() {
    std::wstring dir;
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        dir = g_dataDir;
    }
    if (dir.empty()) dir = GetTempDir() + L"FastPlay Engine" + kPathSeparator;
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    return dir;
}

std::wstring GetTempDir() {
    std::error_code ec;
    std::wstring dir = fs::temp_directory_path(ec).wstring();
    if (dir.empty()) dir = L".";
    if (dir.back() != L'/' && dir.back() != L'\\') dir += kPathSeparator;
    return dir;
}

std::wstring GetExecutableDir() {
#ifdef _WIN32
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n);
    size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
#else
    return std::wstring();
#endif
}

bool IsInstalledMode() { return false; }
std::wstring GetUserMusicDir() { return std::wstring(); }
std::wstring GetUserDownloadsDir() { return std::wstring(); }
std::wstring GetLibraryDir() { return GetExecutableDir(); }
