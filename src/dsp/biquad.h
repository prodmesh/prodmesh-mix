// RBJ "Audio EQ Cookbook" biquads in transposed direct form II. Coefficients
// are computed on the control side; the audio thread only runs process().
#pragma once

#include <cmath>
#include <numbers>

namespace pm::dsp {

struct BiquadCoeffs {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;  // a0 normalised to 1

    static BiquadCoeffs identity() { return {}; }

    static BiquadCoeffs peaking(double fs, double f0, double q, double dB) {
        const double A = std::pow(10.0, dB / 40.0);
        const double w = 2 * std::numbers::pi * f0 / fs;
        const double alpha = std::sin(w) / (2 * q);
        const double c = std::cos(w);
        return norm(1 + alpha * A, -2 * c, 1 - alpha * A,
                    1 + alpha / A, -2 * c, 1 - alpha / A);
    }

    static BiquadCoeffs highpass(double fs, double f0, double q) {
        const double w = 2 * std::numbers::pi * f0 / fs;
        const double alpha = std::sin(w) / (2 * q);
        const double c = std::cos(w);
        return norm((1 + c) / 2, -(1 + c), (1 + c) / 2,
                    1 + alpha, -2 * c, 1 - alpha);
    }

    static BiquadCoeffs lowShelf(double fs, double f0, double dB) {
        return shelf(fs, f0, dB, false);
    }
    static BiquadCoeffs highShelf(double fs, double f0, double dB) {
        return shelf(fs, f0, dB, true);
    }

private:
    static BiquadCoeffs norm(double b0, double b1, double b2, double a0,
                             double a1, double a2) {
        return {float(b0 / a0), float(b1 / a0), float(b2 / a0),
                float(a1 / a0), float(a2 / a0)};
    }

    // Shelf slope S = 1.
    static BiquadCoeffs shelf(double fs, double f0, double dB, bool high) {
        const double A = std::pow(10.0, dB / 40.0);
        const double w = 2 * std::numbers::pi * f0 / fs;
        const double c = std::cos(w);
        const double alpha = std::sin(w) / 2 * std::sqrt(2.0);
        const double sa = 2 * std::sqrt(A) * alpha;
        if (!high)
            return norm(A * ((A + 1) - (A - 1) * c + sa),
                        2 * A * ((A - 1) - (A + 1) * c),
                        A * ((A + 1) - (A - 1) * c - sa),
                        (A + 1) + (A - 1) * c + sa,
                        -2 * ((A - 1) + (A + 1) * c),
                        (A + 1) + (A - 1) * c - sa);
        return norm(A * ((A + 1) + (A - 1) * c + sa),
                    -2 * A * ((A - 1) + (A + 1) * c),
                    A * ((A + 1) + (A - 1) * c - sa),
                    (A + 1) - (A - 1) * c + sa,
                    2 * ((A - 1) - (A + 1) * c),
                    (A + 1) - (A - 1) * c - sa);
    }
};

class Biquad {
public:
    void setCoeffs(const BiquadCoeffs& c) { c_ = c; }
    void reset() { z1_ = z2_ = 0; }

    void process(float* x, int n) {
        float z1 = z1_, z2 = z2_;
        const auto c = c_;
        for (int i = 0; i < n; ++i) {
            const float in = x[i];
            const float out = c.b0 * in + z1;
            z1 = c.b1 * in - c.a1 * out + z2;
            z2 = c.b2 * in - c.a2 * out;
            x[i] = out;
        }
        z1_ = z1;
        z2_ = z2;
    }

private:
    BiquadCoeffs c_;
    float z1_ = 0, z2_ = 0;
};

}  // namespace pm::dsp
