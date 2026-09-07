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

#include <Types/Defines.h> // HE_API — HorizonCore.dll uses explicit exports on Windows
#include <cstdint>
#include <functional>
#include <map>
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
    FnInput,       // function-graph interface: s = name, p[0] = type (0=Float 1=Vec2 2=Vec3 3=Vec4),
                   // p[1] = the value an UNCONNECTED caller pin takes, p[2] = 1 when that
                   // default is authored. The flag is what keeps the number optional: a
                   // graph written before defaults existed has p[1] = p[2] = 0 and must
                   // keep the old blanket 0.5, not silently start reading 0.
                   // One number, splatted across a vector input — a shipped effect wants
                   // "blur by 12 px" to mean something without a wire, not a per-component
                   // literal editor on an interface row.
    FnOutput,      // function-graph interface: s = name, p[0] = type; one input pin
    FunctionCall,  // s = content-relative path of the MaterialFunction asset; pins from its graph

    // ── v4: more inputs ──
    ConstVec2,      // p[0..1]
    ConstVec4,      // p[0..3]
    CameraPos,      // heLight.camPos.xyz
    CameraDistance, // length(camPos - worldPos)
    ScreenPos,      // gl_FragCoord.xy (raw pixels)

    // ── v5: baked constants, more parameter types, logic ──
    ConstBool,      // p[0] = 0/1 (baked literal) → float 0.0/1.0
    ParamVec2,      // named exposed parameter (s = name, p[0..1]) → HeParams .xy
    ParamVec4,      // named exposed parameter (s = name, p[0..3]) → HeParams .xyzw
    ParamBool,      // named exposed parameter (s = name, p[0] = 0/1) → HeParams .x
    // Logic — comparisons/booleans emit 0.0/1.0 floats; If selects branchlessly.
    If,             // (Cond, True, False) → mix(False, True, step(0.5, Cond))
    Greater,        // A > B  → 1/0
    Less,           // A < B  → 1/0
    GreaterEqual,   // A >= B → 1/0
    LessEqual,      // A <= B → 1/0
    Equal,          // |A-B| < eps → 1/0
    NotEqual,       // |A-B| >= eps → 1/0
    And,            // (A>0.5)&&(B>0.5) → 1/0
    Or,             // (A>0.5)||(B>0.5) → 1/0
    Not,            // A<=0.5 → 1/0

    // ── v6: procedural texture ──
    NoiseTexture,   // fbm(vUV*Scale) → grayscale RGB (Vec3) + Value (Float); drop-in to multiply for mottling

    // ── v7: editor ergonomics ──
    Reroute,        // vec4 pass-through pin for tidy link routing (no effect on the shader)

    // ── v8: compile-time permutations ──
    StaticSwitch,   // named COMPILE-TIME branch (s = name, p[0] = default on/off): codegen
                    // emits ONLY the taken input — the other branch is culled entirely.
                    // Material instances may override the value → their own permutation.

    // ── v9: surface features ──
    NormalMapSample,// tangent-space normal map → WORLD-space normal, using a screen-space
                    // cotangent frame (dFdx/dFdy of vWorldPos+vUV, Mikkelsen) — no vertex
                    // tangents needed. s = texture path (like TextureSample), p[0] = strength.

    // ── v10: landscape layers ──
    LandscapeLayerBlend, // one input per named layer, blended by the landscape's painted
                         // weightmap (RGBA8, channel k = layer k). s = '\n'-separated layer
                         // names (these become MaterialAsset::graphLayerNames, which is what
                         // the Landscape paint tool lists). Sampled at the RAW mesh UV — the
                         // weightmap spans the whole terrain, so per-layer detail tiling is
                         // authored with the UV node's Tiling instead of the terrain's.

    // ── v11: the widget under the pixel (docs/he-apps-plan.md D5, Schicht 1) ──
    // What a Surface material reads from the mesh, a UI material reads from the
    // ELEMENT it is drawn on. They take their numbers from one HeUI block, which
    // the UI pass fills per quad; outside MatDomain::UserInterface that block is
    // a compile-time constant (a 1x1 element, no radius, no state), so the same
    // node text compiles in both domains and no second binding exists to collide
    // with the deferred pin table.
    ElementSize,    // the element's rect in PIXELS → Size (Vec2), Width, Height
    ElementUV,      // 0..1 across the element (vUV, untiled) → UV (Vec2)
    RoundedRectSDF, // authored rounded box, centred on the element: signed distance
                    // in pixels (negative inside) + an antialiased Mask
    BorderDistance, // distance from the element's OWN rounded outline, in pixels,
                    // positive inside — the number a border/glow/inner edge wants
    ElementState,   // Hovered / Pressed / Focused / Disabled. 0..1, not 0/1: an
                    // element with a Transition (B8) hands over its BLEND, so a
                    // shipped "button glow" eases in without a single wire.
    Backdrop,       // What was drawn BEHIND this element, blurred: the frosted
                    // glass every modern dialog is made of. A UI-domain node in
                    // the strong sense — outside it there is no pass underneath
                    // to read, and it emits black instead of a sampler.
};

