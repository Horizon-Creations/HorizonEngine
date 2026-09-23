#include "doctest.h"

#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/NetworkComponent.h>
#include <HorizonScene/Components/ReplicatedVarsComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/GameReplication.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Net/NetGameSession.h>
#include <HorizonScene/Net/NetMessages.h>
#include <HorizonScene/Net/PropertyReplicator.h>
#include <HorizonScene/Net/ValueWire.h>

#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>

#include <Net/BitStream.h>
#include <Net/ITransport.h>
#include <Net/LossyTransport.h>

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace HE::Net;
using namespace HE::Net::Game;
using HorizonCode::PinType;
using HorizonCode::Value;
namespace G = HE::Net::Game;

// ─── Replicated variables (plan §6) ──────────────────────────────────────────
// Two NetGameSessions against each other over LossyTransport, headless, on a
// simulated clock — the harness test_net_game_session established, because the
// question here is the same one in a different layer: does a value the host
// wrote reach a client that is losing, reordering and duplicating datagrams.
//
// The oracle for "it arrived" is deliberately the CLIENT'S OWN STORAGE (getVar /
// the ReplicatedVarsComponent), not a counter: a counter would go up for a
// message that was parsed and then dropped, which is precisely the failure the
// unknown-index and type-mismatch paths are about.

namespace {

// ── The multi-client loopback fabric (test_net_game_session's) ───────────────
struct Fabric {
    std::deque<NetEvent>                                  hostIn;
    std::unordered_map<ConnectionId, std::deque<NetEvent>> clientIn;
    std::unordered_map<ConnectionId, bool>                 up;
    ConnectionId                                           next = 1;
};

class HubEndpoint final : public ITransport {
public:
    using ITransport::send;
    HubEndpoint(std::shared_ptr<Fabric> f, bool host, ConnectionId id)
        : m_f(std::move(f)), m_host(host), m_id(id) {}

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

constexpr std::uint32_t kStepMs = 100;   // > latency + jitter, see test_net_game_session

struct Peer {
    std::unique_ptr<HorizonWorld>   world   = std::make_unique<HorizonWorld>();
    std::unique_ptr<NetGameSession> session = std::make_unique<NetGameSession>();
    LossyTransport*                 lossy   = nullptr;   // owned by the session

    // A HorizonCode interpreter of its own, and the entity→instance map the
    // application would normally own (EntityHost). Both ends run one, because a
    // client evaluates the same classes the host does.
    HorizonCode::Runtime                                 runtime;
    std::unordered_map<std::uint32_t, HorizonCode::InstanceId> instances;

    void bindVariableSource() {
        session->setVariableSource(&runtime, [this](Entity e) -> HorizonCode::InstanceId {
            const auto it = instances.find(static_cast<std::uint32_t>(e));
            return it == instances.end() ? 0 : it->second;
        });
    }
};

struct Rig {
    std::shared_ptr<Fabric>            fabric = std::make_shared<Fabric>();
    Peer                               host;
    std::vector<std::unique_ptr<Peer>> clients;

