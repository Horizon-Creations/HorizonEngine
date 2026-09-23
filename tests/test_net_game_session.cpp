#include "doctest.h"

#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/NetworkComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/GameReplication.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Net/NetGameSession.h>
#include <HorizonScene/Net/NetMessages.h>

#include <Net/BitStream.h>
#include <Net/ITransport.h>
#include <Net/LossyTransport.h>
#include <Project/ProjectSettings.h>

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace HE::Net;
namespace G = HE::Net::Game;

// ─── The first real session (plan §11.5) ─────────────────────────────────────
// Two and three NetGameSessions against each other, headless, on a simulated
// clock: no sockets, no ports, no waiting, and the same failure every run.

namespace {

// ── A loopback that takes MORE THAN ONE client ───────────────────────────────
// LoopbackTransport is strictly 1:1 (one peer pointer, kPeer = 1), and a host
// with a late joiner is by definition 1:N. Rather than widen the shipped
// transport for a test, the wiring lives here: one shared fabric, one endpoint
// object per end, connection ids minted by the fabric.
//
// Deliberately dumb — no loss, no ordering games, no clock. Everything hostile
// is LossyTransport's job, which wraps these; mixing the two would make it
// impossible to say which layer a failure came from.
struct Fabric {
    std::deque<NetEvent>                                  hostIn;
    std::unordered_map<ConnectionId, std::deque<NetEvent>> clientIn;
    std::unordered_map<ConnectionId, bool>                 up;
    ConnectionId                                           next = 1;
};

class HubEndpoint final : public ITransport {
public:
    using ITransport::send;
    // `id` is the connection id this client is known by on the host; ignored
    // for the host endpoint, which addresses every client by theirs.
    HubEndpoint(std::shared_ptr<Fabric> f, bool host, ConnectionId id)
        : m_f(std::move(f)), m_host(host), m_id(id) {}

    // A client whose transport is destroyed takes its link down with it, the
    // way LoopbackTransport's destructor does and the way a process that quits
    // does. Without this the host would keep a roster entry for a peer that no
    // longer exists anywhere.
    ~HubEndpoint() override {
        if (m_host) return;
        const auto it = m_f->up.find(m_id);
        if (it == m_f->up.end() || !it->second) return;
        it->second = false;
        m_f->hostIn.push_back(NetEvent{ NetEventType::Disconnected, m_id, {} });
    }

    void update() override {}

    void send(ConnectionId conn, const std::uint8_t* d, std::size_t len, SendMode) override {
        if (m_host) {
            const auto it = m_f->up.find(conn);
            if (it == m_f->up.end() || !it->second) return;
            NetEvent e{ NetEventType::Data, 1, {} };
            e.data.assign(d, d + len);
            m_f->clientIn[conn].push_back(std::move(e));
        } else {
            if (conn != 1) return;
            const auto it = m_f->up.find(m_id);
            if (it == m_f->up.end() || !it->second) return;
            NetEvent e{ NetEventType::Data, m_id, {} };
            e.data.assign(d, d + len);
            m_f->hostIn.push_back(std::move(e));
        }
    }

    bool poll(NetEvent& out) override {
        auto& q = m_host ? m_f->hostIn : m_f->clientIn[m_id];
        if (q.empty()) return false;
        out = std::move(q.front());
        q.pop_front();
        return true;
    }

    void disconnect(ConnectionId conn) override {
        // Like every real transport here: the INITIATOR gets no event of its
        // own, which is exactly the asymmetry NetGameSession has to reap for.
        if (m_host) {
            const auto it = m_f->up.find(conn);
            if (it == m_f->up.end() || !it->second) return;
            it->second = false;
            m_f->clientIn[conn].push_back(NetEvent{ NetEventType::Disconnected, 1, {} });
        } else {
            const auto it = m_f->up.find(m_id);
            if (it == m_f->up.end() || !it->second) return;
            it->second = false;
            m_f->hostIn.push_back(NetEvent{ NetEventType::Disconnected, m_id, {} });
        }
    }

