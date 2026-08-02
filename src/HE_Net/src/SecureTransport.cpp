#include "Net/SecureTransport.h"

#include <Hpak/Aes256Gcm.h>
#include <Hpak/KeyDerivation.h>

#include <cstring>
#include <random>
#include <utility>

namespace HE::Net {
namespace {

// ─── Wire constants ──────────────────────────────────────────────────────────

constexpr std::uint8_t kMsgChallenge = 0x01;
constexpr std::uint8_t kMsgResponse  = 0x02;
constexpr std::uint8_t kMsgAccept    = 0x03;
constexpr std::uint8_t kMsgReject    = 0x04;

constexpr std::uint8_t kProtocolVersion = 1;
constexpr std::size_t  kNonceLen        = 32;
constexpr std::size_t  kMacLen          = 32;
constexpr std::size_t  kCounterLen      = 8;
constexpr std::size_t  kGcmTagLen       = 16;

// Domain-separation labels: the wire-visible mac and the secret session key are
// derived from the same join secret, so they must not be the same HMAC.
constexpr char kAuthLabel[] = "HN-auth-v1";
constexpr char kKeyLabel[]  = "HN-key-v1";

// Direction tags keep host→client and client→host nonces disjoint under one key.
constexpr std::uint32_t kDirHostToClient = 1;
constexpr std::uint32_t kDirClientToHost = 2;

// Fill with CSPRNG bytes, falling back to std::random_device when no crypto
// backend is compiled in (nonces must still be unpredictable).
void fillRandom(std::uint8_t* out, std::size_t n) {
    if (Hpak::randomBytes(out, n)) return;

    std::random_device rd;
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>(rd() & 0xFFu);
    }
}

// Comparison whose timing does not depend on where the first difference is —
// a byte-by-byte early return would leak the correct mac one byte at a time.
bool constantTimeEqual(const std::uint8_t* a, const std::uint8_t* b, std::size_t n) {
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < n; ++i) diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

void writeU64BE(std::uint8_t* out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>((v >> (56 - i * 8)) & 0xFF);
    }
}

std::uint64_t readU64BE(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

void makeGcmNonce(std::uint8_t out[12], std::uint32_t direction, std::uint64_t counter) {
    out[0] = static_cast<std::uint8_t>((direction >> 24) & 0xFF);
    out[1] = static_cast<std::uint8_t>((direction >> 16) & 0xFF);
    out[2] = static_cast<std::uint8_t>((direction >>  8) & 0xFF);
    out[3] = static_cast<std::uint8_t>( direction        & 0xFF);
    writeU64BE(out + 4, counter);
}

} // namespace

// ─── Construction ────────────────────────────────────────────────────────────

std::string SecureTransport::generateJoinSecret() {
    // Crockford-ish base32 without I/L/O/U — unambiguous when read aloud or
    // retyped from a screen share. 26 chars ≈ 128 bits.
    static constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    std::uint8_t raw[16];
    fillRandom(raw, sizeof(raw));

    std::string out;
    out.reserve(26);
    for (std::size_t i = 0; i < 26; ++i) {
        // 5 bits per character, walked across the 128-bit buffer.
        const std::size_t bit  = i * 5;
        const std::size_t byte = bit / 8;
        const int         off  = static_cast<int>(bit % 8);
        std::uint16_t window = static_cast<std::uint16_t>(raw[byte] << 8);
        if (byte + 1 < sizeof(raw)) window |= raw[byte + 1];
        const auto idx = static_cast<std::uint8_t>((window >> (11 - off)) & 0x1F);
        out.push_back(kAlphabet[idx]);
    }
    return out;
}

std::unique_ptr<SecureTransport> SecureTransport::wrap(
    std::unique_ptr<ITransport> inner, Config cfg) {
    if (!inner) return nullptr;
    if (cfg.joinSecret.empty()) return nullptr;
    if (cfg.role != NetRole::Host && cfg.role != NetRole::Client) return nullptr;

    const bool crypto = Hpak::cryptoAvailable();
    // Refuse rather than silently downgrade to plaintext on a public network.
    if (cfg.requireEncryption && !crypto) return nullptr;

    std::unique_ptr<SecureTransport> t(new SecureTransport());
    t->m_inner           = std::move(inner);
    t->m_cfg             = std::move(cfg);
    t->m_cryptoAvailable = crypto;
    return t;
}

SecureTransport::~SecureTransport() = default;

// ─── Pump ────────────────────────────────────────────────────────────────────

void SecureTransport::update() {
    if (m_inner) m_inner->update();
    drainInner();
}

void SecureTransport::drainInner() {
    if (!m_inner) return;

    NetEvent ev;
    while (m_inner->poll(ev)) {
        switch (ev.type) {
        case NetEventType::Connected:
            onInnerConnected(ev.conn);
            break;

        case NetEventType::Disconnected: {
            const auto it = m_peers.find(ev.conn);
            // Only surface the drop if the peer had been announced upward;
            // a link that died mid-handshake was never visible.
            const bool wasVisible =
                (it != m_peers.end() && it->second.state == HandshakeState::Established);
            if (it != m_peers.end()) m_peers.erase(it);
            if (wasVisible) {
                m_events.push_back(NetEvent{ NetEventType::Disconnected, ev.conn, {} });
            }
            break;
        }

        case NetEventType::Data:
            onInnerData(ev.conn, ev.data);
            break;
        }
    }
}

