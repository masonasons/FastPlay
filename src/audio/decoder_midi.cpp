// MIDI files (and the other sequence formats SpessaSynth reads: RMI, XMI, MUS,
// HMI...), played by SpessaSynth with the SoundFont or DLS chosen in Options > MIDI.
//
// The SoundFont is loaded once and kept, as a big one takes a while; each file
// gets its own synthesizer and sequencer. Samples are read from the SoundFont as
// they are first needed, not all loaded into memory.

#include "audio.h"
#include "audio_internal.h"
#include "globals.h"
#include "midi_out.h"
#include "mp4_index.h"
#include "utils.h"

extern "C" {
#include "spessasynth/midi/midi.h"
#include "spessasynth/sequencer/sequencer.h"
#include "spessasynth/sflist/sflist.h"
#include "spessasynth/soundbank/soundbank.h"
#include "spessasynth/synthesizer/synth.h"
#include "spessasynth/utils/file.h"
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace audio {
namespace {

const int kRate = 48000;
const int kBlock = 128;           // SpessaSynth renders in blocks of this
const double kTailSeconds = 2.0;  // reverb and releases after the last event

// SpessaSynth reads files through these, so paths can be Unicode and large
// SoundFonts are read as needed.
struct FileContext {
    FILE* f;
    size_t size;
};

void FileCloseCb(void* context) {
    auto* file = static_cast<FileContext*>(context);
    fclose(file->f);
    delete file;
}

bool FileSeekCb(void* context, size_t offset) {
    return mp4::Seek(static_cast<FileContext*>(context)->f, offset);
}

size_t FileSizeCb(void* context) { return static_cast<FileContext*>(context)->size; }

size_t FileReadCb(void* context, uint8_t* out, size_t count) {
    return fread(out, 1, count, static_cast<FileContext*>(context)->f);
}

SS_File_ReaderCallbacks g_fileCallbacks = {FileCloseCb, FileSeekCb, FileSizeCb, FileReadCb};

SS_File* OpenFile(const std::wstring& path) {
    FILE* f = FileOpen(path, "rb");
    if (!f) return nullptr;
    auto* context = new FileContext{f, 0};
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) == 0) context->size = static_cast<size_t>(_ftelli64(f));
#else
    if (fseeko(f, 0, SEEK_END) == 0) context->size = static_cast<size_t>(ftello(f));
#endif
    mp4::Seek(f, 0);
    SS_File* file = ss_file_open_from_callbacks(&g_fileCallbacks, context);
    if (!file) FileCloseCb(context);
    return file;
}

// The SoundFont (or sflist of them) in use, kept between files
struct Bank {
    std::mutex mutex;
    std::wstring path;
    SS_SoundBank* bank = nullptr;
    std::vector<uint8_t> sflist;  // an sflist's text, loaded again for each synthesizer
    std::string sflistBase;
    int users = 0;                          // files playing with `bank`
    std::vector<SS_SoundBank*> retired;     // replaced while in use, freed when unused
};
Bank g_bank;

bool IsSflist(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    return WStrICmp(path.c_str() + dot, L".sflist") == 0 || WStrICmp(path.c_str() + dot, L".json") == 0;
}

// Loads the configured SoundFont if it is not the one loaded. g_bank.mutex held.
bool LoadBank(std::wstring& error) {
    if (g_midiSoundFont.empty()) {
        error = L"MIDI files need a SoundFont: choose one in Options, on the MIDI tab.";
        return false;
    }
    if (g_bank.path == g_midiSoundFont && (g_bank.bank || !g_bank.sflist.empty())) return true;
    // Another SoundFont was chosen: the old one goes once nothing plays with it
    if (g_bank.bank) {
        if (g_bank.users > 0) {
            g_bank.retired.push_back(g_bank.bank);
        } else {
            ss_soundbank_free(g_bank.bank);
        }
    }
    g_bank.bank = nullptr;
    g_bank.sflist.clear();
    g_bank.path.clear();

    if (IsSflist(g_midiSoundFont)) {
        FILE* f = FileOpen(g_midiSoundFont, "rb");
        if (!f) {
            error = L"The SoundFont list could not be opened: " + g_midiSoundFont;
            return false;
        }
        std::vector<uint8_t> text;
        uint8_t buffer[65536];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) text.insert(text.end(), buffer, buffer + n);
        fclose(f);
        g_bank.sflist = std::move(text);
        size_t slash = g_midiSoundFont.find_last_of(L"\\/");
        g_bank.sflistBase = WideToUtf8(slash == std::wstring::npos ? L"." : g_midiSoundFont.substr(0, slash));
    } else {
        SS_File* file = OpenFile(g_midiSoundFont);
        if (!file) {
            error = L"The SoundFont could not be opened: " + g_midiSoundFont;
            return false;
        }
        g_bank.bank = ss_soundbank_load(file);
        ss_file_close(file);
        if (!g_bank.bank) {
            error = L"The SoundFont could not be read: " + g_midiSoundFont;
            return false;
        }
    }
    g_bank.path = g_midiSoundFont;
    return true;
}

