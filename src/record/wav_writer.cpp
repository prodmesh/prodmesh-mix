#include "record/wav_writer.h"

#include <bit>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace pm::rec {
namespace {

static_assert(std::endian::native == std::endian::little,
              "WAV fields are written in host byte order");

// Fixed offsets of the header we write (see wav_writer.h).
constexpr std::uint64_t kRiffSizeAt = 4;
constexpr std::uint64_t kJunkIdAt = 12;
constexpr std::uint64_t kDs64At = 20;  // 28-byte payload
constexpr std::uint64_t kFmtAt = 48;
constexpr std::uint64_t kBextAt = 72;
constexpr std::uint32_t kBextSize = 602;
constexpr std::uint64_t kDataIdAt = kBextAt + 8 + kBextSize;  // 682
constexpr std::uint64_t kDataSizeAt = kDataIdAt + 4;
static_assert(kDataSizeAt + 4 == WavWriter::kDataOffset);

template <typename T>
void put(std::vector<char>& b, std::size_t at, T v) {
    std::memcpy(&b[at], &v, sizeof v);
}
void putId(std::vector<char>& b, std::size_t at, const char* id) {
    std::memcpy(&b[at], id, 4);
}

bool pwriteAll(int fd, const void* p, std::size_t n, off_t at) {
    const char* c = static_cast<const char*>(p);
    while (n) {
        const ssize_t k = ::pwrite(fd, c, n, at);
        if (k < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        c += k;
        n -= std::size_t(k);
        at += k;
    }
    return true;
}

template <typename T>
bool pwriteVal(int fd, T v, std::uint64_t at) {
    return pwriteAll(fd, &v, sizeof v, off_t(at));
}

}  // namespace

WavWriter::~WavWriter() { close(); }

bool WavWriter::open(const std::string& path, std::uint32_t sampleRate,
                     int channels, std::uint64_t timeReference,
                     const std::string& description, std::string* error) {
    close();
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) {
        if (error) *error = path + ": " + std::strerror(errno);
        return false;
    }
    channels_ = channels;
    dataBytes_ = 0;
    rf64_ = false;

    std::vector<char> h(kDataOffset, 0);
    putId(h, 0, "RIFF");
    put<std::uint32_t>(h, kRiffSizeAt, std::uint32_t(kDataOffset - 8));
    putId(h, 8, "WAVE");
    putId(h, kJunkIdAt, "JUNK");
    put<std::uint32_t>(h, 16, 28);

    putId(h, kFmtAt, "fmt ");
    put<std::uint32_t>(h, kFmtAt + 4, 16);
    put<std::uint16_t>(h, kFmtAt + 8, 3);  // WAVE_FORMAT_IEEE_FLOAT
    put<std::uint16_t>(h, kFmtAt + 10, std::uint16_t(channels));
    put<std::uint32_t>(h, kFmtAt + 12, sampleRate);
    put<std::uint32_t>(h, kFmtAt + 16, sampleRate * std::uint32_t(channels) * 4);
    put<std::uint16_t>(h, kFmtAt + 20, std::uint16_t(channels * 4));
    put<std::uint16_t>(h, kFmtAt + 22, 32);

    putId(h, kBextAt, "bext");
    put<std::uint32_t>(h, kBextAt + 4, kBextSize);
    const std::size_t b = kBextAt + 8;
    std::strncpy(&h[b], description.c_str(), 255);           // Description
    std::strncpy(&h[b + 256], "ProdMesh Mix", 31);            // Originator
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char date[11], tod[9];
    std::strftime(date, sizeof date, "%Y-%m-%d", &tm);
    std::strftime(tod, sizeof tod, "%H:%M:%S", &tm);
    std::memcpy(&h[b + 320], date, 10);                       // OriginationDate
    std::memcpy(&h[b + 330], tod, 8);                         // OriginationTime
    put<std::uint32_t>(h, b + 338, std::uint32_t(timeReference));
    put<std::uint32_t>(h, b + 342, std::uint32_t(timeReference >> 32));
    put<std::uint16_t>(h, b + 346, 2);                        // Version

    putId(h, kDataIdAt, "data");
    put<std::uint32_t>(h, kDataSizeAt, 0);

    if (!pwriteAll(fd_, h.data(), h.size(), 0)) {
        if (error) *error = path + ": " + std::strerror(errno);
        close();
        return false;
    }
    ::lseek(fd_, off_t(kDataOffset), SEEK_SET);
    return true;
}

