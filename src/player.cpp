#include "player.h"
#include "globals.h"
#include "utils.h"
#include "accessibility.h"
#include "settings.h"
#include "ui.h"
#include "effects.h"
#include "resource.h"
#include "bass_fx.h"
#include "bass_aac.h"
#include "bassmidi.h"
#include "bassenc.h"
#include "bassenc_mp3.h"
#include "bassenc_ogg.h"
#include "bassenc_flac.h"
#include "tempo_processor.h"
#include <ctime>
#include <shlobj.h>
#include <map>
#include <algorithm>

// Forward declarations for tag reading helpers (defined later in file)
static std::string GetMetadataTag(HSTREAM stream, const char* tagName);
static std::string GetStreamTitle(HSTREAM stream);
static std::string GetTrimmedTag(const char* data, size_t maxLen);

// Global SoundFont handle for MIDI playback
static HSOUNDFONT g_hSoundFont = 0;

// Track loaded plugins for debugging
static std::vector<std::wstring> g_loadedPlugins;
static std::vector<std::wstring> g_failedPlugins;

void LoadBassPlugins() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Get directory
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
    }

    std::wstring libPath = exePath;
    libPath += L"lib\\";

    // List of plugins to load (playback-related)
    const wchar_t* plugins[] = {
        L"bassflac.dll",   // FLAC
        L"bassopus.dll",   // Opus
        L"basswma.dll",    // WMA
        L"basswv.dll",     // WavPack
        L"bassape.dll",    // Monkey's Audio (APE)
        L"bassalac.dll",   // Apple Lossless (ALAC)
        L"bassmidi.dll",   // MIDI
        L"basscd.dll",     // CD Audio
        L"bassdsd.dll",    // DSD
        L"basshls.dll",    // HLS streaming
        L"bassmix.dll",    // Mixer (for some stream types)
        L"bass_aac.dll",   // AAC/M4A (if available)
    };

    for (const wchar_t* plugin : plugins) {
        std::wstring fullPath = libPath + plugin;
        HPLUGIN hPlugin = BASS_PluginLoad(reinterpret_cast<const char*>(fullPath.c_str()), BASS_UNICODE);
        // If lib\ path failed, try same directory as exe
        if (!hPlugin) {
            std::wstring altPath = exePath;
            altPath += plugin;
            hPlugin = BASS_PluginLoad(reinterpret_cast<const char*>(altPath.c_str()), BASS_UNICODE);
        }

        if (hPlugin) {
            g_loadedPlugins.push_back(plugin);
        } else {
            g_failedPlugins.push_back(plugin);
        }
    }
}

// Get list of loaded plugins (for debugging)
std::wstring GetLoadedPluginsInfo() {
    std::wstring info = L"Loaded: ";
    for (size_t i = 0; i < g_loadedPlugins.size(); i++) {
        if (i > 0) info += L", ";
        info += g_loadedPlugins[i];
    }
    if (g_loadedPlugins.empty()) info += L"(none)";

    info += L"\nFailed: ";
    for (size_t i = 0; i < g_failedPlugins.size(); i++) {
        if (i > 0) info += L", ";
        info += g_failedPlugins[i];
    }
    if (g_failedPlugins.empty()) info += L"(none)";

    return info;
}

// Find device index by name, returns -1 if not found (use default)
int FindDeviceByName(const std::wstring& name) {
    if (name.empty()) return -1;  // Empty name means default device

    BASS_DEVICEINFO info;
    for (int i = 1; BASS_GetDeviceInfo(i, &info); i++) {
        if (info.flags & BASS_DEVICE_ENABLED) {
            // Convert device name to wide string for comparison
            int len = MultiByteToWideChar(CP_ACP, 0, info.name, -1, nullptr, 0);
            std::wstring wideName(len, 0);
            MultiByteToWideChar(CP_ACP, 0, info.name, -1, &wideName[0], len);
            // Remove null terminator from wstring for comparison
            if (!wideName.empty() && wideName.back() == L'\0') {
                wideName.pop_back();
            }
            if (wideName == name) {
                return i;
            }
        }
    }
    return -1;  // Not found, use default
}

// Get device name by index
std::wstring GetDeviceName(int device) {
    if (device <= 0) return L"";  // Default device

    BASS_DEVICEINFO info;
    if (BASS_GetDeviceInfo(device, &info)) {
        int len = MultiByteToWideChar(CP_ACP, 0, info.name, -1, nullptr, 0);
        std::wstring wideName(len, 0);
        MultiByteToWideChar(CP_ACP, 0, info.name, -1, &wideName[0], len);
        // Remove null terminator
        if (!wideName.empty() && wideName.back() == L'\0') {
            wideName.pop_back();
        }
        return wideName;
    }
    return L"";
}

// Show popup menu with audio devices
void ShowAudioDeviceMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    BASS_DEVICEINFO info;
    int itemCount = 0;

    for (int i = 1; BASS_GetDeviceInfo(i, &info); i++) {
        if (info.flags & BASS_DEVICE_ENABLED) {
            // Convert device name to wide string
            int len = MultiByteToWideChar(CP_ACP, 0, info.name, -1, nullptr, 0);
            std::wstring wideName(len, 0);
            MultiByteToWideChar(CP_ACP, 0, info.name, -1, &wideName[0], len);
            if (!wideName.empty() && wideName.back() == L'\0') {
                wideName.pop_back();
            }

            UINT flags = MF_STRING;
            // Check if this is the current device
            if (i == g_selectedDevice || (g_selectedDevice == -1 && (info.flags & BASS_DEVICE_DEFAULT))) {
                flags |= MF_CHECKED;
            }

            AppendMenuW(hMenu, flags, IDM_AUDIO_DEVICE_BASE + i, wideName.c_str());
            itemCount++;
        }
    }

    if (itemCount == 0) {
        DestroyMenu(hMenu);
        Speak("No audio devices found");
        return;
    }

    // Check if window was hidden (for global hotkey support)
    bool wasHidden = !IsWindowVisible(hwnd);
    if (wasHidden) {
        ShowWindow(hwnd, SW_SHOW);
    }

    // Get cursor position for menu
    POINT pt;
    GetCursorPos(&pt);

    // Show the popup menu and get the selected command
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    // Handle device selection
    if (cmd >= IDM_AUDIO_DEVICE_BASE && cmd < IDM_AUDIO_DEVICE_BASE + 100) {
        int deviceIndex = cmd - IDM_AUDIO_DEVICE_BASE;
        SelectAudioDevice(deviceIndex);
    }

    // Re-hide window if it was hidden before
    if (wasHidden) {
        ShowWindow(hwnd, SW_HIDE);
    }
}

// Select and switch to an audio device
void SelectAudioDevice(int deviceIndex) {
    if (deviceIndex <= 0) return;

    // Get device name for announcement
    std::wstring deviceName = GetDeviceName(deviceIndex);

    // Try to reinitialize BASS with the new device
    if (ReinitBass(deviceIndex)) {
        g_selectedDevice = deviceIndex;
        g_selectedDeviceName = deviceName;
        SaveSettings();

        // Announce the change
        std::string msg = "Switched to " + WideToUtf8(deviceName);
        Speak(msg.c_str());
    } else {
        Speak("Failed to switch audio device");
    }
}

// Check if file is a MIDI file by extension
static bool IsMidiFile(const wchar_t* path) {
    const wchar_t* ext = wcsrchr(path, L'.');
    if (!ext) return false;
    return (_wcsicmp(ext, L".mid") == 0 || _wcsicmp(ext, L".midi") == 0 ||
            _wcsicmp(ext, L".kar") == 0 || _wcsicmp(ext, L".rmi") == 0);
}

// Apply MIDI settings (SoundFont, max voices)
void ApplyMidiSettings() {
    // Free previous SoundFont if any
    if (g_hSoundFont) {
        BASS_MIDI_FontFree(g_hSoundFont);
        g_hSoundFont = 0;
    }

    // Set max voices for MIDI playback
    BASS_SetConfig(BASS_CONFIG_MIDI_VOICES, g_midiMaxVoices);

    // Load SoundFont if configured
    if (!g_midiSoundFont.empty()) {
        g_hSoundFont = BASS_MIDI_FontInit(g_midiSoundFont.c_str(), 0);
        if (g_hSoundFont) {
            // Set as default SoundFont for all MIDI streams
            BASS_MIDI_FONT font;
            font.font = g_hSoundFont;
            font.preset = -1;  // All presets
            font.bank = 0;
            BASS_MIDI_StreamSetFonts(0, &font, 1);  // 0 = set default
        }
    }
}

// Initialize BASS library
bool InitBass(HWND hwnd) {
    // Apply buffer settings before init
    BASS_SetConfig(BASS_CONFIG_BUFFER, g_bufferSize);
    BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, g_updatePeriod);

    // Use logarithmic volume curve (more natural for human perception)
    BASS_SetConfig(BASS_CONFIG_CURVE_VOL, TRUE);

    // Find device by saved name
    int device = FindDeviceByName(g_selectedDeviceName);
    g_selectedDevice = device;

    if (!BASS_Init(device, 44100, 0, hwnd, nullptr)) {
        // Try default device as fallback
        if (device != -1) {
            if (BASS_Init(-1, 44100, 0, hwnd, nullptr)) {
                g_selectedDevice = -1;
                g_selectedDeviceName.clear();
            } else {
                MessageBoxW(hwnd, L"Failed to initialize BASS audio library.", APP_NAME, MB_ICONERROR);
                return false;
            }
        } else {
            MessageBoxW(hwnd, L"Failed to initialize BASS audio library.", APP_NAME, MB_ICONERROR);
            return false;
        }
    }

    // Load plugins for additional format support
    LoadBassPlugins();

    // Apply MIDI settings (SoundFont, max voices)
    ApplyMidiSettings();

    // Configure network settings for URL streaming (YouTube, etc.)
    BASS_SetConfigPtr(BASS_CONFIG_NET_AGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    BASS_SetConfig(BASS_CONFIG_NET_TIMEOUT, 30000);  // 30 second timeout
    BASS_SetConfig(BASS_CONFIG_NET_BUFFER, 10000);   // 10 second network buffer (helps with long streams)
    BASS_SetConfig(BASS_CONFIG_NET_PREBUF, 50);      // Start playback when 50% buffered

    return true;
}

// Free BASS resources
void FreeBass() {
    if (g_fxStream) {
        BASS_StreamFree(g_fxStream);
        g_fxStream = 0;
    }
    if (g_stream) {
        BASS_StreamFree(g_stream);
        g_stream = 0;
    }
    g_sourceStream = 0;  // Don't free - owned by tempo processor
    g_currentBitrate = 0;
    BASS_Free();
}

