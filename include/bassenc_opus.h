/*
	BASSenc_OPUS 2.4 C/C++ header file
	Copyright (c) 2016-2025 Un4seen Developments Ltd.

	See the BASSENC_OPUS.CHM file for more detailed documentation
*/

#ifndef BASSENC_OPUS_H
#define BASSENC_OPUS_H

#include "bassenc.h"

#if BASSVERSION!=0x204
#error conflicting BASS and BASSenc_OPUS versions
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BASSENCOPUSDEF
#define BASSENCOPUSDEF(f) WINAPI f
#else
#define NOBASSENCOPUSOVERLOADS
#endif

// BASS_Encode_OPUS_NewStream flags
#define BASS_ENCODE_OPUS_RESET		0x1000000
#define BASS_ENCODE_OPUS_CTLONLY	0x2000000

DWORD BASSENCOPUSDEF(BASS_Encode_OPUS_GetVersion)(void);

HENCODE BASSENCOPUSDEF(BASS_Encode_OPUS_Start)(DWORD handle, const char *options, DWORD flags, ENCODEPROC *proc, void *user);
HENCODE BASSENCOPUSDEF(BASS_Encode_OPUS_StartFile)(DWORD handle, const char *options, DWORD flags, const char *filename);
BOOL BASSENCOPUSDEF(BASS_Encode_OPUS_NewStream)(HENCODE handle, const char *options, DWORD flags);

#ifdef __cplusplus
}

#if defined(_WIN32) && !defined(NOBASSENCOPUSOVERLOADS)
static inline HENCODE BASS_Encode_OPUS_Start(DWORD handle, const WCHAR *options, DWORD flags, ENCODEPROC *proc, void *user)
{
	return BASS_Encode_OPUS_Start(handle, (const char*)options, flags|BASS_UNICODE, proc, user);
}

static inline HENCODE BASS_Encode_OPUS_StartFile(DWORD handle, const WCHAR *options, DWORD flags, const WCHAR *filename)
{
	return BASS_Encode_OPUS_StartFile(handle, (const char*)options, flags|BASS_UNICODE, (const char*)filename);
}

static inline BOOL BASS_Encode_OPUS_NewStream(HENCODE handle, const WCHAR *options, DWORD flags)
{
	return BASS_Encode_OPUS_NewStream(handle, (const char*)options, flags | BASS_UNICODE);
}
#endif
#endif

#endif
