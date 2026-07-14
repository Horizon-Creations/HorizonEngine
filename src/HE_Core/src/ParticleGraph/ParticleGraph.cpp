#include "ParticleGraph/ParticleGraph.h"
#include <cstdint>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace HE
{

namespace
{
using F = ParticlePinType;

const std::vector<ParticleNodeDesc>& registry()
{
    static const std::vector<ParticleNodeDesc> r = {
        // Fixed sink — every field a today's ParticleSystemComponent used to hold
        // inline, now resolved from the graph. Mesh/Material are node-body slots
        // (meshAssetId/materialAssetId), not pins — see ParticleGraphNode.
        // Pins 14-16 (Collision Enabled/Restitution/Kill On Collision) were added
        // after the first 14 shipped — appended at the END so existing pin
        // indices (hardcoded in evaluateParticleGraph and the editor's slot UI)
        // never shift under already-saved graphs.
        { ParticleNodeType::EmitterOutput, "Emitter Output",
          { { "Emit Rate", F::Float, 10.0f }, { "Lifetime Min", F::Float, 1.0f },
            { "Lifetime Max", F::Float, 2.0f }, { "Start Size", F::Float, 0.3f },
            { "End Size", F::Float, 0.0f }, { "Start Color", F::Vec3, 1.0f },
            { "End Color", F::Vec3, 1.0f }, { "Start Alpha", F::Float, 1.0f },
            { "End Alpha", F::Float, 0.0f }, { "Initial Velocity", F::Vec3, 0.0f },
            { "Velocity Spread", F::Float, 0.5f }, { "Gravity", F::Vec3, 0.0f },
            { "Max Particles", F::Float, 100.0f }, { "Looping", F::Float, 1.0f },
            { "Collision Enabled", F::Float, 0.0f }, { "Restitution", F::Float, 0.4f },
            { "Kill On Collision", F::Float, 0.0f } },
          {}, 0 },

        { ParticleNodeType::ConstFloat,  "Const Float",  {}, { { "Out", F::Float, 0 } }, 1 },
        { ParticleNodeType::ConstVec3,   "Const Vec3",   {}, { { "Out", F::Vec3,  0 } }, 3 },
        { ParticleNodeType::ConstColor,  "Const Color",  {}, { { "Out", F::Vec3,  0 } }, 3 },
        // p[0]=min, p[1]=max. Resolves once per evaluate() call (see header note) —
        // "roll a value", not per-particle-spawn variance.
        { ParticleNodeType::RandomRange, "Random Range", {}, { { "Out", F::Float, 0 } }, 2 },

        // Vec3-typed so one node covers scalar use too: an unconnected/Float input
        // splats (def,def,def), and a Float sink truncates the Vec3 output to .x —
        // same coercion rule as MaterialGraph.
        { ParticleNodeType::Add,      "Add",
          { { "A", F::Vec3, 0 }, { "B", F::Vec3, 0 } }, { { "Out", F::Vec3, 0 } }, 0 },
        { ParticleNodeType::Multiply, "Multiply",
          { { "A", F::Vec3, 1 }, { "B", F::Vec3, 1 } }, { { "Out", F::Vec3, 0 } }, 0 },
        { ParticleNodeType::Lerp,     "Lerp",
          { { "A", F::Vec3, 0 }, { "B", F::Vec3, 1 }, { "Alpha", F::Float, 0.5f } },
          { { "Out", F::Vec3, 0 } }, 0 },
    };
    return r;
}
} // namespace

const std::vector<ParticleNodeDesc>& particleNodeRegistry() { return registry(); }

const ParticleNodeDesc& particleNodeDesc(ParticleNodeType type)
{
    for (const auto& d : registry())
        if (d.type == type) return d;
    return registry().front();
}

const ParticleNodeDesc* particleNodeDescByName(const std::string& name)
{
    for (const auto& d : registry())
        if (name == d.name) return &d;
    return nullptr;
}

int ParticleGraph::addNode(ParticleNodeType type, float x, float y)
{
    ParticleGraphNode n;
    n.id = nextId++;
    n.type = type;
    n.x = x; n.y = y;
    switch (type)
    {
        case ParticleNodeType::ConstFloat:  n.p[0] = 1.0f; break;
        case ParticleNodeType::ConstVec3:   n.p[0] = n.p[1] = n.p[2] = 0.0f; break;
        case ParticleNodeType::ConstColor:  n.p[0] = n.p[1] = n.p[2] = 1.0f; break;
        case ParticleNodeType::RandomRange: n.p[0] = 0.0f; n.p[1] = 1.0f; break;
        default: break;
    }
    nodes.push_back(n);
    return n.id;
}

const ParticleGraphNode* ParticleGraph::findNode(int id) const
{
    for (const auto& n : nodes) if (n.id == id) return &n;
    return nullptr;
}

ParticleGraphNode* ParticleGraph::findNode(int id)
{
    for (auto& n : nodes) if (n.id == id) return &n;
    return nullptr;
}

bool ParticleGraph::connect(int srcNode, int srcPin, int dstNode, int dstPin)
{
    if (srcNode == dstNode) return false;
    const ParticleGraphNode* s = findNode(srcNode);
    const ParticleGraphNode* d = findNode(dstNode);
    if (!s || !d) return false;
    const ParticleNodeDesc& sd = particleNodeDesc(s->type);
    const ParticleNodeDesc& dd = particleNodeDesc(d->type);
    if (srcPin < 0 || srcPin >= (int)sd.outputs.size()) return false;
    if (dstPin < 0 || dstPin >= (int)dd.inputs.size())  return false;
    disconnectInput(dstNode, dstPin);
    links.push_back({ srcNode, srcPin, dstNode, dstPin });
    return true;
}

void ParticleGraph::disconnectInput(int dstNode, int dstPin)
{
    links.erase(std::remove_if(links.begin(), links.end(),
        [&](const ParticleGraphLink& l) { return l.dstNode == dstNode && l.dstPin == dstPin; }),
        links.end());
}

void ParticleGraph::removeNode(int id)
{
    const ParticleGraphNode* n = findNode(id);
    if (!n || n->type == ParticleNodeType::EmitterOutput) return;
    links.erase(std::remove_if(links.begin(), links.end(),
        [&](const ParticleGraphLink& l) { return l.srcNode == id || l.dstNode == id; }),
        links.end());
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
        [&](const ParticleGraphNode& nn) { return nn.id == id; }),
        nodes.end());
}

