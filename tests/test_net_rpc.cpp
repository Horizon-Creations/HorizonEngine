#include "doctest.h"

#include <HorizonScene/AntiCheat/AntiCheatService.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/NetworkComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/GameReplication.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Net/NetGameSession.h>
#include <HorizonScene/Net/NetMessages.h>
#include <HorizonScene/Net/RpcRouter.h>
#include <HorizonScene/Net/ValueWire.h>

#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>

#include <Net/BitStream.h>
#include <Net/ITransport.h>
#include <Net/LossyTransport.h>
#include <Project/ProjectSettings.h>

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace HE::Net;
using namespace HE::Net::Game;
using HorizonCode::PinType;
using HorizonCode::RunOn;
using HorizonCode::Value;

// ─── Remote calls (plan §7) ──────────────────────────────────────────────────
// Two NetGameSessions against each other over LossyTransport, headless, on a
// simulated clock — test_net_game_session's harness again, because the question
// is the same one a layer up: does an ACTION a client took reach the host, and
// does an action the host refuses stay refused.
//
// The oracle throughout is the EFFECT, never a counter: a variable the callee's
// graph writes. A counter goes up for a message that was parsed and then
// dropped, which is exactly the failure the owner, rate and format checks are
// about — so a counter alone would pass the tests that matter most.

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

// ── The classes under test ──────────────────────────────────────────────────
// A door with one Server function. `Open [Run On: Server] { Opened = true }` is
// the plan's own example (§7.5), reduced to the part a test can see.
HorizonCode::Graph doorClass(std::uint8_t runOn = (std::uint8_t)RunOn::Server,
                             bool anyClient = false,
                             bool withParam = false)
{
    using namespace HorizonCode;
    Graph g;
    Variable opened; opened.name = "Opened"; opened.type = PinType::Bool;
    Variable amount; amount.name = "Amount"; amount.type = PinType::Int;
    g.variables = { opened, amount };

    // Open(…) { Opened = true }
    Node fe; fe.type = NodeType::FunctionEntry; fe.s = "Open";
    fe.runOn = runOn; fe.anyClient = anyClient;
    if (withParam) fe.params = { { "howMuch", PinType::Int } };
    const int feId = g.addNode(fe);

    Node lit; lit.type = NodeType::ConstBool; lit.f[0] = 1.0f;
    const int litId = g.addNode(lit);
    Node sv; sv.type = NodeType::SetVariable; sv.s = "Opened"; sv.propType = PinType::Bool;
    const int svId = g.addNode(sv);
    REQUIRE(g.connect(feId, 0, svId, 0));          // exec
    REQUIRE(g.connect(litId, 0, svId, 2));         // true → value

    if (withParam)
    {
        // …and Amount = howMuch, so a parameter that travelled can be read back.
        Node sa; sa.type = NodeType::SetVariable; sa.s = "Amount"; sa.propType = PinType::Int;
        const int saId = g.addNode(sa);
        REQUIRE(g.connect(svId, 1, saId, 0));      // exec out of the first set
        REQUIRE(g.connect(feId, 1, saId, 2));      // the parameter into the value pin
    }
    return g;
}

// The same class plus a function that CALLS Open on itself. That is the
// caller's half of the graph, and the whole point of it is that NOTHING in the
// call says "network" — the mode at Open's header is the entire declaration.
HorizonCode::Graph callerClass(std::uint8_t runOn = (std::uint8_t)RunOn::Server,
                               bool anyClient = false)
{
    using namespace HorizonCode;
    Graph g = doorClass(runOn, anyClient);
    Node te; te.type = NodeType::FunctionEntry; te.s = "Trigger";
    const int teId = g.addNode(te);
    Node call; call.type = NodeType::FunctionCall; call.s = "Open";
    const int callId = g.addNode(call);
    REQUIRE(g.connect(teId, 0, callId, 0));
    return g;
}

