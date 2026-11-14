// MIDI output through Core MIDI: the destinations the system lists (a synthesizer
// program's virtual port, an interface, the IAC bus).

#include "midi_out.h"
#include "utils.h"

#import <CoreMIDI/CoreMIDI.h>
#import <Foundation/Foundation.h>

namespace {

MIDIClientRef g_client = 0;
MIDIPortRef g_outPort = 0;

bool EnsureClient() {
    if (g_client) return true;
    if (MIDIClientCreate(CFSTR("FastPlay"), nullptr, nullptr, &g_client) != noErr) return false;
    if (MIDIOutputPortCreate(g_client, CFSTR("FastPlay Out"), &g_outPort) != noErr) {
        MIDIClientDispose(g_client);
        g_client = 0;
        return false;
    }
    return true;
}

std::wstring DestinationName(MIDIEndpointRef endpoint) {
    CFStringRef name = nullptr;
    if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name) != noErr || !name) return L"";
    std::wstring result = Utf8ToWide([(__bridge NSString*)name UTF8String] ?: "");
    CFRelease(name);
    return result;
}

class CoreMidiPort : public MidiOutPort {
public:
    explicit CoreMidiPort(MIDIEndpointRef endpoint) : m_endpoint(endpoint) {}

    void Send(const uint8_t* data, size_t length) override {
        if (length == 0) return;
        // A packet list of one packet, sent now. Big SysEx goes in pieces of what a
        // packet holds; Core MIDI takes a SysEx split over packets.
        while (length > 0) {
            size_t n = std::min(length, sizeof(m_buffer) - 64);
            MIDIPacketList* list = reinterpret_cast<MIDIPacketList*>(m_buffer);
            MIDIPacket* packet = MIDIPacketListInit(list);
            packet = MIDIPacketListAdd(list, sizeof(m_buffer), packet, 0, n, data);
            if (!packet) return;
            MIDISend(g_outPort, m_endpoint, list);
            data += n;
            length -= n;
        }
    }

private:
    MIDIEndpointRef m_endpoint;
    alignas(4) uint8_t m_buffer[1024];
};

}  // namespace

std::vector<std::wstring> ListMidiOutputPorts() {
    std::vector<std::wstring> names;
    ItemCount count = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < count; i++) {
        std::wstring name = DestinationName(MIDIGetDestination(i));
        if (!name.empty()) names.push_back(name);
    }
    return names;
}

std::unique_ptr<MidiOutPort> OpenMidiOutputPort(const std::wstring& name, std::wstring& error) {
    if (!EnsureClient()) {
        error = L"Core MIDI could not be started.";
        return nullptr;
    }
    ItemCount count = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < count; i++) {
        MIDIEndpointRef endpoint = MIDIGetDestination(i);
        if (DestinationName(endpoint) == name) return std::make_unique<CoreMidiPort>(endpoint);
    }
    error = L"There is no MIDI output port named " + name + L".";
    return nullptr;
}
