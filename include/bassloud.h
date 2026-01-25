/*
	BASSloud 2.4 C/C++ header file
	Copyright (c) 2023 Un4seen Developments Ltd.

	See the BASSLOUD.CHM file for more detailed documentation
*/

#ifndef BASSLOUDNESS_H
#define BASSLOUDNESS_H

#include "bass.h"

#if BASSVERSION!=0x204
#error conflicting BASS and BASSLOUDNESS versions
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BASSLOUDDEF
#define BASSLOUDDEF(f) WINAPI f
#endif

typedef DWORD HLOUDNESS;		// loudness handle
	
// BASS_Loudness_Start flags / BASS_Loudness_GetLevel modes
#define BASS_LOUDNESS_CURRENT		0
#define BASS_LOUDNESS_INTEGRATED	1
#define BASS_LOUDNESS_RANGE			2
#define BASS_LOUDNESS_PEAK			4
#define BASS_LOUDNESS_TRUEPEAK		8
#define BASS_LOUDNESS_AUTOFREE		0x8000

DWORD BASSLOUDDEF(BASS_Loudness_GetVersion)(void);

HLOUDNESS BASSLOUDDEF(BASS_Loudness_Start)(DWORD handle, DWORD flags, int priority);
BOOL BASSLOUDDEF(BASS_Loudness_Stop)(DWORD handle);
BOOL BASSLOUDDEF(BASS_Loudness_SetChannel)(HLOUDNESS handle, DWORD channel, int priority);
DWORD BASSLOUDDEF(BASS_Loudness_GetChannel)(HLOUDNESS handle);
BOOL BASSLOUDDEF(BASS_Loudness_GetLevel)(HLOUDNESS handle, DWORD mode, float *level);
BOOL BASSLOUDDEF(BASS_Loudness_GetLevelMulti)(HLOUDNESS *handles, DWORD count, DWORD mode, float *level);

#ifdef __cplusplus
}
#endif

#endif