// Check if a path is a URL
bool IsURL(const wchar_t* path) {
    if (!path) return false;
    return (_wcsnicmp(path, L"http://", 7) == 0 ||
            _wcsnicmp(path, L"https://", 8) == 0 ||
            _wcsnicmp(path, L"ftp://", 6) == 0);
}

// Load and play a URL stream
bool LoadURL(const wchar_t* url) {
    g_isLoading = true;

    // Free existing streams safely
    if (g_fxStream) {
        if (g_endSync) {
            BASS_ChannelRemoveSync(g_fxStream, g_endSync);
            g_endSync = 0;
        }
        RemoveDSPEffects();
        BASS_ChannelStop(g_fxStream);
        BASS_StreamFree(g_fxStream);
        g_fxStream = 0;
    }
    if (g_stream) {
        if (g_metaSync) {
            BASS_ChannelRemoveSync(g_stream, g_metaSync);
            g_metaSync = 0;
        }
        BASS_StreamFree(g_stream);
        g_stream = 0;
    }

    // Convert URL to UTF-8 for BASS
    std::string urlUtf8 = WideToUtf8(url);

    // Create URL stream - try without BLOCK first (allows seeking for podcasts)
    // BLOCK mode is only needed for live streams where seeking isn't expected
    DWORD flags = BASS_STREAM_DECODE | BASS_STREAM_STATUS;

    // Try BASS_AAC_StreamCreateURL first (handles raw AAC/M4A better)
    g_stream = BASS_AAC_StreamCreateURL(urlUtf8.c_str(), 0, flags, nullptr, nullptr);

    // If AAC fails, try generic
    if (!g_stream) {
        g_stream = BASS_StreamCreateURL(urlUtf8.c_str(), 0, flags, nullptr, nullptr);
    }

    // If non-BLOCK fails, try with BLOCK (for live streams)
    if (!g_stream) {
        flags = BASS_STREAM_DECODE | BASS_STREAM_STATUS | BASS_STREAM_BLOCK;
        g_stream = BASS_AAC_StreamCreateURL(urlUtf8.c_str(), 0, flags, nullptr, nullptr);
    }

    if (!g_stream) {
        g_stream = BASS_StreamCreateURL(urlUtf8.c_str(), 0, flags, nullptr, nullptr);
    }

    if (!g_stream) {
        g_isLoading = false;
        int error = BASS_ErrorGetCode();
        const wchar_t* errorMsg;
        switch (error) {
            case BASS_ERROR_NONET:    errorMsg = L"No internet connection."; break;
            case BASS_ERROR_FILEOPEN: errorMsg = L"Could not connect to URL."; break;
            case BASS_ERROR_FILEFORM: errorMsg = L"Unsupported stream format. Check bass_aac.dll is in lib folder."; break;
            case BASS_ERROR_CODEC:    errorMsg = L"Required codec is not available."; break;
            case BASS_ERROR_FORMAT:   errorMsg = L"Unsupported sample format."; break;
            case BASS_ERROR_TIMEOUT:  errorMsg = L"Connection timed out."; break;
            case BASS_ERROR_SSL:      errorMsg = L"SSL/HTTPS not supported."; break;
            default:                  errorMsg = L"Could not open stream."; break;
        }
        // Show truncated URL in error message
        std::wstring displayUrl = url;
        if (displayUrl.length() > 100) {
            displayUrl = displayUrl.substr(0, 100) + L"...";
        }
        std::wstring msg = L"Cannot play URL:\n";
        msg += displayUrl;
        msg += L"\n\nError: ";
        msg += errorMsg;
        msg += L" (code ";
        msg += std::to_wstring(error);
        msg += L")";
        MessageBoxW(g_hwnd, msg.c_str(), APP_NAME, MB_ICONERROR);
        return false;
    }

    // Get original sample frequency for rate control
    BASS_CHANNELINFO info;
    BASS_ChannelGetInfo(g_stream, &info);
    g_originalFreq = static_cast<float>(info.freq);

    // Store source stream for bitrate queries
    g_sourceStream = g_stream;

    // Capture initial bitrate (may come from stream headers or BASS attribute)
    float bitrate = 0;
    BASS_ChannelGetAttribute(g_stream, BASS_ATTRIB_BITRATE, &bitrate);
    g_currentBitrate = static_cast<int>(bitrate);
    // If no BASS bitrate, ICY headers will be checked by GetCurrentBitrate()

    // Set up metadata sync for stream title changes (internet radio)
    g_metaSync = BASS_ChannelSetSync(g_stream, BASS_SYNC_META, 0, OnMetaChange, nullptr);

    // For internet streams, always use SoundTouch - RubberBand and Speedy
    // use push-based processing that doesn't work well with network buffering
    SetCurrentAlgorithm(TempoAlgorithm::SoundTouch);

    // Check if this is a live (non-seekable) stream BEFORE setting up tempo
    // Live streams have unknown length (-1 or 0)
    QWORD streamLen = BASS_ChannelGetLength(g_stream, BASS_POS_BYTE);
    g_isLiveStream = (streamLen == (QWORD)-1 || streamLen == 0);

    // Create or reinitialize tempo processor
    FreeTempoProcessor();
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor) {
        BASS_StreamFree(g_stream);
        g_stream = 0;
        g_isLoading = false;
        MessageBoxW(g_hwnd, L"Failed to create tempo processor.", APP_NAME, MB_ICONERROR);
        return false;
    }

    // Restore pitch settings (always works)
    // Only restore tempo if not a live stream (tempo doesn't work on live streams)
    if (!g_isLiveStream) {
        processor->SetTempo(g_tempo);
    }
    processor->SetPitch(g_pitch);

    // Initialize processor - this creates the output stream
    g_fxStream = processor->Initialize(g_stream, g_originalFreq);
    if (!g_fxStream) {
        BASS_StreamFree(g_stream);
        g_stream = 0;
        g_isLoading = false;
        MessageBoxW(g_hwnd, L"Failed to create tempo stream for URL.", APP_NAME, MB_ICONERROR);
        return false;
    }

    // For SoundTouch, g_stream is now owned by g_fxStream (BASS_FX_FREESOURCE)
    if (processor->GetAlgorithm() == TempoAlgorithm::SoundTouch) {
        g_stream = 0;  // Prevent double-free
    }

    // Apply rate using native BASS frequency attribute (skip for live streams)
    if (g_rate != 1.0f && !g_isLiveStream) {
        BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_FREQ, g_originalFreq * g_rate);
    }

    // Set larger playback buffer for streams (helps prevent choppiness during long playback)
    BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_BUFFER, 1.0f);  // 1 second buffer

    // Apply DSP effects (including volume DSP which handles g_volume/g_muted)
    ApplyDSPEffects();

    // Set up end sync for auto-advance
    g_endSync = BASS_ChannelSetSync(g_fxStream, BASS_SYNC_END, 0, OnTrackEnd, nullptr);

    // Start playback
    BASS_ChannelPlay(g_fxStream, FALSE);

    g_isLoading = false;
    UpdateWindowTitle();
    UpdateStatusBar();
    return true;
}

// Parse time string in format HH:MM:SS.mmm or MM:SS.mmm or SS.mmm
static double ParseChapterTime(const char* timeStr) {
    int hours = 0, mins = 0;
    double secs = 0.0;

    // Try HH:MM:SS.mmm format
    if (sscanf(timeStr, "%d:%d:%lf", &hours, &mins, &secs) == 3) {
        return hours * 3600.0 + mins * 60.0 + secs;
    }
    // Try MM:SS.mmm format
    if (sscanf(timeStr, "%d:%lf", &mins, &secs) == 2) {
        return mins * 60.0 + secs;
    }
    // Try SS.mmm format
    if (sscanf(timeStr, "%lf", &secs) == 1) {
        return secs;
    }
    return 0.0;
}

// Parse VorbisComment chapters (used by Ogg, FLAC, Opus)
// Format: CHAPTER001=00:00:00.000 and CHAPTER001NAME=Chapter Name
static void ParseVorbisCommentChapters(HSTREAM stream) {
    const char* tags = BASS_ChannelGetTags(stream, BASS_TAG_OGG);
    if (!tags) return;

    // Build a map of chapter numbers to times and names
    std::map<int, std::pair<double, std::string>> chapterMap;

    // Iterate through all tags
    while (*tags) {
        std::string tag = tags;
        tags += tag.length() + 1;

        // Check for CHAPTERnnn= format
        if (_strnicmp(tag.c_str(), "CHAPTER", 7) == 0) {
            const char* p = tag.c_str() + 7;
            // Parse the chapter number
            int num = 0;
            while (*p >= '0' && *p <= '9') {
                num = num * 10 + (*p - '0');
                p++;
            }
            if (num > 0) {
                if (_strnicmp(p, "NAME=", 5) == 0) {
                    // This is a chapter name
                    chapterMap[num].second = p + 5;
                } else if (*p == '=') {
                    // This is a chapter time
                    chapterMap[num].first = ParseChapterTime(p + 1);
                }
            }
        }
    }

    // Convert map to sorted chapter list
    for (const auto& kv : chapterMap) {
        if (kv.second.first > 0 || kv.first == 1) {  // Allow chapter 1 at 0:00
            Chapter ch;
            ch.position = kv.second.first;
            if (kv.second.second.empty()) {
                // Generate default name
                wchar_t buf[32];
                swprintf(buf, 32, L"Chapter %d", kv.first);
                ch.name = buf;
            } else {
                ch.name = Utf8ToWide(kv.second.second);
            }
            g_chapters.push_back(ch);
        }
    }
}

