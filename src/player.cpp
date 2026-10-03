#include "player.h"
#include "audio.h"
#include "recorder.h"
#include "globals.h"
#include "utils.h"
#include "http.h"
#include "playlist_io.h"
#include "accessibility.h"
#include "settings.h"
#include "app_ui.h"
#include "radio.h"
#include "effects.h"
#include "database.h"
#include "commands.h"
#include "spatial_audio.h"
#include "mp4_chapters.h"
#include "paths.h"
#include <ctime>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

// Tag reading helpers (defined later in the file)
static std::string GetMetadataTag(const char* tagName);
static std::string GetStreamTitle();

// ---------------------------------------------------------------------------
// Output gain: volume, ReplayGain and mute, applied by the audio engine last
// (after the recording tap, so recordings are at full volume).
// ---------------------------------------------------------------------------

void UpdateOutputGain() {
    // A perceptual volume curve (the fourth power, so the lower half is quieter),
    // then ReplayGain. Above 100% it amplifies, squared, so it doesn't jump too loud.
    float squared = g_volume * g_volume;
    float curve = g_volume <= 1.0f ? squared * squared : squared;
    float gain = g_muted ? 0.0f : curve * g_replayGainScale;
    audio::SetGain(gain);
}

// A live stream kept for rewinding: it can pause and seek within what is kept
static bool IsRewindable() {
    double start, live;
    return g_isLiveStream && audio::LiveRange(start, live);
}

// Compute the linear ReplayGain multiplier for what was just loaded and store it
// in g_replayGainScale. Reads REPLAYGAIN_TRACK_GAIN / REPLAYGAIN_ALBUM_GAIN (and
// the matching _PEAK tags). 1.0 (no change) when disabled or when the file has no
// gain tag, so untagged files and live streams play untouched.
static void ComputeReplayGainScale() {
    g_replayGainScale = 1.0f;
    const audio::Decoder* decoder = audio::Current();
    if (g_replayGainMode == 0 || !decoder) return;

    std::string gainStr, peakStr;
    if (g_replayGainMode == 2) {
        // Album mode, falling back to track gain when no album tag is present.
        gainStr = decoder->Tag("REPLAYGAIN_ALBUM_GAIN");
        peakStr = decoder->Tag("REPLAYGAIN_ALBUM_PEAK");
        if (gainStr.empty()) {
            gainStr = decoder->Tag("REPLAYGAIN_TRACK_GAIN");
            peakStr = decoder->Tag("REPLAYGAIN_TRACK_PEAK");
        }
    } else {
        gainStr = decoder->Tag("REPLAYGAIN_TRACK_GAIN");
        peakStr = decoder->Tag("REPLAYGAIN_TRACK_PEAK");
    }

    if (gainStr.empty()) return;  // No ReplayGain info: leave the file untouched.

    // Tag values look like "-6.48 dB"; strtod reads the leading number and stops at the space.
    float gainDb = static_cast<float>(strtod(gainStr.c_str(), nullptr));
    gainDb += g_replayGainPreamp;

    float scale = powf(10.0f, gainDb / 20.0f);

    // Avoid clipping by capping the gain so the (tagged) peak stays at or below full scale.
    if (g_replayGainPreventClip && !peakStr.empty()) {
        float peak = static_cast<float>(strtod(peakStr.c_str(), nullptr));
        if (peak > 0.0f && scale * peak > 1.0f) {
            scale = 1.0f / peak;
        }
    }

    if (scale > 0.0f) g_replayGainScale = scale;
}

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

// Device numbers are positions in the system's list, from 1; -1 is the default.
int FindDeviceByName(const std::wstring& name) {
    if (name.empty()) return -1;
    std::vector<audio::Device> devices = audio::ListDevices();
    for (size_t i = 0; i < devices.size(); i++) {
        if (devices[i].name == name) return static_cast<int>(i) + 1;
    }
    return -1;
}

std::wstring GetDeviceName(int device) {
    std::vector<audio::Device> devices = audio::ListDevices();
    if (device <= 0 || device > static_cast<int>(devices.size())) return L"";
    return devices[device - 1].name;
}

// The playback devices, for the device menu and Options: first "Default", which
// follows the system's default device as it changes (number 0 here, -1 once
// chosen), then each device
std::vector<AudioDeviceInfo> GetAudioDevices() {
    std::vector<AudioDeviceInfo> list;
    list.push_back({0, L"Default", g_selectedDevice == -1});
    std::vector<audio::Device> devices = audio::ListDevices();
    for (size_t i = 0; i < devices.size(); i++) {
        AudioDeviceInfo dev;
        dev.index = static_cast<int>(i) + 1;
        dev.name = devices[i].name;
        dev.current = dev.index == g_selectedDevice;
        list.push_back(dev);
    }
    return list;
}

// Select and switch to an audio device (0: the default)
void SelectAudioDevice(int deviceIndex) {
    if (deviceIndex < 0) return;
    if (deviceIndex == 0) deviceIndex = -1;
    std::wstring deviceName = deviceIndex > 0 ? GetDeviceName(deviceIndex) : L"Default";
    if (SwitchAudioDevice(deviceIndex)) {
        SaveSettings();
        Speak("Switched to " + WideToUtf8(deviceName));
    } else {
        Speak("Failed to switch audio device");
    }
}

// MIDI playback settings (SoundFont, voices): used once MIDI plays again.
void ApplyMidiSettings() {}

// The track has played to its end: the next one, playing or not.
static void OnTrackEnd() {
    if (g_autoAdvance || g_repeatMode != 0) {
        PostCommand(IDM_PLAY_NEXT, 0);
    } else {
        // Load the next track but don't play it (lParam 1)
        PostCommand(IDM_PLAY_NEXT, 1);
    }
}

// Start the audio engine on the saved device (or the default)
bool InitAudio() {
    audio::SetEndHandler(OnTrackEnd);
    audio::SetStreamTitleHandler([]() {
        AnnounceStreamMetadata();
        UpdateWindowTitle();
    });
    if (!audio::Init(g_selectedDeviceName, g_bufferSize)) {
        ShowMessage(L"FastPlay could not open an audio device.", APP_NAME, MessageIcon::Error);
        return false;
    }
    g_selectedDevice = audio::UsingDefaultDevice() ? -1 : FindDeviceByName(audio::CurrentDeviceName());
    if (g_selectedDevice == -1) g_selectedDeviceName.clear();
    audio::SetSmoothTransitions(g_smoothSeek);
    audio::SetTapBeforeEffects(!g_recordEffects);
    ApplyLiveRewindSetting();
    UpdateOutputGain();
    ApplyMidiSettings();
    return true;
}

