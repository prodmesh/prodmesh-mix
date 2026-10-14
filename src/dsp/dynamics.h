// Channel-strip dynamics: gate, compressor, peak limiter. Feed-forward,
// log-domain gain computers with one-pole attack/release smoothing — the
// standard broadcast-console topology, cheap enough for 128 paths at 96 kHz.
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace pm::dsp {

inline float dbToGain(float dB) { return std::pow(10.0f, dB / 20.0f); }
inline float gainToDb(float g) {
    return 20.0f * std::log10(std::max(g, 1e-9f));
}

// Fast log2 / exp2 for per-sample gain computers, accurate to ~1e-4 dB
// (checked in tests) and several times cheaper than libm's log10 / pow.
//
// log2: split exponent and mantissa m in [1,2), then the atanh series
//   ln m = 2 (t + t^3/3 + t^5/5 + t^7/7),  t = (m-1)/(m+1) in [0, 1/3].
inline float fastLog2(float x) {
    const auto bits = std::bit_cast<std::uint32_t>(std::max(x, 1e-30f));
    const float e = float(int((bits >> 23) & 0xff) - 127);
    const float m = std::bit_cast<float>((bits & 0x007fffffu) | 0x3f800000u);
    const float t = (m - 1.0f) / (m + 1.0f);
    const float t2 = t * t;
    const float series =
        t * (1.0f + t2 * (1.0f / 3 + t2 * (1.0f / 5 + t2 * (1.0f / 7))));
    return e + 2.8853900817779268f * series;  // 2 / ln 2
}

// exp2: round to the nearest integer n, f = x - n in [-0.5, 0.5], Taylor
// series of e^(f ln 2) to degree 5, scaled by 2^n through the exponent.
inline float fastExp2(float x) {
    x = std::clamp(x, -126.0f, 126.0f);
    const float n = std::nearbyint(x);
    const float y = (x - n) * 0.69314718f;
    const float p =
        1.0f + y * (1.0f + y * (0.5f + y * (1.0f / 6 +
                                             y * (1.0f / 24 + y * (1.0f / 120)))));
    return p * std::bit_cast<float>(std::uint32_t(int(n) + 127) << 23);
}

inline constexpr float kDbPerLog2 = 6.0205999f;  // 20 * log10(2)
inline float fastGainToDb(float g) { return kDbPerLog2 * fastLog2(g); }
inline float fastDbToGain(float dB) { return fastExp2(dB / kDbPerLog2); }

// One-pole coefficient for a time constant in ms.
inline float timeCoeff(double fs, double ms) {
    return ms <= 0 ? 0.0f : float(std::exp(-1.0 / (fs * ms * 0.001)));
}

struct CompressorParams {
    float thresholdDb = -18, ratio = 3, kneeDb = 6;
    float attackMs = 10, releaseMs = 120, makeupDb = 0;
    bool operator==(const CompressorParams&) const = default;
};

class Compressor {
public:
    void prepare(double fs, const CompressorParams& p) {
        p_ = p;
        att_ = timeCoeff(fs, p.attackMs);
        rel_ = timeCoeff(fs, p.releaseMs);
        makeup_ = dbToGain(p.makeupDb);
    }
    void reset() { envDb_ = 0; }

    // Processes `nch` channels with a linked detector (max across channels).
    void process(float* const* ch, int nch, int n) {
        for (int i = 0; i < n; ++i) {
            float peak = 0;
            for (int c = 0; c < nch; ++c)
                peak = std::max(peak, std::fabs(ch[c][i]));
            const float over = gainComputer(fastGainToDb(peak));
            const float k = over > envDb_ ? att_ : rel_;
            envDb_ = over + k * (envDb_ - over);
            const float g = fastDbToGain(-envDb_) * makeup_;
            for (int c = 0; c < nch; ++c) ch[c][i] *= g;
        }
    }

    float gainReductionDb() const { return envDb_; }

private:
    // Returns gain reduction in dB (>= 0) for an input level.
    float gainComputer(float xDb) const {
        const float t = p_.thresholdDb, w = p_.kneeDb;
        const float slope = 1.0f - 1.0f / p_.ratio;
        const float d = xDb - t;
        if (2 * d < -w) return 0;
        if (2 * d > w) return slope * d;
        const float k = d + w / 2;
        return slope * k * k / (2 * w);
    }

    CompressorParams p_;
    float att_ = 0, rel_ = 0, makeup_ = 1, envDb_ = 0;
};

struct GateParams {
    float thresholdDb = -50, rangeDb = -40;
    float attackMs = 1, holdMs = 50, releaseMs = 150;
    bool operator==(const GateParams&) const = default;
};

class Gate {
public:
    void prepare(double fs, const GateParams& p) {
        threshold_ = dbToGain(p.thresholdDb);
        floor_ = dbToGain(p.rangeDb);
        att_ = timeCoeff(fs, p.attackMs);
        rel_ = timeCoeff(fs, p.releaseMs);
        holdSamples_ = int(fs * p.holdMs * 0.001);
    }
    void reset() { gain_ = floor_; hold_ = 0; }

    void process(float* x, int n) {
        for (int i = 0; i < n; ++i) {
            if (std::fabs(x[i]) >= threshold_) hold_ = holdSamples_;
            const bool open = hold_ > 0;
            if (hold_ > 0) --hold_;
            const float target = open ? 1.0f : floor_;
            const float k = open ? att_ : rel_;
            gain_ = target + k * (gain_ - target);
            x[i] *= gain_;
        }
    }

private:
    float threshold_ = 0, floor_ = 0, att_ = 0, rel_ = 0, gain_ = 0;
    int holdSamples_ = 0, hold_ = 0;
};

// Zero-latency peak limiter: instant attack, smooth release. A true-peak
// lookahead limiter (v1) adds latency and goes through PDC instead.
class Limiter {
public:
    void prepare(double fs, float ceilingDb, float releaseMs) {
        ceiling_ = dbToGain(ceilingDb);
        rel_ = timeCoeff(fs, releaseMs);
    }
    void reset() { gain_ = 1; }

    void process(float* const* ch, int nch, int n) {
        for (int i = 0; i < n; ++i) {
            float peak = 0;
            for (int c = 0; c < nch; ++c)
                peak = std::max(peak, std::fabs(ch[c][i]));
            const float need = peak > ceiling_ ? ceiling_ / peak : 1.0f;
            gain_ = need < gain_ ? need : need + rel_ * (gain_ - need);
            for (int c = 0; c < nch; ++c) ch[c][i] *= gain_;
        }
    }

private:
    float ceiling_ = 1, rel_ = 0, gain_ = 1;
};

}  // namespace pm::dsp
