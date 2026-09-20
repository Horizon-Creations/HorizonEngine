#include <Crypto/Sha256.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace HE::Crypto {

namespace {

inline std::uint32_t ror32(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

const std::uint32_t kK[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

const std::uint32_t kInit[8] = {
    0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
};

} // namespace

Sha256::Sha256()
{
    std::memcpy(m_state, kInit, sizeof(m_state));
}

Sha256::~Sha256() = default;

void Sha256::processBlock(const std::uint8_t block[64])
{
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
    {
        w[i] = (std::uint32_t)block[i*4]   << 24 | (std::uint32_t)block[i*4+1] << 16
             | (std::uint32_t)block[i*4+2] <<  8 | (std::uint32_t)block[i*4+3];
    }
    for (int i = 16; i < 64; ++i)
    {
        const std::uint32_t s0 = ror32(w[i-15],7) ^ ror32(w[i-15],18) ^ (w[i-15] >> 3);
        const std::uint32_t s1 = ror32(w[i-2],17) ^ ror32(w[i-2],19)  ^ (w[i-2]  >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    std::uint32_t a=m_state[0],b=m_state[1],c=m_state[2],d=m_state[3];
    std::uint32_t e=m_state[4],f=m_state[5],g=m_state[6],hh=m_state[7];
    for (int i = 0; i < 64; ++i)
    {
        const std::uint32_t S1  = ror32(e,6) ^ ror32(e,11) ^ ror32(e,25);
        const std::uint32_t ch  = (e & f) ^ (~e & g);
        const std::uint32_t t1  = hh + S1 + ch + kK[i] + w[i];
        const std::uint32_t S0  = ror32(a,2) ^ ror32(a,13) ^ ror32(a,22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2  = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    m_state[0]+=a; m_state[1]+=b; m_state[2]+=c; m_state[3]+=d;
    m_state[4]+=e; m_state[5]+=f; m_state[6]+=g; m_state[7]+=hh;
}

void Sha256::update(const void* data, std::size_t len)
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    m_total += len;

    // Top up a partial block first.
    if (m_buffered > 0)
    {
        const std::size_t take = std::min(len, sizeof(m_buffer) - m_buffered);
        std::memcpy(m_buffer + m_buffered, p, take);
        m_buffered += take;
        p   += take;
        len -= take;
        if (m_buffered < sizeof(m_buffer)) return;
        processBlock(m_buffer);
        m_buffered = 0;
    }
    // Whole blocks straight from the caller's memory.
    while (len >= 64)
    {
        processBlock(p);
        p   += 64;
        len -= 64;
    }
    if (len > 0)
    {
        std::memcpy(m_buffer, p, len);
        m_buffered = len;
    }
}

void Sha256::finish(std::uint8_t out[kDigestSize])
{
    // Padding: 0x80, zeros to 56 mod 64, then the bit length big-endian.
    std::uint8_t tail[128] = {};
    std::memcpy(tail, m_buffer, m_buffered);
    tail[m_buffered] = 0x80;
    const std::size_t   padLen = (m_buffered < 56) ? 64 : 128;
    const std::uint64_t bitLen = m_total * 8;
    for (int i = 0; i < 8; ++i)
        tail[padLen - 8 + i] = static_cast<std::uint8_t>(bitLen >> (56 - i*8));
    processBlock(tail);
    if (padLen == 128)
        processBlock(tail + 64);

    for (int i = 0; i < 8; ++i)
    {
        out[i*4+0] = static_cast<std::uint8_t>(m_state[i] >> 24);
        out[i*4+1] = static_cast<std::uint8_t>(m_state[i] >> 16);
        out[i*4+2] = static_cast<std::uint8_t>(m_state[i] >>  8);
        out[i*4+3] = static_cast<std::uint8_t>(m_state[i]);
    }

    std::memcpy(m_state, kInit, sizeof(m_state));
    m_buffered = 0;
    m_total    = 0;
}

void Sha256::digest(const void* data, std::size_t len, std::uint8_t out[kDigestSize])
{
    Sha256 h;
    h.update(data, len);
    h.finish(out);
}

bool Sha256::hashFile(const std::filesystem::path& path, std::uint8_t out[kDigestSize])
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    Sha256 h;
    std::vector<char> buf(64 * 1024);
    while (f)
    {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();
        if (got > 0) h.update(buf.data(), static_cast<std::size_t>(got));
    }
    // eof is the normal end; anything else (a read error mid-file) is a failure
    // — a half-hashed file would compare unequal to itself on the next start.
    if (!f.eof()) return false;
    h.finish(out);
    return true;
}

std::string Sha256::toHex(const std::uint8_t digest[kDigestSize])
{
    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.resize(kDigestSize * 2);
    for (std::size_t i = 0; i < kDigestSize; ++i)
    {
        s[i*2]   = kHex[digest[i] >> 4];
        s[i*2+1] = kHex[digest[i] & 0x0F];
    }
    return s;
}

} // namespace HE::Crypto