// What an application does after the session update (GameApplication::
// dispatchNetEvents): drain the router into the frontends. Only the HorizonCode
// station here — the Lua one needs a ScriptContext and gets its own case.
struct Delivered {
    int      count        = 0;
    PlayerId senderDuring = kNoPlayer;   // net.rpcSender WHILE the call ran
    PlayerId senderAfter  = kNoPlayer;   // …and after the drain loop ended
};
Delivered deliver(Peer& p) {
    Delivered out;
    RpcRouter::Call call;
    while (p.session->rpc()->takeCall(call)) {
        ++out.count;
        out.senderDuring = p.session->rpc()->rpcSender();
        const auto it = p.instances.find(static_cast<std::uint32_t>(call.entity));
        if (it != p.instances.end())
            p.runtime.callFunction(it->second, call.name, /*requirePublic*/ false, call.args);
    }
    out.senderAfter = p.session->rpc()->rpcSender();
    return out;
}

// Deliver on every peer, every frame — the frame order an application runs.
void pump(Rig& rig, int frames = 12) {
    for (int i = 0; i < frames; ++i) {
        rig.step(1);
        deliver(rig.host);
        for (auto& c : rig.clients) deliver(*c);
    }
}

// Anti-cheat on, with the defaults, so a Hard observation has somewhere to go.
NetGameSession::HostOptions guardedHost() {
    static HE::ProjectAntiCheatSettings ac;   // static: the session keeps the pointer
    ac.enabled = true;
    NetGameSession::HostOptions o = defaultHost();
    o.antiCheat = &ac;
    return o;
}

// The visible consequence of a HARD observation: the host kicks, so the player
// is gone from the roster and the client is no longer in the session. A better
// oracle than the score, because the score is an implementation detail and
// being thrown out is what the player experiences — and because by the time a
// test could read the score, the kick has already taken the connection the
// score was keyed by.
bool kickedOut(Rig& rig, Peer& client) {
    pump(rig, 20);
    return rig.host.session->roster().size() == 1u &&
           client.session->status() != NetGameSession::Status::Joined;
}

// Put a class on an entity on BOTH sides of a session, wired the way the
// application would: the instance knows its entity (Runtime::ownedEntity is
// what the route hook reads) and the peer's map knows the instance.
HorizonCode::InstanceId place(Peer& p, Entity e, HorizonCode::Graph g) {
    const HorizonCode::InstanceId inst = p.runtime.add(std::move(g));
    REQUIRE(inst != 0);
    p.runtime.setOwnedEntity(inst, static_cast<std::uint32_t>(e));
    p.instances[static_cast<std::uint32_t>(e)] = inst;
    return inst;
}

} // namespace

// ─── 1. The happy path: an owner asks, the host runs it ──────────────────────

TEST_CASE("rpc: a CallServer from the entity's owner runs the function on the host") {
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    const HorizonCode::InstanceId hostInst = place(rig.host, door, callerClass());
    startHost(rig, guardedHost());

    Peer& client = addClient(rig, 21);
    const Entity clientDoor = mirrorOf(*rig.host.world, door, *client.world);
    const HorizonCode::InstanceId clientInst = place(client, clientDoor, callerClass());

    pump(rig);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);

    // The client OWNS this entity — the case §7.6 point 2 lets through.
    REQUIRE(rig.host.session->assignControl(2, door));
    pump(rig);

    // The client's graph calls Open. Nothing in it says "network"; the mode at
    // the function header is the whole of the declaration.
    REQUIRE(client.runtime.callFunction(clientInst, "Trigger"));

    // It did NOT run on the client: that is what Run On means.
    CHECK(client.runtime.getVariable(clientInst, "Opened").b == false);

    pump(rig);
    CHECK(rig.host.runtime.getVariable(hostInst, "Opened").b == true);
    CHECK(rig.host.session->rpc()->stats().delivered == 1u);
    CHECK(rig.host.session->rpc()->stats().notOwner == 0u);
}

TEST_CASE("rpc: the caller is named during delivery and nobody afterwards") {
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    place(rig.host, door, callerClass());
    startHost(rig);

    Peer& client = addClient(rig, 22);
    const Entity clientDoor = mirrorOf(*rig.host.world, door, *client.world);
    const HorizonCode::InstanceId clientInst = place(client, clientDoor, callerClass());
    pump(rig);
    REQUIRE(rig.host.session->assignControl(2, door));
    pump(rig);
    CHECK(rig.host.session->rpc()->rpcSender() == kNoPlayer);

    REQUIRE(client.runtime.callFunction(clientInst, "Trigger"));
    // Stepped WITHOUT delivering, so the call is sitting in the queue.
    rig.step(12);

    const Delivered d = deliver(rig.host);
    REQUIRE(d.count == 1);
    CHECK(d.senderDuring == 2u);       // the joining client is player 2
    // …and the drain loop's exit takes it back: a graph asking outside a
    // handler must not be told whoever went last.
    CHECK(d.senderAfter == kNoPlayer);
}

