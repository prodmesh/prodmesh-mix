// Recording: T0-7 (crash-safe files) and T0-8 (disk stalls lose nothing),
// plus bit-exact capture and sample alignment across gaps.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include "core/rt_guard.h"
#include "engine/engine.h"
#include "record/recorder.h"
#include "record/wav_writer.h"
#include "test.h"

using namespace pm;
using namespace pm::rec;
namespace fs = std::filesystem;

namespace {

constexpr int kBlock = 128;

std::string tempDir(const char* name) {
    auto d = fs::temp_directory_path() / ("pm-mix-test-" + std::string(name));
    fs::remove_all(d);
    fs::create_directories(d);
    return d.string();
}

std::vector<float> readSamples(const std::string& path, WavInfo* infoOut = nullptr) {
    WavInfo info;
    if (!WavWriter::readInfo(path, info)) return {};
    std::vector<float> out(info.frames * std::uint64_t(info.channels));
    FILE* f = std::fopen(path.c_str(), "rb");
    std::fseek(f, long(info.dataOffset), SEEK_SET);
    const auto got = std::fread(out.data(), sizeof(float), out.size(), f);
    std::fclose(f);
    out.resize(got);
    if (infoOut) *infoOut = info;
    return out;
}

// gen(1) -> strip(2) -> mono out(3); strip is recorded.
GraphDesc recordGraph() {
    GraphDesc g;
    NodeDesc gen;
    gen.id = 1;
    gen.kind = NodeKind::Generator;
    gen.generator = {3, 220, -12, -40, true};
    NodeDesc strip;
    strip.id = 2;
    NodeDesc out;
    out.id = 3;
    out.kind = NodeKind::Output;
    g.nodes = {gen, strip, out};
    g.edges = {{1, 2, 0, 0}, {2, 3, 0, 0}};
    return g;
}

// Renders `blocks` and returns what reached the output, which is exactly
// the strip's post-fader signal (unity edge).
std::vector<float> render(Engine& e, int blocks) {
    std::vector<float> all, buf(kBlock);
    float* out[1] = {buf.data()};
    for (int b = 0; b < blocks; ++b) {
        e.pump();
        {
            rt::ScopedRealtime scope;
            e.process(nullptr, 0, out, 1, kBlock);
        }
        all.insert(all.end(), buf.begin(), buf.end());
    }
    return all;
}

EngineConfig cfg() {
    EngineConfig c;
    c.maxBlock = kBlock;
    c.numOutputs = 1;
    return c;
}

}  // namespace

TEST(tap_round_trip_and_gap_accounting) {
    RecordTap tap(1, 256);
    std::vector<float> a(128), b(128);
    for (int i = 0; i < 128; ++i) a[i] = float(i), b[i] = float(1000 + i);
    const float* pa[1] = {a.data()};
    const float* pb[1] = {b.data()};
    tap.push(pa, 128, 0);
    tap.push(pb, 128, 128);
    tap.push(pa, 128, 256);  // ring full: dropped, gap pending
    CHECK(tap.droppedFrames() == 128);
    std::vector<float> got(256);
    std::uint64_t at = 99;
    CHECK(tap.pop(got.data(), 256, &at) == 256 && at == 0);
    CHECK(got[0] == 0 && got[255] == 1127);
    tap.push(pb, 128, 384);  // room again: gap recorded, block kept
    RecordTap::Gap gap;
    CHECK(tap.nextGap(gap));
    CHECK(gap.atFrame == 256 && gap.frames == 128);
    CHECK(tap.pop(got.data(), 256, &at) == 128 && got[0] == 1000);
}

TEST(wav_round_trip) {
    const auto dir = tempDir("wav");
    const auto path = dir + "/t.wav";
    WavWriter w;
    std::string err;
    CHECK_MSG(w.open(path, 48000, 2, 123456789012ull, "test", &err), err);
    std::vector<float> x(2 * 1000);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = std::sin(0.01f * float(i));
    CHECK(w.write(x.data(), 1000));
    w.close();
    WavInfo info;
    const auto y = readSamples(path, &info);
    CHECK(info.channels == 2 && info.sampleRate == 48000);
    CHECK(info.frames == 1000 && !info.rf64);
    CHECK(info.timeReference == 123456789012ull);
    CHECK(y == x);
}

TEST(wav_crash_recovery_restores_full_length) {
    const auto dir = tempDir("crash");
    const auto path = dir + "/t.wav", copy = dir + "/crashed.wav";
    WavWriter w;
    CHECK(w.open(path, 48000, 1, 0, "crash", nullptr));
    std::vector<float> x(48000);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = float(i) / 48000.0f;
    CHECK(w.write(x.data(), 48000));
    CHECK(w.flushHeader(false));       // header says 1 s ...
    CHECK(w.write(x.data(), 24000));   // ... then 0.5 s more, then "crash"
    fs::copy_file(path, copy);         // the file as a killed process leaves it
    WavInfo info;
    CHECK(WavWriter::readInfo(copy, info) && info.frames == 48000);
    std::string err;
    CHECK_MSG(WavWriter::recover(copy, &err), err);
    CHECK(WavWriter::readInfo(copy, info) && info.frames == 72000);
    const auto y = readSamples(copy);
    CHECK(y.size() == 72000 && y[47999] == x[47999] && y[71999] == x[23999]);
}

