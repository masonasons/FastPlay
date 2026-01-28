#pragma once
#ifndef FASTPLAY_ACCESSIBILITY_H
#define FASTPLAY_ACCESSIBILITY_H

#include <windows.h>
#include <string>

// Speech initialization (Universal Speech)
bool InitSpeech(HWND hwnd);
void FreeSpeech();

// Speech output (ANSI - for ASCII text)
void Speak(const char* text, bool interrupt = true);
void Speak(const std::string& text, bool interrupt = true);

// Speech output (Unicode - for ID3 tags, international text)
void SpeakW(const wchar_t* text, bool interrupt = true);
void SpeakW(const std::wstring& text, bool interrupt = true);

void DoSpeak();

#endif // FASTPLAY_ACCESSIBILITY_H
