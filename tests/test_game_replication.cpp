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

    // Claim three entities, supply none.
    BitWriter w;
    w.writeUInt16(3);
    rig->serverNet->send(LoopbackTransport::kPeer, kFirstUserMessage + 200, w,
                         SendMode::Unreliable);

    for (int i = 0; i < 4; ++i) rig->pump(1.0f / 30.0f, 1);

    // It counts as received, but nothing was written into the world from it.
    CHECK(rig->client->stats().snapshotsReceived >= 1);
    CHECK(rig->clientWorld.registry().view<TransformComponent>().size() == 0);
}
