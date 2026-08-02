#pragma once
#include <Types/Defines.h>
#include <cstddef>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
//  X25519 (RFC 7748) elliptic-curve Diffie-Hellman.
//
//  Exists so HorizonNet's session handshake can have **forward secrecy**: each
//  connection derives its key from a throwaway keypair, and the private halves
//  are discarded immediately afterwards. Without this, the session key is a
//  deterministic function of the join secret, so anyone who records the
//  ciphertext and later obtains that secret can decrypt the recording
//  retroactively.
//
//  Backed by the same crypto backends as the AES-GCM wrapper next door: system
//  OpenSSL when available, otherwise the fetched mbedTLS (via its PSA API).
//  Neither implements the curve here — scalar clamping and constant-time
//  arithmetic come from the library.
//
//  Correctness is pinned by the RFC 7748 §6.1 known-answer vectors in
//  tests/test_crypto_x25519.cpp, so both backends are checked against the spec
//  rather than only against each other.
// ─────────────────────────────────────────────────────────────────────────────

namespace HE::Crypto {

// False when the build has no crypto backend, in which case every call below
// fails. Callers must treat that as "no forward secrecy available", not as a
// reason to silently continue.
HE_API bool x25519Available();

// Generate an ephemeral keypair. Both outputs are 32 bytes.
HE_API bool x25519GenerateKeypair(uint8_t privOut[32], uint8_t pubOut[32]);

// Recompute the public key belonging to a private scalar.
HE_API bool x25519PublicFromPrivate(const uint8_t priv[32], uint8_t pubOut[32]);

// Diffie-Hellman: combine our private scalar with the peer's public key.
// Both sides arrive at the same 32-byte secret.
//
// The raw output must NOT be used as a key directly — run it through a KDF
// (HMAC) first, as SecureTransport does. It is a curve point, not uniformly
// random bytes.
HE_API bool x25519SharedSecret(const uint8_t priv[32], const uint8_t peerPub[32],
                               uint8_t out[32]);

} // namespace HE::Crypto
