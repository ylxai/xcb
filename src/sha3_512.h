// Keccak-f[1600] — verified-correct reference implementation
// From: https://github.com/maandree/libkeccak/blob/master/keccak.c
// Simplified for SHA3-512 only, single-file, zero heap allocation.
#include <cstdint>
#include <cstring>

static inline uint64_t rotl64(uint64_t x, int n) {
    return (x << n) | (x >> (64 - n));
}

static const uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL,
    0x800000000000808AULL, 0x8000000080008000ULL,
    0x000000000000808BULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008AULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000AULL,
    0x000000008000808BULL, 0x800000000000008BULL,
    0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800AULL, 0x800000008000000AULL,
    0x8000000080008081ULL, 0x8000000000008080ULL,
    0x0000000080000001ULL, 0x8000000080008008ULL
};

static const int ROTC[24] = {
     1,  3,  6, 10, 15, 21, 28, 36, 45, 55,  2, 14,
    27, 41, 56,  8, 25, 43, 62, 18, 39, 61, 20, 44
};

static const int PILN[24] = {
    10,  7, 11, 17, 18,  3,  5, 16,  8, 21, 24,  4,
    15, 23, 19, 13, 12,  2, 20, 14, 22,  9,  6,  1
};

static void keccak_f(uint64_t st[25]) {
    for (int round = 0; round < 24; round++) {
        uint64_t bc[5];

        // Theta
        for (int i = 0; i < 5; i++)
            bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];

        for (int i = 0; i < 5; i++) {
            uint64_t t = bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5)
                st[j + i] ^= t;
        }

        // Rho and Pi
        {
            uint64_t t = st[1];
            for (int i = 0; i < 24; i++) {
                int j = PILN[i];
                uint64_t tmp = st[j];
                st[j] = rotl64(t, ROTC[i]);
                t = tmp;
            }
        }

        // Chi
        for (int j = 0; j < 25; j += 5) {
            uint64_t t[5];
            for (int i = 0; i < 5; i++)
                t[i] = st[j + i];
            for (int i = 0; i < 5; i++)
                st[j + i] = t[i] ^ ((~t[(i + 1) % 5]) & t[(i + 2) % 5]);
        }

        // Iota
        st[0] ^= RC[round];
    }
}

// SHA3-512: zero heap allocation, pure stack
static void sha3_512(const uint8_t* input, size_t len, uint8_t output[64]) {
    constexpr int RATE = 72; // SHA3-512 rate in bytes
    uint64_t st[25] = {};

    // Absorb
    for (size_t i = 0; i < len; i++) {
        int byte_pos = i % RATE;
        st[byte_pos / 8] ^= static_cast<uint64_t>(input[i]) << ((byte_pos % 8) * 8);
        if (byte_pos == RATE - 1)
            keccak_f(st);
    }

    // Pad: SHA3 domain separator 0x06
    int pad_pos = len % RATE;
    st[pad_pos / 8] ^= 0x06ULL << ((pad_pos % 8) * 8);
    st[(RATE - 1) / 8] ^= 0x80ULL << (7 * 8); // last byte of rate block
    keccak_f(st);

    // Squeeze 64 bytes
    for (int i = 0; i < 8; i++) {
        uint64_t v = st[i];
        for (int j = 0; j < 8; j++)
            output[i * 8 + j] = static_cast<uint8_t>(v >> (j * 8));
    }
}