    void step(int frames = 1, float dt = 1.0f / 60.0f) {
        for (int i = 0; i < frames; ++i) {
            host.lossy->advance(kStepMs);
            for (auto& c : clients) c->lossy->advance(kStepMs);
            host.session->update(dt);
            for (auto& c : clients) c->session->update(dt);
        }
    }
};

NetGameSession::HostOptions defaultHost() {
    NetGameSession::HostOptions o;
    o.displayName  = "Host";
    o.announceLan  = false;
    o.projectId    = "project-a";
    o.projectLabel = "Project A";
    o.scenePath    = "scenes/arena.hescene";
    return o;
}

NetGameSession::JoinOptions defaultJoin(const char* name = "Guest") {
    NetGameSession::JoinOptions o;
    o.displayName = name;
    o.projectId   = "project-a";
    return o;
}

void startHost(Rig& rig, NetGameSession::HostOptions options = defaultHost()) {
    auto ep    = std::make_unique<HubEndpoint>(rig.fabric, /*host*/ true, 0);
    auto lossy = LossyTransport::wrap(std::move(ep), hostileNet(1));
    rig.host.lossy = lossy.get();
    rig.host.session->setWorld(rig.host.world.get());
    rig.host.bindVariableSource();
    REQUIRE(rig.host.session->hostOn(std::move(lossy), options));
}

Peer& addClient(Rig& rig, std::uint32_t seed, NetGameSession::JoinOptions options = defaultJoin()) {
    const ConnectionId id = rig.fabric->next++;
    rig.fabric->up[id] = true;
    rig.fabric->hostIn.push_back(NetEvent{ NetEventType::Connected, id, {} });
    rig.fabric->clientIn[id].push_back(NetEvent{ NetEventType::Connected, 1, {} });

    auto peer  = std::make_unique<Peer>();
    auto ep    = std::make_unique<HubEndpoint>(rig.fabric, /*host*/ false, id);
    auto lossy = LossyTransport::wrap(std::move(ep), hostileNet(seed));
    peer->lossy = lossy.get();
    peer->session->setWorld(peer->world.get());
    peer->bindVariableSource();
    REQUIRE(peer->session->joinOn(std::move(lossy), options));

    rig.clients.push_back(std::move(peer));
    return *rig.clients.back();
}

Entity authoredEntity(HorizonWorld& world, const char* name, bool replicates = true) {
    const Entity e = world.createEntity(name);
    world.registry().emplace_or_replace<TransformComponent>(e);
    NetworkComponent nc;
    nc.replicates = replicates;
    world.registry().emplace_or_replace<NetworkComponent>(e, nc);
    return e;
}

// The same entity in the client's world, under the SAME scene uuid — which is
// what kMsgBind resolves against.
Entity mirrorOf(HorizonWorld& src, Entity e, HorizonWorld& dst) {
    const auto* name = src.registry().try_get<NameComponent>(e);
    const Entity c = dst.createEntity(name ? name->name : std::string("Entity"));
    dst.registry().emplace_or_replace<TransformComponent>(c);
    dst.setEntityId(c, src.entityId(e));
    return c;
}

// Drain everything the client queued, counting per variable.
struct Reps {
    int                                    total = 0;
    std::unordered_map<std::string, int>   perName;
    std::unordered_map<std::string, Value> lastOld;

