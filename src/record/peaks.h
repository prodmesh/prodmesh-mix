// Waveform overviews ("peaks"), written beside each recorded WAV as it is
// recorded, so an editor can draw an 8-hour take without reading gigabytes
// of audio — and follow a take that is still growing.
//
// File: "03 Guest 1.peaks"
//   header (32 bytes): "PMPK", version, channels, bucket frames,
//                      sample rate (double), start sample (u64)
//   records: per bucket, per channel: int16 min, int16 max (x 1/32767)
// Append-only with fixed-size records: a reader takes whole records up to
// the current file size; a partial last record is simply not there yet.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace pm::rec {

inline constexpr std::uint32_t kPeakBucket = 512;

#pragma pack(push, 1)
struct PeakHeader {
    char magic[4] = {'P', 'M', 'P', 'K'};
    std::uint32_t version = 1;
    std::uint32_t channels = 1;
    std::uint32_t bucket = kPeakBucket;
    double sampleRate = 48000;
    std::uint64_t startSample = 0;
};
#pragma pack(pop)
static_assert(sizeof(PeakHeader) == 32);

inline std::string peaksPathFor(const std::string& wavPath) {
    const auto dot = wavPath.rfind('.');
    return (dot == std::string::npos ? wavPath : wavPath.substr(0, dot)) + ".peaks";
}

// Writer side (the recorder's disk thread).
class PeakWriter {
public:
    ~PeakWriter() { close(); }

    bool open(const std::string& path, int channels, double rate,
              std::uint64_t startSample) {
        close();
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) return false;
        ch_ = std::max(1, channels);
        PeakHeader h;
        h.channels = std::uint32_t(ch_);
        h.sampleRate = rate;
        h.startSample = startSample;
        std::fwrite(&h, sizeof h, 1, f_);
        lo_.assign(std::size_t(ch_), 0.0f);
        hi_.assign(std::size_t(ch_), 0.0f);
        n_ = 0;
        return true;
    }
    bool isOpen() const { return f_ != nullptr; }

    // Interleaved frames.
    void add(const float* x, std::size_t frames) {
        if (!f_) return;
        for (std::size_t i = 0; i < frames; ++i) {
            for (int c = 0; c < ch_; ++c) {
                const float v = x[i * std::size_t(ch_) + std::size_t(c)];
                if (n_ == 0) lo_[std::size_t(c)] = hi_[std::size_t(c)] = v;
                lo_[std::size_t(c)] = std::min(lo_[std::size_t(c)], v);
                hi_[std::size_t(c)] = std::max(hi_[std::size_t(c)], v);
            }
            if (++n_ == kPeakBucket) emitBucket();
        }
    }
    // Make what is complete visible to readers.
    void flush() {
        if (!f_) return;
        if (!pending_.empty()) {
            std::fwrite(pending_.data(), sizeof(std::int16_t), pending_.size(), f_);
            pending_.clear();
        }
        std::fflush(f_);
    }
    void close() {
        if (!f_) return;
        if (n_) emitBucket();  // the tail, as a short bucket
        flush();
        std::fclose(f_);
        f_ = nullptr;
    }

private:
    static std::int16_t q(float v) {
        return std::int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
    }
    void emitBucket() {
        for (int c = 0; c < ch_; ++c) {
            pending_.push_back(q(lo_[std::size_t(c)]));
            pending_.push_back(q(hi_[std::size_t(c)]));
        }
        n_ = 0;
    }

    std::FILE* f_ = nullptr;
    int ch_ = 1;
    std::uint32_t n_ = 0;
    std::vector<float> lo_, hi_;
    std::vector<std::int16_t> pending_;
};

// Reader side (the editor). Keeps everything read so far and picks up
// whatever the writer has appended since.
class PeakReader {
public:
    bool open(const std::string& path) {
        path_ = path;
        data_.clear();
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        const bool ok = std::fread(&h_, sizeof h_, 1, f) == 1 &&
                        std::memcmp(h_.magic, "PMPK", 4) == 0 && h_.channels >= 1 &&
                        h_.channels <= 8 && h_.bucket > 0;
        std::fclose(f);
        return ok && (refresh(), true);
    }
    // Reads new complete records; returns how many buckets arrived.
    std::size_t refresh() {
        std::FILE* f = std::fopen(path_.c_str(), "rb");
        if (!f) return 0;
        const std::size_t rec = std::size_t(h_.channels) * 2;  // int16s per bucket
        const long from = long(sizeof h_ + data_.size() * sizeof(std::int16_t));
        std::fseek(f, 0, SEEK_END);
        const long end = std::ftell(f);
        std::size_t added = 0;
        if (end > from) {
            const std::size_t whole =
                std::size_t(end - from) / (rec * sizeof(std::int16_t));
            if (whole) {
                const std::size_t old = data_.size();
                data_.resize(old + whole * rec);
                std::fseek(f, from, SEEK_SET);
                added = std::fread(data_.data() + old, rec * sizeof(std::int16_t),
                                   whole, f);
                data_.resize(old + added * rec);
            }
        }
        std::fclose(f);
        return added;
    }

    const PeakHeader& header() const { return h_; }
    int channels() const { return int(h_.channels); }
    std::size_t buckets() const { return data_.size() / (std::size_t(h_.channels) * 2); }
    double seconds() const { return double(buckets()) * h_.bucket / h_.sampleRate; }
    // min/max of channel c in bucket i, -1..1
    float lo(std::size_t i, int c) const {
        return data_[(i * h_.channels + std::size_t(c)) * 2] / 32767.0f;
    }
    float hi(std::size_t i, int c) const {
        return data_[(i * h_.channels + std::size_t(c)) * 2 + 1] / 32767.0f;
    }

private:
    std::string path_;
    PeakHeader h_;
    std::vector<std::int16_t> data_;
};

}  // namespace pm::rec
