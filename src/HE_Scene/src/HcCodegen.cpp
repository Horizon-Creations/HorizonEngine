#include "HorizonScene/HcCodegen.h"
#include "HorizonScene/EngineApi.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

// The HorizonCode → C++ emitter. Structure (plan §5): Stage A validates a graph
// (or sends it back to the interpreter as a Fallback), Stage B/C walk the exec
// chains exactly like Runner::runExecChain and emit one CompiledInstance
// subclass per class. Every lowering mirrors a specific interpreter behavior in
// HorizonCode.cpp — the semantic contract is plan §3; where a choice is subtle
// the comment cites the clause.

namespace HE::hccg {

using HorizonCode::Graph;
using HorizonCode::Link;
using HorizonCode::Node;
using HorizonCode::NodeSig;
using HorizonCode::NodeType;
using HorizonCode::PinType;
using HorizonCode::Value;
using HorizonCode::Variable;
using NT = NodeType;
using PT = PinType;

namespace {

// A class that can't compile bails out to the interpreter with a reason.
struct FallbackError { std::string reason; };

// ── pin range helpers (same unified index space as the interpreter) ──────────
struct PinRanges { int execIn0, execOut0, dataIn0, dataOut0, end; };
PinRanges pinRanges(const Node& n)
{
    const NodeSig s = HorizonCode::signatureOf(n);
    PinRanges r;
    r.execIn0  = 0;
    r.execOut0 = r.execIn0  + (int)s.execIns.size();
    r.dataIn0  = r.execOut0 + (int)s.execOuts.size();
    r.dataOut0 = r.dataIn0  + (int)s.dataIns.size();
    r.end      = r.dataOut0 + (int)s.dataOuts.size();
    return r;
}

struct TypeRef { PinType t = PT::Float; bool arr = false; };

TypeRef dataInType(const Node& n, int idx)
{
    const NodeSig s = HorizonCode::signatureOf(n);
    if (idx < 0 || idx >= (int)s.dataIns.size()) return {};
    return { s.dataIns[idx].type, s.dataIns[idx].isArray };
}
TypeRef dataOutType(const Node& n, int idx)
{
    const NodeSig s = HorizonCode::signatureOf(n);
    if (idx < 0 || idx >= (int)s.dataOuts.size()) return {};
    return { s.dataOuts[idx].type, s.dataOuts[idx].isArray };
}

// ── C++ type/literal tables (plan §6) ────────────────────────────────────────
const char* cppScalar(PinType t)
{
    switch (t)
    {
        case PT::Float:     return "float";
        case PT::Bool:      return "bool";
        case PT::Int:       return "int";
        case PT::String:    return "std::string";
        case PT::Vec2:      return "glm::vec2";
        case PT::Color:     return "glm::vec4";
        case PT::Ref:       return "uint32_t";
        case PT::Transform: return "hc::Transform";
        default:            return "float";
    }
}
std::string cppType(TypeRef tr)
{
    if (tr.arr) return std::string("hc::Array<") + cppScalar(tr.t) + ">";
    return cppScalar(tr.t);
}

std::string floatLit(float f)
{
    if (!std::isfinite(f)) return "0.0f";   // not authorable; keep generated code sane
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.9g", (double)f);
    std::string s = buf;
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s + "f";
}

std::string strLit(const std::string& s)
{
    std::string out = "\"";
    for (const unsigned char c : s)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\%03o", c); out += b; }
                else out += (char)c;
        }
    }
    return out + "\"";
}

// The zero value of a pin type — a fresh Value's field (§3.3): note Color's
// alpha 1 and Transform's identity scale (hc::zeroOf mirrors this at runtime).
std::string zeroLit(TypeRef tr)
{
    if (tr.arr) return cppType(tr) + "{}";
    switch (tr.t)
    {
        case PT::Float:     return "0.0f";
        case PT::Bool:      return "false";
        case PT::Int:       return "0";
        case PT::String:    return "std::string()";
        case PT::Vec2:      return "glm::vec2(0.0f, 0.0f)";
        case PT::Color:     return "glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)";
        case PT::Ref:       return "0u";
        case PT::Transform: return "hc::Transform{}";
        default:            return "0.0f";
    }
}

// A scalar Value as a C++ literal of its own type.
std::string valueLit(const Value& v)
{
    switch (v.type)
    {
        case PT::Float:  return floatLit(v.f);
        case PT::Bool:   return v.b ? "true" : "false";
        case PT::Int:    return std::to_string(v.i);
        case PT::String: return strLit(v.s);
        case PT::Vec2:   return "glm::vec2(" + floatLit(v.v2.x) + ", " + floatLit(v.v2.y) + ")";
        case PT::Color:  return "glm::vec4(" + floatLit(v.col.x) + ", " + floatLit(v.col.y) + ", " +
                                 floatLit(v.col.z) + ", " + floatLit(v.col.w) + ")";
        case PT::Ref:    return std::to_string(v.ref) + "u";
        case PT::Transform:
            return "hc::Transform{ glm::vec3(" + floatLit(v.tpos.x) + ", " + floatLit(v.tpos.y) + ", " + floatLit(v.tpos.z) +
                   "), glm::vec3(" + floatLit(v.trot.x) + ", " + floatLit(v.trot.y) + ", " + floatLit(v.trot.z) +
                   "), glm::vec3(" + floatLit(v.tscl.x) + ", " + floatLit(v.tscl.y) + ", " + floatLit(v.tscl.z) + ") }";
        default:         return "0.0f";
    }
}

// Generation-time copy of the interpreter's coerce (§3.3) — used to constant-
// fold pin defaults to the pin's type.
Value coerceValue(Value v, PinType want)
{
    if (v.isArray) return v;
    if (v.type == want) return v;
    Value r; r.type = want;
    switch (want)
    {
        case PT::Float: r.f = v.type == PT::Bool ? (v.b ? 1.0f : 0.0f)
                            : v.type == PT::Int ? (float)v.i : 0.0f; break;
        case PT::Int:   r.i = v.type == PT::Float ? (int)v.f
                            : v.type == PT::Bool ? (v.b ? 1 : 0) : 0; break;
        case PT::Bool:  r.b = v.type == PT::Float ? v.f != 0.0f
                            : v.type == PT::Int ? v.i != 0 : false; break;
        default: break;
    }
    return r;
}

// Static conversion between statically-known pin types — the compile-time
// counterpart of coerce for typed C++ expressions. Anything the interpreter
// can't convert becomes the target's zero value.
std::string convertExpr(const std::string& e, TypeRef from, TypeRef to)
{
    if (from.arr || to.arr)
    {
        if (from.arr && to.arr && from.t == to.t) return e;
        return zeroLit(to);   // unwireable; only reachable via stale nodes
    }
    if (from.t == to.t) return e;
    switch (to.t)
    {
        case PT::Float:
            if (from.t == PT::Bool) return "((" + e + ") ? 1.0f : 0.0f)";
            if (from.t == PT::Int)  return "((float)(" + e + "))";
            break;
        case PT::Int:
            if (from.t == PT::Float) return "((int)(" + e + "))";
            if (from.t == PT::Bool)  return "((" + e + ") ? 1 : 0)";
            break;
        case PT::Bool:
            if (from.t == PT::Float) return "((" + e + ") != 0.0f)";
            if (from.t == PT::Int)   return "((" + e + ") != 0)";
            break;
        default: break;
    }
    return zeroLit(to);
}