std::string Text(const uint8_t* bytes, size_t length) {
    std::string s(reinterpret_cast<const char*>(bytes), length);
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
    return s;
}

class MidiDecoder : public Decoder {
public:
    ~MidiDecoder() override {
        if (m_sequencer) ss_sequencer_free(m_sequencer);
        if (m_processor) {
            // The shared SoundFont stays loaded for the next file
            if (m_sharedBank) ss_processor_remove_soundbank(m_processor, "fastplay", true);
            ss_processor_free(m_processor);
        }
        if (m_sharedBank) {
            std::lock_guard<std::mutex> lock(g_bank.mutex);
            if (--g_bank.users == 0) {
                for (SS_SoundBank* bank : g_bank.retired) ss_soundbank_free(bank);
                g_bank.retired.clear();
            }
        }
        if (m_midi) ss_midi_free(m_midi);
    }

    bool Open(const std::wstring& path, std::wstring& error) {
        SS_File* file = OpenFile(path);
        if (!file) {
            error = L"The file could not be opened.";
            return false;
        }
        std::string name = WideToUtf8(path);
        m_midi = ss_midi_load(file, name.c_str());
        ss_file_close(file);
        if (!m_midi) {
            error = L"It is not a MIDI file FastPlay can read.";
            return false;
        }
        if (ss_midi_has_emidi(m_midi)) ss_midi_remove_emidi_non_gm(m_midi);

        SS_ProcessorOptions options = {};
        options.enable_effects = true;
        options.voice_cap = static_cast<uint32_t>(std::clamp(g_midiMaxVoices, 1, 1000));
        options.interpolation = g_midiSincInterp ? SS_INTERP_SINC : SS_INTERP_HERMITE;
        options.preload_all_samples = false;
        options.preload_instruments = true;
        m_processor = ss_processor_create(kRate, &options);
        if (!m_processor) {
            error = L"Out of memory.";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_bank.mutex);
            if (!LoadBank(error)) return false;
            if (g_bank.bank) {
                if (!ss_processor_load_soundbank(m_processor, g_bank.bank, "fastplay", 0, false)) {
                    error = L"The SoundFont could not be used.";
                    return false;
                }
                m_sharedBank = true;
                g_bank.users++;
            } else {
                char message[sflist_max_error] = "";
                std::vector<uint8_t> text = g_bank.sflist;
                SS_FilteredBanks* banks = sflist_load(reinterpret_cast<char*>(text.data()), text.size(),
                                                      g_bank.sflistBase.c_str(), message);
                if (!banks || !ss_processor_load_filtered_banks(m_processor, banks, "fastplay", false)) {
                    if (banks) sflist_free(banks);
                    error = L"The SoundFont list could not be read: " + Utf8ToWide(message);
                    return false;
                }
            }
        }