// Layers a single Landscape Layer Blend node can hold — one RGBA8 weightmap
// channel each. More would mean several weightmap textures + shader permutations.
inline constexpr int kMatMaxLandscapeLayers = 4;

// Split a LandscapeLayerBlend node's `s` into its layer names (newline separated,
// blanks dropped, capped at kMatMaxLandscapeLayers). Empty → one "Layer 1".
HE_API std::vector<std::string> matLandscapeLayerNames(const std::string& s);

// Material blend modes (Output node p[1]; → MaterialAsset::blendMode). They change which
// Output pins are meaningful — see matOutputPins:
//   Opaque      — no Opacity pin; alpha forced to 1.
//   Masked      — kMatOutputOpacityPin becomes OpacityMask: fragments below the cutoff
//                 (p[2]) discard. Stays in the OPAQUE pass (no sorting), hard-edged holes.
//   Translucent — kMatOutputOpacityPin is Opacity: drawn in the sorted alpha-blend pass.
enum class MatBlendMode : uint8_t { Opaque = 0, Masked = 1, Translucent = 2 };

// ── Material domain ──────────────────────────────────────────────────────────
// WHERE a material is allowed to be used, and therefore what it may compute.
//
// Surface is a material on geometry in the world: the full PBR set, lit by the
// scene, fogged with it, and available to the deferred G-buffer.
//
// UserInterface is a material on a WIDGET. The UI is drawn in screen space
// after the scene, so there is no lighting to be had, no world position to fog
// against and no G-buffer to write: the graph's colour IS the pixel. Anything
// else is not a style choice but a wrong answer — a lit material on a widget
// used to be shaded by a stand-in sun and fogged as if the pixel it sits on
// were that many world units away. So the domain forces the unlit tail,
// scene-dependent nodes are meaningless in it, and the editor offers UI
// materials only where a widget asks for one.
enum class MatDomain : uint8_t { Surface = 0, UserInterface = 1 };
HE_API const char* matDomainName(MatDomain d);

// Output-node input pin indices (see the Output entry in the node registry).
// Links are stored by pin INDEX, so these are part of the on-disk format: any
// reorder needs a kMatGraphVersion bump plus a remap in materialGraphFromJson.
enum : int {
    kMatOutputBaseColorPin = 0,
    kMatOutputMetallicPin  = 1,
    kMatOutputSpecularPin  = 2,
    kMatOutputRoughnessPin = 3,
    kMatOutputEmissivePin  = 4,
    kMatOutputOpacityPin   = 5,   // Opacity / OpacityMask, per blend mode
    kMatOutputNormalPin    = 6,
    kMatOutputAOPin        = 7,
    kMatOutputWPOPin       = 8,
};

// Serialized graph format version. v2 inserted Specular at pin 2 and Ambient
// Occlusion at pin 7 of the Output node, shifting everything after them.
constexpr int kMatGraphVersion = 2;