// hc::coerce<T>/coerceArray<T> around a Value expression (dynamic coercion at
// the Value boundaries: event args, host reads, undeclared variables).
std::string coerceCall(const std::string& valueExpr, TypeRef to)
{
    if (to.arr) return "hc::coerceArray<" + std::string(cppScalar(to.t)) + ">(" + valueExpr + ")";
    return "hc::coerce<" + std::string(cppScalar(to.t)) + ">(" + valueExpr + ")";
}
std::string fromValueCall(const std::string& vecExpr, size_t k, TypeRef to)
{
    if (to.arr) return "hc::fromValueArray<" + std::string(cppScalar(to.t)) + ">(" + vecExpr + ", " + std::to_string(k) + ")";
    return "hc::fromValue<" + std::string(cppScalar(to.t)) + ">(" + vecExpr + ", " + std::to_string(k) + ")";
}

std::string sanitize(const std::string& s)
{
    std::string out;
    for (const char c : s)
        out += (std::isalnum((unsigned char)c) || c == '_') ? c : '_';
    if (out.empty()) out = "x";
    if (std::isdigit((unsigned char)out[0])) out = "_" + out;
    return out;
}

// ── the per-class emitter ─────────────────────────────────────────────────────
class ClassEmitter
{
public:
    ClassEmitter(const ClassSource& src, const std::string& className,
                 const Options& opt, std::vector<std::string>& warnings)
        : m_src(src), m_g(src.graph), m_cls(className), m_opt(opt), m_warnings(warnings) {}

    void run(std::string& header, std::string& impl)
    {
        validate();
        buildNames();
        buildSlots();
        emitHeader(header);
        emitImpl(impl);
    }

private:
    const ClassSource& m_src;
    Graph              m_g;      // private copy: EngineCall pins may be re-mirrored
    std::string        m_cls;
    const Options&     m_opt;
    std::vector<std::string>& m_warnings;

    std::unordered_map<std::string, std::string> m_varMember;  // instance var → v_*
    std::unordered_map<std::string, std::string> m_localName;  // local var → l_* (names graph-unique)
    std::unordered_map<std::string, std::string> m_fnName;     // function → f_*
    std::unordered_map<int, std::string>         m_evName;     // Event node id → ev_*
    struct Slot { std::string field; TypeRef tr; };
    std::unordered_map<int, std::vector<Slot>>   m_slots;      // exec-cached node → RunState fields

    void warn(const std::string& msg) { m_warnings.push_back(m_src.key + ": " + msg); }

    const Link* execLinkFrom(int nodeId, int pin) const
    {
        for (const auto& l : m_g.links)
            if (l.srcNode == nodeId && l.srcPin == pin) return &l;
        return nullptr;
    }
    const Link* dataLinkTo(int nodeId, int pin) const
    {
        for (const auto& l : m_g.links)
            if (l.dstNode == nodeId && l.dstPin == pin) return &l;
        return nullptr;
    }
    const Node* fnEntryByName(const std::string& name) const
    {
        for (const auto& n : m_g.nodes)
            if (n.type == NT::FunctionEntry && n.s == name) return &n;
        return nullptr;
    }

    // ── Stage A: validation (plan §5.2) ──────────────────────────────────────
    void validate()
    {
        // 1. EngineCall nodes must resolve in the registry; the registry is
        //    authoritative for the pin mirror (stale assets re-mirror + warn).
        for (Node& n : m_g.nodes)
        {
            if (n.type != NT::EngineCall) continue;
            const HE::api::ApiFn* fn = HE::api::find(n.s);
            if (!fn) throw FallbackError{ "unknown engine api '" + n.s + "'" };
            auto mirrors = [](const std::vector<HorizonCode::FuncParam>& have,
                              const std::vector<HE::api::ApiParam>& want)
            {
                if (have.size() != want.size()) return false;
                for (size_t i = 0; i < have.size(); ++i)
                    if (have[i].type != want[i].type || have[i].isArray != want[i].isArray)
                        return false;
                return true;
            };
            if (!mirrors(n.params, fn->params) || !mirrors(n.results, fn->results) ||
                n.hasArg != fn->isExec)
            {
                warn("engine call '" + n.s + "' re-mirrored from the registry (stale asset)");
                n.hasArg = fn->isExec;
                n.params.clear();
                for (const auto& p : fn->params)  n.params.push_back({ p.name, p.type, p.isArray });
                n.results.clear();
                for (const auto& r : fn->results) n.results.push_back({ r.name, r.type, r.isArray });
            }
        }

        // 2. Exec cycles would compile to unbounded loops (the interpreter only
        //    tolerates them via the step guard) → interpreted fallback.
        checkCycles(/*execEdges=*/true, "exec cycle at node ");
        // 3. Pure-data cycles likewise (the interpreter yields {} via the depth
        //    guard — not worth emulating).
        checkCycles(/*execEdges=*/false, "data cycle at node ");
    }

    void checkCycles(bool execEdges, const char* reasonPrefix)
    {
        // Adjacency over the relevant link subset. For data edges, evaluation
        // only recurses through nodes that re-evaluate their inputs per read —
        // exec-cached outputs (FunctionCall, ForEach, …) terminate recursion.
        auto cachedOut = [](const Node& n)
        {
            switch (n.type)
            {
                case NT::CreateWidget: case NT::CreateObject: case NT::FunctionCall:
                case NT::CallExternal: case NT::ForEach:
                    return true;
                case NT::EngineCall: return n.hasArg;
                default: return false;
            }
        };
        std::unordered_map<int, std::vector<int>> adj;
        for (const auto& l : m_g.links)
        {
            const Node* s = m_g.findNode(l.srcNode);
            const Node* d = m_g.findNode(l.dstNode);
            if (!s || !d) continue;
            const PinRanges sr = pinRanges(*s);
            const bool isExec = l.srcPin >= sr.execOut0 && l.srcPin < sr.dataIn0;
            if (execEdges != isExec) continue;
            if (execEdges) adj[l.srcNode].push_back(l.dstNode);
            else if (!cachedOut(*s)) adj[l.dstNode].push_back(l.srcNode); // reader depends on producer
        }
        std::unordered_map<int, int> state; // 0 unseen, 1 on stack, 2 done
        std::function<void(int)> dfs = [&](int id)
        {
            state[id] = 1;
            for (const int next : adj[id])
            {
                if (state[next] == 1) throw FallbackError{ reasonPrefix + std::to_string(next) };
                if (state[next] == 0) dfs(next);
            }
            state[id] = 2;
        };
        for (const auto& [id, _] : adj)
            if (state[id] == 0) dfs(id);
    }

