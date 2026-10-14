// Crash-safe Broadcast WAV writer (32-bit float, 1-2 channels).
//
// Layout: RIFF/WAVE, a 28-byte JUNK chunk reserved for ds64, fmt, bext
// (EBU Tech 3285: originator + sample-accurate TimeReference), data.
// Audio is written straight through; the size fields are patched in place
// by flushHeader() (the recorder does it every second). After a crash the
// header lags the data by at most that interval, and recover() restores
// the true length from the file size. Past 4 GB the file becomes RF64
// (EBU Tech 3306): JUNK turns into ds64 and the 32-bit sizes read -1.
#pragma once

#include <cstdint>
#include <string>

namespace pm::rec {

struct WavInfo {
    int channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint64_t frames = 0;       // per the header
    std::uint64_t dataOffset = 0;
    std::uint64_t timeReference = 0;
    bool rf64 = false;
};

class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    bool open(const std::string& path, std::uint32_t sampleRate,
              int channels, std::uint64_t timeReference,
              const std::string& description, std::string* error);
    // Interleaved frames.
    bool write(const float* data, std::size_t frames);
    // Patches the RIFF/ds64/data sizes to what has been written; with
    // `sync`, also asks the OS to put data and header on stable storage.
    bool flushHeader(bool sync);
    void close();

    bool isOpen() const { return fd_ >= 0; }
    std::uint64_t frames() const { return dataBytes_ / frameBytes(); }

    // Test hook: promote to RF64 past this many data bytes, not 4 GB.
    void setRf64Threshold(std::uint64_t bytes) { rf64Threshold_ = bytes; }

    static bool readInfo(const std::string& path, WavInfo& out);
    // Rewrites the header sizes from the file's actual length (whole frames).
    static bool recover(const std::string& path, std::string* error);

    static constexpr std::uint64_t kDataOffset = 690;

private:
    std::uint64_t frameBytes() const { return std::uint64_t(channels_) * 4; }
    bool patchSizes(std::uint64_t dataBytes, bool rf64);

    int fd_ = -1;
    int channels_ = 0;
    std::uint64_t dataBytes_ = 0;
    bool rf64_ = false;
    std::uint64_t rf64Threshold_ = 0xFFFFFFFFull - kDataOffset;
};

}  // namespace pm::rec
