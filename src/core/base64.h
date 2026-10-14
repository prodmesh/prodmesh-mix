// Base64 (RFC 4648) for opaque plugin state in JSON sessions.
#pragma once

#include <cstdint>
#include <string>

namespace pm {

inline std::string base64Encode(const std::string& in) {
    static const char* k =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const std::uint32_t v = (std::uint8_t(in[i]) << 16) |
                                (std::uint8_t(in[i + 1]) << 8) |
                                std::uint8_t(in[i + 2]);
        out += k[v >> 18];
        out += k[(v >> 12) & 63];
        out += k[(v >> 6) & 63];
        out += k[v & 63];
    }
    if (i < in.size()) {
        std::uint32_t v = std::uint8_t(in[i]) << 16;
        if (i + 1 < in.size()) v |= std::uint8_t(in[i + 1]) << 8;
        out += k[v >> 18];
        out += k[(v >> 12) & 63];
        out += i + 1 < in.size() ? k[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

inline bool base64Decode(const std::string& in, std::string& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) {
            if (c == '\n' || c == '\r') continue;
            return false;
        }
        acc = (acc << 6) | std::uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((acc >> bits) & 0xff);
        }
    }
    return true;
}

}  // namespace pm