void SecureTransport::onInnerConnected(ConnectionId conn) {
    Peer p;
    if (m_cfg.role == NetRole::Host) {
        // Host drives: emit a fresh challenge immediately.
        fillRandom(p.hostNonce, kNonceLen);
        p.state = HandshakeState::AwaitingResponse;

        std::vector<std::uint8_t> msg;
        msg.reserve(2 + kNonceLen);
        msg.push_back(kMsgChallenge);
        msg.push_back(kProtocolVersion);
        msg.insert(msg.end(), p.hostNonce, p.hostNonce + kNonceLen);

        m_peers[conn] = p;
        m_inner->send(conn, msg.data(), msg.size(), SendMode::ReliableOrdered);
    } else {
        p.state = HandshakeState::AwaitingChallenge;
        m_peers[conn] = p;
    }
    // Note: nothing is surfaced upward yet — the peer is not authenticated.
}

void SecureTransport::onInnerData(ConnectionId conn,
                                  const std::vector<std::uint8_t>& frame) {
    const auto it = m_peers.find(conn);
    if (it == m_peers.end()) return;   // unknown / already failed
    Peer& p = it->second;

    if (p.state == HandshakeState::Established) handleData(conn, p, frame);
    else                                        handleHandshake(conn, p, frame);
}

// ─── Handshake ───────────────────────────────────────────────────────────────

void SecureTransport::deriveSecrets(Peer& p, std::uint8_t outMac[32]) {
    // Both derivations bind to *both* nonces, so neither side can pin the
    // transcript to a value it chose alone.
    std::uint8_t authMsg[sizeof(kAuthLabel) - 1 + kNonceLen * 2];
    std::memcpy(authMsg, kAuthLabel, sizeof(kAuthLabel) - 1);
    std::memcpy(authMsg + sizeof(kAuthLabel) - 1, p.hostNonce, kNonceLen);
    std::memcpy(authMsg + sizeof(kAuthLabel) - 1 + kNonceLen, p.clientNonce, kNonceLen);

    std::uint8_t keyMsg[sizeof(kKeyLabel) - 1 + kNonceLen * 2];
    std::memcpy(keyMsg, kKeyLabel, sizeof(kKeyLabel) - 1);
    std::memcpy(keyMsg + sizeof(kKeyLabel) - 1, p.hostNonce, kNonceLen);
    std::memcpy(keyMsg + sizeof(kKeyLabel) - 1 + kNonceLen, p.clientNonce, kNonceLen);

    const auto* secret = reinterpret_cast<const std::uint8_t*>(m_cfg.joinSecret.data());
    const std::size_t secretLen = m_cfg.joinSecret.size();

    KeyDerivation::hmac(secret, secretLen, authMsg, sizeof(authMsg), outMac);
    KeyDerivation::hmac(secret, secretLen, keyMsg,  sizeof(keyMsg),  p.sessionKey);
}

void SecureTransport::handleHandshake(ConnectionId conn, Peer& p,
                                      const std::vector<std::uint8_t>& frame) {
    if (frame.empty()) { failPeer(conn, p); return; }
    const std::uint8_t msg = frame[0];

    // ── Client: challenge received → answer with the mac ──
    if (msg == kMsgChallenge && m_cfg.role == NetRole::Client &&
        p.state == HandshakeState::AwaitingChallenge) {
        if (frame.size() != 2 + kNonceLen)      { failPeer(conn, p); return; }
        if (frame[1] != kProtocolVersion)       { failPeer(conn, p); return; }

        std::memcpy(p.hostNonce, frame.data() + 2, kNonceLen);
        fillRandom(p.clientNonce, kNonceLen);

        std::uint8_t mac[kMacLen];
        deriveSecrets(p, mac);

        std::vector<std::uint8_t> out;
        out.reserve(1 + kNonceLen + kMacLen);
        out.push_back(kMsgResponse);
        out.insert(out.end(), p.clientNonce, p.clientNonce + kNonceLen);
        out.insert(out.end(), mac, mac + kMacLen);

        p.state = HandshakeState::AwaitingAccept;
        m_inner->send(conn, out.data(), out.size(), SendMode::ReliableOrdered);
        return;
    }

    // ── Host: response received → verify and accept or reject ──
    if (msg == kMsgResponse && m_cfg.role == NetRole::Host &&
        p.state == HandshakeState::AwaitingResponse) {
        if (frame.size() != 1 + kNonceLen + kMacLen) { failPeer(conn, p); return; }

        std::memcpy(p.clientNonce, frame.data() + 1, kNonceLen);

        std::uint8_t expected[kMacLen];
        deriveSecrets(p, expected);

        const std::uint8_t* got = frame.data() + 1 + kNonceLen;
        if (!constantTimeEqual(expected, got, kMacLen)) {
            const std::uint8_t reject[2] = { kMsgReject, 0x01 };
            m_inner->send(conn, reject, sizeof(reject), SendMode::ReliableOrdered);
            failPeer(conn, p);
            return;
        }

        const std::uint8_t accept[1] = { kMsgAccept };
        m_inner->send(conn, accept, sizeof(accept), SendMode::ReliableOrdered);

        p.state = HandshakeState::Established;
        m_events.push_back(NetEvent{ NetEventType::Connected, conn, {} });
        return;
    }

    // ── Client: verdict ──
    if (msg == kMsgAccept && m_cfg.role == NetRole::Client &&
        p.state == HandshakeState::AwaitingAccept) {
        p.state = HandshakeState::Established;
        m_events.push_back(NetEvent{ NetEventType::Connected, conn, {} });
        return;
    }
    if (msg == kMsgReject) { failPeer(conn, p); return; }

    // Anything else is out-of-order or malformed for this state.
    failPeer(conn, p);
}

