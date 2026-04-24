#include "globals.h"
#include "resource.h"

// Constants
const wchar_t* APP_NAME = L"FastPlay";
const wchar_t* WINDOW_CLASS = L"FastPlayWindow";
const wchar_t* MUTEX_NAME = L"FastPlaySingleInstance";

// Window handles
HWND g_hwnd = nullptr;
HWND g_statusBar = nullptr;

HWND GetMessageBoxOwner() {
    HWND active = GetActiveWindow();
    if (active) return active;
    return g_hwnd;
}

// BASS state
HSTREAM g_stream = 0;      // Source stream
HSTREAM g_fxStream = 0;    // Tempo stream (wraps g_stream for pitch/tempo)
HSTREAM g_sourceStream = 0; // Original decode stream (for bitrate queries)
HSYNC g_endSync = 0;
HSYNC g_metaSync = 0;      // Sync for stream metadata changes
float g_volume = 1.0f;
bool g_muted = false;      // Muted state (recording still works)
bool g_legacyVolume = false;  // Use legacy volume (faster, but affects recordings)
bool g_disableBatchDelay = false; // Skip batch delay when opening files from explorer

// Effect state
float g_tempo = 0.0f;
float g_pitch = 0.0f;
float g_rate = 1.0f;
float g_originalFreq = 44100.0f;  // Default, updated when loading files
bool g_isLiveStream = false;      // True if current stream is non-seekable
int g_currentBitrate = 0;         // Cached bitrate of current file (kbps)

// Playlist
std::vector<std::wstring> g_playlist;
int g_currentTrack = -1;

// Loading guards
bool g_isLoading = false;
bool g_isBusy = false;

// Options state
int g_selectedDevice = -1;
std::wstring g_selectedDeviceName;  // Device name for persistent storage
int g_rewindOnPauseMs = 0;
bool g_allowAmplify = false;
bool g_rememberState = false;
int g_rememberPosMinutes = 0;
bool g_bringToFront = true;
bool g_minimizeToTray = true;
bool g_loadFolder = false;
float g_volumeStep = 0.02f;  // Volume change per keypress (default 2%)
bool g_showTitleInWindow = true;  // Show track name in window title (default true)
bool g_playlistFollowPlayback = true;  // Auto-select current track in playlist dialog (default true)
bool g_checkForUpdates = true;  // Check for updates on startup (default true)
bool g_allowMultipleInstances = false;  // Allow multiple instances (default false)

// System tray
NOTIFYICONDATAW g_trayIcon = {0};
bool g_trayIconVisible = false;

// File batching
std::vector<std::wstring> g_pendingFiles;
DWORD g_startupTime = 0;

// Recent files
std::vector<std::wstring> g_recentFiles;

// File associations - all formats BASS and its plugins can play
const FileAssoc g_fileAssocs[] = {
    // Core BASS formats
    {L".mp3", L"MP3 Audio"},
    {L".mp2", L"MP2 Audio"},
    {L".mp1", L"MP1 Audio"},
    {L".wav", L"WAV Audio"},
    {L".ogg", L"OGG Audio"},
    {L".oga", L"OGA Audio"},
    {L".aiff", L"AIFF Audio"},
    {L".aif", L"AIF Audio"},
    // FLAC plugin
    {L".flac", L"FLAC Audio"},
    // AAC plugin
    {L".aac", L"AAC Audio"},
    {L".m4a", L"M4A Audio"},
    {L".m4b", L"M4B Audiobook"},
    {L".m4r", L"M4R Ringtone"},
    {L".mp4", L"MP4 Audio"},
    // WMA plugin
    {L".wma", L"WMA Audio"},
    {L".wmv", L"WMV Video"},
    // Opus plugin
    {L".opus", L"Opus Audio"},
    // WavPack plugin
    {L".wv", L"WavPack Audio"},
    // Monkey's Audio plugin
    {L".ape", L"APE Audio"},
    // ALAC plugin
    {L".alac", L"ALAC Audio"},
    // MIDI plugin
    {L".mid", L"MIDI Audio"},
    {L".midi", L"MIDI Audio"},
    {L".rmi", L"RMI MIDI Audio"},
    {L".kar", L"Karaoke MIDI"},
    // DSD plugin
    {L".dff", L"DSD Audio"},
    {L".dsf", L"DSD Audio"},
    // CD Audio plugin
    {L".cda", L"CD Audio"},
    // HLS streaming
    // (no file extension - network only)
    // MOD/tracker formats (built into BASS)
    {L".mod", L"MOD Audio"},
    {L".s3m", L"S3M Audio"},
    {L".xm", L"XM Audio"},
    {L".it", L"IT Audio"},
    {L".mtm", L"MTM Audio"},
    {L".umx", L"UMX Audio"},
    // Playlist formats
    {L".m3u", L"M3U Playlist"},
    {L".m3u8", L"M3U8 Playlist"},
    {L".pls", L"PLS Playlist"},
};
const int g_fileAssocCount = sizeof(g_fileAssocs) / sizeof(g_fileAssocs[0]);
bool g_registerFileTypes = false;

