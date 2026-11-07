#include "globals.h"
#include "commands.h"

// Constants
const wchar_t* APP_NAME = L"FastPlay";
const wchar_t* MUTEX_NAME = L"FastPlaySingleInstance";

// Playback state (what is loaded is the audio engine's: see audio.h)
float g_volume = 1.0f;
bool g_muted = false;      // Muted state (recording still works)
bool g_disableBatchDelay = false; // Skip batch delay when opening files from explorer
bool g_smoothSeek = true;         // Short fades when seeking, pausing and changing tracks
bool g_liveRewind = false;        // Keep live streams for rewinding
std::vector<LibraryFolder> g_libraryFolders;
int g_liveRewindMinutes = 30;     // ...this many minutes of them
const int g_liveRewindChoices[] = {5, 10, 15, 30, 60, 120};
const int g_liveRewindChoiceCount = sizeof(g_liveRewindChoices) / sizeof(g_liveRewindChoices[0]);

// ReplayGain
int g_replayGainMode = 0;          // Off by default (opt-in)
float g_replayGainPreamp = 0.0f;
bool g_replayGainPreventClip = true;
float g_replayGainScale = 1.0f;    // No change until a track with tags is loaded

// Effect state
float g_tempo = 0.0f;
float g_pitch = 0.0f;
float g_rate = 1.0f;
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
bool g_showChapterInTitle = false;  // Show the chapter playing in the window title
bool g_playlistFollowPlayback = true;  // Auto-select current track in playlist dialog (default true)
bool g_checkForUpdates = true;  // Check for updates on startup (default true)
bool g_allowMultipleInstances = false;  // Allow multiple instances (default false)

// System tray

// File batching
std::vector<std::wstring> g_pendingFiles;
uint32_t g_startupTime = 0;

// Recent files
std::vector<std::wstring> g_recentFiles;

// File associations - all the formats FastPlay plays
const FileAssoc g_fileAssocs[] = {
    // Common formats
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
    // Speex
    {L".spx", L"Speex Audio"},
    // WavPack plugin
    {L".wv", L"WavPack Audio"},
    // Monkey's Audio plugin
    {L".ape", L"APE Audio"},
    // ALAC plugin
    {L".alac", L"ALAC Audio"},
    // Codec 2
    {L".c2", L"Codec 2 Audio"},
    // MIDI plugin
    {L".mid", L"MIDI Audio"},
    {L".midi", L"MIDI Audio"},
    {L".rmi", L"RMI MIDI Audio"},
    {L".kar", L"Karaoke MIDI"},
    // DSD plugin
    {L".dff", L"DSD Audio"},
    {L".dsf", L"DSD Audio"},
#ifdef _WIN32
    // Audio CD tracks
    {L".cda", L"CD Audio Track"},
#endif
    // HLS streaming
    // (no file extension - network only)
    // MOD/tracker formats
    {L".mod", L"MOD Audio"},
    {L".s3m", L"S3M Audio"},
    {L".xm", L"XM Audio"},
    {L".it", L"IT Audio"},
    {L".mtm", L"MTM Audio"},
    {L".umx", L"UMX Audio"},
    {L".669", L"669 Audio"},
    {L".mo3", L"MO3 Audio"},
    {L".mptm", L"MPTM Audio"},
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
    {1.0, "1 second", false},
    {5.0, "5 seconds", false},
    {10.0, "10 seconds", false},
    {30.0, "30 seconds", false},
    {60.0, "1 minute", false},
    {300.0, "5 minutes", false},
    {600.0, "10 minutes", false},
    {1800.0, "30 minutes", false},
    {3600.0, "1 hour", false},
    {1.0, "1 track", true},
    {5.0, "5 tracks", true},
    {10.0, "10 tracks", true}
};
const int g_seekAmountCount = sizeof(g_seekAmounts) / sizeof(g_seekAmounts[0]);
bool g_seekEnabled[12] = {false, true, false, false, false, false, false, false, false, false, false, false};
int g_currentSeekIndex = 1;

// Seek modes
int g_seekMode = SEEK_MODE_JUMP;
int g_springSpeed = 16;
int g_tapeSpeed = 4;
const int g_scrubSpeeds[] = {2, 3, 4, 6, 8, 12, 16, 24, 32};
const int g_scrubSpeedCount = sizeof(g_scrubSpeeds) / sizeof(g_scrubSpeeds[0]);
bool g_springWindUp = true;
int g_springDoublingMs = 500;
bool g_tapeSpinUp = true;
int g_tapeSpinUpMs = 250;
bool g_tapeSpinDown = true;
int g_tapeSpinDownMs = 400;
bool g_tapeStopOnPause = true;
bool g_tapeStopOnStop = true;
int g_tapeStopMs = 600;
bool g_tapeStartOnPlay = true;
int g_tapeStartMs = 600;

