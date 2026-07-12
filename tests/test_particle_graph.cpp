#include "doctest.h"
#include <ParticleGraph/ParticleGraph.h>
#include <random>
#include <cmath>

using namespace HE;

TEST_CASE("ParticleGraph::makeDefault has exactly one EmitterOutput node")
{
    ParticleGraph g = ParticleGraph::makeDefault();
    REQUIRE(g.nodes.size() == 1);
    CHECK(g.nodes[0].type == ParticleNodeType::EmitterOutput);
}

TEST_CASE("ParticleGraph::addNode seeds sensible defaults per type")
{
    ParticleGraph g;
    const int cf = g.addNode(ParticleNodeType::ConstFloat);
    CHECK(g.findNode(cf)->p[0] == doctest::Approx(1.0f));

    const int cc = g.addNode(ParticleNodeType::ConstColor);
    CHECK(g.findNode(cc)->p[0] == doctest::Approx(1.0f));
    CHECK(g.findNode(cc)->p[1] == doctest::Approx(1.0f));
    CHECK(g.findNode(cc)->p[2] == doctest::Approx(1.0f));

    const int rr = g.addNode(ParticleNodeType::RandomRange);
    CHECK(g.findNode(rr)->p[0] == doctest::Approx(0.0f));
    CHECK(g.findNode(rr)->p[1] == doctest::Approx(1.0f));
}

TEST_CASE("ParticleGraph::connect rejects out-of-range pins and self-links")
{
    ParticleGraph g;
    const int out = g.addNode(ParticleNodeType::EmitterOutput);
    const int cf  = g.addNode(ParticleNodeType::ConstFloat);

    CHECK(g.connect(cf, 0, out, 0));            // valid
    CHECK(!g.connect(cf, 0, out, 999));          // out-of-range input pin
    CHECK(!g.connect(cf, 99, out, 1));           // out-of-range output pin
    CHECK(!g.connect(out, 0, out, 0));           // self-link
}

TEST_CASE("ParticleGraph::connect replaces an existing link on the same input pin")
{
    ParticleGraph g;
    const int out = g.addNode(ParticleNodeType::EmitterOutput);
    const int a   = g.addNode(ParticleNodeType::ConstFloat);
    const int b   = g.addNode(ParticleNodeType::ConstFloat);

    REQUIRE(g.connect(a, 0, out, 0));
    REQUIRE(g.connect(b, 0, out, 0)); // same dst pin — should replace, not add a 2nd link
    int count = 0;
    for (const auto& l : g.links) if (l.dstNode == out && l.dstPin == 0) ++count;
    CHECK(count == 1);
    CHECK(g.links.back().srcNode == b);
}

TEST_CASE("ParticleGraph::removeNode cannot remove the EmitterOutput")
{
    ParticleGraph g = ParticleGraph::makeDefault();
    const int outId = g.nodes[0].id;
    g.removeNode(outId);
    CHECK(g.nodes.size() == 1); // still there
}

TEST_CASE("ParticleGraph::removeNode drops the node and any links touching it")
{
    ParticleGraph g;
    const int out = g.addNode(ParticleNodeType::EmitterOutput);
    const int cf  = g.addNode(ParticleNodeType::ConstFloat);
    REQUIRE(g.connect(cf, 0, out, 0));

    g.removeNode(cf);
    CHECK(g.nodes.size() == 1);
    CHECK(g.links.empty());
}

TEST_CASE("particleGraphToJson/FromJson round-trips nodes, links and positions")
{
    ParticleGraph g;
    const int out = g.addNode(ParticleNodeType::EmitterOutput, 300.0f, 120.0f);
    const int cf  = g.addNode(ParticleNodeType::ConstFloat, 50.0f, 50.0f);
    g.findNode(cf)->p[0] = 42.5f;
    REQUIRE(g.connect(cf, 0, out, 0));
    g.findNode(out)->meshAssetId     = HE::UUID::generate();
    g.findNode(out)->materialAssetId = HE::UUID::generate();

    const std::string json = particleGraphToJson(g);

    ParticleGraph loaded;
    REQUIRE(particleGraphFromJson(json, loaded));
    REQUIRE(loaded.nodes.size() == 2);
    REQUIRE(loaded.links.size() == 1);

    const ParticleGraphNode* loadedCf = loaded.findNode(cf);
    REQUIRE(loadedCf != nullptr);
    CHECK(loadedCf->p[0] == doctest::Approx(42.5f));
    CHECK(loadedCf->x == doctest::Approx(50.0f));

    const ParticleGraphNode* loadedOut = loaded.findNode(out);
    REQUIRE(loadedOut != nullptr);
    CHECK(loadedOut->meshAssetId     == g.findNode(out)->meshAssetId);
    CHECK(loadedOut->materialAssetId == g.findNode(out)->materialAssetId);
}