    // Read-only lookups, so a CHECK for a variable nobody notified reports 0
    // rather than inserting an entry (and failing to compile on a const Reps).
    int count(const std::string& name) const {
        const auto it = perName.find(name);
        return it == perName.end() ? 0 : it->second;
    }
    Value old(const std::string& name) const {
        const auto it = lastOld.find(name);
        return it == lastOld.end() ? Value{} : it->second;
    }
};
Reps drainNotifications(Peer& p) {
    Reps out;
    PropertyReplicator::Notification n;
    while (p.session->properties()->takeNotification(n)) {
        ++out.total;
        ++out.perName[n.name];
        out.lastOld[n.name] = n.oldValue;
    }
    return out;
}

} // namespace

// ─── 1. The wire format on its own ───────────────────────────────────────────
// Before any session: a Value that does not survive a round trip would make
// every test below fail for a reason that has nothing to do with the network.

TEST_CASE("value wire: every supported scalar survives a round trip") {
    std::vector<Value> originals = {
        Value::ofBool(true),
        Value::ofInt(-4711),
        Value::ofFloat(1.5f),
        Value::ofString("a string with spaces"),
        Value::ofVec2({ 1.0f, -2.0f }),
        Value::ofVec3({ 1.0f, -2.0f, 3.5f }),
        Value::ofVec4({ 1.0f, -2.0f, 3.5f, 4.0f }),
        Value::ofColor({ 0.25f, 0.5f, 0.75f, 1.0f }),
        Value::ofTransform({ 1, 2, 3 }, { 10, 20, 30 }, { 2, 2, 2 }),
    };
    for (const Value& v : originals) {
        BitWriter w;
        REQUIRE(writeValue(w, v));
        const auto bytes = w.data();
        BitReader r(bytes);
        Value back;
        REQUIRE(readValue(r, back));
        CHECK(valuesEqual(v, back));
    }
}

TEST_CASE("value wire: enum, struct, array, set and map survive a round trip") {
    Value en; en.type = PinType::Enum; en.typeName = "types/Team.htype"; en.i = 2;

    Value st; st.type = PinType::Struct; st.typeName = "types/Loadout.htype";
    st.items = { Value::ofInt(3), Value::ofString("rifle"), Value::ofBool(true) };

    Value arr = Value::ofArray(PinType::Int);
    arr.items = { Value::ofInt(1), Value::ofInt(2), Value::ofInt(3) };

    Value set = Value::ofSet(PinType::String);
    set.items = { Value::ofString("a"), Value::ofString("b") };

    Value map = Value::ofMap(PinType::String, PinType::Int);
    map.keys  = { Value::ofString("red"), Value::ofString("blue") };
    map.items = { Value::ofInt(7), Value::ofInt(9) };

    Value structArr = Value::ofArray(PinType::Struct, "types/Loadout.htype");
    structArr.items = { st, st };

    for (const Value& v : { en, st, arr, set, map, structArr }) {
        BitWriter w;
        REQUIRE(writeValue(w, v));
        const auto bytes = w.data();
        BitReader r(bytes);
        Value back;
        REQUIRE(readValue(r, back));
        CHECK(valuesEqual(v, back));
    }

    // A map's ORDER is part of its value (the containers plan's §1.2), so the
    // same pairs in the other order are a different value and must replicate.
    Value swapped = map;
    std::swap(swapped.keys[0], swapped.keys[1]);
    std::swap(swapped.items[0], swapped.items[1]);
    CHECK_FALSE(valuesEqual(map, swapped));
}

TEST_CASE("value wire: a Ref is refused rather than written as a number") {
    CHECK_FALSE(isReplicableType(PinType::Ref));
    CHECK(std::string(replicationRefusalReason(PinType::Ref)).size() > 0);

    BitWriter w;
    CHECK_FALSE(writeValue(w, Value::ofRef(42)));

    Value refArr = Value::ofArray(PinType::Ref);
    refArr.items = { Value::ofRef(1) };
    BitWriter w2;
    CHECK_FALSE(writeValue(w2, refArr));

    // Negative control: the same shapes with a replicable type DO go through,
    // so the two refusals above are about Ref and not about the shape.
    BitWriter w3;
    CHECK(writeValue(w3, Value::ofInt(42)));
    Value intArr = Value::ofArray(PinType::Int);
    intArr.items = { Value::ofInt(1) };
    BitWriter w4;
    CHECK(writeValue(w4, intArr));
}

TEST_CASE("value wire: a truncated or nonsensical stream is refused, not crashed") {
    BitWriter w;
    REQUIRE(writeValue(w, Value::ofString("something long enough to truncate")));
    auto bytes = w.data();

    for (std::size_t cut = 0; cut + 1 < bytes.size(); ++cut) {
        std::vector<std::uint8_t> partial(bytes.begin(), bytes.begin() + static_cast<long>(cut));
        BitReader r(partial);
        Value out;
        CHECK_FALSE(readValue(r, out));
    }

    // A type byte no PinType has.
    const std::vector<std::uint8_t> nonsense = { 200, 0, 0, 0, 0, 0 };
    BitReader r(nonsense);
    Value out;
    CHECK_FALSE(readValue(r, out));
}

TEST_CASE("value wire: values of different shape are never equal") {
    Value i = Value::ofInt(1);
    Value f = Value::ofFloat(1.0f);
    CHECK_FALSE(valuesEqual(i, f));
    CHECK_FALSE(valueTypesMatch(i, f));

    Value arr = Value::ofArray(PinType::Int);
    arr.items = { Value::ofInt(1) };
    CHECK_FALSE(valuesEqual(i, arr));
    CHECK_FALSE(valueTypesMatch(i, arr));

    Value a; a.type = PinType::Struct; a.typeName = "types/A.htype"; a.items = { Value::ofInt(1) };
    Value b = a; b.typeName = "types/B.htype";
    CHECK_FALSE(valuesEqual(a, b));
    CHECK_FALSE(valueTypesMatch(a, b));
}

// ─── 2. A declared value reaches a client over a bad network ─────────────────

TEST_CASE("properties: a declared variable arrives, and OnRep fires exactly once") {
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    startHost(rig);

    Peer& client = addClient(rig, 2);
    const Entity clientDoor = mirrorOf(*rig.host.world, door, *client.world);

    // Declared BEFORE the join, so the table carries it.
    REQUIRE(rig.host.session->properties()->declareVar(
        door, "open", Value::ofBool(false), /*notify*/ true));

    rig.step(12);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);

