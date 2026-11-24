#include <MaterialGraph/MatCpuEval.h>

#include <algorithm>
#include <cmath>
#include <set>

// See MatCpuEval.h. Every rule below is generateFragment's (MaterialGraph.cpp),
// restated for values instead of text: where a comment names a codegen helper,
// that helper is the reference and this has to agree with it.
namespace HE
{
using F = MatPinType;
using Op = MatCpuProgram::Op;

bool matCpuSupports(MatNodeType t)
{
    switch (t)
    {
    case MatNodeType::Output:
    case MatNodeType::ConstFloat: case MatNodeType::ConstColor: case MatNodeType::ConstVec2:
    case MatNodeType::ConstVec4:  case MatNodeType::ConstBool:
    case MatNodeType::VertexColor: case MatNodeType::UV: case MatNodeType::Time:
    case MatNodeType::ParamFloat: case MatNodeType::ParamColor: case MatNodeType::ParamVec2:
    case MatNodeType::ParamVec4:  case MatNodeType::ParamBool:
    case MatNodeType::TextureSample:
    case MatNodeType::Add: case MatNodeType::Multiply: case MatNodeType::Lerp:
    case MatNodeType::OneMinus: case MatNodeType::Power: case MatNodeType::Saturate:
    case MatNodeType::DotProduct: case MatNodeType::Sine: case MatNodeType::Combine3:
    case MatNodeType::Subtract: case MatNodeType::Divide: case MatNodeType::Absolute:
    case MatNodeType::Fract: case MatNodeType::Smoothstep: case MatNodeType::Step:
    case MatNodeType::Normalize3:
    case MatNodeType::SplitRGBA: case MatNodeType::CombineRGBA:
    case MatNodeType::If: case MatNodeType::Greater: case MatNodeType::Less:
    case MatNodeType::GreaterEqual: case MatNodeType::LessEqual: case MatNodeType::Equal:
    case MatNodeType::NotEqual: case MatNodeType::And: case MatNodeType::Or: case MatNodeType::Not:
    case MatNodeType::Panner: case MatNodeType::Checker:
    case MatNodeType::StaticSwitch: case MatNodeType::Reroute:
    case MatNodeType::FnInput: case MatNodeType::FnOutput: case MatNodeType::FunctionCall:
    case MatNodeType::ElementSize: case MatNodeType::ElementUV: case MatNodeType::RoundedRectSDF:
    case MatNodeType::BorderDistance: case MatNodeType::ElementState: case MatNodeType::Backdrop:
        return true;
    default:
        return false;
    }
}

namespace
{
    // pinTypeFromParam in MaterialGraph.cpp.
    F pinTypeFromParam(float p)
    {
        switch (static_cast<int>(p))
        {
        case 0: return F::Float;
        case 1: return F::Vec2;
        case 3: return F::Vec4;
        default: return F::Vec3;
        }
    }

