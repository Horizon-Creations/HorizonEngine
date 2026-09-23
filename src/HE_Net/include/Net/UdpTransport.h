#pragma once

// ─── HorizonNet Layer 1 — UDP transport ──────────────────────────────────────
// The real-network ITransport for gameplay replication. TcpTransport carries
// editor collaboration, where every message must arrive and order is free;
// a game wants the opposite for its hot path: a 30 Hz snapshot that arrives
// late is worthless, and waiting for its retransmission delays every snapshot
// behind it. UDP delivers datagrams as they come — and nothing else. Everything
// a connection needs on top of that lives here:
//
//   • a handshake that proves the client owns its source address before the
//     host allocates anything for it (cookie, §4.3 of the plan),
//   • all three SendModes: Unreliable (snapshots, input), Reliable (unordered,
//     property deltas), ReliableOrdered (handshake, spawn/despawn, RPC),
//   • acknowledgement by bitfield, RTT estimation, retransmission on timeout
//     and on fast-retransmit (three later packets acked, this one not),
//   • fragmentation of reliable messages above the datagram payload size,
//   • keepalive and timeout, because a silent UDP peer is indistinguishable
//     from a dead one without them,
//   • statistics the diagnostics overlay reads (RTT, loss, resends).
//
// What it deliberately is NOT: encrypted (SecureTransport wraps it, exactly as
// it wraps TCP), NAT-traversing (no STUN, no relay), MTU-discovering (a fixed
// 1200-byte payload holds on every path this engine is tested on).
//
// Wire format, every datagram:
//
//     magic:u16 "HU"   version:u8   flags:u8   channel:u8
//     seq:u16          ack:u16      ackBits:u32
//     [msgSeq:u16]                       on channels 1 and 2 (reliable)
//     [fragId:u16 index:u8 count:u8]     when flags has Fragment
//     payload
//
// `seq` numbers every datagram of one direction, on every channel, in ONE
// sequence space. `ack`/`ackBits` therefore acknowledge datagrams, not
// messages: the sender knows for each datagram whether a reliable message rode
// in it (then it is retransmitted — as a NEW datagram with a new `seq` and the
// same `msgSeq`) or not. That is what lets the unreliable snapshot stream carry
// the reliable channel's acks for free, and lets one ack field serve all three
// channels. Duplicate detection runs on `seq` for every datagram and on
// `msgSeq` for reliable messages (a retransmission has a fresh `seq`).
//
// Threading: single-threaded and poll-based like every other transport. The
// owner pumps update() once per tick and drains poll().
//
// Time: everything time-based (RTO, keepalive, timeout, cookie rotation) reads
// one clock, and the clock is injectable (setClock). Tests use that to run a
// five-second timeout in a millisecond of wall time; production leaves the
// steady clock in place.

#include "Net/ITransport.h"
#include "Net/Socket.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace HE::Net {

class HE_NET_API UdpTransport final : public ITransport {
public:
    // ── Wire constants ────────────────────────────────────────────────────
    // Exposed so a test can build a raw datagram by hand (the cookie test does)
    // and so the constants are checked against exactly one definition.
    static constexpr std::uint16_t kMagic           = 0x4855;   // "HU"
    static constexpr std::uint8_t  kProtocolVersion = 1;
    static constexpr std::size_t   kHeaderSize      = 13;
    static constexpr std::size_t   kMsgSeqSize      = 2;
    static constexpr std::size_t   kFragmentHeaderSize = 4;

    enum Flag : std::uint8_t {
        FlagConnect    = 1u << 0,
        FlagChallenge  = 1u << 1,
        FlagAccept     = 1u << 2,
        FlagReject     = 1u << 3,
        FlagDisconnect = 1u << 4,
        FlagKeepAlive  = 1u << 5,
        FlagFragment   = 1u << 6,
        FlagAckOnly    = 1u << 7,
    };

    enum Channel : std::uint8_t {
        ChannelUnreliable      = 0,
        ChannelReliable        = 1,
        ChannelReliableOrdered = 2,
    };