void FreeAudio() {
    RemoveDSPEffects();
    audio::Shutdown();
    g_currentBitrate = 0;
}

std::wstring GetAudioEngineInfo() {
    std::wstring info = L"Audio engine: miniaudio, with " + Utf8ToWide(audio::DecoderVersion());
    info += L"\nOutput: " + audio::CurrentDeviceName();
    info += L", " + std::to_wstring(audio::MixSampleRate()) + L" Hz";
    audio::Stats stats = audio::GetStats();
    const int rate = audio::MixSampleRate() > 0 ? audio::MixSampleRate() : 48000;
    wchar_t line[256];
    swprintf(line, 256, L"\nDevice period: %.0f ms. Output buffer: %.0f ms, lowest it got: %.0f ms.",
             stats.periodFrames * 1000.0 / rate, stats.bufferFrames * 1000.0 / rate, stats.minBufferedMs);
    info += line;
    swprintf(line, 256, L"\nDropouts: %llu. Slowest block of processing: %.1f ms.",
             static_cast<unsigned long long>(stats.underruns), stats.maxBlockMs);
    info += line;
    return info;
}

// Check if a path is a URL
bool IsURL(const wchar_t* path) {
    if (!path) return false;
    return (WStrNICmp(path, L"http://", 7) == 0 ||
            WStrNICmp(path, L"https://", 8) == 0 ||
            WStrNICmp(path, L"ftp://", 6) == 0);
}

// Move playback to `seconds`. With smooth seeking the sound fades out and in and
// the 3D room rings on through it, as the other reverbs do; without, the room is
// cleared, so it does not go on sounding of where the music was after the cut.
// (A new track always starts with the room silent: loading resets it.)
static void JumpTo(double seconds) {
    audio::Seek(seconds);
    if (!g_smoothSeek) {
        if (SpatialAudio* spatial = GetSpatialAudio()) spatial->ClearTails();
    }
}

static bool IsMp4Path(const wchar_t* path) {
    const wchar_t* dot = wcsrchr(path, L'.');
    if (!dot) return false;
    for (const wchar_t* ext : {L".m4a", L".m4b", L".m4r", L".mp4"}) {
        if (WStrICmp(dot, ext) == 0) return true;
    }
    return false;
}

// The chapters of what was just loaded: the decoder's, or for an MP4 file FastPlay's
// own reading of it if the decoder found none.
static void ReadChapters(const wchar_t* path) {
    g_chapters.clear();
    if (const audio::Decoder* decoder = audio::Current()) g_chapters = decoder->Chapters();
    if (g_chapters.empty() && path && !IsURL(path) && IsMp4Path(path)) {
        ReadMp4Chapters(path, g_chapters);
    }
}

// Unload whatever is playing, effects first.
// The file loaded now, as it was loaded. Its position is saved under this name,
// not the playlist's current entry: the playlist may already hold what is to
// play next (a file opened from Explorer replaces it before it is played).
static std::wstring g_loadedPath;
static TrackMetadata g_loadedMetadata;

void SaveCurrentPosition() {
    if (!g_loadedPath.empty() && !g_isLiveStream) SaveFilePosition(g_loadedPath);
}

static void UnloadCurrent() {
    SaveCurrentPosition();
    g_loadedPath.clear();
	g_loadedMetadata = TrackMetadata();
    RemoveDSPEffects();
    audio::Unload();
    g_isLiveStream = false;
    g_currentBitrate = 0;
}

// Play a decoder just opened for `path`: the effects, gain, chapters and saved
// position, then play.
static bool StartDecoder(std::unique_ptr<audio::Decoder> decoder, const wchar_t* path) {
    g_isLiveStream = decoder->IsLive();
    g_currentBitrate = decoder->Bitrate();

    // The engine keeps these for the tempo processor (live streams keep their speed)
    audio::SetTempo(g_tempo);
    audio::SetPitch(g_pitch);
    audio::SetRate(g_rate);
    if (!audio::Load(std::move(decoder), static_cast<TempoAlgorithm>(g_tempoAlgorithm))) {
        g_isLoading = false;
        g_isLiveStream = false;
        if (g_playlist.size() <= 1) ShowMessage(L"FastPlay could not play it.", APP_NAME, MessageIcon::Error);
        return false;
    }

    g_loadedPath = path;
	g_loadedMetadata = GetTrackMetadata(path);
    ComputeReplayGainScale();
    UpdateOutputGain();
    ApplyDSPEffects();
    ReadChapters(path);

    // Restore the saved position for this file (if any)
    if (!g_isLiveStream) {
        double savedPos = LoadFilePosition(path);
        if (savedPos > 0) JumpTo(savedPos);
    }

    audio::Play();
    g_isLoading = false;
    UpdateWindowTitle();
    UpdateStatusBar();
    return true;
}

// Load and play a URL stream
bool LoadURL(const wchar_t* url) {
    g_isLoading = true;
    UnloadCurrent();

    // If the URL points at a playlist file (.m3u/.pls/.m3u8), resolve it to a
    // direct stream URL first. This covers saved favorites and directly-opened
    // URLs; the radio search path resolves separately. Falls back to the original
    // URL if resolution fails.
    std::wstring resolvedUrl = ResolvePlaylistUrl(url);

    std::wstring error;
    std::unique_ptr<audio::Decoder> decoder = audio::OpenDecoder(resolvedUrl, error);

    // If it couldn't be opened, the URL may redirect somewhere FFmpeg won't follow
    // (an https podcast enclosure that redirects to http). Resolve the redirect
    // chain ourselves and try the final URL.
    if (!decoder) {
        std::wstring finalUrl = ResolveHttpRedirects(resolvedUrl);
        if (finalUrl != resolvedUrl) {
            std::wstring retryError;
            decoder = audio::OpenDecoder(finalUrl, retryError);
        }
    }

    if (!decoder) {
        g_isLoading = false;
        // Show a shortened URL in the message
        std::wstring displayUrl = url;
        if (displayUrl.length() > 100) displayUrl = displayUrl.substr(0, 100) + L"...";
        ShowMessage((L"Cannot play URL:\n" + displayUrl + L"\n\n" + error).c_str(), APP_NAME, MessageIcon::Error);
        return false;
    }
    return StartDecoder(std::move(decoder), url);
}

