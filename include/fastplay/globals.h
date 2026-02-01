#pragma once
#ifndef FASTPLAY_GLOBALS_H
#define FASTPLAY_GLOBALS_H

#include <windows.h>
#include <vector>
#include <string>
#include "bass.h"
#include "bassenc.h"
#include "types.h"

// Forward declarations
template<typename T> class CycleList;
struct CycleItem;

// Constants
extern const wchar_t* APP_NAME;
extern const wchar_t* WINDOW_CLASS;
extern const wchar_t* MUTEX_NAME;
constexpr double SEEK_AMOUNT = 5.0;
constexpr UINT UPDATE_INTERVAL = 250;
constexpr UINT BATCH_DELAY = 300;
constexpr float MAX_VOLUME_NORMAL = 1.0f;
constexpr float MAX_VOLUME_AMPLIFY = 4.0f;

// Status bar part indices
constexpr int SB_PART_POSITION = 0;
constexpr int SB_PART_VOLUME = 1;
constexpr int SB_PART_STATE = 2;
constexpr int SB_PART_COUNT = 3;

// Window handles
extern HWND g_hwnd;
extern HWND g_statusBar;

// BASS state
extern HSTREAM g_stream;      // Source stream
extern HSTREAM g_fxStream;    // Tempo stream (wraps g_stream for pitch/tempo)
extern HSTREAM g_sourceStream; // Original decode stream (for bitrate queries, not freed separately)
extern HSYNC g_endSync;
extern HSYNC g_metaSync;      // Sync for stream metadata changes
extern float g_volume;
extern bool g_muted;          // Muted state (recording still works)
extern bool g_legacyVolume;   // Use legacy volume (faster, but affects recordings)

// Effect state (for BASS_FX)
extern float g_tempo;
extern float g_pitch;
extern float g_rate;
extern float g_originalFreq;  // Original sample rate for rate control
extern bool g_isLiveStream;   // True if current stream is non-seekable (live stream)
extern int g_currentBitrate;  // Cached bitrate of current file (kbps)

// Playlist
extern std::vector<std::wstring> g_playlist;
extern int g_currentTrack;

// Loading guards
extern bool g_isLoading;
extern bool g_isBusy;

// Options state
extern int g_selectedDevice;
extern std::wstring g_selectedDeviceName;  // Device name for persistent storage
extern int g_rewindOnPauseMs;
extern bool g_allowAmplify;
extern bool g_rememberState;
extern int g_rememberPosMinutes;
extern bool g_bringToFront;
extern bool g_minimizeToTray;
extern bool g_loadFolder;
extern float g_volumeStep;                 // Volume change per keypress (default 0.02 = 2%)
extern bool g_showTitleInWindow;           // Show track name in window title (default true)
extern bool g_playlistFollowPlayback;      // Auto-select current track in playlist dialog
extern bool g_checkForUpdates;             // Check for updates on startup
extern bool g_allowMultipleInstances;      // Allow multiple instances (new windows)

// System tray
extern NOTIFYICONDATAW g_trayIcon;
extern bool g_trayIconVisible;

// File batching
extern std::vector<std::wstring> g_pendingFiles;
extern DWORD g_startupTime;

// Recent files
extern std::vector<std::wstring> g_recentFiles;
const int MAX_RECENT_FILES = 10;

// Seek amounts
extern const SeekAmount g_seekAmounts[];
extern const int g_seekAmountCount;
extern bool g_seekEnabled[];
extern int g_currentSeekIndex;

// Hotkey actions
extern const HotkeyAction g_hotkeyActions[];
extern const int g_hotkeyActionCount;

// Hotkeys
extern std::vector<GlobalHotkey> g_hotkeys;
extern int g_nextHotkeyId;
extern bool g_hotkeysEnabled;

// File associations
extern const FileAssoc g_fileAssocs[];
extern const int g_fileAssocCount;

// Position thresholds
extern const int g_posThresholds[];
extern const int g_posThresholdCount;

// Config
extern std::wstring g_configPath;

// Effect parameters for CycleList
extern bool g_effectEnabled[];
extern int g_currentEffectIndex;
extern int g_rateStepMode;         // 0=0.01x, 1=Semitone

// Advanced settings (BASS buffer)
extern int g_bufferSize;       // BASS_CONFIG_BUFFER in ms (default 500)
extern int g_updatePeriod;     // BASS_CONFIG_UPDATEPERIOD in ms (default 100)