    // fnInterfaceNodes: the interface nodes of a function, sorted by id.
    std::vector<const MatGraphNode*> interfaceNodes(const MaterialGraph& g, MatNodeType t)
    {
        std::vector<const MatGraphNode*> out;
        for (const MatGraphNode& n : g.nodes) if (n.type == t) out.push_back(&n);
        std::sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->id < b->id; });
        return out;
    }

    // heUIBoxSDF, the rounded box every UI shader and the rasterizer share.
    float boxSDF(glm::vec2 p, glm::vec2 b, const glm::vec4& r)
    {
        float rad = (p.x >= 0.0f) ? ((p.y >= 0.0f) ? r.z : r.y) : ((p.y >= 0.0f) ? r.w : r.x);
        rad = std::clamp(rad, 0.0f, std::min(b.x, b.y));
        const glm::vec2 q = glm::abs(p) - b + glm::vec2(rad);
        return std::min(std::max(q.x, q.y), 0.0f) + glm::length(glm::max(q, glm::vec2(0.0f))) - rad;
    }

    struct Val { int reg = -1; F type = F::Float; };

    struct Scope
    {
        const MaterialGraph* g = nullptr;
        std::string key;
        const Scope* parent = nullptr;
        const MatGraphNode* call = nullptr;
    };

    struct Compiler
    {
        MatCpuProgram& P;
        const MatFunctionLoader& loader;
        const std::map<std::string, bool>* switchOv;
        std::vector<std::pair<std::string, bool>> switches;
        std::map<std::string, std::vector<Val>> memo;
        std::set<std::string> emitting;
        std::vector<std::string> fnStack;
        std::string error;
        bool uiDomain = false;   // texture reads flip V, as generateFragment's do

        int reg() { return P.m_regs++; }

        Val push(Op op, F type, int a = -1, int b = -1, int c = -1, int d = -1,
                 glm::vec4 k = glm::vec4(0.0f), int aux = 0)
        {
            MatCpuProgram::Instr in;
            in.op = op; in.dst = reg(); in.a = a; in.b = b; in.c = c; in.d = d; in.k = k; in.aux = aux;
            P.m_code.push_back(in);
            return { in.dst, type };
        }
        Val constant(glm::vec4 k, F type) { return push(Op::Const, type, -1, -1, -1, -1, k); }
        Val magenta() { return constant({ 1.0f, 0.0f, 1.0f, 0.0f }, F::Vec3); }

        // coerce(): float splats up, vec3→vec4 appends alpha 1, the rest
        // truncates or zero-extends.
        Val coerce(Val v, F to)
        {
            if (v.type == to) return v;
            return push(Op::Coerce, to, v.reg, -1, -1, -1, glm::vec4(0.0f),
                        static_cast<int>(v.type) * 4 + static_cast<int>(to));
        }

        // defaultExpr(): a pin's literal default in its own type.
        Val pinDefault(const MatPinDesc& pin)
        {
            const float d = pin.def;
            switch (pin.type)
            {
            case F::Float: return constant(glm::vec4(d, 0, 0, 0), F::Float);
            case F::Vec2:  return constant(glm::vec4(d, d, 0, 0), F::Vec2);
            case F::Vec3:  return constant(glm::vec4(d, d, d, 0), F::Vec3);
            case F::Vec4:  return constant(glm::vec4(d, d, d, 1.0f), F::Vec4);
            }
            return constant(glm::vec4(0.0f), F::Float);
        }

        const MaterialGraph* function(const std::string& path)
        {
            return loader ? loader(path) : nullptr;
        }

        // inputExpr(): what feeds input `pin`, coerced to `want`; unconnected,
        // the pin default (a FunctionCall's from its FnInput).
        Val input(const Scope& sc, const MatGraphNode& n, int pin, F want)
        {
            for (const MatGraphLink& l : sc.g->links)
            {
                if (l.dstNode != n.id || l.dstPin != pin) continue;
                const MatGraphNode* src = sc.g->findNode(l.srcNode);
                if (!src) break;
                return coerce(output(sc, *src, l.srcPin), want);
            }
            if (n.type == MatNodeType::FunctionCall)
            {
                float def = kMatFnInputLegacyDefault;
                if (const MaterialGraph* fn = function(n.s))
                {
                    const auto ins = interfaceNodes(*fn, MatNodeType::FnInput);
                    if (pin >= 0 && pin < static_cast<int>(ins.size()) && ins[pin]->p[2] >= 0.5f)
                        def = ins[pin]->p[1];
                }
                return coerce(constant(glm::vec4(def, 0, 0, 0), F::Float), want);
            }
            const MatNodeDesc& d = matNodeDesc(n.type);
            if (pin >= 0 && pin < static_cast<int>(d.inputs.size()))
                return coerce(pinDefault(d.inputs[pin]), want);
            return coerce(constant(glm::vec4(0.0f), F::Float), want);
        }

        bool connected(const Scope& sc, const MatGraphNode& n, int pin)
        {
            for (const MatGraphLink& l : sc.g->links)
                if (l.dstNode == n.id && l.dstPin == pin) return true;
            return false;
        }

        // uvInput(): an unconnected procedural UV is the element's UV.
        Val uvInput(const Scope& sc, const MatGraphNode& n, int pin)
        {
            return connected(sc, n, pin) ? input(sc, n, pin, F::Vec2)
                                         : push(Op::UV, F::Vec2, -1, -1, -1, -1, glm::vec4(1, 1, 0, 0));
        }

        Val extract(Val v, int comp) { return push(Op::Extract, F::Float, v.reg, -1, -1, -1, glm::vec4(0.0f), comp); }

        int paramIndex(const MatGraphNode& n, int kind)
        {
            const std::string name = n.s.empty() ? ("param_" + std::to_string(n.id)) : n.s;
            for (size_t i = 0; i < P.m_params.size(); ++i)
                if (P.m_params[i].name == name) return static_cast<int>(i);
            MatCpuProgram::ParamRef r;
            r.name = name; r.kind = kind;
            // matParamKindComponents: Float/Bool keep 1, Vec2 2, Color 3, Vec4 4.
            const int keep = kind == 1 ? 3 : kind == 2 ? 2 : kind == 3 ? 4 : 1;
            for (int i = 0; i < 4; ++i) r.value[i] = i < keep ? n.p[i] : 0.0f;
            P.m_params.push_back(r);
            return static_cast<int>(P.m_params.size()) - 1;
        }

        int textureSlot(const std::string& path)
        {
            if (path.empty()) return -1;   // heTex0: white for a widget
            for (size_t i = 0; i < P.m_textures.size(); ++i)
                if (P.m_textures[i] == path) return static_cast<int>(i);
            if (static_cast<int>(P.m_textures.size()) >= kMatMaxGraphTextures) return -1;
            P.m_textures.push_back(path);
            return static_cast<int>(P.m_textures.size()) - 1;
        }

        Val output(const Scope& sc, const MatGraphNode& n, int pin)
        {
            const std::vector<Val> pins = node(sc, n);
            if (pin >= 0 && pin < static_cast<int>(pins.size())) return pins[pin];
            return pins.empty() ? magenta() : pins[0];
        }

        std::vector<Val> node(const Scope& sc, const MatGraphNode& n)
        {
            const std::string key = sc.key + ":" + std::to_string(n.id);
            if (auto it = memo.find(key); it != memo.end()) return it->second;
            if (emitting.count(key)) return { magenta() };   // a cycle, as codegen answers it
            emitting.insert(key);
            std::vector<Val> pins = build(sc, n);
            emitting.erase(key);
            memo[key] = pins;
            return pins;
        }

        std::vector<Val> unsupported(const MatGraphNode& n)
        {
            if (error.empty()) error = matNodeDesc(n.type).name;
            return { magenta() };
        }

        std::vector<Val> build(const Scope& sc, const MatGraphNode& n)
        {
            if (!matCpuSupports(n.type)) return unsupported(n);
            switch (n.type)
            {
            case MatNodeType::ConstFloat: return { constant({ n.p[0], 0, 0, 0 }, F::Float) };
            case MatNodeType::ConstColor: return { constant({ n.p[0], n.p[1], n.p[2], 0 }, F::Vec3) };
            case MatNodeType::ConstVec2:  return { constant({ n.p[0], n.p[1], 0, 0 }, F::Vec2) };
            case MatNodeType::ConstVec4:  return { constant({ n.p[0], n.p[1], n.p[2], n.p[3] }, F::Vec4) };
            case MatNodeType::ConstBool:  return { constant({ n.p[0] > 0.5f ? 1.0f : 0.0f, 0, 0, 0 }, F::Float) };
            case MatNodeType::VertexColor: return { push(Op::VColor, F::Vec3) };
            case MatNodeType::UV:
                return { push(Op::UV, F::Vec2, -1, -1, -1, -1, { n.p[0], n.p[1], n.p[2], n.p[3] }) };
            case MatNodeType::Time: P.m_usesTime = true; return { push(Op::Time, F::Float) };
            // MatParamKind: Float 0, Color 1, Vec2 2, Vec4 3, Bool 4.
            case MatNodeType::ParamFloat: return { push(Op::Param, F::Float, -1, -1, -1, -1, {}, paramIndex(n, 0)) };
            case MatNodeType::ParamColor: return { push(Op::Param, F::Vec3,  -1, -1, -1, -1, {}, paramIndex(n, 1)) };
            case MatNodeType::ParamVec2:  return { push(Op::Param, F::Vec2,  -1, -1, -1, -1, {}, paramIndex(n, 2)) };
            case MatNodeType::ParamVec4:  return { push(Op::Param, F::Vec4,  -1, -1, -1, -1, {}, paramIndex(n, 3)) };
            case MatNodeType::ParamBool:  return { push(Op::Param, F::Float, -1, -1, -1, -1, {}, paramIndex(n, 4)) };
            case MatNodeType::TextureSample:
            {
                const Val uv = input(sc, n, 0, F::Vec2);
                // k.x = 1: the UI domain reads the picture upright (see the codegen).
                const Val t = push(Op::TexSample, F::Vec4, uv.reg, -1, -1, -1,
                                   glm::vec4(uiDomain ? 1.0f : 0.0f, 0, 0, 0), textureSlot(n.s));
                return { { t.reg, F::Vec3 }, extract(t, 3) };
            }
            case MatNodeType::Add: case MatNodeType::Multiply:
            case MatNodeType::Subtract: case MatNodeType::Divide: case MatNodeType::DotProduct:
            {
                const Val b = input(sc, n, 1, F::Vec3), a = input(sc, n, 0, F::Vec3);
                const Op op = n.type == MatNodeType::Add ? Op::Add : n.type == MatNodeType::Multiply ? Op::Mul
                            : n.type == MatNodeType::Subtract ? Op::Sub : n.type == MatNodeType::Divide ? Op::Div : Op::Dot;
                return { push(op, op == Op::Dot ? F::Float : F::Vec3, a.reg, b.reg) };
            }
            case MatNodeType::Lerp:
            {
                const Val t = input(sc, n, 2, F::Float), b = input(sc, n, 1, F::Vec3), a = input(sc, n, 0, F::Vec3);
                return { push(Op::Lerp, F::Vec3, a.reg, b.reg, t.reg) };
            }
            case MatNodeType::OneMinus: return { push(Op::OneMinus, F::Vec3, input(sc, n, 0, F::Vec3).reg) };
            case MatNodeType::Saturate: return { push(Op::Saturate, F::Vec3, input(sc, n, 0, F::Vec3).reg) };
            case MatNodeType::Absolute: return { push(Op::Abs, F::Vec3, input(sc, n, 0, F::Vec3).reg) };
            case MatNodeType::Fract:    return { push(Op::Fract, F::Vec3, input(sc, n, 0, F::Vec3).reg) };
            case MatNodeType::Normalize3: return { push(Op::Normalize3, F::Vec3, input(sc, n, 0, F::Vec3).reg) };
            case MatNodeType::Sine:     return { push(Op::Sine, F::Float, input(sc, n, 0, F::Float).reg) };
            case MatNodeType::Power:
            {
                const Val e = input(sc, n, 1, F::Float), b = input(sc, n, 0, F::Float);
                return { push(Op::Power, F::Float, b.reg, e.reg) };
            }
            case MatNodeType::Combine3:
            {
                const Val z = input(sc, n, 2, F::Float), y = input(sc, n, 1, F::Float), x = input(sc, n, 0, F::Float);
                return { push(Op::Combine3, F::Vec3, x.reg, y.reg, z.reg) };
            }
            case MatNodeType::Smoothstep:
            {
                const Val x = input(sc, n, 2, F::Float), e1 = input(sc, n, 1, F::Float), e0 = input(sc, n, 0, F::Float);
                return { push(Op::Smoothstep, F::Float, e0.reg, e1.reg, x.reg) };
            }
            case MatNodeType::Step:
            {
                const Val x = input(sc, n, 1, F::Float), e = input(sc, n, 0, F::Float);
                return { push(Op::Step, F::Float, e.reg, x.reg) };
            }
            case MatNodeType::SplitRGBA:
            {
                const Val v = input(sc, n, 0, F::Vec4);
                return { extract(v, 0), extract(v, 1), extract(v, 2), extract(v, 3) };
            }
            case MatNodeType::CombineRGBA:
            {
                const Val w = input(sc, n, 3, F::Float), z = input(sc, n, 2, F::Float),
                          y = input(sc, n, 1, F::Float), x = input(sc, n, 0, F::Float);
                return { push(Op::CombineRGBA, F::Vec4, x.reg, y.reg, z.reg, w.reg) };
            }
            case MatNodeType::If:
            {
                // A selection, which is what codegen's guarded branch computes
                // (and mix(no, yes, step(0.5, c)) wherever no side is NaN).
                const Val c = input(sc, n, 0, F::Float);
                const Val yes = input(sc, n, 1, F::Vec3), no = input(sc, n, 2, F::Vec3);
                return { push(Op::Select, F::Vec3, c.reg, yes.reg, no.reg) };
            }
            case MatNodeType::Greater: case MatNodeType::Less: case MatNodeType::GreaterEqual:
            case MatNodeType::LessEqual: case MatNodeType::Equal: case MatNodeType::NotEqual:
            case MatNodeType::And: case MatNodeType::Or:
            {
                const Val b = input(sc, n, 1, F::Float), a = input(sc, n, 0, F::Float);
                const Op op = n.type == MatNodeType::Greater ? Op::Greater : n.type == MatNodeType::Less ? Op::Less
                            : n.type == MatNodeType::GreaterEqual ? Op::GreaterEq : n.type == MatNodeType::LessEqual ? Op::LessEq
                            : n.type == MatNodeType::Equal ? Op::Equal : n.type == MatNodeType::NotEqual ? Op::NotEqual
                            : n.type == MatNodeType::And ? Op::And : Op::Or;
                return { push(op, F::Float, a.reg, b.reg) };
            }
            case MatNodeType::Not: return { push(Op::Not, F::Float, input(sc, n, 0, F::Float).reg) };
            case MatNodeType::Panner:
            {
                const Val sy = input(sc, n, 2, F::Float), sx = input(sc, n, 1, F::Float), uv = input(sc, n, 0, F::Vec2);
                P.m_usesTime = true;
                return { push(Op::Panner, F::Vec2, uv.reg, sx.reg, sy.reg) };
            }
            case MatNodeType::Checker:
            {
                const Val uv = uvInput(sc, n, 0);
                const Val s = input(sc, n, 1, F::Float);
                return { push(Op::Checker, F::Float, uv.reg, s.reg) };
            }
            case MatNodeType::StaticSwitch:
            {
                const std::string name = n.s.empty() ? ("switch_" + std::to_string(n.id)) : n.s;
                bool on = n.p[0] > 0.5f;
                if (switchOv)
                    if (auto it = switchOv->find(name); it != switchOv->end()) on = it->second;
                bool seen = false;
                for (const auto& s : switches) if (s.first == name) { on = s.second; seen = true; break; }
                if (!seen) switches.push_back({ name, on });
                return { input(sc, n, on ? 0 : 1, F::Vec3) };
            }
            case MatNodeType::Reroute:
                return { push(Op::Copy, F::Vec4, input(sc, n, 0, F::Vec4).reg) };
            case MatNodeType::FnInput:
            {
                const F t = pinTypeFromParam(n.p[0]);
                if (sc.parent && sc.call)
                {
                    const auto ins = interfaceNodes(*sc.g, MatNodeType::FnInput);
                    int idx = 0;
                    for (size_t i = 0; i < ins.size(); ++i) if (ins[i]->id == n.id) idx = static_cast<int>(i);
                    return { input(*sc.parent, *sc.call, idx, t) };
                }
                return { coerce(constant(glm::vec4(0.5f), F::Float), t) };
            }
            case MatNodeType::FnOutput:
                return { input(sc, n, 0, pinTypeFromParam(n.p[0])) };
            case MatNodeType::FunctionCall:
            {
                const MaterialGraph* fn = function(n.s);
                const bool recursing = std::find(fnStack.begin(), fnStack.end(), n.s) != fnStack.end();
                if (!fn || recursing || fnStack.size() >= 8) return { magenta() };
                const auto outs = interfaceNodes(*fn, MatNodeType::FnOutput);
                if (outs.empty()) return { magenta() };
                fnStack.push_back(n.s);
                Scope child;
                child.g = fn;
                child.key = sc.key + "/" + std::to_string(n.id);
                child.parent = &sc;
                child.call = &n;
                std::vector<Val> pins;
                for (const MatGraphNode* o : outs)
                {
                    Val v = output(child, *o, 0);
                    v.type = pinTypeFromParam(o->p[0]);
                    pins.push_back(v);
                }
                fnStack.pop_back();
                return pins;
            }
            case MatNodeType::ElementSize:
            {
                const Val s = push(Op::ElemSize, F::Vec2);
                return { s, extract(s, 0), extract(s, 1) };
            }
            case MatNodeType::ElementUV: return { push(Op::ElemUV, F::Vec2) };
            case MatNodeType::BorderDistance: return { push(Op::BorderDist, F::Float) };
            case MatNodeType::ElementState:
            {
                const Val s = push(Op::ElemState, F::Vec4);
                return { extract(s, 0), extract(s, 1), extract(s, 2), extract(s, 3) };
            }
            case MatNodeType::RoundedRectSDF:
            {
                const Val rad = input(sc, n, 0, F::Float), ins = input(sc, n, 1, F::Float);
                const Val d = push(Op::RRectSDF, F::Float, rad.reg, ins.reg);
                return { d, push(Op::Mask, F::Float, d.reg) };
            }
            case MatNodeType::Backdrop:
            {
                const Val rad = input(sc, n, 0, F::Float);
                P.m_usesBackdrop = true;
                return { push(Op::Backdrop, F::Vec3, rad.reg) };
            }
            default:
                return unsupported(n);
            }
        }
    };
}

