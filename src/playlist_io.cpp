#include "playlist_io.h"
#include "ini.h"
#include "paths.h"
#include "utils.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

struct FolderEntry {
    std::wstring name;
    bool isFolder;
};

// What is in `dir` (which ends with a separator), in the order the system lists it
// (by name outside Windows, where the order is otherwise arbitrary). With skipLinks,
// junctions and symbolic links are left out, so a recursive walk cannot loop.
// False if the folder cannot be read.
static bool ListFolder(const std::wstring& dir, bool skipLinks, std::vector<FolderEntry>& entries) {
#ifdef _WIN32
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return false;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (skipLinks && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        entries.push_back({fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
#else
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::path(dir), ec);
    if (ec) return false;
    for (const auto& entry : it) {
        if (skipLinks && entry.is_symlink(ec)) continue;
        entries.push_back({entry.path().filename().wstring(), entry.is_directory(ec)});
    }
#endif
    // In the order a person would put them: "track 2" before "track 10" (the file
    // system's own order, where it has one, puts the digits in dictionary order).
    std::sort(entries.begin(), entries.end(), [](const FolderEntry& a, const FolderEntry& b) {
        return WStrNaturalCmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return true;
}

static bool IsFolder(const std::wstring& path) {
    std::error_code ec;
    return std::filesystem::is_directory(std::filesystem::path(path), ec);
}

// A path that does not need the playlist's folder in front of it.
static bool IsAbsolutePath(const std::wstring& path) {
#ifdef _WIN32
    return path.length() > 2 && path[1] == L':';
#else
    return !path.empty() && path[0] == L'/';
#endif
}

// A playlist line: UTF-8 if it is valid UTF-8, otherwise the system's legacy code page
// (Latin-1 outside Windows).
std::wstring PlaylistLineToWide(const char* line) {
#ifdef _WIN32
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, -1, nullptr, 0);
    UINT codePage = len > 0 ? CP_UTF8 : CP_ACP;
    len = MultiByteToWideChar(codePage, 0, line, -1, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring wide(len - 1, L'\0');
    MultiByteToWideChar(codePage, 0, line, -1, &wide[0], len);
    return wide;
#else
    std::string text(line);
    std::wstring wide = Utf8ToWide(text);
    if (wide.find(L'\xFFFD') == std::wstring::npos) return wide;
    wide.clear();
    for (unsigned char ch : text) wide += static_cast<wchar_t>(ch);
    return wide;
#endif
}

// Check if a file extension is a supported audio format
bool IsSupportedAudioExt(const std::wstring& ext) {
    static const wchar_t* exts[] = {
        L".mp3", L".wav", L".ogg", L".oga", L".flac", L".m4a", L".m4b", L".wma", L".aac",
        L".opus", L".aiff", L".ape", L".wv", L".mid", L".midi", L".dff", L".dsf",
        L".aif", L".mp2", L".m4r", L".mka", L".mpc", L".tta", L".tak", L".caf", L".w64",
        L".ac3", L".dts", L".rmi", L".mod", L".xm", L".it", L".s3m", L".mptm", L".mo3"
    };
    std::wstring lowerExt = ext;
    for (auto& c : lowerExt) c = towlower(c);
    for (const auto& e : exts) {
        if (lowerExt == e) return true;
    }
    return false;
}

// Expand a single file to all audio files in its folder
// Returns the index of the original file in the expanded list
int ExpandFileToFolder(const std::wstring& filePath, std::vector<std::wstring>& outFiles) {
    outFiles.clear();

    // Get directory and filename
    size_t lastSlash = filePath.find_last_of(L"\\/");
    if (lastSlash == std::wstring::npos) {
        outFiles.push_back(filePath);
        return 0;
    }

    std::wstring dir = filePath.substr(0, lastSlash + 1);
    std::wstring targetFile = filePath.substr(lastSlash + 1);

    // Find all audio files in the directory
    std::vector<FolderEntry> entries;
    if (!ListFolder(dir, false, entries)) {
        outFiles.push_back(filePath);
        return 0;
    }

    std::vector<std::wstring> files;
    for (const auto& entry : entries) {
        if (entry.isFolder) continue;
        const std::wstring& name = entry.name;
        size_t dotPos = name.find_last_of(L'.');
        if (dotPos != std::wstring::npos) {
            std::wstring ext = name.substr(dotPos);
            if (IsSupportedAudioExt(ext)) {
                files.push_back(dir + name);
            }
        }
    }

    // Find the index of the original file
    int targetIndex = 0;
    for (size_t i = 0; i < files.size(); i++) {
        if (WStrICmp(GetFileName(files[i]).c_str(), targetFile.c_str()) == 0) {
            targetIndex = static_cast<int>(i);
            break;
        }
    }

    outFiles = std::move(files);
    return targetIndex;
}

// Recursively add audio files from a folder
static void AddFilesFromFolderRecursive(const std::wstring& folder, std::vector<std::wstring>& files, int depth) {
    // Limit recursion depth to prevent stack overflow
    if (depth > 32) return;

    std::vector<FolderEntry> entries;
    if (!ListFolder(folder + kPathSeparator, true, entries)) return;

    for (const auto& entry : entries) {
        std::wstring fullPath = folder + kPathSeparator + entry.name;

        if (entry.isFolder) {
            // Recurse into subdirectory
            AddFilesFromFolderRecursive(fullPath, files, depth + 1);
        } else {
            // Check if it's a supported audio file
            size_t dotPos = fullPath.rfind(L'.');
            if (dotPos != std::wstring::npos) {
                std::wstring ext = fullPath.substr(dotPos);
                if (IsSupportedAudioExt(ext)) {
                    files.push_back(fullPath);
                }
            }
        }
    }
}

void AddFilesFromFolder(const std::wstring& folder, std::vector<std::wstring>& files) {
    AddFilesFromFolderRecursive(folder, files, 0);
}

// Check if file is a playlist
bool IsPlaylistFile(const std::wstring& path) {
    size_t dotPos = path.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return false;
    std::wstring ext = path.substr(dotPos);
    for (auto& c : ext) c = towlower(c);
    return (ext == L".m3u" || ext == L".m3u8" || ext == L".pls");
}

// Parse M3U playlist file
static std::vector<std::wstring> ParseM3U(const std::wstring& playlistPath) {
    std::vector<std::wstring> entries;

    // Get directory of playlist for relative paths
    std::wstring baseDir;
    size_t lastSlash = playlistPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        baseDir = playlistPath.substr(0, lastSlash + 1);
    }

    // Read file as binary first to detect BOM
    FILE* f = FileOpen(playlistPath, "rb");
    if (!f) return entries;

    // Check for UTF-8 BOM
    unsigned char bom[3] = {0};
    fread(bom, 1, 3, f);
    bool isUtf8 = (bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF);
    if (!isUtf8) {
        fseek(f, 0, SEEK_SET);  // No BOM, rewind
    }

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        // Trim whitespace
        char* start = line;
        while (*start && (*start == ' ' || *start == '\t')) start++;
        size_t slen = strlen(start);
        if (slen == 0) continue;
        char* end = start + slen - 1;
        while (end >= start && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t')) {
            *end-- = '\0';
        }

        // Skip empty lines and comments
        if (*start == '\0' || *start == '#') continue;

        // Convert to wide string (try UTF-8 first, then the legacy code page)
        std::wstring entry = PlaylistLineToWide(start);

        if (entry.empty()) continue;

        // Build full path
        std::wstring fullPath;
        if (WStrNICmp(entry.c_str(), L"http://", 7) == 0 ||
            WStrNICmp(entry.c_str(), L"https://", 8) == 0 ||
            WStrNICmp(entry.c_str(), L"ftp://", 6) == 0 ||
            IsAbsolutePath(entry)) {
            fullPath = entry;
        } else {
            // Relative path - prepend base directory
            fullPath = baseDir + entry;
        }

        // Check if it's a folder and expand it
        if (IsFolder(fullPath)) {
            // Recursively add folder contents
            AddFilesFromFolder(fullPath, entries);
        } else {
            entries.push_back(fullPath);
        }
    }
    fclose(f);
    return entries;
}

// Parse PLS playlist file
static std::vector<std::wstring> ParsePLS(const std::wstring& playlistPath) {
    std::vector<std::wstring> entries;

    // Get directory of playlist for relative paths
    std::wstring baseDir;
    size_t lastSlash = playlistPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        baseDir = playlistPath.substr(0, lastSlash + 1);
    }

    // Read entries (a .pls file is in INI format)
    for (int i = 1; i <= 1000; i++) {  // Reasonable max
        wchar_t key[32];
        swprintf(key, 32, L"File%d", i);
        wchar_t value[4096] = {0};
        IniGetString(L"playlist", key, L"", value, 4096, playlistPath.c_str());
        if (value[0] == L'\0') break;

        std::wstring entry = value;
        std::wstring fullPath;
        // Check if it's a URL or absolute path
        if (WStrNICmp(entry.c_str(), L"http://", 7) == 0 ||
            WStrNICmp(entry.c_str(), L"https://", 8) == 0 ||
            WStrNICmp(entry.c_str(), L"ftp://", 6) == 0 ||
            IsAbsolutePath(entry)) {
            fullPath = entry;
        } else {
            // Relative path - prepend base directory
            fullPath = baseDir + entry;
        }

        // Check if it's a folder and expand it
        if (IsFolder(fullPath)) {
            // Recursively add folder contents
            AddFilesFromFolder(fullPath, entries);
        } else {
            entries.push_back(fullPath);
        }
    }
    return entries;
}

// Parse playlist file (M3U or PLS)
std::vector<std::wstring> ParsePlaylist(const std::wstring& playlistPath) {
    size_t dotPos = playlistPath.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return {};

    std::wstring ext = playlistPath.substr(dotPos);
    for (auto& c : ext) c = towlower(c);

    if (ext == L".pls") {
        return ParsePLS(playlistPath);
    } else {
        return ParseM3U(playlistPath);
    }
}

// ---------------------------------------------------------------------------
// Track names
// ---------------------------------------------------------------------------

static std::mutex g_trackNamesMutex;
static std::map<std::wstring, std::wstring> g_trackNames;
static std::map<std::wstring, TrackMetadata> g_trackMetadata;

void SetTrackMetadata(const std::wstring& path, const TrackMetadata& metadata) {
	std::lock_guard<std::mutex> lock(g_trackNamesMutex);
	g_trackMetadata[path] = metadata;
}

TrackMetadata GetTrackMetadata(const std::wstring& path) {
	std::lock_guard<std::mutex> lock(g_trackNamesMutex);
	auto it = g_trackMetadata.find(path);
	return it == g_trackMetadata.end() ? TrackMetadata() : it->second;
}

void SetTrackName(const std::wstring& path, const std::wstring& name) {
    std::lock_guard<std::mutex> lock(g_trackNamesMutex);
    g_trackNames[path] = name;
}

std::wstring GetTrackName(const std::wstring& path) {
    {
        std::lock_guard<std::mutex> lock(g_trackNamesMutex);
        auto it = g_trackNames.find(path);
        if (it != g_trackNames.end()) return it->second;
    }
    return GetFileName(path);
}
