#include "crypto/mini_crypto.h"

#include <cstring>

namespace miplay::crypto {

namespace {

inline uint32_t rol32(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t ror32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

// Standard AES S-Box & Inv S-Box
constexpr uint8_t kSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

constexpr uint8_t kInvSbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

constexpr uint8_t kRcon[11] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

inline uint8_t xtime(uint8_t x) {
    return static_cast<uint8_t>((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}

inline uint8_t gmul(uint8_t x, uint8_t y) {
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i) {
        if (y & 1) r ^= x;
        x = xtime(x);
        y >>= 1;
    }
    return r;
}

void key_expansion(const uint8_t key[16], uint8_t rk[176]) {
    std::memcpy(rk, key, 16);
    for (int i = 4; i < 44; ++i) {
        uint8_t tmp[4];
        std::memcpy(tmp, rk + (i - 1) * 4, 4);
        if (i % 4 == 0) {
            uint8_t t = tmp[0];
            tmp[0] = kSbox[tmp[1]] ^ kRcon[i / 4];
            tmp[1] = kSbox[tmp[2]];
            tmp[2] = kSbox[tmp[3]];
            tmp[3] = kSbox[t];
        }
        for (int j = 0; j < 4; ++j) {
            rk[i * 4 + j] = rk[(i - 4) * 4 + j] ^ tmp[j];
        }
    }
}

void add_round_key(uint8_t s[16], const uint8_t* rk) {
    for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
}

void aes128_encrypt_block(uint8_t s[16], const uint8_t rk[176]) {
    add_round_key(s, rk);
    for (int round = 1; round <= 10; ++round) {
        for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]];
        uint8_t t;
        t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
        std::swap(s[2], s[10]); std::swap(s[6], s[14]);
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
        if (round < 10) {
            for (int c = 0; c < 4; ++c) {
                int i = c * 4;
                uint8_t a0 = s[i], a1 = s[i + 1], a2 = s[i + 2], a3 = s[i + 3];
                uint8_t tm = a0 ^ a1 ^ a2 ^ a3;
                s[i]     ^= tm ^ xtime(a0 ^ a1);
                s[i + 1] ^= tm ^ xtime(a1 ^ a2);
                s[i + 2] ^= tm ^ xtime(a2 ^ a3);
                s[i + 3] ^= tm ^ xtime(a3 ^ a0);
            }
        }
        add_round_key(s, rk + round * 16);
    }
}

void aes128_decrypt_block(uint8_t s[16], const uint8_t rk[176]) {
    add_round_key(s, rk + 160);
    for (int round = 9; round >= 0; --round) {
        uint8_t t;
        t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
        std::swap(s[2], s[10]); std::swap(s[6], s[14]);
        t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
        for (int i = 0; i < 16; ++i) s[i] = kInvSbox[s[i]];
        add_round_key(s, rk + round * 16);
        if (round > 0) {
            for (int c = 0; c < 4; ++c) {
                int i = c * 4;
                uint8_t a0 = s[i], a1 = s[i + 1], a2 = s[i + 2], a3 = s[i + 3];
                s[i]     = gmul(a0,0x0e)^gmul(a1,0x0b)^gmul(a2,0x0d)^gmul(a3,0x09);
                s[i + 1] = gmul(a0,0x09)^gmul(a1,0x0e)^gmul(a2,0x0b)^gmul(a3,0x0d);
                s[i + 2] = gmul(a0,0x0d)^gmul(a1,0x09)^gmul(a2,0x0e)^gmul(a3,0x0b);
                s[i + 3] = gmul(a0,0x0b)^gmul(a1,0x0d)^gmul(a2,0x09)^gmul(a3,0x0e);
            }
        }
    }
}

constexpr uint32_t kMd5S[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};

constexpr uint32_t kMd5K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};

constexpr uint32_t kSha256K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

} // namespace

std::array<uint8_t, 16> md5(const uint8_t* data, size_t len) {
    uint32_t a0 = 0x67452301U, b0 = 0xefcdab89U, c0 = 0x98badcfeU, d0 = 0x10325476U;
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0x00);
    uint64_t bits = static_cast<uint64_t>(len) * 8ULL;
    for (int i = 0; i < 8; ++i) msg.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; ++i) {
            size_t o = chunk + i * 4;
            M[i] = static_cast<uint32_t>(msg[o]) |
                   (static_cast<uint32_t>(msg[o + 1]) << 8) |
                   (static_cast<uint32_t>(msg[o + 2]) << 16) |
                   (static_cast<uint32_t>(msg[o + 3]) << 24);
        }
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t F, g;
            if (i < 16)      { F = (B & C) | ((~B) & D); g = i; }
            else if (i < 32) { F = (D & B) | ((~D) & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;            g = (3 * i + 5) % 16; }
            else             { F = C ^ (B | (~D));       g = (7 * i) % 16; }
            F = F + A + kMd5K[i] + M[g];
            A = D; D = C; C = B; B = B + rol32(F, kMd5S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }

    std::array<uint8_t, 16> out{};
    uint32_t hs[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>(hs[i] & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 8) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 16) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>((hs[i] >> 24) & 0xFF);
    }
    return out;
}

