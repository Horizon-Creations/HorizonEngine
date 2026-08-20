#include "HorizonScene/HcCodegen.h"
#include <HorizonCode/HorizonCodeGenSupport.h>   // hc::kMaxSteps — one definition of the limit
#include <Types/TypeRegistry.h>   // enum defs at generation time
#include <cstdint>
#include "HorizonScene/EngineApi.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#if defined(_WIN32)
#include <process.h>  // _getpid — avoids pulling in windows.h just for a pid
#else
#include <unistd.h>   // getpid
#endif

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

// A class that can't compile bails out to the interpreter with a reason and,
// when known, the graph node it anchors to (editor error highlighting).
struct FallbackError { std::string reason; int node = 0; };

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

// A pin's static type. `typeName` is the Enum/Struct definition asset (the
// TypeRegistry key); `cpp` is the generated C++ struct name for Struct pins,
// resolved from the run's TypeTable when the TypeRef is built.
struct TypeRef
{
    PinType     t   = PT::Float;
    bool        arr = false;      // "is a container" — see ContainerKind
    std::string typeName;
    std::string cpp;
    // Which container, and — for a Map, whose `t`/`typeName`/`cpp` describe the
    // VALUE — the key. Appended last with defaults so the many brace-initialized
    // TypeRefs above and below stay as they were.
    HorizonCode::ContainerKind ctr = HorizonCode::ContainerKind::None;
    PinType     keyT = PT::String;
    std::string keyTypeName;
    std::string keyCpp;

    HorizonCode::ContainerKind kind() const
    { return HorizonCode::containerKindOf(arr, ctr); }
    // The key half of a map, as a TypeRef of its own — what every key pin, key
    // literal and key conversion below is built from.
    TypeRef key() const { return TypeRef{ keyT, false, keyTypeName, keyCpp }; }
    // The element/value half: the same type with the container stripped off.
    TypeRef elem() const { return TypeRef{ t, false, typeName, cpp }; }
};

// ── user-defined struct types (plan §6, extended) ────────────────────────────
// Every struct definition the compiled classes touch becomes a REAL C++ struct
// in its own hcgen_type_<Name>.h — no Value plumbing survives into the
// generated bodies, so a struct field read is a member access. Enums need no
// entry here: they stay int-backed (the engine treats them as ints everywhere),
// only their Value boundaries carry the definition path back (hc::toEnumValue).
struct StructType
{
    std::string              cpp;       // generated C++ name ("S_PlayerStats")
    HE::StructDef            def;       // the definition as of GENERATION time
    std::vector<std::string> members;   // C++ member name per field, in def order
};
// An Enum asset becomes a real `enum class E : int` in its own header — the
// underlying int is still what every Value carries, so nothing about the
// engine's int-backed enums changes; the generated code just says which enum it
// means, and so can anyone including the header.
struct EnumType
{
    std::string              cpp;       // generated C++ name ("E_Mood")
    HE::EnumDef              def;
    std::vector<std::string> entries;   // C++ enumerator per entry, in def order
};
struct TypeTable
{
    std::unordered_map<std::string, StructType> byPath;
    std::vector<std::string> order;   // emission order: a struct after everything it embeds
    std::unordered_map<std::string, EnumType> enumsByPath;
    std::vector<std::string> enumOrder;

    const StructType* find(const std::string& assetPath) const
    {
        const auto it = byPath.find(assetPath);
        return it != byPath.end() ? &it->second : nullptr;
    }
    const EnumType* findEnum(const std::string& assetPath) const
    {
        const auto it = enumsByPath.find(assetPath);
        return it != enumsByPath.end() ? &it->second : nullptr;
    }
    // The C++ name for either kind — a pin knows which it is from its PinType.
    std::string cppOf(const std::string& assetPath) const
    {
        if (const StructType* s = find(assetPath))   return s->cpp;
        if (const EnumType* e = findEnum(assetPath)) return e->cpp;
        return {};
    }
    // The enumerator for a value, or an empty string when no entry claims it.
    std::string enumeratorOf(const std::string& assetPath, int value) const
    {
        const EnumType* e = findEnum(assetPath);
        if (!e) return {};
        for (size_t i = 0; i < e->def.entries.size(); ++i)
            if (e->def.entries[i].value == value) return e->cpp + "::" + e->entries[i];
        return {};
    }
};

// ── C++ type/literal tables (plan §6) ────────────────────────────────────────
std::string cppScalar(const TypeRef& tr)
{
    switch (tr.t)
    {
        case PT::Float:     return "float";
        case PT::Bool:      return "bool";
        case PT::Int:       return "int";
        case PT::String:    return "std::string";
        case PT::Vec2:      return "glm::vec2";
        case PT::Color:     return "glm::vec4";
        case PT::Vec3:      return "glm::vec3";
        // hc::Vec4, not glm::vec4 — Color already owns that C++ type and the
        // support header's type-keyed templates have to tell the two apart.
        case PT::Vec4:      return "hc::Vec4";
        case PT::Ref:       return "uint32_t";
        case PT::Transform: return "hc::Transform";
        // Int-backed like everywhere else in the engine, but named: `enum class
        // E_Mood : int`, so the generated code (and anyone including the
        // header) says WHICH enum. Unresolved definitions never reach emission.
        case PT::Enum:      return tr.cpp.empty() ? "int" : tr.cpp;
        // validate() refuses a Struct pin whose definition the table doesn't
        // carry, so `cpp` is set on every Struct TypeRef that reaches emission.
        case PT::Struct:    return tr.cpp.empty() ? "float" : tr.cpp;
        default:            return "float";
    }
}
std::string cppType(const TypeRef& tr)
{
    switch (tr.kind())
    {
        case HorizonCode::ContainerKind::Array: return "hc::Array<" + cppScalar(tr) + ">";
        case HorizonCode::ContainerKind::Set:   return "hc::Set<" + cppScalar(tr) + ">";
        case HorizonCode::ContainerKind::Map:
            return "hc::Map<" + cppScalar(tr.key()) + ", " + cppScalar(tr) + ">";
        default: return cppScalar(tr);
    }
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
// A struct's zero is the all-fields-zero aggregate, which is exactly what the
// generated struct's own member initializers give — matching the interpreter,
// where an unwired Struct pin is an EMPTY Value whose field reads all miss.
std::string zeroLit(const TypeRef& tr)
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
        case PT::Vec3:      return "glm::vec3(0.0f)";
        case PT::Vec4:      return "hc::Vec4{}";
        case PT::Ref:       return "0u";
        case PT::Transform: return "hc::Transform{}";
        case PT::Enum:      return cppScalar(tr) + "{}";   // = 0, like a fresh Value
        case PT::Struct:    return cppScalar(tr) + "{}";
        default:            return "0.0f";
    }
}

// A Value as a C++ literal of the given static type. `want` matters for the
// user-defined types: a Struct literal needs the definition (field order and
// element types), which the Value alone doesn't pin down.
std::string valueLit(const Value& v, const TypeTable& tt, const TypeRef& want);

// `S_X{ f0, f1, … }` — fields in DEFINITION order, each from the matching item
// of `v` (a short/absent item is the field's own zero, like a missing item read).
std::string structLit(const Value& v, const TypeTable& tt, const TypeRef& want)
{
    const StructType* st = tt.find(want.typeName);
    if (!st) return zeroLit(want);
    std::string out = st->cpp + "{";
    for (size_t i = 0; i < st->def.fields.size(); ++i)
    {
        const HE::StructField& f = st->def.fields[i];
        const TypeRef ft{ f.type, f.isArray, f.typeName, tt.cppOf(f.typeName),
                          f.container, f.keyType, f.keyTypeName, tt.cppOf(f.keyTypeName) };
        out += (i ? ", " : " ");
        out += i < v.items.size() ? valueLit(v.items[i], tt, ft) : zeroLit(ft);
    }
    return out + (st->def.fields.empty() ? "}" : " }");
}

std::string valueLit(const Value& v, const TypeTable& tt, const TypeRef& want)
{
    if (want.arr)
    {
        // One list of scalar literals of `as` — the shape every container
        // literal is built from (the items/keys are scalars of their type).
        auto listOf = [&](const std::vector<Value>& src, const TypeRef& as)
        {
            std::string out = "hc::Array<" + cppScalar(as) + ">{";
            for (size_t i = 0; i < src.size(); ++i)
            {
                Value item = src[i];
                item.isArray = false;
                item.container = HorizonCode::ContainerKind::None;
                if (item.type != as.t) item.type = as.t;   // seeded slots carry the element type
                out += (i ? ", " : " ");
                out += valueLit(item, tt, as);
            }
            return out + (src.empty() ? "}" : " }");
        };
        switch (want.kind())
        {
            case HorizonCode::ContainerKind::Set:
                // Aggregate init: hc::Set's single member IS the item list.
                return cppType(want) + "{ " + listOf(v.items, want.elem()) + " }";
            case HorizonCode::ContainerKind::Map:
                // Keys first, values second — the member order of hc::Map, and
                // index-parallel exactly like the Value it came from.
                return cppType(want) + "{ " + listOf(v.keys, want.key()) + ", " +
                       listOf(v.items, want.elem()) + " }";
            default:
                return listOf(v.items, want.elem());
        }
    }
    switch (want.t)
    {
        case PT::Float:  return floatLit(v.f);
        case PT::Bool:   return v.b ? "true" : "false";
        case PT::Int:    return std::to_string(v.i);
        case PT::String: return strLit(v.s);
        case PT::Vec2:   return "glm::vec2(" + floatLit(v.v2.x) + ", " + floatLit(v.v2.y) + ")";
        case PT::Color:  return "glm::vec4(" + floatLit(v.col.x) + ", " + floatLit(v.col.y) + ", " +
                                 floatLit(v.col.z) + ", " + floatLit(v.col.w) + ")";
        case PT::Vec3:   return "glm::vec3(" + floatLit(v.v3.x) + ", " + floatLit(v.v3.y) + ", " +
                                 floatLit(v.v3.z) + ")";
        case PT::Vec4:   return "hc::Vec4(" + floatLit(v.v4.x) + ", " + floatLit(v.v4.y) + ", " +
                                 floatLit(v.v4.z) + ", " + floatLit(v.v4.w) + ")";
        case PT::Ref:    return std::to_string(v.ref) + "u";
        case PT::Enum:
        {
            // The enumerator when one claims the value; a cast otherwise (a
            // stale stored value is still a legal enum object).
            const std::string e = tt.enumeratorOf(want.typeName, v.i);
            return !e.empty() ? e : "(" + cppScalar(want) + ")" + std::to_string(v.i);
        }
        case PT::Transform:
            return "hc::Transform{ glm::vec3(" + floatLit(v.tpos.x) + ", " + floatLit(v.tpos.y) + ", " + floatLit(v.tpos.z) +
                   "), glm::vec3(" + floatLit(v.trot.x) + ", " + floatLit(v.trot.y) + ", " + floatLit(v.trot.z) +
                   "), glm::vec3(" + floatLit(v.tscl.x) + ", " + floatLit(v.tscl.y) + ", " + floatLit(v.tscl.z) + ") }";
        case PT::Struct: return structLit(v, tt, want);
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
                            : v.type == PT::Int ? (float)v.i
                            : v.type == PT::Enum ? (float)v.i : 0.0f; break;
        case PT::Int:   r.i = v.type == PT::Float ? (int)v.f
                            : v.type == PT::Bool ? (v.b ? 1 : 0)
                            : v.type == PT::Enum ? v.i : 0; break;
        case PT::Bool:  r.b = v.type == PT::Float ? v.f != 0.0f
                            : v.type == PT::Int ? v.i != 0 : false; break;
        case PT::Enum:  r.i = v.type == PT::Int ? v.i
                            : v.type == PT::Float ? (int)v.f : 0; break;
        // Same three-way vector conversion as the interpreter, pad by target.
        case PT::Vec3:  r.v3 = v.type == PT::Vec4  ? glm::vec3(v.v4)
                             : v.type == PT::Color ? glm::vec3(v.col) : glm::vec3(0.0f); break;
        case PT::Vec4:  r.v4 = v.type == PT::Vec3  ? glm::vec4(v.v3, 0.0f)
                             : v.type == PT::Color ? v.col : glm::vec4(0.0f); break;
        case PT::Color: r.col = v.type == PT::Vec3 ? glm::vec4(v.v3, 1.0f)
                              : v.type == PT::Vec4 ? v.v4
                                                   : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f); break;
        default: break;
    }
    return r;
}

// Static conversion between statically-known pin types — the compile-time
// counterpart of coerce for typed C++ expressions. Anything the interpreter
// can't convert becomes the target's zero value.
std::string convertExpr(const std::string& e, const TypeRef& from, const TypeRef& to)
{
    // User-defined types only convert into THEMSELVES: a Struct is a distinct
    // C++ type per definition, and an enum of another definition is a different
    // set of entries (the interpreter's coerce passes Enum→Enum through, which
    // is what the shared int backing does anyway).
    const bool sameDef = from.t != PT::Struct || from.typeName == to.typeName;
    if (from.arr || to.arr)
    {
        // Containers convert into their own kind only — and a map has to agree
        // on the key as well. Graph::connect refuses everything else, so the
        // zero is only reachable through a stale node.
        const bool sameKey = to.kind() != HorizonCode::ContainerKind::Map ||
                             (from.keyT == to.keyT &&
                              (from.keyT != PT::Enum || from.keyTypeName == to.keyTypeName));
        if (from.arr && to.arr && from.kind() == to.kind() && from.t == to.t &&
            sameDef && sameKey)
            return e;
        return zeroLit(to);   // unwireable; only reachable via stale nodes
    }
    const bool sameEnum = from.t != PT::Enum || from.typeName == to.typeName;
    if (from.t == to.t) return (sameDef && sameEnum) ? e : zeroLit(to);
    switch (to.t)
    {
        case PT::Float:
            if (from.t == PT::Bool) return "((" + e + ") ? 1.0f : 0.0f)";
            if (from.t == PT::Int)  return "((float)(" + e + "))";
            if (from.t == PT::Enum) return "((float)(int)(" + e + "))";
            break;
        case PT::Int:
            if (from.t == PT::Float) return "((int)(" + e + "))";
            if (from.t == PT::Bool)  return "((" + e + ") ? 1 : 0)";
            if (from.t == PT::Enum)  return "((int)(" + e + "))";
            break;
        case PT::Bool:
            if (from.t == PT::Float) return "((" + e + ") != 0.0f)";
            if (from.t == PT::Int)   return "((" + e + ") != 0)";
            break;
        // Vector ↔ colour on a statically-typed wire. This is the branch a graph
        // authored before the Vec3 split runs through: its Color output still
        // feeds what is now a Vec3 parameter, and links are restored from JSON
        // without re-checking types — so if this were missing, every legacy wire
        // would compile to a zero while the interpreter converted it properly.
        case PT::Vec3:
            if (from.t == PT::Vec4)  return "glm::vec3(glm::vec4(" + e + "))";
            if (from.t == PT::Color) return "glm::vec3(" + e + ")";
            break;
        case PT::Vec4:
            if (from.t == PT::Vec3)  return "hc::Vec4(glm::vec4(" + e + ", 0.0f))";
            if (from.t == PT::Color) return "hc::Vec4(" + e + ")";
            break;
        case PT::Color:
            // Pad alpha 1, not 0 — an opaque colour is Color's own empty.
            if (from.t == PT::Vec3)  return "glm::vec4(" + e + ", 1.0f)";
            if (from.t == PT::Vec4)  return "glm::vec4(" + e + ")";
            break;
        case PT::Enum:
            if (from.t == PT::Int)   return "((" + cppScalar(to) + ")(" + e + "))";
            if (from.t == PT::Float) return "((" + cppScalar(to) + ")(int)(" + e + "))";
            break;
        default: break;
    }
    return zeroLit(to);
}

// hc::coerce<T>/coerceArray<T> around a Value expression (dynamic coercion at
// the Value boundaries: event args, host reads, undeclared variables).
std::string coerceCall(const std::string& valueExpr, const TypeRef& to)
{
    if (to.kind() == HorizonCode::ContainerKind::Set)
        return "hc::coerceSet<" + cppScalar(to) + ">(" + valueExpr + ")";
    if (to.kind() == HorizonCode::ContainerKind::Map)
        return "hc::coerceMap<" + cppScalar(to.key()) + ", " + cppScalar(to) + ">(" +
               valueExpr + ")";
    if (to.arr) return "hc::coerceArray<" + cppScalar(to) + ">(" + valueExpr + ")";
    // An Enum target is NOT coerce<int>: `coerce(v, Enum)` refuses Bool (§3.3).
    if (to.t == PT::Enum)
        return "((" + cppScalar(to) + ")hc::coerceEnum(" + valueExpr + "))";
    return "hc::coerce<" + cppScalar(to) + ">(" + valueExpr + ")";
}
std::string fromValueCall(const std::string& vecExpr, size_t k, const TypeRef& to)
{
    const std::string at = ", " + std::to_string(k) + ")";
    if (to.kind() == HorizonCode::ContainerKind::Set)
        return "hc::fromValueSet<" + cppScalar(to) + ">(" + vecExpr + at;
    if (to.kind() == HorizonCode::ContainerKind::Map)
        return "hc::fromValueMap<" + cppScalar(to.key()) + ", " + cppScalar(to) + ">(" +
               vecExpr + at;
    if (to.arr) return "hc::fromValueArray<" + cppScalar(to) + ">(" + vecExpr + at;
    return "hc::fromValue<" + cppScalar(to) + ">(" + vecExpr + at;
}

