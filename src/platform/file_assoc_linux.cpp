// File associations on Linux: a desktop entry for FastPlay in the user's
// applications folder (~/.local/share/applications/fastplay.desktop), naming the
// MIME types of the extensions in g_fileAssocs, and FastPlay made the default app
// for each of them (~/.config/mimeapps.list), as the desktop's own "Open With"
// settings would.

#include "file_assoc.h"
#include "globals.h"
#include "utils.h"

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <unistd.h>

#include <climits>
#include <cstring>
#include <set>
#include <string>

static const char kDesktopId[] = "fastplay.desktop";

static std::string DesktopFilePath() {
    return std::string(g_get_user_data_dir()) + "/applications/" + kDesktopId;
}

// The MIME types of FastPlay's extensions, as the shared MIME database knows them
static std::set<std::string> MimeTypes() {
    std::set<std::string> types;
    for (int i = 0; i < g_fileAssocCount; i++) {
        std::string ext = WideToUtf8(g_fileAssocs[i].ext);
        if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
        // (An extension of more than one type, as Ogg's, is "uncertain"; the type
        // guessed is still the one for it)
        gchar* type = g_content_type_guess(("file." + ext).c_str(), nullptr, 0, nullptr);
        if (type) {
            gchar* mime = g_content_type_get_mime_type(type);
            // Unknown extensions come back as just "some data"
            if (mime && strcmp(mime, "application/octet-stream") != 0) types.insert(mime);
            g_free(mime);
            g_free(type);
        }
    }
    // A file manager looks inside an Ogg file and calls it by its codec
    if (types.count("audio/ogg")) {
        for (const char* codec : {"audio/x-vorbis+ogg", "audio/x-opus+ogg", "audio/x-flac+ogg", "audio/x-speex+ogg"}) {
            types.insert(codec);
        }
    }
    return types;
}

// The desktop entry's Exec: the path quoted as the specification says
static std::string QuoteExec(const std::string& path) {
    std::string quoted = "\"";
    for (char c : path) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') quoted += '\\';
        quoted += c;
    }
    return quoted + "\"";
}

void RegisterAllFileTypes() {
    std::set<std::string> types = MimeTypes();
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    exe[n] = 0;
    std::string entry =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=FastPlay\n"
        "GenericName=Audio Player\n"
        "Comment=Play audio files, streams and podcasts\n"
        "Exec=" + QuoteExec(exe) + " %F\n"
        "Terminal=false\n"
        "Categories=AudioVideo;Audio;Player;\n"
        "MimeType=";
    for (const std::string& type : types) entry += type + ";";
    entry += "\n";

    std::string path = DesktopFilePath();
    g_mkdir_with_parents((std::string(g_get_user_data_dir()) + "/applications").c_str(), 0755);
    if (!g_file_set_contents(path.c_str(), entry.c_str(), static_cast<gssize>(entry.size()), nullptr)) return;

    GDesktopAppInfo* app = g_desktop_app_info_new_from_filename(path.c_str());
    if (!app) return;
    for (const std::string& type : types) {
        g_app_info_set_as_default_for_type(G_APP_INFO(app), type.c_str(), nullptr);
    }
    g_object_unref(app);
}

void UnregisterAllFileTypes() {
    // Each type FastPlay is the default for goes back to the system's choice;
    // the desktop entry goes, so nothing can choose FastPlay by it.
    for (const std::string& type : MimeTypes()) {
        GAppInfo* current = g_app_info_get_default_for_type(type.c_str(), FALSE);
        if (current) {
            const char* id = g_app_info_get_id(current);
            if (id && strcmp(id, kDesktopId) == 0) g_app_info_reset_type_associations(type.c_str());
            g_object_unref(current);
        }
    }
    g_remove(DesktopFilePath().c_str());
}
