#include "MaterialGraph/MaterialGraph.h"
#include <cstdint>

#include <GraphCommon/GraphJson.h>
#include <GraphCommon/GraphModel.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace HE
{
namespace
{
using F = MatPinType;

// ── The standard node library ──────────────────────────────────────────────────
const std::vector<MatNodeDesc>& registry()
{
    static const std::vector<MatNodeDesc> kReg = {
        // Surface-output pins, ordered like Unreal's material output so the
        // muscle memory carries over. ANY change to this order needs a bump of
        // kMatGraphVersion + a remap in materialGraphFromJson: links are stored
        // by pin INDEX, so a reorder silently rewires every saved material.
        { MatNodeType::Output, "Output", "Material",
          { { "Base Color", F::Vec3, 0.8f },   // 0
            { "Metallic", F::Float, 0.0f },    // 1
            { "Specular", F::Float, 0.5f },    // 2 — dielectric F0 = 0.08 * this (0.5 → 0.04)
            { "Roughness", F::Float, 0.5f },   // 3
            { "Emissive", F::Vec3, 0.0f },     // 4
            { "Opacity", F::Float, 1.0f },     // 5 — meaning depends on the blend mode
            { "Normal", F::Vec3, 0.0f },       // 6 — WORLD-space; unconnected = vNormal
            { "Ambient Occlusion", F::Float, 1.0f }, // 7 — scales the ambient term only
            { "World Position Offset", F::Vec3, 0.0f } }, // 8 — vertex stage
          {}, 3 }, // p[0] = lit, p[1] = blend mode, p[2] = mask cutoff
        { MatNodeType::ConstFloat, "Float", "Constant",
          {}, { { "Value", F::Float, 0 } }, 1 },
        { MatNodeType::ConstColor, "Color", "Constant",
          {}, { { "RGB", F::Vec3, 0 } }, 3 },
        { MatNodeType::VertexColor, "Vertex Color", "Input",
          {}, { { "RGB", F::Vec3, 0 } }, 0 },
        { MatNodeType::NormalWS, "Normal (WS)", "Input",
          {}, { { "N", F::Vec3, 0 } }, 0 },
        // p[0..1] = tiling (how often the texture repeats over the 0..1 range),
        // p[2..3] = offset. Defaults 1/0 → the raw mesh UV, as before.
        { MatNodeType::UV, "UV", "Input",
          {}, { { "UV", F::Vec2, 0 } }, 4 },
        { MatNodeType::Time, "Time", "Input",
          {}, { { "Seconds", F::Float, 0 } }, 0 },
        { MatNodeType::TextureSample, "Texture Sample", "Texture",
          { { "UV", F::Vec2, 0 } }, { { "RGB", F::Vec3, 0 }, { "A", F::Float, 0 } }, 0 },
        { MatNodeType::Add, "Add", "Math",
          { { "A", F::Vec3, 0 }, { "B", F::Vec3, 0 } }, { { "Out", F::Vec3, 0 } }, 0 },
        { MatNodeType::Multiply, "Multiply", "Math",
          { { "A", F::Vec3, 1 }, { "B", F::Vec3, 1 } }, { { "Out", F::Vec3, 0 } }, 0 },
        { MatNodeType::Lerp, "Lerp", "Math",
          { { "A", F::Vec3, 0 }, { "B", F::Vec3, 1 }, { "T", F::Float, 0.5f } },
          { { "Out", F::Vec3, 0 } }, 0 },
        { MatNodeType::OneMinus, "One Minus", "Math",
          { { "X", F::Vec3, 0 } }, { { "Out", F::Vec3, 0 } }, 0 },
        { MatNodeType::Power, "Power", "Math",
          { { "Base", F::Float, 1 }, { "Exp", F::Float, 2 } }, { { "Out", F::Float, 0 } }, 0 },
        { MatNodeType::Saturate, "Saturate", "Math",
          { { "X", F::Vec3, 0 } }, { { "Out", F::Vec3, 0 } }, 0 },
        { MatNodeType::DotProduct, "Dot", "Math",
          { { "A", F::Vec3, 0 }, { "B", F::Vec3, 0 } }, { { "Out", F::Float, 0 } }, 0 },
        { MatNodeType::Sine, "Sine", "Math",
          { { "X", F::Float, 0 } }, { { "Out", F::Float, 0 } }, 0 },
        { MatNodeType::Fresnel, "Fresnel", "Shading",
          {}, { { "F", F::Float, 0 } }, 1 }, // p[0] = power
        { MatNodeType::Combine3, "Combine XYZ", "Channels",
          { { "X", F::Float, 0 }, { "Y", F::Float, 0 }, { "Z", F::Float, 0 } },
          { { "RGB", F::Vec3, 0 } }, 0 },

        { MatNodeType::WorldPos, "World Position", "Input",
          {}, { { "XYZ", F::Vec3, 0 } }, 0 },
        { MatNodeType::ViewDir, "View Direction", "Input",
          {}, { { "V", F::Vec3, 0 } }, 0 },
        { MatNodeType::ParamFloat, "Param (Float)", "Parameter",
          {}, { { "Value", F::Float, 0 } }, 1 },
        { MatNodeType::ParamColor, "Param (Color)", "Parameter",
          {}, { { "RGB", F::Vec3, 0 } }, 3 },
        { MatNodeType::Subtract, "Subtract", "Math",
          { { "A", F::Vec3, 0 }, { "B", F::Vec3, 0 } }, { { "Out", F::Vec3, 0 } }, 0 },
        { MatNodeType::Divide, "Divide", "Math",
          { { "A", F::Vec3, 1 }, { "B", F::Vec3, 1 } }, { { "Out", F::Vec3, 0 } }, 0 },
        { MatNodeType::Absolute, "Abs", "Math",
          { { "X", F::Vec3, 0 } }, { { "Out", F::Vec3, 0 } }, 0 },
        { MatNodeType::Fract, "Fract", "Math",
          { { "X", F::Vec3, 0 } }, { { "Out", F::Vec3, 0 } }, 0 },
        { MatNodeType::Smoothstep, "Smoothstep", "Math",
          { { "Edge0", F::Float, 0 }, { "Edge1", F::Float, 1 }, { "X", F::Float, 0 } },
          { { "Out", F::Float, 0 } }, 0 },
        { MatNodeType::Step, "Step", "Math",
          { { "Edge", F::Float, 0.5f }, { "X", F::Float, 0 } }, { { "Out", F::Float, 0 } }, 0 },
        { MatNodeType::Normalize3, "Normalize", "Math",
          { { "V", F::Vec3, 0 } }, { { "Out", F::Vec3, 0 } }, 0 },
        { MatNodeType::Panner, "Panner", "Texture",
          { { "UV", F::Vec2, 0 }, { "SpeedX", F::Float, 0.1f }, { "SpeedY", F::Float, 0 } },
          { { "UV", F::Vec2, 0 } }, 0 },
        { MatNodeType::ValueNoise, "Noise", "Procedural",
          { { "UV", F::Vec2, 0 }, { "Scale", F::Float, 8 } }, { { "Out", F::Float, 0 } }, 0 },
        { MatNodeType::Fbm, "FBM Noise", "Procedural",
          { { "UV", F::Vec2, 0 }, { "Scale", F::Float, 8 } }, { { "Out", F::Float, 0 } }, 0 },
        { MatNodeType::Checker, "Checker", "Procedural",
          { { "UV", F::Vec2, 0 }, { "Scale", F::Float, 8 } }, { { "Out", F::Float, 0 } }, 0 },

        // ── v3 ──
        { MatNodeType::SplitRGBA, "Split RGBA", "Channels",
          { { "RGBA", F::Vec4, 0 } },
          { { "R", F::Float, 0 }, { "G", F::Float, 0 }, { "B", F::Float, 0 }, { "A", F::Float, 0 } }, 0 },
        { MatNodeType::CombineRGBA, "Combine RGBA", "Channels",
          { { "R", F::Float, 0 }, { "G", F::Float, 0 }, { "B", F::Float, 0 }, { "A", F::Float, 1 } },
          { { "RGBA", F::Vec4, 0 } }, 0 },
        { MatNodeType::FnInput, "Function Input", "Function",
          {}, { { "Value", F::Vec3, 0 } }, 1 }, // s = name, p[0] = type; output type is dynamic
        { MatNodeType::FnOutput, "Function Output", "Function",
          { { "Value", F::Vec3, 0 } }, {}, 1 }, // s = name, p[0] = type; input type is dynamic
        { MatNodeType::FunctionCall, "Material Function", "Function",
          {}, {}, 0 }, // pins resolved from the referenced graph (matFunctionPins)

        // ── v4 inputs ──
        { MatNodeType::ConstVec2, "Vector2", "Constant",
          {}, { { "XY", F::Vec2, 0 } }, 2 },
        { MatNodeType::ConstVec4, "Vector4", "Constant",
          {}, { { "XYZW", F::Vec4, 0 } }, 4 },
        { MatNodeType::CameraPos, "Camera Position", "Input",
          {}, { { "XYZ", F::Vec3, 0 } }, 0 },
        { MatNodeType::CameraDistance, "Camera Distance", "Input",
          {}, { { "Dist", F::Float, 0 } }, 0 },
        { MatNodeType::ScreenPos, "Screen Position", "Input",
          {}, { { "XY", F::Vec2, 0 } }, 0 },

        // ── v5: baked constants, parameter types, logic ──
        { MatNodeType::ConstBool, "Bool", "Constant",
          {}, { { "Out", F::Float, 0 } }, 1 }, // p[0] = 0/1
        { MatNodeType::ParamVec2, "Param (Vector2)", "Parameter",
          {}, { { "XY", F::Vec2, 0 } }, 2 },
        { MatNodeType::ParamVec4, "Param (Vector4)", "Parameter",
          {}, { { "XYZW", F::Vec4, 0 } }, 4 },
        { MatNodeType::ParamBool, "Param (Bool)", "Parameter",
          {}, { { "Out", F::Float, 0 } }, 1 }, // p[0] = 0/1
        { MatNodeType::If, "If", "Logic",
          { { "Cond", F::Float, 0 }, { "True", F::Vec3, 1 }, { "False", F::Vec3, 0 } },
          { { "Out", F::Vec3, 0 } }, 0 },
        { MatNodeType::Greater, "Greater (A>B)", "Logic",
          { { "A", F::Float, 0 }, { "B", F::Float, 0 } }, { { "Out", F::Float, 0 } }, 0 },
        { MatNodeType::Less, "Less (A<B)", "Logic",
          { { "A", F::Float, 0 }, { "B", F::Float, 0 } }, { { "Out", F::Float, 0 } }, 0 },
        { MatNodeType::GreaterEqual, "Greater or Equal", "Logic",
          { { "A", F::Float, 0 }, { "B", F::Float, 0 } }, { { "Out", F::Float, 0 } }, 0 },
        { MatNodeType::LessEqual, "Less or Equal", "Logic",
          { { "A", F::Float, 0 }, { "B", F::Float, 0 } }, { { "Out", F::Float, 0 } }, 0 },
        { MatNodeType::Equal, "Equal", "Logic",
          { { "A", F::Float, 0 }, { "B", F::Float, 0 } }, { { "Out", F::Float, 0 } }, 0 },
        { MatNodeType::NotEqual, "Not Equal", "Logic",
          { { "A", F::Float, 0 }, { "B", F::Float, 0 } }, { { "Out", F::Float, 0 } }, 0 },
        { MatNodeType::And, "And", "Logic",
          { { "A", F::Float, 0 }, { "B", F::Float, 0 } }, { { "Out", F::Float, 0 } }, 0 },
        { MatNodeType::Or, "Or", "Logic",
          { { "A", F::Float, 0 }, { "B", F::Float, 0 } }, { { "Out", F::Float, 0 } }, 0 },
        { MatNodeType::Not, "Not", "Logic",
          { { "X", F::Float, 0 } }, { { "Out", F::Float, 0 } }, 0 },

        // ── v6: procedural texture ──
        // No input pins → always samples the mesh UV, so it "just works": drop it in,
        // set Scale, wire RGB into a Multiply to mottle a colour.
        { MatNodeType::NoiseTexture, "Noise Texture", "Procedural",
          {}, { { "RGB", F::Vec3, 0 }, { "Value", F::Float, 0 } }, 1 }, // p[0] = Scale

        // ── v7: editor ergonomics ──
        // Vec4 pass-through preserves every channel through the coercion rules
        // (float→vec4 splat / vec3→vec4 alpha-1 round-trip cleanly back down).
        { MatNodeType::Reroute, "Reroute", "Misc",
          { { "", F::Vec4, 0 } }, { { "", F::Vec4, 0 } }, 0 },

        // ── v8: compile-time permutations ──
        // The UNTAKEN branch is culled at codegen time (never emitted), unlike the
        // runtime If node. s = switch name, p[0] = default; instances may override.
        { MatNodeType::StaticSwitch, "Static Switch", "Logic",
          { { "True", F::Vec3, 1 }, { "False", F::Vec3, 0 } }, { { "Out", F::Vec3, 0 } }, 1 },

        // ── v9: surface features ──
        { MatNodeType::NormalMapSample, "Normal Map", "Texture",
          { { "UV", F::Vec2, 0 } }, { { "N", F::Vec3, 0 } }, 1 }, // p[0] = strength, s = texture
        // Layer inputs are DYNAMIC (one per name in `s`) — see matLandscapeLayerPins;
        // the registry entry only carries the single output + param count.
        { MatNodeType::LandscapeLayerBlend, "Landscape Layer Blend", "Landscape",
          {}, { { "Blended", F::Vec3, 0 } }, 0 },
    };
    return kReg;
}

std::string fmtF(float v)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

const char* typeName(MatPinType t)
{
    switch (t)
    {
        case F::Float: return "float";
        case F::Vec2:  return "vec2";
        case F::Vec3:  return "vec3";
        case F::Vec4:  return "vec4";
    }
    return "float";
}

MatPinType pinTypeFromParam(float p)
{
    switch (static_cast<int>(p))
    {
        case 0: return F::Float;
        case 1: return F::Vec2;
        case 3: return F::Vec4;
        default: return F::Vec3;
    }
}

// Coerce an expression of type `from` to `to`. Float splats up; vec3→vec4 appends
// alpha 1 (color semantics); other narrowing/widening truncates / zero-extends.
std::string coerce(const std::string& expr, MatPinType from, MatPinType to)
{
    if (from == to) return expr;
    if (from == F::Float) return std::string(typeName(to)) + "(" + expr + ")";
    if (to == F::Float)   return "(" + expr + ").x";
    if (from == F::Vec2 && to == F::Vec3) return "vec3(" + expr + ", 0.0)";
    if (from == F::Vec2 && to == F::Vec4) return "vec4(" + expr + ", 0.0, 1.0)";
    if (from == F::Vec3 && to == F::Vec2) return "(" + expr + ").xy";
    if (from == F::Vec3 && to == F::Vec4) return "vec4(" + expr + ", 1.0)";
    if (from == F::Vec4 && to == F::Vec2) return "(" + expr + ").xy";
    /* Vec4 → Vec3 */     return "(" + expr + ").xyz";
}

std::string defaultExpr(const MatPinDesc& pin)
{
    switch (pin.type)
    {
        case F::Float: return fmtF(pin.def);
        case F::Vec2:  return "vec2(" + fmtF(pin.def) + ")";
        case F::Vec3:  return "vec3(" + fmtF(pin.def) + ")";
        case F::Vec4:  return "vec4(vec3(" + fmtF(pin.def) + "), 1.0)";
    }
    return "0.0";
}
} // namespace

const std::vector<MatNodeDesc>& matNodeRegistry() { return registry(); }

const MatNodeDesc& matNodeDesc(MatNodeType type)
{
    for (const auto& d : registry())
        if (d.type == type) return d;
    return registry().front();
}

const MatNodeDesc* matNodeDescByName(const std::string& name)
{
    for (const auto& d : registry())
        if (name == d.name) return &d;
    return nullptr;
}

void matOutputPins(int blendMode, std::vector<MatPinDesc>& pins, std::vector<int>& regIndex)
{
    const auto& reg = matNodeDesc(MatNodeType::Output).inputs;
    pins.clear(); regIndex.clear();
    for (int i = 0; i < (int)reg.size(); ++i)
    {
        if (i == kMatOutputOpacityPin) // the opacity slot is blend-mode dependent
        {
            if (blendMode == (int)MatBlendMode::Masked)
                pins.push_back({ "OpacityMask", F::Float, 1.0f });
            else if (blendMode == (int)MatBlendMode::Translucent)
                pins.push_back({ "Opacity", F::Float, 1.0f });
            else
                continue; // Opaque: hidden (alpha is forced to 1)
        }
        else
            pins.push_back(reg[i]);
        regIndex.push_back(i);
    }
}

std::vector<std::string> matLandscapeLayerNames(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        // Trim — a name is a UI label AND the paint-tool key, so stray spaces
        // would make two layers that look identical compare unequal.
        size_t b = cur.find_first_not_of(" \t\r");
        size_t e = cur.find_last_not_of(" \t\r");
        if (b != std::string::npos && (int)out.size() < kMatMaxLandscapeLayers)
            out.push_back(cur.substr(b, e - b + 1));
        cur.clear();
    };
    for (char ch : s) { if (ch == '\n') flush(); else cur.push_back(ch); }
    flush();
    if (out.empty()) out.push_back("Layer 1");
    return out;
}