    std::size_t connectionCount() const override {
        if (!m_host) {
            const auto it = m_f->up.find(m_id);
            return (it != m_f->up.end() && it->second) ? 1u : 0u;
        }
        std::size_t n = 0;
        for (const auto& [conn, live] : m_f->up) if (live) ++n;
        return n;
    }

private:
    std::shared_ptr<Fabric> m_f;
    bool                    m_host;
    ConnectionId            m_id;
};

// The bad network both directions run through. Reliable traffic is only
// delayed (that is what LossyTransport models above UdpTransport), so a join
// handshake always completes while the snapshots around it are mistreated.
LossyTransport::Config hostileNet(std::uint32_t seed) {
    LossyTransport::Config c;
    c.lossPercent      = 10.0f;
    c.reorderPercent   = 10.0f;
    c.duplicatePercent = 5.0f;
    c.latencyMs        = 50;
    c.jitterMs         = 20;
    c.seed             = seed;
    return c;
}

// Every frame advances more than latency + jitter (50 + 20). Below that, a
// message enqueued in one frame is not due in the next — and the anti-cheat
// kick would then sever the link before its own notice had left the queue,
// which LossyTransport::disconnect drops with the link. The test would show a
// kick with no notice and blame the wrong layer.
constexpr std::uint32_t kStepMs = 100;

struct Peer {
    std::unique_ptr<HorizonWorld>    world   = std::make_unique<HorizonWorld>();
    std::unique_ptr<NetGameSession>  session = std::make_unique<NetGameSession>();
    LossyTransport*                  lossy   = nullptr;   // owned by the session
    // Client side: what the spawn seam was asked to make. Step 5 binds
    // Ctx::createObject here; at this layer the callback firing IS the oracle
    // for "the class ran on the client".
    int                              spawnCalls   = 0;
    int                              despawnCalls = 0;
    // What kMsgControl did on this side: how often the application was told an
    // entity is ours, and which one it was told about last.
    int                              controlCalls = 0;
    Entity                           controlled   = entt::null;
    std::uint32_t                    controlNetId = 0;
    std::vector<std::string>         spawnedClasses;
};

struct Rig {
    std::shared_ptr<Fabric> fabric = std::make_shared<Fabric>();
    Peer                    host;
    std::vector<std::unique_ptr<Peer>> clients;

    void step(int frames = 1, float dt = 1.0f / 60.0f) {
        for (int i = 0; i < frames; ++i) {
            host.lossy->advance(kStepMs);
            for (auto& c : clients) c->lossy->advance(kStepMs);
            host.session->update(dt);
            for (auto& c : clients) c->session->update(dt);
        }
    }

    // Drain and classify the events a peer collected.
    static int countEvents(Peer& p, NetGameSession::Event::Kind kind, int* lastReason = nullptr) {
        int n = 0;
        NetGameSession::Event e;
        while (p.session->takeEvent(e)) {
            if (e.kind != kind) continue;
            ++n;
            if (lastReason) *lastReason = e.reason;
        }
        return n;
    }
};

void startHost(Rig& rig, NetGameSession::HostOptions options) {
    auto ep    = std::make_unique<HubEndpoint>(rig.fabric, /*host*/ true, 0);
    auto lossy = LossyTransport::wrap(std::move(ep), hostileNet(1));
    rig.host.lossy = lossy.get();
    rig.host.session->setWorld(rig.host.world.get());
    REQUIRE(rig.host.session->hostOn(std::move(lossy), options));
}

// `forcedId` reuses a connection id a previous client held — the case the
// anti-cheat plan warns about, where a new peer would inherit the old one's
// per-connection state.
Peer& addClient(Rig& rig, NetGameSession::JoinOptions options,
                std::uint32_t seed, ConnectionId forcedId = 0) {
    const ConnectionId id = forcedId != 0 ? forcedId : rig.fabric->next++;
    rig.fabric->up[id] = true;
    rig.fabric->hostIn.push_back(NetEvent{ NetEventType::Connected, id, {} });
    rig.fabric->clientIn[id].push_back(NetEvent{ NetEventType::Connected, 1, {} });

    auto peer  = std::make_unique<Peer>();
    auto ep    = std::make_unique<HubEndpoint>(rig.fabric, /*host*/ false, id);
    auto lossy = LossyTransport::wrap(std::move(ep), hostileNet(seed));
    peer->lossy = lossy.get();
    peer->session->setWorld(peer->world.get());
    REQUIRE(peer->session->joinOn(std::move(lossy), options));

    Peer* raw = peer.get();
    raw->session->spawns()->setSpawnFunction(
        [raw](const std::string& classPath, const glm::vec3& pos, const glm::vec3& rot,
              G::PlayerId) -> Entity {
            ++raw->spawnCalls;
            raw->spawnedClasses.push_back(classPath);
            const Entity e = raw->world->createEntity(classPath);
            auto& tc = raw->world->registry().emplace_or_replace<TransformComponent>(e);
            tc.position = pos;
            tc.rotation = rot;
            return e;
        });
    raw->session->spawns()->setDespawnFunction([raw](Entity e) {
        ++raw->despawnCalls;
        if (raw->world->registry().valid(e)) raw->world->destroyEntity(e);
    });
    // The application half of possession: in a real client this is where the
    // local player controller takes the character.
    raw->session->setControlFunction([raw](Entity e, std::uint32_t netId) {
        ++raw->controlCalls;
        raw->controlled   = e;
        raw->controlNetId = netId;
    });

    rig.clients.push_back(std::move(peer));
    return *rig.clients.back();
}

// A replicated scene entity on the host, mirrored into a client world under the
// SAME scene uuid — which is what a client that loaded the same scene file has,
// and what kMsgBind resolves against.
Entity authoredEntity(HorizonWorld& world, const char* name, const glm::vec3& pos,
                      bool replicates, bool replicateTransform = true) {
    const Entity e = world.createEntity(name);
    auto& tc = world.registry().emplace_or_replace<TransformComponent>(e);
    tc.position = pos;
    // ALWAYS emplaced, including when the switch is off. An earlier version
    // skipped the component in that case, which made "Replicates off" and "no
    // component at all" the same entity — and the walk's `if (!replicates)
    // continue` branch, the whole point of this step, was never once executed.
    NetworkComponent nc;
    nc.replicates         = replicates;
    nc.replicateTransform = replicateTransform;
    world.registry().emplace_or_replace<NetworkComponent>(e, nc);
    return e;
}

void mirrorInto(HorizonWorld& src, HorizonWorld& dst) {
    for (const Entity e : src.registry().view<TransformComponent>()) {
        const auto* name = src.registry().try_get<NameComponent>(e);
        const Entity c = dst.createEntity(name ? name->name : std::string("Entity"));
        dst.registry().emplace_or_replace<TransformComponent>(c);
        // The scene uuid is the identity a bind travels under; without this the
        // client is a different scene as far as the protocol is concerned.
        dst.setEntityId(c, src.entityId(e));
    }
}

NetGameSession::HostOptions defaultHost() {
    NetGameSession::HostOptions o;
    o.displayName = "Host";
    o.announceLan = false;   // no sockets in a headless test
    o.projectId   = "project-a";
    o.projectLabel = "Project A";
    o.scenePath   = "scenes/arena.hescene";
    return o;
}

NetGameSession::JoinOptions defaultJoin(const char* name = "Guest") {
    NetGameSession::JoinOptions o;
    o.displayName = name;
    o.projectId   = "project-a";
    return o;
}

} // namespace

