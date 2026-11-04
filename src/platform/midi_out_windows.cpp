// MIDI output through the Windows multimedia MIDI API: the devices the system
// lists (the Microsoft GS synthesizer, an interface, a virtual port from loopMIDI).

#include "midi_out.h"

#include <windows.h>
#include <mmsystem.h>

#include <vector>

namespace {

class WinMidiPort : public MidiOutPort {
public:
    explicit WinMidiPort(HMIDIOUT handle) : m_handle(handle) {}

    ~WinMidiPort() override {
        midiOutReset(m_handle);
        midiOutClose(m_handle);
    }

    void Send(const uint8_t* data, size_t length) override {
        if (length == 0) return;
        if (data[0] == 0xF0 || length > 3) {
            // A SysEx: a buffer the driver reads, waited for here, so it may be reused
            std::vector<uint8_t> copy(data, data + length);
            MIDIHDR header = {};
            header.lpData = reinterpret_cast<LPSTR>(copy.data());
            header.dwBufferLength = static_cast<DWORD>(copy.size());
            if (midiOutPrepareHeader(m_handle, &header, sizeof(header)) != MMSYSERR_NOERROR) return;
            if (midiOutLongMsg(m_handle, &header, sizeof(header)) == MMSYSERR_NOERROR) {
                for (int waited = 0; !(header.dwFlags & MHDR_DONE) && waited < 1000; waited++) Sleep(1);
            }
            midiOutUnprepareHeader(m_handle, &header, sizeof(header));
            return;
        }
        DWORD message = data[0];
        if (length > 1) message |= static_cast<DWORD>(data[1]) << 8;
        if (length > 2) message |= static_cast<DWORD>(data[2]) << 16;
        midiOutShortMsg(m_handle, message);
    }

private:
    HMIDIOUT m_handle;
};

}  // namespace

std::vector<std::wstring> ListMidiOutputPorts() {
    std::vector<std::wstring> names;
    UINT count = midiOutGetNumDevs();
    for (UINT i = 0; i < count; i++) {
        MIDIOUTCAPSW caps = {};
        if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) names.push_back(caps.szPname);
    }
    return names;
}

std::unique_ptr<MidiOutPort> OpenMidiOutputPort(const std::wstring& name, std::wstring& error) {
    UINT count = midiOutGetNumDevs();
    for (UINT i = 0; i < count; i++) {
        MIDIOUTCAPSW caps = {};
        if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR || name != caps.szPname) continue;
        HMIDIOUT handle = nullptr;
        if (midiOutOpen(&handle, i, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            error = L"The MIDI output port could not be opened (another program may have it): " + name;
            return nullptr;
        }
        return std::make_unique<WinMidiPort>(handle);
    }
    error = L"There is no MIDI output port named " + name + L".";
    return nullptr;
}