std::array<uint8_t, 20> sha1(const uint8_t* data, size_t len) {
    uint32_t h0 = 0x67452301U, h1 = 0xEFCDAB89U, h2 = 0x98BADCFEU, h3 = 0x10325476U, h4 = 0xC3D2E1F0U;
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0x00);
    uint64_t bits = static_cast<uint64_t>(len) * 8ULL;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            size_t o = chunk + i * 4;
            w[i] = (static_cast<uint32_t>(msg[o]) << 24) |
                   (static_cast<uint32_t>(msg[o + 1]) << 16) |
                   (static_cast<uint32_t>(msg[o + 2]) << 8) |
                   static_cast<uint32_t>(msg[o + 3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999U; }
            else if (i < 40) { f = b ^ c ^ d;            k = 0x6ED9EBA1U; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCU; }
            else             { f = b ^ c ^ d;            k = 0xCA62C1D6U; }
            uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    std::array<uint8_t, 20> out{};
    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>((hs[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xFF);
    }
    return out;
}

std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len) {
    uint32_t h[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0x00);
    uint64_t bits = static_cast<uint64_t>(len) * 8ULL;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            size_t o = chunk + i * 4;
            w[i] = (static_cast<uint32_t>(msg[o]) << 24) |
                   (static_cast<uint32_t>(msg[o + 1]) << 16) |
                   (static_cast<uint32_t>(msg[o + 2]) << 8) |
                   static_cast<uint32_t>(msg[o + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = hh + S1 + ch + kSha256K[i] + w[i];
            uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xFF);
    }
    return out;
}

std::array<uint8_t, 20> hmac_sha1(const uint8_t* key, size_t key_len, const uint8_t* data, size_t len) {
    uint8_t kblock[64]{};
    if (key_len > 64) {
        auto h = sha1(key, key_len);
        std::memcpy(kblock, h.data(), 20);
    } else {
        std::memcpy(kblock, key, key_len);
    }
    std::vector<uint8_t> inner(64 + len);
    for (int i = 0; i < 64; ++i) inner[i] = kblock[i] ^ 0x36;
    if (len > 0) std::memcpy(inner.data() + 64, data, len);
    auto ih = sha1(inner.data(), inner.size());

    uint8_t outer[64 + 20];
    for (int i = 0; i < 64; ++i) outer[i] = kblock[i] ^ 0x5c;
    std::memcpy(outer + 64, ih.data(), 20);
    return sha1(outer, sizeof(outer));
}

std::array<uint8_t, 32> hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t len) {
    uint8_t kblock[64]{};
    if (key_len > 64) {
        auto h = sha256(key, key_len);
        std::memcpy(kblock, h.data(), 32);
    } else {
        std::memcpy(kblock, key, key_len);
    }
    std::vector<uint8_t> inner(64 + len);
    for (int i = 0; i < 64; ++i) inner[i] = kblock[i] ^ 0x36;
    if (len > 0) std::memcpy(inner.data() + 64, data, len);
    auto ih = sha256(inner.data(), inner.size());

    uint8_t outer[64 + 32];
    for (int i = 0; i < 64; ++i) outer[i] = kblock[i] ^ 0x5c;
    std::memcpy(outer + 64, ih.data(), 32);
    return sha256(outer, sizeof(outer));
}

std::string hex_lower(const uint8_t* data, size_t len) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

std::string base64_encode(const uint8_t* data, size_t len) {
    static const char kTab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) v |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTab[(v >> 18) & 0x3F]);
        out.push_back(kTab[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? kTab[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? kTab[v & 0x3F] : '=');
    }
    return out;
}

uint32_t crc32_mpeg2(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x80000000U) crc = (crc << 1) ^ 0x04C11DB7U;
            else crc <<= 1;
        }
    }
    return crc;
}

std::vector<uint8_t> aes128_cbc_encrypt_zeropad(
    const uint8_t* plaintext, size_t len, const uint8_t key[16], uint8_t iv_inout[16], uint8_t* out_pad_len) {
    uint8_t rk[176];
    key_expansion(key, rk);

    uint8_t pad = static_cast<uint8_t>(16 - (len % 16));
    if (out_pad_len) *out_pad_len = pad;

    std::vector<uint8_t> buf(len + pad, 0x00);
    if (len > 0 && plaintext) std::memcpy(buf.data(), plaintext, len);

    uint8_t prev[16];
    std::memcpy(prev, iv_inout, 16);
    for (size_t off = 0; off < buf.size(); off += 16) {
        for (int i = 0; i < 16; ++i) buf[off + i] ^= prev[i];
        aes128_encrypt_block(buf.data() + off, rk);
        std::memcpy(prev, buf.data() + off, 16);
    }
    std::memcpy(iv_inout, prev, 16);
    return buf;
}

std::optional<std::vector<uint8_t>> aes128_cbc_decrypt_zeropad(
    const uint8_t* ciphertext, size_t cipher_len, uint8_t pad_len, const uint8_t key[16], uint8_t iv_inout[16]) {
    if (cipher_len == 0 || (cipher_len % 16) != 0 || pad_len < 1 || pad_len > 16 || pad_len > cipher_len) {
        return std::nullopt;
    }
    uint8_t rk[176];
    key_expansion(key, rk);

    std::vector<uint8_t> out(ciphertext, ciphertext + cipher_len);
    uint8_t prev[16];
    std::memcpy(prev, iv_inout, 16);

    for (size_t off = 0; off < cipher_len; off += 16) {
        uint8_t cur_ct[16];
        std::memcpy(cur_ct, out.data() + off, 16);
        aes128_decrypt_block(out.data() + off, rk);
        for (int i = 0; i < 16; ++i) out[off + i] ^= prev[i];
        std::memcpy(prev, cur_ct, 16);
    }

    for (size_t i = 0; i < pad_len; ++i) {
        if (out[cipher_len - 1 - i] != 0x00) return std::nullopt;
    }
    std::memcpy(iv_inout, prev, 16);
    out.resize(cipher_len - pad_len);
    return out;
}

} // namespace miplay::crypto
