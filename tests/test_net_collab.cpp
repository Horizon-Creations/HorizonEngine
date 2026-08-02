#include "doctest.h"

#include <Net/CollabSession.h>
#include <Net/LoopbackTransport.h>
#include <Net/NetSession.h>

#include <memory>
#include <string>
#include <vector>

using namespace HE::Net;

namespace {

// Stands in for SceneSerializer. Keeping the real one out of these tests is the
// point of ISessionStateProvider: HorizonNet must stay free of scene
// dependencies, so the protocol is exercised without an ECS anywhere in sight.
class FakeState final : public ISessionStateProvider {
public:
    std::vector<std::uint8_t> data;
    bool captureOk = true;
    bool applyOk   = true;
    int  applyCalls = 0;

    bool captureSnapshot(std::vector<std::uint8_t>& out) override {
        if (!captureOk) return false;
        out = data;
        return true;
    }
    bool applySnapshot(const std::vector<std::uint8_t>& in) override {
        ++applyCalls;
        if (!applyOk) return false;
        data = in;
        return true;
    }
};

// A host and one client wired through loopback, each with its own session stack.
struct Pair {
    std::unique_ptr<LoopbackTransport> hostT, clientT;
    std::unique_ptr<NetSession>        hostNet, clientNet;
    std::unique_ptr<CollabSession>     host, client;
    FakeState hostState, clientState;

    void pump(int rounds = 12) {
        for (int i = 0; i < rounds; ++i) {
            hostT->update();
            clientT->update();
            hostNet->pump();
            clientNet->pump();
            host->update();
            client->update();
        }
    }
};

std::unique_ptr<Pair> makePair(const std::string& hostName = "Anna",
                               const std::string& clientName = "Bob",
                               CollabSession::Config clientCfg = {}) {
    auto p = std::make_unique<Pair>();
    auto [a, b] = LoopbackTransport::createPair();
    p->hostT   = std::move(a);
    p->clientT = std::move(b);

    p->hostNet   = std::make_unique<NetSession>(p->hostT.get(), NetRole::Host);
    p->clientNet = std::make_unique<NetSession>(p->clientT.get(), NetRole::Client);

    CollabSession::Config hostCfg;
    hostCfg.displayName = hostName;
    clientCfg.displayName = clientName;

    p->host   = std::make_unique<CollabSession>(p->hostNet.get(), NetRole::Host, hostCfg);
    p->client = std::make_unique<CollabSession>(p->clientNet.get(), NetRole::Client, clientCfg);

    p->host->setStateProvider(&p->hostState);
    p->client->setStateProvider(&p->clientState);
    return p;
}

std::vector<std::uint8_t> makeBlob(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0xFF);
    return v;
}

} // namespace

// ─── Join flow ───────────────────────────────────────────────────────────────

TEST_CASE("CollabSession: the host is joined immediately and lists itself")
{
    auto p = makePair("Anna");
    CHECK(p->host->isJoined());
    REQUIRE(p->host->participants().size() == 1);
    CHECK(p->host->participants()[0].name == "Anna");
    CHECK(p->host->participants()[0].isHost);
    CHECK(p->host->localId() != kInvalidParticipant);

    // The client has not joined until it has the state.
    CHECK_FALSE(p->client->isJoined());
}

TEST_CASE("CollabSession: a client joins and receives the host's state")
{
    auto p = makePair("Anna", "Bob");
    p->hostState.data = makeBlob(1024);

    ParticipantId joinedAs = kInvalidParticipant;
    p->client->onJoined([&](ParticipantId id) { joinedAs = id; });

    p->pump();

    CHECK(p->client->isJoined());
    CHECK(joinedAs != kInvalidParticipant);
    CHECK(joinedAs == p->client->localId());

    // The snapshot actually transferred, byte for byte.
    CHECK(p->clientState.data == p->hostState.data);

    // Both sides now see two participants.
    CHECK(p->host->participants().size() == 2);
    CHECK(p->client->participants().size() == 2);
}