// A material function's interface pins, in PIN ORDER. The order is the FnInput /
// FnOutput nodes sorted by node id — i.e. creation order — because there is no
// other authored ordering on a canvas, and links are stored by pin INDEX, so the
// order must be stable for a given saved graph. Every place that resolves a
// function's interface (matFunctionPins for the editor/pin metadata, the FnInput
// case that maps a pin back onto the caller's argument, and the FunctionCall case
// that inlines the k-th FnOutput) MUST use this one ordering or a call would wire
// its arguments to the wrong parameters.
static std::vector<const MatGraphNode*> fnInterfaceNodes(const MaterialGraph& g, MatNodeType kind)
{
    std::vector<const MatGraphNode*> out;
    for (const auto& n : g.nodes)
        if (n.type == kind) out.push_back(&n);
    std::sort(out.begin(), out.end(),
              [](const MatGraphNode* a, const MatGraphNode* b){ return a->id < b->id; });
    return out;
}

void matFunctionPins(const MaterialGraph& fnGraph,
                     std::vector<MatPinDesc>& inputs, std::vector<MatPinDesc>& outputs)
{
    inputs.clear(); outputs.clear();
    const std::vector<const MatGraphNode*> ins  = fnInterfaceNodes(fnGraph, MatNodeType::FnInput);
    const std::vector<const MatGraphNode*> outs = fnInterfaceNodes(fnGraph, MatNodeType::FnOutput);
    for (const auto* n : ins)
        inputs.push_back({ n->s.c_str(), pinTypeFromParam(n->p[0]), 0.0f });
    for (const auto* n : outs)
        outputs.push_back({ n->s.c_str(), pinTypeFromParam(n->p[0]), 0.0f });
}

int MaterialGraph::addNode(MatNodeType type, float x, float y)
{
    MatGraphNode n;
    n.type = type;
    n.x = x; n.y = y;
    if (type == MatNodeType::UV)         { n.p[0] = n.p[1] = 1.0f; }          // tiling 1, offset 0
    if (type == MatNodeType::Output)     n.p[0] = 1.0f;                       // lit
    if (type == MatNodeType::ConstColor) { n.p[0] = n.p[1] = n.p[2] = 0.8f; }
    if (type == MatNodeType::ConstFloat) n.p[0] = 1.0f;
    if (type == MatNodeType::Fresnel)    n.p[0] = 3.0f;
    if (type == MatNodeType::ParamFloat) { n.p[0] = 1.0f; n.s = "MyParam"; }
    if (type == MatNodeType::ParamColor) { n.p[0] = n.p[1] = n.p[2] = 0.8f; n.s = "MyColor"; }
    if (type == MatNodeType::FnInput)    { n.p[0] = 2.0f; n.s = "In";  } // vec3
    if (type == MatNodeType::FnOutput)   { n.p[0] = 2.0f; n.s = "Out"; }
    // v5: baked constants + more parameter types
    if (type == MatNodeType::ConstBool)  n.p[0] = 1.0f;
    if (type == MatNodeType::ParamVec2)  { n.p[0] = n.p[1] = 0.0f; n.s = "MyVec2"; }
    if (type == MatNodeType::ParamVec4)  { n.p[0] = n.p[1] = n.p[2] = n.p[3] = 0.0f; n.s = "MyVec4"; }
    if (type == MatNodeType::ParamBool)  { n.p[0] = 1.0f; n.s = "MyBool"; }
    // v6: procedural texture
    if (type == MatNodeType::NoiseTexture) n.p[0] = 6.0f;                     // Scale
    // v8: compile-time switch
    if (type == MatNodeType::StaticSwitch) { n.p[0] = 1.0f; n.s = "MySwitch"; }
    // v9: normal map strength; Output mask cutoff (p[1] blend mode stays 0 = Opaque)
    if (type == MatNodeType::NormalMapSample) n.p[0] = 1.0f;
    // v10: a fresh layer-blend node starts with two named layers.
    if (type == MatNodeType::LandscapeLayerBlend) n.s = "Layer 1\nLayer 2";
    if (type == MatNodeType::Output) n.p[2] = 0.5f;
    return HE::graph::appendNode(nodes, nextId, std::move(n));
}

const MatGraphNode* MaterialGraph::findNode(int id) const
{ return HE::graph::findNodeById(nodes, id); }
MatGraphNode* MaterialGraph::findNode(int id)
{ return HE::graph::findNodeById(nodes, id); }

