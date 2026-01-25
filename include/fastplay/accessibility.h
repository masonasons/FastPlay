#pragma once
#ifndef FASTPLAY_ACCESSIBILITY_H
#define FASTPLAY_ACCESSIBILITY_H

#include <windows.h>
#include <string>

// Speech initialization (Universal Speech)
bool InitSpeech(HWND hwnd);
void FreeSpeech();

// Speech output
void Speak(const char* text, bool interrupt = true);
void Speak(const std::string& text, bool interrupt = true);
void DoSpeak();

#endif // FASTPLAY_ACCESSIBILITY_H
