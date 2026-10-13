#pragma once

// ─── HorizonNet Layer 1 — broadcast through the relay ────────────────────────
// A decorator over the SecureTransport of a relay session (one that runs on a
// RelayTransport). It lets the host hand a message meant for many clients to
// the relay ONCE — one encryption, one frame — and the relay makes the copies
// (BROADCAST/SUBSCRIBE, docs/relay-server-protocol.md). Without a relay nothing
// here is constructed; NetSession then sends per peer exactly as before.
//
// What it has to solve, and how:
//
//  • Confidentiality. The SUBSCRIBE connection is receive-only, so
//    SecureTransport's two-way handshake cannot run on it. The host draws a
//    random group key per session and gives it to each client over that
//    client's own, already encrypted pair (the KEY message). Broadcasts are
//    AES-256-GCM under it; the nonce is the broadcast index, which only ever
//    grows, so it never repeats under one key.
//
//  • Order. A client now has two streams from the host — its pair and the
//    broadcast stream — and the relay does not order one against the other.
//    The app above relies on host send order (a snapshot before the changes
//    after it, a verdict before the apply), so the client merges the two back
//    into exactly that order:
//      – every host→client pair message carries W = how many broadcasts the
//        host had issued before it; it waits until that many are applied;
//      – a broadcast carries, for each subscribed client that got pair
//        messages since the previous broadcast, how many pair messages the
//        host had sent it so far (R); the client applies the broadcast only
//        once it has processed that many. No entry = nothing new since the
//        last one. A client that got nothing per-pair costs nothing here.
//
//  • Joining. A client is "on the relay" only once it said so: KEY → client
//    subscribes → relay confirms → client sends SUBSCRIBED over the pair. Until
//    the host has read that, the client gets every broadcast as a per-pair
//    copy (or a skip, if it was excluded) carrying the same index, so a
//    broadcast that also came over the relay meanwhile is recognised and
//    dropped. Every client sees every index after its KEY exactly once.
//
//  • Exclusion. "Everyone but the sender" travels as a list of excluded client
//    slots; an excluded client advances its index and drops the payload.
//
//  • Loss. A gap that cannot close (a subscription the relay ended, a host
//    whose listener was gone when it broadcast, a hold longer than
//    Config::maxHoldMs or larger than Config::maxHeldBytes) drops the link
//    with a reason in lastError(). A session that silently misses messages
//    is worse than one that says it lost the connection.
//
// The extra headers live entirely in here: nothing above (NetSession) or below
// (SecureTransport, the relay) sees them.

#include "Net/ITransport.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace HE::Net {

class RelayTransport;

class HE_NET_API RelayFanout final : public ITransport {
public:
    struct Config {
        NetRole       role         = NetRole::Client;   // Host or Client
        // Client: how much may wait for the other stream, and for how long,
        // before the link counts as broken. Over TCP an honest gap closes in
        // a round trip; the relay drops a stalled subscriber after 10 s.
        std::size_t   maxHeldBytes = 64u << 20;
        std::uint32_t maxHoldMs    = 30'000;
    };

    // Counters for tests and diagnostics (host side).
    struct Stats {
        std::uint64_t relayFrames = 0;   // BROADCAST frames handed to the relay
        std::uint64_t pairCopies  = 0;   // broadcasts sent per pair (client not on the relay yet)
        std::uint64_t pairSkips   = 0;   // "index n is not for you" to an excluded client
        std::uint64_t unicasts    = 0;   // ordinary per-peer messages
    };

    // `inner` is the session's SecureTransport (or anything with the same
    // contract); `relay` the RelayTransport under it, non-owning — it lives
    // inside `inner`. wrap() owns `inner`, over() does not (the caller keeps
    // it alive for as long as this exists).
    static std::unique_ptr<RelayFanout> wrap(std::unique_ptr<ITransport> inner,
                                             RelayTransport* relay, Config cfg);
    static std::unique_ptr<RelayFanout> over(ITransport* inner, RelayTransport* relay,
                                             Config cfg);

    ~RelayFanout() override;

    RelayFanout(const RelayFanout&)            = delete;
    RelayFanout& operator=(const RelayFanout&) = delete;

    using ITransport::send;

