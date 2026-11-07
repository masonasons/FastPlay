#pragma once
#ifndef FASTPLAY_SCRUBBER_H
#define FASTPLAY_SCRUBBER_H

// Scrubbing: playing through the audio at speed while a seek key is held (see
// audio::StartScrub). While it lasts, the scrubber is what fills the output in
// place of the tempo processor.
//
// Its input is the decoded audio in the order it is to be heard: forward, or for
// scrubbing backward, the source reversed (the decode thread reads it backward).
// Tape: resampled, so pitch rises with speed, spinning up over a moment; let go,
// it winds back down (to normal speed going forward, to a stop going back). Spring: time-stretched
// (Signalsmith Stretch), so pitch stays, and it winds up the longer it is held,
// doubling in speed every half second (ScrubTiming) up to the top speed.
//
// Either way it starts from the sound as it plays (SetSound): the rate runs the
// tape faster or slower still, and the pitch is kept on top, so scrubbing neither
// drops to the file's own pitch nor jumps back when it ends. Spring's stretcher
// transposes; tape, with a pitch set, goes through a pitch shifter of its own,
// which is primed with the tape's first moment so it starts at once (what is
// heard runs that moment, about 80 ms, behind a change of speed).

#include "audio.h"
#include "tempo_processor.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace audio {

class Scrubber {
public:
    // How it starts: held (spinning up), or winding down from `fromSpeed` (a tape
    // let go of).
    enum class Begin { Held, WindDown };

    // `speed`: tape's speed, or spring's top speed (times normal).
    Scrubber(ScrubStyle style, int direction, float speed, int sourceRate, int outputRate, double start,
             const ScrubTiming& timing, Begin begin = Begin::Held, double fromSpeed = 1.0);
    ~Scrubber();

    // Fills `frames` of stereo output at the output rate: fewer if the source is
    // behind. Past the start or end of the source it is silence, staying there.
    int Fill(PcmSource& source, float* out, int frames);
    // Source seconds at output frame `played` (counted from the start of scrubbing)
    double PositionAt(uint64_t played);
    void SetSpeed(float speed) { m_topSpeed = speed; }
    // The pitch (semitones) and rate (a multiplier of speed and pitch) playing has,
    // for scrubbing to keep. Set before the first Fill(); a change after it is
    // followed, except that tape set going without a pitch gets none until the next.
    void SetSound(float pitch, float rate) {
        m_pitch = pitch;
        m_rate = rate > 0 ? rate : 1.0f;
    }
    ScrubStyle Style() const { return m_style; }
    int Direction() const { return m_direction; }
    float TopSpeed() const { return m_topSpeed; }
    // The speed of what is heard at output frame `played`, while held
    double HeardSpeed(uint64_t played) const;
    // Wound down (to normal speed, or going back, to a standstill) and heard to the end of it,
    // `played` being the output frames heard: true once
    bool TakeFinished(uint64_t played);
    // Winding down rather than held
    bool Releasing() const { return m_phase != Phase::Held; }

private:
    struct Stretch;
    struct Shift;

    enum class Phase { Held, Release };
    Phase m_phase = Phase::Held;
    uint64_t m_phaseStart = 0;   // output frame the phase began at
    double m_phaseFrom = 1.0;    // and the speed it began from
    double m_speed = 1.0;        // the speed now
    bool m_finished = false, m_finishTaken = false;
    uint64_t m_finishedAt = 0;   // the output frame it was wound down by
    ScrubStyle m_style;
    const ScrubTiming m_timing;
    const int m_direction;
    std::atomic<float> m_topSpeed;
    const double m_sourceRate, m_outputRate;
    const double m_start;
    std::unique_ptr<Stretch> m_stretch;
    std::atomic<float> m_pitch{0.0f}, m_rate{1.0f};
    std::unique_ptr<Shift> m_shift;  // tape with a pitch: the pitch shifter
    bool m_shiftChecked = false;

    // What the resampler reads: source frames (tape) or stretched ones (spring),
    // interleaved, with a fractional read position into it
    std::vector<float> m_fifo;
    double m_pos = 0.0;
    uint64_t m_consumed = 0;  // source frames taken since the start (spring: after its pre-roll)
    bool m_sourceEnded = false;
    uint64_t m_produced = 0;  // output frames
    float m_last[2] = {};     // the last output frame, to fade from at the start or end

    struct Point {
        uint64_t out;
        double seconds;
    };
    std::deque<Point> m_points;

    double Speed() const;
    int Generate(PcmSource& source, float* out, int frames);
    int FillShifted(PcmSource& source, float* out, int frames);
    size_t FifoFrames() const { return m_fifo.size() / 2; }
    bool Supply(PcmSource& source, size_t need, double speed);
    void Resample(float* out, int frames, double step);
    double SourcePosition(double speed) const;
};

}  // namespace audio

#endif  // FASTPLAY_SCRUBBER_H