// ─── 2. A claim that is not true ─────────────────────────────────────────────

TEST_CASE("rpc: a CallServer for an entity the caller does not own is refused Hard") {
    Rig rig;
    // TWO entities: one the client owns, one it does not. Without the first,
    // the client would be a connection that owns nothing, which is a different
    // refusal and would pass this test for the wrong reason.
    const Entity mine  = authoredEntity(*rig.host.world, "Mine");
    const Entity yours = authoredEntity(*rig.host.world, "Yours");
    const HorizonCode::InstanceId hostYours = place(rig.host, yours, callerClass());
    startHost(rig, guardedHost());

    Peer& client = addClient(rig, 23);
    const Entity cMine  = mirrorOf(*rig.host.world, mine,  *client.world);
    const Entity cYours = mirrorOf(*rig.host.world, yours, *client.world);
    place(client, cMine, callerClass());
    const HorizonCode::InstanceId cYoursInst = place(client, cYours, callerClass());
    pump(rig);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);
    REQUIRE(rig.host.session->assignControl(2, mine));
    pump(rig);

    REQUIRE(client.runtime.callFunction(cYoursInst, "Trigger"));
    pump(rig);

    // It travelled — and was thrown away on arrival.
    CHECK(rig.host.session->rpc()->stats().received >= 1u);
    CHECK(rig.host.session->rpc()->stats().notOwner == 1u);
    CHECK(rig.host.session->rpc()->stats().delivered == 0u);
    CHECK(rig.host.runtime.getVariable(hostYours, "Opened").b == false);

    // Hard, so a single one is Confirmed and the host throws them out.
    CHECK(kickedOut(rig, client));
}

TEST_CASE("rpc: Any Client lets a stranger open a door that belongs to nobody") {
    Rig rig;
    // Nobody is given this entity, which is every authored prop in a scene.
    const Entity door = authoredEntity(*rig.host.world, "Door");
    const HorizonCode::InstanceId hostInst =
        place(rig.host, door, callerClass((std::uint8_t)RunOn::Server, /*anyClient*/ true));
    startHost(rig, guardedHost());

    Peer& client = addClient(rig, 24);
    const Entity clientDoor = mirrorOf(*rig.host.world, door, *client.world);
    const HorizonCode::InstanceId clientInst =
        place(client, clientDoor, callerClass((std::uint8_t)RunOn::Server, true));
    pump(rig);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);

    REQUIRE(client.runtime.callFunction(clientInst, "Trigger"));
    pump(rig);

    CHECK(rig.host.runtime.getVariable(hostInst, "Opened").b == true);
    CHECK(rig.host.session->rpc()->stats().notOwner == 0u);
}

TEST_CASE("rpc: without Any Client the same unowned door stays shut") {
    // The negative control for the case above. Without it, that one would also
    // pass on a build where the owner check never ran at all.
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    const HorizonCode::InstanceId hostInst = place(rig.host, door, callerClass());
    startHost(rig, guardedHost());
    Peer& client = addClient(rig, 25);
    const Entity clientDoor = mirrorOf(*rig.host.world, door, *client.world);
    const HorizonCode::InstanceId clientInst = place(client, clientDoor, callerClass());
    pump(rig);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);

    REQUIRE(client.runtime.callFunction(clientInst, "Trigger"));
    pump(rig);
    CHECK(rig.host.runtime.getVariable(hostInst, "Opened").b == false);
    CHECK(rig.host.session->rpc()->stats().notOwner == 1u);
}

