// Scrubbing: see scrubber.h.

#include "scrubber.h"

#include <algorithm>
#include <cmath>

#ifdef USE_SIGNALSMITH
#include "signalsmith-stretch.h"
#endif

namespace audio {
namespace {

const int kChannels = 2;
// Spring: seconds held for the speed to double (from normal speed)
const double kSpringDoubling = 0.5;
// Tape: seconds to spin up to speed, and let go of, back down
const double kTapeSpinUp = 0.25;
const double kTapeSpinDown = 0.4;
// A tape stop: seconds from normal speed to a standstill
const double kTapeStopSeconds = 0.6;
// Slower than this is a standstill: silence. Below kQuietSpeed it fades, so the
// last rumble does not end in a thump.
const double kStopped = 0.01;
const double kQuietSpeed = 0.15;
// Output frames worked out at one speed; the speed changes between them
const int kSpeedStep = 64;
// Stretched frames made at a time (spring)
const int kStretchBlock = 256;

}  // namespace

struct Scrubber::Stretch {
#ifdef USE_SIGNALSMITH
    signalsmith::stretch::SignalsmithStretch<float> stretcher;
#endif
    bool started = false;
    double inFraction = 0.0;  // the fraction of an input frame carried between blocks
    std::vector<float> interleaved;
    std::vector<float> in[kChannels], out[kChannels];
    float* inPtrs[kChannels] = {};
    float* outPtrs[kChannels] = {};