// ─── 1. Two sessions find each other ─────────────────────────────────────────

TEST_CASE("net session: host and client both reach Joined, with one event each") {
    Rig rig;
    startHost(rig, defaultHost());
    CHECK(rig.host.session->status() == NetGameSession::Status::Hosting);
    CHECK(rig.host.session->isAuthority());
    CHECK(rig.host.session->localPlayer() == G::kHostPlayer);

    Peer& client = addClient(rig, defaultJoin("Anna"), 2);
    CHECK(client.session->isClient());

    // Simulated two seconds is generous for a handshake that is only delayed.
    rig.step(20);

    CHECK(client.session->status() == NetGameSession::Status::Joined);
    CHECK(client.session->localPlayer() == 2u);
    CHECK(rig.host.session->roster().size() == 2u);
    CHECK(rig.host.session->stats().joinsAccepted == 1u);

    // Exactly once each — a Welcome that arrived twice would otherwise show up
    // as a second player with the same name.
    CHECK(Rig::countEvents(rig.host, NetGameSession::Event::Kind::PlayerJoined) == 1);
    CHECK(Rig::countEvents(client, NetGameSession::Event::Kind::Connected) == 1);

    // The host knows who joined, by name and by a player id that is not a
    // connection id.
    const G::PlayerInfo* anna = rig.host.session->roster().find(2);
    REQUIRE(anna != nullptr);
    CHECK(anna->name == "Anna");
    CHECK(anna->local == false);
}

// ─── 2. The registry walk: the switch IS the registration ────────────────────

TEST_CASE("net session: only entities with Replicates are bound") {
    Rig rig;
    authoredEntity(*rig.host.world, "Crate",  { 1, 0, 0 }, true);
    authoredEntity(*rig.host.world, "Door",   { 2, 0, 0 }, true);
    authoredEntity(*rig.host.world, "Statue", { 3, 0, 0 }, true);
    authoredEntity(*rig.host.world, "Grass",  { 4, 0, 0 }, false);   // component, switch OFF
    rig.host.world->createEntity("Decor");                           // no component at all

    // Four entities carry a NetworkComponent; only three have the switch on.
    // The two counts differing is what proves the flag is read at all — with
    // the component's mere presence as the criterion, both would be 4.
    auto& reg = rig.host.world->registry();
    CHECK(reg.view<NetworkComponent>().size() == 4u);

    startHost(rig, defaultHost());
    CHECK(rig.host.session->spawns()->boundCount() == 3u);
    CHECK(rig.host.session->replication()->replicatedCount() == 3u);

    // Grass keeps its component and its tuning — turning the switch off does
    // not throw the radius and the speed limits away (plan §8.1) — and it has
    // no session identity.
    const Entity grass = [&] {
        for (const Entity e : reg.view<NameComponent>())
            if (reg.get<NameComponent>(e).name == "Grass") return e;
        return Entity{ entt::null };
    }();
    REQUIRE((grass != entt::null));
    const auto* grassNet = reg.try_get<NetworkComponent>(grass);
    REQUIRE(grassNet != nullptr);
    CHECK(grassNet->replicates == false);
    CHECK(grassNet->netId == 0u);

    Peer& client = addClient(rig, defaultJoin(), 2);
    mirrorInto(*rig.host.world, *client.world);
    rig.step(20);

    REQUIRE(client.session->status() == NetGameSession::Status::Joined);
    CHECK(client.session->spawns()->stats().bindsReceived == 3u);
    CHECK(client.session->spawns()->stats().bindsUnresolved == 0u);
    CHECK(client.session->spawns()->boundCount() == 3u);

    // The two that were left out carry no session identity on either side.
    int registered = 0;
    for (const Entity e : reg.view<NetworkComponent>())
        if (reg.get<NetworkComponent>(e).netId != 0) ++registered;
    CHECK(registered == 3);
}

