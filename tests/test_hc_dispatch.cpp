#include "doctest.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <HorizonCode/HcClassResolve.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#endif

using namespace HorizonCode;

// ═══ Interpreter dispatch (Thema 213, CORE-302/303/401/402) ══════════════════
// Every event, function call and spawn of an interpreted instance used to pay
// for things that never change while the instance lives: a fresh Context of
// some forty std::functions, a scan of every node of every level for the
// handler, and on spawn a JSON parse and merge of the whole class chain. This
// file holds the measurement that showed it, and the guard that the dispatch
// still picks exactly the level, the element and the access the linear search
// picked.

namespace
{
    // ── Allocation counter ────────────────────────────────────────────────────
    // libmalloc calls `malloc_logger` on every allocation in the process, from
    // every image: HorizonCore is a dylib, and a replaced operator new in this
    // executable would not see what it allocates. Counted on the calling thread
    // only, so a job worker does not land in a frame it has nothing to do with.
    // Elsewhere the counter reports -1 ("not counted"), never a made-up zero.
#if defined(__APPLE__)
    using MallocLogger = void(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t);
    extern "C" MallocLogger* malloc_logger;
    unsigned long long g_allocs = 0;
    pthread_t          g_countThread;
    void countAlloc(uint32_t type, uintptr_t, uintptr_t, uintptr_t, uintptr_t result, uint32_t)
    {
        // 2 = MALLOC_LOG_TYPE_ALLOCATE (realloc carries it too).
        if ((type & 2u) && result && pthread_equal(pthread_self(), g_countThread)) ++g_allocs;
    }
    struct AllocCount
    {
        unsigned long long start;
        AllocCount()  { g_countThread = pthread_self(); start = g_allocs; malloc_logger = &countAlloc; }
        ~AllocCount() { malloc_logger = nullptr; }
        long long sofar() const { return (long long)(g_allocs - start); }
    };
#else
    struct AllocCount { long long sofar() const { return -1; } };
#endif

    struct TempDir
    {
        std::filesystem::path path;
        TempDir()
        {
            path = std::filesystem::temp_directory_path() / "he_test_hc_dispatch";
            std::filesystem::remove_all(path);
            std::filesystem::create_directories(path);
        }
        ~TempDir() { std::filesystem::remove_all(path); }
    };

    void declare(Graph& g, const std::string& name, PinType t)
    {
        Variable v; v.name = name; v.type = t;
        g.variables.push_back(v);
    }

    // entry exec → Set var = true. `entry` is already in the graph; its exec-out
    // is `outPin`.
    void setsBool(Graph& g, int entry, int outPin, const std::string& var)
    {
        Node cb; cb.type = NodeType::ConstBool; cb.f[0] = 1.0f;
        const int c = g.addNode(std::move(cb));
        Node sv; sv.type = NodeType::SetVariable; sv.s = var; sv.propType = PinType::Bool;
        const int s = g.addNode(std::move(sv));
        REQUIRE(g.connect(entry, outPin, s, 0));
        REQUIRE(g.connect(c, 0, s, 2));
    }

    int addEvent(Graph& g, const std::string& name, int elem = 0)
    {
        Node ev; ev.type = NodeType::Event; ev.s = name; ev.elem = elem;
        return g.addNode(std::move(ev));
    }

    int addFunction(Graph& g, const std::string& name, int access)
    {
        Node fe; fe.type = NodeType::FunctionEntry; fe.s = name; fe.access = access;
        return g.addNode(std::move(fe));
    }

    int addInputAction(Graph& g, const std::string& action)
    {
        Node ia; ia.type = NodeType::InputAction; ia.s = action;
        return g.addNode(std::move(ia));
    }

