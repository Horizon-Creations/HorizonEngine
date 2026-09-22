#include "doctest.h"

#include <Net/LoopbackTransport.h>
#include <Net/LossyTransport.h>

#include <memory>
#include <set>
#include <vector>

using namespace HE::Net;

// LossyTransport is the bad network every layer above a transport is tested
// against, so it has to be trustworthy about three things: it does what its
// numbers say, it does it the same way every run for the same seed, and its
// clock is the only thing that moves time. Each case pins one of those.

namespace {

struct LossyPair {
    std::unique_ptr<LossyTransport> a;   // wraps loopback end A: what A sends is mistreated
    std::unique_ptr<LossyTransport> b;
};

LossyPair makePair(LossyTransport::Config cfgA, LossyTransport::Config cfgB = {}) {
    auto [la, lb] = LoopbackTransport::createPair();
    LossyPair p;
    p.a = LossyTransport::wrap(std::move(la), cfgA);
    p.b = LossyTransport::wrap(std::move(lb), cfgB);
    REQUIRE(p.a != nullptr);
    REQUIRE(p.b != nullptr);
    // Loopback hands each end a Connected first.
    NetEvent ev;
    REQUIRE(p.a->poll(ev));
    REQUIRE(ev.type == NetEventType::Connected);
    REQUIRE(p.b->poll(ev));
    REQUIRE(ev.type == NetEventType::Connected);
    return p;
}

std::vector<std::uint8_t> msg(std::uint32_t i) {
    return { static_cast<std::uint8_t>(i >> 24), static_cast<std::uint8_t>(i >> 16),
             static_cast<std::uint8_t>(i >> 8),  static_cast<std::uint8_t>(i) };
}
std::uint32_t idOf(const std::vector<std::uint8_t>& v) {
    return (static_cast<std::uint32_t>(v[0]) << 24) | (static_cast<std::uint32_t>(v[1]) << 16) |
           (static_cast<std::uint32_t>(v[2]) << 8)  |  static_cast<std::uint32_t>(v[3]);
}

std::vector<std::uint32_t> drainIds(ITransport& t) {
    std::vector<std::uint32_t> out;
    NetEvent ev;
    while (t.poll(ev)) {
        if (ev.type == NetEventType::Data) out.push_back(idOf(ev.data));
    }
    return out;
}

// Send `count` messages A → B one per simulated millisecond, then flush.
std::vector<std::uint32_t> run(LossyPair& p, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) {
        p.a->send(LoopbackTransport::kPeer, msg(i), SendMode::Unreliable);
        p.a->advance(1);
        p.a->update();
        p.b->update();
    }
    p.a->flush();
    p.b->update();
    return drainIds(*p.b);
}

} // namespace

TEST_CASE("LossyTransport: wrap refuses a null transport")
{
    CHECK(LossyTransport::wrap(nullptr, {}) == nullptr);
}

TEST_CASE("LossyTransport: with everything at zero it is a transparent pass-through")
{
    LossyPair p = makePair({});
    const auto got = run(p, 200);
    REQUIRE(got.size() == 200);
    for (std::uint32_t i = 0; i < 200; ++i) CHECK(got[i] == i);
    CHECK(p.a->stats().dropped == 0);
    CHECK(p.a->stats().reordered == 0);
    CHECK(p.a->stats().duplicated == 0);
    CHECK(p.a->stats().delivered == 200);
}

TEST_CASE("LossyTransport: loss drops the configured share, and the same seed drops the same ones")
{
    LossyTransport::Config cfg;
    cfg.lossPercent = 25.f;
    cfg.seed        = 42;

    LossyPair first = makePair(cfg);
    const auto gotFirst = run(first, 1000);
    CHECK(gotFirst.size() >= 700);
    CHECK(gotFirst.size() <= 800);
    CHECK(first.a->stats().dropped == 1000 - gotFirst.size());

    // Determinism is the whole point of a seeded generator: a failing test
    // upstairs must fail identically on the next run.
    LossyPair second = makePair(cfg);
    const auto gotSecond = run(second, 1000);
    CHECK(gotSecond == gotFirst);

    // …and a different seed drops a different set (negative control).
    cfg.seed = 43;
    LossyPair third = makePair(cfg);
    const auto gotThird = run(third, 1000);
    CHECK(gotThird != gotFirst);
}

