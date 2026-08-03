#include "doctest.h"

#include <HorizonScene/GameReplication.h>
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/NetworkComponent.h>

#include <Net/LoopbackTransport.h>
#include <Net/NetSession.h>

#include <memory>

using namespace HE::Net;

// ─── Gameplay replication (Layer 3a) ─────────────────────────────────────────
// Deliberately separate from editor collaboration: this replicates simulation
// state at a tick rate and tolerates loss, where collab replicates authored
// edits reliably. They share the transport and nothing above it.

namespace {

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

    // A valid header (ack + count) claiming three entities, but no entity data.
    BitWriter w;
    w.writeUInt32(0);    // last input we acknowledged
    w.writeUInt16(3);
    rig->serverNet->send(LoopbackTransport::kPeer, kFirstUserMessage + 200, w,
                         SendMode::Unreliable);

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
    rig->clientNet->send(LoopbackTransport::kPeer, kFirstUserMessage + 201, w,
                         SendMode::Unreliable);
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
    rig->clientNet->send(LoopbackTransport::kPeer, kFirstUserMessage + 201, w,
                         SendMode::Unreliable);
    for (int i = 0; i < 6; ++i) rig->pump(1.0f / 60.0f, 1);

    // Clamped to a plausible frame, so the move is bounded rather than 1000 units.
    CHECK(posOf(rig->serverWorld, rig->serverEntity).x <= doctest::Approx(10.0f));
}
