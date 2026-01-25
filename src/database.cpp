#include "database.h"
#include "sqlite3.h"
#include "utils.h"
#include <windows.h>
#include <ctime>
#include <vector>

static sqlite3* g_db = nullptr;
static std::wstring g_dbPath;

// Initialize database
bool InitDatabase() {
    if (g_db) return true;  // Already initialized

    // Get database path (same directory as exe)
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    g_dbPath = exePath;
    size_t pos = g_dbPath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        g_dbPath = g_dbPath.substr(0, pos + 1);
    }
    g_dbPath += L"FastPlay.db";

    // Open database (UTF-8 path)
    std::string dbPathUtf8 = WideToUtf8(g_dbPath);
    int rc = sqlite3_open(dbPathUtf8.c_str(), &g_db);
    if (rc != SQLITE_OK) {
        sqlite3_close(g_db);
        g_db = nullptr;
        return false;
    }

    // Set busy timeout to prevent hangs (5 seconds)
    sqlite3_busy_timeout(g_db, 5000);

    // Enable WAL mode for better concurrency
    char* errMsg = nullptr;
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &errMsg);
    if (errMsg) sqlite3_free(errMsg);

    // Create tables if not exists
    const char* sql =
        "CREATE TABLE IF NOT EXISTS file_positions ("
        "  path TEXT PRIMARY KEY,"
        "  position REAL,"
        "  last_updated INTEGER"
        ");";

    errMsg = nullptr;
    rc = sqlite3_exec(g_db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
        sqlite3_close(g_db);
        g_db = nullptr;
        return false;
    }

    // Create bookmarks table
    const char* bookmarkSql =
        "CREATE TABLE IF NOT EXISTS bookmarks ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  path TEXT NOT NULL,"
        "  position REAL NOT NULL,"
        "  created INTEGER NOT NULL"
        ");";

    errMsg = nullptr;
    rc = sqlite3_exec(g_db, bookmarkSql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
        // Don't fail completely, just log
    }

    // Create radio favorites table
    const char* radioSql =
        "CREATE TABLE IF NOT EXISTS radio_favorites ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  url TEXT NOT NULL,"
        "  created INTEGER NOT NULL"
        ");";

    errMsg = nullptr;
    rc = sqlite3_exec(g_db, radioSql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
    }

    // Create scheduled events table
    const char* scheduleSql =
        "CREATE TABLE IF NOT EXISTS scheduled_events ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  action INTEGER NOT NULL,"
        "  source_type INTEGER NOT NULL,"
        "  source_path TEXT NOT NULL,"
        "  radio_station_id INTEGER DEFAULT 0,"
        "  scheduled_time INTEGER NOT NULL,"
        "  repeat_type INTEGER DEFAULT 0,"
        "  enabled INTEGER DEFAULT 1,"
        "  last_run INTEGER DEFAULT 0,"
        "  duration INTEGER DEFAULT 0,"
        "  stop_action INTEGER DEFAULT 0"
        ");";

    errMsg = nullptr;
    rc = sqlite3_exec(g_db, scheduleSql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
    }

    // Migration: Add duration and stop_action columns if they don't exist
    sqlite3_exec(g_db, "ALTER TABLE scheduled_events ADD COLUMN duration INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "ALTER TABLE scheduled_events ADD COLUMN stop_action INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);

    return true;
}

// Close database
void CloseDatabase() {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = nullptr;
    }
}

// Save file position to database
void SaveFilePositionDB(const std::wstring& filePath, double position) {
    if (!g_db) return;

    std::string pathUtf8 = WideToUtf8(filePath);

    const char* sql =
        "INSERT OR REPLACE INTO file_positions (path, position, last_updated) "
        "VALUES (?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, position);
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(time(nullptr)));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// Load file position from database
double LoadFilePositionDB(const std::wstring& filePath) {
    if (!g_db) return 0.0;

    std::string pathUtf8 = WideToUtf8(filePath);
    double position = 0.0;

    const char* sql = "SELECT position FROM file_positions WHERE path = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            position = sqlite3_column_double(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    return position;
}