bool MaterialGraph::connect(int srcNode, int srcPin, int dstNode, int dstPin)
{
    if (srcNode == dstNode) return false;
    const MatGraphNode* s = findNode(srcNode);
    const MatGraphNode* d = findNode(dstNode);
    if (!s || !d) return false;
    // Pin-range checks use the static registry; FunctionCall pins are dynamic, so accept
    // any index for them (the editor creates links only on real pins; codegen falls back
    // to defaults for out-of-range indices).
    if (s->type != MatNodeType::FunctionCall)
    {
        const MatNodeDesc& sd = matNodeDesc(s->type);
        if (srcPin < 0 || srcPin >= (int)sd.outputs.size()) return false;
    }
    if (d->type == MatNodeType::LandscapeLayerBlend)
    {
        // Dynamic inputs: one per name in `s`, so validate against that count.
        if (dstPin < 0 || dstPin >= (int)matLandscapeLayerNames(d->s).size()) return false;
    }
    else if (d->type != MatNodeType::FunctionCall)
    {
        const MatNodeDesc& dd = matNodeDesc(d->type);
        if (dstPin < 0 || dstPin >= (int)dd.inputs.size()) return false;
    }
    disconnectInput(dstNode, dstPin); // an input pin holds at most one link
    links.push_back({ srcNode, srcPin, dstNode, dstPin });
    return true;
}

void MaterialGraph::disconnectInput(int dstNode, int dstPin)
{ HE::graph::disconnectInput(links, dstNode, dstPin); }

void MaterialGraph::removeNode(int id)
{
    const MatGraphNode* n = findNode(id);
    if (!n || n->type == MatNodeType::Output) return; // fixed sink
    HE::graph::removeNodeAndLinks(nodes, links, id);
}

MaterialGraph MaterialGraph::makeDefault()
{
    MaterialGraph g;
    const int out = g.addNode(MatNodeType::Output, 380, 120);
    const int col = g.addNode(MatNodeType::ConstColor, 80, 120);
    g.connect(col, 0, out, 0);
    return g;
}

MaterialGraph MaterialGraph::makeDefaultFunction()
{
    MaterialGraph g;
    const int in  = g.addNode(MatNodeType::FnInput, 80, 120);
    const int out = g.addNode(MatNodeType::FnOutput, 380, 120);
    g.connect(in, 0, out, 0);
    return g;
}

