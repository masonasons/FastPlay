// YouTube: search, listings, and getting videos ready to play.
//
// yt-dlp does the talking to YouTube. It needs a JavaScript runtime (deno) to answer
// the challenges YouTube sets, and it needs to be recent, since YouTube keeps
// changing. So FastPlay keeps its own copies of both in its data folder: downloaded
// the first time they are needed, and yt-dlp updated at most once a day. A yt-dlp
// chosen in Options is used instead when that file exists. Installed mode uses
// user-owned tools from PATH or explicit paths and never downloads or updates them.
//
// Videos stream from YouTube's HLS audio, which FFmpeg plays as it arrives, with
// seeking. For the rare video without it, the audio is downloaded instead: yt-dlp
// downloads it and DefragmentMp4 turns it into an ordinary M4A.

#include "youtube.h"
#include "youtube_tools.h"
#include "accessibility.h"
#include "app_ui.h"
#include "globals.h"
#include "http.h"
#include "mp4_remux.h"
#include "paths.h"
#include "subprocess.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// The tools: yt-dlp and deno
// ---------------------------------------------------------------------------

#if defined(_WIN32)
const wchar_t* const kYtdlpFile = L"yt-dlp.exe";
const wchar_t* const kYtdlpUrl = L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe";
// The same yt-dlp unpacked in a folder: it starts in a fraction of a second, where
// the single file above unpacks itself on every run (about two seconds).
const wchar_t* const kYtdlpZip = L"yt-dlp_win.zip";
const wchar_t* const kYtdlpFolderFile = L"yt-dlp.exe";
const wchar_t* const kDenoFile = L"deno.exe";
const wchar_t* const kDenoUrl =
    L"https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip";
#elif defined(__APPLE__)
const wchar_t* const kYtdlpFile = L"yt-dlp";
const wchar_t* const kYtdlpUrl = L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_macos";
const wchar_t* const kYtdlpZip = L"yt-dlp_macos.zip";
const wchar_t* const kYtdlpFolderFile = L"yt-dlp_macos";
const wchar_t* const kDenoFile = L"deno";
#if defined(__arm64__) || defined(__aarch64__)
const wchar_t* const kDenoUrl =
    L"https://github.com/denoland/deno/releases/latest/download/deno-aarch64-apple-darwin.zip";
#else
const wchar_t* const kDenoUrl =
    L"https://github.com/denoland/deno/releases/latest/download/deno-x86_64-apple-darwin.zip";
#endif
#else
const wchar_t* const kYtdlpFile = L"yt-dlp";
const wchar_t* const kYtdlpUrl = L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux";
const wchar_t* const kYtdlpZip = nullptr;  // the single file (Linux has Python to spare)
const wchar_t* const kYtdlpFolderFile = nullptr;
const wchar_t* const kDenoFile = L"deno";
const wchar_t* const kDenoUrl =
    L"https://github.com/denoland/deno/releases/latest/download/deno-x86_64-unknown-linux-gnu.zip";
#endif

// ffmpeg, for downloads that convert, combine or tag files: yt-dlp's own
// Windows build (the "shared" one, 82 MB rather than 186), and a macOS build of
// ffmpeg and ffprobe for this processor.
#if defined(_WIN32)
const wchar_t* const kFfmpegFile = L"ffmpeg.exe";
const wchar_t* const kFfmpegUrls[] = {
    L"https://github.com/yt-dlp/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip"};
#elif defined(__APPLE__)
const wchar_t* const kFfmpegFile = L"ffmpeg";
#if defined(__arm64__) || defined(__aarch64__)
const wchar_t* const kFfmpegUrls[] = {L"https://ffmpeg.martin-riedl.de/redirect/latest/macos/arm64/release/ffmpeg.zip",
                                      L"https://ffmpeg.martin-riedl.de/redirect/latest/macos/arm64/release/ffprobe.zip"};
#else
const wchar_t* const kFfmpegUrls[] = {L"https://ffmpeg.martin-riedl.de/redirect/latest/macos/amd64/release/ffmpeg.zip",
                                      L"https://ffmpeg.martin-riedl.de/redirect/latest/macos/amd64/release/ffprobe.zip"};
#endif
#else
const wchar_t* const kFfmpegFile = L"ffmpeg";
const wchar_t* const kFfmpegUrls[] = {L""};  // use the system's
#endif

// One thread at a time downloads or updates the tools.
std::mutex g_toolsMutex;

// The imported cookies.txt, if any
std::wstring CookiesPath() {
    return GetDataDirectory() + L"youtube-cookies.txt";
}

void Say(const YouTubeStatus& status, const std::wstring& message) {
    if (status) status(message);
}

bool FileExists(const std::wstring& path) {
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(fs::path(path), ec);
}

std::wstring ToolsDir() {
    std::wstring dir = GetDataDirectory() + L"tools" + kPathSeparator;
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    return dir;
}

void MakeExecutable(const std::wstring& path) {
#ifndef _WIN32
    chmod(WideToUtf8(path).c_str(), 0755);
#else
    (void)path;
#endif
}

// Download a file, replacing `path` only once it has arrived whole.
bool DownloadFile(const std::wstring& url, const std::wstring& path, std::wstring& error) {
    std::wstring partial = path + L".part";
    HttpOptions options;
    options.saveTo = partial;
    options.timeoutMs = 60000;
    HttpResult result = HttpGet(url, options);
    std::error_code ec;
    if (!result.completed || result.status != 200) {
        fs::remove(fs::path(partial), ec);
        error = result.completed ? L"The download failed (HTTP " + std::to_wstring(result.status) + L")."
                                 : L"The download failed: " + result.errorText;
        return false;
    }
    fs::remove(fs::path(path), ec);
    fs::rename(fs::path(partial), fs::path(path), ec);
    if (ec) {
        error = L"Could not save " + path;
        return false;
    }
    MakeExecutable(path);
    return true;
}

bool Unzip(const std::wstring& zip, std::wstring dir);
std::wstring TarPath();

const wchar_t* const kYtdlpReleases = L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/";

// The yt-dlp release's checksum for the folder build's zip, from its SHA2-256SUMS,
// or empty if that could not be read. A new checksum means a new release.
std::string LatestYtdlpZipSum() {
    HttpOptions options;
    options.timeoutMs = 15000;
    HttpResult result = HttpGet(std::wstring(kYtdlpReleases) + L"SHA2-256SUMS", options);
    if (!result.completed || result.status != 200) return "";
    std::string name = WideToUtf8(kYtdlpZip);
    std::istringstream lines(result.body);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t space = line.find("  ");
        if (space != std::string::npos && line.substr(space + 2) == name) return line.substr(0, space);
    }
    return "";
}