// Add a bookmark, returns the bookmark ID or -1 on failure
int AddBookmark(const std::wstring& filePath, double position) {
    if (!g_db) return -1;

    std::string pathUtf8 = WideToUtf8(filePath);

    const char* sql =
        "INSERT INTO bookmarks (path, position, created) VALUES (?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, position);
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(time(nullptr)));

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return static_cast<int>(sqlite3_last_insert_rowid(g_db));
        }
        sqlite3_finalize(stmt);
    }
    return -1;
}

// Remove a bookmark by ID
bool RemoveBookmark(int id) {
    if (!g_db) return false;

    const char* sql = "DELETE FROM bookmarks WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Helper to extract filename from path
static std::wstring ExtractFilename(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

// Helper to format position as HH:MM:SS
static std::wstring FormatPosition(double seconds) {
    int totalSecs = static_cast<int>(seconds);
    int hours = totalSecs / 3600;
    int mins = (totalSecs % 3600) / 60;
    int secs = totalSecs % 60;

    wchar_t buf[32];
    if (hours > 0) {
        swprintf(buf, 32, L"%d:%02d:%02d", hours, mins, secs);
    } else {
        swprintf(buf, 32, L"%d:%02d", mins, secs);
    }
    return buf;
}

// Get all bookmarks
std::vector<Bookmark> GetAllBookmarks() {
    std::vector<Bookmark> bookmarks;
    if (!g_db) return bookmarks;

    const char* sql = "SELECT id, path, position, created FROM bookmarks ORDER BY created DESC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Bookmark bm;
            bm.id = sqlite3_column_int(stmt, 0);

            const char* pathUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            bm.filePath = Utf8ToWide(pathUtf8 ? pathUtf8 : "");

            bm.position = sqlite3_column_double(stmt, 2);
            bm.timestamp = sqlite3_column_int64(stmt, 3);

            // Create display name: filename @ position
            bm.displayName = ExtractFilename(bm.filePath) + L" @ " + FormatPosition(bm.position);

            bookmarks.push_back(bm);
        }
        sqlite3_finalize(stmt);
    }

    return bookmarks;
}

// Add a radio station, returns the station ID or -1 on failure
int AddRadioStation(const std::wstring& name, const std::wstring& url) {
    if (!g_db) return -1;

    std::string nameUtf8 = WideToUtf8(name);
    std::string urlUtf8 = WideToUtf8(url);

    const char* sql =
        "INSERT INTO radio_favorites (name, url, created) VALUES (?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nameUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, urlUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(time(nullptr)));

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return static_cast<int>(sqlite3_last_insert_rowid(g_db));
        }
        sqlite3_finalize(stmt);
    }
    return -1;
}