TEST_CASE("LossyTransport: latency holds a datagram until the simulated clock reaches its due time")
{
    LossyTransport::Config cfg;
    cfg.latencyMs = 50;
    LossyPair p = makePair(cfg);

    p.a->send(LoopbackTransport::kPeer, msg(1), SendMode::ReliableOrdered);
    p.a->update();
    p.b->update();
    CHECK(drainIds(*p.b).empty());
    CHECK(p.a->pending() == 1);

    p.a->advance(49);
    p.a->update();
    p.b->update();
    CHECK(drainIds(*p.b).empty());

    p.a->advance(1);
    p.a->update();
    p.b->update();
    const auto got = drainIds(*p.b);
    REQUIRE(got.size() == 1);
    CHECK(got[0] == 1);
    CHECK(p.a->pending() == 0);
}

TEST_CASE("LossyTransport: reorder delivers out of order, and only when asked to")
{
    LossyTransport::Config cfg;
    cfg.reorderPercent = 30.f;
    cfg.reorderDelayMs = 10;
    cfg.seed           = 7;
    LossyPair p = makePair(cfg);

    const auto got = run(p, 500);
    REQUIRE(got.size() == 500);                    // reorder never loses
    std::set<std::uint32_t> unique(got.begin(), got.end());
    CHECK(unique.size() == 500);                   // …nor duplicates
    bool swapped = false;
    std::uint32_t maxDistance = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (i > 0 && got[i] < got[i - 1]) swapped = true;
        // How far behind its send position a message landed.
        const std::uint32_t pos = static_cast<std::uint32_t>(i);
        if (pos > got[i]) maxDistance = std::max(maxDistance, pos - got[i]);
    }
    CHECK(swapped);
    CHECK(p.a->stats().reordered > 0);
    // A reordered datagram slips by at most reorderDelayMs sends (one per ms).
    CHECK(maxDistance <= cfg.reorderDelayMs);

    // Negative control: 0 % reorder keeps the order exactly.
    LossyPair clean = makePair({});
    const auto ordered = run(clean, 500);
    for (std::uint32_t i = 0; i < 500; ++i) REQUIRE(ordered[i] == i);
}

TEST_CASE("LossyTransport: duplication delivers a datagram twice")
{
    LossyTransport::Config cfg;
    cfg.duplicatePercent = 100.f;
    LossyPair p = makePair(cfg);

    const auto got = run(p, 50);
    REQUIRE(got.size() == 100);
    for (std::uint32_t i = 0; i < 50; ++i) {
        CHECK(got[2 * i] == i);
        CHECK(got[2 * i + 1] == i);
    }
    CHECK(p.a->stats().duplicated == 50);
}

TEST_CASE("LossyTransport: the two directions are independent")
{
    LossyTransport::Config lossy;
    lossy.lossPercent = 100.f;
    LossyPair p = makePair(lossy, {});   // A → B loses everything; B → A is clean

    p.a->send(LoopbackTransport::kPeer, msg(1), SendMode::Unreliable);
    p.b->send(LoopbackTransport::kPeer, msg(2), SendMode::Unreliable);
    p.a->update();
    p.b->update();
    p.a->update();
    p.b->update();
    CHECK(drainIds(*p.b).empty());
    const auto atA = drainIds(*p.a);
    REQUIRE(atA.size() == 1);
    CHECK(atA[0] == 2);
}

TEST_CASE("LossyTransport: disconnect discards what was still in flight to that peer")
{
    LossyTransport::Config cfg;
    cfg.latencyMs = 100;
    LossyPair p = makePair(cfg);

    p.a->send(LoopbackTransport::kPeer, msg(1), SendMode::ReliableOrdered);
    CHECK(p.a->pending() == 1);
    p.a->disconnect(LoopbackTransport::kPeer);
    CHECK(p.a->pending() == 0);
    CHECK(p.a->connectionCount() == 0);
}

TEST_CASE("LossyTransport: setConfig mid-session switches from clean to hostile")
{
    LossyPair p = makePair({});
    auto first = run(p, 100);
    CHECK(first.size() == 100);

    LossyTransport::Config hostile;
    hostile.lossPercent = 100.f;
    p.a->setConfig(hostile);
    auto second = run(p, 100);
    CHECK(second.empty());
}
