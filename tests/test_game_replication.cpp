#include "doctest.h"

#include <HorizonScene/GameReplication.h>
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/NetworkComponent.h>

#include <Net/LoopbackTransport.h>
#include <Net/LossyTransport.h>
#include <Net/NetSession.h>
#include <Net/UdpTransport.h>

#include <memory>
#include <string>
#include <vector>

using namespace HE::Net;

// ─── Gameplay replication (Layer 3a) ─────────────────────────────────────────
// Deliberately separate from editor collaboration: this replicates simulation
// state at a tick rate and tolerates loss, where collab replicates authored
// edits reliably. They share the transport and nothing above it.

namespace {

// The message ids GameReplication uses, for the hand-written datagrams below.
constexpr MessageId kMsgSnapshot  = kFirstUserMessage + 200;
constexpr MessageId kMsgInput     = kFirstUserMessage + 201;
constexpr MessageId kMsgIntegrity = kFirstUserMessage + 202;

// What sits between a GameReplication payload and the UDP datagram: the
// NetSession message id, then SecureTransport's counter and GCM tag.
constexpr std::size_t kSessionIdBytes  = 2;
constexpr std::size_t kSecureOverhead  = 8 + 16;
constexpr std::size_t kUdpMtuPayload   = UdpTransport::Config{}.mtuPayload;
constexpr std::size_t kUdpMaxReliable  =
    (kUdpMtuPayload - UdpTransport::kMsgSeqSize - UdpTransport::kFragmentHeaderSize) *
    UdpTransport::kMaxFragments;

struct Rig {
    std::unique_ptr<LoopbackTransport> serverT, clientT;
    std::unique_ptr<NetSession>        serverNet, clientNet;
    std::unique_ptr<GameReplication>   server, client;
    HorizonWorld serverWorld, clientWorld;

    void pump(float dt, int rounds = 4) {
        for (int i = 0; i < rounds; ++i) {
            serverT->update();
            clientT->update();
            serverNet->pump();
            clientNet->pump();
            server->update(dt);
            client->update(dt);
        }
    }
};

std::unique_ptr<Rig> makeRig(GameReplication::Config cfg = {}) {
    auto r = std::make_unique<Rig>();
    auto [a, b] = LoopbackTransport::createPair();
    r->serverT = std::move(a);
    r->clientT = std::move(b);

    r->serverNet = std::make_unique<NetSession>(r->serverT.get(), NetRole::Server);
    r->clientNet = std::make_unique<NetSession>(r->clientT.get(), NetRole::Client);

    r->server = std::make_unique<GameReplication>(r->serverNet.get(), NetRole::Server, cfg);
    r->client = std::make_unique<GameReplication>(r->clientNet.get(), NetRole::Client, cfg);
    r->server->setWorld(&r->serverWorld);
    r->client->setWorld(&r->clientWorld);

    // Establish the link so connections() is populated.
    r->serverT->update(); r->clientT->update();
    r->serverNet->pump(); r->clientNet->pump();
    return r;
}

// Give both sides an entity under the same net id, which is what the real
// spawn path would do via a spawn message.
Entity mirrorEntity(Rig& rig, const char* name, std::uint32_t& outNetId) {
    const Entity s = rig.serverWorld.createEntity(name);
    rig.serverWorld.registry().emplace_or_replace<TransformComponent>(s);
    outNetId = rig.server->registerEntity(s);

    const Entity c = rig.clientWorld.createEntity(name);
    rig.clientWorld.registry().emplace_or_replace<TransformComponent>(c);
    // The client adopts the SERVER's id — minting its own would make the two
    // peers disagree about which entity a snapshot refers to.
    rig.client->adoptEntity(c, outNetId);
    return c;
}

} // namespace

TEST_CASE("GameReplication: an entity is not replicated until it is registered")
{
    auto rig = makeRig();

    const Entity e = rig->serverWorld.createEntity("LocalEffect");
    rig->serverWorld.registry().emplace_or_replace<TransformComponent>(e);

    // Purely local things — muzzle flashes, debris — must stay off the wire.
    CHECK(rig->server->replicatedCount() == 0);
    rig->pump(1.0f / 30.0f, 4);
    CHECK(rig->server->stats().snapshotsSent == 0);

    rig->server->registerEntity(e);
    CHECK(rig->server->replicatedCount() == 1);
}

TEST_CASE("GameReplication: registration assigns distinct ids")
{
    auto rig = makeRig();

    const Entity a = rig->serverWorld.createEntity("A");
    const Entity b = rig->serverWorld.createEntity("B");
    rig->serverWorld.registry().emplace_or_replace<TransformComponent>(a);
    rig->serverWorld.registry().emplace_or_replace<TransformComponent>(b);

    const std::uint32_t idA = rig->server->registerEntity(a);
    const std::uint32_t idB = rig->server->registerEntity(b);

    CHECK(idA != 0);
    CHECK(idB != 0);
    CHECK(idA != idB);

    // Re-registering keeps the identity, or peers would lose track of it.
    CHECK(rig->server->registerEntity(a) == idA);
}

TEST_CASE("GameReplication: the server sends snapshots at its tick rate, not per frame")
{
    GameReplication::Config cfg;
    cfg.tickHz = 30.0f;
    auto rig = makeRig(cfg);

    const Entity e = rig->serverWorld.createEntity("Mover");
    rig->serverWorld.registry().emplace_or_replace<TransformComponent>(e);
    rig->server->registerEntity(e);

    // Ten 60 Hz frames = 1/6 s, which spans five 30 Hz ticks at most.
    for (int i = 0; i < 10; ++i) rig->pump(1.0f / 60.0f, 1);

    // Sending per frame would double the bandwidth for no benefit.
    CHECK(rig->server->stats().snapshotsSent <= 6);
    CHECK(rig->server->stats().snapshotsSent >= 4);
}