bool MatCpuProgram::compile(const MaterialGraph& graph, const MatFunctionLoader& loader,
                            const std::map<std::string, bool>* switchOverrides, std::string* error)
{
    *this = MatCpuProgram{};
    const MatGraphNode* out = nullptr;
    for (const MatGraphNode& n : graph.nodes)
        if (n.type == MatNodeType::Output) { out = &n; break; }
    if (!out)
    {
        if (error) *error = "no Output node";
        return false;
    }
    // generateFragment: a UI material is unlit whatever the toggle says; a lit
    // Surface material needs the scene's light, which a widget does not have.
    const bool uiDomain = static_cast<int>(out->p[3]) == static_cast<int>(MatDomain::UserInterface);
    if (!uiDomain && out->p[0] > 0.5f)
    {
        if (error) *error = "a lit Surface material (set the Output's domain to User Interface)";
        return false;
    }

    Compiler c{ *this, loader, switchOverrides, {}, {}, {}, {}, {}, uiDomain };
    Scope root;
    root.g = &graph;
    const int blendMode = std::clamp(static_cast<int>(out->p[1]), 0, 2);
    m_cutoff = out->p[2] > 0.0f ? out->p[2] : 0.5f;
    if (blendMode == static_cast<int>(MatBlendMode::Masked))
        m_mask = c.input(root, *out, kMatOutputOpacityPin, F::Float).reg;
    m_base = c.input(root, *out, kMatOutputBaseColorPin, F::Vec3).reg;
    m_emis = c.input(root, *out, kMatOutputEmissivePin, F::Vec3).reg;
    if (blendMode == static_cast<int>(MatBlendMode::Translucent))
        m_opacity = c.input(root, *out, kMatOutputOpacityPin, F::Float).reg;

    if (!c.error.empty())
    {
        if (error) *error = c.error;
        *this = MatCpuProgram{};
        return false;
    }
    m_valid = true;
    return true;
}

