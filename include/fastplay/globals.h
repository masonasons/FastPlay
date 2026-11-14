#pragma once
#ifndef FASTPLAY_GLOBALS_H
#define FASTPLAY_GLOBALS_H

#include <cstdint>
#include <vector>
#include <string>
#include "types.h"

// Forward declarations
template<typename T> class CycleList;
struct CycleItem;

// Constants
extern const wchar_t* APP_NAME;
extern const wchar_t* MUTEX_NAME;
constexpr double SEEK_AMOUNT = 5.0;
constexpr unsigned UPDATE_INTERVAL = 250;
constexpr unsigned BATCH_DELAY = 300;
constexpr float MAX_VOLUME_NORMAL = 1.0f;
constexpr float MAX_VOLUME_AMPLIFY = 4.0f;

// Status bar part indices
constexpr int SB_PART_POSITION = 0;
constexpr int SB_PART_VOLUME = 1;
constexpr int SB_PART_STATE = 2;
constexpr int SB_PART_COUNT = 3;

// Playback state (what is loaded is the audio engine's: see audio.h)
extern float g_volume;
extern bool g_muted;          // Muted state (recording still works)
extern bool g_disableBatchDelay; // Skip batch delay when opening files from explorer
extern bool g_smoothSeek;        // Short fades when seeking, pausing and changing tracks
extern bool g_liveRewind;        // Keep live streams for rewinding
extern std::vector<LibraryFolder> g_libraryFolders;  // the library's folders (library.h)
extern int g_liveRewindMinutes;  // ...this many minutes of them
extern const int g_liveRewindChoices[];
extern const int g_liveRewindChoiceCount;

// ReplayGain (loudness normalization from REPLAYGAIN_* / R128 tags)
extern int g_replayGainMode;        // 0 = Off, 1 = Track, 2 = Album (album falls back to track)
extern float g_replayGainPreamp;    // Extra gain in dB applied on top of the tag value
extern bool g_replayGainPreventClip; // Reduce gain using the peak tag to avoid clipping
extern float g_replayGainScale;     // Computed linear multiplier for the current track (1.0 = no change)

// Tempo, pitch and rate
extern float g_tempo;
extern float g_pitch;
extern float g_rate;
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
extern bool g_showChapterInTitle;          // Show the chapter playing in the window title (default false)
extern bool g_playlistFollowPlayback;      // Auto-select current track in playlist dialog
extern bool g_checkForUpdates;             // Check for updates on startup
extern bool g_allowMultipleInstances;      // Allow multiple instances (new windows)

// File batching
extern std::vector<std::wstring> g_pendingFiles;
extern uint32_t g_startupTime;

// Recent files
extern std::vector<std::wstring> g_recentFiles;
const int MAX_RECENT_FILES = 10;

// Seek amounts
extern const SeekAmount g_seekAmounts[];
extern const int g_seekAmountCount;
extern bool g_seekEnabled[];
extern int g_currentSeekIndex;

// Seek modes (slash cycles): the arrows jump by the seek unit, or held, scrub
// through the audio. Comma and period set the scrubbing speed in those modes.
enum SeekMode { SEEK_MODE_JUMP = 0, SEEK_MODE_SPRING = 1, SEEK_MODE_TAPE = 2, SEEK_MODE_COUNT = 3 };
extern int g_seekMode;
extern int g_springSpeed;  // spring's top speed, times normal
extern int g_tapeSpeed;    // tape's speed, times normal
extern const int g_scrubSpeeds[];
extern const int g_scrubSpeedCount;
// What spring and tape seeking do, and how long it takes (milliseconds): the
// Seeking options. Each behavior can be turned off.
extern bool g_springWindUp;      // spring speeds up the longer it is held (off: its top speed at once)
extern int g_springDoublingMs;   // ...doubling its speed this often
extern bool g_tapeSpinUp;        // tape spins up to speed when held (off: at speed at once)
extern int g_tapeSpinUpMs;
extern bool g_tapeSpinDown;      // tape let go of winds back down (off: normal speed at once)
extern int g_tapeSpinDownMs;
extern bool g_tapeStopOnPause;   // in tape seeking, pausing slows the tape to a standstill first
extern bool g_tapeStopOnStop;    // ...and stopping
extern int g_tapeStopMs;
extern bool g_tapeStartOnPlay;   // in tape seeking, playing speeds the tape up from a standstill
extern int g_tapeStartMs;
const int kScrubTimeMinMs = 50;
const int kScrubTimeMaxMs = 5000;

