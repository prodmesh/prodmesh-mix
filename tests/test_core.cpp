#include <cmath>
#include <thread>
#include <vector>

#include "core/ramp.h"
#include "core/rt_guard.h"
#include "core/spsc_queue.h"
#include "dsp/biquad.h"
#include "dsp/dynamics.h"
#include "test.h"

using namespace pm;

TEST(spsc_fifo_order_and_capacity) {
    SpscQueue<int> q(4);
    CHECK(q.capacity() == 4);
    for (int i = 0; i < 4; ++i) CHECK(q.push(i));
    CHECK(!q.push(99));
    for (int i = 0; i < 4; ++i) CHECK(*q.pop() == i);
    CHECK(!q.pop());
}

TEST(spsc_concurrent_transfer) {
    SpscQueue<std::uint64_t> q(256);
    constexpr std::uint64_t kN = 2'000'000;
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kN;)
            if (q.push(i)) ++i;
    });
    std::uint64_t expect = 0;
    while (expect < kN)
        if (auto v = q.pop()) {
            CHECK(*v == expect);
            ++expect;
        }
    producer.join();
}

TEST(rt_guard_counts_allocations) {
    rt::setPolicy(rt::Policy::Count);
    rt::resetViolations();
    {
        rt::ScopedRealtime scope;
        // volatile keeps the compiler from eliding the new/delete pair.
        int* volatile p = new int(1);
        delete p;
    }
    CHECK(rt::violations() == 2);
    { auto v = std::vector<int>(10); }  // outside scope: allowed
    CHECK(rt::violations() == 2);
    rt::resetViolations();
}

TEST(ramp_reaches_target_exactly) {
    LinearRamp r(0);
    r.setTarget(1, 480);
    float last = 0;
    for (int i = 0; i < 480; ++i) {
        const float v = r.next();
        CHECK(v >= last);
        last = v;
    }
    CHECK(r.current() == 1.0f);
    CHECK(!r.ramping());
    r.setTarget(1, 480);  // same target: no ramp
    CHECK(!r.ramping());
}

TEST(biquad_flat_peaking_is_identity) {
    dsp::Biquad b;
    b.setCoeffs(dsp::BiquadCoeffs::peaking(48000, 1000, 1, 0));
    float x[64];
    for (int i = 0; i < 64; ++i) x[i] = std::sin(0.1f * float(i));
    float y[64];
    std::copy(x, x + 64, y);
    b.process(y, 64);
    for (int i = 0; i < 64; ++i) CHECK(std::fabs(y[i] - x[i]) < 1e-5f);
}

TEST(highpass_removes_dc) {
    dsp::Biquad b;
    b.setCoeffs(dsp::BiquadCoeffs::highpass(48000, 80, 0.7071));
    std::vector<float> x(48000, 1.0f);
    b.process(x.data(), int(x.size()));
    CHECK(std::fabs(x.back()) < 1e-3f);
}

TEST(compressor_reduces_loud_signal) {
    dsp::Compressor c;
    dsp::CompressorParams p;
    p.thresholdDb = -20;
    p.ratio = 4;
    p.kneeDb = 0;
    p.attackMs = 0.1f;
    c.prepare(48000, p);
    std::vector<float> x(48000, 1.0f);  // 0 dBFS, 20 dB over
    float* ch[1] = {x.data()};
    c.process(ch, 1, int(x.size()));
    // 20 dB over at 4:1 -> 15 dB of reduction.
    CHECK(std::fabs(c.gainReductionDb() - 15.0f) < 0.1f);
    CHECK(std::fabs(dsp::gainToDb(x.back()) + 15.0f) < 0.1f);
}

TEST(limiter_holds_ceiling) {
    dsp::Limiter l;
    l.prepare(48000, -1, 50);
    std::vector<float> a(4800), b(4800);
    for (int i = 0; i < 4800; ++i) a[i] = b[i] = 2.0f * std::sin(0.05f * i);
    float* ch[2] = {a.data(), b.data()};
    l.process(ch, 2, 4800);
    const float ceil = dsp::dbToGain(-1);
    for (int i = 0; i < 4800; ++i) CHECK(std::fabs(a[i]) <= ceil + 1e-6f);
}

TEST(fast_log_exp_accuracy) {
    for (float g = 1e-6f; g < 100.0f; g *= 1.37f) {
        const float exact = dsp::gainToDb(g);
        CHECK(std::fabs(dsp::fastGainToDb(g) - exact) < 0.001f);
    }
    for (float dB = -120; dB < 40; dB += 0.37f) {
        const float exact = dsp::dbToGain(dB);
        CHECK(std::fabs(dsp::fastDbToGain(dB) / exact - 1.0f) < 1e-4f);
    }
}

#include "core/base64.h"

TEST(base64_round_trips_binary) {
    for (int len = 0; len < 40; ++len) {
        std::string x;
        for (int i = 0; i < len; ++i) x += char((i * 37 + len) & 0xff);
        std::string y;
        CHECK(pm::base64Decode(pm::base64Encode(x), y));
        CHECK(y == x);
    }
    CHECK(pm::base64Encode("Man") == "TWFu");
    CHECK(pm::base64Encode("Ma") == "TWE=");
}
