#pragma once
#include <Types/Defines.h>
#include <cstdint>
#include <string>

// Derives a deterministic 256-bit key from a passphrase + salt using
// PBKDF2-HMAC-SHA256 (1 iteration). No external dependencies; SHA-256 is
// implemented inline. Replace iterations=1 with >=10000 for stronger security.
class HE_API KeyDerivation
{
public:
    // secret = project-specific passphrase (never hardcode; load from env/config)
    // salt   = 16 stable bytes (e.g. project UUID bytes)
    // outKey = exactly 32 bytes written
    static void derive(const std::string& secret,
                       const uint8_t      salt[16],
                       uint8_t            outKey[32]);
};
