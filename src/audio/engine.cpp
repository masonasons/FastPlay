// The audio engine: see audio.h.
//
// Threads and what they own:
//   - decode thread: the decoder. Reads ahead into m_pcm (stereo float at the
//     source's rate) and carries out seeks.
//   - mix thread: the tempo processor. Fills the output ring (stereo float at the
//     device's rate) while it has room.
//   - device callback (miniaudio): takes from the output ring, counts the frames
//     played (the position), fades, runs the effects and the tap on what is
//     heard, applies the gain, and notices the end.
// A seek stops the mix thread at a safe point (m_mixMutex), has the decode thread
// reposition the decoder (or, into what it decoded lately, its SourceCache),
// restarts the tempo processor and empties the ring.
// Scrubbing is a seek too: to where it starts, with the scrubber filling the ring
// instead of the tempo processor, and backward, the decode thread reading the
// source backward.

#include "audio.h"
#include "audio_internal.h"
#include "app_ui.h"
#include "scrubber.h"
#include "tempo_processor.h"
#include "utils.h"

#include "miniaudio.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace audio {
namespace {

const int kChannels = 2;
const int kMixBlockFrames = 512;
const int kDevicePeriodMs = 10;
// Decoded audio read ahead: a little for files (enough for scrubbing at speed),
// several seconds for streams
const double kFileReadAhead = 8.0;
const double kStreamReadAhead = 10.0;
// A stream starts (and restarts after running dry) once this much is buffered
const double kStreamPrebuffer = 2.0;
// A file's decoded audio kept around where it is playing (SourceCache)
const double kSourceCacheSeconds = 30.0;
// The tape stop and start (ScrubTiming has how long they take): slower than
// kTapeStopped is a standstill, silence; below kTapeQuiet it fades, so the last
// rumble does not end in a thump, nor the first begin with one.
const double kTapeStopped = 0.01;
const double kTapeQuiet = 0.15;

// ---------------------------------------------------------------------------
// The decoded audio buffered between the decode and mix threads
// ---------------------------------------------------------------------------

class PcmBuffer : public PcmSource {
public:
    void Reset(size_t capacityFrames) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_data.assign(capacityFrames * kChannels, 0.0f);
        m_read = m_write = m_count = 0;
        m_ended = false;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_read = m_write = m_count = 0;
        m_ended = false;
    }

    size_t Space() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_data.size() / kChannels - m_count;
    }

    void Write(const float* in, size_t frames) {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t capacity = m_data.size() / kChannels;
        frames = std::min(frames, capacity - m_count);
        for (size_t i = 0; i < frames; i++) {
            m_data[m_write * kChannels] = in[i * kChannels];
            m_data[m_write * kChannels + 1] = in[i * kChannels + 1];
            m_write = (m_write + 1) % capacity;
        }
        m_count += frames;
    }

    void SetEnded() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ended = true;
    }

    int Available() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<int>(m_count);
    }

    int Read(float* out, int frames) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t capacity = m_data.size() / kChannels;
        size_t n = std::min<size_t>(static_cast<size_t>(frames), m_count);
        for (size_t i = 0; i < n; i++) {
            out[i * kChannels] = m_data[m_read * kChannels];
            out[i * kChannels + 1] = m_data[m_read * kChannels + 1];
            m_read = (m_read + 1) % capacity;
        }
        m_count -= n;
        return static_cast<int>(n);
    }

    bool Ended() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_ended && m_count == 0;
    }

    bool DecoderEnded() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_ended;
    }

private:
    std::mutex m_mutex;
    std::vector<float> m_data;
    size_t m_read = 0, m_write = 0, m_count = 0;
    bool m_ended = false;
};

// ---------------------------------------------------------------------------
// The source as lately decoded, kept so that a seek into it - scrubbing's start
// and end, a tape let go of, a short jump back - plays from memory at once rather
// than waiting on the decoder to seek (worst on a CD, whose drive has to move
// back). One unbroken stretch of the source, [Start(), End()), forward in time,
// up to its most frames: what is decoded going forward is added at the end, and
// reading backward (scrubbing back), at the start. A file's only (a live stream
// has its own buffer for rewinding). The decode thread's, under decoderMutex.
// ---------------------------------------------------------------------------

class SourceCache {
public:
    void Reset(size_t maxFrames) {
        std::deque<float>().swap(m_data);
        m_start = 0;
        m_max = maxFrames;
    }

    int64_t Start() const { return m_start; }
    int64_t End() const { return m_start + static_cast<int64_t>(Frames()); }
    bool Has(int64_t frame) const { return frame >= m_start && frame < End(); }
    bool Covers(int64_t from, int64_t to) const { return Frames() > 0 && from >= m_start && to <= End(); }

    // `frames` decoded at source frame `at`: what is new of them joins the stretch
    // if they meet or overlap it, else they start a new one
    void Add(int64_t at, const float* in, size_t frames) {
        if (m_max == 0 || frames == 0) return;
        const int64_t to = at + static_cast<int64_t>(frames);
        if (Frames() == 0 || at > End() || to < m_start) {
            m_data.clear();
            m_start = at;
        }
        if (at < m_start) {
            // Leading up to it: the frames before its start, last first
            for (int64_t f = std::min(to, m_start) - 1; f >= at; f--) {
                const size_t i = static_cast<size_t>(f - at) * kChannels;
                m_data.push_front(in[i + 1]);
                m_data.push_front(in[i]);
            }
            m_start = at;
            while (Frames() > m_max) m_data.resize(m_data.size() - kChannels);
        }
        if (to > End()) {
            for (int64_t f = std::max(at, End()); f < to; f++) {
                const size_t i = static_cast<size_t>(f - at) * kChannels;
                m_data.push_back(in[i]);
                m_data.push_back(in[i + 1]);
            }
            while (Frames() > m_max) {
                m_data.erase(m_data.begin(), m_data.begin() + kChannels);
                m_start++;
            }
        }
    }

    // Up to `frames` of it from source frame `from`; how many it had
    size_t Copy(int64_t from, float* out, size_t frames) const {
        if (!Has(from)) return 0;
        const size_t n = std::min(frames, static_cast<size_t>(End() - from));
        auto it = m_data.begin() + static_cast<std::ptrdiff_t>((from - m_start) * kChannels);
        std::copy(it, it + static_cast<std::ptrdiff_t>(n * kChannels), out);
        return n;
    }

private:
    size_t Frames() const { return m_data.size() / kChannels; }

    std::deque<float> m_data;  // interleaved
    int64_t m_start = 0;
    size_t m_max = 0;
};

struct Dsp {
    int id;
    DspProc proc;
    void* user;
    int priority;
};

}  // namespace

// ---------------------------------------------------------------------------
// A player's state
// ---------------------------------------------------------------------------

struct Player::Impl {
    // Device
    ma_context* context = nullptr;  // shared by every player (AcquireContext())
    ma_device device;
    bool deviceReady = false;
    std::wstring deviceName;
    bool defaultDevice = true;
    int outputRate = 48000;
    size_t ringFrames = 0;

    // The output ring: written by the mix thread, read by the device callback.
    // m_ringMutex is only ever tried by the callback, so it never waits.
    ma_pcm_rb ring;
    bool ringReady = false;
    std::mutex ringMutex;

    // What is loaded
    std::unique_ptr<Decoder> decoder;
    std::unique_ptr<TempoProcessor> processor;
    PcmBuffer pcm;
    std::atomic<bool> loaded{false};
    std::atomic<State> state{State::Empty};
    std::atomic<bool> live{false};
    double length = 0;
    size_t prebufferFrames = 0;

    // Threads
    std::thread decodeThread, mixThread;
    std::atomic<bool> running{false};
    std::mutex decodeMutex;
    std::condition_variable decodeWake;
    std::mutex decoderMutex;       // held while the decoder is in use, and to replace it
    bool seekRequested = false;
    double seekTarget = 0;
    bool seekDone = false;
    bool seekOk = false;
    std::condition_variable seekFinished;
    std::mutex mixMutex;           // held by the mix thread while it works
    std::mutex mixWakeMutex;
    std::condition_variable mixWake;  // the device took audio: there is room again
    std::atomic<bool> mixWaiting{true};  // a stream waiting for its prebuffer
    std::atomic<bool> producerEnded{false};

    // Position: frames played from the ring since the last seek
    std::atomic<uint64_t> played{0};
    std::atomic<bool> endReported{false};

    // Gain, applied in the callback (ramped from block to block)
    std::atomic<float> gain{1.0f};
    float appliedGain = 1.0f;

