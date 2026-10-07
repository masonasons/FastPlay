// Decoding through FFmpeg: every file format FastPlay plays except xHE-AAC, and
// internet streams (HTTP with Shoutcast/Icecast titles, HTTPS, HLS).
//
// The decoded audio is converted to interleaved float stereo at the source's
// own rate: a mono source is copied to both sides, more channels are mixed down.
// Seeking is exact to the sample: FFmpeg seeks to a packet at or before the time,
// and the audio before it is decoded and dropped.
//
// A live stream can be kept for rewinding: a thread of its own reads the stream
// and keeps its packets (compressed, as they came) for so many minutes, each
// given a time from where the stream was opened, with the title changes; playing
// reads from what is kept, and seeking moves about in it.

#include "audio.h"
#include "audio_internal.h"
#include "http_cache.h"
#include "http.h"
#include "utils.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace audio {
namespace {

// FFmpeg would log to stderr, which a windowed program has no use for
void QuietLogging() {
    static const bool quiet = (av_log_set_level(AV_LOG_QUIET), true);
    (void)quiet;
}

// Seconds a network read may stall before the stream is given up
const int kNetworkTimeoutSeconds = 20;

// How much of a live stream to keep for rewinding (SetLiveRewindSeconds)
std::atomic<int> g_liveRewindSeconds{0};

std::string Upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

// FFmpeg's names for the common tags FastPlay asks for
const char* FfmpegKey(const std::string& name) {
    if (name == "TRACKNUMBER" || name == "TRACK") return "TRACK";
    if (name == "YEAR" || name == "DATE") return "DATE";
    return nullptr;
}

// "StreamTitle='Artist - Title';StreamUrl='';" -> "Artist - Title". The value ends
// at "';" rather than the first quote: titles have apostrophes ("Don't Stop").
std::string IcyField(const std::string& meta, const char* key) {
    std::string search = std::string(key) + "='";
    size_t start = meta.find(search);
    if (start == std::string::npos) return "";
    start += search.size();
    size_t end = meta.find("';", start);
    if (end == std::string::npos) {
        end = meta.size();
        while (end > start && (meta[end - 1] == '\0' || meta[end - 1] == ';' || meta[end - 1] == ' ')) end--;
        if (end > start && meta[end - 1] == '\'') end--;
    }
    return meta.substr(start, end - start);
}

// A video's sound is all that is wanted. Video, subtitles and data are set aside
// before the streams are probed: nothing here decodes them, and the probe would
// wait on them, so long that a stream of several videos never got as far as
// finding out what its sound was. An HLS stream of several variants (a video
// in several sizes) keeps one, whose sound is played and whose segments alone
// are fetched: the smallest of at least 400 kbps, as the very smallest often
// cut the sound too, else the biggest. Returns a stream of the variant kept,
// for av_find_best_stream() to keep to (-1 when there is no choosing).
int SetAsideAllButSound(AVFormatContext* format) {
    for (unsigned i = 0; i < format->nb_streams; i++) {
        if (format->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            format->streams[i]->discard = AVDISCARD_ALL;
        }
    }
    if (format->nb_programs < 2) return -1;
    const AVProgram* chosen = nullptr;
    int64_t chosenRate = 0;
    const int64_t kEnough = 400000;
    for (unsigned p = 0; p < format->nb_programs; p++) {
        const AVProgram* program = format->programs[p];
        bool hasSound = false;
        for (unsigned k = 0; k < program->nb_stream_indexes; k++) {
            if (format->streams[program->stream_index[k]]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) hasSound = true;
        }
        if (!hasSound) continue;
        const AVDictionaryEntry* entry = av_dict_get(program->metadata, "variant_bitrate", nullptr, 0);
        const int64_t rate = entry ? strtoll(entry->value, nullptr, 10) : 0;
        bool better;
        if (!chosen) {
            better = true;
        } else if (rate >= kEnough && chosenRate >= kEnough) {
            better = rate < chosenRate;
        } else {
            better = rate > chosenRate;  // the biggest, until one is big enough
        }
        if (better) {
            chosen = program;
            chosenRate = rate;
        }
    }
    if (!chosen) return -1;
    int related = -1;
    for (unsigned i = 0; i < format->nb_streams; i++) {
        bool inChosen = false;
        for (unsigned k = 0; k < chosen->nb_stream_indexes; k++) {
            if (chosen->stream_index[k] == i) inChosen = true;
        }
        if (!inChosen) {
            format->streams[i]->discard = AVDISCARD_ALL;
        } else if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            related = static_cast<int>(i);
        }
    }
    return related;
}

class FfmpegDecoder : public Decoder {
public:
    ~FfmpegDecoder() override {
        m_abort = true;
        if (m_reader.joinable()) m_reader.join();
        for (Kept& kept : m_kept) av_packet_free(&kept.packet);
        if (m_cache) m_cache->Abort();
        swr_free(&m_swr);
        av_frame_free(&m_frame);
        av_packet_free(&m_packet);
        avcodec_free_context(&m_codec);
        avformat_close_input(&m_format);  // not the reader, which was opened here (or is the cache's own)
        m_cache.reset();
        if (m_io) avio_closep(&m_io);
    }