    // Reject reasons, one byte on the wire.
    enum class RejectReason : std::uint8_t {
        VersionMismatch = 1,
        Full            = 2,
    };

    // Reliable packets in flight per peer before send() starts queueing.
    static constexpr std::uint32_t kSendWindow    = 256;
    // Out-of-order ReliableOrdered messages held back per peer.
    static constexpr std::uint32_t kReorderWindow = 256;
    // Fragments per message (index and count are one byte each).
    static constexpr std::uint32_t kMaxFragments  = 255;

    // RTO bounds (Jacobson/Karels estimator, clamped).
    static constexpr std::uint32_t kRtoMinMs = 100;
    static constexpr std::uint32_t kRtoMaxMs = 2000;
    // An ack-only datagram goes out after this much silence with unacked
    // reliable data received; a keepalive after Config::keepAliveMs.
    static constexpr std::uint32_t kAckDelayMs = 50;

    struct Config {
        std::uint32_t mtuPayload        = 1200;      // bytes after the 13-byte header
        std::uint32_t timeoutMs         = 5000;      // no datagram at all → Disconnected
        std::uint32_t keepAliveMs       = 250;       // silence before a KeepAlive
        std::uint32_t maxQueuedReliable = 64 * 1024; // queued BEHIND a full window
        std::uint32_t maxPeers          = 64;        // host: beyond this, Reject
    };

    // Per transport. Counts are monotonic; the two floats are current.
    struct Stats {
        std::uint64_t datagramsSent        = 0;
        std::uint64_t datagramsReceived    = 0;
        std::uint64_t bytesSent            = 0;
        std::uint64_t bytesReceived        = 0;
        std::uint32_t resends              = 0;
        std::uint32_t duplicatesDropped    = 0;
        std::uint32_t fragmentsReassembled = 0;
        std::uint32_t cookiesRejected      = 0;
        float         srttMs               = 0.f;   // mean over live peers
        float         lossPercentWindow    = 0.f;   // resends/sent, sliding 5 s
    };

    struct PeerStats {
        float         srttMs         = 0.f;
        float         rtoMs          = 0.f;
        std::uint32_t resends        = 0;
        std::uint32_t inFlight       = 0;    // reliable packets awaiting ack
        std::uint32_t queuedBytes    = 0;    // reliable bytes waiting for the window
        std::uint64_t lastRecvMs     = 0;
        bool          established    = false;
    };

    // Host side: bind + serve. Port 0 lets the OS choose; read it back with
    // boundPort(). Returns nullptr on failure.
    static std::unique_ptr<UdpTransport> listen(std::uint16_t port, Config cfg);
    static std::unique_ptr<UdpTransport> listen(std::uint16_t port);

    // Client side: begin the handshake. Returns non-null as soon as the first
    // Connect has left — completion surfaces as a Connected event from poll(),
    // or Disconnected if the host rejects or never answers (Config::timeoutMs).
    static std::unique_ptr<UdpTransport> connect(const std::string& host,
                                                 std::uint16_t port, Config cfg);
    static std::unique_ptr<UdpTransport> connect(const std::string& host,
                                                 std::uint16_t port);

    ~UdpTransport() override;

    UdpTransport(const UdpTransport&)            = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    using ITransport::send;

    void        update() override;
    void        send(ConnectionId conn, const std::uint8_t* data,
                     std::size_t len, SendMode mode) override;
    bool        poll(NetEvent& out) override;
    void        disconnect(ConnectionId conn) override;
    std::size_t connectionCount() const override;

    std::uint16_t boundPort() const { return m_boundPort; }
    bool          isListening() const { return m_isHost; }
    const Config& config() const { return m_cfg; }

    // Largest message accepted per mode, for callers that size their own
    // payloads (the replication layer's integrity manifest is one).
    std::size_t maxUnreliableMessage() const;
    std::size_t maxReliableMessage() const;

    Stats     stats() const;
    PeerStats peerStats(ConnectionId conn) const;