    // Short fades, as BASS made by default: out before pausing, stopping, seeking
    // or unloading, and in whenever sound starts again, so none of them click or
    // cut a reverb or echo off dead.
    std::atomic<bool> fadeOut{false};   // asked for by the UI thread
    std::atomic<bool> fadeOutput{false};  // ...the effects' output too (pausing)
    std::atomic<bool> fadedOut{false};  // the callback has reached silence
    float fadeLevel = 0.0f;             // the callback's own: into the effects
    float outputLevel = 1.0f;           // and out of them
    // After a seek, play only once a couple of periods are in hand, so the device
    // never takes half a block and cuts it off.
    std::atomic<bool> primed{false};
    std::atomic<float> fadeStep{1.0f / 384.0f};  // per frame: 8 ms; 1 when off

    // Tempo settings, kept for the processor
    float tempo = 0, pitch = 0, rate = 1;

    // Scrubbing: the scrubber (UI thread and mix thread, under mixMutex), and the
    // decode thread reading backward (under feedMutex): a chunk at a time,
    // each ending where the last began
    std::unique_ptr<Scrubber> scrub;
    int sourceRate = 44100;
    bool seekReverse = false;        // with seekRequested: read backward from there
    int seekChunkFrames = 0;         // ...in chunks this long
    int64_t seekFloorFrames = 0;     // ...back to here
    bool reverse = false;
    int64_t reverseEnd = 0;          // the source frame the next chunk ends at
    int reverseChunkFrames = 0;
    int64_t reverseFloor = 0;        // where reading backward stops (a frame of the source)
    std::vector<float> reverseTail;  // the overlap held back to crossfade with the next chunk

    // What goes into the buffer (pcm), under feedMutex: which way (reverse, and the
    // reading backward above), a file's decoded audio kept around where it plays,
    // the next frame forward into the buffer, and the source's end once the decoder
    // has met it. A seek into the cache, or any seek backward (which reads nothing
    // yet), is made here at once, without waiting for the decode thread, whose
    // reading meanwhile is kept in the cache but not put in the buffer: every seek
    // moves feedGeneration on, and what was read for an older one is not used.
    // Taken after decoderMutex, never before it.
    std::mutex feedMutex;
    uint64_t feedGeneration = 0;
    SourceCache cache;
    int64_t feedFrame = 0;
    int64_t endFrame = -1;
    int64_t decoderFrame = 0;  // the next frame the decoder would read (under decoderMutex)

    // The tape stop and start: the device takes what is already in the ring slower
    // and slower, pitch and all, down to a standstill (or faster and faster up from
    // one), so it starts the moment it is asked for. tapeRamp and tapeRising are set
    // and cleared under ringMutex (which the callback holds while it plays it); the
    // rest is the callback's, set up by TapeStop() and TapeStart() under ringMutex.
    ScrubTiming scrubTiming;  // (UI thread)
    std::atomic<bool> tapeRamp{false};
    std::atomic<bool> tapeRising{false};  // a tape start: up to normal speed
    double tapeFall = 0, tapeRise = 0;    // the speed lost or gained each frame
    bool tapeStarted = false;       // a and b hold the first two frames
    bool tapeStandstill = false;    // stopped, and the scrub end handler called
    double tapeSpeed = 1.0;         // the speed now
    double tapePos = 0;             // between tapeA and tapeB
    float tapeA[kChannels] = {}, tapeB[kChannels] = {};
    float tapeLevel = 1.0f;         // how loud it last was (it fades on the way to a standstill)
    bool tapeHeard = false;         // the last block was the tape's
    std::vector<float> tapeIn;      // the frames a block takes from the ring

    // Playing in reverse (UI thread, under mixMutex): the source read backward
    // into the usual tempo processor and effects, from reverseFrom
    bool playReverse = false;
    double reverseFrom = 0;

    // Effects chain and tap
    std::mutex dspMutex;
    std::vector<Dsp> dsps;
    int nextDspId = 1;
    TapProc tap = nullptr;
    void* tapUser = nullptr;
    bool tapBeforeEffects = false;
    std::vector<float> mixBlock;

    // Stats
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint32_t> minBuffered{0xFFFFFFFF};
    std::atomic<double> maxBlockMs{0};
    std::atomic<uint32_t> lastPeriod{0};

    // For tests (audio_internal.h)
    std::atomic<TapProc> monitor{nullptr};
    void* monitorUser = nullptr;

    // UI handlers
    std::function<void()> endHandler;
    std::function<void()> titleHandler;
    std::function<void()> scrubEndHandler;
};

namespace {

using Engine = Player::Impl;

// ---------------------------------------------------------------------------
// The audio system: one context for every player, made when the first needs it
// ---------------------------------------------------------------------------

bool g_iosSessionPlayback = true;
std::mutex g_contextMutex;
ma_context g_context;
int g_contextUsers = 0;

// FASTPLAY_NULL_AUDIO: play to no device at all (tests, machines without sound).
// From the process's environment itself: in a DLL with its own C runtime, getenv
// sees only what was there when it loaded.
bool NullAudioRequested() {
#ifdef _WIN32
    return GetEnvironmentVariableW(L"FASTPLAY_NULL_AUDIO", nullptr, 0) > 0;
#else
    return getenv("FASTPLAY_NULL_AUDIO") != nullptr;
#endif
}

ma_context* AcquireContext() {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_contextUsers == 0) {
        ma_backend nullBackend = ma_backend_null;
        const bool test = NullAudioRequested();
        ma_context_config contextConfig = ma_context_config_init();
#if TARGET_OS_IOS
        // The iPhone's audio session, for a player: sound with the ring switch
        // off and the screen locked (miniaudio's default is for a phone call)
        contextConfig.coreaudio.sessionCategory =
            g_iosSessionPlayback ? ma_ios_session_category_playback : ma_ios_session_category_none;
#endif
        if (ma_context_init(test ? &nullBackend : nullptr, test ? 1 : 0, &contextConfig, &g_context) != MA_SUCCESS) {
            return nullptr;
        }
    }
    g_contextUsers++;
    return &g_context;
}

void ReleaseContext() {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_contextUsers > 0 && --g_contextUsers == 0) ma_context_uninit(&g_context);
}

// ---------------------------------------------------------------------------
// Device callback
// ---------------------------------------------------------------------------

void Callback(Engine& g, float* out, ma_uint32 frameCount);
void RunDsps(Engine& g, float* samples, int frames);
ma_uint32 TapeBlock(Engine& g, float* out, ma_uint32 frameCount);

void DataCallback(ma_device* device, void* output, const void*, ma_uint32 frameCount) {
    Engine& g = *static_cast<Engine*>(device->pUserData);
    float* out = static_cast<float*>(output);
    Callback(g, out, frameCount);
    if (TapProc monitor = g.monitor.load()) monitor(out, static_cast<int>(frameCount), kChannels, g.outputRate, g.monitorUser);
}