TEST_CASE("net session: a runtime spawn with the switch off is not replicated") {
    Rig rig;
    startHost(rig, defaultHost());
    Peer& client = addClient(rig, defaultJoin(), 2);
    rig.step(20);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);

    // The same switch governs both paths. A spawned effect with Replicates off
    // is a purely local thing — a muzzle flash, debris — and must not cost a
    // message, let alone appear on somebody else's screen.
    const Entity effect = rig.host.world->createEntity("Muzzle flash");
    rig.host.world->registry().emplace_or_replace<TransformComponent>(effect);
    NetworkComponent off;
    off.replicates = false;
    rig.host.world->registry().emplace_or_replace<NetworkComponent>(effect, off);

    const std::uint32_t before = rig.host.session->spawns()->stats().spawnsSent;
    CHECK(rig.host.session->spawns()->notifySpawned(effect, "classes/Flash.hcclass", 0) == 0u);
    CHECK(rig.host.session->spawns()->stats().spawnsSent == before);

    rig.step(10);
    CHECK(client.spawnCalls == 0);
}

// ─── 3. A runtime spawn reaches the client ───────────────────────────────────

TEST_CASE("net session: a host spawn is created on the client") {
    Rig rig;
    startHost(rig, defaultHost());
    Peer& client = addClient(rig, defaultJoin(), 2);
    rig.step(20);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);

    // Placed FIRST, then announced: the pose on the wire is read off the
    // entity, so the message and the host's own transform cannot disagree.
    const Entity projectile = rig.host.world->createEntity("Projectile");
    auto& ptc = rig.host.world->registry().emplace_or_replace<TransformComponent>(projectile);
    ptc.position = { 5, 1, 2 };
    ptc.rotation = { 0, 90, 0 };
    const std::uint32_t netId = rig.host.session->spawns()->notifySpawned(
        projectile, "classes/Projectile.hcclass", /*owner*/ 2);
    CHECK(netId != 0u);

    rig.step(10);

    CHECK(client.spawnCalls == 1);
    REQUIRE(client.spawnedClasses.size() == 1u);
    CHECK(client.spawnedClasses[0] == "classes/Projectile.hcclass");
    CHECK(client.session->spawns()->spawnedCount() == 1u);

    // The client adopted the HOST's id — minting its own would make the two
    // peers disagree about what a snapshot refers to.
    const Entity mirrored = client.session->replication()->entityOf(netId);
    REQUIRE((mirrored != entt::null));
    const auto* tc = client.world->registry().try_get<TransformComponent>(mirrored);
    REQUIRE(tc != nullptr);
    // Approx with a tolerance because the snapshot that follows the spawn
    // quantises: 24 bits over ±4096 for position, 16 over ±180 for rotation.
    CHECK(tc->position.x == doctest::Approx(5.0f).epsilon(0.01));
    CHECK(tc->rotation.y == doctest::Approx(90.0f).epsilon(0.01));
    // Owner travels, because it is what decides whose input the thing hears.
    const auto* nc = client.world->registry().try_get<NetworkComponent>(mirrored);
    REQUIRE(nc != nullptr);
    CHECK(nc->owner == 2u);
}

// ─── 3b. Possession: the host says which entity a player drives ──────────────

TEST_CASE("net session: assignControl reaches the owner and nobody else") {
    Rig rig;
    startHost(rig, defaultHost());
    Peer& anna = addClient(rig, defaultJoin("Anna"), 2);
    Peer& bert = addClient(rig, defaultJoin("Bert"), 3);
    rig.step(20);
    REQUIRE(anna.session->status() == NetGameSession::Status::Joined);
    REQUIRE(bert.session->status() == NetGameSession::Status::Joined);
    const G::PlayerId annaId = anna.session->localPlayer();
    REQUIRE(annaId != G::kNoPlayer);

    // The host spawns Anna's character and gives it to her, in that order —
    // there is nothing to assign until the entity is replicated.
    const Entity body = rig.host.world->createEntity("AnnaBody");
    rig.host.world->registry().emplace_or_replace<TransformComponent>(body).position = { 3, 0, 0 };
    const std::uint32_t netId = rig.host.session->spawns()->notifySpawned(
        body, "classes/PlayerCharacter.hcclass", annaId);
    REQUIRE(netId != 0u);
    CHECK(rig.host.session->assignControl(annaId, body));
    rig.step(10);

    // The owner learned about it; the other client did not. A kMsgControl that
    // went to everyone would have every client predicting the same character.
    CHECK(anna.controlCalls == 1);
    CHECK(anna.controlNetId == netId);
    CHECK((anna.controlled != entt::null));
    CHECK(anna.controlled == anna.session->replication()->entityOf(netId));
    CHECK(anna.session->localCharacter() == anna.controlled);
    CHECK(bert.controlCalls == 0);
    CHECK((bert.session->localCharacter() == entt::null));

    // …and the host recorded it, which is what net.playerName's neighbours read.
    const G::PlayerInfo* info = rig.host.session->roster().find(annaId);
    REQUIRE(info != nullptr);
    CHECK(info->characterNetId == netId);

    SUBCASE("an entity that is not replicated cannot be handed to anybody") {
        const Entity loose = rig.host.world->createEntity("Loose");
        rig.host.world->registry().emplace_or_replace<TransformComponent>(loose);
        CHECK_FALSE(rig.host.session->assignControl(annaId, loose));
    }
    SUBCASE("neither can a player who is not in the session") {
        CHECK_FALSE(rig.host.session->assignControl(99u, body));
    }
    SUBCASE("leaving clears it, so a second session inherits no character") {
        anna.session->leave();
        CHECK((anna.session->localCharacter() == entt::null));
    }
}

