#include "settings.h"
#include "youtube_tools.h"
#include "ini.h"
#include "globals.h"
#include "player.h"
#include "effects.h"
#include "convolution.h"
#include "database.h"
#include "accessibility.h"
#include "audio.h"
#include "paths.h"
#include "utils.h"
#include "commands.h"
#include <algorithm>
#include <cwchar>
#include <cstdio>

// Initialize config file path
void InitConfigPath() {
    g_configPath = GetDataDirectory() + L"FastPlay.ini";
}

// Load settings from INI file
void LoadSettings() {
    InitConfigPath();

    // Load device name (empty means default device)
    wchar_t deviceName[256] = {0};
    IniGetString(L"Playback", L"DeviceName", L"", deviceName, 256, g_configPath.c_str());
    g_selectedDeviceName = deviceName;
    g_selectedDevice = -1;  // Will be resolved by name in InitAudio

    g_rewindOnPauseMs = IniGetInt(L"Playback", L"RewindOnPauseMs", 0, g_configPath.c_str());
    if (g_rewindOnPauseMs < 0) g_rewindOnPauseMs = 0;

    g_allowAmplify = IniGetInt(L"Playback", L"AllowAmplify", 0, g_configPath.c_str()) != 0;
    g_rememberState = IniGetInt(L"Playback", L"RememberState", 0, g_configPath.c_str()) != 0;
    g_rememberPosMinutes = IniGetInt(L"Playback", L"RememberPosMinutes", 0, g_configPath.c_str());
    g_bringToFront = IniGetInt(L"Playback", L"BringToFront", 1, g_configPath.c_str()) != 0;
    g_minimizeToTray = IniGetInt(L"Playback", L"MinimizeToTray", 1, g_configPath.c_str()) != 0;
    g_loadFolder = IniGetInt(L"Playback", L"LoadFolder", 0, g_configPath.c_str()) != 0;
    g_registerFileTypes = IniGetInt(L"Playback", L"RegisterFileTypes", 0, g_configPath.c_str()) != 0;
    g_volumeStep = IniGetInt(L"Playback", L"VolumeStep", 2, g_configPath.c_str()) / 100.0f;
    if (g_volumeStep < 0.01f) g_volumeStep = 0.01f;
    if (g_volumeStep > 0.25f) g_volumeStep = 0.25f;
    g_replayGainMode = IniGetInt(L"Playback", L"ReplayGainMode", 0, g_configPath.c_str());
    if (g_replayGainMode < 0 || g_replayGainMode > 2) g_replayGainMode = 0;
    g_replayGainPreamp = IniGetInt(L"Playback", L"ReplayGainPreamp", 0, g_configPath.c_str()) / 100.0f;
    if (g_replayGainPreamp < -15.0f) g_replayGainPreamp = -15.0f;
    if (g_replayGainPreamp > 15.0f) g_replayGainPreamp = 15.0f;
    g_replayGainPreventClip = IniGetInt(L"Playback", L"ReplayGainPreventClip", 1, g_configPath.c_str()) != 0;
    g_showTitleInWindow = IniGetInt(L"Playback", L"ShowTitleInWindow", 1, g_configPath.c_str()) != 0;
    g_volume = IniGetInt(L"Playback", L"Volume", 100, g_configPath.c_str()) / 100.0f;

    // Clamp volume
    float maxVol = g_allowAmplify ? MAX_VOLUME_AMPLIFY : MAX_VOLUME_NORMAL;
    if (g_volume < 0.0f) g_volume = 0.0f;
    if (g_volume > maxVol) g_volume = maxVol;

    // Load stream effect values (pitch, tempo, rate)
    wchar_t buf[32] = {0};
    IniGetString(L"Playback", L"Pitch", L"0", buf, 32, g_configPath.c_str());
    g_pitch = static_cast<float>(std::wcstod(buf, nullptr));
    if (g_pitch < -12.0f) g_pitch = -12.0f;
    if (g_pitch > 12.0f) g_pitch = 12.0f;

    IniGetString(L"Playback", L"Tempo", L"0", buf, 32, g_configPath.c_str());
    g_tempo = static_cast<float>(std::wcstod(buf, nullptr));
    if (g_tempo < -75.0f) g_tempo = -75.0f;
    if (g_tempo > 200.0f) g_tempo = 200.0f;

    IniGetString(L"Playback", L"Rate", L"1.0", buf, 32, g_configPath.c_str());
    g_rate = static_cast<float>(std::wcstod(buf, nullptr));
    if (g_rate < 0.25f) g_rate = 0.25f;
    if (g_rate > 4.0f) g_rate = 4.0f;

    // Load advanced settings (buffer)
    g_bufferSize = IniGetInt(L"Advanced", L"BufferSize", 500, g_configPath.c_str());
    if (g_bufferSize < 100) g_bufferSize = 100;
    if (g_bufferSize > 5000) g_bufferSize = 5000;


    // 1 Speedy, 2 Signalsmith. 0 was SoundTouch, which is gone: Signalsmith instead.
    g_tempoAlgorithm = IniGetInt(L"Advanced", L"TempoAlgorithm", 2, g_configPath.c_str());
    if (g_tempoAlgorithm != static_cast<int>(TempoAlgorithm::Speedy)) {
        g_tempoAlgorithm = static_cast<int>(TempoAlgorithm::Signalsmith);
    }

    g_disableBatchDelay = IniGetInt(L"Advanced", L"DisableBatchDelay", 0, g_configPath.c_str()) != 0;
    g_smoothSeek = IniGetInt(L"Advanced", L"SmoothSeek", 1, g_configPath.c_str()) != 0;
    g_liveRewind = IniGetInt(L"Advanced", L"LiveRewind", 0, g_configPath.c_str()) != 0;

    // Library folders: Folder0=path, Tagged0=1, ...
    g_libraryFolders.clear();
    int libraryCount = IniGetInt(L"Library", L"Count", 0, g_configPath.c_str());
    for (int i = 0; i < libraryCount; i++) {
        wchar_t folderBuf[1024] = {0};
        IniGetString(L"Library", (L"Folder" + std::to_wstring(i)).c_str(), L"", folderBuf, 1024, g_configPath.c_str());
        if (!folderBuf[0]) continue;
        LibraryFolder folder;
        folder.path = folderBuf;
        folder.tagged = IniGetInt(L"Library", (L"Tagged" + std::to_wstring(i)).c_str(), 1, g_configPath.c_str()) != 0;
        g_libraryFolders.push_back(folder);
    }
    g_liveRewindMinutes = std::clamp(IniGetInt(L"Advanced", L"LiveRewindMinutes", 30, g_configPath.c_str()), 1, 240);


    // Load Speedy settings
    g_speedyNonlinear = IniGetInt(L"Speedy", L"NonlinearSpeedup", 1, g_configPath.c_str()) != 0;

    // Load Signalsmith Stretch settings
    g_ssPreset = IniGetInt(L"Signalsmith", L"Preset", 0, g_configPath.c_str());
    if (g_ssPreset < 0) g_ssPreset = 0;
    if (g_ssPreset > 1) g_ssPreset = 1;
    g_ssTonalityLimit = IniGetInt(L"Signalsmith", L"TonalityLimit", 0, g_configPath.c_str());
    if (g_ssTonalityLimit < 0) g_ssTonalityLimit = 0;
    if (g_ssTonalityLimit > 20000) g_ssTonalityLimit = 20000;

    // Load reverb algorithm (0=Off, 1=Simple, 2=Advanced; the old 2=DX8 and 3=I3DL2 become Advanced)
    g_reverbAlgorithm = IniGetInt(L"Effects", L"ReverbAlgorithm", 0, g_configPath.c_str());
    if (g_reverbAlgorithm < 0) g_reverbAlgorithm = 0;
    if (g_reverbAlgorithm >= (int)ReverbAlgorithm::COUNT) g_reverbAlgorithm = (int)ReverbAlgorithm::Advanced;

    // Load MIDI settings
    wchar_t midiBuf[kMaxPathChars] = {0};
    IniGetString(L"MIDI", L"SoundFont", L"", midiBuf, kMaxPathChars, g_configPath.c_str());
    g_midiSoundFont = midiBuf;
    g_midiMaxVoices = IniGetInt(L"MIDI", L"MaxVoices", 128, g_configPath.c_str());
    if (g_midiMaxVoices < 1) g_midiMaxVoices = 1;
    if (g_midiMaxVoices > 1000) g_midiMaxVoices = 1000;
    g_midiSincInterp = IniGetInt(L"MIDI", L"SincInterp", 0, g_configPath.c_str()) != 0;

    // EQ frequencies loaded using string conversion (IniGetFloat defined later)
    wchar_t eqBuf[32];
    IniGetString(L"Advanced", L"EQBassFreq", L"50", eqBuf, 32, g_configPath.c_str());
    g_eqBassFreq = static_cast<float>(std::wcstod(eqBuf, nullptr));
    IniGetString(L"Advanced", L"EQMidFreq", L"1000", eqBuf, 32, g_configPath.c_str());
    g_eqMidFreq = static_cast<float>(std::wcstod(eqBuf, nullptr));
    IniGetString(L"Advanced", L"EQTrebleFreq", L"12000", eqBuf, 32, g_configPath.c_str());
    g_eqTrebleFreq = static_cast<float>(std::wcstod(eqBuf, nullptr));

    // Load YouTube settings
    wchar_t ytBuf[512] = {0};
	SetYouTubeToolSettings(ReadYouTubeToolSettings(g_configPath));
    IniGetString(L"YouTube", L"ApiKey", L"", ytBuf, 512, g_configPath.c_str());
    g_ytApiKey = ytBuf;
    g_ytFavoritesSort = IniGetInt(L"YouTube", L"FavoritesSort", 0, g_configPath.c_str()) == 1 ? 1 : 0;
    g_ytAutoRefresh = std::clamp(IniGetInt(L"YouTube", L"AutoRefresh", 0, g_configPath.c_str()), 0, 6);
    // YouTube downloads
    IniGetString(L"YouTube", L"DownloadFolder", L"", ytBuf, 512, g_configPath.c_str());
    g_ytDownload.folder = ytBuf;
    g_ytDownload.type = std::clamp(IniGetInt(L"YouTube", L"DownloadType", 0, g_configPath.c_str()), 0, 1);
    g_ytDownload.audioFormat = std::clamp(IniGetInt(L"YouTube", L"DownloadAudioFormat", 0, g_configPath.c_str()), 0, 5);
    g_ytDownload.audioQuality = std::clamp(IniGetInt(L"YouTube", L"DownloadAudioQuality", 0, g_configPath.c_str()), 0, 4);
    g_ytDownload.videoQuality = std::clamp(IniGetInt(L"YouTube", L"DownloadVideoQuality", 0, g_configPath.c_str()), 0, 6);
    g_ytDownload.videoContainer = std::clamp(IniGetInt(L"YouTube", L"DownloadVideoContainer", 0, g_configPath.c_str()), 0, 2);
    g_ytDownload.videoCodec = std::clamp(IniGetInt(L"YouTube", L"DownloadVideoCodec", 0, g_configPath.c_str()), 0, 3);
    g_ytDownload.naming = std::clamp(IniGetInt(L"YouTube", L"DownloadNaming", 0, g_configPath.c_str()), 0, 3);
    g_ytDownload.addMetadata = IniGetInt(L"YouTube", L"DownloadAddMetadata", 0, g_configPath.c_str()) != 0;
    g_ytDownload.embedThumbnail = IniGetInt(L"YouTube", L"DownloadEmbedThumbnail", 0, g_configPath.c_str()) != 0;
    g_ytDownload.writeThumbnail = IniGetInt(L"YouTube", L"DownloadWriteThumbnail", 0, g_configPath.c_str()) != 0;
    g_ytDownload.writeDescription = IniGetInt(L"YouTube", L"DownloadWriteDescription", 0, g_configPath.c_str()) != 0;
    g_ytDownload.writeSubtitles = IniGetInt(L"YouTube", L"DownloadWriteSubtitles", 0, g_configPath.c_str()) != 0;
    g_ytDownload.embedSubtitles = IniGetInt(L"YouTube", L"DownloadEmbedSubtitles", 0, g_configPath.c_str()) != 0;
    g_ytDownload.channelFolder = IniGetInt(L"YouTube", L"DownloadChannelFolder", 0, g_configPath.c_str()) != 0;
    IniGetString(L"YouTube", L"DownloadExtraOptions", L"", ytBuf, 512, g_configPath.c_str());
    g_ytDownload.extraOptions = ytBuf;

    // Load downloads settings
    wchar_t dlBuf[512] = {0};
    IniGetString(L"Downloads", L"Path", L"", dlBuf, 512, g_configPath.c_str());
    g_downloadPath = dlBuf;
    g_downloadOrganizeByFeed = IniGetInt(L"Downloads", L"OrganizeByFeed", 0, g_configPath.c_str()) != 0;

    // Load recording settings
    wchar_t recBuf[512] = {0};
    IniGetString(L"Recording", L"Path", L"", recBuf, 512, g_configPath.c_str());
    g_recordPath = recBuf;
    IniGetString(L"Recording", L"Template", L"%Y-%m-%d_%H-%M-%S", recBuf, 512, g_configPath.c_str());
    g_recordTemplate = recBuf;
    g_recordFormat = IniGetInt(L"Recording", L"Format", 0, g_configPath.c_str());
    if (g_recordFormat < 0) g_recordFormat = 0;
    if (g_recordFormat > 3) g_recordFormat = 3;
    g_recordBitrate = IniGetInt(L"Recording", L"Bitrate", 192, g_configPath.c_str());
    g_recordEffects = IniGetInt(L"Recording", L"Effects", 1, g_configPath.c_str()) != 0;

    // Load speech settings
    g_speechTrackChange = IniGetInt(L"Speech", L"TrackChange", 0, g_configPath.c_str()) != 0;
    g_speechVolume = IniGetInt(L"Speech", L"Volume", 1, g_configPath.c_str()) != 0;
    g_speechEffect = IniGetInt(L"Speech", L"Effect", 1, g_configPath.c_str()) != 0;

    // Load shuffle and auto-advance settings
    g_shuffle = IniGetInt(L"Playback", L"Shuffle", 0, g_configPath.c_str()) != 0;
    g_autoAdvance = IniGetInt(L"Playback", L"AutoAdvance", 1, g_configPath.c_str()) != 0;
    g_repeatMode = IniGetInt(L"Playback", L"RepeatMode", 0, g_configPath.c_str());
    if (g_repeatMode < 0 || g_repeatMode > 2) g_repeatMode = 0;
    g_playlistFollowPlayback = IniGetInt(L"Playback", L"PlaylistFollow", 1, g_configPath.c_str()) != 0;
    g_checkForUpdates = IniGetInt(L"Playback", L"CheckForUpdates", 1, g_configPath.c_str()) != 0;
    g_allowMultipleInstances = IniGetInt(L"Playback", L"AllowMultipleInstances", 0, g_configPath.c_str()) != 0;

    // Load seek settings
    g_seekEnabled[0] = IniGetInt(L"Movement", L"Seek1s", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[1] = IniGetInt(L"Movement", L"Seek5s", 1, g_configPath.c_str()) != 0;
    g_seekEnabled[2] = IniGetInt(L"Movement", L"Seek10s", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[3] = IniGetInt(L"Movement", L"Seek30s", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[4] = IniGetInt(L"Movement", L"Seek1m", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[5] = IniGetInt(L"Movement", L"Seek5m", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[6] = IniGetInt(L"Movement", L"Seek10m", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[7] = IniGetInt(L"Movement", L"Seek30m", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[8] = IniGetInt(L"Movement", L"Seek1h", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[9] = IniGetInt(L"Movement", L"Seek1t", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[10] = IniGetInt(L"Movement", L"Seek5t", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[11] = IniGetInt(L"Movement", L"Seek10t", 0, g_configPath.c_str()) != 0;
    g_chapterSeekEnabled = IniGetInt(L"Movement", L"ChapterSeek", 1, g_configPath.c_str()) != 0;
    g_currentSeekIndex = IniGetInt(L"Movement", L"CurrentSeek", 1, g_configPath.c_str());
    g_seekMode = std::clamp(IniGetInt(L"Movement", L"SeekMode", SEEK_MODE_JUMP, g_configPath.c_str()), 0,
                            SEEK_MODE_COUNT - 1);
    g_springSpeed = std::clamp(IniGetInt(L"Movement", L"SpringSpeed", 16, g_configPath.c_str()), g_scrubSpeeds[0],
                               g_scrubSpeeds[g_scrubSpeedCount - 1]);
    g_tapeSpeed = std::clamp(IniGetInt(L"Movement", L"TapeSpeed", 4, g_configPath.c_str()), g_scrubSpeeds[0],
                             g_scrubSpeeds[g_scrubSpeedCount - 1]);

    // Validate current seek index
    if (g_currentSeekIndex < 0 || g_currentSeekIndex >= g_seekAmountCount || !g_seekEnabled[g_currentSeekIndex]) {
        // Find first enabled seek amount
        g_currentSeekIndex = 1;  // Default to 5s
        for (int i = 0; i < g_seekAmountCount; i++) {
            if (g_seekEnabled[i]) {
                g_currentSeekIndex = i;
                break;
            }
        }
    }

    // Load effect settings
    g_effectEnabled[0] = IniGetInt(L"Effects", L"Volume", 1, g_configPath.c_str()) != 0;  // Volume enabled by default
    g_effectEnabled[1] = IniGetInt(L"Effects", L"Pitch", 0, g_configPath.c_str()) != 0;
    g_effectEnabled[2] = IniGetInt(L"Effects", L"Tempo", 0, g_configPath.c_str()) != 0;
    g_effectEnabled[3] = IniGetInt(L"Effects", L"Rate", 0, g_configPath.c_str()) != 0;
    g_currentEffectIndex = IniGetInt(L"Effects", L"CurrentEffect", 0, g_configPath.c_str());
    g_rateStepMode = IniGetInt(L"Effects", L"RateStepMode", 0, g_configPath.c_str());
    if (g_rateStepMode < 0 || g_rateStepMode > 1) g_rateStepMode = 0;

    // Validate current effect index
    if (g_currentEffectIndex < 0 || g_currentEffectIndex >= 4 || !g_effectEnabled[g_currentEffectIndex]) {
        // Find first enabled effect
        g_currentEffectIndex = 0;  // Default to volume
        for (int i = 0; i < 4; i++) {
            if (g_effectEnabled[i]) {
                g_currentEffectIndex = i;
                break;
            }
        }
    }

    // Note: DSP effect enabled states are loaded in LoadDSPSettings() after InitEffects()
}

// Helper to read float from INI with default
static float IniGetFloat(const wchar_t* section, const wchar_t* key, float defaultVal, const wchar_t* path) {
    wchar_t buf[32] = {0};
    IniGetString(section, key, L"", buf, 32, path);
    if (buf[0] == L'\0') return defaultVal;
    return static_cast<float>(std::wcstod(buf, nullptr));
}

// Load DSP effect settings (call after InitEffects)
void LoadDSPSettings() {
    // Load DSP effect enabled states
    EnableDSPEffect(DSPEffectType::Reverb, IniGetInt(L"DSPEffects", L"Reverb", 0, g_configPath.c_str()) != 0);
    EnableDSPEffect(DSPEffectType::Echo, IniGetInt(L"DSPEffects", L"Echo", 0, g_configPath.c_str()) != 0);
    EnableDSPEffect(DSPEffectType::EQ, IniGetInt(L"DSPEffects", L"EQ", 0, g_configPath.c_str()) != 0);
    EnableDSPEffect(DSPEffectType::Compressor, IniGetInt(L"DSPEffects", L"Compressor", 0, g_configPath.c_str()) != 0);
    EnableDSPEffect(DSPEffectType::StereoWidth, IniGetInt(L"DSPEffects", L"StereoWidth", 0, g_configPath.c_str()) != 0);
    EnableDSPEffect(DSPEffectType::CenterCancel, IniGetInt(L"DSPEffects", L"CenterCancel", 0, g_configPath.c_str()) != 0);
    EnableDSPEffect(DSPEffectType::Convolution, IniGetInt(L"DSPEffects", L"Convolution", 0, g_configPath.c_str()) != 0);
    EnableDSPEffect(DSPEffectType::SpatialAudio, IniGetInt(L"DSPEffects", L"SpatialAudio", 0, g_configPath.c_str()) != 0);
    EnableDSPEffect(DSPEffectType::Normalizer, IniGetInt(L"DSPEffects", L"Normalizer", 0, g_configPath.c_str()) != 0);

    // Load convolution IR path
    {
        wchar_t irPath[kMaxPathChars] = {0};
        IniGetString(L"DSPEffects", L"ConvolutionIR", L"", irPath, kMaxPathChars, g_configPath.c_str());
        g_convolutionIRPath = irPath;
        if (!g_convolutionIRPath.empty()) {
            ConvolutionReverb* conv = GetConvolutionReverb();
            if (conv) {
                conv->LoadIR(g_convolutionIRPath.c_str());
            }
        }
    }

    // Load DSP parameter values (use GetParamDef for defaults)
    const ParamDef* def;

    // Reverb parameters (each preset first: setting it overwrites the values after it)
    def = GetParamDef(ParamId::ReverbPreset);
    SetParamValue(ParamId::ReverbPreset, IniGetFloat(L"DSPParams", L"ReverbPreset", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::ReverbMix);
    SetParamValue(ParamId::ReverbMix, IniGetFloat(L"DSPParams", L"ReverbMix", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::ReverbRoom);
    SetParamValue(ParamId::ReverbRoom, IniGetFloat(L"DSPParams", L"ReverbRoom", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::ReverbDamp);
    SetParamValue(ParamId::ReverbDamp, IniGetFloat(L"DSPParams", L"ReverbDamp", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::ReverbWidth);
    SetParamValue(ParamId::ReverbWidth, IniGetFloat(L"DSPParams", L"ReverbWidth", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::ReverbPreDelay);
    SetParamValue(ParamId::ReverbPreDelay, IniGetFloat(L"DSPParams", L"ReverbPreDelay", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::ReverbLowCut);
    SetParamValue(ParamId::ReverbLowCut, IniGetFloat(L"DSPParams", L"ReverbLowCut", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::ReverbHighCut);
    SetParamValue(ParamId::ReverbHighCut, IniGetFloat(L"DSPParams", L"ReverbHighCut", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::AdvReverbPreset);
    SetParamValue(ParamId::AdvReverbPreset, IniGetFloat(L"DSPParams", L"AdvReverbPreset", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::AdvReverbMix);
    SetParamValue(ParamId::AdvReverbMix, IniGetFloat(L"DSPParams", L"AdvReverbMix", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::AdvReverbDecay);
    SetParamValue(ParamId::AdvReverbDecay, IniGetFloat(L"DSPParams", L"AdvReverbDecay", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::AdvReverbHFRatio);
    SetParamValue(ParamId::AdvReverbHFRatio, IniGetFloat(L"DSPParams", L"AdvReverbHFRatio", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::AdvReverbDensity);
    SetParamValue(ParamId::AdvReverbDensity, IniGetFloat(L"DSPParams", L"AdvReverbDensity", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::AdvReverbDiffusion);
    SetParamValue(ParamId::AdvReverbDiffusion, IniGetFloat(L"DSPParams", L"AdvReverbDiffusion", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::AdvReverbReflections);
    SetParamValue(ParamId::AdvReverbReflections, IniGetFloat(L"DSPParams", L"AdvReverbReflections", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::AdvReverbLate);
    SetParamValue(ParamId::AdvReverbLate, IniGetFloat(L"DSPParams", L"AdvReverbLate", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::AdvReverbReflDelay);
    SetParamValue(ParamId::AdvReverbReflDelay, IniGetFloat(L"DSPParams", L"AdvReverbReflDelay", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::AdvReverbLateDelay);
    SetParamValue(ParamId::AdvReverbLateDelay, IniGetFloat(L"DSPParams", L"AdvReverbLateDelay", def->defaultValue, g_configPath.c_str()));

    def = GetParamDef(ParamId::EchoDelay);
    SetParamValue(ParamId::EchoDelay, IniGetFloat(L"DSPParams", L"EchoDelay", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::EchoFeedback);
    SetParamValue(ParamId::EchoFeedback, IniGetFloat(L"DSPParams", L"EchoFeedback", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::EchoMix);
    SetParamValue(ParamId::EchoMix, IniGetFloat(L"DSPParams", L"EchoMix", def->defaultValue, g_configPath.c_str()));

    def = GetParamDef(ParamId::EQPreamp);
    SetParamValue(ParamId::EQPreamp, IniGetFloat(L"DSPParams", L"EQPreamp", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::EQBass);
    SetParamValue(ParamId::EQBass, IniGetFloat(L"DSPParams", L"EQBass", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::EQMid);
    SetParamValue(ParamId::EQMid, IniGetFloat(L"DSPParams", L"EQMid", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::EQTreble);
    SetParamValue(ParamId::EQTreble, IniGetFloat(L"DSPParams", L"EQTreble", def->defaultValue, g_configPath.c_str()));

    def = GetParamDef(ParamId::CompThreshold);
    SetParamValue(ParamId::CompThreshold, IniGetFloat(L"DSPParams", L"CompThreshold", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::CompRatio);
    SetParamValue(ParamId::CompRatio, IniGetFloat(L"DSPParams", L"CompRatio", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::CompAttack);
    SetParamValue(ParamId::CompAttack, IniGetFloat(L"DSPParams", L"CompAttack", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::CompRelease);
    SetParamValue(ParamId::CompRelease, IniGetFloat(L"DSPParams", L"CompRelease", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::CompGain);
    SetParamValue(ParamId::CompGain, IniGetFloat(L"DSPParams", L"CompGain", def->defaultValue, g_configPath.c_str()));

    const struct {
        ParamId id;
        const wchar_t* key;
    } normParams[] = {{ParamId::NormTarget, L"NormTarget"},
                      {ParamId::NormLookahead, L"NormLookahead"},
                      {ParamId::NormMaxGain, L"NormMaxGain"},
                      {ParamId::NormRelease, L"NormRelease"}};
    for (const auto& p : normParams) {
        def = GetParamDef(p.id);
        SetParamValue(p.id, IniGetFloat(L"DSPParams", p.key, def->defaultValue, g_configPath.c_str()));
    }

    def = GetParamDef(ParamId::StereoWidth);
    SetParamValue(ParamId::StereoWidth, IniGetFloat(L"DSPParams", L"StereoWidth", def->defaultValue, g_configPath.c_str()));

    def = GetParamDef(ParamId::CenterCancel);
    SetParamValue(ParamId::CenterCancel, IniGetFloat(L"DSPParams", L"CenterCancel", def->defaultValue, g_configPath.c_str()));

    def = GetParamDef(ParamId::ConvolutionMix);
    SetParamValue(ParamId::ConvolutionMix, IniGetFloat(L"DSPParams", L"ConvolutionMix", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::ConvolutionGain);
    SetParamValue(ParamId::ConvolutionGain, IniGetFloat(L"DSPParams", L"ConvolutionGain", def->defaultValue, g_configPath.c_str()));

    def = GetParamDef(ParamId::SpatialBlend);
    SetParamValue(ParamId::SpatialBlend, IniGetFloat(L"DSPParams", L"SpatialBlend", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::SpatialWidth);
    SetParamValue(ParamId::SpatialWidth, IniGetFloat(L"DSPParams", L"SpatialWidth", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::SpatialRotation);
    SetParamValue(ParamId::SpatialRotation, IniGetFloat(L"DSPParams", L"SpatialRotation", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::SpatialMode);
    SetParamValue(ParamId::SpatialMode, IniGetFloat(L"DSPParams", L"SpatialMode", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::SpatialRearCenter);
    SetParamValue(ParamId::SpatialRearCenter, IniGetFloat(L"DSPParams", L"SpatialRearCenter", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::SpatialX);
    SetParamValue(ParamId::SpatialX, IniGetFloat(L"DSPParams", L"SpatialX", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::SpatialY);
    SetParamValue(ParamId::SpatialY, IniGetFloat(L"DSPParams", L"SpatialY", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::SpatialZ);
    SetParamValue(ParamId::SpatialZ, IniGetFloat(L"DSPParams", L"SpatialZ", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::SpatialSub);
    SetParamValue(ParamId::SpatialSub, IniGetFloat(L"DSPParams", L"SpatialSub", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::SpatialSubLevel);
    SetParamValue(ParamId::SpatialSubLevel, IniGetFloat(L"DSPParams", L"SpatialSubLevel", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::SpatialCrossover);
    SetParamValue(ParamId::SpatialCrossover, IniGetFloat(L"DSPParams", L"SpatialCrossover", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::SpatialBassFeel);
    SetParamValue(ParamId::SpatialBassFeel, IniGetFloat(L"DSPParams", L"SpatialBassFeel", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::SpatialBass);
    SetParamValue(ParamId::SpatialBass, IniGetFloat(L"DSPParams", L"SpatialBass", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::SpatialConeNoise);
    SetParamValue(ParamId::SpatialConeNoise, IniGetFloat(L"DSPParams", L"SpatialConeNoise", def->defaultValue, g_configPath.c_str()));

    // Load recent files
    g_recentFiles.clear();
    for (int i = 0; i < MAX_RECENT_FILES; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"File%d", i);
        wchar_t path[kMaxPathChars] = {0};
        IniGetString(L"RecentFiles", key, L"", path, kMaxPathChars, g_configPath.c_str());
        if (path[0] != L'\0') {
            g_recentFiles.push_back(path);
        }
    }
}

// Save settings to INI file
void SaveSettings() {
    wchar_t buf[32];

    // Save device name (empty for default device)
    IniWriteString(L"Playback", L"DeviceName", g_selectedDeviceName.c_str(), g_configPath.c_str());

    swprintf(buf, 32, L"%d", g_rewindOnPauseMs);
    IniWriteString(L"Playback", L"RewindOnPauseMs", buf, g_configPath.c_str());

    IniWriteString(L"Playback", L"AllowAmplify", g_allowAmplify ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Playback", L"RememberState", g_rememberState ? L"1" : L"0", g_configPath.c_str());

    swprintf(buf, 32, L"%d", g_rememberPosMinutes);
    IniWriteString(L"Playback", L"RememberPosMinutes", buf, g_configPath.c_str());

    IniWriteString(L"Playback", L"BringToFront", g_bringToFront ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Playback", L"MinimizeToTray", g_minimizeToTray ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Playback", L"LoadFolder", g_loadFolder ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Playback", L"RegisterFileTypes", g_registerFileTypes ? L"1" : L"0", g_configPath.c_str());

    swprintf(buf, 32, L"%d", static_cast<int>(g_volumeStep * 100 + 0.5f));
    IniWriteString(L"Playback", L"VolumeStep", buf, g_configPath.c_str());
    IniWriteString(L"Playback", L"ShowTitleInWindow", g_showTitleInWindow ? L"1" : L"0", g_configPath.c_str());

    swprintf(buf, 32, L"%d", g_replayGainMode);
    IniWriteString(L"Playback", L"ReplayGainMode", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", static_cast<int>(g_replayGainPreamp * 100 + (g_replayGainPreamp >= 0 ? 0.5f : -0.5f)));
    IniWriteString(L"Playback", L"ReplayGainPreamp", buf, g_configPath.c_str());
    IniWriteString(L"Playback", L"ReplayGainPreventClip", g_replayGainPreventClip ? L"1" : L"0", g_configPath.c_str());

    swprintf(buf, 32, L"%d", static_cast<int>(g_volume * 100 + 0.5f));
    IniWriteString(L"Playback", L"Volume", buf, g_configPath.c_str());

    // Save stream effect values (pitch, tempo, rate)
    swprintf(buf, 32, L"%.1f", g_pitch);
    IniWriteString(L"Playback", L"Pitch", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.1f", g_tempo);
    IniWriteString(L"Playback", L"Tempo", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", g_rate);
    IniWriteString(L"Playback", L"Rate", buf, g_configPath.c_str());

    // Save advanced settings (buffer)
    swprintf(buf, 32, L"%d", g_bufferSize);
    IniWriteString(L"Advanced", L"BufferSize", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_tempoAlgorithm);
    IniWriteString(L"Advanced", L"TempoAlgorithm", buf, g_configPath.c_str());
    IniWriteString(L"Advanced", L"DisableBatchDelay", g_disableBatchDelay ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Advanced", L"SmoothSeek", g_smoothSeek ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Advanced", L"LiveRewind", g_liveRewind ? L"1" : L"0", g_configPath.c_str());

    IniWriteString(L"Library", L"Count", std::to_wstring(g_libraryFolders.size()).c_str(), g_configPath.c_str());
    for (size_t i = 0; i < g_libraryFolders.size(); i++) {
        IniWriteString(L"Library", (L"Folder" + std::to_wstring(i)).c_str(), g_libraryFolders[i].path.c_str(),
                       g_configPath.c_str());
        IniWriteString(L"Library", (L"Tagged" + std::to_wstring(i)).c_str(), g_libraryFolders[i].tagged ? L"1" : L"0",
                       g_configPath.c_str());
    }
    IniWriteString(L"Advanced", L"LiveRewindMinutes", std::to_wstring(g_liveRewindMinutes).c_str(),
                   g_configPath.c_str());


    // Save Speedy settings
    IniWriteString(L"Speedy", L"NonlinearSpeedup", g_speedyNonlinear ? L"1" : L"0", g_configPath.c_str());

    // Save Signalsmith Stretch settings
    swprintf(buf, 32, L"%d", g_ssPreset);
    IniWriteString(L"Signalsmith", L"Preset", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_ssTonalityLimit);
    IniWriteString(L"Signalsmith", L"TonalityLimit", buf, g_configPath.c_str());

    // Save reverb algorithm
    swprintf(buf, 32, L"%d", g_reverbAlgorithm);
    IniWriteString(L"Effects", L"ReverbAlgorithm", buf, g_configPath.c_str());

    // Save MIDI settings
    IniWriteString(L"MIDI", L"SoundFont", g_midiSoundFont.c_str(), g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_midiMaxVoices);
    IniWriteString(L"MIDI", L"MaxVoices", buf, g_configPath.c_str());
    IniWriteString(L"MIDI", L"SincInterp", g_midiSincInterp ? L"1" : L"0", g_configPath.c_str());

    swprintf(buf, 32, L"%.1f", g_eqBassFreq);
    IniWriteString(L"Advanced", L"EQBassFreq", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.1f", g_eqMidFreq);
    IniWriteString(L"Advanced", L"EQMidFreq", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.1f", g_eqTrebleFreq);
    IniWriteString(L"Advanced", L"EQTrebleFreq", buf, g_configPath.c_str());

    // Save YouTube settings
	WriteYouTubeToolSettings(g_configPath, GetYouTubeToolSettings());
    IniWriteString(L"YouTube", L"ApiKey", g_ytApiKey.c_str(), g_configPath.c_str());
    IniWriteString(L"YouTube", L"FavoritesSort", g_ytFavoritesSort == 1 ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"YouTube", L"AutoRefresh", std::to_wstring(g_ytAutoRefresh).c_str(), g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadFolder", g_ytDownload.folder.c_str(), g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadType", std::to_wstring(g_ytDownload.type).c_str(), g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadAudioFormat", std::to_wstring(g_ytDownload.audioFormat).c_str(), g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadAudioQuality", std::to_wstring(g_ytDownload.audioQuality).c_str(), g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadVideoQuality", std::to_wstring(g_ytDownload.videoQuality).c_str(), g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadVideoContainer", std::to_wstring(g_ytDownload.videoContainer).c_str(), g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadVideoCodec", std::to_wstring(g_ytDownload.videoCodec).c_str(), g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadNaming", std::to_wstring(g_ytDownload.naming).c_str(), g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadAddMetadata", g_ytDownload.addMetadata ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadEmbedThumbnail", g_ytDownload.embedThumbnail ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadWriteThumbnail", g_ytDownload.writeThumbnail ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadWriteDescription", g_ytDownload.writeDescription ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadWriteSubtitles", g_ytDownload.writeSubtitles ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadEmbedSubtitles", g_ytDownload.embedSubtitles ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadChannelFolder", g_ytDownload.channelFolder ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"YouTube", L"DownloadExtraOptions", g_ytDownload.extraOptions.c_str(), g_configPath.c_str());

    // Save downloads settings
    IniWriteString(L"Downloads", L"Path", g_downloadPath.c_str(), g_configPath.c_str());
    IniWriteString(L"Downloads", L"OrganizeByFeed", g_downloadOrganizeByFeed ? L"1" : L"0", g_configPath.c_str());

    // Save recording settings
    IniWriteString(L"Recording", L"Path", g_recordPath.c_str(), g_configPath.c_str());
    IniWriteString(L"Recording", L"Template", g_recordTemplate.c_str(), g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_recordFormat);
    IniWriteString(L"Recording", L"Format", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_recordBitrate);
    IniWriteString(L"Recording", L"Bitrate", buf, g_configPath.c_str());
    IniWriteString(L"Recording", L"Effects", g_recordEffects ? L"1" : L"0", g_configPath.c_str());

    // Save speech settings
    IniWriteString(L"Speech", L"TrackChange", g_speechTrackChange ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Speech", L"Volume", g_speechVolume ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Speech", L"Effect", g_speechEffect ? L"1" : L"0", g_configPath.c_str());

    // Save shuffle and auto-advance settings
    IniWriteString(L"Playback", L"Shuffle", g_shuffle ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Playback", L"AutoAdvance", g_autoAdvance ? L"1" : L"0", g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_repeatMode);
    IniWriteString(L"Playback", L"RepeatMode", buf, g_configPath.c_str());
    IniWriteString(L"Playback", L"PlaylistFollow", g_playlistFollowPlayback ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Playback", L"CheckForUpdates", g_checkForUpdates ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Playback", L"AllowMultipleInstances", g_allowMultipleInstances ? L"1" : L"0", g_configPath.c_str());

    // Save seek settings
    IniWriteString(L"Movement", L"Seek1s", g_seekEnabled[0] ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Movement", L"Seek5s", g_seekEnabled[1] ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Movement", L"Seek10s", g_seekEnabled[2] ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Movement", L"Seek30s", g_seekEnabled[3] ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Movement", L"Seek1m", g_seekEnabled[4] ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Movement", L"Seek5m", g_seekEnabled[5] ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Movement", L"Seek10m", g_seekEnabled[6] ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Movement", L"Seek30m", g_seekEnabled[7] ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Movement", L"Seek1h", g_seekEnabled[8] ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Movement", L"Seek1t", g_seekEnabled[9] ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Movement", L"Seek5t", g_seekEnabled[10] ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Movement", L"Seek10t", g_seekEnabled[11] ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Movement", L"ChapterSeek", g_chapterSeekEnabled ? L"1" : L"0", g_configPath.c_str());

    swprintf(buf, 32, L"%d", g_currentSeekIndex);
    IniWriteString(L"Movement", L"CurrentSeek", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_seekMode);
    IniWriteString(L"Movement", L"SeekMode", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_springSpeed);
    IniWriteString(L"Movement", L"SpringSpeed", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_tapeSpeed);
    IniWriteString(L"Movement", L"TapeSpeed", buf, g_configPath.c_str());

    // Save effect settings
    IniWriteString(L"Effects", L"Volume", g_effectEnabled[0] ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Effects", L"Pitch", g_effectEnabled[1] ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Effects", L"Tempo", g_effectEnabled[2] ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"Effects", L"Rate", g_effectEnabled[3] ? L"1" : L"0", g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_currentEffectIndex);
    IniWriteString(L"Effects", L"CurrentEffect", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_rateStepMode);
    IniWriteString(L"Effects", L"RateStepMode", buf, g_configPath.c_str());

    // Save DSP effect settings
    IniWriteString(L"DSPEffects", L"Reverb", IsDSPEffectEnabled(DSPEffectType::Reverb) ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"DSPEffects", L"Echo", IsDSPEffectEnabled(DSPEffectType::Echo) ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"DSPEffects", L"EQ", IsDSPEffectEnabled(DSPEffectType::EQ) ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"DSPEffects", L"Compressor", IsDSPEffectEnabled(DSPEffectType::Compressor) ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"DSPEffects", L"StereoWidth", IsDSPEffectEnabled(DSPEffectType::StereoWidth) ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"DSPEffects", L"CenterCancel", IsDSPEffectEnabled(DSPEffectType::CenterCancel) ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"DSPEffects", L"Convolution", IsDSPEffectEnabled(DSPEffectType::Convolution) ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"DSPEffects", L"ConvolutionIR", g_convolutionIRPath.c_str(), g_configPath.c_str());
    IniWriteString(L"DSPEffects", L"SpatialAudio", IsDSPEffectEnabled(DSPEffectType::SpatialAudio) ? L"1" : L"0", g_configPath.c_str());
    IniWriteString(L"DSPEffects", L"Normalizer", IsDSPEffectEnabled(DSPEffectType::Normalizer) ? L"1" : L"0", g_configPath.c_str());
    for (const auto& p : {std::make_pair(ParamId::NormTarget, L"NormTarget"),
                          std::make_pair(ParamId::NormLookahead, L"NormLookahead"),
                          std::make_pair(ParamId::NormMaxGain, L"NormMaxGain"),
                          std::make_pair(ParamId::NormRelease, L"NormRelease")}) {
        swprintf(buf, 32, L"%.2f", GetParamValue(p.first));
        IniWriteString(L"DSPParams", p.second, buf, g_configPath.c_str());
    }

    // Save DSP effect parameter values
    // Reverb parameters
    swprintf(buf, 32, L"%.0f", GetParamValue(ParamId::ReverbPreset));
    IniWriteString(L"DSPParams", L"ReverbPreset", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::ReverbMix));
    IniWriteString(L"DSPParams", L"ReverbMix", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::ReverbRoom));
    IniWriteString(L"DSPParams", L"ReverbRoom", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::ReverbDamp));
    IniWriteString(L"DSPParams", L"ReverbDamp", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::ReverbWidth));
    IniWriteString(L"DSPParams", L"ReverbWidth", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::ReverbPreDelay));
    IniWriteString(L"DSPParams", L"ReverbPreDelay", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::ReverbLowCut));
    IniWriteString(L"DSPParams", L"ReverbLowCut", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::ReverbHighCut));
    IniWriteString(L"DSPParams", L"ReverbHighCut", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.0f", GetParamValue(ParamId::AdvReverbPreset));
    IniWriteString(L"DSPParams", L"AdvReverbPreset", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::AdvReverbMix));
    IniWriteString(L"DSPParams", L"AdvReverbMix", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::AdvReverbDecay));
    IniWriteString(L"DSPParams", L"AdvReverbDecay", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::AdvReverbHFRatio));
    IniWriteString(L"DSPParams", L"AdvReverbHFRatio", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::AdvReverbDensity));
    IniWriteString(L"DSPParams", L"AdvReverbDensity", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::AdvReverbDiffusion));
    IniWriteString(L"DSPParams", L"AdvReverbDiffusion", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::AdvReverbReflections));
    IniWriteString(L"DSPParams", L"AdvReverbReflections", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::AdvReverbLate));
    IniWriteString(L"DSPParams", L"AdvReverbLate", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::AdvReverbReflDelay));
    IniWriteString(L"DSPParams", L"AdvReverbReflDelay", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::AdvReverbLateDelay));
    IniWriteString(L"DSPParams", L"AdvReverbLateDelay", buf, g_configPath.c_str());

    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::EchoDelay));
    IniWriteString(L"DSPParams", L"EchoDelay", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::EchoFeedback));
    IniWriteString(L"DSPParams", L"EchoFeedback", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::EchoMix));
    IniWriteString(L"DSPParams", L"EchoMix", buf, g_configPath.c_str());

    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::EQPreamp));
    IniWriteString(L"DSPParams", L"EQPreamp", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::EQBass));
    IniWriteString(L"DSPParams", L"EQBass", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::EQMid));
    IniWriteString(L"DSPParams", L"EQMid", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::EQTreble));
    IniWriteString(L"DSPParams", L"EQTreble", buf, g_configPath.c_str());

    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::CompThreshold));
    IniWriteString(L"DSPParams", L"CompThreshold", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::CompRatio));
    IniWriteString(L"DSPParams", L"CompRatio", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::CompAttack));
    IniWriteString(L"DSPParams", L"CompAttack", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::CompRelease));
    IniWriteString(L"DSPParams", L"CompRelease", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::CompGain));
    IniWriteString(L"DSPParams", L"CompGain", buf, g_configPath.c_str());

    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::StereoWidth));
    IniWriteString(L"DSPParams", L"StereoWidth", buf, g_configPath.c_str());

    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::CenterCancel));
    IniWriteString(L"DSPParams", L"CenterCancel", buf, g_configPath.c_str());

    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::ConvolutionMix));
    IniWriteString(L"DSPParams", L"ConvolutionMix", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::ConvolutionGain));
    IniWriteString(L"DSPParams", L"ConvolutionGain", buf, g_configPath.c_str());

    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::SpatialBlend));
    IniWriteString(L"DSPParams", L"SpatialBlend", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::SpatialWidth));
    IniWriteString(L"DSPParams", L"SpatialWidth", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::SpatialRotation));
    IniWriteString(L"DSPParams", L"SpatialRotation", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::SpatialMode));
    IniWriteString(L"DSPParams", L"SpatialMode", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::SpatialRearCenter));
    IniWriteString(L"DSPParams", L"SpatialRearCenter", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::SpatialX));
    IniWriteString(L"DSPParams", L"SpatialX", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::SpatialY));
    IniWriteString(L"DSPParams", L"SpatialY", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::SpatialZ));
    IniWriteString(L"DSPParams", L"SpatialZ", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::SpatialSub));
    IniWriteString(L"DSPParams", L"SpatialSub", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::SpatialSubLevel));
    IniWriteString(L"DSPParams", L"SpatialSubLevel", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::SpatialCrossover));
    IniWriteString(L"DSPParams", L"SpatialCrossover", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::SpatialBassFeel));
    IniWriteString(L"DSPParams", L"SpatialBassFeel", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::SpatialBass));
    IniWriteString(L"DSPParams", L"SpatialBass", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::SpatialConeNoise));
    IniWriteString(L"DSPParams", L"SpatialConeNoise", buf, g_configPath.c_str());

    // Save recent files
    // First clear the section
    IniClearSection(L"RecentFiles", g_configPath.c_str());
    for (size_t i = 0; i < g_recentFiles.size() && i < MAX_RECENT_FILES; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"File%d", static_cast<int>(i));
        IniWriteString(L"RecentFiles", key, g_recentFiles[i].c_str(), g_configPath.c_str());
    }
}

// Save current playback state (file and position)
void SavePlaybackState() {
    // Clear old playlist entries
    IniClearSection(L"Playlist", g_configPath.c_str());

    if (!g_rememberState) {
        IniWriteString(L"State", L"LastFile", L"", g_configPath.c_str());
        IniWriteString(L"State", L"LastPosition", L"0", g_configPath.c_str());
        IniWriteString(L"State", L"TrackCount", L"0", g_configPath.c_str());
        IniWriteString(L"State", L"CurrentTrack", L"0", g_configPath.c_str());
        return;
    }

    // Save playlist
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", static_cast<int>(g_playlist.size()));
    IniWriteString(L"State", L"TrackCount", buf, g_configPath.c_str());

    for (size_t i = 0; i < g_playlist.size(); i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Track%zu", i);
        IniWriteString(L"Playlist", key, g_playlist[i].c_str(), g_configPath.c_str());
    }

    // Save current track index
    swprintf(buf, 32, L"%d", g_currentTrack);
    IniWriteString(L"State", L"CurrentTrack", buf, g_configPath.c_str());

    // Save current file (for backwards compatibility) and position
    if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
        IniWriteString(L"State", L"LastFile", g_playlist[g_currentTrack].c_str(), g_configPath.c_str());

        // Always save position with playback state (use GetCurrentPosition for tempo processor compatibility)
        double position = GetCurrentPosition();
        swprintf(buf, 32, L"%.2f", position);
        IniWriteString(L"State", L"LastPosition", buf, g_configPath.c_str());
    } else {
        IniWriteString(L"State", L"LastFile", L"", g_configPath.c_str());
        IniWriteString(L"State", L"LastPosition", L"0", g_configPath.c_str());
    }
}

// Load last playback state and resume if enabled
void LoadPlaybackState() {
    if (!g_rememberState) return;

    // Try to load full playlist first
    int trackCount = IniGetInt(L"State", L"TrackCount", 0, g_configPath.c_str());
    int currentTrack = IniGetInt(L"State", L"CurrentTrack", 0, g_configPath.c_str());

    g_playlist.clear();

    if (trackCount > 0) {
        // Load playlist from [Playlist] section
        for (int i = 0; i < trackCount; i++) {
            wchar_t key[32];
            swprintf(key, 32, L"Track%d", i);
            wchar_t filePath[2048] = {0};  // Larger buffer for URLs
            IniGetString(L"Playlist", key, L"", filePath, 2048, g_configPath.c_str());

            // Add to playlist if non-empty (trust save code - don't validate files/URLs here)
            if (filePath[0] != L'\0') {
                g_playlist.push_back(filePath);
            }
        }

        // Adjust current track if some files were missing
        if (!g_playlist.empty()) {
            if (currentTrack < 0 || currentTrack >= static_cast<int>(g_playlist.size())) {
                currentTrack = 0;
            }
            g_currentTrack = currentTrack;

            // LoadFile handles both files and URLs
            if (LoadFile(g_playlist[g_currentTrack].c_str())) {
                // Restore position for seekable streams only (not live streams)
                if (!g_isLiveStream) {
                    wchar_t posBuf[32] = {0};
                    IniGetString(L"State", L"LastPosition", L"0", posBuf, 32, g_configPath.c_str());
                    double position = std::wcstod(posBuf, nullptr);

                    if (position > 0) {
                        SeekToPosition(position);
                    }
                }
            }
            return;
        }
    }

    // Fall back to single file (backwards compatibility)
    wchar_t lastFile[2048] = {0};  // Larger buffer for URLs
    IniGetString(L"State", L"LastFile", L"", lastFile, 2048, g_configPath.c_str());

    // Trust save code - don't validate files/URLs here
    if (lastFile[0] != L'\0') {
        g_playlist.push_back(lastFile);
        g_currentTrack = 0;

        // LoadFile handles both files and URLs
        if (LoadFile(lastFile)) {
            // Restore position for seekable streams only (not live streams)
            if (!g_isLiveStream) {
                wchar_t posBuf[32] = {0};
                IniGetString(L"State", L"LastPosition", L"0", posBuf, 32, g_configPath.c_str());
                double position = std::wcstod(posBuf, nullptr);

                if (position > 0) {
                    SeekToPosition(position);
                }
            }
        }
    }
}

// Save position for a specific file (if it's long enough)
void SaveFilePosition(const std::wstring& filePath) {
    if (g_rememberPosMinutes == 0 || !audio::IsLoaded()) return;

    double length = audio::Length();

    // Only save if file is longer than threshold
    if (length < g_rememberPosMinutes * 60.0) return;

    double position = audio::Position();
    // Played to the end (or as good as): next time it starts from the beginning
    if (position >= length - 5.0) position = 0.0;

    SaveFilePositionDB(filePath, position);
}

// Load saved position for a specific file (returns 0 if none or file too short)
double LoadFilePosition(const std::wstring& filePath) {
    if (g_rememberPosMinutes == 0 || !audio::IsLoaded()) return 0.0;

    double length = audio::Length();

    // Only load if file is longer than threshold
    if (length < g_rememberPosMinutes * 60.0) return 0.0;

    double position = LoadFilePositionDB(filePath);
    if (position > 0 && position < length) {
        return position;
    }
    return 0.0;
}

// Get current seek amount in seconds
double GetCurrentSeekAmount() {
    if (g_currentSeekIndex >= 0 && g_currentSeekIndex < g_seekAmountCount) {
        return g_seekAmounts[g_currentSeekIndex].value;
    }
    return 5.0;  // Default fallback
}

// Check if a seek amount is currently available (track options need multiple tracks)
bool IsSeekAmountAvailable(int index) {
    // Index 12 is chapter seeking (virtual, not in g_seekAmounts array)
    if (index == 12) {
        return g_chapterSeekEnabled && !g_chapters.empty();
    }
    if (index < 0 || index >= g_seekAmountCount) return false;
    if (!g_seekEnabled[index]) return false;
    // Track-based options only available with multiple tracks
    if (g_seekAmounts[index].isTrack && g_playlist.size() <= 1) return false;
    return true;
}

// Total number of seek options including chapter (index 12)
constexpr int SEEK_AMOUNT_TOTAL = 13;

// Cycle through enabled seek amounts
void CycleSeekAmount(int direction) {
    // Count available amounts (track options need multiple tracks, chapter needs chapters)
    int availableCount = 0;
    for (int i = 0; i < SEEK_AMOUNT_TOTAL; i++) {
        if (IsSeekAmountAvailable(i)) availableCount++;
    }

    if (availableCount == 0) {
        // No seek amounts available, default to 5s
        g_currentSeekIndex = 1;
        Speak("5 seconds");
        return;
    }

    // If current selection is not available, find a valid one
    if (!IsSeekAmountAvailable(g_currentSeekIndex)) {
        for (int i = 0; i < SEEK_AMOUNT_TOTAL; i++) {
            if (IsSeekAmountAvailable(i)) {
                g_currentSeekIndex = i;
                break;
            }
        }
    }

    if (availableCount == 1) {
        // Only one available, just announce it
        if (g_currentSeekIndex == 12) {
            Speak("1 chapter");
        } else {
            Speak(g_seekAmounts[g_currentSeekIndex].label);
        }
        return;
    }

    // Find next/previous available amount (no wrapping)
    int newIndex = g_currentSeekIndex;
    for (int i = 0; i < SEEK_AMOUNT_TOTAL; i++) {
        newIndex += direction;

        // Stop at boundaries instead of wrapping
        if (newIndex >= SEEK_AMOUNT_TOTAL || newIndex < 0) {
            // Already at the edge, just announce current
            if (g_currentSeekIndex == 12) {
                Speak("1 chapter");
            } else {
                Speak(g_seekAmounts[g_currentSeekIndex].label);
            }
            return;
        }

        if (IsSeekAmountAvailable(newIndex)) {
            g_currentSeekIndex = newIndex;
            break;
        }
    }

    // Announce the new seek amount
    if (g_currentSeekIndex == 12) {
        Speak("1 chapter");
    } else {
        Speak(g_seekAmounts[g_currentSeekIndex].label);
    }
}

void SpeakSeekAmount() {
    if (g_currentSeekIndex == 12) {
        Speak("1 chapter");
    } else if (g_currentSeekIndex >= 0 && g_currentSeekIndex < g_seekAmountCount) {
        Speak(g_seekAmounts[g_currentSeekIndex].label);
    }
}

// Add a file to the recent files list
void AddToRecentFiles(const std::wstring& filePath) {
    if (filePath.empty()) return;

    // Don't add URLs to recent files
    if (filePath.find(L"://") != std::wstring::npos) return;

    // Remove if already in list (to move to top)
    for (auto it = g_recentFiles.begin(); it != g_recentFiles.end(); ++it) {
        if (WStrICmp(it->c_str(), filePath.c_str()) == 0) {
            g_recentFiles.erase(it);
            break;
        }
    }

    // Add to front
    g_recentFiles.insert(g_recentFiles.begin(), filePath);

    // Trim to max size
    if (g_recentFiles.size() > MAX_RECENT_FILES) {
        g_recentFiles.resize(MAX_RECENT_FILES);
    }
}

// Whether "allow multiple instances" is on. Read before settings are loaded, from
// FastPlay.ini beside the executable, to decide whether to hand files over.
bool ReadAllowMultipleInstances() {
    std::wstring configPath = GetExecutableDir() + L"FastPlay.ini";
    return IniGetInt(L"Playback", L"AllowMultipleInstances", 0, configPath.c_str()) != 0;
}