// ── Codegen ────────────────────────────────────────────────────────────────────
namespace
{
struct Scope; // fwd

struct EmitCtx
{
    std::string body;
    std::unordered_map<std::string, std::string> outVar; // scopeKey:node:pin → expr
    std::unordered_set<std::string> emitting;            // cycle guard (scoped)
    bool usesTexture = false;                            // legacy default sampler (heTex0)
    const std::map<std::string, bool>* switchOv = nullptr;    // instance permutation values
    std::vector<std::pair<std::string, bool>> switches;       // switches reached (effective)
    bool usesNoise   = false;                            // 2D value-noise/fbm helpers (UV-space)
    bool usesNoise3  = false;                            // 3D value-noise/fbm helpers (world-space)
    bool usesNormalPerturb = false;                      // hePerturbNormal (screen-space TBN)
    // Landscape layer blending: the fragment declares heLandscapeWeights and the
    // material advertises its layer names (order = weightmap channel order).
    bool usesLandscapeWeights = false;
    std::vector<std::string> layerNames;
    int  varCounter  = 0;
    std::vector<MatParamSlot> params;                    // exposed parameters, slot order
    std::vector<std::string>  textures;                  // project textures, slot order (max 4)
    const MatFunctionLoader*  loader = nullptr;
    std::vector<std::string>  fnStack;                   // inline stack (recursion guard)
};

// One inline instance of a graph. The root scope is the material graph; each
// FunctionCall opens a child scope whose FnInput nodes resolve against the call
// node's inputs in the PARENT scope.
struct Scope
{
    const MaterialGraph* g        = nullptr;
    std::string          key;                // unique per inline instance
    const Scope*         parent   = nullptr;
    const MatGraphNode*  callNode = nullptr; // the FunctionCall node in the parent scope
};

std::string inputExpr(EmitCtx& c, const Scope& sc, const MatGraphNode& node,
                      int pinIdx, MatPinType wantType);
bool        hasInput(const Scope& sc, const MatGraphNode& node, int pinIdx);
std::string uvInput(EmitCtx& c, const Scope& sc, const MatGraphNode& n, int pinIdx);

// Allocate (or find) the HeParams slot for a named parameter. `kind` drives how many
// of the vec4's components carry the default value (rest stay 0) AND the typed widget
// shown outside the canvas. Same name reuses the slot so repeated Param nodes share
// one uniform.
//
// Returns -1 when the graph is OVER the kMatMaxParams budget; the caller then bakes
// the node's authored default as a literal instead. This check has to happen HERE,
// before emission: the layout used to be clamped only after the shader text was
// built, so an over-budget graph emitted heParams.v[16] (and up) against a
// `vec4 v[16]` array — an out-of-bounds read in the generated shader.
int paramSlot(EmitCtx& c, const MatGraphNode& n, MatParamKind kind)
{
    const std::string name = n.s.empty() ? ("param_" + std::to_string(n.id)) : n.s;
    for (size_t i = 0; i < c.params.size(); ++i)
        if (c.params[i].name == name)
            return (int)i;
    if ((int)c.params.size() >= kMatMaxParams) return -1;
    const int keep = matParamKindComponents(kind);
    MatParamSlot slot;
    slot.name = name;
    slot.kind = kind;
    slot.isColor = (kind == MatParamKind::Color);
    for (int i = 0; i < 4; ++i) slot.value[i] = (i < keep) ? n.p[i] : 0.0f;
    // Metadata for typed editors: group/tooltip from the node; ParamFloat carries a
    // slider range in p[1]/p[2] (min < max → slider UI instead of a free drag).
    slot.group   = n.group;
    slot.tooltip = n.tooltip;
    if (kind == MatParamKind::Float) { slot.minV = n.p[1]; slot.maxV = n.p[2]; }
    c.params.push_back(std::move(slot));
    return (int)c.params.size() - 1;
}

// Resolve the sampler name a texture-reading node should use. A node with no
// picked texture (empty s) samples the material's legacy/mesh texture (heTex0).
// A picked project texture gets its own slot heTexP{k} (deduplicated by path,
// capped at kMatMaxGraphTextures); extras fall back to heTex0.
// Shared by Texture Sample and Normal Map Sample — they must allocate out of the
// SAME slot table, or two nodes sampling the same file would claim two bindings.
std::string textureSampler(EmitCtx& c, const MatGraphNode& n)
{
    if (n.s.empty()) { c.usesTexture = true; return "heTex0"; }
    int slot = -1;
    for (size_t i = 0; i < c.textures.size(); ++i)
        if (c.textures[i] == n.s) { slot = (int)i; break; }
    if (slot < 0 && (int)c.textures.size() < kMatMaxGraphTextures)
    { slot = (int)c.textures.size(); c.textures.push_back(n.s); }
    if (slot < 0) { c.usesTexture = true; return "heTex0"; } // over budget → default
    return "heTexP" + std::to_string(slot);
}

// Sampling expression for a SEPARATE texture: the graph's textures are declared
// `uniform texture2D` (not `sampler2D`) and combined with one of the two shared
// samplers the lighting preamble declares — see the "two SHARED samplers" note in
// MaterialShaderLibrary's kLightingPreamble for why.
//
// In short: a combined `sampler2D` burns a D3D sampler register PER TEXTURE, and
// shader model 5.0 has only sixteen. That is what stopped a LANDSCAPE material
// (heLandscapeWeights makes seventeen) from compiling on D3D at all. Separate
// textures let all of them share one SamplerState. GLSL 4.10 has no separate
// type, so the cross-compiler recombines them back into `sampler2D` and names the
// result after the TEXTURE — which is why these names must never change: OpenGL
// binds these uniforms BY NAME.
//
// `heSampWrap` (linear + REPEAT) for the graph's own textures: a UV node's Tiling
// is meaningless without repeat. The landscape weightmap uses `heSampClamp`
// instead — it spans the whole terrain exactly once, and wrapping it would fold
// the far edge back over the near one.
std::string sampleTex(const std::string& tex, const std::string& samp, const std::string& uv)
{
    return "texture(sampler2D(" + tex + ", " + samp + "), " + uv + ")";
}

// Emit one node (memoized per scope); returns the expression for output pin `pin`.
std::string emitNode(EmitCtx& c, const Scope& sc, const MatGraphNode& n, int pin)
{
    const std::string memoKey = sc.key + ":" + std::to_string(n.id) + ":" + std::to_string(pin);
    if (auto it = c.outVar.find(memoKey); it != c.outVar.end()) return it->second;
    const std::string cycleKey = sc.key + ":" + std::to_string(n.id);
    if (c.emitting.count(cycleKey))
        return "vec3(1.0, 0.0, 1.0)"; // cycle → magenta
    c.emitting.insert(cycleKey);

    const MatNodeDesc& d = matNodeDesc(n.type);
    const std::string v = "n" + std::to_string(++c.varCounter);
    std::string decl;
    // Per-pin result expressions; default = the single variable for every pin.
    std::vector<std::string> pinExpr;

    switch (n.type)
    {
        case MatNodeType::ConstFloat:
            decl = "float " + v + " = " + fmtF(n.p[0]) + ";"; break;
        case MatNodeType::ConstColor:
            decl = "vec3 " + v + " = vec3(" + fmtF(n.p[0]) + ", " + fmtF(n.p[1]) + ", " + fmtF(n.p[2]) + ");"; break;
        case MatNodeType::VertexColor:
            decl = "vec3 " + v + " = vColor;"; break;
        case MatNodeType::NormalWS:
            decl = "vec3 " + v + " = normalize(vNormal);"; break;
        case MatNodeType::UV:
            // Tiling/offset baked in as literals — the whole point is that a
            // texture can repeat over a surface instead of stretching once.
            // Identity params emit the bare varying so old graphs stay byte-equal.
            if (n.p[0] == 1.0f && n.p[1] == 1.0f && n.p[2] == 0.0f && n.p[3] == 0.0f)
                decl = "vec2 " + v + " = vUV;";
            else
                decl = "vec2 " + v + " = vUV * vec2(" + fmtF(n.p[0]) + ", " + fmtF(n.p[1])
                     + ") + vec2(" + fmtF(n.p[2]) + ", " + fmtF(n.p[3]) + ");";
            break;
        case MatNodeType::Time:
            decl = "float " + v + " = heLight.sunDir.w;"; break;
        case MatNodeType::TextureSample:
        {
            const std::string sampler = textureSampler(c, n);
            decl = "vec4 " + v + " = "
                 + sampleTex(sampler, "heSampWrap", inputExpr(c, sc, n, 0, F::Vec2)) + ";";
            pinExpr = { v + ".xyz", v + ".w" };
            break;
        }
        case MatNodeType::NormalMapSample:
        {
            // Tangent-space normal map → world space WITHOUT vertex tangents: build the
            // cotangent frame from screen-space derivatives of position+UV (Mikkelsen).
            // Same slot machinery as Texture Sample (empty s → the mesh texture).
            const std::string sampler = textureSampler(c, n);
            c.usesNormalPerturb = true;
            const float strength = n.p[0] > 0.0f ? n.p[0] : 1.0f;
            decl = "vec2 " + v + "_uv = " + uvInput(c, sc, n, 0) + ";"
                 + " vec3 " + v + "_t = " + sampleTex(sampler, "heSampWrap", v + "_uv")
                 + ".xyz * 2.0 - 1.0;"
                 + " " + v + "_t.xy *= " + fmtF(strength) + ";"
                 + " vec3 " + v + " = hePerturbNormal(normalize(vNormal), normalize(" + v
                 + "_t), vWorldPos, " + v + "_uv);";
            break;
        }
        case MatNodeType::LandscapeLayerBlend:
        {
            // One input per named layer, weighted by the landscape's painted
            // weightmap: channel k = layer k. Sampled at the RAW vUV, which spans
            // the whole terrain — per-layer detail tiling belongs on each layer's
            // own UV node, not here, or the weights would tile with the detail.
            //
            // The weights are normalised, so a half-painted texel does not darken.
            // An UNPAINTED terrain binds the 1x1 (1,0,0,0) default weightmap, which
            // resolves to layer 0 at full strength rather than to black or to the
            // average of every layer.
            const std::vector<std::string> names = matLandscapeLayerNames(n.s);
            c.usesLandscapeWeights = true;
            if (c.layerNames.empty()) c.layerNames = names;

            static const char* kChan[kMatMaxLandscapeLayers] = { "x", "y", "z", "w" };
            std::string sum, wsum;
            for (size_t i = 0; i < names.size(); ++i)
            {
                const std::string w = v + "_w." + kChan[i];
                sum  += (i ? " + " : "") + inputExpr(c, sc, n, (int)i, F::Vec3) + " * " + w;
                wsum += (i ? " + " : "") + w;
            }
            decl = "vec4 " + v + "_w = "
                 + sampleTex("heLandscapeWeights", "heSampClamp", "vUV") + ";"
                 + " float " + v + "_s = max(" + wsum + ", 1e-4);"
                 + " vec3 " + v + " = (" + sum + ") / " + v + "_s;";
            break;
        }
        case MatNodeType::Add:
            decl = "vec3 " + v + " = " + inputExpr(c, sc, n, 0, F::Vec3) + " + " + inputExpr(c, sc, n, 1, F::Vec3) + ";"; break;
        case MatNodeType::Multiply:
            decl = "vec3 " + v + " = " + inputExpr(c, sc, n, 0, F::Vec3) + " * " + inputExpr(c, sc, n, 1, F::Vec3) + ";"; break;
        case MatNodeType::Lerp:
            decl = "vec3 " + v + " = mix(" + inputExpr(c, sc, n, 0, F::Vec3) + ", " + inputExpr(c, sc, n, 1, F::Vec3)
                 + ", " + inputExpr(c, sc, n, 2, F::Float) + ");"; break;
        case MatNodeType::OneMinus:
            decl = "vec3 " + v + " = vec3(1.0) - " + inputExpr(c, sc, n, 0, F::Vec3) + ";"; break;
        case MatNodeType::Power:
            decl = "float " + v + " = pow(max(" + inputExpr(c, sc, n, 0, F::Float) + ", 0.0), " + inputExpr(c, sc, n, 1, F::Float) + ");"; break;
        case MatNodeType::Saturate:
            decl = "vec3 " + v + " = clamp(" + inputExpr(c, sc, n, 0, F::Vec3) + ", 0.0, 1.0);"; break;
        case MatNodeType::DotProduct:
            decl = "float " + v + " = dot(" + inputExpr(c, sc, n, 0, F::Vec3) + ", " + inputExpr(c, sc, n, 1, F::Vec3) + ");"; break;
        case MatNodeType::Sine:
            decl = "float " + v + " = sin(" + inputExpr(c, sc, n, 0, F::Float) + ");"; break;
        case MatNodeType::Fresnel:
            decl = "float " + v + " = pow(1.0 - max(dot(normalize(vNormal), "
                   "normalize(heLight.camPos.xyz - vWorldPos)), 0.0), "
                 + fmtF(std::max(n.p[0], 0.01f)) + ");"; break;
        case MatNodeType::Combine3:
            decl = "vec3 " + v + " = vec3(" + inputExpr(c, sc, n, 0, F::Float) + ", "
                 + inputExpr(c, sc, n, 1, F::Float) + ", " + inputExpr(c, sc, n, 2, F::Float) + ");"; break;

        case MatNodeType::WorldPos:
            decl = "vec3 " + v + " = vWorldPos;"; break;
        case MatNodeType::ViewDir:
            decl = "vec3 " + v + " = normalize(heLight.camPos.xyz - vWorldPos);"; break;
        case MatNodeType::ParamFloat:
        {
            // Over budget (slot < 0) → bake the authored default; see paramSlot.
            const int slot = paramSlot(c, n, MatParamKind::Float);
            decl = "float " + v + " = "
                 + (slot < 0 ? fmtF(n.p[0]) : "heParams.v[" + std::to_string(slot) + "].x")
                 + "; // param: " + (n.s.empty() ? "?" : n.s);
            break;
        }
        case MatNodeType::ParamColor:
        {
            const int slot = paramSlot(c, n, MatParamKind::Color);
            decl = "vec3 " + v + " = "
                 + (slot < 0 ? "vec3(" + fmtF(n.p[0]) + ", " + fmtF(n.p[1]) + ", " + fmtF(n.p[2]) + ")"
                             : "heParams.v[" + std::to_string(slot) + "].xyz")
                 + "; // param: " + (n.s.empty() ? "?" : n.s);
            break;
        }
        case MatNodeType::Subtract:
            decl = "vec3 " + v + " = " + inputExpr(c, sc, n, 0, F::Vec3) + " - " + inputExpr(c, sc, n, 1, F::Vec3) + ";"; break;
        case MatNodeType::Divide:
            decl = "vec3 " + v + " = " + inputExpr(c, sc, n, 0, F::Vec3) + " / max("
                 + inputExpr(c, sc, n, 1, F::Vec3) + ", vec3(1e-5));"; break;
        case MatNodeType::Absolute:
            decl = "vec3 " + v + " = abs(" + inputExpr(c, sc, n, 0, F::Vec3) + ");"; break;
        case MatNodeType::Fract:
            decl = "vec3 " + v + " = fract(" + inputExpr(c, sc, n, 0, F::Vec3) + ");"; break;
        case MatNodeType::Smoothstep:
            decl = "float " + v + " = smoothstep(" + inputExpr(c, sc, n, 0, F::Float) + ", "
                 + inputExpr(c, sc, n, 1, F::Float) + ", " + inputExpr(c, sc, n, 2, F::Float) + ");"; break;
        case MatNodeType::Step:
            decl = "float " + v + " = step(" + inputExpr(c, sc, n, 0, F::Float) + ", "
                 + inputExpr(c, sc, n, 1, F::Float) + ");"; break;
        case MatNodeType::Normalize3:
            decl = "vec3 " + v + " = normalize(" + inputExpr(c, sc, n, 0, F::Vec3) + ");"; break;
        case MatNodeType::Panner:
            decl = "vec2 " + v + " = " + inputExpr(c, sc, n, 0, F::Vec2) + " + vec2("
                 + inputExpr(c, sc, n, 1, F::Float) + ", " + inputExpr(c, sc, n, 2, F::Float)
                 + ") * heLight.sunDir.w;"; break;
        case MatNodeType::ValueNoise:
            c.usesNoise = true;
            decl = "float " + v + " = heValueNoise(" + uvInput(c, sc, n, 0) + " * "
                 + inputExpr(c, sc, n, 1, F::Float) + ");"; break;
        case MatNodeType::Fbm:
            c.usesNoise = true;
            decl = "float " + v + " = heFbm(" + uvInput(c, sc, n, 0) + " * "
                 + inputExpr(c, sc, n, 1, F::Float) + ");"; break;
        case MatNodeType::Checker:
        {
            const std::string uv = uvInput(c, sc, n, 0);
            const std::string sc2 = inputExpr(c, sc, n, 1, F::Float);
            decl = "float " + v + " = mod(floor(" + uv + ".x * " + sc2 + ") + floor("
                 + uv + ".y * " + sc2 + "), 2.0);"; break;
        }
        case MatNodeType::StaticSwitch:
        {
            // COMPILE-TIME branch: resolve the value now (override map beats the node
            // default; the FIRST resolution wins for repeated names so one switch can
            // gate several spots consistently), then emit ONLY the taken input. The
            // untaken branch is never visited — dead nodes cost nothing in the shader.
            const std::string swName = n.s.empty() ? ("switch_" + std::to_string(n.id)) : n.s;
            bool on = n.p[0] > 0.5f;
            if (c.switchOv)
                if (auto it = c.switchOv->find(swName); it != c.switchOv->end()) on = it->second;
            bool seen = false;
            for (const auto& sw : c.switches)
                if (sw.first == swName) { on = sw.second; seen = true; break; }
            if (!seen) c.switches.push_back({ swName, on });
            decl = "vec3 " + v + " = " + inputExpr(c, sc, n, on ? 0 : 1, F::Vec3) + ";";
            break;
        }
        case MatNodeType::Reroute:
            // Editor-only routing pin: emit a plain pass-through so downstream coercion
            // sees exactly what was fed in (memoized like any node, so no duplication).
            decl = "vec4 " + v + " = " + inputExpr(c, sc, n, 0, F::Vec4) + ";"; break;
        case MatNodeType::NoiseTexture:
        {
            // Self-contained procedural texture: 3D fbm over WORLD-SPACE position, no input
            // pins. World position (not UV) so it works on ANY mesh — a cube with no UVs
            // has vUV = 0 everywhere, which would collapse UV noise to a single value.
            // Output as grayscale RGB (multiply against a colour → mottling) and raw Value.
            c.usesNoise3 = true;
            const float scale = n.p[0] > 0.01f ? n.p[0] : 0.01f;
            decl = "float " + v + " = heFbm3(vWorldPos * " + fmtF(scale) + ");";
            pinExpr = { "vec3(" + v + ")", v };
            break;
        }

        // ── v3 ──
        case MatNodeType::SplitRGBA:
            decl = "vec4 " + v + " = " + inputExpr(c, sc, n, 0, F::Vec4) + ";";
            pinExpr = { v + ".x", v + ".y", v + ".z", v + ".w" };
            break;
        case MatNodeType::CombineRGBA:
            decl = "vec4 " + v + " = vec4(" + inputExpr(c, sc, n, 0, F::Float) + ", "
                 + inputExpr(c, sc, n, 1, F::Float) + ", " + inputExpr(c, sc, n, 2, F::Float) + ", "
                 + inputExpr(c, sc, n, 3, F::Float) + ");"; break;
        case MatNodeType::FnInput:
        {
            // Inside a function scope: resolve to the matching call-node input in the
            // parent scope. At root (editing a function standalone) → typed default.
            const MatPinType t = pinTypeFromParam(n.p[0]);
            if (sc.parent && sc.callNode)
            {
                const std::vector<const MatGraphNode*> ins =
                    fnInterfaceNodes(*sc.g, MatNodeType::FnInput);
                int idx = 0;
                for (size_t i = 0; i < ins.size(); ++i) if (ins[i]->id == n.id) idx = (int)i;
                const std::string src = inputExpr(c, *sc.parent, *sc.callNode, idx, t);
                decl = std::string(typeName(t)) + " " + v + " = " + src + ";";
            }
            else
                decl = std::string(typeName(t)) + " " + v + " = " + std::string(typeName(t)) + "(0.5);";
            break;
        }
        case MatNodeType::FnOutput:
        {
            const MatPinType t = pinTypeFromParam(n.p[0]);
            decl = std::string(typeName(t)) + " " + v + " = " + inputExpr(c, sc, n, 0, t) + ";";
            break;
        }
        case MatNodeType::FunctionCall:
        {
            // Inline the referenced function graph: pin k = its k-th FnOutput.
            const MaterialGraph* fn = (c.loader && *c.loader) ? (*c.loader)(n.s) : nullptr;
            const bool recursing = std::find(c.fnStack.begin(), c.fnStack.end(), n.s) != c.fnStack.end();
            if (!fn || recursing)
            {
                decl = "vec3 " + v + " = vec3(1.0, 0.0, 1.0); // "
                     + std::string(recursing ? "recursive" : "missing") + " function: " + n.s;
                break;
            }
            const std::vector<const MatGraphNode*> outs =
                fnInterfaceNodes(*fn, MatNodeType::FnOutput);
            if (outs.empty())
            {
                decl = "vec3 " + v + " = vec3(1.0, 0.0, 1.0); // function has no output: " + n.s;
                break;
            }
            c.fnStack.push_back(n.s);
            Scope child;
            child.g = fn;
            child.key = sc.key + "/" + std::to_string(n.id);
            child.parent = &sc;
            child.callNode = &n;
            pinExpr.resize(outs.size());
            for (size_t k = 0; k < outs.size(); ++k)
                pinExpr[k] = emitNode(c, child, *outs[k], 0);
            c.fnStack.pop_back();
            decl = ""; // outputs are the inlined FnOutput vars — no own declaration
            break;
        }

        // ── v4 inputs ──
        case MatNodeType::ConstVec2:
            decl = "vec2 " + v + " = vec2(" + fmtF(n.p[0]) + ", " + fmtF(n.p[1]) + ");"; break;
        case MatNodeType::ConstVec4:
            decl = "vec4 " + v + " = vec4(" + fmtF(n.p[0]) + ", " + fmtF(n.p[1]) + ", "
                 + fmtF(n.p[2]) + ", " + fmtF(n.p[3]) + ");"; break;
        case MatNodeType::CameraPos:
            decl = "vec3 " + v + " = heLight.camPos.xyz;"; break;
        case MatNodeType::CameraDistance:
            decl = "float " + v + " = length(heLight.camPos.xyz - vWorldPos);"; break;
        case MatNodeType::ScreenPos:
            decl = "vec2 " + v + " = gl_FragCoord.xy;"; break;

        // ── v5: baked constants, parameter types, logic ──
        case MatNodeType::ConstBool:
            decl = "float " + v + " = " + (n.p[0] > 0.5f ? "1.0" : "0.0") + ";"; break;
        case MatNodeType::ParamVec2:
        {
            const int slot = paramSlot(c, n, MatParamKind::Vec2);
            decl = "vec2 " + v + " = "
                 + (slot < 0 ? "vec2(" + fmtF(n.p[0]) + ", " + fmtF(n.p[1]) + ")"
                             : "heParams.v[" + std::to_string(slot) + "].xy")
                 + "; // param: " + (n.s.empty() ? "?" : n.s);
            break;
        }
        case MatNodeType::ParamVec4:
        {
            const int slot = paramSlot(c, n, MatParamKind::Vec4);
            decl = "vec4 " + v + " = "
                 + (slot < 0 ? "vec4(" + fmtF(n.p[0]) + ", " + fmtF(n.p[1]) + ", "
                                       + fmtF(n.p[2]) + ", " + fmtF(n.p[3]) + ")"
                             : "heParams.v[" + std::to_string(slot) + "]")
                 + "; // param: " + (n.s.empty() ? "?" : n.s);
            break;
        }
        case MatNodeType::ParamBool:
        {
            const int slot = paramSlot(c, n, MatParamKind::Bool);
            // Threshold so a bool param reads cleanly as 0.0/1.0 even if set to e.g. 0.7.
            // Over budget the threshold is applied at bake time (same as ConstBool).
            decl = "float " + v + " = "
                 + (slot < 0 ? std::string(n.p[0] > 0.5f ? "1.0" : "0.0")
                             : "step(0.5, heParams.v[" + std::to_string(slot) + "].x)")
                 + "; // param: " + (n.s.empty() ? "?" : n.s);
            break;
        }
        case MatNodeType::If:
            decl = "vec3 " + v + " = mix(" + inputExpr(c, sc, n, 2, F::Vec3) + ", "
                 + inputExpr(c, sc, n, 1, F::Vec3) + ", step(0.5, "
                 + inputExpr(c, sc, n, 0, F::Float) + "));"; break;
        case MatNodeType::Greater:
            decl = "float " + v + " = float(" + inputExpr(c, sc, n, 0, F::Float) + " > "
                 + inputExpr(c, sc, n, 1, F::Float) + ");"; break;
        case MatNodeType::Less:
            decl = "float " + v + " = float(" + inputExpr(c, sc, n, 0, F::Float) + " < "
                 + inputExpr(c, sc, n, 1, F::Float) + ");"; break;
        case MatNodeType::GreaterEqual:
            decl = "float " + v + " = float(" + inputExpr(c, sc, n, 0, F::Float) + " >= "
                 + inputExpr(c, sc, n, 1, F::Float) + ");"; break;
        case MatNodeType::LessEqual:
            decl = "float " + v + " = float(" + inputExpr(c, sc, n, 0, F::Float) + " <= "
                 + inputExpr(c, sc, n, 1, F::Float) + ");"; break;
        case MatNodeType::Equal:
            decl = "float " + v + " = float(abs(" + inputExpr(c, sc, n, 0, F::Float) + " - "
                 + inputExpr(c, sc, n, 1, F::Float) + ") < 1e-4);"; break;
        case MatNodeType::NotEqual:
            decl = "float " + v + " = float(abs(" + inputExpr(c, sc, n, 0, F::Float) + " - "
                 + inputExpr(c, sc, n, 1, F::Float) + ") >= 1e-4);"; break;
        case MatNodeType::And:
            decl = "float " + v + " = float(" + inputExpr(c, sc, n, 0, F::Float) + " > 0.5 && "
                 + inputExpr(c, sc, n, 1, F::Float) + " > 0.5);"; break;
        case MatNodeType::Or:
            decl = "float " + v + " = float(" + inputExpr(c, sc, n, 0, F::Float) + " > 0.5 || "
                 + inputExpr(c, sc, n, 1, F::Float) + " > 0.5);"; break;
        case MatNodeType::Not:
            decl = "float " + v + " = float(" + inputExpr(c, sc, n, 0, F::Float) + " <= 0.5);"; break;

        case MatNodeType::Output:
            decl = ""; break; // handled by generateFragment
    }

    if (!decl.empty())
        c.body += "    " + decl + "\n";
    c.emitting.erase(cycleKey);

    // Register per-pin expressions (default: the single var for every output pin).
    const size_t pinCount = std::max<size_t>(d.outputs.size(), pinExpr.size());
    for (size_t i = 0; i < std::max<size_t>(pinCount, 1); ++i)
    {
        const std::string expr = i < pinExpr.size() && !pinExpr[i].empty() ? pinExpr[i] : v;
        c.outVar[sc.key + ":" + std::to_string(n.id) + ":" + std::to_string(i)] = expr;
    }
    return c.outVar[memoKey];
}

// Type of output pin `pin` of node `n` (dynamic for FunctionCall/FnInput).
MatPinType outputPinType(EmitCtx& c, const MatGraphNode& n, int pin)
{
    if (n.type == MatNodeType::FnInput) return pinTypeFromParam(n.p[0]);
    if (n.type == MatNodeType::FunctionCall)
    {
        const MaterialGraph* fn = (c.loader && *c.loader) ? (*c.loader)(n.s) : nullptr;
        if (fn)
        {
            std::vector<MatPinDesc> ins, outs;
            matFunctionPins(*fn, ins, outs);
            if (pin >= 0 && pin < (int)outs.size()) return outs[pin].type;
        }
        return F::Vec3;
    }
    const MatNodeDesc& d = matNodeDesc(n.type);
    if (pin >= 0 && pin < (int)d.outputs.size()) return d.outputs[pin].type;
    return F::Vec3;
}

std::string inputExpr(EmitCtx& c, const Scope& sc, const MatGraphNode& node,
                      int pinIdx, MatPinType wantType)
{
    for (const MatGraphLink& l : sc.g->links)
    {
        if (l.dstNode != node.id || l.dstPin != pinIdx) continue;
        const MatGraphNode* src = sc.g->findNode(l.srcNode);
        if (!src) break;
        const std::string expr = emitNode(c, sc, *src, l.srcPin);
        return coerce(expr, outputPinType(c, *src, l.srcPin), wantType);
    }
    // Unconnected: the static pin default (FunctionCall inputs: typed 0.5 default).
    if (node.type == MatNodeType::FunctionCall)
        return coerce("0.5", F::Float, wantType);
    const MatNodeDesc& d = matNodeDesc(node.type);
    if (pinIdx >= 0 && pinIdx < (int)d.inputs.size())
    {
        const MatPinDesc& pin = d.inputs[pinIdx];
        return coerce(defaultExpr(pin), pin.type, wantType);
    }
    return coerce("0.0", F::Float, wantType);
}

// True if input pin `pinIdx` of `node` is wired to a source in this scope's graph.
bool hasInput(const Scope& sc, const MatGraphNode& node, int pinIdx)
{
    for (const MatGraphLink& l : sc.g->links)
        if (l.dstNode == node.id && l.dstPin == pinIdx) return true;
    return false;
}

// A UV input that falls back to the MESH UV (vUV) when unconnected, instead of the
// numeric pin default (which was vec2(0) → constant noise = uniform, "just dark").
// This is what makes procedural nodes vary across the surface out of the box.
std::string uvInput(EmitCtx& c, const Scope& sc, const MatGraphNode& n, int pinIdx)
{
    return hasInput(sc, n, pinIdx) ? inputExpr(c, sc, n, pinIdx, F::Vec2) : std::string("vUV");
}
} // namespace

