#pragma once

// ─── HorizonNet Layer 1 — authenticated, encrypted channel ───────────────────
// A decorator around any ITransport that adds a challenge-response handshake and
// AES-256-GCM frame encryption. Because it wraps the *interface* rather than
// extending TcpTransport, the same code path is exercised over LoopbackTransport
// in tests and over TCP in production.
//
// Threat model — unlike the hpak asset encryption (which only obfuscates shipped
// files against casual ripping), this IS a real security boundary: a collab
// session is reachable from the internet and carries the project's scene data.
// So:
//   • the join secret never travels over the wire — only an HMAC over nonces,
//   • replaying a captured handshake cannot establish a session (fresh nonces),
//   • every frame is authenticated (GCM tag), so tampering drops the link,
//   • a peer is never surfaced upward as Connected until it has authenticated.
//
// Handshake (host challenges, client responds) — protocol v2, with an ephemeral
// X25519 exchange so sessions have forward secrecy:
//   host → client   Challenge  [0x01][version][32 hostNonce][32 hostPub]
//   client → host   Response   [0x02][32 clientNonce][32 clientPub][32 mac]
//   host → client   Accept     [0x03]                        (or Reject [0x04])
//
//   transcript = hostNonce || clientNonce || hostPub || clientPub
//   mac        = HMAC(joinSecret, "HN-auth-v2" || transcript)
//   shared     = X25519(ourEphemeralPriv, peerPub)
//   prk        = HMAC(key = shared, "HN-key-v2" || transcript)
//   sessionKey = HMAC(key = prk,    joinSecret)
//
// Why this shape:
//   • The session key comes from the ephemeral exchange, and both private halves
//     are wiped once the handshake completes. Recording the traffic and learning
//     the join secret afterwards no longer decrypts it — the earlier v1 design,
//     where the key was a deterministic function of the secret and two public
//     nonces, did allow exactly that.
//   • The join secret is still mixed into the final key, so breaking the curve
//     alone is not enough either. Neither input suffices on its own.
//   • The mac authenticates the whole transcript *including both public keys*,
//     so a man in the middle cannot substitute its own ephemeral key: it cannot
//     produce a valid mac without the secret.
//   • Distinct domain-separation labels keep the wire-visible mac from revealing
//     anything about the session key.
//
// IMPORTANT: the join secret must be *high-entropy and machine-generated* (see
// generateJoinSecret). An observer can capture a challenge/response pair and
// brute-force a weak, human-chosen password offline.
//
// Data frames after the handshake: [8-byte counter BE][ciphertext || 16-byte tag]
// The 96-bit GCM nonce is (direction tag || counter), so it is unique per key —
// reuse would break GCM catastrophically. Counters must strictly increase, which
// also rejects replayed frames.

#include "Net/ITransport.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace HE::Net {

enum class HandshakeState : std::uint8_t {
    AwaitingChallenge,   // client: waiting for the host's challenge
    AwaitingResponse,    // host: challenge sent, waiting for the mac
    AwaitingAccept,      // client: mac sent, waiting for the verdict
    Established,         // authenticated; data frames flow
    Failed,              // rejected or tampered — connection is being dropped
};

class HE_NET_API SecureTransport final : public ITransport {
public:
    struct Config {
        // Shared join secret. Use generateJoinSecret() rather than a passphrase.
        std::string joinSecret;
        // Host challenges, Client responds. Anything else is rejected.
        NetRole role = NetRole::Client;
        // Refuse to establish a session when no crypto backend is available,
        // rather than silently falling back to plaintext over the internet.
        bool requireEncryption = true;
    };

    // Generate a high-entropy join secret (Crockford-style base32, ~128 bits).
    // This is the code a host shares out-of-band / via the session directory.
    static std::string generateJoinSecret();

    // Take ownership of `inner` and gate it behind authentication.
    static std::unique_ptr<SecureTransport> wrap(std::unique_ptr<ITransport> inner,
                                                 Config cfg);

    ~SecureTransport() override;

    SecureTransport(const SecureTransport&)            = delete;
    SecureTransport& operator=(const SecureTransport&) = delete;

    using ITransport::send;

    void        update() override;
    void        send(ConnectionId conn, const std::uint8_t* data,
                     std::size_t len, SendMode mode) override;
    bool        poll(NetEvent& out) override;
    void        disconnect(ConnectionId conn) override;
    std::size_t connectionCount() const override;

    // Underlying transport, for host-side queries such as boundPort().
    ITransport* inner() const { return m_inner.get(); }

    // True when frames are actually being encrypted (a crypto backend exists).
    bool encryptionActive() const { return m_cryptoAvailable; }

    // Handshake state of a peer — useful for diagnostics and tests.
    HandshakeState stateOf(ConnectionId conn) const;

    // A short one-way digest of the negotiated session key (never the key
    // itself). Two peers on the same connection compute the same value, so it
    // can be shown in the UI to confirm out-of-band that two users really are in
    // the same session. Empty when the peer is not established.
    std::string sessionFingerprint(ConnectionId conn) const;

private:
    SecureTransport() = default;

    struct Peer {
        HandshakeState state = HandshakeState::AwaitingChallenge;
        std::uint8_t   hostNonce[32]{};
        std::uint8_t   clientNonce[32]{};
        std::uint8_t   hostPub[32]{};
        std::uint8_t   clientPub[32]{};
        // Ephemeral private scalar. Wiped as soon as the shared secret is
        // computed — keeping it would defeat the point of the exchange.
        std::uint8_t   ephemeralPriv[32]{};
        std::uint8_t   sessionKey[32]{};
        std::uint64_t  sendCounter = 0;
        std::uint64_t  lastRecvCounter = 0;
    };

    void drainInner();
    void onInnerConnected(ConnectionId conn);
    void onInnerData(ConnectionId conn, const std::vector<std::uint8_t>& frame);
    void handleHandshake(ConnectionId conn, Peer& p,
                         const std::vector<std::uint8_t>& frame);
    void handleData(ConnectionId conn, Peer& p,
                    const std::vector<std::uint8_t>& frame);
    // Computes the transcript mac and the session key from the exchanged nonces
    // and public keys. Returns false when the ECDH step fails.
    bool deriveSecrets(Peer& p, std::uint8_t outMac[32]);
    void failPeer(ConnectionId conn, Peer& p);

    std::unique_ptr<ITransport>                 m_inner;
    Config                                      m_cfg;
    bool                                        m_cryptoAvailable = false;
    std::unordered_map<ConnectionId, Peer>      m_peers;
    std::vector<NetEvent>                       m_events;
};

} // namespace HE::Net
