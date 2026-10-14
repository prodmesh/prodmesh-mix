// The disk side of always-on recording: one writer thread drains every
// tap of a take into its own Broadcast WAV file. Nothing here runs on the
// audio thread; a slow or stalled disk only fills the taps' rings (30 s by
// default) and, past that, turns into recorded gaps of silence — never into
// an audio dropout.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/graph.h"
#include "record/record_tap.h"
#include "record/wav_writer.h"

namespace pm::rec {

struct RecorderOptions {
    std::uint32_t sampleRate = 48000;
    double headerIntervalS = 1.0;  // T0-7: at most this much header lag
    double pollMs = 20;
};

struct TrackSpec {
    NodeId id = 0;
    std::string name;
    std::shared_ptr<RecordTap> tap;
};

struct TrackStats {
    NodeId id = 0;
    std::string file;
    std::uint64_t framesWritten = 0;
    std::uint64_t droppedFrames = 0;
    float ringFill = 0;
};

class Recorder {
public:
    explicit Recorder(const RecorderOptions& o);
    ~Recorder();

    // Creates `folder` and starts writing the tracks; files open as soon as
    // each tap receives its first block.
    bool start(const std::string& folder, std::vector<TrackSpec> tracks,
               std::string* error);
    // Drains what the taps hold, finalises headers, closes the files.
    void stop();
    bool running() const { return running_.load(); }

    std::vector<TrackStats> stats() const;
    std::string folder() const { return folder_; }
    std::string lastError() const;

    // Test hook: the writer sleeps this long once, as a stalled disk would.
    void simulateStall(int ms) { stallMs_.store(ms); }

    // "03 Guest 1.wav": sortable, readable in any file browser.
    static std::string fileNameFor(int index, const std::string& name);

private:
    struct Track {
        TrackSpec spec;
        WavWriter wav;
        std::string path;
        std::uint64_t pos = 0;  // next pushed-frame index to write
        RecordTap::Gap gap{};
        bool haveGap = false;
        std::uint64_t written = 0;
    };

    void run();
    bool drain(Track& t, std::vector<float>& buf);
    void setError(std::string e);

    RecorderOptions opt_;
    std::string folder_;
    std::vector<std::unique_ptr<Track>> tracks_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> stallMs_{0};
    mutable std::mutex statsMutex_;  // writer <-> control; never audio
    std::vector<TrackStats> stats_;
    std::string error_;
};

}  // namespace pm::rec