// Load and play a file (or URL)
bool LoadFile(const wchar_t* path) {
    if (IsURL(path)) {
        return LoadURL(path);
    }
    g_isLoading = true;
    UnloadCurrent();

    std::wstring error;
    std::unique_ptr<audio::Decoder> decoder = audio::OpenDecoder(path, error);
    if (!decoder) {
        g_isLoading = false;
        // Only show the error if this is the only file in the playlist
        if (g_playlist.size() <= 1) {
            std::wstring msg = L"Cannot play file:\n";
            msg += GetFileName(path);
            msg += L"\n\n";
            msg += error;
            ShowMessage(msg.c_str(), APP_NAME, MessageIcon::Error);
        }
        return false;
    }
    return StartDecoder(std::move(decoder), path);
}

// Called on the UI thread when a stream's title changes - announces the new track
void AnnounceStreamMetadata() {
    std::string streamTitle = GetStreamTitle();
    if (streamTitle.empty()) return;

    // Record to song history (independent of speech setting)
    AddSongHistoryEntry(Utf8ToWide(streamTitle));

    if (g_speechTrackChange) {
        Speak(streamTitle);
    }
}

bool IsPlaying() {
    return audio::GetState() == audio::State::Playing;
}

// Play or pause current track
void PlayPause() {
    if (!audio::IsLoaded()) {
        // Nothing loaded - try to reload current track or play first
        Play();
        return;
    }

    if (IsPlaying()) {
        // For live streams, stop instead of pause
        if (g_isLiveStream && !IsRewindable()) {
            Stop();
        } else {
            Pause();
        }
    } else {
        audio::Play();
        UpdateWindowTitle();
        UpdateStatusBar();
    }
}

// Free current stream (used when stopping live streams)
void FreeCurrentStream() {
    UnloadCurrent();
}

// Play (restart if playing, resume if paused/stopped)
void Play() {
    if (!audio::IsLoaded()) {
        // Nothing loaded - reload the current track if there is one
        // (a live stream that was stopped was freed)
        if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
            LoadFile(g_playlist[g_currentTrack].c_str());
            return;
        }
        // No current track, try to play first track
        if (!g_playlist.empty()) {
            PlayTrack(0);
        }
        return;
    }

    if (IsPlaying() && !g_isLiveStream) {
        // Already playing - restart from beginning
        JumpTo(0);
    }
    audio::Play();
    UpdateWindowTitle();
    UpdateStatusBar();
}

// Pause playback
void Pause() {
    if (!audio::IsLoaded()) return;
    // Don't allow pausing live streams (unless kept for rewinding)
    if (g_isLiveStream && !IsRewindable()) {
        Speak("Cannot pause live stream");
        return;
    }
    audio::Pause();

    if (g_rewindOnPauseMs > 0) {
        Seek(-g_rewindOnPauseMs / 1000.0);
    }

    UpdateWindowTitle();
    UpdateStatusBar();
}

// Stop playback
void Stop() {
    if (audio::IsLoaded()) {
        // A live stream is disconnected entirely (otherwise it would buffer on and
        // stop/play would act like pause/resume)
        if (g_isLiveStream) {
            FreeCurrentStream();
        } else {
            audio::Stop();
            JumpTo(0);
        }
    }
    UpdateWindowTitle();
    UpdateStatusBar();
}

// Seek relative to current position
void Seek(double seconds) {
    if (!audio::IsLoaded() || g_isBusy || g_isLoading) return;
    if (g_isLiveStream) {
        // Kept for rewinding: back into what is kept, or forward as far as live
        double start, live;
        if (!audio::LiveRange(start, live)) return;
        double target = std::clamp(audio::Position() + seconds, start, live);
        JumpTo(target);
        if (target >= live) Speak("Live");
        UpdateStatusBar();
        return;
    }

    double length = audio::Length();
    if (length <= 0) return;  // Invalid or unknown length

    double newPos = audio::Position() + seconds;
    if (newPos < 0) newPos = 0;
    if (newPos > length) newPos = length;

    JumpTo(newPos);
    UpdateStatusBar();
}

// Seek by tracks (positive = forward, negative = backward)
void SeekTracks(int tracks) {
    if (g_playlist.empty() || g_isBusy) return;

    int newTrack = g_currentTrack + tracks;
    if (newTrack < 0) newTrack = 0;
    if (newTrack >= static_cast<int>(g_playlist.size())) {
        newTrack = static_cast<int>(g_playlist.size()) - 1;
    }

    if (newTrack != g_currentTrack) {
        PlayTrack(newTrack);
    }
}

// Seek to absolute position in seconds
void SeekToPosition(double seconds) {
    if (!audio::IsLoaded()) return;
    if (g_isLiveStream) {
        // Kept for rewinding: within what is kept (the engine holds it to that)
        if (!IsRewindable()) return;
        JumpTo(seconds);
        UpdateStatusBar();
        return;
    }

    double duration = audio::Length();
    if (seconds < 0) seconds = 0;
    if (seconds > duration) seconds = duration;

    JumpTo(seconds);
    UpdateStatusBar();
}

// ---------------------------------------------------------------------------
// Seek modes: jumping, or scrubbing while an arrow is held
// ---------------------------------------------------------------------------

static const char* const kSeekModeNames[] = {"Jump seeking", "Spring seeking", "Tape seeking"};

// Paused (or stopped) before scrubbing: paused again after
static bool g_pausedBeforeScrub = false;

static std::string SpeedText(int speed) { return std::to_string(speed) + " times"; }

void CycleSeekMode() {
    StopScrubbing();
    g_seekMode = (g_seekMode + 1) % SEEK_MODE_COUNT;
    Speak(kSeekModeNames[g_seekMode]);
}

bool IsScrubSeekMode() { return g_seekMode == SEEK_MODE_SPRING || g_seekMode == SEEK_MODE_TAPE; }

void ChangeScrubSpeed(int direction) {
    int& speed = g_seekMode == SEEK_MODE_TAPE ? g_tapeSpeed : g_springSpeed;
    // The next speed in the list, stopping at either end
    int index = 0;
    while (index < g_scrubSpeedCount - 1 && g_scrubSpeeds[index] < speed) index++;
    index = std::clamp(index + direction, 0, g_scrubSpeedCount - 1);
    speed = g_scrubSpeeds[index];
    audio::SetScrubSpeed(static_cast<float>(speed));
    Speak(SpeedText(speed));
}

