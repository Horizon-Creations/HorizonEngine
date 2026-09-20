#include "doctest.h"

#include <HorizonScene/AntiCheat/AntiCheatHost.h>
#include <HorizonScene/AntiCheat/AntiCheatService.h>
#include <HorizonScene/GameReplication.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/MovementComponent.h>
#include <HorizonScene/Components/NetworkComponent.h>
#include <HorizonScene/Components/TransformComponent.h>

#include <Diagnostics/Log.h>
#include <Events/EventBus.h>
#include <Net/LoopbackTransport.h>
#include <Net/NetSession.h>
#include <Net/SecureTransport.h>
#include <Project/ProjectSettings.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace HE::Net;
using HE::AntiCheat::AntiCheatService;
using HE::AntiCheat::Kind;
using HE::AntiCheat::Level;

// ─── Anti-cheat, step 2: the server path (docs/anti-cheat-plan.md §7.4) ──────
// Everything here runs over LoopbackTransport, exactly like the replication
// tests: no sockets, deterministic delivery, one pump round = one host frame.
// The nine cases are the plan's test plan one to one. Kick and notice are
// step 4 of the plan and deliberately absent here; the cases that mention
// them pin the current behaviour (the connection stays) instead.

namespace {

constexpr MessageId kMsgInput = kFirstUserMessage + 201;
constexpr float     kFrame    = 1.0f / 60.0f;

// A mover WITHOUT a clamp on |move| — the game bug a cheater exploits, and
// the thing the post-apply check exists to catch. A move.x of 1000 or more is
// this game's dash: a 100-unit teleport, legitimate only when announced.
GameReplication::MoveFn unclampedMover() {
    return [](TransformComponent& tc, const GameReplication::InputCommand& c) {
        if (c.move.x >= 1000.0f) tc.position.x += 100.0f;
        else                     tc.position += c.move * c.deltaTime;
        tc.rotation.y = c.yaw;
    };
}

struct Rig {
    std::unique_ptr<LoopbackTransport> serverT, clientT;
    std::unique_ptr<NetSession>        serverNet, clientNet;
    std::unique_ptr<GameReplication>   server, client;
    HorizonWorld serverWorld, clientWorld;
    AntiCheatService svc;

    Entity        serverEntity = entt::null;
    Entity        clientEntity = entt::null;
    std::uint32_t netId        = 0;

    // One host frame: deliver what is queued, then advance both simulations.
    void frame(float dt = kFrame) {
        serverT->update();
        clientT->update();
        serverNet->pump();
        clientNet->pump();
        server->update(dt);
        client->update(dt);
    }
    void frames(int n, float dt = kFrame) {
        for (int i = 0; i < n; ++i) frame(dt);
    }
    // A frame in which the client's datagrams do NOT reach the server — the
    // TCP stall of the plan. They stay queued and arrive together later.
    void stalledFrame(float dt = kFrame) {
        clientT->update();
        clientNet->pump();
        server->update(dt);
        client->update(dt);
    }

    glm::vec3 serverPos() {
        return serverWorld.registry().get<TransformComponent>(serverEntity).position;
    }

    // Bypass pushInput and write the wire format by hand, as a modified client
    // would. `truncated` drops the yaw so the frame does not parse.
    void sendRaw(std::uint32_t seq, float dt, glm::vec3 move, bool truncated = false) {
        BitWriter w;
        w.writeUInt32(seq);
        w.writeFloat(dt);
        for (int i = 0; i < 3; ++i) w.writeFloat(move[i]);
        if (!truncated) w.writeFloat(0.0f);
        clientNet->send(LoopbackTransport::kPeer, kMsgInput, w, SendMode::Unreliable);
    }
};

struct RigOptions {
    bool  attachService = true;
    bool  assignControl = true;
    float maxSpeed      = 0.0f;   // > 0 adds a MovementComponent with this bound
    HE::AntiCheat::Config config;
};

std::unique_ptr<Rig> makeRig(RigOptions opt = {}) {
    auto r = std::make_unique<Rig>();
    r->svc = AntiCheatService(opt.config);

    auto [a, b] = LoopbackTransport::createPair();
    r->serverT = std::move(a);
    r->clientT = std::move(b);
    r->serverNet = std::make_unique<NetSession>(r->serverT.get(), NetRole::Server);
    r->clientNet = std::make_unique<NetSession>(r->clientT.get(), NetRole::Client);
    r->server = std::make_unique<GameReplication>(r->serverNet.get(), NetRole::Server);
    r->client = std::make_unique<GameReplication>(r->clientNet.get(), NetRole::Client);
    r->server->setWorld(&r->serverWorld);
    r->client->setWorld(&r->clientWorld);
    r->server->setMoveFunction(unclampedMover());
    r->client->setMoveFunction(unclampedMover());
    if (opt.attachService) r->server->setAntiCheat(&r->svc);

    r->serverT->update(); r->clientT->update();
    r->serverNet->pump(); r->clientNet->pump();

    r->serverEntity = r->serverWorld.createEntity("Player");
    auto& sreg = r->serverWorld.registry();
    sreg.emplace_or_replace<TransformComponent>(r->serverEntity);
    if (opt.maxSpeed > 0.0f)
        sreg.emplace_or_replace<MovementComponent>(r->serverEntity).maxSpeed = opt.maxSpeed;
    r->netId = r->server->registerEntity(r->serverEntity);
    if (opt.assignControl) r->server->assignControl(LoopbackTransport::kPeer, r->netId);

    r->clientEntity = r->clientWorld.createEntity("Player");
    r->clientWorld.registry().emplace_or_replace<TransformComponent>(r->clientEntity);
    r->client->adoptEntity(r->clientEntity, r->netId);
    r->client->setLocallyControlled(r->clientEntity, r->netId);
    return r;
}

// Captures every AntiCheat record while alive, with the category opened all
// the way so a Trace-level leak could not hide behind the default filter.
class LogSpy {
public:
    LogSpy() : m_previous(HE::Log::verbosity(HE::Log::Cat::AntiCheat)) {
        HE::Log::setVerbosity(HE::Log::Cat::AntiCheat, HE::LogLevel::Trace);
        m_handle = HE::Log::addSink(&LogSpy::onRecord, this);
    }
    ~LogSpy() {
        HE::Log::removeSink(m_handle);
        HE::Log::setVerbosity(HE::Log::Cat::AntiCheat, m_previous);
    }
    LogSpy(const LogSpy&)            = delete;
    LogSpy& operator=(const LogSpy&) = delete;