    bool Open(const std::wstring& pathOrUrl, std::wstring& error) {
        const bool network = IsNetworkPath(pathOrUrl);
        std::string url = WideToUtf8(pathOrUrl);

        m_format = avformat_alloc_context();
        if (!m_format) return Fail(error, L"Out of memory.");
        m_format->interrupt_callback.callback = &FfmpegDecoder::Interrupted;
        m_format->interrupt_callback.opaque = this;

        AVDictionary* options = nullptr;
        if (network) {
            // Look at a second of a stream, not FFmpeg's five, before playing it
            m_format->probesize = 128 * 1024;
            m_format->max_analyze_duration = AV_TIME_BASE;
            av_dict_set(&options, "user_agent", UserAgent().c_str(), 0);
            av_dict_set(&options, "icy", "1", 0);  // ask Shoutcast/Icecast for titles
            av_dict_set(&options, "reconnect", "1", 0);
            av_dict_set(&options, "reconnect_streamed", "1", 0);
            av_dict_set(&options, "reconnect_on_network_error", "1", 0);
            av_dict_set(&options, "reconnect_delay_max", "10", 0);
            av_dict_set(&options, "rw_timeout", std::to_string(kNetworkTimeoutSeconds * 1000000LL).c_str(), 0);
        }
        Arm();
        if (network) {
            // The connection is opened here, not by the demuxer, so that a file (as
            // against a live stream or an HLS playlist) is read through its local
            // copy (http_cache.h) from the first byte: a seek into what has come is
            // then at once, rather than a new request to the server every time.
            //
            // The copy used to be put in the demuxer's place once it had opened the
            // file. But a demuxer may keep the reader it was opened with (MP4's does,
            // one for each track) and go on reading the connection itself, while the
            // copy's download thread was using it too: two threads on one connection,
            // which crashed wherever it happened to give way.
            const int opened = avio_open2(&m_io, url.c_str(), AVIO_FLAG_READ, &m_format->interrupt_callback, &options);
            if (opened < 0) {
                av_dict_free(&options);
                avformat_free_context(m_format);
                m_format = nullptr;
                return Fail(error, L"Could not open the stream: " + ErrorText(opened));
            }
            const int64_t size = avio_size(m_io);
            if (size > 0 && (m_io->seekable & AVIO_SEEKABLE_NORMAL) && !IsHlsPlaylist(m_io)) {
                m_cache = HttpCache::Create(m_io, size, avio_tell(m_io), [this]() { Arm(); });
                if (m_cache) {
                    m_io = nullptr;  // the copy's own now
                    // An MP3 without a seek table is otherwise sought by reading every
                    // frame up to the time, which over the network is downloading all
                    // of it in between; from its bitrate it goes straight there instead
                    // (exact for a constant bitrate, close for a variable one)
                    m_format->flags |= AVFMT_FLAG_FAST_SEEK;
                }
            }
            m_format->pb = m_cache ? m_cache->Io() : m_io;
        }
        int rc = avformat_open_input(&m_format, url.c_str(), nullptr, &options);
        av_dict_free(&options);
        if (rc < 0) {
            m_format = nullptr;  // freed by avformat_open_input
            return Fail(error, network ? L"Could not open the stream: " + ErrorText(rc) : OpenErrorText(rc));
        }
        Arm();
        const int related = SetAsideAllButSound(m_format);
        if (avformat_find_stream_info(m_format, nullptr) < 0 && network) {
            // Streams can still play; their details show up once audio flows.
        }
        m_stream = av_find_best_stream(m_format, AVMEDIA_TYPE_AUDIO, -1, related, nullptr, 0);
        if (m_stream < 0) return Fail(error, L"There is no audio in it.");
        AVStream* stream = m_format->streams[m_stream];
        for (unsigned i = 0; i < m_format->nb_streams; i++) {
            if (static_cast<int>(i) != m_stream) m_format->streams[i]->discard = AVDISCARD_ALL;
        }

        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) return Fail(error, L"Its audio format is not supported.");
        m_codec = avcodec_alloc_context3(codec);
        if (!m_codec || avcodec_parameters_to_context(m_codec, stream->codecpar) < 0) {
            return Fail(error, L"Out of memory.");
        }
        m_codec->pkt_timebase = stream->time_base;
        if (avcodec_open2(m_codec, codec, nullptr) < 0) return Fail(error, L"Its audio could not be decoded.");
        if (m_codec->sample_rate <= 0) return Fail(error, L"Its audio has no sample rate.");

        m_rate = m_codec->sample_rate;
        m_sourceChannels = m_codec->ch_layout.nb_channels;
        m_sourceRate = m_codec->sample_rate;
        m_bits = stream->codecpar->bits_per_raw_sample > 0 ? stream->codecpar->bits_per_raw_sample
                                                            : stream->codecpar->bits_per_coded_sample;
        const AVCodecDescriptor* desc = avcodec_descriptor_get(stream->codecpar->codec_id);
        if (desc && (desc->props & AV_CODEC_PROP_LOSSY)) m_bits = 0;
        m_codecName = codec->name;