    // ── name mangling (deterministic, collision-suffixed) ────────────────────
    void buildNames()
    {
        std::unordered_set<std::string> used;
        auto unique = [&used](std::string base)
        {
            std::string name = base;
            for (int i = 2; used.count(name); ++i) name = base + "_" + std::to_string(i);
            used.insert(name);
            return name;
        };
        for (const auto& v : m_g.variables)
        {
            if (v.scope == 0) m_varMember[v.name] = unique("v_" + sanitize(v.name));
            else              m_localName[v.name] = unique("l_" + sanitize(v.name));
        }
        for (const auto& n : m_g.nodes)
        {
            if (n.type == NT::FunctionEntry && !m_fnName.count(n.s))
                m_fnName[n.s] = unique("f_" + sanitize(n.s));
            if (n.type == NT::Event)
                m_evName[n.id] = unique("ev_" + sanitize(n.s) + "_" + std::to_string(n.id));
        }
    }

    // ── RunState slots: one field per exec-cached data-out (plan §5.4) ───────
    void buildSlots()
    {
        for (const Node& n : m_g.nodes)
        {
            std::vector<Slot> s;
            auto field = [&n](int k) { return "n" + std::to_string(n.id) + "_o" + std::to_string(k); };
            switch (n.type)
            {
                case NT::CreateWidget:
                case NT::CreateObject:
                    s.push_back({ field(0), { PT::Ref, false } });
                    break;
                case NT::FunctionCall:
                case NT::CallExternal:
                    for (size_t k = 0; k < n.results.size(); ++k)
                        s.push_back({ field((int)k), { n.results[k].type, n.results[k].isArray } });
                    break;
                case NT::EngineCall:
                    if (n.hasArg)
                        for (size_t k = 0; k < n.results.size(); ++k)
                            s.push_back({ field((int)k), { n.results[k].type, n.results[k].isArray } });
                    break;
                case NT::ForEach:
                    s.push_back({ field(0), { n.propType, false } });
                    s.push_back({ field(1), { PT::Int, false } });
                    break;
                default: break;
            }
            if (!s.empty()) m_slots[n.id] = std::move(s);
        }
    }

    // ── expressions (pull evaluation, re-emitted at every read — §3.3) ───────
    // fnCtx = the FunctionEntry id whose body we are emitting (0 = event graph).
    std::string input(const Node& n, int inIdx, int fnCtx)
    {
        const PinRanges r = pinRanges(n);
        const TypeRef want = dataInType(n, inIdx);
        if (const Link* l = dataLinkTo(n.id, r.dataIn0 + inIdx))
        {
            const Node* src = m_g.findNode(l->srcNode);
            if (src)
            {
                const int outIdx = l->srcPin - pinRanges(*src).dataOut0;
                // Wiring guarantees equal pin types (§3.3) → raw typed expression.
                return expr(*src, outIdx, fnCtx);
            }
        }
        // Unwired: the pin default constant-folded through coerce, else the zero.
        if (auto it = n.pinDefaults.find(inIdx); it != n.pinDefaults.end() && !want.arr)
            return valueLit(coerceValue(it->second, want.t));
        return zeroLit(want);
    }

    std::string slotRef(const Node& n, int outIdx) const
    {
        auto it = m_slots.find(n.id);
        if (it == m_slots.end() || outIdx < 0 || outIdx >= (int)it->second.size())
            return zeroLit(dataOutType(n, outIdx));   // §3.3: out-of-range cache read → zero
        return "rs." + it->second[outIdx].field;
    }

