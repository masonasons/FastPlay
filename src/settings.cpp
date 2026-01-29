#include "settings.h"
#include "globals.h"
#include "player.h"
#include "effects.h"
#include "convolution.h"
#include "database.h"
#include "accessibility.h"
#include "tempo_processor.h"
#include <cstdio>

// Initialize config file path
void InitConfigPath() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    g_configPath = exePath;
    size_t pos = g_configPath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        g_configPath = g_configPath.substr(0, pos + 1);
    }
    g_configPath += L"FastPlay.ini";
}

// Load settings from INI file
void LoadSettings() {
    InitConfigPath();

    // Load device name (empty means default device)
    wchar_t deviceName[256] = {0};
    GetPrivateProfileStringW(L"Playback", L"DeviceName", L"", deviceName, 256, g_configPath.c_str());
    g_selectedDeviceName = deviceName;
    g_selectedDevice = -1;  // Will be resolved by name in InitBass

    g_rewindOnPauseMs = GetPrivateProfileIntW(L"Playback", L"RewindOnPauseMs", 0, g_configPath.c_str());
    if (g_rewindOnPauseMs < 0) g_rewindOnPauseMs = 0;

    g_allowAmplify = GetPrivateProfileIntW(L"Playback", L"AllowAmplify", 0, g_configPath.c_str()) != 0;
    g_rememberState = GetPrivateProfileIntW(L"Playback", L"RememberState", 0, g_configPath.c_str()) != 0;
    g_rememberPosMinutes = GetPrivateProfileIntW(L"Playback", L"RememberPosMinutes", 0, g_configPath.c_str());
    g_bringToFront = GetPrivateProfileIntW(L"Playback", L"BringToFront", 1, g_configPath.c_str()) != 0;
    g_minimizeToTray = GetPrivateProfileIntW(L"Playback", L"MinimizeToTray", 1, g_configPath.c_str()) != 0;
    g_loadFolder = GetPrivateProfileIntW(L"Playback", L"LoadFolder", 0, g_configPath.c_str()) != 0;
    g_volumeStep = GetPrivateProfileIntW(L"Playback", L"VolumeStep", 2, g_configPath.c_str()) / 100.0f;
    if (g_volumeStep < 0.01f) g_volumeStep = 0.01f;
    if (g_volumeStep > 0.25f) g_volumeStep = 0.25f;
    g_showTitleInWindow = GetPrivateProfileIntW(L"Playback", L"ShowTitleInWindow", 1, g_configPath.c_str()) != 0;
    g_volume = GetPrivateProfileIntW(L"Playback", L"Volume", 100, g_configPath.c_str()) / 100.0f;

    // Clamp volume
    float maxVol = g_allowAmplify ? MAX_VOLUME_AMPLIFY : MAX_VOLUME_NORMAL;
    if (g_volume < 0.0f) g_volume = 0.0f;
    if (g_volume > maxVol) g_volume = maxVol;

    // Load stream effect values (pitch, tempo, rate)
    wchar_t buf[32] = {0};
    GetPrivateProfileStringW(L"Playback", L"Pitch", L"0", buf, 32, g_configPath.c_str());
    g_pitch = static_cast<float>(_wtof(buf));
    if (g_pitch < -12.0f) g_pitch = -12.0f;
    if (g_pitch > 12.0f) g_pitch = 12.0f;

    GetPrivateProfileStringW(L"Playback", L"Tempo", L"0", buf, 32, g_configPath.c_str());
    g_tempo = static_cast<float>(_wtof(buf));
    if (g_tempo < -50.0f) g_tempo = -50.0f;
    if (g_tempo > 100.0f) g_tempo = 100.0f;

    GetPrivateProfileStringW(L"Playback", L"Rate", L"1.0", buf, 32, g_configPath.c_str());
    g_rate = static_cast<float>(_wtof(buf));
    if (g_rate < 0.5f) g_rate = 0.5f;
    if (g_rate > 2.0f) g_rate = 2.0f;

    // Load advanced settings (buffer)
    g_bufferSize = GetPrivateProfileIntW(L"Advanced", L"BufferSize", 500, g_configPath.c_str());
    if (g_bufferSize < 100) g_bufferSize = 100;
    if (g_bufferSize > 5000) g_bufferSize = 5000;

    g_updatePeriod = GetPrivateProfileIntW(L"Advanced", L"UpdatePeriod", 100, g_configPath.c_str());
    if (g_updatePeriod < 5) g_updatePeriod = 5;
    if (g_updatePeriod > 500) g_updatePeriod = 500;

    g_tempoAlgorithm = GetPrivateProfileIntW(L"Advanced", L"TempoAlgorithm", 0, g_configPath.c_str());
    if (g_tempoAlgorithm < 0) g_tempoAlgorithm = 0;
    if (g_tempoAlgorithm > 3) g_tempoAlgorithm = 3;

    g_legacyVolume = GetPrivateProfileIntW(L"Advanced", L"LegacyVolume", 0, g_configPath.c_str()) != 0;

    // Load SoundTouch settings
    g_stAntiAliasFilter = GetPrivateProfileIntW(L"SoundTouch", L"AntiAliasFilter", 1, g_configPath.c_str()) != 0;
    g_stAAFilterLength = GetPrivateProfileIntW(L"SoundTouch", L"AAFilterLength", 32, g_configPath.c_str());
    if (g_stAAFilterLength < 8) g_stAAFilterLength = 8;
    if (g_stAAFilterLength > 128) g_stAAFilterLength = 128;
    g_stQuickAlgorithm = GetPrivateProfileIntW(L"SoundTouch", L"QuickAlgorithm", 0, g_configPath.c_str()) != 0;
    g_stSequenceMs = GetPrivateProfileIntW(L"SoundTouch", L"SequenceMs", 82, g_configPath.c_str());
    if (g_stSequenceMs < 0) g_stSequenceMs = 0;
    if (g_stSequenceMs > 200) g_stSequenceMs = 200;
    g_stSeekWindowMs = GetPrivateProfileIntW(L"SoundTouch", L"SeekWindowMs", 28, g_configPath.c_str());
    if (g_stSeekWindowMs < 0) g_stSeekWindowMs = 0;
    if (g_stSeekWindowMs > 100) g_stSeekWindowMs = 100;
    g_stOverlapMs = GetPrivateProfileIntW(L"SoundTouch", L"OverlapMs", 8, g_configPath.c_str());
    if (g_stOverlapMs < 0) g_stOverlapMs = 0;
    if (g_stOverlapMs > 50) g_stOverlapMs = 50;
    g_stPreventClick = GetPrivateProfileIntW(L"SoundTouch", L"PreventClick", 0, g_configPath.c_str()) != 0;
    g_stAlgorithm = GetPrivateProfileIntW(L"SoundTouch", L"Algorithm", 1, g_configPath.c_str());
    if (g_stAlgorithm < 0) g_stAlgorithm = 0;
    if (g_stAlgorithm > 2) g_stAlgorithm = 2;

    // Load Rubber Band settings
    g_rbFormantPreserved = GetPrivateProfileIntW(L"RubberBand", L"FormantPreserved", 0, g_configPath.c_str()) != 0;
    g_rbPitchMode = GetPrivateProfileIntW(L"RubberBand", L"PitchMode", 2, g_configPath.c_str());
    if (g_rbPitchMode < 0) g_rbPitchMode = 0;
    if (g_rbPitchMode > 2) g_rbPitchMode = 2;
    g_rbWindowSize = GetPrivateProfileIntW(L"RubberBand", L"WindowSize", 0, g_configPath.c_str());
    if (g_rbWindowSize < 0) g_rbWindowSize = 0;
    if (g_rbWindowSize > 2) g_rbWindowSize = 2;
    g_rbTransients = GetPrivateProfileIntW(L"RubberBand", L"Transients", 0, g_configPath.c_str());
    if (g_rbTransients < 0) g_rbTransients = 0;
    if (g_rbTransients > 2) g_rbTransients = 2;
    g_rbDetector = GetPrivateProfileIntW(L"RubberBand", L"Detector", 0, g_configPath.c_str());
    if (g_rbDetector < 0) g_rbDetector = 0;
    if (g_rbDetector > 2) g_rbDetector = 2;
    g_rbChannels = GetPrivateProfileIntW(L"RubberBand", L"Channels", 0, g_configPath.c_str());
    if (g_rbChannels < 0) g_rbChannels = 0;
    if (g_rbChannels > 1) g_rbChannels = 1;
    g_rbPhase = GetPrivateProfileIntW(L"RubberBand", L"Phase", 0, g_configPath.c_str());
    if (g_rbPhase < 0) g_rbPhase = 0;
    if (g_rbPhase > 1) g_rbPhase = 1;
    g_rbSmoothing = GetPrivateProfileIntW(L"RubberBand", L"Smoothing", 0, g_configPath.c_str()) != 0;

    // Load Speedy settings
    g_speedyNonlinear = GetPrivateProfileIntW(L"Speedy", L"NonlinearSpeedup", 1, g_configPath.c_str()) != 0;

    // Load reverb algorithm (0=Off, 1=Freeverb, 2=DX8, 3=I3DL2)
    g_reverbAlgorithm = GetPrivateProfileIntW(L"Effects", L"ReverbAlgorithm", 0, g_configPath.c_str());
    if (g_reverbAlgorithm < 0) g_reverbAlgorithm = 0;
    if (g_reverbAlgorithm > 3) g_reverbAlgorithm = 3;

    // Load MIDI settings
    wchar_t midiBuf[MAX_PATH] = {0};
    GetPrivateProfileStringW(L"MIDI", L"SoundFont", L"", midiBuf, MAX_PATH, g_configPath.c_str());
    g_midiSoundFont = midiBuf;
    g_midiMaxVoices = GetPrivateProfileIntW(L"MIDI", L"MaxVoices", 128, g_configPath.c_str());
    if (g_midiMaxVoices < 1) g_midiMaxVoices = 1;
    if (g_midiMaxVoices > 1000) g_midiMaxVoices = 1000;
    g_midiSincInterp = GetPrivateProfileIntW(L"MIDI", L"SincInterp", 0, g_configPath.c_str()) != 0;

    // EQ frequencies loaded using string conversion (GetPrivateProfileFloatW defined later)
    wchar_t eqBuf[32];
    GetPrivateProfileStringW(L"Advanced", L"EQBassFreq", L"50", eqBuf, 32, g_configPath.c_str());
    g_eqBassFreq = static_cast<float>(_wtof(eqBuf));
    GetPrivateProfileStringW(L"Advanced", L"EQMidFreq", L"1000", eqBuf, 32, g_configPath.c_str());
    g_eqMidFreq = static_cast<float>(_wtof(eqBuf));
    GetPrivateProfileStringW(L"Advanced", L"EQTrebleFreq", L"12000", eqBuf, 32, g_configPath.c_str());
    g_eqTrebleFreq = static_cast<float>(_wtof(eqBuf));

    // Load YouTube settings
    wchar_t ytBuf[512] = {0};
    GetPrivateProfileStringW(L"YouTube", L"YtdlpPath", L"", ytBuf, 512, g_configPath.c_str());
    g_ytdlpPath = ytBuf;
    GetPrivateProfileStringW(L"YouTube", L"ApiKey", L"", ytBuf, 512, g_configPath.c_str());
    g_ytApiKey = ytBuf;

    // Load downloads settings
    wchar_t dlBuf[512] = {0};
    GetPrivateProfileStringW(L"Downloads", L"Path", L"", dlBuf, 512, g_configPath.c_str());
    g_downloadPath = dlBuf;
    g_downloadOrganizeByFeed = GetPrivateProfileIntW(L"Downloads", L"OrganizeByFeed", 0, g_configPath.c_str()) != 0;

    // Load recording settings
    wchar_t recBuf[512] = {0};
    GetPrivateProfileStringW(L"Recording", L"Path", L"", recBuf, 512, g_configPath.c_str());
    g_recordPath = recBuf;
    GetPrivateProfileStringW(L"Recording", L"Template", L"%Y-%m-%d_%H-%M-%S", recBuf, 512, g_configPath.c_str());
    g_recordTemplate = recBuf;
    g_recordFormat = GetPrivateProfileIntW(L"Recording", L"Format", 0, g_configPath.c_str());
    if (g_recordFormat < 0) g_recordFormat = 0;
    if (g_recordFormat > 3) g_recordFormat = 3;
    g_recordBitrate = GetPrivateProfileIntW(L"Recording", L"Bitrate", 192, g_configPath.c_str());

    // Load speech settings
    g_speechTrackChange = GetPrivateProfileIntW(L"Speech", L"TrackChange", 0, g_configPath.c_str()) != 0;
    g_speechVolume = GetPrivateProfileIntW(L"Speech", L"Volume", 1, g_configPath.c_str()) != 0;
    g_speechEffect = GetPrivateProfileIntW(L"Speech", L"Effect", 1, g_configPath.c_str()) != 0;

    // Load shuffle and auto-advance settings
    g_shuffle = GetPrivateProfileIntW(L"Playback", L"Shuffle", 0, g_configPath.c_str()) != 0;
    g_autoAdvance = GetPrivateProfileIntW(L"Playback", L"AutoAdvance", 1, g_configPath.c_str()) != 0;

    // Load seek settings
    g_seekEnabled[0] = GetPrivateProfileIntW(L"Movement", L"Seek1s", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[1] = GetPrivateProfileIntW(L"Movement", L"Seek5s", 1, g_configPath.c_str()) != 0;
    g_seekEnabled[2] = GetPrivateProfileIntW(L"Movement", L"Seek10s", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[3] = GetPrivateProfileIntW(L"Movement", L"Seek30s", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[4] = GetPrivateProfileIntW(L"Movement", L"Seek1m", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[5] = GetPrivateProfileIntW(L"Movement", L"Seek5m", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[6] = GetPrivateProfileIntW(L"Movement", L"Seek10m", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[7] = GetPrivateProfileIntW(L"Movement", L"Seek30m", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[8] = GetPrivateProfileIntW(L"Movement", L"Seek1h", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[9] = GetPrivateProfileIntW(L"Movement", L"Seek1t", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[10] = GetPrivateProfileIntW(L"Movement", L"Seek5t", 0, g_configPath.c_str()) != 0;
    g_seekEnabled[11] = GetPrivateProfileIntW(L"Movement", L"Seek10t", 0, g_configPath.c_str()) != 0;
    g_chapterSeekEnabled = GetPrivateProfileIntW(L"Movement", L"ChapterSeek", 1, g_configPath.c_str()) != 0;
    g_currentSeekIndex = GetPrivateProfileIntW(L"Movement", L"CurrentSeek", 1, g_configPath.c_str());

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
    g_effectEnabled[0] = GetPrivateProfileIntW(L"Effects", L"Volume", 1, g_configPath.c_str()) != 0;  // Volume enabled by default
    g_effectEnabled[1] = GetPrivateProfileIntW(L"Effects", L"Pitch", 0, g_configPath.c_str()) != 0;
    g_effectEnabled[2] = GetPrivateProfileIntW(L"Effects", L"Tempo", 0, g_configPath.c_str()) != 0;
    g_effectEnabled[3] = GetPrivateProfileIntW(L"Effects", L"Rate", 0, g_configPath.c_str()) != 0;
    g_currentEffectIndex = GetPrivateProfileIntW(L"Effects", L"CurrentEffect", 0, g_configPath.c_str());
    g_rateStepMode = GetPrivateProfileIntW(L"Effects", L"RateStepMode", 0, g_configPath.c_str());
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
static float GetPrivateProfileFloatW(const wchar_t* section, const wchar_t* key, float defaultVal, const wchar_t* path) {
    wchar_t buf[32] = {0};
    GetPrivateProfileStringW(section, key, L"", buf, 32, path);
    if (buf[0] == L'\0') return defaultVal;
    return static_cast<float>(_wtof(buf));
}

// Load DSP effect settings (call after InitEffects)
void LoadDSPSettings() {
    // Load DSP effect enabled states
    EnableDSPEffect(DSPEffectType::Reverb, GetPrivateProfileIntW(L"DSPEffects", L"Reverb", 0, g_configPath.c_str()) != 0);
    EnableDSPEffect(DSPEffectType::Echo, GetPrivateProfileIntW(L"DSPEffects", L"Echo", 0, g_configPath.c_str()) != 0);
    EnableDSPEffect(DSPEffectType::EQ, GetPrivateProfileIntW(L"DSPEffects", L"EQ", 0, g_configPath.c_str()) != 0);
    EnableDSPEffect(DSPEffectType::Compressor, GetPrivateProfileIntW(L"DSPEffects", L"Compressor", 0, g_configPath.c_str()) != 0);
    EnableDSPEffect(DSPEffectType::StereoWidth, GetPrivateProfileIntW(L"DSPEffects", L"StereoWidth", 0, g_configPath.c_str()) != 0);
    EnableDSPEffect(DSPEffectType::CenterCancel, GetPrivateProfileIntW(L"DSPEffects", L"CenterCancel", 0, g_configPath.c_str()) != 0);
    EnableDSPEffect(DSPEffectType::Convolution, GetPrivateProfileIntW(L"DSPEffects", L"Convolution", 0, g_configPath.c_str()) != 0);

    // Load convolution IR path
    {
        wchar_t irPath[MAX_PATH] = {0};
        GetPrivateProfileStringW(L"DSPEffects", L"ConvolutionIR", L"", irPath, MAX_PATH, g_configPath.c_str());
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

    def = GetParamDef(ParamId::ReverbMix);
    SetParamValue(ParamId::ReverbMix, GetPrivateProfileFloatW(L"DSPParams", L"ReverbMix", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::ReverbRoom);
    SetParamValue(ParamId::ReverbRoom, GetPrivateProfileFloatW(L"DSPParams", L"ReverbRoom", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::ReverbDamp);
    SetParamValue(ParamId::ReverbDamp, GetPrivateProfileFloatW(L"DSPParams", L"ReverbDamp", def->defaultValue, g_configPath.c_str()));

    // DX8 Reverb parameters
    def = GetParamDef(ParamId::DX8ReverbTime);
    SetParamValue(ParamId::DX8ReverbTime, GetPrivateProfileFloatW(L"DSPParams", L"DX8ReverbTime", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::DX8ReverbHFRatio);
    SetParamValue(ParamId::DX8ReverbHFRatio, GetPrivateProfileFloatW(L"DSPParams", L"DX8ReverbHFRatio", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::DX8ReverbMix);
    SetParamValue(ParamId::DX8ReverbMix, GetPrivateProfileFloatW(L"DSPParams", L"DX8ReverbMix", def->defaultValue, g_configPath.c_str()));

    // I3DL2 Reverb parameters
    def = GetParamDef(ParamId::I3DL2Room);
    SetParamValue(ParamId::I3DL2Room, GetPrivateProfileFloatW(L"DSPParams", L"I3DL2Room", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::I3DL2DecayTime);
    SetParamValue(ParamId::I3DL2DecayTime, GetPrivateProfileFloatW(L"DSPParams", L"I3DL2DecayTime", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::I3DL2Diffusion);
    SetParamValue(ParamId::I3DL2Diffusion, GetPrivateProfileFloatW(L"DSPParams", L"I3DL2Diffusion", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::I3DL2Density);
    SetParamValue(ParamId::I3DL2Density, GetPrivateProfileFloatW(L"DSPParams", L"I3DL2Density", def->defaultValue, g_configPath.c_str()));

    def = GetParamDef(ParamId::EchoDelay);
    SetParamValue(ParamId::EchoDelay, GetPrivateProfileFloatW(L"DSPParams", L"EchoDelay", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::EchoFeedback);
    SetParamValue(ParamId::EchoFeedback, GetPrivateProfileFloatW(L"DSPParams", L"EchoFeedback", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::EchoMix);
    SetParamValue(ParamId::EchoMix, GetPrivateProfileFloatW(L"DSPParams", L"EchoMix", def->defaultValue, g_configPath.c_str()));

    def = GetParamDef(ParamId::EQPreamp);
    SetParamValue(ParamId::EQPreamp, GetPrivateProfileFloatW(L"DSPParams", L"EQPreamp", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::EQBass);
    SetParamValue(ParamId::EQBass, GetPrivateProfileFloatW(L"DSPParams", L"EQBass", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::EQMid);
    SetParamValue(ParamId::EQMid, GetPrivateProfileFloatW(L"DSPParams", L"EQMid", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::EQTreble);
    SetParamValue(ParamId::EQTreble, GetPrivateProfileFloatW(L"DSPParams", L"EQTreble", def->defaultValue, g_configPath.c_str()));

    def = GetParamDef(ParamId::CompThreshold);
    SetParamValue(ParamId::CompThreshold, GetPrivateProfileFloatW(L"DSPParams", L"CompThreshold", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::CompRatio);
    SetParamValue(ParamId::CompRatio, GetPrivateProfileFloatW(L"DSPParams", L"CompRatio", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::CompAttack);
    SetParamValue(ParamId::CompAttack, GetPrivateProfileFloatW(L"DSPParams", L"CompAttack", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::CompRelease);
    SetParamValue(ParamId::CompRelease, GetPrivateProfileFloatW(L"DSPParams", L"CompRelease", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::CompGain);
    SetParamValue(ParamId::CompGain, GetPrivateProfileFloatW(L"DSPParams", L"CompGain", def->defaultValue, g_configPath.c_str()));

    def = GetParamDef(ParamId::StereoWidth);
    SetParamValue(ParamId::StereoWidth, GetPrivateProfileFloatW(L"DSPParams", L"StereoWidth", def->defaultValue, g_configPath.c_str()));

    def = GetParamDef(ParamId::CenterCancel);
    SetParamValue(ParamId::CenterCancel, GetPrivateProfileFloatW(L"DSPParams", L"CenterCancel", def->defaultValue, g_configPath.c_str()));

    def = GetParamDef(ParamId::ConvolutionMix);
    SetParamValue(ParamId::ConvolutionMix, GetPrivateProfileFloatW(L"DSPParams", L"ConvolutionMix", def->defaultValue, g_configPath.c_str()));
    def = GetParamDef(ParamId::ConvolutionGain);
    SetParamValue(ParamId::ConvolutionGain, GetPrivateProfileFloatW(L"DSPParams", L"ConvolutionGain", def->defaultValue, g_configPath.c_str()));
}

// Save settings to INI file
void SaveSettings() {
    wchar_t buf[32];

    // Save device name (empty for default device)
    WritePrivateProfileStringW(L"Playback", L"DeviceName", g_selectedDeviceName.c_str(), g_configPath.c_str());

    swprintf(buf, 32, L"%d", g_rewindOnPauseMs);
    WritePrivateProfileStringW(L"Playback", L"RewindOnPauseMs", buf, g_configPath.c_str());

    WritePrivateProfileStringW(L"Playback", L"AllowAmplify", g_allowAmplify ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Playback", L"RememberState", g_rememberState ? L"1" : L"0", g_configPath.c_str());

    swprintf(buf, 32, L"%d", g_rememberPosMinutes);
    WritePrivateProfileStringW(L"Playback", L"RememberPosMinutes", buf, g_configPath.c_str());

    WritePrivateProfileStringW(L"Playback", L"BringToFront", g_bringToFront ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Playback", L"MinimizeToTray", g_minimizeToTray ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Playback", L"LoadFolder", g_loadFolder ? L"1" : L"0", g_configPath.c_str());

    swprintf(buf, 32, L"%d", static_cast<int>(g_volumeStep * 100 + 0.5f));
    WritePrivateProfileStringW(L"Playback", L"VolumeStep", buf, g_configPath.c_str());
    WritePrivateProfileStringW(L"Playback", L"ShowTitleInWindow", g_showTitleInWindow ? L"1" : L"0", g_configPath.c_str());

    swprintf(buf, 32, L"%d", static_cast<int>(g_volume * 100 + 0.5f));
    WritePrivateProfileStringW(L"Playback", L"Volume", buf, g_configPath.c_str());

    // Save stream effect values (pitch, tempo, rate)
    swprintf(buf, 32, L"%.1f", g_pitch);
    WritePrivateProfileStringW(L"Playback", L"Pitch", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.1f", g_tempo);
    WritePrivateProfileStringW(L"Playback", L"Tempo", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", g_rate);
    WritePrivateProfileStringW(L"Playback", L"Rate", buf, g_configPath.c_str());

    // Save advanced settings (buffer)
    swprintf(buf, 32, L"%d", g_bufferSize);
    WritePrivateProfileStringW(L"Advanced", L"BufferSize", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_updatePeriod);
    WritePrivateProfileStringW(L"Advanced", L"UpdatePeriod", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_tempoAlgorithm);
    WritePrivateProfileStringW(L"Advanced", L"TempoAlgorithm", buf, g_configPath.c_str());
    WritePrivateProfileStringW(L"Advanced", L"LegacyVolume", g_legacyVolume ? L"1" : L"0", g_configPath.c_str());

    // Save SoundTouch settings
    WritePrivateProfileStringW(L"SoundTouch", L"AntiAliasFilter", g_stAntiAliasFilter ? L"1" : L"0", g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_stAAFilterLength);
    WritePrivateProfileStringW(L"SoundTouch", L"AAFilterLength", buf, g_configPath.c_str());
    WritePrivateProfileStringW(L"SoundTouch", L"QuickAlgorithm", g_stQuickAlgorithm ? L"1" : L"0", g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_stSequenceMs);
    WritePrivateProfileStringW(L"SoundTouch", L"SequenceMs", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_stSeekWindowMs);
    WritePrivateProfileStringW(L"SoundTouch", L"SeekWindowMs", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_stOverlapMs);
    WritePrivateProfileStringW(L"SoundTouch", L"OverlapMs", buf, g_configPath.c_str());
    WritePrivateProfileStringW(L"SoundTouch", L"PreventClick", g_stPreventClick ? L"1" : L"0", g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_stAlgorithm);
    WritePrivateProfileStringW(L"SoundTouch", L"Algorithm", buf, g_configPath.c_str());

    // Save Rubber Band settings
    WritePrivateProfileStringW(L"RubberBand", L"FormantPreserved", g_rbFormantPreserved ? L"1" : L"0", g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_rbPitchMode);
    WritePrivateProfileStringW(L"RubberBand", L"PitchMode", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_rbWindowSize);
    WritePrivateProfileStringW(L"RubberBand", L"WindowSize", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_rbTransients);
    WritePrivateProfileStringW(L"RubberBand", L"Transients", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_rbDetector);
    WritePrivateProfileStringW(L"RubberBand", L"Detector", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_rbChannels);
    WritePrivateProfileStringW(L"RubberBand", L"Channels", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_rbPhase);
    WritePrivateProfileStringW(L"RubberBand", L"Phase", buf, g_configPath.c_str());
    WritePrivateProfileStringW(L"RubberBand", L"Smoothing", g_rbSmoothing ? L"1" : L"0", g_configPath.c_str());

    // Save Speedy settings
    WritePrivateProfileStringW(L"Speedy", L"NonlinearSpeedup", g_speedyNonlinear ? L"1" : L"0", g_configPath.c_str());

    // Save reverb algorithm
    swprintf(buf, 32, L"%d", g_reverbAlgorithm);
    WritePrivateProfileStringW(L"Effects", L"ReverbAlgorithm", buf, g_configPath.c_str());

    // Save MIDI settings
    WritePrivateProfileStringW(L"MIDI", L"SoundFont", g_midiSoundFont.c_str(), g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_midiMaxVoices);
    WritePrivateProfileStringW(L"MIDI", L"MaxVoices", buf, g_configPath.c_str());
    WritePrivateProfileStringW(L"MIDI", L"SincInterp", g_midiSincInterp ? L"1" : L"0", g_configPath.c_str());

    swprintf(buf, 32, L"%.1f", g_eqBassFreq);
    WritePrivateProfileStringW(L"Advanced", L"EQBassFreq", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.1f", g_eqMidFreq);
    WritePrivateProfileStringW(L"Advanced", L"EQMidFreq", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.1f", g_eqTrebleFreq);
    WritePrivateProfileStringW(L"Advanced", L"EQTrebleFreq", buf, g_configPath.c_str());

    // Save YouTube settings
    WritePrivateProfileStringW(L"YouTube", L"YtdlpPath", g_ytdlpPath.c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"YouTube", L"ApiKey", g_ytApiKey.c_str(), g_configPath.c_str());

    // Save downloads settings
    WritePrivateProfileStringW(L"Downloads", L"Path", g_downloadPath.c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"Downloads", L"OrganizeByFeed", g_downloadOrganizeByFeed ? L"1" : L"0", g_configPath.c_str());

    // Save recording settings
    WritePrivateProfileStringW(L"Recording", L"Path", g_recordPath.c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"Recording", L"Template", g_recordTemplate.c_str(), g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_recordFormat);
    WritePrivateProfileStringW(L"Recording", L"Format", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_recordBitrate);
    WritePrivateProfileStringW(L"Recording", L"Bitrate", buf, g_configPath.c_str());

    // Save speech settings
    WritePrivateProfileStringW(L"Speech", L"TrackChange", g_speechTrackChange ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Speech", L"Volume", g_speechVolume ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Speech", L"Effect", g_speechEffect ? L"1" : L"0", g_configPath.c_str());

    // Save shuffle and auto-advance settings
    WritePrivateProfileStringW(L"Playback", L"Shuffle", g_shuffle ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Playback", L"AutoAdvance", g_autoAdvance ? L"1" : L"0", g_configPath.c_str());

    // Save seek settings
    WritePrivateProfileStringW(L"Movement", L"Seek1s", g_seekEnabled[0] ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Movement", L"Seek5s", g_seekEnabled[1] ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Movement", L"Seek10s", g_seekEnabled[2] ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Movement", L"Seek30s", g_seekEnabled[3] ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Movement", L"Seek1m", g_seekEnabled[4] ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Movement", L"Seek5m", g_seekEnabled[5] ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Movement", L"Seek10m", g_seekEnabled[6] ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Movement", L"Seek30m", g_seekEnabled[7] ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Movement", L"Seek1h", g_seekEnabled[8] ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Movement", L"Seek1t", g_seekEnabled[9] ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Movement", L"Seek5t", g_seekEnabled[10] ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Movement", L"Seek10t", g_seekEnabled[11] ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Movement", L"ChapterSeek", g_chapterSeekEnabled ? L"1" : L"0", g_configPath.c_str());

    swprintf(buf, 32, L"%d", g_currentSeekIndex);
    WritePrivateProfileStringW(L"Movement", L"CurrentSeek", buf, g_configPath.c_str());

    // Save effect settings
    WritePrivateProfileStringW(L"Effects", L"Volume", g_effectEnabled[0] ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Effects", L"Pitch", g_effectEnabled[1] ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Effects", L"Tempo", g_effectEnabled[2] ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"Effects", L"Rate", g_effectEnabled[3] ? L"1" : L"0", g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_currentEffectIndex);
    WritePrivateProfileStringW(L"Effects", L"CurrentEffect", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%d", g_rateStepMode);
    WritePrivateProfileStringW(L"Effects", L"RateStepMode", buf, g_configPath.c_str());

    // Save DSP effect settings
    WritePrivateProfileStringW(L"DSPEffects", L"Reverb", IsDSPEffectEnabled(DSPEffectType::Reverb) ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"DSPEffects", L"Echo", IsDSPEffectEnabled(DSPEffectType::Echo) ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"DSPEffects", L"EQ", IsDSPEffectEnabled(DSPEffectType::EQ) ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"DSPEffects", L"Compressor", IsDSPEffectEnabled(DSPEffectType::Compressor) ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"DSPEffects", L"StereoWidth", IsDSPEffectEnabled(DSPEffectType::StereoWidth) ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"DSPEffects", L"CenterCancel", IsDSPEffectEnabled(DSPEffectType::CenterCancel) ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"DSPEffects", L"Convolution", IsDSPEffectEnabled(DSPEffectType::Convolution) ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"DSPEffects", L"ConvolutionIR", g_convolutionIRPath.c_str(), g_configPath.c_str());

    // Save DSP effect parameter values
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::ReverbMix));
    WritePrivateProfileStringW(L"DSPParams", L"ReverbMix", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::ReverbRoom));
    WritePrivateProfileStringW(L"DSPParams", L"ReverbRoom", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::ReverbDamp));
    WritePrivateProfileStringW(L"DSPParams", L"ReverbDamp", buf, g_configPath.c_str());

    // DX8 Reverb parameters
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::DX8ReverbTime));
    WritePrivateProfileStringW(L"DSPParams", L"DX8ReverbTime", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.3f", GetParamValue(ParamId::DX8ReverbHFRatio));
    WritePrivateProfileStringW(L"DSPParams", L"DX8ReverbHFRatio", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::DX8ReverbMix));
    WritePrivateProfileStringW(L"DSPParams", L"DX8ReverbMix", buf, g_configPath.c_str());

    // I3DL2 Reverb parameters
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::I3DL2Room));
    WritePrivateProfileStringW(L"DSPParams", L"I3DL2Room", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::I3DL2DecayTime));
    WritePrivateProfileStringW(L"DSPParams", L"I3DL2DecayTime", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::I3DL2Diffusion));
    WritePrivateProfileStringW(L"DSPParams", L"I3DL2Diffusion", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::I3DL2Density));
    WritePrivateProfileStringW(L"DSPParams", L"I3DL2Density", buf, g_configPath.c_str());

    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::EchoDelay));
    WritePrivateProfileStringW(L"DSPParams", L"EchoDelay", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::EchoFeedback));
    WritePrivateProfileStringW(L"DSPParams", L"EchoFeedback", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::EchoMix));
    WritePrivateProfileStringW(L"DSPParams", L"EchoMix", buf, g_configPath.c_str());

    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::EQPreamp));
    WritePrivateProfileStringW(L"DSPParams", L"EQPreamp", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::EQBass));
    WritePrivateProfileStringW(L"DSPParams", L"EQBass", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::EQMid));
    WritePrivateProfileStringW(L"DSPParams", L"EQMid", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::EQTreble));
    WritePrivateProfileStringW(L"DSPParams", L"EQTreble", buf, g_configPath.c_str());

    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::CompThreshold));
    WritePrivateProfileStringW(L"DSPParams", L"CompThreshold", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::CompRatio));
    WritePrivateProfileStringW(L"DSPParams", L"CompRatio", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::CompAttack));
    WritePrivateProfileStringW(L"DSPParams", L"CompAttack", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::CompRelease));
    WritePrivateProfileStringW(L"DSPParams", L"CompRelease", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::CompGain));
    WritePrivateProfileStringW(L"DSPParams", L"CompGain", buf, g_configPath.c_str());

    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::StereoWidth));
    WritePrivateProfileStringW(L"DSPParams", L"StereoWidth", buf, g_configPath.c_str());

    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::CenterCancel));
    WritePrivateProfileStringW(L"DSPParams", L"CenterCancel", buf, g_configPath.c_str());

    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::ConvolutionMix));
    WritePrivateProfileStringW(L"DSPParams", L"ConvolutionMix", buf, g_configPath.c_str());
    swprintf(buf, 32, L"%.2f", GetParamValue(ParamId::ConvolutionGain));
    WritePrivateProfileStringW(L"DSPParams", L"ConvolutionGain", buf, g_configPath.c_str());
}

// Save current playback state (file and position)
void SavePlaybackState() {
    // Clear old playlist entries
    WritePrivateProfileSectionW(L"Playlist", L"", g_configPath.c_str());

    if (!g_rememberState) {
        WritePrivateProfileStringW(L"State", L"LastFile", L"", g_configPath.c_str());
        WritePrivateProfileStringW(L"State", L"LastPosition", L"0", g_configPath.c_str());
        WritePrivateProfileStringW(L"State", L"TrackCount", L"0", g_configPath.c_str());
        WritePrivateProfileStringW(L"State", L"CurrentTrack", L"0", g_configPath.c_str());
        return;
    }

    // Save playlist
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", static_cast<int>(g_playlist.size()));
    WritePrivateProfileStringW(L"State", L"TrackCount", buf, g_configPath.c_str());

    for (size_t i = 0; i < g_playlist.size(); i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Track%zu", i);
        WritePrivateProfileStringW(L"Playlist", key, g_playlist[i].c_str(), g_configPath.c_str());
    }

    // Save current track index
    swprintf(buf, 32, L"%d", g_currentTrack);
    WritePrivateProfileStringW(L"State", L"CurrentTrack", buf, g_configPath.c_str());

    // Save current file (for backwards compatibility) and position
    if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
        WritePrivateProfileStringW(L"State", L"LastFile", g_playlist[g_currentTrack].c_str(), g_configPath.c_str());

        // Always save position with playback state (use GetCurrentPosition for tempo processor compatibility)
        double position = GetCurrentPosition();
        swprintf(buf, 32, L"%.2f", position);
        WritePrivateProfileStringW(L"State", L"LastPosition", buf, g_configPath.c_str());
    } else {
        WritePrivateProfileStringW(L"State", L"LastFile", L"", g_configPath.c_str());
        WritePrivateProfileStringW(L"State", L"LastPosition", L"0", g_configPath.c_str());
    }
}

// Load last playback state and resume if enabled
void LoadPlaybackState() {
    if (!g_rememberState) return;

    // Try to load full playlist first
    int trackCount = GetPrivateProfileIntW(L"State", L"TrackCount", 0, g_configPath.c_str());
    int currentTrack = GetPrivateProfileIntW(L"State", L"CurrentTrack", 0, g_configPath.c_str());

    g_playlist.clear();

    if (trackCount > 0) {
        // Load playlist from [Playlist] section
        for (int i = 0; i < trackCount; i++) {
            wchar_t key[32];
            swprintf(key, 32, L"Track%d", i);
            wchar_t filePath[2048] = {0};  // Larger buffer for URLs
            GetPrivateProfileStringW(L"Playlist", key, L"", filePath, 2048, g_configPath.c_str());

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
                    GetPrivateProfileStringW(L"State", L"LastPosition", L"0", posBuf, 32, g_configPath.c_str());
                    double position = _wtof(posBuf);

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
    GetPrivateProfileStringW(L"State", L"LastFile", L"", lastFile, 2048, g_configPath.c_str());

    // Trust save code - don't validate files/URLs here
    if (lastFile[0] != L'\0') {
        g_playlist.push_back(lastFile);
        g_currentTrack = 0;

        // LoadFile handles both files and URLs
        if (LoadFile(lastFile)) {
            // Restore position for seekable streams only (not live streams)
            if (!g_isLiveStream) {
                wchar_t posBuf[32] = {0};
                GetPrivateProfileStringW(L"State", L"LastPosition", L"0", posBuf, 32, g_configPath.c_str());
                double position = _wtof(posBuf);

                if (position > 0) {
                    SeekToPosition(position);
                }
            }
        }
    }
}

// Save position for a specific file (if it's long enough)
void SaveFilePosition(const std::wstring& filePath) {
    if (g_rememberPosMinutes == 0 || !g_fxStream) return;

    // Use tempo processor to get length and position (supports both SoundTouch and Rubber Band)
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) return;

    double length = processor->GetLength();

    // Only save if file is longer than threshold
    if (length < g_rememberPosMinutes * 60.0) return;

    double position = processor->GetPosition();

    SaveFilePositionDB(filePath, position);
}

// Load saved position for a specific file (returns 0 if none or file too short)
double LoadFilePosition(const std::wstring& filePath) {
    if (g_rememberPosMinutes == 0 || !g_fxStream) return 0.0;

    // Use tempo processor to get length (supports both SoundTouch and Rubber Band)
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) return 0.0;

    double length = processor->GetLength();

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
    // Index 10 is chapter seeking (virtual, not in g_seekAmounts array)
    if (index == 10) {
        return g_chapterSeekEnabled && !g_chapters.empty();
    }
    if (index < 0 || index >= g_seekAmountCount) return false;
    if (!g_seekEnabled[index]) return false;
    // Track-based options only available with multiple tracks
    if (g_seekAmounts[index].isTrack && g_playlist.size() <= 1) return false;
    return true;
}

// Total number of seek options including chapter (index 10)
constexpr int SEEK_AMOUNT_TOTAL = 11;

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
        if (g_currentSeekIndex == 10) {
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
            if (g_currentSeekIndex == 10) {
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
    if (g_currentSeekIndex == 10) {
        Speak("1 chapter");
    } else {
        Speak(g_seekAmounts[g_currentSeekIndex].label);
    }
}

void SpeakSeekAmount() {
    if (g_currentSeekIndex == 10) {
        Speak("1 chapter");
    } else if (g_currentSeekIndex >= 0 && g_currentSeekIndex < g_seekAmountCount) {
        Speak(g_seekAmounts[g_currentSeekIndex].label);
    }
}
