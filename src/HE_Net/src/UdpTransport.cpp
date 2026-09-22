#include "Net/UdpTransport.h"

#include "NetLog.h"

#include <Hpak/Aes256Gcm.h>
#include <Hpak/KeyDerivation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <utility>

namespace HE::Net {
namespace {

// ─── Serial-number arithmetic (RFC 1982) for the u16 sequence spaces ─────────
// `seq` wraps every 65536 datagrams — under a minute at snapshot rate — so a
// plain `<` would call the first packet after the wrap "older" than everything
// before it and drop it as a duplicate.

bool serialGreater(std::uint16_t a, std::uint16_t b) {
    return (a > b && a - b <= 32768) || (a < b && b - a > 32768);
}
bool serialLess(std::uint16_t a, std::uint16_t b) { return serialGreater(b, a); }

// ─── Byte helpers, big-endian like every other HorizonNet wire format ────────

void put8(std::vector<std::uint8_t>& v, std::uint8_t x)   { v.push_back(x); }
void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
}
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<std::uint8_t>((x >> (i * 8)) & 0xFF));
}
void put64(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int i = 7; i >= 0; --i) v.push_back(static_cast<std::uint8_t>((x >> (i * 8)) & 0xFF));
}
std::uint16_t get16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
std::uint32_t get32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) <<  8) |  static_cast<std::uint32_t>(p[3]);
}
std::uint64_t get64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

std::uint64_t steadyNowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void fillRandom(std::uint8_t* out, std::size_t n) {
    if (Hpak::randomBytes(out, n)) return;
    std::random_device rd;
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::uint8_t>(rd() & 0xFFu);
}

std::uint64_t randomU64() {
    std::uint8_t b[8];
    fillRandom(b, sizeof(b));
    return get64(b);
}

std::string addressKey(const std::string& host, std::uint16_t port) {
    return host + "|" + std::to_string(port);
}

// Link-local IPv6 answers carry a scope ("fe80::1%en0") that recvfrom does not
// report back, so the two spellings of one peer have to be compared without it.
std::string stripScope(const std::string& host) {
    const std::size_t at = host.find('%');
    return at == std::string::npos ? host : host.substr(0, at);
}

// Client-side connect retry cadence. The Connect is unreliable by nature (there
// is no connection to make it reliable yet), so it is simply repeated.
constexpr std::uint32_t kConnectRetryMs = 250;
// Cookie secret lifetime; two are valid at once so a client that received its
// challenge just before the rotation still gets in.
constexpr std::uint64_t kSecretRotateMs = 60'000;
// Disconnect is repeated this often, this many times, unacknowledged.
constexpr std::uint32_t kFarewellGapMs = 50;
constexpr int           kFarewellRepeats = 3;
// Partial reassemblies a peer may hold open at once. A hostile peer that
// starts messages and never finishes them is bounded by count and, below,
// by bytes.
constexpr std::size_t   kMaxReassemblies = 32;
// Initial RTO before the first sample.
constexpr float         kRtoInitialMs = 300.f;

} // namespace

// ─── SeqWindow ───────────────────────────────────────────────────────────────

bool UdpTransport::SeqWindow::accept(std::uint16_t seq) {
    if (!any) {
        any = true;
        highest = seq;
        std::memset(slotSet, 0, sizeof(slotSet));
        slotSeq[seq % kSlots] = seq;
        slotSet[seq % kSlots] = true;
        return true;
    }
    if (serialGreater(seq, highest)) {
        // Everything between the old and the new highest is unseen: clear the
        // slots those values would use, or a stale entry from 256 packets ago
        // would masquerade as "seen".
        const std::uint16_t d = static_cast<std::uint16_t>(seq - highest);
        if (d >= kSlots) {
            std::memset(slotSet, 0, sizeof(slotSet));
        } else {
            for (std::uint16_t i = 1; i < d; ++i) {
                slotSet[static_cast<std::uint16_t>(highest + i) % kSlots] = false;
            }
        }
        highest = seq;
        slotSeq[seq % kSlots] = seq;
        slotSet[seq % kSlots] = true;
        return true;
    }
    const std::uint16_t back = static_cast<std::uint16_t>(highest - seq);
    if (back >= kSlots) return false;                       // too old to tell: treat as duplicate
    const std::uint32_t slot = seq % kSlots;
    if (slotSet[slot] && slotSeq[slot] == seq) return false; // duplicate
    slotSeq[slot] = seq;
    slotSet[slot] = true;
    return true;
}

bool UdpTransport::SeqWindow::seen(std::uint16_t seq) const {
    if (!any) return false;
    if (serialGreater(seq, highest)) return false;
    const std::uint16_t back = static_cast<std::uint16_t>(highest - seq);
    if (back >= kSlots) return false;
    const std::uint32_t slot = seq % kSlots;
    return slotSet[slot] && slotSeq[slot] == seq;
}

// ─── Construction ────────────────────────────────────────────────────────────

std::unique_ptr<UdpTransport> UdpTransport::listen(std::uint16_t port, Config cfg) {
    SocketHandle s = socketCreateUdpDualStack(port);
    if (s == kInvalidSocket) {
        HE_LOG_ERROR(Net, "UDP listen on port %u failed", static_cast<unsigned>(port));
        return nullptr;
    }
    std::unique_ptr<UdpTransport> t(new UdpTransport());
    t->m_socket    = s;
    t->m_boundPort = socketBoundPort(s);
    t->m_isHost    = true;
    t->m_cfg       = cfg;
    t->m_clock     = &steadyNowMs;
    fillRandom(t->m_secret[0], 32);
    std::memcpy(t->m_secret[1], t->m_secret[0], 32);
    t->m_secretRotatedMs = t->m_clock();
    HE_LOG_INFO(Net, "UDP listening on port %u (requested %u)",
                static_cast<unsigned>(t->m_boundPort), static_cast<unsigned>(port));
    return t;
}