ParticleGraph ParticleGraph::makeDefault()
{
    ParticleGraph g;
    // Every EmitterOutput pin has a default matching the old ParticleSystemComponent
    // field defaults, so a fresh graph reproduces today's behavior unmodified.
    g.addNode(ParticleNodeType::EmitterOutput, 300.0f, 120.0f);
    return g;
}

// ── JSON round-trip ──────────────────────────────────────────────────────────

std::string particleGraphToJson(const ParticleGraph& graph)
{
    nlohmann::json j;
    j["version"] = 1;
    j["nextId"]  = graph.nextId;
    for (const auto& n : graph.nodes)
    {
        nlohmann::json jn = { { "id", n.id }, { "type", particleNodeDesc(n.type).name },
                              { "p", { n.p[0], n.p[1], n.p[2], n.p[3] } },
                              { "x", n.x }, { "y", n.y } };
        if (n.meshAssetId != HE::UUID{})
            jn["meshId"] = { { "hi", n.meshAssetId.hi }, { "lo", n.meshAssetId.lo } };
        if (n.materialAssetId != HE::UUID{})
            jn["matId"] = { { "hi", n.materialAssetId.hi }, { "lo", n.materialAssetId.lo } };
        j["nodes"].push_back(std::move(jn));
    }
    for (const auto& l : graph.links)
        j["links"].push_back({ { "sn", l.srcNode }, { "sp", l.srcPin },
                               { "dn", l.dstNode }, { "dp", l.dstPin } });
    return j.dump();
}

