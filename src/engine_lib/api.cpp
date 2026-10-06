// The library's C interface (include/fastplay_engine/fastplay_engine.h) over
// FastPlay's engine (audio.h's Player) and YouTube support (youtube.h).
//
// Each player has one lock, so calls to it from different threads come one at
// a time, as the engine expects of its "UI thread". Opening runs on a thread of
// its own (YouTube and the network take seconds) and hands its result back
// under the lock if it is still what was asked for. Events go out on the event
// thread (support.cpp), never with a lock held.
//
// A player's state is shared with the threads opening for it and the engine's
// notifications, so it outlives fpe_player_destroy() until they are done with
// it; destroying marks it dead, and nothing dead sends an event.

#include "fastplay_engine/fastplay_engine.h"

#include "audio.h"
#include "effect_chain.h"
#include "recorder.h"
#include "http.h"
#include "support.h"
#include "utils.h"
#include "version.h"
#include "youtube.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr int kDefaultBufferMs = 300;

struct PlayerState {
    std::recursive_mutex mutex;
    audio::Player engine;
    bool engineReady = false;
    bool alive = true;
    std::wstring device;
    int bufferMs = kDefaultBufferMs;
    fpe_event_callback callback = nullptr;
    void* user = nullptr;
    fpe_player* handle = nullptr;

    int request = 0;  // the latest fpe_open()
    int state = FPE_STATE_EMPTY;
    std::string title;
    float gain = 1.0f;
    float tempo = 0.0f, pitch = 0.0f, rate = 1.0f;
    TempoAlgorithm algorithm = TempoAlgorithm::Signalsmith;
    bool smooth = true;

    // After the engine, so they go first: the effects come out of its chain, and
    // the recording off its tap, while it is still there
    audio::EffectChain effects;
    std::unique_ptr<audio::Recorder> recorder;
};

std::mutex g_libraryMutex;
bool g_initialized = false;
bool g_iosAppSession = false;

}  // namespace

// What the app holds
struct fpe_player {
    std::shared_ptr<PlayerState> state;
};