    bool mentions(const std::string& needle) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (const std::string& line : m_lines)
            if (line.find(needle) != std::string::npos) return true;
        return false;
    }
    std::size_t count() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_lines.size();
    }

private:
    static void onRecord(const HE::Log::Record& rec, void* user) {
        if (rec.category != HE::Log::Cat::AntiCheat) return;
        auto* self = static_cast<LogSpy*>(user);
        std::lock_guard<std::mutex> lk(self->m_mutex);
        self->m_lines.emplace_back(rec.message ? rec.message : "");
    }

    mutable std::mutex       m_mutex;
    std::vector<std::string> m_lines;
    int                      m_handle = 0;
    HE::LogLevel             m_previous;
};

} // namespace

// ─── 1. Speedhack ────────────────────────────────────────────────────────────

TEST_CASE("AntiCheat: a 1.5x speedhack climbs to Suspect and then Confirmed over seconds, not frames")
{
    auto rig = makeRig();
    const ConnectionId conn = LoopbackTransport::kPeer;

    // 90 commands of 1/60 s per host second: the client's timer runs 1.5× fast.
    // Every other frame carries two commands.
    auto hackedSecond = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            rig->client->pushInput(glm::vec3(1.0f, 0, 0), 0.0f, kFrame);
            if (i % 2 == 1) rig->client->pushInput(glm::vec3(1.0f, 0, 0), 0.0f, kFrame);
            rig->frame();
        }
    };

    // One second in: at most one evaluation (3.5 points), still Info.
    hackedSecond(60);
    CHECK(rig->svc.level(conn) == Level::Info);
    CHECK(rig->svc.score(conn) < rig->svc.config().suspectThreshold);

    // Three seconds: two or three evaluations à 3.5 → Suspect, not Confirmed.
    hackedSecond(120);
    CHECK(rig->svc.level(conn) == Level::Suspect);
    CHECK(rig->svc.score(conn) >= rig->svc.config().suspectThreshold);
    CHECK(rig->svc.score(conn) <  rig->svc.config().confirmedThreshold);
    CHECK(rig->svc.observationCount(conn, Kind::DtBudget) >= 2);

    int id = 0;
    REQUIRE(rig->svc.takeReport(id));
    const auto* suspect = rig->svc.findReport(id);
    REQUIRE(suspect != nullptr);
    CHECK(suspect->level   == Level::Suspect);
    CHECK(suspect->conn    == conn);
    CHECK(suspect->trigger == Kind::DtBudget);
    CHECK_FALSE(suspect->observations.empty());
    CHECK(suspect->detail.find("DtBudget") != std::string::npos);

    // Eight seconds: 3.5 per second against a 30 s half-life crosses 20 at the
    // seventh evaluation. Per frame this would have taken a tenth of a second,
    // which is exactly the cadence the plan rules out.
    hackedSecond(300);
    CHECK(rig->svc.level(conn) == Level::Confirmed);
    REQUIRE(rig->svc.takeReport(id));
    REQUIRE(rig->svc.findReport(id) != nullptr);
    CHECK(rig->svc.findReport(id)->level == Level::Confirmed);
    CHECK_FALSE(rig->svc.takeReport(id));   // one report per level change, no more

    // The dt budget observes, it does not refuse — and nothing kicks (step 4).
    CHECK(rig->svc.stats().rejectedInputs == 0);
    CHECK(rig->serverNet->connections().size() == 1);
}

// ─── 2. Negative control: burst after a stall ────────────────────────────────