bool particleGraphFromJson(const std::string& json, ParticleGraph& out)
{
    nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;

    ParticleGraph g;
    g.nextId = j.value("nextId", 1);
    for (const auto& jn : j.value("nodes", nlohmann::json::array()))
    {
        const ParticleNodeDesc* d = particleNodeDescByName(jn.value("type", ""));
        if (!d) continue;
        ParticleGraphNode n;
        n.id   = jn.value("id", 0);
        n.type = d->type;
        if (auto p = jn.find("p"); p != jn.end() && p->is_array())
            for (size_t i = 0; i < 4 && i < p->size(); ++i) n.p[i] = (*p)[i].get<float>();
        if (auto m = jn.find("meshId"); m != jn.end())
        { n.meshAssetId.hi = m->value("hi", uint64_t(0)); n.meshAssetId.lo = m->value("lo", uint64_t(0)); }
        if (auto m = jn.find("matId"); m != jn.end())
        { n.materialAssetId.hi = m->value("hi", uint64_t(0)); n.materialAssetId.lo = m->value("lo", uint64_t(0)); }
        n.x = jn.value("x", 0.0f);
        n.y = jn.value("y", 0.0f);
        g.nodes.push_back(n);
        g.nextId = std::max(g.nextId, n.id + 1);
    }
    for (const auto& jl : j.value("links", nlohmann::json::array()))
        g.links.push_back({ jl.value("sn", 0), jl.value("sp", 0),
                            jl.value("dn", 0), jl.value("dp", 0) });

    out = std::move(g);
    return true;
}

// ── Evaluation ────────────────────────────────────────────────────────────────
namespace
{
const ParticleGraphLink* findFeedingLink(const ParticleGraph& g, int dstNode, int dstPin)
{
    for (const auto& l : g.links)
        if (l.dstNode == dstNode && l.dstPin == dstPin) return &l;
    return nullptr;
}

void evalNodeVec3(const ParticleGraph& g, int nodeId, std::mt19937& rng, float out[3], int depth);

// Evaluate node `nodeId`'s input pin `pinIndex` as a vec3 (Float pins splat their
// scalar into all three components; a Float SINK later reads back just .x — same
// promotion/truncation coercion MaterialGraph's codegen uses).
void evalInputVec3(const ParticleGraph& g, int nodeId, int pinIndex, std::mt19937& rng, float out[3], int depth)
{
    const ParticleGraphNode* n = g.findNode(nodeId);
    const ParticleNodeDesc& d  = particleNodeDesc(n ? n->type : ParticleNodeType::ConstFloat);
    const float def = pinIndex < (int)d.inputs.size() ? d.inputs[pinIndex].def : 0.0f;

    if (depth <= 32) // cycle guard — a corrupt/hand-edited graph can't hang evaluate()
        if (const ParticleGraphLink* l = findFeedingLink(g, nodeId, pinIndex))
        {
            evalNodeVec3(g, l->srcNode, rng, out, depth + 1);
            return;
        }
    out[0] = out[1] = out[2] = def;
}

float evalInputFloat(const ParticleGraph& g, int nodeId, int pinIndex, std::mt19937& rng, int depth)
{
    float v[3]; evalInputVec3(g, nodeId, pinIndex, rng, v, depth);
    return v[0];
}

void evalNodeVec3(const ParticleGraph& g, int nodeId, std::mt19937& rng, float out[3], int depth)
{
    const ParticleGraphNode* n = g.findNode(nodeId);
    if (!n) { out[0] = out[1] = out[2] = 0.0f; return; }

    switch (n->type)
    {
        case ParticleNodeType::ConstFloat:
            out[0] = out[1] = out[2] = n->p[0];
            break;
        case ParticleNodeType::ConstVec3:
        case ParticleNodeType::ConstColor:
            out[0] = n->p[0]; out[1] = n->p[1]; out[2] = n->p[2];
            break;
        case ParticleNodeType::RandomRange:
        {
            std::uniform_real_distribution<float> dist(n->p[0], n->p[1]);
            const float v = dist(rng);
            out[0] = out[1] = out[2] = v;
            break;
        }
        case ParticleNodeType::Add:
        {
            float a[3], b[3];
            evalInputVec3(g, n->id, 0, rng, a, depth);
            evalInputVec3(g, n->id, 1, rng, b, depth);
            for (int i = 0; i < 3; ++i) out[i] = a[i] + b[i];
            break;
        }
        case ParticleNodeType::Multiply:
        {
            float a[3], b[3];
            evalInputVec3(g, n->id, 0, rng, a, depth);
            evalInputVec3(g, n->id, 1, rng, b, depth);
            for (int i = 0; i < 3; ++i) out[i] = a[i] * b[i];
            break;
        }
        case ParticleNodeType::Lerp:
        {
            float a[3], b[3];
            evalInputVec3(g, n->id, 0, rng, a, depth);
            evalInputVec3(g, n->id, 1, rng, b, depth);
            const float alpha = evalInputFloat(g, n->id, 2, rng, depth);
            for (int i = 0; i < 3; ++i) out[i] = a[i] + (b[i] - a[i]) * alpha;
            break;
        }
        default:
            out[0] = out[1] = out[2] = 0.0f;
            break;
    }
}
} // namespace

