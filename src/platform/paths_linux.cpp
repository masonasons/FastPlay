// Paths on Linux: the XDG base and user directories (~/.local/share/FastPlay for
// settings and the library, the desktop's own Music and Downloads folders).

#include "paths.h"
#include "utils.h"

#include <glib.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <string>

const wchar_t kPathSeparator = L'/';

static std::string HomeDir() {
    if (const char* home = std::getenv("HOME")) {
        if (*home) return home;
    }
    if (struct passwd* pw = getpwuid(getuid())) {
        return pw->pw_dir;
    }
    return "/tmp";
}

std::wstring GetExecutableDir() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return L"./";
    std::string path(buf, static_cast<size_t>(n));
    size_t pos = path.find_last_of('/');
    return Utf8ToWide(pos != std::string::npos ? path.substr(0, pos + 1) : std::string("./"));
}

// As on the Mac: settings always live in the user's data directory.
bool IsInstalledMode() {
    return true;
}

// Every directory on the way, as mkdir -p
static void MakeDirectories(const std::string& dir) {
    for (size_t pos = 1; pos <= dir.size(); pos++) {
        if (pos == dir.size() || dir[pos] == '/') mkdir(dir.substr(0, pos).c_str(), 0755);
    }
}

std::wstring GetDataDirectory() {
    // $XDG_DATA_HOME, or ~/.local/share
    std::string dir = std::string(g_get_user_data_dir()) + "/FastPlay";
    MakeDirectories(dir);
    return Utf8ToWide(dir + "/");
}

static std::wstring SpecialDir(GUserDirectory which, const char* fallback) {
    const char* dir = g_get_user_special_dir(which);
    return Utf8ToWide(dir && *dir ? std::string(dir) : HomeDir() + "/" + fallback);
}

std::wstring GetUserMusicDir() {
    return SpecialDir(G_USER_DIRECTORY_MUSIC, "Music");
}

std::wstring GetUserDownloadsDir() {
    return SpecialDir(G_USER_DIRECTORY_DOWNLOAD, "Downloads");
}

std::wstring GetTempDir() {
    std::string dir = g_get_tmp_dir();
    if (dir.empty() || dir.back() != '/') dir += '/';
    return Utf8ToWide(dir);
}

// Beside the executable, as everything FastPlay uses is linked into it.
std::wstring GetLibraryDir() {
    return GetExecutableDir();
}