    std::string expr(const Node& n, int outIdx, int fnCtx)
    {
        const TypeRef out = dataOutType(n, outIdx);
        switch (n.type)
        {
        case NT::Event:
            // §3.3: Event data-out ← this run's arg coerced to propType.
            return coerceCall("rs.eventArg", { n.propType, false });
        case NT::FunctionEntry:
        {
            // §3.4: FunctionEntry param reads the INNERMOST frame — in C++ that
            // is the enclosing function's parameter. A read from outside the
            // owning function has no equivalent symbol → interpreted fallback.
            if (n.id != fnCtx)
                throw FallbackError{ "function parameter of '" + n.s + "' read outside its function" };
            return "p_" + std::to_string(outIdx);
        }
        case NT::ConstFloat:  return floatLit(n.f[0]);
        case NT::ConstBool:   return n.f[0] != 0.0f ? "true" : "false";
        case NT::ConstInt:    return std::to_string((int)n.f[0]);
        case NT::ConstString: return strLit(n.s);
        case NT::ConstVec2:   return "glm::vec2(" + floatLit(n.f[0]) + ", " + floatLit(n.f[1]) + ")";
        case NT::ConstColor:  return "glm::vec4(" + floatLit(n.f[0]) + ", " + floatLit(n.f[1]) + ", " +
                                      floatLit(n.f[2]) + ", " + floatLit(n.f[3]) + ")";
        case NT::ConstTransform:
        {
            Value v = Value::ofTransform(n.tpos, n.trot, n.tscl);
            return valueLit(v);
        }
        case NT::GetVariable:
        {
            const Variable* v = m_g.findVariable(n.s);
            if (v && v->scope != 0)
            {
                // §13.4: locals are stack locals of the owning function only.
                if (v->scope != fnCtx)
                    throw FallbackError{ "local '" + n.s + "' read outside its function" };
                return convertExpr(m_localName.at(n.s), { v->type, v->isArray },
                                   { n.propType, n.isArray });
            }
            if (v)
                return convertExpr(m_varMember.at(n.s), { v->type, v->isArray },
                                   { n.propType, n.isArray });
            // Undeclared: through the Context → the Runtime store (overflow for
            // compiled instances) so §3.4's dynamic-store semantics stay exact.
            return coerceCall("hc::getVariableCtx(m_ctx, " + strLit(n.s) + ")",
                              { n.propType, n.isArray });
        }
        case NT::GetProperty:
            return coerceCall("hc::getProperty(m_ctx, " + std::to_string(n.elem) + ", " + strLit(n.s) + ")",
                              { n.propType, false });
        case NT::GetExternal:
            return coerceCall("hc::getExternal(m_ctx, " + input(n, 0, fnCtx) + ", " + strLit(n.s) + ")",
                              { n.propType, false });
        // §3.4: Set pass-through re-evaluates the Value input (no store read).
        case NT::SetVariable:
        case NT::SetProperty:
            return input(n, 0, fnCtx);
        case NT::SetExternal:
            return input(n, 1, fnCtx);
        case NT::GetGameInstance: return "hc::gameInstance(m_ctx)";
        case NT::GetSelf:         return "hc::self(m_ctx)";
        // Exec-cached outputs → RunState fields (§5.4).
        case NT::CreateWidget:
        case NT::CreateObject:
        case NT::FunctionCall:
        case NT::CallExternal:
        case NT::ForEach:
            return slotRef(n, outIdx);
        case NT::EngineCall:
        {
            if (n.hasArg) return slotRef(n, outIdx);
            // Pure engine call: dispatched on EVERY data-out read (§3.4).
            std::string args = "std::vector<hc::Value>{";
            for (size_t i = 0; i < n.params.size(); ++i)
            {
                if (i) args += ", ";
                args += "hc::toValue(" + input(n, (int)i, fnCtx) + ")";
            }
            args += "}";
            return fromValueCall("hc::callApi(m_ctx, " + strLit(n.s) + ", " + args + ")",
                                 (size_t)outIdx, out);
        }
        case NT::ArrayMake:   return zeroLit(out);   // empty array of the element type
        case NT::ArrayLength: return "((int)(" + input(n, 0, fnCtx) + ").size())";
        case NT::ArrayGet:
            return "hc::arrGet(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        case NT::ArrayAdd:
            return "hc::arrAdd(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        case NT::ArraySet:
            return "hc::arrSet(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ", " +
                   input(n, 2, fnCtx) + ")";
        case NT::ArrayInsert:
            return "hc::arrInsert(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ", " +
                   input(n, 2, fnCtx) + ")";
        case NT::ArrayRemove:
            return "hc::arrRemove(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        case NT::ArrayContains:
            return "hc::arrContains(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        case NT::ArrayIndexOf:
            return "hc::arrIndexOf(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        case NT::Add:      return "(" + input(n, 0, fnCtx) + " + " + input(n, 1, fnCtx) + ")";
        case NT::Subtract: return "(" + input(n, 0, fnCtx) + " - " + input(n, 1, fnCtx) + ")";
        case NT::Multiply: return "(" + input(n, 0, fnCtx) + " * " + input(n, 1, fnCtx) + ")";
        case NT::Divide:
            // §3.4: the divisor evaluates FIRST; the dividend only when non-zero.
            return "([&]() -> float { const float b__ = " + input(n, 1, fnCtx) +
                   "; return b__ != 0.0f ? (" + input(n, 0, fnCtx) + ") / b__ : 0.0f; }())";
        case NT::Greater: return "((" + input(n, 0, fnCtx) + ") > (" + input(n, 1, fnCtx) + "))";
        case NT::Less:    return "((" + input(n, 0, fnCtx) + ") < (" + input(n, 1, fnCtx) + "))";
        case NT::Equals:  return "hc::feq(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        // §5.5: the interpreter evaluates both sides — no C++ short-circuit.
        case NT::And:     return "hc::land(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        case NT::Or:      return "hc::lor(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        case NT::Not:     return "(!(" + input(n, 0, fnCtx) + "))";
        case NT::Concat:  return "(" + input(n, 0, fnCtx) + " + " + input(n, 1, fnCtx) + ")";
        case NT::ToString:return "hc::toStringG(" + input(n, 0, fnCtx) + ")";
        default:
            return zeroLit(out);   // unknown data-out (§3.3: Value{} → zero)
        }
    }

    // ── statements (exec walk = runExecChain, §3.2) ──────────────────────────
    struct Body
    {
        std::string text;
        int indent = 1;
        void line(const std::string& s)
        {
            for (int i = 0; i < indent; ++i) text += "    ";
            text += s;
            text += "\n";
        }
    };

    void chain(const Node& from, int execOutPin, int fnCtx, Body& b)
    {
        const Link* l = execLinkFrom(from.id, execOutPin);
        while (l)
        {
            const Node* n = m_g.findNode(l->dstNode);
            if (!n) return;
            stmt(*n, fnCtx, b);
            // Branch/Sequence/ForEach steer their own exec-outs; Return is terminal.
            if (n->type == NT::Branch || n->type == NT::Sequence || n->type == NT::ForEach ||
                n->type == NT::FunctionReturn)
                return;
            l = execLinkFrom(n->id, pinRanges(*n).execOut0);
        }
    }

    void stmt(const Node& n, int fnCtx, Body& b)
    {
        const PinRanges r = pinRanges(n);
        b.line("HC_STEP(rs);");
        switch (n.type)
        {
        case NT::Branch:
            b.line("if (" + input(n, 0, fnCtx) + ")");
            b.line("{");
            ++b.indent; chain(n, r.execOut0 + 0, fnCtx, b); --b.indent;
            b.line("}");
            b.line("else");
            b.line("{");
            ++b.indent; chain(n, r.execOut0 + 1, fnCtx, b); --b.indent;
            b.line("}");
            break;
        case NT::Sequence:
            b.line("{");
            ++b.indent; chain(n, r.execOut0 + 0, fnCtx, b); --b.indent;
            b.line("}");
            b.line("{");
            ++b.indent; chain(n, r.execOut0 + 1, fnCtx, b); --b.indent;
            b.line("}");
            break;
        case NT::ForEach:
        {
            const std::string arr = "arr" + std::to_string(n.id);
            const std::string idx = "i" + std::to_string(n.id);
            b.line("{");
            ++b.indent;
            b.line("const " + cppType({ n.propType, true }) + " " + arr + " = " + input(n, 0, fnCtx) + ";");
            b.line("if (++rs.depth <= hc::kMaxDepth)");
            b.line("{");
            ++b.indent;
            b.line("for (size_t " + idx + " = 0; " + idx + " < " + arr + ".size(); ++" + idx + ")");
            b.line("{");
            ++b.indent;
            b.line(slotRef(n, 0) + " = " + arr + "[" + idx + "];");
            b.line(slotRef(n, 1) + " = (int)" + idx + ";");
            chain(n, r.execOut0 + 0, fnCtx, b);   // Body
            --b.indent;
            b.line("}");
            --b.indent;
            b.line("}");
            b.line("--rs.depth;");
            chain(n, r.execOut0 + 1, fnCtx, b);   // Done
            --b.indent;
            b.line("}");
            break;
        }
        case NT::SetVariable:
        {
            const Variable* v = m_g.findVariable(n.s);
            if (v && v->scope != 0)
            {
                if (v->scope != fnCtx)
                {
                    // §13.2: a local write outside its function is dropped.
                    warn("local '" + n.s + "' written outside its function — write dropped");
                    break;
                }
                b.line(m_localName.at(n.s) + " = " +
                       convertExpr(input(n, 0, fnCtx), { n.propType, n.isArray },
                                   { v->type, v->isArray }) + ";");
            }
            else if (v)
                b.line(m_varMember.at(n.s) + " = " +
                       convertExpr(input(n, 0, fnCtx), { n.propType, n.isArray },
                                   { v->type, v->isArray }) + ";");
            else
                // §3.4: Set on an undeclared name creates a store entry — routed
                // through the Context to the Runtime's (overflow) store.
                b.line("hc::setVariableCtx(m_ctx, " + strLit(n.s) + ", hc::toValue(" +
                       input(n, 0, fnCtx) + "));");
            break;
        }
        case NT::SetProperty:
            b.line("hc::setProperty(m_ctx, " + std::to_string(n.elem) + ", " + strLit(n.s) +
                   ", hc::toValue(" + input(n, 0, fnCtx) + "));");
            break;
        case NT::ShowWidget: b.line("hc::showSelf(m_ctx);"); break;
        case NT::HideWidget: b.line("hc::hideSelf(m_ctx);"); break;
        case NT::CreateWidget:
            b.line(slotRef(n, 0) + " = hc::createWidget(m_ctx, " + strLit(n.s) + ");");
            break;
        case NT::ShowWidgetId:
            b.line("hc::showWidget(m_ctx, (int)(" + input(n, 0, fnCtx) + "));");
            break;
        case NT::HideWidgetId:
            b.line("hc::hideWidget(m_ctx, (int)(" + input(n, 0, fnCtx) + "));");
            break;
        case NT::DestroyWidget:
            b.line("hc::destroyWidget(m_ctx, (int)(" + input(n, 0, fnCtx) + "));");
            break;
        case NT::CreateObject:
            b.line(slotRef(n, 0) + " = hc::createObject(m_ctx, " + strLit(n.s) + ");");
            break;
        case NT::DestroyObject:
            b.line("hc::destroyObject(m_ctx, " + input(n, 0, fnCtx) + ");");
            break;
        case NT::SetExternal:
            b.line("hc::setExternal(m_ctx, " + input(n, 0, fnCtx) + ", " + strLit(n.s) +
                   ", hc::toValue(" + input(n, 1, fnCtx) + "));");
            break;
        case NT::BindEvent:
            b.line("hc::bindEvent(m_ctx, " + input(n, 0, fnCtx) + ", " + strLit(n.s) + ");");
            break;
        case NT::EmitEvent:
            if (n.hasArg)
                b.line("hc::emitEvent(m_ctx, " + strLit(n.s) + ", hc::toValue(" +
                       input(n, 0, fnCtx) + "));");
            else
                b.line("hc::emitEvent(m_ctx, " + strLit(n.s) + ", hc::Value{});");
            break;
        case NT::CallExternal:
        {
            const std::string av = "a" + std::to_string(n.id);
            const std::string rv = "r" + std::to_string(n.id);
            b.line("{");
            ++b.indent;
            b.line("std::vector<hc::Value> " + av + ";");
            if (!n.params.empty()) b.line(av + ".reserve(" + std::to_string(n.params.size()) + ");");
            // §3.4: args (data-ins 1..) evaluate before the Target read.
            for (size_t i = 0; i < n.params.size(); ++i)
                b.line(av + ".push_back(hc::toValue(" + input(n, (int)i + 1, fnCtx) + "));");
            b.line("const std::vector<hc::Value> " + rv + " = hc::callExternal(m_ctx, " +
                   input(n, 0, fnCtx) + ", " + strLit(n.s) + ", " + av + ");");
            const auto it = m_slots.find(n.id);
            if (it != m_slots.end())
                for (size_t k = 0; k < it->second.size(); ++k)
                    b.line("rs." + it->second[k].field + " = " +
                           fromValueCall(rv, k, it->second[k].tr) + ";");
            --b.indent;
            b.line("}");
            break;
        }
        case NT::FunctionCall:
        {
            const Node* entry = fnEntryByName(n.s);
            if (!entry)
            {
                // §3.4: no local entry → silent no-op (nothing cached). Warned at
                // generation time so authors notice.
                warn("call to missing function '" + n.s + "' lowered to a no-op");
                break;
            }
            b.line("{");
            ++b.indent;
            // §3.4: args evaluate in the CALLER's frame, before the call.
            std::vector<std::string> argNames, resNames;
            for (size_t i = 0; i < entry->params.size(); ++i)
            {
                const std::string a = "a" + std::to_string(n.id) + "_" + std::to_string(i);
                const TypeRef tr = { entry->params[i].type, entry->params[i].isArray };
                // The call node mirrors the entry (synced) — pin type == param type.
                b.line("const " + cppType(tr) + " " + a + " = " +
                       (i < n.params.size() ? input(n, (int)i, fnCtx) : zeroLit(tr)) + ";");
                argNames.push_back(a);
            }
            for (size_t i = 0; i < entry->results.size(); ++i)
            {
                const std::string rn = "r" + std::to_string(n.id) + "_" + std::to_string(i);
                const TypeRef tr = { entry->results[i].type, entry->results[i].isArray };
                b.line(cppType(tr) + " " + rn + " = " + zeroLit(tr) + ";");   // typed default results
                resNames.push_back(rn);
            }
            std::string call = m_fnName.at(n.s) + "(rs";
            for (const auto& a : argNames) call += ", " + a;
            for (const auto& rn : resNames) call += ", " + rn;
            call += ");";
            b.line("if (++rs.depth <= hc::kMaxDepth) " + call + "   // depth guard (§3.6)");
            b.line("--rs.depth;");
            const auto it = m_slots.find(n.id);
            if (it != m_slots.end())
                for (size_t k = 0; k < it->second.size() && k < resNames.size(); ++k)
                    b.line("rs." + it->second[k].field + " = " + resNames[k] + ";");
            --b.indent;
            b.line("}");
            break;
        }
        case NT::FunctionReturn:
        {
            const Node* fn = fnCtx ? m_g.findNode(fnCtx) : nullptr;
            if (fn)
            {
                const size_t count = std::min(n.results.size(), fn->results.size());
                for (size_t i = 0; i < count; ++i)
                    b.line("r_" + std::to_string(i) + " = " +
                           convertExpr(input(n, (int)i, fnCtx),
                                       { n.results[i].type, n.results[i].isArray },
                                       { fn->results[i].type, fn->results[i].isArray }) + ";");
            }
            b.line("return;");
            break;
        }
        case NT::EngineCall:
        {
            const std::string av = "a" + std::to_string(n.id);
            const std::string rv = "r" + std::to_string(n.id);
            b.line("{");
            ++b.indent;
            b.line("std::vector<hc::Value> " + av + ";");
            if (!n.params.empty()) b.line(av + ".reserve(" + std::to_string(n.params.size()) + ");");
            for (size_t i = 0; i < n.params.size(); ++i)
                b.line(av + ".push_back(hc::toValue(" + input(n, (int)i, fnCtx) + "));");
            b.line("const std::vector<hc::Value> " + rv + " = hc::callApi(m_ctx, " +
                   strLit(n.s) + ", " + av + ");");
            const auto it = m_slots.find(n.id);
            if (it != m_slots.end())
                for (size_t k = 0; k < it->second.size(); ++k)
                    b.line("rs." + it->second[k].field + " = " +
                           fromValueCall(rv, k, it->second[k].tr) + ";");
            --b.indent;
            b.line("}");
            break;
        }
        case NT::Print:
            b.line("hc::print(" + input(n, 0, fnCtx) + ");");
            break;
        default:
            // Event/FunctionEntry never appear mid-chain; anything else is a
            // pure node that can't be exec-wired.
            break;
        }
    }

    // ── class emission ────────────────────────────────────────────────────────
    std::string memberDefault(const Variable& v) const
    {
        const Value d = HorizonCode::variableDefaultValue(v);
        if (v.isArray)
        {
            std::string init = cppType({ v.type, true }) + "{";
            for (size_t i = 0; i < d.items.size(); ++i)
            {
                if (i) init += ", ";
                Value item = d.items[i];
                item.type = v.type;
                init += valueLit(item);
            }
            return init + "}";
        }
        return valueLit(d);
    }

    // Events grouped by name in graph order (dispatch shape, §3.1).
    std::vector<std::pair<std::string, std::vector<const Node*>>> eventGroups() const
    {
        std::vector<std::pair<std::string, std::vector<const Node*>>> groups;
        for (const Node& n : m_g.nodes)
        {
            if (n.type != NT::Event) continue;
            auto it = std::find_if(groups.begin(), groups.end(),
                                   [&](const auto& g) { return g.first == n.s; });
            if (it == groups.end()) { groups.push_back({ n.s, { &n } }); }
            else it->second.push_back(&n);
        }
        return groups;
    }

    // Functions deduplicated by name, first entry wins (§3.1 callFunction).
    std::vector<const Node*> functionEntries() const
    {
        std::vector<const Node*> fns;
        std::unordered_set<std::string> seen;
        for (const Node& n : m_g.nodes)
            if (n.type == NT::FunctionEntry && seen.insert(n.s).second)
                fns.push_back(&n);
        return fns;
    }

    std::string fnSignature(const Node& fn, bool withClass) const
    {
        std::string sig = "void " + (withClass ? m_cls + "::" : std::string()) +
                          m_fnName.at(fn.s) + "(RunState& rs";
        for (size_t i = 0; i < fn.params.size(); ++i)
            sig += ", " + cppType({ fn.params[i].type, fn.params[i].isArray }) +
                   " p_" + std::to_string(i);
        for (size_t i = 0; i < fn.results.size(); ++i)
            sig += ", " + cppType({ fn.results[i].type, fn.results[i].isArray }) +
                   "& r_" + std::to_string(i);
        return sig + ")";
    }

    void emitHeader(std::string& h)
    {
        h += "// GENERATED by HorizonCode → C++ codegen — do not edit.\n";
        h += "// Source: " + m_src.label + " (key " + m_src.key + ")\n";
        h += "#pragma once\n";
        h += "#include <HorizonCode/HorizonCodeCompiled.h>\n";
        h += "#include <HorizonCode/HorizonCodeGenSupport.h>\n\n";
        h += "namespace " + m_opt.namespaceName + " {\n\n";
        h += "class " + m_cls + " final : public HorizonCode::CompiledInstance\n{\npublic:\n";
        h += "    const char* classKey() const override;\n";
        h += "    const std::vector<HorizonCode::CompiledVarInfo>&   varInfos()   const override;\n";
        h += "    const std::vector<HorizonCode::CompiledEventInfo>& eventInfos() const override;\n";
        h += "    void fireEvent(const std::string& name, int elem, const hc::Value& arg) override;\n";
        h += "    bool callFunction(const std::string& name, bool requirePublic,\n";
        h += "                      const std::vector<hc::Value>& args,\n";
        h += "                      std::vector<hc::Value>* results) override;\n";
        h += "    hc::Value getVariable(const std::string& name) const override;\n";
        h += "    bool setVariable(const std::string& name, const hc::Value& v) override;\n";
        h += "    void reseedVariables() override;\n";
        h += "    void collectRefs(std::vector<uint32_t>& out) const override;\n\n";
        h += "private:\n";

        // Instance variables → typed members at their declared defaults.
        for (const auto& v : m_g.variables)
            if (v.scope == 0)
                h += "    " + cppType({ v.type, v.isArray }) + " " + m_varMember.at(v.name) +
                     " = " + memberDefault(v) + ";\n";

        // Per-run state: mirrors the Runner exactly (§3.3/§5.4).
        h += "\n    struct RunState\n    {\n";
        h += "        int steps = 0, depth = 0;\n";
        h += "        bool aborted = false;\n";
        h += "        hc::Value eventArg;\n";
        std::vector<int> slotIds;
        for (const auto& [id, _] : m_slots) slotIds.push_back(id);
        std::sort(slotIds.begin(), slotIds.end());
        for (const int id : slotIds)
            for (const auto& s : m_slots.at(id))
                h += "        " + cppType(s.tr) + " " + s.field + " = " + zeroLit(s.tr) + ";\n";
        h += "    };\n\n";

        for (const auto& [id, name] : sortedEvNames())
            h += "    void " + name + "(RunState& rs);\n";
        for (const Node* fn : functionEntries())
            h += "    " + fnSignature(*fn, false) + ";\n";
        h += "};\n\n";
        h += "} // namespace " + m_opt.namespaceName + "\n";
    }

    std::vector<std::pair<int, std::string>> sortedEvNames() const
    {
        std::vector<std::pair<int, std::string>> evs(m_evName.begin(), m_evName.end());
        std::sort(evs.begin(), evs.end());
        return evs;
    }

    void emitImpl(std::string& c)
    {
        const std::string ns = m_opt.namespaceName;
        c += "// GENERATED by HorizonCode → C++ codegen — do not edit.\n";
        c += "#include \"hcgen_" + m_cls + ".h\"\n\n";
        c += "namespace " + ns + " {\n\n";

        c += "const char* " + m_cls + "::classKey() const { return " + strLit(m_src.key) + "; }\n\n";

        // varInfos: scope-0 declared variables only (locals are invisible, §13.1).
        c += "const std::vector<HorizonCode::CompiledVarInfo>& " + m_cls + "::varInfos() const\n{\n";
        c += "    static const std::vector<HorizonCode::CompiledVarInfo> kVars = {\n";
        for (const auto& v : m_g.variables)
            if (v.scope == 0)
                c += "        { " + strLit(v.name) + ", hc::PinType::" + pinName(v.type) + ", " +
                     (v.isArray ? "true" : "false") + ", " + std::to_string(v.access) + " },\n";
        c += "    };\n    return kVars;\n}\n\n";

        c += "const std::vector<HorizonCode::CompiledEventInfo>& " + m_cls + "::eventInfos() const\n{\n";
        c += "    static const std::vector<HorizonCode::CompiledEventInfo> kEvents = {\n";
        for (const Node& n : m_g.nodes)
            if (n.type == NT::Event)
                c += "        { " + strLit(n.s) + ", " + std::to_string(n.elem) + " },\n";
        c += "    };\n    return kEvents;\n}\n\n";

        // ── fireEvent: fresh RunState per fire == the per-run cache clear (§3.1);
        //    every matching handler of one fire shares it, in graph order.
        c += "void " + m_cls + "::fireEvent(const std::string& name, int elem, const hc::Value& arg)\n{\n";
        c += "    (void)name; (void)elem;\n";
        c += "    RunState rs;\n    rs.eventArg = arg;\n";
        bool first = true;
        for (const auto& [name, nodes] : eventGroups())
        {
            c += std::string("    ") + (first ? "if" : "else if") + " (name == " + strLit(name) + ")\n    {\n";
            for (const Node* n : nodes)
            {
                if (n->elem == 0)
                    c += "        " + m_evName.at(n->id) + "(rs);\n";
                else
                    c += "        if (elem == " + std::to_string(n->elem) + ") " +
                         m_evName.at(n->id) + "(rs);\n";
            }
            c += "    }\n";
            first = false;
        }
        c += "}\n\n";

        // ── callFunction: coerced args, typed default results (§3.1).
        c += "bool " + m_cls + "::callFunction(const std::string& name, bool requirePublic,\n";
        c += "                    const std::vector<hc::Value>& args, std::vector<hc::Value>* results)\n{\n";
        c += "    (void)requirePublic; (void)args;\n";
        for (const Node* fn : functionEntries())
        {
            c += "    if (name == " + strLit(fn->s) + ")\n    {\n";
            if (fn->access != 0)
                c += "        if (requirePublic) return false;   // private function\n";
            c += "        RunState rs;\n";
            for (size_t i = 0; i < fn->params.size(); ++i)
            {
                const TypeRef tr = { fn->params[i].type, fn->params[i].isArray };
                c += "        " + cppType(tr) + " p" + std::to_string(i) + " = " +
                     coerceCall("hc::arg(args, " + std::to_string(i) + ")", tr) + ";\n";
            }
            for (size_t i = 0; i < fn->results.size(); ++i)
            {
                const TypeRef tr = { fn->results[i].type, fn->results[i].isArray };
                c += "        " + cppType(tr) + " r" + std::to_string(i) + " = " + zeroLit(tr) + ";\n";
            }
            std::string call = "        " + m_fnName.at(fn->s) + "(rs";
            for (size_t i = 0; i < fn->params.size(); ++i)  call += ", p" + std::to_string(i);
            for (size_t i = 0; i < fn->results.size(); ++i) call += ", r" + std::to_string(i);
            c += call + ");\n";
            c += "        if (results)\n        {\n            results->clear();\n";
            for (size_t i = 0; i < fn->results.size(); ++i)
                c += "            results->push_back(hc::toValue(r" + std::to_string(i) + "));\n";
            c += "        }\n        return true;\n    }\n";
        }
        c += "    return false;\n}\n\n";

        // ── variable reflection (declared instance vars only).
        c += "hc::Value " + m_cls + "::getVariable(const std::string& name) const\n{\n";
        for (const auto& v : m_g.variables)
            if (v.scope == 0)
                c += "    if (name == " + strLit(v.name) + ") return hc::toValue(" +
                     m_varMember.at(v.name) + ");\n";
        c += "    return hc::Value{};\n}\n\n";

        c += "bool " + m_cls + "::setVariable(const std::string& name, const hc::Value& v)\n{\n";
        c += "    (void)v;\n";
        for (const auto& v : m_g.variables)
            if (v.scope == 0)
                c += "    if (name == " + strLit(v.name) + ") { " + m_varMember.at(v.name) +
                     " = " + coerceCall("v", { v.type, v.isArray }) + "; return true; }\n";
        c += "    return false;\n}\n\n";

        c += "void " + m_cls + "::reseedVariables()\n{\n";
        for (const auto& v : m_g.variables)
            if (v.scope == 0)
                c += "    " + m_varMember.at(v.name) + " = " + memberDefault(v) + ";\n";
        c += "}\n\n";

        c += "void " + m_cls + "::collectRefs(std::vector<uint32_t>& out) const\n{\n";
        c += "    (void)out;\n";
        for (const auto& v : m_g.variables)
        {
            if (v.scope != 0 || v.type != PT::Ref) continue;
            if (v.isArray)
                c += "    for (const uint32_t r__ : " + m_varMember.at(v.name) +
                     ") if (r__ != 0u) out.push_back(r__);\n";
            else
                c += "    if (" + m_varMember.at(v.name) + " != 0u) out.push_back(" +
                     m_varMember.at(v.name) + ");\n";
        }
        c += "}\n\n";

        // ── event handler bodies.
        for (const Node& n : m_g.nodes)
        {
            if (n.type != NT::Event) continue;
            c += "void " + m_cls + "::" + m_evName.at(n.id) + "(RunState& rs)\n{\n";
            c += "    (void)rs;\n";
            Body b;
            chain(n, pinRanges(n).execOut0, /*fnCtx=*/0, b);
            c += b.text;
            c += "}\n\n";
        }

        // ── function bodies (locals = stack locals, §13.4).
        for (const Node* fn : functionEntries())
        {
            c += fnSignature(*fn, true) + "\n{\n";
            c += "    (void)rs;\n";
            for (const auto& v : m_g.variables)
                if (v.scope == fn->id)
                    c += "    " + cppType({ v.type, v.isArray }) + " " + m_localName.at(v.name) +
                         " = " + memberDefault(v) + ";   // local, per invocation\n";
            Body b;
            chain(*fn, pinRanges(*fn).execOut0, /*fnCtx=*/fn->id, b);
            c += b.text;
            c += "}\n\n";
        }

        c += "} // namespace " + ns + "\n";
    }

    static const char* pinName(PinType t)
    {
        switch (t)
        {
            case PT::Float:     return "Float";
            case PT::Bool:      return "Bool";
            case PT::Int:       return "Int";
            case PT::String:    return "String";
            case PT::Vec2:      return "Vec2";
            case PT::Color:     return "Color";
            case PT::Ref:       return "Ref";
            case PT::Transform: return "Transform";
            default:            return "Float";
        }
    }
};

std::string classNameFor(const std::string& key, std::unordered_set<std::string>& used)
{
    std::string stem = key;
    if (stem == "__game_instance__") stem = "GameInstance";
    else
    {
        std::string prefix;
        if (stem.rfind("level:", 0) == 0) { prefix = "Level_"; stem = stem.substr(6); }
        if (const size_t slash = stem.find_last_of('/'); slash != std::string::npos)
            stem = stem.substr(slash + 1);
        if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos && dot > 0)
            stem = stem.substr(0, dot);
        stem = prefix + stem;
    }
    std::string base = "C_" + sanitize(stem);
    std::string name = base;
    for (int i = 2; used.count(name); ++i) name = base + "_" + std::to_string(i);
    used.insert(name);
    return name;
}

} // namespace