std::unique_ptr<UdpTransport> UdpTransport::connect(const std::string& host,
                                                    std::uint16_t port, Config cfg) {
    std::string numeric;
    bool isV6 = false;
    if (!socketResolveUdpAddress(host, port, numeric, isV6)) {
        HE_LOG_ERROR(Net, "UDP connect to %s:%u failed to start (resolve error)",
                     host.c_str(), static_cast<unsigned>(port));
        return nullptr;
    }
    SocketHandle s = socketCreateUdpFor(isV6);
    if (s == kInvalidSocket) {
        HE_LOG_ERROR(Net, "UDP connect to %s:%u failed to start (socket error)",
                     host.c_str(), static_cast<unsigned>(port));
        return nullptr;
    }

    std::unique_ptr<UdpTransport> t(new UdpTransport());
    t->m_socket    = s;
    t->m_boundPort = socketBoundPort(s);
    t->m_isHost    = false;
    t->m_cfg       = cfg;
    t->m_clock     = &steadyNowMs;

    const ConnectionId id = t->m_nextId++;
    Peer p;
    p.host  = numeric;
    p.port  = port;
    p.state = PeerState::Connecting;
    p.clientNonce    = randomU64();
    p.connectStartMs = t->m_clock();
    p.lastRecvMs     = p.connectStartMs;
    t->m_peers.emplace(id, std::move(p));
    t->m_byAddress[addressKey(stripScope(numeric), port)] = id;

    HE_LOG_INFO(Net, "UDP connecting to %s:%u (conn %llu, %s)",
                host.c_str(), static_cast<unsigned>(port),
                static_cast<unsigned long long>(id), isV6 ? "IPv6" : "IPv4");

    // First Connect leaves now; update() repeats it until a Challenge arrives.
    Peer& live = t->m_peers.at(id);
    std::vector<std::uint8_t> body;
    put64(body, live.clientNonce);
    t->emitDatagram(live, FlagConnect, ChannelUnreliable, body.data(), body.size());
    live.nextConnectMs = live.connectStartMs + kConnectRetryMs;
    return t;
}

// The defaulted overloads live here because Config's member initializers are
// not usable as a default argument inside the class that declares it.
std::unique_ptr<UdpTransport> UdpTransport::listen(std::uint16_t port) {
    return listen(port, Config{});
}
std::unique_ptr<UdpTransport> UdpTransport::connect(const std::string& host,
                                                    std::uint16_t port) {
    return connect(host, port, Config{});
}

UdpTransport::~UdpTransport() {
    if (m_socket != kInvalidSocket) {
        socketClose(m_socket);
        m_socket = kInvalidSocket;
    }
}

void UdpTransport::setClock(ClockFn clock) {
    m_clock = clock ? std::move(clock) : ClockFn(&steadyNowMs);
    // A test that swaps the clock before the first pump would otherwise see
    // steady-clock timestamps compared against its own small numbers.
    const std::uint64_t now = m_clock();
    m_secretRotatedMs = now;
    for (auto& [id, p] : m_peers) {
        p.connectStartMs = now;
        p.nextConnectMs  = now + kConnectRetryMs;
        p.lastRecvMs     = now;
        p.lastSendMs     = now;
        for (Packet& pk : p.inFlight) pk.sentMs = now;
    }
}

void UdpTransport::setTestDropFn(TestDatagramFn fn)      { m_testDrop = std::move(fn); }
void UdpTransport::setTestDuplicateFn(TestDatagramFn fn) { m_testDuplicate = std::move(fn); }

std::size_t UdpTransport::maxUnreliableMessage() const { return m_cfg.mtuPayload; }
std::size_t UdpTransport::maxReliableMessage() const {
    const std::size_t perFragment = m_cfg.mtuPayload - kMsgSeqSize - kFragmentHeaderSize;
    return perFragment * kMaxFragments;
}

// ─── Cookies ─────────────────────────────────────────────────────────────────

std::uint64_t UdpTransport::makeCookie(const std::uint8_t secret[32], const std::string& host,
                                       std::uint16_t port, std::uint64_t nonce) const {
    // HMAC over (address, port, nonce): only the true owner of the address can
    // receive the value, and a change of nonce or port invalidates it.
    std::vector<std::uint8_t> msg(host.begin(), host.end());
    put16(msg, port);
    put64(msg, nonce);
    std::uint8_t digest[32];
    KeyDerivation::hmac(secret, 32, msg.data(), msg.size(), digest);
    return get64(digest);
}

bool UdpTransport::cookieValid(const std::string& host, std::uint16_t port,
                               std::uint64_t nonce, std::uint64_t cookie) const {
    // Both the current and the previous secret count, so a rotation between
    // Challenge and the client's reply does not lock it out.
    return cookie == makeCookie(m_secret[0], host, port, nonce) ||
           cookie == makeCookie(m_secret[1], host, port, nonce);
}

void UdpTransport::rotateSecretIfDue() {
    if (!m_isHost) return;
    const std::uint64_t now = m_clock();
    if (now - m_secretRotatedMs < kSecretRotateMs) return;
    std::memcpy(m_secret[1], m_secret[0], 32);
    fillRandom(m_secret[0], 32);
    m_secretRotatedMs = now;
}

// ─── Raw send ────────────────────────────────────────────────────────────────

void UdpTransport::rawSend(const std::string& host, std::uint16_t port,
                           const std::uint8_t* datagram, std::size_t len) {
    if (m_testDrop && m_testDrop(datagram, len)) {
        // Counted as sent: from the reliability layer's point of view it left.
        m_stats.datagramsSent++;
        m_stats.bytesSent += len;
        return;
    }
    const int copies = (m_testDuplicate && m_testDuplicate(datagram, len)) ? 2 : 1;
    for (int i = 0; i < copies; ++i) {
        std::size_t sent = 0;
        const SocketResult rc = socketSendTo(m_socket, datagram, len, host, port, sent);
        if (rc != SocketResult::Ok) {
            // A full send buffer or a transient error loses this datagram
            // exactly as the network would; reliable content is resent.
            HE_LOG_TRACE(Net, "UDP send of %s to %s:%u did not go out (%s)",
                         detail::logBytes(len).c_str(), host.c_str(),
                         static_cast<unsigned>(port),
                         rc == SocketResult::WouldBlock ? "would block" : socketErrorText().c_str());
        }
        m_stats.datagramsSent++;
        m_stats.bytesSent += len;
    }
}

