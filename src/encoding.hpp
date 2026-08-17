#pragma once

#include <cstdint>
#include <cstring>
#include <string>

// --- byte <-> hex helpers (shared by Miner + selftest) ---

inline std::string bytes_to_hex(const uint8_t* data, int len) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve((size_t)len * 2);
    for (int i = 0; i < len; i++) {
        out.push_back(hex[data[i] >> 4]);
        out.push_back(hex[data[i] & 0xf]);
    }
    return out;
}

// Big-endian target hex (stratum style) -> right-aligned bytes[32].
// Returns number of bytes actually present (0 on invalid hex).
// out is zero-filled first; only the low `used` bytes are meaningful.
inline int hex_to_target_bytes(const std::string& hex, uint8_t out[32], bool* ok) {
    memset(out, 0, 32);
    std::string t = hex;
    if (t.size() >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
    if (t.size() % 2 != 0) { *ok = false; return 0; }
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int hexLen = (int)t.size();
    int bytes = 0;
    for (int i = 0; i < 32; i++) {
        int hIdx = hexLen - 2 - 2 * i;  // nibbles from the right (big-endian)
        if (hIdx < 0) break;
        int hi = hexval(t[hIdx]);
        int lo = hexval(t[hIdx + 1]);
        if (hi < 0 || lo < 0) { *ok = false; return 0; }
        out[31 - i] = (uint8_t)((hi << 4) | lo);
        bytes++;
    }
    return bytes;
}

// 8-byte MSB of the target as delivered (left-aligned in the hex string),
// right-padded with zeros. Used for the ethproxy 64-bit share check.
// Sets *ok=false when the hex string contains non-hex characters.
inline void hex_to_target_msb8(const std::string& hex, uint8_t out[8], bool* ok) {
    memset(out, 0, 8);
    std::string t = hex;
    if (t.size() >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 8; i++) {
        int hIdx = 2 * i;
        if (hIdx + 1 >= (int)t.size()) break;
        int hi = hexval(t[hIdx]);
        int lo = hexval(t[hIdx + 1]);
        if (hi < 0 || lo < 0) { *ok = false; return; }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
}