    // `after` exec → Set var = Get var + 1, `count` times in a row. Each link of
    // the chain evaluates four nodes: Set, Add, Get, Const. Returns the last Set.
    int incrementChain(Graph& g, int after, int afterPin, const std::string& var, int count)
    {
        int prev = after, prevPin = afterPin;
        for (int k = 0; k < count; ++k)
        {
            Node get; get.type = NodeType::GetVariable; get.s = var; get.propType = PinType::Int;
            const int gv = g.addNode(std::move(get));
            Node one; one.type = NodeType::ConstInt; one.f[0] = 1.0f;
            const int c = g.addNode(std::move(one));
            Node add; add.type = NodeType::Add;
            const int a = g.addNode(std::move(add));
            Node set; set.type = NodeType::SetVariable; set.s = var; set.propType = PinType::Int;
            const int s = g.addNode(std::move(set));
            REQUIRE(g.connect(gv, 0, a, 0));
            REQUIRE(g.connect(c, 0, a, 1));
            REQUIRE(g.connect(a, 2, s, 2));   // Add has no exec pins: data-out is 2
            REQUIRE(g.connect(prev, prevPin, s, 0));
            prev = s; prevPin = 1;
        }
        return prev;
    }

    // What a grown class carries besides the handler a frame fires: other
    // events, functions and two input actions. `groups` × 4 nodes + 2 actions.
    void filler(Graph& g, const std::string& tag, int groups)
    {
        for (int i = 0; i < groups; ++i)
        {
            setsBool(g, addEvent(g, "On" + tag + std::to_string(i)), 0, "flag");
            addFunction(g, tag + "Fn" + std::to_string(i), 0);
        }
        addInputAction(g, tag + "Jump");
        addInputAction(g, tag + "Fire");
    }

    constexpr int kChain = 8;                 // Set x = x + 1, eight times per Tick
    constexpr int kNodesPerTick = 1 + 4 * kChain;

    // Two levels, the way a derived gameplay class looks: the base declares the
    // Tick and a function, the leaf only adds members of its own. The leaf-first
    // search therefore walks the leaf before it finds the handler.
    std::vector<Graph> tickingClass(bool withTick)
    {
        Graph base;
        declare(base, "x", PinType::Int);
        declare(base, "flag", PinType::Bool);
        filler(base, "Base", 25);
        if (withTick) incrementChain(base, addEvent(base, "Tick"), 0, "x", kChain);
        incrementChain(base, addFunction(base, "Bump", 0), 0, "x", 1);
        Graph leaf;
        filler(leaf, "Leaf", 25);
        std::vector<Graph> levels;
        levels.push_back(std::move(base));
        levels.push_back(std::move(leaf));
        return levels;
    }

    struct Timing { double nsPerFrame; long long allocsPerFrame; };

