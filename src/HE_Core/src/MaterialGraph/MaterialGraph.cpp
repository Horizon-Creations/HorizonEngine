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
// One place drives codegen AND the editor palette/pins. Emission templates live in
// emitNode() below, keyed by type.
const std::vector<MatNodeDesc>& registry()
{
    static const std::vector<MatNodeDesc> kReg = {
        { MatNodeType::Output, "Output", "Material",
          { { "BaseColor", F::Vec3, 0.8f }, { "Metallic", F::Float, 0.0f },
            { "Roughness", F::Float, 0.5f }, { "Emissive", F::Vec3, 0.0f } },
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
          { { "UV", F::Vec2, 0 } }, { { "RGB", F::Vec3, 0 } }, 0 },
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
        { MatNodeType::Combine3, "Combine XYZ", "Math",
          { { "X", F::Float, 0 }, { "Y", F::Float, 0 }, { "Z", F::Float, 0 } },
          { { "RGB", F::Vec3, 0 } }, 0 },

        // ── v2 ──
        { MatNodeType::WorldPos, "World Position", "Input",
          {}, { { "XYZ", F::Vec3, 0 } }, 0 },
        { MatNodeType::ViewDir, "View Direction", "Input",
          {}, { { "V", F::Vec3, 0 } }, 0 },
        { MatNodeType::ParamFloat, "Param (Float)", "Parameter",
          {}, { { "Value", F::Float, 0 } }, 1 }, // s = name, p[0] = value
        { MatNodeType::ParamColor, "Param (Color)", "Parameter",
          {}, { { "RGB", F::Vec3, 0 } }, 3 },    // s = name, p[0..2] = rgb
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
    switch (t) { case F::Float: return "float"; case F::Vec2: return "vec2"; case F::Vec3: return "vec3"; }
    return "float";
}

// Coerce an expression of type `from` to `to` (float splats up; exact match passes).
// Mismatches that can't splat take .x / drop components — defined, never invalid GLSL.
std::string coerce(const std::string& expr, MatPinType from, MatPinType to)
{
    if (from == to) return expr;
    if (from == F::Float) return std::string(typeName(to)) + "(" + expr + ")";
    if (to == F::Float)   return "(" + expr + ").x";
    if (from == F::Vec3 && to == F::Vec2) return "(" + expr + ").xy";
    /* Vec2 → Vec3 */     return "vec3(" + expr + ", 0.0)";
}

std::string defaultExpr(const MatPinDesc& pin)
{
    switch (pin.type)
    {
        case F::Float: return fmtF(pin.def);
        case F::Vec2:  return "vec2(" + fmtF(pin.def) + ")";
        case F::Vec3:  return "vec3(" + fmtF(pin.def) + ")";
    }
    return "0.0";
}
} // namespace

const std::vector<MatNodeDesc>& matNodeRegistry() { return registry(); }

const MatNodeDesc& matNodeDesc(MatNodeType type)
{
    for (const auto& d : registry())
        if (d.type == type) return d;
    return registry().front(); // unreachable for valid types
}

const MatNodeDesc* matNodeDescByName(const std::string& name)
{
    for (const auto& d : registry())
        if (name == d.name) return &d;
    return nullptr;
}

int MaterialGraph::addNode(MatNodeType type, float x, float y)
{
    MatGraphNode n;
    n.id = nextId++;
    n.type = type;
    n.x = x; n.y = y;
    // Seed params with something sensible per type.
    if (type == MatNodeType::Output)     n.p[0] = 1.0f;                       // lit
    if (type == MatNodeType::ConstColor) { n.p[0] = n.p[1] = n.p[2] = 0.8f; }
    if (type == MatNodeType::ConstFloat) n.p[0] = 1.0f;
    if (type == MatNodeType::Fresnel)    n.p[0] = 3.0f;
    if (type == MatNodeType::ParamFloat) { n.p[0] = 1.0f; n.s = "MyParam"; }
    if (type == MatNodeType::ParamColor) { n.p[0] = n.p[1] = n.p[2] = 0.8f; n.s = "MyColor"; }
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
    const MatNodeDesc& sd = matNodeDesc(s->type);
    const MatNodeDesc& dd = matNodeDesc(d->type);
    if (srcPin < 0 || srcPin >= (int)sd.outputs.size()) return false;
    if (dstPin < 0 || dstPin >= (int)dd.inputs.size())  return false;
    // Every type pair is coercible (see coerce()), so no type veto — the editor shows
    // the pin types and the coercion is deterministic.
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
    if (!n || n->type == MatNodeType::Output) return; // the output is not deletable
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
    g.connect(col, 0, out, 0); // RGB → BaseColor
    return g;
}