bool g_wrapEffects = false;

// Hotkey actions. Saved hotkeys name their action by its key, so this list can be
// reordered and added to freely; a key must never change once released.
const HotkeyAction g_hotkeyActions[] = {
    // Playback
    {IDM_PLAY_PLAYPAUSE, L"Play/Pause", L"PlayPause"},
    {IDM_PLAY_PLAY, L"Play", L"Play"},
    {IDM_PLAY_PAUSE, L"Pause", L"Pause"},
    {IDM_PLAY_STOP, L"Stop", L"Stop"},
    {IDM_PLAY_PREV, L"Previous Track", L"PreviousTrack"},
    {IDM_PLAY_NEXT, L"Next Track", L"NextTrack"},
    {IDM_PLAY_GOLIVE, L"Go to Live (Rewound Live Stream)", L"GoLive"},
    {IDM_PLAY_REVERSE, L"Toggle Reverse", L"ToggleReverse"},
    // Seeking
    {IDM_PLAY_SEEKBACK, L"Seek Backward", L"SeekBackward"},
    {IDM_PLAY_SEEKFWD, L"Seek Forward", L"SeekForward"},
    {IDM_SEEK_DECREASE, L"Previous Seek Unit (or Slower Scrubbing)", L"PreviousSeekUnit"},
    {IDM_SEEK_INCREASE, L"Next Seek Unit (or Faster Scrubbing)", L"NextSeekUnit"},
    {IDM_SEEK_MODE, L"Next Seek Mode", L"NextSeekMode"},
    {IDM_PLAY_RANDOM, L"Jump to Random Point", L"JumpToRandomPoint"},
    {IDM_SPEAK_SEEK, L"Speak Seek Unit", L"SpeakSeekUnit"},
    // Volume
    {IDM_PLAY_VOLUP, L"Volume Up", L"VolumeUp"},
    {IDM_PLAY_VOLDOWN, L"Volume Down", L"VolumeDown"},
    {IDM_PLAY_MUTE, L"Toggle Mute", L"ToggleMute"},
    // Speech feedback
    {IDM_PLAY_ELAPSED, L"Speak Elapsed", L"SpeakElapsed"},
    {IDM_PLAY_REMAINING, L"Speak Remaining", L"SpeakRemaining"},
    {IDM_PLAY_TOTAL, L"Speak Total", L"SpeakTotal"},
    {IDM_PLAY_NOWPLAYING, L"Speak Now Playing", L"SpeakNowPlaying"},
    // Effects navigation
    {IDM_EFFECT_PREV, L"Previous Effect", L"PreviousEffect"},
    {IDM_EFFECT_NEXT, L"Next Effect", L"NextEffect"},
    {IDM_EFFECT_UP, L"Increase Effect", L"IncreaseEffect"},
    {IDM_EFFECT_DOWN, L"Decrease Effect", L"DecreaseEffect"},
    // Effect toggles
    {IDM_TOGGLE_VOLUME, L"Toggle Volume", L"ToggleVolume"},
    {IDM_TOGGLE_PITCH, L"Toggle Pitch", L"TogglePitch"},
    {IDM_TOGGLE_TEMPO, L"Toggle Tempo", L"ToggleTempo"},
    {IDM_TOGGLE_RATE, L"Toggle Rate", L"ToggleRate"},
    {IDM_TOGGLE_REVERB, L"Toggle Reverb", L"ToggleReverb"},
    {IDM_TOGGLE_ECHO, L"Toggle Echo", L"ToggleEcho"},
    {IDM_TOGGLE_EQ, L"Toggle EQ", L"ToggleEQ"},
    {IDM_TOGGLE_COMPRESSOR, L"Toggle Compressor", L"ToggleCompressor"},
    {IDM_TOGGLE_STEREOWIDTH, L"Toggle Stereo Width", L"ToggleStereoWidth"},
    {IDM_TOGGLE_CENTERCANCEL, L"Toggle Center Cancel", L"ToggleCenterCancel"},
    {IDM_TOGGLE_CONVOLUTION, L"Toggle Convolution Reverb", L"ToggleConvolution"},
    {IDM_TOGGLE_SPATIAL, L"Toggle 3D Audio", L"Toggle3DAudio"},
    {IDM_TOGGLE_NORMALIZER, L"Toggle Normalizer", L"ToggleNormalizer"},
    // Window/UI
    {IDM_TOGGLE_WINDOW, L"Toggle Window", L"ToggleWindow"},
    {IDM_FILE_YOUTUBE, L"YouTube Search", L"YouTube"},
    {IDM_FILE_LIBRARY, L"Library", L"Library"},
    {IDM_SHOW_AUDIO_DEVICES, L"Audio Device Menu", L"AudioDeviceMenu"},
    // Recording
    {IDM_RECORD_TOGGLE, L"Toggle Recording", L"ToggleRecording"},
    // Shuffle
    {IDM_PLAY_SHUFFLE, L"Toggle Shuffle", L"ToggleShuffle"},
};
const int g_hotkeyActionCount = sizeof(g_hotkeyActions) / sizeof(g_hotkeyActions[0]);