namespace {

// Out on the event thread, if the player is still there by then
void Emit(const std::shared_ptr<PlayerState>& p, int request, int event, const std::string& text) {
    std::weak_ptr<PlayerState> weak = p;
    fpe::Post([weak, request, event, text]() {
        std::shared_ptr<PlayerState> p = weak.lock();
        if (!p) return;
        fpe_event_callback callback;
        void* user;
        fpe_player* handle;
        {
            std::lock_guard<std::recursive_mutex> lock(p->mutex);
            if (!p->alive) return;
            callback = p->callback;
            user = p->user;
            handle = p->handle;
        }
        if (callback) callback(user, handle, request, event, text.c_str());
    });
}

int CopyOut(const std::string& text, char* buffer, int size) {
    if (buffer && size > 0) {
        size_t n = std::min(text.size(), static_cast<size_t>(size - 1));
        // Not in the middle of a UTF-8 character
        while (n > 0 && n < text.size() && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) n--;
        memcpy(buffer, text.data(), n);
        buffer[n] = '\0';
    }
    return static_cast<int>(text.size());
}

// The player's engine and device, the first time they are needed. Lock held.
bool EnsureEngine(const std::shared_ptr<PlayerState>& p) {
    if (p->engineReady) return true;
    if (!p->alive) return false;
    if (!p->engine.Init(p->device, p->bufferMs)) return false;
    p->engine.SetSmoothTransitions(p->smooth);
    p->engine.SetGain(p->gain);
    std::weak_ptr<PlayerState> weak = p;
    p->engine.SetEndHandler([weak]() {
        std::shared_ptr<PlayerState> p = weak.lock();
        if (!p) return;
        int request;
        {
            std::lock_guard<std::recursive_mutex> lock(p->mutex);
            if (p->state != FPE_STATE_PLAYING && p->state != FPE_STATE_PAUSED) return;
            p->state = FPE_STATE_ENDED;
            request = p->request;
        }
        Emit(p, request, FPE_EVENT_ENDED, std::string());
    });
    p->engine.SetScrubEndHandler([weak]() {
        std::shared_ptr<PlayerState> p = weak.lock();
        if (!p) return;
        int request;
        {
            std::lock_guard<std::recursive_mutex> lock(p->mutex);
            if (!p->alive) return;
            request = p->request;
        }
        Emit(p, request, FPE_EVENT_SCRUB_ENDED, std::string());
    });
    p->engine.SetStreamTitleHandler([weak]() {
        std::shared_ptr<PlayerState> p = weak.lock();
        if (!p) return;
        int request;
        std::string title;
        {
            std::lock_guard<std::recursive_mutex> lock(p->mutex);
            const audio::Decoder* decoder = p->engine.Current();
            if (!decoder) return;
            title = decoder->StreamTitle();
            if (title.empty()) return;
            p->title = title;
            request = p->request;
        }
        Emit(p, request, FPE_EVENT_TITLE, title);
    });
    p->engineReady = true;
    return true;
}

// What to call something opened: its tags, its stream title, or its name
std::string TitleOf(const audio::Decoder& decoder, const std::string& source) {
    std::string title = decoder.StreamTitle();
    if (!title.empty()) return title;
    std::string name = decoder.Tag("TITLE");
    std::string artist = decoder.Tag("ARTIST");
    if (!name.empty()) return artist.empty() ? name : artist + " - " + name;
    std::string station = decoder.Tag("icy-name");
    if (!station.empty()) return station;
    std::string path = source.substr(0, source.find_first_of("?#"));
    size_t slash = path.find_last_of("/\\");
    std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    return file.empty() ? source : file;
}

bool IsHttp(const std::wstring& url) {
    return url.compare(0, 7, L"http://") == 0 || url.compare(0, 8, L"https://") == 0;
}

// A YouTube video's id from a link to it, or empty: watch?v=, youtu.be/,
// /shorts/, /live/ and /embed/, on youtube.com, m., music. and youtube-nocookie.
std::wstring YouTubeVideoId(const std::wstring& url) {
    std::wstring lower = url;
    for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
    const bool youtube = lower.find(L"youtube.com/") != std::wstring::npos ||
                         lower.find(L"youtube-nocookie.com/") != std::wstring::npos;
    const bool shortLink = lower.find(L"youtu.be/") != std::wstring::npos;
    if (!youtube && !shortLink) return std::wstring();
    auto take = [&](size_t start) {
        size_t end = url.find_first_of(L"&?#/ ", start);
        std::wstring id = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        // Video ids are 11 characters of A-Z, a-z, 0-9, - and _
        if (id.size() != 11) return std::wstring();
        for (wchar_t c : id) {
            if (!iswalnum(c) && c != L'-' && c != L'_') return std::wstring();
        }
        return id;
    };
    if (shortLink) return take(lower.find(L"youtu.be/") + 9);
    for (const wchar_t* marker : {L"/shorts/", L"/live/", L"/embed/", L"/v/"}) {
        size_t at = lower.find(marker);
        if (at != std::wstring::npos) return take(at + wcslen(marker));
    }
    for (const wchar_t* marker : {L"?v=", L"&v="}) {
        size_t at = lower.find(marker);
        if (at != std::wstring::npos) return take(at + 3);
    }
    return std::wstring();
}

bool YouTubeSupported() {
#if defined(_WIN32) || (defined(__APPLE__) && !TARGET_OS_IPHONE) || defined(__linux__)
    return true;
#else
    return false;
#endif
}

// Whether `request` is still the one wanted. Lock held.
bool Current(const PlayerState& p, int request) { return p.alive && p.request == request; }

// On the open's own thread
void OpenWorker(std::shared_ptr<PlayerState> p, int request, std::string source, bool autoplay) {
    std::wstring target = Utf8ToWide(source);
    std::wstring error;
    std::string title;

    auto stillWanted = [&]() {
        std::lock_guard<std::recursive_mutex> lock(p->mutex);
        return Current(*p, request);
    };
    auto fail = [&](const std::wstring& why) {
        {
            std::lock_guard<std::recursive_mutex> lock(p->mutex);
            if (!Current(*p, request)) return;
            p->state = FPE_STATE_EMPTY;
        }
        Emit(p, request, FPE_EVENT_FAILED, WideToUtf8(why));
    };

    // A YouTube video: yt-dlp finds its audio stream (or downloads it)
    const std::wstring videoId = YouTubeVideoId(target);
    if (!videoId.empty()) {
        if (!YouTubeSupported()) {
            fail(L"YouTube videos cannot be played here.");
            return;
        }
        YouTubeMedia media;
        std::weak_ptr<PlayerState> weak = p;
        auto status = [weak, request](const std::wstring& message) {
            std::shared_ptr<PlayerState> p = weak.lock();
            if (!p) return;
            {
                std::lock_guard<std::recursive_mutex> lock(p->mutex);
                if (!Current(*p, request)) return;
            }
            Emit(p, request, FPE_EVENT_STATUS, WideToUtf8(message));
        };
        if (!YouTubePrepare(videoId, media, error, status)) {
            fail(error.empty() ? L"The video could not be played." : error);
            return;
        }
        target = media.url.empty() ? media.file : media.url;
        title = WideToUtf8(media.channel.empty() ? media.title : media.channel + L" - " + media.title);
    }
    if (!stillWanted()) return;

    std::unique_ptr<audio::Decoder> decoder = audio::OpenDecoder(target, error);
    // A link that redirects somewhere FFmpeg will not follow (https to http, say):
    // follow it here and try where it ends up
    if (!decoder && IsHttp(target) && stillWanted()) {
        HttpOptions options;
        options.readBody = false;
        HttpResult result = HttpGet(target, options);
        if (result.completed && !result.finalUrl.empty() && result.finalUrl != target) {
            std::wstring retryError;
            decoder = audio::OpenDecoder(result.finalUrl, retryError);
        }
    }
    if (!decoder) {
        fail(error.empty() ? L"It could not be played." : error);
        return;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(p->mutex);
        if (!Current(*p, request)) return;
        if (title.empty()) title = TitleOf(*decoder, source);
        p->engine.SetTempo(p->tempo);
        p->engine.SetPitch(p->pitch);
        p->engine.SetRate(p->rate);
        p->engine.SetGain(p->gain);
        if (!p->engine.Load(std::move(decoder), p->algorithm)) {
            p->state = FPE_STATE_EMPTY;
            Emit(p, request, FPE_EVENT_FAILED, "The output device could not play it.");
            return;
        }
        // The effects, fresh for it
        std::wstring effectError;
        if (!p->effects.Apply(&effectError) && !effectError.empty()) {
            Emit(p, request, FPE_EVENT_STATUS, "3D audio could not start: " + WideToUtf8(effectError));
        }
        p->title = title;
        if (autoplay) {
            p->engine.Play();
            p->state = FPE_STATE_PLAYING;
        } else {
            p->state = FPE_STATE_PAUSED;
        }
    }
    Emit(p, request, FPE_EVENT_OPENED, title);
}

// The state behind a handle, locked: `Locked l(player); if (!l) return ...;`
struct Locked {
    std::shared_ptr<PlayerState> p;
    std::unique_lock<std::recursive_mutex> lock;
    explicit Locked(fpe_player* player) {
        if (!player || !player->state) return;
        p = player->state;
        lock = std::unique_lock<std::recursive_mutex>(p->mutex);
        if (!p->alive) {
            lock.unlock();
            p.reset();
        }
    }
    explicit operator bool() const { return p != nullptr; }
    PlayerState* operator->() const { return p.get(); }
};

void Play(PlayerState& p) {
    if (p.state == FPE_STATE_PAUSED) {
        p.engine.Play();
        p.state = FPE_STATE_PLAYING;
    } else if (p.state == FPE_STATE_ENDED) {
        // From the start again
        p.engine.Seek(0.0);
        p.engine.Play();
        p.state = FPE_STATE_PLAYING;
    }
}

void Pause(PlayerState& p) {
    if (p.state != FPE_STATE_PLAYING) return;
    p.engine.Pause();
    p.state = FPE_STATE_PAUSED;
}

int Seek(PlayerState& p, double seconds) {
    if (!p.engineReady || !p.engine.IsLoaded() || p.engine.IsLive()) return 0;
    const double length = p.engine.Length();
    seconds = std::max(0.0, seconds);
    if (length > 0) seconds = std::min(seconds, length);
    if (!p.engine.Seek(seconds)) return 0;
    if (!p.smooth) p.effects.ClearTails();
    // Back from the end: it can play again
    if (p.state == FPE_STATE_ENDED && (length <= 0 || seconds < length)) {
        p.engine.Play();
        p.state = FPE_STATE_PLAYING;
    }
    return 1;
}

}  // namespace