TEST(wav_promotes_to_rf64) {
    const auto dir = tempDir("rf64");
    const auto path = dir + "/t.wav";
    WavWriter w;
    w.setRf64Threshold(4000);  // stand-in for 4 GB
    CHECK(w.open(path, 48000, 1, 0, "big", nullptr));
    std::vector<float> x(2000, 0.5f);
    CHECK(w.write(x.data(), 2000));  // 8000 bytes > threshold
    w.close();
    WavInfo info;
    CHECK(WavWriter::readInfo(path, info));
    CHECK(info.rf64 && info.frames == 2000);
    CHECK(readSamples(path) == x);
}

TEST(recording_is_bit_exact_and_time_stamped) {
    const auto dir = tempDir("e2e");
    Engine e(cfg());
    CHECK(e.setGraph(recordGraph()));
    auto tap = std::make_shared<RecordTap>(1, 48000 * 4);
    render(e, 10);  // recording starts mid-stream
    e.setRecordTaps({{2, tap}});
    Recorder r({});
    CHECK(r.start(dir, {{2, "Host", tap}}, nullptr));
    const auto rendered = render(e, 400);
    r.stop();
    const auto stats = r.stats();
    CHECK(stats.size() == 1 && stats[0].droppedFrames == 0);
    WavInfo info;
    const auto recorded = readSamples(stats[0].file, &info);
    // The tap is armed by the plan swap at a block boundary early in the
    // 400; everything from there on must match exactly.
    CHECK(!recorded.empty() && recorded.size() <= rendered.size());
    const std::size_t offset = rendered.size() - recorded.size();
    CHECK(offset % kBlock == 0 && offset <= 4 * kBlock);
    CHECK(std::memcmp(recorded.data(), rendered.data() + offset,
                      recorded.size() * sizeof(float)) == 0);
    CHECK(info.timeReference == 10 * kBlock + offset);
    CHECK(fs::path(stats[0].file).filename() == "01 Host.wav");
    CHECK(rt::violations() == 0);
}

TEST(disk_stall_within_ring_loses_nothing) {
    const auto dir = tempDir("stall");
    Engine e(cfg());
    CHECK(e.setGraph(recordGraph()));
    auto tap = std::make_shared<RecordTap>(1, 48000 * 2);  // 2 s ring
    e.setRecordTaps({{2, tap}});
    Recorder r({});
    CHECK(r.start(dir, {{2, "Host", tap}}, nullptr));
    r.simulateStall(600);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    // 1.5 s of audio arrives while the writer is stalled.
    const auto rendered = render(e, 48000 * 3 / 2 / kBlock);
    CHECK(tap->droppedFrames() == 0);
    r.stop();
    const auto recorded = readSamples(r.stats()[0].file);
    const std::size_t offset = rendered.size() - recorded.size();
    CHECK(offset <= 4 * kBlock);
    CHECK(std::memcmp(recorded.data(), rendered.data() + offset,
                      recorded.size() * sizeof(float)) == 0);
}

TEST(overflow_becomes_silence_and_keeps_alignment) {
    const auto dir = tempDir("overflow");
    Engine e(cfg());
    CHECK(e.setGraph(recordGraph()));
    auto tap = std::make_shared<RecordTap>(1, 48000 / 4);  // 0.25 s ring
    e.setRecordTaps({{2, tap}});
    Recorder r({});
    CHECK(r.start(dir, {{2, "Host", tap}}, nullptr));
    r.simulateStall(500);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const auto rendered = render(e, 48000 / kBlock);  // 1 s into 0.25 s
    CHECK(tap->droppedFrames() > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const auto tail = render(e, 40);  // after the stall: recorded again
    r.stop();
    const auto recorded = readSamples(r.stats()[0].file);
    std::vector<float> all = rendered;
    all.insert(all.end(), tail.begin(), tail.end());
    // Same length as what was rendered since the tap armed: the timeline
    // survived the overflow.
    const std::size_t offset = all.size() - recorded.size();
    CHECK(offset <= 4 * kBlock);
    // The tail after the stall is exact, at the exact same position.
    const std::size_t n = tail.size();
    CHECK(std::memcmp(recorded.data() + recorded.size() - n,
                      all.data() + all.size() - n, n * sizeof(float)) == 0);
    // And the dropped stretch is silence.
    std::size_t zeros = 0;
    for (float v : recorded) zeros += v == 0.0f;
    CHECK(zeros >= tap->droppedFrames());
}
