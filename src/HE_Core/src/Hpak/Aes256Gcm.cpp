#include <Hpak/Aes256Gcm.h>
#include <cstdint>

#if defined(HE_HAVE_OPENSSL)
#  include <openssl/evp.h>
#  include <openssl/rand.h>

namespace Hpak
{
static constexpr int kTagLen   = 16;
static constexpr int kNonceLen = 12;

bool cryptoAvailable() { return true; }

bool randomBytes(uint8_t* out, size_t n)
{
    if (n == 0) return true;
    if (!out)   return false;
    return RAND_bytes(out, static_cast<int>(n)) == 1;
}

bool aesGcmEncrypt(const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t* plaintext, size_t len,
                   std::vector<uint8_t>& out)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    bool ok = false;
    do {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceLen, nullptr) != 1) break;
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) != 1) break;

        out.resize(len + kTagLen);
        int outLen = 0, total = 0;
        if (len > 0)
        {
            if (EVP_EncryptUpdate(ctx, out.data(), &outLen, plaintext,
                                  static_cast<int>(len)) != 1) break;
            total = outLen;
        }
        if (EVP_EncryptFinal_ex(ctx, out.data() + total, &outLen) != 1) break; // GCM: 0 extra
        total += outLen;

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLen,
                                out.data() + total) != 1) break;
        out.resize(static_cast<size_t>(total) + kTagLen);
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    if (!ok) out.clear();
    return ok;
}

bool aesGcmDecrypt(const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t* data, size_t len,
                   std::vector<uint8_t>& out)
{
    if (len < static_cast<size_t>(kTagLen)) return false;
    const size_t   ctLen = len - kTagLen;
    const uint8_t* tag   = data + ctLen;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    bool ok = false;
    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceLen, nullptr) != 1) break;
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) != 1) break;

        out.resize(ctLen);
        int outLen = 0, total = 0;
        if (ctLen > 0)
        {
            if (EVP_DecryptUpdate(ctx, out.data(), &outLen, data,
                                  static_cast<int>(ctLen)) != 1) break;
            total = outLen;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagLen,
                                const_cast<uint8_t*>(tag)) != 1) break;
        // Returns 0 on tag mismatch (wrong key / tampered / corrupt).
        if (EVP_DecryptFinal_ex(ctx, out.data() + total, &outLen) != 1) break;
        total += outLen;
        out.resize(static_cast<size_t>(total));
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    if (!ok) out.clear();
    return ok;
}

} // namespace Hpak

#elif defined(HE_HAVE_MBEDTLS) // ── mbedTLS fallback (fetched when no system OpenSSL) ──
#  include <mbedtls/gcm.h>
#  include <mbedtls/ctr_drbg.h>
#  include <mbedtls/entropy.h>

namespace Hpak
{
static constexpr int kTagLen   = 16;
static constexpr int kNonceLen = 12;

bool cryptoAvailable() { return true; }

bool randomBytes(uint8_t* out, size_t n)
{
    if (n == 0) return true;
    if (!out)   return false;

    // A fresh entropy-seeded CTR-DRBG per call (mirrors OpenSSL's RAND_bytes and
    // avoids shared mutable state); this path runs at pack time, not in a hot loop.
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    bool ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                    nullptr, 0) == 0;
    // ctr_drbg caps one request at MBEDTLS_CTR_DRBG_MAX_REQUEST (1024) bytes.
    while (ok && n > 0)
    {
        const size_t chunk = n < MBEDTLS_CTR_DRBG_MAX_REQUEST
                                 ? n : static_cast<size_t>(MBEDTLS_CTR_DRBG_MAX_REQUEST);
        if (mbedtls_ctr_drbg_random(&drbg, out, chunk) != 0) { ok = false; break; }
        out += chunk;
        n   -= chunk;
    }

    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return ok;
}

bool aesGcmEncrypt(const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t* plaintext, size_t len,
                   std::vector<uint8_t>& out)
{
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    bool ok = false;
    do {
        if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) != 0) break;
        out.resize(len + kTagLen);
        // One-shot: ciphertext → out[0..len), 16-byte tag → out[len..len+16).
        if (mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len,
                                      nonce, kNonceLen, nullptr, 0,
                                      plaintext, out.data(),
                                      kTagLen, out.data() + len) != 0) break;
        ok = true;
    } while (false);

    mbedtls_gcm_free(&gcm);
    if (!ok) out.clear();
    return ok;
}

bool aesGcmDecrypt(const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t* data, size_t len,
                   std::vector<uint8_t>& out)
{
    if (len < static_cast<size_t>(kTagLen)) return false;
    const size_t   ctLen = len - kTagLen;
    const uint8_t* tag   = data + ctLen;

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    bool ok = false;
    do {
        if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) != 0) break;
        out.resize(ctLen);
        // Returns MBEDTLS_ERR_GCM_AUTH_FAILED on tag mismatch (wrong key / tampered).
        if (mbedtls_gcm_auth_decrypt(&gcm, ctLen, nonce, kNonceLen,
                                     nullptr, 0, tag, kTagLen,
                                     data, out.data()) != 0) break;
        ok = true;
    } while (false);

    mbedtls_gcm_free(&gcm);
    if (!ok) out.clear();
    return ok;
}

} // namespace Hpak

#else // no crypto backend

namespace Hpak
{
bool cryptoAvailable() { return false; }
bool randomBytes(uint8_t*, size_t) { return false; }
bool aesGcmEncrypt(const uint8_t*, const uint8_t*, const uint8_t*, size_t,
                   std::vector<uint8_t>& out) { out.clear(); return false; }
bool aesGcmDecrypt(const uint8_t*, const uint8_t*, const uint8_t*, size_t,
                   std::vector<uint8_t>& out) { out.clear(); return false; }
} // namespace Hpak

#endif
