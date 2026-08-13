#include "doctest.h"

#include <Net/CollabSession.h>
#include <Net/LanBeacon.h>
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
    int  captureCalls = 0;

    bool captureSnapshot(std::vector<std::uint8_t>& out) override {
        ++captureCalls;
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
                               CollabSession::Config clientCfg = {},
                               CollabSession::Config hostCfg = {}) {
    auto p = std::make_unique<Pair>();
    auto [a, b] = LoopbackTransport::createPair();
    p->hostT   = std::move(a);
    p->clientT = std::move(b);

    p->hostNet   = std::make_unique<NetSession>(p->hostT.get(), NetRole::Host);
    p->clientNet = std::make_unique<NetSession>(p->clientT.get(), NetRole::Client);

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

// A square RGBA portrait with recognisable content, so a test can tell one
// participant's picture from another's rather than merely counting bytes.
Avatar makeAvatar(std::uint16_t size, std::uint8_t seed) {
    Avatar a;
    a.size = size;
    a.rgba.resize(static_cast<std::size_t>(size) * size * 4u);
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        a.rgba[i] = static_cast<std::uint8_t>((i * 13 + seed) & 0xFF);
    }
    return a;
}

// A join request written by hand, the way a peer that is not a CollabSession
// would send it. Several tests need to knock on the host's door more than once
// over the same link (a reconnect after a kick or a ban), which a client session
// object deliberately refuses to do.
// `syncLargeAssets` is last and defaults to false, which is what an editor with
// the setting off sends — the ordinary case, and the one every test that does
// not care about this flag wants. It is a trailing parameter for the same
// reason it is last on the wire: it was added afterwards, and none of the calls
// that predate it should have had to change.
void writeJoinRequest(BitWriter& w, const std::string& name,
                      const std::string& projectKey = {},
                      const std::string& clientKey  = {},
                      std::uint16_t avatarSize = 0,
                      ParticipantColor wish = {},
                      bool syncLargeAssets = false) {
    w.writeUInt16(kCollabProtocolVersion);
    w.writeString(name);
    w.writeString(projectKey);
    w.writeString(clientKey);
    w.writeUInt16(avatarSize);
    const std::size_t bytes = static_cast<std::size_t>(avatarSize) * avatarSize * 4u;
    for (std::size_t i = 0; i < bytes; ++i) w.writeByte(static_cast<std::uint8_t>(i & 0xFF));
    w.writeByte(wish.r);
    w.writeByte(wish.g);
    w.writeByte(wish.b);
    w.writeBool(syncLargeAssets);
}

int colorDistSq(const ParticipantColor& a, const ParticipantColor& b) {
    const int dr = int(a.r) - int(b.r), dg = int(a.g) - int(b.g), db = int(a.b) - int(b.b);
    return dr * dr + dg * dg + db * db;
}

// The colour a participant ended up with, by name.
ParticipantColor colorOf(const CollabSession& s, const std::string& name) {
    for (const auto& p : s.participants()) {
        if (p.name == name) return p.color;
    }
    return {};
}

constexpr MessageId kIdJoinRequest  = kFirstUserMessage + 0;
constexpr MessageId kIdJoinRejected = kFirstUserMessage + 2;

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
    p->client->onJoinRejected([&](JoinRejectReason r, const std::string&) { reason = r; });

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
    p->client->onJoinRejected([&](JoinRejectReason r, const std::string&) { reason = r; });

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
    p->client->onJoinRejected([&](JoinRejectReason r, const std::string&) { reason = r; });

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
    writeJoinRequest(w, "Latecomer");
    clientNet.send(LoopbackTransport::kPeer, kIdJoinRequest, w);

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

TEST_CASE("CollabSession: an idle editor drops to keep-alive cadence")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    p->client->setLocalPresence(kPos, kRot, {});
    p->pump(4);

    int updates = 0;
    p->host->onPresenceChanged([&](ParticipantId, const PresenceState&) { ++updates; });

    // Plenty of time passes, but nothing moves. Not silence — a rare keep-alive
    // still goes out (late joiners and lost relays need it) — but nothing like
    // the 10 Hz live rate: 10 s of idling at the 2 s keep-alive is ~5 sends,
    // where 10 Hz would be 100.
    p->pump(20, 500);

    CHECK(updates >= 3);
    CHECK(updates <= 8);
}

TEST_CASE("CollabSession: a late joiner receives existing presence without waiting")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);

    // The host's camera is somewhere BEFORE the client finishes joining, and
    // never moves again afterwards. Presence is change-driven, so without the
    // join-time push the client would stare at an empty scene until the host
    // happened to touch the mouse — the two-instance field test did exactly
    // that: each side wondering where the other one was.
    const float hostPos[3] = { 7.0f, 8.0f, 9.0f };
    p->host->setLocalPresence(hostPos, kRot, {});

    p->pump();
    REQUIRE(p->client->isJoined());

    const PresenceState* seen = p->client->presenceOf(p->host->localId());
    REQUIRE(seen != nullptr);
    CHECK(seen->valid);
    CHECK(seen->cameraPos[0] == doctest::Approx(7.0f));
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

// ─── Authored-asset sync ─────────────────────────────────────────────────────

TEST_CASE("CollabSession: a saved asset reaches the other side intact")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'1234ull;
    REQUIRE(p->client->requestLock(kAsset));
    p->pump(4);

    CollabSession::AssetUpdate got;
    bool arrived = false;
    p->host->onAssetUpdated([&](ParticipantId, const CollabSession::AssetUpdate& a) {
        got = a;
        arrived = true;
    });

    const auto payload = makeBlob(3000);
    REQUIRE(p->client->sendAsset(kAsset, "Content/Materials/Steel.hmat", payload));
    p->pump(6);

    REQUIRE(arrived);
    CHECK(got.subject == kAsset);
    CHECK(got.path == "Content/Materials/Steel.hmat");
    CHECK(got.bytes == payload);   // byte-for-byte, or the asset would be corrupt
    // An ordinary save says so on the wire (v7). It matters that this is the
    // DEFAULT and not merely the common case: a receiver acts differently on a
    // create, and a frame that forgot to say would be taken for one.
    CHECK(got.intent == CollabSession::AssetIntent::Update);
}

TEST_CASE("v7: the asset header's intent survives the round trip, and a bad one is refused")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'4321ull;
    REQUIRE(p->client->requestLock(kAsset));
    p->pump(4);

    // The byte sits IN FRONT of the subject, which is exactly why v6 and v7
    // cannot talk: a v6 reader would fold it into the subject's top byte and
    // then arbitrate a lock nobody holds. Here it simply has to survive.
    CollabSession::AssetUpdate got;
    bool arrived = false;
    p->host->onAssetUpdated([&](ParticipantId, const CollabSession::AssetUpdate& a) {
        got = a;
        arrived = true;
    });

    const auto payload = makeBlob(64);
    CollabSession::AssetUpdate out;
    out.subject = kAsset;
    out.path    = "Content/Materials/New.hasset";
    out.bytes   = payload;
    out.intent  = CollabSession::AssetIntent::Create;
    REQUIRE(p->client->sendAsset(out));
    p->pump(6);

    REQUIRE(arrived);
    CHECK(got.intent == CollabSession::AssetIntent::Create);
    CHECK(got.subject == kAsset);
    CHECK(got.bytes == payload);
}

TEST_CASE("A create the host refuses is not applied, and the creator is told why")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    // The host says the name is taken and offers a free one. This is the whole
    // reason a create is arbitrated instead of relayed: only the host can
    // answer "is this name free", and the answer has to reach the creator.
    p->host->setCreatePolicy([](const CollabSession::AssetUpdate& a,
                                CollabSession::AssetRejectReason& reason,
                                std::string& suggested) {
        if (a.path == "Content/Materials/Taken.hasset") {
            reason    = CollabSession::AssetRejectReason::NameTaken;
            suggested = "Content/Materials/Taken2.hasset";
            return false;
        }
        return true;
    });

    bool applied = false;
    p->host->onAssetUpdated([&](ParticipantId, const CollabSession::AssetUpdate&) {
        applied = true;
    });

    std::string  gotPath, gotSuggested;
    bool         gotAccepted = true;
    auto         gotReason = CollabSession::AssetRejectReason::None;
    bool         answered = false;
    p->client->onAssetCreateResult([&](const std::string& path, bool accepted,
                                       CollabSession::AssetRejectReason reason,
                                       const std::string& suggested) {
        gotPath = path; gotAccepted = accepted; gotReason = reason;
        gotSuggested = suggested; answered = true;
    });

    CollabSession::AssetUpdate out;
    out.subject = 0x8000'0000'0000'9999ull;
    out.path    = "Content/Materials/Taken.hasset";
    out.bytes   = makeBlob(32);
    out.intent  = CollabSession::AssetIntent::Create;
    REQUIRE(p->client->sendAsset(out));
    p->pump(6);

    REQUIRE(answered);
    CHECK_FALSE(gotAccepted);
    CHECK(gotReason == CollabSession::AssetRejectReason::NameTaken);
    CHECK(gotSuggested == "Content/Materials/Taken2.hasset");
    CHECK(gotPath == "Content/Materials/Taken.hasset");
    // And nothing was written anywhere. Half the session holding a file the
    // other half refused is the state this ordering exists to prevent.
    CHECK_FALSE(applied);
}

TEST_CASE("An accepted create reaches everyone and the creator hears that it did")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    // No policy installed at all: a host with no opinion accepts, rather than
    // silently swallowing every create.
    bool applied = false;
    CollabSession::AssetUpdate seen;
    p->host->onAssetUpdated([&](ParticipantId, const CollabSession::AssetUpdate& a) {
        seen = a; applied = true;
    });

    bool accepted = false, answered = false;
    p->client->onAssetCreateResult([&](const std::string&, bool ok,
                                       CollabSession::AssetRejectReason,
                                       const std::string&) {
        accepted = ok; answered = true;
    });

    CollabSession::AssetUpdate out;
    out.subject = 0x8000'0000'0000'aaaaull;
    out.path    = "Content/Materials/Fresh.hasset";
    out.bytes   = makeBlob(48);
    out.intent  = CollabSession::AssetIntent::Create;
    REQUIRE(p->client->sendAsset(out));
    p->pump(6);

    REQUIRE(applied);
    CHECK(seen.intent == CollabSession::AssetIntent::Create);
    CHECK(seen.path == "Content/Materials/Fresh.hasset");
    REQUIRE(answered);
    CHECK(accepted);
}