std::uint16_t UdpTransport::emitDatagram(Peer& p, std::uint8_t flags, std::uint8_t channel,
                                         const std::uint8_t* body, std::size_t len,
                                         const std::uint16_t* msgSeq,
                                         const Packet* fragment) {
    const std::uint16_t seq = p.nextSeq++;

    // Acks ride in every datagram: highest seq received, and the 32 before it.
    std::uint16_t ack     = 0;
    std::uint32_t ackBits = 0;
    if (p.recvSeq.any) {
        ack = p.recvSeq.highest;
        for (std::uint32_t i = 0; i < 32; ++i) {
            if (p.recvSeq.seen(static_cast<std::uint16_t>(ack - 1 - i))) ackBits |= (1u << i);
        }
    }

    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + kMsgSeqSize + kFragmentHeaderSize + len);
    put16(out, kMagic);
    put8(out, kProtocolVersion);
    put8(out, flags | (fragment ? FlagFragment : 0));
    put8(out, channel);
    put16(out, seq);
    put16(out, ack);
    put32(out, ackBits);
    if (msgSeq) put16(out, *msgSeq);
    if (fragment) {
        put16(out, fragment->fragId);
        put8(out, fragment->fragIndex);
        put8(out, fragment->fragCount);
    }
    if (len) out.insert(out.end(), body, body + len);

    rawSend(p.host, p.port, out.data(), out.size());
    p.lastSendMs = m_clock();
    p.ackPending = false;
    p.datagramsSinceAck = 0;
    return seq;
}

void UdpTransport::sendControl(const std::string& host, std::uint16_t port, std::uint8_t flags,
                               const std::uint8_t* body, std::size_t len) {
    // Stateless: no peer, so no seq and no acks. Used for Challenge and Reject
    // before a peer exists, and for Disconnect repeats after one is gone.
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + len);
    put16(out, kMagic);
    put8(out, kProtocolVersion);
    put8(out, flags);
    put8(out, ChannelUnreliable);
    put16(out, 0);
    put16(out, 0);
    put32(out, 0);
    if (len) out.insert(out.end(), body, body + len);
    rawSend(host, port, out.data(), out.size());
}

// ─── Pump ────────────────────────────────────────────────────────────────────

void UdpTransport::update() {
    if (m_socket == kInvalidSocket) return;
    rotateSecretIfDue();
    receiveAll();

    const std::uint64_t now = m_clock();

    // Ids first: handlers below may drop peers.
    std::vector<ConnectionId> ids;
    ids.reserve(m_peers.size());
    for (const auto& [id, p] : m_peers) ids.push_back(id);

    for (const ConnectionId id : ids) {
        auto it = m_peers.find(id);
        if (it == m_peers.end()) continue;
        Peer& p = it->second;

        if (p.state != PeerState::Established) {
            // Client handshake: repeat the Connect until answered, give up
            // after the timeout. The host's reply is not reliable either, so
            // the repeat covers a lost Challenge and a lost Accept alike.
            if (now - p.connectStartMs >= m_cfg.timeoutMs) {
                HE_LOG_WARN(Net, "UDP connect to %s:%u timed out after %u ms (conn %llu) — "
                                 "no host there, or blocked en route",
                            p.host.c_str(), static_cast<unsigned>(p.port),
                            m_cfg.timeoutMs, static_cast<unsigned long long>(id));
                dropPeer(id, /*notify=*/true, "connect timed out");
                continue;
            }
            if (now >= p.nextConnectMs) {
                std::vector<std::uint8_t> body;
                put64(body, p.clientNonce);
                if (p.state == PeerState::Cookie) put64(body, p.cookie);
                emitDatagram(p, FlagConnect, ChannelUnreliable, body.data(), body.size());
                p.nextConnectMs = now + kConnectRetryMs;
            }
            continue;
        }

        if (now - p.lastRecvMs >= m_cfg.timeoutMs) {
            HE_LOG_INFO(Net, "UDP peer %llu timed out (%u ms without a datagram)",
                        static_cast<unsigned long long>(id), m_cfg.timeoutMs);
            dropPeer(id, /*notify=*/true, "timed out");
            continue;
        }

        serviceReliability(id, p);
        if (m_peers.find(id) == m_peers.end()) continue;   // dropped while servicing
        fillWindow(id, p);
        if (m_peers.find(id) == m_peers.end()) continue;

        // Half-built messages whose remaining fragments never came are
        // abandoned after a timeout's worth of waiting; the sender would have
        // been retransmitting them the whole time, so by then it is gone.
        for (auto rit = p.reassembly.begin(); rit != p.reassembly.end();) {
            if (now - rit->second.startedMs >= m_cfg.timeoutMs) {
                p.reassemblyBytes -= rit->second.bytes;
                rit = p.reassembly.erase(rit);
            } else {
                ++rit;
            }
        }

        // Silence handling: an ack for received reliable data goes out on its
        // own after kAckDelayMs when nothing else carried it; otherwise a
        // keepalive after keepAliveMs so the peer's timeout never fires on a
        // link that is merely quiet.
        if (p.ackPending && now - p.lastSendMs >= kAckDelayMs) {
            emitDatagram(p, FlagAckOnly, ChannelUnreliable, nullptr, 0);
        } else if (now - p.lastSendMs >= m_cfg.keepAliveMs) {
            emitDatagram(p, FlagKeepAlive, ChannelUnreliable, nullptr, 0);
        }
    }

    serviceFarewells();
}

