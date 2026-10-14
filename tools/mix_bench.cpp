// mix-bench: runs the Tier 0 reference graph on a driver and reports
// per-callback timing. Optional live edits exercise the graph swap under
// load. Exit status is non-zero on any xrun or real-time violation, so it
// can gate CI and soak runs.
//
//   mix-bench --paths 64 --rate 48000 --block 128 --seconds 60 \
//             --driver sim --workers 0 --edits 1
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bench_graph.h"
#include "core/rt_guard.h"
#include "driver/driver.h"
#include "engine/engine.h"
#include "record/recorder.h"

using namespace pm;

namespace {

struct Options {
    int paths = 64;
    double rate = 48000;
    int block = 128;
    double seconds = 10;
    std::string driver = "sim";  // sim | free | coreaudio
    int workers = 0;
    int heavy = 4;
    float heavyOps = 64;
    double editsPerSecond = 0;
    bool json = false;
    bool audible = false;  // coreaudio: send the render to the speakers
    std::string recordDir;  // record every source + program (T0-8)
    int stallMs = 0;        // stall the disk writer this long ...
    double stallAt = 5;     // ... this many seconds in
    double ringSeconds = 30;
};

void usage() {
    std::puts(
        "mix-bench [--paths N] [--rate HZ] [--block N] [--seconds S]\n"
        "          [--driver sim|free|coreaudio] [--workers N]\n"
        "          [--heavy N] [--heavy-ops N] [--edits PER_SEC] [--json]\n"
        "          [--audible]   (coreaudio renders silence unless set)\n"
        "          [--record DIR [--stall-ms MS] [--stall-at S] [--ring-seconds S]]");
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        auto is = [&](const char* f) { return std::strcmp(argv[i], f) == 0; };
        auto val = [&]() -> const char* {
            return i + 1 < argc ? argv[++i] : "";
        };
        if (is("--paths")) o.paths = std::atoi(val());
        else if (is("--rate")) o.rate = std::atof(val());
        else if (is("--block")) o.block = std::atoi(val());
        else if (is("--seconds")) o.seconds = std::atof(val());
        else if (is("--driver")) o.driver = val();
        else if (is("--workers")) o.workers = std::atoi(val());
        else if (is("--heavy")) o.heavy = std::atoi(val());
        else if (is("--heavy-ops")) o.heavyOps = float(std::atof(val()));
        else if (is("--edits")) o.editsPerSecond = std::atof(val());
        else if (is("--json")) o.json = true;
        else if (is("--audible")) o.audible = true;
        else if (is("--record")) o.recordDir = val();
        else if (is("--stall-ms")) o.stallMs = std::atoi(val());
        else if (is("--stall-at")) o.stallAt = std::atof(val());
        else if (is("--ring-seconds")) o.ringSeconds = std::atof(val());
        else return false;
    }
    return true;
}

struct RenderCtx {
    Engine* engine;
    bool silence;  // render fully, then hand the device zeros
};

void render(void* ctx, const float* const* in, int numIn, float* const* out,
            int numOut, int frames) {
    auto* rc = static_cast<RenderCtx*>(ctx);
    rc->engine->process(in, numIn, out, numOut, frames);
    if (rc->silence)
        for (int c = 0; c < numOut; ++c)
            std::memset(out[c], 0, sizeof(float) * std::size_t(frames));
}

// 1 µs buckets up to 100 ms; everything beyond lands in the last.
struct Histogram {
    std::vector<std::uint64_t> bins = std::vector<std::uint64_t>(100000);
    std::uint64_t count = 0, maxNs = 0;
    long double sumNs = 0;

    void add(std::uint32_t ns) {
        ++bins[std::min<std::size_t>(ns / 1000, bins.size() - 1)];
        ++count;
        maxNs = std::max<std::uint64_t>(maxNs, ns);
        sumNs += ns;
    }
    double percentileUs(double q) const {
        const auto want = std::uint64_t(q * double(count));
        std::uint64_t acc = 0;
        for (std::size_t i = 0; i < bins.size(); ++i)
            if ((acc += bins[i]) > want) return double(i + 1);
        return double(bins.size());
    }
};