MatShaderGen generateFragment(const MaterialGraph& graph, const MatFunctionLoader& loader,
                              const std::map<std::string, bool>* switchOverrides)
{
    const MatGraphNode* out = nullptr;
    for (const auto& n : graph.nodes)
        if (n.type == MatNodeType::Output) { out = &n; break; }

    const std::string headerCommon =
        "#version 450\n"
        "// GENERATED by the material node graph — do not edit by hand.\n"
        "layout(location = 0) in vec3 vNormal;\n"
        "layout(location = 1) in vec3 vColor;\n"
        "layout(location = 2) in vec2 vUV;\n"
        "layout(location = 3) in vec3 vWorldPos;\n";
    const std::string header = headerCommon + "layout(location = 0) out vec4 oColor;\n";
    // Deferred variant: three MRT outputs instead of the lit colour — the SAME
    // graph body feeds both, only the emit tail differs (see the plan's §2).
    const std::string headerGB = headerCommon +
        "layout(location = 0) out vec4 oGB0;\n"   // rgb = BaseColor, a = Metallic
        "layout(location = 1) out vec4 oGB1;\n"   // rg = oct normal, b = Roughness, a = Specular
        "layout(location = 2) out vec4 oGB2;\n"   // rgb = Emissive (HDR), a = Material-AO
        // NDC depth for the tile-memory resolve (Metal single-pass P6, which can
        // framebuffer-fetch colour attachments but not the depth buffer). GL
        // binds only 3 draw buffers → the write is dropped there; the two-pass
        // Metal fallback stores it unused (R32F, DontCare).
        "layout(location = 3) out vec4 oGB3;\n";

    MatShaderGen gen;
    if (!out)
    {
        gen.glsl = header + "void main() { oColor = vec4(1.0, 0.0, 1.0, 1.0); } // no Output node\n";
        gen.glslGBuffer = headerGB +
            "void main() { oGB0 = vec4(1.0, 0.0, 1.0, 0.0);"
            " oGB1 = vec4(0.5, 0.5, 1.0, 0.5); oGB2 = vec4(0.0);"
            " oGB3 = vec4(gl_FragCoord.z, 0.0, 0.0, 0.0); } // no Output node\n";
        return gen;
    }

    EmitCtx c;
    c.loader   = &loader;
    c.switchOv = switchOverrides;
    Scope root;
    root.g = &graph;

    const std::string base    = inputExpr(c, root, *out, kMatOutputBaseColorPin, F::Vec3);
    const std::string met     = inputExpr(c, root, *out, kMatOutputMetallicPin,  F::Float);
    const std::string spec    = inputExpr(c, root, *out, kMatOutputSpecularPin,  F::Float);
    const std::string rough   = inputExpr(c, root, *out, kMatOutputRoughnessPin, F::Float);
    const std::string emis    = inputExpr(c, root, *out, kMatOutputEmissivePin,  F::Vec3);
    const std::string ao      = inputExpr(c, root, *out, kMatOutputAOPin,        F::Float);
    const bool lit = out->p[0] > 0.5f;

    // ── Blend mode (Output p[1]) decides what pin 4 means and where alpha comes from. ──
    const int   blendMode = std::clamp(static_cast<int>(out->p[1]), 0, 2);
    const float cutoff    = out->p[2] > 0.0f ? out->p[2] : 0.5f; // mask threshold
    std::string opacity   = "1.0";                                // Opaque/Masked → solid
    std::string mask;
    if (blendMode == (int)MatBlendMode::Masked)
        mask = inputExpr(c, root, *out, kMatOutputOpacityPin, F::Float);
    else if (blendMode == (int)MatBlendMode::Translucent)
        opacity = inputExpr(c, root, *out, kMatOutputOpacityPin, F::Float);

    // ── Surface normal (pin 5): unconnected → the interpolated vertex normal. ──
    const std::string normalExpr = hasInput(root, *out, kMatOutputNormalPin)
        ? "normalize(" + inputExpr(c, root, *out, kMatOutputNormalPin, F::Vec3) + ")"
        : "normalize(vNormal)";

    // ── World Position Offset (pin 6) → a VERTEX-stage body, emitted into a separate
    // scope ("vs") so its statements never share variables with the fragment body. The
    // same varying NAMES are readable in the vertex template, so the text is reusable. ──
    if (hasInput(root, *out, kMatOutputWPOPin))
    {
        Scope vs;
        vs.g   = &graph;
        vs.key = "vs";
        const size_t mark = c.body.size();
        const std::string wpoExpr = inputExpr(c, vs, *out, kMatOutputWPOPin, F::Vec3);
        gen.vertexBody = c.body.substr(mark)
                       + "    vec3 heWpo = " + wpoExpr + ";\n";
        c.body.resize(mark); // the WPO statements belong to the vertex stage only
    }

    std::string src; // shared declaration block (appended to either header)
    // SEPARATE textures (`texture2D`, not `sampler2D`) — they sample through the two
    // shared samplers the lighting preamble declares (heSampWrap / heSampClamp; see
    // sampleTex above). The preamble is injected ahead of this block by
    // MaterialShaderLibrary::injectPreamble, which is the only place both files can
    // agree on those declarations, so nothing here declares a sampler of its own.
    if (c.usesTexture)
        src += "layout(set = 0, binding = 2) uniform texture2D heTex0;\n"; // legacy/mesh texture
    if (c.usesLandscapeWeights)
        // Binding 14 — the first free slot after the shared preamble's shadow/GI
        // pins (see MaterialShaderLibrary's MSL binding map). Bound per DRAW from
        // the terrain chunk's parent landscape, not per material.
        //
        // This is the texture that made the separate-sampler rewrite necessary: as a
        // COMBINED sampler it was the seventeenth on a shader model 5.0 stage that
        // has sixteen sampler registers, so a landscape graph material could not
        // compile on D3D11/D3D12 at all. It now costs a texture register and no
        // sampler register of its own.
        src += "layout(set = 0, binding = 14) uniform texture2D heLandscapeWeights;\n";
    for (size_t i = 0; i < c.textures.size(); ++i) // project textures (binding 4 + slot)
        src += "layout(set = 0, binding = " + std::to_string(4 + i)
             + ") uniform texture2D heTexP" + std::to_string(i) + ";\n";
    if (!c.params.empty())
        src += "layout(std140, set = 0, binding = 3) uniform HeParams { vec4 v["
             + std::to_string(kMatMaxParams) + "]; } heParams;\n";
    if (c.usesNoise)
        src +=
            "float heHash21(vec2 p) { p = fract(p * vec2(123.34, 456.21));"
            " p += dot(p, p + 45.32); return fract(p.x * p.y); }\n"
            "float heValueNoise(vec2 p) { vec2 i = floor(p); vec2 f = fract(p);"
            " vec2 u = f * f * (3.0 - 2.0 * f);"
            " float a = heHash21(i); float b = heHash21(i + vec2(1.0, 0.0));"
            " float cc = heHash21(i + vec2(0.0, 1.0)); float d = heHash21(i + vec2(1.0, 1.0));"
            " return mix(mix(a, b, u.x), mix(cc, d, u.x), u.y); }\n"
            "float heFbm(vec2 p) { float v = 0.0; float a = 0.5;"
            " for (int i = 0; i < 4; i++) { v += a * heValueNoise(p); p *= 2.0; a *= 0.5; }"
            " return v; }\n";
    if (c.usesNoise3)
        src +=
            "float heHash31(vec3 p) { p = fract(p * 0.1031); p += dot(p, p.zyx + 31.32);"
            " return fract((p.x + p.y) * p.z); }\n"
            "float heValueNoise3(vec3 p) { vec3 i = floor(p); vec3 f = fract(p);"
            " vec3 u = f * f * (3.0 - 2.0 * f);"
            " float n000 = heHash31(i); float n100 = heHash31(i + vec3(1.0, 0.0, 0.0));"
            " float n010 = heHash31(i + vec3(0.0, 1.0, 0.0)); float n110 = heHash31(i + vec3(1.0, 1.0, 0.0));"
            " float n001 = heHash31(i + vec3(0.0, 0.0, 1.0)); float n101 = heHash31(i + vec3(1.0, 0.0, 1.0));"
            " float n011 = heHash31(i + vec3(0.0, 1.0, 1.0)); float n111 = heHash31(i + vec3(1.0, 1.0, 1.0));"
            " float x00 = mix(n000, n100, u.x); float x10 = mix(n010, n110, u.x);"
            " float x01 = mix(n001, n101, u.x); float x11 = mix(n011, n111, u.x);"
            " return mix(mix(x00, x10, u.y), mix(x01, x11, u.y), u.z); }\n"
            "float heFbm3(vec3 p) { float v = 0.0; float a = 0.5;"
            " for (int i = 0; i < 4; i++) { v += a * heValueNoise3(p); p *= 2.0; a *= 0.5; }"
            " return v; }\n";
    if (c.usesNormalPerturb)
        src +=
            "vec3 hePerturbNormal(vec3 N, vec3 mapN, vec3 pos, vec2 uv) {\n"
            "    vec3 dp1 = dFdx(pos); vec3 dp2 = dFdy(pos);\n"
            "    vec2 duv1 = dFdx(uv); vec2 duv2 = dFdy(uv);\n"
            "    vec3 dp2perp = cross(dp2, N); vec3 dp1perp = cross(N, dp1);\n"
            "    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;\n"
            "    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;\n"
            "    float invmax = inversesqrt(max(dot(T, T), dot(B, B)));\n"
            "    return normalize(mat3(T * invmax, B * invmax, N) * mapN); }\n";
    src += "void main() {\n" + c.body;
    // Masked: kill sub-cutoff fragments BEFORE shading — hard-edged holes, opaque pass.
    if (blendMode == (int)MatBlendMode::Masked)
        src += "    if (" + mask + " < " + fmtF(cutoff) + ") discard;\n";
    src += "    vec3 heN = " + normalExpr + ";\n";

    // The declarations + body + masked-discard + heN prologue is IDENTICAL for
    // both variants; only the emit tail below differs. Snapshot it here so the
    // G-buffer variant can never drift from the forward one.
    const std::string common = std::move(src);

    src = header + common;
    if (lit)
        // Aerial perspective wraps the WHOLE lit colour (emissive included), the
        // same place the built-in scene shaders apply it — without it a distant
        // graph material stayed saturated while everything around it faded into
        // the horizon. UNLIT output is deliberately left alone: there "unlit"
        // means "my colour, verbatim".
        src += "    oColor = vec4(heApplyFog(heLitP(" + base + ", heN, " + met + ", " + rough
             + ", vWorldPos, " + spec + ", " + ao + ") + "
             + emis + ", vWorldPos), " + opacity + ");\n";
    else
        src += "    oColor = vec4(" + base + " + " + emis + ", " + opacity + ");\n";
    src += "}\n";

    // ── Deferred G-buffer tail: the SAME expression variables, written to MRT
    // instead of being lit (heOctEncode comes from the injected lighting
    // preamble, exactly like heLitP in the forward tail). Unlit materials write
    // base+emis as pure emissive with metallic=1 / specular=0 / black base, so
    // the resolve's heLitP contributes nothing and the colour passes through.
    std::string gb = headerGB + common;
    if (lit)
        gb += "    oGB0 = vec4(" + base + ", " + met + ");\n"
              "    oGB1 = vec4(heOctEncode(normalize(heN)) * 0.5 + 0.5, " + rough + ", " + spec + ");\n"
              "    oGB2 = vec4(" + emis + ", " + ao + ");\n";
    else
        gb += "    oGB0 = vec4(0.0, 0.0, 0.0, 1.0);\n"
              "    oGB1 = vec4(heOctEncode(normalize(heN)) * 0.5 + 0.5, 1.0, 0.0);\n"
              "    oGB2 = vec4(" + base + " + " + emis + ", 1.0);\n";
    gb += "    oGB3 = vec4(gl_FragCoord.z, 0.0, 0.0, 0.0);\n";
    gb += "}\n";

    // No clamp here any more: the kMatMaxParams budget is enforced in paramSlot(),
    // BEFORE emission, so an over-budget node bakes its default instead of indexing
    // past the UBO array. Clamping afterwards (what this used to do) shrank the
    // uploaded layout but left the already-emitted heParams.v[16+] reads in the
    // shader — an out-of-bounds index. c.params can no longer exceed kMatMaxParams.
    gen.glsl        = std::move(src);
    gen.glslGBuffer = std::move(gb);
    gen.params   = std::move(c.params);
    gen.textures = std::move(c.textures);
    gen.switches  = std::move(c.switches);
    gen.blendMode = static_cast<uint8_t>(blendMode);
    gen.layerNames = std::move(c.layerNames);
    return gen;
}