TEST_CASE("AntiCheat: thirty commands arriving at once after a 500 ms stall are not a speedhack")
{
    auto rig = makeRig();
    const ConnectionId conn = LoopbackTransport::kPeer;
    float peak = 0.0f;
    auto track = [&] { peak = std::max(peak, rig->svc.score(conn)); };

    // Two seconds of honest play to fill the window.
    for (int i = 0; i < 120; ++i) {
        rig->client->pushInput(glm::vec3(1.0f, 0, 0), 0.0f, kFrame);
        rig->frame();
        track();
    }
    // The link stalls for 500 ms: the client keeps producing, nothing arrives.
    for (int i = 0; i < 30; ++i) {
        rig->client->pushInput(glm::vec3(1.0f, 0, 0), 0.0f, kFrame);
        rig->stalledFrame();
        track();
    }
    // Then everything lands in one frame, and play continues normally.
    for (int i = 0; i < 240; ++i) {
        rig->client->pushInput(glm::vec3(1.0f, 0, 0), 0.0f, kFrame);
        rig->frame();
        track();
    }

    // The burst's dt sums to exactly the wall time the host waited; a per-frame
    // check would have flagged it, the window does not. Without this case the
    // speedhack test above would be green without saying anything.
    CHECK(peak < rig->svc.config().suspectThreshold);
    CHECK(rig->svc.level(conn) == Level::Info);
    CHECK(rig->svc.pendingReportCount() == 0);
    CHECK(rig->server->stats().inputsProcessed == 390);
}

// ─── 3. Negative control: host hitch ─────────────────────────────────────────

TEST_CASE("AntiCheat: a half-second host hitch produces no observation")
{
    auto rig = makeRig();
    const ConnectionId conn = LoopbackTransport::kPeer;

    for (int i = 0; i < 60; ++i) {
        rig->client->pushInput(glm::vec3(1.0f, 0, 0), 0.0f, kFrame);
        rig->frame();
    }
    // One frame of 500 ms on the host, no extra input: wall jumps, the accepted
    // sum does not, the ratio can only fall.
    rig->frame(0.5f);
    for (int i = 0; i < 120; ++i) {
        rig->client->pushInput(glm::vec3(1.0f, 0, 0), 0.0f, kFrame);
        rig->frame();
    }

    CHECK(rig->svc.observationCount(conn) == 0);
    CHECK(rig->svc.score(conn) == doctest::Approx(0.0f));
    CHECK(rig->svc.level(conn) == Level::Info);
}

// ─── 4. Displacement: an unclamped mover ─────────────────────────────────────

TEST_CASE("AntiCheat: a move five times faster than maxSpeed is rolled back and weighed about five")
{
    RigOptions opt;
    opt.maxSpeed = 1.0f;
    auto rig = makeRig(opt);
    const ConnectionId conn = LoopbackTransport::kPeer;

    // |move| = 5 at maxSpeed 1: the game's mover applies it unclamped and
    // carries the character 0.5 units in 0.1 s. Allowed is 1 · 0.1 · 1.15
    // (plus one quantisation step), so the weight is 0.5 / 0.115 ≈ 4.35.
    rig->client->pushInput(glm::vec3(5.0f, 0, 0), 0.0f, 0.1f);
    rig->frames(4);

    CHECK(rig->serverPos().x == doctest::Approx(0.0f));
    CHECK(rig->svc.stats().rolledBack == 1);
    REQUIRE(rig->svc.observationCount(conn, Kind::Displacement) == 1);
    CHECK(rig->svc.score(conn) > 4.0f);
    CHECK(rig->svc.score(conn) < 5.5f);
    // Still processed: the client's replay retires the command and its own
    // reconcile pulls it back, exactly like a misprediction.
    CHECK(rig->server->stats().inputsProcessed == 1);

    // A legal move at the bound passes, quantisation step included.
    rig->client->pushInput(glm::vec3(1.0f, 0, 0), 0.0f, 0.1f);
    rig->frames(4);
    CHECK(rig->serverPos().x == doctest::Approx(0.1f));
    CHECK(rig->svc.stats().rolledBack == 1);
}

// ─── 5. expectDisplacement ───────────────────────────────────────────────────

TEST_CASE("AntiCheat: an announced teleport passes, the same teleport unannounced is Confirmed at once")
{
    RigOptions opt;
    opt.maxSpeed = 1.0f;
    auto rig = makeRig(opt);
    const ConnectionId conn = LoopbackTransport::kPeer;

    // The game says "this entity is about to move up to 100 units" — a dash,
    // a portal, a respawn — and the next post-apply check lets it through.
    rig->svc.expectDisplacement(rig->netId, 100.0f);
    rig->client->pushInput(glm::vec3(1000.0f, 0, 0), 0.0f, 0.1f);
    rig->frames(4);

    CHECK(rig->serverPos().x == doctest::Approx(100.0f));
    CHECK(rig->svc.observationCount(conn) == 0);
    CHECK(rig->svc.level(conn) == Level::Info);

    // The allowance was one shot. The same dash without it is a 100-unit jump
    // against an allowance of a few centimetres: capped at the maximum weight,
    // it goes straight from Info to Confirmed, and the position stays.
    rig->client->pushInput(glm::vec3(1000.0f, 0, 0), 0.0f, 0.1f);
    rig->frames(4);

    CHECK(rig->serverPos().x == doctest::Approx(100.0f));
    REQUIRE(rig->svc.observationCount(conn, Kind::Displacement) == 1);
    CHECK(rig->svc.score(conn) >= 50.0f);
    CHECK(rig->svc.score(conn) <= HE::AntiCheat::kMaxObservationWeight);
    CHECK(rig->svc.level(conn) == Level::Confirmed);

    int id = 0;
    REQUIRE(rig->svc.takeReport(id));
    const auto* report = rig->svc.findReport(id);
    REQUIRE(report != nullptr);
    CHECK(report->level   == Level::Confirmed);
    CHECK(report->trigger == Kind::Displacement);
    CHECK(report->netId   == rig->netId);
    CHECK_FALSE(rig->svc.takeReport(id));   // Info → Confirmed is ONE change

    // An announcement that is smaller than what happened does not cover it.
    rig->svc.expectDisplacement(rig->netId, 10.0f);
    rig->client->pushInput(glm::vec3(1000.0f, 0, 0), 0.0f, 0.1f);
    rig->frames(4);
    CHECK(rig->serverPos().x == doctest::Approx(100.0f));
    CHECK(rig->svc.observationCount(conn, Kind::Displacement) == 2);
}