    // ── Time and test hooks ───────────────────────────────────────────────
    // Milliseconds, monotonic. Default is std::chrono::steady_clock.
    using ClockFn = std::function<std::uint64_t()>;
    void setClock(ClockFn clock);
    std::uint64_t nowMs() const { return m_clock(); }

    // Called for every datagram about to leave the socket. Return true to drop
    // it. This is the only way to create loss UNDER the reliability layer
    // without asking the kernel, so it exists for tests; production never
    // sets it.
    using TestDatagramFn = std::function<bool(const std::uint8_t* datagram, std::size_t len)>;
    void setTestDropFn(TestDatagramFn fn);
    // Same shape; return true to send the datagram twice.
    void setTestDuplicateFn(TestDatagramFn fn);

private:
    UdpTransport() = default;

    // One reliable packet: what it takes to send it, and to send it again.
    struct Packet {
        std::uint16_t seq       = 0;    // datagram seq of the LAST transmission
        std::uint8_t  channel   = 0;
        std::uint16_t msgSeq    = 0;
        bool          fragment  = false;
        std::uint16_t fragId    = 0;
        std::uint8_t  fragIndex = 0;
        std::uint8_t  fragCount = 0;
        std::vector<std::uint8_t> payload;
        std::uint64_t sentMs    = 0;
        std::uint32_t resends   = 0;
        std::uint32_t ackedAfter = 0;   // newer packets acked while this one was not
    };

    // 256-slot sequence buffer: "have I seen seq?" for a window behind the
    // highest seen value, with u16 wraparound handled by serial arithmetic.
    struct SeqWindow {
        static constexpr std::uint32_t kSlots = 256;
        bool          any     = false;
        std::uint16_t highest = 0;
        std::uint16_t slotSeq[kSlots] = {};
        bool          slotSet[kSlots] = {};

        // false = duplicate or too old; true = new (and now recorded)
        bool accept(std::uint16_t seq);
        bool seen(std::uint16_t seq) const;
    };

    struct Reassembly {
        std::uint8_t  channel = 0;
        std::uint16_t msgSeq  = 0;
        std::uint8_t  count   = 0;
        std::uint8_t  have    = 0;
        std::uint64_t startedMs = 0;
        std::vector<std::vector<std::uint8_t>> parts;
        std::size_t   bytes   = 0;
    };

    enum class PeerState : std::uint8_t {
        Connecting,     // client: Connect sent, waiting for Challenge
        Cookie,         // client: Connect+cookie sent, waiting for Accept
        Established,
    };

    struct Peer {
        std::string   host;        // exactly as socketRecvFrom reports it
        std::uint16_t port  = 0;
        PeerState     state = PeerState::Connecting;
        std::uint64_t clientNonce = 0;
        std::uint64_t cookie      = 0;
        std::uint64_t connectStartMs   = 0;
        std::uint64_t nextConnectMs    = 0;
        std::uint64_t lastRecvMs = 0;
        std::uint64_t lastSendMs = 0;
        bool          ackPending = false;   // reliable data received, not yet acked
        std::uint32_t datagramsSinceAck = 0; // received since we last sent anything

        // Send side.
        std::uint16_t nextSeq          = 1;
        std::uint16_t nextMsgOrdered   = 0;
        std::uint16_t nextMsgReliable  = 0;
        std::uint16_t nextFragId       = 0;
        std::deque<Packet> inFlight;        // ascending by seq of last transmission
        std::deque<Packet> queued;          // waiting for the window
        std::size_t   queuedBytes = 0;
        float         srttMs   = -1.f;      // <0 = no sample yet
        float         rttvarMs = 0.f;
        std::uint32_t resends  = 0;

        // Receive side.
        SeqWindow     recvSeq;              // datagram seqs → acks + dedup
        SeqWindow     recvAcked;            // acked seqs already processed once
        SeqWindow     recvReliable;         // msgSeq on channel 1
        std::uint16_t nextExpectedOrdered = 0;
        std::unordered_map<std::uint16_t, std::vector<std::uint8_t>> reorder;
        std::unordered_map<std::uint16_t, Reassembly> reassembly;
        std::size_t   reassemblyBytes = 0;
    };