std::string generateFragmentGlsl(const MaterialGraph& graph)
{
    return generateFragment(graph, {}).glsl;
}

// ── JSON ───────────────────────────────────────────────────────────────────────
// The item writers/readers come first; the document ones are assembled from
// them, so the per-item form collaboration sends (CollabDocSync) and the on-disk
// form cannot drift apart.
namespace
{
nlohmann::json matNodeToJsonObj(const MatGraphNode& n)
{
    nlohmann::json jn = { { "id", n.id }, { "type", matNodeDesc(n.type).name },
                          { "p", { n.p[0], n.p[1], n.p[2], n.p[3] } },
                          { "x", n.x }, { "y", n.y } };
    if (!n.s.empty())       jn["s"]  = n.s;
    if (!n.group.empty())   jn["g"]  = n.group;   // param metadata (optional keys)
    if (!n.tooltip.empty()) jn["tt"] = n.tooltip;
    return jn;
}

// False = unknown node type (removed from the standard library), which the
// document loader skips and the delta layer ignores.
bool matNodeFromJsonObj(const nlohmann::json& jn, MatGraphNode& n)
{
    const MatNodeDesc* d = matNodeDescByName(jn.value("type", ""));
    if (!d) return false;
    n.id   = jn.value("id", 0);
    n.type = d->type;
    if (auto p = jn.find("p"); p != jn.end() && p->is_array())
        for (size_t i = 0; i < 4 && i < p->size(); ++i) n.p[i] = (*p)[i].get<float>();
    n.s       = jn.value("s", std::string());
    n.group   = jn.value("g", std::string());
    n.tooltip = jn.value("tt", std::string());
    n.x = jn.value("x", 0.0f);
    n.y = jn.value("y", 0.0f);
    // Migration: UV gained tiling params (p[0..1]) after these graphs were
    // authored, where p was all-zero. Zero tiling would collapse every UV to
    // the offset — read a legacy node as the identity it used to be.
    if (n.type == MatNodeType::UV && n.p[0] == 0.0f && n.p[1] == 0.0f)
    { n.p[0] = 1.0f; n.p[1] = 1.0f; }
    return true;
}

nlohmann::json matCommentToJsonObj(const MatGraphComment& cm)
{
    return { { "id", cm.id }, { "t", cm.text }, { "x", cm.x }, { "y", cm.y },
             { "w", cm.w }, { "h", cm.h } };
}

void matCommentFromJsonObj(const nlohmann::json& jc, MatGraphComment& cm)
{
    cm.id   = jc.value("id", 0);
    cm.text = jc.value("t", std::string());
    cm.x = jc.value("x", 0.0f); cm.y = jc.value("y", 0.0f);
    cm.w = jc.value("w", 260.0f); cm.h = jc.value("h", 180.0f);
}
} // namespace

