#include "MaterialGraph/MaterialGraph.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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
        { MatNodeType::Output, "Output", "Material",
          { { "BaseColor", F::Vec3, 0.8f }, { "Metallic", F::Float, 0.0f },
            { "Roughness", F::Float, 0.5f }, { "Emissive", F::Vec3, 0.0f },
            { "Opacity", F::Float, 1.0f } },
          {}, 1 }, // p[0] = lit
        { MatNodeType::ConstFloat, "Float", "Input",
          {}, { { "Value", F::Float, 0 } }, 1 },
        { MatNodeType::ConstColor, "Color", "Input",
          {}, { { "RGB", F::Vec3, 0 } }, 3 },
        { MatNodeType::VertexColor, "Vertex Color", "Input",
          {}, { { "RGB", F::Vec3, 0 } }, 0 },
        { MatNodeType::NormalWS, "Normal (WS)", "Input",
          {}, { { "N", F::Vec3, 0 } }, 0 },
        { MatNodeType::UV, "UV", "Input",
          {}, { { "UV", F::Vec2, 0 } }, 0 },
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
        { MatNodeType::ConstVec2, "Vector2", "Input",
          {}, { { "XY", F::Vec2, 0 } }, 2 },
        { MatNodeType::ConstVec4, "Vector4", "Input",
          {}, { { "XYZW", F::Vec4, 0 } }, 4 },
        { MatNodeType::CameraPos, "Camera Position", "Input",
          {}, { { "XYZ", F::Vec3, 0 } }, 0 },
        { MatNodeType::CameraDistance, "Camera Distance", "Input",
          {}, { { "Dist", F::Float, 0 } }, 0 },
        { MatNodeType::ScreenPos, "Screen Position", "Input",
          {}, { { "XY", F::Vec2, 0 } }, 0 },

        // ── v5: baked constants, parameter types, logic ──
        { MatNodeType::ConstBool, "Bool", "Input",
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

void matFunctionPins(const MaterialGraph& fnGraph,
                     std::vector<MatPinDesc>& inputs, std::vector<MatPinDesc>& outputs)
{
    inputs.clear(); outputs.clear();
    std::vector<const MatGraphNode*> ins, outs;
    for (const auto& n : fnGraph.nodes)
    {
        if (n.type == MatNodeType::FnInput)  ins.push_back(&n);
        if (n.type == MatNodeType::FnOutput) outs.push_back(&n);
    }
    auto byId = [](const MatGraphNode* a, const MatGraphNode* b){ return a->id < b->id; };
    std::sort(ins.begin(), ins.end(), byId);
    std::sort(outs.begin(), outs.end(), byId);
    for (const auto* n : ins)
        inputs.push_back({ n->s.c_str(), pinTypeFromParam(n->p[0]), 0.0f });
    for (const auto* n : outs)
        outputs.push_back({ n->s.c_str(), pinTypeFromParam(n->p[0]), 0.0f });
}

int MaterialGraph::addNode(MatNodeType type, float x, float y)
{
    MatGraphNode n;
    n.id = nextId++;
    n.type = type;
    n.x = x; n.y = y;
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
    nodes.push_back(n);
    return n.id;
}

const MatGraphNode* MaterialGraph::findNode(int id) const
{
    for (const auto& n : nodes) if (n.id == id) return &n;
    return nullptr;
}
MatGraphNode* MaterialGraph::findNode(int id)
{
    for (auto& n : nodes) if (n.id == id) return &n;
    return nullptr;
}

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
    if (d->type != MatNodeType::FunctionCall)
    {
        const MatNodeDesc& dd = matNodeDesc(d->type);
        if (dstPin < 0 || dstPin >= (int)dd.inputs.size()) return false;
    }
    disconnectInput(dstNode, dstPin);
    links.push_back({ srcNode, srcPin, dstNode, dstPin });
    return true;
}

void MaterialGraph::disconnectInput(int dstNode, int dstPin)
{
    links.erase(std::remove_if(links.begin(), links.end(),
        [&](const MatGraphLink& l){ return l.dstNode == dstNode && l.dstPin == dstPin; }),
        links.end());
}

void MaterialGraph::removeNode(int id)
{
    const MatGraphNode* n = findNode(id);
    if (!n || n->type == MatNodeType::Output) return;
    links.erase(std::remove_if(links.begin(), links.end(),
        [&](const MatGraphLink& l){ return l.srcNode == id || l.dstNode == id; }),
        links.end());
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
        [&](const MatGraphNode& nn){ return nn.id == id; }),
        nodes.end());
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
    bool usesNoise   = false;
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

