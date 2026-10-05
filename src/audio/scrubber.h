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
// it winds back down (to normal speed going forward, to a stop going back). A tape
// stop is tape slowing from normal speed to a standstill. Spring: time-stretched
// (Signalsmith Stretch), so pitch stays, and it winds up the longer it is held,
// doubling in speed every half second up to the top speed.

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
    // How it starts: held (spinning up), winding down from `fromSpeed` (a tape
    // let go of), or a tape stop (forward from normal speed to a standstill).
    enum class Begin { Held, WindDown, TapeStop };

    // `speed`: tape's speed, or spring's top speed (times normal).
    Scrubber(ScrubStyle style, int direction, float speed, int sourceRate, int outputRate, double start,
             Begin begin = Begin::Held, double fromSpeed = 1.0);
    ~Scrubber();

    // Fills `frames` of stereo output at the output rate: fewer if the source is
    // behind. Past the start or end of the source it is silence, staying there.
    int Fill(PcmSource& source, float* out, int frames);
    // Source seconds at output frame `played` (counted from the start of scrubbing)
    double PositionAt(uint64_t played);
    void SetSpeed(float speed) { m_topSpeed = speed; }
    ScrubStyle Style() const { return m_style; }
    int Direction() const { return m_direction; }
    float TopSpeed() const { return m_topSpeed; }
    // The speed of what is heard at output frame `played`, while held
    double HeardSpeed(uint64_t played) const;
    // Wound down (to normal speed, or to a standstill) and heard to the end of it,
    // `played` being the output frames heard: true once
    bool TakeFinished(uint64_t played);
    // Winding down rather than held
    bool Releasing() const { return m_phase != Phase::Held; }

private:
    struct Stretch;

    enum class Phase { Held, Release, TapeStop };
    Phase m_phase = Phase::Held;
    uint64_t m_phaseStart = 0;   // output frame the phase began at
    double m_phaseFrom = 1.0;    // and the speed it began from
    double m_speed = 1.0;        // the speed now
    bool m_finished = false, m_finishTaken = false;
    uint64_t m_finishedAt = 0;   // the output frame it was wound down by
    ScrubStyle m_style;
    const int m_direction;
    std::atomic<float> m_topSpeed;
    const double m_sourceRate, m_outputRate;
    const double m_start;
    std::unique_ptr<Stretch> m_stretch;

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
    size_t FifoFrames() const { return m_fifo.size() / 2; }
    bool Supply(PcmSource& source, size_t need, double speed);
    void Resample(float* out, int frames, double step);
    double SourcePosition(double speed) const;
};

}  // namespace audio

#endif  // FASTPLAY_SCRUBBER_H