TEST_CASE("CollabSession: a large asset survives chunking")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'5555ull;
    REQUIRE(p->client->requestLock(kAsset));
    p->pump(4);

    CollabSession::AssetUpdate got;
    bool arrived = false;
    p->host->onAssetUpdated([&](ParticipantId, const CollabSession::AssetUpdate& a) {
        got = a; arrived = true;
    });

    // Several chunks' worth — a graph-heavy scene or widget can reach this.
    const auto payload = makeBlob(800 * 1024);
    REQUIRE(p->client->sendAsset(kAsset, "Content/UI/Menu.huiw", payload));
    p->pump(30);

    REQUIRE(arrived);
    CHECK(got.bytes.size() == payload.size());
    CHECK(got.bytes == payload);
}

TEST_CASE("CollabSession: saving an asset someone else holds is refused")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'7777ull;
    REQUIRE(p->host->requestLock(kAsset));
    p->pump(4);

    bool arrived = false;
    p->host->onAssetUpdated([&](ParticipantId, const CollabSession::AssetUpdate&) {
        arrived = true;
    });

    CHECK_FALSE(p->client->sendAsset(kAsset, "Content/Materials/Steel.hmat",
                                     makeBlob(128)));
    p->pump(6);
    CHECK_FALSE(arrived);
}

TEST_CASE("CollabSession: the host's asset save reaches clients")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'0001ull;
    REQUIRE(p->host->requestLock(kAsset));
    p->pump(4);

    CollabSession::AssetUpdate got;
    bool arrived = false;
    p->client->onAssetUpdated([&](ParticipantId, const CollabSession::AssetUpdate& a) {
        got = a; arrived = true;
    });

    const auto payload = makeBlob(2048);
    REQUIRE(p->host->sendAsset(kAsset, "Content/Scripts/Player.hcode", payload));
    p->pump(8);

    REQUIRE(arrived);
    CHECK(got.path == "Content/Scripts/Player.hcode");
    CHECK(got.bytes == payload);
}

// ─── Structural changes ──────────────────────────────────────────────────────

TEST_CASE("CollabSession: a created entity reaches the other side with its subtree")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    CollabSession::StructuralChange got;
    bool arrived = false;
    p->host->onStructural([&](ParticipantId, const CollabSession::StructuralChange& c) {
        got = c; arrived = true;
    });

    CollabSession::StructuralChange c;
    c.kind      = CollabSession::StructuralChange::Kind::Created;
    c.netId     = (2ull << 32) | 1ull;   // participant-scoped id
    c.parentNet = 5;
    c.blob      = makeBlob(512);
    REQUIRE(p->client->sendStructural(c));

    p->pump(4);
    REQUIRE(arrived);
    CHECK(got.kind == CollabSession::StructuralChange::Kind::Created);
    CHECK(got.netId == c.netId);
    CHECK(got.parentNet == 5);
    CHECK(got.blob == c.blob);   // the subtree must survive byte-for-byte
}

TEST_CASE("CollabSession: creating needs no lock, but destroying does")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kEntity = 42;

    // A brand new entity cannot be held by anyone, so creating is always allowed.
    CollabSession::StructuralChange create;
    create.kind  = CollabSession::StructuralChange::Kind::Created;
    create.netId = (2ull << 32) | 7ull;
    create.blob  = makeBlob(64);
    CHECK(p->client->sendStructural(create));

    // Destroying something the host is editing would yank it out from under them.
    REQUIRE(p->host->requestLock(kEntity));
    p->pump(4);

    bool hostSaw = false;
    p->host->onStructural([&](ParticipantId, const CollabSession::StructuralChange&) {
        hostSaw = true;
    });

    CollabSession::StructuralChange destroy;
    destroy.kind  = CollabSession::StructuralChange::Kind::Destroyed;
    destroy.netId = kEntity;
    CHECK_FALSE(p->client->sendStructural(destroy));

    p->pump(4);
    CHECK_FALSE(hostSaw);
}

TEST_CASE("CollabSession: destroying an entity frees its lock")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kEntity = 314;
    REQUIRE(p->client->requestLock(kEntity));
    p->pump(4);
    REQUIRE(p->host->lockFor(kEntity) != nullptr);

    CollabSession::StructuralChange destroy;
    destroy.kind  = CollabSession::StructuralChange::Kind::Destroyed;
    destroy.netId = kEntity;
    REQUIRE(p->client->sendStructural(destroy));
    p->pump(4);

    // Otherwise the table would keep blocking a subject that no longer exists.
    CHECK(p->host->lockFor(kEntity) == nullptr);
}

TEST_CASE("CollabSession: reparenting travels with its new parent")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    CollabSession::StructuralChange got;
    bool arrived = false;
    p->client->onStructural([&](ParticipantId, const CollabSession::StructuralChange& c) {
        got = c; arrived = true;
    });

    CollabSession::StructuralChange c;
    c.kind      = CollabSession::StructuralChange::Kind::Reparented;
    c.netId     = 11;
    c.parentNet = 22;
    REQUIRE(p->host->sendStructural(c));

    p->pump(4);
    REQUIRE(arrived);
    CHECK(got.kind == CollabSession::StructuralChange::Kind::Reparented);
    CHECK(got.netId == 11);
    CHECK(got.parentNet == 22);
}

TEST_CASE("CollabSession: a malformed structural frame is rejected")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    bool hostSaw = false;
    p->host->onStructural([&](ParticipantId, const CollabSession::StructuralChange&) {
        hostSaw = true;
    });

    // An out-of-range kind, and a blob length far past what we would accept.
    BitWriter w;
    w.writeByte(99);
    w.writeUInt64(1);
    w.writeUInt64(0);
    w.writeUInt32(0xFFFFFFFFu);
    p->clientNet->send(LoopbackTransport::kPeer, kFirstUserMessage + 20, w);

    p->pump(4);
    CHECK_FALSE(hostSaw);
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

// ─── Document deltas ─────────────────────────────────────────────────────────
// AssetUpdate replicates a whole file; these replicate one item inside one, so
// an open graph/UI editor can be patched instead of reloaded.

namespace {
CollabSession::DocDelta upsert(std::uint8_t kind, std::int64_t id, std::string json)
{
    CollabSession::DocDelta d;
    d.kind = kind; d.op = 0; d.itemId = id; d.json = std::move(json);
    return d;
}
} // namespace

TEST_CASE("CollabSession: a document delta batch reaches the other side intact")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'2001ull;
    REQUIRE(p->client->requestLock(kAsset));
    p->pump(4);

    std::vector<CollabSession::DocDelta> got;
    std::uint64_t gotSubject = 0;
    std::string   gotPath;
    p->host->onDocDeltas([&](ParticipantId, std::uint64_t s, const std::string& path,
                             const std::vector<CollabSession::DocDelta>& batch, std::uint32_t) {
        gotSubject = s; gotPath = path; got = batch;
    });

    std::vector<CollabSession::DocDelta> batch;
    batch.push_back(upsert(/*kind Node*/ 0, 7, R"({"id":7,"type":"Add"})"));
    batch.push_back(upsert(/*kind Link*/ 1, 0x0007'0000'0009'0001ll, "[7,0,9,1]"));
    CollabSession::DocDelta rm;
    rm.kind = 0; rm.op = 1; rm.itemId = 3;      // a removal carries no payload
    batch.push_back(rm);
    batch[1].scope = 1;                          // the widget's logic graph

    REQUIRE(p->client->sendDocDeltas(kAsset, "Content/UI/Menu.hasset", batch, 1));
    p->pump(6);

    REQUIRE(got.size() == 3);
    CHECK(gotSubject == kAsset);
    CHECK(gotPath == "Content/UI/Menu.hasset");
    CHECK(got[0].kind == 0);
    CHECK(got[0].itemId == 7);
    CHECK(got[0].json == R"({"id":7,"type":"Add"})");
    CHECK(got[1].scope == 1);
    CHECK(got[1].itemId == 0x0007'0000'0009'0001ll);
    CHECK(got[2].op == 1);
    CHECK(got[2].itemId == 3);
    CHECK(got[2].json.empty());
}

TEST_CASE("CollabSession: editing a document you do not hold is refused")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'2002ull;
    REQUIRE(p->host->requestLock(kAsset));       // the HOST holds it
    p->pump(4);

    bool arrived = false;
    p->host->onDocDeltas([&](ParticipantId, std::uint64_t, const std::string&,
                             const std::vector<CollabSession::DocDelta>&, std::uint32_t) {
        arrived = true;
    });

    std::vector<CollabSession::DocDelta> batch{ upsert(0, 1, "{}") };
    CHECK_FALSE(p->client->sendDocDeltas(kAsset, "Content/M.hasset", batch, 1));
    p->pump(6);
    CHECK_FALSE(arrived);
}

TEST_CASE("CollabSession: a forged document delta is rejected by the host, not merely by the sender")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'2003ull;
    REQUIRE(p->host->requestLock(kAsset));
    p->pump(4);

    bool hostSaw = false;
    p->host->onDocDeltas([&](ParticipantId, std::uint64_t, const std::string&,
                             const std::vector<CollabSession::DocDelta>&, std::uint32_t) {
        hostSaw = true;
    });

    // Bypass sendDocDeltas entirely — a peer that skips its own guard must still
    // be stopped by the authority, or the lock means nothing.
    BitWriter w;
    w.writeUInt64(kAsset);
    w.writeString("Content/M.hasset");
    w.writeUInt16(1);
    w.writeByte(0); w.writeByte(0); w.writeByte(0);
    w.writeUInt64(1);
    w.writeString("{}");
    p->clientNet->send(LoopbackTransport::kPeer, kFirstUserMessage + 24, w);

    p->pump(4);
    CHECK_FALSE(hostSaw);
}

