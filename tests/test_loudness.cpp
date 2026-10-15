// Loudness metering against BS.1770-4 and EBU Tech 3341 reference cases.
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "dsp/loudness.h"
#include "test.h"

using namespace pm;

namespace {

// Stereo sine, same in both channels, `dBFS` peak.
void tone(dsp::LoudnessMeter& m, double fs, double hz, double dBFS, double seconds,
          double phase = 0) {
    const auto n = std::size_t(seconds * fs);
    const double a = std::pow(10.0, dBFS / 20.0);
    std::vector<float> buf(2 * 4800);
    std::size_t done = 0;
    while (done < n) {
        const std::size_t k = std::min<std::size_t>(4800, n - done);
        for (std::size_t i = 0; i < k; ++i) {
            const double t = double(done + i) / fs;
            const float v = float(a * std::sin(2 * std::numbers::pi * hz * t + phase));
            buf[2 * i] = buf[2 * i + 1] = dBFS <= -200 ? 0.0f : v;
        }
        m.process(buf.data(), k);
        done += k;
    }
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
std::string s(double v) { return std::to_string(v); }

}  // namespace

TEST(k_weighting_matches_the_spec_at_48k) {
    dsp::KStage shelf, hp;
    dsp::kWeighting(48000, shelf, hp);
    // BS.1770-4, tables 1 and 2.
    CHECK(near(shelf.b0, 1.53512485958697, 1e-9));
    CHECK(near(shelf.b1, -2.69169618940638, 1e-9));
    CHECK(near(shelf.b2, 1.19839281085285, 1e-9));
    CHECK(near(shelf.a1, -1.69065929318241, 1e-9));
    CHECK(near(shelf.a2, 0.73248077421585, 1e-9));
    CHECK(near(hp.a1, -1.99004745483398, 1e-9));
    CHECK(near(hp.a2, 0.99007225036621, 1e-9));
}

// Tech 3341 case 1/2: stereo 1 kHz at -23 dBFS reads -23.0 LUFS (M, S, I),
// at 48 and 44.1 kHz.
TEST(loudness_of_the_reference_tone) {
    for (double fs : {48000.0, 44100.0, 96000.0}) {
        dsp::LoudnessMeter m(fs, 2);
        tone(m, fs, 1000, -23, 20);
        CHECK_MSG(near(m.integrated(), -23.0, 0.1), "I " + s(m.integrated()) + " @ " + s(fs));
        CHECK_MSG(near(m.momentary(), -23.0, 0.1), "M " + s(m.momentary()));
        CHECK_MSG(near(m.shortTerm(), -23.0, 0.1), "S " + s(m.shortTerm()));
    }
}

// Tech 3341 cases 3-5: the relative gate drops quiet passages, the
// absolute gate drops near-silence; all read -23.0 integrated.
TEST(loudness_gating) {
    struct Seg { double dB, s; };
    const std::vector<std::vector<Seg>> cases = {
        {{-36, 10}, {-23, 60}, {-36, 10}},                          // case 3
        {{-72, 10}, {-36, 10}, {-23, 60}, {-36, 10}, {-72, 10}},    // case 4
        {{-26, 20}, {-20, 20.1}, {-26, 20}},                        // case 5
    };
    int n = 3;
    for (const auto& c : cases) {
        dsp::LoudnessMeter m(48000, 2);
        for (const auto& seg : c) tone(m, 48000, 1000, seg.dB, seg.s);
        CHECK_MSG(near(m.integrated(), -23.0, 0.1),
                  "case " + std::to_string(n) + ": " + s(m.integrated()));
        ++n;
    }

    dsp::LoudnessMeter z(48000, 2);
    tone(z, 48000, 1000, -300, 5);  // digital silence
    CHECK(z.integrated() == dsp::LoudnessMeter::kFloor);
    CHECK(z.momentary() == dsp::LoudnessMeter::kFloor);
    z.reset();
    tone(z, 48000, 1000, -23, 5);
    CHECK(near(z.integrated(), -23.0, 0.1));
}

// A sine at fs/4 sampled at 45 degrees: every sample is 3 dB under the
// waveform's peak. Sample peak says -3 dB; true peak must say ~0 dBTP.
TEST(true_peak_finds_inter_sample_peaks) {
    dsp::LoudnessMeter m(48000, 2);
    tone(m, 48000, 12000, 0, 1, std::numbers::pi / 4);
    CHECK_MSG(near(m.truePeakMax(), 0.0, 0.5), "TP " + s(m.truePeakMax()));
    dsp::LoudnessMeter k(48000, 2);
    tone(k, 48000, 997, -6, 1);
    CHECK_MSG(near(k.truePeakMax(), -6.0, 0.1), "TP 997 Hz " + s(k.truePeakMax()));
}
