// The application: startup, the single-instance handoff, and the main window.

#include "ui/main_frame.h"
#include "ui/ui_common.h"
#include "ui/dialogs.h"

#include "globals.h"
#include "settings.h"
#include "hotkeys.h"
#include "youtube.h"
#include "file_assoc.h"
#include "playlist_io.h"
#include "platform.h"
#include "app_ui.h"

#include <wx/ipc.h>
#include <wx/snglinst.h>
#include <wx/filename.h>
#ifdef __WXGTK__
#include <unistd.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif
#include <chrono>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

// Files are handed from a newly started FastPlay to the one already running.
#ifdef __WXGTK__
// On Unix wxWidgets takes a name with a slash in it as a socket's path, and any
// other as a TCP service; this socket is in the user's own runtime folder.
wxString IpcService() {
    wxString dir = wxGetenv("XDG_RUNTIME_DIR");
    if (dir.empty()) return wxString::Format("/tmp/FastPlay-%lu.ipc", static_cast<unsigned long>(getuid()));
    return dir + "/FastPlay.ipc";
}
#else
wxString IpcService() {
    return "FastPlay";
}
#endif
const char* kIpcTopic = "files";
// Sent instead when it was started without files (from the taskbar, a shortcut,
// Windows+number): the running one shows its window, hidden to the tray or not.
// No file is called this ('<' and '>' can't be in a Windows path).
const char* kIpcShow = "<show>";

// Receives the files, one Execute() per path, or kIpcShow.
class FileConnection : public wxConnection {
public:
    bool OnExecute(const wxString&, const void* data, size_t size, wxIPCFormat format) override {
        wxString text = GetTextFromData(data, size, format);
        // Handle it after the call returns, so the sender is not kept waiting on playback.
        if (text == kIpcShow) {
            RunOnUiThread([]() {
                if (MainFrame* frame = GetMainFrame()) frame->RestoreFromTray();
            });
            return true;
        }
        std::wstring path = WS(text);
        RunOnUiThread([path]() {
            if (MainFrame* frame = GetMainFrame()) frame->ReceiveFile(path);
        });
        return true;
    }
};

class FileServer : public wxServer {
public:
    wxConnectionBase* OnAcceptConnection(const wxString& topic) override {
        return topic == kIpcTopic ? new FileConnection() : nullptr;
    }
};

// wxWidgets reports problems it considers worth a message box (a hotkey another
// program already owns, for instance). FastPlay has always carried on silently.
class SilentLog : public wxLog {
protected:
    void DoLogText(const wxString&) override {}
};

}  // namespace

class FastPlayApp : public wxApp {
public:
    bool OnInit() override;
    int OnExit() override;
#ifdef __WXOSX__
    // macOS keeps one FastPlay running and opens files in it (Finder, the Dock,
    // "open"), including the files FastPlay is launched with.
    void MacOpenFiles(const wxArrayString& fileNames) override;
    // Clicking the Dock icon brings back a window hidden to the menu bar.
    void MacReopenApp() override;
#endif

private:
    std::vector<std::wstring> ExistingPathArgs() const;
    bool HandOverToRunningInstance(const std::vector<std::wstring>& files);

    std::unique_ptr<wxSingleInstanceChecker> m_instance;
    std::unique_ptr<FileServer> m_server;
};

std::vector<std::wstring> FastPlayApp::ExistingPathArgs() const {
    std::vector<std::wstring> files;
    for (int i = 1; i < argc; i++) {
        wxString arg = argv[i];
        if (wxFileName::Exists(arg)) files.push_back(WS(arg));
    }
    return files;
}

bool FastPlayApp::HandOverToRunningInstance(const std::vector<std::wstring>& files) {
    wxClient client;
    std::unique_ptr<wxConnectionBase> connection(client.MakeConnection("localhost", IpcService(), kIpcTopic));
    if (!connection) return false;
#ifdef _WIN32
    // Windows lets only the program the user just started come to the front;
    // this one was, so it passes that on to the running one, which would
    // otherwise only flash on the taskbar when it shows its window
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    for (const auto& file : files) {
        connection->Execute(WX(file));
    }
    if (files.empty()) connection->Execute(wxString(kIpcShow));
    connection->Disconnect();
    return true;
}

bool FastPlayApp::OnInit() {
    wxDISABLE_ASSERTS_IN_RELEASE_BUILD();
    delete wxLog::SetActiveTarget(new SilentLog());
    SetAppName(APP_NAME);

    // Seed the RNG so shuffle order differs between runs.
    srand(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));

    // Must run before any audio library DLL loads.
    PlatformStartup();

    // Single instance:
    // - multiple instances not allowed: always hand over to the running one (the
    //   files, or without any, a request that it show its window)
    // - multiple instances allowed and started with files: hand the files over
    // - multiple instances allowed and started without files: start a new one
    std::vector<std::wstring> files = ExistingPathArgs();
#ifndef __WXOSX__
    bool useSingleInstance = !ReadAllowMultipleInstances() || !files.empty();

    m_instance = std::make_unique<wxSingleInstanceChecker>();
    m_instance->Create(MUTEX_NAME);
    bool anotherRunning = m_instance->IsAnotherRunning();
    if (anotherRunning && useSingleInstance && HandOverToRunningInstance(files)) {
        return false;
    }
    // Every running FastPlay accepts files, as any FastPlay window used to, so a
    // handoff still works after the first of several instances has closed.
    m_server = std::make_unique<FileServer>();
    m_server->Create(IpcService());
#endif

    LoadSettings();
    if (g_registerFileTypes) {
        RegisterAllFileTypes();
    }
    LoadHotkeys();
    YouTubeCleanup();  // Clean up any leftover temp files from previous sessions

    // Files FastPlay was started with become the playlist.
    for (const auto& path : files) {
        if (IsPlaylistFile(path)) {
            auto entries = ParsePlaylist(path);
            g_playlist.insert(g_playlist.end(), entries.begin(), entries.end());
        } else {
            g_playlist.push_back(path);
        }
    }

    auto* frame = new MainFrame();
    if (!frame->Initialize()) {
        frame->Destroy();
        return false;
    }
    SetTopWindow(frame);
    frame->Show();
    StartYouTubeAutoRefresh(true);

    if (g_playlist.empty()) {
        LoadPlaybackState();
    }
    return true;
}

#ifdef __WXOSX__
void FastPlayApp::MacOpenFiles(const wxArrayString& fileNames) {
    MainFrame* frame = GetMainFrame();
    if (!frame) return;
    for (const auto& name : fileNames) {
        frame->ReceiveFile(WS(name));
    }
}

void FastPlayApp::MacReopenApp() {
    if (MainFrame* frame = GetMainFrame()) frame->RestoreFromTray();
}
#endif

int FastPlayApp::OnExit() {
    m_server.reset();
    m_instance.reset();
    return wxApp::OnExit();
}

wxIMPLEMENT_APP(FastPlayApp);