TEST_CASE("CollabSession: a truncated document delta frame is rejected whole")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'2004ull;
    REQUIRE(p->client->requestLock(kAsset));
    p->pump(4);

    bool hostSaw = false;
    p->host->onDocDeltas([&](ParticipantId, std::uint64_t, const std::string&,
                             const std::vector<CollabSession::DocDelta>&, std::uint32_t) {
        hostSaw = true;
    });

    // Announces three deltas and supplies one. Half a paste is a broken graph,
    // not a smaller one, so nothing at all may be applied.
    BitWriter w;
    w.writeUInt64(kAsset);
    w.writeString("Content/M.hasset");
    w.writeUInt16(3);
    w.writeByte(0); w.writeByte(0); w.writeByte(0);
    w.writeUInt64(1);
    w.writeString("{}");
    p->clientNet->send(LoopbackTransport::kPeer, kFirstUserMessage + 24, w);

    p->pump(4);
    CHECK_FALSE(hostSaw);
}

TEST_CASE("CollabSession: an oversized item is refused rather than truncated")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'2005ull;
    REQUIRE(p->client->requestLock(kAsset));
    p->pump(4);

    bool arrived = false;
    p->host->onDocDeltas([&](ParticipantId, std::uint64_t, const std::string&,
                             const std::vector<CollabSession::DocDelta>&, std::uint32_t) {
        arrived = true;
    });

    // The wire's string length prefix is 16-bit and truncates SILENTLY, which
    // would land as unparseable JSON on the peer. Refusing sends the editor to
    // its whole-file fallback instead.
    std::vector<CollabSession::DocDelta> batch{
        upsert(0, 1, std::string(200 * 1024, 'x')) };
    CHECK_FALSE(p->client->sendDocDeltas(kAsset, "Content/M.hasset", batch, 1));
    p->pump(4);
    CHECK_FALSE(arrived);
}

TEST_CASE("CollabSession: the host's own document edit reaches clients")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'2006ull;
    REQUIRE(p->host->requestLock(kAsset));
    p->pump(4);

    std::vector<CollabSession::DocDelta> got;
    p->client->onDocDeltas([&](ParticipantId, std::uint64_t, const std::string&,
                               const std::vector<CollabSession::DocDelta>& b, std::uint32_t) { got = b; });

    std::vector<CollabSession::DocDelta> batch{ upsert(0, 42, R"({"id":42})") };
    REQUIRE(p->host->sendDocDeltas(kAsset, "Content/M.hasset", batch, 1));
    p->pump(6);

    REQUIRE(got.size() == 1);
    CHECK(got[0].itemId == 42);
}

TEST_CASE("v9: the revision travels with a delta and with a whole file")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'2007ull;
    REQUIRE(p->host->requestLock(kAsset));
    p->pump(4);

    // Both channels carry the same document, so both have to carry the same
    // number — that is the only thing that lets a receiver order them against
    // each other. This is the wire half; REFUSING the older one is the editor's
    // job (CollabController::acceptRevision), and that is where "applied for a
    // moment, then snapped back" actually happened.
    std::uint32_t deltaRev = 0;
    p->client->onDocDeltas([&](ParticipantId, std::uint64_t, const std::string&,
                               const std::vector<CollabSession::DocDelta>&,
                               std::uint32_t rev) { deltaRev = rev; });

    std::vector<CollabSession::DocDelta> batch{ upsert(0, 7, R"({"id":7})") };
    REQUIRE(p->host->sendDocDeltas(kAsset, "Content/M.hasset", batch, 12));
    p->pump(6);
    CHECK(deltaRev == 12);

    CollabSession::AssetUpdate got;
    bool arrived = false;
    p->client->onAssetUpdated([&](ParticipantId, const CollabSession::AssetUpdate& a) {
        got = a; arrived = true;
    });

    CollabSession::AssetUpdate out;
    out.subject  = kAsset;
    out.path     = "Content/M.hasset";
    out.bytes    = makeBlob(128);
    out.revision = 11;          // an OLDER statement about the same document
    REQUIRE(p->host->sendAsset(out));
    p->pump(6);

    REQUIRE(arrived);
    CHECK(got.revision == 11);
    // The wire delivers it faithfully — deciding what is stale is not the
    // transport's place. It is the pair of numbers that makes the decision
    // possible at all, and before v9 neither frame carried one.
    CHECK(got.revision < deltaRev);
}

// ─── Lock query ──────────────────────────────────────────────────────────────

TEST_CASE("CollabSession: the host answers who holds a subject")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    constexpr std::uint64_t kFree = 0x8000'0000'0000'3001ull;
    constexpr std::uint64_t kHeld = 0x8000'0000'0000'3002ull;
    REQUIRE(p->host->requestLock(kHeld));
    p->pump(4);

    struct Answer { std::uint64_t subject; bool held; ParticipantId owner; std::string name; };
    std::vector<Answer> answers;
    p->client->onLockQueryResult([&](std::uint64_t s, bool held, ParticipantId o,
                                     const std::string& n) {
        answers.push_back({ s, held, o, n });
    });

    REQUIRE(p->client->queryLock(kFree));
    REQUIRE(p->client->queryLock(kHeld));
    p->pump(6);

    REQUIRE(answers.size() == 2);
    CHECK(answers[0].subject == kFree);
    CHECK_FALSE(answers[0].held);
    CHECK(answers[1].subject == kHeld);
    CHECK(answers[1].held);
    CHECK(answers[1].owner == p->host->localId());
}

TEST_CASE("CollabSession: the host answers its own lock query without a round trip")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    constexpr std::uint64_t kHeld = 0x8000'0000'0000'3003ull;
    REQUIRE(p->client->requestLock(kHeld));
    p->pump(4);

    bool answered = false;
    bool held = false;
    p->host->onLockQueryResult([&](std::uint64_t, bool h, ParticipantId,
                                   const std::string&) { answered = true; held = h; });

    // No pump between the call and the check: the host IS the table, so a tab
    // opening on the host must not have to wait a frame to know it may edit.
    REQUIRE(p->host->queryLock(kHeld));
    CHECK(answered);
    CHECK(held);
}

// ─── Project identity ────────────────────────────────────────────────────────
// A session addresses everything — scene entities, asset references, lock
// subjects — by uuids that only mean something inside ONE project. Joining a
// host who has a different project open used to succeed: the scene arrived and
// every asset reference in it dangled.

namespace {
// A pair whose two sides have different projects open. Same wiring as makePair,
// with a project identity on each side.
std::unique_ptr<Pair> makeProjectPair(const std::string& hostKey,
                                      const std::string& hostLabel,
                                      const std::string& clientKey)
{
    auto p = std::make_unique<Pair>();
    auto [a, b] = LoopbackTransport::createPair();
    p->hostT   = std::move(a);
    p->clientT = std::move(b);
    p->hostNet   = std::make_unique<NetSession>(p->hostT.get(),   NetRole::Host);
    p->clientNet = std::make_unique<NetSession>(p->clientT.get(), NetRole::Client);

    CollabSession::Config hostCfg;
    hostCfg.displayName  = "Anna";
    hostCfg.projectKey   = hostKey;
    hostCfg.projectLabel = hostLabel;
    CollabSession::Config clientCfg;
    clientCfg.displayName = "Bob";
    clientCfg.projectKey  = clientKey;

    p->host   = std::make_unique<CollabSession>(p->hostNet.get(),   NetRole::Host,   hostCfg);
    p->client = std::make_unique<CollabSession>(p->clientNet.get(), NetRole::Client, clientCfg);
    p->host->setStateProvider(&p->hostState);
    p->client->setStateProvider(&p->clientState);
    return p;
}
} // namespace

TEST_CASE("CollabSession: a peer with a different project open is refused, and told which")
{
    auto p = makeProjectPair("proj-aaa", "ShadowValidation", "proj-bbb");
    p->hostState.data = makeBlob(64);

    JoinRejectReason reason = JoinRejectReason::None;
    std::string      detail;
    p->client->onJoinRejected([&](JoinRejectReason r, const std::string& d) {
        reason = r; detail = d;
    });

    p->pump();

    CHECK_FALSE(p->client->isJoined());
    CHECK(reason == JoinRejectReason::ProjectMismatch);
    // The name matters as much as the refusal: without it the user is told "no"
    // and cannot tell which of their projects to open.
    CHECK(detail == "ShadowValidation");
    // And the scene never transferred — that is the damage being prevented.
    CHECK(p->clientState.applyCalls == 0);
    CHECK(p->host->participants().size() == 1);
}

TEST_CASE("CollabSession: the same project id joins normally")
{
    auto p = makeProjectPair("proj-aaa", "ShadowValidation", "proj-aaa");
    p->hostState.data = makeBlob(64);

    bool rejected = false;
    p->client->onJoinRejected([&](JoinRejectReason, const std::string&) { rejected = true; });

    p->pump();

    CHECK_FALSE(rejected);
    CHECK(p->client->isJoined());
    CHECK(p->host->participants().size() == 2);
}

TEST_CASE("CollabSession: two projectless editors still collaborate")
{
    // Nothing forces a project to have an id — one written by an older build has
    // none until it is loaded once. Two empty keys match, so this stays usable
    // rather than locking people out of their own session.
    auto p = makeProjectPair("", "", "");
    p->hostState.data = makeBlob(64);

    bool rejected = false;
    p->client->onJoinRejected([&](JoinRejectReason, const std::string&) { rejected = true; });

    p->pump();

    CHECK_FALSE(rejected);
    CHECK(p->client->isJoined());
}

// ─── Profile pictures ────────────────────────────────────────────────────────

TEST_CASE("CollabSession: profile pictures travel in both directions")
{
    CollabSession::Config clientCfg, hostCfg;
    clientCfg.avatar = makeAvatar(16, 7);
    hostCfg.avatar   = makeAvatar(16, 200);

    auto p = makePair("Anna", "Bob", clientCfg, hostCfg);
    p->hostState.data = makeBlob(64);
    p->pump();

    REQUIRE(p->client->isJoined());

    // The host received Bob's picture in the join handshake…
    const Participant* bobOnHost = nullptr;
    for (const auto& part : p->host->participants()) {
        if (part.name == "Bob") bobOnHost = &part;
    }
    REQUIRE(bobOnHost != nullptr);
    CHECK(bobOnHost->avatar.size == 16);
    CHECK(bobOnHost->avatar.rgba == clientCfg.avatar.rgba);

    // …and Bob received Anna's with the accepted roster.
    const Participant* annaOnClient = nullptr;
    for (const auto& part : p->client->participants()) {
        if (part.name == "Anna") annaOnClient = &part;
    }
    REQUIRE(annaOnClient != nullptr);
    CHECK(annaOnClient->avatar.size == 16);
    CHECK(annaOnClient->avatar.rgba == hostCfg.avatar.rgba);
}