    // A peer that was told goodbye: the remaining Disconnect repeats.
    struct Farewell {
        std::string   host;
        std::uint16_t port = 0;
        int           sendsLeft = 0;
        std::uint64_t nextMs = 0;
    };

    // Socket / receive.
    void receiveAll();
    void handleDatagram(const std::string& host, std::uint16_t port,
                        const std::uint8_t* data, std::size_t len);
    void handleConnect(const std::string& host, std::uint16_t port,
                       std::uint8_t version, const std::uint8_t* body, std::size_t len);
    void handleEstablished(ConnectionId id, Peer& p, std::uint8_t flags,
                           std::uint8_t channel, std::uint16_t seq,
                           const std::uint8_t* body, std::size_t len);
    void handleReliablePayload(ConnectionId id, Peer& p, std::uint8_t channel,
                               std::uint16_t msgSeq, std::vector<std::uint8_t> payload);
    void processAcks(Peer& p, std::uint16_t ack, std::uint32_t ackBits);
    void ackOne(Peer& p, std::uint16_t seq);

    // Send side.
    void sendReliable(ConnectionId id, Peer& p, std::uint8_t channel,
                      const std::uint8_t* data, std::size_t len);
    void transmitPacket(Peer& p, Packet& pk);
    // May this packet leave now: room in the packet window AND within the
    // receiver's message window of its channel?
    bool windowAdmits(const Peer& p, const Packet& pk) const;
    void fillWindow(ConnectionId id, Peer& p);
    void serviceReliability(ConnectionId id, Peer& p);
    // Emits one datagram: header + optional msgSeq/fragment fields + body.
    // Returns the seq it went out with.
    std::uint16_t emitDatagram(Peer& p, std::uint8_t flags, std::uint8_t channel,
                               const std::uint8_t* body, std::size_t len,
                               const std::uint16_t* msgSeq = nullptr,
                               const Packet* fragment = nullptr);
    void rawSend(const std::string& host, std::uint16_t port,
                 const std::uint8_t* datagram, std::size_t len);
    void sendControl(const std::string& host, std::uint16_t port, std::uint8_t flags,
                     const std::uint8_t* body, std::size_t len);

    // Lifecycle.
    ConnectionId findPeer(const std::string& host, std::uint16_t port) const;
    void dropPeer(ConnectionId id, bool notify, const char* why);
    void serviceFarewells();

    // Cookies.
    std::uint64_t makeCookie(const std::uint8_t secret[32], const std::string& host,
                             std::uint16_t port, std::uint64_t nonce) const;
    bool cookieValid(const std::string& host, std::uint16_t port,
                     std::uint64_t nonce, std::uint64_t cookie) const;
    void rotateSecretIfDue();

    float rtoMs(const Peer& p, std::uint32_t resends) const;
    void  recordSent(bool resend);

    SocketHandle   m_socket    = kInvalidSocket;
    std::uint16_t  m_boundPort = 0;
    bool           m_isHost    = false;
    Config         m_cfg;
    ClockFn        m_clock;
    TestDatagramFn m_testDrop;
    TestDatagramFn m_testDuplicate;

    ConnectionId                            m_nextId = 1;
    std::unordered_map<ConnectionId, Peer>  m_peers;
    std::unordered_map<std::string, ConnectionId> m_byAddress;   // "host|port"
    std::vector<Farewell>                   m_farewells;
    std::deque<NetEvent>                    m_events;

    std::uint8_t   m_secret[2][32] = {};
    std::uint64_t  m_secretRotatedMs = 0;

    Stats          m_stats;
    // Sliding 5-second loss window: per-second buckets of sent/resent.
    struct LossBucket { std::uint64_t second = 0; std::uint32_t sent = 0, resent = 0; };
    LossBucket     m_loss[5];
};

} // namespace HE::Net
