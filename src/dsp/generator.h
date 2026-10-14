// Deterministic test sources for benches and null tests: a sine plus noise,
// optionally gated in bursts so gates and compressors actually work. Same
// seed, same samples — renders are bit-reproducible.
#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>

namespace pm::dsp {

struct GeneratorParams {
    std::uint32_t seed = 1;
    float freqHz = 220, sineDb = -12, noiseDb = -30;
    bool bursts = true;  // ~speech-like on/off pattern
    bool operator==(const GeneratorParams&) const = default;
};

class Generator {
public:
    void prepare(double fs, const GeneratorParams& p) {
        fs_ = fs;
        inc_ = 2 * std::numbers::pi * p.freqHz / fs;
        sine_ = std::pow(10.0f, p.sineDb / 20.0f);
        noise_ = std::pow(10.0f, p.noiseDb / 20.0f);
        bursts_ = p.bursts;
        rng_ = p.seed * 2654435761u + 1;
        phase_ = 0;
        nextToggle_ = 0;
        on_ = true;
    }

    void process(float* x, int n) {
        for (int i = 0; i < n; ++i) {
            if (bursts_ && nextToggle_-- <= 0) {
                on_ = !on_;
                // 0.2–1.5 s segments
                nextToggle_ = int(fs_ * (0.2 + 1.3 * uniform()));
            }
            const float s = float(std::sin(phase_)) * sine_ +
                            (uniform() * 2 - 1) * noise_;
            phase_ += inc_;
            if (phase_ > 2 * std::numbers::pi) phase_ -= 2 * std::numbers::pi;
            x[i] = on_ ? s : s * 0.01f;
        }
    }

private:
    float uniform() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return float(rng_ >> 8) * (1.0f / 16777216.0f);
    }

    double fs_ = 48000, inc_ = 0, phase_ = 0;
    float sine_ = 0, noise_ = 0;
    std::uint32_t rng_ = 1;
    long nextToggle_ = 0;
    bool bursts_ = true, on_ = true;
};

}  // namespace pm::dsp