// Parse ID3v2 CHAP frames (used by MP3)
// CHAP frame structure:
//   Element ID (null-terminated string)
//   Start time (4 bytes, big-endian, milliseconds)
//   End time (4 bytes)
//   Start offset (4 bytes)
//   End offset (4 bytes)
//   Optional sub-frames (e.g., TIT2 for title)
static void ParseID3v2Chapters(HSTREAM stream) {
    const unsigned char* id3v2 = (const unsigned char*)BASS_ChannelGetTags(stream, BASS_TAG_ID3V2);
    if (!id3v2) return;

    // Check ID3v2 header
    if (memcmp(id3v2, "ID3", 3) != 0) return;

    unsigned char version = id3v2[3];
    unsigned char flags = id3v2[5];

    // Calculate tag size (syncsafe integer)
    size_t tagSize = ((id3v2[6] & 0x7F) << 21) |
                     ((id3v2[7] & 0x7F) << 14) |
                     ((id3v2[8] & 0x7F) << 7) |
                     (id3v2[9] & 0x7F);

    size_t pos = 10;  // Header size

    // Skip extended header if present
    if (flags & 0x40) {
        size_t extSize = (version >= 4) ?
            (((id3v2[pos] & 0x7F) << 21) | ((id3v2[pos+1] & 0x7F) << 14) |
             ((id3v2[pos+2] & 0x7F) << 7) | (id3v2[pos+3] & 0x7F)) :
            ((id3v2[pos] << 24) | (id3v2[pos+1] << 16) |
             (id3v2[pos+2] << 8) | id3v2[pos+3]);
        pos += (version >= 4) ? extSize : (extSize + 4);
    }

    // Parse frames
    while (pos + 10 < tagSize + 10) {
        const unsigned char* frame = id3v2 + pos;

        // Check for padding (all zeros)
        if (frame[0] == 0) break;

        // Get frame ID
        char frameId[5] = {0};
        memcpy(frameId, frame, 4);

        // Get frame size
        size_t frameSize;
        if (version >= 4) {
            // v2.4 uses syncsafe integers
            frameSize = ((frame[4] & 0x7F) << 21) |
                        ((frame[5] & 0x7F) << 14) |
                        ((frame[6] & 0x7F) << 7) |
                        (frame[7] & 0x7F);
        } else {
            // v2.3 uses normal integers
            frameSize = (frame[4] << 24) | (frame[5] << 16) |
                        (frame[6] << 8) | frame[7];
        }

        if (frameSize == 0 || pos + 10 + frameSize > tagSize + 10) break;

        // Check if this is a CHAP frame
        if (strcmp(frameId, "CHAP") == 0) {
            const unsigned char* data = frame + 10;
            size_t dataLen = frameSize;

            // Find end of element ID (null-terminated)
            size_t elemIdLen = 0;
            while (elemIdLen < dataLen && data[elemIdLen] != 0) elemIdLen++;

            if (elemIdLen + 17 <= dataLen) {  // Element ID + null + 16 bytes for times/offsets
                // Start time in milliseconds (big-endian)
                uint32_t startMs = (data[elemIdLen + 1] << 24) |
                                   (data[elemIdLen + 2] << 16) |
                                   (data[elemIdLen + 3] << 8) |
                                   data[elemIdLen + 4];

                Chapter ch;
                ch.position = startMs / 1000.0;

                // Look for TIT2 sub-frame for chapter title
                size_t subPos = elemIdLen + 17;  // After times/offsets
                while (subPos + 10 < dataLen) {
                    char subFrameId[5] = {0};
                    memcpy(subFrameId, data + subPos, 4);

                    size_t subFrameSize;
                    if (version >= 4) {
                        subFrameSize = ((data[subPos + 4] & 0x7F) << 21) |
                                       ((data[subPos + 5] & 0x7F) << 14) |
                                       ((data[subPos + 6] & 0x7F) << 7) |
                                       (data[subPos + 7] & 0x7F);
                    } else {
                        subFrameSize = (data[subPos + 4] << 24) |
                                       (data[subPos + 5] << 16) |
                                       (data[subPos + 6] << 8) |
                                       data[subPos + 7];
                    }

                    if (subFrameSize == 0 || subPos + 10 + subFrameSize > dataLen) break;

                    if (strcmp(subFrameId, "TIT2") == 0 && subFrameSize > 1) {
                        // TIT2 frame: first byte is encoding, rest is text
                        unsigned char encoding = data[subPos + 10];
                        const unsigned char* textStart = data + subPos + 11;
                        size_t textLen = subFrameSize - 1;

                        if (encoding == 0 || encoding == 3) {
                            // ISO-8859-1 or UTF-8
                            std::string text((const char*)textStart, textLen);
                            // Remove trailing nulls
                            while (!text.empty() && text.back() == 0) text.pop_back();
                            ch.name = Utf8ToWide(text);
                        } else if (encoding == 1 || encoding == 2) {
                            // UTF-16 with or without BOM
                            if (textLen >= 2) {
                                bool bigEndian = (encoding == 2) ||
                                                 (textStart[0] == 0xFE && textStart[1] == 0xFF);
                                const unsigned char* start = textStart;
                                if (textStart[0] == 0xFF || textStart[0] == 0xFE) {
                                    start += 2;
                                    textLen -= 2;
                                }
                                std::wstring wtext;
                                for (size_t i = 0; i + 1 < textLen; i += 2) {
                                    wchar_t wc = bigEndian ?
                                        ((start[i] << 8) | start[i + 1]) :
                                        (start[i] | (start[i + 1] << 8));
                                    if (wc == 0) break;
                                    wtext += wc;
                                }
                                ch.name = wtext;
                            }
                        }
                        break;
                    }
                    subPos += 10 + subFrameSize;
                }

                // Generate default name if none found
                if (ch.name.empty()) {
                    wchar_t buf[32];
                    swprintf(buf, 32, L"Chapter %d", (int)g_chapters.size() + 1);
                    ch.name = buf;
                }

                g_chapters.push_back(ch);
            }
        }

        pos += 10 + frameSize;
    }

    // Sort chapters by position
    std::sort(g_chapters.begin(), g_chapters.end(),
              [](const Chapter& a, const Chapter& b) { return a.position < b.position; });
}

// Parse chapters from the current stream
void ParseChapters(HSTREAM stream) {
    g_chapters.clear();

    if (!stream) return;

    // Try VorbisComment format (Ogg/FLAC/Opus)
    ParseVorbisCommentChapters(stream);

    // If no chapters found, try ID3v2 format (MP3)
    if (g_chapters.empty()) {
        ParseID3v2Chapters(stream);
    }
}

// Load and play a file (or URL)
bool LoadFile(const wchar_t* path) {
    // Check if this is a URL
    if (IsURL(path)) {
        return LoadURL(path);
    }
    g_isLoading = true;
    g_isLiveStream = false;  // Local files are always seekable

    // Free existing streams safely
    if (g_fxStream) {
        // Remove sync first to prevent callbacks during cleanup
        if (g_endSync) {
            BASS_ChannelRemoveSync(g_fxStream, g_endSync);
            g_endSync = 0;
        }
        // Remove DSP effects before freeing stream
        RemoveDSPEffects();
        // Stop playback before freeing
        BASS_ChannelStop(g_fxStream);
        BASS_StreamFree(g_fxStream);
        g_fxStream = 0;
    }
    if (g_stream) {
        BASS_StreamFree(g_stream);
        g_stream = 0;
    }

    // Create source stream (use MIDI-specific function for MIDI files if sinc interp enabled)
    if (IsMidiFile(path) && g_midiSincInterp) {
        DWORD flags = BASS_UNICODE | BASS_STREAM_DECODE | BASS_MIDI_SINCINTER;
        g_stream = BASS_MIDI_StreamCreateFile(FALSE, path, 0, 0, flags, 0);
        // Apply SoundFont to this specific stream if loaded
        if (g_stream && g_hSoundFont) {
            BASS_MIDI_FONT font;
            font.font = g_hSoundFont;
            font.preset = -1;
            font.bank = 0;
            BASS_MIDI_StreamSetFonts(g_stream, &font, 1);
        }
    } else {
        g_stream = BASS_StreamCreateFile(FALSE, path, 0, 0, BASS_UNICODE | BASS_STREAM_DECODE);
    }
    if (!g_stream) {
        g_isLoading = false;
        // Only show error if this is the only file in the playlist
        if (g_playlist.size() <= 1) {
            int error = BASS_ErrorGetCode();
            const wchar_t* errorMsg;
            switch (error) {
                case BASS_ERROR_FILEOPEN: errorMsg = L"Could not open the file."; break;
                case BASS_ERROR_FILEFORM: errorMsg = L"Unsupported file format."; break;
                case BASS_ERROR_CODEC:    errorMsg = L"Required codec is not available."; break;
                case BASS_ERROR_FORMAT:   errorMsg = L"Unsupported sample format."; break;
                case BASS_ERROR_MEM:      errorMsg = L"Out of memory."; break;
                case BASS_ERROR_NO3D:     errorMsg = L"3D sound is not available."; break;
                default:                  errorMsg = L"Unknown error."; break;
            }
            std::wstring msg = L"Cannot play file:\n";
            msg += GetFileName(path);
            msg += L"\n\n";
            msg += errorMsg;
            MessageBoxW(g_hwnd, msg.c_str(), APP_NAME, MB_ICONERROR);
        }
        return false;
    }

    // Get original sample frequency for rate control
    BASS_CHANNELINFO info;
    BASS_ChannelGetInfo(g_stream, &info);
    g_originalFreq = static_cast<float>(info.freq);

    // Store source stream for VBR bitrate queries (not freed separately - owned by tempo processor)
    g_sourceStream = g_stream;

    // Capture initial bitrate
    float bitrate = 0;
    BASS_ChannelGetAttribute(g_stream, BASS_ATTRIB_BITRATE, &bitrate);
    g_currentBitrate = static_cast<int>(bitrate);

    // Set up tempo processor based on selected algorithm
    TempoAlgorithm algo = static_cast<TempoAlgorithm>(g_tempoAlgorithm);
    SetCurrentAlgorithm(algo);

    // Create or reinitialize tempo processor
    FreeTempoProcessor();
    TempoProcessor* processor = GetTempoProcessor();

    // Restore tempo/pitch settings to processor before initializing
    // Note: Rate is handled via BASS_ATTRIB_FREQ below, not through tempo processor
    processor->SetTempo(g_tempo);
    processor->SetPitch(g_pitch);

    // Initialize processor - this creates the output stream
    g_fxStream = processor->Initialize(g_stream, g_originalFreq);
    if (!g_fxStream) {
        // Fall back to SoundTouch if Rubber Band fails
        if (algo != TempoAlgorithm::SoundTouch) {
            FreeTempoProcessor();
            SetCurrentAlgorithm(TempoAlgorithm::SoundTouch);
            processor = GetTempoProcessor();
            processor->SetTempo(g_tempo);
            processor->SetPitch(g_pitch);
            g_fxStream = processor->Initialize(g_stream, g_originalFreq);
        }

        if (!g_fxStream) {
            BASS_StreamFree(g_stream);
            g_stream = 0;
            g_isLoading = false;
            if (g_playlist.size() <= 1) {
                MessageBoxW(g_hwnd, L"Failed to create tempo stream.", APP_NAME, MB_ICONERROR);
            }
            return false;
        }
    }

    // For SoundTouch, g_stream is now owned by g_fxStream (BASS_FX_FREESOURCE)
    if (processor->GetAlgorithm() == TempoAlgorithm::SoundTouch) {
        g_stream = 0;  // Prevent double-free
    }

    // Apply rate using native BASS frequency attribute
    if (g_rate != 1.0f) {
        BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_FREQ, g_originalFreq * g_rate);
    }

    // Apply DSP effects (including volume DSP which handles g_volume/g_muted)
    ApplyDSPEffects();

    // Set up end sync for auto-advance on output stream
    g_endSync = BASS_ChannelSetSync(g_fxStream, BASS_SYNC_END, 0, OnTrackEnd, nullptr);

    // Restore saved position for this file (if any)
    double savedPos = LoadFilePosition(path);
    if (savedPos > 0) {
        processor->SetPosition(savedPos);
    }

    // Start playback on output stream
    BASS_ChannelPlay(g_fxStream, FALSE);

    // Parse chapters from file (if any)
    // For SoundTouch, use g_fxStream since it owns g_stream
    // For Rubber Band, use g_stream (original source)
    HSTREAM chapterStream = g_stream ? g_stream : g_fxStream;
    ParseChapters(chapterStream);

    g_isLoading = false;
    UpdateWindowTitle();
    UpdateStatusBar();
    return true;
}

