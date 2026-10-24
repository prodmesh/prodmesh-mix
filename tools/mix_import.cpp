// mix-import: converts a WAV from anywhere (PCM 16/24/32-bit or float32,
// any chunk layout) into ProdMesh Mix take files — float32 with our
// crash-safe header, plus the .peaks overview the Editor draws from — so
// outside multitracks can be played back like a recorded take.
//
//   mix-import SRC.wav DST.wav            all channels into one file
//   mix-import SRC.wav DST1.wav DST2.wav  one mono file per channel
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "record/peaks.h"
#include "record/wav_writer.h"

namespace {

struct Source {
    std::FILE* f = nullptr;
    int channels = 0, bits = 0, format = 0;  // format 1 = PCM, 3 = float
    std::uint32_t rate = 0;
    std::uint64_t frames = 0;
};

bool openSource(const char* path, Source& s, std::string& err) {
    s.f = std::fopen(path, "rb");
    if (!s.f) return err = "cannot open", false;
    char riff[12];
    if (std::fread(riff, 1, 12, s.f) != 12 || std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(riff + 8, "WAVE", 4) != 0)
        return err = "not a RIFF/WAVE file", false;
    for (;;) {
        char id[4];
        std::uint32_t size = 0;
        if (std::fread(id, 1, 4, s.f) != 4 || std::fread(&size, 4, 1, s.f) != 1)
            return err = "no data chunk", false;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            std::vector<unsigned char> b(size);
            if (std::fread(b.data(), 1, size, s.f) != size) return err = "short fmt", false;
            std::uint16_t fmt, ch, bits;
            std::memcpy(&fmt, &b[0], 2);
            std::memcpy(&ch, &b[2], 2);
            std::memcpy(&s.rate, &b[4], 4);
            std::memcpy(&bits, &b[14], 2);
            if (fmt == 0xFFFE && size >= 26) std::memcpy(&fmt, &b[24], 2);  // extensible
            s.format = fmt;
            s.channels = ch;
            s.bits = bits;
            if (size & 1) std::fseek(s.f, 1, SEEK_CUR);
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (!s.channels) return err = "data before fmt", false;
            s.frames = size / (std::uint64_t(s.channels) * std::uint64_t(s.bits / 8));
            break;  // positioned at the samples
        } else {
            std::fseek(s.f, long(size + (size & 1)), SEEK_CUR);
        }
    }
    const bool ok = (s.format == 1 && (s.bits == 16 || s.bits == 24 || s.bits == 32)) ||
                    (s.format == 3 && s.bits == 32);
    if (!ok) return err = "unsupported sample format", false;
    return true;
}

float sample(const unsigned char* p, const Source& s) {
    if (s.format == 3) {
        float f;
        std::memcpy(&f, p, 4);
        return f;
    }
    if (s.bits == 16) return float(std::int16_t(p[0] | p[1] << 8)) / 32768.0f;
    if (s.bits == 24) {
        const std::int32_t v = std::int32_t(std::uint32_t(p[0]) << 8 | std::uint32_t(p[1]) << 16 |
                                            std::uint32_t(p[2]) << 24) >> 8;
        return float(v) / 8388608.0f;
    }
    std::int32_t v;
    std::memcpy(&v, p, 4);
    return float(double(v) / 2147483648.0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: mix-import SRC.wav DST.wav [DST2.wav ...]\n");
        return 2;
    }
    Source src;
    std::string err;
    if (!openSource(argv[1], src, err)) {
        std::fprintf(stderr, "mix-import: %s: %s\n", argv[1], err.c_str());
        return 1;
    }
    const int outs = argc - 2;
    if (outs != 1 && outs != src.channels) {
        std::fprintf(stderr, "mix-import: %s has %d channels: give 1 or %d outputs\n", argv[1],
                     src.channels, src.channels);
        return 2;
    }
    const int outCh = outs == 1 ? src.channels : 1;
    const auto n = static_cast<std::size_t>(outs);
    std::vector<pm::rec::WavWriter> wavs(n);
    std::vector<pm::rec::PeakWriter> peaks(n);
    for (int i = 0; i < outs; ++i) {
        const std::string dst = argv[2 + i];
        if (!wavs[std::size_t(i)].open(dst, src.rate, outCh, 0, "ProdMesh Mix import", &err)) {
            std::fprintf(stderr, "mix-import: %s: %s\n", dst.c_str(), err.c_str());
            return 1;
        }
        peaks[std::size_t(i)].open(pm::rec::peaksPathFor(dst), outCh, src.rate, 0);
    }
    const std::size_t chunk = 8192, bps = std::size_t(src.bits / 8),
                      frameBytes = bps * std::size_t(src.channels);
    std::vector<unsigned char> raw(chunk * frameBytes);
    std::vector<float> buf(chunk * std::size_t(src.channels));
    for (std::uint64_t done = 0; done < src.frames;) {
        const std::size_t want = std::size_t(std::min<std::uint64_t>(chunk, src.frames - done));
        const std::size_t got = std::fread(raw.data(), frameBytes, want, src.f);
        if (!got) break;
        for (int c = 0; c < outs; ++c) {
            float* o = buf.data();
            for (std::size_t i = 0; i < got; ++i)
                for (int k = 0; k < outCh; ++k)
                    *o++ = sample(&raw[i * frameBytes + std::size_t(outs == 1 ? k : c) * bps], src);
            wavs[std::size_t(c)].write(buf.data(), got);
            peaks[std::size_t(c)].add(buf.data(), got);
        }
        done += got;
    }
    for (int i = 0; i < outs; ++i) {
        peaks[std::size_t(i)].close();
        wavs[std::size_t(i)].close();
    }
    std::printf("%s: %d ch, %u Hz, %d-bit, %.1f s -> %d file(s)\n", argv[1], src.channels,
                src.rate, src.bits, double(src.frames) / src.rate, outs);
    return 0;
}