        m_packet = av_packet_alloc();
        m_frame = av_frame_alloc();
        if (!m_packet || !m_frame) return Fail(error, L"Out of memory.");

        // Length, and whether this is live (no length and no seeking).
        if (m_format->duration > 0) {
            m_length = m_format->duration / static_cast<double>(AV_TIME_BASE);
        } else if (stream->duration > 0) {
            m_length = stream->duration * av_q2d(stream->time_base);
        }
        const bool seekable = m_format->pb && (m_format->pb->seekable & AVIO_SEEKABLE_NORMAL);
        m_live = network && (m_length <= 0 || !seekable);
        if (m_live) m_length = 0;

        if (m_format->start_time != AV_NOPTS_VALUE) m_startTime = m_format->start_time / static_cast<double>(AV_TIME_BASE);

        m_nominalBitrate = static_cast<int>((stream->codecpar->bit_rate > 0 ? stream->codecpar->bit_rate
                                                                            : m_format->bit_rate) / 1000);
        ReadInfo();

        const int keep = g_liveRewindSeconds.load();
        if (m_live && keep > 0) {
            m_rewind = true;
            m_keepSeconds = keep;
            m_startTime = 0;  // times are the kept timeline's
            // A packet that doesn't say how long it is: a frame of the codec's
            const int frameSize = stream->codecpar->frame_size > 0 ? stream->codecpar->frame_size : 1024;
            m_defaultPacketSeconds = static_cast<double>(frameSize) / m_rate;
            {
                std::lock_guard<std::mutex> lock(m_infoMutex);
                m_titleTimes.push_back({0.0, m_streamTitle});
            }
            m_reader = std::thread(&FfmpegDecoder::KeepStream, this);
        }
        return true;
    }

    int SampleRate() const override { return m_rate; }

    int Read(float* out, int frames) override {
        int written = 0;
        while (written < frames) {
            size_t available = (m_pcm.size() - m_pcmRead) / 2;
            if (available > 0) {
                size_t n = std::min<size_t>(available, static_cast<size_t>(frames - written));
                std::memcpy(out + written * 2, m_pcm.data() + m_pcmRead, n * 2 * sizeof(float));
                m_pcmRead += n * 2;
                written += static_cast<int>(n);
                continue;
            }
            m_pcm.clear();
            m_pcmRead = 0;
            if (!DecodeMore()) break;
        }
        return written;
    }

    bool Seek(double seconds) override {
        if ((m_live && !m_rewind) || !m_format) return false;
        if (seconds < 0) seconds = 0;
        // From a little before, dropped: the first frame after a seek decodes
        // wrongly in some formats (MP3's bit reservoir, AAC's overlapping windows)
        const double settle = 0.1;
        if (m_rewind) {
            // Within what is kept: from the first packet that ends after the time
            std::lock_guard<std::mutex> lock(m_keptMutex);
            if (m_kept.empty()) return false;
            seconds = std::clamp(seconds, m_kept.front().start, m_keptEnd);
            const double from = std::max(m_kept.front().start, seconds - settle);
            auto it = std::upper_bound(m_kept.begin(), m_kept.end(), from,
                                       [](double t, const Kept& kept) { return t < kept.end; });
            m_cursor = m_keptBase + static_cast<uint64_t>(it - m_kept.begin());
        } else {
            int64_t target = static_cast<int64_t>((std::max(0.0, seconds - settle) + m_startTime) * AV_TIME_BASE);
            Arm();
            if (avformat_seek_file(m_format, -1, INT64_MIN, target, target, 0) < 0 &&
                av_seek_frame(m_format, -1, target, AVSEEK_FLAG_BACKWARD) < 0) {
                return false;
            }
        }
        avcodec_flush_buffers(m_codec);
        if (m_swr) swr_init(m_swr);  // drops anything still inside it
        m_pcm.clear();
        m_pcmRead = 0;
        m_ended = false;
        m_flushing = false;
        m_skipUntil = seconds;  // decoded audio before this is dropped
        return true;
    }

    void Abort() override {
        m_abort = true;
        if (m_cache) m_cache->Abort();
    }

    double Length() const override { return m_length; }
    bool IsLive() const override { return m_live; }

    std::string Tag(const std::string& name) const override {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        std::string key = Upper(name);
        auto it = m_tags.find(key);
        if (it == m_tags.end()) {
            if (const char* alias = FfmpegKey(key)) it = m_tags.find(alias);
        }
        return it == m_tags.end() ? "" : it->second;
    }

    std::string StreamTitle() const override {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        return m_streamTitle;
    }

    std::vector<Chapter> Chapters() const override {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        return m_chapters;
    }

    int Bitrate() const override {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        return m_recentBitrate > 0 ? m_recentBitrate : m_nominalBitrate;
    }

    bool IsVbr() const override {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        return m_vbr;
    }

    bool Rewindable(double& start, double& end) const override {
        if (!m_rewind) return false;
        std::lock_guard<std::mutex> lock(m_keptMutex);
        start = m_kept.empty() ? 0.0 : m_kept.front().start;
        // Short of the newest by what arrives at a time (an HLS stream's segment),
        // so playing from there never runs out before the next arrives
        end = std::max(start, m_keptEnd - m_largestArrival);
        return true;
    }

    bool Starved() const override {
        if (!m_rewind) return false;
        std::lock_guard<std::mutex> lock(m_keptMutex);
        return !m_readerEnded && std::max(m_cursor, m_keptBase) >= m_keptBase + m_kept.size();
    }

    int SourceChannels() const override { return m_sourceChannels; }
    int SourceSampleRate() const override { return m_sourceRate; }
    int SourceBits() const override { return m_bits; }
    std::string CodecName() const override { return m_codecName; }

    // A new stream title since the last call (the decode thread asks after reads)
    bool TakeTitleChange() {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        bool changed = m_titleChanged;
        m_titleChanged = false;
        return changed;
    }

