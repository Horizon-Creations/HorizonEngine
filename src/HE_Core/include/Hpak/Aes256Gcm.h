#pragma once
#include <Types/Defines.h>
#include <cstdint>
#include <cstddef>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  AES-256-GCM (AEAD) wrapper over OpenSSL for hpak entry encryption.
//
//  IMPORTANT — threat model: this obfuscates shipped assets against casual
//  ripping. It is NOT a security boundary: a shipped game must decrypt its own
//  assets, so the key travels with the game (in project.hcfg). A determined
//  attacker recovers it from the running process. The AEAD auth tag additionally
//  makes tampering detectable (decrypt fails on a modified pak / wrong key).
//
//  Layout of the stored blob for an encrypted entry: ciphertext || 16-byte tag.
//  The 96-bit nonce is stored per entry in EntryDesc::nonce and MUST be unique
//  per (key, entry) — reusing a nonce under one key breaks GCM catastrophically.
// ─────────────────────────────────────────────────────────────────────────────

namespace Hpak
{
// True when the build has a crypto backend (OpenSSL). When false, encrypt
// requests fall back to storing plaintext and encrypted entries cannot be read.
HE_API bool cryptoAvailable();

// Fill `out[0..n)` with cryptographically-random bytes. Returns false on failure
// or when no backend is available.
HE_API bool randomBytes(uint8_t* out, size_t n);

// Encrypt `plaintext` → `out` (= ciphertext || 16-byte tag). Returns false on
// failure or when no backend is available.
HE_API bool aesGcmEncrypt(const uint8_t key[32], const uint8_t nonce[12],
                          const uint8_t* plaintext, size_t len,
                          std::vector<uint8_t>& out);

// Decrypt `data` (= ciphertext || 16-byte tag) → `out`. Returns false on an
// authentication-tag mismatch (wrong key / tampered / corrupt) or when no
// backend is available.
HE_API bool aesGcmDecrypt(const uint8_t key[32], const uint8_t nonce[12],
                          const uint8_t* data, size_t len,
                          std::vector<uint8_t>& out);

} // namespace Hpak