// [ and ] go round from the last effect to the first and back (off: they stop at the ends)
extern bool g_wrapEffects;

// Hotkey actions
extern const HotkeyAction g_hotkeyActions[];
extern const int g_hotkeyActionCount;
extern const wchar_t* const g_legacyHotkeyActions[];
extern const int g_legacyHotkeyActionCount;

// Hotkeys
extern std::vector<GlobalHotkey> g_hotkeys;
extern int g_nextHotkeyId;
extern bool g_hotkeysEnabled;

// File associations
extern const FileAssoc g_fileAssocs[];
extern const int g_fileAssocCount;
extern bool g_registerFileTypes;

// Position thresholds
extern const int g_posThresholds[];
extern const int g_posThresholdCount;

// Config
extern std::wstring g_configPath;

// Effect parameters for CycleList
extern bool g_effectEnabled[];
extern int g_currentEffectIndex;
extern int g_rateStepMode;         // 0=0.01x, 1=Semitone

// Advanced settings (audio buffer)
extern int g_bufferSize;       // Output buffer in ms (default 500): how far effects and tempo changes lag

// Buffer size options (in ms)
extern const int g_bufferSizes[];
extern const int g_bufferSizeCount;

// Update period options (in ms)

// Tempo/pitch algorithm setting
extern int g_tempoAlgorithm;   // 1=Speedy, 2=Signalsmith (TempoAlgorithm)


// Speedy settings
extern bool g_speedyNonlinear;     // Enable nonlinear speedup (default true, recommended for speech)

// Signalsmith Stretch settings
extern int g_ssPreset;             // 0=Default, 1=Cheaper
extern int g_ssTonalityLimit;      // Tonality limit in Hz (0=auto)

// Reverb algorithm (0=Off, 1=Simple, 2=Advanced)
extern int g_reverbAlgorithm;

// Convolution reverb settings
extern std::wstring g_convolutionIRPath;  // Path to impulse response WAV file

// MIDI settings
extern std::wstring g_midiSoundFont;  // Path to SoundFont (.sf2/.sf3) file
extern int g_midiMaxVoices;           // Max polyphony (1-1000, default 128)
extern bool g_midiSincInterp;         // Use sinc interpolation (higher quality, more CPU)
extern std::wstring g_midiOutputPort;  // A MIDI output port to play through instead; "" for the synthesizer

// EQ frequency settings (Hz)
extern float g_eqBassFreq;
extern float g_eqMidFreq;
extern float g_eqTrebleFreq;

// YouTube settings
extern std::wstring g_ytApiKey;     // YouTube Data API key (optional)
extern int g_ytFavoritesSort;       // YouTube favorites order: 0 newest upload first, 1 by name
extern YouTubeDownloadSettings g_ytDownload;  // how YouTube videos are downloaded
extern int g_ytAutoRefresh;         // refresh favorites: 0 off, 1 at startup, 2-6 every 30 min to 8 hours

// Downloads settings
extern std::wstring g_downloadPath;      // Output directory for podcast downloads
extern bool g_downloadOrganizeByFeed;    // Organize downloads into folders by feed title

// Recording settings
extern std::wstring g_recordPath;       // Output directory for recordings
extern std::wstring g_recordTemplate;   // Filename template (default: "%Y-%m-%d_%H-%M-%S")
extern int g_recordFormat;              // 0=WAV, 1=MP3, 2=OGG, 3=FLAC
extern int g_recordBitrate;             // MP3/OGG bitrate in kbps (128, 192, 256, 320)
extern bool g_recordEffects;            // Recordings have the effects (else tapped before them)
extern bool g_isRecording;              // Currently recording?

// Speech settings
extern bool g_speechTrackChange;        // Announce track changes
extern bool g_speechVolume;             // Speak volume when adjusted
extern bool g_speechEffect;             // Speak effect value when adjusted

// Shuffle and auto-advance
extern bool g_shuffle;                  // Shuffle playback order
extern bool g_autoAdvance;              // Auto-play next track when current ends (default true)
extern int g_repeatMode;                // 0 = off, 1 = repeat one, 2 = repeat all

// Chapter support
extern std::vector<Chapter> g_chapters;     // Chapters for current file
extern bool g_chapterSeekEnabled;           // Enable chapter seeking in movement options

#endif // FASTPLAY_GLOBALS_H
