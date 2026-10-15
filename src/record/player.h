// The disk side of playback: reads a take's WAVs into a PlaybackSet's
// read-ahead rings on its own thread. The audio thread never waits for it;
// a ring that runs dry is an underrun (counted), not a stall.
#pragma once

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "record/playback.h"
#include "record/wav_writer.h"

namespace pm::rec {

struct PlayerTrack {
    NodeId node = 0;   // the source node this file feeds
    std::string wav;
};

class Player {
public:
    ~Player() { close(); }

    // Opens every file at `startFrame` (take frames) and starts filling
    // rings of `aheadSeconds`. Files that cannot be read are skipped (their
    // node plays silence); false only if none can.
    bool open(const std::vector<PlayerTrack>& tracks, std::uint64_t startFrame,
              double aheadSeconds, std::string* error);
    void close();

    std::shared_ptr<PlaybackSet> set() const { return set_; }
    // Every ring holds at least `frames` (ready to roll without underrun).
    bool primed(std::size_t frames) const { return set_ && set_->allHave(frames); }
    std::uint64_t lengthFrames() const { return length_; }

private:
    struct File {
        std::FILE* f = nullptr;
        WavInfo info;
        PlaybackFeed* feed = nullptr;
        std::uint64_t next = 0;  // next frame to read
    };
    void run();

    std::shared_ptr<PlaybackSet> set_;
    std::vector<File> files_;
    std::uint64_t length_ = 0;
    std::thread thread_;
    std::atomic<bool> quit_{false};
};

}  // namespace pm::rec
