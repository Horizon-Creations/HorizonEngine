// Material node graph — the authoring model behind a Material (M3, Unreal-style).
//
// The graph is the SOURCE OF TRUTH stored in the MaterialAsset; generateFragmentGlsl()
// turns it into the canonical-GLSL fragment (the same customShaderFragGlsl the whole
// M0–M2 pipeline consumes: cross-compiled per backend, cached per hash, standard-lit via
// the heLit() preamble). Shaders are NOT a user-facing asset type — users only ever edit
// this graph; the engine generates and applies the shaders.
//
// Deliberately UI-free (lives in HE_Core): the editor's node canvas renders/edits this
// model, tests exercise codegen headlessly.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace HE
{
// Pin value types. Coercion rules in codegen: Float promotes to Vec2/Vec3 by splat;
// anything else must match exactly.
enum class MatPinType : uint8_t { Float, Vec2, Vec3 };

// The standard node library (v1). Order is serialized by NAME (not enum value), so the
// enum may be reordered/extended freely.
enum class MatNodeType : uint8_t
{
    Output,        // material output: BaseColor, Metallic, Roughness, Emissive; p[0] = lit (1) / unlit (0)
    ConstFloat,    // p[0] = value
    ConstColor,    // p[0..2] = rgb
    VertexColor,   // vColor (the material's baseColor tint fed through the vertex)
    NormalWS,      // normalized world-space normal
    UV,            // mesh UV (vUV)
    Time,          // engine time in seconds (heLight.sunDir.w)
    TextureSample, // texture slot 0 sampled at UV input
    Add, Multiply, Lerp, OneMinus, Power, Saturate, DotProduct, Sine,
    Fresnel,       // pow(1 - max(dot(N, view), 0), p[0]); view ≈ +Z (matches heLit's model)
    Combine3,      // (x, y, z) → vec3
};

struct MatGraphNode
{
    int         id   = 0;
    MatNodeType type = MatNodeType::ConstFloat;
    float       p[4] = { 0, 0, 0, 0 }; // node params (const values / lit flag / fresnel power)
    float       x = 0.0f, y = 0.0f;    // canvas position (editor-only, serialized for layout)
};

// One connection: output pin (srcNode, srcPin) → input pin (dstNode, dstPin).
// An input pin holds at most one link.
struct MatGraphLink
{
    int srcNode = 0, srcPin = 0;
    int dstNode = 0, dstPin = 0;
};

// Static description of a node type (name, pins, defaults) — drives both the codegen and
// the editor UI (node palette, pin layout), so the two can never disagree.
struct MatPinDesc  { const char* name; MatPinType type; float def; };
struct MatNodeDesc
{
    MatNodeType type;
    const char* name;       // display + serialization name
    const char* category;   // palette grouping
    std::vector<MatPinDesc> inputs;
    std::vector<MatPinDesc> outputs;
    int         paramCount; // how many of p[] the node uses (drives inline widgets)
};

struct MaterialGraph
{
    std::vector<MatGraphNode> nodes;
    std::vector<MatGraphLink> links;
    int nextId = 1;

    // Returns the new node's id (NOT a reference — the nodes vector reallocates).
    int addNode(MatNodeType type, float x = 0, float y = 0);
    const MatGraphNode* findNode(int id) const;
    MatGraphNode*       findNode(int id);
    // Connect src output pin → dst input pin (replaces any existing link into that input).
    // Rejects self-links and type-incompatible pins; returns success.
    bool connect(int srcNode, int srcPin, int dstNode, int dstPin);
    void disconnectInput(int dstNode, int dstPin);
    void removeNode(int id); // also removes its links; the Output node cannot be removed

    // A minimal default graph: Output + ConstColor wired into BaseColor.
    static MaterialGraph makeDefault();
};

// Node-type registry (the standard library). Stable lookup by enum or serialized name.
const std::vector<MatNodeDesc>& matNodeRegistry();
const MatNodeDesc&              matNodeDesc(MatNodeType type);
const MatNodeDesc*              matNodeDescByName(const std::string& name);

// Generate the canonical-GLSL fragment for this graph. Always succeeds (unconnected
// inputs fall back to pin defaults; a missing Output node yields a magenta error shader).
// The result plugs directly into MaterialAsset::customShaderFragGlsl.
std::string generateFragmentGlsl(const MaterialGraph& graph);

// JSON (de)serialization — stored in MaterialAsset::nodeGraphJson. Node types are
// serialized by NAME so the enum can evolve. fromJson returns false on parse errors
// (out left untouched).
std::string materialGraphToJson(const MaterialGraph& graph);
bool        materialGraphFromJson(const std::string& json, MaterialGraph& out);
} // namespace HE
