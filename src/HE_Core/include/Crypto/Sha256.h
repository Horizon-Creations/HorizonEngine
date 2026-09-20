#pragma once
#include <Types/Defines.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
//  SHA-256 (FIPS 180-4), dependency-free and streaming.
//
//  Exists in this form because the integrity probe (Integrity/IntegrityProbe.h)
//  hashes the executable and every engine library at startup, and those are
//  tens of megabytes each: loading a file whole just to hash it would double
//  the peak memory of a cold start for nothing. The incremental update/finish
//  pair below works on any chunk size, so a file is hashed through one 64 KB
//  buffer.
//
//  KeyDerivation (PBKDF2 / HMAC-SHA256) uses the same implementation through
//  digest(), so there is exactly one SHA-256 in the engine and the HMAC tests
//  (test_hpak, test_net_secure) pin this one as well.
// ─────────────────────────────────────────────────────────────────────────────

namespace HE::Crypto {

class HE_API Sha256
{
public:
    static constexpr std::size_t kDigestSize = 32;

    Sha256();
    ~Sha256();

    // Feed any number of bytes, in any chunking.
    void update(const void* data, std::size_t len);
    // Write the digest and reset to the initial state, so the object can be
    // reused for the next message.
    void finish(std::uint8_t out[kDigestSize]);

    // One-shot over a memory range.
    static void digest(const void* data, std::size_t len, std::uint8_t out[kDigestSize]);

    // Hash a file streamed from disk. False when it cannot be opened or a read
    // fails part-way; `out` is untouched then.
    static bool hashFile(const std::filesystem::path& path, std::uint8_t out[kDigestSize]);

    // Lowercase hex, 64 characters — the form manifests and logs carry.
    static std::string toHex(const std::uint8_t digest[kDigestSize]);

private:
    void processBlock(const std::uint8_t block[64]);

    std::uint32_t m_state[8];
    std::uint8_t  m_buffer[64];
    std::size_t   m_buffered = 0;
    std::uint64_t m_total    = 0;   // bytes fed so far
};

} // namespace HE::Crypto