std::string materialGraphToJson(const MaterialGraph& graph)
{
    nlohmann::json j;
    j["version"] = kMatGraphVersion;
    j["nextId"]  = graph.nextId;
    for (const auto& n : graph.nodes)
        j["nodes"].push_back(matNodeToJsonObj(n));
    for (const auto& l : graph.links) // OBJECT link form — see GraphJson.h
        j["links"].push_back(HE::graph::linkToObject(l.srcNode, l.srcPin, l.dstNode, l.dstPin));
    // Editor-only comment boxes. Older parsers ignore the extra key (forward-compatible);
    // absent key → no comments (backward-compatible).
    for (const auto& cm : graph.comments)
        j["comments"].push_back(matCommentToJsonObj(cm));
    return j.dump();
}

// ── Item-level JSON, public (collaboration addresses single items) ──────────
std::string matNodeToJson(const MatGraphNode& n)       { return matNodeToJsonObj(n).dump(); }
std::string matCommentToJson(const MatGraphComment& c) { return matCommentToJsonObj(c).dump(); }

bool matNodeFromJson(const std::string& json, MatGraphNode& out)
{
    nlohmann::json j;
    if (!HE::graph::parseGraphObject(json, j)) return false;
    MatGraphNode n;
    if (!matNodeFromJsonObj(j, n)) return false;
    out = n;
    return true;
}

bool matCommentFromJson(const std::string& json, MatGraphComment& out)
{
    nlohmann::json j;
    if (!HE::graph::parseGraphObject(json, j)) return false;
    matCommentFromJsonObj(j, out);
    return true;
}

bool materialGraphFromJson(const std::string& json, MaterialGraph& out)
{
    nlohmann::json j;
    if (!HE::graph::parseGraphObject(json, j)) return false;
    MaterialGraph g;
    g.nextId = j.value("nextId", 1);
    for (const auto& jn : j.value("nodes", nlohmann::json::array()))
    {
        MatGraphNode n;
        if (!matNodeFromJsonObj(jn, n)) continue;   // unknown node type
        g.nodes.push_back(n);
        HE::graph::bumpNextId(g.nextId, n.id);
    }
    // v1 → v2: Specular was inserted at Output pin 2 and Ambient Occlusion at
    // pin 7, shifting everything after them. Links are stored by pin INDEX, so
    // a v1 graph read as-is would rewire Roughness→Specular, Emissive→Roughness,
    // and so on. Remap the Output node's INPUT pins on the way in.
    const int fileVersion = j.value("version", 1);
    const bool remapOutput = fileVersion < 2;
    std::vector<int> outputNodeIds;
    if (remapOutput)
        for (const auto& n : g.nodes)
            if (n.type == MatNodeType::Output) outputNodeIds.push_back(n.id);
    auto isOutputNode = [&](int id) {
        return std::find(outputNodeIds.begin(), outputNodeIds.end(), id) != outputNodeIds.end();
    };
    // old index → new index (old: base, metal, rough, emis, opacity, normal, wpo)
    static const int kV1ToV2[7] = {
        kMatOutputBaseColorPin, kMatOutputMetallicPin, kMatOutputRoughnessPin,
        kMatOutputEmissivePin,  kMatOutputOpacityPin,  kMatOutputNormalPin,
        kMatOutputWPOPin };

    for (const auto& jl : j.value("links", nlohmann::json::array()))
    {
        MatGraphLink l;
        HE::graph::linkFromObject(jl, l.srcNode, l.srcPin, l.dstNode, l.dstPin);
        if (remapOutput && isOutputNode(l.dstNode) &&
            l.dstPin >= 0 && l.dstPin < 7)
            l.dstPin = kV1ToV2[l.dstPin];
        g.links.push_back(l);
    }
    for (const auto& jc : j.value("comments", nlohmann::json::array()))
    {
        MatGraphComment cm;
        matCommentFromJsonObj(jc, cm);
        HE::graph::bumpNextId(g.nextId, cm.id);
        g.comments.push_back(std::move(cm));
    }
    out = std::move(g);
    return true;
}

