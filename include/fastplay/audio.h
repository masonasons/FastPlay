#pragma once
#ifndef FASTPLAY_AUDIO_H
#define FASTPLAY_AUDIO_H

// FastPlay's audio engine.
//
// A Decoder turns a file or stream into stereo float PCM: FFmpeg for nearly every
// format and for internet streams, FDK AAC for xHE-AAC. The engine plays one
// decoder at a time through a pipeline of three threads:
//   - the decode thread reads the decoder ahead into a buffer, so a slow network
//     never stalls playback;
//   - the mix thread runs the tempo processor (tempo, pitch and rate) and fills
//     the output buffer;
//   - miniaudio's device callback takes from the output buffer and runs the
//     effects chain and the recording tap on exactly what is heard, then applies
//     the volume last, so it answers at once and recordings are made at full volume.
// Control functions are called from the UI thread. The end of a track and a new
// stream title are reported back on the UI thread.

#include "types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace audio {

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

class Decoder {
public:
    virtual ~Decoder() = default;

    // The rate of the PCM Read() returns. Always two channels, interleaved.
    virtual int SampleRate() const = 0;
    // Up to `frames` frames into `out`; 0 at the end. May block (network).
    virtual int Read(float* out, int frames) = 0;
    // To `seconds`; the next Read() starts there. False if it cannot seek.
    virtual bool Seek(double seconds) = 0;
    // Asks a blocking Read() or Seek() to give up (from another thread).
    virtual void Abort() = 0;

    // Seconds, or 0 when unknown (a live stream).
    virtual double Length() const = 0;
    // A live stream: no length, no seeking, no pausing.
    virtual bool IsLive() const = 0;

    // What is known about the source. Safe to call from any thread.
    // A tag by its common name (TITLE, ARTIST, ALBUM, DATE, TRACK, GENRE, COMMENT,
    // REPLAYGAIN_TRACK_GAIN...), or a stream header (icy-name, icy-genre, icy-br).
    virtual std::string Tag(const std::string& name) const = 0;
    // The current title of an internet radio stream (its StreamTitle), if any.
    virtual std::string StreamTitle() const = 0;
    virtual std::vector<Chapter> Chapters() const = 0;
    // The bitrate in kbps: the recent average of a VBR file, else the nominal one.
    virtual int Bitrate() const = 0;
    virtual bool IsVbr() const = 0;
    // The source's own format, as it was before the conversion to stereo float.
    virtual int SourceChannels() const = 0;
    virtual int SourceSampleRate() const = 0;
    virtual int SourceBits() const = 0;  // 0 for a lossy format
    virtual std::string CodecName() const = 0;

    // A live stream kept for rewinding (SetLiveRewindSeconds): the seconds of it
    // held, in its own time (from 0 where it was opened), for Seek(): from the
    // oldest to as near the newest as plays without running out before more
    // arrives. False if not kept.
    virtual bool Rewindable(double& start, double& end) const {
        (void)start;
        (void)end;
        return false;
    }
    // Such a stream has played all it has so far: Read() would come back empty,
    // not because it ended, but because the rest has not arrived yet.
    virtual bool Starved() const { return false; }
};

// Live streams opened from now on keep their last `seconds` for rewinding (0: not
// kept). What is kept is the stream as it came (compressed), about a megabyte a
// minute at 128 kbps.
void SetLiveRewindSeconds(int seconds);

// Opens a file or URL. Null with `error` set if it cannot be played.
std::unique_ptr<Decoder> OpenDecoder(const std::wstring& pathOrUrl, std::wstring& error);

// Decodes a whole file (an impulse response) to interleaved float at its own rate
// and channel count.
bool DecodeWholeFile(const std::wstring& path, std::vector<float>& samples, int& channels, int& sampleRate,
                     std::wstring& error);

// The version of the decoding library, for Help > Audio Engine.
std::string DecoderVersion();

// A file's tags and length, read as quickly as can be (the header only, nothing
// decoded), for the library. Empty where the file has none.
struct FileTags {
    std::string title, artist, album, albumArtist, genre;  // UTF-8
    int year = 0, track = 0, disc = 0;
    double duration = 0;
};
bool ReadFileTags(const std::wstring& path, FileTags& tags);

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

struct Device {
    std::wstring name;
    bool isDefault = false;
};

// The playback devices, in the system's order.
std::vector<Device> ListDevices();

