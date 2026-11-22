// MIDI output through the ALSA sequencer: the ports the system lists that take
// MIDI in (a synthesizer program such as FluidSynth or TiMidity, an interface,
// Midi Through). PipeWire's MIDI shows up here too.

#include "midi_out.h"
#include "utils.h"

#include <alsa/asoundlib.h>

namespace {

// A port's name as the list shows it, and as the setting keeps it: "client: port".
std::wstring PortName(snd_seq_client_info_t* client, snd_seq_port_info_t* port) {
    return Utf8ToWide(std::string(snd_seq_client_info_get_name(client)) + ": " + snd_seq_port_info_get_name(port));
}

// Calls `found` with each port another client has that can be sent to; stops if it
// returns true.
template <typename Found>
void ForEachDestination(snd_seq_t* seq, Found found) {
    snd_seq_client_info_t* client;
    snd_seq_port_info_t* port;
    snd_seq_client_info_alloca(&client);
    snd_seq_port_info_alloca(&port);
    const int self = snd_seq_client_id(seq);
    const unsigned int wanted = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
    snd_seq_client_info_set_client(client, -1);
    while (snd_seq_query_next_client(seq, client) >= 0) {
        int id = snd_seq_client_info_get_client(client);
        if (id == SND_SEQ_CLIENT_SYSTEM || id == self) continue;
        snd_seq_port_info_set_client(port, id);
        snd_seq_port_info_set_port(port, -1);
        while (snd_seq_query_next_port(seq, port) >= 0) {
            unsigned int caps = snd_seq_port_info_get_capability(port);
            if ((caps & wanted) != wanted || (caps & SND_SEQ_PORT_CAP_NO_EXPORT)) continue;
            if (found(client, port)) return;
        }
    }
}

snd_seq_t* OpenSequencer() {
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) return nullptr;
    snd_seq_set_client_name(seq, "FastPlay");
    return seq;
}

class AlsaMidiPort : public MidiOutPort {
public:
    AlsaMidiPort(snd_seq_t* seq, int ownPort, snd_midi_event_t* encoder)
        : m_seq(seq), m_ownPort(ownPort), m_encoder(encoder) {}

    ~AlsaMidiPort() override {
        snd_midi_event_free(m_encoder);
        snd_seq_delete_simple_port(m_seq, m_ownPort);
        snd_seq_close(m_seq);
    }

    void Send(const uint8_t* data, size_t length) override {
        // Each message is whole, status first, so the encoder starts afresh; a
        // SysEx longer than its buffer comes out as several SysEx events.
        snd_midi_event_reset_encode(m_encoder);
        while (length > 0) {
            snd_seq_event_t event;
            snd_seq_ev_clear(&event);
            long used = snd_midi_event_encode(m_encoder, data, static_cast<long>(length), &event);
            if (used <= 0) return;
            data += used;
            length -= static_cast<size_t>(used);
            if (event.type == SND_SEQ_EVENT_NONE) continue;
            snd_seq_ev_set_source(&event, m_ownPort);
            snd_seq_ev_set_subs(&event);
            snd_seq_ev_set_direct(&event);
            snd_seq_event_output_direct(m_seq, &event);
        }
    }

private:
    snd_seq_t* m_seq;
    int m_ownPort;
    snd_midi_event_t* m_encoder;
};

}  // namespace

std::vector<std::wstring> ListMidiOutputPorts() {
    std::vector<std::wstring> names;
    snd_seq_t* seq = OpenSequencer();
    if (!seq) return names;
    ForEachDestination(seq, [&](snd_seq_client_info_t* client, snd_seq_port_info_t* port) {
        names.push_back(PortName(client, port));
        return false;
    });
    snd_seq_close(seq);
    return names;
}

std::unique_ptr<MidiOutPort> OpenMidiOutputPort(const std::wstring& name, std::wstring& error) {
    snd_seq_t* seq = OpenSequencer();
    if (!seq) {
        error = L"The ALSA sequencer could not be opened.";
        return nullptr;
    }
    snd_seq_addr_t dest = {};
    bool found = false;
    ForEachDestination(seq, [&](snd_seq_client_info_t* client, snd_seq_port_info_t* port) {
        if (PortName(client, port) != name) return false;
        dest = *snd_seq_port_info_get_addr(port);
        found = true;
        return true;
    });
    if (!found) {
        snd_seq_close(seq);
        error = L"There is no MIDI output port named " + name + L".";
        return nullptr;
    }

    // Our own port, connected to theirs; what it sends goes to whoever is connected
    int ownPort = snd_seq_create_simple_port(seq, "FastPlay Out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    snd_midi_event_t* encoder = nullptr;
    if (ownPort < 0 || snd_midi_event_new(256, &encoder) < 0) {
        snd_seq_close(seq);
        error = L"The ALSA sequencer could not make a port to send from.";
        return nullptr;
    }
    if (snd_seq_connect_to(seq, ownPort, dest.client, dest.port) < 0) {
        snd_midi_event_free(encoder);
        snd_seq_close(seq);
        error = L"The MIDI output port could not be connected to: " + name;
        return nullptr;
    }
    return std::make_unique<AlsaMidiPort>(seq, ownPort, encoder);
}
