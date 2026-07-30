#pragma once
#include <Types/Defines.h>
#include <cstdint>
#include <string>

// Derives a deterministic 256-bit key from a passphrase + salt using
// PBKDF2-HMAC-SHA256 (1 iteration — a single HMAC block). No external
// dependencies; SHA-256 is implemented inline.
//
// Only the hpak_packer CLI (--secret) derives its pak key this way, because both
// sides have to reproduce it from the passphrase. The editor's export path does
// NOT come through here: it draws a random 32-byte key (Hpak::randomBytes) and
// ships it with the build. Iterations=1 is therefore not what limits the pak's
// secrecy — the key travels with the game either way (threat model in
// Aes256Gcm.h). Raise it to >=10000 if a derived key ever has to withstand
// offline passphrase guessing.
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