void UdpTransport::receiveAll() {
    // Large enough for any datagram the socket can hand over, so an oversized
    // stray is read whole (and then rejected) rather than truncated and
    // misparsed.
    static thread_local std::vector<std::uint8_t> buf(65536);
    for (;;) {
        std::size_t   got = 0;
        std::string   fromHost;
        std::uint16_t fromPort = 0;
        const SocketResult rc = socketRecvFrom(m_socket, buf.data(), buf.size(), got,
                                               fromHost, fromPort);
        if (rc == SocketResult::WouldBlock) break;
        if (rc != SocketResult::Ok) {
            // Not the end of the socket. On Windows an ICMP "port unreachable"
            // for an earlier send surfaces here as an error on the NEXT read,
            // and treating that as fatal would kill a host every time a client
            // quit. Stop reading for this tick; the next update() tries again.
            HE_LOG_TRACE(Net, "UDP receive reported an error this tick — %s",
                         socketErrorText().c_str());
            break;
        }
        handleDatagram(fromHost, fromPort, buf.data(), got);
    }
}

// ─── Receive ─────────────────────────────────────────────────────────────────

ConnectionId UdpTransport::findPeer(const std::string& host, std::uint16_t port) const {
    const auto it = m_byAddress.find(addressKey(stripScope(host), port));
    return it == m_byAddress.end() ? kInvalidConnection : it->second;
}

void UdpTransport::handleDatagram(const std::string& host, std::uint16_t port,
                                  const std::uint8_t* data, std::size_t len) {
    if (len < kHeaderSize) return;
    if (get16(data) != kMagic) return;   // not ours: silently ignored, like every stray
    m_stats.datagramsReceived++;
    m_stats.bytesReceived += len;

    const std::uint8_t  version = data[2];
    const std::uint8_t  flags   = data[3];
    const std::uint8_t  channel = data[4];
    const std::uint16_t seq     = get16(data + 5);
    const std::uint16_t ack     = get16(data + 7);
    const std::uint32_t ackBits = get32(data + 9);
    const std::uint8_t* body    = data + kHeaderSize;
    const std::size_t   blen    = len - kHeaderSize;

    if (flags & FlagConnect) {
        if (m_isHost) handleConnect(host, port, version, body, blen);
        return;
    }

    const ConnectionId id = findPeer(host, port);
    // A datagram from an address without peer state and without Connect is
    // dropped without an answer: a stale client gets no Reject per snapshot
    // to amplify, and runs into its own timeout instead.
    if (id == kInvalidConnection) return;
    if (version != kProtocolVersion) return;
    Peer& p = m_peers.at(id);

    switch (p.state) {
    case PeerState::Connecting:
        if ((flags & FlagChallenge) && blen == 16) {
            // The echoed nonce ties the challenge to OUR connect; a stray or
            // forged challenge with another nonce is ignored.
            if (get64(body) != p.clientNonce) return;
            p.cookie = get64(body + 8);
            p.state  = PeerState::Cookie;
            std::vector<std::uint8_t> reply;
            put64(reply, p.clientNonce);
            put64(reply, p.cookie);
            emitDatagram(p, FlagConnect, ChannelUnreliable, reply.data(), reply.size());
            p.nextConnectMs = m_clock() + kConnectRetryMs;
            HE_LOG_DEBUG(Net, "UDP handshake: challenge received, cookie returned (conn %llu)",
                         static_cast<unsigned long long>(id));
            return;
        }
        if (flags & FlagReject) {
            HE_LOG_WARN(Net, "UDP host rejected the connection (reason %u, conn %llu)",
                        blen ? static_cast<unsigned>(body[0]) : 0u,
                        static_cast<unsigned long long>(id));
            dropPeer(id, /*notify=*/true, "rejected by host");
        }
        return;

    case PeerState::Cookie:
        if (flags & FlagAccept) {
            p.state      = PeerState::Established;
            p.lastRecvMs = m_clock();
            p.recvSeq.accept(seq);
            HE_LOG_INFO(Net, "UDP connect established (conn %llu, host's id %u)",
                        static_cast<unsigned long long>(id),
                        blen >= 4 ? get32(body) : 0u);
            m_events.push_back(NetEvent{ NetEventType::Connected, id, {} });
            return;
        }
        if (flags & FlagReject) {
            HE_LOG_WARN(Net, "UDP host rejected the connection (reason %u, conn %llu)",
                        blen ? static_cast<unsigned>(body[0]) : 0u,
                        static_cast<unsigned long long>(id));
            dropPeer(id, /*notify=*/true, "rejected by host");
        }
        return;

    case PeerState::Established:
        handleEstablished(id, p, flags, channel, seq, body, blen);
        // Acks are processed inside; the fields are read here so the header
        // parse stays in one place.
        if (m_peers.find(id) != m_peers.end()) processAcks(m_peers.at(id), ack, ackBits);
        return;
    }
}