void SpeakSeekMode() {
    if (!IsScrubSeekMode()) {
        SpeakSeekAmount();
        return;
    }
    Speak(std::string(kSeekModeNames[g_seekMode]) + ", " +
          SpeedText(g_seekMode == SEEK_MODE_TAPE ? g_tapeSpeed : g_springSpeed));
}

void StartScrubbing(int direction) {
    if (!audio::IsLoaded() || g_isBusy || g_isLoading) return;
    if (g_isLiveStream ? !IsRewindable() : audio::Length() <= 0) return;
    if (!audio::IsScrubbing()) g_pausedBeforeScrub = !IsPlaying();
    const bool tape = g_seekMode == SEEK_MODE_TAPE;
    if (!audio::StartScrub(tape ? audio::ScrubStyle::Tape : audio::ScrubStyle::Spring, direction,
                           static_cast<float>(tape ? g_tapeSpeed : g_springSpeed))) {
        return;
    }
    if (!IsPlaying()) audio::Play();
    UpdateStatusBar();
}

void StopScrubbing() {
    if (!audio::IsScrubbing()) return;
    audio::StopScrub();
    if (!g_smoothSeek) {
        if (SpatialAudio* spatial = GetSpatialAudio()) spatial->ClearTails();
    }
    if (g_pausedBeforeScrub) audio::Pause();
    UpdateWindowTitle();
    UpdateStatusBar();
}

void GoLive() {
    double start, live;
    if (!g_isLiveStream || !audio::LiveRange(start, live)) return;
    StopScrubbing();
    if (live - audio::Position() > 1.0) JumpTo(live);
    if (!IsPlaying()) audio::Play();
    Speak("Live");
    UpdateWindowTitle();
    UpdateStatusBar();
}

void ApplyLiveRewindSetting() {
    audio::SetLiveRewindSeconds(g_liveRewind ? g_liveRewindMinutes * 60 : 0);
}

// Get current playback position in seconds
double GetCurrentPosition() {
    return audio::Position();
}

double GetCurrentLength() {
    return audio::Length();
}

// Get index of current chapter based on playback position (-1 if no chapters)
int GetCurrentChapterIndex() {
    if (g_chapters.empty()) return -1;

    double pos = GetCurrentPosition();
    int currentChapter = -1;

    // Find the last chapter whose position is <= current position
    for (size_t i = 0; i < g_chapters.size(); i++) {
        if (g_chapters[i].position <= pos) {
            currentChapter = static_cast<int>(i);
        } else {
            break;  // Chapters are sorted
        }
    }
    return currentChapter;
}

// Say which chapter this is: its title, or its number if it has none
static void SpeakChapter(size_t index) {
    const Chapter& ch = g_chapters[index];
    Speak(ch.name.empty() ? "Chapter " + std::to_string(index + 1) : WideToUtf8(ch.name));
}

// Seek to next chapter (returns true if successful)
bool SeekToNextChapter() {
    if (g_chapters.empty() || !audio::IsLoaded()) return false;

    double pos = GetCurrentPosition();

    // Find the first chapter after current position (with small tolerance for current chapter)
    for (size_t i = 0; i < g_chapters.size(); i++) {
        if (g_chapters[i].position > pos + 0.5) {  // 0.5s tolerance
            SeekToPosition(g_chapters[i].position);

            SpeakChapter(i);
            return true;
        }
    }
    return false;  // Already at or past last chapter
}

// Seek to previous chapter (returns true if successful)
bool SeekToPrevChapter() {
    if (g_chapters.empty() || !audio::IsLoaded()) return false;

    double pos = GetCurrentPosition();

    // If we're more than 3 seconds into a chapter, go to its start;
    // otherwise go to the previous chapter
    int currentChapter = GetCurrentChapterIndex();

    if (currentChapter < 0) {
        // Before first chapter, go to start
        SeekToPosition(0);
        return true;
    }

    double chapterStart = g_chapters[currentChapter].position;

    if (pos - chapterStart > 3.0 && currentChapter >= 0) {
        // More than 3 seconds into current chapter - restart it
        SeekToPosition(chapterStart);
        SpeakChapter(static_cast<size_t>(currentChapter));
        return true;
    } else if (currentChapter > 0) {
        // Go to previous chapter
        int prevChapter = currentChapter - 1;
        SeekToPosition(g_chapters[prevChapter].position);
        SpeakChapter(static_cast<size_t>(prevChapter));
        return true;
    } else {
        // At first chapter, go to start
        SeekToPosition(0);
        Speak("Beginning");
        return true;
    }
}

// Set volume (0.0 - 1.0, more when amplifying). Applied after the recording tap,
// so recording captures full volume.
void SetVolume(float vol) {
    float maxVol = g_allowAmplify ? MAX_VOLUME_AMPLIFY : MAX_VOLUME_NORMAL;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > maxVol) vol = maxVol;
    g_volume = vol;
    UpdateOutputGain();

    // Announce volume if setting enabled
    if (g_speechVolume) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Volume %d%%", static_cast<int>(g_volume * 100 + 0.5f));
        Speak(buf);
    }

    UpdateStatusBar();
}

// Toggle mute (recording still captures full volume)
void ToggleMute() {
    g_muted = !g_muted;
    UpdateOutputGain();
    Speak(g_muted ? "Muted" : "Unmuted");
    UpdateStatusBar();
}

// Recompute ReplayGain for the current track and apply it at once, so changing
// the ReplayGain options takes effect without restarting the track.
void RefreshReplayGain() {
    ComputeReplayGainScale();
    UpdateOutputGain();
}

// Speak elapsed time
void SpeakElapsed() {
    if (!audio::IsLoaded()) return;
    Speak(WideToUtf8(FormatTime(audio::Position())));
}

// Speak remaining time
void SpeakRemaining() {
    if (!audio::IsLoaded()) return;
    double start, live;
    if (g_isLiveStream && audio::LiveRange(start, live)) {
        double behind = live - audio::Position();
        Speak(behind < 1.0 ? std::string("Live") : WideToUtf8(FormatTime(behind)) + " behind live");
        return;
    }
    double remaining = audio::Length() - audio::Position();
    if (remaining < 0) remaining = 0;
    Speak(WideToUtf8(FormatTime(remaining)));
}

// Speak total time
void SpeakTotal() {
    if (!audio::IsLoaded()) return;
    Speak(WideToUtf8(FormatTime(audio::Length())));
}