TEST_CASE("CollabSession: the roster names both sides correctly")
{
    auto p = makePair("Anna", "Bob");
    p->hostState.data = makeBlob(64);
    p->pump();

    bool sawAnnaAsHost = false, sawBob = false;
    for (const auto& part : p->client->participants()) {
        if (part.name == "Anna" && part.isHost) sawAnnaAsHost = true;
        if (part.name == "Bob"  && !part.isHost) sawBob = true;
    }
    CHECK(sawAnnaAsHost);
    CHECK(sawBob);

    // Ids must be unique, or later per-participant state (locks, presence)
    // would collide.
    const auto& list = p->client->participants();
    REQUIRE(list.size() == 2);
    CHECK(list[0].id != list[1].id);
}

TEST_CASE("CollabSession: the host is notified when someone joins")
{
    auto p = makePair();
    p->hostState.data = makeBlob(32);

    std::string joinedName;
    p->host->onParticipantJoined([&](const Participant& part) { joinedName = part.name; });

    p->pump();
    CHECK(joinedName == "Bob");
}

// ─── Snapshot transfer ───────────────────────────────────────────────────────

TEST_CASE("CollabSession: a large snapshot transfers across many chunks")
{
    CollabSession::Config cfg;
    auto p = makePair("Anna", "Bob", cfg);

    // Well beyond one chunk, so the split/reassembly path is what is measured.
    p->hostState.data = makeBlob(700 * 1024);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> progress;
    p->client->onSnapshotProgress([&](std::uint32_t got, std::uint32_t total) {
        progress.emplace_back(got, total);
    });

    p->pump(30);

    REQUIRE(p->client->isJoined());
    CHECK(p->clientState.data.size() == 700 * 1024);
    CHECK(p->clientState.data == p->hostState.data);

    // Progress must be reported and must end at 100%, or a UI could never show
    // a meaningful bar for a multi-megabyte scene.
    REQUIRE(progress.size() > 2);
    CHECK(progress.front().second == 700 * 1024);
    CHECK(progress.back().first == progress.back().second);
}

TEST_CASE("CollabSession: an empty scene still completes the join")
{
    auto p = makePair();
    p->hostState.data.clear();   // brand new project

    p->pump();
    CHECK(p->client->isJoined());
    CHECK(p->clientState.data.empty());
}

TEST_CASE("CollabSession: a client that cannot apply the snapshot does not count as joined")
{
    auto p = makePair();
    p->hostState.data  = makeBlob(256);
    p->clientState.applyOk = false;   // deserialization fails

    JoinRejectReason reason = JoinRejectReason::None;
    p->client->onJoinRejected([&](JoinRejectReason r) { reason = r; });

    p->pump();

    // Claiming success here would leave the user editing a scene they never got.
    CHECK_FALSE(p->client->isJoined());
    CHECK(reason == JoinRejectReason::SnapshotFailed);
}

TEST_CASE("CollabSession: the host refuses the join when it cannot capture state")
{
    auto p = makePair();
    p->hostState.captureOk = false;

    JoinRejectReason reason = JoinRejectReason::None;
    p->client->onJoinRejected([&](JoinRejectReason r) { reason = r; });

    p->pump();

    CHECK(reason == JoinRejectReason::SnapshotFailed);
    CHECK_FALSE(p->client->isJoined());
    // The joiner must not appear in the roster after a refused join.
    CHECK(p->host->participants().size() == 1);
    CHECK(p->clientState.applyCalls == 0);
}

TEST_CASE("CollabSession: a snapshot larger than the client's limit is refused")
{
    CollabSession::Config tight;
    tight.maxSnapshotBytes = 1024;      // client will not accept more
    auto p = makePair("Anna", "Bob", tight);

    p->hostState.data = makeBlob(64 * 1024);

    JoinRejectReason reason = JoinRejectReason::None;
    p->client->onJoinRejected([&](JoinRejectReason r) { reason = r; });

    p->pump(30);

    // The buffer must never be grown to whatever the peer announces.
    CHECK(reason == JoinRejectReason::SnapshotFailed);
    CHECK_FALSE(p->client->isJoined());
}