// Buffer size options (in ms)
extern const int g_bufferSizes[];
extern const int g_bufferSizeCount;

// Update period options (in ms)
extern const int g_updatePeriods[];
extern const int g_updatePeriodCount;

// Tempo/pitch algorithm setting
extern int g_tempoAlgorithm;   // 0=SoundTouch, 1=RubberBandR2, 2=RubberBandR3

// SoundTouch settings
extern bool g_stAntiAliasFilter;   // Enable anti-alias filter (default true)
extern int g_stAAFilterLength;     // AA filter length 8-128 (default 32)
extern bool g_stQuickAlgorithm;    // Quick/simple algorithm (default false)
extern int g_stSequenceMs;         // Sequence window 0-200ms (default 82, 0=auto)
extern int g_stSeekWindowMs;       // Seek window 0-100ms (default 28, 0=auto)
extern int g_stOverlapMs;          // Overlap window 0-50ms (default 8)
extern bool g_stPreventClick;      // Click prevention (default false)
extern int g_stAlgorithm;          // 0=Linear, 1=Cubic, 2=Shannon (default 1)

// Rubber Band settings
extern bool g_rbFormantPreserved;  // Preserve formants when pitch shifting (default false)
extern int g_rbPitchMode;          // 0=HighSpeed, 1=HighQuality, 2=HighConsistency (default 2)
extern int g_rbWindowSize;         // 0=Standard, 1=Short, 2=Long (default 0)
extern int g_rbTransients;         // 0=Crisp, 1=Mixed, 2=Smooth (default 0, R2 only)
extern int g_rbDetector;           // 0=Compound, 1=Percussive, 2=Soft (default 0, R2 only)
extern int g_rbChannels;           // 0=Apart, 1=Together (default 0)
extern int g_rbPhase;              // 0=Laminar, 1=Independent (default 0, R2 only)
extern bool g_rbSmoothing;         // Time-domain smoothing (default false, R2 only)

// Speedy settings
extern bool g_speedyNonlinear;     // Enable nonlinear speedup (default true, recommended for speech)

// Reverb algorithm (0=Off, 1=Freeverb, 2=DX8, 3=I3DL2)
extern int g_reverbAlgorithm;

// Convolution reverb settings
extern std::wstring g_convolutionIRPath;  // Path to impulse response WAV file

// MIDI settings (BASSMIDI)
extern std::wstring g_midiSoundFont;  // Path to SoundFont (.sf2/.sf3) file
extern int g_midiMaxVoices;           // Max polyphony (1-1000, default 128)
extern bool g_midiSincInterp;         // Use sinc interpolation (higher quality, more CPU)

// EQ frequency settings (Hz)
extern float g_eqBassFreq;
extern float g_eqMidFreq;
extern float g_eqTrebleFreq;

// YouTube settings
extern std::wstring g_ytdlpPath;    // Path to yt-dlp executable
extern std::wstring g_ytApiKey;     // YouTube Data API key (optional)

// Downloads settings
extern std::wstring g_downloadPath;      // Output directory for podcast downloads
extern bool g_downloadOrganizeByFeed;    // Organize downloads into folders by feed title

// Recording settings
extern std::wstring g_recordPath;       // Output directory for recordings
extern std::wstring g_recordTemplate;   // Filename template (default: "%Y-%m-%d_%H-%M-%S")
extern int g_recordFormat;              // 0=WAV, 1=MP3, 2=OGG, 3=FLAC
extern int g_recordBitrate;             // MP3/OGG bitrate in kbps (128, 192, 256, 320)
extern bool g_isRecording;              // Currently recording?
extern HENCODE g_encoder;               // BASS encoder handle

// Speech settings
extern bool g_speechTrackChange;        // Announce track changes
extern bool g_speechVolume;             // Speak volume when adjusted
extern bool g_speechEffect;             // Speak effect value when adjusted

// Shuffle and auto-advance
extern bool g_shuffle;                  // Shuffle playback order
extern bool g_autoAdvance;              // Auto-play next track when current ends (default true)

// Chapter support
struct Chapter {
    double position;        // Position in seconds
    std::wstring name;      // Chapter name
};
extern std::vector<Chapter> g_chapters;     // Chapters for current file
extern bool g_chapterSeekEnabled;           // Enable chapter seeking in movement options

#endif // FASTPLAY_GLOBALS_H
