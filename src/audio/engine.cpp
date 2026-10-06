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
// reposition the decoder, restarts the tempo processor and empties the ring.
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
    // decode thread reading backward (under decoderMutex): a chunk at a time,
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
            if (g.primed.load()) {
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

// ---------------------------------------------------------------------------
// Decode thread
// ---------------------------------------------------------------------------

// Reading backward, for scrubbing: the chunk before reverseEnd, reversed, into the
// buffer. Each is read with a little more before it, held back and crossfaded
// into the start of the next, so a seek that lands a few samples off never
// clicks; and a little more before that again, decoded and dropped, since some
// formats (MP3) take a frame or two to settle after a seek.
void ReadBackward(Engine& g, std::vector<float>& chunk, std::vector<float>& reversed) {
    std::lock_guard<std::mutex> use(g.decoderMutex);
    if (!g.decoder || !g.reverse) return;
    const int64_t end = g.reverseEnd;
    if (end <= g.reverseFloor) {
        g.pcm.SetEnded();  // back at the start (or the oldest a live stream keeps)
        return;
    }
    const int64_t overlap = g.sourceRate / 50;  // 20 ms
    const int64_t settle = g.sourceRate / 10;   // 100 ms
    const int64_t start = std::max<int64_t>(g.reverseFloor, end - g.reverseChunkFrames);
    const int64_t from = std::max<int64_t>(g.reverseFloor, start - overlap);
    const int64_t seekTo = std::max<int64_t>(g.reverseFloor, from - settle);
    const size_t frames = static_cast<size_t>(end - seekTo);
    chunk.resize(frames * kChannels);
    size_t got = 0;
    if (g.decoder->Seek(static_cast<double>(seekTo) / g.sourceRate)) {
        while (got < frames) {
            int n = g.decoder->Read(chunk.data() + got * kChannels, static_cast<int>(std::min<size_t>(4096, frames - got)));
            if (n <= 0) break;
            got += static_cast<size_t>(n);
        }
    }
    g.reverseEnd = start;
    const size_t keepStart = static_cast<size_t>(from - seekTo);
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
                bool ok;
                {
                    std::lock_guard<std::mutex> use(g.decoderMutex);
                    g.reverse = reverse && g.decoder;
                    if (g.reverse) {
                        // Nothing to do yet: the first chunk ends here
                        g.reverseEnd = static_cast<int64_t>(std::llround(target * g.sourceRate));
                        g.reverseChunkFrames = std::max(chunkFrames, g.sourceRate / 10);
                        g.reverseFloor = floorFrames;
                        g.reverseTail.clear();
                        ok = true;
                    } else {
                        ok = g.decoder && g.decoder->Seek(target);
                    }
                }
                g.pcm.Clear();
                lock.lock();
                g.seekOk = ok;
                g.seekDone = true;
                g.seekFinished.notify_all();
                continue;
            }
            backward = g.reverse;
            // Room for a block, or backward, for a whole chunk and its overlap
            size_t room = backward ? static_cast<size_t>(g.reverseChunkFrames + g.sourceRate / 50) : 4096;
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
        bool titleChanged;
        {
            // Written while the decoder is still held, so a block read from a
            // decoder being unloaded never lands in the next one's buffer.
            std::lock_guard<std::mutex> use(g.decoderMutex);
            if (!g.decoder) continue;
            int got = g.decoder->Read(block.data(), 4096);
            if (got > 0) {
                g.pcm.Write(block.data(), static_cast<size_t>(got));
            } else if (!g.decoder->Starved()) {
                g.pcm.SetEnded();
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
    g.primed = false;
    g.played = 0;
    g.producerEnded = false;
    g.endReported = false;
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
        g.reverse = false;
    }
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
        g.reverse = false;
    }
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
	StopIdleDevice(g);
}

void Player::Stop() {
    Engine& g = *m;
    if (!g.loaded) return;
    FadeOut(g, true);
    g.state = State::Stopped;
    g.fadeOut = false;
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

// Carries on from `seconds`: played normally (scrub null), or by the scrubber,
// reading the source backward in chunks of `reverseChunkFrames` if that is not 0,
// back as far as `floorSeconds`.
bool Reposition(Engine& g, double seconds, std::unique_ptr<Scrubber> scrub, int reverseChunkFrames, double floorSeconds = 0) {
    if (g.length > 0) seconds = std::min(seconds, g.length);
    if (seconds < 0) seconds = 0;
    FadeOut(g, false);
    struct FadeBackIn {
        Engine& g;
        ~FadeBackIn() { g.fadeOut = false; }
    } fadeBackIn{g};  // however the seek ends
    std::unique_lock<std::mutex> mix(g.mixMutex);
    bool ok;
    {
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
    if (g.processor && !g.scrub) g.processor->Restart(seconds);
    ResetOutput(g);
    g.decodeWake.notify_all();
    return ok;
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
    auto scrub = std::make_unique<Scrubber>(style, direction, speed, g.sourceRate, g.outputRate, from);
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
                                            Scrubber::Begin::WindDown, speed);
    int chunk = 0;
    if (direction < 0) chunk = static_cast<int>(g.sourceRate * std::clamp(speed * 0.125, 0.25, 2.0));
    return Reposition(g, from, std::move(scrub), chunk, floor);
}

bool Player::TapeStop() {
    Engine& g = *m;
    if (!g.loaded) return false;
    if (g.live) {
        double start, live;
        if (!LiveRange(start, live)) return false;
    }
    double from = Position();
    auto scrub = std::make_unique<Scrubber>(ScrubStyle::Tape, 1, 1.0f, g.sourceRate, g.outputRate, from,
                                            Scrubber::Begin::TapeStop);
    return Reposition(g, from, std::move(scrub), 0);
}

void Player::SetScrubEndHandler(std::function<void()> handler) {
    Engine& g = *m;
    g.scrubEndHandler = std::move(handler);
}

void Player::SetTempo(float percent) {
    Engine& g = *m;
    g.tempo = percent;
    std::lock_guard<std::mutex> mix(g.mixMutex);
    if (g.processor && !g.live) g.processor->SetTempo(percent);
}

void Player::SetPitch(float semitones) {
    Engine& g = *m;
    g.pitch = semitones;
    std::lock_guard<std::mutex> mix(g.mixMutex);
    if (g.processor) g.processor->SetPitch(semitones);
}

void Player::SetRate(float rate) {
    Engine& g = *m;
    g.rate = rate;
    std::lock_guard<std::mutex> mix(g.mixMutex);
    if (g.processor && !g.live) g.processor->SetRate(rate);
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
bool ReleaseScrub() { return DefaultPlayer().ReleaseScrub(); }
bool TapeStop() { return DefaultPlayer().TapeStop(); }
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