TEST_CASE("GameReplication: a moved entity arrives at the client")
{
    GameReplication::Config cfg;
    cfg.tickHz = 60.0f;
    cfg.interpolationDelaySec = 0.001f;   // converge immediately, for assertions
    auto rig = makeRig(cfg);

    std::uint32_t netId = 0;
    const Entity clientEntity = mirrorEntity(*rig, "Mover", netId);
    REQUIRE(netId != 0);

    auto& serverTc = rig->serverWorld.registry().get<TransformComponent>(
        rig->serverWorld.registry().view<NetworkComponent>().front());
    serverTc.position = glm::vec3(12.0f, 3.0f, -7.0f);
    serverTc.rotation = glm::vec3(0.0f, 90.0f, 0.0f);

    for (int i = 0; i < 12; ++i) rig->pump(1.0f / 60.0f, 2);

    CHECK(rig->client->stats().snapshotsReceived > 0);

    const auto& clientTc =
        rig->clientWorld.registry().get<TransformComponent>(clientEntity);
    // Quantized to 24 bits over ±4096 — sub-millimetre, far below anything a
    // player could see.
    CHECK(clientTc.position.x == doctest::Approx(12.0f).epsilon(0.001));
    CHECK(clientTc.position.z == doctest::Approx(-7.0f).epsilon(0.001));
    CHECK(clientTc.rotation.y == doctest::Approx(90.0f).epsilon(0.01));
}

TEST_CASE("GameReplication: interest management culls distant entities")
{
    GameReplication::Config cfg;
    cfg.tickHz = 60.0f;
    auto rig = makeRig(cfg);

    const Entity near_ = rig->serverWorld.createEntity("Near");
    const Entity far_  = rig->serverWorld.createEntity("Far");
    auto& reg = rig->serverWorld.registry();
    reg.emplace_or_replace<TransformComponent>(near_).position = glm::vec3(0.0f);
    reg.emplace_or_replace<TransformComponent>(far_).position  = glm::vec3(10000.0f, 0, 0);
    rig->server->registerEntity(near_);
    rig->server->registerEntity(far_);

    // The client is standing at the origin.
    rig->server->setViewpoint(LoopbackTransport::kPeer, glm::vec3(0.0f));

    for (int i = 0; i < 6; ++i) rig->pump(1.0f / 60.0f, 1);

    // The single most effective bandwidth lever there is — far more than any
    // per-field compression.
    CHECK(rig->server->stats().entitiesCulled > 0);
    CHECK(rig->server->stats().entitiesSent > 0);
}

TEST_CASE("GameReplication: an entity that opts out of transform replication is skipped")
{
    auto rig = makeRig();

    const Entity e = rig->serverWorld.createEntity("StaticProp");
    auto& reg = rig->serverWorld.registry();
    reg.emplace_or_replace<TransformComponent>(e);
    rig->server->registerEntity(e);
    reg.get<NetworkComponent>(e).replicateTransform = false;

    for (int i = 0; i < 6; ++i) rig->pump(1.0f / 30.0f, 1);

    // Level geometry would otherwise occupy a slot in every single snapshot.
    CHECK(rig->server->stats().entitiesSent == 0);
}

TEST_CASE("GameReplication: unregistering stops replication")
{
    auto rig = makeRig();

    const Entity e = rig->serverWorld.createEntity("Temp");
    rig->serverWorld.registry().emplace_or_replace<TransformComponent>(e);
    rig->server->registerEntity(e);
    REQUIRE(rig->server->replicatedCount() == 1);

    rig->server->unregisterEntity(e);
    CHECK(rig->server->replicatedCount() == 0);

    rig->server->resetStats();
    for (int i = 0; i < 6; ++i) rig->pump(1.0f / 30.0f, 1);
    CHECK(rig->server->stats().entitiesSent == 0);
}

TEST_CASE("GameReplication: a client never acts as the authority")
{
    auto rig = makeRig();

    // A server that applied an incoming snapshot would be taking orders from a
    // peer — the opposite of server-authoritative.
    const Entity e = rig->serverWorld.createEntity("Thing");
    rig->serverWorld.registry().emplace_or_replace<TransformComponent>(e).position =
        glm::vec3(5.0f, 5.0f, 5.0f);
    rig->server->registerEntity(e);

    for (int i = 0; i < 6; ++i) rig->pump(1.0f / 30.0f, 1);

    CHECK(rig->server->stats().snapshotsReceived == 0);
}

TEST_CASE("GameReplication: a truncated snapshot is ignored rather than half-applied")
{
    auto rig = makeRig();

    // A valid header (tick + ack + count) claiming three entities, but no
    // entity data.
    BitWriter w;
    w.writeUInt32(1);    // tick
    w.writeUInt32(0);    // last input we acknowledged
    w.writeUInt16(3);
    rig->serverNet->send(LoopbackTransport::kPeer, kMsgSnapshot, w, SendMode::Unreliable);

    for (int i = 0; i < 4; ++i) rig->pump(1.0f / 30.0f, 1);

    // It counts as received, but nothing was written into the world from it.
    CHECK(rig->client->stats().snapshotsReceived >= 1);
    CHECK(rig->clientWorld.registry().view<TransformComponent>().size() == 0);
}

// ─── Prediction & reconciliation ─────────────────────────────────────────────
// Interpolation is about smoothness; prediction is about latency. Only the
// second makes your OWN character respond on the frame you pressed the key.