// Sync callback when track ends
void CALLBACK OnTrackEnd(HSYNC handle, DWORD channel, DWORD data, void* user) {
    // Post message to main thread to advance track
    // Use a custom message if auto-advance is disabled to load but not play
    if (g_autoAdvance) {
        PostMessage(g_hwnd, WM_COMMAND, IDM_PLAY_NEXT, 0);
    } else {
        // Load next track but don't auto-play - use lParam=1 to indicate no auto-play
        PostMessage(g_hwnd, WM_COMMAND, IDM_PLAY_NEXT, 1);
    }
}

// Sync callback when stream metadata changes (for internet radio)
void CALLBACK OnMetaChange(HSYNC handle, DWORD channel, DWORD data, void* user) {
    // Post message to main thread to announce new track
    PostMessage(g_hwnd, WM_META_CHANGED, 0, 0);
}

// Called from main thread when metadata changes - announces new stream track
void AnnounceStreamMetadata() {
    if (!g_speechTrackChange) return;

    HSTREAM stream = g_stream ? g_stream : g_fxStream;
    if (!stream) return;

    std::string streamTitle = GetStreamTitle(stream);
    if (!streamTitle.empty()) {
        Speak(streamTitle);
    }
}

// Play or pause current track
void PlayPause() {
    if (!g_fxStream) {
        // Nothing loaded - try to reload current track or play first
        Play();
        return;
    }

    DWORD state = BASS_ChannelIsActive(g_fxStream);
    if (state == BASS_ACTIVE_PLAYING) {
        // For live streams, stop instead of pause
        if (g_isLiveStream) {
            Stop();
        } else {
            BASS_ChannelPause(g_fxStream);
        }
    } else {
        BASS_ChannelPlay(g_fxStream, FALSE);
    }
    UpdateWindowTitle();
    UpdateStatusBar();
}

// Free current stream (used when stopping live streams)
void FreeCurrentStream() {
    if (g_fxStream) {
        if (g_endSync) {
            BASS_ChannelRemoveSync(g_fxStream, g_endSync);
            g_endSync = 0;
        }
        RemoveDSPEffects();
        BASS_ChannelStop(g_fxStream);
        BASS_StreamFree(g_fxStream);
        g_fxStream = 0;
    }
    if (g_stream) {
        if (g_metaSync) {
            BASS_ChannelRemoveSync(g_stream, g_metaSync);
            g_metaSync = 0;
        }
        BASS_StreamFree(g_stream);
        g_stream = 0;
    }
    g_sourceStream = 0;
    g_isLiveStream = false;
    g_currentBitrate = 0;
    FreeTempoProcessor();
}

// Play (restart if playing, resume if paused/stopped)
void Play() {
    if (!g_fxStream) {
        // Nothing loaded - check if we have a current track to reload
        // (This handles the case where a live stream was stopped/freed)
        if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
            const std::wstring& path = g_playlist[g_currentTrack];
            if (IsURL(path.c_str())) {
                LoadURL(path.c_str());
            } else {
                LoadFile(path.c_str());
            }
            if (g_fxStream) {
                BASS_ChannelPlay(g_fxStream, FALSE);
                UpdateWindowTitle();
                UpdateStatusBar();
            }
            return;
        }
        // No current track, try to play first track
        if (!g_playlist.empty()) {
            PlayTrack(0);
        }
        return;
    }

    DWORD state = BASS_ChannelIsActive(g_fxStream);
    if (state == BASS_ACTIVE_PLAYING) {
        // Already playing - restart from beginning
        TempoProcessor* processor = GetTempoProcessor();
        if (processor && processor->IsActive()) {
            processor->SetPosition(0);
        }
    }
    BASS_ChannelPlay(g_fxStream, FALSE);
    UpdateWindowTitle();
    UpdateStatusBar();
}

// Pause playback
void Pause() {
    if (g_fxStream) {
        // Don't allow pausing live streams
        if (g_isLiveStream) {
            Speak("Cannot pause live stream");
            return;
        }
        BASS_ChannelPause(g_fxStream);
        UpdateWindowTitle();
        UpdateStatusBar();
    }
}

// Stop playback
void Stop() {
    if (g_fxStream) {
        // For live streams, free the stream entirely to disconnect
        // (otherwise BASS buffers it and stop/play acts like pause/resume)
        if (g_isLiveStream) {
            FreeCurrentStream();
        } else {
            BASS_ChannelStop(g_fxStream);
            TempoProcessor* processor = GetTempoProcessor();
            if (processor && processor->IsActive()) {
                processor->SetPosition(0);
            }
        }
    }
    UpdateWindowTitle();
    UpdateStatusBar();
}

// Seek relative to current position
void Seek(double seconds) {
    if (!g_fxStream || g_isBusy || g_isLoading) return;

    // Verify stream is still valid
    DWORD state = BASS_ChannelIsActive(g_fxStream);
    if (state == BASS_ACTIVE_STOPPED && BASS_ErrorGetCode() == BASS_ERROR_HANDLE) {
        g_fxStream = 0;
        g_stream = 0;
        return;
    }

    // Use tempo processor for position handling (supports both SoundTouch and Rubber Band)
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) return;

    double length = processor->GetLength();
    if (length <= 0) return;  // Invalid or unknown length

    double currentPos = processor->GetPosition();
    double newPos = currentPos + seconds;
    if (newPos < 0) newPos = 0;
    if (newPos > length) newPos = length;

    processor->SetPosition(newPos);

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
    if (!g_fxStream) return;
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) return;

    double duration = processor->GetLength();
    if (seconds < 0) seconds = 0;
    if (seconds > duration) seconds = duration;

    processor->SetPosition(seconds);
    UpdateStatusBar();
}

// Get current playback position in seconds
double GetCurrentPosition() {
    if (!g_fxStream) return 0.0;
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) return 0.0;
    return processor->GetPosition();
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

// Seek to next chapter (returns true if successful)
bool SeekToNextChapter() {
    if (g_chapters.empty() || !g_fxStream) return false;

    double pos = GetCurrentPosition();

    // Find the first chapter after current position (with small tolerance for current chapter)
    for (size_t i = 0; i < g_chapters.size(); i++) {
        if (g_chapters[i].position > pos + 0.5) {  // 0.5s tolerance
            SeekToPosition(g_chapters[i].position);

            // Announce chapter name
            if (!g_chapters[i].name.empty()) {
                Speak("Chapter " + std::to_string(i + 1) + ": " + WideToUtf8(g_chapters[i].name));
            } else {
                Speak("Chapter " + std::to_string(i + 1));
            }
            return true;
        }
    }
    return false;  // Already at or past last chapter
}

// Seek to previous chapter (returns true if successful)
bool SeekToPrevChapter() {
    if (g_chapters.empty() || !g_fxStream) return false;

    double pos = GetCurrentPosition();

    // Find the chapter before current position
    // If we're more than 3 seconds into a chapter, go to its start
    // Otherwise go to the previous chapter
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
        if (!g_chapters[currentChapter].name.empty()) {
            Speak("Chapter " + std::to_string(currentChapter + 1) + ": " + WideToUtf8(g_chapters[currentChapter].name));
        } else {
            Speak("Chapter " + std::to_string(currentChapter + 1));
        }
        return true;
    } else if (currentChapter > 0) {
        // Go to previous chapter
        int prevChapter = currentChapter - 1;
        SeekToPosition(g_chapters[prevChapter].position);
        if (!g_chapters[prevChapter].name.empty()) {
            Speak("Chapter " + std::to_string(prevChapter + 1) + ": " + WideToUtf8(g_chapters[prevChapter].name));
        } else {
            Speak("Chapter " + std::to_string(prevChapter + 1));
        }
        return true;
    } else {
        // At first chapter, go to start
        SeekToPosition(0);
        Speak("Beginning");
        return true;
    }
}

// Set volume (0.0 - 1.0)
// Volume is applied via DSP (not BASS_ATTRIB_VOL) so recording captures full volume
// Unless legacy mode is enabled, which uses BASS_ATTRIB_VOL (faster but affects recordings)
void SetVolume(float vol) {
    float maxVol = g_allowAmplify ? MAX_VOLUME_AMPLIFY : MAX_VOLUME_NORMAL;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > maxVol) vol = maxVol;
    g_volume = vol;

    // In legacy mode, use BASS_ATTRIB_VOL (faster but affects recordings)
    // In normal mode, volume DSP automatically uses updated g_volume
    if (g_legacyVolume && g_fxStream) {
        float curvedVolume = vol * vol;  // Apply perceptual curve
        BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, curvedVolume);
    }

    // Announce volume if setting enabled
    if (g_speechVolume) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Volume %d%%", static_cast<int>(g_volume * 100 + 0.5f));
        Speak(buf);
    }

    UpdateStatusBar();
}

// Toggle mute (recording captures full volume since mute is applied via DSP)
// In legacy mode, mute uses BASS_ATTRIB_VOL (faster but affects recordings)
void ToggleMute() {
    g_muted = !g_muted;

    // In legacy mode, use BASS_ATTRIB_VOL
    if (g_legacyVolume && g_fxStream) {
        if (g_muted) {
            BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, 0.0f);
        } else {
            float curvedVolume = g_volume * g_volume;
            BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, curvedVolume);
        }
    }
    // In normal mode, volume DSP automatically uses updated g_muted

    Speak(g_muted ? "Muted" : "Unmuted");
    UpdateStatusBar();
}

