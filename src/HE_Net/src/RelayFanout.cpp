#include "Net/RelayFanout.h"

#include "Net/RelayTransport.h"

#include "NetLog.h"

#include <Hpak/Aes256Gcm.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace HE::Net {
namespace {

constexpr std::size_t  kIndexLen  = 8;
constexpr std::size_t  kKeyMsgLen = 1 + 1 + 32 + 8 + 4;   // kind flags key index slot
constexpr std::size_t  kTagLen    = 16;
constexpr std::uint8_t kFlagBroadcast = 0x01;   // KEY: the host broadcasts through the relay

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void putU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}
void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}
void putU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}
std::uint64_t getU64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
std::uint32_t getU32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}
std::uint16_t getU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

// The broadcast index is the GCM nonce counter: one host, one key per
// session, an index that only grows — never the same nonce twice.
void broadcastNonce(std::uint8_t out[12], std::uint64_t index) {
    out[0] = 'H'; out[1] = 'E'; out[2] = 'B'; out[3] = 'C';
    for (int i = 0; i < 8; ++i) out[4 + i] = static_cast<std::uint8_t>(index >> (56 - i * 8));
}

void wipe(std::uint8_t* p, std::size_t n) {
    volatile std::uint8_t* v = p;
    for (std::size_t i = 0; i < n; ++i) v[i] = 0;
}

bool isHostRole(NetRole r) { return r == NetRole::Host || r == NetRole::Server; }

} // namespace

// ─── Construction ────────────────────────────────────────────────────────────

std::unique_ptr<RelayFanout> RelayFanout::over(ITransport* inner, RelayTransport* relay,
                                               Config cfg) {
    if (!inner) return nullptr;
    std::unique_ptr<RelayFanout> f(new RelayFanout());
    f->m_inner = inner;
    f->m_relay = relay;
    f->m_cfg   = cfg;
    if (isHostRole(cfg.role)) {
        f->m_keyOk = Hpak::randomBytes(f->m_key, sizeof(f->m_key));
        if (!f->m_keyOk)
            HE_LOG_WARN(Net, "Relay fan-out: no random source for a group key — every "
                             "broadcast goes per link");
    }
    return f;
}

std::unique_ptr<RelayFanout> RelayFanout::wrap(std::unique_ptr<ITransport> inner,
                                               RelayTransport* relay, Config cfg) {
    auto f = over(inner.get(), relay, cfg);
    if (f) f->m_owned = std::move(inner);
    return f;
}

RelayFanout::~RelayFanout() {
    wipe(m_key, sizeof(m_key));
    wipe(m_client.key, sizeof(m_client.key));
}

// ─── Pump ────────────────────────────────────────────────────────────────────

void RelayFanout::update() {
    m_inner->update();
    drain();
}

bool RelayFanout::poll(NetEvent& out) {
    if (m_events.empty()) drain();
    if (m_events.empty()) return false;
    out = std::move(m_events.front());
    m_events.pop_front();
    return true;
}

void RelayFanout::drain() {
    NetEvent ev;
    while (m_inner->poll(ev)) onInner(ev);
    if (isHostRole(m_cfg.role)) return;

    ClientState& c = m_client;
    if (m_relay) {
        ConnectionId conn = 0;
        std::vector<std::uint8_t> frame;
        while (m_relay->pollBroadcast(conn, frame)) {
            if (conn == c.host && c.keyed) clientBroadcast(frame);
        }
    }
    if (c.host != kInvalidConnection && c.subscribing && !c.announced && m_relay &&
        m_relay->subscribed(c.host)) {
        const std::uint8_t msg = static_cast<std::uint8_t>(Kind::Subscribed);
        m_inner->send(c.host, &msg, 1, SendMode::ReliableOrdered);
        c.announced = true;
        HE_LOG_INFO(Net, "Relay fan-out: subscribed to the host's broadcasts (conn %llu)",
                    static_cast<unsigned long long>(c.host));
    }
    merge();
}