    // Median of `frames` runs of `frame` (after two warm-up runs). Allocations
    // are counted over one extra run, outside the timed ones.
    template <class F> Timing measure(int frames, F&& frame)
    {
        frame(); frame();
        std::vector<double> ns;
        for (int f = 0; f < frames; ++f)
        {
            const auto t0 = std::chrono::steady_clock::now();
            frame();
            ns.push_back(std::chrono::duration<double, std::nano>(
                std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(ns.begin(), ns.end());
        long long allocs = 0;
        {
            AllocCount count;
            frame();
            allocs = count.sofar();
        }
        return { ns[ns.size() / 2], allocs };
    }

    // The levels as a level-0-is-root graph list, with each level setting its own
    // marker: which level a dispatch picked is then a variable away.
    struct Marked
    {
        Runtime    rt;
        InstanceId id = 0;
        bool hit(const char* var) { return rt.getVariable(id, var).b; }
        void clear()
        {
            for (const char* v : { "root", "mid", "leaf" })
                rt.setVariable(id, v, Value::ofBool(false));
        }
    };
    void addMarkers(Graph& root)
    {
        for (const char* v : { "root", "mid", "leaf" }) declare(root, v, PinType::Bool);
    }
}

// ── Which level answers ──────────────────────────────────────────────────────
// Pinned before the per-instance index replaced the linear leaf-first scan, and
// green against both: the index has to choose what the scan chose, filters and
// all.

TEST_CASE("dispatch: a derived handler replaces the base's, a missing one falls through")
{
    Graph root; addMarkers(root);
    setsBool(root, addEvent(root, "Ping"), 0, "root");
    setsBool(root, addEvent(root, "OnlyBase"), 0, "root");
    Graph leaf;
    setsBool(leaf, addEvent(leaf, "Ping"), 0, "leaf");

    Marked m;
    std::vector<Graph> lv; lv.push_back(std::move(root)); lv.push_back(std::move(leaf));
    m.id = m.rt.addLevels(std::move(lv));

    m.rt.fireEvent(m.id, "Ping");
    CHECK(m.hit("leaf"));
    CHECK_FALSE(m.hit("root"));

    m.clear();
    m.rt.fireEvent(m.id, "OnlyBase");
    CHECK(m.hit("root"));
    CHECK_FALSE(m.hit("leaf"));

    m.clear();
    m.rt.fireEvent(m.id, "Nobody");
    CHECK_FALSE(m.hit("root"));
    CHECK_FALSE(m.hit("leaf"));
}

TEST_CASE("dispatch: an element-bound handler only answers for its element")
{
    // Leaf: Click on element 3. Root: Click on any element. Element 5 is not the
    // leaf's, so the search carries on to the root; element 3 stops at the leaf.
    Graph root; addMarkers(root);
    setsBool(root, addEvent(root, "Click", 0), 0, "root");
    Graph leaf;
    setsBool(leaf, addEvent(leaf, "Click", 3), 0, "leaf");

    Marked m;
    std::vector<Graph> lv; lv.push_back(std::move(root)); lv.push_back(std::move(leaf));
    m.id = m.rt.addLevels(std::move(lv));

    m.rt.fireEvent(m.id, "Click", 5);
    CHECK(m.hit("root"));
    CHECK_FALSE(m.hit("leaf"));

    m.clear();
    m.rt.fireEvent(m.id, "Click", 3);
    CHECK(m.hit("leaf"));
    CHECK_FALSE(m.hit("root"));
}

TEST_CASE("dispatch: an Input Action is a handler like an Event, and overrides like one")
{
    Graph root; addMarkers(root);
    setsBool(root, addInputAction(root, "Jump"), 0, "root");   // 0 = Pressed
    setsBool(root, addInputAction(root, "Fire"), 1, "root");   // 1 = Released
    Graph leaf;
    setsBool(leaf, addInputAction(leaf, "Jump"), 0, "leaf");

    Marked m;
    std::vector<Graph> lv; lv.push_back(std::move(root)); lv.push_back(std::move(leaf));
    m.id = m.rt.addLevels(std::move(lv));

    m.rt.fireEvent(m.id, "Input.Jump.Pressed");
    CHECK(m.hit("leaf"));
    CHECK_FALSE(m.hit("root"));

    m.clear();
    m.rt.fireEvent(m.id, "Input.Fire.Released");
    CHECK(m.hit("root"));

    // The leaf's Jump answers Released too (with nothing wired), so the root's
    // Jump is overridden for both of its names.
    m.clear();
    m.rt.fireEvent(m.id, "Input.Jump.Released");
    CHECK_FALSE(m.hit("root"));
    CHECK_FALSE(m.hit("leaf"));

    // An axis name of an action nobody declared as an axis reaches nothing.
    m.rt.fireEvent(m.id, "Input.Jump.Axis", 0, Value::ofFloat(1.0f));
    CHECK_FALSE(m.hit("root"));
}

TEST_CASE("dispatch: two handlers of one name in one level both run")
{
    Graph g; addMarkers(g);
    setsBool(g, addEvent(g, "Ping"), 0, "root");
    setsBool(g, addEvent(g, "Ping"), 0, "leaf");
    Marked m;
    m.id = m.rt.add(std::move(g));
    m.rt.fireEvent(m.id, "Ping");
    CHECK(m.hit("root"));
    CHECK(m.hit("leaf"));
}

TEST_CASE("dispatch: an inherited call skips a base's private function for a public one")
{
    // Root: public h. Mid: private h. Leaf: Go → Call h (not its own). The call
    // is inherited, so only public members of the other levels are candidates:
    // the mid level's private h is skipped and the root's runs.
    Graph root; addMarkers(root);
    setsBool(root, addFunction(root, "h", 0), 0, "root");
    Graph mid;
    setsBool(mid, addFunction(mid, "h", 1), 0, "mid");
    Graph leaf;
    {
        const int go = addEvent(leaf, "Go");
        Node call; call.type = NodeType::FunctionCall; call.s = "h";
        const int c = leaf.addNode(std::move(call));
        REQUIRE(leaf.connect(go, 0, c, 0));
    }

    Marked m;
    std::vector<Graph> lv;
    lv.push_back(std::move(root)); lv.push_back(std::move(mid)); lv.push_back(std::move(leaf));
    m.id = m.rt.addLevels(std::move(lv));

    m.rt.fireEvent(m.id, "Go");
    CHECK(m.hit("root"));
    CHECK_FALSE(m.hit("mid"));

    // From outside, the search is not public-only: the nearest declaration wins
    // and then decides by its own access. The mid level's private h shadows the
    // root's public one: a host call reaches it, a public-only call is refused.
    m.clear();
    CHECK(m.rt.callFunction(m.id, "h", /*requirePublic=*/false));
    CHECK(m.hit("mid"));
    CHECK_FALSE(m.hit("root"));

    m.clear();
    CHECK_FALSE(m.rt.callFunction(m.id, "h", /*requirePublic=*/true));
    CHECK_FALSE(m.hit("mid"));
    CHECK_FALSE(m.hit("root"));

    CHECK_FALSE(m.rt.callFunction(m.id, "missing", false));
}

TEST_CASE("dispatch: a function's signature is read off its nearest declaration")
{
    Graph root;
    {
        Node fe; fe.type = NodeType::FunctionEntry; fe.s = "f";
        fe.params.push_back(FuncParam{}); fe.params.push_back(FuncParam{});
        fe.runOn = (std::uint8_t)RunOn::Server;
        root.addNode(std::move(fe));
    }
    Graph leaf;
    {
        Node fe; fe.type = NodeType::FunctionEntry; fe.s = "f";
        fe.params.push_back(FuncParam{});
        leaf.addNode(std::move(fe));
    }
    Runtime rt;
    std::vector<Graph> lv; lv.push_back(std::move(root)); lv.push_back(std::move(leaf));
    const InstanceId id = rt.addLevels(std::move(lv));

    const Runtime::FunctionSignature sig = rt.functionSignatureOf(id, "f");
    CHECK(sig.found);
    CHECK(sig.params.size() == 1);
    CHECK(sig.runOn == (std::uint8_t)RunOn::Local);
    CHECK_FALSE(rt.functionSignatureOf(id, "g").found);
}

// ── The measurement ──────────────────────────────────────────────────────────
// The before/after numbers in docs/audit-core-fundamente-2026-10-17.md come from
// this case. Skipped in a normal run (it measures, it does not check, and a
// timing is no assertion): he_tests --source-file=*test_hc_dispatch.cpp --no-skip
TEST_CASE("HorizonCode dispatch: frame cost of 1000 interpreted instances" * doctest::skip())
{
    constexpr int kInstances = 1000;
    constexpr int kFrames    = 31;

    // (a) Tick inherited from the base: a chain of 33 nodes per instance.
    {
        Runtime rt;
        std::vector<InstanceId> ids;
        for (int i = 0; i < kInstances; ++i) ids.push_back(rt.addLevels(tickingClass(true)));
        const Timing t = measure(kFrames, [&] { for (InstanceId id : ids) rt.fireTick(id, 0.016f); });
        const int frames = kFrames + 3;
        CHECK(rt.getVariable(ids.front(), "x").i == frames * kChain);
        std::printf("[hc-dispatch-bench] tick:    %d inst  %.3f ms/frame  %.1f ns/inst  "
                    "%.1f ns/node  %lld allocs/frame\n",
                    kInstances, t.nsPerFrame / 1e6, t.nsPerFrame / kInstances,
                    t.nsPerFrame / (kInstances * (double)kNodesPerTick), t.allocsPerFrame);
    }
    // (b) No Tick handler anywhere: everything a frame costs is the dispatch.
    {
        Runtime rt;
        std::vector<InstanceId> ids;
        for (int i = 0; i < kInstances; ++i) ids.push_back(rt.addLevels(tickingClass(false)));
        const Timing t = measure(kFrames, [&] { for (InstanceId id : ids) rt.fireTick(id, 0.016f); });
        std::printf("[hc-dispatch-bench] no-tick: %d inst  %.3f ms/frame  %.1f ns/inst  "
                    "%lld allocs/frame\n",
                    kInstances, t.nsPerFrame / 1e6, t.nsPerFrame / kInstances, t.allocsPerFrame);
    }
    // (c) A host call of a base function (one Set x = x + 1, five nodes).
    {
        Runtime rt;
        std::vector<InstanceId> ids;
        for (int i = 0; i < kInstances; ++i) ids.push_back(rt.addLevels(tickingClass(false)));
        const Timing t = measure(kFrames, [&]
        { for (InstanceId id : ids) rt.callFunction(id, "Bump", /*requirePublic=*/true); });
        CHECK(rt.getVariable(ids.front(), "x").i == kFrames + 3);
        std::printf("[hc-dispatch-bench] call:    %d inst  %.3f ms/frame  %.1f ns/call  "
                    "%lld allocs/frame\n",
                    kInstances, t.nsPerFrame / 1e6, t.nsPerFrame / kInstances, t.allocsPerFrame);
    }
    // (d) Spawn: what EntityHost::bind does per instance, minus the entity —
    // resolve the class through the content system, register its levels.
    {
        TempDir dir;
        ContentManager cm(dir.path.string());
        std::vector<Graph> lv = tickingClass(true);
        auto write = [&](const char* name, const std::string& base, const Graph& g)
        {
            HorizonCodeClassAsset a;
            a.type      = HE::AssetType::HorizonCodeClass;
            a.name      = name;
            a.path      = std::string(name) + ".hasset";
            a.baseClass = base;
            a.graphJson = toJson(g);
            REQUIRE(cm.saveAsset(a));
            return a.path;
        };
        const std::string basePath = write("BenchBase", "Entity", lv[0]);
        const std::string leafPath = write("BenchLeaf", basePath, lv[1]);

        constexpr int kSpawns = 1000;
        Runtime rt;
        std::vector<InstanceId> ids;
        ids.reserve(kSpawns * 2);
        AllocCount count;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kSpawns; ++i)
        {
            ResolvedClass rc = resolveClassAsset(cm, leafPath);
            const ClassIdentity cls{ leafPath, rc.engineBase, rc.chain };
            ids.push_back(rt.addLevels(std::move(rc.levels), {}, cls));
        }
        const double ns = std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - t0).count();
        const long long allocs = count.sofar();
        CHECK(rt.count() == (size_t)kSpawns);
        rt.fireTick(ids.back(), 0.016f);
        CHECK(rt.getVariable(ids.back(), "x").i == kChain);
        std::printf("[hc-dispatch-bench] spawn:   %d inst  %.3f ms total  %.1f us/spawn  "
                    "%lld allocs/spawn\n",
                    kSpawns, ns / 1e6, ns / kSpawns / 1e3, allocs < 0 ? -1 : allocs / kSpawns);
    }
}