// Position thresholds
const int g_posThresholds[] = {0, 5, 10, 20, 30, 45, 60};
const int g_posThresholdCount = sizeof(g_posThresholds) / sizeof(g_posThresholds[0]);

// Seek amounts
const SeekAmount g_seekAmounts[] = {
    {1.0, "1 second", IDC_SEEK_1S, false},
    {5.0, "5 seconds", IDC_SEEK_5S, false},
    {10.0, "10 seconds", IDC_SEEK_10S, false},
    {30.0, "30 seconds", IDC_SEEK_30S, false},
    {60.0, "1 minute", IDC_SEEK_1M, false},
    {300.0, "5 minutes", IDC_SEEK_5M, false},
    {600.0, "10 minutes", IDC_SEEK_10M, false},
    {1800.0, "30 minutes", IDC_SEEK_30M, false},
    {3600.0, "1 hour", IDC_SEEK_1H, false},
    {1.0, "1 track", IDC_SEEK_1T, true},
    {5.0, "5 tracks", IDC_SEEK_5T, true},
    {10.0, "10 tracks", IDC_SEEK_10T, true}
};
const int g_seekAmountCount = sizeof(g_seekAmounts) / sizeof(g_seekAmounts[0]);
bool g_seekEnabled[12] = {false, true, false, false, false, false, false, false, false, false, false, false};
int g_currentSeekIndex = 1;

// Hotkey actions
const HotkeyAction g_hotkeyActions[] = {
    // Playback
    {IDM_PLAY_PLAYPAUSE, L"Play/Pause"},
    {IDM_PLAY_PLAY, L"Play"},
    {IDM_PLAY_PAUSE, L"Pause"},
    {IDM_PLAY_STOP, L"Stop"},
    {IDM_PLAY_PREV, L"Previous Track"},
    {IDM_PLAY_NEXT, L"Next Track"},
    // Seeking
    {IDM_PLAY_SEEKBACK, L"Seek Backward"},
    {IDM_PLAY_SEEKFWD, L"Seek Forward"},
    {IDM_SEEK_DECREASE, L"Previous Seek Unit"},
    {IDM_SEEK_INCREASE, L"Next Seek Unit"},
    {IDM_SPEAK_SEEK, L"Speak Seek Unit"},
    // Volume
    {IDM_PLAY_VOLUP, L"Volume Up"},
    {IDM_PLAY_VOLDOWN, L"Volume Down"},
    // Speech feedback
    {IDM_PLAY_ELAPSED, L"Speak Elapsed"},
    {IDM_PLAY_REMAINING, L"Speak Remaining"},
    {IDM_PLAY_TOTAL, L"Speak Total"},
    {IDM_PLAY_NOWPLAYING, L"Speak Now Playing"},
    // Effects navigation
    {IDM_EFFECT_PREV, L"Previous Effect"},
    {IDM_EFFECT_NEXT, L"Next Effect"},
    {IDM_EFFECT_UP, L"Increase Effect"},
    {IDM_EFFECT_DOWN, L"Decrease Effect"},
    // Effect toggles
    {IDM_TOGGLE_VOLUME, L"Toggle Volume"},
    {IDM_TOGGLE_PITCH, L"Toggle Pitch"},
    {IDM_TOGGLE_TEMPO, L"Toggle Tempo"},
    {IDM_TOGGLE_RATE, L"Toggle Rate"},
    {IDM_TOGGLE_REVERB, L"Toggle Reverb"},
    {IDM_TOGGLE_ECHO, L"Toggle Echo"},
    {IDM_TOGGLE_EQ, L"Toggle EQ"},
    {IDM_TOGGLE_COMPRESSOR, L"Toggle Compressor"},
    {IDM_TOGGLE_STEREOWIDTH, L"Toggle Stereo Width"},
    {IDM_TOGGLE_CENTERCANCEL, L"Toggle Center Cancel"},
    {IDM_TOGGLE_CONVOLUTION, L"Toggle Convolution Reverb"},
    {IDM_TOGGLE_SPATIAL, L"Toggle 3D Audio"},
    // Window/UI
    {IDM_TOGGLE_WINDOW, L"Toggle Window"},
    {IDM_FILE_YOUTUBE, L"YouTube Search"},
    // Recording
    {IDM_RECORD_TOGGLE, L"Toggle Recording"},
    // Shuffle
    {IDM_PLAY_SHUFFLE, L"Toggle Shuffle"},
    // Audio device
    {IDM_SHOW_AUDIO_DEVICES, L"Audio Device Menu"},
    // Mute
    {IDM_PLAY_MUTE, L"Toggle Mute"}
};
const int g_hotkeyActionCount = sizeof(g_hotkeyActions) / sizeof(g_hotkeyActions[0]);