void MatCpuProgram::bindParams(const std::vector<std::string>& names, const std::vector<float>& data)
{
    for (ParamRef& p : m_params)
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == p.name && data.size() >= i * 4 + 4)
            {
                p.value = { data[i * 4], data[i * 4 + 1], data[i * 4 + 2], data[i * 4 + 3] };
                break;
            }
}

glm::vec4 MatCpuProgram::eval(const MatCpuElement& e, glm::vec2 uv, const glm::vec3& color, float time,
                              const MatCpuHooks& hooks, bool& discard) const
{
    discard = false;
    if (!m_valid) return glm::vec4(1.0f, 0.0f, 1.0f, 1.0f);
    // Per thread: the stripes of one draw run this concurrently.
    thread_local std::vector<glm::vec4> regs;
    if (regs.size() < static_cast<size_t>(m_regs)) regs.resize(static_cast<size_t>(m_regs));
    glm::vec4* r = regs.data();
    auto f = [&](int i) { return r[i].x; };
    auto v3 = [&](int i) { return glm::vec3(r[i]); };

    for (const Instr& in : m_code)
    {
        glm::vec4& d = r[in.dst];
        switch (in.op)
        {
        case Op::Const:     d = in.k; break;
        case Op::VColor:    d = glm::vec4(color, 0.0f); break;
        case Op::UV:
            // Identity tiling is the bare varying (codegen's byte-equal case).
            d = glm::vec4(uv * glm::vec2(in.k.x, in.k.y) + glm::vec2(in.k.z, in.k.w), 0.0f, 0.0f);
            break;
        case Op::Time:      d = glm::vec4(time, 0, 0, 0); break;
        case Op::Param:
        {
            const ParamRef& p = m_params[in.aux];
            d = p.kind == 4 ? glm::vec4(p.value.x >= 0.5f ? 1.0f : 0.0f, 0, 0, 0) : p.value;
            break;
        }
        case Op::ElemSize:  d = glm::vec4(e.size, 0, 0); break;
        case Op::ElemUV:    d = glm::vec4(uv, 0, 0); break;
        case Op::BorderDist: d = glm::vec4(-boxSDF((uv - 0.5f) * e.size, e.size * 0.5f, e.radius), 0, 0, 0); break;
        case Op::ElemState: d = e.state; break;
        case Op::Extract:   d = glm::vec4(r[in.a][in.aux], 0, 0, 0); break;
        case Op::Coerce:
        {
            const glm::vec4 s = r[in.a];
            const F from = static_cast<F>(in.aux / 4), to = static_cast<F>(in.aux % 4);
            if (from == F::Float)      d = glm::vec4(s.x);                          // splat (vec4 too)
            else if (to == F::Float)   d = glm::vec4(s.x, 0, 0, 0);
            else if (from == F::Vec2 && to == F::Vec3) d = glm::vec4(s.x, s.y, 0, 0);
            else if (from == F::Vec2 && to == F::Vec4) d = glm::vec4(s.x, s.y, 0, 1);
            else if (to == F::Vec2)    d = glm::vec4(s.x, s.y, 0, 0);
            else if (from == F::Vec3 && to == F::Vec4) d = glm::vec4(s.x, s.y, s.z, 1);
            else                       d = glm::vec4(s.x, s.y, s.z, 0);          // vec4 → vec3
            break;
        }
        case Op::Add:   d = glm::vec4(v3(in.a) + v3(in.b), 0); break;
        case Op::Sub:   d = glm::vec4(v3(in.a) - v3(in.b), 0); break;
        case Op::Mul:   d = glm::vec4(v3(in.a) * v3(in.b), 0); break;
        case Op::Div:   d = glm::vec4(v3(in.a) / glm::max(v3(in.b), glm::vec3(1e-5f)), 0); break;
        case Op::Lerp:  d = glm::vec4(glm::mix(v3(in.a), v3(in.b), f(in.c)), 0); break;
        case Op::OneMinus: d = glm::vec4(glm::vec3(1.0f) - v3(in.a), 0); break;
        case Op::Power: d = glm::vec4(std::pow(std::max(f(in.a), 0.0f), f(in.b)), 0, 0, 0); break;
        case Op::Saturate: d = glm::vec4(glm::clamp(v3(in.a), 0.0f, 1.0f), 0); break;
        case Op::Dot:   d = glm::vec4(glm::dot(v3(in.a), v3(in.b)), 0, 0, 0); break;
        case Op::Sine:  d = glm::vec4(std::sin(f(in.a)), 0, 0, 0); break;
        case Op::Combine3: d = glm::vec4(f(in.a), f(in.b), f(in.c), 0); break;
        case Op::Abs:   d = glm::vec4(glm::abs(v3(in.a)), 0); break;
        case Op::Fract: { const glm::vec3 x = v3(in.a); d = glm::vec4(x - glm::floor(x), 0); break; }
        case Op::Smoothstep:
        {
            const float e0 = f(in.a), e1 = f(in.b), x = f(in.c);
            const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
            d = glm::vec4(t * t * (3.0f - 2.0f * t), 0, 0, 0);
            break;
        }
        case Op::Step:  d = glm::vec4(f(in.b) < f(in.a) ? 0.0f : 1.0f, 0, 0, 0); break;
        case Op::Normalize3: d = glm::vec4(v3(in.a) / glm::length(v3(in.a)), 0); break;
        case Op::CombineRGBA: d = glm::vec4(f(in.a), f(in.b), f(in.c), f(in.d)); break;
        case Op::Select: d = glm::vec4(f(in.a) >= 0.5f ? v3(in.b) : v3(in.c), 0); break;
        case Op::Greater:   d = glm::vec4(f(in.a) >  f(in.b) ? 1.0f : 0.0f, 0, 0, 0); break;
        case Op::Less:      d = glm::vec4(f(in.a) <  f(in.b) ? 1.0f : 0.0f, 0, 0, 0); break;
        case Op::GreaterEq: d = glm::vec4(f(in.a) >= f(in.b) ? 1.0f : 0.0f, 0, 0, 0); break;
        case Op::LessEq:    d = glm::vec4(f(in.a) <= f(in.b) ? 1.0f : 0.0f, 0, 0, 0); break;
        case Op::Equal:     d = glm::vec4(std::abs(f(in.a) - f(in.b)) <  1e-4f ? 1.0f : 0.0f, 0, 0, 0); break;
        case Op::NotEqual:  d = glm::vec4(std::abs(f(in.a) - f(in.b)) >= 1e-4f ? 1.0f : 0.0f, 0, 0, 0); break;
        case Op::And: d = glm::vec4(f(in.a) > 0.5f && f(in.b) > 0.5f ? 1.0f : 0.0f, 0, 0, 0); break;
        case Op::Or:  d = glm::vec4(f(in.a) > 0.5f || f(in.b) > 0.5f ? 1.0f : 0.0f, 0, 0, 0); break;
        case Op::Not: d = glm::vec4(f(in.a) <= 0.5f ? 1.0f : 0.0f, 0, 0, 0); break;
        case Op::Panner:
            d = glm::vec4(glm::vec2(r[in.a]) + glm::vec2(f(in.b), f(in.c)) * time, 0, 0);
            break;
        case Op::Checker:
        {
            const glm::vec2 p = glm::vec2(r[in.a]);
            const float s = f(in.b);
            const float x = std::floor(p.x * s) + std::floor(p.y * s);
            d = glm::vec4(x - 2.0f * std::floor(x / 2.0f), 0, 0, 0);   // GLSL mod(x, 2.0)
            break;
        }
        case Op::TexSample:
        {
            glm::vec2 tuv(r[in.a]);
            if (in.k.x > 0.5f) tuv.y = 1.0f - tuv.y;
            d = (in.aux >= 0 && hooks.sample) ? hooks.sample(hooks.user, in.aux, tuv) : glm::vec4(1.0f);
            break;
        }
        case Op::RRectSDF:
        {
            const float rad = f(in.a), ins = f(in.b);
            d = glm::vec4(boxSDF((uv - 0.5f) * e.size, glm::max(e.size * 0.5f - glm::vec2(ins), glm::vec2(0.0f)),
                                 glm::vec4(rad)), 0, 0, 0);
            break;
        }
        case Op::Mask: d = glm::vec4(std::clamp(0.5f - f(in.a), 0.0f, 1.0f), 0, 0, 0); break;
        case Op::Backdrop:
        {
            const glm::vec2 vp = glm::max(e.screen, glm::vec2(1.0f));
            const glm::vec2 suv = (e.pos + uv * e.size) / vp;
            d = glm::vec4(hooks.backdrop ? hooks.backdrop(hooks.user, suv, std::max(f(in.a), 0.0f))
                                         : glm::vec3(0.0f), 0);
            break;
        }
        case Op::Copy: d = r[in.a]; break;
        }
    }

    if (m_mask >= 0 && r[m_mask].x < m_cutoff) { discard = true; return glm::vec4(0.0f); }
    const glm::vec3 rgb = v3(m_base) + v3(m_emis);
    return glm::vec4(rgb, m_opacity >= 0 ? r[m_opacity].x : 1.0f);
}

} // namespace HE