Result generate(const std::vector<ClassSource>& sources, const Options& opt)
{
    Result res;
    res.ok = true;
    std::unordered_set<std::string> usedNames;

    struct Entry { std::string key, className; };
    std::vector<Entry> compiled;

    for (const ClassSource& src : sources)
    {
        const std::string className = classNameFor(src.key, usedNames);
        try
        {
            ClassEmitter em(src, className, opt, res.warnings);
            std::string header, impl;
            em.run(header, impl);
            res.files.push_back({ "hcgen_" + className + ".h", std::move(header) });
            res.files.push_back({ "hcgen_" + className + ".cpp", std::move(impl) });
            compiled.push_back({ src.key, className });
        }
        catch (const FallbackError& e)
        {
            usedNames.erase(className);   // name freed — the class ships interpreted
            res.fallbacks.push_back({ src.key, e.reason });
        }
    }

    // ── hc_registry.h/.cpp: the manifest (plan §5.6/§9) ──────────────────────
    const std::string ns = opt.namespaceName;
    std::string rh;
    rh += "// GENERATED by HorizonCode → C++ codegen — do not edit.\n";
    rh += "#pragma once\n#include <HorizonCode/HorizonCodeCompiled.h>\n\n";
    rh += "namespace " + ns + " {\n";
    rh += "// The compiled-class manifest: key → factory. nullptr/0 when empty.\n";
    rh += "const HorizonCode::CompiledClassEntry* classes(int* count);\n";
    rh += "const char* engineVersion();\n";
    rh += "} // namespace " + ns + "\n";
    res.files.push_back({ "hc_registry.h", std::move(rh) });

    std::string rc;
    rc += "// GENERATED by HorizonCode → C++ codegen — do not edit.\n";
    rc += "#include \"hc_registry.h\"\n";
    for (const auto& e : compiled)
        rc += "#include \"hcgen_" + e.className + ".h\"\n";
    rc += "\nnamespace " + ns + " {\n\n";
    if (!compiled.empty())
    {
        rc += "static const HorizonCode::CompiledClassEntry kClasses[] = {\n";
        for (const auto& e : compiled)
        {
            rc += "    { " + strLit(e.key) + ",\n";
            rc += "      +[]() -> HorizonCode::CompiledInstance* { return new " + ns + "::" + e.className + "(); },\n";
            rc += "      +[](HorizonCode::CompiledInstance* p) { delete p; } },\n";
        }
        rc += "};\n\n";
        rc += "const HorizonCode::CompiledClassEntry* classes(int* count)\n";
        rc += "{ if (count) *count = " + std::to_string(compiled.size()) + "; return kClasses; }\n";
    }
    else
    {
        rc += "const HorizonCode::CompiledClassEntry* classes(int* count)\n";
        rc += "{ if (count) *count = 0; return nullptr; }\n";
    }
    rc += "const char* engineVersion() { return " + strLit(opt.engineVersion) + "; }\n\n";
    rc += "} // namespace " + ns + "\n\n";
    rc += "// The C-ABI manifest export, only when building the shipped dylib.\n";
    rc += "#if defined(HCGEN_BUILD_DYLIB)\n";
    rc += "extern \"C\"\n";
    rc += "#if defined(_WIN32)\n__declspec(dllexport)\n#endif\n";
    rc += "const HorizonCode::CompiledClassEntry* HE_HorizonCodeGenClasses(int* count, const char** engineVersion)\n";
    rc += "{\n    if (engineVersion) *engineVersion = " + ns + "::engineVersion();\n";
    rc += "    return " + ns + "::classes(count);\n}\n";
    rc += "#endif\n";
    res.files.push_back({ "hc_registry.cpp", std::move(rc) });

    return res;
}