void RelayFanout::onInner(NetEvent& ev) {
    const bool host = isHostRole(m_cfg.role);
    switch (ev.type) {
        case NetEventType::Connected:
            if (host) {
                hostConnected(ev.conn);
            } else {
                resetClient();
                m_client.host = ev.conn;
            }
            m_events.push_back(std::move(ev));
            return;
        case NetEventType::Disconnected:
            if (host) {
                m_peers.erase(ev.conn);
            } else if (ev.conn == m_client.host) {
                merge();   // what is deliverable goes up before the close
                resetClient();
            }
            m_events.push_back(std::move(ev));
            return;
        case NetEventType::Data:
            if (host) hostData(ev.conn, ev.data);
            else      clientData(ev.conn, ev.data);
            return;
    }
}

// ─── Host ────────────────────────────────────────────────────────────────────

void RelayFanout::hostConnected(ConnectionId conn) {
    Peer p;
    p.slot = m_nextSlot++;
    // First message on the pair, before the layer above even knows the peer:
    // pair counting starts after it, on both sides.
    std::vector<std::uint8_t> msg;
    msg.reserve(kKeyMsgLen);
    msg.push_back(static_cast<std::uint8_t>(Kind::Key));
    msg.push_back(m_keyOk ? kFlagBroadcast : 0);
    if (m_keyOk) msg.insert(msg.end(), m_key, m_key + sizeof(m_key));
    else         msg.insert(msg.end(), sizeof(m_key), 0);
    putU64(msg, m_nextIndex);
    putU32(msg, p.slot);
    m_inner->send(conn, msg.data(), msg.size(), SendMode::ReliableOrdered);
    wipe(msg.data(), msg.size());
    m_peers[conn] = p;
    HE_LOG_DEBUG(Net, "Relay fan-out: conn %llu is slot %u, broadcasts from index %llu",
                 static_cast<unsigned long long>(conn), p.slot,
                 static_cast<unsigned long long>(m_nextIndex));
}

void RelayFanout::hostData(ConnectionId conn, std::vector<std::uint8_t>& data) {
    auto it = m_peers.find(conn);
    if (it == m_peers.end() || data.empty()) return;
    switch (static_cast<Kind>(data[0])) {
        case Kind::Data:
            data.erase(data.begin());
            m_events.push_back(NetEvent{ NetEventType::Data, conn, std::move(data) });
            return;
        case Kind::Subscribed:
            if (!it->second.onRelay) {
                it->second.onRelay = true;
                it->second.dirty   = true;   // the next relay frame tells it where we are
                HE_LOG_INFO(Net, "Relay fan-out: conn %llu now receives broadcasts through the "
                                 "relay (%zu of %zu)",
                            static_cast<unsigned long long>(conn), relayedPeers(), m_peers.size());
            }
            return;
        default:
            failLink(conn, "a client sent an unknown fan-out message");
            return;
    }
}

void RelayFanout::sendPair(ConnectionId conn, Peer& p, Kind kind, std::uint64_t index,
                           const std::uint8_t* data, std::size_t len, SendMode mode) {
    std::vector<std::uint8_t> buf;
    buf.reserve(1 + kIndexLen + len);
    buf.push_back(static_cast<std::uint8_t>(kind));
    putU64(buf, index);
    if (len) buf.insert(buf.end(), data, data + len);
    m_inner->send(conn, buf.data(), buf.size(), mode);
    ++p.pairCount;
    p.dirty = true;
}

void RelayFanout::send(ConnectionId conn, const std::uint8_t* data, std::size_t len,
                       SendMode mode) {
    if (isHostRole(m_cfg.role)) {
        auto it = m_peers.find(conn);
        if (it == m_peers.end()) {
            HE_LOG_DEBUG(Net, "Relay fan-out send dropped: conn %llu is not established (%s)",
                         static_cast<unsigned long long>(conn), detail::logBytes(len).c_str());
            return;
        }
        // W = broadcasts issued before this message; the client applies it after them.
        sendPair(conn, it->second, Kind::Data, m_nextIndex, data, len, mode);
        ++m_stats.unicasts;
        return;
    }
    if (conn != m_client.host) return;
    std::vector<std::uint8_t> buf;
    buf.reserve(1 + len);
    buf.push_back(static_cast<std::uint8_t>(Kind::Data));
    buf.insert(buf.end(), data, data + len);
    m_inner->send(conn, buf.data(), buf.size(), mode);
}