    void        update() override;
    void        send(ConnectionId conn, const std::uint8_t* data,
                     std::size_t len, SendMode mode) override;
    bool        poll(NetEvent& out) override;
    void        disconnect(ConnectionId conn) override;
    std::size_t connectionCount() const override;

    bool fansOut() const override { return true; }
    void sendToMany(const ConnectionId* to, std::size_t count,
                    const std::uint8_t* data, std::size_t len, SendMode mode) override;

    ITransport* inner() const { return m_inner; }
    const Stats& stats() const { return m_stats; }
    // Host: clients that receive broadcasts through the relay right now.
    std::size_t relayedPeers() const;
    // Client: the host has the KEY delivered and knows we are subscribed.
    bool onRelay() const { return m_client.announced; }
    // Why the last link was dropped by this layer. Empty while nothing went wrong.
    const std::string& lastError() const { return m_lastError; }

    // Wire kinds inside the pair (after SecureTransport decrypted it). Public
    // for tests.
    enum class Kind : std::uint8_t {
        Data       = 0x01,   // host→client: [u64 W][payload]; client→host: [payload]
        Copy       = 0x02,   // host→client: [u64 index][payload]
        Skip       = 0x03,   // host→client: [u64 index]
        Key        = 0x10,   // host→client: [u8 flags][32 key][u64 next index][u32 slot]
        Subscribed = 0x11,   // client→host: (empty)
    };

private:
    RelayFanout() = default;

    struct Peer {                       // host side, one per established client
        std::uint32_t slot      = 0;
        bool          onRelay   = false;   // SUBSCRIBED read
        bool          dirty     = false;   // pair messages since the last relay frame
        std::uint64_t pairCount = 0;       // pair messages sent after KEY
    };

    struct HeldPair {                   // client: a pair message waiting for broadcasts
        Kind                      kind  = Kind::Data;
        std::uint64_t             index = 0;   // W for Data, the broadcast index otherwise
        std::vector<std::uint8_t> payload;
        std::uint64_t             atMs  = 0;
    };
    struct HeldBroadcast {              // client: a relay broadcast waiting for the pair
        std::uint64_t             index = 0;
        bool                      hasCount = false;
        std::uint64_t             count = 0;   // R for this client, if the frame said
        bool                      excluded = false;
        std::vector<std::uint8_t> payload;
        std::uint64_t             atMs  = 0;
    };
    struct ClientState {
        ConnectionId  host        = kInvalidConnection;
        bool          keyed       = false;
        bool          broadcastOk = false;   // KEY flags: the host broadcasts at all
        bool          subscribing = false;
        bool          announced   = false;   // SUBSCRIBED sent
        std::uint32_t slot        = 0;
        std::uint8_t  key[32]{};
        std::uint64_t nextIndex   = 0;       // next broadcast index to apply
        std::uint64_t pairDone    = 0;       // pair messages processed after KEY
        bool          countKnown  = false;
        std::uint64_t knownCount  = 0;       // latest R the host told us
        std::deque<HeldPair>      pair;
        std::deque<HeldBroadcast> relay;
        std::size_t   heldBytes   = 0;
    };

    void drain();
    void onInner(NetEvent& ev);
    // Host
    void hostConnected(ConnectionId conn);
    void hostData(ConnectionId conn, std::vector<std::uint8_t>& data);
    void sendPair(ConnectionId conn, Peer& p, Kind kind, std::uint64_t index,
                  const std::uint8_t* data, std::size_t len, SendMode mode);
    // Client
    void clientData(ConnectionId conn, std::vector<std::uint8_t>& data);
    void clientBroadcast(std::vector<std::uint8_t>& frame);
    void merge();
    void failLink(ConnectionId conn, const std::string& why);
    void resetClient();

    std::unique_ptr<ITransport> m_owned;
    ITransport*                 m_inner = nullptr;
    RelayTransport*             m_relay = nullptr;
    Config                      m_cfg;
    std::deque<NetEvent>        m_events;
    std::string                 m_lastError;
    Stats                       m_stats;

    // Host
    std::map<ConnectionId, Peer> m_peers;
    std::uint8_t                 m_key[32]{};
    bool                         m_keyOk     = false;
    std::uint64_t                m_nextIndex = 0;
    std::uint32_t                m_nextSlot  = 1;

    // Client
    ClientState                  m_client;
};

} // namespace HE::Net
