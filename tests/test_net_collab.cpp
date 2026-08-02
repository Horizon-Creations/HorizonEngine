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

    // Simulated clock, so presence throttling is deterministic rather than
    // dependent on how fast the test machine happens to run.
    std::uint64_t nowMs = 0;

    void pump(int rounds = 12, std::uint64_t stepMs = 1000) {
        for (int i = 0; i < rounds; ++i) {
            hostT->update();
            clientT->update();
            hostNet->pump();
            clientNet->pump();
            host->update(nowMs);
            client->update(nowMs);
            nowMs += stepMs;
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

// ─── Presence ────────────────────────────────────────────────────────────────

namespace {
const float kPos[3] = { 1.5f, -2.25f, 30.0f };
const float kRot[4] = { 0.0f, 0.7071068f, 0.0f, 0.7071068f };   // 90° about Y
} // namespace

TEST_CASE("CollabSession: a client's camera and selection reach the host")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    ParticipantId changedFor = kInvalidParticipant;
    p->host->onPresenceChanged([&](ParticipantId id, const PresenceState&) {
        changedFor = id;
    });

    p->client->setLocalPresence(kPos, kRot, { 111, 222, 333 });
    p->pump(4);

    const ParticipantId clientId = p->client->localId();
    CHECK(changedFor == clientId);

    const PresenceState* seen = p->host->presenceOf(clientId);
    REQUIRE(seen != nullptr);
    CHECK(seen->valid);

    // Position keeps full float precision — a scene can span kilometres.
    CHECK(seen->cameraPos[0] == doctest::Approx(kPos[0]));
    CHECK(seen->cameraPos[1] == doctest::Approx(kPos[1]));
    CHECK(seen->cameraPos[2] == doctest::Approx(kPos[2]));

    // Rotation is quantized to 16 bits per component; the error must stay far
    // below anything a camera gizmo could show.
    for (int i = 0; i < 4; ++i) {
        CHECK(seen->cameraRot[i] == doctest::Approx(kRot[i]).epsilon(0.001));
    }

    CHECK(seen->selection == std::vector<std::uint64_t>{ 111, 222, 333 });
}

TEST_CASE("CollabSession: the host's own presence reaches clients")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    p->host->setLocalPresence(kPos, kRot, { 42 });
    p->pump(4);

    const PresenceState* seen = p->client->presenceOf(p->host->localId());
    REQUIRE(seen != nullptr);
    CHECK(seen->cameraPos[2] == doctest::Approx(kPos[2]));
    CHECK(seen->selection == std::vector<std::uint64_t>{ 42 });
}

TEST_CASE("CollabSession: presence is throttled rather than sent every frame")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    int updates = 0;
    p->host->onPresenceChanged([&](ParticipantId, const PresenceState&) { ++updates; });

    // 60 "frames" of continuous camera movement inside one 100 ms window.
    for (int i = 0; i < 60; ++i) {
        const float pos[3] = { static_cast<float>(i), 0.0f, 0.0f };
        p->client->setLocalPresence(pos, kRot, {});
        p->clientT->update(); p->hostT->update();
        p->clientNet->pump(); p->hostNet->pump();
        p->client->update(p->nowMs);
        p->host->update(p->nowMs);
        p->nowMs += 1;   // 1 ms per frame
    }

    // A gizmo drag must not put 60 messages on the wire for state that is stale
    // a frame later.
    CHECK(updates <= 2);
    CHECK(updates >= 1);
}

TEST_CASE("CollabSession: an idle editor stops sending presence")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    p->client->setLocalPresence(kPos, kRot, {});
    p->pump(4);

    int updates = 0;
    p->host->onPresenceChanged([&](ParticipantId, const PresenceState&) { ++updates; });

    // Plenty of time passes, but nothing moves.
    p->pump(20, 500);

    CHECK(updates == 0);
}

TEST_CASE("CollabSession: a moved camera resumes sending after the idle period")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    p->client->setLocalPresence(kPos, kRot, {});
    p->pump(4);

    int updates = 0;
    p->host->onPresenceChanged([&](ParticipantId, const PresenceState&) { ++updates; });

    const float moved[3] = { 99.0f, 5.0f, -1.0f };
    p->client->setLocalPresence(moved, kRot, {});
    p->pump(4);

    CHECK(updates >= 1);
    const PresenceState* seen = p->host->presenceOf(p->client->localId());
    REQUIRE(seen != nullptr);
    CHECK(seen->cameraPos[0] == doctest::Approx(99.0f));
}

