// Programme loudness per ITU-R BS.1770-4 / EBU R128: K-weighting,
// momentary (400 ms), short-term (3 s) and gated integrated loudness, and
// true peak (4x oversampled). Written from the published recommendation.
//
// Not for the audio thread: the engine hands the program bus to a ring
// (RecordTap) and the daemon runs this at its leisure.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <numbers>
#include <vector>

namespace pm::dsp {

// One second-order section, direct form I in double (the K-weighting
// shelf sits close to the unit circle at high sample rates).
struct KStage {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double run(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

// The two K-weighting stages for a sample rate: the spec's 48 kHz
// coefficients come from these analog prototypes (shelf at 1681.97 Hz,
// +4 dB; high-pass at 38.14 Hz), so any rate gets the same response.
inline void kWeighting(double fs, KStage& shelf, KStage& hp) {
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347,
                     Q = 0.7071752369554196;
        const double K = std::tan(std::numbers::pi * f0 / fs);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
        shelf.b1 = 2.0 * (K * K - Vh) / a0;
        shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
        shelf.a1 = 2.0 * (K * K - 1.0) / a0;
        shelf.a2 = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(std::numbers::pi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;
        hp.b0 = 1.0;
        hp.b1 = -2.0;
        hp.b2 = 1.0;
        hp.a1 = 2.0 * (K * K - 1.0) / a0;
        hp.a2 = (1.0 - K / Q + K * K) / a0;
    }
}

// 4x oversampling peak detector: a 48-tap windowed-sinc interpolator
// (12 taps per phase), each phase normalised to unity at DC.
class TruePeak {
public:
    TruePeak() {
        constexpr int N = 48;
        double h[N];
        for (int n = 0; n < N; ++n) {
            const double t = (n - (N - 1) / 2.0) / 4.0;
            const double sinc = t == 0 ? 1.0 : std::sin(std::numbers::pi * t) /
                                                   (std::numbers::pi * t);
            const double w = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * (n + 0.5) / N);
            h[n] = sinc * w;
        }
        for (int p = 0; p < 4; ++p) {
            double sum = 0;
            for (int k = 0; k < 12; ++k) sum += h[p + 4 * k];
            for (int k = 0; k < 12; ++k) coef_[p][k] = h[p + 4 * k] / sum;
        }
    }
    // Returns the largest |sample| of the oversampled signal for x.
    double push(double x) {
        pos_ = (pos_ + 11) % 12;
        hist_[pos_] = x;
        double peak = 0;
        for (int p = 0; p < 4; ++p) {
            double y = 0;
            for (int k = 0; k < 12; ++k) y += coef_[p][k] * hist_[(pos_ + k) % 12];
            peak = std::max(peak, std::fabs(y));
        }
        return std::max(peak, std::fabs(x));
    }

private:
    double coef_[4][12]{};
    double hist_[12]{};
    int pos_ = 0;
};

class LoudnessMeter {
public:
    static constexpr double kFloor = -120.0;  // "no signal", in LUFS / dB

    LoudnessMeter(double sampleRate, int channels)
        : fs_(sampleRate), channels_(std::clamp(channels, 1, 8)),
          sub_(std::size_t(std::lround(sampleRate / 10))),  // 100 ms
          k_(std::size_t(channels_)), tp_(std::size_t(channels_)),
          acc_(std::size_t(channels_), 0.0) {
        for (auto& [shelf, hp] : k_) kWeighting(fs_, shelf, hp);
    }

    // Interleaved frames.
    void process(const float* x, std::size_t frames) {
        for (std::size_t i = 0; i < frames; ++i) {
            for (int c = 0; c < channels_; ++c) {
                const double v = x[i * std::size_t(channels_) + std::size_t(c)];
                auto& [shelf, hp] = k_[std::size_t(c)];
                const double y = hp.run(shelf.run(v));
                acc_[std::size_t(c)] += y * y;
                const double p = tp_[std::size_t(c)].push(v);
                tpRecent_ = std::max(tpRecent_, p);
                tpMax_ = std::max(tpMax_, p);
            }
            if (++inSub_ == sub_) closeSubBlock();
        }
    }

    double momentary() const { return lufs(meanOfLast(4)); }
    double shortTerm() const { return lufs(meanOfLast(30)); }
    double maxMomentary() const { return maxMomentary_; }

    // Gated over everything since reset(): absolute gate -70 LUFS, then
    // relative gate 10 LU below the absolute-gated loudness.
    double integrated() const {
        double sum = 0;
        std::size_t n = 0;
        for (double p : blocks_) {
            sum += p;
            ++n;
        }
        if (!n) return kFloor;
        const double rel = power(lufs(sum / double(n)) - 10.0);
        sum = 0;
        n = 0;
        for (double p : blocks_)
            if (p > rel) {
                sum += p;
                ++n;
            }
        return n ? lufs(sum / double(n)) : kFloor;
    }

    // dBTP: highest since reset(), and since the last takeRecentPeak().
    double truePeakMax() const { return db(tpMax_); }
    double takeRecentPeak() {
        const double p = db(tpRecent_);
        tpRecent_ = 0;
        return p;
    }
    double seconds() const { return double(subBlocks_) / 10.0; }

    void reset() {
        blocks_.clear();
        subs_.clear();
        tpMax_ = tpRecent_ = 0;
        maxMomentary_ = kFloor;
        subBlocks_ = 0;
    }

private:
    static double lufs(double power) {
        return power > 0 ? std::max(kFloor, -0.691 + 10.0 * std::log10(power)) : kFloor;
    }
    static double power(double l) { return std::pow(10.0, (l + 0.691) / 10.0); }
    static double db(double lin) {
        return lin > 1e-6 ? 20.0 * std::log10(lin) : kFloor;
    }
    double meanOfLast(std::size_t n) const {
        if (subs_.empty()) return 0;
        n = std::min(n, subs_.size());
        double s = 0;
        for (std::size_t i = subs_.size() - n; i < subs_.size(); ++i) s += subs_[i];
        return s / double(n);
    }

    // Channel weights: 1.0 for front channels (1.41 for surrounds, which
    // this console does not have yet).
    void closeSubBlock() {
        double p = 0;
        for (auto& a : acc_) {
            p += a / double(sub_);
            a = 0;
        }
        inSub_ = 0;
        ++subBlocks_;
        subs_.push_back(p);
        if (subs_.size() > 30) subs_.pop_front();
        // A 400 ms gating block every 100 ms (75% overlap).
        if (subs_.size() >= 4) {
            const double block = meanOfLast(4);
            maxMomentary_ = std::max(maxMomentary_, lufs(block));
            if (lufs(block) > -70.0) blocks_.push_back(block);
        }
    }

    double fs_;
    int channels_;
    std::size_t sub_, inSub_ = 0;
    std::vector<std::pair<KStage, KStage>> k_;
    std::vector<TruePeak> tp_;
    std::vector<double> acc_;
    std::deque<double> subs_;     // last 30 sub-block powers (3 s)
    std::vector<double> blocks_;  // gating blocks above -70 LUFS
    double tpMax_ = 0, tpRecent_ = 0, maxMomentary_ = kFloor;
    std::uint64_t subBlocks_ = 0;
};

}  // namespace pm::dsp