    // The baseline: the value the host held when the client arrived, with one
    // OnRep for it (plan §6.4 — "a door that was already open").
    {
        Value v;
        REQUIRE(client.session->properties()->getVar(clientDoor, "open", v));
        CHECK(v.b == false);
    }
    drainNotifications(client);   // the baseline's own OnRep, counted in case 5

    // Now the host opens it.
    REQUIRE(rig.host.session->properties()->setVar(door, "open", Value::ofBool(true)));
    rig.step(12);

    Value v;
    REQUIRE(client.session->properties()->getVar(clientDoor, "open", v));
    CHECK(v.b == true);

    const Reps reps = drainNotifications(client);
    CHECK(reps.count("open") == 1);          // once, not once per frame
    CHECK(reps.old("open").b == false);    // and with the value it replaced

    // The host does NOT notify itself (plan §6.4): it set the value and knows.
    CHECK(rig.host.session->properties()->pendingNotificationCount() == 0);

    // Nothing more goes out while nothing changes — this is the dirty tracking,
    // and without it a value would be resent every frame forever.
    const std::uint32_t sent = rig.host.session->properties()->stats().deltasSent;
    rig.step(10);
    CHECK(rig.host.session->properties()->stats().deltasSent == sent);
}

TEST_CASE("properties: a variable without Notify replicates but calls nothing") {
    Rig rig;
    const Entity crate = authoredEntity(*rig.host.world, "Crate");
    startHost(rig);
    Peer& client = addClient(rig, 3);
    const Entity clientCrate = mirrorOf(*rig.host.world, crate, *client.world);

    REQUIRE(rig.host.session->properties()->declareVar(
        crate, "hp", Value::ofInt(100), /*notify*/ false));
    rig.step(12);
    drainNotifications(client);

    REQUIRE(rig.host.session->properties()->setVar(crate, "hp", Value::ofInt(40)));
    rig.step(12);

    Value v;
    REQUIRE(client.session->properties()->getVar(clientCrate, "hp", v));
    CHECK(v.i == 40);                          // the value travelled
    CHECK(drainNotifications(client).total == 0);   // the callback did not
}

// ─── 3. Authority ────────────────────────────────────────────────────────────

