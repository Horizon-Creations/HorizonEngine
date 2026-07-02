#include <Hpak/Aes256Gcm.h>

#ifdef HE_HAVE_OPENSSL
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

#else // !HE_HAVE_OPENSSL — no crypto backend

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
