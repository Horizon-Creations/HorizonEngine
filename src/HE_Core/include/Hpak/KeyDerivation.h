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

    // Raw HMAC-SHA256, exposed because it is a general-purpose primitive rather
    // than an hpak detail: HorizonNet's session handshake uses it both for the
    // challenge-response MAC and for deriving the per-connection session key
    // (with distinct domain-separation labels, so the MAC that travels on the
    // wire reveals nothing about the key). Implementation is dependency-free —
    // SHA-256 is inline here, so this works even without a crypto backend.
    static void hmac(const uint8_t* key, size_t keyLen,
                     const uint8_t* msg, size_t msgLen,
                     uint8_t        out[32]);
};