TEST_CASE("CollabSession: your own roster entry carries your own picture")
{
    // The UI draws every participant, itself included, from one list — so the
    // local entry has to be as complete as everyone else's.
    CollabSession::Config clientCfg;
    clientCfg.avatar = makeAvatar(8, 3);

    auto p = makePair("Anna", "Bob", clientCfg);
    p->hostState.data = makeBlob(64);
    p->pump();

    const Participant* self = nullptr;
    for (const auto& part : p->client->participants()) {
        if (part.id == p->client->localId()) self = &part;
    }
    REQUIRE(self != nullptr);
    CHECK(self->avatar.rgba == clientCfg.avatar.rgba);
}

TEST_CASE("CollabSession: a participant without a picture joins normally")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    REQUIRE(p->client->isJoined());
    for (const auto& part : p->host->participants()) CHECK(part.avatar.empty());
}

TEST_CASE("CollabSession: an oversized picture is dropped, the join is not")
{
    // The two sides may legitimately disagree about how big a portrait may be.
    // Losing the picture is acceptable; losing the session over it is not.
    CollabSession::Config clientCfg, hostCfg;
    clientCfg.avatar        = makeAvatar(64, 5);
    clientCfg.maxAvatarSize = 64;
    hostCfg.maxAvatarSize   = 32;

    auto p = makePair("Anna", "Bob", clientCfg, hostCfg);
    p->hostState.data = makeBlob(64);
    p->pump();

    CHECK(p->client->isJoined());
    for (const auto& part : p->host->participants()) {
        if (part.name == "Bob") CHECK(part.avatar.empty());
    }
}

TEST_CASE("CollabSession: a picture the sender itself cannot carry is discarded")
{
    // 128² against a 64 limit: the session drops it at construction rather than
    // announcing a size it would refuse to accept from anyone else.
    CollabSession::Config clientCfg;
    clientCfg.avatar        = makeAvatar(128, 9);
    clientCfg.maxAvatarSize = 64;

    auto p = makePair("Anna", "Bob", clientCfg);
    p->hostState.data = makeBlob(64);
    p->pump();

    CHECK(p->client->isJoined());
    for (const auto& part : p->host->participants()) CHECK(part.avatar.empty());
}

TEST_CASE("CollabSession: an absurd picture size is refused without allocating it")
{
    // 4096² RGBA would be 64 MiB sized from a two-byte claim. The frame does not
    // contain those bytes, and the point is that nothing tries to reserve them.
    auto [a, b] = LoopbackTransport::createPair();
    NetSession hostNet(a.get(), NetRole::Host);
    NetSession clientNet(b.get(), NetRole::Client);

    FakeState hostState;
    CollabSession host(&hostNet, NetRole::Host);
    host.setStateProvider(&hostState);

    a->update(); b->update();
    hostNet.pump(); clientNet.pump();

    BitWriter w;
    w.writeUInt16(kCollabProtocolVersion);
    w.writeString("Bomber");
    w.writeString("");
    w.writeString("bomber-key");
    w.writeUInt16(4096);          // …and no pixels behind it
    clientNet.send(LoopbackTransport::kPeer, kIdJoinRequest, w);

    for (int i = 0; i < 6; ++i) {
        a->update(); b->update();
        hostNet.pump(); clientNet.pump();
    }

    CHECK(host.participants().size() == 1);   // nobody admitted
}

// ─── Kick and ban ────────────────────────────────────────────────────────────

TEST_CASE("CollabSession: the host kicks a participant, who is told why")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->host->participants().size() == 2);

    ParticipantId bob = kInvalidParticipant;
    for (const auto& part : p->host->participants()) {
        if (!part.isHost) bob = part.id;
    }
    REQUIRE(bob != kInvalidParticipant);

    int  removedCalls = 0;
    auto why = RemovalReason::Banned;
    p->client->onRemoved([&](RemovalReason r) { ++removedCalls; why = r; });

    std::string ejectedName;
    p->host->onParticipantRemoved([&](const Participant& part, RemovalReason) {
        ejectedName = part.name;
    });

    CHECK(p->host->kickParticipant(bob));

    // The host's roster drops them at once — it does not wait for the link.
    CHECK(p->host->participants().size() == 1);
    CHECK(ejectedName == "Bob");

    p->pump();

    CHECK(removedCalls == 1);
    CHECK(why == RemovalReason::Kicked);
    // A kick is not a ban: nothing is recorded against them.
    CHECK(p->host->bans().empty());
}

TEST_CASE("CollabSession: a kicked participant's locks are freed")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    p->client->requestLock(4242);
    p->pump();
    REQUIRE(p->host->lockFor(4242) != nullptr);

    ParticipantId bob = kInvalidParticipant;
    for (const auto& part : p->host->participants()) {
        if (!part.isHost) bob = part.id;
    }
    p->host->kickParticipant(bob);

    // Whatever they were editing has to become editable again, or the session
    // stays blocked on someone who is no longer in it.
    CHECK(p->host->lockFor(4242) == nullptr);
}

TEST_CASE("CollabSession: the link to a kicked participant is closed")
{
    CollabSession::Config hostCfg;
    hostCfg.removalGraceMs = 100;   // the pump steps a second at a time

    auto p = makePair("Anna", "Bob", {}, hostCfg);
    p->hostState.data = makeBlob(64);
    p->pump();

    ParticipantId bob = kInvalidParticipant;
    for (const auto& part : p->host->participants()) {
        if (!part.isHost) bob = part.id;
    }
    p->host->kickParticipant(bob);
    p->pump();

    // Told first, cut loose after — the client saw the reason before the link
    // went down, which is the whole reason for the grace period.
    CHECK_FALSE(p->client->isJoined());
    CHECK(p->hostT->connectionCount() == 0);
}

TEST_CASE("CollabSession: a client cannot kick, and the host cannot kick itself")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    ParticipantId anna = kInvalidParticipant, bob = kInvalidParticipant;
    for (const auto& part : p->host->participants()) {
        (part.isHost ? anna : bob) = part.id;
    }

    CHECK_FALSE(p->client->kickParticipant(anna));   // not the authority
    CHECK_FALSE(p->client->banParticipant(anna));
    CHECK_FALSE(p->host->kickParticipant(anna));     // itself
    CHECK_FALSE(p->host->kickParticipant(kInvalidParticipant));
    CHECK(p->host->participants().size() == 2);
    CHECK(p->host->bans().empty());

    // The one call that IS legitimate still works.
    CHECK(p->host->kickParticipant(bob));
}

TEST_CASE("CollabSession: a ban refuses the same client when it comes back")
{
    // Driven by hand: a CollabSession client sends exactly one join request per
    // link by design, and the point here is the SECOND one.
    auto [a, b] = LoopbackTransport::createPair();
    NetSession hostNet(a.get(), NetRole::Host);
    NetSession clientNet(b.get(), NetRole::Client);

    FakeState hostState;
    CollabSession::Config hostCfg;
    // Long enough that the link survives the ban, so the reconnect can be tested
    // on the same transport pair.
    hostCfg.removalGraceMs = 1000 * 1000;
    CollabSession host(&hostNet, NetRole::Host, hostCfg);
    host.setStateProvider(&hostState);

    JoinRejectReason reason = JoinRejectReason::None;
    clientNet.on(kIdJoinRejected, [&](ConnectionId, BitReader& r) {
        std::uint8_t code = 0;
        r.readByte(code);
        reason = static_cast<JoinRejectReason>(code);
    });

    const auto step = [&] {
        for (int i = 0; i < 6; ++i) {
            a->update(); b->update();
            hostNet.pump(); clientNet.pump();
            host.update(0);
        }
    };

    step();

    BitWriter first;
    writeJoinRequest(first, "Bob", "", "bob-install-key");
    clientNet.send(LoopbackTransport::kPeer, kIdJoinRequest, first);
    step();

    REQUIRE(host.participants().size() == 2);
    ParticipantId bob = kInvalidParticipant;
    for (const auto& part : host.participants()) {
        if (!part.isHost) bob = part.id;
    }

    CHECK(host.banParticipant(bob));
    REQUIRE(host.bans().size() == 1);
    CHECK(host.bans()[0].clientKey == "bob-install-key");
    CHECK(host.bans()[0].name == "Bob");
    step();

    const int capturesBefore = hostState.captureCalls;

    // Back again, under a different display name — the ban follows the install,
    // not the label the user typed into the panel.
    BitWriter second;
    writeJoinRequest(second, "Definitely Not Bob", "", "bob-install-key");
    clientNet.send(LoopbackTransport::kPeer, kIdJoinRequest, second);
    step();

    CHECK(reason == JoinRejectReason::Banned);
    CHECK(host.participants().size() == 1);
    // Refused before any work: a banned peer must not be able to make the host
    // serialize its whole scene by knocking repeatedly.
    CHECK(hostState.captureCalls == capturesBefore);
}

TEST_CASE("CollabSession: a kicked client may come back, a banned one may not")
{
    auto [a, b] = LoopbackTransport::createPair();
    NetSession hostNet(a.get(), NetRole::Host);
    NetSession clientNet(b.get(), NetRole::Client);

    FakeState hostState;
    CollabSession::Config hostCfg;
    hostCfg.removalGraceMs = 1000 * 1000;
    CollabSession host(&hostNet, NetRole::Host, hostCfg);
    host.setStateProvider(&hostState);

    const auto step = [&] {
        for (int i = 0; i < 6; ++i) {
            a->update(); b->update();
            hostNet.pump(); clientNet.pump();
            host.update(0);
        }
    };
    const auto knock = [&](const std::string& name) {
        BitWriter w;
        writeJoinRequest(w, name, "", "bob-install-key");
        clientNet.send(LoopbackTransport::kPeer, kIdJoinRequest, w);
        step();
    };

    step();
    knock("Bob");
    REQUIRE(host.participants().size() == 2);

    ParticipantId bob = kInvalidParticipant;
    for (const auto& part : host.participants()) if (!part.isHost) bob = part.id;
    host.kickParticipant(bob);
    step();
    REQUIRE(host.participants().size() == 1);

    // A kick is a door held open.
    knock("Bob");
    CHECK(host.participants().size() == 2);
}