void RelayFanout::sendToMany(const ConnectionId* to, std::size_t count,
                             const std::uint8_t* data, std::size_t len, SendMode mode) {
    if (!isHostRole(m_cfg.role)) {
        for (std::size_t i = 0; i < count; ++i) send(to[i], data, len, mode);
        return;
    }
    std::vector<ConnectionId> wanted(to, to + count);
    std::sort(wanted.begin(), wanted.end());
    auto isWanted = [&](ConnectionId c) {
        return std::binary_search(wanted.begin(), wanted.end(), c);
    };

    // Every established peer learns about this index exactly once: through
    // the relay, or per pair as a copy or a skip.
    const std::uint64_t index = m_nextIndex++;
    std::vector<std::uint32_t> excluded;
    bool anyOnRelay = false;
    for (auto& [conn, p] : m_peers) {
        const bool want = isWanted(conn);
        if (p.onRelay) {
            anyOnRelay = true;
            if (!want) excluded.push_back(p.slot);
        } else if (want) {
            sendPair(conn, p, Kind::Copy, index, data, len, mode);
            ++m_stats.pairCopies;
        } else {
            sendPair(conn, p, Kind::Skip, index, nullptr, 0, mode);
            ++m_stats.pairSkips;
        }
    }
    if (!anyOnRelay) return;

    // [u16 n][n × (u32 slot, u64 pair count)][u16 m][m × u32 slot][payload]
    std::vector<std::uint8_t> plain;
    std::uint16_t entries = 0;
    for (const auto& [conn, p] : m_peers)
        if (p.onRelay && p.dirty) ++entries;
    plain.reserve(4 + entries * 12u + excluded.size() * 4u + len);
    putU16(plain, entries);
    for (const auto& [conn, p] : m_peers) {
        if (!p.onRelay || !p.dirty) continue;
        putU32(plain, p.slot);
        putU64(plain, p.pairCount);
    }
    putU16(plain, static_cast<std::uint16_t>(excluded.size()));
    for (const std::uint32_t s : excluded) putU32(plain, s);
    if (len) plain.insert(plain.end(), data, data + len);

    std::uint8_t nonce[12];
    broadcastNonce(nonce, index);
    std::vector<std::uint8_t> cipher;
    std::vector<std::uint8_t> frame;
    bool sent = false;
    if (Hpak::aesGcmEncrypt(m_key, nonce, plain.data(), plain.size(), cipher)) {
        frame.reserve(kIndexLen + cipher.size());
        putU64(frame, index);
        frame.insert(frame.end(), cipher.begin(), cipher.end());
        sent = m_relay && m_relay->sendBroadcast(frame.data(), frame.size());
    }
    if (!sent) {
        // The clients on the relay will never see this index, and everything
        // after it waits for it. Say so now rather than leave them hanging.
        std::vector<ConnectionId> lost;
        for (const auto& [conn, p] : m_peers)
            if (p.onRelay) lost.push_back(conn);
        for (const ConnectionId c : lost)
            failLink(c, "a broadcast could not be handed to the relay (its listener "
                        "connection is down); this client would have missed it");
        return;
    }
    for (auto& [conn, p] : m_peers)
        if (p.onRelay) p.dirty = false;
    ++m_stats.relayFrames;
    HE_LOG_TRACE(Net, "Relay fan-out: broadcast %llu, %s, to %zu through the relay "
                      "(%u position entr%s, %zu excluded)",
                 static_cast<unsigned long long>(index), detail::logBytes(len).c_str(),
                 relayedPeers() - excluded.size(), static_cast<unsigned>(entries),
                 entries == 1 ? "y" : "ies", excluded.size());
}