TEST_CASE("CollabSession: a selection change alone is enough to send")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    p->client->setLocalPresence(kPos, kRot, { 1 });
    p->pump(4);

    int updates = 0;
    p->host->onPresenceChanged([&](ParticipantId, const PresenceState&) { ++updates; });

    // Camera unchanged, but the user clicked something else — selection is
    // discrete, so any difference must propagate.
    p->client->setLocalPresence(kPos, kRot, { 7, 8 });
    p->pump(4);

    CHECK(updates >= 1);
    const PresenceState* seen = p->host->presenceOf(p->client->localId());
    REQUIRE(seen != nullptr);
    CHECK(seen->selection == std::vector<std::uint64_t>{ 7, 8 });
}

TEST_CASE("CollabSession: a client cannot publish presence as another participant")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    const ParticipantId hostId   = p->host->localId();
    const ParticipantId clientId = p->client->localId();

    // Forge a presence update. The client→host message carries no id at all, so
    // even a hand-built frame cannot claim to be the host — the host stamps the
    // identity from the connection.
    BitWriter forged;
    forged.writeFloat(1000.0f); forged.writeFloat(1000.0f); forged.writeFloat(1000.0f);
    for (int i = 0; i < 4; ++i) forged.writeFloatQuantized(0.0f, -1.0f, 1.0f, 16);
    forged.writeUInt16(0);
    p->clientNet->send(LoopbackTransport::kPeer, kFirstUserMessage + 8, forged,
                       SendMode::Unreliable);

    p->pump(4);

    // It landed on the client's own record, not the host's.
    const PresenceState* asClient = p->host->presenceOf(clientId);
    REQUIRE(asClient != nullptr);
    CHECK(asClient->cameraPos[0] == doctest::Approx(1000.0f));

    const PresenceState* asHost = p->host->presenceOf(hostId);
    if (asHost) CHECK(asHost->cameraPos[0] != doctest::Approx(1000.0f));
}

TEST_CASE("CollabSession: presence disappears when a participant leaves")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    p->client->setLocalPresence(kPos, kRot, { 5 });
    p->pump(4);

    const ParticipantId clientId = p->client->localId();
    REQUIRE(p->host->presenceOf(clientId) != nullptr);

    p->clientT.reset();
    for (int i = 0; i < 6; ++i) {
        p->hostT->update();
        p->hostNet->pump();
        p->host->update(p->nowMs);
        p->nowMs += 100;
    }

    // Otherwise their camera gizmo would hang in everyone's viewport forever.
    CHECK(p->host->presenceOf(clientId) == nullptr);
}

TEST_CASE("CollabSession: an oversized selection is clamped, not trusted")
{
    CollabSession::Config small;
    small.maxSelectionIds = 4;
    auto p = makePair("Anna", "Bob", small);
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    std::vector<std::uint64_t> huge(100);
    for (std::size_t i = 0; i < huge.size(); ++i) huge[i] = i;
    p->client->setLocalPresence(kPos, kRot, huge);
    p->pump(4);

    // Clamped locally before it ever reaches the wire.
    const PresenceState* local = p->client->presenceOf(p->client->localId());
    REQUIRE(local != nullptr);
    CHECK(local->selection.size() == 4);
}

// ─── Locks ───────────────────────────────────────────────────────────────────

TEST_CASE("CollabSession: a client's lock request is granted by the host")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    constexpr std::uint64_t kSubject = 4242;

    bool clientGotIt = false;
    p->client->onLockChanged([&](const LockInfo& l, bool acquired) {
        if (l.subject == kSubject && acquired) clientGotIt = true;
    });

    REQUIRE(p->client->requestLock(kSubject));
    p->pump(4);

    CHECK(clientGotIt);
    CHECK(p->client->ownsLock(kSubject));

    // The host's table is authoritative, so it must agree.
    const LockInfo* onHost = p->host->lockFor(kSubject);
    REQUIRE(onHost != nullptr);
    CHECK(onHost->owner == p->client->localId());
    CHECK(onHost->ownerName == "Bob");
}

