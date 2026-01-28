#include "accessibility.h"
#include "globals.h"
#include "resource.h"
#include <string>

#ifdef USE_UNIVERSAL_SPEECH
#include "UniversalSpeech.h"

static std::string g_pendingSpeech;
static std::wstring g_pendingSpeechW;
static bool g_speechInterrupt = true;
static bool g_speechInitialized = false;
static bool g_useWideString = false;

void DoSpeak() {
    if (!g_speechInitialized) return;

    if (g_useWideString && !g_pendingSpeechW.empty()) {
        if (g_speechInterrupt) {
            speechStop();
        }
        speechSay(g_pendingSpeechW.c_str(), g_speechInterrupt ? 1 : 0);
        g_pendingSpeechW.clear();
        g_useWideString = false;
    } else if (!g_pendingSpeech.empty()) {
        if (g_speechInterrupt) {
            speechStop();
        }
        speechSayA(g_pendingSpeech.c_str(), g_speechInterrupt ? 1 : 0);
        g_pendingSpeech.clear();
    }
}

void Speak(const char* text, bool interrupt) {
    if (g_speechInitialized && g_hwnd) {
        g_pendingSpeech = text;
        g_useWideString = false;
        g_speechInterrupt = interrupt;
        PostMessage(g_hwnd, WM_SPEAK, 0, 0);
    }
}

void Speak(const std::string& text, bool interrupt) {
    Speak(text.c_str(), interrupt);
}

void SpeakW(const wchar_t* text, bool interrupt) {
    if (g_speechInitialized && g_hwnd) {
        g_pendingSpeechW = text;
        g_useWideString = true;
        g_speechInterrupt = interrupt;
        PostMessage(g_hwnd, WM_SPEAK, 0, 0);
    }
}

void SpeakW(const std::wstring& text, bool interrupt) {
    SpeakW(text.c_str(), interrupt);
}

bool InitSpeech(HWND hwnd) {
    (void)hwnd;  // Not needed for Universal Speech
    g_speechInitialized = true;
    return true;
}

void FreeSpeech() {
    if (g_speechInitialized) {
        speechStop();
        g_speechInitialized = false;
    }
}

#else

void DoSpeak() {}
void Speak(const char*, bool) {}
void Speak(const std::string&, bool) {}
void SpeakW(const wchar_t*, bool) {}
void SpeakW(const std::wstring&, bool) {}
bool InitSpeech(HWND) { return false; }
void FreeSpeech() {}

#endif