std::size_t RelayFanout::relayedPeers() const {
    return static_cast<std::size_t>(std::count_if(
        m_peers.begin(), m_peers.end(), [](const auto& kv) { return kv.second.onRelay; }));
}

// ─── Client ──────────────────────────────────────────────────────────────────

void RelayFanout::clientData(ConnectionId conn, std::vector<std::uint8_t>& data) {
    ClientState& c = m_client;
    if (conn != c.host) return;
    if (data.empty()) return failLink(conn, "an empty fan-out message from the host");
    const Kind kind = static_cast<Kind>(data[0]);

    if (kind == Kind::Key) {
        if (c.keyed || data.size() != kKeyMsgLen)
            return failLink(conn, "a malformed or repeated KEY from the host");
        c.keyed       = true;
        c.broadcastOk = (data[1] & kFlagBroadcast) != 0;
        std::memcpy(c.key, data.data() + 2, sizeof(c.key));
        c.nextIndex   = getU64(data.data() + 34);
        c.slot        = getU32(data.data() + 42);
        wipe(data.data(), data.size());
        if (c.broadcastOk && m_relay) c.subscribing = m_relay->subscribe(conn);
        HE_LOG_DEBUG(Net, "Relay fan-out: slot %u, broadcasts from index %llu%s", c.slot,
                     static_cast<unsigned long long>(c.nextIndex),
                     c.subscribing ? ", subscribing" : ", per link");
        return;
    }
    if (!c.keyed) return failLink(conn, "the host sent a message before its KEY");

    HeldPair m;
    m.kind = kind;
    m.atMs = nowMs();
    switch (kind) {
        case Kind::Data:
        case Kind::Copy:
            if (data.size() < 1 + kIndexLen)
                return failLink(conn, "a truncated message from the host");
            m.index = getU64(data.data() + 1);
            m.payload.assign(data.begin() + 1 + kIndexLen, data.end());
            break;
        case Kind::Skip:
            if (data.size() != 1 + kIndexLen)
                return failLink(conn, "a malformed skip from the host");
            m.index = getU64(data.data() + 1);
            break;
        default:
            return failLink(conn, "an unknown fan-out message from the host");
    }
    c.heldBytes += m.payload.size();
    c.pair.push_back(std::move(m));
    merge();
}

void RelayFanout::clientBroadcast(std::vector<std::uint8_t>& frame) {
    ClientState& c = m_client;
    const ConnectionId host = c.host;
    if (frame.size() < kIndexLen + kTagLen)
        return failLink(host, "a truncated broadcast from the relay");
    HeldBroadcast b;
    b.index = getU64(frame.data());
    b.atMs  = nowMs();
    std::uint8_t nonce[12];
    broadcastNonce(nonce, b.index);
    std::vector<std::uint8_t> plain;
    if (!Hpak::aesGcmDecrypt(c.key, nonce, frame.data() + kIndexLen, frame.size() - kIndexLen,
                             plain))
        return failLink(host, "a broadcast failed authentication (tampered, or not from "
                              "this session's host)");

    std::size_t pos = 0;
    auto need = [&](std::size_t n) { return plain.size() - pos >= n; };
    if (!need(2)) return failLink(host, "a malformed broadcast");
    const std::uint16_t entries = getU16(plain.data() + pos);
    pos += 2;
    if (!need(entries * 12u + 2u)) return failLink(host, "a malformed broadcast");
    for (std::uint16_t i = 0; i < entries; ++i, pos += 12) {
        if (getU32(plain.data() + pos) == c.slot) {
            b.hasCount = true;
            b.count    = getU64(plain.data() + pos + 4);
        }
    }
    const std::uint16_t excluded = getU16(plain.data() + pos);
    pos += 2;
    if (!need(excluded * 4u)) return failLink(host, "a malformed broadcast");
    for (std::uint16_t i = 0; i < excluded; ++i, pos += 4)
        if (getU32(plain.data() + pos) == c.slot) b.excluded = true;
    if (!b.excluded) b.payload.assign(plain.begin() + static_cast<std::ptrdiff_t>(pos), plain.end());

    c.heldBytes += b.payload.size();
    c.relay.push_back(std::move(b));
    merge();
}