TEST_CASE("properties: a client's own write takes effect and is then overwritten") {
    Rig rig;
    const Entity gun = authoredEntity(*rig.host.world, "Gun");
    startHost(rig);
    Peer& client = addClient(rig, 4);
    const Entity clientGun = mirrorOf(*rig.host.world, gun, *client.world);

    REQUIRE(rig.host.session->properties()->declareVar(
        gun, "ammo", Value::ofInt(30), /*notify*/ true));
    rig.step(12);
    drainNotifications(client);

    // Prediction: the client fires and decrements locally (plan §6.3).
    REQUIRE(client.session->properties()->setVar(clientGun, "ammo", Value::ofInt(29)));
    {
        Value v;
        REQUIRE(client.session->properties()->getVar(clientGun, "ammo", v));
        CHECK(v.i == 29);                  // it WORKED — E4 is not a refusal
    }
    CHECK(client.session->properties()->stats().clientWrites == 1u);

    // The host disagrees: it never saw a shot.
    REQUIRE(rig.host.session->properties()->setVar(gun, "ammo", Value::ofInt(28)));
    rig.step(12);

    Value v;
    REQUIRE(client.session->properties()->getVar(clientGun, "ammo", v));
    CHECK(v.i == 28);                       // the authority wins
    const Reps reps = drainNotifications(client);
    CHECK(reps.count("ammo") == 1);
    // The old value is what THIS machine held, which is its own prediction —
    // "the value before this change" means the value that was here.
    CHECK(reps.old("ammo").i == 29);
}

TEST_CASE("properties: a delta that matches the prediction fires no OnRep") {
    Rig rig;
    const Entity gun = authoredEntity(*rig.host.world, "Gun");
    startHost(rig);
    Peer& client = addClient(rig, 5);
    const Entity clientGun = mirrorOf(*rig.host.world, gun, *client.world);

    REQUIRE(rig.host.session->properties()->declareVar(
        gun, "ammo", Value::ofInt(30), /*notify*/ true));
    rig.step(12);
    drainNotifications(client);

    // Both sides reach 29 independently; the arriving delta agrees with what is
    // already here, so it is not a change and must not flicker a handler.
    REQUIRE(client.session->properties()->setVar(clientGun, "ammo", Value::ofInt(29)));
    REQUIRE(rig.host.session->properties()->setVar(gun, "ammo", Value::ofInt(29)));
    rig.step(12);

    CHECK(drainNotifications(client).total == 0);
    CHECK(client.session->properties()->stats().predictedMatches >= 1u);
}

// ─── 4. Malformed and mismatched input ───────────────────────────────────────

TEST_CASE("properties: a delta with an unknown index or a wrong type is dropped, not applied") {
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    startHost(rig);
    Peer& client = addClient(rig, 6);
    const Entity clientDoor = mirrorOf(*rig.host.world, door, *client.world);

    REQUIRE(rig.host.session->properties()->declareVar(
        door, "open", Value::ofBool(false), /*notify*/ true));
    rig.step(12);
    drainNotifications(client);

    const std::uint32_t netId =
        rig.host.world->registry().get<NetworkComponent>(door).netId;
    REQUIRE(netId != 0u);

    // Forged by hand, because no honest host sends either of these: a host that
    // did would be a host running a different build, which is the case the
    // client has to survive rather than trust.
    auto sendRaw = [&](const BitWriter& w) {
        for (const ConnectionId conn : rig.host.session->session()->connections())
            rig.host.session->session()->send(conn, kMsgProperties, w,
                                              SendMode::ReliableOrdered);
        rig.step(12);
    };

    // (a) an index the table does not have.
    {
        BitWriter w;
        w.writeUInt32(netId);
        w.writeByte(1);
        w.writeByte(99);
        REQUIRE(writeValue(w, Value::ofBool(true)));
        sendRaw(w);
    }
    CHECK(client.session->properties()->stats().unknownIndex >= 1u);

    // (b) index 0 is real, but the value is a String where a Bool was declared.
    {
        BitWriter w;
        w.writeUInt32(netId);
        w.writeByte(1);
        w.writeByte(0);
        REQUIRE(writeValue(w, Value::ofString("wide open")));
        sendRaw(w);
    }
    CHECK(client.session->properties()->stats().typeMismatch >= 1u);

    // Neither reached the variable, and neither called anything.
    Value v;
    REQUIRE(client.session->properties()->getVar(clientDoor, "open", v));
    CHECK(v.type == PinType::Bool);
    CHECK(v.b == false);
    CHECK(drainNotifications(client).total == 0);

    // Negative control: the same path with an honest value still works, so the
    // two checks above are rejecting the fault and not the whole message.
    REQUIRE(rig.host.session->properties()->setVar(door, "open", Value::ofBool(true)));
    rig.step(12);
    REQUIRE(client.session->properties()->getVar(clientDoor, "open", v));
    CHECK(v.b == true);
}