// Shuffle playback order. Rather than picking a random track on every advance
// (which makes small playlists replay the same handful of tracks before others
// have played), we build a fixed random permutation of the playlist and walk it
// in order, reshuffling only after every track has played once.
static std::vector<int> g_shuffleOrder;   // permutation of playlist indices
static int g_shufflePos = -1;             // index of the current track within g_shuffleOrder
static bool g_shuffleAdvance = false;     // set by Next/PrevTrack so PlayTrack keeps the current order

// Discard the current shuffle order so the next advance builds a fresh one.
// Called when shuffle is toggled on and when a new playlist/selection loads,
// so a fresh random order is produced instead of replaying the last one.
void ResetShuffleOrder() {
    g_shuffleOrder.clear();
    g_shufflePos = -1;
}

// Play a specific track by index
void PlayTrack(int index, bool autoPlay) {
    // Consume the advance flag up front so it never leaks past an early return.
    bool advancing = g_shuffleAdvance;
    g_shuffleAdvance = false;

    if (g_isBusy) return;  // Prevent re-entrancy
    if (index < 0 || index >= static_cast<int>(g_playlist.size())) {
        return;
    }

    // A direct selection or a freshly loaded playlist starts a new shuffle cycle;
    // only Next/Prev advances keep the existing order.
    if (!advancing) ResetShuffleOrder();

    g_isBusy = true;

    // (The position of what is playing is saved as it is unloaded, under its own name)

    // Try to load tracks, skipping failures (up to 10 attempts to prevent infinite loop)
    int attempts = 0;
    bool loadedSuccessfully = false;
    while (index < static_cast<int>(g_playlist.size()) && attempts < 10) {
        g_currentTrack = index;
        if (LoadFile(g_playlist[index].c_str())) {
            loadedSuccessfully = true;
            // Add to recent files (only for local files, not URLs)
            AddToRecentFiles(g_playlist[index]);
            break;  // Success
        }
        // Try next track if this one failed and we have multiple files
        if (g_playlist.size() > 1) {
            index++;
            attempts++;
        } else {
            break;  // Single file, don't loop
        }
    }

    // If autoPlay is false, pause straight after loading
    if (loadedSuccessfully && !autoPlay) {
        audio::Pause();
    }

    // Notify playlist dialog about track change
    if (loadedSuccessfully) {
        NotifyPlaylistTrackChanged();
    }

    // Announce track change if setting is enabled
    if (loadedSuccessfully && g_speechTrackChange) {
        // For streams, announce the stream title; for files, announce title or filename
        std::string streamTitle = GetStreamTitle();
        if (!streamTitle.empty()) {
            Speak(streamTitle);
        } else {
            std::string title = GetMetadataTag("TITLE");
            std::string artist = GetMetadataTag("ARTIST");
            if (!title.empty() && !artist.empty()) {
                Speak(artist + " - " + title);
            } else if (!title.empty()) {
                Speak(title);
            } else {
                // Fall back to filename
                Speak(WideToUtf8(GetTrackName(g_playlist[g_currentTrack])));
            }
        }
    }

    g_isBusy = false;
}

// Build a fresh Fisher-Yates shuffle of the current playlist. If startIndex is a
// valid index it is moved to the front so the currently-playing track stays put.
static void BuildShuffleOrder(int startIndex) {
    int n = static_cast<int>(g_playlist.size());
    g_shuffleOrder.resize(n);
    for (int i = 0; i < n; i++) g_shuffleOrder[i] = i;
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        std::swap(g_shuffleOrder[i], g_shuffleOrder[j]);
    }
    if (startIndex >= 0 && startIndex < n) {
        for (int i = 0; i < n; i++) {
            if (g_shuffleOrder[i] == startIndex) { std::swap(g_shuffleOrder[0], g_shuffleOrder[i]); break; }
        }
        g_shufflePos = 0;
    } else {
        g_shufflePos = -1;
    }
}

// Make sure g_shufflePos points at g_currentTrack, rebuilding the order if it's
// stale (playlist changed) or the user jumped to a track directly.
static void SyncShufflePos() {
    int n = static_cast<int>(g_playlist.size());
    if (static_cast<int>(g_shuffleOrder.size()) != n || g_shufflePos < 0) {
        BuildShuffleOrder(g_currentTrack);
        return;
    }
    if (g_shufflePos >= n || g_shuffleOrder[g_shufflePos] != g_currentTrack) {
        for (int i = 0; i < n; i++) {
            if (g_shuffleOrder[i] == g_currentTrack) { g_shufflePos = i; return; }
        }
        // Current track not in the order at all - rebuild around it.
        BuildShuffleOrder(g_currentTrack);
    }
}

// Play next track
void NextTrack(bool autoPlay) {
    if (g_playlist.empty() || g_isBusy) return;

    // Repeat one: restart current track
    if (g_repeatMode == 1 && g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
        g_shuffleAdvance = true;
        PlayTrack(g_currentTrack, autoPlay);
        return;
    }

    int next;
    if (g_shuffle && g_playlist.size() > 1) {
        int n = static_cast<int>(g_playlist.size());
        SyncShufflePos();
        g_shufflePos++;
        if (g_shufflePos >= n) {
            // Whole playlist has played - reshuffle for the next cycle.
            if (g_repeatMode == 2) {
                int last = g_currentTrack;
                BuildShuffleOrder(-1);
                // Avoid repeating the just-played track as the first of the new cycle.
                if (n > 1 && g_shuffleOrder[0] == last) std::swap(g_shuffleOrder[0], g_shuffleOrder[n - 1]);
                g_shufflePos = 0;
            } else {
                // End of playlist - stop.
                Stop();
                return;
            }
        }
        next = g_shuffleOrder[g_shufflePos];
    } else {
        next = g_currentTrack + 1;
        if (next >= static_cast<int>(g_playlist.size())) {
            // Repeat all: wrap to start
            if (g_repeatMode == 2) {
                next = 0;
            } else {
                // End of playlist - stop
                Stop();
                return;
            }
        }
    }
    g_shuffleAdvance = true;
    PlayTrack(next, autoPlay);
}

// Cycle repeat mode: off -> repeat one -> repeat all -> off
void ToggleRepeatMode() {
    g_repeatMode = (g_repeatMode + 1) % 3;
    const char* names[] = {"Repeat off", "Repeat track", "Repeat all"};
    Speak(names[g_repeatMode]);
    SaveSettings();
}