ParticleEmitterConfig evaluateParticleGraph(const ParticleGraph& graph, std::mt19937& rng)
{
    ParticleEmitterConfig cfg; // defaults match a fresh makeDefault() graph exactly

    const ParticleGraphNode* out = nullptr;
    for (const auto& n : graph.nodes)
        if (n.type == ParticleNodeType::EmitterOutput) { out = &n; break; }
    if (!out) return cfg; // no output node → old-component-compatible defaults

    cfg.emitRate    = evalInputFloat(graph, out->id, 0, rng, 0);
    cfg.lifetimeMin = evalInputFloat(graph, out->id, 1, rng, 0);
    cfg.lifetimeMax = evalInputFloat(graph, out->id, 2, rng, 0);
    cfg.startSize   = evalInputFloat(graph, out->id, 3, rng, 0);
    cfg.endSize     = evalInputFloat(graph, out->id, 4, rng, 0);

    float v[3];
    evalInputVec3(graph, out->id, 5, rng, v, 0);
    cfg.startColor[0] = v[0]; cfg.startColor[1] = v[1]; cfg.startColor[2] = v[2];
    evalInputVec3(graph, out->id, 6, rng, v, 0);
    cfg.endColor[0] = v[0]; cfg.endColor[1] = v[1]; cfg.endColor[2] = v[2];

    cfg.startAlpha = evalInputFloat(graph, out->id, 7, rng, 0);
    cfg.endAlpha   = evalInputFloat(graph, out->id, 8, rng, 0);

    evalInputVec3(graph, out->id, 9, rng, v, 0);
    cfg.initialVelocity[0] = v[0]; cfg.initialVelocity[1] = v[1]; cfg.initialVelocity[2] = v[2];

    cfg.velocitySpread = evalInputFloat(graph, out->id, 10, rng, 0);

    evalInputVec3(graph, out->id, 11, rng, v, 0);
    cfg.gravity[0] = v[0]; cfg.gravity[1] = v[1]; cfg.gravity[2] = v[2];

    cfg.maxParticles = std::max(0, static_cast<int>(std::lround(evalInputFloat(graph, out->id, 12, rng, 0))));
    cfg.looping      = evalInputFloat(graph, out->id, 13, rng, 0) > 0.5f;

    cfg.collisionEnabled = evalInputFloat(graph, out->id, 14, rng, 0) > 0.5f;
    cfg.restitution      = evalInputFloat(graph, out->id, 15, rng, 0);
    cfg.killOnCollision  = evalInputFloat(graph, out->id, 16, rng, 0) > 0.5f;

    cfg.meshAssetId     = out->meshAssetId;
    cfg.materialAssetId = out->materialAssetId;
    return cfg;
}

// ── Shader baking ─────────────────────────────────────────────────────────────

ParticleShaderGen generateParticleShaderSource(const ParticleEmitterConfig& config, bool metalSyntax)
{
    const char* vecType = metalSyntax ? "float3" : "vec3";
    char buf[512];
    ParticleShaderGen gen;
    std::snprintf(buf, sizeof(buf),
        "%s heParticleColor(float t01) { return mix(%s(%.6f, %.6f, %.6f), %s(%.6f, %.6f, %.6f), clamp(t01, 0.0, 1.0)); }",
        vecType, vecType, config.startColor[0], config.startColor[1], config.startColor[2],
        vecType, config.endColor[0], config.endColor[1], config.endColor[2]);
    gen.colorFn = buf;
    std::snprintf(buf, sizeof(buf),
        "float heParticleAlpha(float t01) { return mix(%.6f, %.6f, clamp(t01, 0.0, 1.0)); }",
        config.startAlpha, config.endAlpha);
    gen.alphaFn = buf;
    return gen;
}

} // namespace HE