namespace {

// The shared simulation step. Both sides run exactly this — anything else and
// they diverge by construction, which the player feels as constant correction.
GameReplication::MoveFn simpleMover() {
    return [](TransformComponent& tc, const GameReplication::InputCommand& c) {
        tc.position += c.move * c.deltaTime;
        tc.rotation.y = c.yaw;
    };
}

// Server + client that both control the same entity, wired for prediction.
struct PredRig : Rig {
    Entity serverEntity = entt::null;
    Entity clientEntity = entt::null;
    std::uint32_t netId = 0;
};

std::unique_ptr<PredRig> makePredRig(GameReplication::Config cfg = {}) {
    auto r = std::make_unique<PredRig>();
    auto [a, b] = LoopbackTransport::createPair();
    r->serverT = std::move(a);
    r->clientT = std::move(b);
    r->serverNet = std::make_unique<NetSession>(r->serverT.get(), NetRole::Server);
    r->clientNet = std::make_unique<NetSession>(r->clientT.get(), NetRole::Client);
    r->server = std::make_unique<GameReplication>(r->serverNet.get(), NetRole::Server, cfg);
    r->client = std::make_unique<GameReplication>(r->clientNet.get(), NetRole::Client, cfg);
    r->server->setWorld(&r->serverWorld);
    r->client->setWorld(&r->clientWorld);
    r->server->setMoveFunction(simpleMover());
    r->client->setMoveFunction(simpleMover());

    r->serverT->update(); r->clientT->update();
    r->serverNet->pump(); r->clientNet->pump();

    r->serverEntity = r->serverWorld.createEntity("Player");
    r->serverWorld.registry().emplace_or_replace<TransformComponent>(r->serverEntity);
    r->netId = r->server->registerEntity(r->serverEntity);
    r->server->assignControl(LoopbackTransport::kPeer, r->netId);

    r->clientEntity = r->clientWorld.createEntity("Player");
    r->clientWorld.registry().emplace_or_replace<TransformComponent>(r->clientEntity);
    r->client->adoptEntity(r->clientEntity, r->netId);
    r->client->setLocallyControlled(r->clientEntity, r->netId);
    return r;
}

glm::vec3 posOf(HorizonWorld& w, Entity e) {
    return w.registry().get<TransformComponent>(e).position;
}

} // namespace

TEST_CASE("GameReplication: input moves the client immediately, before any round trip")
{
    auto rig = makePredRig();

    // Not a single byte has been pumped yet.
    rig->client->pushInput(glm::vec3(10.0f, 0.0f, 0.0f), 0.0f, 0.1f);

    // This is the entire point of prediction: waiting for the server would cost
    // a full round trip on every key press.
    CHECK(posOf(rig->clientWorld, rig->clientEntity).x == doctest::Approx(1.0f));
    CHECK(posOf(rig->serverWorld, rig->serverEntity).x == doctest::Approx(0.0f));
    CHECK(rig->client->pendingInputCount() == 1);
}

TEST_CASE("GameReplication: the server runs the same input and acknowledges it")
{
    auto rig = makePredRig();

    rig->client->pushInput(glm::vec3(10.0f, 0.0f, 0.0f), 0.0f, 0.1f);
    for (int i = 0; i < 8; ++i) rig->pump(1.0f / 60.0f, 1);

    // Server applied it...
    CHECK(posOf(rig->serverWorld, rig->serverEntity).x == doctest::Approx(1.0f));
    CHECK(rig->server->stats().inputsProcessed == 1);

    // ...and the acknowledgement retired the pending command on the client.
    CHECK(rig->client->pendingInputCount() == 0);
}