// Box a typed C++ expression back into a Value (Set{Property,External},
// EmitEvent, call args, variable reflection). The user-defined types carry
// their definition path across this seam — for enums it is a baked literal
// (they are plain ints in C++), for structs the generated converter sets it.
std::string toValueCall(const std::string& expr, const TypeRef& from, const std::string& ns)
{
    // Set/Map have their own hc::toValue overloads, which already strip the
    // definition path off enum elements the way toEnumValueArray does.
    const bool plainArray = from.kind() == HorizonCode::ContainerKind::Array;
    if (from.t == PT::Enum && !from.arr)
        return "hc::toEnumValue((int)(" + expr + "), " + strLit(from.typeName) + ")";
    if (from.t == PT::Enum && plainArray) return "hc::toEnumValueArray(" + expr + ")";
    if (from.t == PT::Struct && !from.arr) return ns + "::toValue(" + expr + ")";
    return "hc::toValue(" + expr + ")";
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

// Struct members are the only user-authored identifiers that reach the output
// WITHOUT a prefix (variables get v_, functions f_, events ev_, classes C_,
// struct types S_), because the generated struct is meant to be readable. Field
// names are free text in the type panel, so a field called `class` or `int`
// would otherwise emit a header that does not compile — those get a trailing _.
bool isCppKeyword(const std::string& s)
{
    static const std::unordered_set<std::string> kKeywords = {
        "alignas","alignof","and","and_eq","asm","auto","bitand","bitor","bool","break","case",
        "catch","char","char8_t","char16_t","char32_t","class","compl","concept","const",
        "consteval","constexpr","constinit","const_cast","continue","co_await","co_return",
        "co_yield","decltype","default","delete","do","double","dynamic_cast","else","enum",
        "explicit","export","extern","false","float","for","friend","goto","if","inline","int",
        "long","mutable","namespace","new","noexcept","not","not_eq","nullptr","operator","or",
        "or_eq","private","protected","public","register","reinterpret_cast","requires","return",
        "short","signed","sizeof","static","static_assert","static_cast","struct","switch",
        "template","this","thread_local","throw","true","try","typedef","typeid","typename",
        "union","unsigned","using","virtual","void","volatile","wchar_t","while","xor","xor_eq",
    };
    return kKeywords.count(s) != 0;
}
std::string cppIdent(const std::string& raw)
{
    const std::string s = sanitize(raw);
    return isCppKeyword(s) ? s + "_" : s;
}

// Claim `base` in `used`, suffixing _2, _3 … until it is free. Every generated
// C++ identifier (member/local vars, functions, events, class names) goes through
// this so name mangling stays deterministic and collision-free in one place.
std::string uniqueName(const std::string& base, std::unordered_set<std::string>& used)
{
    std::string name = base;
    for (int i = 2; used.count(name); ++i) name = base + "_" + std::to_string(i);
    used.insert(name);
    return name;
}

// ── the run's struct types (one hcgen_type_*.h per definition) ───────────────
// Every struct definition reachable from the sources, closed over nested
// fields, ordered so a struct is emitted after everything it embeds.
void collectUserTypePaths(const Graph& g, PinType kind, std::unordered_set<std::string>& out)
{
    auto take = [&out, kind](PinType t, const std::string& name)
    { if (t == kind && !name.empty()) out.insert(name); };

    for (const Node& n : g.nodes)
    {
        switch (n.type)
        {
            case NT::MakeStruct: case NT::BreakStruct:
            case NT::GetStructField: case NT::SetStructField:
                if (kind == PT::Struct && !n.typeName.empty()) out.insert(n.typeName);
                break;
            case NT::ConstEnum: case NT::SwitchOnEnum:
            case NT::EnumToInt: case NT::IntToEnum: case NT::EnumToString:
                if (kind == PT::Enum && !n.typeName.empty()) out.insert(n.typeName);
                break;
            default: break;
        }
        // User-defined values also ride on nodes that merely pass them through:
        // Get/SetVariable, Get/SetExternal, arrays, ForEach, function pins.
        take(n.propType, n.typeName);
        for (const auto& p : n.params)  take(p.type, p.typeName);
        for (const auto& r : n.results) take(r.type, r.typeName);
    }
    for (const Variable& v : g.variables) take(v.type, v.typeName);
}

TypeTable buildTypeTable(const std::vector<ClassSource>& sources)
{
    const HE::TypeRegistry& reg = HE::TypeRegistry::instance();

    std::unordered_set<std::string> wanted;
    for (const ClassSource& s : sources) collectUserTypePaths(s.graph, PT::Struct, wanted);

    // Transitive closure over nested struct fields (the registry's own cycle
    // guard makes this terminate; a hand-edited cycle stops at the seen-set).
    std::unordered_map<std::string, HE::StructDef> defs;
    std::vector<std::string> stack(wanted.begin(), wanted.end());
    while (!stack.empty())
    {
        const std::string path = stack.back();
        stack.pop_back();
        if (defs.count(path)) continue;
        HE::StructDef def;
        if (!reg.getStruct(path, def)) continue;   // unknown → the class falls back
        for (const auto& f : def.fields)
            if (f.type == PT::Struct && !f.typeName.empty() && !defs.count(f.typeName))
                stack.push_back(f.typeName);
        defs.emplace(path, std::move(def));
    }

    // Deterministic order: sort by asset path, then topologically lift the
    // dependencies (same shape as CppTypesHeaderGen's dependency pass).
    TypeTable tt;
    std::vector<std::string> paths;
    paths.reserve(defs.size());
    for (const auto& [path, _] : defs) paths.push_back(path);
    std::sort(paths.begin(), paths.end());

    std::unordered_set<std::string> emitted;
    for (bool progress = true; progress; )
    {
        progress = false;
        for (const std::string& path : paths)
        {
            if (emitted.count(path)) continue;
            const HE::StructDef& def = defs.at(path);
            const bool ready = std::all_of(def.fields.begin(), def.fields.end(),
                [&](const HE::StructField& f)
                {
                    return f.type != PT::Struct || f.typeName.empty() ||
                           !defs.count(f.typeName) || emitted.count(f.typeName);
                });
            if (!ready) continue;
            emitted.insert(path);
            tt.order.push_back(path);
            progress = true;
        }
    }

    std::unordered_set<std::string> usedNames;

    // Enums first: they depend on nothing, and a struct field may name one.
    {
        std::unordered_set<std::string> wantedEnums;
        for (const ClassSource& s : sources) collectUserTypePaths(s.graph, PT::Enum, wantedEnums);
        for (const auto& [path, def] : defs)
            for (const auto& f : def.fields)
                if (f.type == PT::Enum && !f.typeName.empty()) wantedEnums.insert(f.typeName);
        std::vector<std::string> epaths(wantedEnums.begin(), wantedEnums.end());
        std::sort(epaths.begin(), epaths.end());
        for (const std::string& path : epaths)
        {
            HE::EnumDef def;
            if (!reg.getEnum(path, def)) continue;   // unknown → the class falls back
            EnumType et;
            et.def = def;
            et.cpp = uniqueName("E_" + sanitize(def.name), usedNames);
            std::unordered_set<std::string> usedEntries;
            for (const auto& e : def.entries)
                et.entries.push_back(uniqueName(cppIdent(e.name), usedEntries));
            tt.enumOrder.push_back(path);
            tt.enumsByPath.emplace(path, std::move(et));
        }
    }

    for (const std::string& path : tt.order)
    {
        StructType st;
        st.def = defs.at(path);
        st.cpp = uniqueName("S_" + sanitize(st.def.name), usedNames);
        std::unordered_set<std::string> usedFields;
        for (const auto& f : st.def.fields)
            st.members.push_back(uniqueName(cppIdent(f.name), usedFields));
        tt.byPath.emplace(path, std::move(st));
    }
    return tt;
}

// A member's initializer, omitted where default construction already IS the
// zero value (std::string, hc::Array, hc::Transform, a nested struct) — the
// generated struct should read like one somebody wrote by hand.
std::string memberInit(const TypeRef& tr)
{
    if (tr.arr) return {};
    switch (tr.t)
    {
        case PT::String: case PT::Transform: case PT::Struct: return {};
        default: return " = " + zeroLit(tr);
    }
}

// One header per definition, so a project's type headers are not one shared
// dependency: editing a Struct asset must rebuild what uses THAT struct, not
// every translation unit that happens to touch some user-defined type. The
// file name is the C++ name, which is already unique across the run.
std::string typeHeaderName(const std::string& cppName)
{ return "hcgen_type_" + cppName + ".h"; }

// hcgen_type_<Enum>.h — the `enum class` plus the hc:: template hooks the
// generic helpers dispatch through. Depends on nothing.
std::string emitEnumHeader(const EnumType& et, const std::string& path, const std::string& ns)
{
    std::string h;
    h += "// GENERATED by HorizonCode → C++ codegen — do not edit.\n";
    h += "// The Enum asset \"" + et.def.name + "\" (" + path + ") as a plain C++ type.\n";
    h += "#pragma once\n";
    h += "#include <HorizonCode/HorizonCodeGenSupport.h>\n\n";
    h += "namespace " + ns + " {\n";
    {
        h += "\n// \"" + et.def.name + "\" (" + path + ")\n";
        h += "enum class " + et.cpp + " : int\n{\n";
        // Two entries may share a value; EnumDef::findValue answers with the
        // first, so the later one is unreachable — and a duplicate enumerator
        // value would be legal C++ but a lie about what this enum can be.
        std::unordered_set<int> seenValues;
        for (size_t i = 0; i < et.def.entries.size(); ++i)
        {
            const auto& e = et.def.entries[i];
            if (!seenValues.insert(e.value).second)
            {
                h += "    // " + et.entries[i] + " = " + std::to_string(e.value) +
                     " — same value as an earlier entry, unreachable\n";
                continue;
            }
            h += "    " + et.entries[i] + " = " + std::to_string(e.value) + ",";
            if (et.entries[i] != e.name) h += "   // \"" + e.name + "\"";
            h += "\n";
        }
        h += "};\n";
        h += "inline hc::Value toValue(" + et.cpp + " e)\n";
        h += "{ return hc::toEnumValue((int)e, " + strLit(path) + "); }\n";
    }
    // The generic hc:: helpers dispatch on these, exactly like the structs do;
    // a struct FIELD of enum type needs them declared before its converter.
    h += "\n} // namespace " + ns + "\n\nnamespace hc {\n";
    {
        const std::string t = ns + "::" + et.cpp;
        h += "template <> inline " + t + " zeroOf<" + t + ">() { return {}; }\n";
        h += "template <> inline " + t + " raw<" + t + ">(const Value& v) { return (" + t + ")v.i; }\n";
        h += "template <> inline " + t + " coerce<" + t + ">(const Value& v) { return (" + t + ")coerceEnum(v); }\n";
        h += "template <> inline PinType tagOf<" + t + ">() { return PinType::Enum; }\n";
    }
    h += "} // namespace hc\n";
    return h;
}

// hcgen_type_<Struct>.h — the C++ aggregate plus its Value converters and the
// same hc:: hooks. Order inside the file matters: the converter reads its
// nested-struct fields through hc::coerce<Nested>, so that specialization has
// to exist first — hence the definition, then the declarations, then the hc::
// specializations, then the bodies. Nested definitions live in their own
// headers and come in through the includes at the top.
std::string emitStructHeader(const TypeTable& tt, const std::string& path, const std::string& ns)
{
    const StructType& st = tt.byPath.at(path);

    auto fieldType = [&tt](const HE::StructField& f)
    { return TypeRef{ f.type, f.isArray, f.typeName, tt.cppOf(f.typeName),
                      f.container, f.keyType, f.keyTypeName, tt.cppOf(f.keyTypeName) }; };

    std::string h;
    h += "// GENERATED by HorizonCode → C++ codegen — do not edit.\n";
    h += "// The Struct asset \"" + st.def.name + "\" (" + path + ") as a plain C++ type.\n";
    h += "#pragma once\n";
    h += "#include <HorizonCode/HorizonCodeGenSupport.h>\n";
    {
        // The definitions this one embeds, each from its own header. Arrays
        // count: an array field's converter still names the element type. The
        // set mirrors buildTypeTable's closure, so anything reachable is here.
        std::set<std::string> deps;
        for (const HE::StructField& f : st.def.fields)
        {
            if (f.type != PT::Struct && f.type != PT::Enum) continue;
            const std::string cpp = tt.cppOf(f.typeName);
            if (!cpp.empty()) deps.insert(typeHeaderName(cpp));
        }
        for (const std::string& d : deps) h += "#include \"" + d + "\"\n";
    }
    h += "\nnamespace " + ns + " {\n";

    h += "\n// \"" + st.def.name + "\" (" + path + ")\n";
    h += "struct " + st.cpp + "\n{\n";
    for (size_t i = 0; i < st.def.fields.size(); ++i)
    {
        const auto& f = st.def.fields[i];
        h += "    " + cppType(fieldType(f)) + " " + st.members[i] + memberInit(fieldType(f)) + ";";
        if (st.members[i] != f.name) h += "   // \"" + f.name + "\"";
        h += "\n";
    }
    h += "};\n";

    h += "\ninline hc::Value toValue(const " + st.cpp + "& s);\n";
    h += "inline " + st.cpp + " fromValue_" + st.cpp + "(const hc::Value& v);\n";
    h += "\n} // namespace " + ns + "\n";

    // The generic hc:: helpers (arrays, coerce, cached results) dispatch on
    // these; hc::toValue is found by ADL in the namespace above.
    h += "\nnamespace hc {\n";
    {
        const std::string t = ns + "::" + st.cpp;
        h += "template <> inline " + t + " zeroOf<" + t + ">() { return {}; }\n";
        h += "template <> inline " + t + " raw<" + t + ">(const Value& v) { return " +
             ns + "::fromValue_" + st.cpp + "(v); }\n";
        h += "template <> inline " + t + " coerce<" + t + ">(const Value& v) { return " +
             ns + "::fromValue_" + st.cpp + "(v); }\n";
        h += "template <> inline PinType tagOf<" + t + ">() { return PinType::Struct; }\n";
    }
    h += "} // namespace hc\n";

    h += "\nnamespace " + ns + " {\n";
    {
        // → Value: fields in DEFINITION order, the layout every consumer
        //   (interpreter, savegames, scripts) resolves against.
        h += "\ninline hc::Value toValue(const " + st.cpp + "& s)\n{\n";
        h += "    hc::Value v; v.type = hc::PinType::Struct; v.typeName = " + strLit(path) + ";\n";
        if (st.def.fields.empty()) h += "    (void)s;\n";
        else h += "    v.items.reserve(" + std::to_string(st.def.fields.size()) + ");\n";
        for (size_t i = 0; i < st.def.fields.size(); ++i)
        {
            const TypeRef ft = fieldType(st.def.fields[i]);
            std::string boxed = toValueCall("s." + st.members[i], ft, ns);
            // TypeRegistry::fieldDefault stamps a container FIELD's Value and
            // its elements with the field's definition path (and a map's keys
            // with the key definition), so the generated converter has to do
            // the same or the two seeds are not identical Values.
            if (ft.kind() == HorizonCode::ContainerKind::Map)
                boxed = "hc::tagMap(" + boxed + ", " + strLit(ft.typeName) + ", " +
                        strLit(ft.keyTypeName) + ")";
            else if (ft.arr && !ft.typeName.empty())
                boxed = "hc::tagArray(" + boxed + ", " + strLit(ft.typeName) + ")";
            h += "    v.items.push_back(" + boxed + ");\n";
        }
        h += "    return v;\n}\n";

        // ← Value: a non-struct Value reads as all-zero fields, exactly like the
        //   interpreter's coerce(v, Struct) followed by field reads (§3.3).
        h += "inline " + st.cpp + " fromValue_" + st.cpp + "(const hc::Value& v)\n{\n";
        h += "    " + st.cpp + " s;\n";
        h += "    if (v.type != hc::PinType::Struct) return s;\n";
        for (size_t i = 0; i < st.def.fields.size(); ++i)
        {
            const auto& f  = st.def.fields[i];
            const TypeRef ft = fieldType(f);
            const std::string k = std::to_string(i);
            std::string read;
            switch (ft.kind())
            {
            case HorizonCode::ContainerKind::Set:
                read = "hc::itemSet<" + cppScalar(ft) + ">(v.items, " + k + ")"; break;
            case HorizonCode::ContainerKind::Map:
                read = "hc::itemMap<" + cppScalar(ft.key()) + ", " + cppScalar(ft) +
                       ">(v.items, " + k + ")"; break;
            case HorizonCode::ContainerKind::Array:
                read = "hc::itemArray<" + cppScalar(ft) + ">(v.items, " + k + ")"; break;
            default:
                read = "hc::item<" + cppScalar(ft) + ">(v.items, " + k + ")"; break;
            }
            h += "    s." + st.members[i] + " = " + read + ";\n";
        }
        h += "    return s;\n}\n";
    }
    h += "\n} // namespace " + ns + "\n";
    return h;
}

// hcgen_types.h — every type header at once, for hand-written C++ that wants
// the project's whole type surface. NOTHING generated includes this: a class
// pulls in exactly the definitions it uses, so editing one Struct asset does
// not invalidate every translation unit that happens to touch some other type.
std::string emitTypesUmbrella(const TypeTable& tt)
{
    std::string h;
    h += "// GENERATED by HorizonCode → C++ codegen — do not edit.\n";
    h += "// The project's Struct and Enum assets as plain C++ types, in one\n";
    h += "// include. Each definition also has its own hcgen_type_*.h; the\n";
    h += "// generated classes include those directly, so that a changed\n";
    h += "// definition rebuilds only what actually uses it.\n";
    h += "#pragma once\n";
    for (const std::string& path : tt.enumOrder)
        h += "#include \"" + typeHeaderName(tt.enumsByPath.at(path).cpp) + "\"\n";
    for (const std::string& path : tt.order)
        h += "#include \"" + typeHeaderName(tt.byPath.at(path).cpp) + "\"\n";
    return h;
}

// ── the run's compiled classes ───────────────────────────────────────────────
// Which class keys made it to native C++, and under which C++ name. A graph can
// only call another class DIRECTLY if that class is in here — so the run
// compiles once to find out, then again for real (generateInto). The graph
// comes along because the caller has to check, at generation time, that the
// function or variable it wants exists and is public on the target.
struct CompiledClass
{
    std::string key;              // the registry key, for diagnostics
    std::string cpp;
    const Graph* graph = nullptr;
    // The HorizonCode class this one derives from ("" = none). With inheritance
    // emitted as inheritance, a lookup that has to reach "anywhere up the chain"
    // — an inherited variable's member, an inherited function's method — walks
    // this rather than re-reading the source's chain.
    std::string parentKey;
    // The C++ names its public surface got — a caller cannot re-derive them
    // (they are uniquified against that class's whole scope), so the probe pass
    // records them.
    std::unordered_map<std::string, std::string> publicFn;    // graph function → method
    std::unordered_map<std::string, std::string> publicVar;   // graph variable → accessor
    // Every public C++ method this class exposes for its events and functions,
    // with its parameter list — what a derived class compares against to know
    // whether its same-named member OVERRIDES one or merely shares a name.
    std::unordered_map<std::string, std::string> publicMethods;   // method → params
    // EVERY instance variable's member name, public or not. A derived class
    // reads and writes an inherited variable as a plain member (the members are
    // protected, and the name resolves through the base) — but only if it knows
    // what that member is called.
    std::unordered_map<std::string, std::string> varMember;   // graph variable → v_*
};
struct ClassTable
{
    std::unordered_map<std::string, CompiledClass> byKey;   // class key → class
    // Keys some other source derives from. A class in here cannot be `final`.
    std::unordered_set<std::string> isBase;
    const CompiledClass* find(const std::string& key) const
    {
        const auto it = byKey.find(key);
        return it != byKey.end() ? &it->second : nullptr;
    }
    // The nearest ANCESTOR of `key` that declares `name` — an instance variable
    // when `var`, else a function. `key` itself is skipped: this answers "did I
    // inherit this", and a class's own members are found without it. Null when
    // no ancestor declares it (or the chain leaves this run).
    const CompiledClass* declaring(const std::string& key, const std::string& name,
                                   bool var) const
    {
        const CompiledClass* start = find(key);
        for (std::string k = start ? start->parentKey : std::string(); !k.empty(); )
        {
            const CompiledClass* p = find(k);
            if (!p) break;
            if (p->graph)
            {
                if (var)
                {
                    const Variable* v = p->graph->findVariable(name);
                    if (v && v->scope == 0) return p;
                }
                else
                {
                    for (const Node& n : p->graph->nodes)
                        if (n.type == NT::FunctionEntry && n.s == name) return p;
                }
            }
            k = p->parentKey;
        }
        return nullptr;
    }
};

// ── the engine's own events ──────────────────────────────────────────────────
// The table lives in HE_Core next to CompiledInstance, because the hook names
// ARE that interface — see HorizonCode::engineEvents().
using EngineEvent = HorizonCode::EngineEventDesc;
const EngineEvent* engineEventFor(const std::string& name)
{ return HorizonCode::findEngineEvent(name); }

// The hook's parameter list, and how the typed argument reaches rs.eventArg.
std::string hookParams(const EngineEvent& e, bool argUsed)
{
    std::string p;
    if (e.elem) p = "int elem";
    if (e.arg != PT::Exec)
    {
        const char* t = e.arg == PT::String ? "const std::string&"
                      : e.arg == PT::Float  ? "float"
                      : e.arg == PT::Bool   ? "bool" : "int";
        if (!p.empty()) p += ", ";
        p += std::string(t) + (argUsed ? " arg" : "");
    }
    return p;
}

// ── the per-class emitter ─────────────────────────────────────────────────────
class ClassEmitter
{
public:
    ClassEmitter(const ClassSource& src, const std::string& className,
                 const Options& opt, const TypeTable& tt, const ClassTable& ct,
                 std::vector<std::string>& warnings)
        : m_src(src), m_g(src.graph), m_cls(className), m_opt(opt), m_tt(tt), m_ct(ct),
          m_warnings(warnings) {}

    // A header carrying the whole class (its bodies inline — a generated class
    // has no reason to spread a declaration and a definition over two files),
    // and a .cpp holding just the factory pair the manifest links against.
    // The header is what lets other C++ — generated or hand-written — include
    // the class and call its events and functions directly, instead of through
    // the Runtime's name-based seam.
    const std::unordered_map<std::string, std::string>& publicFunctions() const { return m_publicFn; }
    const std::unordered_map<std::string, std::string>& publicVariables()  const { return m_publicVar; }
    const std::unordered_map<std::string, std::string>& variableMembers()  const { return m_varMember; }
    // Valid after run(): the public surface a derived class matches against.
    std::unordered_map<std::string, std::string> publicMethodSigs() const
    {
        std::unordered_map<std::string, std::string> out;
        for (const auto& [id, name] : m_publicEv) out.emplace(name, std::string());
        for (const Node& n : m_g.nodes)
            if (n.type == NT::FunctionEntry && m_publicFn.count(n.s))
                out.emplace(m_publicFn.at(n.s), publicFnParams(n));
        return out;
    }

    void run(std::string& header, std::string& impl)
    {
        validate();
        buildNames();
        buildSlots();
        emitClassFiles(header, impl);
    }

private:
    const ClassSource& m_src;
    Graph              m_g;      // private copy: EngineCall pins may be re-mirrored
    std::string        m_cls;
    const Options&     m_opt;
    const TypeTable&   m_tt;
    const ClassTable&  m_ct;
    std::vector<std::string>& m_warnings;

    // Pin types, with the user-defined definitions resolved against the run's
    // type table (Struct pins get their generated C++ name here).
    TypeRef trOf(const HorizonCode::PinDesc& d) const
    {
        const std::string tn  = d.typeName    ? d.typeName    : "";
        const std::string ktn = d.keyTypeName ? d.keyTypeName : "";
        return { d.type, d.isArray, tn, m_tt.cppOf(tn),
                 d.container, d.keyType, ktn, m_tt.cppOf(ktn) };
    }
    TypeRef dataInType(const Node& n, int idx) const
    {
        const NodeSig s = HorizonCode::signatureOf(n);
        if (idx < 0 || idx >= (int)s.dataIns.size()) return {};
        return trOf(s.dataIns[idx]);
    }
    TypeRef dataOutType(const Node& n, int idx) const
    {
        const NodeSig s = HorizonCode::signatureOf(n);
        if (idx < 0 || idx >= (int)s.dataOuts.size()) return {};
        return trOf(s.dataOuts[idx]);
    }
    // Variables/params carry their own definition path rather than a pin desc.
    TypeRef trOf(PinType t, bool isArray, const std::string& typeName,
                 HorizonCode::ContainerKind ctr = HorizonCode::ContainerKind::None,
                 PinType keyT = PT::String, const std::string& keyTypeName = {}) const
    { return { t, isArray, typeName, m_tt.cppOf(typeName),
               ctr, keyT, keyTypeName, m_tt.cppOf(keyTypeName) }; }
    // The declaration shapes that carry containers, spelled once each — the
    // three-argument form silently drops the kind, so anything holding a
    // declaration goes through these instead.
    TypeRef trOf(const HorizonCode::Variable& v) const
    { return trOf(v.type, v.isArray, v.typeName, v.container, v.keyType, v.keyTypeName); }
    TypeRef trOf(const HorizonCode::FuncParam& p) const
    { return trOf(p.type, p.isArray, p.typeName, p.container, p.keyType, p.keyTypeName); }
    TypeRef trOf(const HE::StructField& f) const
    { return trOf(f.type, f.isArray, f.typeName, f.container, f.keyType, f.keyTypeName); }
    // A node's OWN container shape (Get/SetVariable mirror their variable's;
    // the Set/Map ops set theirs explicitly).
    TypeRef nodeType(const Node& n) const
    { return trOf(n.propType, n.isArray, n.typeName, n.container, n.keyType, n.keyTypeName); }
    // The same, forced to a container of `k` — the Set/Map node lowering, where
    // the kind comes from the node TYPE rather than from a stored flag.
    TypeRef ctrType(const Node& n, HorizonCode::ContainerKind k) const
    { return trOf(n.propType, true, n.typeName, k, n.keyType, n.keyTypeName); }

    std::unordered_map<std::string, std::string> m_varMember;  // instance var → v_*
    std::unordered_map<std::string, std::string> m_localName;  // local var → l_* (names graph-unique)
    std::unordered_map<std::string, std::string> m_fnName;     // function → f_*
    std::unordered_map<int, std::string>         m_evName;     // Event node id → ev_*
    // InputAction node id → one body method per exec-out (Pressed, Released),
    // or a single one for an axis action.
    std::unordered_map<int, std::vector<std::string>> m_iaName;
    std::unordered_map<int, std::string>         m_publicEv;   // Event node id → public method
    std::unordered_map<std::string, std::string> m_publicFn;   // function → public method
    std::unordered_map<std::string, std::string> m_publicVar;  // public variable → accessor
    std::unordered_map<std::string, std::string> m_evtConst;   // event name → EventId constant
    // Other compiled classes this one calls directly — its .cpp includes exactly
    // these headers. Sorted, so the emitted include list is deterministic and an
    // unrelated edit cannot churn it.
    std::set<std::string>                        m_usedClasses;
    struct Slot { std::string field; TypeRef tr; };
    std::unordered_map<int, std::vector<Slot>>   m_slots;      // exec-cached node → RunState fields

    // ── what this class actually needs from the per-run scaffolding ─────────
    // A small graph carries none of it, and then none of it is emitted: no
    // RunState, no step guard, no `rs` parameter — just the statements.
    bool m_guard        = false;   // step/depth guards can be reached at all
    bool m_usesEventArg = false;   // some Event data-out is read
    bool m_rsTouched    = false;   // the body being emitted right now mentions `rs`
    int  m_stmtCount    = 0;       // statements emitted in this translation pass
    bool useRunState() const { return m_guard || m_usesEventArg || !m_slots.empty(); }
    std::string rsParam(bool used = true) const
    { return useRunState() ? (used ? "RunState& rs" : "RunState&") : std::string(); }
    std::string rsArg() const { return useRunState() ? "rs" : std::string(); }

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

    // ── inheritance ─────────────────────────────────────────────────────────
    // The class this one derives FROM, as C++. chain.front() is the nearest
    // HorizonCode ancestor; an empty chain means this class derives from an
    // engine row (or nothing), and CompiledInstance stays the base.
    const std::string& parentKey() const
    {
        static const std::string kNone;
        return m_src.chain.empty() ? kNone : m_src.chain.front();
    }
    const CompiledClass* parent() const
    { return parentKey().empty() ? nullptr : m_ct.find(parentKey()); }
    // The ancestor that declares `name`, or null when this class does / nobody
    // does. Answers "is this member inherited" for the body emitters.
    const CompiledClass* inheritedVarFrom(const std::string& name) const
    {
        if (m_g.findVariable(name)) return nullptr;   // ours wins
        return m_ct.declaring(m_src.key, name, /*var=*/true);
    }
    const CompiledClass* inheritedFnFrom(const std::string& name) const
    {
        if (fnEntryByName(name)) return nullptr;
        return m_ct.declaring(m_src.key, name, /*var=*/false);
    }
    // That ancestor's declaration, for the signature a call has to match.
    static const Node* fnEntryIn(const CompiledClass& c, const std::string& name)
    {
        if (!c.graph) return nullptr;
        for (const Node& n : c.graph->nodes)
            if (n.type == NT::FunctionEntry && n.s == name) return &n;
        return nullptr;
    }

    // ── the public methods' virtual-ness ────────────────────────────────────
    // These are the entry points hand-written C++ uses to run a graph's events
    // and functions directly. Without `virtual` a derived class's same-named
    // method HIDES the base's rather than replacing it — so a caller holding a
    // base pointer to a derived object would run the BASE's body, while the very
    // same instance runs the derived one through the Runtime (fireEvent resolves
    // leaf-first). Two ways to the same member, two different answers.
    //
    // Note this does NOT read Node::overridable. That flag decides what the
    // editor OFFERS as an override; the Runtime lets any same-named declaration
    // win regardless, and the generated C++ has to follow the Runtime.
    //
    // A class that nothing derives from stays non-virtual — the vtable cost only
    // lands where inheritance actually happens.
    struct MethodDecl { std::string prefix, suffix; };
    MethodDecl methodDecl(const std::string& method, const std::string& params) const
    {
        for (const CompiledClass* a = parent(); a;
             a = a->parentKey.empty() ? nullptr : m_ct.find(a->parentKey))
        {
            const auto it = a->publicMethods.find(method);
            // Same name AND same signature — anything else is not an override in
            // C++'s sense, and marking it one would not compile.
            if (it != a->publicMethods.end() && it->second == params)
                return { {}, " override" };
        }
        return { m_ct.isBase.count(m_src.key) ? "virtual " : "", {} };
    }

    // An instance variable by name, wherever in the chain it is declared, with
    // the C++ member that reads it. The members are protected, so an inherited
    // one is reached by its plain name from here — which is what makes the
    // generated code agree with the interpreter's ONE variable store instead of
    // detouring through the Runtime's overflow map.
    // The number a Delay schedules its resume under. The Runtime keys a pending
    // resume by (instance, LEVEL, node) for an interpreted instance, but a
    // compiled one is a single object with no levels — so a base class's node 7
    // and this class's node 7 would be one and the same pending Delay: the
    // second would be swallowed as a retrigger, and the wrong chain would
    // resume. Shifting by the class's depth in the chain keeps them apart, and
    // the value is opaque to everyone in between — it only has to come back.
    int resumeId(int nodeId) const
    { return (int)m_src.chain.size() * 1000000 + nodeId; }

    struct VarRef { const Variable* decl = nullptr; std::string member; };
    VarRef instanceVar(const std::string& name) const
    {
        if (const Variable* v = m_g.findVariable(name); v && v->scope == 0)
            return { v, m_varMember.at(name) };
        const CompiledClass* from = m_ct.declaring(m_src.key, name, /*var=*/true);
        if (!from || !from->graph) return {};
        const auto it = from->varMember.find(name);
        if (it == from->varMember.end()) return {};
        return { from->graph->findVariable(name), it->second };
    }

    // ── Stage A0: recover missing user-type definitions ─────────────────────
    // Array/ForEach nodes carry their element type in `propType` but not the
    // DEFINITION path, so a struct-array graph would have Struct pins with no
    // C++ type. fromJson already repairs that on load; do it again here because
    // generate() also accepts graphs handed in directly (tests, tooling), and
    // the pass is idempotent.

    // ── Stage A: validation (plan §5.2) ──────────────────────────────────────
    void validate()
    {
        HorizonCode::inferUserTypeNames(m_g);

        // 0. Inheritance. A class whose base did not reach C++ cannot either:
        //    `C_Goblin : public C_Enemy` needs C_Enemy to exist. Falling back
        //    with it is not a loss — the interpreted pair behaves identically —
        //    and it is the only answer that keeps ONE object per instance.
        if (!parentKey().empty() && !parent())
            throw FallbackError{ "base class '" + parentKey() +
                                 "' is not compiled, so this class cannot derive from it", 0 };

        //    A variable an ancestor already declares would become a SECOND C++
        //    member under one name, where the interpreter has one store slot —
        //    the derived write would leave the base's reads on the old value.
        //    The editor refuses to create these; an asset authored before it did
        //    gets told why instead of silently diverging.
        for (const Variable& v : m_g.variables)
        {
            if (v.scope != 0) continue;
            if (const CompiledClass* from = m_ct.declaring(m_src.key, v.name, /*var=*/true))
                throw FallbackError{ "variable '" + v.name + "' is already declared by the base "
                                     "class '" + from->key + "' — rename it, or use the "
                                     "inherited one", 0 };
        }

        // 1. EngineCall nodes must resolve in the registry; the registry is
        //    authoritative for the pin mirror (stale assets re-mirror + warn).
        for (Node& n : m_g.nodes)
        {
            if (n.type != NT::EngineCall) continue;
            const HE::api::ApiFn* fn = HE::api::find(n.s);
            if (!fn) throw FallbackError{ "unknown engine api '" + n.s + "'", n.id };
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

        // 2. User-defined types compile against their DEFINITION as of now:
        //    structs become real C++ types (the run's TypeTable), enums stay
        //    int-backed but need their entries resolved. Either way a missing
        //    definition means the emitted code couldn't match the interpreter's
        //    by-name field/entry resolution → interpreted fallback.
        auto requireStruct = [this](const std::string& typeName, int node)
        {
            if (typeName.empty())
                throw FallbackError{ "struct pin without a definition (generic struct boundary)", node };
            if (!m_tt.find(typeName))
                throw FallbackError{ "struct definition '" + typeName +
                                     "' not registered at generation time", node };
        };
        for (const Node& n : m_g.nodes)
        {
            switch (n.type)
            {
            case NT::MakeStruct: case NT::BreakStruct:
            case NT::GetStructField: case NT::SetStructField:
                requireStruct(n.typeName, n.id);
                break;
            case NT::SwitchOnEnum: case NT::EnumToString:
            case NT::ConstEnum: case NT::EnumToInt: case NT::IntToEnum:
                if (!HE::TypeRegistry::instance().hasEnum(n.typeName))
                    throw FallbackError{ "enum definition '" + n.typeName +
                                         "' not registered at generation time", n.id };
                break;
            default: break;
            }
            // Struct values also travel on plain pass-through nodes.
            if (n.propType == PT::Struct) requireStruct(n.typeName, n.id);
            for (const auto& p : n.params)
                if (p.type == PT::Struct) requireStruct(p.typeName, n.id);
            for (const auto& r : n.results)
                if (r.type == PT::Struct) requireStruct(r.typeName, n.id);
        }
        for (const auto& v : m_g.variables)
            if (v.type == PT::Struct) requireStruct(v.typeName, 0);

        // 2b. A function name is the whole identity of a call: callFunction and
        //     every Call Function node resolve BY NAME, and the interpreter takes
        //     the FIRST entry (§3.1). A second one with the same name is dead
        //     code that looks live in the editor, so say so.
        {
            std::unordered_set<std::string> seenFns;
            for (const Node& n : m_g.nodes)
                if (n.type == NT::FunctionEntry && !seenFns.insert(n.s).second)
                    warn("duplicate function '" + n.s + "' — only the first is ever "
                         "called (node " + std::to_string(n.id) + " is dead)");
        }

        // 2c. A Cast with no target class compiles and runs — it simply always
        //     takes Failure, in both backends alike. That is consistent rather
        //     than broken, so it is a warning and not a fallback: failing the
        //     build over it would be the generator overruling the interpreter.
        for (const Node& n : m_g.nodes)
            if (n.type == NT::Cast && n.s.empty())
                warn("Cast node " + std::to_string(n.id) + " has no target class — "
                     "it always takes Failure");

        // 3. Exec cycles would compile to unbounded loops (the interpreter only
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
                case NT::CallExternal: case NT::ForEach: case NT::Cast:
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
            // A Delay breaks the chain (its exec-out runs via resumeFrom, a
            // fresh tick) — loops THROUGH a Delay are the legit timer pattern.
            if (execEdges && s->type == NT::Delay) continue;
            if (execEdges) adj[l.srcNode].push_back(l.dstNode);
            else if (!cachedOut(*s)) adj[l.dstNode].push_back(l.srcNode); // reader depends on producer
        }
        std::unordered_map<int, int> state; // 0 unseen, 1 on stack, 2 done
        std::function<void(int)> dfs = [&](int id)
        {
            state[id] = 1;
            for (const int next : adj[id])
            {
                if (state[next] == 1) throw FallbackError{ reasonPrefix + std::to_string(next), next };
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
        // ONE namespace for everything that lands in the class: members, bodies,
        // and — new — the public methods named after the graph's own events and
        // functions. Seeded with the CompiledInstance surface so an event called
        // "fireEvent" cannot shadow the override it is dispatched from.
        std::unordered_set<std::string> used = {
            "classKey", "baseClassKey", "classChain", "classTag", "classTag_",
            "varInfos", "eventInfos", "fireEvent", "callFunction",
            "getVariable", "setVariable", "reseedVariables", "collectRefs",
            "resumeFrom", "bindContext", "slots", "RunState", "m_ctx",
        };
        auto unique = [&used](const std::string& base) { return uniqueName(base, used); };
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
            // An Input Action node is TWO entry points (Pressed and Released),
            // or one for an axis — so it gets one body method per exec-out,
            // named after the event it answers to rather than the pin.
            if (n.type == NT::InputAction)
            {
                std::vector<std::string> names;
                for (const std::string& ev : HorizonCode::inputActionEventNames(n))
                    names.push_back(unique("ia_" + sanitize(ev) + "_" + std::to_string(n.id)));
                m_iaName[n.id] = std::move(names);
            }
            if (n.type == NT::DoOnce || n.type == NT::FlipFlop) used.insert(stateMember(n.id));
            if (n.type == NT::Delay) used.insert("resume_" + std::to_string(n.id));
        }
        // Public, authored-name methods. An event only gets one when it is
        // unambiguous — several nodes on one name, or an element filter, is a
        // dispatch decision that no single method can stand for.
        for (const auto& [name, nodes] : eventGroups())
            if (nodes.size() == 1 && nodes[0]->elem == 0 && !engineEventFor(name))
                m_publicEv[nodes[0]->id] = unique(cppIdent(name));   // engine events
                                                                     // get the hook instead
        for (const Node* fn : functionEntries())
            m_publicFn[fn->s] = unique(cppIdent(fn->s));
        // One EventId constant per event this class names in an Emit or a Bind.
        // The constant carries the meaning, so the generated code has no magic
        // numbers — and the id itself is interned at load, never baked.
        {
            std::unordered_set<std::string> evtUsed;
            for (const auto& n : m_g.nodes)
                if ((n.type == NT::EmitEvent || n.type == NT::BindEvent) && !n.s.empty())
                    evtUsed.insert(n.s);
            std::vector<std::string> sorted(evtUsed.begin(), evtUsed.end());
            std::sort(sorted.begin(), sorted.end());
            for (const std::string& e : sorted)
                m_evtConst[e] = unique("kEvt_" + sanitize(e));
        }
        // A public variable gets a typed accessor: it is how another compiled
        // class reads and writes it without going through the name seam.
        for (const auto& v : m_g.variables)
            if (v.scope == 0 && v.access == 0)
                m_publicVar[v.name] = unique(cppIdent(v.name));
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
                        s.push_back({ field((int)k), trOf(n.results[k]) });
                    break;
                case NT::EngineCall:
                    if (n.hasArg)
                        for (size_t k = 0; k < n.results.size(); ++k)
                            s.push_back({ field((int)k), trOf(n.results[k]) });
                    break;
                case NT::ForEachMap:
                    // Key + Value + Index, in pin order (the Key is data-out 0).
                    s.push_back({ field(0), trOf(n.keyType, false, n.keyTypeName) });
                    s.push_back({ field(1), trOf(n.propType, false, n.typeName) });
                    s.push_back({ field(2), { PT::Int, false } });
                    break;
                case NT::ForEach:
                case NT::ForEachSet:
                    s.push_back({ field(0), trOf(n.propType, false, n.typeName) });
                    s.push_back({ field(1), { PT::Int, false } });
                    break;
                // The cast result, cached exactly like the interpreter's: the
                // branch condition and the "As <Class>" output are one value,
                // so the check runs once no matter how often the Success chain
                // reads it.
                case NT::Cast:
                    s.push_back({ field(0), { PT::Ref, false } });
                    break;
                default: break;
            }
            if (!s.empty()) m_slots[n.id] = std::move(s);

            // The step/depth guards (§3.6) exist for graphs that can repeat or
            // nest. Only ForEach and FunctionCall do either — everything else
            // lowers to straight-line code whose statement count is fixed at
            // generation time, so the 4096-step abort is unreachable and the
            // whole counter (and every HC_STEP with it) is dead weight.
            if (n.type == NT::ForEach || n.type == NT::ForEachSet ||
                n.type == NT::ForEachMap || n.type == NT::FunctionCall)
                m_guard = true;
        }
    }

    // ── who is on the other end of a Ref pin ─────────────────────────────────
    // Only two target shapes are statically known: Get Self (it is `this`, so
    // there is nothing to resolve at all) and a Ref VARIABLE whose declared
    // class compiled to C++. Everything else stays on the Runtime's seam.
    struct TargetInfo
    {
        bool                 self = false;   // Get Self → this, nothing to resolve
        bool                 gi   = false;   // Get Game Instance → the Runtime holds it
        const CompiledClass* cls  = nullptr;
    };
    TargetInfo targetOf(const Node& n, int dataIn)
    {
        TargetInfo t;
        const Link* l = dataLinkTo(n.id, pinRanges(n).dataIn0 + dataIn);
        if (!l) return t;
        const Node* src = m_g.findNode(l->srcNode);
        if (!src) return t;
        if (src->type == NT::GetSelf) { t.self = true; return t; }
        if (src->type == NT::GetGameInstance)
        {
            // Its class is fixed at generation time, and the Runtime keeps the
            // object beside the id — so there is no handle to look up either.
            if (const CompiledClass* c = m_ct.find("__game_instance__")) { t.gi = true; t.cls = c; }
        }
        else if (src->type == NT::GetVariable)
        {
            if (const Variable* v = m_g.findVariable(src->s))
                if (v->type == PT::Ref && !v->isArray && !v->className.empty())
                    t.cls = m_ct.find(v->className);
        }
        // Whichever class a direct call lands on, this .cpp needs its header —
        // and ONLY the ones it lands on, so a change over there does not rebuild
        // every other translation unit.
        if (t.cls) m_usedClasses.insert(t.cls->cpp);
        return t;
    }
    // How the object is obtained on the fast path — the GameInstance is handed
    // over directly, everything else goes through its handle.
    static std::string resolveExpr(const TargetInfo& t, const std::string& refVar)
    {
        return t.gi ? "hc::gameInstanceAs<" + t.cls->cpp + ">(m_ctx)"
                    : "hc::as<" + t.cls->cpp + ">(m_ctx, " + refVar + ")";
    }
    static const Node* entryOf(const Graph& g, const std::string& name)
    {
        for (const Node& n : g.nodes)
            if (n.type == NT::FunctionEntry && n.s == name) return &n;
        return nullptr;
    }
    // A call is only direct when the callee's signature is EXACTLY what the call
    // node mirrors — a stale mirror would otherwise emit a type mismatch.
    static bool signatureMatches(const Node& call, const Node& entry)
    {
        auto same = [](const std::vector<HorizonCode::FuncParam>& a,
                       const std::vector<HorizonCode::FuncParam>& b)
        {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i].type != b[i].type || a[i].isArray != b[i].isArray ||
                    a[i].typeName != b[i].typeName) return false;
            return true;
        };
        return same(call.params, entry.params) && same(call.results, entry.results);
    }
    // The public method for `fnName` on `cls`, or empty when calling it directly
    // would not mean the same thing (missing, private, or a stale mirror).
    std::string directFn(const Node& call, const CompiledClass& cls, const Node** entryOut) const
    {
        const Node* e = cls.graph ? entryOf(*cls.graph, call.s) : nullptr;
        if (!e || e->access != 0 || !signatureMatches(call, *e)) return {};
        const auto it = cls.publicFn.find(call.s);
        if (it == cls.publicFn.end()) return {};
        if (entryOut) *entryOut = e;
        return it->second;
    }
    // The accessor for a public variable of `cls`, and its declared type.
    std::string directVar(const std::string& var, const CompiledClass& cls, TypeRef& typeOut) const
    {
        if (!cls.graph) return {};
        const Variable* v = cls.graph->findVariable(var);
        if (!v || v->scope != 0 || v->access != 0) return {};
        const auto it = cls.publicVar.find(var);
        if (it == cls.publicVar.end()) return {};
        typeOut = trOf(*v);
        return it->second;
    }
    // Same, against THIS class (a Get Self target).
    std::string selfFn(const Node& call, const Node** entryOut) const
    {
        const Node* e = entryOf(m_g, call.s);
        if (!e || e->access != 0 || !signatureMatches(call, *e)) return {};
        const auto it = m_publicFn.find(call.s);
        if (it == m_publicFn.end()) return {};
        if (entryOut) *entryOut = e;
        return it->second;
    }
    std::string selfVar(const std::string& var, TypeRef& typeOut) const
    {
        const Variable* v = m_g.findVariable(var);
        if (!v || v->scope != 0 || v->access != 0) return {};
        const auto it = m_publicVar.find(var);
        if (it == m_publicVar.end()) return {};
        typeOut = trOf(*v);
        return it->second;
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
                // Elementary types convert on the wire (canConvertPinType), so
                // the reader casts to ITS pin type — equal types, the common
                // case, come back untouched.
                return convertExpr(expr(*src, outIdx, fnCtx), dataOutType(*src, outIdx), want);
            }
        }
        // Unwired: the pin default constant-folded through coerce, else the zero.
        // A Struct default is already a struct Value (there is nothing to coerce
        // it into) — its items are read positionally, like the interpreter's.
        if (auto it = n.pinDefaults.find(inIdx); it != n.pinDefaults.end() && !want.arr)
            return valueLit(want.t == PT::Struct ? it->second : coerceValue(it->second, want.t),
                            m_tt, want);
        return zeroLit(want);
    }

    // Per-instance node state (DoOnce fired?, FlipFlop side) → a bool member.
    static std::string stateMember(int nodeId)
    { return "n" + std::to_string(nodeId) + "_state"; }
    std::vector<int> stateNodeIds() const
    {
        std::vector<int> ids;
        for (const Node& n : m_g.nodes)
            if (n.type == NT::DoOnce || n.type == NT::FlipFlop) ids.push_back(n.id);
        return ids;
    }
    std::vector<int> delayNodeIds() const
    {
        std::vector<int> ids;
        for (const Node& n : m_g.nodes)
            if (n.type == NT::Delay) ids.push_back(n.id);
        return ids;
    }

    std::string slotRef(const Node& n, int outIdx)
    {
        auto it = m_slots.find(n.id);
        if (it == m_slots.end() || outIdx < 0 || outIdx >= (int)it->second.size())
            return zeroLit(dataOutType(n, outIdx));   // §3.3: out-of-range cache read → zero
        m_rsTouched = true;
        return "rs." + it->second[outIdx].field;
    }

    std::string expr(const Node& n, int outIdx, int fnCtx)
    {
        const TypeRef out = dataOutType(n, outIdx);
        switch (n.type)
        {
        case NT::Event:
            // §3.3: Event data-out ← this run's arg coerced to propType.
            m_usesEventArg = m_rsTouched = true;
            return coerceCall("rs.eventArg", trOf(n.propType, false, n.typeName));
        case NT::InputAction:
            // An axis action's Value rides in on the same argument (the host
            // sends it with the axis event); a digital one has no data-out.
            // Vec2 for a two-dimensional axis — coercing that to a Float would
            // silently hand the graph one component or a zero.
            m_usesEventArg = m_rsTouched = true;
            return coerceCall("rs.eventArg",
                              trOf(n.propType == PT::Vec2 ? PT::Vec2 : PT::Float, false, {}));
        case NT::FunctionEntry:
        {
            // §3.4: FunctionEntry param reads the INNERMOST frame — in C++ that
            // is the enclosing function's parameter. A read from outside the
            // owning function has no equivalent symbol → interpreted fallback.
            if (n.id != fnCtx)
                throw FallbackError{ "function parameter of '" + n.s + "' read outside its function", n.id };
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
            return valueLit(v, m_tt, { PT::Transform, false });
        }
        case NT::GetVariable:
        {
            const Variable* v = m_g.findVariable(n.s);
            const TypeRef pin = nodeType(n);
            if (v && v->scope != 0)
            {
                // §13.4: locals are stack locals of the owning function only.
                if (v->scope != fnCtx)
                    throw FallbackError{ "local '" + n.s + "' read outside its function", n.id };
                return convertExpr(m_localName.at(n.s), trOf(*v), pin);
            }
            // This class's own, or a base class's — both are members here.
            if (const VarRef iv = instanceVar(n.s); iv.decl)
                return convertExpr(iv.member,
                                   trOf(*iv.decl), pin);
            // Undeclared: through the Context → the Runtime store (overflow for
            // compiled instances) so §3.4's dynamic-store semantics stay exact.
            return coerceCall("hc::getVariableCtx(m_ctx, " + strLit(n.s) + ")", pin);
        }
        case NT::GetProperty:
            return coerceCall("hc::getProperty(m_ctx, " + std::to_string(n.elem) + ", " + strLit(n.s) + ")",
                              trOf(n.propType, false, n.typeName));
        case NT::GetExternal:
        {
            const TypeRef want = trOf(n.propType, false, n.typeName);
            const std::string seam = coerceCall(
                "hc::getExternal(m_ctx, " + input(n, 0, fnCtx) + ", " + strLit(n.s) + ")", want);
            const TargetInfo t = targetOf(n, 0);
            TypeRef have;
            if (t.self)
            {
                // `this`: no handle to resolve, no class to check.
                const std::string acc = selfVar(n.s, have);
                return acc.empty() ? seam : convertExpr(acc + "()", have, want);
            }
            if (t.gi && t.cls)
            {
                const std::string acc = directVar(n.s, *t.cls, have);
                if (!acc.empty())
                    // No handle at all on the fast path; the id is only fetched
                    // for the fallback, and reading it has no observable effect.
                    return "([&]() -> " + cppType(want) + " {\n"
                           "        if (" + t.cls->cpp + "* o__ = hc::gameInstanceAs<" + t.cls->cpp + ">(m_ctx)) return " +
                           convertExpr("o__->" + acc + "()", have, want) + ";\n"
                           "        return " + coerceCall("hc::getExternal(m_ctx, hc::gameInstance(m_ctx), " +
                                                          strLit(n.s) + ")", want) + "; }())";
            }
            if (t.cls)
            {
                const std::string acc = directVar(n.s, *t.cls, have);
                if (!acc.empty())
                {
                    // The Ref is read ONCE — it may be a pure engine call, and
                    // its dispatch count is part of the trace (§3.4).
                    const std::string ref = input(n, 0, fnCtx);
                    return "([&]() -> " + cppType(want) + " { const uint32_t t__ = " + ref + ";\n"
                           "        if (" + t.cls->cpp + "* o__ = hc::as<" + t.cls->cpp + ">(m_ctx, t__)) return " +
                           convertExpr("o__->" + acc + "()", have, want) + ";\n"
                           "        return " + coerceCall("hc::getExternal(m_ctx, t__, " + strLit(n.s) + ")", want) +
                           "; }())";
                }
            }
            return seam;
        }
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
        case NT::ForEachSet:
        case NT::ForEachMap:
            return slotRef(n, outIdx);
        case NT::EngineCall:
        {
            if (n.hasArg) return slotRef(n, outIdx);
            // Pure engine call: dispatched on EVERY data-out read (§3.4).
            std::string args = "std::vector<hc::Value>{";
            for (size_t i = 0; i < n.params.size(); ++i)
            {
                if (i) args += ", ";
                args += toValueCall(input(n, (int)i, fnCtx), dataInType(n, (int)i), m_opt.namespaceName);
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
        // §3.4: both search on valueEquals, which has no Struct case — a struct
        // element never compares equal, so these are constant for struct arrays
        // (the *Never helpers still take both inputs, so both still evaluate).
        case NT::ArrayContains:
            return std::string(n.propType == PT::Struct ? "hc::arrContainsNever(" : "hc::arrContains(") +
                   input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        case NT::ArrayIndexOf:
            return std::string(n.propType == PT::Struct ? "hc::arrIndexOfNever(" : "hc::arrIndexOf(") +
                   input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        // ── Set<T> ───────────────────────────────────────────────────────────
        // Straight onto hc::Set's ops, which reproduce the three ordering rules.
        // A struct element never compares equal (scalarValueEquals has no Struct
        // case), and hc::sameElem answers the same way for a type with no
        // operator== — so nothing needs a *Never variant here.
        case NT::SetMake:
        case NT::SetClear:
            return zeroLit(out);   // an empty set of the element type
        case NT::SetLength:
            return "hc::setLength(" + input(n, 0, fnCtx) + ")";
        case NT::SetAdd:
            return "hc::setAdd(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        case NT::SetRemove:
            return "hc::setRemove(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        case NT::SetContains:
            return "hc::setContains(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        case NT::SetToArray:
            return "hc::setToArray(" + input(n, 0, fnCtx) + ")";
        // ── Map<K,V> ─────────────────────────────────────────────────────────
        case NT::MapMake:
        case NT::MapClear:
            return zeroLit(out);
        case NT::MapLength:
            return "hc::mapLength(" + input(n, 0, fnCtx) + ")";
        case NT::MapSet:
            return "hc::mapSet(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ", " +
                   input(n, 2, fnCtx) + ")";
        case NT::MapRemove:
            return "hc::mapRemove(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        case NT::MapContains:
            return "hc::mapContains(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        case NT::MapGet:
            return "hc::mapGet(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ", " +
                   input(n, 2, fnCtx) + ")";
        case NT::MapKeys:
            return "hc::mapKeys(" + input(n, 0, fnCtx) + ")";
        case NT::MapValues:
            return "hc::mapValues(" + input(n, 0, fnCtx) + ")";
        case NT::IsValid:
            return "hc::isValidRef(m_ctx, " + input(n, 0, fnCtx) + ")";
        case NT::Cast:
            // The reference the cast cached when it ran (0 on failure) — the
            // same slot the Success/Failure branch was chosen from, so the
            // check never runs twice.
            return slotRef(n, 0);
        case NT::FlipFlop:
            // IsA = the side the LAST execution took (false before any run).
            return stateMember(n.id);
        case NT::MakeVector2:
            return "glm::vec2(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ")";
        case NT::MakeVector3:
            return "glm::vec3(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ", " +
                   input(n, 2, fnCtx) + ")";
        case NT::MakeVector4:
            return "hc::Vec4(" + input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ", " +
                   input(n, 2, fnCtx) + ", " + input(n, 3, fnCtx) + ")";
        case NT::BreakVector2:
            return "(" + input(n, 0, fnCtx) + ")." + (outIdx == 1 ? "y" : "x");
        case NT::BreakVector3:
            return "(" + input(n, 0, fnCtx) + ")." + (outIdx == 1 ? "y" : outIdx == 2 ? "z" : "x");
        case NT::BreakVector4:
            // hc::Vec4 wraps its components, so reach through .v.
            return "(" + input(n, 0, fnCtx) + ").v." +
                   (outIdx == 1 ? "y" : outIdx == 2 ? "z" : outIdx == 3 ? "w" : "x");
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
        case NT::ConstEnum:
        {
            Value v; v.type = PT::Enum; v.i = (int)n.f[0];
            return valueLit(v, m_tt, out);
        }
        case NT::EnumToInt: return "((int)(" + input(n, 0, fnCtx) + "))";
        case NT::IntToEnum: return "((" + cppScalar(out) + ")(" + input(n, 0, fnCtx) + "))";
        case NT::EnumToString:
        {
            // Entry names resolved at GENERATION time (validate() guaranteed the
            // def; packed defs are immutable, so this matches the interpreter's
            // runtime lookup). Unmatched values → "" like the interpreter.
            HE::EnumDef def;
            HE::TypeRegistry::instance().getEnum(n.typeName, def);
            const std::string et = cppScalar(dataInType(n, 0));
            std::string e = "([](" + et + " _ev) -> std::string { switch (_ev) {";
            // Nothing stops an author giving two entries the same value;
            // EnumDef::findValue answers with the FIRST, so later duplicates are
            // dead — and emitting them would be a duplicate `case` label, i.e. a
            // generated file that does not compile.
            std::unordered_set<int> seenValues;
            for (const auto& en : def.entries)
                if (seenValues.insert(en.value).second)
                {
                    const std::string lbl = m_tt.enumeratorOf(n.typeName, en.value);
                    e += " case " + (lbl.empty() ? "(" + et + ")" + std::to_string(en.value) : lbl) +
                         ": return " + strLit(en.name) + ";";
                }
            e += " default: return std::string(); } })(" + input(n, 0, fnCtx) + ")";
            return e;
        }
        // ── user-defined structs: plain C++ aggregates (hcgen_types.h) ───────
        case NT::MakeStruct:
        {
            // The definition's field defaults, with the wired/authored pins
            // overwriting them — the interpreter builds exactly this, starting
            // from makeDefaultValue and only touching pins that carry a value.
            const StructType* st = m_tt.find(n.typeName);
            if (!st) return zeroLit(out);
            const Value defaults = HE::TypeRegistry::instance().makeDefaultValue(n.typeName);
            std::string e = st->cpp + "{";
            for (size_t i = 0; i < st->def.fields.size(); ++i)
            {
                const HE::StructField& f = st->def.fields[i];
                const TypeRef ft = trOf(f);
                e += (i ? ", " : " ");
                const int idx = fieldPinIndex(n, f.name);
                const bool authored = idx >= 0 &&
                    (dataLinkTo(n.id, pinRanges(n).dataIn0 + idx) || n.pinDefaults.count(idx));
                e += authored ? convertExpr(input(n, idx, fnCtx), dataInType(n, idx), ft)
                              : (i < defaults.items.size() ? valueLit(defaults.items[i], m_tt, ft)
                                                           : zeroLit(ft));
            }
            return e + (st->def.fields.empty() ? "}" : " }");
        }
        case NT::BreakStruct:
            return structFieldRead(n, outIdx, fnCtx, out);
        case NT::GetStructField:
            return structFieldRead(n, 0, fnCtx, out);
        case NT::SetStructField:
        {
            // A pure "copy with one field replaced". Evaluation order matters:
            // the struct input is read before the value input (§3.4).
            const StructType* st = m_tt.find(n.typeName);
            const int fi = st && !n.params.empty() ? defFieldIndex(*st, n.params[0].name) : -1;
            // §3.4: when the incoming Value is not a struct of THIS definition
            // the interpreter throws it away and starts from makeDefaultValue —
            // which is exactly what an UNWIRED Struct pin hits (its typeName is
            // empty). A wired one can only be the same definition (connect), so
            // that is the one case the copy below is a copy at all.
            const bool seeded = dataLinkTo(n.id, pinRanges(n).dataIn0 + 0) != nullptr;
            const std::string src =
                seeded ? input(n, 0, fnCtx)
                       : (st ? structLit(HE::TypeRegistry::instance().makeDefaultValue(n.typeName),
                                         m_tt, out)
                             : zeroLit(out));
            if (fi < 0) return src;   // field gone from the definition → unchanged copy
            const HE::StructField& f = st->def.fields[(size_t)fi];
            const TypeRef ft = trOf(f);
            return "([&]{ " + st->cpp + " s__ = " + src + "; s__." + structMember(*st, (size_t)fi) +
                   " = " + convertExpr(input(n, 1, fnCtx), dataInType(n, 1), ft) + "; return s__; }())";
        }
        default:
            // A node this emitter has no case for. This used to return zeroLit,
            // and that was the worst failure mode the generator had: the class
            // compiled, shipped, and the node evaluated to 0 — no error, no
            // report line, no fallback, and an editor that behaved differently
            // from the build. A wrong answer that looks like a right one.
            //
            // Falling back is strictly better in every direction: the class runs
            // interpreted, which is CORRECT (just slower), and it says so in
            // hc_report.txt and the export dialog with the node id. Verified free
            // — the whole fixture suite generates without reaching this.
            //
            // It also fires for a known node asked for a data-out it does not
            // emit. Same answer for the same reason: interpret rather than guess.
            throw FallbackError{ std::string("no C++ emitter for node type '") +
                                 HorizonCode::nodeDisplayName(n.type) +
                                 "' (data-out " + std::to_string(outIdx) + ")", n.id };
        }
    }

    // ── struct field plumbing ────────────────────────────────────────────────
    static const std::string& structMember(const StructType& st, size_t fieldIndex)
    { return st.members[fieldIndex]; }
    static int defFieldIndex(const StructType& st, const std::string& fieldName)
    {
        for (size_t i = 0; i < st.def.fields.size(); ++i)
            if (st.def.fields[i].name == fieldName) return (int)i;
        return -1;
    }
    // The data-in index of a MakeStruct pin mirroring `fieldName` (the node's
    // params are the mirror; syncTypeSignatures keeps them in definition order,
    // but resolve BY NAME anyway — that is what the interpreter does).
    static int fieldPinIndex(const Node& n, const std::string& fieldName)
    {
        for (size_t i = 0; i < n.params.size(); ++i)
            if (n.params[i].name == fieldName) return (int)i;
        return -1;
    }
    // BreakStruct / GetStructField: resolve the mirrored pin name against the
    // CURRENT definition, then read that member (§3.4 — never positional).
    std::string structFieldRead(const Node& n, int paramIdx, int fnCtx, const TypeRef& out)
    {
        const StructType* st = m_tt.find(n.typeName);
        if (!st || paramIdx < 0 || paramIdx >= (int)n.params.size()) return zeroLit(out);
        const int fi = defFieldIndex(*st, n.params[(size_t)paramIdx].name);
        if (fi < 0) return zeroLit(out);   // field gone from the definition → typed zero
        const HE::StructField& f = st->def.fields[(size_t)fi];
        return convertExpr("(" + input(n, 0, fnCtx) + ")." + structMember(*st, (size_t)fi),
                           trOf(f), out);
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
            // Branch/Sequence/ForEach/DoOnce/FlipFlop/Cast steer their own
            // exec-outs; Return and Delay (latent — resumes via resumeFrom) are
            // terminal. Mirrors Runner::runExecChain's list exactly.
            if (n->type == NT::Branch || n->type == NT::Sequence || n->type == NT::ForEach ||
                n->type == NT::ForEachSet || n->type == NT::ForEachMap ||
                n->type == NT::FunctionReturn || n->type == NT::Delay ||
                n->type == NT::DoOnce || n->type == NT::FlipFlop ||
                n->type == NT::SwitchOnEnum || n->type == NT::Cast)
                return;
            l = execLinkFrom(n->id, pinRanges(*n).execOut0);
        }
    }

    // ── the Cast node's two lowerings ────────────────────────────────────────
    // With OnFailure::Interpret the build is a per-asset hybrid: an interpreted
    // instance can turn up in the same run, so the cast has to ask the Context
    // seam, which lands on the very Runtime::instanceIsA the interpreter asks.
    // Anything cleverer would answer differently for an interpreted target and
    // break the parity harness rather than the build.
    //
    // With OnFailure::Stop every class is native by construction — there is no
    // interpreted instance to stay compatible with, so the compatibility layer
    // is what goes:
    //   • a HorizonCode class target lowers to hc::castClass — its own key and
    //     its chain, without the Runtime's map. NOT hc::as<T>: that is one exact
    //     address per class, and since classes derive from one another the
    //     target is often an ANCESTOR of what the reference holds.
    //   • an engine base class still walks the chain, but through
    //     resolveCompiled + baseClassKey() instead of the Runtime's map.
    // Both shapes yield the reference or 0, so the caller above is unchanged.
    // `ref` is the local the Object input was already read into — never the
    // input expression itself, which some shapes below spell twice.
    std::string castExpr(const Node& n, const std::string& ref)
    {
        if (m_opt.onFailure == OnFailure::Stop)
        {
            if (HorizonCode::findEngineClass(n.s))
                return "hc::castBase(m_ctx, " + ref + ", " + strLit(n.s) + ")";
            // A HorizonCode class target. hc::as would be a pointer compare —
            // but with class inheritance the target may be an ANCESTOR of what
            // the reference holds, and one exact address per class cannot say
            // that. castClass asks the instance's own key AND its chain, still
            // without the Runtime's map.
            if (m_ct.find(n.s))
                return "hc::castClass(m_ctx, " + ref + ", " + strLit(n.s) + ")";
            // A class this run did NOT compile (deleted asset, a widget, a
            // stale target string) — the seam still answers it correctly, so
            // fall back rather than fail the build.
        }
        return "hc::castRef(m_ctx, " + ref + ", " + strLit(n.s) + ")";
    }

    // The exec emission shared by CallExternal and EngineCall: pack the data-in
    // args into a std::vector<hc::Value>, make the call, then cache the returned
    // values into this node's RunState slots. The two differ only in where the
    // args start (CallExternal's data-in 0 is the Target) and in which hc:: entry
    // point is called. `makeCall` is invoked AFTER the arg pushes are emitted,
    // preserving §3.4's ordering (args evaluate before the Target read).
    void emitValueCall(const Node& n, int fnCtx, Body& b, int firstArgPin,
                       const std::function<std::string(const std::string&)>& makeCall)
    {
        const std::string av = "a" + std::to_string(n.id);
        const std::string rv = "r" + std::to_string(n.id);
        b.line("{");
        ++b.indent;
        b.line("std::vector<hc::Value> " + av + ";");
        if (!n.params.empty()) b.line(av + ".reserve(" + std::to_string(n.params.size()) + ");");
        for (size_t i = 0; i < n.params.size(); ++i)
        {
            const int pin = (int)i + firstArgPin;
            b.line(av + ".push_back(" +
                   toValueCall(input(n, pin, fnCtx), dataInType(n, pin), m_opt.namespaceName) + ");");
        }
        b.line("const std::vector<hc::Value> " + rv + " = " + makeCall(av) + ";");
        const auto it = m_slots.find(n.id);
        if (it != m_slots.end())
        {
            m_rsTouched = true;
            for (size_t k = 0; k < it->second.size(); ++k)
                b.line("rs." + it->second[k].field + " = " +
                       fromValueCall(rv, k, it->second[k].tr) + ";");
        }
        --b.indent;
        b.line("}");
    }

    void stmt(const Node& n, int fnCtx, Body& b)
    {
        const PinRanges r = pinRanges(n);
        ++m_stmtCount;
        if (m_guard) { b.line("HC_STEP(rs);"); m_rsTouched = true; }
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
        case NT::Cast:
        {
            // The Object input is read ONCE into a local: a pure Engine Call
            // feeding it would otherwise dispatch again for every place the
            // expression is spelled, and the interpreter evaluates it once.
            const std::string rv = "c" + std::to_string(n.id);
            b.line("{");
            ++b.indent;
            b.line("const uint32_t " + rv + " = " + input(n, 0, fnCtx) + ";");
            b.line(slotRef(n, 0) + " = " + castExpr(n, rv) + ";");
            b.line("if (" + slotRef(n, 0) + " != 0u)");
            b.line("{");
            ++b.indent; chain(n, r.execOut0 + 0, fnCtx, b); --b.indent;   // Success
            b.line("}");
            b.line("else");
            b.line("{");
            ++b.indent; chain(n, r.execOut0 + 1, fnCtx, b); --b.indent;   // Failure
            b.line("}");
            --b.indent;
            b.line("}");
            break;
        }
        case NT::ForEach:
        case NT::ForEachSet:
        case NT::ForEachMap:
        {
            // One loop shape for all three: read the container ONCE into a local
            // (the interpreter evaluates its input once too), then walk it by
            // index. A set's items and a map's key/value pairs ARE their
            // iteration order, so this walk IS the insertion order both backends
            // promise — nothing here sorts and nothing hashes.
            const bool isMap = n.type == NT::ForEachMap;
            const HorizonCode::ContainerKind ck =
                n.type == NT::ForEach ? HorizonCode::ContainerKind::Array
                : isMap               ? HorizonCode::ContainerKind::Map
                                      : HorizonCode::ContainerKind::Set;
            const std::string arr = "arr" + std::to_string(n.id);
            const std::string idx = "i" + std::to_string(n.id);
            const std::string span = n.type == NT::ForEach ? arr + ".size()"
                                   : isMap                 ? arr + ".keys.size()"
                                                           : arr + ".items.size()";
            b.line("{");
            ++b.indent;
            b.line("const " + cppType(ctrType(n, ck)) + " " + arr + " = " +
                   input(n, 0, fnCtx) + ";");
            b.line("if (++rs.depth <= hc::kMaxDepth)");
            b.line("{");
            ++b.indent;
            b.line("for (size_t " + idx + " = 0; " + idx + " < " + span + "; ++" + idx + ")");
            b.line("{");
            ++b.indent;
            if (isMap)
            {
                b.line(slotRef(n, 0) + " = " + arr + ".keys[" + idx + "];");
                b.line(slotRef(n, 1) + " = " + arr + ".values[" + idx + "];");
                b.line(slotRef(n, 2) + " = (int)" + idx + ";");
            }
            else
            {
                b.line(slotRef(n, 0) + " = " +
                       (n.type == NT::ForEach ? arr + "[" + idx + "]"
                                              : arr + ".items[" + idx + "]") + ";");
                b.line(slotRef(n, 1) + " = (int)" + idx + ";");
            }
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
            const TypeRef pin = nodeType(n);
            if (v && v->scope != 0)
            {
                if (v->scope != fnCtx)
                {
                    // §13.2: a local write outside its function is dropped.
                    warn("local '" + n.s + "' written outside its function — write dropped");
                    break;
                }
                b.line(m_localName.at(n.s) + " = " +
                       convertExpr(input(n, 0, fnCtx), pin,
                                   trOf(*v)) + ";");
            }
            else if (const VarRef iv = instanceVar(n.s); iv.decl)
                b.line(iv.member + " = " +
                       convertExpr(input(n, 0, fnCtx), pin,
                                   trOf(*iv.decl)) + ";");
            else
                // §3.4: Set on an undeclared name creates a store entry — routed
                // through the Context to the Runtime's (overflow) store.
                b.line("hc::setVariableCtx(m_ctx, " + strLit(n.s) + ", " +
                       toValueCall(input(n, 0, fnCtx), pin, m_opt.namespaceName) + ");");
            break;
        }
        case NT::SetProperty:
            b.line("hc::setProperty(m_ctx, " + std::to_string(n.elem) + ", " + strLit(n.s) + ", " +
                   toValueCall(input(n, 0, fnCtx), dataInType(n, 0), m_opt.namespaceName) + ");");
            break;
        case NT::ShowSelf: b.line("hc::showSelf(m_ctx);"); break;
        case NT::HideSelf: b.line("hc::hideSelf(m_ctx);"); break;
        case NT::CreateWidget:
            b.line(slotRef(n, 0) + " = hc::createWidget(m_ctx, " + strLit(n.s) + ");");
            break;
        case NT::ShowWidget:
            b.line("hc::showWidget(m_ctx, (int)(" + input(n, 0, fnCtx) + "));");
            break;
        case NT::HideWidget:
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
        {
            const TypeRef vt = dataInType(n, 1);
            const TargetInfo t = targetOf(n, 0);
            TypeRef have;
            std::string acc;
            if (t.self)      acc = selfVar(n.s, have);
            else if (t.cls)  acc = directVar(n.s, *t.cls, have);
            if (t.self && !acc.empty())
            {
                b.line(acc + "() = " + convertExpr(input(n, 1, fnCtx), vt, have) + ";");
                break;
            }
            if (acc.empty())
            {
                b.line("hc::setExternal(m_ctx, " + input(n, 0, fnCtx) + ", " + strLit(n.s) + ", " +
                       toValueCall(input(n, 1, fnCtx), vt, m_opt.namespaceName) + ");");
                break;
            }
            b.line("{");
            ++b.indent;
            if (!t.gi)
                b.line("const uint32_t t" + std::to_string(n.id) + " = " + input(n, 0, fnCtx) + ";");
            b.line("const " + cppType(vt) + " v" + std::to_string(n.id) + " = " + input(n, 1, fnCtx) + ";");
            b.line("if (" + t.cls->cpp + "* o__ = " + resolveExpr(t, "t" + std::to_string(n.id)) + ")");
            b.line("    o__->" + acc + "() = " +
                   convertExpr("v" + std::to_string(n.id), vt, have) + ";");
            b.line("else");
            b.line("    hc::setExternal(m_ctx, " +
                   (t.gi ? std::string("hc::gameInstance(m_ctx)") : "t" + std::to_string(n.id)) +
                   ", " + strLit(n.s) + ", " +
                   toValueCall("v" + std::to_string(n.id), vt, m_opt.namespaceName) + ");");
            --b.indent;
            b.line("}");
            break;
        }
        case NT::BindEvent:
            b.line("hc::bindEvent(m_ctx, " + input(n, 0, fnCtx) + ", " +
                   (m_evtConst.count(n.s) ? m_evtConst.at(n.s) : strLit(n.s)) + ");");
            break;
        case NT::EmitEvent:
            {
                const std::string ev = m_evtConst.count(n.s) ? m_evtConst.at(n.s) : strLit(n.s);
                if (n.hasArg)
                    b.line("hc::emitEvent(m_ctx, " + ev + ", " +
                           toValueCall(input(n, 0, fnCtx), dataInType(n, 0), m_opt.namespaceName) + ");");
                else
                    b.line("hc::emitEvent(m_ctx, " + ev + ", hc::Value{});");
            }
            break;
        case NT::CallExternal:
        {
            const TargetInfo t = targetOf(n, 0);
            const Node* entry = nullptr;
            std::string method;
            if (t.self)     method = selfFn(n, &entry);
            else if (t.cls) method = directFn(n, *t.cls, &entry);
            if (method.empty())
            {
                // §3.4: args (data-ins 1..) evaluate before the Target read (data-in 0).
                emitValueCall(n, fnCtx, b, /*firstArgPin=*/1, [&](const std::string& av)
                {
                    return "hc::callExternal(m_ctx, " + input(n, 0, fnCtx) + ", " +
                           strLit(n.s) + ", " + av + ")";
                });
                break;
            }
            // Direct call: the arguments are typed C++ values and the results
            // come back in typed out-params, so neither side needs a
            // std::vector<Value>. The public method makes a FRESH RunState,
            // which is what callExternal's own per-call Runner does (§3.1).
            const std::string id = std::to_string(n.id);
            b.line("{");
            ++b.indent;
            std::vector<std::string> argNames, resNames;
            for (size_t i = 0; i < n.params.size(); ++i)   // args first (§3.4)
            {
                const std::string a = "a" + id + "_" + std::to_string(i);
                const TypeRef tr = trOf(n.params[i]);
                b.line("const " + cppType(tr) + " " + a + " = " + input(n, (int)i + 1, fnCtx) + ";");
                argNames.push_back(a);
            }
            for (size_t i = 0; i < n.results.size(); ++i)
            {
                const std::string r = "r" + id + "_" + std::to_string(i);
                const TypeRef tr = trOf(n.results[i]);
                b.line(cppType(tr) + " " + r + " = " + zeroLit(tr) + ";");
                resNames.push_back(r);
            }
            std::string args;
            for (size_t i = 0; i < argNames.size(); ++i) args += (i ? ", " : "") + argNames[i];
            for (size_t i = 0; i < resNames.size(); ++i)
                args += (args.empty() ? "" : ", ") + resNames[i];
            if (t.self)
            {
                b.line(method + "(" + args + ");");
            }
            else
            {
                if (!t.gi) b.line("const uint32_t t" + id + " = " + input(n, 0, fnCtx) + ";");
                b.line("if (" + t.cls->cpp + "* o__ = " + resolveExpr(t, "t" + id) + ")");
                b.line("    o__->" + method + "(" + args + ");");
                b.line("else");
                b.line("{");
                ++b.indent;
                const std::string av = "a" + id;
                b.line("std::vector<hc::Value> " + av + ";");
                if (!argNames.empty()) b.line(av + ".reserve(" + std::to_string(argNames.size()) + ");");
                for (size_t i = 0; i < argNames.size(); ++i)
                    b.line(av + ".push_back(" +
                           toValueCall(argNames[i],
                                       trOf(n.params[i]), m_opt.namespaceName) + ");");
                b.line("const std::vector<hc::Value> rr" + id + " = hc::callExternal(m_ctx, " +
                       (t.gi ? std::string("hc::gameInstance(m_ctx)") : "t" + id) +
                       ", " + strLit(n.s) + ", " + av + ");");
                for (size_t i = 0; i < resNames.size(); ++i)
                    b.line(resNames[i] + " = " +
                           fromValueCall("rr" + id, i,
                                         trOf(n.results[i])) + ";");
                --b.indent;
                b.line("}");
            }
            if (const auto it = m_slots.find(n.id); it != m_slots.end())
            {
                m_rsTouched = true;
                for (size_t k = 0; k < it->second.size() && k < resNames.size(); ++k)
                    b.line("rs." + it->second[k].field + " = " + resNames[k] + ";");
            }
            --b.indent;
            b.line("}");
            break;
        }
        case NT::FunctionCall:
        {
            const Node* entry = fnEntryByName(n.s);
            if (!entry)
            {
                // Inherited: the base class has it, and since this class really
                // derives from it in C++, the call is a plain qualified call to
                // the base's own public method — no seam, no name lookup.
                if (emitInheritedCall(n, fnCtx, b)) break;
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
                const TypeRef tr = trOf(entry->params[i]);
                // The call node mirrors the entry (synced) — pin type == param type.
                b.line("const " + cppType(tr) + " " + a + " = " +
                       (i < n.params.size() ? input(n, (int)i, fnCtx) : zeroLit(tr)) + ";");
                argNames.push_back(a);
            }
            for (size_t i = 0; i < entry->results.size(); ++i)
            {
                const std::string rn = "r" + std::to_string(n.id) + "_" + std::to_string(i);
                const TypeRef tr = trOf(entry->results[i]);
                b.line(cppType(tr) + " " + rn + " = " + zeroLit(tr) + ";");   // typed default results
                resNames.push_back(rn);
            }
            std::string call = m_fnName.at(n.s) + "(rs";
            for (const auto& a : argNames) call += ", " + a;
            for (const auto& rn : resNames) call += ", " + rn;
            call += ");";
            b.line("if (++rs.depth <= hc::kMaxDepth) " + call + "   // depth guard (§3.6)");
            b.line("--rs.depth;");
            m_rsTouched = true;
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
                           convertExpr(input(n, (int)i, fnCtx), dataInType(n, (int)i),
                                       trOf(fn->results[i])) + ";");
            }
            // NO `return;`. Return has no exec-out, so the interpreter ends only
            // the chain it sits in (`runExecChain` finds no next link) — it does
            // NOT unwind the caller: Sequence still runs its second arm and
            // ForEach still runs its remaining iterations after one. Since
            // Branch/Sequence/ForEach arms are emitted INLINE in the same C++
            // function, a real `return` would leave all of them. Ending the
            // emitted chain here (Return is terminal in chain()) is the exact
            // mirror; a later Return simply overwrites the result slots, which
            // is what the interpreter's frame does too.
            break;
        }
        case NT::EngineCall:
            emitValueCall(n, fnCtx, b, /*firstArgPin=*/0, [&](const std::string& av)
            {
                return "hc::callApi(m_ctx, " + strLit(n.s) + ", " + av + ")";
            });
            break;
        case NT::Print:
            b.line("hc::print(" + input(n, 0, fnCtx) + ");");
            break;
        case NT::Delay:
            // Latent: schedule the continuation (resume_<id> via resumeFrom)
            // and stop the chain — like the interpreter's runExecChain break.
            b.line("hc::scheduleResume(m_ctx, " + std::to_string(resumeId(n.id)) + ", " +
                   input(n, 0, fnCtx) + ", " + input(n, 1, fnCtx) + ");");
            break;
        case NT::DoOnce:
            b.line("if (!" + stateMember(n.id) + ")");
            b.line("{");
            ++b.indent;
            b.line(stateMember(n.id) + " = true;");
            chain(n, r.execOut0 + 0, fnCtx, b);
            --b.indent;
            b.line("}");
            break;
        case NT::FlipFlop:
            b.line(stateMember(n.id) + " = !" + stateMember(n.id) + ";   // A first");
            b.line("if (" + stateMember(n.id) + ")");
            b.line("{");
            ++b.indent;
            chain(n, r.execOut0 + 0, fnCtx, b);   // A
            --b.indent;
            b.line("}");
            b.line("else");
            b.line("{");
            ++b.indent;
            chain(n, r.execOut0 + 1, fnCtx, b);   // B
            --b.indent;
            b.line("}");
            break;
        case NT::SwitchOnEnum:
        {
            // Route on the entry VALUES, resolved from the definition at
            // generation time by the mirrored entry NAMES (params) — the same
            // renumber-safe matching the interpreter does at runtime.
            HE::EnumDef def;
            HE::TypeRegistry::instance().getEnum(n.typeName, def);
            b.line("switch (" + input(n, 0, fnCtx) + ")");
            b.line("{");
            // Two entries may share a value; the interpreter routes through
            // findValue (the FIRST match), so only that entry's branch is
            // reachable — and emitting the others would be a duplicate `case`.
            std::unordered_set<int> seenValues;
            for (size_t i = 0; i < n.params.size(); ++i)
            {
                const HE::EnumEntry* e = def.findEntry(n.params[i].name);
                if (!e) continue;   // stale mirror entry: unreachable branch
                if (!def.findValue(e->value) ||
                    def.findValue(e->value)->name != n.params[i].name) continue;
                if (!seenValues.insert(e->value).second) continue;
                const std::string lbl = m_tt.enumeratorOf(n.typeName, e->value);
                b.line("case " + (lbl.empty()
                    ? "(" + cppScalar(dataInType(n, 0)) + ")" + std::to_string(e->value) : lbl) + ":");
                b.line("{");
                ++b.indent;
                chain(n, r.execOut0 + (int)i, fnCtx, b);
                b.line("break;");
                --b.indent;
                b.line("}");
            }
            b.line("default:");
            b.line("{");
            ++b.indent;
            chain(n, r.execOut0 + (int)n.params.size(), fnCtx, b);
            b.line("break;");
            --b.indent;
            b.line("}");
            b.line("}");
            break;
        }
        default:
            // Event/FunctionEntry never appear mid-chain; anything else is a
            // pure node that can't be exec-wired.
            break;
        }
    }

    // ── class emission ────────────────────────────────────────────────────────
    TypeRef varType(const Variable& v) const { return trOf(v); }

    std::string memberDefault(const Variable& v) const
    { return valueLit(HorizonCode::variableDefaultValue(v), m_tt, varType(v)); }

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

    // The Input Action nodes' entry points, one per exec-out: the event name the
    // host fires, the node, and which chain that name enters. Kept apart from
    // eventGroups because the two differ in exactly that last field — an Event
    // node has one chain and an Input Action has one PER NAME.
    struct InputEntry { std::string event; const Node* node; int chain; };
    std::vector<InputEntry> inputEntries() const
    {
        std::vector<InputEntry> out;
        for (const Node& n : m_g.nodes)
        {
            if (n.type != NT::InputAction) continue;
            const std::vector<std::string> names = HorizonCode::inputActionEventNames(n);
            for (size_t k = 0; k < names.size(); ++k) out.push_back({ names[k], &n, (int)k });
        }
        return out;
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

    // Parameter list of a generated function body: the RunState only when the
    // class has one, results by reference (the callee's out-params, §3.1).
    std::string fnParamList(const Node& fn, bool rsUsed) const
    {
        std::string sig = rsParam(rsUsed);
        auto add = [&sig](const std::string& p) { if (!sig.empty()) sig += ", "; sig += p; };
        for (size_t i = 0; i < fn.params.size(); ++i)
            add(cppType(trOf(fn.params[i])) +
                " p_" + std::to_string(i));
        for (size_t i = 0; i < fn.results.size(); ++i)
            add(cppType(trOf(fn.results[i])) +
                "& r_" + std::to_string(i));
        return sig;
    }

    std::vector<std::pair<std::string, std::string>> sortedEvtConsts() const
    {
        std::vector<std::pair<std::string, std::string>> v(m_evtConst.begin(), m_evtConst.end());
        std::sort(v.begin(), v.end());
        return v;
    }

    std::vector<std::pair<int, std::string>> sortedPublicEv() const
    {
        std::vector<std::pair<int, std::string>> evs(m_publicEv.begin(), m_publicEv.end());
        std::sort(evs.begin(), evs.end());
        return evs;
    }

    std::vector<std::pair<int, std::string>> sortedEvNames() const
    {
        std::vector<std::pair<int, std::string>> evs(m_evName.begin(), m_evName.end());
        std::sort(evs.begin(), evs.end());
        return evs;
    }

    // One translated body: an event handler, a graph function, or a Delay
    // continuation. `fn` is set for graph functions (their parameter list is
    // rendered later, once it is known whether a RunState exists at all).
    struct Fragment
    {
        std::string name;
        const Node* fn      = nullptr;
        std::string prelude;   // function locals
        std::string body;
        bool        rsUsed  = false;
    };

    std::vector<Fragment> translateBodies()
    {
        m_stmtCount = 0;
        std::vector<Fragment> out;
        // `outPin` is the exec-out to enter from, counted off execOut0 — 0 for
        // everything with one chain, and 0/1 for an Input Action's Pressed and
        // Released. It is the first ENTRY node with more than one chain, which
        // is why this took a parameter it never needed before.
        auto translate = [this, &out](const std::string& name, const Node* start,
                                      const Node* fn, int fnCtx, int outPin = 0)
        {
            Fragment f;
            f.name = name;
            f.fn   = fn;
            if (fn)
                for (const auto& v : m_g.variables)
                    if (v.scope == fn->id)
                        f.prelude += "        " + cppType(varType(v)) + " " + m_localName.at(v.name) +
                                     " = " + memberDefault(v) + ";   // local, per invocation\n";
            Body b;
            b.indent = 2;   // inside a member function inside the class
            m_rsTouched = false;
            if (start) chain(*start, pinRanges(*start).execOut0 + outPin, fnCtx, b);
            f.body   = std::move(b.text);
            f.rsUsed = m_rsTouched;
            out.push_back(std::move(f));
        };

        for (const Node& n : m_g.nodes)
            if (n.type == NT::Event) translate(m_evName.at(n.id), &n, nullptr, 0);
        for (const Node& n : m_g.nodes)
            if (n.type == NT::InputAction && m_iaName.count(n.id))
                for (size_t k = 0; k < m_iaName.at(n.id).size(); ++k)
                    translate(m_iaName.at(n.id)[k], &n, nullptr, 0, (int)k);
        for (const Node* fn : functionEntries())
            translate(m_fnName.at(fn->s), fn, fn, fn->id);
        // Delay continuations (Runner::resumeFrom's mirror): a FRESH run.
        for (const int id : delayNodeIds())
            translate("resume_" + std::to_string(id), m_g.findNode(id), nullptr, 0);
        return out;
    }

    // The public, authored-name signature of a graph function: the same
    // parameters, named as the graph names them, results by reference.
    std::string publicFnParams(const Node& fn) const
    {
        std::string sig;
        std::unordered_set<std::string> used;
        auto add = [&](const std::string& type, const std::string& name)
        { if (!sig.empty()) sig += ", "; sig += type + " " + uniqueName(cppIdent(name), used); };
        for (size_t i = 0; i < fn.params.size(); ++i)
            add(cppType(trOf(fn.params[i])),
                fn.params[i].name.empty() ? "p" + std::to_string(i) : fn.params[i].name);
        for (size_t i = 0; i < fn.results.size(); ++i)
            add(cppType(trOf(fn.results[i])) + "&",
                fn.results[i].name.empty() ? "r" + std::to_string(i) : fn.results[i].name);
        return sig;
    }
    // Its forwarding body: names in declaration order, straight into the
    // internal body function.
    std::string publicFnForward(const Node& fn) const
    {
        std::vector<std::string> names;
        std::unordered_set<std::string> used;
        for (size_t i = 0; i < fn.params.size(); ++i)
            names.push_back(uniqueName(cppIdent(fn.params[i].name.empty()
                ? "p" + std::to_string(i) : fn.params[i].name), used));
        for (size_t i = 0; i < fn.results.size(); ++i)
            names.push_back(uniqueName(cppIdent(fn.results[i].name.empty()
                ? "r" + std::to_string(i) : fn.results[i].name), used));
        std::string call = m_fnName.at(fn.s) + "(" + rsArg();
        for (size_t i = 0; i < names.size(); ++i)
            call += (i || useRunState() ? ", " : "") + names[i];
        return call + ");";
    }

    // A Call Function whose entry lives in a BASE class. Returns false when the
    // name is not inherited at all (the caller then keeps the no-op).
    //
    // Only PUBLIC base functions are reachable, which is not a shortcut but the
    // rule: Runtime::callFunction refuses a private member across a class
    // boundary, so a private one has to stay the no-op the interpreter performs.
    bool emitInheritedCall(const Node& n, int fnCtx, Body& b)
    {
        const CompiledClass* from = inheritedFnFrom(n.s);
        if (!from) return false;
        const Node* entry = fnEntryIn(*from, n.s);
        if (!entry) return false;
        if (entry->access != 0)
        {
            warn("call to '" + n.s + "' is private to the base class '" + from->key +
                 "' — lowered to a no-op, as the interpreter does");
            return true;   // handled: the no-op IS the answer
        }
        const auto pf = from->publicFn.find(n.s);
        if (pf == from->publicFn.end()) return false;   // the base did not expose it

        b.line("{");
        ++b.indent;
        std::vector<std::string> argNames, resNames;
        for (size_t i = 0; i < entry->params.size(); ++i)
        {
            const std::string a = "a" + std::to_string(n.id) + "_" + std::to_string(i);
            const TypeRef tr = trOf(entry->params[i].type, entry->params[i].isArray,
                                    entry->params[i].typeName);
            b.line("const " + cppType(tr) + " " + a + " = " +
                   (i < n.params.size() ? input(n, (int)i, fnCtx) : zeroLit(tr)) + ";");
            argNames.push_back(a);
        }
        for (size_t i = 0; i < entry->results.size(); ++i)
        {
            const std::string r = "r" + std::to_string(n.id) + "_" + std::to_string(i);
            const TypeRef tr = trOf(entry->results[i].type, entry->results[i].isArray,
                                    entry->results[i].typeName);
            b.line(cppType(tr) + " " + r + " = " + zeroLit(tr) + ";");
            resNames.push_back(r);
        }
        // The base's public method takes params then results by reference, and
        // opens its OWN RunState — the depth guard is still ours, because the
        // recursion this counts is the graph's, not C++'s.
        std::string call = from->cpp + "::" + pf->second + "(";
        for (size_t i = 0; i < argNames.size(); ++i) call += (i ? ", " : "") + argNames[i];
        for (size_t i = 0; i < resNames.size(); ++i)
            call += (i || !argNames.empty() ? ", " : "") + resNames[i];
        call += ");";
        b.line("if (++rs.depth <= hc::kMaxDepth) " + call + "   // depth guard (§3.6)");
        b.line("--rs.depth;");
        m_rsTouched = true;
        const auto it = m_slots.find(n.id);
        if (it != m_slots.end())
            for (size_t k = 0; k < it->second.size() && k < resNames.size(); ++k)
                b.line("rs." + it->second[k].field + " = " + resNames[k] + ";");
        --b.indent;
        b.line("}");
        return true;
    }

    void emitClassFiles(std::string& h, std::string& c)
    {
        const std::string ns = m_opt.namespaceName;

        // Bodies FIRST: translating them is what reveals whether this class ever
        // reads the event argument, and that decides whether a RunState exists —
        // and with it the `rs` parameter every body would otherwise carry.
        const size_t warningsBefore = m_warnings.size();
        std::vector<Fragment> frags = translateBodies();
        // The "no ForEach/FunctionCall ⇒ the step abort is unreachable" argument
        // rests on the emitted statement count staying under the limit. A graph
        // whose branches fan out far enough to break that assumption gets the
        // guard back — measured, not assumed.
        if (!m_guard && m_stmtCount > hc::kMaxSteps)
        {
            m_guard = true;
            m_warnings.resize(warningsBefore);   // the second pass re-warns
            frags = translateBodies();
        }

        const bool rsOn    = useRunState();
        const auto delays  = delayNodeIds();
        const auto states  = stateNodeIds();
        const auto groups  = eventGroups();
        const auto inputs  = inputEntries();
        // Both are dispatched by fireEvent and both belong in eventInfos, so
        // every "does this class handle events at all" test asks about both.
        const bool anyEvents = !groups.empty() || !inputs.empty();
        const auto fns     = functionEntries();
        bool anyElemFilter = false, anyPrivateFn = false, anyFnParams = false, anyRefVar = false;
        for (const Node& n : m_g.nodes) if (n.type == NT::Event && n.elem != 0) anyElemFilter = true;
        for (const Node* fn : fns)
        {
            if (fn->access != 0)      anyPrivateFn = true;
            if (!fn->params.empty())  anyFnParams  = true;
        }
        size_t varCount = 0;
        for (const auto& v : m_g.variables)
        {
            if (v.scope != 0) continue;
            ++varCount;
            if (v.type == PT::Ref) anyRefVar = true;
        }
        // ── what inheritance changes about the frame ────────────────────────
        // The name-keyed entry points (varInfos, getVariable, setVariable,
        // collectRefs) all read slots(), and slots() is a STATIC function — the
        // base's copy would answer with the base's table and lose everything
        // this class declares. So a derived class re-emits them whenever the
        // CHAIN has variables, even when it declares none itself.
        const CompiledClass* par = parent();
        size_t chainVarCount = varCount;
        bool   chainRefVar   = anyRefVar;
        for (const CompiledClass* a = par; a; a = a->parentKey.empty() ? nullptr
                                                                      : m_ct.find(a->parentKey))
        {
            if (!a->graph) continue;
            for (const Variable& v : a->graph->variables)
            {
                if (v.scope != 0) continue;
                ++chainVarCount;
                if (v.type == PT::Ref) chainRefVar = true;
            }
        }
        // slots() is a GENERATED static, not a virtual on CompiledInstance — so
        // unlike fireEvent/callFunction/reseedVariables, a qualified call to a
        // base's slots() only resolves if that base actually emitted one. A base
        // whose whole chain declares no variable emits none (a class of nothing
        // but events and functions is an ordinary shape), and concatenating with
        // it would produce a .cpp that fails to compile at EXPORT time, in front
        // of the user. So the concat only exists when there is something above.
        const size_t parentChainVars = chainVarCount - varCount;
        const bool   inheritsVars    = par && parentChainVars > 0;
        const bool   ownTable        = inheritsVars && varCount > 0;   // ownSlots() beside slots()
        // A parameter the DEFINITION never touches loses its name (the
        // declaration keeps it) — no `(void)x;` noise, and the header still
        // documents what the argument is.
        auto p = [](const char* type, const char* name, bool used)
        { return used ? std::string(type) + " " + name : std::string(type); };

        // ── the header: declarations, and the members' declared defaults ─────
        h += "// GENERATED by HorizonCode → C++ codegen — do not edit.\n";
        h += "// Source: " + m_src.label + " (key " + m_src.key + ")\n";
        h += "#pragma once\n";
        h += "#include <HorizonCode/HorizonCodeCompiled.h>\n";
        h += "#include <HorizonCode/HorizonCodeGenSupport.h>\n";
        {
            // Only the definitions this graph names, each from its own header —
            // never the hcgen_types.h umbrella, which would make every type
            // asset a dependency of every class. Nested definitions arrive
            // through their owner's header. Sorted, because the include list is
            // part of the file's bytes and those decide whether it recompiles.
            std::unordered_set<std::string> used;
            collectUserTypePaths(m_g, PT::Struct, used);
            collectUserTypePaths(m_g, PT::Enum, used);
            std::set<std::string> headers;
            for (const std::string& path : used)
            {
                // An unknown definition has no header (the class falls back).
                const std::string cpp = m_tt.cppOf(path);
                if (!cpp.empty()) headers.insert(typeHeaderName(cpp));
            }
            for (const std::string& inc : headers) h += "#include \"" + inc + "\"\n";
        }
        // The base class is a real C++ base, so its declaration has to be here —
        // in the HEADER, not just the .cpp: anyone including this one gets an
        // incomplete type otherwise.
        if (par) h += "#include \"hcgen_" + par->cpp + ".h\"\n";
        h += "\nnamespace " + ns + " {\n\n";
        // `final` only when nothing in this run derives from it. It is worth
        // keeping where it applies (the devirtualization it allows is the reason
        // every generated class had it), and it must go where it does not.
        const std::string fin = m_ct.isBase.count(m_src.key) ? "" : " final";
        h += "class " + m_cls + fin + " : public " +
             (par ? par->cpp : std::string("HorizonCode::CompiledInstance")) + "\n{\npublic:\n";

        if (!m_publicEv.empty() || !m_publicFn.empty())
        {
            h += "    // This graph's own events and functions — call these straight from C++.\n";
            for (const auto& [id, name] : sortedPublicEv())
            {
                const MethodDecl d = methodDecl(name, {});
                h += "    " + d.prefix + "void " + name + "()" + d.suffix + ";\n";
            }
            for (const Node* fn : fns)
                if (m_publicFn.count(fn->s))
                {
                    const std::string params = publicFnParams(*fn);
                    const MethodDecl d = methodDecl(m_publicFn.at(fn->s), params);
                    h += "    " + d.prefix + "void " + m_publicFn.at(fn->s) + "(" + params + ")" +
                         d.suffix + ";\n";
                }
            h += "\n";
        }
        {
            // One override per engine event this graph handles. The ones it does
            // not handle need nothing: CompiledInstance's empty default is the
            // "leave it reachable but silent" case.
            bool any = false;
            for (const auto& [name, nodes] : groups)
            {
                const EngineEvent* e = engineEventFor(name);
                if (!e) continue;
                if (!any)
                {
                    h += "    // The engine events this graph handles (the rest keep the\n";
                    h += "    // empty default from CompiledInstance).\n";
                    any = true;
                }
                h += "    void " + std::string(e->hook) + "(" + hookParams(*e, true) + ") override;\n";
            }
            if (any) h += "\n";
        }
        if (!m_publicVar.empty())
        {
            h += "    // Its public variables, by reference — read and write them directly.\n";
            for (const auto& v : m_g.variables)
                if (v.scope == 0 && v.access == 0)
                    h += "    " + cppType(varType(v)) + "& " + m_publicVar.at(v.name) + "();\n";
            h += "\n";
        }

        h += "    const char* classKey() const override;\n";
        // Only when the class actually has one: the base's default already
        // returns "" (= Object), so emitting an override that repeats it would
        // be noise in every generated widget and level script header.
        if (!m_src.baseClass.empty())
            h += "    const char* baseClassKey() const override;\n";
        if (!m_src.chain.empty())
            h += "    const std::vector<const char*>& classChain() const override;\n";
        h += "    // Identity for hc::as — see CompiledInstance::classTag.\n";
        h += "    static const void* classTag_();\n";
        h += "    const void* classTag() const override;\n";
        if (chainVarCount)
        {
            h += "    // The declared variables as ONE table: the Runtime's name-based entry\n";
            h += "    // points below all read it instead of carrying a branch per variable.\n";
            if (ownTable)
            {
                h += "    // ownSlots() is what THIS class declares; slots() is the whole chain,\n";
                h += "    // so a name-based read finds an inherited variable too.\n";
                h += "    static const hc::VarSlots& ownSlots();\n";
            }
            h += "    static const hc::VarSlots& slots();\n";
            h += "    const std::vector<HorizonCode::CompiledVarInfo>& varInfos() const override;\n";
        }
        if (anyEvents)
        {
            h += "    const std::vector<HorizonCode::CompiledEventInfo>& eventInfos() const override;\n";
            h += "    void fireEvent(const std::string& name, int elem, const hc::Value& arg) override;\n";
        }
        if (!fns.empty())
        {
            h += "    bool callFunction(const std::string& name, bool requirePublic,\n";
            h += "                      const std::vector<hc::Value>& args,\n";
            h += "                      std::vector<hc::Value>* results) override;\n";
        }
        if (chainVarCount)
        {
            h += "    hc::Value getVariable(const std::string& name) const override;\n";
            h += "    bool setVariable(const std::string& name, const hc::Value& v) override;\n";
            if (chainRefVar) h += "    void collectRefs(std::vector<uint32_t>& out) const override;\n";
        }
        if (varCount || !states.empty()) h += "    void reseedVariables() override;\n";
        if (!delays.empty())             h += "    void resumeFrom(int nodeId) override;\n";

        // protected, not private: a derived class's generated code reads and
        // writes the inherited members directly — that is what keeps ONE C++
        // member per graph variable, matching the interpreter's one store.
        h += "\nprotected:\n";
        if (rsOn)
        {
            // Per-run state: mirrors the Runner exactly (§3.3/§5.4) — and only the
            // parts this class can actually reach (see m_guard/useRunState).
            h += "    struct RunState\n    {\n";
            if (m_guard)
            {
                h += "        int steps = 0, depth = 0;\n";
                h += "        bool aborted = false;\n";
            }
            if (m_usesEventArg) h += "        hc::Value eventArg;\n";
            std::vector<int> slotIds;
            for (const auto& [id, _] : m_slots) slotIds.push_back(id);
            std::sort(slotIds.begin(), slotIds.end());
            for (const int id : slotIds)
                for (const auto& s : m_slots.at(id))
                    h += "        " + cppType(s.tr) + " " + s.field + " = " + zeroLit(s.tr) + ";\n";
            h += "    };\n\n";
        }
        for (const Fragment& f : frags)
            h += "    void " + f.name + "(" +
                 (f.fn ? fnParamList(*f.fn, true) : rsParam(true)) + ");\n";
        if (!frags.empty()) h += "\n";
        for (const auto& v : m_g.variables)
            if (v.scope == 0)
                h += "    " + cppType(varType(v)) + " " + m_varMember.at(v.name) +
                     " = " + memberDefault(v) + ";\n";
        // Per-instance node state (DoOnce/FlipFlop) — persists like variables,
        // reset by reseedVariables, never part of the public surface.
        for (const int id : states)
            h += "    bool " + stateMember(id) + " = false;\n";
        h += "};\n\n} // namespace " + ns + "\n";

        // ── the implementation ──────────────────────────────────────────────
        c += "// GENERATED by HorizonCode → C++ codegen — do not edit.\n";
        c += "// Source: " + m_src.label + " (key " + m_src.key + ")\n";
        // Exactly what this file needs, never the library umbrella (hcgen.h):
        // including that would make every class header a dependency of every
        // translation unit, so editing one graph would rebuild all of them.
        // hcgen.h stays for YOUR code, which wants the whole surface at once.
        c += "#include \"hcgen_" + m_cls + ".h\"\n";
        for (const std::string& other : m_usedClasses)
            if (other != m_cls) c += "#include \"hcgen_" + other + ".h\"\n";
        c += "\nnamespace " + ns + " {\n\n";

        for (const auto& [id, name] : sortedPublicEv())
            c += "void " + m_cls + "::" + name + "() { " + (rsOn ? "RunState rs; " : "") +
                 m_evName.at(id) + "(" + rsArg() + "); }\n";
        for (const Node* fn : fns)
            if (m_publicFn.count(fn->s))
                c += "void " + m_cls + "::" + m_publicFn.at(fn->s) + "(" + publicFnParams(*fn) +
                     ") { " + (rsOn ? "RunState rs; " : "") + publicFnForward(*fn) + " }\n";
        if (!m_publicEv.empty() || !m_publicFn.empty()) c += "\n";

        for (const auto& v : m_g.variables)
            if (v.scope == 0 && v.access == 0)
                c += cppType(varType(v)) + "& " + m_cls + "::" + m_publicVar.at(v.name) +
                     "() { return " + m_varMember.at(v.name) + "; }\n";
        if (!m_publicVar.empty()) c += "\n";

        // The interned ids this class names. One constant per event, so the code
        // below reads the event, not a number — the id is assigned at load and
        // is nobody's business but the Runtime's.
        if (!m_evtConst.empty())
        {
            c += "// Event ids, interned once on first use:\n";
            for (const auto& [name, konst] : sortedEvtConsts())
                c += "//   " + konst + " = " + strLit(name) + "\n";
            for (const auto& [name, konst] : sortedEvtConsts())
                c += "static const hc::EventId " + konst + " = hc::eventId(" + strLit(name) + ");\n";
            c += "\n";
        }

        c += "const char* " + m_cls + "::classKey() const { return " + strLit(m_src.key) + "; }\n";
        if (!m_src.baseClass.empty())
            c += "const char* " + m_cls + "::baseClassKey() const { return " +
                 strLit(m_src.baseClass) + "; }\n";
        if (!m_src.chain.empty())
        {
            c += "const std::vector<const char*>& " + m_cls + "::classChain() const\n{\n";
            c += "    static const std::vector<const char*> k = {";
            for (size_t i = 0; i < m_src.chain.size(); ++i)
                c += (i ? ", " : " ") + strLit(m_src.chain[i]);
            c += " };\n    return k;\n}\n";
        }
        c += "const void* " + m_cls + "::classTag_() { static const char k = 0; return &k; }\n";
        c += "const void* " + m_cls + "::classTag() const { return classTag_(); }\n\n";

        if (chainVarCount)
        {
            if (varCount)
            {
                c += "const hc::VarSlots& " + m_cls + "::" +
                     (ownTable ? "ownSlots" : "slots") + "()\n{\n";
                c += "    static const hc::VarSlots k = {\n";
                for (const auto& v : m_g.variables)
                {
                    if (v.scope != 0) continue;
                    const TypeRef tr = varType(v);
                    c += "        hc::slot<&" + m_cls + "::" + m_varMember.at(v.name) + ">(" +
                         strLit(v.name) + ", hc::PinType::" + pinName(v.type) + ", " +
                         (v.isArray ? "true" : "false") + ", " + std::to_string(v.access) + ", " +
                         strLit(v.typeName) + ", " +
                         toValueCall(memberDefault(v), tr, ns) +
                         // The container kind and (for a map) the key type ride
                         // along: the GC reads keyType to reach objects held only
                         // as map KEYS, which `type` — the value side — never
                         // mentions. Written only when it is not the default, so
                         // scalar and array slots emit exactly as before.
                         (tr.kind() == HorizonCode::ContainerKind::Set ||
                          tr.kind() == HorizonCode::ContainerKind::Map
                              ? ", hc::ContainerKind::" +
                                std::string(tr.kind() == HorizonCode::ContainerKind::Set ? "Set" : "Map") +
                                ", hc::PinType::" + pinName(v.keyType)
                              : std::string()) +
                         "),\n";
                }
                c += "    };\n    return k;\n}\n\n";
            }
            if (inheritsVars)
            {
                // The base's table plus this class's. The base's entries keep
                // accessors typed on the BASE — correct for an object of this
                // class, since the base sub-object is right there.
                c += "const hc::VarSlots& " + m_cls + "::slots()\n";
                c += "{ static const hc::VarSlots k = hc::concatSlots(" + par->cpp + "::slots()" +
                     (ownTable ? ", ownSlots()" : ", {}") + "); return k; }\n\n";
            }
            c += "const std::vector<HorizonCode::CompiledVarInfo>& " + m_cls + "::varInfos() const\n";
            c += "{ static const std::vector<HorizonCode::CompiledVarInfo> k = hc::varInfosOf(slots()); return k; }\n\n";
        }

        if (anyEvents)
        {
            c += "const std::vector<HorizonCode::CompiledEventInfo>& " + m_cls + "::eventInfos() const\n{\n";
            c += "    static const std::vector<HorizonCode::CompiledEventInfo> kEvents = {\n";
            for (const Node& n : m_g.nodes)
                if (n.type == NT::Event)
                    c += "        { " + strLit(n.s) + ", " + std::to_string(n.elem) + " },\n";
            // One row per input event this class handles — same reason the
            // inherited ones are listed below: this table is what says the
            // instance cares, and a handler missing from it is never fired.
            for (const InputEntry& e : inputs)
                c += "        { " + strLit(e.event) + ", 0 },\n";
            c += "    };\n";
            if (par)
            {
                // Plus the base's. Runtime::eventBindingsOf reports this list as
                // everything the instance handles — that is what a Bind Event
                // subscribes against — so leaving the inherited handlers out
                // would make them unreachable: the handler exists, and nobody
                // ever fires it.
                c += "    static const std::vector<HorizonCode::CompiledEventInfo> kAll = [this]\n";
                c += "    {\n";
                c += "        std::vector<HorizonCode::CompiledEventInfo> v = " + par->cpp +
                     "::eventInfos();\n";
                c += "        v.insert(v.end(), kEvents.begin(), kEvents.end());\n";
                c += "        return v;\n";
                c += "    }();\n";
                c += "    return kAll;\n}\n\n";
            }
            else
                c += "    return kEvents;\n}\n\n";

            // fireEvent: fresh RunState per fire == the per-run cache clear (§3.1);
            // every matching handler of one fire shares it, in graph order.
            // With a base class the parameters always keep their names: whatever
            // this class does not handle is passed on, so `elem` and `arg` are
            // used even when none of its own handlers reads them.
            c += "void " + m_cls + "::fireEvent(const std::string& name, " +
                 p("int", "elem", anyElemFilter || par != nullptr) + ", " +
                 p("const hc::Value&", "arg", m_usesEventArg || par != nullptr) + ")\n{\n";
            if (rsOn) c += "    RunState rs;\n";
            if (m_usesEventArg) c += "    rs.eventArg = arg;\n";
            bool first = true;
            for (const auto& [name, nodes] : groups)
            {
                c += std::string("    ") + (first ? "if" : "else if") + " (name == " +
                     strLit(name) + ")\n    {\n";
                for (const Node* n : nodes)
                {
                    const std::string call = m_evName.at(n->id) + "(" + rsArg() + ");";
                    if (n->elem == 0) c += "        " + call + "\n";
                    else c += "        if (elem == " + std::to_string(n->elem) + ") " + call + "\n";
                }
                c += "    }\n";
                first = false;
            }
            // The input entries, in the same if/else chain: each name enters ONE
            // chain of its node, which is the whole difference from an Event.
            for (const InputEntry& e : inputs)
            {
                c += std::string("    ") + (first ? "if" : "else if") + " (name == " +
                     strLit(e.event) + ")\n    {\n";
                c += "        " + m_iaName.at(e.node->id)[(size_t)e.chain] +
                     "(" + rsArg() + ");\n";
                c += "    }\n";
                first = false;
            }
            if (par)
            {
                // Not handled here → the base class. Leaf first, stopping at the
                // first class that declares the handler: an override REPLACES
                // the base's version, exactly as Runtime::runEventOnLevel
                // resolves it for an interpreted instance.
                c += std::string("    ") + (first ? "" : "else ") + "{ " + par->cpp +
                     "::fireEvent(name, elem, arg); }\n";
            }
            c += "}\n\n";
        }

        // The engine-event overrides. Same bodies fireEvent reaches, entered
        // without a name: one fresh RunState per call, the typed argument boxed
        // straight into it, and the element filter applied exactly as fireEvent
        // applies it (a handler on element 0 answers for every element).
        for (const auto& [name, nodes] : groups)
        {
            const EngineEvent* e = engineEventFor(name);
            if (!e) continue;
            std::vector<const Node*> reachable;
            for (const Node* n : nodes)
                if (e->elem || n->elem == 0) reachable.push_back(n);   // no elem ⇒ only 0 matches
            if (reachable.empty()) continue;
            const bool usesArg = m_usesEventArg && e->arg != PT::Exec;
            const bool usesElem = e->elem &&
                std::any_of(reachable.begin(), reachable.end(),
                            [](const Node* n) { return n->elem != 0; });
            std::string params;
            if (e->elem) params = usesElem ? "int elem" : "int";
            if (e->arg != PT::Exec)
            {
                const char* t = e->arg == PT::String ? "const std::string&"
                              : e->arg == PT::Float  ? "float"
                              : e->arg == PT::Bool   ? "bool" : "int";
                if (!params.empty()) params += ", ";
                params += std::string(t) + (usesArg ? " arg" : "");
            }
            c += "void " + m_cls + "::" + e->hook + "(" + params + ")\n{\n";
            if (rsOn) c += "    RunState rs;\n";
            if (usesArg) c += "    rs.eventArg = hc::toValue(arg);\n";
            for (const Node* n : reachable)
            {
                const std::string call = m_evName.at(n->id) + "(" + rsArg() + ");";
                if (n->elem == 0) c += "    " + call + "\n";
                else c += "    if (elem == " + std::to_string(n->elem) + ") " + call + "\n";
            }
            c += "}\n\n";
        }

        // callFunction: coerced args, typed default results (§3.1).
        if (!fns.empty())
        {
            c += "bool " + m_cls + "::callFunction(const std::string& name, " +
                 p("bool", "requirePublic", anyPrivateFn) + ",\n                  " +
                 p("const std::vector<hc::Value>&", "args", anyFnParams) + ", " +
                 "std::vector<hc::Value>* results)\n{\n";
            for (const Node* fn : fns)
            {
                c += "    if (name == " + strLit(fn->s) + ")\n    {\n";
                if (fn->access != 0)
                    c += "        if (requirePublic) return false;   // private function\n";
                if (rsOn) c += "        RunState rs;\n";
                for (size_t i = 0; i < fn->params.size(); ++i)
                {
                    const TypeRef tr = trOf(fn->params[i]);
                    c += "        " + cppType(tr) + " p" + std::to_string(i) + " = " +
                         coerceCall("hc::arg(args, " + std::to_string(i) + ")", tr) + ";\n";
                }
                std::vector<TypeRef> resTypes;
                for (size_t i = 0; i < fn->results.size(); ++i)
                {
                    resTypes.push_back(trOf(fn->results[i]));
                    c += "        " + cppType(resTypes[i]) + " r" + std::to_string(i) + " = " +
                         zeroLit(resTypes[i]) + ";\n";
                }
                std::vector<std::string> callArgs;
                if (rsOn) callArgs.push_back("rs");
                for (size_t i = 0; i < fn->params.size(); ++i)  callArgs.push_back("p" + std::to_string(i));
                for (size_t i = 0; i < fn->results.size(); ++i) callArgs.push_back("r" + std::to_string(i));
                c += "        " + m_fnName.at(fn->s) + "(";
                for (size_t i = 0; i < callArgs.size(); ++i) c += (i ? ", " : "") + callArgs[i];
                c += ");\n";
                c += "        if (results)\n        {\n            results->clear();\n";
                for (size_t i = 0; i < fn->results.size(); ++i)
                    c += "            results->push_back(" +
                         toValueCall("r" + std::to_string(i), resTypes[i], ns) + ");\n";
                c += "        }\n        return true;\n    }\n";
            }
            // An inherited function is the base's to run — and only if it is
            // public, which is the base's own check, not something to repeat.
            if (par)
                c += "    return " + par->cpp + "::callFunction(name, " +
                     (anyPrivateFn ? "requirePublic" : "true") + ", " +
                     (anyFnParams ? "args" : "{}") + ", results);\n}\n\n";
            else
                c += "    return false;\n}\n\n";
        }

        if (chainVarCount)
        {
            // slots() is the whole chain, so these three answer for an inherited
            // name as readily as for an own one — which is what the Runtime's
            // name-keyed seam and the garbage collector both need.
            c += "hc::Value " + m_cls + "::getVariable(const std::string& name) const\n";
            c += "{ return hc::getVar(slots(), this, name); }\n\n";
            c += "bool " + m_cls + "::setVariable(const std::string& name, const hc::Value& v)\n";
            c += "{ return hc::setVar(slots(), this, name, v); }\n\n";
            if (chainRefVar)
            {
                c += "void " + m_cls + "::collectRefs(std::vector<uint32_t>& out) const\n";
                c += "{ hc::collectVarRefs(slots(), this, out); }\n\n";
            }
        }
        if (varCount || !states.empty())
        {
            c += "void " + m_cls + "::reseedVariables()\n{\n";
            // The base's variables AND its node states first — its own reseed is
            // the only thing that can reach the DoOnce flags it declares.
            if (par) c += "    " + par->cpp + "::reseedVariables();\n";
            // Own slots only: the base's were just reseeded, and doing them twice
            // would only be slower, not wrong.
            if (varCount) c += "    hc::reseedVars(" + std::string(ownTable ? "ownSlots()" : "slots()") +
                               ", this);\n";
            for (const int id : states)
                c += "    " + stateMember(id) + " = false;\n";   // DoOnce/FlipFlop start over
            c += "}\n\n";
        }
        if (!delays.empty())
        {
            c += "void " + m_cls + "::resumeFrom(int nodeId)\n{\n";
            if (rsOn) c += "    RunState rs;\n";
            for (const int id : delays)
                c += "    if (nodeId == " + std::to_string(resumeId(id)) + ") { resume_" +
                     std::to_string(id) + "(" + rsArg() + "); return; }\n";
            // A Delay in a base class schedules a resume by ITS node id; the id
            // means nothing here, so the lookup has to continue up the chain.
            if (par) c += "    " + par->cpp + "::resumeFrom(nodeId);\n";
            c += "}\n\n";
        }

        for (const Fragment& f : frags)
        {
            const std::string params = f.fn ? fnParamList(*f.fn, f.rsUsed) : rsParam(f.rsUsed);
            c += "void " + m_cls + "::" + f.name + "(" + params + ")\n{\n";
            c += unindentBody(f.prelude);
            c += unindentBody(f.body);
            c += "}\n\n";
        }

        c += "// The manifest's factory pair (hc_registry.cpp links against these).\n";
        c += "HorizonCode::CompiledInstance* create_" + m_cls + "() { return new " + m_cls + "(); }\n";
        c += "void destroy_" + m_cls + "(HorizonCode::CompiledInstance* p) { delete p; }\n\n";
        c += "} // namespace " + ns + "\n";
    }

    // Bodies are translated at class-member indentation (two levels); a
    // free-standing definition wants one less.
    static std::string unindentBody(const std::string& body)
    {
        std::string out;
        size_t i = 0;
        while (i < body.size())
        {
            size_t e = body.find('\n', i);
            if (e == std::string::npos) e = body.size();
            std::string line = body.substr(i, e - i);
            if (line.rfind("    ", 0) == 0) line.erase(0, 4);
            out += line;
            out += '\n';
            i = e + 1;
        }
        return out;
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
            case PT::Enum:      return "Enum";
            case PT::Struct:    return "Struct";
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
    return uniqueName("C_" + sanitize(stem), used);
}

} // namespace