void Callback(Engine& g, float* out, ma_uint32 frameCount) {
    std::memset(out, 0, static_cast<size_t>(frameCount) * kChannels * sizeof(float));
    g.lastPeriod = frameCount;
    if (g.state.load() != State::Playing) {
        g.fadeLevel = 0.0f;  // fade in when it plays again
        g.outputLevel = 0.0f;
        return;
    }
    const bool fadingOut = g.fadeOut.load();
    const bool fadingOutput = fadingOut && g.fadeOutput.load();
    // A pause: once both fades are down, silence (and the effects rest)
    if (fadingOutput && g.fadeLevel <= 0.0f && g.outputLevel <= 0.0f) {
        g.fadedOut = true;
        return;
    }
    const float fadeStep = g.fadeStep.load();

    // What the device takes from the ring: nothing while a seek's fade-out has
    // reached silence (the ring is being emptied), nor until a couple of periods
    // are in hand after one, so no block is ever cut off half way.
    ma_uint32 done = 0;
    const bool inputSilent = fadingOut && g.fadeLevel <= 0.0f;
    if (!inputSilent) {
        std::unique_lock<std::mutex> lock(g.ringMutex, std::try_to_lock);
        if (lock.owns_lock() && g.ringReady) {
            if (!g.primed.load() &&
                (ma_pcm_rb_available_read(&g.ring) >= frameCount * 2 || g.producerEnded.load())) {
                g.primed = true;
            }
            if (g.primed.load() && g.tapeRamp.load()) {
                // Every frame of the block is made, from fewer of the ring's
                const ma_uint32 took = TapeBlock(g, out, frameCount);
                done = frameCount;
                g.played += took;
                g.tapeHeard = true;
                if (took > 0) g.mixWake.notify_one();
            } else if (g.primed.load()) {
                // A tape stop called off (playing on), or a tape start up to speed:
                // faded back up from as quiet as it had got, so the sound does not jump
                if (g.tapeHeard) {
                    g.fadeLevel = std::min(g.fadeLevel, g.tapeLevel);
                    g.tapeHeard = false;
                }
                ma_uint32 buffered = ma_pcm_rb_available_read(&g.ring);
                if (buffered < g.minBuffered.load()) g.minBuffered = buffered;
                if (buffered < frameCount && !g.producerEnded.load() && !g.mixWaiting.load()) g.underruns++;
                while (done < frameCount) {
                    ma_uint32 frames = frameCount - done;
                    void* buffer = nullptr;
                    if (ma_pcm_rb_acquire_read(&g.ring, &frames, &buffer) != MA_SUCCESS || frames == 0) break;
                    std::memcpy(out + static_cast<size_t>(done) * kChannels, buffer,
                                static_cast<size_t>(frames) * kChannels * sizeof(float));
                    ma_pcm_rb_commit_read(&g.ring, frames);
                    done += frames;
                }
                g.played += done;
                if (done > 0) g.mixWake.notify_one();  // room in the ring: make more
            }
        }
    }

    // The fade, on the way into the effects, so their tails fade with the sound
    for (ma_uint32 i = 0; i < done; i++) {
        if (fadingOut) {
            g.fadeLevel = std::max(0.0f, g.fadeLevel - fadeStep);
        } else if (g.fadeLevel < 1.0f) {
            g.fadeLevel = std::min(1.0f, g.fadeLevel + fadeStep);
        }
        out[i * 2] *= g.fadeLevel;
        out[i * 2 + 1] *= g.fadeLevel;
    }
    // Nothing more came (a seek, or a stream waiting on the network): what comes
    // next fades in
    if (done < frameCount) g.fadeLevel = 0.0f;
    // A seek: the sound into the effects is silent, so the seek may go ahead,
    // while their tails ring on below
    if (fadingOut && !fadingOutput && g.fadeLevel <= 0.0f) g.fadedOut = true;

    // The effects and the recording tap, here, on exactly what is heard: nothing
    // they hold was thrown away unheard by a seek or pause, and a change to them
    // is heard at once. The whole block, silence included, so tails ring on.
    auto t0 = std::chrono::steady_clock::now();
    RunDsps(g, out, static_cast<int>(frameCount));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > g.maxBlockMs.load()) g.maxBlockMs = ms;

    // Pausing fades the effects' output too, so a tail still sounding is not cut
    // off dead; anything else leaves it to ring on
    if (fadingOutput || g.outputLevel < 1.0f) {
        for (ma_uint32 i = 0; i < frameCount; i++) {
            g.outputLevel = fadingOutput ? std::max(0.0f, g.outputLevel - fadeStep)
                                         : std::min(1.0f, g.outputLevel + fadeStep);
            out[i * 2] *= g.outputLevel;
            out[i * 2 + 1] *= g.outputLevel;
        }
    }
    if (fadingOutput && g.fadeLevel <= 0.0f && g.outputLevel <= 0.0f) g.fadedOut = true;

    // The gain, ramped across the block so a change never clicks
    float target = g.gain.load();
    float start = g.appliedGain;
    float step = (target - start) / static_cast<float>(frameCount);
    for (ma_uint32 i = 0; i < frameCount; i++) {
        float gain = start + step * static_cast<float>(i + 1);
        out[i * 2] *= gain;
        out[i * 2 + 1] *= gain;
    }
    g.appliedGain = target;

    // The end: everything produced and played
    if (g.primed.load() && done < frameCount && g.producerEnded.load() && !g.endReported.exchange(true)) {
        RunOnUiThread([handler = g.endHandler]() {
            if (handler) handler();
        });
    }
}

// A block of the tape stop or start: what comes next in the ring, resampled slower
// and slower (so lower and lower) down to a standstill, then silence, and the
// scrub end handler is called once; or faster and faster up from one, until at
// normal speed the ring is taken as it comes again. Returns the ring frames it
// took. Called with ringMutex held.
ma_uint32 TapeBlock(Engine& g, float* out, ma_uint32 frameCount) {
    const bool rising = g.tapeRising.load();
    const double change = rising ? g.tapeRise : -g.tapeFall;
    auto after = [change](double s) { return std::clamp(s + change, 0.0, 1.0); };
    auto still = [rising](double s) { return !rising && s < kTapeStopped; };

    // The ring frames the block moves on by, at the speeds it plays at (and the
    // first two, to start between)
    size_t want = g.tapeStarted ? 0 : 2;
    double pos = g.tapePos;
    double s = g.tapeSpeed;
    for (ma_uint32 i = 0; i < frameCount; i++) {
        if (still(s)) break;
        pos += s;
        while (pos >= 1.0) {
            pos -= 1.0;
            want++;
        }
        s = after(s);
    }
    want = std::min(want, g.tapeIn.size() / kChannels);
    size_t got = 0;
    while (got < want) {
        ma_uint32 n = static_cast<ma_uint32>(want - got);
        void* buffer = nullptr;
        if (ma_pcm_rb_acquire_read(&g.ring, &n, &buffer) != MA_SUCCESS || n == 0) break;
        std::memcpy(g.tapeIn.data() + got * kChannels, buffer, static_cast<size_t>(n) * kChannels * sizeof(float));
        ma_pcm_rb_commit_read(&g.ring, n);
        got += n;
    }

    // The next frame into b: from the ring, or when it has run dry (the end of
    // the track), ramping down to silence so it doesn't click
    size_t next = 0;
    auto take = [&]() {
        for (int ch = 0; ch < kChannels; ch++) {
            g.tapeA[ch] = g.tapeB[ch];
            g.tapeB[ch] = next < got ? g.tapeIn[next * kChannels + ch] : g.tapeB[ch] * 0.99f;
        }
        if (next < got) next++;
    };
    if (!g.tapeStarted) {
        take();
        take();
        g.tapeStarted = true;
    }
    for (ma_uint32 i = 0; i < frameCount; i++) {
        const double speed = g.tapeSpeed;
        if (still(speed)) {
            out[i * kChannels] = out[i * kChannels + 1] = 0.0f;
            g.tapeLevel = 0.0f;
            if (!g.tapeStandstill) {
                g.tapeStandstill = true;
                RunOnUiThread([handler = g.scrubEndHandler]() {
                    if (handler) handler();
                });
            }
            continue;
        }
        // Quiet near a standstill, coming from it or going to it
        const float level =
            static_cast<float>(std::clamp((speed - kTapeStopped) / (kTapeQuiet - kTapeStopped), 0.0, 1.0));
        g.tapeLevel = level;
        const float f = static_cast<float>(g.tapePos);
        for (int ch = 0; ch < kChannels; ch++) {
            out[i * kChannels + ch] = (g.tapeA[ch] + (g.tapeB[ch] - g.tapeA[ch]) * f) * level;
        }
        g.tapePos += speed;
        while (g.tapePos >= 1.0) {
            g.tapePos -= 1.0;
            take();
        }
        g.tapeSpeed = after(speed);
    }
    // Up to speed: from the next block, the ring as it comes
    if (rising && g.tapeSpeed >= 1.0) g.tapeRamp = false;
    return static_cast<ma_uint32>(got);
}

// ---------------------------------------------------------------------------
// Decode thread
// ---------------------------------------------------------------------------

// Where the buffer is filled from, from now on (feedMutex held): forward from
// source frame `frame`, or backward from it in chunks of `chunkFrames`, back as far
// as `floorFrames`. The buffer is emptied, and what is being read for before is
// not used.
void SetFeed(Engine& g, int64_t frame, bool reverse, int chunkFrames, int64_t floorFrames) {
    g.feedGeneration++;
    g.reverse = reverse;
    if (reverse) {
        g.reverseEnd = frame;
        g.reverseChunkFrames = std::max(chunkFrames, g.sourceRate / 10);
        g.reverseFloor = floorFrames;
        g.reverseTail.clear();
    } else {
        g.feedFrame = frame;
    }
    g.pcm.Clear();
}

// Reading backward, for scrubbing: the chunk before reverseEnd, reversed, into the
// buffer. Each is read with a little more before it, held back and crossfaded
// into the start of the next, so a seek that lands a few samples off never
// clicks; and a little more before that again, decoded and dropped, since some
// formats (MP3) take a frame or two to settle after a seek.
struct BackChunk {
    int64_t end;     // it ends here (reverseEnd)
    int64_t start;   // and the next ends here
    int64_t from;    // what is kept begins here, overlapping the next
    int64_t seekTo;  // and it is read from here, to settle
};

// The next chunk backward (feedMutex held). False at the floor, which is the end.
bool NextBackChunk(Engine& g, BackChunk& c) {
    c.end = g.reverseEnd;
    if (c.end <= g.reverseFloor) {
        g.pcm.SetEnded();  // back at the start (or the oldest a live stream keeps)
        return false;
    }
    const int64_t overlap = g.sourceRate / 50;  // 20 ms
    const int64_t settle = g.sourceRate / 10;   // 100 ms
    c.start = std::max<int64_t>(g.reverseFloor, c.end - g.reverseChunkFrames);
    c.from = std::max<int64_t>(g.reverseFloor, c.start - overlap);
    c.seekTo = std::max<int64_t>(g.reverseFloor, c.from - settle);
    return true;
}

