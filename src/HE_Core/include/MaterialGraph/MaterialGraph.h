// Material node graph — the authoring model behind a Material (M3, Unreal-style).
//
// The graph is the SOURCE OF TRUTH stored in the MaterialAsset; generateFragment()
// turns it into the canonical-GLSL fragment (the same customShaderFragGlsl the whole
// M0–M2 pipeline consumes: cross-compiled per backend, cached per hash, standard-lit via
// the heLit() preamble) plus the exposed-parameter layout (a std140 HeParams UBO the
// engine uploads per material — parameter edits never recompile the shader). Shaders are
// NOT a user-facing asset type — users only ever edit graphs; the engine generates and
// applies the shaders.
//
// Material FUNCTIONS are reusable sub-graphs stored as their own assets
// (MaterialFunctionAsset): a graph whose interface is its Function Input / Function
// Output nodes. A material references one via a FunctionCall node (s = content-relative
// asset path); codegen INLINES the function body (recursion-guarded), so functions cost
// nothing at runtime.
//
// Deliberately UI-free (lives in HE_Core): the editor's node canvas renders/edits this
// model, tests exercise codegen headlessly.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace HE
{
// Pin value types. Coercion rules in codegen: Float promotes to vectors by splat;
// vec3→vec4 appends alpha 1; vecN→vecM otherwise truncates / zero-extends.
enum class MatPinType : uint8_t { Float, Vec2, Vec3, Vec4 };

// The standard node library. Order is serialized by NAME (not enum value), so the enum
// may be reordered/extended freely.
enum class MatNodeType : uint8_t
{
    Output,        // material output: BaseColor, Metallic, Roughness, Emissive, Opacity; p[0] = lit
    ConstFloat,    // p[0] = value
    ConstColor,    // p[0..2] = rgb
    VertexColor,   // vColor (the material's baseColor tint fed through the vertex)
    NormalWS,      // normalized world-space normal
    UV,            // mesh UV (vUV)
    Time,          // engine time in seconds (heLight.sunDir.w)
    TextureSample, // texture slot 0 sampled at UV input → RGB + A
    Add, Multiply, Lerp, OneMinus, Power, Saturate, DotProduct, Sine,
    Fresnel,       // pow(1 - max(dot(N, V), 0), p[0]) with the TRUE per-pixel view direction
    Combine3,      // (x, y, z) → vec3

    WorldPos,      // world-space fragment position (vWorldPos)
    ViewDir,       // normalize(camera - worldPos)
    ParamFloat,    // named exposed parameter (s = name, p[0] = value) → HeParams uniform
    ParamColor,    // named exposed parameter (s = name, p[0..2] = rgb) → HeParams uniform
    Subtract, Divide, Absolute, Fract, Smoothstep, Step, Normalize3,
    Panner,        // UV + vec2(SpeedX, SpeedY) * time
    ValueNoise,    // value noise (UV, Scale) → float
    Fbm,           // 4-octave fractal noise (UV, Scale) → float
    Checker,       // checkerboard (UV, Scale) → float

    // ── v3: channels + functions ──
    SplitRGBA,     // vec4 → R, G, B, A
    CombineRGBA,   // (R, G, B, A) → vec4
    FnInput,       // function-graph interface: s = name, p[0] = type (0=Float 1=Vec2 2=Vec3 3=Vec4)
    FnOutput,      // function-graph interface: s = name, p[0] = type; one input pin
    FunctionCall,  // s = content-relative path of the MaterialFunction asset; pins from its graph

    // ── v4: more inputs ──
    ConstVec2,      // p[0..1]
    ConstVec4,      // p[0..3]
    CameraPos,      // heLight.camPos.xyz
    CameraDistance, // length(camPos - worldPos)
    ScreenPos,      // gl_FragCoord.xy (raw pixels)
};

struct MatGraphNode
{
    int         id   = 0;
    MatNodeType type = MatNodeType::ConstFloat;
    float       p[4] = { 0, 0, 0, 0 }; // node params (const values / lit flag / fresnel power)
    std::string s;                     // string param (parameter name / function asset path)
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
// the editor UI (node palette, pin layout), so the two can never disagree. FunctionCall
// nodes have DYNAMIC pins (from the referenced function graph) — see matFunctionPins.
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
    bool connect(int srcNode, int srcPin, int dstNode, int dstPin);
    void disconnectInput(int dstNode, int dstPin);
    void removeNode(int id); // the (material) Output node cannot be removed

    static MaterialGraph makeDefault();         // material: Output + ConstColor
    static MaterialGraph makeDefaultFunction(); // function: FnInput → FnOutput
};

// Node-type registry (the standard library). Stable lookup by enum or serialized name.
const std::vector<MatNodeDesc>& matNodeRegistry();
const MatNodeDesc&              matNodeDesc(MatNodeType type);
const MatNodeDesc*              matNodeDescByName(const std::string& name);

// Interface pins of a FUNCTION graph: its FnInput nodes (sorted by id) become the call
// node's inputs, FnOutput nodes its outputs. Used by codegen and the editor canvas.
void matFunctionPins(const MaterialGraph& fnGraph,
                     std::vector<MatPinDesc>& inputs, std::vector<MatPinDesc>& outputs);

// Resolves a FunctionCall node's asset path to its graph (nullptr = unavailable; the
// call then emits its defaults). The editor backs this with the ContentManager; tests
// with a lambda. Returned pointer must stay valid for the duration of the generate call.
using MatFunctionLoader = std::function<const MaterialGraph*(const std::string& path)>;

// One exposed parameter slot (a vec4 in the HeParams UBO, in slot order).
struct MatParamSlot
{
    std::string name;
    bool        isColor = false;      // color (xyz) vs scalar (x)
    float       value[4] = { 0, 0, 0, 0 };
};

struct MatShaderGen
{
    std::string               glsl;     // canonical fragment (→ MaterialAsset::customShaderFragGlsl)
    std::vector<MatParamSlot> params;   // HeParams layout (→ MaterialAsset::shaderParamData)
    // Content-relative paths of the project textures referenced by Texture Sample nodes,
    // in slot order (heTexP0..heTexP3). → MaterialAsset::graphTexturePaths. Max 4.
    std::vector<std::string>  textures;
};

// Max project textures a single material graph may sample (fixed so the per-backend
// binding pins stay static).
inline constexpr int kMatMaxGraphTextures = 4;

// Generate shader + parameter layout. Always succeeds (unconnected inputs fall back to
// pin defaults; a missing Output node yields a magenta error shader; recursive function
// calls emit magenta instead of hanging).
MatShaderGen generateFragment(const MaterialGraph& graph, const MatFunctionLoader& loader = {});

// Convenience wrapper (no function loader) — kept for existing callers/tests.
std::string generateFragmentGlsl(const MaterialGraph& graph);

// JSON (de)serialization — stored in MaterialAsset::nodeGraphJson /
// MaterialFunctionAsset::nodeGraphJson. Node types serialized by NAME.
std::string materialGraphToJson(const MaterialGraph& graph);
bool        materialGraphFromJson(const std::string& json, MaterialGraph& out);
} // namespace HE
