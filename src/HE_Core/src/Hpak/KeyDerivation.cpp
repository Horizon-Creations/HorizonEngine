#include <Hpak/KeyDerivation.h>
#include <cstring>
#include <cstdint>
#include <vector>

// ─── Minimal standalone SHA-256 ───────────────────────────────────────────────

static inline uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static const uint32_t kSha256K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256Block(uint32_t h[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
    {
        w[i] = (uint32_t)block[i*4]   << 24 | (uint32_t)block[i*4+1] << 16
             | (uint32_t)block[i*4+2] <<  8 | (uint32_t)block[i*4+3];
    }
    for (int i = 16; i < 64; ++i)
    {
        uint32_t s0 = ror32(w[i-15],7) ^ ror32(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ror32(w[i-2],17) ^ ror32(w[i-2],19)  ^ (w[i-2]  >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; ++i)
    {
        uint32_t S1  = ror32(e,6) ^ ror32(e,11) ^ ror32(e,25);
        uint32_t ch  = (e & f) ^ (~e & g);
        uint32_t t1  = hh + S1 + ch + kSha256K[i] + w[i];
        uint32_t S0  = ror32(a,2) ^ ror32(a,13) ^ ror32(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2  = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

static void sha256(const uint8_t* data, size_t len, uint8_t out[32])
{
    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    // Process all full 64-byte blocks
    const size_t fullBlocks = len / 64;
    for (size_t i = 0; i < fullBlocks; ++i)
        sha256Block(h, data + i * 64);

    // Padding block(s)
    uint8_t tail[128] = {};
    const size_t rem = len - fullBlocks * 64;
    std::memcpy(tail, data + fullBlocks * 64, rem);
    tail[rem] = 0x80;
    const size_t padLen = (rem < 56) ? 64 : 128;
    const uint64_t bitLen = static_cast<uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i)
        tail[padLen - 8 + i] = static_cast<uint8_t>(bitLen >> (56 - i*8));
    sha256Block(h, tail);
    if (padLen == 128)
        sha256Block(h, tail + 64);

    for (int i = 0; i < 8; ++i)
    {
        out[i*4+0] = static_cast<uint8_t>(h[i] >> 24);
        out[i*4+1] = static_cast<uint8_t>(h[i] >> 16);
        out[i*4+2] = static_cast<uint8_t>(h[i] >>  8);
        out[i*4+3] = static_cast<uint8_t>(h[i]);
    }
}

// ─── HMAC-SHA256 ─────────────────────────────────────────────────────────────

static void hmacSha256(const uint8_t* key, size_t keyLen,
                       const uint8_t* msg, size_t msgLen,
                       uint8_t out[32])
{
    uint8_t k0[64] = {};
    if (keyLen > 64) { sha256(key, keyLen, k0); }
    else             { std::memcpy(k0, key, keyLen); }

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = k0[i] ^ 0x36; opad[i] = k0[i] ^ 0x5c; }

    // inner = SHA256(ipad || msg)
    std::vector<uint8_t> inner(64 + msgLen);
    std::memcpy(inner.data(), ipad, 64);
    std::memcpy(inner.data() + 64, msg, msgLen);
    uint8_t innerHash[32];
    sha256(inner.data(), inner.size(), innerHash);

    // outer = SHA256(opad || inner)
    uint8_t outerBuf[96];
    std::memcpy(outerBuf, opad, 64);
    std::memcpy(outerBuf + 64, innerHash, 32);
    sha256(outerBuf, 96, out);
}

// ─── PBKDF2-HMAC-SHA256 (1 iteration, 32-byte output) ───────────────────────

void KeyDerivation::derive(const std::string& secret,
                            const uint8_t      salt[16],
                            uint8_t            outKey[32])
{
    // PBKDF2 block 1: HMAC-SHA256(password, salt || 0x00000001)
    uint8_t saltBlock[16 + 4];
    std::memcpy(saltBlock, salt, 16);
    saltBlock[16] = 0; saltBlock[17] = 0; saltBlock[18] = 0; saltBlock[19] = 1;

    hmacSha256(reinterpret_cast<const uint8_t*>(secret.c_str()), secret.size(),
               saltBlock, sizeof(saltBlock), outKey);
}