// Speak elapsed time
void SpeakElapsed() {
    if (!g_fxStream) return;
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) return;
    double pos = processor->GetPosition();
    std::wstring posStr = FormatTime(pos);
    Speak(WideToUtf8(posStr));
}

// Speak remaining time
void SpeakRemaining() {
    if (!g_fxStream) return;
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) return;
    double pos = processor->GetPosition();
    double len = processor->GetLength();
    double remaining = len - pos;
    if (remaining < 0) remaining = 0;
    std::wstring remStr = FormatTime(remaining);
    Speak(WideToUtf8(remStr));
}

// Speak total time
void SpeakTotal() {
    if (!g_fxStream) return;
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) return;
    double len = processor->GetLength();
    std::wstring lenStr = FormatTime(len);
    Speak(WideToUtf8(lenStr));
}

// Play a specific track by index
void PlayTrack(int index, bool autoPlay) {
    if (g_isBusy) return;  // Prevent re-entrancy
    if (index < 0 || index >= static_cast<int>(g_playlist.size())) {
        return;
    }

    g_isBusy = true;

    // Save position of current track before switching
    if (g_fxStream && g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
        SaveFilePosition(g_playlist[g_currentTrack]);
    }

    // Try to load tracks, skipping failures (up to 10 attempts to prevent infinite loop)
    int attempts = 0;
    bool loadedSuccessfully = false;
    while (index < static_cast<int>(g_playlist.size()) && attempts < 10) {
        g_currentTrack = index;
        if (LoadFile(g_playlist[index].c_str())) {
            loadedSuccessfully = true;
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

    // If autoPlay is false, pause immediately after loading
    if (loadedSuccessfully && !autoPlay && g_fxStream) {
        BASS_ChannelPause(g_fxStream);
    }

    // Announce track change if setting is enabled
    if (loadedSuccessfully && g_speechTrackChange) {
        // For streams, announce the stream title; for files, announce title or filename
        HSTREAM stream = g_stream ? g_stream : g_fxStream;
        if (stream) {
            std::string streamTitle = GetStreamTitle(stream);
            if (!streamTitle.empty()) {
                Speak(streamTitle);
            } else {
                std::string title = GetMetadataTag(stream, "TITLE");
                std::string artist = GetMetadataTag(stream, "ARTIST");

                // Try ID3v1 if nothing found
                if (title.empty()) {
                    const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
                    if (id3) {
                        title = GetTrimmedTag(id3->title, 30);
                        if (artist.empty()) artist = GetTrimmedTag(id3->artist, 30);
                    }
                }

                if (!title.empty() && !artist.empty()) {
                    Speak(artist + " - " + title);
                } else if (!title.empty()) {
                    Speak(title);
                } else {
                    // Fall back to filename
                    std::wstring path = g_playlist[g_currentTrack];
                    const wchar_t* lastSlash = wcsrchr(path.c_str(), L'\\');
                    if (!lastSlash) lastSlash = wcsrchr(path.c_str(), L'/');
                    std::wstring filename = lastSlash ? (lastSlash + 1) : path;
                    char buf[512];
                    WideCharToMultiByte(CP_UTF8, 0, filename.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
                    Speak(buf);
                }
            }
        }
    }

    g_isBusy = false;
}

// Play next track
void NextTrack(bool autoPlay) {
    if (g_playlist.empty() || g_isBusy) return;

    int next;
    if (g_shuffle && g_playlist.size() > 1) {
        // Random track (excluding current)
        do {
            next = rand() % static_cast<int>(g_playlist.size());
        } while (next == g_currentTrack && g_playlist.size() > 1);
    } else {
        next = g_currentTrack + 1;
        if (next >= static_cast<int>(g_playlist.size())) {
            // End of playlist - stop
            Stop();
            return;
        }
    }
    PlayTrack(next, autoPlay);
}

// Play previous track
void PrevTrack() {
    if (g_playlist.empty() || g_isBusy) return;

    // If we're more than 3 seconds in, restart current track
    if (g_fxStream) {
        TempoProcessor* processor = GetTempoProcessor();
        if (processor && processor->IsActive()) {
            double pos = processor->GetPosition();
            if (pos > 3.0) {
                processor->SetPosition(0);
                UpdateStatusBar();
                return;
            }
        }
    }

    int prev = g_currentTrack - 1;
    if (prev < 0) prev = 0;
    PlayTrack(prev);
}

// Reinitialize BASS with a different device
bool ReinitBass(int device) {
    // Save current state
    bool wasPlaying = g_fxStream && (BASS_ChannelIsActive(g_fxStream) == BASS_ACTIVE_PLAYING);
    bool wasPaused = g_fxStream && (BASS_ChannelIsActive(g_fxStream) == BASS_ACTIVE_PAUSED);
    double position = 0;
    std::wstring currentFile;

    if (g_fxStream) {
        // Use tempo processor to get position
        TempoProcessor* processor = GetTempoProcessor();
        if (processor && processor->IsActive()) {
            position = processor->GetPosition();
        }
        if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
            currentFile = g_playlist[g_currentTrack];
        }
        // Remove DSP effects before freeing stream (resets handles to 0)
        RemoveDSPEffects();
        // Free tempo processor before freeing BASS
        FreeTempoProcessor();
        if (g_fxStream) {
            BASS_StreamFree(g_fxStream);
            g_fxStream = 0;
        }
        if (g_stream) {
            BASS_StreamFree(g_stream);
            g_stream = 0;
        }
    }

    BASS_Free();

    if (!BASS_Init(device, 44100, 0, g_hwnd, nullptr)) {
        // Try default device as fallback
        if (device != -1) {
            if (BASS_Init(-1, 44100, 0, g_hwnd, nullptr)) {
                g_selectedDevice = -1;
                g_selectedDeviceName.clear();
            }
        }
        return false;
    }

    g_selectedDevice = device;
    // Update device name from the actual device
    g_selectedDeviceName = GetDeviceName(device);

    // Restore playback state if we had a file loaded
    if (!currentFile.empty()) {
        LoadFile(currentFile.c_str());
        if (g_fxStream) {
            // Use tempo processor to set position
            TempoProcessor* processor = GetTempoProcessor();
            if (processor && processor->IsActive()) {
                processor->SetPosition(position);
            }
            // LoadFile() auto-starts playback, so we need to pause/stop if we weren't playing
            if (!wasPlaying) {
                if (wasPaused) {
                    BASS_ChannelPause(g_fxStream);
                } else {
                    // Was stopped - pause the stream (stop would reset position)
                    BASS_ChannelPause(g_fxStream);
                }
            }
            UpdateWindowTitle();
            UpdateStatusBar();
        }
    }

    return true;
}

// ID3v1 genre names table
static const char* g_id3Genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
    "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
    "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
    "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop",
    "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental",
    "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "AlternRock",
    "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop",
    "Instrumental Rock", "Ethnic", "Gothic", "Darkwave", "Techno-Industrial",
    "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy",
    "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
    "Native American", "Cabaret", "New Wave", "Psychadelic", "Rave", "Showtunes",
    "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro",
    "Musical", "Rock & Roll", "Hard Rock"
};
static const int g_id3GenreCount = sizeof(g_id3Genres) / sizeof(g_id3Genres[0]);

// Helper: Get a tag value from null-terminated string list (OGG, APE, MP4, etc.)
static std::string GetTagFromList(const char* tags, const char* key) {
    if (!tags || !key) return "";

    size_t keyLen = strlen(key);
    const char* p = tags;

    while (*p) {
        // Check if this line starts with key= (case insensitive)
        if (_strnicmp(p, key, keyLen) == 0 && p[keyLen] == '=') {
            return std::string(p + keyLen + 1);
        }
        p += strlen(p) + 1;  // Move to next string
    }
    return "";
}

// Helper: Parse ID3v2 text frame (handles encoding byte)
static std::string ParseID3v2TextFrame(const unsigned char* data, size_t size) {
    if (!data || size < 1) return "";

    unsigned char encoding = data[0];
    const unsigned char* text = data + 1;
    size_t textLen = size - 1;

    if (textLen == 0) return "";

    if (encoding == 0) {
        // ISO-8859-1 (Latin-1) - convert to UTF-8
        // Remove trailing nulls (single-byte encoding)
        while (textLen > 0 && text[textLen - 1] == 0) textLen--;
        if (textLen == 0) return "";

        std::wstring wstr;
        for (size_t i = 0; i < textLen; i++) {
            wstr += static_cast<wchar_t>(text[i]);
        }
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (utf8Len > 0) {
            std::string result(utf8Len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], utf8Len, nullptr, nullptr);
            return result;
        }
        return std::string((const char*)text, textLen);
    } else if (encoding == 1) {
        // UTF-16 with BOM
        if (textLen < 2) return "";
        bool bigEndian = (text[0] == 0xFE && text[1] == 0xFF);
        const unsigned char* textStart = text + 2;
        size_t byteCount = textLen - 2;

        // Remove trailing null WORDS (2 bytes at a time for UTF-16)
        while (byteCount >= 2 && textStart[byteCount - 1] == 0 && textStart[byteCount - 2] == 0) {
            byteCount -= 2;
        }

        size_t charCount = byteCount / 2;

        // Build wstring, handling endianness
        std::wstring wstr;
        for (size_t i = 0; i < charCount; i++) {
            wchar_t ch;
            if (bigEndian) {
                ch = (textStart[i * 2] << 8) | textStart[i * 2 + 1];
            } else {
                ch = textStart[i * 2] | (textStart[i * 2 + 1] << 8);
            }
            if (ch == 0) break;  // Stop at null terminator
            wstr += ch;
        }

        // Convert to UTF-8
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (utf8Len > 0) {
            std::string result(utf8Len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], utf8Len, nullptr, nullptr);
            return result;
        }
        return "";
    } else if (encoding == 2) {
        // UTF-16BE without BOM
        // Remove trailing null WORDS (2 bytes at a time)
        while (textLen >= 2 && text[textLen - 1] == 0 && text[textLen - 2] == 0) {
            textLen -= 2;
        }

        size_t charCount = textLen / 2;
        std::wstring wstr;
        for (size_t i = 0; i < charCount; i++) {
            wchar_t ch = (text[i * 2] << 8) | text[i * 2 + 1];
            if (ch == 0) break;
            wstr += ch;
        }
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (utf8Len > 0) {
            std::string result(utf8Len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], utf8Len, nullptr, nullptr);
            return result;
        }
        return "";
    } else if (encoding == 3) {
        // UTF-8 - remove trailing nulls (single-byte null terminator)
        while (textLen > 0 && text[textLen - 1] == 0) textLen--;
        if (textLen == 0) return "";
        return std::string((const char*)text, textLen);
    }
    return "";
}

