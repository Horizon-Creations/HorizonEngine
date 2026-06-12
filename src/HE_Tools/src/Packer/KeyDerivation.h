#pragma once
#include <cstdint>
#include <string>

// Derives a deterministic 256-bit AES key from a project secret string.
// Uses PBKDF2-HMAC-SHA256 with a fixed salt per project.
// The same secret + same salt always yields the same key —
// so HorizonGame can re-derive the key at runtime without storing it in plaintext.
class KeyDerivation {
public:
    // secret = project-specific passphrase (never hardcode in source, load from env)
    // salt   = project UUID as bytes (stable, not secret)
    // outKey = exactly 32 bytes output
    static void derive(const std::string& secret,
                       const uint8_t      salt[16],
                       uint8_t            outKey[32]);
};