// Download and unpack the folder build, replacing the one there (unless it is
// running, in which case it stays until next time).
bool InstallYtdlpFolder(const std::wstring& folder, std::wstring& error) {
    std::wstring zip = ToolsDir() + kYtdlpZip;
    if (!DownloadFile(std::wstring(kYtdlpReleases) + kYtdlpZip, zip, error)) return false;
    std::error_code ec;
    std::wstring fresh = folder + L".new";
    std::wstring old = folder + L".old";
    fs::remove_all(fs::path(fresh), ec);
    fs::create_directories(fs::path(fresh), ec);
    bool unpacked = Unzip(zip, fresh);
    fs::remove(fs::path(zip), ec);
    if (!unpacked || !FileExists(fresh + kPathSeparator + kYtdlpFolderFile)) {
        fs::remove_all(fs::path(fresh), ec);
        error = L"Could not unpack yt-dlp.";
        return false;
    }
    MakeExecutable(fresh + kPathSeparator + kYtdlpFolderFile);
    fs::remove_all(fs::path(old), ec);
    if (fs::exists(fs::path(folder), ec)) {
        fs::rename(fs::path(folder), fs::path(old), ec);
        if (ec) {
            fs::remove_all(fs::path(fresh), ec);
            error = L"yt-dlp is in use.";
            return false;
        }
    }
    fs::rename(fs::path(fresh), fs::path(folder), ec);
    if (ec) {
        fs::rename(fs::path(old), fs::path(folder), ec);
        error = L"Could not save yt-dlp.";
        return false;
    }
    fs::remove_all(fs::path(old), ec);
    return true;
}

std::string ReadSmallFile(const std::wstring& path) {
    std::string text;
    if (FILE* f = FileOpen(path, "rb")) {
        char buffer[256];
        size_t n = fread(buffer, 1, sizeof(buffer), f);
        text.assign(buffer, n);
        fclose(f);
    }
    return text;
}

// The yt-dlp to run.
bool EnsureYtdlp(std::wstring& ytdlp, std::wstring& error, const YouTubeStatus& status,
	const YouTubeToolSettings& tools) {
	if (tools.source == YouTubeToolSource::Installed) {
		return ResolveYouTubeTool(tools, YouTubeTool::Ytdlp, ytdlp, error);
	}
	if (FileExists(tools.ytdlpPath)) {  // chosen in Options; its owner keeps it up to date
		ytdlp = tools.ytdlpPath;
        return true;
    }

    std::lock_guard<std::mutex> lock(g_toolsMutex);
    std::error_code ec;

    // The folder build, checked for a new release once a day (it cannot update
    // itself). The stamp holds the checksum of the one installed.
    if (kYtdlpZip && FileExists(TarPath())) {
        std::wstring zipName = kYtdlpZip;
        std::wstring folder = ToolsDir() + zipName.substr(0, zipName.size() - 4);
        std::wstring exe = folder + kPathSeparator + kYtdlpFolderFile;
        std::wstring stamp = ToolsDir() + L"yt-dlp-folder-checked";
        auto checked = fs::last_write_time(fs::path(stamp), ec);
        bool recent = !ec && fs::file_time_type::clock::now() - checked < std::chrono::hours(24);
        if (FileExists(exe) && recent) {
            ytdlp = exe;
            return true;
        }
        if (!FileExists(exe)) Say(status, L"Downloading yt-dlp for YouTube. This happens once.");
        std::string latest = LatestYtdlpZipSum();
        std::string installed = ReadSmallFile(stamp);
        bool installedNow = false;
        if (!FileExists(exe) || (!latest.empty() && latest != installed)) {
            std::wstring installError;
            installedNow = InstallYtdlpFolder(folder, installError);
        }
        if (FileExists(exe)) {
            // Checked (or the check failed: try again tomorrow, not every run)
            if (FILE* f = FileOpen(stamp, "wb")) {
                const std::string& sum = installedNow ? latest : installed;
                fwrite(sum.data(), 1, sum.size(), f);
                fclose(f);
            }
            fs::last_write_time(fs::path(stamp), fs::file_time_type::clock::now(), ec);
            // The single file it replaces
            fs::remove(fs::path(ToolsDir() + kYtdlpFile), ec);
            fs::remove(fs::path(ToolsDir() + L"yt-dlp-checked"), ec);
            ytdlp = exe;
            return true;
        }
        // Could not be had (no tar to unpack it): the single file
    }

    ytdlp = ToolsDir() + kYtdlpFile;
    std::wstring stamp = ToolsDir() + L"yt-dlp-checked";
    if (!FileExists(ytdlp)) {
        Say(status, L"Downloading yt-dlp for YouTube. This happens once.");
        if (!DownloadFile(kYtdlpUrl, ytdlp, error)) {
            error = L"Could not download yt-dlp. " + error;
            return false;
        }
    } else {
        // An old yt-dlp soon stops working with YouTube: update it once a day.
        auto checked = fs::last_write_time(fs::path(stamp), ec);
        if (ec || fs::file_time_type::clock::now() - checked > std::chrono::hours(24)) {
            std::string output;
            RunProcessCapture(ytdlp, {L"-U"}, output);
        } else {
            return true;
        }
    }
    if (FILE* f = FileOpen(stamp, "w")) fclose(f);
    fs::last_write_time(fs::path(stamp), fs::file_time_type::clock::now(), ec);
    return true;
}

// tar reads zip files, and comes with Windows 10 and macOS.
std::wstring TarPath() {
#ifdef _WIN32
    wchar_t system[kMaxPathChars] = {};
    GetSystemDirectoryW(system, kMaxPathChars);
    return std::wstring(system) + L"\\tar.exe";
#else
    return L"/usr/bin/tar";
#endif
}

bool Unzip(const std::wstring& zip, std::wstring dir) {
    std::wstring tar = TarPath();
    if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    std::string output;
    int exitCode = -1;
    return RunProcessCapture(tar, {L"-xf", zip, L"-C", dir}, output, nullptr, &exitCode) && exitCode == 0;
}

// FastPlay's deno, or empty if it could not be had (yt-dlp then tries without).
std::wstring EnsureDeno(const YouTubeStatus& status) {
    std::lock_guard<std::mutex> lock(g_toolsMutex);
    std::wstring deno = ToolsDir() + kDenoFile;
    if (FileExists(deno)) return deno;

    Say(status, L"Downloading deno, which yt-dlp needs for YouTube. This happens once and takes a minute.");
    std::wstring zip = ToolsDir() + L"deno.zip";
    std::wstring error;
    if (!DownloadFile(kDenoUrl, zip, error)) return L"";

    Unzip(zip, ToolsDir());
    std::error_code ec;
    fs::remove(fs::path(zip), ec);
    if (!FileExists(deno)) return L"";
    MakeExecutable(deno);
    return deno;
}