// Play previous track
void PrevTrack() {
    if (g_playlist.empty() || g_isBusy) return;

    // If we're more than 3 seconds in, restart current track
    if (audio::IsLoaded() && !g_isLiveStream && audio::Position() > 3.0) {
        JumpTo(0);
        UpdateStatusBar();
        return;
    }

    int prev;
    if (g_shuffle && g_playlist.size() > 1) {
        // Walk the shuffle order backward so Previous retraces the shuffled path.
        SyncShufflePos();
        if (g_shufflePos > 0) {
            g_shufflePos--;
            prev = g_shuffleOrder[g_shufflePos];
        } else {
            prev = g_currentTrack;  // already at the start of the cycle
        }
    } else {
        prev = g_currentTrack - 1;
        if (prev < 0) prev = 0;
    }
    g_shuffleAdvance = true;
    PlayTrack(prev);
}

// Move playback to another device, carrying on where it was
bool SwitchAudioDevice(int device) {
    // Save current state
    const bool wasLoaded = audio::IsLoaded();
    const bool wasPlaying = IsPlaying();
    double position = wasLoaded ? audio::Position() : 0.0;
    std::wstring currentFile;
    if (wasLoaded && g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
        currentFile = g_playlist[g_currentTrack];
    }

    UnloadCurrent();
    std::wstring name = device > 0 ? GetDeviceName(device) : L"";
    if (!audio::SwitchDevice(name, g_bufferSize)) {
        // Back to the default device
        audio::SwitchDevice(L"", g_bufferSize);
        g_selectedDevice = -1;
        g_selectedDeviceName.clear();
        return false;
    }
    g_selectedDevice = audio::UsingDefaultDevice() ? -1 : device;
    g_selectedDeviceName = audio::UsingDefaultDevice() ? L"" : audio::CurrentDeviceName();
    audio::SetSmoothTransitions(g_smoothSeek);  // over 8 ms at the new device's rate

    // Carry on with what was loaded
    if (!currentFile.empty() && LoadFile(currentFile.c_str())) {
        if (!g_isLiveStream) JumpTo(position);
        if (!wasPlaying) audio::Pause();
        UpdateWindowTitle();
        UpdateStatusBar();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tags. The decoder has them all in one list (ID3v1 and v2, Vorbis comments, APE,
// MP4, WMA and RIFF tags alike); internet radio adds its stream title and the
// station's headers.
// ---------------------------------------------------------------------------

// Helper: Parse iHeart-style StreamTitle
// Format 1: title="...",artist="...",url="..."
// Format 2: Artist - text="Title" song_spot="M" ...
static void ParseIHeartTitle(const std::string& streamTitle, std::string& artist, std::string& title) {
    // Check for Format 2: "Artist - text="Title" ..."
    size_t textPos = streamTitle.find(" - text=\"");
    if (textPos != std::string::npos) {
        // Artist is everything before " - text="
        artist = streamTitle.substr(0, textPos);
        // Title is inside text="..."
        size_t titleStart = textPos + 9;  // Skip ' - text="'
        size_t titleEnd = streamTitle.find("\"", titleStart);
        if (titleEnd != std::string::npos) {
            title = streamTitle.substr(titleStart, titleEnd - titleStart);
        }
        return;
    }

    // Check for Format 1: title="...",artist="..."
    if (streamTitle.find("title=\"") != std::string::npos) {
        size_t titleStart = streamTitle.find("title=\"");
        if (titleStart != std::string::npos) {
            titleStart += 7;  // Skip 'title="'
            size_t titleEnd = streamTitle.find("\"", titleStart);
            if (titleEnd != std::string::npos) {
                title = streamTitle.substr(titleStart, titleEnd - titleStart);
            }
        }
        size_t artistStart = streamTitle.find("artist=\"");
        if (artistStart != std::string::npos) {
            artistStart += 8;  // Skip 'artist="'
            size_t artistEnd = streamTitle.find("\"", artistStart);
            if (artistEnd != std::string::npos) {
                artist = streamTitle.substr(artistStart, artistEnd - artistStart);
            }
        }
    }
}

// Helper: Parse StreamTitle into artist and title (format: "Artist - Title" or iHeart format)
static void ParseStreamTitle(const std::string& streamTitle, std::string& artist, std::string& title) {
    artist = "";
    title = "";

    // First try iHeart format (title="...",artist="...")
    ParseIHeartTitle(streamTitle, artist, title);
    if (!title.empty() || !artist.empty()) {
        return;
    }

    // Then try standard "Artist - Title" format
    size_t sep = streamTitle.find(" - ");
    if (sep != std::string::npos) {
        artist = streamTitle.substr(0, sep);
        title = streamTitle.substr(sep + 3);
    } else {
        // No separator, treat entire string as title
        title = streamTitle;
    }
}

// The raw title of the stream playing (its StreamTitle), if any
static std::string RawStreamTitle() {
    const audio::Decoder* decoder = audio::Current();
    return decoder ? decoder->StreamTitle() : "";
}

// A tag from the file, or for a stream its title, artist, station and genre
static std::string GetMetadataTag(const char* tagName) {
    const audio::Decoder* decoder = audio::Current();
    if (!decoder) return "";

	// Service metadata describes the video; HLS container tags can instead
	// describe the transport (or the tool that produced it).
	auto supplied = g_loadedMetadata.tags.find(tagName);
	if (supplied != g_loadedMetadata.tags.end() && !supplied->second.empty()) return supplied->second;

    std::string result = decoder->Tag(tagName);
    if (!result.empty()) return result;

    // Internet radio: the current song's title and artist, the station's name
    // (as the title when there is no song) and genre
    const bool isTitle = StrICmp(tagName, "TITLE") == 0;
    const bool isArtist = StrICmp(tagName, "ARTIST") == 0;
    if (isTitle || isArtist) {
        std::string streamTitle = decoder->StreamTitle();
        if (!streamTitle.empty()) {
            std::string artist, title;
            ParseStreamTitle(streamTitle, artist, title);
            if (isTitle && !title.empty()) return title;
            if (isArtist && !artist.empty()) return artist;
        }
        if (isTitle) return decoder->Tag("icy-name");
    } else if (StrICmp(tagName, "GENRE") == 0) {
        return decoder->Tag("icy-genre");
    }
    return "";
}

// The stream's title as "Artist - Title" (or just the title)
static std::string GetStreamTitle() {
    std::string rawTitle = RawStreamTitle();
    if (rawTitle.empty()) return "";

    std::string artist, title;
    ParseStreamTitle(rawTitle, artist, title);
    if (!artist.empty() && !title.empty()) {
        return artist + " - " + title;
    } else if (!title.empty()) {
        return title;
    }
    return rawTitle;  // parsing failed
}

// Get station name from ICY headers
static std::string GetStationName() {
    const audio::Decoder* decoder = audio::Current();
    return decoder ? decoder->Tag("icy-name") : "";
}

// Helper to speak UTF-8 text with proper Unicode support
static void SpeakUtf8(const std::string& text) {
    SpeakW(Utf8ToWide(text));
}

// "Artist - Title", or whichever there is
static std::string TitleText() {
    std::string streamTitle = GetStreamTitle();
    if (!streamTitle.empty()) return streamTitle;
    std::string title = GetMetadataTag("TITLE");
    std::string artist = GetMetadataTag("ARTIST");
    if (!artist.empty() && !title.empty()) return artist + " - " + title;
    return !title.empty() ? title : artist;
}

static std::string YearText() {
    std::string year = GetMetadataTag("DATE");
    if (year.empty()) year = GetMetadataTag("YEAR");
    return year;
}

static std::string TrackText() {
    std::string track = GetMetadataTag("TRACKNUMBER");
    if (track.empty()) track = GetMetadataTag("TRACK");
    return track;
}

static std::string CommentText() {
    std::string comment = GetMetadataTag("COMMENT");
    if (comment.empty()) comment = GetMetadataTag("DESCRIPTION");
    return comment;
}

void SpeakTagTitle() {
    if (!audio::IsLoaded()) {
        Speak("Nothing playing");
        return;
    }
    std::string text = TitleText();
    if (!text.empty()) {
        SpeakUtf8(text);
    } else if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
        // No usable metadata - fall back to the filename
        SpeakW(GetTrackName(g_playlist[g_currentTrack]));
    } else {
        Speak("No title");
    }
}

void SpeakTagArtist() {
    if (!audio::IsLoaded()) {
        Speak("Nothing playing");
        return;
    }
    std::string artist = GetMetadataTag("ARTIST");
    if (!artist.empty()) {
        SpeakUtf8("Artist: " + artist);
    } else {
        Speak("No artist");
    }
}

void SpeakTagAlbum() {
    if (!audio::IsLoaded()) {
        Speak("Nothing playing");
        return;
    }
    std::string album = GetMetadataTag("ALBUM");
    // For streams, fall back to station name
    if (album.empty()) {
        std::string station = GetStationName();
        if (!station.empty()) {
            SpeakUtf8("Station: " + station);
            return;
        }
    }
    if (!album.empty()) {
        SpeakUtf8("Album: " + album);
    } else {
        Speak("No album");
    }
}

void SpeakTagYear() {
    if (!audio::IsLoaded()) {
        Speak("Nothing playing");
        return;
    }
    std::string year = YearText();
    if (!year.empty()) {
        SpeakUtf8("Year: " + year);
    } else {
        Speak("No year");
    }
}

void SpeakTagTrack() {
    if (!audio::IsLoaded()) {
        Speak("Nothing playing");
        return;
    }
    std::string track = TrackText();
    if (!track.empty()) {
        SpeakUtf8("Track: " + track);
    } else {
        Speak("No track number");
    }
}

void SpeakTagGenre() {
    if (!audio::IsLoaded()) {
        Speak("Nothing playing");
        return;
    }
    std::string genre = GetMetadataTag("GENRE");
    if (!genre.empty()) {
        SpeakUtf8("Genre: " + genre);
    } else {
        Speak("No genre");
    }
}

void SpeakTagComment() {
    if (!audio::IsLoaded()) {
        Speak("Nothing playing");
        return;
    }
    std::string comment = CommentText();
    if (!comment.empty()) {
        SpeakUtf8("Comment: " + comment);
    } else {
        Speak("No comment");
    }
}

// "128 kbps, 44100 Hz, stereo", or for a lossless file "16-bit, 44100 Hz, stereo"
static std::string FormatText(bool capitalized) {
    const audio::Decoder* decoder = audio::Current();
    if (!decoder) return "";
    int channels = decoder->SourceChannels();
    const char* layout = channels == 1 ? (capitalized ? "Mono" : "mono")
                         : channels == 2 ? (capitalized ? "Stereo" : "stereo")
                                         : (capitalized ? "Multi-channel" : "multi-channel");
    int bitrate = GetCurrentBitrate();
    char buf[128];
    if (bitrate > 0) {
        snprintf(buf, sizeof(buf), "%d kbps, %d Hz, %s", bitrate, decoder->SourceSampleRate(), layout);
    } else if (decoder->SourceBits() > 0) {
        snprintf(buf, sizeof(buf), "%d-bit, %d Hz, %s", decoder->SourceBits(), decoder->SourceSampleRate(), layout);
    } else {
        snprintf(buf, sizeof(buf), "%d Hz, %s", decoder->SourceSampleRate(), layout);
    }
    return buf;
}

void SpeakTagBitrate() {
    if (!audio::IsLoaded()) {
        Speak("Nothing playing");
        return;
    }
    Speak(FormatText(false));
}

int GetCurrentBitrate() {
    const audio::Decoder* decoder = audio::Current();
    if (!decoder) return 0;
    // The decoder's (a VBR file's recent average), else a stream's header
    int bitrate = decoder->Bitrate();
    if (bitrate > 0) return bitrate;
    std::string icy = decoder->Tag("icy-br");
    if (!icy.empty()) return atoi(icy.c_str());
	if (g_loadedMetadata.bitrate > 0) return g_loadedMetadata.bitrate;
    return g_currentBitrate;
}

bool IsCurrentVbr() {
    const audio::Decoder* decoder = audio::Current();
    return decoder && decoder->IsVbr();
}

void SpeakTagDuration() {
    if (!audio::IsLoaded()) {
        Speak("Nothing playing");
        return;
    }

	double length = audio::Length();
	if (length <= 0) length = g_loadedMetadata.duration;
    if (length <= 0) {
        if (g_isLiveStream || (g_currentTrack >= 0 && g_currentTrack < (int)g_playlist.size() &&
                               IsURL(g_playlist[g_currentTrack].c_str()))) {
            Speak("Live stream");
            return;
        }
        Speak("Unknown duration");
        return;
    }

    int totalSecs = (int)length;
    int hours = totalSecs / 3600;
    int mins = (totalSecs % 3600) / 60;
    int secs = totalSecs % 60;

    char buf[64];
    if (hours > 0) {
        snprintf(buf, sizeof(buf), "Duration: %d hours, %d minutes, %d seconds", hours, mins, secs);
    } else if (mins > 0) {
        snprintf(buf, sizeof(buf), "Duration: %d minutes, %d seconds", mins, secs);
    } else {
        snprintf(buf, sizeof(buf), "Duration: %d seconds", secs);
    }

    Speak(buf);
}

void SpeakTagFilename() {
    if (g_currentTrack < 0 || g_currentTrack >= (int)g_playlist.size()) {
        Speak("Nothing playing");
        return;
    }

    std::wstring path = g_playlist[g_currentTrack];
	if (audio::IsLoaded() && !g_loadedMetadata.sourceUrl.empty()) path = g_loadedMetadata.sourceUrl;

    // Check if it's a URL
    if (IsURL(path.c_str())) {
        // For streams, show the full URL
        SpeakW(L"URL: " + path);
        return;
    }

    SpeakW(L"Filename: " + GetFileName(path));
}

// Tag retrieval functions for display in dialog
std::wstring GetTagTitle() {
    if (!audio::IsLoaded()) return L"Nothing playing";
    std::string text = TitleText();
    return text.empty() ? L"No title" : Utf8ToWide(text);
}

std::wstring GetTagArtist() {
    if (!audio::IsLoaded()) return L"Nothing playing";
    std::string artist = GetMetadataTag("ARTIST");
    return artist.empty() ? L"No artist" : Utf8ToWide(artist);
}

std::wstring GetTagAlbum() {
    if (!audio::IsLoaded()) return L"Nothing playing";
    std::string album = GetMetadataTag("ALBUM");
    if (album.empty()) {
        std::string station = GetStationName();
        if (!station.empty()) return Utf8ToWide(station);
    }
    return album.empty() ? L"No album" : Utf8ToWide(album);
}

std::wstring GetTagYear() {
    if (!audio::IsLoaded()) return L"Nothing playing";
    std::string year = YearText();
    return year.empty() ? L"No year" : Utf8ToWide(year);
}

std::wstring GetTagTrack() {
    if (!audio::IsLoaded()) return L"Nothing playing";
    std::string track = TrackText();
    return track.empty() ? L"No track" : Utf8ToWide(track);
}

std::wstring GetTagGenre() {
    if (!audio::IsLoaded()) return L"Nothing playing";
    std::string genre = GetMetadataTag("GENRE");
    return genre.empty() ? L"No genre" : Utf8ToWide(genre);
}

std::wstring GetTagComment() {
    if (!audio::IsLoaded()) return L"Nothing playing";
    std::string comment = CommentText();
    return comment.empty() ? L"No comment" : Utf8ToWide(comment);
}

std::wstring GetTagBitrate() {
    if (!audio::IsLoaded()) return L"Nothing playing";
    std::string text = FormatText(true);
    return text.empty() ? L"Unknown bitrate" : Utf8ToWide(text);
}

std::wstring GetTagDuration() {
    if (!audio::IsLoaded()) return L"Nothing playing";
    double length = audio::Length();
	if (length <= 0) length = g_loadedMetadata.duration;
    if (length <= 0) return g_isLiveStream ? L"Live stream" : L"Unknown duration";
    return FormatTime(length);
}

std::wstring GetTagFilename() {
    if (g_currentTrack < 0 || g_currentTrack >= (int)g_playlist.size()) {
        return L"Nothing playing";
    }

    std::wstring path = g_playlist[g_currentTrack];
	if (audio::IsLoaded() && !g_loadedMetadata.sourceUrl.empty()) return g_loadedMetadata.sourceUrl;
    if (IsURL(path.c_str())) return path;
    return GetFileName(path);
}

// ---------------------------------------------------------------------------
// Recording: what plays (after the effects, before the volume) to a file
// ---------------------------------------------------------------------------

static std::unique_ptr<audio::Recorder> g_recorder;

static void RecordingTap(const float* samples, int frames, int, int, void* user) {
    static_cast<audio::Recorder*>(user)->Write(samples, frames);
}

// The file name from the template, with the format's extension
static std::wstring GenerateRecordingFilename(int format) {
    time_t now = time(nullptr);
    struct tm localTime;
    LocalTime(now, localTime);
    wchar_t buffer[256];
    wcsftime(buffer, 256, g_recordTemplate.c_str(), &localTime);

    const wchar_t* ext;
    switch (format) {
        case 1: ext = L".mp3"; break;
        case 2: ext = L".ogg"; break;
        case 3: ext = L".flac"; break;
        default: ext = L".wav"; break;
    }
    return std::wstring(buffer) + ext;
}

void StopRecording() {
    if (!g_isRecording) return;
    // No more blocks after this; then the file is finished
    audio::SetTap(nullptr, nullptr);
    g_recorder.reset();
    g_isRecording = false;

    Speak("Recording stopped");
    UpdateStatusBar();
}

void ToggleRecording() {
    if (g_isRecording) {
        StopRecording();
        return;
    }

    // Need something playing to record
    if (!audio::IsLoaded()) {
        Speak("Nothing to record");
        return;
    }

    // The folder: the one chosen, else Music, else the current directory
    std::wstring outputPath = g_recordPath;
    if (outputPath.empty()) {
        outputPath = GetUserMusicDir();
        if (outputPath.empty()) {
            std::error_code ec;
            outputPath = std::filesystem::current_path(ec).wstring();
        }
    }
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(outputPath), ec);
    }
    if (!outputPath.empty() && outputPath.back() != L'\\' && outputPath.back() != L'/') {
        outputPath += kPathSeparator;
    }

    std::wstring error;
    int format = std::clamp(g_recordFormat, 0, 3);
    g_recorder = audio::Recorder::Start(outputPath + GenerateRecordingFilename(format),
                                        static_cast<audio::RecordFormat>(format), g_recordBitrate,
                                        audio::MixSampleRate(), error);
    if (!g_recorder && format != 0) {
        // Fall back to WAV if the chosen encoder cannot start
        ShowMessage((error + L"\nRecording to WAV instead.").c_str(), APP_NAME, MessageIcon::Warning);
        g_recorder = audio::Recorder::Start(outputPath + GenerateRecordingFilename(0), audio::RecordFormat::Wav,
                                            g_recordBitrate, audio::MixSampleRate(), error);
    }
    if (!g_recorder) {
        ShowMessage((L"Failed to start recording.\n\n" + error).c_str(), APP_NAME, MessageIcon::Error);
        return;
    }

    audio::SetTap(RecordingTap, g_recorder.get());
    g_isRecording = true;
    Speak("Recording started");
    UpdateStatusBar();
}
