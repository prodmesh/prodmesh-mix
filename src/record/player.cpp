#include "record/player.h"

#include <algorithm>
#include <chrono>

namespace pm::rec {

bool Player::open(const std::vector<PlayerTrack>& tracks, std::uint64_t startFrame,
                  double aheadSeconds, std::string* error) {
    close();
    set_ = std::make_shared<PlaybackSet>();
    set_->startFrame = startFrame;
    set_->position.store(startFrame);
    length_ = 0;
    for (const auto& t : tracks) {
        File file;
        if (!WavWriter::readInfo(t.wav, file.info) || file.info.channels < 1) continue;
        file.f = std::fopen(t.wav.c_str(), "rb");
        if (!file.f) continue;
        set_->sampleRate = file.info.sampleRate;
        const auto cap = std::size_t(std::max(1.0, aheadSeconds) * file.info.sampleRate);
        set_->feeds.push_back(std::make_unique<PlaybackFeed>(t.node, file.info.channels, cap));
        file.feed = set_->feeds.back().get();
        file.next = startFrame;
        length_ = std::max(length_, file.info.frames);
        files_.push_back(file);
    }
    if (files_.empty()) {
        if (error) *error = "none of the take's files could be read";
        set_.reset();
        return false;
    }
    quit_.store(false);
    thread_ = std::thread([this] { run(); });
    return true;
}

void Player::close() {
    quit_.store(true);
    if (thread_.joinable()) thread_.join();
    for (auto& f : files_)
        if (f.f) std::fclose(f.f);
    files_.clear();
}

void Player::run() {
    constexpr std::size_t kChunk = 4096;
    std::vector<float> buf;
    while (!quit_.load(std::memory_order_relaxed)) {
        bool did = false;
        for (auto& file : files_) {
            const int ch = file.info.channels;
            buf.resize(kChunk * std::size_t(ch));
            while (file.feed->space() >= kChunk && !quit_.load(std::memory_order_relaxed)) {
                std::size_t got = 0;
                if (file.next < file.info.frames) {
                    const auto want = std::min<std::uint64_t>(kChunk, file.info.frames - file.next);
                    std::fseek(file.f, long(file.info.dataOffset + file.next * std::uint64_t(ch) * 4),
                               SEEK_SET);
                    got = std::fread(buf.data(), sizeof(float) * std::size_t(ch), std::size_t(want),
                                     file.f);
                }
                // Past the end (or a short read): silence keeps every
                // track on the same frame.
                std::fill(buf.begin() + std::ptrdiff_t(got * std::size_t(ch)), buf.end(), 0.0f);
                file.feed->write(buf.data(), kChunk);
                file.next += kChunk;
                did = true;
            }
        }
        if (!did) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace pm::rec