TEST_CASE("properties: a truncated delta is dropped without touching the variable") {
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    startHost(rig);
    Peer& client = addClient(rig, 7);
    const Entity clientDoor = mirrorOf(*rig.host.world, door, *client.world);

    REQUIRE(rig.host.session->properties()->declareVar(
        door, "open", Value::ofBool(false), /*notify*/ true));
    rig.step(12);
    drainNotifications(client);

    const std::uint32_t netId = rig.host.world->registry().get<NetworkComponent>(door).netId;

    BitWriter w;
    w.writeUInt32(netId);
    w.writeByte(4);          // claims four properties and carries none
    for (const ConnectionId conn : rig.host.session->session()->connections())
        rig.host.session->session()->send(conn, kMsgProperties, w, SendMode::ReliableOrdered);
    rig.step(12);

    CHECK(client.session->properties()->stats().malformed >= 1u);
    Value v;
    REQUIRE(client.session->properties()->getVar(clientDoor, "open", v));
    CHECK(v.b == false);
}

// ─── 5. Late join ────────────────────────────────────────────────────────────

TEST_CASE("properties: a late joiner gets the current values and one OnRep each") {
    Rig rig;
    const Entity door  = authoredEntity(*rig.host.world, "Door");
    const Entity score = authoredEntity(*rig.host.world, "Scoreboard");
    startHost(rig);

    REQUIRE(rig.host.session->properties()->declareVar(door, "open", Value::ofBool(false), true));
    REQUIRE(rig.host.session->properties()->declareVar(score, "points", Value::ofInt(0), true));

    // The session runs for a while and things happen in it.
    Peer& early = addClient(rig, 8);
    mirrorOf(*rig.host.world, door, *early.world);
    mirrorOf(*rig.host.world, score, *early.world);
    rig.step(12);
    REQUIRE(rig.host.session->properties()->setVar(door, "open", Value::ofBool(true)));
    REQUIRE(rig.host.session->properties()->setVar(score, "points", Value::ofInt(17)));
    rig.step(12);

    // And only THEN somebody else joins.
    Peer& late = addClient(rig, 9, defaultJoin("Late"));
    const Entity lateDoor  = mirrorOf(*rig.host.world, door, *late.world);
    const Entity lateScore = mirrorOf(*rig.host.world, score, *late.world);
    rig.step(16);
    REQUIRE(late.session->status() == NetGameSession::Status::Joined);

    Value v;
    REQUIRE(late.session->properties()->getVar(lateDoor, "open", v));
    CHECK(v.b == true);                       // not the default it was declared with
    REQUIRE(late.session->properties()->getVar(lateScore, "points", v));
    CHECK(v.i == 17);

    const Reps reps = drainNotifications(late);
    CHECK(reps.count("open") == 1);
    CHECK(reps.count("points") == 1);
}

// ─── 6. The composite types, end to end ──────────────────────────────────────