// ── Codegen ────────────────────────────────────────────────────────────────────
namespace
{
struct EmitCtx
{
    const MaterialGraph& g;
    std::string          body;
    std::unordered_map<long long, std::string> outVar;  // (node<<8|pin) → var name
    std::unordered_set<int> emitting;                   // cycle guard
    bool usesTexture = false;
    bool usesNoise   = false; // pulls the heHash/heValueNoise/heFbm helpers into the shader
    int  varCounter  = 0;
};

std::string inputExpr(EmitCtx& c, const MatGraphNode& node, int pinIdx, MatPinType wantType);

// Emit one node's outputs (memoized); returns the variable holding output pin `pin`.
std::string emitNode(EmitCtx& c, const MatGraphNode& n, int pin)
{
    const long long key = (static_cast<long long>(n.id) << 8) | pin;
    if (auto it = c.outVar.find(key); it != c.outVar.end()) return it->second;
    if (c.emitting.count(n.id))
        return "vec3(1.0, 0.0, 1.0)"; // cycle → magenta, never infinite recursion
    c.emitting.insert(n.id);

    const MatNodeDesc& d = matNodeDesc(n.type);
    const std::string v = "n" + std::to_string(++c.varCounter);
    std::string decl;

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
            c.usesTexture = true;
            decl = "vec3 " + v + " = texture(heTex0, " + inputExpr(c, n, 0, F::Vec2) + ").rgb;"; break;
        case MatNodeType::Add:
            decl = "vec3 " + v + " = " + inputExpr(c, n, 0, F::Vec3) + " + " + inputExpr(c, n, 1, F::Vec3) + ";"; break;
        case MatNodeType::Multiply:
            decl = "vec3 " + v + " = " + inputExpr(c, n, 0, F::Vec3) + " * " + inputExpr(c, n, 1, F::Vec3) + ";"; break;
        case MatNodeType::Lerp:
            decl = "vec3 " + v + " = mix(" + inputExpr(c, n, 0, F::Vec3) + ", " + inputExpr(c, n, 1, F::Vec3)
                 + ", " + inputExpr(c, n, 2, F::Float) + ");"; break;
        case MatNodeType::OneMinus:
            decl = "vec3 " + v + " = vec3(1.0) - " + inputExpr(c, n, 0, F::Vec3) + ";"; break;
        case MatNodeType::Power:
            decl = "float " + v + " = pow(max(" + inputExpr(c, n, 0, F::Float) + ", 0.0), " + inputExpr(c, n, 1, F::Float) + ");"; break;
        case MatNodeType::Saturate:
            decl = "vec3 " + v + " = clamp(" + inputExpr(c, n, 0, F::Vec3) + ", 0.0, 1.0);"; break;
        case MatNodeType::DotProduct:
            decl = "float " + v + " = dot(" + inputExpr(c, n, 0, F::Vec3) + ", " + inputExpr(c, n, 1, F::Vec3) + ");"; break;
        case MatNodeType::Sine:
            decl = "float " + v + " = sin(" + inputExpr(c, n, 0, F::Float) + ");"; break;
        case MatNodeType::Fresnel:
            // True per-pixel view direction (camera − fragment), not a fixed axis.
            decl = "float " + v + " = pow(1.0 - max(dot(normalize(vNormal), "
                   "normalize(heLight.camPos.xyz - vWorldPos)), 0.0), "
                 + fmtF(std::max(n.p[0], 0.01f)) + ");"; break;
        case MatNodeType::Combine3:
            decl = "vec3 " + v + " = vec3(" + inputExpr(c, n, 0, F::Float) + ", "
                 + inputExpr(c, n, 1, F::Float) + ", " + inputExpr(c, n, 2, F::Float) + ");"; break;

        // ── v2 ──
        case MatNodeType::WorldPos:
            decl = "vec3 " + v + " = vWorldPos;"; break;
        case MatNodeType::ViewDir:
            decl = "vec3 " + v + " = normalize(heLight.camPos.xyz - vWorldPos);"; break;
        case MatNodeType::ParamFloat:
            // Exposed named parameter — emitted as its current value (the name marks it
            // for the Simple-mode parameter surface; true uniforms come with M3.5).
            decl = "float " + v + " = " + fmtF(n.p[0]) + "; // param: " + (n.s.empty() ? "?" : n.s); break;
        case MatNodeType::ParamColor:
            decl = "vec3 " + v + " = vec3(" + fmtF(n.p[0]) + ", " + fmtF(n.p[1]) + ", " + fmtF(n.p[2])
                 + "); // param: " + (n.s.empty() ? "?" : n.s); break;
        case MatNodeType::Subtract:
            decl = "vec3 " + v + " = " + inputExpr(c, n, 0, F::Vec3) + " - " + inputExpr(c, n, 1, F::Vec3) + ";"; break;
        case MatNodeType::Divide:
            decl = "vec3 " + v + " = " + inputExpr(c, n, 0, F::Vec3) + " / max("
                 + inputExpr(c, n, 1, F::Vec3) + ", vec3(1e-5));"; break;
        case MatNodeType::Absolute:
            decl = "vec3 " + v + " = abs(" + inputExpr(c, n, 0, F::Vec3) + ");"; break;
        case MatNodeType::Fract:
            decl = "vec3 " + v + " = fract(" + inputExpr(c, n, 0, F::Vec3) + ");"; break;
        case MatNodeType::Smoothstep:
            decl = "float " + v + " = smoothstep(" + inputExpr(c, n, 0, F::Float) + ", "
                 + inputExpr(c, n, 1, F::Float) + ", " + inputExpr(c, n, 2, F::Float) + ");"; break;
        case MatNodeType::Step:
            decl = "float " + v + " = step(" + inputExpr(c, n, 0, F::Float) + ", "
                 + inputExpr(c, n, 1, F::Float) + ");"; break;
        case MatNodeType::Normalize3:
            decl = "vec3 " + v + " = normalize(" + inputExpr(c, n, 0, F::Vec3) + ");"; break;
        case MatNodeType::Panner:
            decl = "vec2 " + v + " = " + inputExpr(c, n, 0, F::Vec2) + " + vec2("
                 + inputExpr(c, n, 1, F::Float) + ", " + inputExpr(c, n, 2, F::Float)
                 + ") * heLight.sunDir.w;"; break;
        case MatNodeType::ValueNoise:
            c.usesNoise = true;
            decl = "float " + v + " = heValueNoise(" + inputExpr(c, n, 0, F::Vec2) + " * "
                 + inputExpr(c, n, 1, F::Float) + ");"; break;
        case MatNodeType::Fbm:
            c.usesNoise = true;
            decl = "float " + v + " = heFbm(" + inputExpr(c, n, 0, F::Vec2) + " * "
                 + inputExpr(c, n, 1, F::Float) + ");"; break;
        case MatNodeType::Checker:
            decl = "float " + v + " = mod(floor(" + inputExpr(c, n, 0, F::Vec2) + ".x * "
                 + inputExpr(c, n, 1, F::Float) + ") + floor(" + inputExpr(c, n, 0, F::Vec2) + ".y * "
                 + inputExpr(c, n, 1, F::Float) + "), 2.0);"; break;

        case MatNodeType::Output:
            decl = ""; break; // handled by generateFragmentGlsl
    }