TEST_CASE("rpc: allowAnyClient opens a function for the frontends with no header") {
    // Lua, Python and a native module have no function header to tick, so they
    // declare it on the entity instead. Same effect, same check.
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    // No HorizonCode class on the host side at all: nothing here has a
    // signature, which is the situation the entity-side declaration is for.
    //
    // Anti-cheat OFF on purpose. What is under test is the ROUTER's refusal and
    // then its acceptance; with the guard on, the first refusal is Hard, the
    // host kicks, and the second call never arrives to prove anything. The Hard
    // path has its own cases above.
    startHost(rig);

    Peer& client = addClient(rig, 26);
    const Entity clientDoor = mirrorOf(*rig.host.world, door, *client.world);
    const HorizonCode::InstanceId clientInst =
        place(client, clientDoor, callerClass((std::uint8_t)RunOn::Server, true));
    pump(rig);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);

    // Without the declaration: refused, because the entity belongs to nobody
    // and the host can see no Any Client flag anywhere.
    REQUIRE(client.runtime.callFunction(clientInst, "Trigger"));
    pump(rig);
    CHECK(rig.host.session->rpc()->stats().notOwner == 1u);
    CHECK(rig.host.session->rpc()->stats().delivered == 0u);

    // With it: through.
    rig.host.session->rpc()->allowAnyClient(door, "Open");
    REQUIRE(client.runtime.callFunction(clientInst, "Trigger"));
    pump(rig);
    CHECK(rig.host.session->rpc()->stats().delivered == 1u);
}

TEST_CASE("rpc: a client that tells the host to run a client call is refused Hard") {
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    place(rig.host, door, doorClass());
    startHost(rig, guardedHost());
    Peer& client = addClient(rig, 27);
    mirrorOf(*rig.host.world, door, *client.world);
    pump(rig);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);

    // Hand-built, because our own writer never produces it: that is exactly
    // what makes it worth a Hard observation (plan §7.3).
    const auto* nc = rig.host.world->registry().try_get<NetworkComponent>(door);
    REQUIRE(nc != nullptr);
    REQUIRE(nc->netId != 0u);
    BitWriter w;
    w.writeUInt32(nc->netId);
    w.writeByte(static_cast<std::uint8_t>(RunOn::AllClients));
    w.writeUInt32(2);
    w.writeString("Open");
    w.writeByte(0);
    const auto conns = client.session->session()->connections();
    REQUIRE_FALSE(conns.empty());
    client.session->session()->send(conns.front(), kMsgRpc, w, SendMode::ReliableOrdered);
    pump(rig);

    CHECK(rig.host.session->rpc()->stats().wrongDirection == 1u);
    CHECK(rig.host.session->rpc()->stats().delivered == 0u);
    CHECK(kickedOut(rig, client));
}

TEST_CASE("rpc: arguments that do not match the function are refused Hard") {
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    // The host's Open takes ONE Int. A call with no arguments, or with a
    // String, is a message our own writer could not have produced from this
    // graph — both sides generated it from the same class.
    const HorizonCode::InstanceId hostInst = place(
        rig.host, door, doorClass((std::uint8_t)RunOn::Server, /*anyClient*/ true,
                                  /*withParam*/ true));
    startHost(rig, guardedHost());
    Peer& client = addClient(rig, 28);
    mirrorOf(*rig.host.world, door, *client.world);
    pump(rig);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);

    const auto* nc = rig.host.world->registry().try_get<NetworkComponent>(door);
    REQUIRE(nc != nullptr);
    const auto conns = client.session->session()->connections();
    REQUIRE_FALSE(conns.empty());

    auto sendOpen = [&](const std::vector<Value>& args) {
        BitWriter w;
        w.writeUInt32(nc->netId);
        w.writeByte(static_cast<std::uint8_t>(RunOn::Server));
        w.writeUInt32(2);
        w.writeString("Open");
        w.writeByte(static_cast<std::uint8_t>(args.size()));
        for (const Value& v : args) REQUIRE(writeValue(w, v));
        client.session->session()->send(conns.front(), kMsgRpc, w, SendMode::ReliableOrdered);
    };

    // POSITIVE CONTROL FIRST, and that order is not cosmetic: a format mismatch
    // is Hard, the host kicks on it, and a good call sent afterwards would
    // never arrive — so a test that checked the good one last would "fail"
    // against a router that works perfectly.
    sendOpen({ Value::ofInt(7) });
    pump(rig);
    CHECK(rig.host.session->rpc()->stats().delivered == 1u);
    CHECK(rig.host.runtime.getVariable(hostInst, "Opened").b == true);
    CHECK(rig.host.runtime.getVariable(hostInst, "Amount").i == 7);

    sendOpen({});                                   // too few
    sendOpen({ Value::ofString("lots") });          // right count, wrong type
    pump(rig);
    CHECK(rig.host.session->rpc()->stats().formatMismatch == 2u);
    CHECK(rig.host.session->rpc()->stats().delivered == 1u);   // still just the good one
    CHECK(rig.host.runtime.getVariable(hostInst, "Amount").i == 7);
    CHECK(kickedOut(rig, client));
}

