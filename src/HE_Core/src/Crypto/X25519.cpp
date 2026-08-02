#include <Crypto/X25519.h>

#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  Backend selection mirrors Hpak/Aes256Gcm.cpp: system OpenSSL first, fetched
//  mbedTLS otherwise, and a hard-failing stub when neither is present.
// ─────────────────────────────────────────────────────────────────────────────

#if defined(HE_HAVE_OPENSSL)
// ─── OpenSSL ─────────────────────────────────────────────────────────────────
#  include <openssl/evp.h>

namespace HE::Crypto {

bool x25519Available() { return true; }

bool x25519GenerateKeypair(uint8_t privOut[32], uint8_t pubOut[32])
{
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx) return false;

    EVP_PKEY* key = nullptr;
    bool ok = false;
    do {
        if (EVP_PKEY_keygen_init(ctx) != 1) break;
        if (EVP_PKEY_keygen(ctx, &key) != 1) break;

        size_t len = 32;
        if (EVP_PKEY_get_raw_private_key(key, privOut, &len) != 1 || len != 32) break;
        len = 32;
        if (EVP_PKEY_get_raw_public_key(key, pubOut, &len) != 1 || len != 32) break;
        ok = true;
    } while (false);

    if (key) EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(ctx);
    return ok;
}

bool x25519PublicFromPrivate(const uint8_t priv[32], uint8_t pubOut[32])
{
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv, 32);
    if (!key) return false;

    size_t len = 32;
    const bool ok = (EVP_PKEY_get_raw_public_key(key, pubOut, &len) == 1) && len == 32;
    EVP_PKEY_free(key);
    return ok;
}

bool x25519SharedSecret(const uint8_t priv[32], const uint8_t peerPub[32], uint8_t out[32])
{
    EVP_PKEY* ours  = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv, 32);
    EVP_PKEY* theirs = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peerPub, 32);
    EVP_PKEY_CTX* ctx = ours ? EVP_PKEY_CTX_new(ours, nullptr) : nullptr;

    bool ok = false;
    if (ctx && theirs) {
        size_t len = 32;
        ok = EVP_PKEY_derive_init(ctx) == 1
          && EVP_PKEY_derive_set_peer(ctx, theirs) == 1
          && EVP_PKEY_derive(ctx, out, &len) == 1
          && len == 32;
    }

    if (ctx)    EVP_PKEY_CTX_free(ctx);
    if (theirs) EVP_PKEY_free(theirs);
    if (ours)   EVP_PKEY_free(ours);
    return ok;
}

} // namespace HE::Crypto

#elif defined(HE_HAVE_MBEDTLS)
// ─── mbedTLS (PSA Crypto API) ────────────────────────────────────────────────
// The PSA API is used rather than the legacy mbedtls_ecdh_* one because the
// latter needs MBEDTLS_ALLOW_PRIVATE_ACCESS to reach the point coordinates and
// hand-rolled little-endian conversion (RFC 7748 byte order), both of which are
// easy to get subtly wrong. psa_raw_key_agreement does exactly this job.
#  include <psa/crypto.h>

namespace HE::Crypto {
namespace {

// psa_crypto_init() must run once before any other PSA call; it is idempotent
// but not automatically invoked.
bool ensurePsaInit()
{
    static bool initialized = false;
    static bool ok = false;
    if (!initialized) {
        initialized = true;
        ok = (psa_crypto_init() == PSA_SUCCESS);
    }
    return ok;
}

// Import a raw 32-byte scalar as an X25519 private key usable for ECDH.
psa_status_t importPrivate(const uint8_t priv[32], psa_key_id_t& outKey)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attr, 255);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
    return psa_import_key(&attr, priv, 32, &outKey);
}

} // namespace

bool x25519Available() { return ensurePsaInit(); }

bool x25519GenerateKeypair(uint8_t privOut[32], uint8_t pubOut[32])
{
    if (!ensurePsaInit()) return false;

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attr, 255);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);

    psa_key_id_t key = 0;
    if (psa_generate_key(&attr, &key) != PSA_SUCCESS) return false;

    size_t len = 0;
    bool ok = false;
    do {
        if (psa_export_key(key, privOut, 32, &len) != PSA_SUCCESS || len != 32) break;
        if (psa_export_public_key(key, pubOut, 32, &len) != PSA_SUCCESS || len != 32) break;
        ok = true;
    } while (false);

    psa_destroy_key(key);
    return ok;
}

bool x25519PublicFromPrivate(const uint8_t priv[32], uint8_t pubOut[32])
{
    if (!ensurePsaInit()) return false;

    psa_key_id_t key = 0;
    if (importPrivate(priv, key) != PSA_SUCCESS) return false;

    size_t len = 0;
    const bool ok = psa_export_public_key(key, pubOut, 32, &len) == PSA_SUCCESS && len == 32;
    psa_destroy_key(key);
    return ok;
}

bool x25519SharedSecret(const uint8_t priv[32], const uint8_t peerPub[32], uint8_t out[32])
{
    if (!ensurePsaInit()) return false;

    psa_key_id_t key = 0;
    if (importPrivate(priv, key) != PSA_SUCCESS) return false;

    size_t len = 0;
    const bool ok = psa_raw_key_agreement(PSA_ALG_ECDH, key, peerPub, 32,
                                          out, 32, &len) == PSA_SUCCESS && len == 32;
    psa_destroy_key(key);
    return ok;
}

} // namespace HE::Crypto

#else
// ─── No backend ──────────────────────────────────────────────────────────────
namespace HE::Crypto {

bool x25519Available() { return false; }
bool x25519GenerateKeypair(uint8_t[32], uint8_t[32])          { return false; }
bool x25519PublicFromPrivate(const uint8_t[32], uint8_t[32])  { return false; }
bool x25519SharedSecret(const uint8_t[32], const uint8_t[32], uint8_t[32]) { return false; }

} // namespace HE::Crypto

#endif
