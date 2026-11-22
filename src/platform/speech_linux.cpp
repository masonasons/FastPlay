// Linux speech through Speech Dispatcher, which Orca speaks through as well, so
// FastPlay's announcements come in the voice and at the rate the user set there.
// Interrupting cancels only what FastPlay said, never the screen reader.

#include "accessibility.h"
#include "utils.h"

#include <libspeechd.h>

#include <mutex>
#include <string>

static std::mutex g_speechMutex;
static SPDConnection* g_connection = nullptr;

void SpeakW(const wchar_t* text, bool interrupt) {
    Speak(WideToUtf8(text), interrupt);
}

void SpeakW(const std::wstring& text, bool interrupt) {
    Speak(WideToUtf8(text), interrupt);
}

void Speak(const char* text, bool interrupt) {
    std::lock_guard<std::mutex> lock(g_speechMutex);
    if (!g_connection || !text || !*text) return;
    if (interrupt) spd_cancel(g_connection);
    spd_say(g_connection, SPD_TEXT, text);
}

void Speak(const std::string& text, bool interrupt) {
    Speak(text.c_str(), interrupt);
}

bool InitSpeech() {
    std::lock_guard<std::mutex> lock(g_speechMutex);
    if (g_connection) return true;
    char* error = nullptr;
    // Threaded, so a call never waits for the reply to the one before it; the
    // daemon is started if it is not running.
    g_connection = spd_open2("FastPlay", "main", nullptr, SPD_MODE_THREADED, nullptr, 1, &error);
    free(error);
    return g_connection != nullptr;
}

void FreeSpeech() {
    std::lock_guard<std::mutex> lock(g_speechMutex);
    if (g_connection) {
        spd_close(g_connection);
        g_connection = nullptr;
    }
}