// ─── 6. Foreign entity / malformed: Hard ─────────────────────────────────────

TEST_CASE("AntiCheat: input from a connection that controls nothing is Hard in one step")
{
    RigOptions opt;
    opt.assignControl = false;
    auto rig = makeRig(opt);
    const ConnectionId conn = LoopbackTransport::kPeer;

    // A hand-built command from a peer the host never gave an entity.
    rig->sendRaw(1, 0.1f, glm::vec3(50.0f, 0, 0));
    rig->frames(4);

    CHECK(rig->serverPos().x == doctest::Approx(0.0f));
    CHECK(rig->server->stats().inputsProcessed == 0);
    CHECK(rig->svc.level(conn) == Level::Hard);
    CHECK(rig->svc.observationCount(conn, Kind::ForeignEntity) == 1);
    // Hard is a level, not a score: no hitch or burst could have produced it,
    // and it must not be something the score could reach on its own.
    CHECK(rig->svc.score(conn) == doctest::Approx(0.0f));

    int id = 0;
    REQUIRE(rig->svc.takeReport(id));
    const auto* report = rig->svc.findReport(id);
    REQUIRE(report != nullptr);
    CHECK(report->level   == Level::Hard);
    CHECK(report->trigger == Kind::ForeignEntity);

    // The service makes reports; it never touches the transport. Without an
    // AntiCheatHost pumping them (step 4, below) the connection stays — the
    // kick is the host's frame-end decision, not the service's.
    CHECK(rig->serverNet->connections().size() == 1);

    // Hard sticks for the session, whatever the score does afterwards.
    rig->frames(120);
    CHECK(rig->svc.level(conn) == Level::Hard);
}

TEST_CASE("AntiCheat: a truncated command is Hard, and stays out of the simulation")
{
    auto rig = makeRig();
    const ConnectionId conn = LoopbackTransport::kPeer;

    rig->sendRaw(1, 0.1f, glm::vec3(50.0f, 0, 0), /*truncated*/ true);
    rig->frames(4);

    CHECK(rig->serverPos().x == doctest::Approx(0.0f));
    CHECK(rig->server->stats().inputsProcessed == 0);
    CHECK(rig->svc.level(conn) == Level::Hard);
    CHECK(rig->svc.observationCount(conn, Kind::Malformed) == 1);
    int id = 0;
    REQUIRE(rig->svc.takeReport(id));
    REQUIRE(rig->svc.findReport(id) != nullptr);
    CHECK(rig->svc.findReport(id)->level == Level::Hard);
}

TEST_CASE("AntiCheat: a command carrying NaN is refused before it reaches the mover")
{
    auto rig = makeRig();
    const ConnectionId conn = LoopbackTransport::kPeer;

    rig->sendRaw(1, 0.1f, glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0, 0));
    rig->frames(4);

    // Had it run, the position would be NaN and the entity gone for everyone.
    CHECK(rig->serverPos().x == doctest::Approx(0.0f));
    CHECK(rig->server->stats().inputsProcessed == 0);
    CHECK(rig->svc.stats().rejectedInputs == 1);
    CHECK(rig->svc.level(conn) == Level::Hard);
    CHECK(rig->svc.observationCount(conn, Kind::Malformed) == 1);
}

// ─── 7. Decay ────────────────────────────────────────────────────────────────

