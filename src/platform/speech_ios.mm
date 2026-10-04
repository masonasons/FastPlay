// iOS speech: while VoiceOver is running, text is given to it as an announcement,
// so it is spoken in the user's voice and settings. With VoiceOver off nothing is
// spoken (the screen shows it), as on Windows with no screen reader running.

#include "accessibility.h"
#include "app_ui.h"
#include "utils.h"

#import <UIKit/UIKit.h>

#include <mutex>
#include <string>

static std::mutex g_speechMutex;
static std::wstring g_pendingSpeechW;
static bool g_speechInterrupt = true;
static bool g_speechInitialized = false;
static bool g_speechMuted = false;

// The app mutes speech around a change that VoiceOver reads out by itself (a
// control's new value), so it is not said twice.
void SetSpeechMuted(bool muted) {
    std::lock_guard<std::mutex> lock(g_speechMutex);
    g_speechMuted = muted;
}

bool IsSpeechMuted() {
    std::lock_guard<std::mutex> lock(g_speechMutex);
    return g_speechMuted;
}

// Speak whatever is pending. Runs on the UI thread.
static void DoSpeak() {
    std::wstring text;
    bool interrupt;
    {
        std::lock_guard<std::mutex> lock(g_speechMutex);
        if (!g_speechInitialized || g_pendingSpeechW.empty()) return;
        text.swap(g_pendingSpeechW);
        interrupt = g_speechInterrupt;
    }
    if (!UIAccessibilityIsVoiceOverRunning()) return;
    NSString* message = [NSString stringWithUTF8String:WideToUtf8(text).c_str()];
    if (message.length == 0) return;
    // High priority cuts off what VoiceOver is saying; default waits its turn.
    NSAttributedString* announcement = [[NSAttributedString alloc]
        initWithString:message
            attributes:@{
                UIAccessibilitySpeechAttributeAnnouncementPriority:
                    interrupt ? UIAccessibilityPriorityHigh : UIAccessibilityPriorityDefault
            }];
    UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, announcement);
}

void SpeakW(const wchar_t* text, bool interrupt) {
    {
        std::lock_guard<std::mutex> lock(g_speechMutex);
        if (!g_speechInitialized || g_speechMuted) return;
        g_pendingSpeechW = text;
        g_speechInterrupt = interrupt;
    }
    RunOnUiThread(DoSpeak);
}

void SpeakW(const std::wstring& text, bool interrupt) {
    SpeakW(text.c_str(), interrupt);
}

void Speak(const char* text, bool interrupt) {
    SpeakW(Utf8ToWide(text), interrupt);
}

void Speak(const std::string& text, bool interrupt) {
    Speak(text.c_str(), interrupt);
}

bool InitSpeech() {
    std::lock_guard<std::mutex> lock(g_speechMutex);
    g_speechInitialized = true;
    return true;
}

void FreeSpeech() {
    std::lock_guard<std::mutex> lock(g_speechMutex);
    g_speechInitialized = false;
}