// Where ffmpeg is for yt-dlp: FastPlay's own copy (its folder), or empty for one
// already installed on the PATH. False when there is none and none could be had.
bool EnsureFfmpeg(std::wstring& location, std::wstring& error, const YouTubeStatus& status,
	const YouTubeToolSettings& tools) {
	if (tools.source == YouTubeToolSource::Installed) {
		std::wstring executable;
		if (!ResolveYouTubeTool(tools, YouTubeTool::Ffmpeg, executable, error)) return false;
		location = fs::path(executable).parent_path().wstring();
		return true;
	}
    std::lock_guard<std::mutex> lock(g_toolsMutex);
    std::wstring dir = ToolsDir() + L"ffmpeg" + kPathSeparator;
    location = dir;
    if (FileExists(dir + kFfmpegFile)) return true;

    std::string output;
    int exitCode = -1;
    if (RunProcessCapture(L"ffmpeg", {L"-version"}, output, nullptr, &exitCode) && exitCode == 0) {
        location.clear();
        return true;
    }
    if (!kFfmpegUrls[0][0]) {
        error = L"This download needs ffmpeg. Install it and try again.";
        return false;
    }

    Say(status, L"Downloading ffmpeg, which this kind of download needs. This happens once and takes a few minutes.");
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    std::wstring unpack = ToolsDir() + L"ffmpeg-unpack";
    for (const wchar_t* url : kFfmpegUrls) {
        std::wstring zip = ToolsDir() + L"ffmpeg-download.zip";
        if (!DownloadFile(url, zip, error)) {
            error = L"Could not download ffmpeg. " + error;
            return false;
        }
        fs::remove_all(fs::path(unpack), ec);
        fs::create_directories(fs::path(unpack), ec);
        bool unpacked = Unzip(zip, unpack);
        fs::remove(fs::path(zip), ec);
        if (!unpacked) {
            error = L"Could not unpack ffmpeg.";
            return false;
        }
        // Keep the programs and the libraries beside them, wherever the archive
        // put them (Windows: a bin folder inside a named folder).
        for (auto it = fs::recursive_directory_iterator(fs::path(unpack), ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::wstring name = it->path().filename().wstring();
            std::wstring ext = it->path().extension().wstring();
            bool wanted = name.rfind(L"ffmpeg", 0) == 0 || name.rfind(L"ffprobe", 0) == 0 ||
                          WStrICmp(ext.c_str(), L".dll") == 0;
            if (!wanted || WStrICmp(ext.c_str(), L".html") == 0 || WStrICmp(ext.c_str(), L".1") == 0) continue;
            fs::copy_file(it->path(), fs::path(dir) / it->path().filename(), fs::copy_options::overwrite_existing, ec);
            MakeExecutable((fs::path(dir) / it->path().filename()).wstring());
        }
        fs::remove_all(fs::path(unpack), ec);
    }
    if (!FileExists(dir + kFfmpegFile)) {
        error = L"The ffmpeg download did not contain ffmpeg.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Running yt-dlp
// ---------------------------------------------------------------------------

struct YtdlpRun {
    std::string output;  // stdout, UTF-8
    std::string errors;  // stderr
    int exitCode = -1;
};

// The last "ERROR:" line yt-dlp wrote, for telling the user what went wrong, or
// failing that its last line (a mistyped option is "yt-dlp: error: ...").
std::wstring YtdlpError(const YtdlpRun& run) {
    std::istringstream lines(run.errors);
    std::string line, error, lastLine;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("ERROR:", 0) == 0) error = line;
        if (!line.empty()) lastLine = line;
    }
    if (error.empty()) error = lastLine;
    if (error.empty()) return L"yt-dlp failed (exit code " + std::to_wstring(run.exitCode) + L").";
    return Utf8ToWide(error);
}

bool RunYtdlp(const std::vector<std::wstring>& args, bool needsDeno, YtdlpRun& run, std::wstring& error,
	const YouTubeStatus& status, const YouTubeToolSettings& tools = GetYouTubeToolSettings()) {
    std::wstring ytdlp;
    if (!EnsureYtdlp(ytdlp, error, status, tools)) return false;
    std::vector<std::wstring> all = {L"--ignore-config", L"--no-update", L"--encoding", L"utf-8"};
    if (needsDeno) {
		std::wstring deno;
		if (tools.source == YouTubeToolSource::Installed) {
			if (!ResolveYouTubeTool(tools, YouTubeTool::Deno, deno, error)) return false;
		} else {
			deno = EnsureDeno(status);
		}
        if (!deno.empty()) {
            all.push_back(L"--js-runtimes");
            all.push_back(L"deno:" + deno);
        }
    }
    if (YouTubeHasCookies()) {
        all.push_back(L"--cookies");
        all.push_back(CookiesPath());
    }
    all.insert(all.end(), args.begin(), args.end());
	ProcessOptions options;
	options.pathDirectories = YouTubeToolChildDirectories(tools);
    if (!RunProcessCapture(ytdlp, all, run.output, &run.errors, &run.exitCode, options)) {
        error = L"Could not run yt-dlp (" + ytdlp + L").";
        return false;
    }
    return true;
}

// The lines yt-dlp printed with --print, split at tabs.
std::vector<std::vector<std::wstring>> PrintedRows(const std::string& output) {
    std::vector<std::vector<std::wstring>> rows;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find('\t') == std::string::npos) continue;
        std::vector<std::wstring> fields;
        size_t start = 0;
        for (;;) {
            size_t tab = line.find('\t', start);
            fields.push_back(Utf8ToWide(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start)));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        rows.push_back(std::move(fields));
    }
    return rows;
}

// A flat listing (search results, playlist, channel): one line per entry.
const wchar_t* const kListFormat = L"%(id)s\t%(title)s\t%(channel,uploader|)s\t%(duration_string|)s"
                                   L"\t%(live_status|)s\t%(ie_key|)s\t%(channel_id|)s";

// yt-dlp listings are paged by position: a page token says where the next page starts.
const wchar_t* const kPagePrefix = L"ytdlp:";

size_t PageStart(const std::wstring& pageToken) {
    if (pageToken.compare(0, 6, kPagePrefix) != 0) return 1;
    size_t start = static_cast<size_t>(std::wcstoul(pageToken.c_str() + 6, nullptr, 10));
    return start ? start : 1;
}

bool IsChannelId(const std::wstring& id) {
    return id.size() == 24 && id.compare(0, 2, L"UC") == 0;
}

// One page of a flat listing of `target`. A "{end}" in target becomes the number of
// the page's last entry (for "ytsearch{end}:query", which must ask for that many).
bool ListWithYtdlp(std::wstring target, size_t pageSize, const std::wstring& pageToken,
                   std::vector<YouTubeResult>& results, std::wstring& nextPageToken, std::wstring& error,
                   const YouTubeStatus& status) {
    size_t first = PageStart(pageToken), last = first + pageSize - 1;
    size_t marker = target.find(L"{end}");
    if (marker != std::wstring::npos) target.replace(marker, 5, std::to_wstring(last));
    std::wstring items = std::to_wstring(first) + L":" + std::to_wstring(last);

    YtdlpRun run;
    if (!RunYtdlp({L"--flat-playlist", L"--playlist-items", items, L"--print", kListFormat, target}, false, run,
                  error, status)) {
        return false;
    }
    size_t found = 0;
    for (const auto& row : PrintedRows(run.output)) {
        if (row.size() < 7 || row[0].empty() || row[1].empty()) continue;
        found++;
        YouTubeResult result;
        result.id = row[0];
        result.title = row[1];
        result.channel = row[2];
        result.channelId = row[6];
        if (row[5] == L"YoutubeTab") {
            // A channel or playlist among search results
            if (IsChannelId(result.id)) {
                result.kind = YouTubeKind::Channel;
                result.channelId = result.id;
                if (result.channel.empty()) result.channel = result.title;
            } else {
                result.kind = YouTubeKind::Playlist;
            }
        } else {
            result.duration = row[4] == L"is_live" ? L"live" : row[3];
        }
        results.push_back(result);
    }
    if (found == pageSize) nextPageToken = kPagePrefix + std::to_wstring(last + 1);
    if (found == 0 && run.exitCode != 0) {
        error = YtdlpError(run);
        return false;
    }
    return true;
}