TEST_CASE("AntiCheat: the score halves every half-life and falling never reports")
{
    AntiCheatService svc;
    const ConnectionId conn = 7;

    // 4.9 sits just under Suspect: an observation, no report.
    svc.report(conn, "Test", 4.9f, "unit test");
    CHECK(svc.score(conn) == doctest::Approx(4.9f));
    CHECK(svc.level(conn) == Level::Info);
    CHECK(svc.pendingReportCount() == 0);

    // Thirty seconds of frames — the half-life — in 60 Hz steps, so the decay
    // is the frame-rate-independent product and not a single big pow().
    for (int i = 0; i < 1800; ++i) svc.update(kFrame);
    CHECK(svc.score(conn) == doctest::Approx(2.45f).epsilon(0.01));
    CHECK(svc.pendingReportCount() == 0);

    // Up through Suspect is a report; decaying back below it is not.
    svc.report(conn, "Test", 4.0f);
    CHECK(svc.level(conn) == Level::Suspect);
    int id = 0;
    REQUIRE(svc.takeReport(id));
    REQUIRE(svc.findReport(id) != nullptr);
    CHECK(svc.findReport(id)->level == Level::Suspect);
    CHECK(svc.findReport(id)->detail.find("Test") != std::string::npos);

    for (int i = 0; i < 1800; ++i) svc.update(kFrame);
    CHECK(svc.level(conn) == Level::Info);
    CHECK_FALSE(svc.takeReport(id));

    // Crossing Suspect a second time is a new escalation, and a new report.
    svc.report(conn, "Test", 4.0f);
    CHECK(svc.level(conn) == Level::Suspect);
    CHECK(svc.takeReport(id));
}

TEST_CASE("AntiCheat: report tickets expire and evicted ones read as unknown")
{
    HE::AntiCheat::Config cfg;
    cfg.reportTtlSec = 5.0f;
    AntiCheatService svc(cfg);

    svc.report(3, "Rule", 6.0f);
    int id = 0;
    REQUIRE(svc.takeReport(id));
    REQUIRE(svc.findReport(id) != nullptr);

    for (int i = 0; i < 6 * 60; ++i) svc.update(kFrame);
    CHECK(svc.findReport(id) == nullptr);

    // The table is bounded: a persistent offender cannot grow the host through it.
    for (int i = 0; i < 100; ++i) {
        svc.report(static_cast<ConnectionId>(100 + i), "Rule", 6.0f);
    }
    int taken = 0;
    while (svc.takeReport(id)) {
        ++taken;
        CHECK(svc.findReport(id) != nullptr);   // never an id that cannot be read
    }
    CHECK(taken == static_cast<int>(AntiCheatService::kMaxReports));
}

// ─── 8. nullptr service: off is the absence of the object ────────────────────

TEST_CASE("AntiCheat: without a service every case behaves exactly as before")
{
    RigOptions opt;
    opt.attachService = false;
    opt.maxSpeed      = 1.0f;   // present, and ignored: nobody reads it
    auto rig = makeRig(opt);
    CHECK(rig->server->antiCheat() == nullptr);

    // The unclamped move lands in full — no rollback.
    rig->client->pushInput(glm::vec3(5.0f, 0, 0), 0.0f, 0.1f);
    rig->frames(4);
    CHECK(rig->serverPos().x == doctest::Approx(0.5f));

    // The unannounced dash lands in full.
    rig->client->pushInput(glm::vec3(1000.0f, 0, 0), 0.0f, 0.1f);
    rig->frames(4);
    CHECK(rig->serverPos().x == doctest::Approx(100.5f));

    // A speedhack's commands are all applied.
    rig->server->resetStats();
    for (int i = 0; i < 120; ++i) {
        rig->client->pushInput(glm::vec3(1.0f, 0, 0), 0.0f, kFrame);
        if (i % 2 == 1) rig->client->pushInput(glm::vec3(1.0f, 0, 0), 0.0f, kFrame);
        rig->frame();
    }
    rig->frames(4);
    CHECK(rig->server->stats().inputsProcessed == 180);

    // A host hitch and a stall change nothing either.
    rig->frame(0.5f);
    for (int i = 0; i < 30; ++i) {
        rig->client->pushInput(glm::vec3(1.0f, 0, 0), 0.0f, kFrame);
        rig->stalledFrame();
    }
    rig->frames(4);
    CHECK(rig->server->stats().inputsProcessed == 210);

    // The foreign-entity refusal is what it always was: silent, and refused.
    rig->server->assignControl(LoopbackTransport::kPeer, 99999);
    const float before = rig->serverPos().x;
    rig->client->pushInput(glm::vec3(50.0f, 0, 0), 0.0f, 0.1f);
    rig->frames(4);
    CHECK(rig->serverPos().x == doctest::Approx(before));
    CHECK(rig->server->stats().inputsProcessed == 210);

    // The service that was never attached saw none of it.
    CHECK(rig->svc.stats().observations == 0);
    CHECK(rig->svc.stats().rejectedInputs == 0);
    CHECK(rig->svc.stats().rolledBack == 0);
}

// ─── 9. Log sink: says something, never the secret ───────────────────────────