TEST_CASE("rpc: a flood is refused and weighed, an ordinary rate is not") {
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    place(rig.host, door, doorClass((std::uint8_t)RunOn::Server, /*anyClient*/ true));
    startHost(rig, guardedHost());
    Peer& client = addClient(rig, 29);
    mirrorOf(*rig.host.world, door, *client.world);
    pump(rig);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);

    const auto* nc = rig.host.world->registry().try_get<NetworkComponent>(door);
    const auto conns = client.session->session()->connections();
    REQUIRE_FALSE(conns.empty());
    auto shout = [&](int n) {
        for (int i = 0; i < n; ++i) {
            BitWriter w;
            w.writeUInt32(nc->netId);
            w.writeByte(static_cast<std::uint8_t>(RunOn::Server));
            w.writeUInt32(2);
            w.writeString("Open");
            w.writeByte(0);
            client.session->session()->send(conns.front(), kMsgRpc, w, SendMode::ReliableOrdered);
        }
    };

    // Comfortably inside the window's allowance (60/s over a 2 s window = 120).
    rig.host.session->rpc()->resetStats();
    shout(40);
    pump(rig, 20);
    CHECK(rig.host.session->rpc()->stats().rateLimited == 0u);
    CHECK(rig.host.session->rpc()->stats().delivered == 40u);

    // …and well past it. The surplus is dropped, not applied.
    shout(300);
    pump(rig, 20);
    CHECK(rig.host.session->rpc()->stats().rateLimited > 0u);
    CHECK(rig.host.session->rpc()->stats().delivered < 340u);
}

// ─── 3. Order, under a network that reorders ─────────────────────────────────

TEST_CASE("rpc: calls arrive in the order they were made, through loss and reorder") {
    Rig rig;
    const Entity board = authoredEntity(*rig.host.world, "Board");
    startHost(rig, guardedHost());
    Peer& client = addClient(rig, 30);
    mirrorOf(*rig.host.world, board, *client.world);
    pump(rig);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);
    rig.host.session->rpc()->allowAnyClient(board, "Note");

    const auto* nc = rig.host.world->registry().try_get<NetworkComponent>(board);
    const auto conns = client.session->session()->connections();
    REQUIRE_FALSE(conns.empty());

    // The reorder the transport is configured for only has something to chew on
    // when several messages are in flight at once, so they all go out in ONE
    // frame (Memory `lossy-transport-reorder-vs-tick`).
    constexpr int kCalls = 40;
    for (int i = 0; i < kCalls; ++i) {
        BitWriter w;
        w.writeUInt32(nc->netId);
        w.writeByte(static_cast<std::uint8_t>(RunOn::Server));
        w.writeUInt32(2);
        w.writeString("Note");
        w.writeByte(1);
        REQUIRE(writeValue(w, Value::ofInt(i)));
        client.session->session()->send(conns.front(), kMsgRpc, w, SendMode::ReliableOrdered);
    }

    // Collected off the queue rather than through a graph: the ORDER is what is
    // under test, and a graph would only be a slower way to write it down.
    std::vector<int> got;
    for (int frame = 0; frame < 80; ++frame) {
        rig.step(1);
        RpcRouter::Call call;
        while (rig.host.session->rpc()->takeCall(call)) {
            REQUIRE(call.args.size() == 1u);
            got.push_back(call.args[0].i);
        }
    }

    REQUIRE(got.size() == (std::size_t)kCalls);
    for (int i = 0; i < kCalls; ++i) CHECK(got[(std::size_t)i] == i);

    // The witness: the link really did mangle the stream. Without it this case
    // would pass on a network that never reordered anything, which proves
    // nothing about ordering.
    const auto ls = rig.host.lossy->stats();
    CHECK(ls.dropped + ls.reordered + ls.duplicated > 0u);
}