// Remove a radio station by ID
bool RemoveRadioStation(int id) {
    if (!g_db) return false;

    const char* sql = "DELETE FROM radio_favorites WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

bool RenameRadioStation(int id, const std::wstring& newName) {
    if (!g_db) return false;

    std::string nameUtf8 = WideToUtf8(newName);
    const char* sql = "UPDATE radio_favorites SET name = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nameUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

bool UpdateRadioStation(int id, const std::wstring& newName, const std::wstring& newUrl) {
    if (!g_db) return false;

    std::string nameUtf8 = WideToUtf8(newName);
    std::string urlUtf8 = WideToUtf8(newUrl);
    const char* sql = "UPDATE radio_favorites SET name = ?, url = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nameUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, urlUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Get all radio favorites
std::vector<RadioStation> GetRadioFavorites() {
    std::vector<RadioStation> stations;
    if (!g_db) return stations;

    const char* sql = "SELECT id, name, url, created FROM radio_favorites ORDER BY name COLLATE NOCASE ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            RadioStation rs;
            rs.id = sqlite3_column_int(stmt, 0);

            const char* nameUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            rs.name = Utf8ToWide(nameUtf8 ? nameUtf8 : "");

            const char* urlUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            rs.url = Utf8ToWide(urlUtf8 ? urlUtf8 : "");

            rs.timestamp = sqlite3_column_int64(stmt, 3);

            stations.push_back(rs);
        }
        sqlite3_finalize(stmt);
    }

    return stations;
}

// Helper to format schedule time
static std::wstring FormatScheduleTime(int64_t timestamp) {
    time_t t = static_cast<time_t>(timestamp);
    struct tm tm;
    localtime_s(&tm, &t);
    wchar_t buf[64];
    wcsftime(buf, 64, L"%Y-%m-%d %H:%M", &tm);
    return buf;
}

// Helper to get action name
static std::wstring GetActionName(ScheduleAction action) {
    switch (action) {
        case ScheduleAction::Playback: return L"Play";
        case ScheduleAction::Recording: return L"Record";
        case ScheduleAction::Both: return L"Play+Record";
        default: return L"Unknown";
    }
}

// Helper to get repeat name
static std::wstring GetRepeatName(ScheduleRepeat repeat) {
    switch (repeat) {
        case ScheduleRepeat::None: return L"Once";
        case ScheduleRepeat::Daily: return L"Daily";
        case ScheduleRepeat::Weekly: return L"Weekly";
        case ScheduleRepeat::Weekdays: return L"Weekdays";
        case ScheduleRepeat::Weekends: return L"Weekends";
        case ScheduleRepeat::Monthly: return L"Monthly";
        default: return L"Unknown";
    }
}

// Add a scheduled event
int AddScheduledEvent(const std::wstring& name, ScheduleAction action,
                      ScheduleSource sourceType, const std::wstring& sourcePath,
                      int radioStationId, int64_t scheduledTime,
                      ScheduleRepeat repeat, bool enabled,
                      int duration, ScheduleStopAction stopAction) {
    if (!g_db) return -1;

    std::string nameUtf8 = WideToUtf8(name);
    std::string pathUtf8 = WideToUtf8(sourcePath);

    const char* sql =
        "INSERT INTO scheduled_events (name, action, source_type, source_path, "
        "radio_station_id, scheduled_time, repeat_type, enabled, last_run, duration, stop_action) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nameUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, static_cast<int>(action));
        sqlite3_bind_int(stmt, 3, static_cast<int>(sourceType));
        sqlite3_bind_text(stmt, 4, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, radioStationId);
        sqlite3_bind_int64(stmt, 6, scheduledTime);
        sqlite3_bind_int(stmt, 7, static_cast<int>(repeat));
        sqlite3_bind_int(stmt, 8, enabled ? 1 : 0);
        sqlite3_bind_int(stmt, 9, duration);
        sqlite3_bind_int(stmt, 10, static_cast<int>(stopAction));

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return static_cast<int>(sqlite3_last_insert_rowid(g_db));
        }
        sqlite3_finalize(stmt);
    }
    return -1;
}