// Unix time from an ISO 8601 UTC-offset timestamp ("2026-09-20T08:52:27+00:00").
int64_t ParseIsoTime(const std::string& text) {
    int y, mo, d, h = 0, mi = 0, sec = 0;
    if (sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3) return 0;
    // Days since 1970-01-01 of a civil date
    y -= mo <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    int64_t seconds = days * 86400 + h * 3600 + mi * 60 + sec;
    // The offset, if any, after the seconds
    size_t t = text.find('T');
    size_t sign = t == std::string::npos ? std::string::npos : text.find_first_of("+-", t);
    if (sign != std::string::npos) {
        int oh = 0, om = 0;
        if (sscanf(text.c_str() + sign + 1, "%d:%d", &oh, &om) >= 1) {
            int64_t offset = oh * 3600 + om * 60;
            seconds += text[sign] == '+' ? -offset : offset;
        }
    }
    return seconds;
}

// ---------------------------------------------------------------------------
// The YouTube Data API (search with a key)
// ---------------------------------------------------------------------------

// URL encode a string
std::wstring UrlEncode(const std::wstring& str) {
    std::string utf8 = WideToUtf8(str);
    std::wostringstream encoded;
    for (unsigned char c : utf8) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << static_cast<wchar_t>(c);
        } else {
            encoded << L'%' << std::hex << std::uppercase << ((c >> 4) & 0xF) << (c & 0xF);
        }
    }
    return encoded.str();
}

// The string value of "key" in a JSON text (the first one), with its escapes decoded.
std::wstring ParseJsonString(const std::wstring& json, const std::wstring& key) {
    std::wstring searchKey = L"\"" + key + L"\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::wstring::npos) return L"";
    size_t colon = json.find(L':', keyPos + searchKey.length());
    if (colon == std::wstring::npos) return L"";
    size_t pos = json.find_first_not_of(L" \t\r\n", colon + 1);
    if (pos == std::wstring::npos || json[pos] != L'"') return L"";

    std::u16string units;  // \u escapes are UTF-16 code units (pairs for emoji)
    std::wstring value;
    auto flushUnits = [&]() {
        if (units.empty()) return;
        value += Utf8ToWide(Utf16ToUtf8(units));
        units.clear();
    };
    for (pos++; pos < json.size() && json[pos] != L'"'; pos++) {
        wchar_t c = json[pos];
        if (c != L'\\' || pos + 1 >= json.size()) {
            flushUnits();
            value += c;
            continue;
        }
        wchar_t e = json[++pos];
        if (e == L'u' && pos + 4 < json.size()) {
            units += static_cast<char16_t>(std::wcstol(json.substr(pos + 1, 4).c_str(), nullptr, 16));
            pos += 4;
            continue;
        }
        flushUnits();
        switch (e) {
            case L'n': value += L' '; break;
            case L't': value += L'\t'; break;
            case L'r': break;
            case L'b': case L'f': break;
            default: value += e; break;  // \" \\ \/
        }
    }
    flushUnits();
    return value;
}

// Find the end of a JSON object or array, ignoring delimiters inside strings.
// Search entries must be read as whole objects: a snippet can be arbitrarily
// long, and its position relative to the video ID is not fixed.
size_t JsonContainerEnd(const std::wstring& json, size_t start) {
	int depth = 0;
	bool inString = false;
	for (size_t pos = start; pos < json.size(); pos++) {
		wchar_t c = json[pos];
		if (inString) {
			if (c == L'\\') pos++;
			else if (c == L'"') inString = false;
			continue;
		}
		if (c == L'"') inString = true;
		else if (c == L'{' || c == L'[') depth++;
		else if ((c == L'}' || c == L']') && --depth == 0) return pos + 1;
	}
	return std::wstring::npos;
}

// JSON keeps tabs, newlines and quotes in titles/descriptions from changing the
// boundaries of the printed fields. Numeric fields are printed as strings too.
const wchar_t* const kMediaFormat =
	L"{\"title\":%(title|null)j,\"url\":%(url|null)j,\"channel\":%(channel,uploader|null)j,"
	L"\"artist\":%(artist,creator,channel,uploader|null)j,\"album\":%(album|null)j,"
	L"\"year\":\"%(release_year,release_date>%Y,upload_date>%Y|)s\","
	L"\"track\":\"%(track_number|)s\",\"genre\":%(genre|null)j,\"description\":%(description|null)j,"
	L"\"duration\":\"%(duration|)s\",\"bitrate\":\"%(abr|)s\",\"live_status\":%(live_status|null)j}";

bool ReadPreparedMedia(const std::string& output, YouTubeMedia& media) {
	std::istringstream lines(output);
	std::string line;
	while (std::getline(lines, line)) {
		std::wstring json = Utf8ToWide(line);
		size_t start = json.find_first_not_of(L" \t\r");
		if (start == std::wstring::npos || json[start] != L'{' ||
			JsonContainerEnd(json, start) == std::wstring::npos) continue;
		std::wstring title = ParseJsonString(json, L"title");
		if (title.empty()) continue;
		media = YouTubeMedia();
		media.title = title;
		media.url = ParseJsonString(json, L"url");
		media.channel = ParseJsonString(json, L"channel");
		media.metadata.tags["TITLE"] = WideToUtf8(title);
		for (const auto& field : {std::pair<const char*, const wchar_t*>{"ARTIST", L"artist"},
			{"ALBUM", L"album"}, {"DATE", L"year"}, {"TRACKNUMBER", L"track"},
			{"GENRE", L"genre"}, {"COMMENT", L"description"}}) {
			std::wstring value = ParseJsonString(json, field.second);
			if (!value.empty()) media.metadata.tags[field.first] = WideToUtf8(value);
		}
		if (ParseJsonString(json, L"live_status") != L"is_live") {
			double duration = std::wcstod(ParseJsonString(json, L"duration").c_str(), nullptr);
			if (duration > 0 && duration < 2147483647.0) media.metadata.duration = duration;
		}
		double bitrate = std::wcstod(ParseJsonString(json, L"bitrate").c_str(), nullptr);
		if (bitrate > 0 && bitrate < 2147483646.5) media.metadata.bitrate = static_cast<int>(bitrate + 0.5);
		return true;
	}
	return false;
}