    c.body += "    " + decl + "\n";
    c.emitting.erase(n.id);

    // Register every output pin of this node as the same variable (v1 nodes have one output).
    for (int i = 0; i < (int)d.outputs.size(); ++i)
        c.outVar[(static_cast<long long>(n.id) << 8) | i] = v;
    return v;
}

// Expression feeding input pin `pinIdx` of `node`: the linked output (coerced) or the
// pin's default.
std::string inputExpr(EmitCtx& c, const MatGraphNode& node, int pinIdx, MatPinType wantType)
{
    const MatNodeDesc& d = matNodeDesc(node.type);
    for (const MatGraphLink& l : c.g.links)
    {
        if (l.dstNode != node.id || l.dstPin != pinIdx) continue;
        const MatGraphNode* src = c.g.findNode(l.srcNode);
        if (!src) break;
        const MatNodeDesc& sd = matNodeDesc(src->type);
        if (l.srcPin < 0 || l.srcPin >= (int)sd.outputs.size()) break;
        const std::string var = emitNode(c, *src, l.srcPin);
        return coerce(var, sd.outputs[l.srcPin].type, wantType);
    }
    const MatPinDesc& pin = d.inputs[pinIdx];
    return coerce(defaultExpr(pin), pin.type, wantType);
}
} // namespace

std::string generateFragmentGlsl(const MaterialGraph& graph)
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

    if (!out)
        return header + "void main() { oColor = vec4(1.0, 0.0, 1.0, 1.0); } // no Output node\n";

    EmitCtx c{ graph };
    const std::string base  = inputExpr(c, *out, 0, F::Vec3);
    const std::string met   = inputExpr(c, *out, 1, F::Float);
    const std::string rough = inputExpr(c, *out, 2, F::Float);
    const std::string emis  = inputExpr(c, *out, 3, F::Vec3);
    const bool lit = out->p[0] > 0.5f;

    std::string src = header;
    if (c.usesTexture)
        src += "layout(set = 0, binding = 2) uniform sampler2D heTex0;\n";
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
             + emis + ", 1.0);\n";
    else
        src += "    oColor = vec4(" + base + " + " + emis + ", 1.0);\n";
    src += "}\n";
    return src;
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
        if (!n.s.empty()) jn["s"] = n.s; // exposed-parameter name
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
        if (!d) continue; // unknown node type (newer file) — skip, keep the rest
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