// A chunk read (`got` frames from c.seekTo), reversed into the buffer (feedMutex held)
void PutBackChunk(Engine& g, const BackChunk& c, const std::vector<float>& chunk, size_t got,
                  std::vector<float>& reversed);

// The next chunk backward from the cache, if it has its end (feedMutex held). If
// the cache begins part way into it, a shorter chunk, from where the cache
// begins (decoded on from before it, it needs no settling).
bool BackChunkFromCache(Engine& g, std::vector<float>& chunk, std::vector<float>& reversed) {
    BackChunk c;
    if (!g.reverse || !NextBackChunk(g, c) || !g.cache.Has(c.end - 1)) return false;
    if (c.seekTo < g.cache.Start()) {
        const int64_t overlap = g.sourceRate / 50;
        if (c.from < g.cache.Start()) {
            c.from = g.cache.Start();
            c.start = std::max(c.start, c.from + overlap);
        }
        c.seekTo = c.from;
        if (c.end - c.start < overlap) return false;  // next to none of it
    }
    const size_t frames = static_cast<size_t>(c.end - c.seekTo);
    chunk.resize(frames * kChannels);
    PutBackChunk(g, c, chunk, g.cache.Copy(c.seekTo, chunk.data(), frames), reversed);
    return true;
}

void ReadBackward(Engine& g, std::vector<float>& chunk, std::vector<float>& reversed) {
    uint64_t generation;
    BackChunk c;
    {
        std::lock_guard<std::mutex> feed(g.feedMutex);
        if (!g.reverse) return;
        if (BackChunkFromCache(g, chunk, reversed)) return;  // decoded lately: no seek
        if (!NextBackChunk(g, c)) return;
        generation = g.feedGeneration;
    }
    const size_t frames = static_cast<size_t>(c.end - c.seekTo);
    const size_t keepStart = static_cast<size_t>(c.from - c.seekTo);
    chunk.resize(frames * kChannels);
    size_t got = 0;
    const auto began = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> use(g.decoderMutex);
        if (!g.decoder) return;
        g.decoderFrame = -1;  // wherever the seek and the reading leave it
        if (g.decoder->Seek(static_cast<double>(c.seekTo) / g.sourceRate)) {
            while (got < frames) {
                int n = g.decoder->Read(chunk.data() + got * kChannels,
                                        static_cast<int>(std::min<size_t>(4096, frames - got)));
                if (n <= 0) break;
                got += static_cast<size_t>(n);
            }
            g.decoderFrame = c.seekTo + static_cast<int64_t>(got);
            // Kept from where it has settled, so scrubbing back over it again, or
            // playing on forward from it, needs no seek
            std::lock_guard<std::mutex> feed(g.feedMutex);
            if (got > keepStart) g.cache.Add(c.from, chunk.data() + keepStart * kChannels, got - keepStart);
        }
    }
    std::lock_guard<std::mutex> feed(g.feedMutex);
    if (generation != g.feedGeneration) return;  // a seek came meanwhile
    PutBackChunk(g, c, chunk, got, reversed);
    // Slow to seek (a CD's drive moving back each time): longer chunks, so it
    // seeks less often and keeps up
    if (std::chrono::steady_clock::now() - began > std::chrono::milliseconds(50)) {
        g.reverseChunkFrames = std::min(g.reverseChunkFrames * 2, 4 * g.sourceRate);
    }
}

void PutBackChunk(Engine& g, const BackChunk& c, const std::vector<float>& chunk, size_t got,
                  std::vector<float>& reversed) {
    const int64_t start = c.start, from = c.from;
    const size_t keepStart = static_cast<size_t>(c.from - c.seekTo);
    g.reverseEnd = start;
    if (got <= keepStart) return;
    const size_t count = got - keepStart;
    reversed.resize(count * kChannels);
    for (size_t i = 0; i < count; i++) {
        reversed[i * kChannels] = chunk[(got - 1 - i) * kChannels];
        reversed[i * kChannels + 1] = chunk[(got - 1 - i) * kChannels + 1];
    }
    // Its first frames are the same audio as the last chunk's held-back tail
    const size_t fade = std::min(count, g.reverseTail.size() / kChannels);
    for (size_t i = 0; i < fade; i++) {
        float w = (static_cast<float>(i) + 0.5f) / static_cast<float>(fade);
        for (int ch = 0; ch < kChannels; ch++) {
            float& s = reversed[i * kChannels + ch];
            s = s * w + g.reverseTail[i * kChannels + ch] * (1.0f - w);
        }
    }
    // Hold back its own overlap (none at the very start: nothing comes after it)
    const size_t hold = std::min(count, static_cast<size_t>(start - from));
    g.pcm.Write(reversed.data(), count - hold);
    g.reverseTail.assign(reversed.end() - static_cast<std::ptrdiff_t>(hold * kChannels), reversed.end());
}

// A file, going forward: the next block into the buffer, from what was decoded
// lately while there is some, then from the decoder (seeking it there first if
// it is elsewhere). What the decoder reads goes into the cache whatever happens,
// and into the buffer unless a seek came while it read.
void ReadForward(Engine& g, std::vector<float>& block) {
    uint64_t generation;
    int64_t frame;
    {
        std::lock_guard<std::mutex> feed(g.feedMutex);
        if (g.reverse) return;
        generation = g.feedGeneration;
        frame = g.feedFrame;
        size_t got = g.cache.Copy(frame, block.data(), 4096);
        if (got > 0) {
            g.pcm.Write(block.data(), got);
            g.feedFrame += static_cast<int64_t>(got);
            return;
        }
        if (g.endFrame >= 0 && frame >= g.endFrame) {
            g.pcm.SetEnded();
            return;
        }
    }
    std::lock_guard<std::mutex> use(g.decoderMutex);
    if (!g.decoder) return;
    bool there = g.decoderFrame == frame;
    if (!there && g.decoder->Seek(static_cast<double>(frame) / g.sourceRate)) {
        g.decoderFrame = frame;
        there = true;
    }
    int n = there ? g.decoder->Read(block.data(), 4096) : 0;
    std::lock_guard<std::mutex> feed(g.feedMutex);
    if (n > 0) {
        g.cache.Add(g.decoderFrame, block.data(), static_cast<size_t>(n));
        g.decoderFrame += n;
    } else if (there) {
        g.endFrame = g.decoderFrame;
    }
    if (generation != g.feedGeneration) return;
    if (n > 0) {
        g.pcm.Write(block.data(), static_cast<size_t>(n));
        g.feedFrame += n;
    } else {
        g.pcm.SetEnded();  // the end, or it could not seek there
    }
}

void DecodeLoop(Engine* engine) {
    Engine& g = *engine;
    std::vector<float> block(4096 * kChannels);
    std::vector<float> chunk, reversed;
    while (g.running) {
        bool backward;
        {
            std::unique_lock<std::mutex> lock(g.decodeMutex);
            if (g.seekRequested) {
                g.seekRequested = false;
                double target = g.seekTarget;
                bool reverse = g.seekReverse;
                int chunkFrames = g.seekChunkFrames;
                int64_t floorFrames = g.seekFloorFrames;
                lock.unlock();
                bool ok = true;
                {
                    std::lock_guard<std::mutex> use(g.decoderMutex);
                    bool seek;
                    const int64_t frame = std::llround(target * g.sourceRate);
                    {
                        std::lock_guard<std::mutex> feed(g.feedMutex);
                        SetFeed(g, frame, reverse && g.decoder, chunkFrames, floorFrames);
                        // Backward, nothing to do yet (the first chunk ends here); a
                        // file, nothing either into what was decoded lately, or where
                        // the decoder already is
                        seek = g.decoder && !g.reverse &&
                               (g.live || !(g.cache.Has(frame) || frame == g.decoderFrame));
                    }
                    if (seek) {
                        ok = g.decoder->Seek(target);
                        g.decoderFrame = ok ? frame : -1;
                    } else if (!g.decoder) {
                        ok = false;
                    }
                }
                lock.lock();
                g.seekOk = ok;
                g.seekDone = true;
                g.seekFinished.notify_all();
                continue;
            }
            size_t room;
            {
                std::lock_guard<std::mutex> feed(g.feedMutex);
                backward = g.reverse;
                // Room for a block, or backward, for a whole chunk and its overlap
                room = backward ? static_cast<size_t>(g.reverseChunkFrames + g.sourceRate / 50) : 4096;
            }
            // (a live stream kept for rewinding that has played all it has: until more comes)
            if (!g.decoder || g.pcm.DecoderEnded() || g.pcm.Space() < room || (!backward && g.decoder->Starved())) {
                g.decodeWake.wait_for(lock, std::chrono::milliseconds(20));
                continue;
            }
        }
        if (backward) {
            ReadBackward(g, chunk, reversed);
            continue;
        }
        if (!g.live) {
            ReadForward(g, block);
            continue;
        }
        bool titleChanged;
        {
            // Written while the decoder is still held, so a block read from a
            // decoder being unloaded never lands in the next one's buffer.
            std::lock_guard<std::mutex> use(g.decoderMutex);
            if (!g.decoder) continue;
            int got = g.decoder->Read(block.data(), 4096);
            {
                std::lock_guard<std::mutex> feed(g.feedMutex);
                if (got > 0) {
                    g.pcm.Write(block.data(), static_cast<size_t>(got));
                } else if (!g.decoder->Starved()) {
                    g.pcm.SetEnded();
                }
            }
            titleChanged = TakeStreamTitleChange(g.decoder.get());
        }
        if (titleChanged) {
            RunOnUiThread([handler = g.titleHandler]() {
                if (handler) handler();
            });
        }
    }
}