std::string generateCMakeLists(const Options& opt, const std::vector<std::string>& cppFiles)
{
    std::string s;
    s += "# GENERATED by HorizonCode → C++ codegen — do not edit.\n";
    s += "cmake_minimum_required(VERSION 3.20)\n";
    s += "project(HorizonCodeGen CXX)\n";
    s += "set(CMAKE_CXX_STANDARD 20)\n";
    s += "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
    s += "# SDK layout: either a staged HE_SDK_DIR (include/ + lib/) or explicit\n";
    s += "# dirs (development builds pass the source tree + build dir directly).\n";
    s += "if(NOT DEFINED HE_SDK_INCLUDE_DIRS)\n";
    s += "    set(HE_SDK_INCLUDE_DIRS \"${HE_SDK_DIR}/include\")\n";
    s += "endif()\n";
    s += "if(NOT DEFINED HE_SDK_LIB_DIR)\n";
    s += "    set(HE_SDK_LIB_DIR \"${HE_SDK_DIR}/lib\")\n";
    s += "endif()\n";
    s += "add_library(HorizonCodeGen SHARED\n";
    for (const auto& f : cppFiles) s += "    " + f + "\n";
    s += ")\n";
    s += "target_compile_definitions(HorizonCodeGen PRIVATE HCGEN_BUILD_DYLIB)\n";
    s += "target_include_directories(HorizonCodeGen PRIVATE ${HE_SDK_INCLUDE_DIRS})\n";
    s += "target_link_directories(HorizonCodeGen PRIVATE \"${HE_SDK_LIB_DIR}\")\n";
    s += "target_link_libraries(HorizonCodeGen PRIVATE HorizonCore)\n";
    s += "if(APPLE)\n";
    s += "    set_target_properties(HorizonCodeGen PROPERTIES INSTALL_RPATH \"@loader_path\" BUILD_WITH_INSTALL_RPATH ON)\n";
    s += "endif()\n";
    (void)opt;
    return s;
}