TEST_CASE("CollabSession: a second claim on the same subject is refused")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    constexpr std::uint64_t kSubject = 99;

    // The host takes it first.
    REQUIRE(p->host->requestLock(kSubject));
    p->pump(4);
    CHECK(p->host->ownsLock(kSubject));

    LockDenyReason denied = LockDenyReason::None;
    p->client->onLockDenied([&](std::uint64_t s, LockDenyReason why) {
        if (s == kSubject) denied = why;
    });

    p->client->requestLock(kSubject);
    p->pump(4);

    // This is the whole point of the table: the second person is told no rather
    // than both of them believing they may edit.
    CHECK(denied == LockDenyReason::HeldByOther);
    CHECK_FALSE(p->client->ownsLock(kSubject));
    CHECK(p->host->lockFor(kSubject)->owner == p->host->localId());
}

TEST_CASE("CollabSession: everyone sees who holds a lock")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kSubject = 7;
    REQUIRE(p->client->requestLock(kSubject));
    p->pump(4);

    // The host must be able to name the holder in its UI even though it is not
    // the owner.
    const LockInfo* seenByHost = p->host->lockFor(kSubject);
    REQUIRE(seenByHost != nullptr);
    CHECK(seenByHost->ownerName == "Bob");
    CHECK_FALSE(p->host->ownsLock(kSubject));
}

TEST_CASE("CollabSession: releasing frees the subject for someone else")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kSubject = 55;
    REQUIRE(p->client->requestLock(kSubject));
    p->pump(4);
    REQUIRE(p->client->ownsLock(kSubject));

    p->client->releaseLock(kSubject);
    p->pump(4);

    CHECK(p->client->lockFor(kSubject) == nullptr);
    CHECK(p->host->lockFor(kSubject) == nullptr);

    // And now the other side can take it.
    REQUIRE(p->host->requestLock(kSubject));
    p->pump(4);
    CHECK(p->host->ownsLock(kSubject));
}

TEST_CASE("CollabSession: a participant cannot release a lock it does not hold")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kSubject = 1234;
    REQUIRE(p->host->requestLock(kSubject));
    p->pump(4);

    // Otherwise anyone could free someone else's lock and edit underneath them.
    p->client->releaseLock(kSubject);
    p->pump(4);

    const LockInfo* still = p->host->lockFor(kSubject);
    REQUIRE(still != nullptr);
    CHECK(still->owner == p->host->localId());
}

TEST_CASE("CollabSession: locks are freed when their holder leaves")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kSubject = 31337;
    REQUIRE(p->client->requestLock(kSubject));
    p->pump(4);
    REQUIRE(p->host->lockFor(kSubject) != nullptr);

    p->clientT.reset();   // the holder disappears

    for (int i = 0; i < 8; ++i) {
        p->hostT->update();
        p->hostNet->pump();
        p->host->update(p->nowMs);
        p->nowMs += 100;
    }

    // Otherwise their locks would block the session for everyone, forever.
    CHECK(p->host->lockFor(kSubject) == nullptr);
}

TEST_CASE("CollabSession: a joiner receives the locks that already exist")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);

    // The host takes a lock BEFORE anyone joins.
    p->pump(2);
    constexpr std::uint64_t kSubject = 808;
    REQUIRE(p->host->requestLock(kSubject));

    p->pump(8);
    REQUIRE(p->client->isJoined());

    // A late arrival that believed everything was free would immediately collide
    // with whoever is already editing.
    const LockInfo* seen = p->client->lockFor(kSubject);
    REQUIRE(seen != nullptr);
    CHECK(seen->owner == p->host->localId());
    CHECK_FALSE(p->client->ownsLock(kSubject));
}

TEST_CASE("CollabSession: re-requesting a lock you already hold is not a denial")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kSubject = 12;
    REQUIRE(p->client->requestLock(kSubject));
    p->pump(4);

    LockDenyReason denied = LockDenyReason::None;
    p->client->onLockDenied([&](std::uint64_t, LockDenyReason why) { denied = why; });

    // Re-selecting the same object must not report a conflict with yourself.
    p->client->requestLock(kSubject);
    p->pump(4);

    CHECK(denied == LockDenyReason::None);
    CHECK(p->client->ownsLock(kSubject));
}