// Remove a scheduled event
bool RemoveScheduledEvent(int id) {
    if (!g_db) return false;

    const char* sql = "DELETE FROM scheduled_events WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Update enabled state
bool UpdateScheduledEventEnabled(int id, bool enabled) {
    if (!g_db) return false;

    const char* sql = "UPDATE scheduled_events SET enabled = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
        sqlite3_bind_int(stmt, 2, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Update last run time
bool UpdateScheduledEventLastRun(int id, int64_t lastRun) {
    if (!g_db) return false;

    const char* sql = "UPDATE scheduled_events SET last_run = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, lastRun);
        sqlite3_bind_int(stmt, 2, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Update scheduled time (for repeating events)
bool UpdateScheduledEventTime(int id, int64_t scheduledTime) {
    if (!g_db) return false;

    const char* sql = "UPDATE scheduled_events SET scheduled_time = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, scheduledTime);
        sqlite3_bind_int(stmt, 2, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Get all scheduled events
std::vector<ScheduledEvent> GetAllScheduledEvents() {
    std::vector<ScheduledEvent> events;
    if (!g_db) return events;

    const char* sql = "SELECT id, name, action, source_type, source_path, "
                      "radio_station_id, scheduled_time, repeat_type, enabled, last_run, "
                      "duration, stop_action "
                      "FROM scheduled_events ORDER BY scheduled_time ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ScheduledEvent ev;
            ev.id = sqlite3_column_int(stmt, 0);

            const char* nameUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            ev.name = Utf8ToWide(nameUtf8 ? nameUtf8 : "");

            ev.action = static_cast<ScheduleAction>(sqlite3_column_int(stmt, 2));
            ev.sourceType = static_cast<ScheduleSource>(sqlite3_column_int(stmt, 3));

            const char* pathUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            ev.sourcePath = Utf8ToWide(pathUtf8 ? pathUtf8 : "");

            ev.radioStationId = sqlite3_column_int(stmt, 5);
            ev.scheduledTime = sqlite3_column_int64(stmt, 6);
            ev.repeat = static_cast<ScheduleRepeat>(sqlite3_column_int(stmt, 7));
            ev.enabled = sqlite3_column_int(stmt, 8) != 0;
            ev.lastRun = sqlite3_column_int64(stmt, 9);
            ev.duration = sqlite3_column_int(stmt, 10);
            ev.stopAction = static_cast<ScheduleStopAction>(sqlite3_column_int(stmt, 11));

            // Build display name: [Enabled] Name - Action @ Time (Repeat) [Duration]
            std::wstring enabledStr = ev.enabled ? L"[On] " : L"[Off] ";
            ev.displayName = enabledStr + ev.name + L" - " + GetActionName(ev.action) +
                            L" @ " + FormatScheduleTime(ev.scheduledTime) +
                            L" (" + GetRepeatName(ev.repeat) + L")";
            if (ev.duration > 0) {
                wchar_t durBuf[32];
                swprintf(durBuf, 32, L" [%d min]", ev.duration);
                ev.displayName += durBuf;
            }

            events.push_back(ev);
        }
        sqlite3_finalize(stmt);
    }

    return events;
}

// Get pending events that should trigger now
std::vector<ScheduledEvent> GetPendingScheduledEvents() {
    std::vector<ScheduledEvent> events;
    if (!g_db) return events;

    int64_t now = static_cast<int64_t>(time(nullptr));

    // Get events where: enabled=1, scheduled_time <= now, and last_run < scheduled_time
    const char* sql = "SELECT id, name, action, source_type, source_path, "
                      "radio_station_id, scheduled_time, repeat_type, enabled, last_run, "
                      "duration, stop_action "
                      "FROM scheduled_events "
                      "WHERE enabled = 1 AND scheduled_time <= ? AND last_run < scheduled_time "
                      "ORDER BY scheduled_time ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ScheduledEvent ev;
            ev.id = sqlite3_column_int(stmt, 0);

            const char* nameUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            ev.name = Utf8ToWide(nameUtf8 ? nameUtf8 : "");

            ev.action = static_cast<ScheduleAction>(sqlite3_column_int(stmt, 2));
            ev.sourceType = static_cast<ScheduleSource>(sqlite3_column_int(stmt, 3));

            const char* pathUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            ev.sourcePath = Utf8ToWide(pathUtf8 ? pathUtf8 : "");

            ev.radioStationId = sqlite3_column_int(stmt, 5);
            ev.scheduledTime = sqlite3_column_int64(stmt, 6);
            ev.repeat = static_cast<ScheduleRepeat>(sqlite3_column_int(stmt, 7));
            ev.enabled = sqlite3_column_int(stmt, 8) != 0;
            ev.lastRun = sqlite3_column_int64(stmt, 9);
            ev.duration = sqlite3_column_int(stmt, 10);
            ev.stopAction = static_cast<ScheduleStopAction>(sqlite3_column_int(stmt, 11));

            events.push_back(ev);
        }
        sqlite3_finalize(stmt);
    }

    return events;
}