TEST_CASE("particleGraphFromJson rejects garbage but doesn't crash")
{
    ParticleGraph g;
    CHECK(!particleGraphFromJson("not json at all", g));
    CHECK(!particleGraphFromJson("[]", g)); // valid JSON, not an object
}

TEST_CASE("evaluateParticleGraph on a bare default graph matches old component defaults")
{
    ParticleGraph g = ParticleGraph::makeDefault();
    std::mt19937 rng{ 7 };
    const ParticleEmitterConfig cfg = evaluateParticleGraph(g, rng);

    CHECK(cfg.emitRate       == doctest::Approx(10.0f));
    CHECK(cfg.lifetimeMin    == doctest::Approx(1.0f));
    CHECK(cfg.lifetimeMax    == doctest::Approx(2.0f));
    CHECK(cfg.startSize      == doctest::Approx(0.3f));
    CHECK(cfg.endSize        == doctest::Approx(0.0f));
    CHECK(cfg.startAlpha     == doctest::Approx(1.0f));
    CHECK(cfg.endAlpha       == doctest::Approx(0.0f));
    CHECK(cfg.velocitySpread == doctest::Approx(0.5f));
    CHECK(cfg.maxParticles   == 100);
    CHECK(cfg.looping);
    CHECK(cfg.meshAssetId     == HE::UUID{});
    CHECK(cfg.materialAssetId == HE::UUID{});
}

TEST_CASE("evaluateParticleGraph reads a ConstFloat wired into Emit Rate")
{
    ParticleGraph g;
    const int out = g.addNode(ParticleNodeType::EmitterOutput);
    const int cf  = g.addNode(ParticleNodeType::ConstFloat);
    g.findNode(cf)->p[0] = 77.0f;
    REQUIRE(g.connect(cf, 0, out, 0)); // pin 0 = Emit Rate

    std::mt19937 rng{ 1 };
    CHECK(evaluateParticleGraph(g, rng).emitRate == doctest::Approx(77.0f));
}

TEST_CASE("evaluateParticleGraph reads a ConstVec3 wired into Gravity")
{
    ParticleGraph g;
    const int out = g.addNode(ParticleNodeType::EmitterOutput);
    const int cv  = g.addNode(ParticleNodeType::ConstVec3);
    HE::ParticleGraphNode* n = g.findNode(cv);
    n->p[0] = 1.0f; n->p[1] = -9.8f; n->p[2] = 3.0f;
    REQUIRE(g.connect(cv, 0, out, 11)); // pin 11 = Gravity

    std::mt19937 rng{ 1 };
    const ParticleEmitterConfig cfg = evaluateParticleGraph(g, rng);
    CHECK(cfg.gravity[0] == doctest::Approx(1.0f));
    CHECK(cfg.gravity[1] == doctest::Approx(-9.8f));
    CHECK(cfg.gravity[2] == doctest::Approx(3.0f));
}

TEST_CASE("evaluateParticleGraph: RandomRange stays within [min, max]")
{
    ParticleGraph g;
    const int out = g.addNode(ParticleNodeType::EmitterOutput);
    const int rr  = g.addNode(ParticleNodeType::RandomRange);
    g.findNode(rr)->p[0] = 5.0f;
    g.findNode(rr)->p[1] = 15.0f;
    REQUIRE(g.connect(rr, 0, out, 0)); // Emit Rate

    std::mt19937 rng{ 123 };
    for (int i = 0; i < 20; ++i)
    {
        const float v = evaluateParticleGraph(g, rng).emitRate;
        CHECK(v >= 5.0f);
        CHECK(v <= 15.0f);
    }
}