// Allocate (or find) the HeParams slot for a named parameter. `kind` drives how many
// of the vec4's components carry the default value (rest stay 0) AND the typed widget
// shown outside the canvas. Same name reuses the slot so repeated Param nodes share
// one uniform.
int paramSlot(EmitCtx& c, const MatGraphNode& n, MatParamKind kind)
{
    const std::string name = n.s.empty() ? ("param_" + std::to_string(n.id)) : n.s;
    for (size_t i = 0; i < c.params.size(); ++i)
        if (c.params[i].name == name)
            return (int)i;
    const int keep = matParamKindComponents(kind);
    MatParamSlot slot;
    slot.name = name;
    slot.kind = kind;
    slot.isColor = (kind == MatParamKind::Color);
    for (int i = 0; i < 4; ++i) slot.value[i] = (i < keep) ? n.p[i] : 0.0f;
    c.params.push_back(std::move(slot));
    return (int)c.params.size() - 1;
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
            decl = "vec2 " + v + " = vUV;"; break;
        case MatNodeType::Time:
            decl = "float " + v + " = heLight.sunDir.w;"; break;
        case MatNodeType::TextureSample:
        {
            // A node with no picked texture (empty s) samples the material's legacy/mesh
            // texture (heTex0). A picked project texture gets its own slot heTexP{k}
            // (deduplicated, capped at kMatMaxGraphTextures); extras fall back to heTex0.
            std::string sampler = "heTex0";
            if (!n.s.empty())
            {
                int slot = -1;
                for (size_t i = 0; i < c.textures.size(); ++i)
                    if (c.textures[i] == n.s) { slot = (int)i; break; }
                if (slot < 0 && (int)c.textures.size() < kMatMaxGraphTextures)
                { slot = (int)c.textures.size(); c.textures.push_back(n.s); }
                if (slot >= 0) sampler = "heTexP" + std::to_string(slot);
                else           c.usesTexture = true; // over budget → default
            }
            else
                c.usesTexture = true;
            decl = "vec4 " + v + " = texture(" + sampler + ", " + inputExpr(c, sc, n, 0, F::Vec2) + ");";
            pinExpr = { v + ".xyz", v + ".w" };
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
            const int slot = paramSlot(c, n, MatParamKind::Float);
            decl = "float " + v + " = heParams.v[" + std::to_string(slot) + "].x; // param: "
                 + (n.s.empty() ? "?" : n.s);
            break;
        }
        case MatNodeType::ParamColor:
        {
            const int slot = paramSlot(c, n, MatParamKind::Color);
            decl = "vec3 " + v + " = heParams.v[" + std::to_string(slot) + "].xyz; // param: "
                 + (n.s.empty() ? "?" : n.s);
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
            decl = "float " + v + " = heValueNoise(" + inputExpr(c, sc, n, 0, F::Vec2) + " * "
                 + inputExpr(c, sc, n, 1, F::Float) + ");"; break;
        case MatNodeType::Fbm:
            c.usesNoise = true;
            decl = "float " + v + " = heFbm(" + inputExpr(c, sc, n, 0, F::Vec2) + " * "
                 + inputExpr(c, sc, n, 1, F::Float) + ");"; break;
        case MatNodeType::Checker:
            decl = "float " + v + " = mod(floor(" + inputExpr(c, sc, n, 0, F::Vec2) + ".x * "
                 + inputExpr(c, sc, n, 1, F::Float) + ") + floor(" + inputExpr(c, sc, n, 0, F::Vec2) + ".y * "
                 + inputExpr(c, sc, n, 1, F::Float) + "), 2.0);"; break;

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
                std::vector<const MatGraphNode*> ins;
                for (const auto& nn : sc.g->nodes)
                    if (nn.type == MatNodeType::FnInput) ins.push_back(&nn);
                std::sort(ins.begin(), ins.end(),
                          [](const MatGraphNode* a, const MatGraphNode* b){ return a->id < b->id; });
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
            std::vector<const MatGraphNode*> outs;
            for (const auto& nn : fn->nodes)
                if (nn.type == MatNodeType::FnOutput) outs.push_back(&nn);
            std::sort(outs.begin(), outs.end(),
                      [](const MatGraphNode* a, const MatGraphNode* b){ return a->id < b->id; });
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
            decl = "vec2 " + v + " = heParams.v[" + std::to_string(slot) + "].xy; // param: "
                 + (n.s.empty() ? "?" : n.s);
            break;
        }
        case MatNodeType::ParamVec4:
        {
            const int slot = paramSlot(c, n, MatParamKind::Vec4);
            decl = "vec4 " + v + " = heParams.v[" + std::to_string(slot) + "]; // param: "
                 + (n.s.empty() ? "?" : n.s);
            break;
        }
        case MatNodeType::ParamBool:
        {
            const int slot = paramSlot(c, n, MatParamKind::Bool);
            // Threshold so a bool param reads cleanly as 0.0/1.0 even if set to e.g. 0.7.
            decl = "float " + v + " = step(0.5, heParams.v[" + std::to_string(slot) + "].x); // param: "
                 + (n.s.empty() ? "?" : n.s);
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
} // namespace

MatShaderGen generateFragment(const MaterialGraph& graph, const MatFunctionLoader& loader)
{
    const MatGraphNode* out = nullptr;
    for (const auto& n : graph.nodes)
        if (n.type == MatNodeType::Output) { out = &n; break; }

    std::string header =
        "#version 450\n"
        "// GENERATED by the material node graph — do not edit by hand.\n"
        "layout(location = 0) in vec3 vNormal;\n"
        "layout(location = 1) in vec3 vColor;\n"
        "layout(location = 2) in vec2 vUV;\n"
        "layout(location = 3) in vec3 vWorldPos;\n"
        "layout(location = 0) out vec4 oColor;\n";

    MatShaderGen gen;
    if (!out)
    {
        gen.glsl = header + "void main() { oColor = vec4(1.0, 0.0, 1.0, 1.0); } // no Output node\n";
        return gen;
    }

    EmitCtx c;
    c.loader = &loader;
    Scope root;
    root.g = &graph;

    const std::string base    = inputExpr(c, root, *out, 0, F::Vec3);
    const std::string met     = inputExpr(c, root, *out, 1, F::Float);
    const std::string rough   = inputExpr(c, root, *out, 2, F::Float);
    const std::string emis    = inputExpr(c, root, *out, 3, F::Vec3);
    const std::string opacity = inputExpr(c, root, *out, 4, F::Float);
    const bool lit = out->p[0] > 0.5f;

    std::string src = header;
    if (c.usesTexture)
        src += "layout(set = 0, binding = 2) uniform sampler2D heTex0;\n"; // legacy/mesh texture
    for (size_t i = 0; i < c.textures.size(); ++i) // project textures (binding 4 + slot)
        src += "layout(set = 0, binding = " + std::to_string(4 + i)
             + ") uniform sampler2D heTexP" + std::to_string(i) + ";\n";
    if (!c.params.empty())
        src += "layout(std140, set = 0, binding = 3) uniform HeParams { vec4 v[16]; } heParams;\n";
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
    src += "void main() {\n" + c.body;
    if (lit)
        src += "    oColor = vec4(heLit(" + base + ", normalize(vNormal), " + met + ", " + rough + ") + "
             + emis + ", " + opacity + ");\n";
    else
        src += "    oColor = vec4(" + base + " + " + emis + ", " + opacity + ");\n";
    src += "}\n";

    // Cap at the UBO's 16 slots (over-budget params were still emitted with their slot
    // index — clamp the layout so uploads stay in bounds; realistically never hit).
    if (c.params.size() > 16) c.params.resize(16);
    gen.glsl     = std::move(src);
    gen.params   = std::move(c.params);
    gen.textures = std::move(c.textures);
    return gen;
}

std::string generateFragmentGlsl(const MaterialGraph& graph)
{
    return generateFragment(graph, {}).glsl;
}

// ── JSON ───────────────────────────────────────────────────────────────────────
std::string materialGraphToJson(const MaterialGraph& graph)
{
    nlohmann::json j;
    j["version"] = 1;
    j["nextId"]  = graph.nextId;
    for (const auto& n : graph.nodes)
    {
        nlohmann::json jn = { { "id", n.id }, { "type", matNodeDesc(n.type).name },
                              { "p", { n.p[0], n.p[1], n.p[2], n.p[3] } },
                              { "x", n.x }, { "y", n.y } };
        if (!n.s.empty()) jn["s"] = n.s;
        j["nodes"].push_back(std::move(jn));
    }
    for (const auto& l : graph.links)
        j["links"].push_back({ { "sn", l.srcNode }, { "sp", l.srcPin },
                               { "dn", l.dstNode }, { "dp", l.dstPin } });
    return j.dump();
}

bool materialGraphFromJson(const std::string& json, MaterialGraph& out)
{
    nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;
    MaterialGraph g;
    g.nextId = j.value("nextId", 1);
    for (const auto& jn : j.value("nodes", nlohmann::json::array()))
    {
        const MatNodeDesc* d = matNodeDescByName(jn.value("type", ""));
        if (!d) continue;
        MatGraphNode n;
        n.id   = jn.value("id", 0);
        n.type = d->type;
        if (auto p = jn.find("p"); p != jn.end() && p->is_array())
            for (size_t i = 0; i < 4 && i < p->size(); ++i) n.p[i] = (*p)[i].get<float>();
        n.s = jn.value("s", std::string());
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
} // namespace HE