void UdpTransport::handleConnect(const std::string& host, std::uint16_t port,
                                 std::uint8_t version, const std::uint8_t* body,
                                 std::size_t len) {
    if (version != kProtocolVersion) {
        // Told once per attempt; this is the one datagram a mismatched client
        // needs to stop trying.
        const std::uint8_t reason = static_cast<std::uint8_t>(RejectReason::VersionMismatch);
        sendControl(host, port, FlagReject, &reason, 1);
        HE_LOG_WARN(Net, "UDP connect from %s:%u rejected: protocol v%u, ours is v%u",
                    host.c_str(), static_cast<unsigned>(port),
                    static_cast<unsigned>(version), static_cast<unsigned>(kProtocolVersion));
        return;
    }

    if (len == 8) {
        // First contact: answer statelessly. Nothing is allocated for this
        // address until it proves it can receive at it.
        const std::uint64_t nonce = get64(body);
        std::vector<std::uint8_t> reply;
        put64(reply, nonce);
        put64(reply, makeCookie(m_secret[0], host, port, nonce));
        sendControl(host, port, FlagChallenge, reply.data(), reply.size());
        return;
    }
    if (len != 16) return;   // malformed

    const std::uint64_t nonce  = get64(body);
    const std::uint64_t cookie = get64(body + 8);
    if (!cookieValid(host, port, nonce, cookie)) {
        m_stats.cookiesRejected++;
        // Debug, not warning: a forged source address is precisely the case
        // this exists for, and it must not be able to fill the log either.
        HE_LOG_DEBUG(Net, "UDP connect from %s:%u carried an invalid cookie — ignored",
                     host.c_str(), static_cast<unsigned>(port));
        return;
    }

    const ConnectionId existing = findPeer(host, port);
    if (existing != kInvalidConnection) {
        Peer& ep = m_peers.at(existing);
        if (ep.clientNonce == nonce) {
            // Our Accept was lost; the client is still knocking. Say it again.
            std::vector<std::uint8_t> reply;
            put32(reply, existing);
            emitDatagram(ep, FlagAccept, ChannelUnreliable, reply.data(), reply.size());
            return;
        }
        // Same address, new nonce: a restarted client on the same port. The
        // old state cannot serve it (its sequence numbers start over).
        HE_LOG_INFO(Net, "UDP peer %llu reconnected from the same address — replacing",
                    static_cast<unsigned long long>(existing));
        dropPeer(existing, /*notify=*/true, "replaced by a new connect from the same address");
    }

    if (m_peers.size() >= m_cfg.maxPeers) {
        const std::uint8_t reason = static_cast<std::uint8_t>(RejectReason::Full);
        sendControl(host, port, FlagReject, &reason, 1);
        HE_LOG_WARN(Net, "UDP connect from %s:%u rejected: host is full (%u peers)",
                    host.c_str(), static_cast<unsigned>(port), m_cfg.maxPeers);
        return;
    }

    const ConnectionId id = m_nextId++;
    Peer p;
    p.host        = host;
    p.port        = port;
    p.state       = PeerState::Established;
    p.clientNonce = nonce;
    p.lastRecvMs  = m_clock();
    p.lastSendMs  = p.lastRecvMs;
    m_peers.emplace(id, std::move(p));
    m_byAddress[addressKey(stripScope(host), port)] = id;

    Peer& live = m_peers.at(id);
    std::vector<std::uint8_t> reply;
    put32(reply, id);
    emitDatagram(live, FlagAccept, ChannelUnreliable, reply.data(), reply.size());
    m_events.push_back(NetEvent{ NetEventType::Connected, id, {} });
    // Info, like TCP's accept: the moment that proves reachability.
    HE_LOG_INFO(Net, "UDP accepted %s:%u (conn %llu, %zu total)",
                host.c_str(), static_cast<unsigned>(port),
                static_cast<unsigned long long>(id), m_peers.size());
}

void UdpTransport::handleEstablished(ConnectionId id, Peer& p, std::uint8_t flags,
                                     std::uint8_t channel, std::uint16_t seq,
                                     const std::uint8_t* body, std::size_t len) {
    p.lastRecvMs = m_clock();

    // Before the duplicate check: the repeats of a Disconnect carry no seq.
    if (flags & FlagDisconnect) {
        HE_LOG_INFO(Net, "UDP peer %llu disconnected", static_cast<unsigned long long>(id));
        dropPeer(id, /*notify=*/true, "peer disconnected");
        return;
    }

    if (!p.recvSeq.accept(seq)) {
        m_stats.duplicatesDropped++;
        return;
    }
    // The ack field names the highest seq and the 32 before it, no more. A
    // burst larger than that must be acknowledged in pieces, or its early
    // packets fall out of the field before they were ever named and get
    // resent although they arrived. Half a field is the trigger.
    if (++p.datagramsSinceAck >= 16 && p.ackPending) {
        emitDatagram(p, FlagAckOnly, ChannelUnreliable, nullptr, 0);
    }

    if (flags & (FlagKeepAlive | FlagAckOnly | FlagAccept)) return;

    if (channel == ChannelUnreliable) {
        if (flags & FlagFragment) {
            // Fragmenting an unreliable message cannot work — one lost piece
            // loses the whole — so the sender refused it; a peer that sends
            // one anyway is running different code.
            HE_LOG_WARN(Net, "UDP fragment on the unreliable channel from conn %llu — dropped",
                        static_cast<unsigned long long>(id));
            return;
        }
        if (len == 0) return;
        NetEvent ev{ NetEventType::Data, id, {} };
        ev.data.assign(body, body + len);
        m_events.push_back(std::move(ev));
        return;
    }
    if (channel != ChannelReliable && channel != ChannelReliableOrdered) return;
    if (len < kMsgSeqSize) return;

    const std::uint16_t msgSeq = get16(body);
    body += kMsgSeqSize;
    len  -= kMsgSeqSize;

    // Already delivered? A retransmission arrives with a fresh datagram seq,
    // so this is where reliable duplicates are caught. The ack still has to
    // go out — the peer resent precisely because it never saw one.
    bool delivered = false;
    if (channel == ChannelReliableOrdered) {
        delivered = serialLess(msgSeq, p.nextExpectedOrdered) ||
                    p.reorder.find(msgSeq) != p.reorder.end();
    } else {
        delivered = p.recvReliable.seen(msgSeq);
    }
    p.ackPending = true;
    if (p.datagramsSinceAck >= 16) {
        emitDatagram(p, FlagAckOnly, ChannelUnreliable, nullptr, 0);
    }
    if (delivered) {
        m_stats.duplicatesDropped++;
        return;
    }

    if (!(flags & FlagFragment)) {
        handleReliablePayload(id, p, channel, msgSeq, std::vector<std::uint8_t>(body, body + len));
        return;
    }

    if (len < kFragmentHeaderSize) return;
    const std::uint16_t fragId = get16(body);
    const std::uint8_t  index  = body[2];
    const std::uint8_t  count  = body[3];
    body += kFragmentHeaderSize;
    len  -= kFragmentHeaderSize;
    if (count < 2 || index >= count) return;

    auto rit = p.reassembly.find(fragId);
    if (rit == p.reassembly.end()) {
        if (p.reassembly.size() >= kMaxReassemblies ||
            p.reassemblyBytes + len > maxReliableMessage() * 2) {
            HE_LOG_WARN(Net, "UDP conn %llu has too many half-built messages — fragment dropped",
                        static_cast<unsigned long long>(id));
            return;
        }
        Reassembly r;
        r.channel   = channel;
        r.msgSeq    = msgSeq;
        r.count     = count;
        r.startedMs = m_clock();
        r.parts.resize(count);
        rit = p.reassembly.emplace(fragId, std::move(r)).first;
    }
    Reassembly& r = rit->second;
    if (r.msgSeq != msgSeq || r.count != count || r.channel != channel) return;   // inconsistent
    if (!r.parts[index].empty()) return;                                          // repeat
    r.parts[index].assign(body, body + len);
    r.bytes += len;
    p.reassemblyBytes += len;
    if (++r.have < r.count) return;

    std::vector<std::uint8_t> whole;
    whole.reserve(r.bytes);
    for (const auto& part : r.parts) whole.insert(whole.end(), part.begin(), part.end());
    p.reassemblyBytes -= r.bytes;
    p.reassembly.erase(rit);
    m_stats.fragmentsReassembled++;
    handleReliablePayload(id, p, channel, msgSeq, std::move(whole));
}

