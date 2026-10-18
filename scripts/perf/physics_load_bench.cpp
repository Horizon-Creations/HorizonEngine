// physics_load_bench: Jolt driven exactly the way PhysicsWorld::Impl drives it, without the engine.
// Pure measurement tool for Thema 178 (physics threading), step 1. Nothing in the engine links against it.
//
//   scripts/perf/physics_load_bench.sh build            (compiles this file against the engine's libJolt.a)
//   /tmp/he178/physics_load_bench --scene bounce --bodies 5000 --workers 4 --steps 300
//
// What it copies from src/HE_Scene/src/PhysicsWorld.cpp (read there before trusting a number here):
//   - PhysicsSystem::Init(kMaxBodies, 0, 65536 pairs, 10240 contact constraints)
//   - JobSystemThreadPool(cMaxPhysicsJobs, cMaxPhysicsBarriers, clamp(hw/2-1, 0, 4))  (--workers overrides)
//   - Update(dt = 1/60, collisionSteps = 1, tempAllocator = 10 MB TempAllocatorImpl, jobSystem)
//   - the contact listener's locking: one std::mutex taken in OnContactValidate, OnContactAdded and
//     OnContactRemoved, ref-counted active-contact map, entered/exited event vectors (--listener he|none)
//   - CharacterVirtual::ExtendedUpdate with the same settings shape (stairs step up), serial per character
//   - NarrowPhaseQuery::CastRay for the raycast cost
// What it does NOT copy: the entity <-> body write-back loop and the world <-> local pose conversion
// of PhysicsWorld::step (those are in the engine and show in the real-loop PhysicsStep scope), HE's
// object-layer matrix (two layers here), the engine's own thread pool running alongside.
//
// The temp allocator: the engine's TempAllocatorImpl ABORTS the process when a step needs more than its
// 10 MB (Jolt 5.5 TempAllocator.h, "Out of memory trying to allocate", std::abort). So this bench
// wraps a big one and records the PEAK; "tempPeak > 10 MB" means the real thing would have died.
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <mach/mach.h>
#include <mach/thread_info.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// ── layers: NON_MOVING (floor) / MOVING (everything else), like the engine's two broad-phase layers ──
namespace L { static constexpr JPH::ObjectLayer Static = 0, Moving = 1; }
class BPI final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return JPH::BroadPhaseLayer(l == L::Static ? 0 : 1); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "bp"; }
#endif
};
class OVBP final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer o, JPH::BroadPhaseLayer bp) const override { return o == L::Moving ? true : bp == JPH::BroadPhaseLayer(1); }
};
class OOP final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return !(a == L::Static && b == L::Static); }
};

// ── the engine's contact listener, locking pattern only ─────────────────────────────────────────
class HeListener final : public JPH::ContactListener {
public:
    JPH::ValidateResult OnContactValidate(const JPH::Body& b1, const JPH::Body& b2, JPH::RVec3Arg, const JPH::CollideShapeResult&) override {
        { std::lock_guard<std::mutex> lock(m_mutex);
          if (!m_noCollide.empty() && m_noCollide.count(key(b1.GetID(), b2.GetID()))) return JPH::ValidateResult::RejectAllContactsForThisBodyPair; }
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }
    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold&, JPH::ContactSettings&) override {
        Ev ev{ static_cast<uint32_t>(b1.GetUserData()), static_cast<uint32_t>(b2.GetUserData()) };
        const bool overlap = b1.IsSensor() || b2.IsSensor();
        std::lock_guard<std::mutex> lock(m_mutex);
        Active& c = m_active[key(b1.GetID(), b2.GetID())];
        if (c.ref++ == 0) { c.ev = ev; c.overlap = overlap; (overlap ? m_enteredOv : m_entered).push_back(ev); }
        ++added;
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& p) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_active.find(key(p.GetBody1ID(), p.GetBody2ID()));
        if (it == m_active.end()) return;
        if (--it->second.ref == 0) { (it->second.overlap ? m_exitedOv : m_exited).push_back(it->second.ev); m_active.erase(it); }
        ++removed;
    }
    // what CollisionSystem::dispatch does once a frame: drain + sort
    size_t drain() {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t n = m_entered.size() + m_exited.size() + m_enteredOv.size() + m_exitedOv.size();
        for (auto* v : { &m_entered, &m_exited, &m_enteredOv, &m_exitedOv }) {
            std::sort(v->begin(), v->end(), [](const Ev& a, const Ev& b) { return a.a != b.a ? a.a < b.a : a.b < b.b; });
            v->clear();
        }
        return n;
    }
    size_t activeContacts() { std::lock_guard<std::mutex> lock(m_mutex); return m_active.size(); }
    uint64_t added = 0, removed = 0;