// Helper: Get ID3v2 frame by ID (e.g., "TIT2", "TPE1", "TALB")
static std::string GetID3v2Frame(const unsigned char* tag, const char* frameId) {
    if (!tag || !frameId) return "";

    // Check ID3v2 header
    if (tag[0] != 'I' || tag[1] != 'D' || tag[2] != '3') return "";

    unsigned char version = tag[3];
    // unsigned char revision = tag[4];
    unsigned char flags = tag[5];

    // Tag size (syncsafe integer)
    size_t tagSize = ((tag[6] & 0x7F) << 21) | ((tag[7] & 0x7F) << 14) |
                     ((tag[8] & 0x7F) << 7) | (tag[9] & 0x7F);

    const unsigned char* pos = tag + 10;
    const unsigned char* end = tag + 10 + tagSize;

    // Skip extended header if present
    if (flags & 0x40) {
        size_t extSize = (pos[0] << 24) | (pos[1] << 16) | (pos[2] << 8) | pos[3];
        pos += 4 + extSize;
    }

    // Parse frames
    while (pos < end - 10) {
        // Frame header
        char id[5] = {(char)pos[0], (char)pos[1], (char)pos[2], (char)pos[3], 0};

        // End of frames (padding)
        if (id[0] == 0) break;

        size_t frameSize;
        if (version >= 4) {
            // ID3v2.4: syncsafe integer
            frameSize = ((pos[4] & 0x7F) << 21) | ((pos[5] & 0x7F) << 14) |
                        ((pos[6] & 0x7F) << 7) | (pos[7] & 0x7F);
        } else {
            // ID3v2.3: regular integer
            frameSize = (pos[4] << 24) | (pos[5] << 16) | (pos[6] << 8) | pos[7];
        }

        // unsigned short frameFlags = (pos[8] << 8) | pos[9];
        pos += 10;

        if (frameSize == 0 || pos + frameSize > end) break;

        // Check if this is the frame we want
        if (strcmp(id, frameId) == 0) {
            return ParseID3v2TextFrame(pos, frameSize);
        }

        pos += frameSize;
    }

    return "";
}

// Map common tag names to ID3v2 frame IDs
static const char* GetID3v2FrameId(const char* tagName) {
    if (_stricmp(tagName, "TITLE") == 0) return "TIT2";
    if (_stricmp(tagName, "ARTIST") == 0) return "TPE1";
    if (_stricmp(tagName, "ALBUM") == 0) return "TALB";
    if (_stricmp(tagName, "YEAR") == 0) return "TYER";
    if (_stricmp(tagName, "DATE") == 0) return "TDRC";
    if (_stricmp(tagName, "TRACK") == 0) return "TRCK";
    if (_stricmp(tagName, "TRACKNUMBER") == 0) return "TRCK";
    if (_stricmp(tagName, "GENRE") == 0) return "TCON";
    if (_stricmp(tagName, "COMMENT") == 0) return "COMM";
    return nullptr;
}

// Helper: Get tag string trimmed (for ID3v1 fixed-length fields)
static std::string GetTrimmedTag(const char* data, size_t maxLen) {
    if (!data) return "";

    // Find actual length (ID3v1 fields are space-padded)
    size_t len = 0;
    for (size_t i = 0; i < maxLen && data[i] != '\0'; i++) {
        len = i + 1;
    }

    // Trim trailing spaces
    while (len > 0 && (data[len - 1] == ' ' || data[len - 1] == '\0')) {
        len--;
    }

    return std::string(data, len);
}

// Helper: Parse ICY headers (format: "key:value\r\n")
static std::string GetICYTag(const char* tags, const char* key) {
    if (!tags || !key) return "";

    size_t keyLen = strlen(key);
    const char* p = tags;

    while (*p) {
        // Check if line starts with key: (case insensitive)
        if (_strnicmp(p, key, keyLen) == 0 && p[keyLen] == ':') {
            const char* value = p + keyLen + 1;
            // Skip leading spaces
            while (*value == ' ') value++;
            // Find end (newline or null)
            const char* end = value;
            while (*end && *end != '\r' && *end != '\n') end++;
            return std::string(value, end - value);
        }
        // Move to next line
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return "";
}

// Helper: Parse Shoutcast META tags (format: "StreamTitle='value';")
static std::string GetMetaTag(const char* meta, const char* key) {
    if (!meta || !key) return "";

    // Try standard Shoutcast format: key='value';
    // Note: Look for '; to handle apostrophes in titles (e.g., "Don't Call Me Up")
    std::string searchKey = std::string(key) + "='";
    const char* start = strstr(meta, searchKey.c_str());
    if (start) {
        start += searchKey.length();
        // Look for '; which marks end of field, not just ' which could be an apostrophe
        const char* end = strstr(start, "';");
        if (end) {
            return std::string(start, end - start);
        } else {
            // No semicolon - might be last field, find trailing quote
            end = start + strlen(start);
            if (end > start && *(end - 1) == '\'') {
                return std::string(start, end - start - 1);
            }
        }
    }

    // Try iHeart/alternative format: key="value" (with double quotes)
    searchKey = std::string(key) + "=\"";
    start = strstr(meta, searchKey.c_str());
    if (start) {
        start += searchKey.length();
        // Find closing quote, but handle nested quotes in url field
        const char* end = start;
        while (*end && *end != '"') end++;
        if (*end == '"') return std::string(start, end - start);
    }

    return "";
}

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
        // Extract title
        size_t titleStart = streamTitle.find("title=\"");
        if (titleStart != std::string::npos) {
            titleStart += 7;  // Skip 'title="'
            size_t titleEnd = streamTitle.find("\"", titleStart);
            if (titleEnd != std::string::npos) {
                title = streamTitle.substr(titleStart, titleEnd - titleStart);
            }
        }

        // Extract artist
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

// Helper: Try to get a tag from any available format
static std::string GetMetadataTag(HSTREAM stream, const char* tagName) {
    if (!stream) return "";

    std::string result;

    // Try OGG/Vorbis comments first (also used by FLAC, Opus)
    const char* oggTags = BASS_ChannelGetTags(stream, BASS_TAG_OGG);
    if (oggTags) {
        result = GetTagFromList(oggTags, tagName);
        if (!result.empty()) return result;
    }

    // Try APE tags
    const char* apeTags = BASS_ChannelGetTags(stream, BASS_TAG_APE);
    if (apeTags) {
        result = GetTagFromList(apeTags, tagName);
        if (!result.empty()) return result;
    }

    // Try MP4/iTunes tags
    const char* mp4Tags = BASS_ChannelGetTags(stream, BASS_TAG_MP4);
    if (mp4Tags) {
        result = GetTagFromList(mp4Tags, tagName);
        if (!result.empty()) return result;
    }

    // Try WMA tags
    const char* wmaTags = BASS_ChannelGetTags(stream, BASS_TAG_WMA);
    if (wmaTags) {
        result = GetTagFromList(wmaTags, tagName);
        if (!result.empty()) return result;
    }

    // Try RIFF INFO tags (for WAV files)
    const char* riffTags = BASS_ChannelGetTags(stream, BASS_TAG_RIFF_INFO);
    if (riffTags) {
        result = GetTagFromList(riffTags, tagName);
        if (!result.empty()) return result;
    }

    // Try Media Foundation tags
    const char* mfTags = BASS_ChannelGetTags(stream, BASS_TAG_MF);
    if (mfTags) {
        result = GetTagFromList(mfTags, tagName);
        if (!result.empty()) return result;
    }

    // Try ID3v2 tags (common for MP3 files)
    const unsigned char* id3v2 = (const unsigned char*)BASS_ChannelGetTags(stream, BASS_TAG_ID3V2);
    if (id3v2) {
        const char* frameId = GetID3v2FrameId(tagName);
        if (frameId) {
            result = GetID3v2Frame(id3v2, frameId);
            if (!result.empty()) return result;
        }
    }

    // Try ICY (Shoutcast/Icecast) headers for streams
    const char* icyTags = BASS_ChannelGetTags(stream, BASS_TAG_ICY);
    if (icyTags) {
        // Map common tag names to ICY header names
        if (_stricmp(tagName, "TITLE") == 0 || _stricmp(tagName, "ARTIST") == 0) {
            // For title/artist, check META first (has current song info)
            const char* meta = BASS_ChannelGetTags(stream, BASS_TAG_META);
            if (meta) {
                std::string streamTitle = GetMetaTag(meta, "StreamTitle");
                if (!streamTitle.empty()) {
                    std::string artist, title;
                    ParseStreamTitle(streamTitle, artist, title);
                    if (_stricmp(tagName, "TITLE") == 0 && !title.empty()) return title;
                    if (_stricmp(tagName, "ARTIST") == 0 && !artist.empty()) return artist;
                }
            }
            // Fall back to station name for title
            if (_stricmp(tagName, "TITLE") == 0) {
                result = GetICYTag(icyTags, "icy-name");
                if (!result.empty()) return result;
            }
        } else if (_stricmp(tagName, "GENRE") == 0) {
            result = GetICYTag(icyTags, "icy-genre");
            if (!result.empty()) return result;
        }
    }

    // Also try HTTP headers for streams
    const char* httpTags = BASS_ChannelGetTags(stream, BASS_TAG_HTTP);
    if (httpTags) {
        // HTTP headers use similar format to ICY
        if (_stricmp(tagName, "TITLE") == 0) {
            result = GetICYTag(httpTags, "icy-name");
            if (!result.empty()) return result;
        } else if (_stricmp(tagName, "GENRE") == 0) {
            result = GetICYTag(httpTags, "icy-genre");
            if (!result.empty()) return result;
        }
    }

    return "";
}

// Get stream title directly from META tags (for streams)
// Returns formatted "Artist - Title" or just raw stream title
static std::string GetStreamTitle(HSTREAM stream) {
    if (!stream) return "";

    const char* meta = BASS_ChannelGetTags(stream, BASS_TAG_META);
    if (meta) {
        std::string rawTitle = GetMetaTag(meta, "StreamTitle");
        if (!rawTitle.empty()) {
            // Try to parse iHeart or standard format
            std::string artist, title;
            ParseStreamTitle(rawTitle, artist, title);

            // Return formatted string
            if (!artist.empty() && !title.empty()) {
                return artist + " - " + title;
            } else if (!title.empty()) {
                return title;
            }
            // Fall back to raw title if parsing failed
            return rawTitle;
        }
    }
    return "";
}

// Get station name from ICY headers
static std::string GetStationName(HSTREAM stream) {
    if (!stream) return "";

    const char* icyTags = BASS_ChannelGetTags(stream, BASS_TAG_ICY);
    if (icyTags) {
        std::string name = GetICYTag(icyTags, "icy-name");
        if (!name.empty()) return name;
    }

    const char* httpTags = BASS_ChannelGetTags(stream, BASS_TAG_HTTP);
    if (httpTags) {
        std::string name = GetICYTag(httpTags, "icy-name");
        if (!name.empty()) return name;
    }

    return "";
}

// Get stream bitrate from ICY headers
static int GetStreamBitrate(HSTREAM stream) {
    if (!stream) return 0;

    const char* icyTags = BASS_ChannelGetTags(stream, BASS_TAG_ICY);
    if (icyTags) {
        std::string br = GetICYTag(icyTags, "icy-br");
        if (!br.empty()) return atoi(br.c_str());
    }

    const char* httpTags = BASS_ChannelGetTags(stream, BASS_TAG_HTTP);
    if (httpTags) {
        std::string br = GetICYTag(httpTags, "icy-br");
        if (!br.empty()) return atoi(br.c_str());
    }

    return 0;
}

// Get the underlying stream (before tempo processing) for tag reading
static HSTREAM GetTagStream() {
    // For tag reading, we need the original stream, not the tempo stream
    // g_stream is the original file stream, g_fxStream is the tempo-processed one
    return g_stream ? g_stream : g_fxStream;
}

void SpeakTagTitle() {
    HSTREAM stream = GetTagStream();
    if (!stream) {
        Speak("Nothing playing");
        return;
    }

    // For streams, first check if there's a full stream title (often "Artist - Title")
    std::string streamTitle = GetStreamTitle(stream);
    if (!streamTitle.empty()) {
        Speak(streamTitle);
        return;
    }

    std::string title = GetMetadataTag(stream, "TITLE");
    std::string artist = GetMetadataTag(stream, "ARTIST");

    // Try ID3v1 if nothing found
    if (title.empty() || artist.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) {
            if (title.empty()) {
                title = GetTrimmedTag(id3->title, 30);
            }
            if (artist.empty()) {
                artist = GetTrimmedTag(id3->artist, 30);
            }
        }
    }

    if (!artist.empty() && !title.empty()) {
        Speak(artist + " - " + title);
    } else if (!title.empty()) {
        Speak(title);
    } else if (!artist.empty()) {
        Speak(artist);
    } else {
        Speak("No title");
    }
}

