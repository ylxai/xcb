#include "encoding.hpp"
#include <cstdint>
#include <cstring>
#include <endian.h>
#include <iostream>
#include <string>

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, const char* name) {
    g_checks++;
    if (!cond) {
        g_failed++;
        std::cout << "[SELFTEST] FAIL: " << name << std::endl;
    }
}

}  // namespace

bool run_selftest() {
    std::cout << "=== SELFTEST ===" << std::endl;

    // 1. 64-hex target -> 32 bytes right-aligned, big-endian
    {
        uint8_t out[32];
        bool ok = true;
        int used = hex_to_target_bytes(
            "0x00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
            out, &ok);
        check(ok && used == 32, "64hex used==32");
        check(out[0] == 0x00 && out[1] == 0x00 && out[2] == 0x00 && out[3] == 0x00,
              "64hex leading zeros");
        check(out[4] == 0xff && out[31] == 0xff, "64hex trailing ff");
    }

    // 2. 60-hex ethproxy target -> 30 bytes, right-aligned (low 2 bytes zero)
    {
        uint8_t out[32];
        bool ok = true;
        int used = hex_to_target_bytes(
            "0x00000000dbe6fecebdedd5beb573440e5a884d1b2fbf06fcce912adcb8d8",
            out, &ok);
        check(ok && used == 30, "60hex used==30");
        check(out[0] == 0x00 && out[1] == 0x00, "60hex low bytes zero");
        check(out[2] == 0x00 && out[31] == 0xd8, "60hex right aligned");
    }

    // 3. invalid hex rejected
    {
        uint8_t out[32];
        bool ok = true;
        int used = hex_to_target_bytes("0xzz00", out, &ok);
        check(!ok && used == 0, "invalid hex rejected");
    }

    // 4. odd-length hex rejected
    {
        uint8_t out[32];
        bool ok = true;
        int used = hex_to_target_bytes("0xabc", out, &ok);
        check(!ok && used == 0, "odd hex rejected");
    }

    // 5. msb8 from 60-hex target (left-aligned)
    {
        uint8_t out[8];
        bool ok = true;
        hex_to_target_msb8("0x00000000dbe6fecebdedd5be", out, &ok);
        check(ok, "msb8 ok");
        check(out[0] == 0x00 && out[3] == 0x00 && out[4] == 0xdb && out[5] == 0xe6 &&
                  out[6] == 0xfe && out[7] == 0xce,
              "msb8 values");
    }

    // 6. msb8 invalid hex
    {
        uint8_t out[8];
        bool ok = true;
        hex_to_target_msb8("0xzz", out, &ok);
        check(!ok, "msb8 invalid rejected");
    }

    // 7. bytes_to_hex round-trip
    {
        uint8_t data[4] = {0xde, 0xad, 0xbe, 0xef};
        check(bytes_to_hex(data, 4) == "deadbeef", "bytes_to_hex");
    }

    // 8. full-target compare: less / equal / greater
    {
        uint8_t target[32];
        bool ok = true;
        hex_to_target_bytes("0x00000000ffffffff000000000000000000000000000000000000000000000000",
                            target, &ok);
        uint8_t h1[32] = {0};
        h1[4] = 0xff; h1[5] = 0xff; h1[6] = 0xff; h1[7] = 0xfe;  // < target
        check(full_target_meets(h1, target, 32), "full less");
        uint8_t h2[32];
        memcpy(h2, target, 32);
        check(full_target_meets(h2, target, 32), "full equal");
        uint8_t h3[32] = {0};
        h3[4] = 0xff; h3[5] = 0xff; h3[6] = 0xff; h3[7] = 0xff;
        h3[8] = 0x00; h3[9] = 0x00; h3[10] = 0x01;  // > target
        check(!full_target_meets(h3, target, 32), "full greater");
    }

    // 9. msb8 compare: less / equal / greater (strict <, as pool validates)
    {
        uint8_t msb8[8] = {0x00, 0x00, 0x00, 0x00, 0xdb, 0xe6, 0xfe, 0xce};
        uint8_t h1[32] = {0};
        h1[4] = 0xdb; h1[5] = 0xe6; h1[6] = 0xfe; h1[7] = 0xcd;
        check(msb8_target_meets(h1, msb8), "msb8 less");
        uint8_t h2[32] = {0};
        memcpy(h2 + 4, msb8 + 4, 4);
        check(!msb8_target_meets(h2, msb8), "msb8 equal strict");
        uint8_t h3[32] = {0};
        h3[4] = 0xdc;
        check(!msb8_target_meets(h3, msb8), "msb8 greater");
    }

    // 10. blob = header(32) + nonce LE(8)
    {
        uint8_t header[32];
        for (int i = 0; i < 32; i++) header[i] = (uint8_t)(0xaa + i);
        uint8_t blob[40];
        uint64_t nonce = 1;
        memcpy(blob, header, 32);
        memcpy(blob + 32, &nonce, 8);
        check(blob[32] == 0x01 && blob[33] == 0x00 && blob[39] == 0x00,
              "blob nonce LE");
        // nonce BE hex for submission
        uint64_t nonceBE = htobe64(nonce);
        check(bytes_to_hex(reinterpret_cast<const uint8_t*>(&nonceBE), 8) ==
                  "0000000000000001",
              "nonce BE hex");
    }

    std::cout << (g_failed == 0 ? "[SELFTEST] OK: " : "[SELFTEST] FAILED: ")
              << g_checks << " checks" << std::endl;
    return g_failed == 0;
}