// Live edits that touch only odd strips: toggle an insert (node crossfade)
// or a group-bus route (edge ramps).
void randomEdit(Engine& e, std::mt19937& rng, int strips) {
    GraphDesc g = e.graph();
    std::uniform_int_distribution<int> pick(0, std::max(0, strips / 2 - 1));
    const NodeId id = bench::kStripBase + NodeId(pick(rng) * 2 + 1);
    if (rng() & 1) {
        auto* n = g.find(id);
        if (!n) return;
        auto& ins = n->processing.inserts;
        if (ins.empty()) ins.push_back({InsertDesc::Type::Gain, -3});
        else ins.clear();
    } else {
        auto it = std::find_if(g.edges.begin(), g.edges.end(), [&](auto& d) {
            return d.from == id && d.to >= bench::kGroupBase;
        });
        if (it != g.edges.end()) g.edges.erase(it);
        else g.edges.push_back({id, bench::kGroupBase, -12, 0});
    }
    e.setGraph(g);
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) {
        usage();
        return 2;
    }

    DriverConfig dc;
    dc.sampleRate = o.rate;
    dc.blockSize = o.block;
    dc.numOutputs = 4;
    std::unique_ptr<Driver> driver;
    std::string err;
    if (o.driver == "sim" || o.driver == "free") {
        driver = makeSimDriver(dc, o.driver == "free");
#if defined(__APPLE__)
    } else if (o.driver == "coreaudio") {
        driver = makeCoreAudioDriver(dc, &err);
        if (!driver) {
            std::fprintf(stderr, "coreaudio: %s\n", err.c_str());
            return 1;
        }
#endif
    } else {
        usage();
        return 2;
    }
    const auto& cfg = driver->config();

    EngineConfig ec;
    ec.sampleRate = cfg.sampleRate;
    ec.maxBlock = cfg.blockSize;
    ec.numOutputs = cfg.numOutputs;
    ec.workerThreads = o.workers;
    ec.workgroup = driver->workgroup();
    Engine engine(ec);

    bench::RefParams rp;
    rp.paths = o.paths;
    rp.heavyInserts = o.heavy;
    rp.heavyOpsPerSample = o.heavyOps;
    if (!engine.setGraph(bench::makeReferenceGraph(rp), &err)) {
        std::fprintf(stderr, "graph: %s\n", err.c_str());
        return 1;
    }

    std::unique_ptr<rec::Recorder> recorder;
    std::vector<std::shared_ptr<RecordTap>> allTaps;
    if (!o.recordDir.empty()) {
        const auto& g = engine.graph();
        std::vector<rec::TrackSpec> tracks;
        std::vector<std::pair<NodeId, std::shared_ptr<RecordTap>>> taps;
        const auto ring = std::size_t(o.ringSeconds * cfg.sampleRate);
        for (const auto& n : g.nodes)
            if (n.kind == NodeKind::Generator || n.id == bench::kProgram) {
                auto tap = std::make_shared<RecordTap>(n.channels, ring);
                tracks.push_back({n.id, n.name, tap});
                taps.push_back({n.id, tap});
                allTaps.push_back(tap);
            }
        rec::RecorderOptions ro;
        ro.sampleRate = std::uint32_t(cfg.sampleRate);
        recorder = std::make_unique<rec::Recorder>(ro);
        if (!recorder->start(o.recordDir, std::move(tracks), &err)) {
            std::fprintf(stderr, "record: %s\n", err.c_str());
            return 1;
        }
        engine.setRecordTaps(std::move(taps));
    }

    rt::resetViolations();
    RenderCtx rctx{&engine, o.driver == "coreaudio" && !o.audible};
    if (!driver->start(&render, &rctx, &err)) {
        std::fprintf(stderr, "driver: %s\n", err.c_str());
        return 1;
    }

    Histogram hist;
    bool stalled = false;
    double maxFill = 0;
    std::mt19937 rng(42);
    std::uint64_t edits = 0;
    const auto strips = bench::layoutFor(o.paths).strips;
    const auto t0 = std::chrono::steady_clock::now();
    auto nextEdit = t0 + std::chrono::milliseconds(500);
    auto lastReport = t0;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - t0).count();
        if (elapsed >= o.seconds) break;
        engine.pump();
        while (auto s = driver->stats().samples.pop()) hist.add(s->durationNs);
        if (recorder) {
            if (o.stallMs && !stalled && elapsed >= o.stallAt) {
                recorder->simulateStall(o.stallMs);
                stalled = true;
            }
            // Straight from the rings: the writer's own stats freeze while
            // it is stalled.
            for (const auto& t : allTaps)
                maxFill = std::max(maxFill, double(t->fill()));
        }
        if (o.editsPerSecond > 0 && now >= nextEdit) {
            randomEdit(engine, rng, strips);
            ++edits;
            nextEdit = now + std::chrono::microseconds(
                                 std::int64_t(1e6 / o.editsPerSecond));
        }
        if (!o.json && now - lastReport >= std::chrono::seconds(10)) {
            lastReport = now;
            std::fprintf(stderr, "  %6.0fs  callbacks %llu  xruns %llu\n",
                         elapsed,
                         (unsigned long long)driver->stats().callbacks.load(),
                         (unsigned long long)driver->stats().xruns.load());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    driver->stop();
    while (auto s = driver->stats().samples.pop()) hist.add(s->durationNs);
    std::uint64_t recDropped = 0, recFrames = 0;
    std::size_t recTracks = 0;
    if (recorder) {
        recorder->stop();
        for (const auto& t : recorder->stats()) {
            recDropped += t.droppedFrames;
            recFrames = std::max(recFrames, t.framesWritten);
            ++recTracks;
        }
    }

    const double periodUs = 1e6 * cfg.blockSize / cfg.sampleRate;
    const double meanUs = hist.count ? double(hist.sumNs / hist.count) / 1e3 : 0;
    const double maxUs = double(hist.maxNs) / 1e3;
    const auto xruns = driver->stats().xruns.load();
    const auto viol = rt::violations();
    const auto dropped = driver->stats().dropped.load();
    const auto lay = bench::layoutFor(o.paths);

    if (o.json) {
        std::printf(
            "{\"driver\":\"%s\",\"paths\":%d,\"strips\":%d,\"buses\":%d,"
            "\"rate\":%.0f,\"block\":%d,\"workers\":%d,\"seconds\":%.1f,"
            "\"callbacks\":%llu,\"xruns\":%llu,\"rt_violations\":%llu,"
            "\"edits\":%llu,\"period_us\":%.1f,\"mean_us\":%.1f,"
            "\"p50_us\":%.0f,\"p99_us\":%.0f,\"p9999_us\":%.0f,"
            "\"max_us\":%.1f,\"max_load\":%.3f,\"mean_load\":%.3f,"
            "\"dropped_samples\":%llu}\n",
            o.driver.c_str(), o.paths, lay.strips, lay.groups + 2,
            cfg.sampleRate, cfg.blockSize, o.workers, o.seconds,
            (unsigned long long)hist.count, (unsigned long long)xruns,
            (unsigned long long)viol, (unsigned long long)edits, periodUs,
            meanUs, hist.percentileUs(0.5), hist.percentileUs(0.99),
            hist.percentileUs(0.9999), maxUs, maxUs / periodUs,
            meanUs / periodUs, (unsigned long long)dropped);
    } else {
        std::printf("driver %s  %d paths (%d strips, %d buses)  %.0f Hz  "
                    "block %d (%.0f us)  workers %d\n",
                    o.driver.c_str(), o.paths, lay.strips, lay.groups + 2,
                    cfg.sampleRate, cfg.blockSize, periodUs, o.workers);
        std::printf("callbacks %llu  xruns %llu  rt violations %llu  "
                    "edits %llu\n",
                    (unsigned long long)hist.count,
                    (unsigned long long)xruns, (unsigned long long)viol,
                    (unsigned long long)edits);
        std::printf("callback us: mean %.1f  p50 %.0f  p99 %.0f  "
                    "p99.99 %.0f  max %.1f\n",
                    meanUs, hist.percentileUs(0.5), hist.percentileUs(0.99),
                    hist.percentileUs(0.9999), maxUs);
        std::printf("load: mean %.1f%%  worst %.1f%% of period  "
                    "(T0-2 target < 70%%)\n",
                    100 * meanUs / periodUs, 100 * maxUs / periodUs);
    }
    if (recorder && !o.json)
        std::printf("recording: %zu tracks, %.1f s each, dropped %llu frames, "
                    "ring peak %.0f%% (stall %d ms)\n",
                    recTracks, double(recFrames) / cfg.sampleRate,
                    (unsigned long long)recDropped, 100 * maxFill, o.stallMs);
    const bool freeRun = o.driver == "free";
    return (!freeRun && xruns) || viol || recDropped ? 1 : 0;
}
