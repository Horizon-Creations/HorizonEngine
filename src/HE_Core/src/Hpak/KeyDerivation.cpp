#include <Hpak/KeyDerivation.h>
#include <Crypto/Sha256.h>
#include <cstring>
#include <cstdint>
#include <vector>

// The SHA-256 itself lives in Crypto/Sha256.cpp (one implementation for HMAC
// here and for the file hashing of the integrity probe).
static inline void sha256(const uint8_t* data, size_t len, uint8_t out[32])
{
    HE::Crypto::Sha256::digest(data, len, out);
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

void KeyDerivation::hmac(const uint8_t* key, size_t keyLen,
                         const uint8_t* msg, size_t msgLen,
                         uint8_t        out[32])
{
    hmacSha256(key, keyLen, msg, msgLen, out);
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