// ---------------------------------------------------------------------------
// Mix thread
// ---------------------------------------------------------------------------

void RunDsps(Engine& g, float* samples, int frames) {
    std::lock_guard<std::mutex> lock(g.dspMutex);
    if (g.tap && g.tapBeforeEffects) g.tap(samples, frames, kChannels, g.outputRate, g.tapUser);
    for (const Dsp& dsp : g.dsps) dsp.proc(samples, frames, kChannels, g.outputRate, dsp.user);
    if (g.tap && !g.tapBeforeEffects) g.tap(samples, frames, kChannels, g.outputRate, g.tapUser);
}

void MixLoop(Engine* engine) {
    Engine& g = *engine;
    g.mixBlock.resize(static_cast<size_t>(kMixBlockFrames) * kChannels);
    while (g.running) {
        bool idle = true;
        {
            std::lock_guard<std::mutex> lock(g.mixMutex);
            if (g.processor && g.ringReady && !g.producerEnded) {
                // A stream fills its buffer before it plays (and after it runs dry).
                if (g.mixWaiting) {
                    if (g.pcm.Available() >= static_cast<int>(g.prebufferFrames) || g.pcm.DecoderEnded()) {
                        g.mixWaiting = false;
                    }
                }
                ma_uint32 space = ma_pcm_rb_available_write(&g.ring);
                if (!g.mixWaiting && space >= static_cast<ma_uint32>(kMixBlockFrames)) {
                    bool ended = false;
                    int frames = g.scrub ? g.scrub->Fill(g.pcm, g.mixBlock.data(), kMixBlockFrames)
                                         : g.processor->Fill(g.pcm, g.mixBlock.data(), kMixBlockFrames, ended);
                    if (frames > 0) {
                        ma_uint32 left = static_cast<ma_uint32>(frames);
                        const float* src = g.mixBlock.data();
                        while (left > 0) {
                            ma_uint32 n = left;
                            void* buffer = nullptr;
                            if (ma_pcm_rb_acquire_write(&g.ring, &n, &buffer) != MA_SUCCESS || n == 0) break;
                            std::memcpy(buffer, src, static_cast<size_t>(n) * kChannels * sizeof(float));
                            ma_pcm_rb_commit_write(&g.ring, n);
                            src += static_cast<size_t>(n) * kChannels;
                            left -= n;
                        }
                        idle = false;
                    }
                    if (ended) {
                        g.producerEnded = true;
                    } else if (frames < kMixBlockFrames && g.live && !g.pcm.DecoderEnded() &&
                               ma_pcm_rb_available_read(&g.ring) == 0) {
                        g.mixWaiting = true;  // ran dry: buffer again
                    }
                }
            }
            // Tape wound down, and heard to the end of it: the player carries on
            if (g.scrub && g.scrub->TakeFinished(g.played.load())) {
                RunOnUiThread([handler = g.scrubEndHandler]() {
                    if (handler) handler();
                });
            }
        }
        g.decodeWake.notify_one();
        if (idle) {
            // Until the device takes audio (or a little while, for a stream
            // waiting on the network)
            std::unique_lock<std::mutex> wait(g.mixWakeMutex);
            g.mixWake.wait_for(wait, std::chrono::milliseconds(5));
        }
    }
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

bool FindDevice(Engine& g, const std::wstring& name, ma_device_id& id) {
    if (name.empty()) return false;
    ma_device_info* devices = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(g.context, &devices, &count, nullptr, nullptr) != MA_SUCCESS) return false;
    for (ma_uint32 i = 0; i < count; i++) {
        if (Utf8ToWide(devices[i].name) == name) {
            id = devices[i].id;
            return true;
        }
    }
    return false;
}

void CloseDevice(Engine& g) {
    if (g.deviceReady) {
        ma_device_uninit(&g.device);
        g.deviceReady = false;
    }
    std::lock_guard<std::mutex> lock(g.ringMutex);
    if (g.ringReady) {
        ma_pcm_rb_uninit(&g.ring);
        g.ringReady = false;
    }
    g.tapeRamp = false;
}

void StopIdleDevice(Engine& g) {
#if TARGET_OS_IOS
	// iOS determines Now Playing's state from the audio output. Sending silence
	// still counts as playing, so pause the device as well as the source.
	if (g.deviceReady && ma_device_stop(&g.device) == MA_SUCCESS) {
		g.fadeLevel = 0.0f;
		g.outputLevel = 0.0f;
	}
#endif
}

bool OpenDevice(Engine& g, const std::wstring& name, int bufferMs) {
    ma_device_id id;
    bool found = FindDevice(g, name, id);
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = found ? &id : nullptr;
    config.playback.format = ma_format_f32;
    config.playback.channels = kChannels;
    config.sampleRate = 0;  // the device's own
    config.dataCallback = DataCallback;
    config.pUserData = &g;
    // The device takes small periods (as BASS's device buffer was small); the
    // output buffer (bufferMs, the setting) is what holds audio in hand. A
    // device period as long as that buffer would find it short every time.
    config.performanceProfile = ma_performance_profile_low_latency;
    config.periodSizeInMilliseconds = kDevicePeriodMs;
    config.periods = 3;
    if (ma_device_init(g.context, &config, &g.device) != MA_SUCCESS) {
        if (!found) return false;
        config.playback.pDeviceID = nullptr;  // fall back to the default
        found = false;
        if (ma_device_init(g.context, &config, &g.device) != MA_SUCCESS) return false;
    }
    g.deviceReady = true;
    g.defaultDevice = !found;
    g.deviceName = Utf8ToWide(g.device.playback.name);
    g.outputRate = static_cast<int>(g.device.sampleRate);

    // At least a few of the device's periods, whatever the setting
    size_t period = g.device.playback.internalPeriodSizeInFrames;
    g.ringFrames = static_cast<size_t>(g.outputRate) * static_cast<size_t>(std::clamp(bufferMs, 50, 5000)) / 1000;
    g.ringFrames = std::max(g.ringFrames, period * 4);
    {
        std::lock_guard<std::mutex> lock(g.ringMutex);
        if (ma_pcm_rb_init(ma_format_f32, kChannels, static_cast<ma_uint32>(g.ringFrames), nullptr, nullptr, &g.ring) !=
            MA_SUCCESS) {
            ma_device_uninit(&g.device);
            g.deviceReady = false;
            return false;
        }
        g.ringReady = true;
    }
#if TARGET_OS_IOS
	// Play() starts it when there is audio to play.
	return true;
#else
	return ma_device_start(&g.device) == MA_SUCCESS;
#endif
}

// Empties the output ring and starts counting played frames again. The mix
// thread must be held (m_mixMutex).
void ResetOutput(Engine& g) {
    std::lock_guard<std::mutex> lock(g.ringMutex);
    if (g.ringReady) ma_pcm_rb_reset(&g.ring);
    g.tapeRamp = false;
    g.primed = false;
    g.played = 0;
    g.producerEnded = false;
    g.endReported = false;
}

// The speed a tape stop or start changes by each frame, to take `seconds` (at
// once, for none)
double RampStep(const Engine& g, double seconds) {
    return seconds > 0 ? 1.0 / (seconds * g.outputRate) : 1.0;
}

// A tape stop or start from `speed`, with ringMutex held
void StartTapeRamp(Engine& g, double speed, bool rising) {
    g.tapeIn.resize(g.ringFrames * kChannels);
    g.tapeStarted = false;
    g.tapeStandstill = false;
    g.tapeSpeed = speed;
    g.tapePos = 0;
    g.tapeRising = rising;
    g.tapeRamp = true;
}

// Back to taking the ring as it comes. What the tape stop left in it plays next.
void EndTapeStop(Engine& g) {
    std::lock_guard<std::mutex> lock(g.ringMutex);
    g.tapeRamp = false;
}