void SpeakTagArtist() {
    HSTREAM stream = GetTagStream();
    if (!stream) {
        Speak("Nothing playing");
        return;
    }

    std::string artist = GetMetadataTag(stream, "ARTIST");

    // Try ID3v1 if nothing found
    if (artist.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) {
            artist = GetTrimmedTag(id3->artist, 30);
        }
    }

    if (!artist.empty()) {
        Speak("Artist: " + artist);
    } else {
        Speak("No artist");
    }
}

void SpeakTagAlbum() {
    HSTREAM stream = GetTagStream();
    if (!stream) {
        Speak("Nothing playing");
        return;
    }

    std::string album = GetMetadataTag(stream, "ALBUM");

    // Try ID3v1 if nothing found
    if (album.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) {
            album = GetTrimmedTag(id3->album, 30);
        }
    }

    // For streams, fall back to station name
    if (album.empty()) {
        std::string station = GetStationName(stream);
        if (!station.empty()) {
            Speak("Station: " + station);
            return;
        }
    }

    if (!album.empty()) {
        Speak("Album: " + album);
    } else {
        Speak("No album");
    }
}

void SpeakTagYear() {
    HSTREAM stream = GetTagStream();
    if (!stream) {
        Speak("Nothing playing");
        return;
    }

    std::string year = GetMetadataTag(stream, "DATE");
    if (year.empty()) year = GetMetadataTag(stream, "YEAR");

    // Try ID3v1 if nothing found
    if (year.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) {
            year = GetTrimmedTag(id3->year, 4);
        }
    }

    if (!year.empty()) {
        Speak("Year: " + year);
    } else {
        Speak("No year");
    }
}

void SpeakTagTrack() {
    HSTREAM stream = GetTagStream();
    if (!stream) {
        Speak("Nothing playing");
        return;
    }

    std::string track = GetMetadataTag(stream, "TRACKNUMBER");
    if (track.empty()) track = GetMetadataTag(stream, "TRACK");

    // Try ID3v1.1 track number (stored in comment field)
    if (track.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3 && id3->comment[28] == '\0' && id3->comment[29] != 0) {
            // ID3v1.1 format: comment[28] is 0, comment[29] is track number
            int trackNum = (unsigned char)id3->comment[29];
            if (trackNum > 0) {
                track = std::to_string(trackNum);
            }
        }
    }

    if (!track.empty()) {
        Speak("Track: " + track);
    } else {
        Speak("No track number");
    }
}

void SpeakTagGenre() {
    HSTREAM stream = GetTagStream();
    if (!stream) {
        Speak("Nothing playing");
        return;
    }

    std::string genre = GetMetadataTag(stream, "GENRE");

    // Try ID3v1 if nothing found
    if (genre.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3 && id3->genre < g_id3GenreCount) {
            genre = g_id3Genres[id3->genre];
        }
    }

    if (!genre.empty()) {
        Speak("Genre: " + genre);
    } else {
        Speak("No genre");
    }
}

void SpeakTagComment() {
    HSTREAM stream = GetTagStream();
    if (!stream) {
        Speak("Nothing playing");
        return;
    }

    std::string comment = GetMetadataTag(stream, "COMMENT");
    if (comment.empty()) comment = GetMetadataTag(stream, "DESCRIPTION");

    // Try ID3v1 if nothing found
    if (comment.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) {
            // Check for ID3v1.1 (comment is only 28 chars if track is present)
            if (id3->comment[28] == '\0' && id3->comment[29] != 0) {
                comment = GetTrimmedTag(id3->comment, 28);
            } else {
                comment = GetTrimmedTag(id3->comment, 30);
            }
        }
    }

    if (!comment.empty()) {
        Speak("Comment: " + comment);
    } else {
        Speak("No comment");
    }
}

void SpeakTagBitrate() {
    if (!g_fxStream) {
        Speak("Nothing playing");
        return;
    }

    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(g_fxStream, &info)) {
        Speak("Cannot get info");
        return;
    }

    HSTREAM sourceStream = g_stream ? g_stream : g_fxStream;

    // Get bitrate if available (for compressed formats)
    float bitrate = 0;
    BASS_ChannelGetAttribute(sourceStream, BASS_ATTRIB_BITRATE, &bitrate);

    // If no bitrate from attribute, try ICY headers (for streams)
    if (bitrate <= 0) {
        int icyBitrate = GetStreamBitrate(sourceStream);
        if (icyBitrate > 0) bitrate = (float)icyBitrate;
    }

    char buf[128];
    if (bitrate > 0) {
        snprintf(buf, sizeof(buf), "%d kbps, %d Hz, %s",
                 (int)bitrate, info.freq,
                 info.chans == 1 ? "mono" : (info.chans == 2 ? "stereo" : "multi-channel"));
    } else {
        // Probably a lossless format
        int bits = (info.flags & BASS_SAMPLE_8BITS) ? 8 :
                   (info.flags & BASS_SAMPLE_FLOAT) ? 32 : 16;
        snprintf(buf, sizeof(buf), "%d-bit, %d Hz, %s",
                 bits, info.freq,
                 info.chans == 1 ? "mono" : (info.chans == 2 ? "stereo" : "multi-channel"));
    }

    Speak(buf);
}

int GetCurrentBitrate() {
    // Try to get live bitrate from source stream (updates for VBR files)
    // g_sourceStream is the original decode stream before tempo processing
    if (g_sourceStream) {
        float bitrate = 0;
        if (BASS_ChannelGetAttribute(g_sourceStream, BASS_ATTRIB_BITRATE, &bitrate) && bitrate > 0) {
            return static_cast<int>(bitrate);
        }
    }

    // Fall back to cached bitrate (captured at load time)
    if (g_currentBitrate > 0) return g_currentBitrate;

    // Fall back to ICY headers for internet streams
    if (g_sourceStream) {
        int icyBitrate = GetStreamBitrate(g_sourceStream);
        if (icyBitrate > 0) return icyBitrate;
    }

    return 0;
}

void SpeakTagDuration() {
    if (!g_fxStream) {
        Speak("Nothing playing");
        return;
    }

    TempoProcessor* processor = GetTempoProcessor();
    double length = 0;
    if (processor && processor->IsActive()) {
        length = processor->GetLength();
    }

    if (length <= 0) {
        // Check if it's a stream (URL)
        if (g_currentTrack >= 0 && g_currentTrack < (int)g_playlist.size()) {
            if (IsURL(g_playlist[g_currentTrack].c_str())) {
                Speak("Live stream");
                return;
            }
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

    // Check if it's a URL
    if (IsURL(path.c_str())) {
        // For streams, show the full URL
        char buf[1024];
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
        Speak(std::string("URL: ") + buf);
        return;
    }

    // Get just the filename
    const wchar_t* lastSlash = wcsrchr(path.c_str(), L'\\');
    if (!lastSlash) lastSlash = wcsrchr(path.c_str(), L'/');

    std::wstring filename = lastSlash ? (lastSlash + 1) : path;

    // Convert to narrow string for speech
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, filename.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);

    Speak(std::string("Filename: ") + buf);
}

// Tag retrieval functions for display in dialog
std::wstring GetTagTitle() {
    HSTREAM stream = GetTagStream();
    if (!stream) return L"Nothing playing";

    std::string streamTitle = GetStreamTitle(stream);
    if (!streamTitle.empty()) {
        return Utf8ToWide(streamTitle);
    }

    std::string title = GetMetadataTag(stream, "TITLE");
    std::string artist = GetMetadataTag(stream, "ARTIST");

    if (title.empty() || artist.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) {
            if (title.empty()) title = GetTrimmedTag(id3->title, 30);
            if (artist.empty()) artist = GetTrimmedTag(id3->artist, 30);
        }
    }

    if (!artist.empty() && !title.empty()) {
        return Utf8ToWide(artist + " - " + title);
    } else if (!title.empty()) {
        return Utf8ToWide(title);
    } else if (!artist.empty()) {
        return Utf8ToWide(artist);
    }
    return L"No title";
}