// Opens the named device (empty, or not found: the system default). On iOS the
// device stays idle until Play(), and pauses with playback. The output
// buffer holds `bufferMs` of audio: what effects and tempo changes lag behind.
bool Init(const std::wstring& deviceName, int bufferMs);
void Shutdown();
// Moves to another device. Only while nothing is loaded.
bool SwitchDevice(const std::wstring& deviceName, int bufferMs);
// Starts the device again if something stopped it: on iOS, pausing playback or
// another app taking the sound. Play() does this itself. `beforeStart`, if set,
// is called first (the phone's audio session must be active by then).
// False if the device would not start.
bool EnsureDeviceRunning();
void SetBeforeDeviceStart(void (*beforeStart)());
// The device in use ("" before Init), and whether it is the system default.
std::wstring CurrentDeviceName();
bool UsingDefaultDevice();

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

enum class State { Empty, Playing, Paused, Stopped };

// Makes `decoder` the one playing (paused until Play()), through the given tempo
// algorithm. Whatever was loaded is unloaded first.
bool Load(std::unique_ptr<Decoder> decoder, TempoAlgorithm algorithm);
void Unload();
bool IsLoaded();
// The loaded decoder, for its tags and format (null when nothing is loaded).
const Decoder* Current();

void Play();
void Pause();
// Silent and marked stopped (the player seeks back to the start)
void Stop();
State GetState();

// Seconds into the source of what is being heard now.
double Position();
double Length();
bool IsLive();
// To `seconds` into the source; what was buffered is dropped. Ends scrubbing.
// A live stream kept for rewinding seeks within LiveRange().
bool Seek(double seconds);
// A live stream kept for rewinding: where it can be played from, in Position()'s
// time: `start`, the oldest kept, to `live`, the live edge less the little a
// stream keeps in hand. False for anything else.
bool LiveRange(double& start, double& live);

// Scrubbing: playing through the audio at speed, forward (direction 1) or
// backward (-1), from what is heard now, while a seek key is held. Tape plays it
// faster, pitch and all, at `speed` times normal once spun up; spring keeps the
// pitch and winds up the longer it goes, up to `speed`. StopScrub() carries on
// playing normally from wherever scrubbing got to. Not for live streams,
// unless kept for rewinding.
enum class ScrubStyle { Tape, Spring };
bool StartScrub(ScrubStyle style, int direction, float speed);
void SetScrubSpeed(float speed);
bool StopScrub();
bool IsScrubbing();
// Tape let go of: it winds back down (to normal speed going forward, to a stop
// going back), then the scrub end handler is called. False for spring, which
// stops at once (call StopScrub()).
bool ReleaseScrub();
// The tape stop effect: from normal speed down to a standstill, then the scrub
// end handler is called (to pause or stop for real). False if it cannot be done
// here (a live stream not kept for rewinding).
bool TapeStop();
// Called on the UI thread when a release or tape stop has wound down.
void SetScrubEndHandler(std::function<void()> handler);

// Tempo in percent (0 = normal), pitch in semitones, rate as a multiplier that
// changes speed and pitch together.
void SetTempo(float percent);
void SetPitch(float semitones);
void SetRate(float rate);

// The output gain (volume, ReplayGain and mute together), applied last.
void SetGain(float linear);

// Smooth seeking: an 8 ms fade out before a seek, pause, stop or track change,
// and in when sound starts again, so none of them click. Off: instant, as cut.
void SetSmoothTransitions(bool smooth);

// The effects chain: `proc` is called on the device callback with each block of stereo
// audio, in priority order (higher first). Returns an id for RemoveDsp().
using DspProc = void (*)(float* samples, int frames, int channels, int sampleRate, void* user);
int AddDsp(DspProc proc, void* user, int priority);
void RemoveDsp(int id);

// The recording tap: every block after the effects, before the volume. Null stops.
using TapProc = void (*)(const float* samples, int frames, int channels, int sampleRate, void* user);
void SetTap(TapProc proc, void* user);
// Where the tap is: after the effects (the default), or before them, so a
// recording has the sound without the effects (tempo, pitch and rate, which come
// before the effects chain, are still in it).
void SetTapBeforeEffects(bool before);
// The rate the effects and the tap see (the device's).
int MixSampleRate();

// How playback has been going since Init(), for Help > Audio Engine: underruns (the
// device found the output buffer empty while playing), the lowest the buffer got,
// the slowest the effects took on a block, and the device's period.
struct Stats {
    uint64_t underruns = 0;
    double minBufferedMs = 0;
    double maxBlockMs = 0;
    int periodFrames = 0;
    int bufferFrames = 0;
};
Stats GetStats();
void ResetStats();

// Called on the UI thread: when the loaded track has played to its end, and when
// a stream's title changes.
void SetEndHandler(std::function<void()> handler);
void SetStreamTitleHandler(std::function<void()> handler);

}  // namespace audio

#endif  // FASTPLAY_AUDIO_H