void UdpTransport::handleReliablePayload(ConnectionId id, Peer& p, std::uint8_t channel,
                                         std::uint16_t msgSeq,
                                         std::vector<std::uint8_t> payload) {
    if (channel == ChannelReliable) {
        if (!p.recvReliable.accept(msgSeq)) {
            m_stats.duplicatesDropped++;
            return;
        }
        m_events.push_back(NetEvent{ NetEventType::Data, id, std::move(payload) });
        return;
    }

    // ReliableOrdered: deliver in msgSeq order, holding later ones back.
    if (msgSeq == p.nextExpectedOrdered) {
        m_events.push_back(NetEvent{ NetEventType::Data, id, std::move(payload) });
        p.nextExpectedOrdered++;
        for (;;) {
            auto it = p.reorder.find(p.nextExpectedOrdered);
            if (it == p.reorder.end()) break;
            m_events.push_back(NetEvent{ NetEventType::Data, id, std::move(it->second) });
            p.reorder.erase(it);
            p.nextExpectedOrdered++;
        }
        return;
    }
    const std::uint16_t ahead = static_cast<std::uint16_t>(msgSeq - p.nextExpectedOrdered);
    if (ahead >= kReorderWindow) {
        // Cannot happen with a well-behaved sender (its window is the same
        // size); a peer that gets here is either broken or hostile.
        HE_LOG_WARN(Net, "UDP ordered message %u from conn %llu is %u ahead of the next "
                         "expected — outside the reorder window, dropped",
                    static_cast<unsigned>(msgSeq), static_cast<unsigned long long>(id),
                    static_cast<unsigned>(ahead));
        return;
    }
    p.reorder.emplace(msgSeq, std::move(payload));
}

// ─── Acks and retransmission ─────────────────────────────────────────────────

void UdpTransport::processAcks(Peer& p, std::uint16_t ack, std::uint32_t ackBits) {
    if (p.inFlight.empty()) return;
    // Oldest first. The fast-retransmit counter of a packet grows for every
    // NEWER packet acked while it waits; walking the bitfield newest-first
    // would count the acks in this same field as evidence against packets
    // that this same field is about to acknowledge.
    for (int i = 31; i >= 0; --i) {
        if (ackBits & (1u << i)) ackOne(p, static_cast<std::uint16_t>(ack - 1 - i));
    }
    ackOne(p, ack);
}

void UdpTransport::ackOne(Peer& p, std::uint16_t seq) {
    // Every datagram repeats the last 33 acks, so each acked seq must count
    // once — otherwise the fast-retransmit counter of every older packet would
    // reach three within three datagrams of ordinary traffic.
    if (!p.recvAcked.accept(seq)) return;

    const std::uint64_t now = m_clock();
    for (auto it = p.inFlight.begin(); it != p.inFlight.end(); ++it) {
        Packet& pk = *it;
        if (pk.seq == seq) {
            // Karn: only a packet that was sent exactly once yields an RTT
            // sample — for a retransmitted one the ack is ambiguous.
            if (pk.resends == 0) {
                const float sample = static_cast<float>(now - pk.sentMs);
                if (p.srttMs < 0.f) {
                    p.srttMs   = sample;
                    p.rttvarMs = sample * 0.5f;
                } else {
                    p.rttvarMs = 0.75f * p.rttvarMs + 0.25f * std::fabs(p.srttMs - sample);
                    p.srttMs   = 0.875f * p.srttMs + 0.125f * sample;
                }
            }
            p.inFlight.erase(it);
            return;
        }
        if (serialLess(pk.seq, seq)) pk.ackedAfter++;
    }
}

float UdpTransport::rtoMs(const Peer& p, std::uint32_t resends) const {
    float rto = (p.srttMs < 0.f) ? kRtoInitialMs : (p.srttMs + 4.f * p.rttvarMs);
    rto = std::clamp(rto, static_cast<float>(kRtoMinMs), static_cast<float>(kRtoMaxMs));
    // Exponential backoff per retransmission of the same packet.
    for (std::uint32_t i = 0; i < resends && rto < static_cast<float>(kRtoMaxMs); ++i) rto *= 2.f;
    return std::min(rto, static_cast<float>(kRtoMaxMs));
}

void UdpTransport::recordSent(bool resend) {
    const std::uint64_t sec = m_clock() / 1000;
    LossBucket& b = m_loss[sec % 5];
    if (b.second != sec) { b.second = sec; b.sent = 0; b.resent = 0; }
    b.sent++;
    if (resend) b.resent++;
}