TEST_CASE("evaluateParticleGraph: Add/Multiply/Lerp combine their inputs")
{
    ParticleGraph g;
    const int out = g.addNode(ParticleNodeType::EmitterOutput);

    // Add: 2 + 3 = 5 → Emit Rate
    {
        const int add = g.addNode(ParticleNodeType::Add);
        const int a = g.addNode(ParticleNodeType::ConstFloat); g.findNode(a)->p[0] = 2.0f;
        const int b = g.addNode(ParticleNodeType::ConstFloat); g.findNode(b)->p[0] = 3.0f;
        REQUIRE(g.connect(a, 0, add, 0));
        REQUIRE(g.connect(b, 0, add, 1));
        REQUIRE(g.connect(add, 0, out, 0));
    }
    // Multiply: 4 * 5 = 20 → Lifetime Min
    {
        const int mul = g.addNode(ParticleNodeType::Multiply);
        const int a = g.addNode(ParticleNodeType::ConstFloat); g.findNode(a)->p[0] = 4.0f;
        const int b = g.addNode(ParticleNodeType::ConstFloat); g.findNode(b)->p[0] = 5.0f;
        REQUIRE(g.connect(a, 0, mul, 0));
        REQUIRE(g.connect(b, 0, mul, 1));
        REQUIRE(g.connect(mul, 0, out, 1));
    }
    // Lerp(10, 20, 0.5) = 15 → Lifetime Max
    {
        const int lerp = g.addNode(ParticleNodeType::Lerp);
        const int a = g.addNode(ParticleNodeType::ConstFloat); g.findNode(a)->p[0] = 10.0f;
        const int b = g.addNode(ParticleNodeType::ConstFloat); g.findNode(b)->p[0] = 20.0f;
        const int alpha = g.addNode(ParticleNodeType::ConstFloat); g.findNode(alpha)->p[0] = 0.5f;
        REQUIRE(g.connect(a, 0, lerp, 0));
        REQUIRE(g.connect(b, 0, lerp, 1));
        REQUIRE(g.connect(alpha, 0, lerp, 2));
        REQUIRE(g.connect(lerp, 0, out, 2));
    }

    std::mt19937 rng{ 1 };
    const ParticleEmitterConfig cfg = evaluateParticleGraph(g, rng);
    CHECK(cfg.emitRate    == doctest::Approx(5.0f));
    CHECK(cfg.lifetimeMin == doctest::Approx(20.0f));
    CHECK(cfg.lifetimeMax == doctest::Approx(15.0f));
}

TEST_CASE("evaluateParticleGraph: unconnected pins fall back to registry defaults")
{
    // No output node at all → every field should be the ParticleEmitterConfig
    // struct's own defaults (which match ParticleGraph::makeDefault()'s pins).
    ParticleGraph g;
    std::mt19937 rng{ 1 };
    const ParticleEmitterConfig cfg = evaluateParticleGraph(g, rng);
    CHECK(cfg.emitRate == doctest::Approx(10.0f));
    CHECK(cfg.maxParticles == 100);
}

TEST_CASE("evaluateParticleGraph: a link cycle does not hang or crash")
{
    ParticleGraph g;
    const int out = g.addNode(ParticleNodeType::EmitterOutput);
    const int a = g.addNode(ParticleNodeType::Add);
    const int b = g.addNode(ParticleNodeType::Add);
    // a.A <- b.Out, b.A <- a.Out : a genuine cycle (hand-constructed — the normal
    // connect() call graph can't produce this without going through pins that
    // don't exist, so we poke g.links directly to simulate a corrupt/hand-edited file).
    g.links.push_back({ b, 0, a, 0 });
    g.links.push_back({ a, 0, b, 0 });
    REQUIRE(g.connect(a, 0, out, 0));

    std::mt19937 rng{ 1 };
    // Must return (not hang) and produce SOME finite float.
    const float v = evaluateParticleGraph(g, rng).emitRate;
    CHECK(std::isfinite(v));
}

TEST_CASE("evaluateParticleGraph copies mesh/material UUIDs from the Emitter Output node")
{
    ParticleGraph g;
    const int out = g.addNode(ParticleNodeType::EmitterOutput);
    const HE::UUID mesh = HE::UUID::generate();
    const HE::UUID mat  = HE::UUID::generate();
    g.findNode(out)->meshAssetId     = mesh;
    g.findNode(out)->materialAssetId = mat;

    std::mt19937 rng{ 1 };
    const ParticleEmitterConfig cfg = evaluateParticleGraph(g, rng);
    CHECK(cfg.meshAssetId     == mesh);
    CHECK(cfg.materialAssetId == mat);
}