// ─── Rejections ──────────────────────────────────────────────────────────────

TEST_CASE("CollabSession: a mismatched protocol version is rejected")
{
    // Simulate an older/newer peer by sending a JoinRequest by hand.
    auto [a, b] = LoopbackTransport::createPair();
    NetSession hostNet(a.get(), NetRole::Host);
    NetSession clientNet(b.get(), NetRole::Client);

    FakeState hostState;
    CollabSession host(&hostNet, NetRole::Host);
    host.setStateProvider(&hostState);

    JoinRejectReason reason = JoinRejectReason::None;
    clientNet.on(kFirstUserMessage + 2, [&](ConnectionId, BitReader& r) {
        std::uint8_t code = 0;
        r.readByte(code);
        reason = static_cast<JoinRejectReason>(code);
    });

    a->update(); b->update();
    hostNet.pump(); clientNet.pump();

    BitWriter w;
    w.writeUInt16(kCollabProtocolVersion + 99);   // wrong version
    w.writeString("Intruder");
    clientNet.send(LoopbackTransport::kPeer, kFirstUserMessage + 0, w);

    for (int i = 0; i < 6; ++i) {
        a->update(); b->update();
        hostNet.pump(); clientNet.pump();
    }

    CHECK(reason == JoinRejectReason::VersionMismatch);
    CHECK(host.participants().size() == 1);   // nobody admitted
}

TEST_CASE("CollabSession: a full session refuses further joins")
{
    auto [a, b] = LoopbackTransport::createPair();
    NetSession hostNet(a.get(), NetRole::Host);
    NetSession clientNet(b.get(), NetRole::Client);

    FakeState hostState;
    CollabSession::Config cfg;
    cfg.maxParticipants = 1;          // host alone fills the session
    CollabSession host(&hostNet, NetRole::Host, cfg);
    host.setStateProvider(&hostState);

    JoinRejectReason reason = JoinRejectReason::None;
    clientNet.on(kFirstUserMessage + 2, [&](ConnectionId, BitReader& r) {
        std::uint8_t code = 0;
        r.readByte(code);
        reason = static_cast<JoinRejectReason>(code);
    });

    a->update(); b->update();
    hostNet.pump(); clientNet.pump();

    BitWriter w;
    w.writeUInt16(kCollabProtocolVersion);
    w.writeString("Latecomer");
    clientNet.send(LoopbackTransport::kPeer, kFirstUserMessage + 0, w);

    for (int i = 0; i < 6; ++i) {
        a->update(); b->update();
        hostNet.pump(); clientNet.pump();
    }

    CHECK(reason == JoinRejectReason::SessionFull);
    CHECK(host.participants().size() == 1);
}

// ─── Leaving ─────────────────────────────────────────────────────────────────

TEST_CASE("CollabSession: the host drops a participant when its link goes away")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->host->participants().size() == 2);

    ParticipantId leftId = kInvalidParticipant;
    p->host->onParticipantLeft([&](ParticipantId id) { leftId = id; });

    p->clientT.reset();   // the peer disappears

    for (int i = 0; i < 6; ++i) {
        p->hostT->update();
        p->hostNet->pump();
        p->host->update();
    }

    CHECK(leftId != kInvalidParticipant);
    CHECK(p->host->participants().size() == 1);   // only the host remains
}

TEST_CASE("CollabSession: a client that loses the host is no longer joined")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    p->hostT.reset();   // host goes away

    for (int i = 0; i < 6; ++i) {
        p->clientT->update();
        p->clientNet->pump();
        p->client->update();
    }

    CHECK_FALSE(p->client->isJoined());
    CHECK(p->client->participants().empty());
}

// ─── State sequence ──────────────────────────────────────────────────────────

TEST_CASE("CollabSession: the joiner adopts the host's state sequence")
{
    auto p = makePair();
    p->hostState.data = makeBlob(128);
    p->pump();

    // Live deltas (N6) will continue from this number, so both sides must agree
    // on where the shared state currently stands.
    CHECK(p->client->stateSequence() == p->host->stateSequence());
}
