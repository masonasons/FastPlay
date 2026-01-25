#pragma once
#ifndef FASTPLAY_DATABASE_H
#define FASTPLAY_DATABASE_H

#include <string>
#include <vector>

// Bookmark structure
struct Bookmark {
    int id;
    std::wstring filePath;
    double position;
    std::wstring displayName;  // filename + position for display
    int64_t timestamp;
};

// Radio station structure
struct RadioStation {
    int id;
    std::wstring name;
    std::wstring url;
    int64_t timestamp;
};

// Schedule action type
enum class ScheduleAction {
    Playback = 0,
    Recording = 1,
    Both = 2
};

// Schedule source type
enum class ScheduleSource {
    File = 0,
    Radio = 1
};

// Schedule repeat type
enum class ScheduleRepeat {
    None = 0,
    Daily = 1,
    Weekly = 2,
    Weekdays = 3,
    Weekends = 4,
    Monthly = 5
};

// What to stop when duration expires (for "Both" action)
enum class ScheduleStopAction {
    StopBoth = 0,
    StopPlayback = 1,
    StopRecording = 2
};

// Scheduled event structure
struct ScheduledEvent {
    int id;
    std::wstring name;
    ScheduleAction action;
    ScheduleSource sourceType;
    std::wstring sourcePath;    // File path or radio station URL
    int radioStationId;         // Radio station ID (0 if file)
    int64_t scheduledTime;      // Unix timestamp of next scheduled run
    ScheduleRepeat repeat;
    bool enabled;
    int64_t lastRun;            // Unix timestamp of last run
    int duration;               // Duration in minutes (0 = no limit)
    ScheduleStopAction stopAction;  // What to stop when duration expires
    std::wstring displayName;   // For display in list
};

// Initialize database (call once at startup)
bool InitDatabase();

// Close database (call at shutdown)
void CloseDatabase();

// File position operations
void SaveFilePositionDB(const std::wstring& filePath, double position);
double LoadFilePositionDB(const std::wstring& filePath);

// Bookmark operations
int AddBookmark(const std::wstring& filePath, double position);
bool RemoveBookmark(int id);
std::vector<Bookmark> GetAllBookmarks();

// Radio station operations
int AddRadioStation(const std::wstring& name, const std::wstring& url);
bool RemoveRadioStation(int id);
bool RenameRadioStation(int id, const std::wstring& newName);
bool UpdateRadioStation(int id, const std::wstring& newName, const std::wstring& newUrl);
std::vector<RadioStation> GetRadioFavorites();

// Schedule operations
int AddScheduledEvent(const std::wstring& name, ScheduleAction action,
                      ScheduleSource sourceType, const std::wstring& sourcePath,
                      int radioStationId, int64_t scheduledTime,
                      ScheduleRepeat repeat, bool enabled,
                      int duration, ScheduleStopAction stopAction);
bool RemoveScheduledEvent(int id);
bool UpdateScheduledEventEnabled(int id, bool enabled);
bool UpdateScheduledEventLastRun(int id, int64_t lastRun);
bool UpdateScheduledEventTime(int id, int64_t scheduledTime);
std::vector<ScheduledEvent> GetAllScheduledEvents();
std::vector<ScheduledEvent> GetPendingScheduledEvents();

#endif // FASTPLAY_DATABASE_H