private:
    struct Ev { uint32_t a, b; };
    struct Active { uint32_t ref = 0; Ev ev{}; bool overlap = false; };
    static uint64_t key(JPH::BodyID a, JPH::BodyID b) {
        uint32_t lo = a.GetIndexAndSequenceNumber(), hi = b.GetIndexAndSequenceNumber(); if (lo > hi) std::swap(lo, hi);
        return (uint64_t(lo) << 32) | hi; }
    std::mutex m_mutex;
    std::unordered_map<uint64_t, Active> m_active;
    std::unordered_set<uint64_t> m_noCollide;
    std::vector<Ev> m_entered, m_exited, m_enteredOv, m_exitedOv;
};

// ── a temp allocator that records its peak (the engine's real one aborts instead) ───────────────
class PeakTemp final : public JPH::TempAllocator {
public:
    explicit PeakTemp(size_t bytes) : m_impl(bytes) {}
    void* Allocate(JPH::uint inSize) override { void* p = m_impl.Allocate(inSize); peak = std::max(peak, m_impl.GetUsage()); return p; }
    void Free(void* a, JPH::uint s) override { m_impl.Free(a, s); }
    size_t peak = 0;
private:
    JPH::TempAllocatorImpl m_impl;
};

// ── per-thread CPU time (Mach), the only way to see Jolt's workers from inside the process ───────
struct ThreadCpu { std::string name; double cpuMs; };
static uint64_t g_mainTid = 0;   // Jolt names its workers only on Windows/Linux; on macOS they are "(unnamed)", the main thread is told apart by id
static std::map<uint64_t, ThreadCpu> threadCpu() {
    std::map<uint64_t, ThreadCpu> out;
    thread_act_array_t ths; mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &ths, &n) != KERN_SUCCESS) return out;
    for (mach_msg_type_number_t i = 0; i < n; ++i) {
        thread_identifier_info_data_t id; mach_msg_type_number_t ic = THREAD_IDENTIFIER_INFO_COUNT;
        thread_basic_info_data_t bi; mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;
        thread_extended_info_data_t ei; mach_msg_type_number_t ec = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(ths[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &ic) != KERN_SUCCESS) continue;
        if (thread_info(ths[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &bc) != KERN_SUCCESS) continue;
        std::string name;
        if (thread_info(ths[i], THREAD_EXTENDED_INFO, (thread_info_t)&ei, &ec) == KERN_SUCCESS) name = ei.pth_name;
        double cpu = (bi.user_time.seconds + bi.system_time.seconds) * 1e3 + (bi.user_time.microseconds + bi.system_time.microseconds) / 1e3;
        out[id.thread_id] = { id.thread_id == g_mainTid ? "main" : name.empty() ? "jolt-worker?" : name, cpu };
        mach_port_deallocate(mach_task_self(), ths[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)ths, n * sizeof(thread_t));
    return out;
}

static double pct(std::vector<double> v, double p) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, size_t(p * (v.size() - 1) + 0.5))]; }