TEST_CASE("CollabSession: unbanning lets someone back in")
{
    auto [a, b] = LoopbackTransport::createPair();
    NetSession hostNet(a.get(), NetRole::Host);
    NetSession clientNet(b.get(), NetRole::Client);

    FakeState hostState;
    CollabSession::Config hostCfg;
    hostCfg.removalGraceMs = 1000 * 1000;
    CollabSession host(&hostNet, NetRole::Host, hostCfg);
    host.setStateProvider(&hostState);

    const auto step = [&] {
        for (int i = 0; i < 6; ++i) {
            a->update(); b->update();
            hostNet.pump(); clientNet.pump();
            host.update(0);
        }
    };
    const auto knock = [&] {
        BitWriter w;
        writeJoinRequest(w, "Bob", "", "bob-install-key");
        clientNet.send(LoopbackTransport::kPeer, kIdJoinRequest, w);
        step();
    };

    step();
    knock();
    ParticipantId bob = kInvalidParticipant;
    for (const auto& part : host.participants()) if (!part.isHost) bob = part.id;
    host.banParticipant(bob);
    step();

    knock();
    REQUIRE(host.participants().size() == 1);   // still out

    CHECK(host.unban("bob-install-key", "Bob"));
    CHECK(host.bans().empty());
    CHECK_FALSE(host.unban("bob-install-key", "Bob"));   // idempotent, and says so

    knock();
    CHECK(host.participants().size() == 2);
}

TEST_CASE("CollabSession: a ban on a nameless install falls back to the display name")
{
    // An editor that has never written its identity file sends no client key.
    // Matching on the name alone is weaker, but a ban that does nothing at all
    // would be worse.
    auto [a, b] = LoopbackTransport::createPair();
    NetSession hostNet(a.get(), NetRole::Host);
    NetSession clientNet(b.get(), NetRole::Client);

    FakeState hostState;
    CollabSession::Config hostCfg;
    hostCfg.removalGraceMs = 1000 * 1000;
    CollabSession host(&hostNet, NetRole::Host, hostCfg);
    host.setStateProvider(&hostState);

    const auto step = [&] {
        for (int i = 0; i < 6; ++i) {
            a->update(); b->update();
            hostNet.pump(); clientNet.pump();
            host.update(0);
        }
    };
    const auto knock = [&] {
        BitWriter w;
        writeJoinRequest(w, "Nameless", "", "");
        clientNet.send(LoopbackTransport::kPeer, kIdJoinRequest, w);
        step();
    };

    step();
    knock();
    ParticipantId who = kInvalidParticipant;
    for (const auto& part : host.participants()) if (!part.isHost) who = part.id;
    REQUIRE(who != kInvalidParticipant);

    host.banParticipant(who);
    step();
    REQUIRE(host.bans().size() == 1);
    CHECK(host.bans()[0].clientKey.empty());

    knock();
    CHECK(host.participants().size() == 1);
}

TEST_CASE("CollabSession: a ban does not catch a namesake with a different install")
{
    // Two people who both left the display name at its default must not be able
    // to ban each other by accident — the client key is what a ban is really on.
    auto [a, b] = LoopbackTransport::createPair();
    NetSession hostNet(a.get(), NetRole::Host);
    NetSession clientNet(b.get(), NetRole::Client);

    FakeState hostState;
    CollabSession::Config hostCfg;
    hostCfg.removalGraceMs = 1000 * 1000;
    CollabSession host(&hostNet, NetRole::Host, hostCfg);
    host.setStateProvider(&hostState);

    const auto step = [&] {
        for (int i = 0; i < 6; ++i) {
            a->update(); b->update();
            hostNet.pump(); clientNet.pump();
            host.update(0);
        }
    };
    const auto knock = [&](const std::string& key) {
        BitWriter w;
        writeJoinRequest(w, "Horizon User", "", key);
        clientNet.send(LoopbackTransport::kPeer, kIdJoinRequest, w);
        step();
    };

    step();
    knock("install-one");
    ParticipantId who = kInvalidParticipant;
    for (const auto& part : host.participants()) if (!part.isHost) who = part.id;
    host.banParticipant(who);
    step();
    REQUIRE(host.participants().size() == 1);

    knock("install-two");
    CHECK(host.participants().size() == 2);
}

// ─── Participant colours ─────────────────────────────────────────────────────

TEST_CASE("CollabSession: the palette's own colours are all distinguishable")
{
    // The whole colour mechanism rests on this: if two presets are within the
    // collision distance of each other, the host will refuse the second one for
    // no reason a user could understand, and two people who picked visibly
    // different swatches would still be told they clashed.
    constexpr int n = static_cast<int>(sizeof(kParticipantPalette) /
                                       sizeof(kParticipantPalette[0]));
    constexpr int minSq = kColorCollisionDistance * kColorCollisionDistance;
    for (int i = 0; i < n; ++i) {
        CHECK_FALSE(kParticipantPalette[i].unset());   // black is the sentinel
        for (int j = i + 1; j < n; ++j) {
            CHECK(colorDistSq(kParticipantPalette[i], kParticipantPalette[j]) >= minSq);
        }
    }
}

TEST_CASE("CollabSession: a chosen colour is honoured when nobody has it")
{
    CollabSession::Config clientCfg, hostCfg;
    clientCfg.preferredColor = kParticipantPalette[5];   // blue
    hostCfg.preferredColor   = kParticipantPalette[0];   // red

    auto p = makePair("Anna", "Bob", clientCfg, hostCfg);
    p->hostState.data = makeBlob(64);
    p->pump();

    REQUIRE(p->client->isJoined());

    // The host's own wish, and the joiner's, both survive on both sides.
    CHECK(colorOf(*p->host,   "Anna").r == kParticipantPalette[0].r);
    CHECK(colorOf(*p->client, "Anna").r == kParticipantPalette[0].r);
    CHECK(colorOf(*p->host,   "Bob").r  == kParticipantPalette[5].r);
    CHECK(colorOf(*p->client, "Bob").b  == kParticipantPalette[5].b);
}

TEST_CASE("CollabSession: your own entry shows the colour you were given")
{
    // The joiner has to learn the answer explicitly — it cannot work out on its
    // own that its wish was taken, and drawing itself in the wish would put it in
    // a colour nobody else uses for it.
    CollabSession::Config clientCfg, hostCfg;
    clientCfg.preferredColor = kParticipantPalette[2];
    hostCfg.preferredColor   = kParticipantPalette[2];   // the same one

    auto p = makePair("Anna", "Bob", clientCfg, hostCfg);
    p->hostState.data = makeBlob(64);
    p->pump();

    REQUIRE(p->client->isJoined());

    const ParticipantColor mineOnClient = colorOf(*p->client, "Bob");
    const ParticipantColor mineOnHost   = colorOf(*p->host,   "Bob");
    CHECK(mineOnClient.r == mineOnHost.r);
    CHECK(mineOnClient.g == mineOnHost.g);
    CHECK(mineOnClient.b == mineOnHost.b);
    CHECK_FALSE(mineOnClient.unset());
    // And it is NOT the one that was already taken.
    CHECK(colorDistSq(mineOnClient, kParticipantPalette[2]) >=
          kColorCollisionDistance * kColorCollisionDistance);
}

TEST_CASE("CollabSession: a colour close to a taken one is moved aside")
{
    // Not an exact match — a nudge on the picker. The point of a distance
    // threshold rather than equality is that two markers a shade apart are
    // useless as markers.
    CollabSession::Config clientCfg, hostCfg;
    hostCfg.preferredColor = ParticipantColor{ 232, 74, 74 };
    clientCfg.preferredColor = ParticipantColor{ 236, 80, 70 };   // all but identical

    auto p = makePair("Anna", "Bob", clientCfg, hostCfg);
    p->hostState.data = makeBlob(64);
    p->pump();

    REQUIRE(p->client->isJoined());
    const ParticipantColor bob = colorOf(*p->host, "Bob");
    CHECK(colorDistSq(bob, hostCfg.preferredColor) >=
          kColorCollisionDistance * kColorCollisionDistance);
}

TEST_CASE("CollabSession: no preference still gets a colour")
{
    auto p = makePair();   // neither side asked for anything
    p->hostState.data = makeBlob(64);
    p->pump();

    REQUIRE(p->client->isJoined());
    const ParticipantColor anna = colorOf(*p->host, "Anna");
    const ParticipantColor bob  = colorOf(*p->host, "Bob");
    CHECK_FALSE(anna.unset());
    CHECK_FALSE(bob.unset());
    CHECK(colorDistSq(anna, bob) >= kColorCollisionDistance * kColorCollisionDistance);
}

TEST_CASE("CollabSession: a roomful of joiners all end up distinguishable")
{
    // Everyone asks for the same colour. Each one after the first has to be
    // moved, and none of them may be moved onto each other.
    auto [a, b] = LoopbackTransport::createPair();
    NetSession hostNet(a.get(), NetRole::Host);
    NetSession clientNet(b.get(), NetRole::Client);

    FakeState hostState;
    CollabSession::Config hostCfg;
    hostCfg.maxParticipants = 8;
    hostCfg.preferredColor  = kParticipantPalette[0];
    CollabSession host(&hostNet, NetRole::Host, hostCfg);
    host.setStateProvider(&hostState);

    const auto step = [&] {
        for (int i = 0; i < 6; ++i) {
            a->update(); b->update();
            hostNet.pump(); clientNet.pump();
            host.update(0);
        }
    };
    step();

    // Driven by hand: one link, several join requests, which is the only way to
    // put more than two participants in front of a loopback pair.
    for (int i = 0; i < 7; ++i) {
        BitWriter w;
        writeJoinRequest(w, "Guest" + std::to_string(i), "", "key-" + std::to_string(i),
                         0, kParticipantPalette[0]);
        clientNet.send(LoopbackTransport::kPeer, kIdJoinRequest, w);
        step();
    }

    REQUIRE(host.participants().size() == 8);
    constexpr int minSq = kColorCollisionDistance * kColorCollisionDistance;
    for (std::size_t i = 0; i < host.participants().size(); ++i) {
        CHECK_FALSE(host.participants()[i].color.unset());
        for (std::size_t j = i + 1; j < host.participants().size(); ++j) {
            CHECK(colorDistSq(host.participants()[i].color,
                              host.participants()[j].color) >= minSq);
        }
    }
}