// ─── matGraphApproxSurface (see the header for the contract) ─────────────────
namespace
{
const MatGraphLink* approxFindLink(const MaterialGraph& g, int dstNode, int dstPin)
{
    for (const MatGraphLink& l : g.links)
        if (l.dstNode == dstNode && l.dstPin == dstPin) return &l;
    return nullptr;
}

bool approxFoldNode(const MaterialGraph& g, int nodeId, int depth, float out[4],
                    const std::map<std::string, bool>* sw);

// Analytic means of the procedural generators (MaterialShaderLibrary's noise
// helpers). Folding them to their average instead of failing is what keeps the
// ubiquitous `Colour × Noise` mottling chain from collapsing the WHOLE pin to
// white: heHash21/heHash31 are uniform on [0,1) → mean 0.5, and heFbm/heFbm3
// sum four octaves with amplitudes 0.5+0.25+0.125+0.0625 = 0.9375, so their
// mean is 0.9375 × 0.5. Checker emits 0 or 1 in equal measure.
constexpr float kApproxNoiseMean   = 0.5f;
constexpr float kApproxFbmMean     = 0.46875f;
constexpr float kApproxCheckerMean = 0.5f;

// Fold one INPUT pin: linked → fold the source node, unconnected → the pin's
// registry default (splat — the same coercion codegen applies to scalars).
bool approxFoldInput(const MaterialGraph& g, const MatGraphNode& node, int pin,
                     int depth, float out[4], const std::map<std::string, bool>* sw)
{
    if (const MatGraphLink* l = approxFindLink(g, node.id, pin))
        return approxFoldNode(g, l->srcNode, depth, out, sw);
    const MatNodeDesc& d = matNodeDesc(node.type);
    const float def = (pin >= 0 && pin < (int)d.inputs.size()) ? d.inputs[pin].def : 0.0f;
    out[0] = out[1] = out[2] = out[3] = def;
    return true;
}

// Fold a Landscape Layer Blend's layer inputs, ONE colour each (index = weightmap
// channel). `foldedMask` bit k = layer k folded; an UNCONNECTED layer is not
// authored at all and never counts, so it stays out of both the mask and any
// average built from these.
int approxFoldLayers(const MaterialGraph& g, const MatGraphNode& n, int depth,
                     float layers[kMatMaxLandscapeLayers][4], uint32_t& foldedMask,
                     const std::map<std::string, bool>* sw)
{
    const std::vector<std::string> names = matLandscapeLayerNames(n.s);
    const int count = std::min((int)names.size(), kMatMaxLandscapeLayers);
    foldedMask = 0;
    for (int i = 0; i < count; ++i)
    {
        for (int k = 0; k < 4; ++k) layers[i][k] = 0.0f;
        if (!approxFindLink(g, n.id, i)) continue;  // unconnected → not authored
        if (approxFoldInput(g, n, i, depth + 1, layers[i], sw))
            foldedMask |= (1u << i);
    }
    return count;
}

// The shader weights the layers by the terrain's painted weightmap — per-texel
// data no CPU fold can see — so the flat average over the AUTHORED layers is the
// honest per-instance answer. All-unfoldable → false (the caller keeps its
// default). Consumers that DO know the paint (giInstanceSurface, via the
// terrain's average weights) re-blend MatApproxSurface::layerColor instead.
bool approxFoldLayerBlend(const MaterialGraph& g, const MatGraphNode& n, int depth,
                          float out[4], const std::map<std::string, bool>* sw)
{
    float layers[kMatMaxLandscapeLayers][4];
    uint32_t mask = 0;
    const int count = approxFoldLayers(g, n, depth, layers, mask, sw);
    float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int   folded = 0;
    for (int i = 0; i < count; ++i)
    {
        if (!(mask & (1u << i))) continue;
        for (int k = 0; k < 4; ++k) acc[k] += layers[i][k];
        ++folded;
    }
    if (folded == 0) return false;
    for (int k = 0; k < 4; ++k) out[k] = acc[k] / static_cast<float>(folded);
    return true;
}

bool approxFoldNode(const MaterialGraph& g, int nodeId, int depth, float out[4],
                    const std::map<std::string, bool>* sw)
{
    if (depth > 32) return false; // cycle/degenerate guard
    const MatGraphNode* n = g.findNode(nodeId);
    if (!n) return false;
    float a[4], b[4], t[4];
    switch (n->type)
    {
    case MatNodeType::ConstFloat:
    case MatNodeType::ConstBool:
    case MatNodeType::ParamFloat:  // Param values fold from the node DEFAULT —
    case MatNodeType::ParamBool:   // live values go through MatApproxSurface::*Param
        out[0] = out[1] = out[2] = out[3] = n->p[0];
        return true;
    case MatNodeType::ConstColor:
    case MatNodeType::ParamColor:
        out[0] = n->p[0]; out[1] = n->p[1]; out[2] = n->p[2]; out[3] = 1.0f;
        return true;
    case MatNodeType::ConstVec2:
    case MatNodeType::ParamVec2:
        out[0] = n->p[0]; out[1] = n->p[1]; out[2] = 0.0f; out[3] = 0.0f;
        return true;
    case MatNodeType::ConstVec4:
    case MatNodeType::ParamVec4:
        for (int k = 0; k < 4; ++k) out[k] = n->p[k];
        return true;
    case MatNodeType::Reroute:
        return approxFoldInput(g, *n, 0, depth + 1, out, sw);
    case MatNodeType::StaticSwitch:
    {
        // Same resolution as codegen: the override map beats the node default,
        // and only the TAKEN input folds (pin 0 = True, 1 = False) — a switch
        // instance's reflected colour follows its permutation.
        const std::string swName = n->s.empty()
            ? ("switch_" + std::to_string(n->id)) : n->s;
        bool on = n->p[0] > 0.5f;
        if (sw)
            if (auto it = sw->find(swName); it != sw->end()) on = it->second;
        return approxFoldInput(g, *n, on ? 0 : 1, depth + 1, out, sw);
    }
    case MatNodeType::OneMinus:
        if (!approxFoldInput(g, *n, 0, depth + 1, a, sw)) return false;
        for (int k = 0; k < 4; ++k) out[k] = 1.0f - a[k];
        return true;
    case MatNodeType::Saturate:
        if (!approxFoldInput(g, *n, 0, depth + 1, a, sw)) return false;
        for (int k = 0; k < 4; ++k) out[k] = std::clamp(a[k], 0.0f, 1.0f);
        return true;
    case MatNodeType::Add:
    case MatNodeType::Subtract:
    case MatNodeType::Multiply:
    case MatNodeType::Divide:
        if (!approxFoldInput(g, *n, 0, depth + 1, a, sw) ||
            !approxFoldInput(g, *n, 1, depth + 1, b, sw)) return false;
        for (int k = 0; k < 4; ++k)
            out[k] = n->type == MatNodeType::Add      ? a[k] + b[k]
                   : n->type == MatNodeType::Subtract ? a[k] - b[k]
                   : n->type == MatNodeType::Multiply ? a[k] * b[k]
                   : (std::abs(b[k]) > 1e-8f ? a[k] / b[k] : 0.0f);
        return true;
    case MatNodeType::Lerp:
        if (!approxFoldInput(g, *n, 0, depth + 1, a, sw) ||
            !approxFoldInput(g, *n, 1, depth + 1, b, sw) ||
            !approxFoldInput(g, *n, 2, depth + 1, t, sw)) return false;
        for (int k = 0; k < 4; ++k) out[k] = a[k] + (b[k] - a[k]) * t[0];
        return true;
    case MatNodeType::Absolute:
        if (!approxFoldInput(g, *n, 0, depth + 1, a, sw)) return false;
        for (int k = 0; k < 4; ++k) out[k] = std::abs(a[k]);
        return true;
    case MatNodeType::Fract:
        if (!approxFoldInput(g, *n, 0, depth + 1, a, sw)) return false;
        for (int k = 0; k < 4; ++k) out[k] = a[k] - std::floor(a[k]);
        return true;
    // Scalar-result nodes: codegen coerces their inputs to float (component .x)
    // and the single result splats on the way out — the fold mirrors both.
    case MatNodeType::Power:
        if (!approxFoldInput(g, *n, 0, depth + 1, a, sw) ||
            !approxFoldInput(g, *n, 1, depth + 1, b, sw)) return false;
        out[0] = out[1] = out[2] = out[3] = std::pow(std::max(a[0], 0.0f), b[0]);
        return true;
    case MatNodeType::Sine:
        if (!approxFoldInput(g, *n, 0, depth + 1, a, sw)) return false;
        out[0] = out[1] = out[2] = out[3] = std::sin(a[0]);
        return true;
    case MatNodeType::Step:
        if (!approxFoldInput(g, *n, 0, depth + 1, a, sw) ||
            !approxFoldInput(g, *n, 1, depth + 1, b, sw)) return false;
        out[0] = out[1] = out[2] = out[3] = (b[0] < a[0]) ? 0.0f : 1.0f;
        return true;
    case MatNodeType::Smoothstep:
    {
        if (!approxFoldInput(g, *n, 0, depth + 1, a, sw) ||
            !approxFoldInput(g, *n, 1, depth + 1, b, sw) ||
            !approxFoldInput(g, *n, 2, depth + 1, t, sw)) return false;
        const float den = b[0] - a[0];
        const float x   = std::abs(den) > 1e-8f
            ? std::clamp((t[0] - a[0]) / den, 0.0f, 1.0f) : 0.0f;
        out[0] = out[1] = out[2] = out[3] = x * x * (3.0f - 2.0f * x);
        return true;
    }
    case MatNodeType::Combine3:
        if (!approxFoldInput(g, *n, 0, depth + 1, a, sw) ||
            !approxFoldInput(g, *n, 1, depth + 1, b, sw) ||
            !approxFoldInput(g, *n, 2, depth + 1, t, sw)) return false;
        out[0] = a[0]; out[1] = b[0]; out[2] = t[0]; out[3] = 1.0f;
        return true;
    case MatNodeType::CombineRGBA:
    {
        float w[4];
        if (!approxFoldInput(g, *n, 0, depth + 1, a, sw) ||
            !approxFoldInput(g, *n, 1, depth + 1, b, sw) ||
            !approxFoldInput(g, *n, 2, depth + 1, t, sw) ||
            !approxFoldInput(g, *n, 3, depth + 1, w, sw)) return false;
        out[0] = a[0]; out[1] = b[0]; out[2] = t[0]; out[3] = w[0];
        return true;
    }
    // Procedural generators: their analytic mean (see the k*Mean constants).
    // Noise Texture's two pins (vec3(v) and v) are the same scalar, so the
    // splat is right for whichever pin the consumer read.
    case MatNodeType::ValueNoise:
        out[0] = out[1] = out[2] = out[3] = kApproxNoiseMean;
        return true;
    case MatNodeType::Fbm:
    case MatNodeType::NoiseTexture:
        out[0] = out[1] = out[2] = out[3] = kApproxFbmMean;
        return true;
    case MatNodeType::Checker:
        out[0] = out[1] = out[2] = out[3] = kApproxCheckerMean;
        return true;
    case MatNodeType::LandscapeLayerBlend:
        return approxFoldLayerBlend(g, *n, depth, out, sw);
    default:
        return false; // textures, per-fragment inputs … cannot fold
    }
}

// Real source of an Output input pin (nullptr = unconnected): Reroutes are
// skipped, StaticSwitches follow their TAKEN input — so a Param behind a
// switch still resolves to a live slot for the winning permutation.
const MatGraphNode* approxSource(const MaterialGraph& g, int dstNode, int dstPin,
                                 const std::map<std::string, bool>* sw)
{
    for (int guard = 0; guard < 32; ++guard)
    {
        const MatGraphLink* l = approxFindLink(g, dstNode, dstPin);
        if (!l) return nullptr;
        const MatGraphNode* n = g.findNode(l->srcNode);
        if (!n) return nullptr;
        if (n->type == MatNodeType::Reroute) { dstNode = n->id; dstPin = 0; continue; }
        if (n->type == MatNodeType::StaticSwitch)
        {
            const std::string swName = n->s.empty()
                ? ("switch_" + std::to_string(n->id)) : n->s;
            bool on = n->p[0] > 0.5f;
            if (sw)
                if (auto it = sw->find(swName); it != sw->end()) on = it->second;
            dstNode = n->id; dstPin = on ? 0 : 1;
            continue;
        }
        return n;
    }
    return nullptr;
}
} // namespace

MatApproxSurface matGraphApproxSurface(const MaterialGraph& g,
                                       const std::map<std::string, bool>* switchOverrides)
{
    MatApproxSurface out;
    const MatGraphNode* output = nullptr;
    for (const MatGraphNode& n : g.nodes)
        if (n.type == MatNodeType::Output) { output = &n; break; }
    if (!output) return out;

    auto foldPin = [&](int pin, float rgb[3], std::string& param)
    {
        // Directly Param-driven (rgb-carrying kinds only — a Float param splat
        // cannot be reconstructed from a slot's raw .rgb) → live lookup.
        if (const MatGraphNode* src = approxSource(g, output->id, pin, switchOverrides);
            src && (src->type == MatNodeType::ParamColor ||
                    src->type == MatNodeType::ParamVec4))
            param = src->s;
        const MatGraphLink* l = approxFindLink(g, output->id, pin);
        float v[4];
        if (l && approxFoldNode(g, l->srcNode, 0, v, switchOverrides))
        {
            rgb[0] = v[0]; rgb[1] = v[1]; rgb[2] = v[2];
        }
        else if (!l)
        {
            const float def = matNodeDesc(MatNodeType::Output).inputs[pin].def;
            rgb[0] = rgb[1] = rgb[2] = def;
        }
        // linked but unfoldable → keep the caller-visible defaults (white/black)
    };
    foldPin(kMatOutputBaseColorPin, out.baseColor, out.baseColorParam);
    foldPin(kMatOutputEmissivePin,  out.emissive,  out.emissiveParam);
    // BaseColor straight off a Landscape Layer Blend: keep the layers SEPARATE
    // as well, so a consumer holding a specific terrain's painted weights can
    // reproduce that terrain's colour instead of the layer average above. A
    // layer that will not fold takes the average as its stand-in — the blend
    // then stays in the material's own colour range rather than jumping white.
    if (const MatGraphNode* src = approxSource(g, output->id, kMatOutputBaseColorPin,
                                               switchOverrides);
        src && src->type == MatNodeType::LandscapeLayerBlend)
    {
        float layers[kMatMaxLandscapeLayers][4];
        uint32_t mask = 0;
        out.layerCount = approxFoldLayers(g, *src, 0, layers, mask, switchOverrides);
        for (int i = 0; i < out.layerCount; ++i)
            for (int k = 0; k < 3; ++k)
                out.layerColor[i][k] = (mask & (1u << i)) ? layers[i][k] : out.baseColor[k];
        if (mask == 0) out.layerCount = 0; // nothing folded → no usable split
    }
    // Scalar pins (metallic/roughness) — constants only: the GI bounce loop
    // needs a per-instance mirror-ness, a fold-time snapshot is good enough.
    auto foldScalar = [&](int pin, float& v)
    {
        const MatGraphLink* l = approxFindLink(g, output->id, pin);
        float f[4];
        if (l && approxFoldNode(g, l->srcNode, 0, f, switchOverrides)) v = f[0];
        else if (!l) v = matNodeDesc(MatNodeType::Output).inputs[pin].def;
        // linked but unfoldable → keep the caller-visible defaults (0 / 0.5)
    };
    foldScalar(kMatOutputMetallicPin,  out.metallic);
    foldScalar(kMatOutputRoughnessPin, out.roughness);
    return out;
}
} // namespace HE