TEST_CASE("net session: a spawn is not sent to a peer that has not been welcomed") {
    // The hole step 4 left open, and it is not cosmetic. Before the Welcome a
    // client's own localPlayer is still 0, and 0 is also what a HOST-owned
    // entity carries as its owner — so a spawn arriving early reads as "mine"
    // on the client, and its PlayerHost adopts a crate.
    //
    // MUTATION: drop the setJoinedFilter call in NetGameSession::startCommon
    // (or make joinedConnections return every connection) and the first CHECK
    // below sees two spawns instead of one.
    Rig rig;
    startHost(rig, defaultHost());

    // A peer whose link is up and whose Hello has NOT been processed: the
    // connection is announced to the host, and nothing is stepped.
    const ConnectionId lurker = rig.fabric->next++;
    rig.fabric->up[lurker] = true;
    rig.fabric->hostIn.push_back(NetEvent{ NetEventType::Connected, lurker, {} });
    rig.host.session->update(0.016f);   // the host now knows the link, not the player
    REQUIRE(rig.host.session->roster().size() == 1u);   // the host itself, alone

    const Entity crate = rig.host.world->createEntity("Crate");
    rig.host.world->registry().emplace_or_replace<TransformComponent>(crate);
    REQUIRE(rig.host.session->spawns()->notifySpawned(
        crate, "classes/Crate.hcclass", G::kNoPlayer) != 0u);

    // One datagram for the spawn, and it went nowhere: the only connection is
    // the lurker's, and it is not in the roster.
    CHECK(rig.host.session->spawns()->stats().spawnsSent == 0u);

    // A properly joined client does get it, from the same call.
    Peer& client = addClient(rig, defaultJoin(), 2);
    rig.step(20);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);
    CHECK(client.spawnCalls == 1);   // out of the late-join baseline

    const Entity second = rig.host.world->createEntity("Barrel");
    rig.host.world->registry().emplace_or_replace<TransformComponent>(second);
    REQUIRE(rig.host.session->spawns()->notifySpawned(
        second, "classes/Barrel.hcclass", G::kNoPlayer) != 0u);
    rig.step(10);
    CHECK(client.spawnCalls == 2);
}

// ─── 4. Late join: binds, spawns and a baseline for what never moves ─────────

TEST_CASE("net session: a late joiner gets the whole world, including the static one") {
    Rig rig;
    authoredEntity(*rig.host.world, "Crate",  { 1, 0, 0 }, true);
    authoredEntity(*rig.host.world, "Door",   { 2, 0, 0 }, true);
    // The entity a snapshot would never carry: this is the one the baseline
    // exists for, and the only state it will ever be sent.
    authoredEntity(*rig.host.world, "Statue", { 7, 3, 9 }, true, /*replicateTransform*/ false);

    startHost(rig, defaultHost());

    Peer& first = addClient(rig, defaultJoin("First"), 2);
    mirrorInto(*rig.host.world, *first.world);
    rig.step(20);
    REQUIRE(first.session->status() == NetGameSession::Status::Joined);

    const Entity spawned = rig.host.world->createEntity("Turret");
    auto& stc = rig.host.world->registry().emplace_or_replace<TransformComponent>(spawned);
    stc.position = { 10, 0, 10 };
    rig.host.session->spawns()->notifySpawned(spawned, "classes/Turret.hcclass", 0);
    rig.step(10);

    // Now a third peer arrives into a session that is already running.
    Peer& late = addClient(rig, defaultJoin("Late"), 3);
    mirrorInto(*rig.host.world, *late.world);
    rig.step(25);

    REQUIRE(late.session->status() == NetGameSession::Status::Joined);
    CHECK(rig.host.session->roster().size() == 3u);
    CHECK(late.session->spawns()->stats().bindsReceived == 3u);
    CHECK(late.spawnCalls == 1);

    // The static entity's position arrived, which a snapshot alone could never
    // have delivered: sendSnapshots filters replicateTransform = false out.
    CHECK(late.session->replication()->stats().baselinesReceived >= 1u);
    const Entity statueHost = [&] {
        for (const Entity e : rig.host.world->registry().view<NameComponent>())
            if (rig.host.world->registry().get<NameComponent>(e).name == "Statue") return e;
        return Entity{ entt::null };
    }();
    REQUIRE((statueHost != entt::null));
    const std::uint32_t statueId =
        rig.host.world->registry().get<NetworkComponent>(statueHost).netId;
    const Entity statueLate = late.session->replication()->entityOf(statueId);
    REQUIRE((statueLate != entt::null));
    const auto* tc = late.world->registry().try_get<TransformComponent>(statueLate);
    REQUIRE(tc != nullptr);
    CHECK(tc->position.x == doctest::Approx(7.0f).epsilon(0.01));
    CHECK(tc->position.z == doctest::Approx(9.0f).epsilon(0.01));
}

