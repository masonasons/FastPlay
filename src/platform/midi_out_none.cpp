// No MIDI output ports on this platform: MIDI plays through the built-in synthesizer.

#include "midi_out.h"

std::vector<std::wstring> ListMidiOutputPorts() {
    return {};
}

std::unique_ptr<MidiOutPort> OpenMidiOutputPort(const std::wstring&, std::wstring& error) {
    error = L"MIDI output ports are not supported here.";
    return nullptr;
}