bool WavWriter::write(const float* data, std::size_t frames) {
    if (fd_ < 0) return false;
    const std::size_t n = frames * std::size_t(frameBytes());
    if (!pwriteAll(fd_, data, n, off_t(kDataOffset + dataBytes_)))
        return false;
    dataBytes_ += n;
    return true;
}

bool WavWriter::patchSizes(std::uint64_t dataBytes, bool rf64) {
    const std::uint64_t riff = kDataOffset - 8 + dataBytes;
    if (!rf64)
        return pwriteVal<std::uint32_t>(fd_, std::uint32_t(riff), kRiffSizeAt) &&
               pwriteVal<std::uint32_t>(fd_, std::uint32_t(dataBytes),
                                        kDataSizeAt);
    // ds64: riffSize, dataSize, sampleCount (frames), table length 0.
    struct {
        std::uint64_t riff, data, frames;
        std::uint32_t table;
    } __attribute__((packed)) ds64{riff, dataBytes, dataBytes / frameBytes(), 0};
    static_assert(sizeof ds64 == 28);
    return pwriteAll(fd_, &ds64, sizeof ds64, off_t(kDs64At)) &&
           pwriteAll(fd_, "ds64", 4, off_t(kJunkIdAt)) &&
           pwriteAll(fd_, "RF64", 4, 0) &&
           pwriteVal<std::uint32_t>(fd_, 0xFFFFFFFFu, kRiffSizeAt) &&
           pwriteVal<std::uint32_t>(fd_, 0xFFFFFFFFu, kDataSizeAt);
}

bool WavWriter::flushHeader(bool sync) {
    if (fd_ < 0) return false;
    if (!rf64_ && dataBytes_ > rf64Threshold_) rf64_ = true;
    if (!patchSizes(dataBytes_, rf64_)) return false;
    // fsync survives a process or OS crash; surviving power loss on macOS
    // needs F_FULLFSYNC, which is far slower — the recorder chooses.
    return !sync || ::fsync(fd_) == 0;
}

void WavWriter::close() {
    if (fd_ < 0) return;
    flushHeader(true);
    ::close(fd_);
    fd_ = -1;
}

bool WavWriter::readInfo(const std::string& path, WavInfo& out) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    char h[kDataOffset];
    const bool ok = ::pread(fd, h, sizeof h, 0) == ssize_t(sizeof h);
    ::close(fd);
    if (!ok) return false;
    auto u16 = [&](std::size_t at) {
        std::uint16_t v;
        std::memcpy(&v, h + at, 2);
        return v;
    };
    auto u32 = [&](std::size_t at) {
        std::uint32_t v;
        std::memcpy(&v, h + at, 4);
        return v;
    };
    auto u64 = [&](std::size_t at) {
        std::uint64_t v;
        std::memcpy(&v, h + at, 8);
        return v;
    };
    out.rf64 = std::memcmp(h, "RF64", 4) == 0;
    if (!out.rf64 && std::memcmp(h, "RIFF", 4) != 0) return false;
    if (std::memcmp(h + kDataIdAt, "data", 4) != 0) return false;
    out.channels = u16(kFmtAt + 10);
    out.sampleRate = u32(kFmtAt + 12);
    out.dataOffset = kDataOffset;
    const std::uint64_t bytes = out.rf64 ? u64(kDs64At + 8) : u32(kDataSizeAt);
    out.frames = out.channels ? bytes / (std::uint64_t(out.channels) * 4) : 0;
    out.timeReference = std::uint64_t(u32(kBextAt + 8 + 338)) |
                        (std::uint64_t(u32(kBextAt + 8 + 342)) << 32);
    return true;
}

bool WavWriter::recover(const std::string& path, std::string* error) {
    WavInfo info;
    if (!readInfo(path, info)) {
        if (error) *error = path + ": not a ProdMesh Mix WAV";
        return false;
    }
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return false;
    const std::uint64_t frameBytes = std::uint64_t(info.channels) * 4;
    const std::uint64_t bytes =
        (std::uint64_t(st.st_size) - kDataOffset) / frameBytes * frameBytes;

    WavWriter w;
    w.fd_ = ::open(path.c_str(), O_WRONLY);
    if (w.fd_ < 0) return false;
    w.channels_ = info.channels;
    w.dataBytes_ = bytes;
    w.rf64_ = info.rf64;
    const bool ok = w.flushHeader(true);
    ::ftruncate(w.fd_, off_t(kDataOffset + bytes));  // drop a torn frame
    ::close(w.fd_);
    w.fd_ = -1;
    return ok;
}

}  // namespace pm::rec