// ─── 5. Leaving, and the state that must not outlive the peer ────────────────

TEST_CASE("net session: a leaving player is reaped, and a reused id inherits nothing") {
    Rig rig;
    startHost(rig, defaultHost());
    Peer& client = addClient(rig, defaultJoin("Anna"), 2);
    rig.step(20);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);
    REQUIRE(rig.host.session->roster().size() == 2u);

    // The host's own character for that player, so the despawn path runs.
    const Entity character = rig.host.world->createEntity("Anna's character");
    rig.host.world->registry().emplace_or_replace<TransformComponent>(character);
    const std::uint32_t charId = rig.host.session->spawns()->notifySpawned(
        character, "classes/PlayerCharacter.hcclass", 2);
    G::PlayerInfo* anna = const_cast<G::PlayerInfo*>(rig.host.session->roster().find(2));
    REQUIRE(anna != nullptr);
    anna->characterNetId = charId;
    rig.step(5);

    const ConnectionId reused = 1;   // the id Anna held

    // The goodbye, as leave() sends it. Sent through the session's own message
    // path rather than by calling leave() here, because leave() destroys the
    // transport in the same breath — over a real socket the datagram is already
    // gone, but LossyTransport holds it for its latency and it would die with
    // the queue. What is being tested is the HOST's handling of it.
    client.session->session()->broadcast(G::kMsgBye, SendMode::ReliableOrdered);
    rig.step(10);

    CHECK(rig.host.session->roster().size() == 1u);
    int reason = -1;
    CHECK(Rig::countEvents(rig.host, NetGameSession::Event::Kind::PlayerLeft, &reason) == 1);
    CHECK(reason == static_cast<int>(NetGameSession::DisconnectReason::Leave));
    CHECK(rig.host.session->stats().playersLeft == 1u);
    // Their character went with them.
    CHECK(rig.host.session->spawns()->spawnedCount() == 0u);

    // A NEW peer on the SAME connection id. Nothing of the old one may follow
    // it: not the input tracking, not the control assignment, not a score.
    client.session->leave();
    Peer& second = addClient(rig, defaultJoin("Bert"), 4, reused);
    rig.step(20);
    REQUIRE(second.session->status() == NetGameSession::Status::Joined);
    CHECK(rig.host.session->roster().size() == 2u);
    // A fresh player id, not Anna's: a connection id may be reused, a player
    // id never is.
    CHECK(second.session->localPlayer() == 3u);
    REQUIRE(rig.host.session->roster().find(2) == nullptr);
    const G::PlayerInfo* bert = rig.host.session->roster().find(3);
    REQUIRE(bert != nullptr);
    CHECK(bert->name == "Bert");
    CHECK(bert->characterNetId == 0u);
}

// ─── 6. The wrong project is refused, with a reason the client can act on ────

TEST_CASE("net session: a hello for a different project is rejected") {
    Rig rig;
    startHost(rig, defaultHost());

    NetGameSession::JoinOptions wrong = defaultJoin();
    wrong.projectId = "some-other-project";
    Peer& client = addClient(rig, wrong, 2);
    rig.step(20);

    CHECK(client.session->status() == NetGameSession::Status::Failed);
    CHECK(rig.host.session->roster().size() == 1u);      // the host alone
    CHECK(rig.host.session->stats().joinsRejected == 1u);
    CHECK(rig.host.session->stats().joinsAccepted == 0u);

    int reason = -1;
    CHECK(Rig::countEvents(client, NetGameSession::Event::Kind::Disconnected, &reason) == 1);
    CHECK(reason == static_cast<int>(NetGameSession::DisconnectReason::WrongProject));
    // The refusal says WHICH project, or the joiner has nothing to fix.
    CHECK(client.session->lastError().find("Project A") != std::string::npos);
}

TEST_CASE("net session: a hello with the wrong protocol version is rejected") {
    // A bare NetSession rather than a NetGameSession on the client end: the
    // session always sends the version it was built with, and the case worth
    // testing is a peer that does not — a build from before or after this one.
    Rig other;
    startHost(other, defaultHost());
    auto ep    = std::make_unique<HubEndpoint>(other.fabric, false, 1);
    other.fabric->up[1] = true;
    other.fabric->hostIn.push_back(NetEvent{ NetEventType::Connected, 1, {} });
    other.fabric->clientIn[1].push_back(NetEvent{ NetEventType::Connected, 1, {} });
    auto lossy = LossyTransport::wrap(std::move(ep), hostileNet(9));
    LossyTransport* raw = lossy.get();
    NetSession bare(raw, NetRole::Client);

    raw->advance(kStepMs);
    other.host.lossy->advance(kStepMs);
    raw->update();
    bare.pump();
    other.host.session->update(1.0f / 60.0f);

    BitWriter w;
    w.writeUInt16(G::kGameProtocolVersion + 1);
    w.writeString("Ancient");
    w.writeString("project-a");
    bare.send(1, G::kMsgHello, w, SendMode::ReliableOrdered);

    for (int i = 0; i < 6; ++i) {
        raw->advance(kStepMs);
        other.host.lossy->advance(kStepMs);
        raw->update();
        bare.pump();
        other.host.session->update(1.0f / 60.0f);
    }

    CHECK(other.host.session->stats().joinsRejected == 1u);
    CHECK(other.host.session->roster().size() == 1u);
}