bool SearchWithAPI(const std::wstring& query, std::vector<YouTubeResult>& results, std::wstring& nextPageToken,
                   const std::wstring& pageToken) {
	std::wstring url = L"https://www.googleapis.com/youtube/v3/search?part=snippet&type=video&maxResults=25&q=";
	url += UrlEncode(query);
	url += L"&key=" + g_ytApiKey;
	if (!pageToken.empty()) {
		url += L"&pageToken=" + pageToken;
	}

	std::wstring response = Utf8ToWide(HttpGet(url).body);
	if (response.empty()) return false;

	nextPageToken = ParseJsonString(response, L"nextPageToken");
	size_t itemsPos = response.find(L"\"items\"");
	if (itemsPos == std::wstring::npos) return false;
	size_t arrayStart = response.find(L'[', itemsPos + 7);
	if (arrayStart == std::wstring::npos) return false;
	size_t arrayEnd = JsonContainerEnd(response, arrayStart);
	if (arrayEnd == std::wstring::npos) return false;

	// Keep the ID, title and channel within the same entry, regardless of key
	// order. Never search backwards into the previous result's snippet.
	size_t pos = arrayStart + 1;
	while ((pos = response.find_first_not_of(L" \t\r\n,", pos)) < arrayEnd - 1) {
		if (response[pos] != L'{') return false;
		size_t end = JsonContainerEnd(response, pos);
		if (end == std::wstring::npos || end >= arrayEnd) return false;
		std::wstring item = response.substr(pos, end - pos);
		YouTubeResult result;
		result.id = ParseJsonString(item, L"videoId");
		result.title = ParseJsonString(item, L"title");
		result.channel = ParseJsonString(item, L"channelTitle");
		result.channelId = ParseJsonString(item, L"channelId");
		if (!result.id.empty() && !result.title.empty()) results.push_back(std::move(result));
		pos = end;
	}
	return !results.empty();
}

// ---------------------------------------------------------------------------
// Downloaded videos
// ---------------------------------------------------------------------------

std::wstring CacheDir() {
    std::wstring dir = GetTempDir() + L"FastPlay YouTube" + kPathSeparator;
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    return dir;
}

// A title made safe as a file name.
std::wstring FileNameFrom(const std::wstring& title) {
    std::wstring name;
    for (wchar_t c : title) {
        if (c < 32 || std::wstring(L"\\/:*?\"<>|").find(c) != std::wstring::npos) {
            name += L'_';
        } else {
            name += c;
        }
    }
    if (name.size() > 120) name.resize(120);
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    return name.empty() ? L"YouTube" : name;
}

// A video downloaded before, found by the " [id].m4a" its file name ends with.
std::wstring FindDownloaded(const std::wstring& videoId) {
    std::wstring suffix = L" [" + videoId + L"].m4a";
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::path(CacheDir()), ec)) {
        std::wstring name = entry.path().filename().wstring();
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return entry.path().wstring();
        }
    }
    return L"";
}

}  // namespace

// ---------------------------------------------------------------------------
// Public functions
// ---------------------------------------------------------------------------

bool YouTubeSearch(const std::wstring& query, std::vector<YouTubeResult>& results, std::wstring& nextPageToken,
                   const std::wstring& pageTokenIn, std::wstring& error, const YouTubeStatus& status) {
    const std::wstring pageToken = pageTokenIn;  // may be the same string as nextPageToken
    results.clear();
    nextPageToken.clear();
    error.clear();

    // The Data API when there is a key (its own page tokens), yt-dlp otherwise or
    // when the API fails
    bool ytdlpPage = pageToken.compare(0, 6, kPagePrefix) == 0;
    if (!g_ytApiKey.empty() && !ytdlpPage && SearchWithAPI(query, results, nextPageToken, pageToken)) {
        return true;
    }
    if (!pageToken.empty() && !ytdlpPage) return false;
    return ListWithYtdlp(L"ytsearch{end}:" + query, 25, pageToken, results, nextPageToken, error, status);
}

bool YouTubeGetListContents(const std::wstring& listUrl, std::vector<YouTubeResult>& results,
                            std::wstring& nextPageToken, const std::wstring& pageTokenIn, std::wstring& error,
                            const YouTubeStatus& status) {
    const std::wstring pageToken = pageTokenIn;  // may be the same string as nextPageToken
    results.clear();
    nextPageToken.clear();
    error.clear();
    return ListWithYtdlp(listUrl, 50, pageToken, results, nextPageToken, error, status);
}

std::wstring YouTubePlaylistUrl(const std::wstring& playlistId) {
    return L"https://www.youtube.com/playlist?list=" + playlistId;
}

std::wstring YouTubeChannelUrl(const std::wstring& channelId) {
    // Channel IDs are "UC" and 22 more characters; anything else is a @handle.
    if (IsChannelId(channelId)) {
        return L"https://www.youtube.com/channel/" + channelId + L"/videos";
    }
    return L"https://www.youtube.com/@" + channelId + L"/videos";
}

std::wstring YouTubeListUrl(YouTubeKind kind, const std::wstring& id) {
    return kind == YouTubeKind::Playlist ? YouTubePlaylistUrl(id) : YouTubeChannelUrl(id);
}

// A channel's home page URL turned into its Videos tab, which yt-dlp lists with the
// channel's ID (the home page gives only the @handle).
static std::wstring ChannelVideosUrl(const std::wstring& url) {
    bool channel = url.find(L"/@") != std::wstring::npos || url.find(L"/channel/") != std::wstring::npos ||
                   url.find(L"/c/") != std::wstring::npos || url.find(L"/user/") != std::wstring::npos;
    if (!channel) return url;
    std::wstring base = url.substr(0, url.find_first_of(L"?#"));
    while (!base.empty() && base.back() == L'/') base.pop_back();
    for (const wchar_t* tab : {L"/videos", L"/streams", L"/shorts", L"/playlists", L"/featured"}) {
        std::wstring suffix = tab;
        if (base.size() > suffix.size() && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
            base.resize(base.size() - suffix.size());
            break;
        }
    }
    return base + L"/videos";
}

bool YouTubeIdentify(const std::wstring& url, YouTubeListInfo& info, std::wstring& error,
                     const YouTubeStatus& status) {
    info = YouTubeListInfo();
    error.clear();
    YtdlpRun run;
    if (!RunYtdlp({L"--flat-playlist", L"--playlist-items", L"0", L"--print",
                   L"playlist:%(id)s\t%(title)s\t%(channel_id|)s\t%(channel,uploader|)s", ChannelVideosUrl(url)},
                  false, run, error, status)) {
        return false;
    }
    auto rows = PrintedRows(run.output);
    if (rows.empty() || rows[0].size() < 4 || rows[0][0].empty()) {
        error = run.exitCode != 0 ? YtdlpError(run) : L"This is not a YouTube channel or playlist.";
        return false;
    }
    const auto& row = rows[0];
    if (IsChannelId(row[0]) || (!row[2].empty() && row[0] == row[2]) || row[0][0] == L'@') {
        // A channel page ("Name - Videos" is the tab's title; the channel's name is plainer)
        info.kind = YouTubeKind::Channel;
        info.id = IsChannelId(row[0]) ? row[0] : row[2];
        info.name = row[3].empty() ? row[1] : row[3];
        if (!IsChannelId(info.id)) {
            error = L"YouTube did not give this channel's ID.";
            return false;
        }
    } else {
        info.kind = YouTubeKind::Playlist;
        info.id = row[0];
        info.name = row[1];
        info.channel = row[3];
    }
    return true;
}