TEST_CASE("GameReplication: a correct prediction survives the server's answer unchanged")
{
    auto rig = makePredRig();

    rig->client->pushInput(glm::vec3(10.0f, 0.0f, 0.0f), 0.0f, 0.1f);
    for (int i = 0; i < 8; ++i) rig->pump(1.0f / 60.0f, 1);

    // Both ran the same deterministic step, so reconciliation must be a no-op —
    // a correction here would be felt as a jitter on every single move.
    CHECK(posOf(rig->clientWorld, rig->clientEntity).x == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(rig->client->stats().reconciliations == 0);
}

TEST_CASE("GameReplication: inputs still in flight are replayed on top of the server state")
{
    auto rig = makePredRig();

    // Three moves of +1 each (10 units/s for 0.1 s — the largest step a single
    // command may carry, so the arithmetic stays exact).
    rig->client->pushInput(glm::vec3(10.0f, 0, 0), 0.0f, 0.1f);
    for (int i = 0; i < 6; ++i) rig->pump(1.0f / 60.0f, 1);

    rig->client->pushInput(glm::vec3(10.0f, 0, 0), 0.0f, 0.1f);
    rig->client->pushInput(glm::vec3(10.0f, 0, 0), 0.0f, 0.1f);

    // The client is at 3; the server only knows about the first move.
    CHECK(posOf(rig->clientWorld, rig->clientEntity).x == doctest::Approx(3.0f));

    for (int i = 0; i < 8; ++i) rig->pump(1.0f / 60.0f, 1);

    // Everything reconciles to 3, not back to 1 — replaying the unacknowledged
    // moves is what stops the character snapping backwards mid-run.
    CHECK(posOf(rig->clientWorld, rig->clientEntity).x == doctest::Approx(3.0f).epsilon(0.01));
}

TEST_CASE("GameReplication: a large misprediction snaps instead of sliding")
{
    GameReplication::Config cfg;
    cfg.reconcileSnapDistance = 1.0f;
    auto rig = makePredRig(cfg);

    // Teleport the server's copy — as a hit, a trigger or an anti-cheat
    // correction would.
    rig->serverWorld.registry().get<TransformComponent>(rig->serverEntity).position =
        glm::vec3(100.0f, 0.0f, 0.0f);

    for (int i = 0; i < 10; ++i) rig->pump(1.0f / 60.0f, 1);

    // Easing a 100-unit error would leave the player acting on a position that
    // is wrong for a visible stretch of time.
    CHECK(rig->client->stats().hardSnaps > 0);
    CHECK(posOf(rig->clientWorld, rig->clientEntity).x == doctest::Approx(100.0f).epsilon(0.01));
}

TEST_CASE("GameReplication: a small misprediction is eased, not jumped")
{
    GameReplication::Config cfg;
    cfg.reconcileSnapDistance = 5.0f;
    cfg.reconcileSmoothing    = 4.0f;   // slow enough to observe mid-flight
    auto rig = makePredRig(cfg);

    rig->serverWorld.registry().get<TransformComponent>(rig->serverEntity).position =
        glm::vec3(1.0f, 0.0f, 0.0f);

    // One exchange: the correction has landed but smoothing has barely started.
    for (int i = 0; i < 4; ++i) rig->pump(1.0f / 240.0f, 1);

    const float justAfter = posOf(rig->clientWorld, rig->clientEntity).x;
    CHECK(rig->client->stats().hardSnaps == 0);
    CHECK(justAfter < 0.9f);   // not teleported to the server's value

    // Given time, it converges.
    for (int i = 0; i < 120; ++i) rig->pump(1.0f / 60.0f, 1);
    CHECK(posOf(rig->clientWorld, rig->clientEntity).x == doctest::Approx(1.0f).epsilon(0.05));
}

TEST_CASE("GameReplication: a client cannot move an entity it was not assigned")
{
    auto rig = makePredRig();

    // Revoke the assignment, then try to drive it anyway.
    rig->server->assignControl(LoopbackTransport::kPeer, 99999);
    rig->client->pushInput(glm::vec3(50.0f, 0, 0), 0.0f, 0.1f);
    for (int i = 0; i < 8; ++i) rig->pump(1.0f / 60.0f, 1);

    // Otherwise anyone could drive anyone else's character.
    CHECK(posOf(rig->serverWorld, rig->serverEntity).x == doctest::Approx(0.0f));
    CHECK(rig->server->stats().inputsProcessed == 0);
}

TEST_CASE("GameReplication: replayed input is not applied twice by the server")
{
    auto rig = makePredRig();

    rig->client->pushInput(glm::vec3(10.0f, 0, 0), 0.0f, 0.1f);
    for (int i = 0; i < 6; ++i) rig->pump(1.0f / 60.0f, 1);
    const float afterFirst = posOf(rig->serverWorld, rig->serverEntity).x;

    // Hand-send the SAME sequence number again, as a duplicated datagram would.
    BitWriter w;
    w.writeUInt32(1);
    w.writeFloat(0.1f);
    for (int i = 0; i < 3; ++i) w.writeFloat(i == 0 ? 10.0f : 0.0f);
    w.writeFloat(0.0f);
    rig->clientNet->send(LoopbackTransport::kPeer, kMsgInput, w, SendMode::Unreliable);
    for (int i = 0; i < 6; ++i) rig->pump(1.0f / 60.0f, 1);

    // Duplicates and reordering are normal on an unreliable channel; applying
    // one twice would move the character further than the player asked.
    CHECK(posOf(rig->serverWorld, rig->serverEntity).x == doctest::Approx(afterFirst));
}

TEST_CASE("GameReplication: an absurd client timestep cannot teleport the character")
{
    auto rig = makePredRig();

    // A modified client claiming a 10-second frame.
    BitWriter w;
    w.writeUInt32(1);
    w.writeFloat(10.0f);
    w.writeFloat(100.0f); w.writeFloat(0.0f); w.writeFloat(0.0f);
    w.writeFloat(0.0f);
    rig->clientNet->send(LoopbackTransport::kPeer, kMsgInput, w, SendMode::Unreliable);
    for (int i = 0; i < 6; ++i) rig->pump(1.0f / 60.0f, 1);

    // Clamped to a plausible frame, so the move is bounded rather than 1000 units.
    CHECK(posOf(rig->serverWorld, rig->serverEntity).x <= doctest::Approx(10.0f));
}

TEST_CASE("GameReplication: rotation takes the short way across the ±180° seam")
{
    GameReplication::Config cfg;
    cfg.tickHz = 60.0f;
    cfg.interpolationDelaySec = 0.2f;   // slow, so mid-flight is observable
    auto rig = makeRig(cfg);

    std::uint32_t netId = 0;
    const Entity clientEntity = mirrorEntity(*rig, "Spinner", netId);
    const Entity serverEntity =
        rig->serverWorld.registry().view<NetworkComponent>().front();

    auto& serverTc = rig->serverWorld.registry().get<TransformComponent>(serverEntity);

    // Establish 179° as the previous sample...
    serverTc.rotation = glm::vec3(0.0f, 179.0f, 0.0f);
    for (int i = 0; i < 6; ++i) rig->pump(1.0f / 60.0f, 1);

    // ...then cross the wrap to -179°. That is a 2° turn.
    serverTc.rotation = glm::vec3(0.0f, -179.0f, 0.0f);
    for (int i = 0; i < 3; ++i) rig->pump(1.0f / 60.0f, 1);

    // Sample mid-interpolation. A componentwise lerp of the Euler values would
    // walk 179 → 0 → -179: the long way round, a visible 358° spin every time
    // something crosses the wrap. Spherical interpolation stays near ±180.
    const float y = rig->clientWorld.registry()
                        .get<TransformComponent>(clientEntity).rotation.y;
    CHECK(std::abs(y) > 170.0f);
}

// ─── Snapshot ordering over an unreliable channel ────────────────────────────
// Over UDP a snapshot datagram may arrive late, twice, or not at all. The
// header carries a tick number for exactly that: older than the newest applied
// is dropped whole, the same tick is another part of a split snapshot, and a
// second sample of an entity for the same tick is a duplicate. These cases
// hand-write datagrams so the ordering logic is pinned directly, with no
// randomness in between.

namespace {

// One-entity snapshot in the exact wire layout sendSnapshots produces.
void sendRawSnapshot(Rig& rig, std::uint32_t tick, std::uint32_t ack, std::uint32_t netId,
                     const glm::vec3& pos, const GameReplication::Config& cfg = {}) {
    BitWriter w;
    w.writeUInt32(tick);
    w.writeUInt32(ack);
    w.writeUInt16(1);
    w.writeUInt32(netId);
    for (int i = 0; i < 3; ++i)
        w.writeFloatQuantized(pos[i], -cfg.worldExtent, cfg.worldExtent, cfg.positionBits);
    for (int i = 0; i < 3; ++i) w.writeFloatQuantized(0.0f, -180.0f, 180.0f, cfg.rotationBits);
    rig.serverNet->send(LoopbackTransport::kPeer, kMsgSnapshot, w, SendMode::Unreliable);
}

// A client-side entity under a chosen id, with nothing registered on the
// server so its own tick loop stays silent and only the hand-written
// datagrams reach the client.
Entity adoptOnly(Rig& rig, const char* name, std::uint32_t netId) {
    const Entity c = rig.clientWorld.createEntity(name);
    rig.clientWorld.registry().emplace_or_replace<TransformComponent>(c);
    rig.client->adoptEntity(c, netId);
    return c;
}

} // namespace

TEST_CASE("GameReplication: a snapshot older than the newest applied is dropped whole")
{
    GameReplication::Config cfg;
    cfg.interpolationDelaySec = 0.001f;   // converge at once, for assertions
    auto rig = makeRig(cfg);
    const Entity e = adoptOnly(*rig, "Late", 7);

    sendRawSnapshot(*rig, 5, 0, 7, glm::vec3(5.0f, 0, 0), cfg);
    rig->pump(1.0f / 30.0f, 2);
    CHECK(posOf(rig->clientWorld, e).x == doctest::Approx(5.0f).epsilon(0.001));
    CHECK(rig->client->stats().snapshotsStale == 0);

    // Tick 3 arrives after tick 5: a reordered datagram. Applying it would
    // move the entity backwards in time.
    sendRawSnapshot(*rig, 3, 0, 7, glm::vec3(3.0f, 0, 0), cfg);
    rig->pump(1.0f / 30.0f, 2);
    CHECK(posOf(rig->clientWorld, e).x == doctest::Approx(5.0f).epsilon(0.001));
    CHECK(rig->client->stats().snapshotsStale == 1);
    CHECK(rig->client->stats().snapshotsReceived == 2);

    // A newer tick is applied as usual.
    sendRawSnapshot(*rig, 6, 0, 7, glm::vec3(6.0f, 0, 0), cfg);
    rig->pump(1.0f / 30.0f, 2);
    CHECK(posOf(rig->clientWorld, e).x == doctest::Approx(6.0f).epsilon(0.001));
    CHECK(rig->client->stats().snapshotsStale == 1);
}

TEST_CASE("GameReplication: the same tick in a second datagram is another part, not a stale one")
{
    GameReplication::Config cfg;
    cfg.interpolationDelaySec = 0.001f;
    auto rig = makeRig(cfg);
    const Entity a = adoptOnly(*rig, "A", 7);
    const Entity b = adoptOnly(*rig, "B", 8);

    // The server splits a large snapshot across datagrams that all carry the
    // same tick; the second part must land even though the tick is not newer.
    sendRawSnapshot(*rig, 9, 0, 7, glm::vec3(1.0f, 0, 0), cfg);
    sendRawSnapshot(*rig, 9, 0, 8, glm::vec3(2.0f, 0, 0), cfg);
    rig->pump(1.0f / 30.0f, 2);

    CHECK(posOf(rig->clientWorld, a).x == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(posOf(rig->clientWorld, b).x == doctest::Approx(2.0f).epsilon(0.001));
    CHECK(rig->client->stats().snapshotsStale == 0);
    CHECK(rig->client->stats().samplesDuplicate == 0);
}

TEST_CASE("GameReplication: the tick counter may wrap without a stall")
{
    GameReplication::Config cfg;
    cfg.interpolationDelaySec = 0.001f;
    auto rig = makeRig(cfg);
    const Entity e = adoptOnly(*rig, "Wrap", 7);

    // Walk the client's notion of "newest" up to the top of the range in two
    // signed-positive steps; a fresh client (newest 0) would rightly treat
    // 0xFFFFFFFE itself as two ticks BEFORE anything it has seen.
    sendRawSnapshot(*rig, 0x7FFFFFFFu, 0, 7, glm::vec3(0.5f, 0, 0), cfg);
    rig->pump(1.0f / 30.0f, 2);
    sendRawSnapshot(*rig, 0xFFFFFFFEu, 0, 7, glm::vec3(1.0f, 0, 0), cfg);
    rig->pump(1.0f / 30.0f, 2);
    REQUIRE(posOf(rig->clientWorld, e).x == doctest::Approx(1.0f).epsilon(0.001));
    REQUIRE(rig->client->stats().snapshotsStale == 0);

    // Numerically far smaller, but three ticks LATER once the counter wrapped.
    // A plain "tick > newest" would drop every snapshot from here on.
    sendRawSnapshot(*rig, 1, 0, 7, glm::vec3(2.0f, 0, 0), cfg);
    rig->pump(1.0f / 30.0f, 2);
    CHECK(posOf(rig->clientWorld, e).x == doctest::Approx(2.0f).epsilon(0.001));
    CHECK(rig->client->stats().snapshotsStale == 0);

    // And a straggler from before the wrap is still recognised as old.
    sendRawSnapshot(*rig, 0xFFFFFFF0u, 0, 7, glm::vec3(3.0f, 0, 0), cfg);
    rig->pump(1.0f / 30.0f, 2);
    CHECK(posOf(rig->clientWorld, e).x == doctest::Approx(2.0f).epsilon(0.001));
    CHECK(rig->client->stats().snapshotsStale == 1);
}

TEST_CASE("GameReplication: a duplicated snapshot does not restart the interpolation")
{
    GameReplication::Config cfg;
    cfg.interpolationDelaySec = 0.2f;   // slow, so mid-flight is observable
    auto rig = makeRig(cfg);
    const Entity e = adoptOnly(*rig, "Dup", 7);

    // Origin sample, then the target: the entity is now easing 0 → 10 over 0.2 s.
    sendRawSnapshot(*rig, 1, 0, 7, glm::vec3(0.0f), cfg);
    rig->pump(0.001f, 1);
    sendRawSnapshot(*rig, 2, 0, 7, glm::vec3(10.0f, 0, 0), cfg);
    rig->pump(0.05f, 1);
    REQUIRE(posOf(rig->clientWorld, e).x == doctest::Approx(2.5f).epsilon(0.01));

    // The same datagram again, as a duplicated one would arrive. Applying it
    // would set previous = current and put the entity at 10 immediately,
    // frozen there until the next tick.
    sendRawSnapshot(*rig, 2, 0, 7, glm::vec3(10.0f, 0, 0), cfg);
    rig->pump(0.05f, 1);
    CHECK(posOf(rig->clientWorld, e).x == doctest::Approx(5.0f).epsilon(0.01));
    CHECK(rig->client->stats().samplesDuplicate == 1);
    CHECK(rig->client->stats().snapshotsStale == 0);
}

TEST_CASE("GameReplication: a duplicated snapshot reconciles the controlled entity once")
{
    auto rig = makePredRig();

    // One move, acknowledged normally, so the pending list is empty.
    rig->client->pushInput(glm::vec3(10.0f, 0, 0), 0.0f, 0.1f);
    for (int i = 0; i < 8; ++i) rig->pump(1.0f / 60.0f, 1);
    REQUIRE(rig->client->pendingInputCount() == 0);
    // Take in whatever the server's last tick left in the loopback, so the two
    // datagrams below are the only ones the client sees next.
    rig->serverT->update(); rig->clientT->update();
    rig->clientNet->pump();
    const auto before = rig->client->stats();

    // Two moves in flight, and the server's answer for the FIRST of them
    // arrives twice (tick 1000: far ahead of the server's own counter, so
    // its regular snapshots in between are the stale ones here, not ours).
    rig->client->pushInput(glm::vec3(10.0f, 0, 0), 0.0f, 0.1f);
    rig->client->pushInput(glm::vec3(10.0f, 0, 0), 0.0f, 0.1f);
    sendRawSnapshot(*rig, 1000, 2, rig->netId, glm::vec3(2.0f, 0, 0));
    sendRawSnapshot(*rig, 1000, 2, rig->netId, glm::vec3(2.0f, 0, 0));
    rig->serverT->update(); rig->clientT->update();
    rig->clientNet->pump();

    // Once: ack 2 retired one command, the other is replayed on top of 2.
    CHECK(rig->client->pendingInputCount() == 1);
    CHECK(posOf(rig->clientWorld, rig->clientEntity).x == doctest::Approx(3.0f).epsilon(0.01));
    CHECK(rig->client->stats().samplesDuplicate == before.samplesDuplicate + 1);
    CHECK(rig->client->stats().snapshotsReceived == before.snapshotsReceived + 2);
}

// ─── Datagram budget ─────────────────────────────────────────────────────────
// UdpTransport refuses an unreliable message over its MTU payload rather than
// fragmenting it (one lost piece would lose the whole), so a snapshot has to
// fit one datagram by construction, and the integrity manifest — the largest
// reliable message this layer sends — has to fit the fragment budget. Both
// are measured on the wire, not computed from the writer.

namespace {

// Everything the client's raw transport received, without the client
// NetSession consuming it. `id` is the NetSession frame's message id.
struct RawFrame {
    MessageId   id = 0;
    std::size_t bytes = 0;   // frame size: id + payload
};

std::vector<RawFrame> drainRaw(ITransport& t) {
    std::vector<RawFrame> out;
    NetEvent ev;
    while (t.poll(ev)) {
        if (ev.type != NetEventType::Data) continue;
        // Decoded the way NetSession does it, so the id is read in the bit
        // order the writer used.
        BitReader     reader(ev.data);
        std::uint16_t id = 0;
        if (!reader.readUInt16(id)) continue;
        RawFrame f;
        f.id    = static_cast<MessageId>(id);
        f.bytes = ev.data.size();
        out.push_back(f);
    }
    return out;
}

} // namespace

TEST_CASE("GameReplication: a tick with many entities is split into datagrams under the budget")
{
    GameReplication::Config cfg;
    cfg.tickHz = 60.0f;
    cfg.interpolationDelaySec = 0.001f;
    auto rig = makeRig(cfg);

    // The budget has to leave room for what sits between us and the datagram.
    REQUIRE(cfg.snapshotBudgetBytes + kSessionIdBytes + kSecureOverhead <= kUdpMtuPayload);

    constexpr int kCount = 200;   // ~3.8 KiB of samples: four datagrams' worth
    std::vector<Entity> clientEntities;
    for (int i = 0; i < kCount; ++i) {
        std::uint32_t netId = 0;
        clientEntities.push_back(mirrorEntity(*rig, "E", netId));
    }
    for (const auto [entity, nc] : rig->serverWorld.registry().view<NetworkComponent>().each())
        rig->serverWorld.registry().get<TransformComponent>(entity).position =
            glm::vec3(static_cast<float>(nc.netId), 0.0f, 0.0f);

    // Phase 1: one tick, read raw off the client's transport so every
    // datagram's size is visible before NetSession unpacks it.
    rig->serverT->update(); rig->clientT->update();
    rig->serverNet->pump();
    rig->server->update(1.0f / 60.0f);
    rig->serverT->update(); rig->clientT->update();
    const auto frames = drainRaw(*rig->clientT);

    std::size_t snapshots = 0, largest = 0;
    for (const RawFrame& f : frames) {
        if (f.id != kMsgSnapshot) continue;
        ++snapshots;
        largest = std::max(largest, f.bytes);
        CHECK(f.bytes <= cfg.snapshotBudgetBytes + kSessionIdBytes);
    }
    CHECK(snapshots >= 4);
    CHECK(largest > cfg.snapshotBudgetBytes / 2);   // the budget is actually used, not one entity per datagram
    CHECK(rig->server->stats().snapshotsSplit >= 3);
    CHECK(rig->server->stats().entitiesSent == kCount);

    // Phase 2: normal pumping. Every entity lands, each with its own sample.
    for (int i = 0; i < 6; ++i) rig->pump(1.0f / 60.0f, 2);
    for (const Entity e : clientEntities) {
        const auto& nc = rig->clientWorld.registry().get<NetworkComponent>(e);
        CHECK(posOf(rig->clientWorld, e).x ==
              doctest::Approx(static_cast<float>(nc.netId)).epsilon(0.001));
    }
    CHECK(rig->client->stats().snapshotsStale == 0);
    CHECK(rig->client->stats().samplesDuplicate == 0);
}

TEST_CASE("GameReplication: the worst-case integrity manifest fits the reliable fragment budget")
{
    auto rig = makeRig();

    // The largest manifest the writer can produce: the entry cap, file names
    // at the file-system maximum, SHA-256 hashes.
    HE::Integrity::Manifest m;
    for (std::uint16_t i = 0; i < GameReplication::kMaxManifestEntries; ++i) {
        HE::Integrity::Entry e;
        e.kind = HE::Integrity::FileKind::Pak;
        e.name = std::string(255, 'n');
        e.hash = std::string(64, 'a');
        m.entries.push_back(std::move(e));
    }
    rig->client->setLocalManifest(std::move(m));
    rig->client->update(1.0f / 60.0f);   // sends it once per connection
    REQUIRE(rig->client->stats().manifestsSent == 1);

    rig->clientT->update(); rig->serverT->update();
    const auto frames = drainRaw(*rig->serverT);
    std::size_t manifestBytes = 0;
    for (const RawFrame& f : frames)
        if (f.id == kMsgIntegrity) manifestBytes = f.bytes;
    REQUIRE(manifestBytes > 0);

    const std::size_t onTheWire = manifestBytes + kSecureOverhead;
    const std::size_t perFragment =
        kUdpMtuPayload - UdpTransport::kMsgSeqSize - UdpTransport::kFragmentHeaderSize;
    const std::size_t fragments = (onTheWire + perFragment - 1) / perFragment;
    MESSAGE("worst-case manifest: " << manifestBytes << " B, " << fragments
            << " fragments of " << UdpTransport::kMaxFragments);
    CHECK(onTheWire <= kUdpMaxReliable);
    CHECK(fragments <= UdpTransport::kMaxFragments);
    // Well under, not just under: the fragment count is a byte, and a manifest
    // twice this size would still have to fit.
    CHECK(fragments * 2 <= UdpTransport::kMaxFragments);
}

// ─── Over a bad network ──────────────────────────────────────────────────────
// LossyTransport over the loopback pair: loss, reorder and duplication on the
// unreliable channel with a seeded generator, so a failure is the same failure
// every run. Each hostile case has a clean twin with identical steps and an
// all-zero config: the counters that must fire under damage must stay at zero
// there, or they would be measuring something other than the damage.

namespace {

LossyTransport::Config hostileConfig(std::uint32_t seed) {
    LossyTransport::Config c;
    c.lossPercent      = 10.f;
    c.reorderPercent   = 20.f;
    c.duplicatePercent = 5.f;
    c.latencyMs        = 30;
    c.jitterMs         = 10;
    // Further than one tick (33 ms at 30 Hz), or a reordered snapshot could
    // never overtake the next one and the ordering would go untested.
    c.reorderDelayMs   = 100;
    c.seed             = seed;
    return c;
}

LossyTransport::Config cleanConfig(std::uint32_t seed) {
    LossyTransport::Config c;
    c.seed = seed;
    return c;
}

struct LossyRig {
    std::unique_ptr<LossyTransport>  serverT, clientT;
    std::unique_ptr<NetSession>      serverNet, clientNet;
    std::unique_ptr<GameReplication> server, client;
    HorizonWorld serverWorld, clientWorld;
    Entity        serverEntity = entt::null;
    Entity        clientEntity = entt::null;
    std::uint32_t netId = 0;

    // Fixed 20 ms frames, so the decorator's millisecond clock advances by a
    // whole number each round.
    static constexpr float kDt = 0.02f;

    void pump(int rounds) {
        for (int i = 0; i < rounds; ++i) {
            serverT->advance(20);
            clientT->advance(20);
            serverT->update();
            clientT->update();
            serverNet->pump();
            clientNet->pump();
            server->update(kDt);
            client->update(kDt);
        }
    }

    // Deliver everything still queued, then run a quiet stretch so the last
    // snapshot lands and any smoothing offset decays.
    void drain() {
        serverT->flush();
        clientT->flush();
        serverT->setConfig(cleanConfig(1));
        clientT->setConfig(cleanConfig(2));
        pump(60);
    }
};

std::unique_ptr<LossyRig> makeLossyRig(const LossyTransport::Config& serverSide,
                                       const LossyTransport::Config& clientSide,
                                       GameReplication::Config cfg = {}) {
    auto r = std::make_unique<LossyRig>();
    auto [a, b] = LoopbackTransport::createPair();
    r->serverT = LossyTransport::wrap(std::move(a), serverSide);
    r->clientT = LossyTransport::wrap(std::move(b), clientSide);
    REQUIRE(r->serverT != nullptr);
    REQUIRE(r->clientT != nullptr);

    r->serverNet = std::make_unique<NetSession>(r->serverT.get(), NetRole::Server);
    r->clientNet = std::make_unique<NetSession>(r->clientT.get(), NetRole::Client);
    r->server = std::make_unique<GameReplication>(r->serverNet.get(), NetRole::Server, cfg);
    r->client = std::make_unique<GameReplication>(r->clientNet.get(), NetRole::Client, cfg);
    r->server->setWorld(&r->serverWorld);
    r->client->setWorld(&r->clientWorld);
    r->server->setMoveFunction(simpleMover());
    r->client->setMoveFunction(simpleMover());

    r->serverT->update(); r->clientT->update();
    r->serverNet->pump(); r->clientNet->pump();

    r->serverEntity = r->serverWorld.createEntity("Player");
    r->serverWorld.registry().emplace_or_replace<TransformComponent>(r->serverEntity);
    r->netId = r->server->registerEntity(r->serverEntity);

    r->clientEntity = r->clientWorld.createEntity("Player");
    r->clientWorld.registry().emplace_or_replace<TransformComponent>(r->clientEntity);
    r->client->adoptEntity(r->clientEntity, r->netId);
    return r;
}

// An interpolated entity the server drives monotonically along +x. Returns the
// client's x after every frame, so the caller can check the order in which
// samples took effect.
std::vector<float> runInterpolatedWalk(LossyRig& rig, int frames) {
    std::vector<float> trace;
    auto& tc = rig.serverWorld.registry().get<TransformComponent>(rig.serverEntity);
    for (int i = 0; i < frames; ++i) {
        tc.position.x += 0.5f;   // 25 units/s
        tc.dirty = true;
        rig.pump(1);
        trace.push_back(posOf(rig.clientWorld, rig.clientEntity).x);
    }
    rig.drain();
    return trace;
}

bool nonDecreasing(const std::vector<float>& v) {
    for (std::size_t i = 1; i < v.size(); ++i)
        if (v[i] < v[i - 1] - 1e-3f) return false;
    return true;
}

// The predicted player: a burst of moves, then still frames with zero moves so
// the server's ack passes any command a lost datagram swallowed, then quiet.
void runPredictedBurst(LossyRig& rig) {
    rig.server->assignControl(LoopbackTransport::kPeer, rig.netId);
    rig.client->setLocallyControlled(rig.clientEntity, rig.netId);
    for (int i = 0; i < 60; ++i) {
        rig.client->pushInput(glm::vec3(10.0f, 0, 0), 0.0f, LossyRig::kDt);   // +0.2 each
        rig.pump(1);
    }
    for (int i = 0; i < 60; ++i) {
        rig.client->pushInput(glm::vec3(0.0f), 0.0f, LossyRig::kDt);
        rig.pump(1);
    }
    rig.drain();
}

} // namespace

TEST_CASE("GameReplication: interpolation never runs backwards under loss, reorder and duplication")
{
    GameReplication::Config cfg;
    cfg.interpolationDelaySec = 0.05f;
    auto rig = makeLossyRig(hostileConfig(7), hostileConfig(11), cfg);

    const auto trace = runInterpolatedWalk(*rig, 300);   // 6 s, ~180 ticks

    // The damage really happened...
    CHECK(rig->serverT->stats().dropped > 0);
    CHECK(rig->serverT->stats().reordered > 0);
    CHECK(rig->serverT->stats().duplicated > 0);
    // ...and the ordering logic dealt with it: late ticks dropped, duplicates
    // recognised, nothing applied out of order.
    CHECK(rig->client->stats().snapshotsStale > 0);
    CHECK(rig->client->stats().samplesDuplicate > 0);
    CHECK(nonDecreasing(trace));
    // The last snapshot that made it through brings the client to the truth.
    CHECK(posOf(rig->clientWorld, rig->clientEntity).x ==
          doctest::Approx(posOf(rig->serverWorld, rig->serverEntity).x).epsilon(0.001));
}

TEST_CASE("GameReplication: (control) over a clean link none of the ordering counters fire")
{
    GameReplication::Config cfg;
    cfg.interpolationDelaySec = 0.05f;
    auto rig = makeLossyRig(cleanConfig(7), cleanConfig(11), cfg);

    const auto trace = runInterpolatedWalk(*rig, 300);

    CHECK(rig->serverT->stats().dropped == 0);
    CHECK(rig->serverT->stats().reordered == 0);
    CHECK(rig->serverT->stats().duplicated == 0);
    CHECK(rig->client->stats().snapshotsStale == 0);
    CHECK(rig->client->stats().samplesDuplicate == 0);
    // Everything sent arrived (the last round's snapshot may still be in the
    // decorator's queue, sent after the client pumped).
    CHECK(rig->client->stats().snapshotsReceived + 1 >= rig->server->stats().snapshotsSent);
    CHECK(rig->client->stats().snapshotsReceived <= rig->server->stats().snapshotsSent);
    CHECK(nonDecreasing(trace));
    CHECK(posOf(rig->clientWorld, rig->clientEntity).x ==
          doctest::Approx(posOf(rig->serverWorld, rig->serverEntity).x).epsilon(0.001));
}

TEST_CASE("GameReplication: prediction converges on the server under loss, reorder and duplication")
{
    auto rig = makeLossyRig(hostileConfig(3), hostileConfig(5));

    runPredictedBurst(*rig);

    // Inputs were lost and duplicated on the way up...
    CHECK(rig->clientT->stats().dropped > 0);
    CHECK(rig->clientT->stats().duplicated > 0);
    // ...so the server ran fewer commands than were sent, and never one twice:
    // what it processed is bounded by what the decorator let through ONCE
    // (sent minus dropped), no matter how many copies of those it made.
    const auto uniqueArrivals = rig->client->stats().inputsSent -
                                static_cast<std::uint32_t>(rig->clientT->stats().dropped);
    CHECK(rig->server->stats().inputsProcessed <= uniqueArrivals);
    CHECK(rig->server->stats().inputsProcessed < rig->client->stats().inputsSent);
    // A lost command is a misprediction the client corrected...
    CHECK(rig->client->stats().reconciliations > 0);
    // ...and after the quiet stretch both sides agree on where the player is.
    // The server moved less than the 12 units asked for, because commands
    // went missing; the client must have followed it there, not stayed at 12.
    const float serverX = posOf(rig->serverWorld, rig->serverEntity).x;
    CHECK(serverX < 12.0f);
    CHECK(serverX > 6.0f);
    CHECK(posOf(rig->clientWorld, rig->clientEntity).x == doctest::Approx(serverX).epsilon(0.002));
}

TEST_CASE("GameReplication: (control) over a clean link prediction is never corrected")
{
    auto rig = makeLossyRig(cleanConfig(3), cleanConfig(5));

    runPredictedBurst(*rig);

    CHECK(rig->clientT->stats().dropped == 0);
    CHECK(rig->server->stats().inputsProcessed == rig->client->stats().inputsSent);
    CHECK(rig->client->stats().reconciliations == 0);
    CHECK(rig->client->stats().hardSnaps == 0);
    CHECK(rig->client->stats().snapshotsStale == 0);
    CHECK(posOf(rig->serverWorld, rig->serverEntity).x == doctest::Approx(12.0f).epsilon(0.001));
    CHECK(posOf(rig->clientWorld, rig->clientEntity).x == doctest::Approx(12.0f).epsilon(0.001));
}
