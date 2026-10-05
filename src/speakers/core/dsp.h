#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace speakers {

// Shared DSP building blocks. Everything here is per-channel mono state: a
// stereo stage owns two of whatever it needs. All filters are direct-form-II
// transposed biquads, which stay well behaved at the low corner frequencies
// subwoofer simulation needs.

namespace dsp {

constexpr float kPi = 3.14159265358979f;

inline float DbToGain(float db) { return std::pow(10.0f, db * 0.05f); }
inline float GainToDb(float gain) { return 20.0f * std::log10(std::max(gain, 1e-9f)); }

inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------------------
// Biquad
// ---------------------------------------------------------------------------
struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void Reset() { z1 = z2 = 0.0f; }

    inline float Process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    // RBJ cookbook designs. `sr` is the sample rate in Hz.
    void SetLowpass(float sr, float freq, float q);
    void SetHighpass(float sr, float freq, float q);
    void SetBandpass(float sr, float freq, float q); // constant peak gain
    void SetPeaking(float sr, float freq, float q, float gainDb);
    void SetLowShelf(float sr, float freq, float q, float gainDb);
    void SetHighShelf(float sr, float freq, float q, float gainDb);
    void SetAllpass(float sr, float freq, float q);
};

// ---------------------------------------------------------------------------
// Cascaded biquads: Butterworth of arbitrary even order, and Linkwitz-Riley.
//
// Driver roll-off, crossovers and band limiting all want a specific slope, so
// these are expressed in dB/octave terms by the callers:
//   order 2 = 12 dB/oct, order 4 = 24 dB/oct, order 8 = 48 dB/oct.
// ---------------------------------------------------------------------------
class Cascade {
public:
    void Reset();
    inline float Process(float x) {
        for (auto &b : m_stages) x = b.Process(x);
        return x;
    }
    bool Empty() const { return m_stages.empty(); }
    void Clear() { m_stages.clear(); }

    // Butterworth: maximally flat passband. `order` is rounded up to even.
    void SetButterworthLowpass(float sr, float freq, int order);
    void SetButterworthHighpass(float sr, float freq, int order);

    // Linkwitz-Riley: two cascaded Butterworth sections of half the order, so
    // that a matched low/high pair sums to a flat magnitude response. This is
    // what the system crossover between subs and mains uses.
    void SetLinkwitzRileyLowpass(float sr, float freq, int order);
    void SetLinkwitzRileyHighpass(float sr, float freq, int order);

private:
    std::vector<Biquad> m_stages;
};

// ---------------------------------------------------------------------------
// One-pole lowpass. Used where the exact slope does not matter but cheapness
// and smooth coefficient modulation do: head shadow, air absorption, envelopes.
// ---------------------------------------------------------------------------
struct OnePole {
    float a = 0.0f, z = 0.0f;

    void SetCutoff(float sr, float freq) {
        freq = Clampf(freq, 1.0f, sr * 0.49f);
        a = std::exp(-2.0f * kPi * freq / sr);
    }
    void Reset() { z = 0.0f; }
    inline float Process(float x) {
        z = x * (1.0f - a) + z * a;
        return z;
    }
};

// Smooths a control value towards a target, one sample at a time, so that
// moving speakers or turning the listener never clicks.
struct Smoother {
    float value = 0.0f, target = 0.0f, coeff = 0.0f;

    void SetTime(float sr, float ms) { coeff = std::exp(-1.0f / (std::max(ms, 0.01f) * 0.001f * sr)); }
    void Snap(float v) { value = target = v; }
    inline float Next() {
        value = target + (value - target) * coeff;
        return value;
    }
};

// ---------------------------------------------------------------------------
// Fractional delay line, the basis of the interaural time difference, the
// early reflections and the reverb tank.
// ---------------------------------------------------------------------------
class DelayLine {
public:
    void Init(int maxSamples);
    void Reset();
    inline void Write(float x) {
        m_buf[m_write] = x;
        if (++m_write >= (int)m_buf.size()) m_write = 0;
    }
    // Linear interpolation is plenty for the sub-sample shifts we ask for, and
    // its gentle high-frequency loss is itself a reasonable head-shadow cue.
    inline float Read(float delaySamples) const {
        delaySamples = Clampf(delaySamples, 0.0f, (float)m_buf.size() - 2.0f);
        float pos = (float)m_write - delaySamples;
        if (pos < 0.0f) pos += (float)m_buf.size();
        int i0 = (int)pos;
        float frac = pos - (float)i0;
        // A hair below zero plus the size rounds to the size itself in a float.
        if (i0 >= (int)m_buf.size()) i0 -= (int)m_buf.size();
        int i1 = i0 + 1;
        if (i1 >= (int)m_buf.size()) i1 = 0;
        return m_buf[i0] + (m_buf[i1] - m_buf[i0]) * frac;
    }
    inline float ReadInt(int delaySamples) const {
        int pos = m_write - delaySamples;
        if (pos < 0) pos += (int)m_buf.size();
        return m_buf[pos];
    }
    int Capacity() const { return (int)m_buf.size(); }

private:
    std::vector<float> m_buf;
    int m_write = 0;
};

// ---------------------------------------------------------------------------
// Envelope follower with separate attack and release, in dB-free linear terms.
// Drives the driver excursion model.
// ---------------------------------------------------------------------------
struct Envelope {
    float attack = 0.0f, release = 0.0f, value = 0.0f;

    void SetTimes(float sr, float attackMs, float releaseMs) {
        attack = std::exp(-1.0f / (std::max(attackMs, 0.01f) * 0.001f * sr));
        release = std::exp(-1.0f / (std::max(releaseMs, 0.01f) * 0.001f * sr));
    }
    void Reset() { value = 0.0f; }
    inline float Process(float x) {
        float rect = std::fabs(x);
        float c = rect > value ? attack : release;
        value = rect + (value - rect) * c;
        return value;
    }
};

// ---------------------------------------------------------------------------
// Deterministic white noise. A fixed seed keeps renders reproducible.
// ---------------------------------------------------------------------------
struct Noise {
    uint32_t state = 0x9e3779b9u;

    explicit Noise(uint32_t seed = 0x9e3779b9u) : state(seed ? seed : 1u) {}
    inline float Next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float)((int32_t)state) * (1.0f / 2147483648.0f);
    }
};

// Soft saturation used for amplifier clipping and cone compression. Unity slope
// at the origin, so quiet signals pass through untouched.
inline float SoftClip(float x) {
    return std::tanh(x);
}

} // namespace dsp

}  // namespace speakers
