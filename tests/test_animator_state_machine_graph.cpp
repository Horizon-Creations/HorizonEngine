#include "doctest.h"
#include <AnimatorStateMachine/AnimatorStateMachineGraph.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>

using namespace HE;

TEST_CASE("AnimatorStateMachineGraph defaults")
{
    AnimatorStateMachineGraph g;
    CHECK(g.states.empty());
    CHECK(g.transitions.empty());
    CHECK(g.defaultParams.empty());
    CHECK(g.startState.empty());
}

TEST_CASE("AnimationState defaults")
{
    AnimationState s;
    CHECK(s.id == 0);
    CHECK(s.name.empty());
    CHECK(s.clipId == HE::UUID{});
    CHECK(s.looping);
    CHECK(s.x == doctest::Approx(0.0f));
    CHECK(s.y == doctest::Approx(0.0f));
}

TEST_CASE("animatorStateMachineToJson/FromJson round-trips states, transitions, params, startState")
{
    AnimatorStateMachineGraph g;
    AnimationState idle; idle.id = 1; idle.name = "Idle"; idle.clipId = HE::UUID::generate();
    idle.looping = true; idle.x = 10.0f; idle.y = 20.0f;
    AnimationState walk; walk.id = 2; walk.name = "Walk"; walk.clipId = HE::UUID::generate();
    walk.looping = false; walk.x = 210.0f; walk.y = 30.0f;
    g.states = { idle, walk };

    AnimationTransition t;
    t.fromState = "Idle"; t.toState = "Walk"; t.paramName = "speed";
    t.op = TransitionOp::Greater; t.threshold = 0.1f; t.duration = 0.25f;
    g.transitions = { t };
    g.defaultParams["speed"] = 0.5f;
    g.startState = "Idle";

    const std::string json = animatorStateMachineToJson(g);
    AnimatorStateMachineGraph parsed;
    REQUIRE(animatorStateMachineFromJson(json, parsed));

    REQUIRE(parsed.states.size() == 2);
    CHECK(parsed.states[0].id == 1);
    CHECK(parsed.states[0].name == "Idle");
    CHECK(parsed.states[0].clipId == idle.clipId);
    CHECK(parsed.states[0].looping);
    CHECK(parsed.states[0].x == doctest::Approx(10.0f));
    CHECK(parsed.states[0].y == doctest::Approx(20.0f));
    CHECK(parsed.states[1].id == 2);
    CHECK_FALSE(parsed.states[1].looping);
    CHECK(parsed.states[1].x == doctest::Approx(210.0f));

    REQUIRE(parsed.transitions.size() == 1);
    CHECK(parsed.transitions[0].fromState == "Idle");
    CHECK(parsed.transitions[0].toState   == "Walk");
    CHECK(parsed.transitions[0].paramName == "speed");
    CHECK(parsed.transitions[0].op == TransitionOp::Greater);
    CHECK(parsed.transitions[0].threshold == doctest::Approx(0.1f));
    CHECK(parsed.transitions[0].duration  == doctest::Approx(0.25f));

    REQUIRE(parsed.defaultParams.count("speed"));
    CHECK(parsed.defaultParams.at("speed") == doctest::Approx(0.5f));
    CHECK(parsed.startState == "Idle");
}

TEST_CASE("animatorStateMachineToJson/FromJson round-trips an empty graph")
{
    AnimatorStateMachineGraph g;
    const std::string json = animatorStateMachineToJson(g);
    AnimatorStateMachineGraph parsed;
    REQUIRE(animatorStateMachineFromJson(json, parsed));
    CHECK(parsed.states.empty());
    CHECK(parsed.transitions.empty());
    CHECK(parsed.defaultParams.empty());
    CHECK(parsed.startState.empty());
}

TEST_CASE("animatorStateMachineFromJson rejects garbage but doesn't crash")
{
    AnimatorStateMachineGraph g;
    CHECK_FALSE(animatorStateMachineFromJson("not json", g));
    CHECK_FALSE(animatorStateMachineFromJson("[1,2,3]", g)); // valid JSON, not an object
}

TEST_CASE("ContentManager registers and retrieves an AnimatorStateMachineAsset")
{
    ContentManager cm;

    AnimatorStateMachineGraph g;
    g.states.push_back({ 1, "Idle", HE::UUID::generate(), true, 0.0f, 0.0f });
    g.startState = "Idle";

    AnimatorStateMachineAsset asset;
    asset.name      = "TestSM";
    asset.graphJson = animatorStateMachineToJson(g);

    const HE::UUID id = cm.registerAnimatorStateMachine(std::move(asset));
    REQUIRE(id != HE::UUID{});
    REQUIRE(cm.assetType(id) == HE::AssetType::AnimatorStateMachine);

    const AnimatorStateMachineAsset* got = cm.getAnimatorStateMachine(id);
    REQUIRE(got != nullptr);
    CHECK(got->name == "TestSM");

    AnimatorStateMachineGraph parsed;
    REQUIRE(animatorStateMachineFromJson(got->graphJson, parsed));
    REQUIRE(parsed.states.size() == 1);
    CHECK(parsed.states[0].name == "Idle");
}

TEST_CASE("ContentManager AnimatorStateMachineAsset wrong-type lookup returns nullptr")
{
    ContentManager cm;
    StaticMeshAsset mesh;
    const HE::UUID id = cm.registerStaticMesh(std::move(mesh));
    CHECK(cm.getAnimatorStateMachine(id) == nullptr);
}