TEST_CASE("CollabSession: a freed colour becomes available again")
{
    auto [a, b] = LoopbackTransport::createPair();
    NetSession hostNet(a.get(), NetRole::Host);
    NetSession clientNet(b.get(), NetRole::Client);

    FakeState hostState;
    CollabSession::Config hostCfg;
    hostCfg.removalGraceMs = 1000 * 1000;
    hostCfg.preferredColor = kParticipantPalette[0];
    CollabSession host(&hostNet, NetRole::Host, hostCfg);
    host.setStateProvider(&hostState);

    const auto step = [&] {
        for (int i = 0; i < 6; ++i) {
            a->update(); b->update();
            hostNet.pump(); clientNet.pump();
            host.update(0);
        }
    };
    const auto knock = [&](const std::string& name, const std::string& key,
                           ParticipantColor wish) {
        BitWriter w;
        writeJoinRequest(w, name, "", key, 0, wish);
        clientNet.send(LoopbackTransport::kPeer, kIdJoinRequest, w);
        step();
    };

    step();
    knock("Bob", "bob", kParticipantPalette[3]);
    REQUIRE(colorOf(host, "Bob").g == kParticipantPalette[3].g);

    // Bob leaves; green is nobody's now, so the next person asking for it gets
    // it rather than being pushed onto a colour that is free for no reason.
    ParticipantId bob = kInvalidParticipant;
    for (const auto& p : host.participants()) if (!p.isHost) bob = p.id;
    host.kickParticipant(bob);
    step();

    knock("Cleo", "cleo", kParticipantPalette[3]);
    CHECK(colorOf(host, "Cleo").g == kParticipantPalette[3].g);
    CHECK(colorOf(host, "Cleo").r == kParticipantPalette[3].r);
}

// ─── Asking the holder for an asset ──────────────────────────────────────────
// The one request the host does not answer: it interrupts somebody's work, so
// the person being interrupted decides. The host only knows who that is.

namespace {
// Bob's participant id, as the host knows it.
ParticipantId clientIdOf(const CollabSession& host) {
    for (const auto& p : host.participants()) if (!p.isHost) return p.id;
    return kInvalidParticipant;
}
constexpr std::uint64_t kAssetSubject = 0x8000'0000'0000'1234ull;
} // namespace

TEST_CASE("CollabSession: the holding host hands an asset to the client that asked")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    // Anna (the host) is editing it.
    p->host->requestLock(kAssetSubject);
    p->pump();
    REQUIRE(p->host->ownsLock(kAssetSubject));

    ParticipantId  asker = kInvalidParticipant;
    std::uint32_t  reqId = 0;
    std::string    askedPath;
    p->host->onAssetEditRequested([&](ParticipantId from, std::uint32_t id,
                                      const std::string& path) {
        asker = from; reqId = id; askedPath = path;
    });
    bool verdict = false, answered = false;
    p->client->onAssetOpVerdict([&](std::uint32_t, bool ok) {
        answered = true; verdict = ok;
    });

    REQUIRE(p->client->requestAssetOp(CollabSession::AssetOp::Edit,
                                      "Content/Foo.hasset", {}, false,
                                      kAssetSubject) != 0);
    p->pump();

    // It reached the holder, not the host's delete/rename queue.
    CHECK(asker == clientIdOf(*p->host));
    CHECK(askedPath == "Content/Foo.hasset");

    p->host->sendAssetEditAnswer(asker, reqId, askedPath, true, kAssetSubject);
    p->pump();

    CHECK(answered);
    CHECK(verdict);
    // Handed over, not merely released: it is Bob's now, and it was never free
    // in between for a third peer to take.
    CHECK(p->client->ownsLock(kAssetSubject));
    CHECK_FALSE(p->host->ownsLock(kAssetSubject));
    REQUIRE(p->host->lockFor(kAssetSubject) != nullptr);
    CHECK(p->host->lockFor(kAssetSubject)->owner == clientIdOf(*p->host));
}

TEST_CASE("CollabSession: the host asks a client for the asset it is holding")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    p->client->requestLock(kAssetSubject);
    p->pump();
    REQUIRE(p->client->ownsLock(kAssetSubject));

    ParticipantId asker = kInvalidParticipant;
    std::uint32_t reqId = 0;
    std::string   askedPath;
    p->client->onAssetEditRequested([&](ParticipantId from, std::uint32_t id,
                                        const std::string& path) {
        asker = from; reqId = id; askedPath = path;
    });
    // The host is the one waiting this time. There is no connection to send its
    // own verdict down, so the callback is the delivery.
    bool answered = false, verdict = false;
    p->host->onAssetOpVerdict([&](std::uint32_t, bool ok) {
        answered = true; verdict = ok;
    });

    REQUIRE(p->host->requestAssetOp(CollabSession::AssetOp::Edit,
                                    "Content/Bar.hasset", {}, false,
                                    kAssetSubject) != 0);
    p->pump();
    CHECK(asker == p->host->localId());

    p->client->sendAssetEditAnswer(asker, reqId, askedPath, true, kAssetSubject);
    p->pump();

    CHECK(answered);
    CHECK(verdict);
    CHECK(p->host->ownsLock(kAssetSubject));
    CHECK_FALSE(p->client->ownsLock(kAssetSubject));
}

TEST_CASE("CollabSession: a refused asset stays with the one holding it")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    p->host->requestLock(kAssetSubject);
    p->pump();

    ParticipantId asker = kInvalidParticipant;
    std::uint32_t reqId = 0;
    p->host->onAssetEditRequested([&](ParticipantId from, std::uint32_t id,
                                      const std::string&) { asker = from; reqId = id; });
    bool answered = false, verdict = true;
    p->client->onAssetOpVerdict([&](std::uint32_t, bool ok) {
        answered = true; verdict = ok;
    });

    p->client->requestAssetOp(CollabSession::AssetOp::Edit, "Content/Foo.hasset",
                              {}, false, kAssetSubject);
    p->pump();
    p->host->sendAssetEditAnswer(asker, reqId, "Content/Foo.hasset", false,
                                 kAssetSubject);
    p->pump();

    // A no is an answer, and it travels the same way a yes does.
    CHECK(answered);
    CHECK_FALSE(verdict);
    CHECK(p->host->ownsLock(kAssetSubject));
    CHECK_FALSE(p->client->ownsLock(kAssetSubject));
}

TEST_CASE("CollabSession: asking for an asset nobody holds is granted at once")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    bool asked = false;
    p->host->onAssetEditRequested([&](ParticipantId, std::uint32_t,
                                      const std::string&) { asked = true; });
    bool answered = false, verdict = false;
    p->client->onAssetOpVerdict([&](std::uint32_t, bool ok) {
        answered = true; verdict = ok;
    });

    p->client->requestAssetOp(CollabSession::AssetOp::Edit, "Content/Free.hasset",
                              {}, false, kAssetSubject);
    p->pump();

    // Nobody is being interrupted, so nobody is asked — and the asker is not
    // left waiting for an answer that has no one to come from.
    CHECK_FALSE(asked);
    CHECK(answered);
    CHECK(verdict);
}

TEST_CASE("CollabSession: an answer from someone who is not holding it still answers")
{
    auto p = makePair();
    p->hostState.data = makeBlob(64);
    p->pump();

    // Anna holds it. Bob answers anyway — a stale answer sent after letting go,
    // or a peer that never held it at all.
    p->host->requestLock(kAssetSubject);
    p->pump();

    bool answered = false, verdict = true;
    p->host->onAssetOpVerdict([&](std::uint32_t id, bool ok) {
        if (id == 77) { answered = true; verdict = ok; }
    });

    p->client->sendAssetEditAnswer(p->host->localId(), 77, "Content/Foo.hasset",
                                   true, kAssetSubject);
    p->pump();

    // Nothing changes hands on a non-holder's word — but the requester is told
    // what is actually true, instead of waiting forever for an answer the host
    // dropped on the floor.
    CHECK(answered);
    CHECK_FALSE(verdict);
    CHECK(p->host->ownsLock(kAssetSubject));
    CHECK_FALSE(p->client->ownsLock(kAssetSubject));
}

// ─── v10: one user action, one bundle ────────────────────────────────────────

TEST_CASE("v10: the batch a request belongs to reaches the host")
{
    auto p = makePair();
    p->pump();

    struct Seen { std::uint32_t requestId, batch; std::string path; };
    std::vector<Seen> seen;
    p->host->onAssetOpRequested([&](ParticipantId, std::uint32_t id,
                                    CollabSession::AssetOp, const std::string& path,
                                    const std::string&, bool, std::uint32_t batch) {
        seen.push_back({ id, batch, path });
    });

    // Three files from one selection, and one asked for on its own afterwards.
    for (const char* f : { "Content/A.hasset", "Content/B.hasset", "Content/C.hasset" }) {
        REQUIRE(p->client->requestAssetOp(CollabSession::AssetOp::Delete, f, {},
                                          false, 0, /*batch=*/7) != 0);
    }
    REQUIRE(p->client->requestAssetOp(CollabSession::AssetOp::Delete,
                                      "Content/Lonely.hasset") != 0);
    p->pump();

    REQUIRE(seen.size() == 4);
    for (int i = 0; i < 3; ++i) CHECK(seen[i].batch == 7);
    // The default is 0 — "on its own" — so every existing caller keeps meaning
    // exactly what it always meant.
    CHECK(seen[3].batch == 0);
    // Request ids stay per-request: a bundle is drawn together and answered
    // file by file, which only works while each file keeps its own id.
    CHECK(seen[0].requestId != seen[1].requestId);
}