struct Args {
    std::string scene = "bounce";   // bounce | pile
    std::string listener = "he";    // he | none
    int bodies = 5000, characters = 0, rays = 0, workers = -1, steps = 300, warmup = 120;
    uint32_t maxPairs = 65536, maxContacts = 10240;   // the engine's Impl::kMaxBodyPairs / kMaxContactConstraints
    int contend = 0;                // K threads spinning flat out next to the simulation: the engine pool when every job runs at once
    bool sleeping = false;          // pile only: let the pile fall asleep (the steady state of a real game)
    int tempMB = 10;                // the cap the engine has; peak is recorded either way
    unsigned seed = 1;
};

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        auto is = [&](const char* k) { return std::strcmp(argv[i], k) == 0 && i + 1 < argc; };
        if (is("--scene")) a.scene = argv[++i]; else if (is("--listener")) a.listener = argv[++i];
        else if (is("--bodies")) a.bodies = atoi(argv[++i]); else if (is("--characters")) a.characters = atoi(argv[++i]);
        else if (is("--rays")) a.rays = atoi(argv[++i]); else if (is("--workers")) a.workers = atoi(argv[++i]);
        else if (is("--steps")) a.steps = atoi(argv[++i]); else if (is("--warmup")) a.warmup = atoi(argv[++i]);
        else if (is("--max-pairs")) a.maxPairs = uint32_t(atoi(argv[++i]));
        else if (is("--max-contacts")) a.maxContacts = uint32_t(atoi(argv[++i]));
        else if (is("--contend")) a.contend = atoi(argv[++i]);
        else if (is("--temp-mb")) a.tempMB = atoi(argv[++i]); else if (is("--seed")) a.seed = unsigned(atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--sleeping") == 0) a.sleeping = true;
        else { std::fprintf(stderr, "unknown/incomplete arg %s\n", argv[i]); return 2; }
    }

    pthread_threadid_np(nullptr, &g_mainTid);
    std::atomic<bool> stopSpin{false};
    std::vector<std::thread> spinners;
    for (int k = 0; k < a.contend; ++k)
        spinners.emplace_back([&stopSpin, k] { volatile double x = 1.0 + k; while (!stopSpin.load(std::memory_order_relaxed)) for (int i = 0; i < 20000; ++i) x = x * 1.0000001 + 1e-9; });
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    // exactly Impl's numbers
    constexpr uint32_t kMaxBodies = 65536;
    const uint32_t kMaxPairs = a.maxPairs, kMaxContacts = a.maxContacts;
    const int hw = int(std::thread::hardware_concurrency());
    const int workers = a.workers >= 0 ? a.workers : std::clamp(hw / 2 - 1, 0, 4);
    JPH::JobSystemThreadPool jobs;
    jobs.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workers);
    BPI bpi; OVBP ovbp; OOP oop;
    JPH::PhysicsSystem sys;
    sys.Init(kMaxBodies, 0, kMaxPairs, kMaxContacts, bpi, ovbp, oop);
    HeListener listener;
    if (a.listener == "he") sys.SetContactListener(&listener);
    PeakTemp temp(size_t(512) * 1024 * 1024);
    JPH::BodyInterface& bi = sys.GetBodyInterface();

    // floor, restitution high enough that a dropped body keeps bouncing for the whole run
    JPH::BodyCreationSettings floor(new JPH::BoxShape(JPH::Vec3(2000, 1, 2000)), JPH::RVec3(0, -1, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, L::Static);
    floor.mRestitution = 0.95f; floor.mFriction = 0.5f;
    bi.CreateAndAddBody(floor, JPH::EActivation::DontActivate);

    std::mt19937 rng(a.seed);
    std::uniform_real_distribution<float> u01(0.f, 1.f);
    int side = std::max(1, int(std::ceil(std::sqrt(double(a.bodies)))));
    int created = 0;
    JPH::RefConst<JPH::Shape> sphere = new JPH::SphereShape(0.5f), box = new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f));
    if (a.scene == "bounce") {
        // spread out on a 3 m grid, dropped from 4..24 m: bodies seldom touch each other, so this is the
        // "many active bodies, few contacts" end (the cost of N bodies, not of a pile)
        for (int i = 0; i < a.bodies; ++i) {
            JPH::BodyCreationSettings s(i % 2 ? sphere : box, JPH::RVec3((i % side - side / 2) * 3.f, 4.f + u01(rng) * 20.f, (i / side - side / 2) * 3.f),
                                        JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, L::Moving);
            s.mRestitution = 0.9f; s.mFriction = 0.5f; s.mUserData = uint64_t(i + 1); s.mAllowSleeping = false;
            if (!bi.CreateAndAddBody(s, JPH::EActivation::Activate).IsInvalid()) ++created;
        }
    } else {
        // "pile": towers of unit boxes on a 1.05 m grid, 10 high, slightly overlapping spheres of influence -
        // the "many bodies in contact" end. sleeping off by default (worst case), --sleeping shows the settled game
        const int towerH = 10, towers = (a.bodies + towerH - 1) / towerH, tside = std::max(1, int(std::ceil(std::sqrt(double(towers)))));
        for (int i = 0; i < a.bodies; ++i) {
            const int t = i / towerH, h = i % towerH;
            JPH::BodyCreationSettings s(box, JPH::RVec3((t % tside - tside / 2) * 1.05f, 0.5f + h * 1.001f, (t / tside - tside / 2) * 1.05f),
                                        JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, L::Moving);
            s.mRestitution = 0.1f; s.mFriction = 0.6f; s.mUserData = uint64_t(i + 1); s.mAllowSleeping = a.sleeping;
            if (!bi.CreateAndAddBody(s, JPH::EActivation::Activate).IsInvalid()) ++created;
        }
    }

    // characters, standing in the middle of the field, walking in circles (so ExtendedUpdate has real work)
    std::vector<JPH::Ref<JPH::CharacterVirtual>> chars;
    for (int c = 0; c < a.characters; ++c) {
        JPH::Ref<JPH::CharacterVirtualSettings> cs = new JPH::CharacterVirtualSettings();
        cs->mShape = JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0, 0.9f, 0), JPH::Quat::sIdentity(), new JPH::CapsuleShape(0.5f, 0.4f)).Create().Get();
        cs->mMass = 70.f; cs->mMaxSlopeAngle = JPH::DegreesToRadians(50.f); cs->mUp = JPH::Vec3::sAxisY();
        cs->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -0.4f);
        float ang = float(c) * 0.37f, r = 4.f + float(c % 40);
        chars.push_back(new JPH::CharacterVirtual(cs, JPH::RVec3(std::cos(ang) * r, 0.5f, std::sin(ang) * r), JPH::Quat::sIdentity(), 0, &sys));
    }

    const float dt = 1.0f / 60.0f;
    JPH::CharacterVirtual::ExtendedUpdateSettings eu; eu.mWalkStairsStepUp = JPH::Vec3(0, 0.4f, 0);
    JPH::DefaultBroadPhaseLayerFilter bpf = sys.GetDefaultBroadPhaseLayerFilter(L::Moving);
    JPH::DefaultObjectLayerFilter olf = sys.GetDefaultLayerFilter(L::Moving);

    std::vector<double> updMs, charMs, rayMs, totalMs, drainMs;
    JPH::EPhysicsUpdateError errs = JPH::EPhysicsUpdateError::None;
    int stepsWithErrors = 0; size_t maxActive = 0;
    std::map<uint64_t, ThreadCpu> cpu0; Clock::time_point wall0;

    const int total = a.warmup + a.steps;
    for (int s = 0; s < total; ++s) {
        if (s == a.warmup) { cpu0 = threadCpu(); wall0 = Clock::now(); temp.peak = 0; errs = JPH::EPhysicsUpdateError::None; stepsWithErrors = 0; }
        const bool measure = s >= a.warmup;
        auto t0 = Clock::now();
        JPH::EPhysicsUpdateError e = sys.Update(dt, 1, &temp, &jobs);
        double u = msSince(t0);
        if (e != JPH::EPhysicsUpdateError::None && measure) { errs = errs | e; ++stepsWithErrors; }

        auto t1 = Clock::now();
        for (size_t c = 0; c < chars.size(); ++c) {
            JPH::Vec3 v(std::cos(s * 0.05f + c) * 3.f, chars[c]->IsSupported() ? 0.f : chars[c]->GetLinearVelocity().GetY() - 9.81f * dt, std::sin(s * 0.05f + c) * 3.f);
            chars[c]->SetLinearVelocity(v);
            chars[c]->ExtendedUpdate(dt, JPH::Vec3(0, -9.81f, 0), eu, bpf, olf, JPH::BodyFilter(), JPH::ShapeFilter(), temp);
        }
        double cm = msSince(t1);

        auto t2 = Clock::now();
        int hits = 0;
        for (int r = 0; r < a.rays; ++r) {
            JPH::RRayCast ray{ JPH::RVec3((u01(rng) - 0.5f) * side * 3.f, 40.f, (u01(rng) - 0.5f) * side * 3.f), JPH::Vec3(0, -80.f, 0) };
            JPH::RayCastResult hit;
            if (sys.GetNarrowPhaseQuery().CastRay(ray, hit, bpf, olf, JPH::BodyFilter())) ++hits;
        }
        double rm = msSince(t2);

        auto t3 = Clock::now();
        if (a.listener == "he") listener.drain();
        double dm = msSince(t3);

        if (measure) {
            updMs.push_back(u); charMs.push_back(cm); rayMs.push_back(rm); drainMs.push_back(dm); totalMs.push_back(u + cm + rm + dm);
            maxActive = std::max(maxActive, size_t(sys.GetNumActiveBodies(JPH::EBodyType::RigidBody)));
        }
        (void)hits;
    }
    double wall = msSince(wall0);
    auto cpu1 = threadCpu();

    std::printf("scene=%s listener=%s bodies=%d(created %d) chars=%d rays/step=%d workers=%d hw=%d sleeping=%d steps=%d contend=%d\n",
                a.scene.c_str(), a.listener.c_str(), a.bodies, created, a.characters, a.rays, workers, hw, a.sleeping, a.steps, a.contend);
    std::printf("  Update ms      p50 %.3f  p95 %.3f  max %.3f  mean %.3f\n", pct(updMs, .5), pct(updMs, .95), pct(updMs, 1.0), std::accumulate(updMs.begin(), updMs.end(), 0.0) / updMs.size());
    if (a.characters) std::printf("  ExtendedUpdate ms(all %d chars) p50 %.3f  p95 %.3f  max %.3f  -> %.1f us/char\n", a.characters, pct(charMs, .5), pct(charMs, .95), pct(charMs, 1.0), pct(charMs, .5) * 1000.0 / a.characters);
    if (a.rays) std::printf("  CastRay ms(%d rays) p50 %.3f  p95 %.3f  max %.3f  -> %.2f us/ray\n", a.rays, pct(rayMs, .5), pct(rayMs, .95), pct(rayMs, 1.0), pct(rayMs, .5) * 1000.0 / a.rays);
    if (a.listener == "he") std::printf("  listener drain ms p50 %.4f max %.4f | contacts added %llu removed %llu active-now %zu\n", pct(drainMs, .5), pct(drainMs, 1.0), (unsigned long long)listener.added, (unsigned long long)listener.removed, listener.activeContacts());
    std::printf("  total physics ms p50 %.3f p95 %.3f max %.3f  (60 Hz budget 16.667)\n", pct(totalMs, .5), pct(totalMs, .95), pct(totalMs, 1.0));
    std::printf("  active bodies at end %u\n", unsigned(sys.GetNumActiveBodies(JPH::EBodyType::RigidBody)));
    std::printf("  active bodies max %zu | temp peak %.2f MB of %d MB (engine aborts above) %s\n", maxActive, temp.peak / 1048576.0, a.tempMB, temp.peak > size_t(a.tempMB) * 1048576 ? "** WOULD ABORT **" : "ok");
    std::printf("  update errors: stepsWithErrors=%d flags=%s%s%s\n", stepsWithErrors,
                (uint32_t(errs) & uint32_t(JPH::EPhysicsUpdateError::ManifoldCacheFull)) ? "ManifoldCacheFull " : "",
                (uint32_t(errs) & uint32_t(JPH::EPhysicsUpdateError::BodyPairCacheFull)) ? "BodyPairCacheFull " : "",
                (uint32_t(errs) & uint32_t(JPH::EPhysicsUpdateError::ContactConstraintsFull)) ? "ContactConstraintsFull" : "");
    double sumCpu = 0;
    std::printf("  thread CPU over %.0f ms wall (utilisation = cpu/wall):\n", wall);
    for (auto& kv : cpu1) {
        auto it = cpu0.find(kv.first);
        double d = kv.second.cpuMs - (it == cpu0.end() ? 0.0 : it->second.cpuMs);
        if (d < 0.5) continue;
        sumCpu += d;
        std::printf("    %-14s %7.0f ms  %5.1f %%\n", kv.second.name.c_str(), d, 100.0 * d / wall);
    }
    std::printf("  all threads: %.0f ms cpu = %.2f cores busy on average\n", sumCpu, sumCpu / wall);
    chars.clear();
    stopSpin = true;
    for (auto& t : spinners) t.join();
    return 0;
}