bool YouTubeVideoChannel(const std::wstring& videoId, YouTubeListInfo& info, std::wstring& error,
                         const YouTubeStatus& status) {
    info = YouTubeListInfo();
    error.clear();
    YtdlpRun run;
    if (!RunYtdlp({L"--no-playlist", L"--skip-download", L"--print", L"%(channel_id)s\t%(channel,uploader)s",
                   L"https://www.youtube.com/watch?v=" + videoId},
                  true, run, error, status)) {
        return false;
    }
    auto rows = PrintedRows(run.output);
    if (rows.empty() || rows[0].size() < 2 || !IsChannelId(rows[0][0])) {
        error = run.exitCode != 0 ? YtdlpError(run) : L"YouTube did not say which channel this video is from.";
        return false;
    }
    info.kind = YouTubeKind::Channel;
    info.id = rows[0][0];
    info.name = rows[0][1];
    return true;
}

// Text between <tag> and </tag> from `from` on, with XML's escapes decoded.
static std::string XmlText(const std::string& xml, const char* tag, size_t from, size_t before) {
    std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
    size_t start = xml.find(open, from);
    if (start == std::string::npos || start > before) return "";
    start += open.size();
    size_t stop = xml.find(close, start);
    if (stop == std::string::npos) return "";
    std::string text = xml.substr(start, stop - start), out;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] != '&') {
            out += text[i];
            continue;
        }
        size_t semi = text.find(';', i);
        if (semi == std::string::npos) {
            out += text[i];
            continue;
        }
        std::string entity = text.substr(i + 1, semi - i - 1);
        if (entity == "amp") out += '&';
        else if (entity == "lt") out += '<';
        else if (entity == "gt") out += '>';
        else if (entity == "quot") out += '"';
        else if (entity == "apos") out += '\'';
        else if (!entity.empty() && entity[0] == '#') {
            unsigned long code = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')
                                     ? std::strtoul(entity.c_str() + 2, nullptr, 16)
                                     : std::strtoul(entity.c_str() + 1, nullptr, 10);
            out += WideToUtf8(std::wstring(1, static_cast<wchar_t>(code)));
        } else {
            out += text.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

bool YouTubeReadFeed(YouTubeKind kind, const std::wstring& id, YouTubeListInfo& info, int64_t& published,
                     std::wstring* newestTitle) {
    published = 0;
    std::wstring url = L"https://www.youtube.com/feeds/videos.xml?" +
                       std::wstring(kind == YouTubeKind::Playlist ? L"playlist_id=" : L"channel_id=") + id;
    HttpOptions options;
    options.timeoutMs = 15000;
    HttpResult result = HttpGet(url, options);
    if (!result.completed || result.status != 200) return false;

    // The feed's own title, author and <published> (when the channel or playlist
    // was made) come before the first <entry>; each entry has its video's date.
    const std::string& xml = result.body;
    size_t firstEntry = xml.find("<entry>");
    info.kind = kind;
    info.id = id;
    info.name = Utf8ToWide(XmlText(xml, "title", 0, firstEntry));
    info.channel = kind == YouTubeKind::Playlist ? Utf8ToWide(XmlText(xml, "name", 0, firstEntry)) : L"";
    for (size_t entry = firstEntry; entry != std::string::npos; entry = xml.find("<entry>", entry + 7)) {
        size_t end = xml.find("</entry>", entry);
        int64_t when = ParseIsoTime(XmlText(xml, "published", entry, end));
        if (when > published) {
            published = when;
            if (newestTitle) *newestTitle = Utf8ToWide(XmlText(xml, "title", entry, end));
        }
    }
    return true;
}

bool YouTubeLatestUpload(YouTubeKind kind, const std::wstring& id, int64_t& published) {
    YouTubeListInfo info;
    return YouTubeReadFeed(kind, id, info, published);
}

bool YouTubeResolveFavorite(const std::wstring& text, YouTubeListInfo& info, int64_t& published,
                            std::wstring& error, const YouTubeStatus& status) {
    info = YouTubeListInfo();
    published = 0;
    error.clear();

    std::wstring line = text;
    size_t first = line.find_first_not_of(L" \t\r\n\"'<>");
    size_t last = line.find_last_not_of(L" \t\r\n\"'<>");
    if (first == std::wstring::npos) {
        error = L"The line is empty.";
        return false;
    }
    line = line.substr(first, last - first + 1);

    // An ID can be read straight from the line: then the feed says the rest.
    auto idAfter = [&line](const wchar_t* marker) {
        size_t pos = line.find(marker);
        if (pos == std::wstring::npos) return std::wstring();
        pos += wcslen(marker);
        return line.substr(pos, line.find_first_of(L"/?&#", pos) - pos);
    };
    YouTubeKind kind = YouTubeKind::Channel;
    std::wstring id;
    if (IsChannelId(line)) {
        id = line;
    } else if (!(id = idAfter(L"list=")).empty()) {
        kind = YouTubeKind::Playlist;
    } else {
        id = idAfter(L"/channel/");
    }
    if (!id.empty() && (kind == YouTubeKind::Playlist || IsChannelId(id)) && YouTubeReadFeed(kind, id, info, published) &&
        !info.name.empty()) {
        return true;
    }

    // A @handle or custom URL: yt-dlp finds the channel.
    std::wstring url = line;
    if (url[0] == L'@') {
        url = L"https://www.youtube.com/" + url;
    } else if (url.find(L"://") == std::wstring::npos) {
        url = L"https://" + url;
    }
    if (!IsYouTubeURL(url)) {
        error = L"Not a YouTube channel or playlist.";
        return false;
    }
    if (!YouTubeIdentify(url, info, error, status)) return false;
    YouTubeListInfo feed;
    YouTubeReadFeed(info.kind, info.id, feed, published);
    return true;
}

bool YouTubePrepare(const std::wstring& videoId, YouTubeMedia& media, std::wstring& error,
	const YouTubeStatus& status) {
	const auto tools = GetYouTubeToolSettings();
	media = YouTubeMedia();
	error.clear();
	std::wstring url = L"https://www.youtube.com/watch?v=" + videoId;

	// Stream it: YouTube's HLS audio (234, 233), or for a live stream the
	// smallest HLS video, whose audio FFmpeg plays as it arrives, with seeking.
	YtdlpRun run;
	if (!RunYtdlp({L"--no-playlist", L"-f", L"234/233/93/92/91/94/95", L"--print",
		kMediaFormat, url}, true, run, error, status, tools)) {
		return false;
	}
	if (run.exitCode == 0 && ReadPreparedMedia(run.output, media) && !media.url.empty()) {
		media.metadata.sourceUrl = url;
		return true;
	}
	if (run.errors.find("Requested format is not available") == std::string::npos) {
		error = YtdlpError(run);
		return false;
	}

	// Nothing to stream: download the AAC audio and play it from the file.
	std::wstring cached = FindDownloaded(videoId);
	if (!cached.empty()) {
		std::error_code ec;
		fs::last_write_time(fs::path(cached), fs::file_time_type::clock::now(), ec);  // keep it another week
		// The older cached files contain only title and channel tags. Refresh
		// service metadata without downloading their audio again; a lookup
		// failure still leaves the cached file playable.
		YtdlpRun lookup;
		std::wstring lookupError;
		if (RunYtdlp({L"--no-playlist", L"--skip-download", L"-f", L"140/bestaudio[ext=m4a]",
			L"--print", kMediaFormat, url}, true, lookup, lookupError, status, tools) && lookup.exitCode == 0) {
			ReadPreparedMedia(lookup.output, media);
		}
		media.url.clear();
		media.file = cached;
		media.metadata.sourceUrl = url;
		return true;
	}

	std::wstring download = CacheDir() + videoId + L".download.m4a";
	std::error_code ec;
	fs::remove(fs::path(download), ec);
	run = YtdlpRun();
	if (!RunYtdlp({L"--no-playlist", L"-f", L"140/bestaudio[ext=m4a]", L"--fixup", L"never", L"--no-part",
		L"--no-mtime", L"-o", download, L"--no-simulate", L"--print", kMediaFormat, url},
		true, run, error, status, tools)) {
		return false;
	}
	if (run.exitCode != 0 || !FileExists(download) || !ReadPreparedMedia(run.output, media)) {
		fs::remove(fs::path(download), ec);
		error = run.exitCode != 0 ? YtdlpError(run) : L"YouTube gave no audio for this video.";
		return false;
	}
	media.url.clear();
	media.metadata.sourceUrl = url;
	std::wstring file = CacheDir() + FileNameFrom(media.title) + L" [" + videoId + L"].m4a";
	if (!DefragmentMp4(download, file, media.title, media.channel)) {
		// Not fragmented after all: it plays as it is.
		fs::remove(fs::path(file), ec);
		fs::rename(fs::path(download), fs::path(file), ec);
		if (ec) {
			error = L"Could not save the downloaded audio.";
			return false;
		}
	}
	fs::remove(fs::path(download), ec);
	media.file = file;
	return true;
}

bool YouTubeImportCookies(const std::wstring& path, std::wstring& error) {
    FILE* in = FileOpen(path, "rb");
    if (!in) {
        error = L"Could not open " + path;
        return false;
    }
    std::string text;
    char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) text.append(buffer, n);
    fclose(in);

    // Netscape format: tab-separated lines, as "Get cookies.txt"-style extensions export.
    bool youtube = text.find("youtube.com\t") != std::string::npos;
    if (!youtube) {
        error = L"This file has no YouTube cookies in cookies.txt (Netscape) format. Export the cookies for "
                L"youtube.com from your browser while signed in, and import that file.";
        return false;
    }
    // yt-dlp wants the format's header line
    if (text.compare(0, 1, "#") != 0) text = "# Netscape HTTP Cookie File\n" + text;

    FILE* out = FileOpen(CookiesPath(), "wb");
    if (!out || fwrite(text.data(), 1, text.size(), out) != text.size()) {
        if (out) fclose(out);
        error = L"Could not save the cookies in FastPlay's data folder.";
        return false;
    }
    fclose(out);
    return true;
}