std::wstring GetTagArtist() {
    HSTREAM stream = GetTagStream();
    if (!stream) return L"Nothing playing";

    std::string artist = GetMetadataTag(stream, "ARTIST");
    if (artist.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) artist = GetTrimmedTag(id3->artist, 30);
    }
    return artist.empty() ? L"No artist" : Utf8ToWide(artist);
}

std::wstring GetTagAlbum() {
    HSTREAM stream = GetTagStream();
    if (!stream) return L"Nothing playing";

    std::string album = GetMetadataTag(stream, "ALBUM");
    if (album.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) album = GetTrimmedTag(id3->album, 30);
    }
    if (album.empty()) {
        std::string station = GetStationName(stream);
        if (!station.empty()) return Utf8ToWide(station);
    }
    return album.empty() ? L"No album" : Utf8ToWide(album);
}

std::wstring GetTagYear() {
    HSTREAM stream = GetTagStream();
    if (!stream) return L"Nothing playing";

    std::string year = GetMetadataTag(stream, "DATE");
    if (year.empty()) year = GetMetadataTag(stream, "YEAR");
    if (year.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) year = GetTrimmedTag(id3->year, 4);
    }
    return year.empty() ? L"No year" : Utf8ToWide(year);
}

std::wstring GetTagTrack() {
    HSTREAM stream = GetTagStream();
    if (!stream) return L"Nothing playing";

    std::string track = GetMetadataTag(stream, "TRACKNUMBER");
    if (track.empty()) track = GetMetadataTag(stream, "TRACK");
    if (track.empty()) {
        // Try ID3v1.1 track number (stored in comment field)
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3 && id3->comment[28] == '\0' && id3->comment[29] != 0) {
            int trackNum = (unsigned char)id3->comment[29];
            if (trackNum > 0) track = std::to_string(trackNum);
        }
    }
    return track.empty() ? L"No track" : Utf8ToWide(track);
}

std::wstring GetTagGenre() {
    HSTREAM stream = GetTagStream();
    if (!stream) return L"Nothing playing";

    std::string genre = GetMetadataTag(stream, "GENRE");
    if (genre.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3 && id3->genre < 192) {
            // ID3v1 genre lookup (simplified - first few common ones)
            static const char* genres[] = {
                "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
                "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
                "Rap", "Reggae", "Rock", "Techno", "Industrial"
            };
            if (id3->genre < 20) genre = genres[id3->genre];
            else genre = std::to_string(id3->genre);
        }
    }
    return genre.empty() ? L"No genre" : Utf8ToWide(genre);
}

std::wstring GetTagComment() {
    HSTREAM stream = GetTagStream();
    if (!stream) return L"Nothing playing";

    std::string comment = GetMetadataTag(stream, "COMMENT");
    if (comment.empty()) comment = GetMetadataTag(stream, "DESCRIPTION");
    if (comment.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) comment = GetTrimmedTag(id3->comment, 30);
    }
    return comment.empty() ? L"No comment" : Utf8ToWide(comment);
}

std::wstring GetTagBitrate() {
    HSTREAM stream = GetTagStream();
    if (!stream) return L"Nothing playing";

    BASS_CHANNELINFO info;
    BASS_ChannelGetInfo(stream, &info);

    float bitrate = 0;
    BASS_ChannelGetAttribute(stream, BASS_ATTRIB_BITRATE, &bitrate);

    if (bitrate > 0) {
        wchar_t buf[64];
        swprintf(buf, 64, L"%.0f kbps, %d Hz, %s",
            bitrate, info.freq,
            info.chans == 1 ? L"Mono" : (info.chans == 2 ? L"Stereo" : L"Multi-channel"));
        return buf;
    }
    return L"Unknown bitrate";
}

std::wstring GetTagDuration() {
    HSTREAM stream = GetTagStream();
    if (!stream) return L"Nothing playing";

    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) {
        QWORD len = BASS_ChannelGetLength(stream, BASS_POS_BYTE);
        if (len == (QWORD)-1) return L"Unknown duration";
        double duration = BASS_ChannelBytes2Seconds(stream, len);
        return FormatTime(duration);
    }
    return FormatTime(processor->GetLength());
}

std::wstring GetTagFilename() {
    if (g_currentTrack < 0 || g_currentTrack >= (int)g_playlist.size()) {
        return L"Nothing playing";
    }

    std::wstring path = g_playlist[g_currentTrack];
    if (IsURL(path.c_str())) return path;

    const wchar_t* lastSlash = wcsrchr(path.c_str(), L'\\');
    if (!lastSlash) lastSlash = wcsrchr(path.c_str(), L'/');
    return lastSlash ? (lastSlash + 1) : path;
}

// Generate output filename based on template
static std::wstring GenerateRecordingFilename() {
    // Get current time
    time_t now = time(nullptr);
    struct tm localTime;
    localtime_s(&localTime, &now);

    // Format using the template
    wchar_t buffer[256];
    wcsftime(buffer, 256, g_recordTemplate.c_str(), &localTime);

    // Add appropriate extension
    const wchar_t* ext;
    switch (g_recordFormat) {
        case 1: ext = L".mp3"; break;
        case 2: ext = L".ogg"; break;
        case 3: ext = L".flac"; break;
        default: ext = L".wav"; break;
    }

    std::wstring filename = buffer;
    filename += ext;

    return filename;
}

// Stop recording
void StopRecording() {
    if (!g_isRecording || !g_encoder) return;

    BASS_Encode_Stop(g_encoder);
    g_encoder = 0;
    g_isRecording = false;

    Speak("Recording stopped");
    UpdateStatusBar();
}

// Toggle recording on/off
void ToggleRecording() {
    // If already recording, stop
    if (g_isRecording) {
        StopRecording();
        return;
    }

    // Need a stream to record from
    if (!g_fxStream) {
        Speak("Nothing to record");
        return;
    }

    // Determine output path
    std::wstring outputPath;
    if (g_recordPath.empty()) {
        // Default to Music folder
        wchar_t musicPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYMUSIC, nullptr, 0, musicPath))) {
            outputPath = musicPath;
        } else {
            // Fall back to current directory
            wchar_t currentDir[MAX_PATH];
            GetCurrentDirectoryW(MAX_PATH, currentDir);
            outputPath = currentDir;
        }
    } else {
        outputPath = g_recordPath;
    }

    // Ensure output directory exists
    CreateDirectoryW(outputPath.c_str(), nullptr);

    // Generate filename
    std::wstring filename = GenerateRecordingFilename();
    std::wstring fullPath = outputPath;
    if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/') {
        fullPath += L'\\';
    }
    fullPath += filename;

    // Get stream info for encoder setup
    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(g_fxStream, &info)) {
        Speak("Cannot get stream info");
        return;
    }

    // Start appropriate encoder based on format
    // Note: BASS_ENCODE_FP_16BIT converts floating-point audio to 16-bit integer
    // which is required for WAV and FLAC encoders
    DWORD wavFlags = BASS_ENCODE_AUTOFREE | BASS_ENCODE_FP_16BIT;

    switch (g_recordFormat) {
        case 0: {
            // WAV - use BASS_Encode_StartPCMFile for direct WAV output
            g_encoder = BASS_Encode_StartPCMFile(g_fxStream, wavFlags, fullPath.c_str());
            break;
        }
        case 1: {
            // MP3 - use bassenc_mp3
            wchar_t options[64];
            swprintf(options, 64, L"--preset cbr %d", g_recordBitrate);
            g_encoder = BASS_Encode_MP3_StartFile(g_fxStream, options, BASS_ENCODE_AUTOFREE, fullPath.c_str());
            if (!g_encoder) {
                // Fall back to WAV if MP3 encoding fails
                MessageBoxW(g_hwnd, L"MP3 encoding failed.\nFalling back to WAV format.",
                            APP_NAME, MB_ICONWARNING);
                fullPath = outputPath;
                if (!fullPath.empty() && fullPath.back() != L'\\') fullPath += L'\\';
                fullPath += GenerateRecordingFilename();
                g_encoder = BASS_Encode_StartPCMFile(g_fxStream, wavFlags, fullPath.c_str());
            }
            break;
        }
        case 2: {
            // OGG - use bassenc_ogg
            wchar_t options[64];
            swprintf(options, 64, L"--bitrate %d", g_recordBitrate);
            g_encoder = BASS_Encode_OGG_StartFile(g_fxStream, options, BASS_ENCODE_AUTOFREE, fullPath.c_str());
            if (!g_encoder) {
                // Fall back to WAV if OGG encoding fails
                MessageBoxW(g_hwnd, L"OGG encoding failed.\nFalling back to WAV format.",
                            APP_NAME, MB_ICONWARNING);
                fullPath = outputPath;
                if (!fullPath.empty() && fullPath.back() != L'\\') fullPath += L'\\';
                fullPath += GenerateRecordingFilename();
                g_encoder = BASS_Encode_StartPCMFile(g_fxStream, wavFlags, fullPath.c_str());
            }
            break;
        }
        case 3: {
            // FLAC - use bassenc_flac (also needs FP conversion)
            g_encoder = BASS_Encode_FLAC_StartFile(g_fxStream, nullptr, wavFlags, fullPath.c_str());
            if (!g_encoder) {
                // Fall back to WAV if FLAC encoding fails
                MessageBoxW(g_hwnd, L"FLAC encoding failed.\nFalling back to WAV format.",
                            APP_NAME, MB_ICONWARNING);
                fullPath = outputPath;
                if (!fullPath.empty() && fullPath.back() != L'\\') fullPath += L'\\';
                fullPath += GenerateRecordingFilename();
                g_encoder = BASS_Encode_StartPCMFile(g_fxStream, wavFlags, fullPath.c_str());
            }
            break;
        }
    }

    if (!g_encoder) {
        int err = BASS_ErrorGetCode();
        wchar_t msg[256];
        swprintf(msg, 256, L"Failed to start recording (error %d)", err);
        MessageBoxW(g_hwnd, msg, APP_NAME, MB_ICONERROR);
        return;
    }

    g_isRecording = true;
    Speak("Recording started");
    UpdateStatusBar();
}