// Fades the sound out and waits until it is silent (a few milliseconds), so what
// follows - a pause, a seek, an unload - makes no click. `output`: the effects'
// output too (pausing); otherwise only what goes into them, and their tails ring
// on (seeking). The caller clears fadeOut when the sound may come back. Does
// nothing if nothing is playing.
void FadeOut(Engine& g, bool output) {
    if (g.fadeStep.load() >= 1.0f) return;  // smooth seeking is off
    g.fadedOut = false;
    g.fadeOutput = output;
    g.fadeOut = true;
    if (g.state.load() != State::Playing || !g.deviceReady) return;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
    while (!g.fadedOut.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

std::vector<Device> ListDevices() {
    std::vector<Device> list;
    ma_context* context = AcquireContext();
    if (!context) return list;
    ma_device_info* devices = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(context, &devices, &count, nullptr, nullptr) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; i++) {
            Device device;
            device.name = Utf8ToWide(devices[i].name);
            device.isDefault = devices[i].isDefault != 0;
            list.push_back(device);
        }
    }
    ReleaseContext();
    return list;
}

void SetIosAudioSession(bool playback) { g_iosSessionPlayback = playback; }

Player::Player() : m(std::make_unique<Impl>()) {}

Player::~Player() { Shutdown(); }

bool Player::Init(const std::wstring& deviceName, int bufferMs) {
    Engine& g = *m;
    if (!g.context) {
        g.context = AcquireContext();
        if (!g.context) return false;
    }
    if (!OpenDevice(g, deviceName, bufferMs)) return false;
    if (!g.running) {
        g.running = true;
        g.decodeThread = std::thread(DecodeLoop, &g);
        g.mixThread = std::thread(MixLoop, &g);
    }
    return true;
}

void Player::Shutdown() {
    Engine& g = *m;
    Unload();
    if (g.running) {
        g.running = false;
        g.decodeWake.notify_all();
        g.mixWake.notify_all();
        if (g.decodeThread.joinable()) g.decodeThread.join();
        if (g.mixThread.joinable()) g.mixThread.join();
    }
    CloseDevice(g);
    if (g.context) {
        ReleaseContext();
        g.context = nullptr;
    }
}

bool Player::SwitchDevice(const std::wstring& deviceName, int bufferMs) {
    Engine& g = *m;
    if (g.loaded) return false;
    std::lock_guard<std::mutex> mix(g.mixMutex);
    CloseDevice(g);
    return OpenDevice(g, deviceName, bufferMs);
}

std::wstring Player::CurrentDeviceName() {
    Engine& g = *m;
    return g.deviceName;
}
bool Player::UsingDefaultDevice() {
    Engine& g = *m;
    return g.defaultDevice;
}
int Player::MixSampleRate() {
    Engine& g = *m;
    return g.outputRate;
}

bool Player::Load(std::unique_ptr<Decoder> decoder, TempoAlgorithm algorithm) {
    Engine& g = *m;
    Unload();
    if (!decoder || !g.deviceReady) return false;

    std::unique_lock<std::mutex> mix(g.mixMutex);
    std::lock_guard<std::mutex> decode(g.decodeMutex);
    g.live = decoder->IsLive();
    g.length = decoder->Length();
    const int rate = decoder->SampleRate();
    g.pcm.Reset(static_cast<size_t>(rate * (g.live ? kStreamReadAhead : kFileReadAhead)));
    g.prebufferFrames = g.live ? static_cast<size_t>(rate * kStreamPrebuffer) : 0;
    g.mixWaiting = g.live.load();
    g.processor = TempoProcessor::Create(algorithm, rate, g.outputRate);
    g.processor->SetTempo(g.live ? 0.0f : g.tempo);
    g.processor->SetPitch(g.pitch);
    g.processor->SetRate(g.live ? 1.0f : g.rate);
    g.processor->Restart(0.0);
    g.scrub.reset();
    g.sourceRate = rate;
    {
        std::lock_guard<std::mutex> use(g.decoderMutex);
        g.decoder = std::move(decoder);
        g.decoderFrame = 0;
        std::lock_guard<std::mutex> feed(g.feedMutex);
        g.feedGeneration++;
        g.reverse = false;
        g.cache.Reset(g.live ? 0 : static_cast<size_t>(rate * kSourceCacheSeconds));
        g.feedFrame = 0;
        g.endFrame = -1;
    }
    g.playReverse = false;
    g.seekRequested = false;
    ResetOutput(g);
    g.state = State::Paused;
    g.loaded = true;
    g.decodeWake.notify_one();
    return true;
}

void Player::Unload() {
    Engine& g = *m;
    if (!g.loaded) return;
    FadeOut(g, true);
    g.state = State::Empty;
    g.fadeOut = false;
	StopIdleDevice(g);
    // A decoder blocked on the network is asked to give up, then the threads are
    // held while it goes.
    if (g.decoder) g.decoder->Abort();
    std::unique_lock<std::mutex> mix(g.mixMutex);
    std::lock_guard<std::mutex> decode(g.decodeMutex);
    g.loaded = false;
    g.processor.reset();
    g.scrub.reset();
    {
        std::lock_guard<std::mutex> use(g.decoderMutex);
        g.decoder.reset();
        std::lock_guard<std::mutex> feed(g.feedMutex);
        g.feedGeneration++;
        g.reverse = false;
        g.cache.Reset(0);
    }
    g.playReverse = false;
    g.pcm.Clear();
    ResetOutput(g);
    g.live = false;
    g.length = 0;
}

bool Player::IsLoaded() {
    Engine& g = *m;
    return g.loaded;
}
const Decoder* Player::Current() {
    Engine& g = *m;
    return g.loaded ? g.decoder.get() : nullptr;
}

namespace {
void (*g_beforeDeviceStart)() = nullptr;
}

void SetBeforeDeviceStart(void (*beforeStart)()) { g_beforeDeviceStart = beforeStart; }

bool Player::EnsureDeviceRunning() {
    Engine& g = *m;
    if (!g.deviceReady) return false;
    if (ma_device_get_state(&g.device) == ma_device_state_started) return true;
    if (g_beforeDeviceStart) g_beforeDeviceStart();
    return ma_device_start(&g.device) == MA_SUCCESS;
}

void Player::Play() {
    Engine& g = *m;
    if (!g.loaded) return;
	EnsureDeviceRunning();  // a pause or interruption may have stopped it
    g.state = State::Playing;
}

void Player::Pause() {
    Engine& g = *m;
    if (!g.loaded) return;
    FadeOut(g, true);
    g.state = State::Paused;
    g.fadeOut = false;
    EndTapeStop(g);  // played on from where the tape stopped
	StopIdleDevice(g);
}

void Player::Stop() {
    Engine& g = *m;
    if (!g.loaded) return;
    FadeOut(g, true);
    g.state = State::Stopped;
    g.fadeOut = false;
    EndTapeStop(g);
	StopIdleDevice(g);
}

State Player::GetState() {
    Engine& g = *m;
    return g.state.load();
}

double Player::Position() {
    Engine& g = *m;
    if (!g.loaded) return 0.0;
    std::lock_guard<std::mutex> mix(g.mixMutex);
    double seconds = g.scrub       ? g.scrub->PositionAt(g.played.load())
                     : g.processor ? g.processor->PositionAt(g.played.load())
                                   : 0.0;
    // In reverse the processor counts forward from where it started, while the
    // source goes back from there
    if (!g.scrub && g.processor && g.playReverse) seconds = std::max(0.0, 2 * g.reverseFrom - seconds);
    return g.length > 0 ? std::min(seconds, g.length) : seconds;
}

double Player::Length() {
    Engine& g = *m;
    return g.loaded ? g.length : 0.0;
}
bool Player::IsLive() {
    Engine& g = *m;
    return g.loaded && g.live;
}