// Hotkeys saved before they were saved by key named their action by its place in
// this list as it was then: those places, in order.
const wchar_t* const g_legacyHotkeyActions[] = {
    L"PlayPause", L"Play", L"Pause", L"Stop", L"PreviousTrack", L"NextTrack", L"SeekBackward", L"SeekForward",
    L"PreviousSeekUnit", L"NextSeekUnit", L"SpeakSeekUnit", L"VolumeUp", L"VolumeDown", L"SpeakElapsed",
    L"SpeakRemaining", L"SpeakTotal", L"SpeakNowPlaying", L"PreviousEffect", L"NextEffect", L"IncreaseEffect",
    L"DecreaseEffect", L"ToggleVolume", L"TogglePitch", L"ToggleTempo", L"ToggleRate", L"ToggleReverb",
    L"ToggleEcho", L"ToggleEQ", L"ToggleCompressor", L"ToggleStereoWidth", L"ToggleCenterCancel",
    L"ToggleConvolution", L"Toggle3DAudio", L"ToggleWindow", L"YouTube", L"ToggleRecording", L"ToggleShuffle",
    L"AudioDeviceMenu", L"ToggleMute", L"NextSeekMode"};
const int g_legacyHotkeyActionCount = sizeof(g_legacyHotkeyActions) / sizeof(g_legacyHotkeyActions[0]);

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

// Advanced settings (audio buffer)
int g_bufferSize = 500;    // Default 500ms

// Buffer size options (in ms)
const int g_bufferSizes[] = {100, 200, 300, 500, 1000, 2000};
const int g_bufferSizeCount = sizeof(g_bufferSizes) / sizeof(g_bufferSizes[0]);

// Update period options (in ms)

// Tempo/pitch algorithm (1=Speedy, 2=Signalsmith)
int g_tempoAlgorithm = 2;  // Signalsmith (TempoAlgorithm)


// Speedy settings
bool g_speedyNonlinear = true;     // Enable nonlinear speedup (recommended)

// Signalsmith Stretch settings
int g_ssPreset = 0;                // 0=Default, 1=Cheaper
int g_ssTonalityLimit = 0;         // Tonality limit in Hz (0=auto)

// Reverb algorithm (0=Off, 1=Simple, 2=Advanced)
int g_reverbAlgorithm = 0;

// Convolution reverb settings
std::wstring g_convolutionIRPath;

// MIDI settings
std::wstring g_midiSoundFont;      // Path to SoundFont file
int g_midiMaxVoices = 128;         // Max polyphony (1-1000)
bool g_midiSincInterp = false;     // Use sinc interpolation
std::wstring g_midiOutputPort;     // "" plays through the built-in synthesizer

// EQ frequency settings (Hz)
float g_eqBassFreq = 50.0f;
float g_eqMidFreq = 1000.0f;
float g_eqTrebleFreq = 12000.0f;

// YouTube settings
std::wstring g_ytApiKey;    // YouTube Data API key (optional)
int g_ytFavoritesSort = 0;  // YouTube favorites order: 0 newest upload first, 1 by name
YouTubeDownloadSettings g_ytDownload;  // how YouTube videos are downloaded
int g_ytAutoRefresh = 0;               // refresh favorites: 0 off, 1 at startup, 2-6 every 30 min to 8 hours

// Downloads settings
std::wstring g_downloadPath;             // Output directory for podcast downloads
bool g_downloadOrganizeByFeed = false;   // Organize downloads into folders by feed title

// Recording settings
std::wstring g_recordPath;                          // Output directory
std::wstring g_recordTemplate = L"%Y-%m-%d_%H-%M-%S";  // Filename template
int g_recordFormat = 0;                             // 0=WAV, 1=MP3, 2=OGG, 3=FLAC
int g_recordBitrate = 192;                          // Bitrate for MP3/OGG
bool g_recordEffects = true;                        // Recordings have the effects
bool g_isRecording = false;                         // Currently recording?

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