// The whole emission, writing into `res`; generate() below wraps it so that
// nothing thrown in here reaches a caller.
static void generateInto(const std::vector<ClassSource>& sources, const Options& opt, Result& res)
{
    std::unordered_set<std::string> usedNames;

    // The project's Struct and Enum assets, as C++ types — one header each, so
    // a class depends on the definitions it uses and not on the others. Built
    // before any class so the names are stable across them.
    const TypeTable types = buildTypeTable(sources);
    for (const std::string& path : types.enumOrder)
    {
        const EnumType& et = types.enumsByPath.at(path);
        res.files.push_back({ typeHeaderName(et.cpp),
                              emitEnumHeader(et, path, opt.namespaceName) });
    }
    for (const std::string& path : types.order)
        res.files.push_back({ typeHeaderName(types.byPath.at(path).cpp),
                              emitStructHeader(types, path, opt.namespaceName) });
    if (!types.order.empty() || !types.enumOrder.empty())
        res.files.push_back({ "hcgen_types.h", emitTypesUmbrella(types) });

    struct Entry { std::string key, className; };
    std::vector<Entry> compiled;

    // ── base classes first ───────────────────────────────────────────────────
    // `C_Goblin : public C_Enemy` needs C_Enemy's emitter to have run: its C++
    // name, its member names and its public methods are all things the child
    // asks the ClassTable for. Sorting by how deep a class sits in its chain
    // does that in one pass — the chain is already resolved, so no graph walk is
    // needed, and classes at the same depth keep the caller's order.
    std::vector<const ClassSource*> order;
    order.reserve(sources.size());
    for (const ClassSource& s : sources) order.push_back(&s);
    std::stable_sort(order.begin(), order.end(),
        [](const ClassSource* a, const ClassSource* b)
        { return a->chain.size() < b->chain.size(); });

    // Probe pass: which classes reach native C++ at all. A direct call into
    // another class is only emittable if that class HAS a C++ type, and the only
    // way to know is to try. Output and warnings are thrown away; the real pass
    // below repeats the work with the answer in hand.
    ClassTable classes;
    // Who is somebody's base — decided before any emission, because it is what
    // says whether a class may be `final`.
    for (const ClassSource& s : sources)
        if (!s.chain.empty()) classes.isBase.insert(s.chain.front());
    {
        std::unordered_set<std::string> probeNames;
        const size_t warningsBefore = res.warnings.size();
        for (const ClassSource* srcp : order)
        {
            const ClassSource& src = *srcp;
            const std::string className = classNameFor(src.key, probeNames);
            try
            {
                ClassEmitter em(src, className, opt, types, classes, res.warnings);
                std::string h, c;
                em.run(h, c);
                classes.byKey[src.key] = { src.key, className, &src.graph,
                                           src.chain.empty() ? std::string() : src.chain.front(),
                                           em.publicFunctions(), em.publicVariables(),
                                           em.publicMethodSigs(), em.variableMembers() };
            }
            catch (...) { probeNames.erase(className); }
        }
        res.warnings.resize(warningsBefore);
    }

    // ── event wiring across the run ──────────────────────────────────────────
    // Emit/Bind/handle all name an event as free text in three different places,
    // so a typo binds to nothing and reports nothing. Now it reports.
    {
        std::unordered_set<std::string> handled, emitted;
        for (const ClassSource& src : sources)
            for (const Node& n : src.graph.nodes)
            {
                if (n.type == NT::Event)     handled.insert(n.s);
                if (n.type == NT::EmitEvent) emitted.insert(n.s);
            }
        // "Nobody else does the other half" only means anything when the whole
        // project is on the table — the editor's per-asset check passes ONE.
        const bool wholeProject = sources.size() > 1;
        for (const ClassSource& src : sources)
        {
            for (const Node& n : src.graph.nodes)
            {
                if (wholeProject && n.type == NT::EmitEvent && !n.s.empty() && !handled.count(n.s))
                    res.warnings.push_back(src.key + ": emits '" + n.s +
                        "' but no class in this project handles it (node " +
                        std::to_string(n.id) + ")");
                if (n.type != NT::BindEvent || n.s.empty()) continue;
                // Binding subscribes THIS class, so THIS class needs the handler.
                bool mine = false;
                for (const Node& h : src.graph.nodes)
                    if (h.type == NT::Event && h.s == n.s) { mine = true; break; }
                if (!mine)
                    res.warnings.push_back(src.key + ": binds to '" + n.s +
                        "' but has no handler for it, so nothing will run (node " +
                        std::to_string(n.id) + ")");
                else if (wholeProject && !emitted.count(n.s))
                    res.warnings.push_back(src.key + ": binds to '" + n.s +
                        "' but no class in this project emits it (node " +
                        std::to_string(n.id) + ")");
            }
        }
    }

    // A class that fell back in the REAL pass takes its descendants with it: the
    // probe said the parent was compilable, so the child was translated against
    // a C++ base that will not exist. (The probe can disagree with the real pass
    // — it runs against a partially filled table.)
    std::unordered_set<std::string> failed;
    for (size_t si = 0; si < order.size(); ++si)
    {
        const ClassSource& src = *order[si];
        if (opt.onClass)
            opt.onClass(src.label.empty() ? src.key : src.label, si, order.size());
        const std::string className = classNameFor(src.key, usedNames);
        try
        {
            if (!src.chain.empty() && failed.count(src.chain.front()))
                throw FallbackError{ "base class '" + src.chain.front() +
                                     "' could not be compiled", 0 };
            ClassEmitter em(src, className, opt, types, classes, res.warnings);
            std::string header, impl;
            em.run(header, impl);
            res.files.push_back({ "hcgen_" + className + ".h",   std::move(header) });
            res.files.push_back({ "hcgen_" + className + ".cpp", std::move(impl) });
            compiled.push_back({ src.key, className });
        }
        catch (const FallbackError& e)
        {
            usedNames.erase(className);   // name freed — the class ships interpreted
            failed.insert(src.key);
            res.fallbacks.push_back({ src.key, e.reason, e.node });
            // Stop mode: a class that cannot be compiled is the build's problem,
            // not something to paper over. Every one is still collected, so the
            // author fixes them all in one pass instead of one per build.
            if (opt.onFailure == OnFailure::Stop) res.ok = false;
        }
        catch (const std::exception& e)
        {
            // NOT a "this graph can't be compiled" verdict (that is FallbackError
            // above) but a slip in the emitter itself — a lookup we thought total
            // (.at()) or an allocation failure. Keep translating the remaining
            // classes so one bad graph doesn't cost the whole build, but fail the
            // run: this class drops out of the manifest and ships interpreted.
            // It gets a fallback entry as well as the warning because the editor's
            // per-class compile check reads only `fallbacks` — without one it would
            // report the graph as compiling cleanly.
            usedNames.erase(className);
            failed.insert(src.key);
            res.ok = false;
            res.warnings.push_back("internal codegen error in '" +
                                   (src.label.empty() ? src.key : src.label) + "': " + e.what());
            res.fallbacks.push_back({ src.key, "internal codegen error — shipped interpreted", 0 });
        }
    }

    // ── hcgen.h: the whole library in one include ────────────────────────────
    // Every compiled class and every user-defined type, declared once. This is
    // what a generated .cpp includes (so a graph can reach into any other class
    // without per-file include bookkeeping, and mutually calling classes cannot
    // deadlock on each other's header), and what hand-written C++ includes to
    // use them.
    {
        std::string gh;
        gh += "// GENERATED by HorizonCode → C++ codegen — do not edit.\n";
        gh += "// Every compiled HorizonCode class and user-defined type of this\n";
        gh += "// project, in one include. Header-only declarations: including this\n";
        gh += "// from anywhere in the library (or from your own C++) is free of\n";
        gh += "// ordering concerns.\n";
        gh += "#pragma once\n";
        if (!types.order.empty() || !types.enumOrder.empty())
            gh += "#include \"hcgen_types.h\"\n";
        for (const auto& e : compiled) gh += "#include \"hcgen_" + e.className + ".h\"\n";
        gh += "#include \"hc_registry.h\"\n";
        res.files.push_back({ "hcgen.h", std::move(gh) });
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
    rc += "\nnamespace " + ns + " {\n\n";
    if (!compiled.empty())
    {
        // Each class lives entirely in its own .cpp; only this factory pair
        // crosses the file boundary, so there are no generated headers.
        rc += "// Defined in the per-class translation units.\n";
        for (const auto& e : compiled)
        {
            rc += "HorizonCode::CompiledInstance* create_" + e.className + "();\n";
            rc += "void destroy_" + e.className + "(HorizonCode::CompiledInstance* p);\n";
        }
        rc += "\nstatic const HorizonCode::CompiledClassEntry kClasses[] = {\n";
        for (const auto& e : compiled)
            rc += "    { " + strLit(e.key) + ", &create_" + e.className +
                  ", &destroy_" + e.className + " },\n";
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
    rc += "// The C-ABI manifest export, only when building the shipped dylib. It is\n";
    rc += "// the ONLY symbol the library exports (the project builds with hidden\n";
    rc += "// visibility): the generated classes live in headers, so leaving them\n";
    rc += "// visible would let a host binary that compiled the same headers share\n";
    rc += "// their inline statics with this library — and lose them on unload.\n";
    rc += "#if defined(HCGEN_BUILD_DYLIB)\n";
    rc += "extern \"C\"\n";
    rc += "#if defined(_WIN32)\n__declspec(dllexport)\n#else\n__attribute__((visibility(\"default\")))\n#endif\n";
    rc += "const HorizonCode::CompiledClassEntry* HE_HorizonCodeGenClasses(int* count, const char** engineVersion)\n";
    rc += "{\n    if (engineVersion) *engineVersion = " + ns + "::engineVersion();\n";
    rc += "    return " + ns + "::classes(count);\n}\n";
    rc += "#endif\n";
    res.files.push_back({ "hc_registry.cpp", std::move(rc) });
}

Result generate(const std::vector<ClassSource>& sources, const Options& opt)
{
    Result res;
    res.ok = true;
    // Exception firewall. Two of the four callers press this from an ImGui
    // button mid-frame (LevelScriptPanel, UIEditorPanel), where an escaping
    // exception unwinds through the unfinished frame — a torn frame at best.
    // So an internal slip (a .at() we thought total, an allocation failure while
    // building the sources) has to come back as ok=false + a warning instead.
    // Per-class errors are already handled one level down; this catches whatever
    // happens outside a class (the manifest emission) and anything the inner
    // handler itself throws. Files produced so far are kept for diagnosis — the
    // set is incomplete, which is exactly what ok=false says.
    try
    {
        generateInto(sources, opt, res);
    }
    catch (const std::exception& e)
    {
        res.ok = false;
        res.warnings.push_back(std::string("internal codegen error: ") + e.what());
    }
    catch (...)
    {
        res.ok = false;
        res.warnings.push_back("internal codegen error: unknown exception");
    }
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
    s += "# Only HE_HorizonCodeGenClasses leaves this library — see hc_registry.cpp.\n";
    s += "set_target_properties(HorizonCodeGen PROPERTIES\n";
    s += "    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)\n";
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

// macOS/Linux apps launched from Finder/Launchpad (a packaged .app, Spotlight, the
// Dock) inherit a minimal PATH — typically "/usr/bin:/bin:/usr/sbin:/sbin" — that
// omits the Homebrew prefixes: /opt/homebrew/bin on Apple Silicon, /usr/local/bin on
// Intel. That's the whole reason a perfectly-installed `brew` (and any cmake or
// compiler it provides) is invisible to every popen() below, so the toolchain probe
// reports "cmake not found" and the auto-installer reports "Homebrew not found" even
// though both are present. Prepend the standard tool locations to this process's PATH
// exactly once so cmake/brew resolution and the installer all see a Homebrew install.
// No-op on Windows, where GUI processes already inherit the full user PATH.
void ensureToolPathAugmented()
{
#if !defined(_WIN32)
    static const bool s_once = []
    {
        const char* cur = std::getenv("PATH");
        const std::string path = cur ? cur : "";

        std::unordered_set<std::string> have;
        {
            std::istringstream iss(path);
            for (std::string tok; std::getline(iss, tok, ':'); )
                if (!tok.empty()) have.insert(tok);
        }
        // Apple Silicon Homebrew, then Intel Homebrew / manual /usr/local installs.
        std::string prefix;
        for (const char* dir : { "/opt/homebrew/bin", "/opt/homebrew/sbin",
                                 "/usr/local/bin",    "/usr/local/sbin" })
            if (!have.count(dir)) prefix += std::string(dir) + ":";

        if (!prefix.empty())
            setenv("PATH", (prefix + path).c_str(), 1);
        return true;
    }();
    (void)s_once;
#endif
}
} // namespace

namespace {
std::filesystem::path g_bundledCmakeDir; // set by the editor to <app>/cmake (SDL_GetBasePath)

bool cmakeAnswers(const std::string& cmd)
{
#if defined(_WIN32)
    return std::system((cmd + " --version >NUL 2>&1").c_str()) == 0;
#else
    return std::system((cmd + " --version >/dev/null 2>&1").c_str()) == 0;
#endif
}

// Resolve the cmake command to use, preferring a cmake bundled next to the editor
// (<app>/cmake/bin/cmake[.exe]) so a user only has to install a C++ compiler, then
// falling back to a system cmake on PATH. Returns a shell-ready token (a quoted path,
// or the bare word "cmake"), or empty when neither answers --version — the caller then
// surfaces the Toolchain-Missing dialog. Resolved once and cached.
const std::string& resolveCmake()
{
    static std::string s_cmake;
    static bool s_done = false;
    if (s_done) return s_cmake;
    s_done = true;
    ensureToolPathAugmented(); // make a Homebrew cmake on /opt/homebrew visible (see note above)
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!g_bundledCmakeDir.empty())
    {
#if defined(_WIN32)
        const fs::path exe = g_bundledCmakeDir / "bin" / "cmake.exe";
#else
        const fs::path exe = g_bundledCmakeDir / "bin" / "cmake";
#endif
        if (fs::is_regular_file(exe, ec))
        {
            const std::string quoted = shq(exe);
            if (cmakeAnswers(quoted)) { s_cmake = quoted; return s_cmake; }
        }
    }
    if (cmakeAnswers("cmake")) s_cmake = "cmake";
    return s_cmake; // empty ⇒ not found anywhere
}
} // namespace

void setBundledCmakeDir(const std::filesystem::path& dir) { g_bundledCmakeDir = dir; }

bool toolchainAvailable()
{
    return !resolveCmake().empty();
}

namespace {
// Run a shell command, streaming each output line (stdout+stderr merged) to
// `onLine` and appending everything to `captured`. Returns the exit status.
int runStreaming(const std::string& cmd, const std::function<void(const std::string&)>& onLine,
                 std::string& captured)
{
#if defined(_WIN32)
    FILE* pipe = _popen((cmd + " 2>&1").c_str(), "r");
#else
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
#endif
    if (!pipe) return -1;
    char buf[1024];
    while (std::fgets(buf, sizeof buf, pipe))
    {
        captured += buf;
        if (onLine)
        {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            onLine(line);
        }
    }
#if defined(_WIN32)
    return _pclose(pipe);
#else
    return pclose(pipe);
#endif
}
} // namespace

ToolchainProbe probeToolchain()
{
    namespace fs = std::filesystem;
    ToolchainProbe p;

    const std::string cmake = resolveCmake();
    if (cmake.empty())
        return p; // cmake missing entirely (bundled + PATH both absent) — nothing to probe
    std::string versionOut;
    runStreaming(cmake + " --version", nullptr, versionOut);
    p.cmakeFound = true;
    // First line looks like "cmake version 3.28.3".
    if (const size_t pos = versionOut.find("cmake version "); pos != std::string::npos)
    {
        const size_t start = pos + std::strlen("cmake version ");
        const size_t end   = versionOut.find_first_of("\r\n", start);
        p.cmakeVersion = versionOut.substr(start, end == std::string::npos ? end : end - start);
    }

    // A real (buildless) configure of a throwaway CXX project — this is the
    // only reliable way to know whether cmake will find a working compiler,
    // since detection (MSVC on Windows especially) doesn't just read PATH.
    // PID-suffixed so two editor instances probing concurrently don't race on
    // the same directory.
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const long pid = static_cast<long>(getpid());
#endif
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) /
        ("he_toolchain_probe_" + std::to_string(pid));
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec)
    {
        p.detail = "could not create probe directory: " + ec.message();
        return p;
    }
    {
        std::ofstream f(dir / "CMakeLists.txt", std::ios::binary | std::ios::trunc);
        f << "cmake_minimum_required(VERSION 3.20)\nproject(he_toolchain_probe CXX)\n";
    }
    std::string captured;
    const int rc = runStreaming(
        cmake + " -S " + shq(dir) + " -B " + shq(dir / "build"), nullptr, captured);
    p.compilerFound = (rc == 0);
    if (p.compilerFound)
    {
        if (const size_t pos = captured.find("CXX compiler identification is ");
            pos != std::string::npos)
        {
            const size_t start = pos + std::strlen("CXX compiler identification is ");
            const size_t end   = captured.find_first_of("\r\n", start);
            p.compilerId = captured.substr(start, end == std::string::npos ? end : end - start);
        }
    }
    else
    {
        // Tail of the log (last ~15 lines) — enough to show the user why.
        std::vector<std::string> lines;
        std::istringstream iss(captured);
        for (std::string line; std::getline(iss, line); ) lines.push_back(line);
        const size_t from = lines.size() > 15 ? lines.size() - 15 : 0;
        for (size_t i = from; i < lines.size(); ++i) { p.detail += lines[i]; p.detail += '\n'; }
    }
    fs::remove_all(dir, ec);
    return p;
}