TEST_CASE("v10: a reimport notice survives BOTH bounds — request and apply")
{
    // Two separate range checks stand between a new op and the peers, and they
    // were three apart: requests accepted up to Edit, applies only up to
    // Create. An op that passed one and not the other would be dropped in
    // silence on every peer — the failure this test exists to catch.
    auto p = makePair();
    p->pump();

    CollabSession::AssetOp askedOp = CollabSession::AssetOp::Delete;
    std::string            askedPath;
    p->host->onAssetOpRequested([&](ParticipantId, std::uint32_t,
                                    CollabSession::AssetOp op, const std::string& path,
                                    const std::string&, bool, std::uint32_t) {
        askedOp = op; askedPath = path;
    });
    CollabSession::AssetOp appliedOp = CollabSession::AssetOp::Delete;
    std::string            appliedPath;
    p->client->onAssetOpApply([&](ParticipantId, CollabSession::AssetOp op,
                                  const std::string& path, const std::string&, bool) {
        appliedOp = op; appliedPath = path;
    });

    REQUIRE(p->client->requestAssetOp(CollabSession::AssetOp::Reimport,
                                      "Content/Rock.hasset") != 0);
    p->pump();
    CHECK(askedOp == CollabSession::AssetOp::Reimport);
    CHECK(askedPath == "Content/Rock.hasset");

    p->host->broadcastAssetOpApply(CollabSession::AssetOp::Reimport,
                                   "Content/Rock.hasset", {}, clientIdOf(*p->host));
    p->pump();
    CHECK(appliedOp == CollabSession::AssetOp::Reimport);
    CHECK(appliedPath == "Content/Rock.hasset");
}

TEST_CASE("v10: an Edit can be requested but never arrives as an apply")
{
    // Edit sits in the middle of the enum, so widening the apply bound for
    // Reimport would have let it through — and a peer reading an Edit as an
    // apply sees an op with an empty newPath, which the editor's handler treats
    // as a rename to nowhere.
    auto p = makePair();
    p->pump();

    bool applied = false;
    p->client->onAssetOpApply([&](ParticipantId, CollabSession::AssetOp,
                                  const std::string&, const std::string&, bool) {
        applied = true;
    });

    BitWriter w;
    w.writeUInt32(1);                                                   // by
    w.writeByte(static_cast<std::uint8_t>(CollabSession::AssetOp::Edit));
    w.writeString("Content/Foo.hasset");
    w.writeString("");
    w.writeByte(0);
    p->hostNet->send(LoopbackTransport::kPeer, kFirstUserMessage + 34, w);
    p->pump();

    CHECK_FALSE(applied);
}

TEST_CASE("v10: the version before this one is still refused at the door")
{
    // The generic "+99" test above proves an unknown version is refused; this
    // one pins the version we just left behind, which is the peer that actually
    // exists in the wild. A v9 request stops one uint32 short of the batch id,
    // so a v10 host would drop every request it sent and the requester would
    // wait for a verdict nobody is going to give.
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
    w.writeUInt16(9);              // the protocol before the batch id existed
    w.writeString("Yesterday");
    clientNet.send(LoopbackTransport::kPeer, kFirstUserMessage + 0, w);

    for (int i = 0; i < 6; ++i) {
        a->update(); b->update();
        hostNet.pump(); clientNet.pump();
    }

    CHECK(reason == JoinRejectReason::VersionMismatch);
    CHECK(host.participants().size() == 1);
}

// ─── Larger assets: whose decision, and who is asked ─────────────────────────
// The host decides what its session carries. The guest pays for it in bandwidth,
// so a guest who has not agreed is refused BEFORE anything transfers — and
// refused in a way it can tell apart from every other refusal, because the right
// response is to ask the user rather than to give up.

TEST_CASE("CollabSession: a host that sends the big assets refuses a guest who has not agreed")
{
    CollabSession::Config hostCfg;
    hostCfg.syncLargeAssets = true;
    CollabSession::Config clientCfg;
    clientCfg.syncLargeAssets = false;   // the default, spelled out: it is the point

    auto p = makePair("Anna", "Bob", clientCfg, hostCfg);
    p->hostState.data = makeBlob(512);

    JoinRejectReason reason = JoinRejectReason::None;
    p->client->onJoinRejected([&](JoinRejectReason r, const std::string&) { reason = r; });

    p->pump();

    // Its own reason, not a generic refusal: the editor turns exactly this into
    // "this session sends the big files, is that all right?" and retries. Folded
    // into any other code it would reach the user as a dead end.
    CHECK(reason == JoinRejectReason::LargeAssetsRequired);
    CHECK_FALSE(p->client->isJoined());
    // Refused before the snapshot, not after: the host must not have serialized
    // its scene for a peer it was never going to admit.
    CHECK(p->hostState.captureCalls == 0);
    CHECK(p->host->participants().size() == 1);
}

TEST_CASE("CollabSession: a guest that has agreed joins the same host, and learns the rule")
{
    CollabSession::Config hostCfg;
    hostCfg.syncLargeAssets = true;
    CollabSession::Config clientCfg;
    clientCfg.syncLargeAssets = true;

    auto p = makePair("Anna", "Bob", clientCfg, hostCfg);
    p->hostState.data = makeBlob(512);

    p->pump();

    REQUIRE(p->client->isJoined());
    // Both sides agree on what the session is, which is the whole reason the
    // accept carries it — the guest publishes by this from here on.
    CHECK(p->host->sessionSyncsLargeAssets());
    CHECK(p->client->sessionSyncsLargeAssets());
}

TEST_CASE("CollabSession: a guest that would send the big assets follows a host that does not")
{
    // The reverse case is NOT an error. The host decides what the session
    // carries, so a guest whose own setting is more permissive simply follows it
    // — there is nobody for its preference to bind, and refusing it would deny
    // a join over a difference that costs nobody anything.
    CollabSession::Config hostCfg;
    hostCfg.syncLargeAssets = false;
    CollabSession::Config clientCfg;
    clientCfg.syncLargeAssets = true;

    auto p = makePair("Anna", "Bob", clientCfg, hostCfg);
    p->hostState.data = makeBlob(512);

    JoinRejectReason reason = JoinRejectReason::None;
    p->client->onJoinRejected([&](JoinRejectReason r, const std::string&) { reason = r; });

    p->pump();

    CHECK(reason == JoinRejectReason::None);
    REQUIRE(p->client->isJoined());
    // And it learned the HOST's answer, not its own — this is what stops it
    // publishing meshes into a session where nobody else carries them.
    CHECK_FALSE(p->client->sessionSyncsLargeAssets());
    CHECK_FALSE(p->host->sessionSyncsLargeAssets());
}

TEST_CASE("CollabSession: an ordinary session says it does not carry the big assets")
{
    auto p = makePair("Anna", "Bob");
    p->hostState.data = makeBlob(256);
    p->pump();

    REQUIRE(p->client->isJoined());
    CHECK_FALSE(p->host->sessionSyncsLargeAssets());
    CHECK_FALSE(p->client->sessionSyncsLargeAssets());
}

TEST_CASE("CollabSession: the version is still checked before the large-asset rule")
{
    // A peer that speaks an older protocol did not send the large-asset answer
    // at all, so reading one would land on whatever follows — and refusing it as
    // "you have not agreed to large assets" would send its user to a setting
    // that cannot fix anything. The version check runs first and says the one
    // true thing: these two builds do not speak the same protocol.
    auto [a, b] = LoopbackTransport::createPair();
    NetSession hostNet(a.get(), NetRole::Host);
    NetSession clientNet(b.get(), NetRole::Client);

    CollabSession::Config hostCfg;
    hostCfg.syncLargeAssets = true;
    FakeState hostState;
    CollabSession host(&hostNet, NetRole::Host, hostCfg);
    host.setStateProvider(&hostState);

    JoinRejectReason reason = JoinRejectReason::None;
    clientNet.on(kIdJoinRejected, [&](ConnectionId, BitReader& r) {
        std::uint8_t code = 0;
        r.readByte(code);
        reason = static_cast<JoinRejectReason>(code);
    });

    a->update(); b->update();
    hostNet.pump(); clientNet.pump();

    BitWriter w;
    w.writeUInt16(kCollabProtocolVersion - 1);   // the protocol before this flag
    w.writeString("Yesterday");
    clientNet.send(LoopbackTransport::kPeer, kIdJoinRequest, w);

    for (int i = 0; i < 6; ++i) {
        a->update(); b->update();
        hostNet.pump(); clientNet.pump();
    }

    CHECK(reason == JoinRejectReason::VersionMismatch);
    CHECK(host.participants().size() == 1);
}

// ─── The transfer ceiling ────────────────────────────────────────────────────
// One number per MACHINE, not per session, and the two sides are expected to
// disagree: each refuses what it is not willing to hold, so the lower of the two
// is what gets through in practice. Both halves are tested because having only
// one is the interesting failure — a sender that trusts the receiver's ceiling
// lets a peer make it allocate whatever it announces, and a receiver that trusts
// the sender's has no bound at all.

TEST_CASE("CollabSession: a client's asset under its ceiling arrives and one over it never leaves")
{
    CollabSession::Config small;
    small.maxAssetBytes = 4096;
    auto p = makePair("Anna", "Bob", small);
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'AA01ull;
    REQUIRE(p->client->requestLock(kAsset));
    p->pump(4);

    CollabSession::AssetUpdate got;
    bool arrived = false;
    p->host->onAssetUpdated([&](ParticipantId, const CollabSession::AssetUpdate& a) {
        got = a; arrived = true;
    });

    const auto fits = makeBlob(4000);
    REQUIRE(p->client->sendAsset(kAsset, "Content/Meshes/Small.hasset", fits));
    p->pump(6);
    REQUIRE(arrived);
    CHECK(got.bytes == fits);

    // Over the ceiling: refused by the SENDER, before a byte reaches the wire.
    // The false is the whole contract — nothing is truncated and nothing is
    // half-sent, so the caller is in a position to say so to the user.
    arrived = false;
    const auto tooBig = makeBlob(4097);
    CHECK_FALSE(p->client->sendAsset(kAsset, "Content/Meshes/Big.hasset", tooBig));
    p->pump(6);
    CHECK_FALSE(arrived);
}

TEST_CASE("CollabSession: the host's own ceiling stops its own oversized save just the same")
{
    // The same rule from the other end. A host enforcing this only on what
    // arrives would be a host that can flood every client in the session.
    CollabSession::Config small;
    small.maxAssetBytes = 4096;
    auto p = makePair("Anna", "Bob", {}, small);
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'AA02ull;
    REQUIRE(p->host->requestLock(kAsset));
    p->pump(4);

    bool arrived = false;
    p->client->onAssetUpdated([&](ParticipantId, const CollabSession::AssetUpdate&) {
        arrived = true;
    });

    REQUIRE(p->host->sendAsset(kAsset, "Content/Textures/Fine.hasset", makeBlob(4096)));
    p->pump(6);
    CHECK(arrived);

    arrived = false;
    CHECK_FALSE(p->host->sendAsset(kAsset, "Content/Textures/Huge.hasset", makeBlob(9000)));
    p->pump(6);
    CHECK_FALSE(arrived);
}

