#include "record/recorder.h"

#include <chrono>
#include <cstdio>
#include <filesystem>

namespace pm::rec {

Recorder::Recorder(const RecorderOptions& o) : opt_(o) {}

Recorder::~Recorder() { stop(); }

std::string Recorder::fileNameFor(int index, const std::string& name) {
    std::string clean;
    for (char c : name)
        clean += (c == '/' || c == ':' || c == '\\') ? '-' : c;
    char prefix[8];
    std::snprintf(prefix, sizeof prefix, "%02d ", index + 1);
    return prefix + clean + ".wav";
}

bool Recorder::start(const std::string& folder, std::vector<TrackSpec> tracks,
                     std::string* error) {
    stop();
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        if (error) *error = folder + ": " + ec.message();
        return false;
    }
    folder_ = folder;
    tracks_.clear();
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        auto t = std::make_unique<Track>();
        t->spec = std::move(tracks[i]);
        t->path = folder + "/" + fileNameFor(int(i), t->spec.name);
        tracks_.push_back(std::move(t));
    }
    {
        std::lock_guard lock(statsMutex_);
        error_.clear();
        stats_.clear();
    }
    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void Recorder::stop() {
    if (!thread_.joinable()) return;
    running_.store(false);
    thread_.join();
}

std::vector<TrackStats> Recorder::stats() const {
    std::lock_guard lock(statsMutex_);
    return stats_;
}

std::string Recorder::lastError() const {
    std::lock_guard lock(statsMutex_);
    return error_;
}

void Recorder::setError(std::string e) {
    std::lock_guard lock(statsMutex_);
    error_ = std::move(e);
}

// Writes everything the tap holds, splicing in silence where the audio
// thread had to drop blocks.
bool Recorder::drain(Track& t, std::vector<float>& buf) {
    auto& tap = *t.spec.tap;
    if (!tap.started()) return true;
    if (!t.wav.isOpen()) {
        std::string err;
        if (!t.wav.open(t.path, opt_.sampleRate, tap.channels(),
                        tap.startSample(), t.spec.name, &err)) {
            setError(err);
            return false;
        }
        // An overview that cannot be written costs the editor its picture,
        // never the recording.
        if (!t.peaks.open(peaksPathFor(t.path), tap.channels(), opt_.sampleRate,
                          tap.startSample()))
            setError(t.path + ": could not write its waveform overview");
    }
    const std::size_t chunk = buf.size() / std::size_t(tap.channels());
    while (true) {
        // Read the fill level before the gap queue: a gap is queued before
        // the data that follows it is published.
        const std::size_t avail = tap.available();
        if (!t.haveGap) t.haveGap = tap.nextGap(t.gap);
        if (t.haveGap && t.gap.atFrame == t.pos) {
            std::fill(buf.begin(), buf.end(), 0.0f);
            for (std::uint32_t left = t.gap.frames; left;) {
                const auto n = std::min<std::size_t>(left, chunk);
                if (!write(t, buf.data(), n)) return false;
                t.written += n;
                left -= std::uint32_t(n);
            }
            t.haveGap = false;
            continue;
        }
        std::size_t want = std::min(avail, chunk);
        if (t.haveGap) want = std::min<std::size_t>(want, t.gap.atFrame - t.pos);
        if (!want) return true;
        std::uint64_t at = 0;
        const std::size_t n = tap.pop(buf.data(), want, &at);
        if (!write(t, buf.data(), n)) {
            setError(t.path + ": write failed");
            return false;
        }
        t.pos += n;
        t.written += n;
    }
}

bool Recorder::write(Track& t, const float* x, std::size_t frames) {
    if (!t.wav.write(x, frames)) return false;
    t.peaks.add(x, frames);
    return true;
}

void Recorder::run() {
    using clock = std::chrono::steady_clock;
    std::vector<float> buf(8192 * 2);
    auto lastHeader = clock::now();
    const auto headerEvery =
        std::chrono::duration<double>(opt_.headerIntervalS);

    auto publishStats = [&] {
        std::vector<TrackStats> s;
        for (const auto& t : tracks_)
            s.push_back({t->spec.id, t->path, t->written,
                         t->spec.tap->droppedFrames(), t->spec.tap->fill()});
        std::lock_guard lock(statsMutex_);
        stats_ = std::move(s);
    };

    while (true) {
        const bool last = !running_.load();
        if (const int stall = stallMs_.exchange(0))
            std::this_thread::sleep_for(std::chrono::milliseconds(stall));
        for (auto& t : tracks_) drain(*t, buf);
        if (last || clock::now() - lastHeader >= headerEvery) {
            for (auto& t : tracks_)
            {
                if (t->wav.isOpen() && !t->wav.flushHeader(true))
                    setError(t->path + ": header update failed");
                t->peaks.flush();
            }
            lastHeader = clock::now();
        }
        publishStats();
        if (last) break;
        std::this_thread::sleep_for(
            std::chrono::duration<double, std::milli>(opt_.pollMs));
    }
    for (auto& t : tracks_) {
        t->wav.close();
        t->peaks.close();
    }
}

}  // namespace pm::rec