void SecureTransport::failPeer(ConnectionId conn, Peer& p) {
    p.state = HandshakeState::Failed;
    if (m_inner) m_inner->disconnect(conn);
    m_peers.erase(conn);
    // Nothing is emitted upward: an unauthenticated peer was never visible, so
    // there is no Connected for this Disconnected to pair with.
}

// ─── Data frames ─────────────────────────────────────────────────────────────

void SecureTransport::handleData(ConnectionId conn, Peer& p,
                                 const std::vector<std::uint8_t>& frame) {
    if (!m_cryptoAvailable) {
        // Plaintext mode (only reachable with requireEncryption=false).
        m_events.push_back(NetEvent{ NetEventType::Data, conn, frame });
        return;
    }

    if (frame.size() < kCounterLen + kGcmTagLen) { failPeer(conn, p); return; }

    const std::uint64_t counter = readU64BE(frame.data());
    // Strictly increasing: rejects replayed and reordered frames.
    if (counter <= p.lastRecvCounter) { failPeer(conn, p); return; }

    // The peer's direction tag is the opposite of ours.
    const std::uint32_t dir = (m_cfg.role == NetRole::Host) ? kDirClientToHost
                                                            : kDirHostToClient;
    std::uint8_t nonce[12];
    makeGcmNonce(nonce, dir, counter);

    std::vector<std::uint8_t> plain;
    const bool ok = Hpak::aesGcmDecrypt(p.sessionKey, nonce,
                                        frame.data() + kCounterLen,
                                        frame.size() - kCounterLen, plain);
    if (!ok) {
        // Auth-tag mismatch — tampered, corrupt, or wrong key. Drop the link
        // rather than handing unverified bytes to the application.
        failPeer(conn, p);
        return;
    }

    p.lastRecvCounter = counter;
    m_events.push_back(NetEvent{ NetEventType::Data, conn, std::move(plain) });
}

// ─── ITransport ──────────────────────────────────────────────────────────────

void SecureTransport::send(ConnectionId conn, const std::uint8_t* data,
                           std::size_t len, SendMode mode) {
    const auto it = m_peers.find(conn);
    // Refuse to emit anything before the peer is authenticated.
    if (it == m_peers.end() || it->second.state != HandshakeState::Established) return;
    Peer& p = it->second;

    if (!m_cryptoAvailable) {
        m_inner->send(conn, data, len, mode);
        return;
    }

    const std::uint64_t counter = ++p.sendCounter;
    const std::uint32_t dir = (m_cfg.role == NetRole::Host) ? kDirHostToClient
                                                            : kDirClientToHost;
    std::uint8_t nonce[12];
    makeGcmNonce(nonce, dir, counter);

    std::vector<std::uint8_t> cipher;
    if (!Hpak::aesGcmEncrypt(p.sessionKey, nonce, data, len, cipher)) return;

    std::vector<std::uint8_t> out;
    out.reserve(kCounterLen + cipher.size());
    out.resize(kCounterLen);
    writeU64BE(out.data(), counter);
    out.insert(out.end(), cipher.begin(), cipher.end());

    m_inner->send(conn, out.data(), out.size(), mode);
}

bool SecureTransport::poll(NetEvent& out) {
    // Also drain inward here, so the class behaves correctly regardless of
    // whether the caller pumps update() first.
    if (m_events.empty()) drainInner();
    if (m_events.empty()) return false;

    out = std::move(m_events.front());
    m_events.erase(m_events.begin());
    return true;
}

void SecureTransport::disconnect(ConnectionId conn) {
    m_peers.erase(conn);
    if (m_inner) m_inner->disconnect(conn);
}

std::size_t SecureTransport::connectionCount() const {
    std::size_t n = 0;
    for (const auto& [id, p] : m_peers) {
        if (p.state == HandshakeState::Established) ++n;
    }
    return n;
}

HandshakeState SecureTransport::stateOf(ConnectionId conn) const {
    const auto it = m_peers.find(conn);
    return (it == m_peers.end()) ? HandshakeState::Failed : it->second.state;
}

} // namespace HE::Net