TEST_CASE("ContentManager getAnimatorStateMachineMutable allows in-place edits")
{
    ContentManager cm;
    AnimatorStateMachineAsset asset;
    asset.name = "Mutable";
    const HE::UUID id = cm.registerAnimatorStateMachine(std::move(asset));

    AnimatorStateMachineAsset* mut = cm.getAnimatorStateMachineMutable(id);
    REQUIRE(mut != nullptr);
    AnimatorStateMachineGraph g;
    g.startState = "Edited";
    mut->graphJson = animatorStateMachineToJson(g);

    const AnimatorStateMachineAsset* got = cm.getAnimatorStateMachine(id);
    AnimatorStateMachineGraph parsed;
    REQUIRE(animatorStateMachineFromJson(got->graphJson, parsed));
    CHECK(parsed.startState == "Edited");
}

// ═══ On-disk format: what today's parser must keep accepting ═════════════════

TEST_CASE("animatorStateMachineFromJson loads a hand-written OLD-format document")
{
    // Byte-for-byte the shape the shipped editor writes: compact dump, states
    // with id/name/clipId{hi,lo}/looping/x/y, transitions referencing states BY
    // NAME, defaultParams as a plain object. Kept as a literal so a refactor of
    // the writer can never quietly redefine what "loadable" means.
    const std::string old =
        R"({"version":1,"startState":"Idle",)"
        R"("states":[{"id":1,"name":"Idle","clipId":{"hi":7,"lo":9},"looping":true,"x":10.0,"y":20.0},)"
        R"({"id":2,"name":"Run","clipId":{"hi":0,"lo":0},"looping":false,"x":200.0,"y":20.0}],)"
        R"("transitions":[{"fromState":"Idle","toState":"Run","paramName":"Speed","op":0,)"
        R"("threshold":0.25,"duration":0.15}],)"
        R"("defaultParams":{"Speed":1.5}})";

    AnimatorStateMachineGraph g;
    REQUIRE(animatorStateMachineFromJson(old, g));
    CHECK(g.startState == "Idle");
    REQUIRE(g.states.size() == 2);
    CHECK(g.states[0].id == 1);
    CHECK(g.states[0].name == "Idle");
    CHECK(g.states[0].clipId.hi == 7);
    CHECK(g.states[0].clipId.lo == 9);
    CHECK(g.states[0].looping);
    CHECK(g.states[0].x == doctest::Approx(10.0f));
    CHECK(g.states[1].name == "Run");
    CHECK_FALSE(g.states[1].looping);
    REQUIRE(g.transitions.size() == 1);
    CHECK(g.transitions[0].fromState == "Idle");
    CHECK(g.transitions[0].toState == "Run");
    CHECK(g.transitions[0].paramName == "Speed");
    CHECK(g.transitions[0].op == TransitionOp::Greater);
    CHECK(g.transitions[0].threshold == doctest::Approx(0.25f));
    CHECK(g.transitions[0].duration == doctest::Approx(0.15f));
    REQUIRE(g.defaultParams.count("Speed") == 1);
    CHECK(g.defaultParams.at("Speed") == doctest::Approx(1.5f));
}

TEST_CASE("animatorStateMachineFromJson reads every in-range TransitionOp")
{
    for (int op = 0; op <= 2; ++op)
    {
        const std::string json =
            R"({"startState":"","states":[],"transitions":[{"fromState":"A","toState":"B",)"
            R"("paramName":"p","op":)" + std::to_string(op) +
            R"(,"threshold":0.5,"duration":0.2}],"defaultParams":{}})";
        AnimatorStateMachineGraph g;
        REQUIRE(animatorStateMachineFromJson(json, g));
        REQUIRE(g.transitions.size() == 1);
        CHECK(g.transitions[0].op == static_cast<TransitionOp>(op));
    }
}

TEST_CASE("animatorStateMachineFromJson clamps an OUT-OF-RANGE TransitionOp")
{
    // A file from a newer editor (or a hand-edit) can name an operator this build
    // has no enumerator for. Casting it blind produced a TransitionOp outside the
    // enum, which the evaluating switch then compared against — an undefined
    // transition decision. Unknown ops read as the field default (Greater).
    for (int op : { 3, 99, -1 })
    {
        const std::string json =
            R"({"startState":"","states":[],"transitions":[{"fromState":"A","toState":"B",)"
            R"("paramName":"p","op":)" + std::to_string(op) +
            R"(,"threshold":0.5,"duration":0.2}],"defaultParams":{}})";
        AnimatorStateMachineGraph g;
        REQUIRE(animatorStateMachineFromJson(json, g));
        REQUIRE(g.transitions.size() == 1);
        CHECK(g.transitions[0].op == TransitionOp::Greater);
    }
}

TEST_CASE("animatorStateMachineFromJson survives a state whose clipId is not an object")
{
    const std::string json =
        R"({"startState":"","states":[{"id":1,"name":"A","clipId":42,"looping":true}],)"
        R"("transitions":[],"defaultParams":{}})";
    AnimatorStateMachineGraph g;
    REQUIRE(animatorStateMachineFromJson(json, g));
    REQUIRE(g.states.size() == 1);
    CHECK(g.states[0].clipId == HE::UUID{}); // null UUID, not a throw
}