// ── toolchain integration ─────────────────────────────────────────────────────

SdkInfo resolveSdk(const std::filesystem::path& editorBaseDir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    // 1. Explicit override for CI / unusual layouts.
    if (const char* env = std::getenv("HE_HCGEN_SDK"); env && *env)
    {
        const fs::path root(env);
        if (fs::is_directory(root / "include", ec))
            return { { root / "include" }, root / "lib" };
    }
    // 2. A staged SDK beside the deployed editor.
    if (const fs::path staged = editorBaseDir / "SDK"; fs::is_directory(staged / "include", ec))
        return { { staged / "include" }, staged / "lib" };
    // 3. Development build: the config CMake wrote beside the editor binary,
    //    pointing straight into the source tree + build dir.
    std::ifstream cfg(editorBaseDir / "he_sdk_config.json", std::ios::binary);
    if (cfg)
    {
        // Tiny hand-rolled parse would be fragile — this is engine-authored JSON.
        std::stringstream ss;
        ss << cfg.rdbuf();
        const auto j = nlohmann::json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
        if (j.is_object())
        {
            SdkInfo info;
            for (const auto& d : j.value("includeDirs", nlohmann::json::array()))
                if (d.is_string()) info.includeDirs.push_back(fs::path(d.get<std::string>()));
            info.libDir = fs::path(j.value("libDir", std::string()));
            if (info.valid()) return info;
        }
    }
    return {};
}