namespace {

// The pitch and rate playing has, for the scrubber to keep (a live stream plays at
// its own rate, as the processor has it)
void SetScrubSound(Engine& g, Scrubber& scrub) { scrub.SetSound(g.pitch, g.live ? 1.0f : g.rate); }

// A file's seek made here and now, not waiting for the decode thread (which may be
// in the middle of reading a CD, say): backward, where nothing is read until the
// first chunk, or forward into what was decoded lately. False if the decoder
// has to seek.
bool SeekNow(Engine& g, double seconds, int reverseChunkFrames, double floorSeconds) {
    if (g.live || !g.decoder) return false;
    const int64_t frame = std::llround(seconds * g.sourceRate);
    std::lock_guard<std::mutex> feed(g.feedMutex);
    if (reverseChunkFrames <= 0 && !g.cache.Has(frame)) return false;
    SetFeed(g, frame, reverseChunkFrames > 0, reverseChunkFrames, static_cast<int64_t>(floorSeconds * g.sourceRate));
    // A start on the buffer from the cache, here, so it plays at once even while
    // the decode thread is still busy with the drive or the decoder
    if (g.reverse) {
        std::vector<float> chunk, reversed;
        for (int i = 0; i < 2; i++) {
            if (g.pcm.Space() < static_cast<size_t>(g.reverseChunkFrames + g.sourceRate / 50)) break;
            if (!BackChunkFromCache(g, chunk, reversed)) break;
        }
    } else {
        std::vector<float> block(4096 * kChannels);
        const int64_t until = frame + 2 * g.sourceRate;  // a couple of seconds
        while (g.feedFrame < until && g.pcm.Space() >= 4096) {
            size_t got = g.cache.Copy(g.feedFrame, block.data(), 4096);
            if (got == 0) break;
            g.pcm.Write(block.data(), got);
            g.feedFrame += static_cast<int64_t>(got);
        }
    }
    return true;
}

// Carries on from `seconds`: played normally (scrub null), or by the scrubber,
// reading the source backward in chunks of `reverseChunkFrames` if that is not 0,
// back as far as `floorSeconds`. Played normally while playing in reverse, it
// carries on in reverse.
bool Reposition(Engine& g, double seconds, std::unique_ptr<Scrubber> scrub, int reverseChunkFrames, double floorSeconds = 0) {
    if (g.length > 0) seconds = std::min(seconds, g.length);
    if (seconds < 0) seconds = 0;
    if (!scrub && g.playReverse) reverseChunkFrames = g.sourceRate;  // a second at a time
    FadeOut(g, false);
    struct FadeBackIn {
        Engine& g;
        ~FadeBackIn() { g.fadeOut = false; }
    } fadeBackIn{g};  // however the seek ends
    std::unique_lock<std::mutex> mix(g.mixMutex);
    bool ok = true;
    if (!SeekNow(g, seconds, reverseChunkFrames, floorSeconds)) {
        std::unique_lock<std::mutex> lock(g.decodeMutex);
        g.seekTarget = seconds;
        g.seekReverse = reverseChunkFrames > 0;
        g.seekChunkFrames = reverseChunkFrames;
        g.seekFloorFrames = static_cast<int64_t>(floorSeconds * g.sourceRate);
        g.seekRequested = true;
        g.seekDone = false;
        g.decodeWake.notify_all();
        if (!g.seekFinished.wait_for(lock, std::chrono::seconds(30), [&g] { return g.seekDone; })) return false;
        ok = g.seekOk;
    }
    g.scrub = std::move(scrub);
    g.reverseFrom = seconds;
    if (g.processor && !g.scrub) g.processor->Restart(seconds);
    ResetOutput(g);
    g.decodeWake.notify_all();
    return ok;
}

// The ring is made ahead with the pitch, tempo and rate as they were. Changed
// while stopped or paused, it is made again from where playing will go on, so the
// change is heard from the start and not only once the old ring has played.
void RemakeAhead(Player& player) {
    if (!player.IsLoaded() || player.GetState() == State::Playing) return;
    if (player.IsScrubbing() || player.IsTapeStopping()) return;
    player.Seek(player.Position());
}

}  // namespace

bool Player::LiveRange(double& start, double& live) {
    Engine& g = *m;
    if (!g.loaded || !g.live || !g.decoder) return false;
    double end;
    if (!g.decoder->Rewindable(start, end)) return false;
    // As much in hand as a stream starts with, and a little
    live = std::max(start, end - kStreamPrebuffer - 0.5);
    return true;
}

bool Player::Seek(double seconds) {
    Engine& g = *m;
    if (!g.loaded) return false;
    if (g.live) {
        double start, live;
        if (!LiveRange(start, live)) return false;
        seconds = std::clamp(seconds, start, live);
    }
    return Reposition(g, seconds, nullptr, 0);
}

bool Player::StartScrub(ScrubStyle style, int direction, float speed) {
    Engine& g = *m;
    if (!g.loaded) return false;
    double floor = 0, live = 0;
    if (g.live) {
        if (!LiveRange(floor, live)) return false;
        floor += 1.0;  // clear of what is about to be dropped
    }
    double from = Position();
    auto scrub = std::make_unique<Scrubber>(style, direction, speed, g.sourceRate, g.outputRate, from, g.scrubTiming);
    SetScrubSound(g, *scrub);
    // Backward, the source is read in chunks: longer the faster it goes, so the
    // seeks between them stay few
    int chunk = 0;
    if (direction < 0) chunk = static_cast<int>(g.sourceRate * std::clamp(speed * 0.125, 0.25, 2.0));
    return Reposition(g, from, std::move(scrub), chunk, floor);
}

void Player::SetScrubSpeed(float speed) {
    Engine& g = *m;
    std::lock_guard<std::mutex> mix(g.mixMutex);
    if (g.scrub) g.scrub->SetSpeed(speed);
}

bool Player::StopScrub() {
    Engine& g = *m;
    if (!g.loaded || !g.scrub) return false;
    double at = Position();
    double start, live;
    if (g.live && LiveRange(start, live)) at = std::clamp(at, start, live);
    return Reposition(g, at, nullptr, 0);
}

bool Player::SetReverse(bool on) {
    Engine& g = *m;
    if (!g.loaded || g.live || !g.decoder) return false;
    if (on == g.playReverse) return true;
    double at = Position();
    {
        std::lock_guard<std::mutex> mix(g.mixMutex);
        g.playReverse = on;
        // Scrubbing carries on as it was, and ends in the new direction
        if (g.scrub) return true;
    }
    return Reposition(g, at, nullptr, 0);
}

bool Player::IsReverse() {
    Engine& g = *m;
    return g.loaded && g.playReverse;
}

bool Player::IsScrubbing() {
    Engine& g = *m;
    return g.loaded && g.scrub != nullptr;
}

bool Player::ReleaseScrub() {
    Engine& g = *m;
    if (!g.loaded) return false;
    int direction;
    float top;
    double speed;
    {
        std::lock_guard<std::mutex> mix(g.mixMutex);
        if (!g.scrub || g.scrub->Style() != ScrubStyle::Tape) return false;
        if (g.scrub->Releasing()) return true;
        direction = g.scrub->Direction();
        top = g.scrub->TopSpeed();
        speed = g.scrub->HeardSpeed(g.played.load());
    }
    // From what is heard now, not from what was made ahead of it: the wind down
    // starts at once, at the speed being heard
    double floor = 0, live = 0;
    if (g.live) {
        if (!LiveRange(floor, live)) return false;
        floor += 1.0;
    }
    double from = Position();
    auto scrub = std::make_unique<Scrubber>(ScrubStyle::Tape, direction, top, g.sourceRate, g.outputRate, from,
                                            g.scrubTiming, Scrubber::Begin::WindDown, speed);
    SetScrubSound(g, *scrub);
    int chunk = 0;
    if (direction < 0) chunk = static_cast<int>(g.sourceRate * std::clamp(speed * 0.125, 0.25, 2.0));
    return Reposition(g, from, std::move(scrub), chunk, floor);
}

bool Player::TapeStop() {
    Engine& g = *m;
    if (!g.loaded || g.state.load() != State::Playing) return false;
    {
        std::lock_guard<std::mutex> mix(g.mixMutex);
        if (g.scrub) return false;
    }
    // No seek: the device slows down what is already in the ring, at once
    std::lock_guard<std::mutex> lock(g.ringMutex);
    if (!g.ringReady) return false;
    g.tapeFall = RampStep(g, g.scrubTiming.tapeStop);
    if (g.tapeRamp) {
        // A tape start: back down from as fast as it had got
        if (!g.tapeRising) return false;
        g.tapeRising = false;
        g.tapeStandstill = false;
        return true;
    }
    // Played to the end: nothing is left to slow down
    if (g.producerEnded && ma_pcm_rb_available_read(&g.ring) == 0) return false;
    StartTapeRamp(g, 1.0, false);
    return true;
}

bool Player::TapeStart() {
    Engine& g = *m;
    if (!g.loaded) return false;
    {
        std::lock_guard<std::mutex> mix(g.mixMutex);
        if (g.scrub) return false;
    }
    std::lock_guard<std::mutex> lock(g.ringMutex);
    if (!g.ringReady) return false;
    g.tapeRise = RampStep(g, g.scrubTiming.tapeStart);
    if (g.tapeRamp) {
        // A tape stop: back up from as slow as it had got
        g.tapeRising = true;
        return true;
    }
    if (g.producerEnded && ma_pcm_rb_available_read(&g.ring) == 0) return false;
    StartTapeRamp(g, 0.0, true);
    return true;
}

void Player::SetScrubTiming(const ScrubTiming& timing) {
    Engine& g = *m;
    g.scrubTiming = timing;
}