// ─── 7. Anti-cheat, for the first time inside a session ──────────────────────

TEST_CASE("net session: a foreign input frame is a Hard observation and a kick") {
    Rig rig;
    HE::ProjectAntiCheatSettings ac;
    ac.enabled = true;
    // No manifest exists in a headless test, so the integrity comparison is off
    // either way; saying so keeps the test about the input path.
    ac.integrityCheck = false;

    NetGameSession::HostOptions options = defaultHost();
    options.antiCheat = &ac;
    startHost(rig, options);
    REQUIRE(rig.host.session->antiCheatService() != nullptr);

    Peer& client = addClient(rig, defaultJoin("Cheater"), 2);
    rig.step(20);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);
    (void)Rig::countEvents(client, NetGameSession::Event::Kind::Connected);

    // Hand-built: input from a connection that was never assigned an entity.
    // No legitimate client produces it, which is why it is Hard rather than
    // weighed.
    BitWriter w;
    w.writeUInt32(1);          // sequence
    w.writeFloat(0.016f);      // dt
    w.writeFloat(1.0f); w.writeFloat(0.0f); w.writeFloat(0.0f);   // move
    w.writeFloat(0.0f);        // yaw
    client.session->session()->send(1, G::kMsgInput, w, SendMode::ReliableOrdered);

    rig.step(8);

    const auto& acStats = rig.host.session->antiCheat()->stats();
    CHECK(acStats.noticesSent >= 1u);
    CHECK(acStats.kicked >= 1u);
    CHECK(rig.host.session->roster().size() == 1u);   // the host alone again

    // The client learned it was removed, and learned it as a KICK rather than
    // as a link that died: the notice arrived before the disconnect.
    CHECK(client.session->replication()->stats().noticesReceived >= 1u);
    CHECK(rig.host.session->antiCheat()->stats().noticesSent >= 1u);
    int reason = -1;
    CHECK(Rig::countEvents(client, NetGameSession::Event::Kind::Disconnected, &reason) == 1);
    CHECK(reason == static_cast<int>(NetGameSession::DisconnectReason::Kicked));
    // The notice became a local ticket on the client — the same readers a host
    // report answers, which is what makes one graph work on both sides.
    CHECK(client.session->antiCheat()->stats().noticesTaken >= 1u);
}

// ─── 8. The negative control ─────────────────────────────────────────────────

TEST_CASE("net session: with anti-cheat off the same frame changes nothing") {
    Rig rig;
    startHost(rig, defaultHost());   // no ProjectAntiCheatSettings = OFF
    REQUIRE(rig.host.session->antiCheatService() == nullptr);
    CHECK(rig.host.session->antiCheat()->isEnabled() == false);

    Peer& client = addClient(rig, defaultJoin("Cheater"), 2);
    rig.step(20);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);

    BitWriter w;
    w.writeUInt32(1);
    w.writeFloat(0.016f);
    w.writeFloat(1.0f); w.writeFloat(0.0f); w.writeFloat(0.0f);
    w.writeFloat(0.0f);
    client.session->session()->send(1, G::kMsgInput, w, SendMode::ReliableOrdered);

    rig.step(8);

    // Nobody dropped, nothing observed, no notice — the behaviour that existed
    // before the service did. Without this control, case 7 would only prove
    // that a kick can happen, not that the frame caused it.
    CHECK(rig.host.session->antiCheat()->stats().kicked == 0u);
    CHECK(rig.host.session->antiCheat()->stats().noticesSent == 0u);
    CHECK(rig.host.session->roster().size() == 2u);
    CHECK(client.session->status() == NetGameSession::Status::Joined);
    CHECK(client.session->replication()->stats().noticesReceived == 0u);
}

// ─── The switch itself survives a save/load round trip ───────────────────────

TEST_CASE("net session: Replicates defaults to on and turning it off keeps the tuning") {
    NetworkComponent nc;
    // Default true on purpose: a component somebody added by hand, and every
    // scene saved before the field existed, means "replicate this".
    CHECK(nc.replicates == true);

    nc.replicates      = false;
    nc.relevanceRadius = 42.0f;
    nc.maxSpeed        = 9.5f;
    // Turning it off KEEPS the component, so a second click restores the
    // numbers rather than resetting them.
    CHECK(nc.relevanceRadius == doctest::Approx(42.0f));
    CHECK(nc.maxSpeed == doctest::Approx(9.5f));
}

// ─── The project's Multiplayer page reaches the session ──────────────────────
// The page is only worth having if something reads it. This is that something:
// setProjectDefaults + the two factories every entry point (the editor's Play
// as Host, a packaged game's --host, the net.host row) now starts from.