// ─── 4. The sides that are already home ──────────────────────────────────────

TEST_CASE("rpc: offline, a Run On function is an ordinary function") {
    // The single-player answer the whole feature rests on: the door in the docs
    // has to work before anybody hosts anything.
    HorizonCode::Runtime rt;
    const HorizonCode::InstanceId inst = rt.add(callerClass());
    REQUIRE(inst != 0);
    CHECK_FALSE(rt.hasRpcRoute());
    REQUIRE(rt.callFunction(inst, "Trigger"));
    CHECK(rt.getVariable(inst, "Opened").b == true);
}

TEST_CASE("rpc: the host calling its own Server function runs it and sends nothing") {
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    const HorizonCode::InstanceId hostInst = place(rig.host, door, callerClass());
    startHost(rig);
    Peer& client = addClient(rig, 31);
    mirrorOf(*rig.host.world, door, *client.world);
    pump(rig);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);
    rig.host.session->rpc()->resetStats();

    REQUIRE(rig.host.runtime.callFunction(hostInst, "Trigger"));
    CHECK(rig.host.runtime.getVariable(hostInst, "Opened").b == true);
    CHECK(rig.host.session->rpc()->stats().sent == 0u);
}

TEST_CASE("rpc: an Owning Client call on the host's own character stays here") {
    Rig rig;
    const Entity mine = authoredEntity(*rig.host.world, "HostCharacter");
    const HorizonCode::InstanceId hostInst =
        place(rig.host, mine, callerClass((std::uint8_t)RunOn::OwningClient));
    startHost(rig);
    Peer& client = addClient(rig, 32);
    mirrorOf(*rig.host.world, mine, *client.world);
    pump(rig);
    // Player 1 is the host itself, so the host IS the owning client.
    REQUIRE(rig.host.session->assignControl(1, mine));
    rig.host.session->rpc()->resetStats();

    REQUIRE(rig.host.runtime.callFunction(hostInst, "Trigger"));
    CHECK(rig.host.runtime.getVariable(hostInst, "Opened").b == true);
    CHECK(rig.host.session->rpc()->stats().sent == 0u);
}

TEST_CASE("rpc: an Owning Client call reaches the one player and nobody else") {
    Rig rig;
    const Entity character = authoredEntity(*rig.host.world, "Character");
    place(rig.host, character, callerClass((std::uint8_t)RunOn::OwningClient));
    startHost(rig);

    Peer& owner     = addClient(rig, 33, defaultJoin("Owner"));
    Peer& bystander = addClient(rig, 34, defaultJoin("Bystander"));
    const HorizonCode::InstanceId ownerInst =
        place(owner, mirrorOf(*rig.host.world, character, *owner.world),
              callerClass((std::uint8_t)RunOn::OwningClient));
    const HorizonCode::InstanceId byInst =
        place(bystander, mirrorOf(*rig.host.world, character, *bystander.world),
              callerClass((std::uint8_t)RunOn::OwningClient));
    pump(rig);
    REQUIRE(owner.session->status() == NetGameSession::Status::Joined);
    REQUIRE(bystander.session->status() == NetGameSession::Status::Joined);
    REQUIRE(rig.host.session->assignControl(2, character));
    pump(rig);

    const auto hostInstIt = rig.host.instances.find(static_cast<std::uint32_t>(character));
    REQUIRE(hostInstIt != rig.host.instances.end());
    REQUIRE(rig.host.runtime.callFunction(hostInstIt->second, "Trigger"));
    // Not on the host: the owner is somebody else.
    CHECK(rig.host.runtime.getVariable(hostInstIt->second, "Opened").b == false);

    pump(rig);
    CHECK(owner.runtime.getVariable(ownerInst, "Opened").b == true);
    CHECK(bystander.runtime.getVariable(byInst, "Opened").b == false);
}