extern "C" {

// ---- the library ----

FPE_API int fpe_init(const fpe_config* config) {
    if (!config || config->api_version != FPE_API_VERSION) return 0;
    std::lock_guard<std::mutex> lock(g_libraryMutex);
    if (g_initialized) return 1;
    fpe::SetDataDirectory(config->data_dir ? Utf8ToWide(config->data_dir) : std::wstring());
    g_iosAppSession = config->ios_session == FPE_IOS_SESSION_APP;
    audio::SetIosAudioSession(!g_iosAppSession);
    fpe::StartEventThread();
    g_initialized = true;
    return 1;
}

FPE_API void fpe_shutdown(void) {
    {
        std::lock_guard<std::mutex> lock(g_libraryMutex);
        if (!g_initialized) return;
        g_initialized = false;
    }
    fpe::StopEventThread();
}

FPE_API const char* fpe_version(void) {
    static const std::string version =
        std::string("FastPlay Engine ") + APP_VERSION + " (" + audio::DecoderVersion() + ")";
    return version.c_str();
}

// ---- devices ----

FPE_API int fpe_device_count(void) { return static_cast<int>(audio::ListDevices().size()); }

FPE_API int fpe_device_name(int index, char* buffer, int size) {
    std::vector<audio::Device> devices = audio::ListDevices();
    if (index < 0 || index >= static_cast<int>(devices.size())) return -1;
    return CopyOut(WideToUtf8(devices[static_cast<size_t>(index)].name), buffer, size);
}

// ---- YouTube ----

FPE_API int fpe_youtube_supported(void) { return YouTubeSupported() ? 1 : 0; }

FPE_API int fpe_is_youtube_url(const char* url) {
    return url && !YouTubeVideoId(Utf8ToWide(url)).empty() ? 1 : 0;
}

FPE_API void fpe_prepare_youtube(void) {
    if (!YouTubeSupported()) return;
    {
        std::lock_guard<std::mutex> lock(g_libraryMutex);
        if (!g_initialized) return;
    }
    std::thread([]() { YouTubePrepareTools(); }).detach();
}

// ---- players ----

FPE_API fpe_player* fpe_player_create(const fpe_player_config* config) {
    {
        std::lock_guard<std::mutex> lock(g_libraryMutex);
        if (!g_initialized) return nullptr;
    }
    auto* player = new fpe_player();
    player->state = std::make_shared<PlayerState>();
    PlayerState& p = *player->state;
    p.handle = player;
    p.effects.Attach(&p.engine);
    if (config) {
        p.device = config->device ? Utf8ToWide(config->device) : std::wstring();
        if (config->buffer_ms > 0) p.bufferMs = config->buffer_ms;
        p.callback = config->on_event;
        p.user = config->user;
    }
    return player;
}

FPE_API void fpe_player_destroy(fpe_player* player) {
    if (!player) return;
    {
        Locked p(player);
        if (p) {
            p->alive = false;  // whatever is opening is no longer wanted, and nothing more is said
            p->request++;
            if (p->recorder) {
                p->engine.SetTap(nullptr, nullptr);
                p->recorder.reset();  // the file finished
            }
            p->effects.Remove();
            if (p->engineReady) {
                p->engine.Shutdown();
                p->engineReady = false;
            }
            p->callback = nullptr;
        }
    }
    // An event already on its way has been delivered by the time this returns
    fpe::Flush();
    delete player;
}

FPE_API int fpe_set_device(fpe_player* player, const char* name) {
    Locked p(player);
    if (!p) return 0;
    p->device = name ? Utf8ToWide(name) : std::wstring();
    if (!p->engineReady) return 1;  // used when its engine starts
    p->request++;
    p->effects.Remove();
    p->engine.Unload();
    p->state = FPE_STATE_EMPTY;
    return p->engine.SwitchDevice(p->device, p->bufferMs) ? 1 : 0;
}

FPE_API int fpe_open(fpe_player* player, const char* url_or_path, int autoplay) {
    Locked p(player);
    if (!p || !url_or_path || !*url_or_path) return 0;
    const int request = ++p->request;
    if (!EnsureEngine(p.p)) {
        p->state = FPE_STATE_EMPTY;
        Emit(p.p, request, FPE_EVENT_FAILED, "The output device could not be opened.");
        return request;
    }
    p->effects.Remove();
    p->engine.Unload();
    p->state = FPE_STATE_OPENING;
    p->title.clear();
    std::thread(OpenWorker, p.p, request, std::string(url_or_path), autoplay != 0).detach();
    return request;
}

FPE_API void fpe_close(fpe_player* player) {
    Locked p(player);
    if (!p) return;
    p->request++;
    if (p->engineReady) {
        p->effects.Remove();
        p->engine.Unload();
    }
    p->state = FPE_STATE_EMPTY;
    p->title.clear();
}

FPE_API int fpe_state(fpe_player* player) {
    Locked p(player);
    return p ? p->state : FPE_STATE_EMPTY;
}

FPE_API void fpe_play(fpe_player* player) {
    Locked p(player);
    if (p) Play(*p.p);
}

FPE_API void fpe_pause(fpe_player* player) {
    Locked p(player);
    if (p) Pause(*p.p);
}

FPE_API int fpe_toggle_pause(fpe_player* player) {
    Locked p(player);
    if (!p) return 0;
    if (p->state == FPE_STATE_PLAYING) {
        Pause(*p.p);
    } else {
        Play(*p.p);
    }
    return p->state == FPE_STATE_PLAYING ? 1 : 0;
}

FPE_API double fpe_position(fpe_player* player) {
    Locked p(player);
    return p && p->engineReady && p->engine.IsLoaded() ? p->engine.Position() : 0.0;
}

FPE_API double fpe_length(fpe_player* player) {
    Locked p(player);
    return p && p->engineReady && p->engine.IsLoaded() ? p->engine.Length() : 0.0;
}

FPE_API int fpe_is_live(fpe_player* player) {
    Locked p(player);
    return p && p->engineReady && p->engine.IsLoaded() && p->engine.IsLive() ? 1 : 0;
}

FPE_API int fpe_seek(fpe_player* player, double seconds) {
    Locked p(player);
    return p ? Seek(*p.p, seconds) : 0;
}

FPE_API int fpe_seek_by(fpe_player* player, double delta_seconds) {
    Locked p(player);
    if (!p || !p->engineReady || !p->engine.IsLoaded()) return 0;
    return Seek(*p.p, p->engine.Position() + delta_seconds);
}

FPE_API void fpe_set_volume(fpe_player* player, float gain) {
    Locked p(player);
    if (!p) return;
    p->gain = std::max(0.0f, gain);
    if (p->engineReady) p->engine.SetGain(p->gain);
}

FPE_API void fpe_set_tempo(fpe_player* player, float percent) {
    Locked p(player);
    if (!p) return;
    p->tempo = percent;
    if (p->engineReady) p->engine.SetTempo(percent);
}

FPE_API void fpe_set_pitch(fpe_player* player, float semitones) {
    Locked p(player);
    if (!p) return;
    p->pitch = semitones;
    if (p->engineReady) p->engine.SetPitch(semitones);
}

FPE_API void fpe_set_rate(fpe_player* player, float rate) {
    Locked p(player);
    if (!p) return;
    p->rate = rate > 0 ? rate : 1.0f;
    if (p->engineReady) p->engine.SetRate(p->rate);
}

FPE_API int fpe_title(fpe_player* player, char* buffer, int size) {
    Locked p(player);
    return CopyOut(p ? p->title : std::string(), buffer, size);
}

FPE_API int fpe_tag(fpe_player* player, const char* name, char* buffer, int size) {
    Locked p(player);
    std::string value;
    if (p && name && p->engineReady) {
        if (const audio::Decoder* d = p->engine.Current()) value = d->Tag(name);
    }
    return CopyOut(value, buffer, size);
}

FPE_API int fpe_get_stream_info(fpe_player* player, fpe_stream_info* info) {
    if (!info) return 0;
    *info = fpe_stream_info{};
    Locked p(player);
    if (!p || !p->engineReady) return 0;
    const audio::Decoder* d = p->engine.Current();
    if (!d) return 0;
    CopyOut(d->CodecName(), info->codec, static_cast<int>(sizeof info->codec));
    info->bitrate_kbps = d->Bitrate();
    info->vbr = d->IsVbr() ? 1 : 0;
    info->channels = d->SourceChannels();
    info->sample_rate = d->SourceSampleRate();
    info->bits = d->SourceBits();
    return 1;
}

FPE_API int fpe_chapter_count(fpe_player* player) {
    Locked p(player);
    if (!p || !p->engineReady) return 0;
    const audio::Decoder* d = p->engine.Current();
    return d ? static_cast<int>(d->Chapters().size()) : 0;
}

FPE_API int fpe_chapter(fpe_player* player, int index, double* start, char* title, int size) {
    Locked p(player);
    if (!p || !p->engineReady) return -1;
    const audio::Decoder* d = p->engine.Current();
    if (!d) return -1;
    std::vector<Chapter> chapters = d->Chapters();
    if (index < 0 || index >= static_cast<int>(chapters.size())) return -1;
    const Chapter& c = chapters[static_cast<size_t>(index)];
    if (start) *start = c.position;
    return CopyOut(WideToUtf8(c.name), title, size);
}

FPE_API void fpe_set_tempo_algorithm(fpe_player* player, int algorithm) {
    Locked p(player);
    if (p && (algorithm == 1 || algorithm == 2)) p->algorithm = static_cast<TempoAlgorithm>(algorithm);
}

FPE_API void fpe_set_smooth_transitions(fpe_player* player, int on) {
    Locked p(player);
    if (!p) return;
    p->smooth = on != 0;
    if (p->engineReady) p->engine.SetSmoothTransitions(p->smooth);
}

// ---- live streams ----

FPE_API void fpe_set_live_rewind(int seconds) { audio::SetLiveRewindSeconds(seconds); }

FPE_API int fpe_live_range(fpe_player* player, double* oldest, double* live) {
    Locked p(player);
    if (!p || !p->engineReady) return 0;
    double a = 0, b = 0;
    if (!p->engine.LiveRange(a, b)) return 0;
    if (oldest) *oldest = a;
    if (live) *live = b;
    return 1;
}

// ---- scrubbing ----

FPE_API int fpe_scrub_start(fpe_player* player, int style, int direction, float speed) {
    Locked p(player);
    if (!p || !p->engineReady || !p->engine.IsLoaded()) return 0;
    audio::ScrubStyle s = style == FPE_SCRUB_SPRING ? audio::ScrubStyle::Spring : audio::ScrubStyle::Tape;
    if (!p->engine.StartScrub(s, direction < 0 ? -1 : 1, speed)) return 0;
    // It is heard, paused or not
    if (p->state == FPE_STATE_PAUSED || p->state == FPE_STATE_ENDED) {
        p->engine.Play();
        p->state = FPE_STATE_PLAYING;
    }
    return 1;
}

FPE_API void fpe_scrub_speed(fpe_player* player, float speed) {
    Locked p(player);
    if (p && p->engineReady) p->engine.SetScrubSpeed(speed);
}

FPE_API int fpe_scrub_stop(fpe_player* player) {
    Locked p(player);
    return p && p->engineReady && p->engine.StopScrub() ? 1 : 0;
}

FPE_API int fpe_scrub_release(fpe_player* player) {
    Locked p(player);
    return p && p->engineReady && p->engine.ReleaseScrub() ? 1 : 0;
}

FPE_API int fpe_tape_stop(fpe_player* player) {
    Locked p(player);
    return p && p->engineReady && p->state == FPE_STATE_PLAYING && p->engine.TapeStop() ? 1 : 0;
}

// ---- effects ----

FPE_API int fpe_effect_count(void) { return static_cast<int>(DSPEffectType::COUNT); }

FPE_API const char* fpe_effect_key(int index) {
    if (index < 0 || index >= static_cast<int>(DSPEffectType::COUNT)) return nullptr;
    return audio::EffectKey(static_cast<DSPEffectType>(index));
}

FPE_API int fpe_set_effect(fpe_player* player, const char* effect, int on) {
    DSPEffectType type;
    if (!effect || !audio::EffectFromKey(effect, type)) return 0;
    Locked p(player);
    if (!p) return 0;
    p->effects.Enable(type, on != 0);
    return p->effects.Enabled(type) == (on != 0) ? 1 : 0;  // 3D audio may not start
}

FPE_API int fpe_effect_enabled(fpe_player* player, const char* effect) {
    DSPEffectType type;
    if (!effect || !audio::EffectFromKey(effect, type)) return 0;
    Locked p(player);
    return p && p->effects.Enabled(type) ? 1 : 0;
}

FPE_API int fpe_set_reverb_type(fpe_player* player, int type) {
    Locked p(player);
    if (!p || type < 0 || type > 2) return 0;
    p->effects.SetReverbAlgorithm(type);
    return 1;
}

FPE_API int fpe_param_count(void) { return static_cast<int>(audio::ParamDefs().size()); }

FPE_API int fpe_param_at(int index, fpe_param_info* info) {
    const std::vector<ParamDef>& defs = audio::ParamDefs();
    if (!info || index < 0 || index >= static_cast<int>(defs.size())) return 0;
    const ParamDef& d = defs[static_cast<size_t>(index)];
    info->key = audio::ParamKey(d.id);
    info->name = d.name;
    info->unit = d.unit;
    info->effect = static_cast<int>(d.dspEffect) < 0 ? "" : audio::EffectKey(d.dspEffect);
    info->min_value = d.minValue;
    info->max_value = d.maxValue;
    info->step = d.step;
    info->default_value = d.defaultValue;
    info->choices = static_cast<int>(audio::ParamChoices(d.id).size());
    return 1;
}

FPE_API int fpe_param_choice(const char* key, int value, char* buffer, int size) {
    ParamId id;
    if (!key || !audio::ParamFromKey(key, id)) return -1;
    std::vector<std::string> names = audio::ParamChoices(id);
    // Values count from the parameter's minimum (0 for all of them)
    const ParamDef* def = audio::FindParamDef(id);
    int i = value - (def ? static_cast<int>(def->minValue) : 0);
    if (i < 0 || i >= static_cast<int>(names.size())) return -1;
    return CopyOut(names[static_cast<size_t>(i)], buffer, size);
}

FPE_API int fpe_set_param(fpe_player* player, const char* key, float value) {
    ParamId id;
    if (!key || !audio::ParamFromKey(key, id)) return 0;
    switch (id) {
        case ParamId::Volume: fpe_set_volume(player, value); return 1;
        case ParamId::Tempo: fpe_set_tempo(player, value); return 1;
        case ParamId::Pitch: fpe_set_pitch(player, value); return 1;
        case ParamId::Rate: fpe_set_rate(player, value); return 1;
        default: break;
    }
    Locked p(player);
    if (!p) return 0;
    p->effects.Set(id, value);
    return 1;
}

FPE_API float fpe_get_param(fpe_player* player, const char* key) {
    ParamId id;
    if (!key || !audio::ParamFromKey(key, id)) return 0.0f;
    Locked p(player);
    if (!p) return 0.0f;
    switch (id) {
        case ParamId::Volume: return p->gain;
        case ParamId::Tempo: return p->tempo;
        case ParamId::Pitch: return p->pitch;
        case ParamId::Rate: return p->rate;
        default: return p->effects.Get(id);
    }
}

FPE_API void fpe_set_eq_frequencies(fpe_player* player, float bass, float mid, float treble) {
    Locked p(player);
    if (p) p->effects.SetEqFrequencies(bass, mid, treble);
}

FPE_API int fpe_load_impulse_response(fpe_player* player, const char* path) {
    Locked p(player);
    if (!p || !path) return 0;
    std::wstring error;
    return p->effects.LoadImpulseResponse(Utf8ToWide(path), error) ? 1 : 0;
}

// ---- recording ----

namespace {
void RecordingTap(const float* samples, int frames, int, int, void* user) {
    static_cast<audio::Recorder*>(user)->Write(samples, frames);
}
}  // namespace

FPE_API int fpe_record_start(fpe_player* player, const char* path, int format, int bitrate_kbps,
                             int before_effects) {
    Locked p(player);
    if (!p || !path || !*path || format < 0 || format > 3) return 0;
    if (!p->engineReady || !p->engine.IsLoaded()) return 0;  // its rate is the device's
    if (p->recorder) {
        p->engine.SetTap(nullptr, nullptr);
        p->recorder.reset();
    }
    std::wstring error;
    p->recorder = audio::Recorder::Start(Utf8ToWide(path), static_cast<audio::RecordFormat>(format),
                                         bitrate_kbps > 0 ? bitrate_kbps : 192, p->engine.MixSampleRate(), error);
    if (!p->recorder) return 0;
    p->engine.SetTapBeforeEffects(before_effects != 0);
    p->engine.SetTap(RecordingTap, p->recorder.get());
    return 1;
}

FPE_API void fpe_record_stop(fpe_player* player) {
    Locked p(player);
    if (!p || !p->recorder) return;
    // No more blocks after this; then the file is finished
    p->engine.SetTap(nullptr, nullptr);
    p->recorder.reset();
}

FPE_API int fpe_recording(fpe_player* player) {
    Locked p(player);
    return p && p->recorder ? 1 : 0;
}

}  // extern "C"