private:
    static bool Fail(std::wstring& error, const std::wstring& message) {
        error = message;
        return false;
    }

    static std::wstring ErrorText(int rc) {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(rc, text, sizeof(text));
        return Utf8ToWide(text);
    }

    static std::wstring OpenErrorText(int rc) {
        if (rc == AVERROR(ENOENT)) return L"The file was not found.";
        if (rc == AVERROR(EACCES)) return L"The file could not be opened.";
        if (rc == AVERROR_INVALIDDATA) return L"Unsupported file format.";
        return L"Could not open the file: " + ErrorText(rc);
    }

    // Starts the clock a blocking call may run for before it is given up.
    // (From the decode thread and a download or keeping thread at once: atomic.)
    // An HLS playlist: its demuxer opens the playlist and its segments again by
    // itself, so a local copy of the playlist would be no use to it. Looks at the
    // first bytes and leaves the reader where it was.
    static bool IsHlsPlaylist(AVIOContext* io) {
        unsigned char head[7];
        const int n = avio_read(io, head, sizeof(head));
        avio_seek(io, 0, SEEK_SET);  // within the reader's buffer: no new request
        return n == static_cast<int>(sizeof(head)) && std::memcmp(head, "#EXTM3U", sizeof(head)) == 0;
    }

    void Arm() {
        m_deadline = (std::chrono::steady_clock::now() + std::chrono::seconds(kNetworkTimeoutSeconds * 2))
                         .time_since_epoch()
                         .count();
    }

    static int Interrupted(void* opaque) {
        auto* self = static_cast<FfmpegDecoder*>(opaque);
        return self->m_abort || std::chrono::steady_clock::now().time_since_epoch().count() > self->m_deadline ? 1
                                                                                                                 : 0;
    }

    // The tags, chapters and stream headers, read once the file is open.
    void ReadInfo() {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        auto add = [this](AVDictionary* dict) {
            const AVDictionaryEntry* e = nullptr;
            while ((e = av_dict_get(dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
                std::string key = Upper(e->key);
                if (m_tags.find(key) == m_tags.end()) m_tags[key] = e->value;
            }
        };
        add(m_format->metadata);
        add(m_format->streams[m_stream]->metadata);
        for (unsigned i = 0; i < m_format->nb_chapters; i++) {
            const AVChapter* c = m_format->chapters[i];
            Chapter chapter;
            // Chapter times count from the audible start already (after an MP3's
            // encoder delay, which start_time is), so they are taken as they are.
            chapter.position = c->start * av_q2d(c->time_base);
            if (chapter.position < 0) chapter.position = 0;
            const AVDictionaryEntry* title = av_dict_get(c->metadata, "title", nullptr, 0);
            if (title) chapter.name = Utf8ToWide(title->value);
            m_chapters.push_back(chapter);
        }
        std::sort(m_chapters.begin(), m_chapters.end(),
                  [](const Chapter& a, const Chapter& b) { return a.position < b.position; });
        // Shoutcast/Icecast headers ("icy-name: ...") come as the stream's metadata
        // too; keep their names lower case, as they are known by.
        for (const char* key : {"icy-name", "icy-genre", "icy-br", "icy-url", "icy-description"}) {
            auto it = m_tags.find(Upper(key));
            if (it != m_tags.end()) m_tags[key] = it->second;
        }
        auto st = m_tags.find("STREAMTITLE");
        if (st != m_tags.end()) m_streamTitle = st->second;
        PollIcyLocked();
    }

    // A Shoutcast stream's title comes with the audio; see if it changed.
    void PollIcy() {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        PollIcyLocked();
    }

    void PollIcyLocked() {
        if (!m_format || !m_format->pb) return;
        uint8_t* packet = nullptr;
        if (av_opt_get(m_format, "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN, &packet) < 0 || !packet) return;
        std::string meta = reinterpret_cast<char*>(packet);
        av_free(packet);
        if (meta.empty() || meta == m_lastIcyPacket) return;
        m_lastIcyPacket = meta;
        std::string title = IcyField(meta, "StreamTitle");
        if (m_rewind) {
            // Kept with the time it came at; it is the title once playing gets there
            double at, oldest;
            {
                std::lock_guard<std::mutex> lock(m_keptMutex);
                at = m_keptEnd;
                oldest = m_kept.empty() ? 0.0 : m_kept.front().start;
            }
            m_titleTimes.push_back({at, title});
            while (m_titleTimes.size() > 1 && m_titleTimes[1].first <= oldest) m_titleTimes.pop_front();
            return;
        }
        if (title != m_streamTitle) {
            m_streamTitle = title;
            m_titleChanged = true;
        }
    }

    // Averages the bitrate over the last few seconds of packets.
    void CountPacket(const AVPacket* packet) {
        if (packet->duration <= 0) return;
        double seconds = packet->duration * av_q2d(m_format->streams[m_stream]->time_base);
        if (seconds <= 0) return;
        int kbps = static_cast<int>(packet->size * 8 / seconds / 1000 + 0.5);
        m_window.push_back({packet->size, seconds});
        m_windowBytes += packet->size;
        m_windowSeconds += seconds;
        while (m_window.size() > 1 && m_windowSeconds - m_window.front().second > 2.0) {
            m_windowBytes -= m_window.front().first;
            m_windowSeconds -= m_window.front().second;
            m_window.pop_front();
        }
        std::lock_guard<std::mutex> lock(m_infoMutex);
        if (m_windowSeconds > 0) m_recentBitrate = static_cast<int>(m_windowBytes * 8 / m_windowSeconds / 1000 + 0.5);
        // Frames of different sizes: a variable bitrate (a constant one varies by
        // a padding byte at most).
        if (m_firstKbps == 0) {
            m_firstKbps = kbps;
        } else if (!m_vbr && std::abs(kbps - m_firstKbps) > std::max(8, m_firstKbps / 20)) {
            m_vbr = m_bits == 0;
        }
    }

    // Decodes more audio into m_pcm. False at the end.
    bool DecodeMore() {
        while (true) {
            int rc = avcodec_receive_frame(m_codec, m_frame);
            if (rc == 0) {
                Convert(m_frame);
                av_frame_unref(m_frame);
                if (!m_pcm.empty()) return true;
                continue;
            }
            if (rc == AVERROR_EOF) return false;
            if (rc != AVERROR(EAGAIN)) return false;
            if (m_flushing) return false;

            if (m_rewind) {
                if (!NextKept(m_packet)) {
                    if (!Starved()) {
                        m_flushing = true;  // the stream ended
                        avcodec_send_packet(m_codec, nullptr);
                        continue;
                    }
                    return false;  // nothing more yet
                }
                avcodec_send_packet(m_codec, m_packet);
                av_packet_unref(m_packet);
                continue;
            }

            Arm();
            rc = av_read_frame(m_format, m_packet);
            if (rc < 0) {
                // The end (or a stream gone for good): drain the decoder.
                m_flushing = true;
                avcodec_send_packet(m_codec, nullptr);
                continue;
            }
            if (m_packet->stream_index == m_stream) {
                CountPacket(m_packet);
                rc = avcodec_send_packet(m_codec, m_packet);
                // A damaged packet is skipped; the audio carries on after it.
                (void)rc;
            }
            av_packet_unref(m_packet);
            PollIcy();
        }
    }

    // Kept for rewinding: reads the stream into m_kept until it ends or the
    // decoder goes (its own thread; the only user of m_format from then on).
    void KeepStream() {
        AVPacket* packet = av_packet_alloc();
        const AVRational timeBase = m_format->streams[m_stream]->time_base;
        while (packet && !m_abort) {
            Arm();
            if (av_read_frame(m_format, packet) < 0) break;
            if (packet->stream_index == m_stream) {
                CountPacket(packet);
                double seconds = packet->duration > 0 ? packet->duration * av_q2d(timeBase) : 0.0;
                if (seconds <= 0) seconds = m_defaultPacketSeconds;
                Kept kept{av_packet_clone(packet), 0.0, 0.0};
                if (kept.packet) {
                    std::lock_guard<std::mutex> lock(m_keptMutex);
                    kept.start = m_keptEnd;
                    kept.end = m_keptEnd + seconds;
                    // The stream's own times, which start anywhere and can jump, give
                    // way to the kept timeline's, so a seek's target is found in frames
                    kept.packet->pts = kept.packet->dts = std::llround(kept.start / av_q2d(timeBase));
                    kept.packet->duration = std::llround(seconds / av_q2d(timeBase));
                    m_kept.push_back(kept);
                    m_keptEnd = kept.end;
                    // How much comes at a time: packets in a run with no pause
                    // between them. The first run (a server's opening burst, or
                    // HLS's first few segments) is left out.
                    const auto now = std::chrono::steady_clock::now();
                    if (now - m_lastArrival > std::chrono::milliseconds(250)) {
                        if (m_arrivals++ > 1) m_largestArrival = std::max(m_largestArrival, kept.start - m_arrivalStart);
                        m_arrivalStart = kept.start;
                    }
                    m_lastArrival = now;
                    while (m_kept.size() > 1 && m_kept.front().end < m_keptEnd - m_keepSeconds) {
                        av_packet_free(&m_kept.front().packet);
                        m_kept.pop_front();
                        m_keptBase++;
                    }
                }
            }
            av_packet_unref(packet);
            PollIcy();
        }
        av_packet_free(&packet);
        std::lock_guard<std::mutex> lock(m_keptMutex);
        m_readerEnded = true;
    }

    // The next kept packet to decode, if it has come; the title is what it was then.
    // Paused longer than what is kept, playing carries on from the oldest.
    bool NextKept(AVPacket* out) {
        double at;
        {
            std::lock_guard<std::mutex> lock(m_keptMutex);
            if (m_cursor < m_keptBase) m_cursor = m_keptBase;
            const size_t i = static_cast<size_t>(m_cursor - m_keptBase);
            if (i >= m_kept.size()) return false;
            if (av_packet_ref(out, m_kept[i].packet) < 0) return false;
            at = m_kept[i].start;
            m_cursor++;
        }
        std::lock_guard<std::mutex> lock(m_infoMutex);
        const std::string* title = nullptr;
        for (const auto& change : m_titleTimes) {
            if (change.first > at) break;
            title = &change.second;
        }
        if (title && *title != m_streamTitle) {
            m_streamTitle = *title;
            m_titleChanged = true;
        }
        return true;
    }

    // Converts a decoded frame to stereo float and appends it, dropping what lies
    // before a seek's target.
    void Convert(const AVFrame* frame) {
        if (frame->nb_samples <= 0) return;
        const int channels = frame->ch_layout.nb_channels;
        // The source layout can change mid-stream (a radio station's ad break):
        // the converter follows, and resamples to the rate the stream began at.
        if (!m_swr || channels != m_swrChannels || frame->sample_rate != m_swrRate ||
            frame->format != m_swrFormat) {
            swr_free(&m_swr);
            AVChannelLayout in = frame->ch_layout;
            AVChannelLayout layout;
            if (in.order == AV_CHANNEL_ORDER_UNSPEC || in.nb_channels <= 0) {
                av_channel_layout_default(&layout, channels > 0 ? channels : 2);
            } else {
                av_channel_layout_copy(&layout, &in);
            }
            // Mono stays one channel here and is copied to both sides below: a mix
            // to stereo would play it 3 dB quieter.
            AVChannelLayout outLayout;
            if (channels == 1) {
                av_channel_layout_default(&outLayout, 1);
            } else {
                av_channel_layout_default(&outLayout, 2);
            }
            if (swr_alloc_set_opts2(&m_swr, &outLayout, AV_SAMPLE_FMT_FLT, m_rate, &layout,
                                    static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0,
                                    nullptr) < 0 ||
                swr_init(m_swr) < 0) {
                swr_free(&m_swr);
                av_channel_layout_uninit(&layout);
                return;
            }
            av_channel_layout_uninit(&layout);
            m_swrChannels = channels;
            m_swrRate = frame->sample_rate;
            m_swrFormat = frame->format;
            m_swrOutChannels = outLayout.nb_channels;
        }

        int outCapacity = swr_get_out_samples(m_swr, frame->nb_samples);
        if (outCapacity <= 0) return;
        m_convert.resize(static_cast<size_t>(outCapacity) * m_swrOutChannels);
        uint8_t* outPlanes[1] = {reinterpret_cast<uint8_t*>(m_convert.data())};
        int got = swr_convert(m_swr, outPlanes, outCapacity, const_cast<const uint8_t**>(frame->extended_data),
                              frame->nb_samples);
        if (got <= 0) return;

        // Where this frame starts, to drop what precedes a seek's target
        int skip = 0;
        if (m_skipUntil >= 0) {
            int64_t pts = frame->best_effort_timestamp;
            if (pts != AV_NOPTS_VALUE) {
                double start = pts * av_q2d(m_format->streams[m_stream]->time_base) - m_startTime;
                double end = start + static_cast<double>(got) / m_rate;
                if (end <= m_skipUntil) return;  // wholly before it
                if (start < m_skipUntil) skip = static_cast<int>((m_skipUntil - start) * m_rate);
            }
            m_skipUntil = -1;
        }
        if (skip >= got) return;

        size_t base = m_pcm.size();
        m_pcm.resize(base + static_cast<size_t>(got - skip) * 2);
        float* dst = m_pcm.data() + base;
        if (m_swrOutChannels == 1) {
            for (int i = skip; i < got; i++) {
                *dst++ = m_convert[i];
                *dst++ = m_convert[i];
            }
        } else {
            std::memcpy(dst, m_convert.data() + static_cast<size_t>(skip) * 2,
                        static_cast<size_t>(got - skip) * 2 * sizeof(float));
        }
    }

    AVFormatContext* m_format = nullptr;
    AVCodecContext* m_codec = nullptr;
    SwrContext* m_swr = nullptr;
    AVPacket* m_packet = nullptr;
    AVFrame* m_frame = nullptr;
    int m_stream = -1;
    int m_rate = 0;
    int m_swrChannels = 0, m_swrRate = 0, m_swrFormat = -1, m_swrOutChannels = 2;
    std::vector<float> m_convert;
    std::vector<float> m_pcm;  // decoded, interleaved stereo, from m_pcmRead on
    size_t m_pcmRead = 0;
    bool m_ended = false;
    bool m_flushing = false;
    double m_skipUntil = -1;
    double m_startTime = 0;

    double m_length = 0;
    bool m_live = false;
    int m_sourceChannels = 0, m_sourceRate = 0, m_bits = 0;
    std::string m_codecName;

    std::atomic<bool> m_abort{false};
    std::atomic<std::chrono::steady_clock::rep> m_deadline{0};
    std::unique_ptr<HttpCache> m_cache;  // a file over HTTP: its local copy
    AVIOContext* m_io = nullptr;         // a stream's connection, when it has no local copy

    std::deque<std::pair<int, double>> m_window;  // recent packets: bytes, seconds
    double m_windowBytes = 0, m_windowSeconds = 0;
    int m_firstKbps = 0;

    mutable std::mutex m_infoMutex;
    std::map<std::string, std::string> m_tags;  // upper-case names
    std::vector<Chapter> m_chapters;
    std::string m_streamTitle, m_lastIcyPacket;
    bool m_titleChanged = false;
    std::deque<std::pair<double, std::string>> m_titleTimes;  // kept for rewinding: when each title came

    // Kept for rewinding: the stream's packets, with their times from where it was
    // opened. Filled by m_reader; read at m_cursor, counted as m_keptBase is (the
    // number dropped off the front).
    struct Kept {
        AVPacket* packet;
        double start, end;
    };
    bool m_rewind = false;
    double m_keepSeconds = 0;
    double m_defaultPacketSeconds = 0;
    std::thread m_reader;
    mutable std::mutex m_keptMutex;
    std::deque<Kept> m_kept;
    uint64_t m_keptBase = 0;
    uint64_t m_cursor = 0;
    double m_keptEnd = 0;
    bool m_readerEnded = false;
    std::chrono::steady_clock::time_point m_lastArrival;  // for how much arrives at a time
    double m_arrivalStart = 0;
    int m_arrivals = 0;
    double m_largestArrival = 0;
    int m_nominalBitrate = 0, m_recentBitrate = 0;
    bool m_vbr = false;
};

}  // namespace

void SetLiveRewindSeconds(int seconds) { g_liveRewindSeconds = std::max(0, seconds); }

bool IsNetworkPath(const std::wstring& path) {
    // FTP too (where FFmpeg is built with it): a file on a server, read through a
    // local copy as one over HTTP is
    return WStrNICmp(path.c_str(), L"http://", 7) == 0 || WStrNICmp(path.c_str(), L"https://", 8) == 0 ||
           WStrNICmp(path.c_str(), L"ftp://", 6) == 0;
}

std::unique_ptr<Decoder> OpenFfmpegDecoder(const std::wstring& pathOrUrl, std::wstring& error) {
    QuietLogging();
    auto decoder = std::make_unique<FfmpegDecoder>();
    if (!decoder->Open(pathOrUrl, error)) return nullptr;
    return decoder;
}

bool TakeStreamTitleChange(Decoder* decoder) {
    auto* ffmpeg = dynamic_cast<FfmpegDecoder*>(decoder);
    return ffmpeg && ffmpeg->TakeTitleChange();
}

bool DecodeWholeFile(const std::wstring& path, std::vector<float>& samples, int& channels, int& sampleRate,
                     std::wstring& error) {
    // Kept in the file's own channels: an impulse response's left and right differ.
    QuietLogging();
    AVFormatContext* format = nullptr;
    std::string url = WideToUtf8(path);
    if (avformat_open_input(&format, url.c_str(), nullptr, nullptr) < 0) {
        error = L"The file could not be opened.";
        return false;
    }
    struct Closer {
        AVFormatContext*& f;
        ~Closer() { avformat_close_input(&f); }
    } closeFormat{format};
    avformat_find_stream_info(format, nullptr);
    int index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (index < 0) {
        error = L"There is no audio in it.";
        return false;
    }
    const AVCodec* codec = avcodec_find_decoder(format->streams[index]->codecpar->codec_id);
    AVCodecContext* ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
    struct CodecCloser {
        AVCodecContext*& c;
        ~CodecCloser() { avcodec_free_context(&c); }
    } closeCodec{ctx};
    if (!ctx || avcodec_parameters_to_context(ctx, format->streams[index]->codecpar) < 0 ||
        avcodec_open2(ctx, codec, nullptr) < 0) {
        error = L"Its audio could not be decoded.";
        return false;
    }
    channels = ctx->ch_layout.nb_channels;
    sampleRate = ctx->sample_rate;
    if (channels <= 0 || sampleRate <= 0) {
        error = L"Its audio format is not supported.";
        return false;
    }

    SwrContext* swr = nullptr;
    AVChannelLayout layout;
    av_channel_layout_default(&layout, channels);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<float> convert;
    samples.clear();
    auto drain = [&]() {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (!swr) {
                AVChannelLayout in = frame->ch_layout;
                if (in.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&in, channels);
                swr_alloc_set_opts2(&swr, &layout, AV_SAMPLE_FMT_FLT, sampleRate, &in,
                                    static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
                if (swr) swr_init(swr);
            }
            if (!swr) break;
            int capacity = swr_get_out_samples(swr, frame->nb_samples);
            convert.resize(static_cast<size_t>(std::max(capacity, 0)) * channels);
            uint8_t* out[1] = {reinterpret_cast<uint8_t*>(convert.data())};
            int got = swr_convert(swr, out, capacity, const_cast<const uint8_t**>(frame->extended_data),
                                  frame->nb_samples);
            if (got > 0) samples.insert(samples.end(), convert.begin(), convert.begin() + static_cast<size_t>(got) * channels);
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == index && avcodec_send_packet(ctx, packet) >= 0) drain();
        av_packet_unref(packet);
    }
    avcodec_send_packet(ctx, nullptr);
    drain();
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr);
    av_channel_layout_uninit(&layout);
    if (samples.empty()) {
        error = L"No audio could be decoded from it.";
        return false;
    }
    return true;
}

namespace {

// The demuxer for a file's extension, so opening it for its tags skips guessing
// the format (reading and testing its start against every format there is).
const AVInputFormat* FormatForExtension(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return nullptr;
    std::wstring ext = path.substr(dot + 1);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    static const struct {
        const wchar_t* ext;
        const char* format;
    } kFormats[] = {
        {L"mp3", "mp3"},  {L"mp2", "mp3"},   {L"flac", "flac"}, {L"m4a", "mov"}, {L"m4b", "mov"},
        {L"m4r", "mov"},  {L"mp4", "mov"},   {L"ogg", "ogg"},   {L"oga", "ogg"}, {L"opus", "ogg"},
        {L"wav", "wav"},  {L"wma", "asf"},   {L"aiff", "aiff"}, {L"aif", "aiff"}, {L"ape", "ape"},
        {L"wv", "wv"},    {L"dsf", "dsf"},   {L"mka", "matroska"}, {L"aac", "aac"}, {L"tta", "tta"},
        {L"tak", "tak"},  {L"mpc", "mpc8"},  {L"caf", "caf"},   {L"w64", "w64"},
    };
    for (const auto& f : kFormats) {
        if (ext == f.ext) return av_find_input_format(f.format);
    }
    return nullptr;
}

int NumberBefore(const char* text) {
    // "3/12" -> 3, "2004-05-01" -> 2004
    return text ? atoi(text) : 0;
}

}  // namespace

bool ReadFileTags(const std::wstring& path, FileTags& tags) {
    QuietLogging();
    tags = FileTags();
    std::string url = WideToUtf8(path);
    AVFormatContext* format = nullptr;
    const AVInputFormat* hint = FormatForExtension(path);
    if (avformat_open_input(&format, url.c_str(), hint, nullptr) < 0) {
        // Named for another format than it is: guess after all
        format = nullptr;
        if (!hint || avformat_open_input(&format, url.c_str(), nullptr, nullptr) < 0) return false;
    }
    // The file's tags, then its audio stream's (Ogg keeps them there)
    AVStream* audio = nullptr;
    for (unsigned i = 0; i < format->nb_streams; i++) {
        if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio = format->streams[i];
            break;
        }
    }
    AVDictionary* sources[2] = {format->metadata, audio ? audio->metadata : nullptr};
    auto get = [&](const char* key) -> const char* {
        for (AVDictionary* dict : sources) {
            if (!dict) continue;
            if (const AVDictionaryEntry* e = av_dict_get(dict, key, nullptr, 0)) {
                if (e->value && *e->value) return e->value;
            }
        }
        return nullptr;
    };
    auto text = [&](const char* key) {
        const char* value = get(key);
        return value ? std::string(value) : std::string();
    };
    tags.title = text("title");
    tags.artist = text("artist");
    tags.album = text("album");
    tags.albumArtist = text("album_artist");
    if (tags.albumArtist.empty()) tags.albumArtist = text("albumartist");
    tags.genre = text("genre");
    tags.track = NumberBefore(get("track"));
    tags.disc = NumberBefore(get("disc"));
    const char* date = get("date");
    if (!date) date = get("year");
    tags.year = NumberBefore(date);
    // The length: most formats say it in their header (as the stream's); the rest
    // (an MP3 without a VBR header, say) take a look at the first few frames
    auto length = [&]() {
        if (format->duration > 0) return format->duration / static_cast<double>(AV_TIME_BASE);
        if (audio && audio->duration > 0) return audio->duration * av_q2d(audio->time_base);
        return 0.0;
    };
    tags.duration = length();
    if (tags.duration <= 0) {
        format->probesize = 64 * 1024;
        format->max_analyze_duration = AV_TIME_BASE / 2;
        avformat_find_stream_info(format, nullptr);
        tags.duration = length();
    }
    avformat_close_input(&format);
    return true;
}

std::string DecoderVersion() {
    return std::string("FFmpeg ") + av_version_info();
}

}  // namespace audio
