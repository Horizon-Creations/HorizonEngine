#include "doctest.h"
#include "TestFsUtil.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/AnimationNotify.h>
#include <HorizonScene/AnimationNotifySystem.h>
#include <HorizonScene/AnimationSystem.h>
#include <HorizonScene/AnimationBlendSystem.h>
#include <HorizonScene/AnimationStateMachineSystem.h>
#include <HorizonScene/AnimatorHost.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/AnimatorBlendComponent.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <AnimatorStateMachine/AnimatorStateMachineGraph.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
//  Fixtures
//
//  Nothing here needs physics or a renderer: a notify is a name and a time, and
//  the only machinery under test is which of them a span contains.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

using Kind = HE::AnimationNotifyEvent::Kind;

// One root joint, one child. A skeleton is only needed by the tests that drive a
// whole system; the walk itself never looks at one.
SkeletalMeshAsset makeSkeleton(const HE::UUID& meshId)
{
    SkeletalMeshAsset sma;
    sma.id   = meshId;
    sma.name = "notifySkel";
    const std::array<float, 16> identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    SkeletonJoint root;  root.name  = "Root";  root.parent  = -1; root.inverseBindMatrix = identity;
    SkeletonJoint child; child.name = "Child"; child.parent =  0; child.inverseBindMatrix = identity;
    sma.skeleton.push_back(root);
    sma.skeleton.push_back(child);
    return sma;
}

// A clip that poses nothing: the notify walk reads only `duration` and the list,
// and a keyframe channel would just be noise in these cases.
AnimationClipAsset makeClip(float duration, std::vector<AnimationNotify> notifies)
{
    AnimationClipAsset clip;
    clip.duration = duration;
    clip.name     = "notifyClip";
    clip.notifies = std::move(notifies);
    return clip;
}

// A clip that DOES pose something, for the cases that go through a system: the
// systems need a real channel or they warn and skip.
AnimationClipAsset makePosedClip(float duration, std::vector<AnimationNotify> notifies)
{
    AnimationClipAsset clip = makeClip(duration, std::move(notifies));
    AnimationChannel ch;
    ch.jointIndex = 0;
    ch.path       = AnimPathType::Translation;
    ch.times      = { 0.0f, duration };
    ch.values     = { 0,0,0,  0,0,0 };
    clip.channels.push_back(std::move(ch));
    return clip;
}

// Names in the order they were fired, as one string: "a|b|c". Order is half of
// what these cases are about, and comparing one string says so more plainly than
// three index lookups.
std::string trace(const HE::NotifyQueue& q)
{
    std::string s;
    for (const auto& ev : q)
    {
        if (!s.empty()) s += '|';
        s += ev.name;
        if (ev.kind == Kind::Begin) s += ":begin";
        if (ev.kind == Kind::End)   s += ":end";
    }
    return s;
}

int countOf(const HE::NotifyQueue& q, const char* name)
{
    int n = 0;
    for (const auto& ev : q) if (ev.name == name) ++n;
    return n;
}

struct TempContentDir
{
    fs::path path;
    explicit TempContentDir(const char* name)
    {
        path = fs::temp_directory_path() / name;
        he_test::removeAllQuiet(path);
        fs::create_directories(path);
    }
    ~TempContentDir() { he_test::removeAllQuiet(path); }
};

