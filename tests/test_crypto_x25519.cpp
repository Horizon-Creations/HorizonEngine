#include "doctest.h"

#include <Crypto/X25519.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

using namespace HE::Crypto;

namespace {

std::array<std::uint8_t, 32> fromHex(const std::string& hex) {
    std::array<std::uint8_t, 32> out{};
    for (std::size_t i = 0; i < 32; ++i) {
        out[i] = static_cast<std::uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    }
    return out;
}

std::string toHex(const std::uint8_t* data, std::size_t n) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0F]);
    }
    return out;
}

// RFC 7748 §6.1 — the canonical X25519 Diffie-Hellman test vector.
constexpr const char* kAlicePriv  = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
constexpr const char* kAlicePub   = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
constexpr const char* kBobPriv    = "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
constexpr const char* kBobPub     = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
constexpr const char* kSharedSecret = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";

} // namespace

// ─── Known-answer tests against the spec ─────────────────────────────────────
// These pin the implementation to RFC 7748 rather than merely checking that the
// two sides agree with each other — a consistently wrong implementation would
// pass a self-consistency test. They also mean the OpenSSL and mbedTLS backends
// are each measured against the same external reference.

TEST_CASE("X25519: public keys match the RFC 7748 vector")
{
    if (!x25519Available()) {
        WARN("no crypto backend — X25519 unavailable, forward secrecy impossible");
        return;
    }

    const auto alicePriv = fromHex(kAlicePriv);
    const auto bobPriv   = fromHex(kBobPriv);

    std::uint8_t pub[32];
    REQUIRE(x25519PublicFromPrivate(alicePriv.data(), pub));
    CHECK(toHex(pub, 32) == kAlicePub);

    REQUIRE(x25519PublicFromPrivate(bobPriv.data(), pub));
    CHECK(toHex(pub, 32) == kBobPub);
}

TEST_CASE("X25519: shared secret matches the RFC 7748 vector from both sides")
{
    if (!x25519Available()) return;

    const auto alicePriv = fromHex(kAlicePriv);
    const auto alicePub  = fromHex(kAlicePub);
    const auto bobPriv   = fromHex(kBobPriv);
    const auto bobPub    = fromHex(kBobPub);

    std::uint8_t fromAlice[32], fromBob[32];
    REQUIRE(x25519SharedSecret(alicePriv.data(), bobPub.data(), fromAlice));
    REQUIRE(x25519SharedSecret(bobPriv.data(), alicePub.data(), fromBob));

    CHECK(toHex(fromAlice, 32) == kSharedSecret);
    CHECK(toHex(fromBob, 32) == kSharedSecret);   // both directions agree
}

// ─── Generated keys ──────────────────────────────────────────────────────────

TEST_CASE("X25519: freshly generated keypairs agree on a shared secret")
{
    if (!x25519Available()) return;

    std::uint8_t aPriv[32], aPub[32], bPriv[32], bPub[32];
    REQUIRE(x25519GenerateKeypair(aPriv, aPub));
    REQUIRE(x25519GenerateKeypair(bPriv, bPub));

    std::uint8_t s1[32], s2[32];
    REQUIRE(x25519SharedSecret(aPriv, bPub, s1));
    REQUIRE(x25519SharedSecret(bPriv, aPub, s2));
    CHECK(std::memcmp(s1, s2, 32) == 0);

    // The exported public key must belong to the exported private key, or the
    // handshake would derive mismatched secrets.
    std::uint8_t derived[32];
    REQUIRE(x25519PublicFromPrivate(aPriv, derived));
    CHECK(std::memcmp(derived, aPub, 32) == 0);
}

TEST_CASE("X25519: each keypair is distinct")
{
    if (!x25519Available()) return;

    // Ephemeral keys are the whole point: repeated keys would destroy forward
    // secrecy while everything still appeared to work.
    std::uint8_t p1[32], k1[32], p2[32], k2[32];
    REQUIRE(x25519GenerateKeypair(p1, k1));
    REQUIRE(x25519GenerateKeypair(p2, k2));

    CHECK(std::memcmp(p1, p2, 32) != 0);
    CHECK(std::memcmp(k1, k2, 32) != 0);
}

TEST_CASE("X25519: a different peer key yields a different secret")
{
    if (!x25519Available()) return;

    std::uint8_t aPriv[32], aPub[32], bPriv[32], bPub[32], cPriv[32], cPub[32];
    REQUIRE(x25519GenerateKeypair(aPriv, aPub));
    REQUIRE(x25519GenerateKeypair(bPriv, bPub));
    REQUIRE(x25519GenerateKeypair(cPriv, cPub));

    std::uint8_t withB[32], withC[32];
    REQUIRE(x25519SharedSecret(aPriv, bPub, withB));
    REQUIRE(x25519SharedSecret(aPriv, cPub, withC));
    CHECK(std::memcmp(withB, withC, 32) != 0);
}
