// Particle node graph — the authoring model behind a ParticleSystem asset, sharing
// the same GraphEditor canvas as Material/HorizonCode (M3-style: a small standard
// library of nodes feeding one fixed "Emitter Output" sink).
//
// Deliberately SIMPLER than MaterialGraph: particles are CPU-simulated already
// (ParticleSystem::update), so evaluate() walks the graph into a plain
// ParticleEmitterConfig POD once — no shader cross-compile, no codegen. Mesh/
// Material references live as inline node-body slots (UUIDs, like most other
// asset references in the engine — AnimatorComponent::clipAssetId etc. — rather
// than Material's path-based hot-reloadable texture slots, which particles have
// no need for) on the Emitter Output, not as separate pin-fed nodes.
//
// KNOWN LIMITATION (v1): RandomRange resolves ONCE per evaluate() call (e.g. on
// asset edit/(re)load), not per spawned particle — it is a "pick a value, retry
// by re-evaluating" tool, not per-particle variance. Per-particle randomization
// still comes from the existing LifetimeMin/Max + VelocitySpread simulation in
// ParticleSystem::update, which is unchanged.
//
// Deliberately UI-free (lives in HE_Core): the editor's node canvas renders/edits
// this model, tests exercise evaluate() headlessly.
#pragma once

#include <Types/Defines.h> // HE_API
#include <Types/UUID.h>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace HE
{

enum class ParticlePinType : uint8_t { Float, Vec3 };

// The standard node library. Order is serialized by NAME (not enum value).
enum class ParticleNodeType : uint8_t
{
    EmitterOutput, // the fixed sink node (cannot be removed); meshAssetId/materialAssetId
                   // are inline node-body slots, not pins — see ParticleGraphNode
    ConstFloat,    // p[0]
    ConstVec3,     // p[0..2] — plain 3-float (velocity, gravity)
    ConstColor,    // p[0..2] — same shape as ConstVec3, color-picker UI in the editor
    RandomRange,   // p[0]=min, p[1]=max → Float (see KNOWN LIMITATION above)
    Add, Multiply, Lerp, // Lerp: (A, B, Alpha) → mix(A, B, Alpha), per-component for Vec3
};

struct ParticleGraphNode
{
    int              id   = 0;
    ParticleNodeType type = ParticleNodeType::ConstFloat;
    float            p[4] = { 0, 0, 0, 0 }; // node params (const values / range bounds)
    float            x = 0.0f, y = 0.0f;    // canvas position (editor-only, serialized)

    // EmitterOutput-only: drag-drop asset slots in the node body (Content-Browser
    // drop resolves the path to a UUID once, same as e.g. AnimatorComponent's clip
    // slot) rather than connectable pins.
    HE::UUID meshAssetId;
    HE::UUID materialAssetId;
};

// One connection: output pin (srcNode, srcPin) → input pin (dstNode, dstPin).
struct ParticleGraphLink
{
    int srcNode = 0, srcPin = 0;
    int dstNode = 0, dstPin = 0;
};

struct ParticlePinDesc { const char* name; ParticlePinType type; float def; };
struct ParticleNodeDesc
{
    ParticleNodeType type;
    const char* name;     // display + serialization name
    std::vector<ParticlePinDesc> inputs;
    std::vector<ParticlePinDesc> outputs;
    int paramCount;       // how many of p[] the node uses (drives inline widgets)
};

struct HE_API ParticleGraph
{
    std::vector<ParticleGraphNode> nodes;
    std::vector<ParticleGraphLink> links;
    int nextId = 1;

    // Returns the new node's id (NOT a reference — the nodes vector reallocates).
    int addNode(ParticleNodeType type, float x = 0, float y = 0);
    const ParticleGraphNode* findNode(int id) const;
    ParticleGraphNode*       findNode(int id);
    bool connect(int srcNode, int srcPin, int dstNode, int dstPin);
    void disconnectInput(int dstNode, int dstPin);
    void removeNode(int id); // the EmitterOutput node cannot be removed

    static ParticleGraph makeDefault(); // a lone EmitterOutput with sensible defaults
};

HE_API const std::vector<ParticleNodeDesc>& particleNodeRegistry();
HE_API const ParticleNodeDesc&              particleNodeDesc(ParticleNodeType type);
HE_API const ParticleNodeDesc*              particleNodeDescByName(const std::string& name);

HE_API std::string particleGraphToJson(const ParticleGraph& graph);
HE_API bool         particleGraphFromJson(const std::string& json, ParticleGraph& out);

// ── Evaluation ──────────────────────────────────────────────────────────────
// Mirrors ParticleSystemComponent's old inline config fields 1:1 — evaluate()
// replaces "read the component's fields directly" with "read the graph's
// resolved values", everything downstream (ParticleSystem::update) is unchanged.
struct ParticleEmitterConfig
{
    HE::UUID meshAssetId;
    HE::UUID materialAssetId;
    float emitRate       = 10.0f;
    float lifetimeMin    = 1.0f;
    float lifetimeMax    = 2.0f;
    float startSize      = 0.3f;
    float endSize        = 0.0f;
    float startColor[3]  = { 1.0f, 1.0f, 1.0f };
    float endColor[3]    = { 1.0f, 1.0f, 1.0f };
    float startAlpha     = 1.0f;
    float endAlpha       = 0.0f;
    float initialVelocity[3] = { 0.0f, 2.0f, 0.0f };
    float velocitySpread = 0.5f;
    float gravity[3]     = { 0.0f, -2.0f, 0.0f };
    int   maxParticles   = 100;
    bool  looping        = true;
};

HE_API ParticleEmitterConfig evaluateParticleGraph(const ParticleGraph& graph, std::mt19937& rng);

} // namespace HE
