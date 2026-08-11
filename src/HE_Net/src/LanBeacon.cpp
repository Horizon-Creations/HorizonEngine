#include "Net/LanBeacon.h"
#include "Net/BitStream.h"
#include "NetLog.h"
#include <algorithm>

namespace HE::Net::LanBeacon {
namespace {

// Everything a stranger sent is length-capped before it becomes a std::string.
// The wire format allows a 32-bit length; without this, one forged datagram
// would ask for a gigabyte.
bool readCapped(BitReader& r, std::string& out) {
    if (!r.readString(out)) return false;
    if (out.size() > kMaxStringLen) out.resize(kMaxStringLen);
    return true;
}

// The SENDING side caps too, and that is not belt-and-braces — it is the whole
// reason a datagram fits. Four strings at kMaxStringLen plus the fixed fields
// come to roughly 400 bytes, comfortably under kMaxDatagram; without the cap a
// user whose project is named at length would emit something every receiver
// throws away on the size check, and their session would simply never appear
// for anyone. Silent, and impossible to guess from the symptom.
std::string capped(const std::string& s) {
    return s.size() <= kMaxStringLen ? s : s.substr(0, kMaxStringLen);
}

} // namespace

std::vector<std::uint8_t> encode(const Announcement& a) {
    BitWriter w;
    w.writeUInt32(kMagic);
    w.writeUInt16(a.protocol);
    w.writeUInt64(a.instance);
    w.writeString(capped(a.sessionId));
    w.writeUInt16(a.port);
    w.writeString(capped(a.hostName));
    w.writeString(capped(a.projectLabel));
    w.writeString(capped(a.projectKey));
    w.writeByte(a.participants);
    w.writeByte(a.closing ? 1 : 0);
    return w.data();
}

bool decode(const std::uint8_t* data, std::size_t len, Announcement& out) {
    if (!data || len < 8 || len > kMaxDatagram) return false;
    BitReader r(data, len);

    std::uint32_t magic = 0;
    if (!r.readUInt32(magic) || magic != kMagic) return false;

    // The version is read but NOT rejected here. A peer on an older protocol is
    // worth showing greyed out with "needs a newer build" — a session that
    // simply never appears is a support thread, one that appears and explains
    // itself is not. The caller decides what to do with a mismatch.
    if (!r.readUInt16(out.protocol))      return false;
    if (!r.readUInt64(out.instance))      return false;
    if (!readCapped(r, out.sessionId))    return false;
    if (!r.readUInt16(out.port))          return false;
    if (!readCapped(r, out.hostName))     return false;
    if (!readCapped(r, out.projectLabel)) return false;
    if (!readCapped(r, out.projectKey))   return false;

    std::uint8_t participants = 0, closing = 0;
    if (!r.readByte(participants) || !r.readByte(closing)) return false;
    out.participants = participants;
    out.closing      = closing != 0;

    // A port of zero cannot be connected to, so an announcement carrying one is
    // malformed however well-formed the rest looks.
    return out.port != 0;
}

// ─── Announcer ───────────────────────────────────────────────────────────────

Announcer::~Announcer() { stop(); }

bool Announcer::start(const Announcement& what) {
    stop();
    m_what = what;

    m_sock = socketCreateUdp();
    if (m_sock == kInvalidSocket) return false;

    // Both paths are enabled, because which one survives depends on hardware we
    // cannot see: consumer access points routinely drop one or the other.
    socketSetBroadcast(m_sock, true);
    socketSetMulticastTtl(m_sock, 1);   // this segment only, never routed onward

    // Leave by the interface that reaches the network, not by whichever one the
    // routing table picks for a multicast group — on a machine with Hyper-V, WSL
    // or a VPN that is a coin toss, and the failure is silent (the send
    // succeeds, nothing ever hears it).
    if (const std::string local = socketLocalAddress(); !local.empty())
        socketSetMulticastInterface(m_sock, local);

    m_lastMs = 0;   // speak on the first update rather than after a full interval
    // Abbreviated, like everywhere else: a full session id in a pasted log is a
    // working invitation. The join code is not here at all, by design.
    HE_LOG_INFO(Net, "LAN: announcing session %s on port %u",
                detail::logSessionId(m_what.sessionId).c_str(), unsigned(m_what.port));
    return true;
}

void Announcer::stop() {
    if (m_sock == kInvalidSocket) return;
    // The goodbye. Without it the session lingers in every browser's list until
    // it expires, and someone clicking a corpse meets a connection error as
    // their first experience of the feature.
    Announcement bye = m_what;
    bye.closing = true;
    send(bye);

    socketClose(m_sock);
    m_sock = kInvalidSocket;
}

void Announcer::send(const Announcement& a) {
    if (m_sock == kInvalidSocket) return;
    const std::vector<std::uint8_t> bytes = encode(a);
    std::size_t sent = 0;
    socketSendTo(m_sock, bytes.data(), bytes.size(), kMulticastGroup, kPort, sent);
    socketSendTo(m_sock, bytes.data(), bytes.size(), kBroadcast,      kPort, sent);
}

void Announcer::update(std::uint64_t nowMs) {
    if (m_sock == kInvalidSocket) return;
    if (m_lastMs != 0 && nowMs - m_lastMs < kAnnounceMs) return;
    m_lastMs = nowMs;
    send(m_what);
}

// ─── Browser ─────────────────────────────────────────────────────────────────

Browser::~Browser() { stop(); }

bool Browser::start() {
    stop();
    m_sock = socketCreateUdp();
    if (m_sock == kInvalidSocket) return false;

    // Shared, so a second editor on the same machine can listen too — testing a
    // session against yourself is the first thing anyone tries.
    if (!socketBindUdpShared(m_sock, kPort)) {
        socketClose(m_sock);
        m_sock = kInvalidSocket;
        return false;
    }
    const std::string local = socketLocalAddress();
    socketJoinMulticastGroup(m_sock, kMulticastGroup, local);
    HE_LOG_INFO(Net, "LAN: listening for sessions on port %u", unsigned(kPort));
    return true;
}

void Browser::stop() {
    if (m_sock != kInvalidSocket) {
        socketClose(m_sock);
        m_sock = kInvalidSocket;
    }
    m_sessions.clear();
}

void Browser::ingest(const std::string& fromHost, const std::uint8_t* data,
                     std::size_t len, std::uint64_t nowMs) {
    Announcement a;
    if (!decode(data, len, a)) return;

    // Identity is the INSTANCE, not the session id: the same beacon arrives
    // twice within milliseconds (once multicast, once broadcast), and a host
    // that restarts its session keeps neither.
    const auto it = std::find_if(m_sessions.begin(), m_sessions.end(),
        [&](const Session& s) { return s.instance == a.instance; });

    if (a.closing) {
        if (it != m_sessions.end()) m_sessions.erase(it);
        return;
    }

    Session* s = nullptr;
    if (it != m_sessions.end()) {
        s = &*it;
    } else {
        // Bounded: a peer forging announcements must not be able to grow this
        // without limit or push a real session off the end of the list.
        if (m_sessions.size() >= kMaxSessions) return;
        m_sessions.push_back({});
        s = &m_sessions.back();
    }

    // The address comes from the packet, never from the payload — an announcer
    // must not be able to point joins at a machine that is not itself.
    s->address      = fromHost;
    s->port         = a.port;
    s->sessionId    = a.sessionId;
    s->hostName     = a.hostName;
    s->projectLabel = a.projectLabel;
    s->projectKey   = a.projectKey;
    s->protocol     = a.protocol;
    s->participants = a.participants;
    s->instance     = a.instance;
    s->lastSeenMs   = nowMs;
}

void Browser::update(std::uint64_t nowMs) {
    if (m_sock != kInvalidSocket) {
        // Drain whatever arrived. Non-blocking (timeout 0), so this costs
        // nothing on a frame where nobody announced.
        std::uint8_t buf[kMaxDatagram];
        while (socketWaitReadable(m_sock, 0)) {
            std::size_t got = 0;
            std::string from;
            std::uint16_t fromPort = 0;
            if (socketRecvFrom(m_sock, buf, sizeof(buf), got, from, fromPort)
                != SocketResult::Ok) {
                break;
            }
            if (got == 0) break;
            ingest(from, buf, got, nowMs);
        }
    }

    // A host that stopped talking — crashed, closed the lid, walked out of Wi-Fi
    // range — never sends a goodbye, so silence has to be enough.
    m_sessions.erase(
        std::remove_if(m_sessions.begin(), m_sessions.end(),
            [&](const Session& s) {
                return nowMs > s.lastSeenMs && nowMs - s.lastSeenMs > kExpiryMs;
            }),
        m_sessions.end());
}

} // namespace HE::Net::LanBeacon