namespace {
// Quote a path for the shell command line (v1: std::system on a worker thread).
std::string shq(const std::filesystem::path& p)
{
#if defined(_WIN32)
    return "\"" + p.string() + "\"";
#else
    std::string s = p.string(), out = "'";
    for (const char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    return out + "'";
#endif
}
} // namespace

bool toolchainAvailable()
{
#if defined(_WIN32)
    return std::system("cmake --version >NUL 2>&1") == 0;
#else
    return std::system("cmake --version >/dev/null 2>&1") == 0;
#endif
}

BuildOutcome buildDylib(const std::filesystem::path& genDir, const SdkInfo& sdk)
{
    namespace fs = std::filesystem;
    BuildOutcome out;
    out.logFile = genDir / "build.log";
    if (!sdk.valid())
    {
        out.message = "no codegen SDK found (HE_HCGEN_SDK, <editor>/SDK, he_sdk_config.json)";
        return out;
    }
    if (!toolchainAvailable())
    {
        out.message = "cmake not found on PATH";
        return out;
    }

    std::string includes;
    for (size_t i = 0; i < sdk.includeDirs.size(); ++i)
    {
        if (i) includes += ";";
        includes += sdk.includeDirs[i].string();
    }
    const fs::path buildDir = genDir / "build";
    const std::string log = shq(out.logFile);
    const std::string configure =
        "cmake -S " + shq(genDir) + " -B " + shq(buildDir) +
        " -DCMAKE_BUILD_TYPE=Release"
        " \"-DHE_SDK_INCLUDE_DIRS=" + includes + "\""
        " -DHE_SDK_LIB_DIR=" + shq(sdk.libDir) +
        " > " + log + " 2>&1";
    if (std::system(configure.c_str()) != 0)
    {
        out.message = "cmake configure failed (see " + out.logFile.string() + ")";
        return out;
    }
    const std::string build =
        "cmake --build " + shq(buildDir) + " --config Release >> " + log + " 2>&1";
    if (std::system(build.c_str()) != 0)
    {
        out.message = "compile failed (see " + out.logFile.string() + ")";
        return out;
    }

    // Locate the artifact (single-config generators put it flat; MSVC under
    // Release/).
    const char* names[] = { "libHorizonCodeGen.dylib", "libHorizonCodeGen.so", "HorizonCodeGen.dll" };
    for (const char* n : names)
        for (const fs::path dir : { buildDir, buildDir / "Release" })
        {
            std::error_code ec;
            if (fs::exists(dir / n, ec)) { out.artifact = dir / n; out.ok = true; return out; }
        }
    out.message = "build succeeded but no HorizonCodeGen library was produced";
    return out;
}

} // namespace HE::hccg