    void Deinterleave(int frames) {
        for (int ch = 0; ch < kChannels; ch++) {
            in[ch].resize(static_cast<size_t>(std::max(frames, 1)));
            for (int i = 0; i < frames; i++) in[ch][i] = interleaved[static_cast<size_t>(i) * kChannels + ch];
            inPtrs[ch] = in[ch].data();
        }
    }
};

Scrubber::Scrubber(ScrubStyle style, int direction, float speed, int sourceRate, int outputRate, double start,
                   Begin begin, double fromSpeed)
    : m_style(begin == Begin::Held ? style : ScrubStyle::Tape),
      m_direction(direction < 0 ? -1 : 1),
      m_topSpeed(speed),
      m_sourceRate(sourceRate),
      m_outputRate(outputRate),
      m_start(start) {
#ifdef USE_SIGNALSMITH
    if (style == ScrubStyle::Spring) {
        m_stretch = std::make_unique<Stretch>();
        m_stretch->stretcher.presetDefault(kChannels, static_cast<float>(sourceRate));
    }
#else
    m_style = ScrubStyle::Tape;  // nothing to stretch with
#endif
    if (begin == Begin::WindDown) {
        m_phase = Phase::Release;
        m_phaseFrom = fromSpeed;
    } else if (begin == Begin::TapeStop) {
        m_phase = Phase::TapeStop;
        m_phaseFrom = 1.0;
    }
    m_points.push_back({0, start});
}

Scrubber::~Scrubber() = default;

double Scrubber::HeardSpeed(uint64_t played) const {
    const double top = std::max(1.0, static_cast<double>(m_topSpeed.load()));
    const double held = static_cast<double>(played) / m_outputRate;
    if (m_style == ScrubStyle::Spring) return std::min(top, std::pow(2.0, held / kSpringDoubling));
    return std::pow(top, std::min(1.0, held / kTapeSpinUp));
}

bool Scrubber::TakeFinished(uint64_t played) {
    // The output is made ahead of what is heard: not until it has been heard
    if (!m_finished || m_finishTaken || played < m_finishedAt) return false;
    m_finishTaken = true;
    return true;
}

double Scrubber::Speed() const {
    const double top = std::max(1.0, static_cast<double>(m_topSpeed.load()));
    const double since = static_cast<double>(m_produced - m_phaseStart) / m_outputRate;
    switch (m_phase) {
        case Phase::Held:
            (void)top;
            return HeardSpeed(m_produced);
        case Phase::Release: {
            const double t = std::min(1.0, since / kTapeSpinDown);
            // Forward, back down to normal speed (evenly in pitch); backward, slowing
            // to a stop before it plays on forward
            if (m_direction > 0) return std::pow(std::max(1.0, m_phaseFrom), 1.0 - t);
            return m_phaseFrom * (1.0 - t);
        }
        case Phase::TapeStop:
        default: {
            const double t = std::min(1.0, since / kTapeStopSeconds);
            return m_phaseFrom * (1.0 - t);
        }
    }
}

int Scrubber::Fill(PcmSource& source, float* out, int frames) {
    int done = 0;
    while (done < frames) {
        const int n = std::min(kSpeedStep, frames - done);
        const double speed = Speed();
        m_speed = speed;
        if (m_phase != Phase::Held) {
            // Wound down: to normal speed (and on at it), or to a standstill
            const bool atNormal = m_phase == Phase::Release && m_direction > 0 && speed <= 1.0001;
            if ((atNormal || speed < kStopped) && !m_finished) {
                m_finished = true;
                m_finishedAt = m_produced;
            }
            if (speed < kStopped) {
                for (int i = done; i < frames; i++) {
                    m_last[0] *= 0.99f;
                    m_last[1] *= 0.99f;
                    out[static_cast<size_t>(i) * kChannels] = m_last[0];
                    out[static_cast<size_t>(i) * kChannels + 1] = m_last[1];
                }
                m_produced += static_cast<uint64_t>(frames - done);
                done = frames;
                break;
            }
        }
        // Tape plays the source faster; spring's stretcher has already done that
        const double step = (m_stretch ? 1.0 : speed) * m_sourceRate / m_outputRate;
        const size_t need = static_cast<size_t>(m_pos + n * step) + 2;
        if (FifoFrames() < need && !Supply(source, need, speed)) {
            if (!m_sourceEnded) break;  // the decoder is behind
            // At the start or the end: silence, staying there, ramped down to from
            // the last sound over a few milliseconds so it doesn't click
            for (int i = done; i < frames; i++) {
                m_last[0] *= 0.99f;
                m_last[1] *= 0.99f;
                out[static_cast<size_t>(i) * kChannels] = m_last[0];
                out[static_cast<size_t>(i) * kChannels + 1] = m_last[1];
            }
            m_produced += static_cast<uint64_t>(frames - done);
            done = frames;
            break;
        }
        Resample(out + static_cast<size_t>(done) * kChannels, n, step);
        if (m_phase != Phase::Held && speed < kQuietSpeed) {
            const float level = static_cast<float>(speed / kQuietSpeed);
            for (int i = 0; i < n * kChannels; i++) out[static_cast<size_t>(done) * kChannels + i] *= level;
        }
        m_last[0] = out[static_cast<size_t>(done + n - 1) * kChannels];
        m_last[1] = out[static_cast<size_t>(done + n - 1) * kChannels + 1];
        done += n;
        m_produced += static_cast<uint64_t>(n);
    }
    if (done > 0) {
        m_points.push_back({m_produced, SourcePosition(Speed())});
        if (m_points.size() > 4096) m_points.pop_front();
    }
    return done;
}

double Scrubber::PositionAt(uint64_t played) {
    while (m_points.size() > 1 && m_points[1].out <= played) m_points.pop_front();
    const Point& a = m_points.front();
    if (m_points.size() == 1 || played <= a.out) return a.seconds;
    const Point& b = m_points[1];
    double t = static_cast<double>(played - a.out) / static_cast<double>(b.out - a.out);
    return a.seconds + (b.seconds - a.seconds) * t;
}

// Where in the source the output has got to: what was taken from it, less what is
// still waiting in the fifo (each stretched frame stands for `speed` source frames).
double Scrubber::SourcePosition(double speed) const {
    double pending = static_cast<double>(FifoFrames()) - m_pos;
    double frames = static_cast<double>(m_consumed) - pending * (m_stretch ? speed : 1.0);
    double seconds = m_start + m_direction * std::max(0.0, frames) / m_sourceRate;
    return std::max(0.0, seconds);
}

// Tops the fifo up to `need` frames. False if the source ran short.
bool Scrubber::Supply(PcmSource& source, size_t need, double speed) {
    if (!m_stretch) {
        while (FifoFrames() < need) {
            int available = source.Available();
            if (available <= 0) {
                if (source.Ended()) m_sourceEnded = true;
                return false;
            }
            size_t n = std::min(need - FifoFrames(), static_cast<size_t>(available));
            size_t old = m_fifo.size();
            m_fifo.resize(old + n * kChannels);
            int got = source.Read(m_fifo.data() + old, static_cast<int>(n));
            m_fifo.resize(old + static_cast<size_t>(got) * kChannels);
            m_consumed += static_cast<uint64_t>(got);
        }
        return true;
    }
#ifdef USE_SIGNALSMITH
    Stretch& s = *m_stretch;
    if (!s.started) {
        // Pre-roll, so the first output lines up with where scrubbing began
        const int seekLength = s.stretcher.outputSeekLength(static_cast<float>(speed));
        if (source.Available() < seekLength) {
            if (source.Ended()) m_sourceEnded = true;
            return false;
        }
        s.interleaved.assign(static_cast<size_t>(seekLength) * kChannels, 0.0f);
        source.Read(s.interleaved.data(), seekLength);
        s.Deinterleave(seekLength);
        s.stretcher.outputSeek(s.inPtrs, seekLength);
        s.started = true;
    }
    while (FifoFrames() < need) {
        const double wanted = s.inFraction + kStretchBlock * speed;
        const int inFrames = static_cast<int>(wanted);
        if (source.Available() < inFrames) {
            if (source.Ended()) m_sourceEnded = true;
            return false;
        }
        s.inFraction = wanted - inFrames;
        s.interleaved.resize(static_cast<size_t>(inFrames) * kChannels);
        source.Read(s.interleaved.data(), inFrames);
        s.Deinterleave(inFrames);
        for (int ch = 0; ch < kChannels; ch++) {
            s.out[ch].resize(kStretchBlock);
            s.outPtrs[ch] = s.out[ch].data();
        }
        s.stretcher.process(s.inPtrs, inFrames, s.outPtrs, kStretchBlock);
        size_t old = m_fifo.size();
        m_fifo.resize(old + static_cast<size_t>(kStretchBlock) * kChannels);
        for (int i = 0; i < kStretchBlock; i++) {
            m_fifo[old + static_cast<size_t>(i) * kChannels] = s.out[0][i];
            m_fifo[old + static_cast<size_t>(i) * kChannels + 1] = s.out[1][i];
        }
        m_consumed += static_cast<uint64_t>(inFrames);
    }
    return true;
#else
    (void)speed;
    return false;
#endif
}

// `frames` of output from the fifo, `step` fifo frames apart: interpolated when
// slower than the source, averaged over each step when faster, so the highs that
// speeding up pushes past what the output can carry are smoothed away rather than
// folded back as noise.
void Scrubber::Resample(float* out, int frames, double step) {
    const float* in = m_fifo.data();
    for (int j = 0; j < frames; j++) {
        const size_t i = static_cast<size_t>(m_pos);
        float left, right;
        if (step <= 1.0) {
            const float f = static_cast<float>(m_pos - static_cast<double>(i));
            left = in[i * 2] + (in[i * 2 + 2] - in[i * 2]) * f;
            right = in[i * 2 + 1] + (in[i * 2 + 3] - in[i * 2 + 1]) * f;
        } else {
            const size_t end = static_cast<size_t>(m_pos + step);
            left = right = 0.0f;
            for (size_t k = i; k < end; k++) {
                left += in[k * 2];
                right += in[k * 2 + 1];
            }
            const float scale = 1.0f / static_cast<float>(end - i);
            left *= scale;
            right *= scale;
        }
        out[static_cast<size_t>(j) * kChannels] = left;
        out[static_cast<size_t>(j) * kChannels + 1] = right;
        m_pos += step;
    }
    const size_t used = static_cast<size_t>(m_pos);
    m_fifo.erase(m_fifo.begin(), m_fifo.begin() + static_cast<std::ptrdiff_t>(used * kChannels));
    m_pos -= static_cast<double>(used);
}

}  // namespace audio