TEST_CASE("properties: struct, enum and map values replicate through a session") {
    Rig rig;
    const Entity player = authoredEntity(*rig.host.world, "Player");
    startHost(rig);
    Peer& client = addClient(rig, 10);
    const Entity clientPlayer = mirrorOf(*rig.host.world, player, *client.world);

    Value team; team.type = PinType::Enum; team.typeName = "types/Team.htype"; team.i = 0;
    Value loadout; loadout.type = PinType::Struct; loadout.typeName = "types/Loadout.htype";
    loadout.items = { Value::ofInt(0), Value::ofString(""), Value::ofBool(false) };
    Value votes = Value::ofMap(PinType::String, PinType::Int);

    REQUIRE(rig.host.session->properties()->declareVar(player, "team", team, true));
    REQUIRE(rig.host.session->properties()->declareVar(player, "loadout", loadout, true));
    REQUIRE(rig.host.session->properties()->declareVar(player, "votes", votes, true));
    rig.step(12);
    drainNotifications(client);

    team.i = 2;
    loadout.items = { Value::ofInt(3), Value::ofString("rifle"), Value::ofBool(true) };
    votes.keys  = { Value::ofString("map_a"), Value::ofString("map_b") };
    votes.items = { Value::ofInt(4), Value::ofInt(1) };

    REQUIRE(rig.host.session->properties()->setVar(player, "team", team));
    REQUIRE(rig.host.session->properties()->setVar(player, "loadout", loadout));
    REQUIRE(rig.host.session->properties()->setVar(player, "votes", votes));
    rig.step(16);

    Value v;
    REQUIRE(client.session->properties()->getVar(clientPlayer, "team", v));
    CHECK(valuesEqual(v, team));
    REQUIRE(client.session->properties()->getVar(clientPlayer, "loadout", v));
    CHECK(valuesEqual(v, loadout));
    REQUIRE(client.session->properties()->getVar(clientPlayer, "votes", v));
    CHECK(valuesEqual(v, votes));

    const Reps reps = drainNotifications(client);
    CHECK(reps.count("team") == 1);
    CHECK(reps.count("loadout") == 1);
    CHECK(reps.count("votes") == 1);
}

// ─── 7. The HorizonCode source ───────────────────────────────────────────────
// The other half of §6.1: a class variable with the checkbox ticked, with no
// declareVar anywhere. The Runtime is the one that answers "which variables
// replicate" for both backends, so this is what proves the checkbox is the
// whole declaration.

namespace {
HorizonCode::Graph doorClass() {
    HorizonCode::Graph g;
    HorizonCode::Variable open;
    open.name       = "Open";
    open.type       = PinType::Bool;
    open.replicated = true;
    open.repNotify  = true;
    HorizonCode::Variable secret;         // deliberately NOT replicated
    secret.name = "Secret";
    secret.type = PinType::Int;
    HorizonCode::Variable local;          // replicated but function-local: never
    local.name       = "Temp";
    local.type       = PinType::Int;
    local.replicated = true;
    local.scope      = 7;
    g.variables = { open, secret, local };
    return g;
}
} // namespace

TEST_CASE("properties: a HorizonCode variable with the checkbox ticked replicates by itself") {
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    const HorizonCode::InstanceId hostInst = rig.host.runtime.add(doorClass());
    rig.host.instances[static_cast<std::uint32_t>(door)] = hostInst;
    startHost(rig);

    Peer& client = addClient(rig, 11);
    const Entity clientDoor = mirrorOf(*rig.host.world, door, *client.world);
    const HorizonCode::InstanceId clientInst = client.runtime.add(doorClass());
    client.instances[static_cast<std::uint32_t>(clientDoor)] = clientInst;

    // Only "Open" is on the wire: the private one has no checkbox, and the
    // function-local one has nobody to replicate to.
    const auto vars = rig.host.runtime.replicatedVariablesOf(hostInst);
    REQUIRE(vars.size() == 1u);
    CHECK(vars[0].name == "Open");
    CHECK(vars[0].notify);

    rig.step(12);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);
    drainNotifications(client);

    // Written the way a graph writes it — Set Variable, no net call at all.
    rig.host.runtime.setVariable(hostInst, "Open", Value::ofBool(true));
    rig.step(12);

    CHECK(client.runtime.getVariable(clientInst, "Open").b == true);
    const Reps reps = drainNotifications(client);
    CHECK(reps.count("Open") == 1);
    CHECK(reps.old("Open").b == false);

    // The unreplicated one stayed put on both sides.
    rig.host.runtime.setVariable(hostInst, "Secret", Value::ofInt(99));
    rig.step(12);
    CHECK(client.runtime.getVariable(clientInst, "Secret").i == 0);
}

