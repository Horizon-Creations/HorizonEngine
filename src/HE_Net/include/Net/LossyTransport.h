#pragma once

// ─── HorizonNet Layer 1 — deterministic bad-network decorator ────────────────
// Wraps any ITransport and mistreats what is SENT through it: drops datagrams,
// delays them with jitter, reorders them, duplicates them — all from a seeded
// generator, so a failing test fails the same way every time.
//
// Sits ABOVE the wrapped transport, which is what makes it useful for
// everything that sits above a transport: SecureTransport's replay window,
// GameReplication under loss and reorder, property deltas, RPC ordering. It
// cannot exercise UdpTransport's own reliability (that runs underneath, and
// would repair the damage before anyone saw it); for that, UdpTransport has a
// drop hook of its own.
//
// Time is simulated. Nothing is delivered until update() runs, and a delayed
// datagram is delivered by the first update() at or after its due time on the
// decorator's own clock, which only advance() moves. A test with 50 ms latency
// therefore does not wait 50 ms; it calls advance(50).
//
// The SendMode decides what may happen to a message, because the decorator
// models what arrives ABOVE UdpTransport, and that transport has already
// repaired the reliable channels:
//   • Unreliable       — loss, reorder, duplication, latency and jitter
//   • Reliable         — latency, jitter and reorder only (delivery is
//                        guaranteed but unordered: a retransmission overtakes)
//   • ReliableOrdered  — latency and jitter only, and never overtaking an
//                        earlier ordered message to the same peer
// A session handshake sent ReliableOrdered therefore always completes, while
// the snapshots around it are mistreated exactly as the numbers say.
//
// Wrapping a LoopbackTransport pair — one decorator per end, each with its
// own seed — gives two independently bad directions.

#include "Net/ITransport.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

namespace HE::Net {

class HE_NET_API LossyTransport final : public ITransport {
public:
    struct Config {
        float         lossPercent      = 0.f;   // dropped outright
        float         reorderPercent   = 0.f;   // delayed by an extra 1..reorderDelayMs
        float         duplicatePercent = 0.f;   // delivered twice
        std::uint32_t latencyMs        = 0;     // base one-way delay
        std::uint32_t jitterMs         = 0;     // ± uniform on top of latency
        std::uint32_t reorderDelayMs   = 20;    // how far a reordered datagram slips
        std::uint32_t seed             = 1;
    };

    struct Stats {
        std::uint64_t offered    = 0;   // send() calls
        std::uint64_t dropped    = 0;
        std::uint64_t reordered  = 0;
        std::uint64_t duplicated = 0;
        std::uint64_t delivered  = 0;   // handed to the inner transport
    };

    static std::unique_ptr<LossyTransport> wrap(std::unique_ptr<ITransport> inner, Config cfg);

    ~LossyTransport() override;

    LossyTransport(const LossyTransport&)            = delete;
    LossyTransport& operator=(const LossyTransport&) = delete;

    using ITransport::send;

    void        update() override;
    void        send(ConnectionId conn, const std::uint8_t* data,
                     std::size_t len, SendMode mode) override;
    bool        poll(NetEvent& out) override;
    void        disconnect(ConnectionId conn) override;
    std::size_t connectionCount() const override;

    ITransport*   inner() const { return m_inner.get(); }
    const Config& config() const { return m_cfg; }
    // Reconfigure mid-session, e.g. clean for the handshake, then hostile.
    void          setConfig(Config cfg) { m_cfg = cfg; }
    const Stats&  stats() const { return m_stats; }

    // Simulated clock.
    void          advance(std::uint32_t ms) { m_nowMs += ms; }
    std::uint64_t nowMs() const { return m_nowMs; }
    // Datagrams still waiting for their due time.
    std::size_t   pending() const { return m_queue.size(); }
    // Deliver everything queued regardless of due time (end-of-test flush).
    void          flush();

private:
    LossyTransport() = default;

    struct Delayed {
        std::uint64_t             dueMs = 0;
        std::uint64_t             order = 0;   // ties broken by submission order
        ConnectionId              conn  = kInvalidConnection;
        SendMode                  mode  = SendMode::Unreliable;
        std::vector<std::uint8_t> data;
    };

    bool roll(float percent);
    void enqueue(ConnectionId conn, const std::uint8_t* data, std::size_t len,
                 SendMode mode, std::uint64_t dueMs);

    std::unique_ptr<ITransport> m_inner;
    Config                      m_cfg;
    std::mt19937                m_rng;
    std::uint64_t               m_nowMs   = 0;
    std::uint64_t               m_counter = 0;
    // Latest due time of a ReliableOrdered message per peer: the floor for
    // the next one, so jitter can never reorder that channel.
    std::unordered_map<ConnectionId, std::uint64_t> m_orderedFloor;
    std::vector<Delayed>        m_queue;   // unsorted; update() picks the due ones
    Stats                       m_stats;
};

} // namespace HE::Net