namespace {
// True if <exe> resolves on PATH — used to pick an available package manager.
bool commandExists(const std::string& exe)
{
    std::string cap;
#if defined(_WIN32)
    return runStreaming("where " + exe, nullptr, cap) == 0;
#else
    return runStreaming("command -v " + exe, nullptr, cap) == 0;
#endif
}

#if defined(__APPLE__)
// Resolve a usable Homebrew. ensureToolPathAugmented() already puts the standard
// prefixes on PATH, but a Finder-launched app that ran before this fix, or an
// install in a non-standard spot, can still hide `brew` from `command -v` — so also
// probe the two canonical binary locations directly. Returns a shell-ready token
// (a quoted absolute path, or the bare word "brew"), or empty when none is found.
std::string resolveBrew()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* p : { "/opt/homebrew/bin/brew", "/usr/local/bin/brew" })
        if (fs::is_regular_file(p, ec)) return shq(fs::path(p));
    if (commandExists("brew")) return "brew";
    return {};
}
#endif
} // namespace

ToolchainInstall installToolchain(bool needCmake, bool needCompiler,
                                  const std::function<void(const std::string&)>& onLine)
{
    ToolchainInstall r;
    const auto emit = [&](const std::string& s) { if (onLine) onLine(s); };
    const auto run  = [&](const std::string& cmd) -> int
    {
        emit("$ " + cmd);
        std::string cap;
        const int rc = runStreaming(cmd, onLine, cap);
        emit(rc == 0 ? "[done]" : "[exit code " + std::to_string(rc) + "]");
        return rc;
    };

#if defined(__APPLE__)
    ensureToolPathAugmented(); // so an already-installed brew/cmake is actually visible
    const std::string brew = resolveBrew();

    if (needCmake && brew.empty())
    {
        // Homebrew is absent — bootstrap it AND install cmake in one automated pass.
        // Homebrew's own installer also installs the Xcode Command Line Tools, so this
        // covers the compiler too; there's no need to fire xcode-select separately.
        // The bootstrap needs interactive administrator rights (a sudo password
        // prompt) that a windowless popen pipe can't service, so hand the whole chain
        // to Terminal.app: the user enters their password once there and Homebrew and
        // cmake both install without a second trip back here — then Recheck goes green.
        // The $(...) and the absolute brew paths stay single-quoted so THIS shell
        // passes them through literally; Terminal's shell is what expands and runs them.
        r.attempted = true;
        emit("Homebrew was not found — installing it (this also installs the Xcode");
        emit("Command Line Tools compiler), then cmake, in a Terminal window.");
        emit("Enter your password when Terminal asks; click Recheck once it finishes.");
        run("osascript"
            " -e 'tell application \"Terminal\" to activate'"
            " -e 'tell application \"Terminal\" to do script \""
            "/bin/bash -c \\\"$(curl -fsSL "
            "https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\\\""
            " && { /opt/homebrew/bin/brew install cmake || /usr/local/bin/brew install cmake; }"
            "\"'");
    }
    else
    {
        if (needCompiler)
        {
            r.attempted = true;
            emit("Requesting the Xcode Command Line Tools installer…");
            emit("Complete the macOS dialog that appears, then click Recheck.");
            run("xcode-select --install"); // triggers the system GUI installer, returns fast
        }
        if (needCmake) // brew is present here → install cmake in-process (no sudo needed)
        {
            r.attempted = true;
            emit("Homebrew found (" + brew + ") — installing cmake…");
            if (const int rc = run(brew + " install cmake")) r.exitCode = rc;
        }
    }
#elif defined(_WIN32)
    if (!commandExists("winget"))
    {
        r.message = "winget (App Installer) not found — update Windows or install it "
                    "from the Microsoft Store, then retry.";
        emit(r.message);
        return r;
    }
    const std::string agree = " --accept-source-agreements --accept-package-agreements";
    if (needCmake)
    {
        r.attempted = true;
        if (const int rc = run("winget install --id Kitware.CMake -e" + agree)) r.exitCode = rc;
    }
    if (needCompiler)
    {
        r.attempted = true;
        if (const int rc = run(
                "winget install --id Microsoft.VisualStudio.2022.BuildTools -e"
                " --override \"--add Microsoft.VisualStudio.Workload.VCTools "
                "--includeRecommended --quiet --wait --norestart\"" + agree)) r.exitCode = rc;
    }
#else // Linux / other Unix
    std::string install;
    if      (commandExists("apt-get")) install = "apt-get update && apt-get install -y build-essential cmake";
    else if (commandExists("dnf"))     install = "dnf install -y gcc-c++ make cmake";
    else if (commandExists("pacman"))  install = "pacman -S --needed --noconfirm base-devel cmake";
    else if (commandExists("zypper"))  install = "zypper install -y gcc-c++ make cmake";
    if (install.empty())
    {
        r.message = "No supported package manager (apt/dnf/pacman/zypper) found — "
                    "install a C++ compiler and cmake manually.";
        emit(r.message);
        return r;
    }
    if (!commandExists("pkexec"))
    {
        r.message = "pkexec (polkit) not found — run in a terminal: sudo " + install;
        emit(r.message);
        return r;
    }
    r.attempted = true;
    emit("A graphical prompt (pkexec) will ask for administrator access…");
    if (const int rc = run("pkexec sh -c " + shq(install))) r.exitCode = rc;
#endif

    (void)needCmake; (void)needCompiler; // not every platform branch uses both
    return r;
}