TEST_CASE("net session: the project's multiplayer settings are what host and join start from") {
    NetGameSession s;
    // Untouched: exactly the options both structs carried before the page
    // existed, so a project that never opened it is unaffected.
    {
        const NetGameSession::HostOptions d = s.defaultHostOptions();
        CHECK(d.port == 47824);
        CHECK(d.maxPlayers == 8u);
        CHECK(d.announceLan == true);
        CHECK(d.replication.tickHz == doctest::Approx(30.0f));
        CHECK(d.replication.worldExtent == doctest::Approx(4096.0f));
    }

    HE::ProjectMultiplayerSettings mp;
    mp.defaultPort           = 40123;
    mp.maxPlayers            = 4;
    mp.timeoutSec            = 12.0f;
    mp.tickHz                = 60.0f;
    mp.worldExtent           = 1024.0f;
    mp.interpolationDelaySec = 0.05f;
    mp.reconcileSnapDistance = 3.0f;
    mp.reconcileSmoothing    = 8.0f;
    mp.maxPendingInputs      = 32;
    mp.discoverLan           = false;
    s.setProjectDefaults(mp);

    const NetGameSession::HostOptions h = s.defaultHostOptions();
    CHECK(h.port == 40123);
    CHECK(h.maxPlayers == 4u);
    CHECK(h.announceLan == false);
    CHECK(h.timeoutSec == doctest::Approx(12.0f));
    CHECK(h.replication.tickHz == doctest::Approx(60.0f));
    CHECK(h.replication.worldExtent == doctest::Approx(1024.0f));
    CHECK(h.replication.interpolationDelaySec == doctest::Approx(0.05f));
    CHECK(h.replication.reconcileSnapDistance == doctest::Approx(3.0f));
    CHECK(h.replication.reconcileSmoothing == doctest::Approx(8.0f));
    CHECK(h.replication.maxPendingInputs == std::size_t(32));
    // The wire format is NOT the project's to nudge: two builds of the same
    // game that packed positions differently would fail in a way nothing
    // reports, so the bit counts and the datagram budget stay at their
    // constants whatever the page says.
    CHECK(h.replication.positionBits == GameReplication::Config{}.positionBits);
    CHECK(h.replication.rotationBits == GameReplication::Config{}.rotationBits);
    CHECK(h.replication.snapshotBudgetBytes == GameReplication::Config{}.snapshotBudgetBytes);

    // A client needs the same tick and the same prediction bounds, and nothing
    // else from the page — it does not open a port or hold seats.
    const NetGameSession::JoinOptions j = s.defaultJoinOptions();
    CHECK(j.timeoutSec == doctest::Approx(12.0f));
    CHECK(j.replication.tickHz == doctest::Approx(60.0f));
    CHECK(j.replication.maxPendingInputs == std::size_t(32));

    // No socket under an idle session: 0 rather than a guess (plan §8.5).
    CHECK(s.linkStats().pingMs == doctest::Approx(0.0f));
    CHECK(s.linkStats().lossPercent == doctest::Approx(0.0f));
}

// ─── The socket path, once, for real ─────────────────────────────────────────
// Everything above runs on an injected transport, which is what makes it fast
// and deterministic — and what leaves host()/joinDirect() themselves, the two
// functions that assemble UdpTransport → SecureTransport → NetSession, proven
// only to compile. This runs that chain once over localhost.
//
// Real time, therefore a deadline rather than a fixed frame count, and
// RUN_SERIAL in CMake: under a full -j load a fixed count is a race against the
// scheduler, which is the one thing a test may never be.
TEST_CASE("net session: host() and joinDirect() complete over real UDP sockets") {
    NetGameSession::HostOptions hostOpts = defaultHost();
    hostOpts.port        = 0;        // let the OS pick
    hostOpts.announceLan = false;    // no multicast, no Local Network permission

    HorizonWorld    hostWorld, clientWorld;
    NetGameSession  host, client;
    host.setWorld(&hostWorld);
    client.setWorld(&clientWorld);

    if (!host.host(hostOpts)) {
        // No UDP on this machine at all (a locked-down CI container). Saying so
        // beats a red test that blames the session for the sandbox.
        MESSAGE("no local UDP socket available: " << host.lastError());
        return;
    }
    REQUIRE(host.status() == NetGameSession::Status::Hosting);
    REQUIRE_FALSE(host.joinCode().empty());

    const std::uint16_t port = host.boundPort();
    REQUIRE(port != 0);

    NetGameSession::JoinOptions joinOpts = defaultJoin("Anna");
    joinOpts.joinCode = host.joinCode();
    REQUIRE(client.joinDirect("127.0.0.1", port, joinOpts));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (client.status() != NetGameSession::Status::Joined &&
           std::chrono::steady_clock::now() < deadline) {
        host.update(1.0f / 60.0f);
        client.update(1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    CHECK(client.status() == NetGameSession::Status::Joined);
    CHECK(client.localPlayer() == 2u);
    CHECK(host.roster().size() == 2u);
    CHECK(host.stats().joinsAccepted == 1u);
}