        m_sequencer = ss_sequencer_create(m_processor);
        if (!m_sequencer || !ss_sequencer_load_midi(m_sequencer, m_midi)) {
            error = L"The MIDI file could not be played.";
            return false;
        }
        ss_sequencer_set_skip_to_first_note_on(m_sequencer, true);
        ss_sequencer_set_loop_count(m_sequencer, 0);
        ss_sequencer_play(m_sequencer);

        m_length = m_midi->duration;
        m_title = Text(m_midi->binary_name, m_midi->binary_name_length);
        return true;
    }

    int SampleRate() const override { return kRate; }

    int Read(float* out, int frames) override {
        int done = 0;
        while (done < frames) {
            if (ss_sequencer_is_finished(m_sequencer)) {
                // Then the reverb and the notes' releases, and the end
                if (m_tail <= 0) break;
            }
            int n = std::min(kBlock, frames - done);
            if (ss_sequencer_is_finished(m_sequencer)) {
                n = std::min(n, m_tail);
                m_tail -= n;
            } else {
                ss_sequencer_tick(m_sequencer, static_cast<uint32_t>(n));
            }
            ss_processor_render_interleaved(m_processor, out + static_cast<size_t>(done) * 2, static_cast<uint32_t>(n));
            done += n;
        }
        return done;
    }

    bool Seek(double seconds) override {
        ss_sequencer_set_time(m_sequencer, std::clamp(seconds, 0.0, m_length));
        ss_sequencer_play(m_sequencer);
        m_tail = static_cast<int>(kTailSeconds * kRate);
        return true;
    }

    void Abort() override {}
    double Length() const override { return m_length; }
    bool IsLive() const override { return false; }

    std::string Tag(const std::string& name) const override {
        if (!m_midi) return "";
        const SS_RMIDIInfo& info = m_midi->rmidi_info;
        auto field = [](const uint8_t* bytes, size_t length) { return bytes ? Text(bytes, length) : std::string(); };
        const char* key = name.c_str();
        if (StrICmp(key, "TITLE") == 0) {
            std::string title = field(info.name, info.name_len);
            return title.empty() ? m_title : title;
        }
        if (StrICmp(key, "ARTIST") == 0) return field(info.artist, info.artist_len);
        if (StrICmp(key, "ALBUM") == 0) return field(info.album, info.album_len);
        if (StrICmp(key, "GENRE") == 0) return field(info.genre, info.genre_len);
        if (StrICmp(key, "DATE") == 0 || StrICmp(key, "YEAR") == 0) {
            return field(info.creation_date, info.creation_date_len);
        }
        if (StrICmp(key, "COMMENT") == 0) {
            std::string comment = field(info.comment, info.comment_len);
            return comment.empty() ? field(info.copyright, info.copyright_len) : comment;
        }
        return "";
    }

    std::string StreamTitle() const override { return ""; }
    std::vector<Chapter> Chapters() const override { return {}; }
    int Bitrate() const override { return 0; }
    bool IsVbr() const override { return false; }
    int SourceChannels() const override { return 2; }
    int SourceSampleRate() const override { return kRate; }
    int SourceBits() const override { return 0; }
    std::string CodecName() const override { return "MIDI"; }

private:
    SS_MIDIFile* m_midi = nullptr;
    SS_Processor* m_processor = nullptr;
    SS_Sequencer* m_sequencer = nullptr;
    bool m_sharedBank = false;
    double m_length = 0;
    int m_tail = static_cast<int>(kTailSeconds * kRate);
    std::string m_title;
};