void UdpTransport::transmitPacket(Peer& p, Packet& pk) {
    pk.seq = emitDatagram(p, 0, pk.channel, pk.payload.data(), pk.payload.size(),
                          &pk.msgSeq, pk.fragment ? &pk : nullptr);
    pk.sentMs     = m_clock();
    pk.ackedAfter = 0;
    recordSent(pk.resends > 0);
}

void UdpTransport::serviceReliability(ConnectionId id, Peer& p) {
    const std::uint64_t now = m_clock();
    // Collect first: a resend moves the packet to the back of the deque.
    std::vector<std::size_t> due;
    for (std::size_t i = 0; i < p.inFlight.size(); ++i) {
        const Packet& pk = p.inFlight[i];
        const bool timedOut = now - pk.sentMs >= static_cast<std::uint64_t>(rtoMs(p, pk.resends));
        const bool fast     = pk.ackedAfter >= 3;
        if (timedOut || fast) due.push_back(i);
    }
    if (due.empty()) return;

    // Resend as new datagrams with new seqs: each seq is transmitted exactly
    // once, so the ack bookkeeping stays unambiguous; the receiver recognises
    // the message by its msgSeq.
    std::vector<Packet> resend;
    resend.reserve(due.size());
    for (auto it = due.rbegin(); it != due.rend(); ++it) {
        resend.push_back(std::move(p.inFlight[*it]));
        p.inFlight.erase(p.inFlight.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    for (auto it = resend.rbegin(); it != resend.rend(); ++it) {
        Packet& pk = *it;
        pk.resends++;
        p.resends++;
        m_stats.resends++;
        HE_LOG_TRACE(Net, "UDP resend #%u of msg %u on channel %u to conn %llu (%s)",
                     pk.resends, static_cast<unsigned>(pk.msgSeq),
                     static_cast<unsigned>(pk.channel), static_cast<unsigned long long>(id),
                     pk.ackedAfter >= 3 ? "fast retransmit" : "rto");
        transmitPacket(p, pk);
        p.inFlight.push_back(std::move(pk));
    }
}

bool UdpTransport::windowAdmits(const Peer& p, const Packet& pk) const {
    if (p.inFlight.size() >= kSendWindow) return false;
    // The packet count is not the only bound. The receiver keeps its reorder
    // buffer (ordered channel) and its duplicate window (reliable channel)
    // relative to the OLDEST message it still waits for, 256 wide; a sender
    // that kept pushing newer messages while one old one was stuck in
    // retransmission would run past that window, and the receiver would drop
    // the newcomers as unbufferable while acknowledging their datagrams —
    // a hole no retransmission ever fills. So a message may only leave while
    // it is within the window of the oldest unacknowledged one on its channel.
    bool          any    = false;
    std::uint16_t oldest = 0;
    for (const Packet& f : p.inFlight) {
        if (f.channel != pk.channel) continue;
        if (!any || serialLess(f.msgSeq, oldest)) { oldest = f.msgSeq; any = true; }
    }
    if (!any) return true;
    const std::uint16_t span = static_cast<std::uint16_t>(pk.msgSeq - oldest);
    return span < kReorderWindow;
}

void UdpTransport::fillWindow(ConnectionId /*id*/, Peer& p) {
    while (!p.queued.empty() && windowAdmits(p, p.queued.front())) {
        Packet pk = std::move(p.queued.front());
        p.queued.pop_front();
        p.queuedBytes -= pk.payload.size();
        transmitPacket(p, pk);
        p.inFlight.push_back(std::move(pk));
    }
}

void UdpTransport::sendReliable(ConnectionId id, Peer& p, std::uint8_t channel,
                                const std::uint8_t* data, std::size_t len) {
    const std::uint16_t msgSeq = (channel == ChannelReliableOrdered) ? p.nextMsgOrdered++
                                                                     : p.nextMsgReliable++;
    const std::size_t single      = m_cfg.mtuPayload - kMsgSeqSize;
    const std::size_t perFragment = single - kFragmentHeaderSize;

    std::vector<Packet> packets;
    if (len <= single) {
        Packet pk;
        pk.channel = channel;
        pk.msgSeq  = msgSeq;
        pk.payload.assign(data, data + len);
        packets.push_back(std::move(pk));
    } else {
        const std::size_t count = (len + perFragment - 1) / perFragment;
        if (count > kMaxFragments) {
            HE_LOG_ERROR(Net, "UDP send refused: %s exceeds the %s reliable message limit (conn %llu)",
                         detail::logBytes(len).c_str(),
                         detail::logBytes(maxReliableMessage()).c_str(),
                         static_cast<unsigned long long>(id));
            return;
        }
        const std::uint16_t fragId = p.nextFragId++;
        packets.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            Packet pk;
            pk.channel   = channel;
            pk.msgSeq    = msgSeq;
            pk.fragment  = true;
            pk.fragId    = fragId;
            pk.fragIndex = static_cast<std::uint8_t>(i);
            pk.fragCount = static_cast<std::uint8_t>(count);
            const std::size_t from = i * perFragment;
            const std::size_t to   = std::min(len, from + perFragment);
            pk.payload.assign(data + from, data + to);
            packets.push_back(std::move(pk));
        }
        HE_LOG_TRACE(Net, "UDP message of %s split into %zu fragments (conn %llu)",
                     detail::logBytes(len).c_str(), count, static_cast<unsigned long long>(id));
    }

    for (Packet& pk : packets) {
        // Strictly behind whatever is already queued: a message may not
        // overtake its predecessors into the window.
        if (p.queued.empty() && windowAdmits(p, pk)) {
            transmitPacket(p, pk);
            p.inFlight.push_back(std::move(pk));
            continue;
        }
        p.queuedBytes += pk.payload.size();
        p.queued.push_back(std::move(pk));
        if (p.queuedBytes > m_cfg.maxQueuedReliable) {
            // A receiver that lets this much reliable data pile up behind a
            // full window is not going to catch up; TcpTransport draws the
            // same line at its frame limit. Unlike disconnect(), this is an
            // involuntary drop, so it IS reported upward.
            HE_LOG_WARN(Net, "UDP conn %llu has %s of reliable data waiting behind a full "
                             "window (limit %s) — dropping the peer",
                        static_cast<unsigned long long>(id),
                        detail::logBytes(p.queuedBytes).c_str(),
                        detail::logBytes(m_cfg.maxQueuedReliable).c_str());
            dropPeer(id, /*notify=*/true, "reliable backlog exceeded");
            return;
        }
    }
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

void UdpTransport::dropPeer(ConnectionId id, bool notify, const char* why) {
    auto it = m_peers.find(id);
    if (it == m_peers.end()) return;
    HE_LOG_DEBUG(Net, "UDP conn %llu removed: %s", static_cast<unsigned long long>(id),
                 why ? why : "unspecified");
    m_byAddress.erase(addressKey(stripScope(it->second.host), it->second.port));
    m_peers.erase(it);
    if (notify) m_events.push_back(NetEvent{ NetEventType::Disconnected, id, {} });
}

void UdpTransport::serviceFarewells() {
    if (m_farewells.empty()) return;
    const std::uint64_t now = m_clock();
    for (auto it = m_farewells.begin(); it != m_farewells.end();) {
        if (now >= it->nextMs) {
            sendControl(it->host, it->port, FlagDisconnect, nullptr, 0);
            it->sendsLeft--;
            it->nextMs = now + kFarewellGapMs;
        }
        if (it->sendsLeft <= 0) it = m_farewells.erase(it);
        else                    ++it;
    }
}

// ─── ITransport ──────────────────────────────────────────────────────────────

void UdpTransport::send(ConnectionId conn, const std::uint8_t* data,
                        std::size_t len, SendMode mode) {
    auto it = m_peers.find(conn);
    if (it == m_peers.end() || it->second.state != PeerState::Established) {
        HE_LOG_DEBUG(Net, "UDP send dropped: connection %llu is %s (%s)",
                     static_cast<unsigned long long>(conn),
                     it == m_peers.end() ? "unknown" : "not established yet",
                     detail::logBytes(len).c_str());
        return;
    }
    Peer& p = it->second;

    switch (mode) {
    case SendMode::Unreliable:
        if (len > maxUnreliableMessage()) {
            // Refused rather than fragmented: one lost piece would lose the
            // whole, so a snapshot this size is the sender's design error.
            HE_LOG_ERROR(Net, "UDP unreliable send refused: %s exceeds the %s datagram "
                              "payload (conn %llu)",
                         detail::logBytes(len).c_str(),
                         detail::logBytes(maxUnreliableMessage()).c_str(),
                         static_cast<unsigned long long>(conn));
            return;
        }
        emitDatagram(p, 0, ChannelUnreliable, data, len);
        recordSent(false);
        return;
    case SendMode::Reliable:
        sendReliable(conn, p, ChannelReliable, data, len);
        return;
    case SendMode::ReliableOrdered:
        sendReliable(conn, p, ChannelReliableOrdered, data, len);
        return;
    }
}

bool UdpTransport::poll(NetEvent& out) {
    if (m_events.empty()) return false;
    out = std::move(m_events.front());
    m_events.pop_front();
    return true;
}

void UdpTransport::disconnect(ConnectionId conn) {
    auto it = m_peers.find(conn);
    if (it == m_peers.end()) return;
    Peer& p = it->second;
    HE_LOG_INFO(Net, "UDP closing connection %llu locally", static_cast<unsigned long long>(conn));
    if (!p.queued.empty() || !p.inFlight.empty()) {
        HE_LOG_WARN(Net, "  %zu reliable packets were still unacknowledged and are discarded",
                    p.queued.size() + p.inFlight.size());
    }
    // Three unacknowledged goodbyes 50 ms apart; the peer is forgotten now.
    // Like TCP and loopback: no Disconnected event for the initiator.
    if (p.state == PeerState::Established) {
        emitDatagram(p, FlagDisconnect, ChannelUnreliable, nullptr, 0);
        m_farewells.push_back(Farewell{ p.host, p.port, kFarewellRepeats - 1,
                                        m_clock() + kFarewellGapMs });
    }
    dropPeer(conn, /*notify=*/false, "closed locally");
}

std::size_t UdpTransport::connectionCount() const {
    std::size_t n = 0;
    for (const auto& [id, p] : m_peers) {
        if (p.state == PeerState::Established) ++n;
    }
    return n;
}

// ─── Statistics ──────────────────────────────────────────────────────────────

UdpTransport::Stats UdpTransport::stats() const {
    Stats s = m_stats;
    float sum = 0.f;
    int   n   = 0;
    for (const auto& [id, p] : m_peers) {
        if (p.state == PeerState::Established && p.srttMs >= 0.f) { sum += p.srttMs; ++n; }
    }
    s.srttMs = n ? sum / static_cast<float>(n) : 0.f;

    const std::uint64_t sec = m_clock() / 1000;
    std::uint64_t sent = 0, resent = 0;
    for (const LossBucket& b : m_loss) {
        if (b.second + 5 > sec && b.second <= sec) { sent += b.sent; resent += b.resent; }
    }
    s.lossPercentWindow = sent ? 100.f * static_cast<float>(resent) / static_cast<float>(sent) : 0.f;
    return s;
}

UdpTransport::PeerStats UdpTransport::peerStats(ConnectionId conn) const {
    PeerStats ps;
    const auto it = m_peers.find(conn);
    if (it == m_peers.end()) return ps;
    const Peer& p = it->second;
    ps.srttMs      = p.srttMs < 0.f ? 0.f : p.srttMs;
    ps.rtoMs       = rtoMs(p, 0);
    ps.resends     = p.resends;
    ps.inFlight    = static_cast<std::uint32_t>(p.inFlight.size());
    ps.queuedBytes = static_cast<std::uint32_t>(p.queuedBytes);
    ps.lastRecvMs  = p.lastRecvMs;
    ps.established = (p.state == PeerState::Established);
    return ps;
}

} // namespace HE::Net