// A HorizonCode class that appends every notify name it receives to a String
// variable. Sibling of test_collision_system's countingGraph, for a String
// payload instead of an Int one.
HorizonCode::Graph notifyTraceGraph(const char* event, const char* var)
{
    using namespace HorizonCode;
    Graph g;
    Variable v; v.name = var; v.type = PinType::String; g.variables.push_back(v);

    Node ev; ev.type = NodeType::Event; ev.s = event;
    ev.hasArg = true; ev.propType = PinType::String;
    const int e = g.addNode(std::move(ev));

    Node cat; cat.type = NodeType::Concat;
    const int c = g.addNode(std::move(cat));
    Node get; get.type = NodeType::GetVariable; get.s = var; get.propType = PinType::String;
    const int gv = g.addNode(std::move(get));

    Node set; set.type = NodeType::SetVariable; set.s = var; set.propType = PinType::String;
    const int sv = g.addNode(std::move(set));

    REQUIRE(g.connect(gv, 0, c, 0));     // old value
    REQUIRE(g.connect(e,  1, c, 1));     // the event's String arg
    REQUIRE(g.connect(e,  0, sv, 0));    // exec
    REQUIRE(g.connect(c,  2, sv, 2));    // value
    return g;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  1 — storage
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("notifies: a clip's notify list survives a save and a reload")
{
    TempContentDir dir("he_test_notify_chunk");

    {
        ContentManager cm(dir.path.string());
        AnimationClipAsset clip;
        clip.type     = HE::AssetType::AnimationClip;
        clip.name     = "walk";
        clip.path     = "walk.hasset";
        clip.duration = 1.25f;
        clip.notifies = {
            { "FootL", 0.30f, 0.0f },
            { "FootR", 0.80f, 0.0f },
            { "HitWindow", 0.40f, 0.35f },
        };
        REQUIRE(cm.saveAsset(clip));
    }

    {
        ContentManager cm(dir.path.string());
        const HE::UUID id = cm.loadAsset("walk.hasset");
        const AnimationClipAsset* clip = cm.getAnimationClip(id);
        REQUIRE(clip != nullptr);
        REQUIRE(clip->notifies.size() == 3);
        CHECK(clip->notifies[0].name == "FootL");
        CHECK(clip->notifies[0].time == doctest::Approx(0.30f));
        CHECK(clip->notifies[0].duration == 0.0f);
        CHECK(clip->notifies[2].name == "HitWindow");
        CHECK(clip->notifies[2].time == doctest::Approx(0.40f));
        CHECK(clip->notifies[2].duration == doctest::Approx(0.35f));
    }

    // A file written before notifies existed has no ANOT chunk at all. Rebuilt
    // by hand from the very same file, so this is the real layout minus that one
    // chunk rather than a guess at what an old build produced.
    {
        const fs::path file = dir.path / "walk.hasset";
        HAsset::Reader r;
        REQUIRE(r.open(file.string()));
        HAsset::Writer w;
        for (const auto& c : r.chunks())
            if (c.id != HAsset::CHUNK_ANOT) w.addChunk(c.id, c.data.data(), c.data.size());
        const std::vector<uint8_t> bytes = w.toBytes(r.assetType());
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    {
        ContentManager cm(dir.path.string());
        const AnimationClipAsset* clip = cm.getAnimationClip(cm.loadAsset("walk.hasset"));
        REQUIRE(clip != nullptr);
        CHECK(clip->notifies.empty());
        CHECK(clip->duration == doctest::Approx(1.25f));
        // …and the flag that shares the chunk falls back to its default rather
        // than to whatever an uninitialised byte held.
        CHECK(clip->hasRootMotion);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  2-5 — the firing rule, against the walk itself
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("notifies: a timestamp inside the span fires exactly once")
{
    const AnimationClipAsset clip = makeClip(1.0f, { { "Step", 0.50f, 0.0f } });

    // Two consecutive frames around the timestamp: the first ends before it, the
    // second sweeps over it. Not none, and not twice — the span is half-open on
    // the left, so the boundary belongs to exactly one of the two.
    HE::NotifyQueue q;
    HE::collectNotifies(clip, 7, 0.40f, 0.50f, false, q);
    CHECK(trace(q) == "Step");

    q.clear();
    HE::collectNotifies(clip, 7, 0.50f, 0.60f, false, q);
    CHECK(q.empty());

    q.clear();
    HE::collectNotifies(clip, 7, 0.30f, 0.40f, false, q);
    CHECK(q.empty());

    // The entity travels with the event; nothing else does.
    q.clear();
    HE::collectNotifies(clip, 42, 0.45f, 0.55f, false, q);
    REQUIRE(q.size() == 1);
    CHECK(q[0].entity == 42u);
    CHECK(q[0].kind == Kind::Fire);
}

TEST_CASE("notifies: a span running over the loop edge fires what is on the far side")
{
    const AnimationClipAsset clip = makeClip(1.0f, { { "Early", 0.05f, 0.0f },
                                                     { "Late",  0.95f, 0.0f } });

    HE::NotifyQueue q;
    HE::collectNotifies(clip, 1, 0.90f, 1.10f, false, q);
    CHECK(trace(q) == "Late|Early");   // in the order the playhead met them
}

TEST_CASE("notifies: a notify on frame 0 fires on the first frame and once per lap")
{
    // The seam case. In a loop, 0 and `duration` are the same instant, and the
    // convention is that `duration` closes the lap that ends while 0 opens the
    // one that starts. Since a span is open at its origin, a notify at 0 would
    // otherwise never fire in the FIRST lap at all — which is why a playhead's
    // first frame closes its origin edge.
    const AnimationClipAsset clip = makeClip(1.0f, { { "Zero", 0.0f, 0.0f } });

    HE::NotifyQueue q;
    HE::collectNotifies(clip, 1, 0.0f, 0.10f, /*includeStart=*/true, q);
    CHECK(countOf(q, "Zero") == 1);

    // Without the first-frame allowance the same span holds nothing: that is the
    // bug the flag exists for, stated as a check rather than as a comment.
    q.clear();
    HE::collectNotifies(clip, 1, 0.0f, 0.10f, /*includeStart=*/false, q);
    CHECK(q.empty());

    // And afterwards: once per lap, on the frame that crosses the seam, without
    // the allowance ever being needed again.
    q.clear();
    HE::collectNotifies(clip, 1, 0.95f, 1.05f, false, q);
    CHECK(countOf(q, "Zero") == 1);

    q.clear();
    HE::collectNotifies(clip, 1, 0.10f, 0.20f, false, q);
    CHECK(q.empty());
}

TEST_CASE("notifies: a span covering one and a half laps fires the full lap once and the half once")
{
    const AnimationClipAsset clip = makeClip(1.0f, { { "A", 0.10f, 0.0f },
                                                     { "B", 0.50f, 0.0f },
                                                     { "C", 0.90f, 0.0f } });

    // From 0.25 forward by 1.5 laps: A and C are passed once, B twice (at 0.50
    // of the first lap and again at 0.50 of the second).
    HE::NotifyQueue q;
    HE::collectNotifies(clip, 1, 0.25f, 1.75f, false, q);
    CHECK(countOf(q, "A") == 1);
    CHECK(countOf(q, "B") == 2);
    CHECK(countOf(q, "C") == 1);
    CHECK(trace(q) == "B|C|A|B");

    // Two whole laps pass everything exactly twice, which is the same rule with
    // the seam landing on both ends.
    q.clear();
    HE::collectNotifies(clip, 1, 0.25f, 2.25f, false, q);
    CHECK(countOf(q, "A") == 2);
    CHECK(countOf(q, "B") == 2);
    CHECK(countOf(q, "C") == 2);
}

TEST_CASE("notifies: a notify state reports Begin and End, in that order")
{
    const AnimationClipAsset clip = makeClip(1.0f, { { "Window", 0.20f, 0.40f } });

    HE::NotifyQueue q;
    HE::collectNotifies(clip, 1, 0.10f, 0.25f, false, q);
    CHECK(trace(q) == "Window:begin");

    q.clear();
    HE::collectNotifies(clip, 1, 0.25f, 0.50f, false, q);
    CHECK(q.empty());               // inside the window, nothing to report

    q.clear();
    HE::collectNotifies(clip, 1, 0.55f, 0.65f, false, q);
    CHECK(trace(q) == "Window:end");

    // A whole state inside one frame comes out opened then closed, never the
    // other way round.
    q.clear();
    HE::collectNotifies(clip, 1, 0.10f, 0.90f, false, q);
    CHECK(trace(q) == "Window:begin|Window:end");
}

TEST_CASE("notifies: a state authored past the end of its clip ends with the clip")
{
    // 0.8 + 0.5 is 1.3 on a clip that lasts 1.0. Clamped, so it closes at the
    // end; wrapping it into the next lap would give a non-looping clip an End
    // before its Begin.
    const AnimationClipAsset clip = makeClip(1.0f, { { "Long", 0.80f, 0.50f } });

    HE::NotifyQueue q;
    HE::collectNotifies(clip, 1, 0.75f, 1.00f, false, q);
    CHECK(trace(q) == "Long:begin|Long:end");
}

TEST_CASE("notifies: a rewinding span reports the same events in the opposite order")
{
    const AnimationClipAsset clip = makeClip(1.0f, { { "Window", 0.20f, 0.40f },
                                                     { "Step",   0.50f, 0.0f } });

    // Backwards over the whole middle: the End edge sits at the later timestamp,
    // so it is met first. The kinds stay bound to their timestamps — a reversed
    // attack reports the window closing before it opens, because that is the
    // order the playhead passed them, and giving the same data a second meaning
    // for a rewind would be worse than the oddity.
    HE::NotifyQueue q;
    HE::collectNotifies(clip, 1, 0.90f, 0.10f, false, q);
    CHECK(trace(q) == "Window:end|Step|Window:begin");

    // The origin is open going backwards too, and the destination is closed.
    q.clear();
    HE::collectNotifies(clip, 1, 0.50f, 0.30f, false, q);
    CHECK(q.empty());                                  // 0.50 is the open origin
    q.clear();
    HE::collectNotifies(clip, 1, 0.60f, 0.50f, false, q);
    CHECK(trace(q) == "Step");                         // 0.50 is the closed end
}

TEST_CASE("notifies: an empty span fires nothing, and a clip without duration is inert")
{
    const AnimationClipAsset clip = makeClip(1.0f, { { "Step", 0.50f, 0.0f } });

    HE::NotifyQueue q;
    HE::collectNotifies(clip, 1, 0.50f, 0.50f, false, q);
    CHECK(q.empty());

    const AnimationClipAsset dead = makeClip(0.0f, { { "Step", 0.0f, 0.0f } });
    HE::collectNotifies(dead, 1, 0.0f, 1.0f, true, q);
    CHECK(q.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
//  5, 6, 7 — through the systems
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("notifies: a non-looping clip fires the notify on its last frame exactly once")
{
    HorizonWorld world;
    ContentManager cm;

    const HE::UUID meshId = cm.registerSkeletalMesh(makeSkeleton(HE::UUID{}));
    const HE::UUID clipId = cm.registerAnimationClip(
        makePosedClip(1.0f, { { "End", 1.0f, 0.0f } }));

    const Entity e = world.createEntity("Actor");
    world.addComponent(e, TransformComponent{});
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorComponent an; an.clipAssetId = clipId; an.looping = false; an.playing = true;
    world.addComponent(e, an);

    HE::NotifyQueue q;
    // Twenty frames of 0.1 s: the clip is over after ten, and the eleventh must
    // not fire it again — nor any of the nine after that.
    for (int i = 0; i < 20; ++i)
        AnimationSystem::update(world, cm, 0.1f, nullptr, &q);

    CHECK(countOf(q, "End") == 1);
    CHECK_FALSE(world.registry().get<AnimatorComponent>(e).playing);
}

TEST_CASE("notifies: a null queue collects nothing and spends no priming")
{
    HorizonWorld world;
    ContentManager cm;

    const HE::UUID meshId = cm.registerSkeletalMesh(makeSkeleton(HE::UUID{}));
    const HE::UUID clipId = cm.registerAnimationClip(
        makePosedClip(1.0f, { { "Zero", 0.0f, 0.0f } }));

    const Entity e = world.createEntity("Actor");
    world.addComponent(e, TransformComponent{});
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorComponent an; an.clipAssetId = clipId; an.playing = true;
    world.addComponent(e, an);

    // The editor's case: the phase runs, the pose is produced, nothing is
    // collected. The playhead is genuinely advancing while this happens.
    for (int i = 0; i < 3; ++i)
        AnimationSystem::update(world, cm, 0.1f, nullptr, nullptr);
    CHECK(world.registry().get<AnimatorComponent>(e).playbackTime > 0.0f);
    CHECK_FALSE(world.registry().get<AnimatorComponent>(e).notifiesPrimed);

    // …and when a session starts, the frame-0 notify is still there to be had:
    // the priming was not spent on frames nobody was listening to. It fires on
    // the lap seam this frame crosses.
    world.registry().get<AnimatorComponent>(e).playbackTime = 0.95f;
    HE::NotifyQueue q;
    AnimationSystem::update(world, cm, 0.1f, nullptr, &q);
    CHECK(countOf(q, "Zero") == 1);
}

TEST_CASE("notifies: only the heavier half of a crossfade fires, and the lighter half hoards nothing")
{
    HorizonWorld world;
    ContentManager cm;

    const HE::UUID meshId = cm.registerSkeletalMesh(makeSkeleton(HE::UUID{}));
    const HE::UUID idleId = cm.registerAnimationClip(
        makePosedClip(1.0f, { { "IdleTick", 0.05f, 0.0f } }));
    const HE::UUID attackId = cm.registerAnimationClip(
        makePosedClip(1.0f, { { "AttackStart", 0.0f, 0.0f } }));

    HE::AnimatorStateMachineGraph g;
    { HE::AnimationState s; s.name = "Idle";   s.clipId = idleId;   s.looping = true;  g.states.push_back(s); }
    { HE::AnimationState s; s.name = "Attack"; s.clipId = attackId; s.looping = false; g.states.push_back(s); }
    { HE::AnimationTransition t; t.fromState = "Idle"; t.toState = "Attack";
      t.paramName = "go"; t.op = HE::TransitionOp::Greater; t.threshold = 0.5f;
      t.duration = 0.4f; g.transitions.push_back(t); }
    g.startState = "Idle";

    AnimatorStateMachineAsset asset;
    asset.name      = "fsm";
    asset.graphJson = HE::animatorStateMachineToJson(g);
    const HE::UUID fsmId = cm.registerAnimatorStateMachine(std::move(asset));

    const Entity e = world.createEntity("Fighter");
    world.addComponent(e, TransformComponent{});
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorStateMachineComponent sm; sm.stateMachineAssetId = fsmId;
    world.addComponent(e, sm);

    HE::NotifyQueue q;
    // Settle into Idle, past its 0.05 notify.
    for (int i = 0; i < 3; ++i)
        AnimationStateMachineSystem::update(world, cm, 0.1f, nullptr, nullptr, &q);
    REQUIRE(countOf(q, "IdleTick") >= 1);
    q.clear();

    // Start the crossfade. Two frames of 0.1 s over a 0.4 s transition put the
    // weight at 0.25 and 0.5: the outgoing clip still owns the first, and the
    // incoming one takes over on the second.
    world.registry().get<AnimatorStateMachineComponent>(e).params["go"] = 1.0f;
    AnimationStateMachineSystem::update(world, cm, 0.1f, nullptr, nullptr, &q);
    // The lighter half fires nothing at all, its frame-0 notify included.
    CHECK(countOf(q, "AttackStart") == 0);

    q.clear();
    AnimationStateMachineSystem::update(world, cm, 0.1f, nullptr, nullptr, &q);
    // And when the weight tips over it does NOT arrive late: the playhead was
    // walked past while it was light, so there is no backlog to dump. This is
    // the whole reason the lighter half is still primed.
    CHECK(countOf(q, "AttackStart") == 0);
    CHECK(countOf(q, "IdleTick") == 0);
}

TEST_CASE("notifies: a two-clip blend fires from the weighted side only")
{
    HorizonWorld world;
    ContentManager cm;

    const HE::UUID meshId = cm.registerSkeletalMesh(makeSkeleton(HE::UUID{}));
    const HE::UUID walkId = cm.registerAnimationClip(
        makePosedClip(1.0f, { { "WalkStep", 0.50f, 0.0f } }));
    const HE::UUID runId = cm.registerAnimationClip(
        makePosedClip(1.0f, { { "RunStep", 0.50f, 0.0f } }));

    const Entity e = world.createEntity("Runner");
    world.addComponent(e, TransformComponent{});
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorBlendComponent b;
    b.clipAId = walkId; b.clipBId = runId; b.blendAlpha = 0.2f; b.playing = true;
    world.addComponent(e, b);

    HE::NotifyQueue q;
    for (int i = 0; i < 10; ++i)
        AnimationBlendSystem::update(world, cm, 0.1f, nullptr, &q);
    CHECK(countOf(q, "WalkStep") == 1);
    CHECK(countOf(q, "RunStep") == 0);

    // Lean the other way and the other clip's footstep is the one that lands.
    world.registry().get<AnimatorBlendComponent>(e).blendAlpha = 0.8f;
    q.clear();
    for (int i = 0; i < 10; ++i)
        AnimationBlendSystem::update(world, cm, 0.1f, nullptr, &q);
    CHECK(countOf(q, "WalkStep") == 0);
    CHECK(countOf(q, "RunStep") == 1);
}

TEST_CASE("notifies: tickAnimation reaches the queue, and a null one costs nothing")
{
    HorizonWorld world;
    ContentManager cm;

    const HE::UUID meshId = cm.registerSkeletalMesh(makeSkeleton(HE::UUID{}));
    const HE::UUID clipId = cm.registerAnimationClip(
        makePosedClip(1.0f, { { "Step", 0.50f, 0.0f } }));

    const Entity e = world.createEntity("Actor");
    world.addComponent(e, TransformComponent{});
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorComponent an; an.clipAssetId = clipId; an.playing = true;
    world.addComponent(e, an);

    // The editor's shape of the call: no queue, so nothing is evaluated.
    for (int i = 0; i < 10; ++i)
        SceneSystems::tickAnimation(world, cm, 0.1f);

    HE::NotifyQueue q;
    world.registry().get<AnimatorComponent>(e).playbackTime = 0.4f;
    SceneSystems::tickAnimation(world, cm, 0.2f, nullptr, nullptr, &q);
    CHECK(countOf(q, "Step") == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
//  8 — delivery
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("notifies: one dispatch serves the script, the entity class and the sync graph")
{
    using namespace HorizonCode;
    TempContentDir dir("he_test_notify_dispatch");
    ContentManager cm(dir.path.string());
    HorizonWorld world;

    // A state machine whose asset also carries a sync graph — that graph is the
    // third receiver, and it is a SECOND instance on the same entity. Which is
    // exactly why AnimatorHost keeps its own table instead of the runtime being
    // asked "who is on this entity": the answer would be ambiguous.
    HE::AnimatorStateMachineGraph fsm;
    fsm.states.push_back({ 1, "Idle", HE::UUID{}, true, 0.0f, 0.0f });
    fsm.startState = "Idle";

    AnimatorStateMachineAsset asset;
    asset.type          = HE::AssetType::AnimatorStateMachine;
    asset.name          = "Locomotion";
    asset.path          = "Locomotion.hasset";
    asset.graphJson     = HE::animatorStateMachineToJson(fsm);
    asset.syncGraphJson = toJson(notifyTraceGraph("OnAnimationNotify", "heard"));
    REQUIRE(cm.saveAsset(asset));
    const HE::UUID smId = cm.loadAsset(asset.path);

    const Entity actor = world.createEntity("Actor");
    world.addComponent(actor, TransformComponent{});
    SkeletalMeshComponent smc; smc.meshAssetId = cm.registerSkeletalMesh(makeSkeleton(HE::UUID{}));
    world.addComponent(actor, smc);
    AnimatorStateMachineComponent sm; sm.stateMachineAssetId = smId;
    world.addComponent(actor, sm);

    ScriptContext sctx(world);
    static const char* kScript = R"lua(
local M = {}
function M.onStart(self) _heHeard = "" end
function M.onAnimationNotify(self, name)      _heHeard = _heHeard .. name end
function M.onAnimationNotifyBegin(self, name) _heHeard = _heHeard .. "+" .. name end
function M.onAnimationNotifyEnd(self, name)   _heHeard = _heHeard .. "-" .. name end
return M
)lua";
    REQUIRE(sctx.engine().loadScript("notified", kScript));
    const auto luaId = sctx.engine().createInstance("notified", static_cast<uint32_t>(actor));
    REQUIRE(luaId != ScriptEngine::kInvalidInstance);
    sctx.engine().callOnStart(luaId);
    AnimationNotifySystem::InstanceMap lua{ { static_cast<uint32_t>(actor), luaId } };

    Runtime rt;
    const InstanceId cls = rt.add(notifyTraceGraph("OnAnimationNotify", "heard"), {},
                                  { "Content/Actor.hasset", "Entity" });
    AnimationNotifySystem::HcInstanceMap hc{ { static_cast<uint32_t>(actor), cls } };

    AnimatorHost host;
    host.begin(rt, world, cm);
    REQUIRE(host.count() == 1);
    const InstanceId sync = host.instanceOf(actor);
    REQUIRE(sync != 0);
    REQUIRE(sync != cls);

    HE::NotifyQueue q;
    q.push_back({ static_cast<uint32_t>(actor), "Step",   Kind::Fire  });
    q.push_back({ static_cast<uint32_t>(actor), "Window", Kind::Begin });
    q.push_back({ static_cast<uint32_t>(actor), "Window", Kind::End   });

    AnimationNotifySystem::dispatch(q, world, &sctx, lua, &rt, hc, &host);

    // One call, three receivers. Lua hears all three kinds; the two graphs
    // handle only the plain one, which is the point of splitting the event.
    CHECK(sctx.engine().getGlobalString("_heHeard") == "Step+Window-Window");
    CHECK(rt.getVariable(cls,  "heard").s == "Step");
    CHECK(rt.getVariable(sync, "heard").s == "Step");

    // Dispatch DRAINS: a second call finds nothing left, the way polling the
    // physics queues does. That is what makes "one drain point" structural
    // instead of a rule two applications have to remember.
    CHECK(q.empty());
    AnimationNotifySystem::dispatch(q, world, &sctx, lua, &rt, hc, &host);
    CHECK(sctx.engine().getGlobalString("_heHeard") == "Step+Window-Window");
    CHECK(rt.getVariable(cls, "heard").s == "Step");

    host.end();
}

TEST_CASE("notifies: an entity destroyed by an earlier handler receives nothing")
{
    using namespace HorizonCode;
    HorizonWorld world;

    const Entity alive = world.createEntity("Alive");
    const Entity dead  = world.createEntity("Dead");

    Runtime rt;
    const InstanceId aliveCls = rt.add(notifyTraceGraph("OnAnimationNotify", "heard"), {},
                                       { "Content/A.hasset", "Entity" });
    const InstanceId deadCls  = rt.add(notifyTraceGraph("OnAnimationNotify", "heard"), {},
                                       { "Content/B.hasset", "Entity" });
    AnimationNotifySystem::HcInstanceMap hc{
        { static_cast<uint32_t>(alive), aliveCls },
        { static_cast<uint32_t>(dead),  deadCls  },
    };

    HE::NotifyQueue q;
    q.push_back({ static_cast<uint32_t>(alive), "Live", Kind::Fire });
    q.push_back({ static_cast<uint32_t>(dead),  "Gone", Kind::Fire });

    // The ordinary case this guards: a death animation whose notify despawns the
    // corpse. Everything queued for it afterwards names an id that resolves to
    // nothing — or, once entt recycles the slot, to somebody else entirely.
    world.destroyEntity(dead);

    AnimationNotifySystem::dispatch(q, world, nullptr, {}, &rt, hc);
    CHECK(rt.getVariable(aliveCls, "heard").s == "Live");
    CHECK(rt.getVariable(deadCls, "heard").s.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
//  The registry row
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("notifies: animator.notifiesOf answers what a clip carries")
{
    TempContentDir dir("he_test_notify_api");
    {
        ContentManager cm(dir.path.string());
        AnimationClipAsset clip;
        clip.type     = HE::AssetType::AnimationClip;
        clip.name     = "attack";
        clip.path     = "attack.hasset";
        clip.duration = 1.0f;
        clip.notifies = { { "Swing", 0.2f, 0.0f }, { "HitWindow", 0.3f, 0.2f } };
        REQUIRE(cm.saveAsset(clip));
    }

    ContentManager cm(dir.path.string());
    HE::api::Ctx c;
    c.content = &cm;

    const std::vector<std::string> names = HE::api::animator::notifiesOf(c, "attack.hasset");
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "Swing");
    CHECK(names[1] == "HitWindow");

    // An unknown path is a question with an empty answer, not an error: a graph
    // asking about a clip that is not there gets a list it can iterate over.
    CHECK(HE::api::animator::notifiesOf(c, "nope.hasset").empty());
    CHECK(HE::api::animator::notifiesOf(c, "").empty());

    // And with no content manager at all, like every other null-Ctx row.
    HE::api::Ctx empty;
    CHECK(HE::api::animator::notifiesOf(empty, "attack.hasset").empty());
}