// ─── Live transform deltas ───────────────────────────────────────────────────

TEST_CASE("CollabSession: a transform change reaches the other side")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    constexpr std::uint64_t kEntity = 77;
    REQUIRE(p->client->requestLock(kEntity));
    p->pump(4);
    REQUIRE(p->client->ownsLock(kEntity));

    CollabSession::TransformDelta received;
    bool got = false;
    p->host->onTransform([&](ParticipantId, const CollabSession::TransformDelta& d) {
        received = d;
        got = true;
    });

    CollabSession::TransformDelta sent;
    sent.subject = kEntity;
    sent.position[0] = 12.5f; sent.position[1] = -3.25f; sent.position[2] = 100.0f;
    sent.rotation[0] = 45.0f; sent.rotation[1] = 270.0f; sent.rotation[2] = -90.0f;
    sent.scale[0] = 2.0f; sent.scale[1] = 0.5f; sent.scale[2] = 1.0f;
    REQUIRE(p->client->sendTransform(sent));

    p->pump(4);
    REQUIRE(got);

    CHECK(received.subject == kEntity);
    CHECK(received.position[0] == doctest::Approx(12.5f));
    CHECK(received.position[2] == doctest::Approx(100.0f));
    // Euler angles run past 180°, which is exactly why they are sent at full
    // precision instead of quantized to [-1,1] like a unit quaternion.
    CHECK(received.rotation[1] == doctest::Approx(270.0f));
    CHECK(received.rotation[2] == doctest::Approx(-90.0f));
    CHECK(received.scale[1] == doctest::Approx(0.5f));
}

TEST_CASE("CollabSession: moving something you do not hold is refused")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kEntity = 88;
    // The host takes the lock, the client tries to move it anyway.
    REQUIRE(p->host->requestLock(kEntity));
    p->pump(4);

    bool hostSawChange = false;
    p->host->onTransform([&](ParticipantId, const CollabSession::TransformDelta&) {
        hostSawChange = true;
    });

    CollabSession::TransformDelta sneaky;
    sneaky.subject = kEntity;
    sneaky.position[0] = 999.0f;
    CHECK_FALSE(p->client->sendTransform(sneaky));   // refused locally...

    p->pump(4);
    CHECK_FALSE(hostSawChange);                      // ...and nothing arrived
}

TEST_CASE("CollabSession: a forged transform is rejected by the host, not merely by the sender")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kEntity = 91;
    REQUIRE(p->host->requestLock(kEntity));
    p->pump(4);

    bool hostSawChange = false;
    p->host->onTransform([&](ParticipantId, const CollabSession::TransformDelta&) {
        hostSawChange = true;
    });

    // Hand-build the frame, bypassing the local ownsLock() guard entirely — the
    // host must re-check authority rather than trust what arrives.
    BitWriter w;
    w.writeUInt64(kEntity);
    for (int i = 0; i < 3; ++i) w.writeFloat(999.0f);
    for (int i = 0; i < 3; ++i) w.writeFloat(0.0f);
    for (int i = 0; i < 3; ++i) w.writeFloat(1.0f);
    p->clientNet->send(LoopbackTransport::kPeer, kFirstUserMessage + 15, w,
                       SendMode::Unreliable);

    p->pump(4);
    CHECK_FALSE(hostSawChange);
}

TEST_CASE("CollabSession: the host's own move reaches clients")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kEntity = 5;
    REQUIRE(p->host->requestLock(kEntity));
    p->pump(4);

    bool got = false;
    CollabSession::TransformDelta received;
    p->client->onTransform([&](ParticipantId, const CollabSession::TransformDelta& d) {
        received = d;
        got = true;
    });

    CollabSession::TransformDelta d;
    d.subject = kEntity;
    d.position[1] = 42.0f;
    REQUIRE(p->host->sendTransform(d));

    p->pump(4);
    REQUIRE(got);
    CHECK(received.position[1] == doctest::Approx(42.0f));
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