// A MIDI file played through a MIDI output port (Options > MIDI), by a synthesizer
// or other program outside FastPlay. Nothing is heard from FastPlay itself: the
// decoder gives silence, so that the player's transport works as it does for any
// file, and a thread of its own sends the events as the player's position passes
// them. Pause stops the position, and so the events; a seek moves the sequencer,
// which sends the notes off and the setup of the new place itself. Tempo and
// rate changes move the position faster or slower, and so the music. Pitch
// changes and the effects cannot reach it.
class MidiPortDecoder : public Decoder {
public:
    ~MidiPortDecoder() override {
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
        if (m_port) {
            AllNotesOff();
            m_port.reset();
        }
        if (m_sequencer) ss_sequencer_free(m_sequencer);
        if (m_midi) ss_midi_free(m_midi);
    }

    bool Open(const std::wstring& path, std::wstring& error) {
        m_port = OpenMidiOutputPort(g_midiOutputPort, error);
        if (!m_port) return false;

        SS_File* file = OpenFile(path);
        if (!file) {
            error = L"The file could not be opened.";
            return false;
        }
        std::string name = WideToUtf8(path);
        m_midi = ss_midi_load(file, name.c_str());
        ss_file_close(file);
        if (!m_midi) {
            error = L"It is not a MIDI file FastPlay can read.";
            return false;
        }

        SS_SequencerCallbacks callbacks = {};
        callbacks.sample_rate = kRate;
        callbacks.context = this;
        callbacks.midi_command = [](void* context, const uint8_t* data, size_t length, double) {
            static_cast<MidiPortDecoder*>(context)->Send(data, length);
        };
        m_sequencer = ss_sequencer_create_callbacks(&callbacks);
        if (!m_sequencer || !ss_sequencer_load_midi(m_sequencer, m_midi)) {
            error = L"The MIDI file could not be played.";
            return false;
        }
        ss_sequencer_set_skip_to_first_note_on(m_sequencer, false);  // the file's own timing
        ss_sequencer_set_loop_count(m_sequencer, 0);
        ss_sequencer_play(m_sequencer);

        m_length = m_midi->duration;
        m_title = Text(m_midi->binary_name, m_midi->binary_name_length);
        m_thread = std::thread([this]() { Run(); });
        return true;
    }

    int SampleRate() const override { return kRate; }

    // Silence, for as long as the file is; what is heard comes from the port
    int Read(float* out, int frames) override {
        double end = m_length + kTailSeconds;
        int left = static_cast<int>((end - m_readSeconds) * kRate);
        int n = std::clamp(std::min(frames, left), 0, frames);
        if (n > 0) std::memset(out, 0, static_cast<size_t>(n) * 2 * sizeof(float));
        m_readSeconds += static_cast<double>(n) / kRate;
        return n;
    }

    bool Seek(double seconds) override {
        m_readSeconds = std::clamp(seconds, 0.0, m_length);
        return true;
    }

    void Abort() override {}
    double Length() const override { return m_length; }
    bool IsLive() const override { return false; }

    std::string Tag(const std::string& name) const override {
        if (!m_midi) return "";
        const SS_RMIDIInfo& info = m_midi->rmidi_info;
        auto field = [](const uint8_t* bytes, size_t length) { return bytes ? Text(bytes, length) : std::string(); };
        const char* key = name.c_str();
        if (StrICmp(key, "TITLE") == 0) {
            std::string title = field(info.name, info.name_len);
            return title.empty() ? m_title : title;
        }
        if (StrICmp(key, "ARTIST") == 0) return field(info.artist, info.artist_len);
        if (StrICmp(key, "ALBUM") == 0) return field(info.album, info.album_len);
        return "";
    }

    std::string StreamTitle() const override { return ""; }
    std::vector<Chapter> Chapters() const override { return {}; }
    int Bitrate() const override { return 0; }
    bool IsVbr() const override { return false; }
    int SourceChannels() const override { return 2; }
    int SourceSampleRate() const override { return kRate; }
    int SourceBits() const override { return 0; }
    std::string CodecName() const override { return "MIDI, through " + WideToUtf8(g_midiOutputPort); }

private:
    // The sequencer's events, as they come; the port-select messages of a file
    // for several ports are not for a port
    void Send(const uint8_t* data, size_t length) {
        if (length == 0 || data[0] == 0xF5) return;
        if ((data[0] & 0xF0) == 0x90 && length >= 3) {
            m_sounding[data[0] & 0x0F][data[1] & 0x7F] = data[2] != 0;
        } else if ((data[0] & 0xF0) == 0x80 && length >= 2) {
            m_sounding[data[0] & 0x0F][data[1] & 0x7F] = false;
        }
        m_port->Send(data, length);
    }