// Hotkeys
std::vector<GlobalHotkey> g_hotkeys;
int g_nextHotkeyId = 1;
bool g_hotkeysEnabled = true;

// Config
std::wstring g_configPath;

// Effect parameters
bool g_effectEnabled[4] = {true, false, false, false};  // Volume enabled by default
int g_currentEffectIndex = 0;
int g_rateStepMode = 0;  // 0=0.01x, 1=Semitone

// Advanced settings (BASS buffer)
int g_bufferSize = 500;    // Default 500ms
int g_updatePeriod = 100;  // Default 100ms

// Buffer size options (in ms)
const int g_bufferSizes[] = {100, 200, 300, 500, 1000, 2000};
const int g_bufferSizeCount = sizeof(g_bufferSizes) / sizeof(g_bufferSizes[0]);

// Update period options (in ms)
const int g_updatePeriods[] = {5, 10, 20, 50, 100, 200};
const int g_updatePeriodCount = sizeof(g_updatePeriods) / sizeof(g_updatePeriods[0]);

// Tempo/pitch algorithm (0=SoundTouch, 1=Speedy, 2=Signalsmith)
int g_tempoAlgorithm = 0;  // Default to SoundTouch

// SoundTouch settings
bool g_stAntiAliasFilter = true;   // Enable anti-alias filter
int g_stAAFilterLength = 32;       // AA filter length (8-128 taps)
bool g_stQuickAlgorithm = false;   // Quick/simple algorithm
int g_stSequenceMs = 82;           // Sequence window (0=auto)
int g_stSeekWindowMs = 28;         // Seek window (0=auto)
int g_stOverlapMs = 8;             // Overlap window
bool g_stPreventClick = false;     // Click prevention
int g_stAlgorithm = 1;             // 0=Linear, 1=Cubic, 2=Shannon

// Speedy settings
bool g_speedyNonlinear = true;     // Enable nonlinear speedup (recommended)

// Signalsmith Stretch settings
int g_ssPreset = 0;                // 0=Default, 1=Cheaper
int g_ssTonalityLimit = 0;         // Tonality limit in Hz (0=auto)

// Reverb algorithm (0=Off, 1=Freeverb, 2=DX8, 3=I3DL2)
int g_reverbAlgorithm = 0;

// Convolution reverb settings
std::wstring g_convolutionIRPath;

// MIDI settings (BASSMIDI)
std::wstring g_midiSoundFont;      // Path to SoundFont file
int g_midiMaxVoices = 128;         // Max polyphony (1-1000)
bool g_midiSincInterp = false;     // Use sinc interpolation

// EQ frequency settings (Hz)
float g_eqBassFreq = 50.0f;
float g_eqMidFreq = 1000.0f;
float g_eqTrebleFreq = 12000.0f;

// YouTube settings
std::wstring g_ytdlpPath;   // Path to yt-dlp executable
std::wstring g_ytApiKey;    // YouTube Data API key (optional)

// Downloads settings
std::wstring g_downloadPath;             // Output directory for podcast downloads
bool g_downloadOrganizeByFeed = false;   // Organize downloads into folders by feed title

// Recording settings
std::wstring g_recordPath;                          // Output directory
std::wstring g_recordTemplate = L"%Y-%m-%d_%H-%M-%S";  // Filename template
int g_recordFormat = 0;                             // 0=WAV, 1=MP3, 2=OGG, 3=FLAC
int g_recordBitrate = 192;                          // Bitrate for MP3/OGG
bool g_isRecording = false;                         // Currently recording?
HENCODE g_encoder = 0;                              // BASS encoder handle

// Speech settings
bool g_speechTrackChange = false;                   // Announce track changes (default off)
bool g_speechVolume = true;                         // Speak volume when adjusted (default on)
bool g_speechEffect = true;                         // Speak effect value when adjusted (default on)

// Shuffle and auto-advance
bool g_shuffle = false;                             // Shuffle playback order
bool g_autoAdvance = true;                          // Auto-play next track when current ends
int g_repeatMode = 0;                               // 0 = off, 1 = repeat one, 2 = repeat all

// Chapter support
std::vector<Chapter> g_chapters;                    // Chapters for current file
bool g_chapterSeekEnabled = true;                   // Enable chapter seeking (default on)