void RelayFanout::merge() {
    ClientState& c = m_client;
    if (!c.keyed) return;
    for (bool progress = true; progress;) {
        progress = false;

        if (!c.pair.empty()) {
            HeldPair& m = c.pair.front();
            bool take = false, deliver = false;
            if (m.kind == Kind::Data) {
                take = deliver = c.nextIndex >= m.index;
            } else if (m.index < c.nextIndex) {
                take = true;   // already applied from the relay
            } else if (m.index == c.nextIndex) {
                take    = true;
                deliver = m.kind == Kind::Copy;
                ++c.nextIndex;
            }
            if (take) {
                ++c.pairDone;
                c.heldBytes -= m.payload.size();
                if (deliver)
                    m_events.push_back(NetEvent{ NetEventType::Data, c.host, std::move(m.payload) });
                c.pair.pop_front();
                progress = true;
            }
        }

        if (!c.relay.empty()) {
            HeldBroadcast& b = c.relay.front();
            if (b.index == c.nextIndex && b.hasCount) {
                c.countKnown = true;
                c.knownCount = b.count;
            }
            bool take = false, deliver = false;
            if (b.index < c.nextIndex) {
                take = true;   // also came per pair, before we were on the relay
            } else if (b.index == c.nextIndex && c.countKnown && c.pairDone >= c.knownCount) {
                take    = true;
                deliver = !b.excluded;
                ++c.nextIndex;
            }
            if (take) {
                c.heldBytes -= b.payload.size();
                if (deliver)
                    m_events.push_back(NetEvent{ NetEventType::Data, c.host, std::move(b.payload) });
                c.relay.pop_front();
                progress = true;
            }
        }
    }

    // What is still queued is waiting for the other stream.
    if (c.host == kInvalidConnection) return;
    const std::uint64_t now = nowMs();
    auto tooOld = [&](std::uint64_t at) { return now - at > m_cfg.maxHoldMs; };
    if (c.heldBytes > m_cfg.maxHeldBytes) {
        failLink(c.host, "more than " + detail::logBytes(m_cfg.maxHeldBytes) +
                         " waited for the other stream from the host");
    } else if (!c.pair.empty() && tooOld(c.pair.front().atMs)) {
        failLink(c.host, "broadcast " + std::to_string(c.nextIndex) + " never arrived from "
                         "the relay (a message from the host waited for it for " +
                         std::to_string(m_cfg.maxHoldMs / 1000) + " s)");
    } else if (!c.relay.empty() && tooOld(c.relay.front().atMs)) {
        failLink(c.host, "broadcast " + std::to_string(c.relay.front().index) + " waited " +
                         std::to_string(m_cfg.maxHoldMs / 1000) + " s for messages from the "
                         "host that never came");
    }
}

void RelayFanout::failLink(ConnectionId conn, const std::string& why) {
    m_lastError = why;
    HE_LOG_WARN(Net, "Relay fan-out: dropping conn %llu — %s",
                static_cast<unsigned long long>(conn), why.c_str());
    m_inner->disconnect(conn);
    if (isHostRole(m_cfg.role)) {
        m_peers.erase(conn);
    } else if (conn == m_client.host) {
        resetClient();
    }
    // The layer above did not ask for this close, so it has to hear of it.
    m_events.push_back(NetEvent{ NetEventType::Disconnected, conn, {} });
}

void RelayFanout::resetClient() {
    wipe(m_client.key, sizeof(m_client.key));
    m_client = ClientState{};
}

// ─── ITransport ──────────────────────────────────────────────────────────────

void RelayFanout::disconnect(ConnectionId conn) {
    if (isHostRole(m_cfg.role)) m_peers.erase(conn);
    else if (conn == m_client.host) resetClient();
    m_inner->disconnect(conn);
}

std::size_t RelayFanout::connectionCount() const {
    return m_inner->connectionCount();
}

} // namespace HE::Net