    // Every note off, and the sustain pedal up, on every channel: for a pause, and
    // the end. Notes are let go one by one too, for a device that ignores the
    // all-notes-off controller.
    void AllNotesOff() {
        for (int channel = 0; channel < 16; channel++) {
            for (int key = 0; key < 128; key++) {
                if (!m_sounding[channel][key]) continue;
                uint8_t off[3] = {static_cast<uint8_t>(0x80 | channel), static_cast<uint8_t>(key), 0};
                m_port->Send(off, 3);
                m_sounding[channel][key] = false;
            }
            uint8_t sustain[3] = {static_cast<uint8_t>(0xB0 | channel), 64, 0};
            m_port->Send(sustain, 3);
            uint8_t all[3] = {static_cast<uint8_t>(0xB0 | channel), 123, 0};
            m_port->Send(all, 3);
        }
    }

    // Follows the player's position, sending what it passes. The position comes
    // from the device callback, in steps; between steps it is reckoned on from the
    // last, at the speed the last steps went, so the timing is the file's.
    void Run() {
        using clock = std::chrono::steady_clock;
        double sent = -1;            // the sequencer's time
        double reported = -1;        // the player's last position
        clock::time_point reportedAt = clock::now();
        double speed = 1;            // position per second, lately
        bool silent = true;
        while (!m_stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (m_stop) break;
            const bool playing = audio::GetState() == State::Playing && !audio::IsReverse();
            if (!playing) {
                if (!silent) {
                    AllNotesOff();
                    silent = true;
                }
                reported = -1;
                continue;
            }
            double position = audio::Position();
            clock::time_point now = clock::now();
            if (position != reported) {
                if (reported >= 0) {
                    double seconds = std::chrono::duration<double>(now - reportedAt).count();
                    if (seconds > 0.01 && position > reported && position - reported < 2) {
                        double measured = (position - reported) / seconds;
                        speed = speed * 0.7 + measured * 0.3;
                    }
                }
                reported = position;
                reportedAt = now;
            } else {
                position = reported + std::chrono::duration<double>(now - reportedAt).count() * speed;
            }
            if (sent < 0 || position < sent || position - sent > 1.0) {
                // A start, a seek, or a jump: the sequencer goes there (sending the
                // notes off and the setup of that place itself)
                ss_sequencer_set_time(m_sequencer, std::clamp(position, 0.0, m_length));
                ss_sequencer_play(m_sequencer);
                sent = position;
                silent = false;
                continue;
            }
            if (position > sent) {
                ss_sequencer_tick(m_sequencer, static_cast<uint32_t>((position - sent) * kRate));
                sent = position;
                silent = false;
            }
        }
    }

    std::unique_ptr<MidiOutPort> m_port;
    SS_MIDIFile* m_midi = nullptr;
    SS_Sequencer* m_sequencer = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    bool m_sounding[16][128] = {};
    double m_length = 0;
    double m_readSeconds = 0;
    std::string m_title;
};

}  // namespace

std::unique_ptr<Decoder> OpenMidiDecoder(const std::wstring& path, std::wstring& error) {
    if (!g_midiOutputPort.empty()) {
        // Through the port chosen; if it is gone, the synthesizer, as if none were
        auto port = std::make_unique<MidiPortDecoder>();
        std::wstring portError;
        if (port->Open(path, portError)) return port;
    }
    auto decoder = std::make_unique<MidiDecoder>();
    if (!decoder->Open(path, error)) return nullptr;
    return decoder;
}

}  // namespace audio