BuildOutcome buildDylib(const std::filesystem::path& genDir, const SdkInfo& sdk,
                        const std::function<void(const std::string& line)>& onLine)
{
    namespace fs = std::filesystem;
    BuildOutcome out;
    out.logFile = genDir / "build.log";
    if (!sdk.valid())
    {
        out.message = "no codegen SDK found (HE_HCGEN_SDK, <editor>/SDK, he_sdk_config.json)";
        return out;
    }
    const std::string cmake = resolveCmake();
    if (cmake.empty())
    {
        out.message = "cmake not found (bundled next to the editor or on PATH)";
        return out;
    }

    std::string includes;
    for (size_t i = 0; i < sdk.includeDirs.size(); ++i)
    {
        if (i) includes += ";";
        includes += sdk.includeDirs[i].string();
    }
    const fs::path buildDir = genDir / "build";
    std::string captured;
    const auto flushLog = [&]
    {
        std::ofstream f(out.logFile, std::ios::binary | std::ios::trunc);
        if (f) f << captured;
    };
    const std::string configure =
        cmake + " -S " + shq(genDir) + " -B " + shq(buildDir) +
        " -DCMAKE_BUILD_TYPE=Release"
        " \"-DHE_SDK_INCLUDE_DIRS=" + includes + "\""
        " -DHE_SDK_LIB_DIR=" + shq(sdk.libDir);
    if (runStreaming(configure, onLine, captured) != 0)
    {
        flushLog();
        out.message = "cmake configure failed (see " + out.logFile.string() + ")";
        return out;
    }
    const std::string build = cmake + " --build " + shq(buildDir) + " --config Release";
    if (runStreaming(build, onLine, captured) != 0)
    {
        flushLog();
        out.message = "compile failed (see " + out.logFile.string() + ")";
        return out;
    }
    flushLog();

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