TEST_CASE("CollabSession: a host refuses an asset over ITS ceiling and says which file it was")
{
    // The case the sender cannot see: their own ceiling let the file go, ours did
    // not. Nobody over there failed at anything, so nothing over there will ever
    // mention it — which is why the refusal has to be announced on this side.
    CollabSession::Config tightHost;
    tightHost.maxAssetBytes = 4096;
    auto p = makePair("Anna", "Bob", {}, tightHost);
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'AA03ull;
    REQUIRE(p->client->requestLock(kAsset));
    p->pump(4);

    bool applied = false;
    p->host->onAssetUpdated([&](ParticipantId, const CollabSession::AssetUpdate&) {
        applied = true;
    });
    std::string   refusedPath;
    std::uint32_t refusedBytes = 0;
    ParticipantId refusedFrom  = kInvalidParticipant;
    p->host->onAssetRefused([&](ParticipantId who, const std::string& path,
                                std::uint32_t bytes) {
        refusedFrom  = who;
        refusedPath  = path;
        refusedBytes = bytes;
    });

    // The client's own ceiling is the default, so it sends this happily.
    REQUIRE(p->client->sendAsset(kAsset, "Content/Meshes/Statue.hasset", makeBlob(20000)));
    p->pump(8);

    CHECK_FALSE(applied);
    CHECK(refusedPath == "Content/Meshes/Statue.hasset");
    // The ANNOUNCED size, which is what the refusal was made on: not one byte of
    // the file was read, so this is the only number this side ever has.
    CHECK(refusedBytes == 20000);
    CHECK(refusedFrom != kInvalidParticipant);
}

TEST_CASE("CollabSession: a client refuses an asset over ITS ceiling too")
{
    CollabSession::Config tightClient;
    tightClient.maxAssetBytes = 4096;
    auto p = makePair("Anna", "Bob", tightClient);
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'AA04ull;
    REQUIRE(p->host->requestLock(kAsset));
    p->pump(4);

    bool applied = false;
    p->client->onAssetUpdated([&](ParticipantId, const CollabSession::AssetUpdate&) {
        applied = true;
    });
    std::string refusedPath;
    p->client->onAssetRefused([&](ParticipantId, const std::string& path, std::uint32_t) {
        refusedPath = path;
    });

    REQUIRE(p->host->sendAsset(kAsset, "Content/Audio/Score.hasset", makeBlob(30000)));
    p->pump(8);

    // Being the guest is not being obliged: the host decides what the session
    // CARRIES, never how much memory this machine will put behind one file.
    CHECK_FALSE(applied);
    CHECK(refusedPath == "Content/Audio/Score.hasset");
}

TEST_CASE("CollabSession: raising the ceiling lets the same file through, without a rejoin")
{
    // What the notification tells the user to do has to actually work from where
    // they are — which is inside the session, watching a file fail to arrive.
    CollabSession::Config tight;
    tight.maxAssetBytes = 4096;
    auto p = makePair("Anna", "Bob", tight, tight);
    p->hostState.data = makeBlob(64);
    p->pump();
    REQUIRE(p->client->isJoined());

    constexpr std::uint64_t kAsset = 0x8000'0000'0000'AA05ull;
    REQUIRE(p->client->requestLock(kAsset));
    p->pump(4);

    CollabSession::AssetUpdate got;
    bool arrived = false;
    p->host->onAssetUpdated([&](ParticipantId, const CollabSession::AssetUpdate& a) {
        got = a; arrived = true;
    });

    const auto payload = makeBlob(50000);
    CHECK_FALSE(p->client->sendAsset(kAsset, "Content/Meshes/Terrain.hasset", payload));
    p->pump(6);
    REQUIRE_FALSE(arrived);

    // Both ends, because either one alone still refuses — that IS the design,
    // and a test that raised only the sender's would pass for the wrong reason.
    p->client->setMaxAssetBytes(1024u * 1024u);
    p->host->setMaxAssetBytes(1024u * 1024u);

    REQUIRE(p->client->sendAsset(kAsset, "Content/Meshes/Terrain.hasset", payload));
    p->pump(8);
    REQUIRE(arrived);
    CHECK(got.bytes == payload);
}

TEST_CASE("CollabSession: the snapshot keeps a ceiling of its own")
{
    // The two used to be one number, so raising the asset limit for a 200 MB mesh
    // silently raised what a host could make a joiner hold for a scene nobody
    // asked about. Splitting them is only true if each still answers for itself.
    CollabSession::Config cfg;
    cfg.maxAssetBytes    = 4096;                  // assets: barely anything
    cfg.maxSnapshotBytes = 64u * 1024u * 1024u;   // the scene: untouched
    auto p = makePair("Anna", "Bob", cfg);

    p->hostState.data = makeBlob(700 * 1024);
    p->pump(30);

    REQUIRE(p->client->isJoined());
    CHECK(p->clientState.data == p->hostState.data);

    // And the reverse: a client that would take any asset still refuses a
    // snapshot over the snapshot limit.
    CollabSession::Config other;
    other.maxSnapshotBytes = 1024;
    other.maxAssetBytes    = 64u * 1024u * 1024u;
    auto q = makePair("Anna", "Bob", other);
    q->hostState.data = makeBlob(64 * 1024);

    JoinRejectReason reason = JoinRejectReason::None;
    q->client->onJoinRejected([&](JoinRejectReason r, const std::string&) { reason = r; });
    q->pump(20);
    CHECK_FALSE(q->client->isJoined());
    CHECK(reason == JoinRejectReason::SnapshotFailed);
}

// ─── What a LAN announcement says about the session ──────────────────────────
// Their natural home is test_net_lanbeacon.cpp; they are here because that file
// belongs to another part of this same change.

TEST_CASE("LanBeacon: the large-asset flag survives the round trip")
{
    LanBeacon::Announcement a;
    a.protocol         = kCollabProtocolVersion;
    a.instance         = 0x1234'5678'9ABC'DEF0ull;
    a.sessionId        = "sess-large";
    a.port             = 7777;
    a.hostName         = "Anna";
    a.projectLabel     = "Catania";
    a.projectKey       = "proj-1";
    a.participants     = 2;
    a.syncsLargeAssets = true;

    const std::vector<std::uint8_t> bytes = LanBeacon::encode(a);
    LanBeacon::Announcement out;
    REQUIRE(LanBeacon::decode(bytes.data(), bytes.size(), out));
    CHECK(out.syncsLargeAssets);
    // Nothing in front of it moved. The field was appended precisely so this
    // stays true for a peer that has never heard of it.
    CHECK(out.sessionId    == "sess-large");
    CHECK(out.port         == 7777);
    CHECK(out.projectKey   == "proj-1");
    CHECK(out.participants == 2);

    a.syncsLargeAssets = false;
    const std::vector<std::uint8_t> off = LanBeacon::encode(a);
    LanBeacon::Announcement out2;
    REQUIRE(LanBeacon::decode(off.data(), off.size(), out2));
    CHECK_FALSE(out2.syncsLargeAssets);
}

TEST_CASE("LanBeacon: an announcement from a peer that never heard of the flag still decodes")
{
    // Hand-written in the pre-v12 field order rather than produced by encode()
    // and truncated: what is under test is that the OLD shape is still a valid
    // announcement, and a datagram built from the current writer would stop
    // testing that the moment the writer changes again.
    BitWriter w;
    w.writeUInt32(LanBeacon::kMagic);
    w.writeUInt16(kCollabProtocolVersion - 1);   // the build before this field
    w.writeUInt64(0xFEED'FACE'0000'0001ull);
    w.writeString("sess-old");
    w.writeUInt16(7788);
    w.writeString("Yesterday");
    w.writeString("Old Project");
    w.writeString("proj-old");
    w.writeByte(3);      // participants
    w.writeByte(0);      // closing
    // ...and that is where an older announcement ends. No flag.
    const std::vector<std::uint8_t> bytes = w.data();

    LanBeacon::Announcement out;
    REQUIRE(LanBeacon::decode(bytes.data(), bytes.size(), out));
    CHECK(out.sessionId    == "sess-old");
    CHECK(out.port         == 7788);
    CHECK(out.hostName     == "Yesterday");
    CHECK(out.participants == 3);
    // Defaulted, not guessed. It is safe to show only because the row is
    // un-joinable anyway — the protocol was bumped WITH the field, so a peer that
    // cannot state it is a peer this build refuses at the handshake.
    CHECK_FALSE(out.syncsLargeAssets);
    CHECK(out.protocol != kCollabProtocolVersion);

    // And it is still a row, not a discarded datagram: dropping older peers would
    // be the "my friend's session never shows up" discovery exists to end.
    LanBeacon::Browser b;
    b.ingest("192.168.1.60", bytes.data(), bytes.size(), 1000);
    REQUIRE(b.sessions().size() == 1);
    CHECK_FALSE(b.sessions()[0].syncsLargeAssets);
}

TEST_CASE("LanBeacon: the browser carries the flag through to the row the panel draws")
{
    LanBeacon::Announcement a;
    a.protocol         = kCollabProtocolVersion;
    a.instance         = 99;
    a.sessionId        = "sess-heavy";
    a.port             = 7777;
    a.hostName         = "Anna";
    a.syncsLargeAssets = true;

    const std::vector<std::uint8_t> bytes = LanBeacon::encode(a);
    LanBeacon::Browser b;
    b.ingest("192.168.1.61", bytes.data(), bytes.size(), 1000);

    REQUIRE(b.sessions().size() == 1);
    // What the list shows before anybody clicks. A hint and never a verdict: the
    // handshake is what admits or refuses a guest, and this datagram is
    // unauthenticated — anyone on the segment can write one.
    CHECK(b.sessions()[0].syncsLargeAssets);
    CHECK(b.sessions()[0].address == "192.168.1.61");
}