TEST_CASE("AntiCheat: the log carries the observations and never the join secret")
{
    // Deliberately distinctive, so a substring match cannot happen by accident.
    const std::string secret = "ZZANTICHEATSECRET7NEVERLOGME9XYZQ";

    LogSpy spy;

    // The real session shape: loopback under SecureTransport, so the secret is
    // genuinely in the process the anti-cheat runs in.
    auto [a, b] = LoopbackTransport::createPair();
    auto hostT   = SecureTransport::wrap(std::move(a),
                       SecureTransport::Config{ secret, NetRole::Host,   false });
    auto clientT = SecureTransport::wrap(std::move(b),
                       SecureTransport::Config{ secret, NetRole::Client, false });
    REQUIRE(hostT);
    REQUIRE(clientT);

    NetSession hostNet(hostT.get(), NetRole::Host);
    NetSession clientNet(clientT.get(), NetRole::Client);
    bool established = false;
    for (int i = 0; i < 64 && !established; ++i) {
        hostT->update(); clientT->update();
        hostNet.pump();  clientNet.pump();
        established = !hostNet.connections().empty() && !clientNet.connections().empty();
    }
    REQUIRE(established);
    const ConnectionId conn = hostNet.connections().front();

    HorizonWorld hostWorld, clientWorld;
    AntiCheatService svc;
    GameReplication host(&hostNet, NetRole::Host);
    GameReplication client(&clientNet, NetRole::Client);
    host.setWorld(&hostWorld);
    client.setWorld(&clientWorld);
    host.setMoveFunction(unclampedMover());
    client.setMoveFunction(unclampedMover());
    host.setAntiCheat(&svc);
    svc.setPlayerLabel(conn, "Alice");

    const Entity he = hostWorld.createEntity("Player");
    hostWorld.registry().emplace_or_replace<TransformComponent>(he);
    hostWorld.registry().emplace_or_replace<MovementComponent>(he).maxSpeed = 1.0f;
    const std::uint32_t netId = host.registerEntity(he);
    host.assignControl(conn, netId);
    const Entity ce = clientWorld.createEntity("Player");
    clientWorld.registry().emplace_or_replace<TransformComponent>(ce);
    client.adoptEntity(ce, netId);
    client.setLocallyControlled(ce, netId);

    auto frame = [&] {
        hostT->update(); clientT->update();
        hostNet.pump();  clientNet.pump();
        host.update(kFrame); client.update(kFrame);
    };

    // One weighed observation (Debug line) and one report (Warning line).
    client.pushInput(glm::vec3(5.0f, 0, 0), 0.0f, 0.1f);
    for (int i = 0; i < 4; ++i) frame();
    client.pushInput(glm::vec3(1000.0f, 0, 0), 0.0f, 0.1f);
    for (int i = 0; i < 4; ++i) frame();
    REQUIRE(svc.level(conn) == Level::Confirmed);

    // Sanity first: the run has to have logged something, or the test would
    // pass simply because the category was silent.
    CHECK(spy.count() >= 2);
    CHECK(spy.mentions("Displacement"));
    CHECK(spy.mentions("Confirmed"));
    // The label is what the game chose to show; it is meant to be there.
    CHECK(spy.mentions("Alice"));
    // The secret is not, and the service has no path by which it could be.
    CHECK_FALSE(spy.mentions(secret));
}

// ─── Step 4: events and responses (docs/anti-cheat-plan.md §5) ──────────────
// The host takes the reports the service makes, fires them, and executes the
// policy — or what a handler put in its place — at the FRAME END. Everything
// below runs on the same loopback rig, with an AntiCheatHost on each side: the
// server's pumps the service, the client's pumps the notices.

using HE::AntiCheat::AntiCheatHost;
using HE::AntiCheat::Report;
using HE::AntiCheat::Response;

namespace {

// One host frame with the two AntiCheatHosts in it, in the order the frame
// loop keeps: deliver, simulate, pump (events), …game…, flush (responses).
struct HostedRig {
    std::unique_ptr<Rig> rig;
    AntiCheatHost server, client;
    std::vector<int>    firedOnServer;   // report ids the server's sink saw
    std::vector<int>    firedOnClient;   // tickets the client's sink saw
    std::vector<Report> seenOnServer;

    explicit HostedRig(RigOptions opt = {}) : rig(makeRig(opt)) {
        server.attach(&rig->svc, rig->server.get());
        client.attach(nullptr, rig->client.get());
        server.setEventSink([this](int id, const Report& r) {
            firedOnServer.push_back(id);
            seenOnServer.push_back(r);
        });
        client.setEventSink([this](int id, const Report&) { firedOnClient.push_back(id); });
    }
    void frame(float dt = kFrame) {
        rig->frame(dt);
        server.pump();
        client.pump();
        server.flush();
        client.flush();
    }
    void frames(int n) { for (int i = 0; i < n; ++i) frame(); }
    bool serverHasPeer() const { return !rig->serverNet->connections().empty(); }
};

} // namespace

