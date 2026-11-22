#pragma once
#ifndef FASTPLAY_FOLDER_WATCH_H
#define FASTPLAY_FOLDER_WATCH_H

// Watching folders, and everything under them, for files and folders coming,
// going, being renamed and changing. ReadDirectoryChangesW on Windows
// (src/platform/folder_watch_windows.cpp), FSEvents on macOS
// (folder_watch_mac.cpp), inotify on Linux (folder_watch_linux.cpp); elsewhere
// nothing is watched.

#include <functional>
#include <memory>
#include <string>
#include <vector>

class FolderWatch {
public:
    // Called on the watch's own thread with the paths that changed under `root`
    // (a rename gives both names). `everything`: too much changed to say what, or
    // the system lost track; look at the whole of `root` again.
    using Handler = std::function<void(const std::wstring& root, const std::vector<std::wstring>& paths, bool everything)>;

    // Starts watching `roots`. Null if nothing can be watched here.
    static std::unique_ptr<FolderWatch> Start(const std::vector<std::wstring>& roots, Handler handler);
    virtual ~FolderWatch() = default;
};

#endif  // FASTPLAY_FOLDER_WATCH_H
