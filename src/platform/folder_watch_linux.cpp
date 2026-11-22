// Watching folders on Linux: inotify, with a watch on every folder under each
// root (inotify watches one folder, not what is under it), on a thread of its own.
// Folders that come later are watched as they come.

#include "folder_watch.h"
#include "utils.h"

#include <dirent.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <thread>

namespace {

constexpr uint32_t kEvents = IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                             IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW;

class LinuxFolderWatch : public FolderWatch {
public:
    LinuxFolderWatch(const std::vector<std::wstring>& roots, Handler handler) : m_handler(std::move(handler)) {
        m_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        m_stop = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (m_inotify < 0 || m_stop < 0) return;
        for (const std::wstring& root : roots) {
            std::string utf8 = WideToUtf8(root);
            while (utf8.size() > 1 && utf8.back() == '/') utf8.pop_back();
            m_roots.push_back(utf8);
            WatchTree(m_roots.size() - 1, utf8);
        }
        m_thread = std::thread([this] { Run(); });
    }

    ~LinuxFolderWatch() override {
        if (m_thread.joinable()) {
            uint64_t one = 1;
            if (write(m_stop, &one, sizeof(one)) < 0) {}
            m_thread.join();
        }
        if (m_inotify >= 0) close(m_inotify);
        if (m_stop >= 0) close(m_stop);
    }

    bool Ok() const { return m_thread.joinable(); }

private:
    struct Folder {
        size_t root;
        std::string path;
        ino_t inode;
    };

    // A watch on `dir` and on every folder under it (not following links, which
    // could lead round in a circle)
    void WatchTree(size_t root, const std::string& dir) {
        struct stat info;
        if (lstat(dir.c_str(), &info) != 0) return;
        // (A folder watched already, moved here, keeps its watch, with this path now)
        int wd = inotify_add_watch(m_inotify, dir.c_str(), kEvents);
        if (wd < 0) return;  // gone, unreadable, or the system's limit of watches reached
        m_folders[wd] = Folder{root, dir, info.st_ino};
        DIR* listing = opendir(dir.c_str());
        if (!listing) return;
        while (dirent* entry = readdir(listing)) {
            const char* name = entry->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
            std::string path = dir + "/" + name;
            bool folder = entry->d_type == DT_DIR;
            if (entry->d_type == DT_UNKNOWN) {
                struct stat info;
                folder = lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
            }
            if (folder) WatchTree(root, path);
        }
        closedir(listing);
    }

    void Run() {
        pollfd fds[2] = {{m_inotify, POLLIN, 0}, {m_stop, POLLIN, 0}};
        alignas(inotify_event) char buffer[64 * 1024];
        for (;;) {
            if (poll(fds, 2, -1) < 0) continue;
            if (fds[1].revents) return;
            if (!(fds[0].revents & POLLIN)) continue;

            // By root: the paths under it, and whether it must be looked at whole
            std::vector<std::vector<std::wstring>> changed(m_roots.size());
            std::vector<bool> everything(m_roots.size(), false);
            for (;;) {
                ssize_t length = read(m_inotify, buffer, sizeof(buffer));
                if (length <= 0) break;
                for (char* at = buffer; at < buffer + length;) {
                    const auto* event = reinterpret_cast<const inotify_event*>(at);
                    at += sizeof(inotify_event) + event->len;
                    Handle(*event, changed, everything);
                }
            }
            for (size_t r = 0; r < m_roots.size(); r++) {
                if (everything[r] || !changed[r].empty()) {
                    m_handler(Utf8ToWide(m_roots[r]), changed[r], everything[r]);
                }
            }
        }
    }

    void Handle(const inotify_event& event, std::vector<std::vector<std::wstring>>& changed,
                std::vector<bool>& everything) {
        if (event.mask & IN_Q_OVERFLOW) {
            // Events were lost: everything, everywhere
            everything.assign(everything.size(), true);
            return;
        }
        auto found = m_folders.find(event.wd);
        if (found == m_folders.end()) return;
        const Folder folder = found->second;
        if (event.mask & IN_IGNORED) {
            m_folders.erase(found);  // its folder is gone, or no longer watched
            return;
        }
        if (event.mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
            // A root itself gone or moved: look again at all of it. A folder under
            // one is reported by its parent, as deleted or moved from there.
            if (folder.path == m_roots[folder.root]) {
                everything[folder.root] = true;
            } else if (event.mask & IN_MOVE_SELF) {
                // Moved within the roots, it was watched again at its new path as it
                // came; moved out of them, it and what is under it are not wanted
                struct stat info;
                if (lstat(folder.path.c_str(), &info) != 0 || info.st_ino != folder.inode) Unwatch(folder.path);
            }
            return;
        }
        if (event.len == 0) return;
        std::string path = folder.path + "/" + event.name;
        // A folder made or moved in: watch it and what it has already
        if ((event.mask & IN_ISDIR) && (event.mask & (IN_CREATE | IN_MOVED_TO))) WatchTree(folder.root, path);
        // (A new file comes as made, then as written: once is enough)
        std::wstring wide = Utf8ToWide(path);
        std::vector<std::wstring>& paths = changed[folder.root];
        if (paths.empty() || paths.back() != wide) paths.push_back(std::move(wide));
    }

    // No more watches on `dir` or on anything under it
    void Unwatch(const std::string& dir) {
        for (auto it = m_folders.begin(); it != m_folders.end();) {
            const std::string& path = it->second.path;
            if (path == dir || (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
                                path[dir.size()] == '/')) {
                inotify_rm_watch(m_inotify, it->first);
                it = m_folders.erase(it);
            } else {
                ++it;
            }
        }
    }

    Handler m_handler;
    std::vector<std::string> m_roots;
    std::map<int, Folder> m_folders;  // by watch descriptor; the watch thread's, once it starts
    int m_inotify = -1;
    int m_stop = -1;
    std::thread m_thread;
};

}  // namespace

std::unique_ptr<FolderWatch> FolderWatch::Start(const std::vector<std::wstring>& roots, Handler handler) {
    if (roots.empty()) return nullptr;
    auto watch = std::make_unique<LinuxFolderWatch>(roots, std::move(handler));
    if (!watch->Ok()) return nullptr;
    return watch;
}