void YouTubeRemoveCookies() {
    std::error_code ec;
    fs::remove(fs::path(CookiesPath()), ec);
}

bool YouTubeHasCookies() {
    return FileExists(CookiesPath());
}

// ---------------------------------------------------------------------------
// Downloads to keep
// ---------------------------------------------------------------------------

namespace {

struct DownloadJob {
    std::wstring url;
    std::wstring title;
    YouTubeDownloadSettings settings;  // as they were when it was asked for
	YouTubeToolSettings tools;
};

std::mutex g_downloadMutex;
std::deque<DownloadJob> g_downloadQueue;
bool g_downloadRunning = false;

void SpeakLater(const std::wstring& message) {
    RunOnUiThread([message]() { SpeakW(message); });
}

// Whether a download with these settings needs ffmpeg: converting, combining
// video with audio, or writing tags, thumbnails or subtitles into the file.
bool NeedsFfmpeg(const YouTubeDownloadSettings& s) {
    return s.type == 1 || s.audioFormat >= 2 || s.addMetadata || s.embedThumbnail;
}

// Options typed as on a command line: split at spaces, "quoted" parts kept whole.
std::vector<std::wstring> SplitOptions(const std::wstring& text) {
    std::vector<std::wstring> words;
    std::wstring word;
    bool quoted = false, any = false;
    for (wchar_t c : text) {
        if (c == L'"') {
            quoted = !quoted;
            any = true;
        } else if (!quoted && (c == L' ' || c == L'\t')) {
            if (any) words.push_back(word);
            word.clear();
            any = false;
        } else {
            word += c;
            any = true;
        }
    }
    if (any) words.push_back(word);
    return words;
}

std::vector<std::wstring> DownloadArgs(const DownloadJob& job, const std::wstring& folder, const std::wstring& ffmpeg,
                                       bool haveFfmpeg) {
    const YouTubeDownloadSettings& s = job.settings;
    bool playlist = job.url.find(L"list=") != std::wstring::npos && job.url.find(L"watch?v=") == std::wstring::npos;
    std::vector<std::wstring> args = {playlist ? L"--yes-playlist" : L"--no-playlist", L"-P", folder};

    static const wchar_t* const names[] = {L"%(title)s.%(ext)s", L"%(title)s [%(id)s].%(ext)s",
                                           L"%(channel,uploader)s - %(title)s.%(ext)s",
                                           L"%(upload_date>%Y-%m-%d)s - %(title)s.%(ext)s"};
    std::wstring name = names[std::clamp(s.naming, 0, 3)];
    if (playlist) name = L"%(playlist_title,playlist)s/" + name;
    if (s.channelFolder) name = L"%(channel,uploader)s/" + name;
    args.insert(args.end(), {L"-o", name});

    if (s.type == 1) {
        // Video: the best picture up to the chosen size, with the best sound.
        static const wchar_t* const heights[] = {L"", L"2160", L"1440", L"1080", L"720", L"480", L"360"};
        static const wchar_t* const codecs[] = {L"", L"h264", L"vp9", L"av01"};
        static const wchar_t* const containers[] = {L"mp4", L"mkv", L"webm"};
        std::wstring sort;
        auto add = [&sort](const std::wstring& part) { sort += (sort.empty() ? L"" : L",") + part; };
        if (s.videoQuality > 0) add(std::wstring(L"res:") + heights[std::clamp(s.videoQuality, 0, 6)]);
        if (s.videoCodec > 0) add(std::wstring(L"vcodec:") + codecs[std::clamp(s.videoCodec, 0, 3)]);
        if (s.videoContainer == 0) add(L"ext:mp4:m4a");
        if (s.videoContainer == 2) add(L"ext:webm:webm");
        args.insert(args.end(), {L"-f", L"bv*+ba/b"});
        if (!sort.empty()) args.insert(args.end(), {L"-S", sort});
        args.insert(args.end(), {L"--merge-output-format", containers[std::clamp(s.videoContainer, 0, 2)]});
        if (s.embedSubtitles) args.push_back(L"--embed-subs");
    } else if (s.audioFormat == 0) {
        // M4A: YouTube's own AAC, without converting (and repaired below if need be)
        args.insert(args.end(), {L"-f", L"bestaudio[ext=m4a]/bestaudio"});
        if (!haveFfmpeg) args.insert(args.end(), {L"--fixup", L"never"});
    } else if (s.audioFormat == 1) {
        args.insert(args.end(), {L"-f", L"bestaudio/best"});
    } else {
        static const wchar_t* const formats[] = {L"", L"", L"mp3", L"opus", L"flac", L"wav"};
        static const wchar_t* const qualities[] = {L"0", L"320K", L"256K", L"192K", L"128K"};
        args.insert(args.end(), {L"-f", L"bestaudio/best", L"-x", L"--audio-format",
                                 formats[std::clamp(s.audioFormat, 2, 5)], L"--audio-quality",
                                 qualities[std::clamp(s.audioQuality, 0, 4)]});
    }

    if (s.addMetadata) args.push_back(L"--embed-metadata");
    if (s.embedThumbnail) args.push_back(L"--embed-thumbnail");
    if (s.writeThumbnail) args.push_back(L"--write-thumbnail");
    if (s.writeDescription) args.push_back(L"--write-description");
    if (s.writeSubtitles || (s.type == 1 && s.embedSubtitles)) {
        args.insert(args.end(), {L"--write-subs", L"--write-auto-subs", L"--sub-langs", L"en.*"});
    }
    if (haveFfmpeg && !ffmpeg.empty()) args.insert(args.end(), {L"--ffmpeg-location", ffmpeg});
    for (const auto& word : SplitOptions(s.extraOptions)) args.push_back(word);
    // Say where each file ended up, and actually download while doing it
    args.insert(args.end(), {L"--no-simulate", L"--print", L"after_move:filepath", job.url});
    return args;
}

void RunDownload(const DownloadJob& job) {
    auto fail = [&job](const std::wstring& why) {
        RunOnUiThread([job, why]() {
            SpeakW(L"Could not download " + job.title);
            ShowMessage(L"Could not download " + job.title + L".\n\n" + why, L"YouTube Download", MessageIcon::Error);
        });
    };

    std::wstring folder = job.settings.folder.empty() ? YouTubeDownloadFolder() : job.settings.folder;
    std::error_code ec;
    fs::create_directories(fs::path(folder), ec);

    std::wstring ffmpeg, error;
    bool haveFfmpeg = false;
    if (NeedsFfmpeg(job.settings)) {
        if (!EnsureFfmpeg(ffmpeg, error, SpeakLater, job.tools)) return fail(error);
        haveFfmpeg = true;
    }

    YtdlpRun run;
    if (!RunYtdlp(DownloadArgs(job, folder, ffmpeg, haveFfmpeg), true, run, error, SpeakLater, job.tools)) return fail(error);
    if (run.exitCode != 0) return fail(YtdlpError(run));

    // M4A straight from YouTube is fragmented, which not every player reads;
    // without ffmpeg to repair it, rewrite it as an ordinary M4A here.
    std::istringstream lines(run.output);
    std::string line;
    int files = 0;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        files++;
        std::wstring path = Utf8ToWide(line);
        if (path.size() > 4 && WStrICmp(path.c_str() + path.size() - 4, L".m4a") == 0) {
            std::wstring fixed = path + L".fixed.m4a";
            if (DefragmentMp4(path, fixed, L"", L"")) {
                fs::remove(fs::path(path), ec);
                fs::rename(fs::path(fixed), fs::path(path), ec);
            } else {
                fs::remove(fs::path(fixed), ec);
            }
        }
    }
    std::wstring done = L"Downloaded " + job.title;
    if (files > 1) done += L", " + std::to_wstring(files) + L" files";
    SpeakLater(done);
}