bool Player::IsTapeStopping() {
    Engine& g = *m;
    return g.loaded && g.tapeRamp.load() && !g.tapeRising.load();
}

bool Player::CancelTapeStop() {
    Engine& g = *m;
    if (!IsTapeStopping()) return false;
    EndTapeStop(g);  // on at normal speed from where the tape got to
    return true;
}

void Player::SetScrubEndHandler(std::function<void()> handler) {
    Engine& g = *m;
    g.scrubEndHandler = std::move(handler);
}

void Player::SetTempo(float percent) {
    Engine& g = *m;
    const bool changed = percent != g.tempo && !g.live;
    g.tempo = percent;
    {
        std::lock_guard<std::mutex> mix(g.mixMutex);
        if (g.processor && !g.live) g.processor->SetTempo(percent);
    }
    if (changed) RemakeAhead(*this);
}

void Player::SetPitch(float semitones) {
    Engine& g = *m;
    const bool changed = semitones != g.pitch;
    g.pitch = semitones;
    {
        std::lock_guard<std::mutex> mix(g.mixMutex);
        if (g.processor) g.processor->SetPitch(semitones);
        if (g.scrub) SetScrubSound(g, *g.scrub);
    }
    if (changed) RemakeAhead(*this);
}

void Player::SetRate(float rate) {
    Engine& g = *m;
    const bool changed = rate != g.rate && !g.live;
    g.rate = rate;
    {
        std::lock_guard<std::mutex> mix(g.mixMutex);
        if (g.processor && !g.live) g.processor->SetRate(rate);
        if (g.scrub) SetScrubSound(g, *g.scrub);
    }
    if (changed) RemakeAhead(*this);
}

void Player::SetGain(float linear) {
    Engine& g = *m;
    g.gain = linear;
}

void Player::SetSmoothTransitions(bool smooth) {
    Engine& g = *m;
    // Over 8 ms at the device's rate, or at once
    g.fadeStep = smooth ? 1.0f / std::max(1.0f, g.outputRate * 0.008f) : 1.0f;
}

int Player::AddDsp(DspProc proc, void* user, int priority) {
    Engine& g = *m;
    std::lock_guard<std::mutex> lock(g.dspMutex);
    Dsp dsp{g.nextDspId++, proc, user, priority};
    // After any of the same priority already there, as BASS did
    auto at = std::find_if(g.dsps.begin(), g.dsps.end(), [&](const Dsp& d) { return d.priority < priority; });
    g.dsps.insert(at, dsp);
    return dsp.id;
}

void Player::RemoveDsp(int id) {
    Engine& g = *m;
    std::lock_guard<std::mutex> lock(g.dspMutex);
    g.dsps.erase(std::remove_if(g.dsps.begin(), g.dsps.end(), [&](const Dsp& d) { return d.id == id; }),
                 g.dsps.end());
}

void Player::SetTap(TapProc proc, void* user) {
    Engine& g = *m;
    std::lock_guard<std::mutex> lock(g.dspMutex);
    g.tap = proc;
    g.tapUser = user;
}

void Player::SetTapBeforeEffects(bool before) {
    Engine& g = *m;
    std::lock_guard<std::mutex> lock(g.dspMutex);
    g.tapBeforeEffects = before;
}

Stats Player::GetStats() {
    Engine& g = *m;
    Stats s;
    s.underruns = g.underruns.load();
    uint32_t minBuffered = g.minBuffered.load();
    s.minBufferedMs = minBuffered == 0xFFFFFFFF ? 0 : minBuffered * 1000.0 / g.outputRate;
    s.maxBlockMs = g.maxBlockMs.load();
    s.periodFrames = static_cast<int>(g.lastPeriod.load());
    s.bufferFrames = static_cast<int>(g.ringFrames);
    return s;
}

void Player::ResetStats() {
    Engine& g = *m;
    g.underruns = 0;
    g.minBuffered = 0xFFFFFFFF;
    g.maxBlockMs = 0;
}

void Player::SetOutputMonitor(TapProc proc, void* user) {
    Engine& g = *m;
    g.monitorUser = user;
    g.monitor = proc;
}

void Player::SetEndHandler(std::function<void()> handler) {
    Engine& g = *m;
    g.endHandler = std::move(handler);
}
void Player::SetStreamTitleHandler(std::function<void()> handler) {
    Engine& g = *m;
    g.titleHandler = std::move(handler);
}

// ---------------------------------------------------------------------------
// The app's player
// ---------------------------------------------------------------------------

Player& DefaultPlayer() {
    // Never destroyed: the app shuts it down itself, and nothing at exit can
    // find it gone
    static Player* player = new Player();
    return *player;
}

bool Init(const std::wstring& deviceName, int bufferMs) { return DefaultPlayer().Init(deviceName, bufferMs); }
void Shutdown() { return DefaultPlayer().Shutdown(); }
bool SwitchDevice(const std::wstring& deviceName, int bufferMs) { return DefaultPlayer().SwitchDevice(deviceName, bufferMs); }
std::wstring CurrentDeviceName() { return DefaultPlayer().CurrentDeviceName(); }
bool UsingDefaultDevice() { return DefaultPlayer().UsingDefaultDevice(); }
int MixSampleRate() { return DefaultPlayer().MixSampleRate(); }
bool Load(std::unique_ptr<Decoder> decoder, TempoAlgorithm algorithm) { return DefaultPlayer().Load(std::move(decoder), algorithm); }
void Unload() { return DefaultPlayer().Unload(); }
bool IsLoaded() { return DefaultPlayer().IsLoaded(); }
const Decoder* Current() { return DefaultPlayer().Current(); }
bool EnsureDeviceRunning() { return DefaultPlayer().EnsureDeviceRunning(); }
void Play() { return DefaultPlayer().Play(); }
void Pause() { return DefaultPlayer().Pause(); }
void Stop() { return DefaultPlayer().Stop(); }
State GetState() { return DefaultPlayer().GetState(); }
double Position() { return DefaultPlayer().Position(); }
double Length() { return DefaultPlayer().Length(); }
bool IsLive() { return DefaultPlayer().IsLive(); }
bool LiveRange(double& start, double& live) { return DefaultPlayer().LiveRange(start, live); }
bool Seek(double seconds) { return DefaultPlayer().Seek(seconds); }
bool StartScrub(ScrubStyle style, int direction, float speed) { return DefaultPlayer().StartScrub(style, direction, speed); }
void SetScrubSpeed(float speed) { return DefaultPlayer().SetScrubSpeed(speed); }
bool StopScrub() { return DefaultPlayer().StopScrub(); }
bool IsScrubbing() { return DefaultPlayer().IsScrubbing(); }
bool SetReverse(bool on) { return DefaultPlayer().SetReverse(on); }
bool IsReverse() { return DefaultPlayer().IsReverse(); }
bool ReleaseScrub() { return DefaultPlayer().ReleaseScrub(); }
bool TapeStop() { return DefaultPlayer().TapeStop(); }
bool IsTapeStopping() { return DefaultPlayer().IsTapeStopping(); }
bool CancelTapeStop() { return DefaultPlayer().CancelTapeStop(); }
bool TapeStart() { return DefaultPlayer().TapeStart(); }
void SetScrubTiming(const ScrubTiming& timing) { DefaultPlayer().SetScrubTiming(timing); }
void SetScrubEndHandler(std::function<void()> handler) { return DefaultPlayer().SetScrubEndHandler(std::move(handler)); }
void SetTempo(float percent) { return DefaultPlayer().SetTempo(percent); }
void SetPitch(float semitones) { return DefaultPlayer().SetPitch(semitones); }
void SetRate(float rate) { return DefaultPlayer().SetRate(rate); }
void SetGain(float linear) { return DefaultPlayer().SetGain(linear); }
void SetSmoothTransitions(bool smooth) { return DefaultPlayer().SetSmoothTransitions(smooth); }
int AddDsp(DspProc proc, void* user, int priority) { return DefaultPlayer().AddDsp(proc, user, priority); }
void RemoveDsp(int id) { return DefaultPlayer().RemoveDsp(id); }
void SetTap(TapProc proc, void* user) { return DefaultPlayer().SetTap(proc, user); }
void SetTapBeforeEffects(bool before) { return DefaultPlayer().SetTapBeforeEffects(before); }
Stats GetStats() { return DefaultPlayer().GetStats(); }
void ResetStats() { return DefaultPlayer().ResetStats(); }
void SetOutputMonitor(TapProc proc, void* user) { return DefaultPlayer().SetOutputMonitor(proc, user); }
void SetEndHandler(std::function<void()> handler) { return DefaultPlayer().SetEndHandler(std::move(handler)); }
void SetStreamTitleHandler(std::function<void()> handler) { return DefaultPlayer().SetStreamTitleHandler(std::move(handler)); }

}  // namespace audio