TEST_CASE("rpc: All Clients runs on every client and on the host as well") {
    Rig rig;
    const Entity horn = authoredEntity(*rig.host.world, "Horn");
    const HorizonCode::InstanceId hostInst =
        place(rig.host, horn, callerClass((std::uint8_t)RunOn::AllClients));
    startHost(rig);

    Peer& a = addClient(rig, 35, defaultJoin("A"));
    Peer& b = addClient(rig, 36, defaultJoin("B"));
    const HorizonCode::InstanceId aInst =
        place(a, mirrorOf(*rig.host.world, horn, *a.world),
              callerClass((std::uint8_t)RunOn::AllClients));
    const HorizonCode::InstanceId bInst =
        place(b, mirrorOf(*rig.host.world, horn, *b.world),
              callerClass((std::uint8_t)RunOn::AllClients));
    pump(rig);
    REQUIRE(a.session->status() == NetGameSession::Status::Joined);
    REQUIRE(b.session->status() == NetGameSession::Status::Joined);

    REQUIRE(rig.host.runtime.callFunction(hostInst, "Trigger"));
    // A multicast reaches every machine, and the host is one — it does not
    // stand outside its own broadcast.
    CHECK(rig.host.runtime.getVariable(hostInst, "Opened").b == true);

    pump(rig);
    CHECK(a.runtime.getVariable(aInst, "Opened").b == true);
    CHECK(b.runtime.getVariable(bInst, "Opened").b == true);
}

TEST_CASE("rpc: a client may not make other machines run anything") {
    Rig rig;
    const Entity horn = authoredEntity(*rig.host.world, "Horn");
    place(rig.host, horn, callerClass((std::uint8_t)RunOn::AllClients));
    startHost(rig);
    Peer& a = addClient(rig, 37, defaultJoin("A"));
    Peer& b = addClient(rig, 38, defaultJoin("B"));
    const HorizonCode::InstanceId aInst =
        place(a, mirrorOf(*rig.host.world, horn, *a.world),
              callerClass((std::uint8_t)RunOn::AllClients));
    const HorizonCode::InstanceId bInst =
        place(b, mirrorOf(*rig.host.world, horn, *b.world),
              callerClass((std::uint8_t)RunOn::AllClients));
    pump(rig);
    REQUIRE(a.session->status() == NetGameSession::Status::Joined);
    a.session->rpc()->resetStats();

    // A client running the same graph: it runs HERE and travels nowhere.
    REQUIRE(a.runtime.callFunction(aInst, "Trigger"));
    CHECK(a.runtime.getVariable(aInst, "Opened").b == true);
    CHECK(a.session->rpc()->stats().sent == 0u);
    pump(rig);
    CHECK(b.runtime.getVariable(bInst, "Opened").b == false);
}

// ─── 5. What a call carries ──────────────────────────────────────────────────

TEST_CASE("rpc: arguments survive the trip, and a Ref never leaves") {
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    const HorizonCode::InstanceId hostInst = place(
        rig.host, door, doorClass((std::uint8_t)RunOn::Server, true, /*withParam*/ true));
    startHost(rig);
    Peer& client = addClient(rig, 39);
    const Entity clientDoor = mirrorOf(*rig.host.world, door, *client.world);
    pump(rig);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);

    REQUIRE(client.session->rpc()->callServer(clientDoor, "Open", { Value::ofInt(42) }));
    pump(rig);
    CHECK(rig.host.runtime.getVariable(hostInst, "Amount").i == 42);

    // A Ref is a local handle that names nothing over there (plan §6.1). It is
    // refused before a byte goes out rather than arriving as a number that
    // resolves to a stranger's object.
    Value ref; ref.type = PinType::Ref; ref.ref = 7;
    const std::uint32_t sentBefore = client.session->rpc()->stats().sent;
    CHECK_FALSE(client.session->rpc()->callServer(clientDoor, "Open", { ref }));
    CHECK(client.session->rpc()->stats().sent == sentBefore);
    CHECK(client.session->rpc()->stats().refusedArgument == 1u);
}