struct MatGraphNode
{
    int         id   = 0;
    MatNodeType type = MatNodeType::ConstFloat;
    float       p[4] = { 0, 0, 0, 0 }; // node params (const values / lit flag / fresnel power)
    std::string s;                     // string param (parameter name / function asset path)
    float       x = 0.0f, y = 0.0f;    // canvas position (editor-only, serialized for layout)
    // Param-node METADATA (meaningful on Param* nodes only; optional JSON keys "g"/"tt").
    // ParamFloat additionally uses p[1]/p[2] as slider min/max (min<max → slider UI).
    std::string group;                 // panel grouping header ("" = ungrouped)
    std::string tooltip;               // hover help shown next to the parameter
};

// One connection: output pin (srcNode, srcPin) → input pin (dstNode, dstPin).
// An input pin holds at most one link.
struct MatGraphLink
{
    int srcNode = 0, srcPin = 0;
    int dstNode = 0, dstPin = 0;
};

// Editor-only comment/group box drawn behind the nodes it frames. Purely cosmetic —
// never touches codegen — but serialized with the graph so layouts survive reloads.
// Ids share the graph's `nextId` counter with nodes (uniqueness, not meaning).
struct MatGraphComment
{
    int         id = 0;
    std::string text;                       // header label
    float       x = 0, y = 0;               // graph-space top-left
    float       w = 260.0f, h = 180.0f;     // graph-space size
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

struct HE_API MaterialGraph
{
    std::vector<MatGraphNode>    nodes;
    std::vector<MatGraphLink>    links;
    std::vector<MatGraphComment> comments; // editor-only group boxes (see MatGraphComment)
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
HE_API const std::vector<MatNodeDesc>& matNodeRegistry();
HE_API const MatNodeDesc&              matNodeDesc(MatNodeType type);
HE_API const MatNodeDesc*              matNodeDescByName(const std::string& name);

// The Output node's DISPLAYED input pins for a blend mode, plus the registry pin index
// each row maps to — those indices are exactly the kMatOutput*Pin constants above (named
// rather than spelled out here, so this comment cannot rot when a pin is inserted). They
// stay stable across modes so serialized links never break: Opaque hides
// kMatOutputOpacityPin, Masked renames it to OpacityMask.
HE_API void matOutputPins(int blendMode, std::vector<MatPinDesc>& pins, std::vector<int>& regIndex);

// What an unconnected FunctionCall input was worth before FnInput could carry a
// default (see the FnInput entry): one blanket number for every pin of every type.
// Still the answer for a function whose input declares none.
inline constexpr float kMatFnInputLegacyDefault = 0.5f;

// Interface pins of a FUNCTION graph: its FnInput nodes (sorted by id) become the call
// node's inputs, FnOutput nodes its outputs. Used by codegen and the editor canvas.
// The pin `def` is the FnInput's authored default (or kMatFnInputLegacyDefault).
// NOTE: pin NAMES point into fnGraph's node strings — it must outlive the result.
HE_API void matFunctionPins(const MaterialGraph& fnGraph,
                     std::vector<MatPinDesc>& inputs, std::vector<MatPinDesc>& outputs);

// Resolves a FunctionCall node's asset path to its graph (nullptr = unavailable; the
// call then emits its defaults). The editor backs this with the ContentManager; tests
// with a lambda. Returned pointer must stay valid for the duration of the generate call.
using MatFunctionLoader = std::function<const MaterialGraph*(const std::string& path)>;

// Best-effort CPU approximation of the Output node's BaseColor and Emissive pins,
// for consumers that need a per-instance CONSTANT (the GI ray kernels shade TLAS
// hits with per-instance colours — no material evaluation on the GPU hit).
//
// Two result forms per pin:
//   *Param non-empty — the pin connects (through Reroutes) directly to a Param
//     node: the caller resolves the CURRENT value from the material's param block
//     (live under editor slider drags, setMaterialParam and per-entity overrides;
//     the float[3] holds the node default as fallback).
//   *Param empty — the folded constant (Const*/Param nodes and pure component-wise
//     math: Add/Subtract/Multiply/Divide/Lerp/OneMinus/Saturate folded recursively;
//     Param values fold from the node defaults, i.e. fold-time snapshots).
// The procedural GENERATORS (Value Noise / FBM / Noise Texture / Checker) fold to
// their analytic MEAN rather than failing, so the common `Colour × Noise` mottling
// chain keeps the colour instead of collapsing the whole pin to white.
// Anything genuinely per-texel (textures, UV/time/view inputs …) still does not
// fold: BaseColor falls back to white, Emissive to black — the pre-approx look.
struct MatApproxSurface
{
    float       baseColor[3] = { 1.0f, 1.0f, 1.0f };
    float       emissive[3]  = { 0.0f, 0.0f, 0.0f };
    float       metallic     = 0.0f;  // scalar pins fold constants only (no live slot)
    float       roughness    = 0.5f;
    std::string baseColorParam; // non-empty = read the pin live from this param slot
    std::string emissiveParam;
    // BaseColor driven by a Landscape Layer Blend: each layer folded on its own,
    // in weightmap-channel order. layerCount = 0 → not layer-blended, baseColor is
    // the whole answer. A consumer that knows the terrain's PAINTED weights blends
    // these by them (a green-painted landscape then reflects green); one that does
    // not falls back to baseColor, which holds the flat average of the layers.
    float       layerColor[kMatMaxLandscapeLayers][3] = {};
    int         layerCount = 0;
};
// switchOverrides: a material INSTANCE's StaticSwitch permutation (name → on);
// the fold follows the TAKEN branch exactly like codegen. Null = node defaults.
HE_API MatApproxSurface matGraphApproxSurface(
    const MaterialGraph& g, const std::map<std::string, bool>* switchOverrides = nullptr);

// Widget kind of an exposed parameter — drives typed editors OUTSIDE the node
// canvas (central param panel, entity Details). Serialized per slot (1 byte) in
// MaterialAsset::graphParamTypes, parallel to graphParamNames.
enum class MatParamKind : uint8_t { Float = 0, Color = 1, Vec2 = 2, Vec4 = 3, Bool = 4 };

// How many of the vec4's components a param kind carries (rest stay 0).
inline int matParamKindComponents(MatParamKind k)
{
    switch (k) { case MatParamKind::Vec2: return 2; case MatParamKind::Color: return 3;
                 case MatParamKind::Vec4: return 4; default: return 1; } // Float/Bool → 1
}

// One exposed parameter slot (a vec4 in the HeParams UBO, in slot order).
struct MatParamSlot
{
    std::string  name;
    bool         isColor = false;             // color (xyz) vs scalar (x) — legacy proxy
    MatParamKind kind    = MatParamKind::Float; // full widget type (Float/Color/Vec2/Vec4/Bool)
    float        value[4] = { 0, 0, 0, 0 };
    // Metadata from the Param node: slider range (min<max → slider), panel group, tooltip.
    float        minV = 0.0f, maxV = 0.0f;
    std::string  group;
    std::string  tooltip;
};

struct MatShaderGen
{
    std::string               glsl;     // canonical fragment (→ MaterialAsset::customShaderFragGlsl)
    // Deferred G-buffer variant: SAME graph evaluation (byte-identical body and
    // attribute expressions), but the tail writes base/metallic/normal/roughness/
    // specular/emissive/AO into three MRT outputs (oGB0/oGB1/oGB2) instead of
    // calling heLitP — the lighting happens once, in the fullscreen resolve
    // (MaterialShaderLibrary::deferredResolve). Empty only when glsl is empty.
    // → MaterialAsset::customShaderGBufGlsl (derived, regenerated at load).
    std::string               glslGBuffer;
    std::vector<MatParamSlot> params;   // HeParams layout (→ MaterialAsset::shaderParamData)
    // Content-relative paths of the project textures referenced by Texture Sample nodes,
    // in slot order (heTexP0..heTexP3). → MaterialAsset::graphTexturePaths. Max 4.
    std::vector<std::string>  textures;
    // Static switches reached during codegen: name + the EFFECTIVE value baked into this
    // shader (node default, or the entry from generateFragment's override map).
    std::vector<std::pair<std::string, bool>> switches;
    // Blend mode baked from the Output node (→ MaterialAsset::blendMode; Translucent
    // routes the material into the sorted alpha-blend pass).
    uint8_t blendMode = 0;
    // Domain baked from the Output node (→ MaterialAsset::domain). See MatDomain:
    // a UI material is unlit by construction and never gets a G-buffer variant.
    uint8_t domain = 0;
    // World-Position-Offset vertex BODY (canonical GLSL statements ending in `vec3 heWpo`).
    // Empty when the WPO pin is unconnected → the standard vertex is used. The renderers
    // wrap it into their per-backend vertex template (MaterialShaderLibrary::customVertex).
    std::string vertexBody;
    // Landscape paint layers this material declares, in weightmap-channel order
    // (→ MaterialAsset::graphLayerNames). Non-empty makes it a LANDSCAPE material:
    // the Landscape tool lists exactly these as its paintable layers, so the
    // material — not the terrain — is the single source of truth for what a layer
    // means. Empty for every ordinary material.
    std::vector<std::string> layerNames;
};

// Max project textures a single material graph may sample (fixed so the per-backend
// binding pins stay static).
inline constexpr int kMatMaxGraphTextures = 4;

// Exposed parameters a single material graph may declare — the length of the
// HeParams UBO array (`uniform HeParams { vec4 v[kMatMaxParams]; }`, emitted by
// generateFragment and mirrored in MaterialShaderLibrary's WPO preamble).
// A graph with MORE distinct parameter names bakes the surplus ones as literal
// constants instead of indexing the UBO: the layout used to be clamped only
// AFTER emission, so the generated shader kept reading heParams.v[16] and up —
// out of bounds in the declared array.
inline constexpr int kMatMaxParams = 16;

// Generate shader + parameter layout. Always succeeds (unconnected inputs fall back to
// pin defaults; a missing Output node yields a magenta error shader; recursive function
// calls emit magenta instead of hanging). `switchOverrides` (name → on) replaces Static
// Switch node defaults at COMPILE time — each distinct combination yields a distinct
// shader source (its own pipeline-cache entry), which is the whole permutation system.
HE_API MatShaderGen generateFragment(const MaterialGraph& graph, const MatFunctionLoader& loader = {},
                              const std::map<std::string, bool>* switchOverrides = nullptr);

// Convenience wrapper (no function loader) — kept for existing callers/tests.
HE_API std::string generateFragmentGlsl(const MaterialGraph& graph);

// JSON (de)serialization — stored in MaterialAsset::nodeGraphJson /
// MaterialFunctionAsset::nodeGraphJson. Node types serialized by NAME.
HE_API std::string materialGraphToJson(const MaterialGraph& graph);
HE_API bool        materialGraphFromJson(const std::string& json, MaterialGraph& out);

// ── Item-level JSON ─────────────────────────────────────────────────────────
// One node / one comment, in EXACTLY the form materialGraphToJson() puts into
// the document's arrays — the document serializers are implemented on top of
// these. Collaboration addresses a single item at a time (CollabDocSync); a
// separate serializer for it would drift from the on-disk one. false = the JSON
// does not describe a usable item (an unknown node type), which callers skip.
//
// Links have no item form: they are already a 4-int object (GraphJson.h) and are
// identified by their endpoints, not by an id.
HE_API std::string matNodeToJson(const MatGraphNode& n);
HE_API bool        matNodeFromJson(const std::string& json, MatGraphNode& out);
HE_API std::string matCommentToJson(const MatGraphComment& c);
HE_API bool        matCommentFromJson(const std::string& json, MatGraphComment& out);
} // namespace HE