TEST_CASE("properties: declareVar does not shadow a HorizonCode variable of the same name") {
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    const HorizonCode::InstanceId inst = rig.host.runtime.add(doorClass());
    rig.host.instances[static_cast<std::uint32_t>(door)] = inst;
    startHost(rig);

    CHECK_FALSE(rig.host.session->properties()->declareVar(
        door, "Open", Value::ofInt(0), /*notify*/ false));
    // And nothing was written into the component behind the class's back.
    CHECK(rig.host.world->registry().try_get<ReplicatedVarsComponent>(door) == nullptr);

    // A different name still declares normally.
    CHECK(rig.host.session->properties()->declareVar(
        door, "Paint", Value::ofString("red"), false));
}

TEST_CASE("properties: a Ref variable is refused rather than replicated") {
    Rig rig;
    const Entity e = authoredEntity(*rig.host.world, "Thing");
    startHost(rig);
    CHECK_FALSE(rig.host.session->properties()->declareVar(
        e, "target", Value::ofRef(17), /*notify*/ false));

    // The Runtime refuses one from the other direction too: a graph edited by
    // hand can carry `replicated` on a Ref, and it must not get past here.
    HorizonCode::Graph g;
    HorizonCode::Variable v;
    v.name = "Target"; v.type = PinType::Ref; v.replicated = true;
    g.variables = { v };
    const HorizonCode::InstanceId inst = rig.host.runtime.add(std::move(g));
    CHECK(rig.host.runtime.replicatedVariablesOf(inst).empty());
}

// ─── 8. Housekeeping ─────────────────────────────────────────────────────────

TEST_CASE("properties: an entity with Replicates off carries no properties") {
    Rig rig;
    const Entity secret = authoredEntity(*rig.host.world, "Secret", /*replicates*/ false);
    startHost(rig);
    Peer& client = addClient(rig, 12);
    const Entity clientSecret = mirrorOf(*rig.host.world, secret, *client.world);

    // Declaring is allowed — it is ordinary local state — but nothing leaves.
    REQUIRE(rig.host.session->properties()->declareVar(secret, "code", Value::ofInt(1234), true));
    rig.step(12);
    REQUIRE(rig.host.session->properties()->setVar(secret, "code", Value::ofInt(4321)));
    rig.step(12);

    Value v;
    CHECK_FALSE(client.session->properties()->getVar(clientSecret, "code", v));
    CHECK(client.session->properties()->stats().tablesReceived == 0u);
    CHECK(drainNotifications(client).total == 0);
}

TEST_CASE("properties: the overlay's cost list names the expensive variable") {
    Rig rig;
    const Entity e = authoredEntity(*rig.host.world, "Chatty");
    startHost(rig);
    Peer& client = addClient(rig, 13);
    mirrorOf(*rig.host.world, e, *client.world);

    REQUIRE(rig.host.session->properties()->declareVar(e, "tiny", Value::ofBool(false), false));
    REQUIRE(rig.host.session->properties()->declareVar(
        e, "chatter", Value::ofString(""), false));
    rig.step(12);

    for (int i = 0; i < 4; ++i) {
        REQUIRE(rig.host.session->properties()->setVar(
            e, "chatter", Value::ofString(std::string(200, 'x') + std::to_string(i))));
        rig.step(2);
    }
    REQUIRE(rig.host.session->properties()->setVar(e, "tiny", Value::ofBool(true)));
    rig.step(4);

    const auto costs = rig.host.session->properties()->costliestProperties(2);
    REQUIRE(costs.size() >= 1u);
    CHECK(costs[0].name == "chatter");
    CHECK(costs[0].bytes > 0u);
}