TEST_CASE("rpc: a call for an entity the host does not know is dropped, not crashed") {
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    startHost(rig, guardedHost());
    Peer& client = addClient(rig, 40);
    mirrorOf(*rig.host.world, door, *client.world);
    pump(rig);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);

    BitWriter w;
    w.writeUInt32(999999);                                   // no such net id
    w.writeByte(static_cast<std::uint8_t>(RunOn::Server));
    w.writeUInt32(2);
    w.writeString("Open");
    w.writeByte(0);
    const auto conns = client.session->session()->connections();
    REQUIRE_FALSE(conns.empty());
    client.session->session()->send(conns.front(), kMsgRpc, w, SendMode::ReliableOrdered);
    pump(rig);

    CHECK(rig.host.session->rpc()->stats().unknownEntity == 1u);
    CHECK(rig.host.session->rpc()->stats().delivered == 0u);
    // NOT a cheat: an entity destroyed while a call was in flight is server
    // state, and the client did nothing wrong.
    auto* ac = rig.host.session->antiCheatService();
    REQUIRE(ac != nullptr);
    const auto hostConns = rig.host.session->session()->connections();
    REQUIRE_FALSE(hostConns.empty());
    CHECK(ac->level(hostConns.front()) == HE::AntiCheat::Level::Info);
}

TEST_CASE("rpc: a truncated call is dropped and the next one still arrives") {
    Rig rig;
    const Entity door = authoredEntity(*rig.host.world, "Door");
    const HorizonCode::InstanceId hostInst =
        place(rig.host, door, doorClass((std::uint8_t)RunOn::Server, true));
    startHost(rig, guardedHost());
    Peer& client = addClient(rig, 41);
    mirrorOf(*rig.host.world, door, *client.world);
    pump(rig);
    REQUIRE(client.session->status() == NetGameSession::Status::Joined);

    const auto* nc = rig.host.world->registry().try_get<NetworkComponent>(door);
    const auto conns = client.session->session()->connections();
    REQUIRE_FALSE(conns.empty());

    BitWriter bad;
    bad.writeUInt32(nc->netId);
    bad.writeByte(static_cast<std::uint8_t>(RunOn::Server));
    // …and it stops here, mid-message.
    client.session->session()->send(conns.front(), kMsgRpc, bad, SendMode::ReliableOrdered);

    BitWriter good;
    good.writeUInt32(nc->netId);
    good.writeByte(static_cast<std::uint8_t>(RunOn::Server));
    good.writeUInt32(2);
    good.writeString("Open");
    good.writeByte(0);
    client.session->session()->send(conns.front(), kMsgRpc, good, SendMode::ReliableOrdered);
    pump(rig);

    CHECK(rig.host.session->rpc()->stats().malformed == 1u);
    CHECK(rig.host.session->rpc()->stats().delivered == 1u);
    CHECK(rig.host.runtime.getVariable(hostInst, "Opened").b == true);
}

// ─── 6. The arguments a native module sees ───────────────────────────────────

TEST_CASE("rpc: arguments become JSON for the native module boundary") {
    // IGameLogic::onRpc takes a string, the same trade onRep makes. What it
    // takes has to be parseable, and a map has to keep its authored order —
    // which is why a map is an array of pairs and not an object.
    CHECK(argsToJson({}) == "[]");
    CHECK(argsToJson({ Value::ofInt(3), Value::ofBool(true) }) == "[3,true]");
    CHECK(argsToJson({ Value::ofString("a\"b") }) == "[\"a\\\"b\"]");
    CHECK(argsToJson({ Value::ofVec3({ 1.0f, 2.0f, 3.0f }) }) == "[[1,2,3]]");

    Value arr; arr.type = PinType::Int; arr.isArray = true;
    arr.container = HorizonCode::ContainerKind::Array;
    arr.items = { Value::ofInt(1), Value::ofInt(2) };
    CHECK(argsToJson({ arr }) == "[[1,2]]");

    Value map; map.type = PinType::Int; map.isArray = true;
    map.container = HorizonCode::ContainerKind::Map;
    map.keyType = PinType::String;
    map.keys  = { Value::ofString("b"), Value::ofString("a") };
    map.items = { Value::ofInt(1), Value::ofInt(2) };
    // "b" first, because that is the order it was written in — the one thing a
    // JSON object would have quietly changed.
    CHECK(argsToJson({ map }) == "[[{\"key\":\"b\",\"value\":1},{\"key\":\"a\",\"value\":2}]]");
}