void DownloadWorker() {
    for (;;) {
        DownloadJob job;
        {
            std::lock_guard<std::mutex> lock(g_downloadMutex);
            if (g_downloadQueue.empty()) {
                g_downloadRunning = false;
                return;
            }
            job = g_downloadQueue.front();
            g_downloadQueue.pop_front();
        }
        SpeakLater(L"Downloading " + job.title);
        RunDownload(job);
    }
}

}  // namespace

std::wstring YouTubeDownloadFolder() {
    if (!g_ytDownload.folder.empty()) return g_ytDownload.folder;
    std::wstring base = GetUserDownloadsDir();
    if (base.empty()) base = GetUserMusicDir();
    return base + kPathSeparator + L"FastPlay";
}

void YouTubeDownload(const std::wstring& url, const std::wstring& title) {
	DownloadJob job{url, title, g_ytDownload, GetYouTubeToolSettings()};
    if (job.settings.folder.empty()) job.settings.folder = YouTubeDownloadFolder();
    size_t ahead;
    bool start;
    {
        std::lock_guard<std::mutex> lock(g_downloadMutex);
        ahead = g_downloadQueue.size() + (g_downloadRunning ? 1 : 0);
        g_downloadQueue.push_back(job);
        start = !g_downloadRunning;
        g_downloadRunning = true;
    }
    if (start) {
        std::thread(DownloadWorker).detach();
    } else {
        SpeakW(L"Queued " + title + L", " + std::to_wstring(ahead) + L" ahead");
    }
}

void YouTubeCleanup() {
    std::error_code ec;
    auto now = fs::file_time_type::clock::now();
    for (const auto& entry : fs::directory_iterator(fs::path(CacheDir()), ec)) {
        std::error_code entryError;
        auto modified = fs::last_write_time(entry.path(), entryError);
        std::wstring name = entry.path().filename().wstring();
        bool partial = name.find(L".download.") != std::wstring::npos;
        if (!entryError && (partial || now - modified > std::chrono::hours(24 * 7))) {
            fs::remove(entry.path(), entryError);
        }
    }
}

// Check if input is a YouTube URL
bool IsYouTubeURL(const std::wstring& input) {
    return input.find(L"youtube.com") != std::wstring::npos ||
           input.find(L"youtu.be") != std::wstring::npos;
}

// Parse YouTube URL
bool ParseYouTubeURL(const std::wstring& url, std::wstring& id, bool& isPlaylist, bool& isChannel) {
    isPlaylist = false;
    isChannel = false;
    id.clear();

    // Check for playlist
    size_t listPos = url.find(L"list=");
    if (listPos != std::wstring::npos) {
        size_t start = listPos + 5;
        size_t end = url.find_first_of(L"&# ", start);
        id = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        isPlaylist = true;
        return !id.empty();
    }

    // Check for channel
    if (url.find(L"/channel/") != std::wstring::npos || url.find(L"/@") != std::wstring::npos) {
        isChannel = true;
        // Extract channel ID or handle
        size_t pos = url.find(L"/channel/");
        if (pos != std::wstring::npos) {
            pos += 9;
        } else {
            pos = url.find(L"/@");
            if (pos != std::wstring::npos) pos += 2;
        }
        if (pos != std::wstring::npos) {
            size_t end = url.find_first_of(L"/?# ", pos);
            id = url.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
            return !id.empty();
        }
    }

    // Check for video ID
    size_t vPos = url.find(L"v=");
    if (vPos != std::wstring::npos) {
        size_t start = vPos + 2;
        size_t end = url.find_first_of(L"&# ", start);
        id = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        return !id.empty();
    }

    // youtu.be format
    size_t bePos = url.find(L"youtu.be/");
    if (bePos != std::wstring::npos) {
        size_t start = bePos + 9;
        size_t end = url.find_first_of(L"?# ", start);
        id = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        return !id.empty();
    }

    return false;
}