TEST_CASE("AntiCheatHost: a Hard report fires the event, and the policy kick lands at the frame end")
{
    RigOptions opt;
    opt.assignControl = false;
    HostedRig h(opt);
    const ConnectionId conn = LoopbackTransport::kPeer;

    // The event fires in pump(), before any response: the handler's window.
    h.rig->sendRaw(1, 0.1f, glm::vec3(50.0f, 0, 0));
    h.rig->frame();
    CHECK(h.server.pump() == 1);
    REQUIRE(h.firedOnServer.size() == 1);
    const int id = h.firedOnServer[0];
    CHECK(h.server.reportLevel(id) == (int)Level::Hard);
    CHECK(h.server.reportPlayer(id) == (int)conn);
    CHECK(h.server.reportRule(id) == "ForeignEntity");
    CHECK(h.seenOnServer[0].level == Level::Hard);
    // Nothing has happened to the connection yet — not in pump, not until flush.
    CHECK(h.serverHasPeer());
    CHECK(h.rig->server->stats().noticesSent == 0);

    // Flush: the notice leaves now, the link closes on the NEXT flush, so the
    // notice has a frame to get out before the socket does.
    h.server.flush();
    CHECK(h.rig->server->stats().noticesSent == 1);
    CHECK(h.serverHasPeer());
    CHECK(h.server.stats().kicked == 0);

    // The client sees the notice BEFORE the link drops, as a ticket of its own
    // with the rule's name and nothing else about the heuristics.
    h.rig->frame();
    CHECK(h.client.pump() == 1);
    REQUIRE(h.firedOnClient.size() == 1);
    const int ticket = h.firedOnClient[0];
    CHECK(h.client.reportLevel(ticket) == (int)Level::Hard);
    CHECK(h.client.reportRule(ticket) == "ForeignEntity");
    CHECK(h.client.reportDetail(ticket) == "ForeignEntity");   // no observation list
    CHECK(h.client.reportPlayer(ticket) == 0);                 // "you"
    CHECK(h.client.reportScore(ticket) == doctest::Approx(0.0f));
    CHECK(h.client.reportReason(ticket) == HE::AntiCheat::kReasonPolicy);
    CHECK(h.rig->clientNet->connections().size() == 1);

    h.server.flush();
    CHECK_FALSE(h.serverHasPeer());
    CHECK(h.server.stats().kicked == 1);
    // The service forgot the connection with it: the state must not outlive
    // the peer, or a reused id would inherit a Hard level.
    CHECK(h.rig->svc.level(conn) == Level::Info);
    h.rig->frame();
    CHECK(h.rig->clientNet->connections().empty());
}

TEST_CASE("AntiCheatHost: respond() in the handler replaces the policy, and a frame late it is a no-op")
{
    RigOptions opt;
    opt.assignControl = false;
    HostedRig h(opt);
    const ConnectionId conn = LoopbackTransport::kPeer;

    // The handler overrules Hard's kick with "flag only" — the plan's observation
    // mode for one report. It has to be called INSIDE the event.
    bool answered = false;
    h.server.setEventSink([&](int id, const Report&) {
        answered = h.server.respond(id, Response::Flag);
    });
    h.rig->sendRaw(1, 0.1f, glm::vec3(50.0f, 0, 0));
    h.frames(3);
    CHECK(answered);
    CHECK(h.serverHasPeer());
    CHECK(h.rig->server->stats().noticesSent == 0);
    CHECK(h.server.isFlagged(conn));
    CHECK(h.server.stats().responded == 1);
    CHECK(h.server.stats().kicked == 0);

    // A frame later the report is no longer pending: the answer is refused
    // rather than kicking somebody the handler already spared.
    int id = 0;
    for (int i = 1; i < 64 && id == 0; ++i)
        if (h.rig->svc.findReport(i)) id = i;
    REQUIRE(id != 0);
    CHECK_FALSE(h.server.respond(id, Response::Kick));
    h.frames(2);
    CHECK(h.serverHasPeer());
}

TEST_CASE("AntiCheatHost: an explicit kick carries the game's reason code to the client")
{
    HostedRig h;
    const ConnectionId conn = LoopbackTransport::kPeer;

    h.rig->svc.setPlayerLabel(conn, "Mallory");
    h.server.kick(conn, 42);
    // Still a frame-end kick, even though nobody fired an event for it.
    CHECK(h.serverHasPeer());
    h.frame();                         // notice out
    CHECK(h.rig->server->stats().noticesSent == 1);
    CHECK(h.serverHasPeer());
    h.frame();                         // client reads it; server drops the link
    REQUIRE(h.firedOnClient.size() == 1);
    const int ticket = h.firedOnClient[0];
    CHECK(h.client.reportReason(ticket) == 42);
    CHECK(h.client.reportRule(ticket).empty());
    CHECK(h.client.reportLevel(ticket) == (int)Level::Info);
    CHECK_FALSE(h.serverHasPeer());
    // The server fired nothing: an explicit kick is the game's own decision.
    CHECK(h.firedOnServer.empty());
}

TEST_CASE("AntiCheatHost: preview keeps everyone in the session, and a ban remembers the label")
{
    RigOptions opt;
    opt.assignControl = false;
    HostedRig h(opt);
    const ConnectionId conn = LoopbackTransport::kPeer;

    h.server.setPreview(true);
    h.rig->sendRaw(1, 0.1f, glm::vec3(50.0f, 0, 0));
    h.frames(4);
    // The event fired — the author sees the report — but nothing left the
    // machine and nobody was dropped.
    CHECK(h.firedOnServer.size() == 1);
    CHECK(h.serverHasPeer());
    CHECK(h.rig->server->stats().noticesSent == 0);
    CHECK(h.server.stats().kicked == 0);
    h.server.kick(conn, 1);
    h.frames(2);
    CHECK(h.serverHasPeer());

    // Out of preview: a Ban policy on Hard kicks and refuses the label for the
    // rest of the session — the one identity a session has.
    h.server.setPreview(false);
    HE::AntiCheat::Policy p;
    p.hard = Response::Log | Response::Ban;
    h.server.setPolicy(p);
    // Hard is sticky, so a second foreign input would make no second report:
    // drop the connection's state first, as a fresh join would.
    h.rig->svc.forgetConnection(conn);
    h.rig->svc.setPlayerLabel(conn, "Mallory");
    h.rig->sendRaw(2, 0.1f, glm::vec3(50.0f, 0, 0));
    h.frames(3);
    CHECK_FALSE(h.serverHasPeer());
    CHECK(h.server.isBanned("Mallory"));
    CHECK_FALSE(h.server.isBanned("Alice"));
    CHECK(h.server.stats().banned == 1);
}

