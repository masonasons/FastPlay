// MIDI output ports: a synthesizer or other program the system knows of, to play
// MIDI files through instead of the built-in synthesizer (Options > MIDI). Core
// MIDI on the Mac, the multimedia MIDI API on Windows; nowhere else.

#ifndef FASTPLAY_MIDI_OUT_H
#define FASTPLAY_MIDI_OUT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The names of the output ports there are, as the system gives them.
std::vector<std::wstring> ListMidiOutputPorts();

class MidiOutPort {
public:
    virtual ~MidiOutPort() = default;
    // Sends one MIDI message, status byte first: a channel message, or a SysEx
    // (0xF0 ... 0xF7). Safe to call from any one thread at a time.
    virtual void Send(const uint8_t* data, size_t length) = 0;
};

// Opens the port of that name; null, with `error` said, if there is none or it
// cannot be opened.
std::unique_ptr<MidiOutPort> OpenMidiOutputPort(const std::wstring& name, std::wstring& error);

#endif  // FASTPLAY_MIDI_OUT_H