TEST_CASE("AntiCheatHost: telemetry is queued at the frame end for the sink of a later step")
{
    RigOptions opt;
    opt.assignControl = false;
    HostedRig h(opt);
    h.rig->sendRaw(1, 0.1f, glm::vec3(50.0f, 0, 0));
    h.frames(2);
    Report r;
    REQUIRE(h.server.takeTelemetry(r));
    CHECK(r.level == Level::Hard);
    CHECK(r.rule == "ForeignEntity");
    CHECK_FALSE(h.server.takeTelemetry(r));
}

TEST_CASE("AntiCheatHost: the EventBus gets the same fields the readers answer")
{
    RigOptions opt;
    opt.assignControl = false;
    HostedRig h(opt);
    EventBus bus;
    h.server.setEventBus(&bus);
    std::vector<HE::AntiCheat::CheatDetected> seen;
    auto sub = bus.subscribe<HE::AntiCheat::CheatDetected>(
        [&](const HE::AntiCheat::CheatDetected& e) { seen.push_back(e); });

    h.rig->svc.setPlayerLabel(LoopbackTransport::kPeer, "Mallory");
    h.rig->sendRaw(1, 0.1f, glm::vec3(50.0f, 0, 0));
    h.frames(2);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].reportId == h.firedOnServer.at(0));
    CHECK(seen[0].level == Level::Hard);
    CHECK(seen[0].player == LoopbackTransport::kPeer);
    CHECK(seen[0].rule == "ForeignEntity");
    CHECK(seen[0].label == "Mallory");
    CHECK_FALSE(seen[0].local);
    CHECK(seen[0].detail == h.server.reportDetail(seen[0].reportId));
}

TEST_CASE("AntiCheatHost: without a service every row is neutral, and check passes")
{
    AntiCheatHost host;
    CHECK_FALSE(host.isEnabled());
    // The one inverted default: a check the engine cannot make must not block
    // the game, so "no service" answers true where every other row answers 0.
    CHECK(host.check("Damage", 50.0f, 1));
    host.expectDisplacement(1, 10.0f);
    host.report(1, "Sight", 5.0f, "");
    host.setPlayerLabel(1, "x");
    host.kick(1, 7);
    CHECK_FALSE(host.respond(1, Response::Kick));
    CHECK(host.reportLevel(1) == 0);
    CHECK(host.reportRule(1).empty());
    CHECK(host.reportPlayer(1) == 0);
    CHECK(host.reportEntity(1) == 0);
    CHECK(host.reportScore(1) == doctest::Approx(0.0f));
    CHECK(host.reportDetail(1).empty());
    CHECK(host.playerScore(1) == doctest::Approx(0.0f));
    CHECK(host.pump() == 0);
    host.flush();
    CHECK(host.stats().kicked == 0);
}

TEST_CASE("AntiCheatHost: the project settings become the service's knobs and the policy")
{
    HE::ProjectAntiCheatSettings s;
    s.tolerance          = 0.25f;
    s.windowSec          = 4.0f;
    s.maxInputsPerSecond = 120;
    s.scoreHalfLifeSec   = 10.0f;
    s.scoreSuspect       = 3.0f;
    s.scoreConfirmed     = 9.0f;
    s.policySuspect      = HE::ProjectAntiCheatSettings::Log;
    s.policyConfirmed    = HE::ProjectAntiCheatSettings::Log | HE::ProjectAntiCheatSettings::Event
                         | HE::ProjectAntiCheatSettings::Kick;
    s.policyHard         = HE::ProjectAntiCheatSettings::Log | HE::ProjectAntiCheatSettings::Ban;

    const HE::AntiCheat::Config c = AntiCheatHost::configFrom(s);
    CHECK(c.tolerance == doctest::Approx(0.25f));
    CHECK(c.windowSec == doctest::Approx(4.0f));
    CHECK(c.maxInputsPerSecond == doctest::Approx(120.0f));
    CHECK(c.halfLifeSec == doctest::Approx(10.0f));
    CHECK(c.suspectThreshold == doctest::Approx(3.0f));
    CHECK(c.confirmedThreshold == doctest::Approx(9.0f));

    const HE::AntiCheat::Policy p = AntiCheatHost::policyFrom(s);
    CHECK(p.forLevel(Level::Suspect)   == (Response::Log));
    CHECK(p.forLevel(Level::Confirmed) == (Response::Log | Response::Event | Response::Kick));
    CHECK(p.forLevel(Level::Hard)      == (Response::Log | Response::Ban));
    // Log can never be taken out of a set: a level change is always a line.
    HE::AntiCheat::Policy none;
    none.suspect = 0;
    CHECK(none.forLevel(Level::Suspect) == Response::Log);
}
