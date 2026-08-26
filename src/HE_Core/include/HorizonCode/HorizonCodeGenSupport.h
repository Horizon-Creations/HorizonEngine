#pragma once
#include <Types/Defines.h>
#include "HorizonCode.h"
#include "HorizonCodeCompiled.h"   // VarSlot binds members of a CompiledInstance
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>   // make_move_iterator — concatSlots
#include <string>
#include <type_traits>   // the element-equality detection behind Set/Map
#include <utility>
#include <vector>

// ── hc:: — the runtime support library for GENERATED HorizonCode C++ ─────────
// Included by every class the HorizonCode → C++ codegen emits (and by the parity
// tests). Each helper reproduces one clause of the interpreter's semantic
// contract (docs/horizoncode-cpp-codegen-implementation-plan.md §3) — the
// comments below cite the clause they mirror. Nothing here may consult wall
// clock or locale: generated code must be deterministic.

namespace hc {

using HorizonCode::ContainerKind;
using HorizonCode::containerKindOf;
using HorizonCode::Context;
using HorizonCode::PinType;
using HorizonCode::Value;

template <typename T> using Array = std::vector<T>;

// The Transform pin's C++ shape (Value carries tpos/trot/tscl; rotation in
// euler degrees, identity scale — the same defaults as a fresh Value).
struct Transform
{
    glm::vec3 pos{ 0.0f }, rot{ 0.0f }, scl{ 1.0f };
    bool operator==(const Transform& o) const { return pos == o.pos && rot == o.rot && scl == o.scl; }
};

// The Vec4 pin's C++ shape. It is four floats and nothing else — the wrapper
// exists ONLY so the type-keyed templates below (raw/toValue/tagOf/coerce) can
// tell a Vec4 apart from a Color, which is also glm::vec4 and got there first.
// Without it an array of Vec4 would box itself as an array of Color and the
// parity harness would (correctly) call that a divergence.
//
// Vec3 needs no such wrapper: glm::vec3 was not spoken for.
struct Vec4
{
    glm::vec4 v{ 0.0f };
    Vec4() = default;
    Vec4(const glm::vec4& x) : v(x) {}
    Vec4(float x, float y, float z, float w) : v(x, y, z, w) {}
    operator glm::vec4() const { return v; }
    bool operator==(const Vec4& o) const { return v == o.v; }
};

// ── zero values ──────────────────────────────────────────────────────────────
// "The zero value of the target type" (§3.3) is a FRESH Value's field — note
// Color's alpha 1 and Transform's identity scale.
template <typename T> inline T zeroOf();
template <> inline float       zeroOf<float>()       { return 0.0f; }
template <> inline bool        zeroOf<bool>()        { return false; }
template <> inline int         zeroOf<int>()         { return 0; }
template <> inline std::string zeroOf<std::string>() { return {}; }
template <> inline glm::vec2   zeroOf<glm::vec2>()   { return glm::vec2(0.0f); }
template <> inline glm::vec4   zeroOf<glm::vec4>()   { return glm::vec4(0.0f, 0.0f, 0.0f, 1.0f); }
// A vector's empty is the null vector — Color's alpha-1 belongs to Color alone.
template <> inline glm::vec3   zeroOf<glm::vec3>()   { return glm::vec3(0.0f); }
template <> inline Vec4        zeroOf<Vec4>()        { return Vec4{}; }
template <> inline uint32_t    zeroOf<uint32_t>()    { return 0u; }
template <> inline Transform   zeroOf<Transform>()   { return {}; }

// ── raw field reads (typed value ⇄ Value) ────────────────────────────────────
// A wire between equal pin types is a RAW field read in the interpreter (§3.3);
// raw<T> reproduces that (no coercion, no type-tag check).
template <typename T> inline T raw(const Value& v);
template <> inline float       raw<float>(const Value& v)       { return v.f; }
template <> inline bool        raw<bool>(const Value& v)        { return v.b; }
template <> inline int         raw<int>(const Value& v)         { return v.i; }
template <> inline std::string raw<std::string>(const Value& v) { return v.s; }
template <> inline glm::vec2   raw<glm::vec2>(const Value& v)   { return v.v2; }
template <> inline glm::vec4   raw<glm::vec4>(const Value& v)   { return v.col; }
template <> inline glm::vec3   raw<glm::vec3>(const Value& v)   { return v.v3; }
template <> inline Vec4        raw<Vec4>(const Value& v)        { return Vec4{ v.v4 }; }
template <> inline uint32_t    raw<uint32_t>(const Value& v)    { return v.ref; }
template <> inline Transform   raw<Transform>(const Value& v)   { return { v.tpos, v.trot, v.tscl }; }

template <typename T> inline Array<T> rawArray(const Value& v)
{
    Array<T> out;
    out.reserve(v.items.size());
    for (const Value& it : v.items) out.push_back(raw<T>(it));
    return out;
}

inline Value toValue(float v)              { return Value::ofFloat(v); }
inline Value toValue(bool v)               { return Value::ofBool(v); }
inline Value toValue(int v)                { return Value::ofInt(v); }
inline Value toValue(const std::string& v) { return Value::ofString(v); }
inline Value toValue(const char* v)        { return Value::ofString(v); }
inline Value toValue(const glm::vec2& v)   { return Value::ofVec2(v); }
inline Value toValue(const glm::vec4& v)   { return Value::ofColor(v); }
inline Value toValue(const glm::vec3& v)   { return Value::ofVec3(v); }
inline Value toValue(const Vec4& v)        { return Value::ofVec4(v.v); }
inline Value toValue(uint32_t v)           { return Value::ofRef(v); }
inline Value toValue(const Transform& v)   { return Value::ofTransform(v.pos, v.rot, v.scl); }

// Element type tag for array Values (matches the pin's element PinType).
template <typename T> inline PinType tagOf();
template <> inline PinType tagOf<float>()       { return PinType::Float; }
template <> inline PinType tagOf<bool>()        { return PinType::Bool; }
template <> inline PinType tagOf<int>()         { return PinType::Int; }
template <> inline PinType tagOf<std::string>() { return PinType::String; }
template <> inline PinType tagOf<glm::vec2>()   { return PinType::Vec2; }
template <> inline PinType tagOf<glm::vec4>()   { return PinType::Color; }
template <> inline PinType tagOf<glm::vec3>()   { return PinType::Vec3; }
template <> inline PinType tagOf<Vec4>()        { return PinType::Vec4; }
template <> inline PinType tagOf<uint32_t>()    { return PinType::Ref; }
template <> inline PinType tagOf<Transform>()   { return PinType::Transform; }

template <typename T> inline Value toValueArray(const Array<T>& a)
{
    Value v; v.isArray = true; v.type = tagOf<T>();
    v.items.reserve(a.size());
    for (const T& e : a) v.items.push_back(toValue(e));
    return v;
}
// Generic overload so emitted code can always spell hc::toValue(x).
template <typename T> inline Value toValue(const Array<T>& a) { return toValueArray(a); }

// Result-vector read: a cached exec output / call result read out of range is
// Value{} in the interpreter → the target type's zero (§3.3).
template <typename T> inline T fromValue(const std::vector<Value>& r, size_t k)
{ return k < r.size() ? raw<T>(r[k]) : zeroOf<T>(); }
template <typename T> inline Array<T> fromValueArray(const std::vector<Value>& r, size_t k)
{ return k < r.size() ? rawArray<T>(r[k]) : Array<T>{}; }

// Bounds-tolerant argument read (§3.1: missing args → typed default).
inline Value arg(const std::vector<Value>& args, size_t i)
{ return i < args.size() ? args[i] : Value{}; }

// ── coerce (§3.3, byte-for-byte the interpreter's `coerce` + raw field read) ─
// Arrays pass through coerce untouched, then the reader reads the field raw —
// so each helper reads the raw field for arrays too. Only Float↔Int↔Bool
// convert; any other mismatch yields the target's zero value.
//
// *** DELIBERATE DUPLICATE — KEEP IN SYNC WITH HorizonCode.cpp's `coerce`. ***
// This is the parity contract between the interpreter (what the editor previews)
// and generated C++ (what a packaged build actually runs): the generated code
// must reach the same value WITHOUT linking the interpreter, so the rule is
// written out twice on purpose. Any edit here needs the matching edit there (and
// vice versa), or a shipped build diverges from the editor with no error at all.
// A third implementation of the same rule lives in UIWidgetBinding.cpp
// (uiHcValueToProp), coercing into UIPropValue.
inline float coerceFloat(const Value& v)
{
    if (v.isArray || v.type == PinType::Float) return v.f;
    if (v.type == PinType::Bool) return v.b ? 1.0f : 0.0f;
    if (v.type == PinType::Int)  return (float)v.i;
    if (v.type == PinType::Enum) return (float)v.i;   // int-backed
    return 0.0f;
}
inline int coerceInt(const Value& v)
{
    if (v.isArray || v.type == PinType::Int) return v.i;
    if (v.type == PinType::Float) return (int)v.f;
    if (v.type == PinType::Bool)  return v.b ? 1 : 0;
    if (v.type == PinType::Enum)  return v.i;   // int-backed
    return 0;
}
inline bool coerceBool(const Value& v)
{
    if (v.isArray || v.type == PinType::Bool) return v.b;
    if (v.type == PinType::Float) return v.f != 0.0f;
    if (v.type == PinType::Int)   return v.i != 0;
    return false;
}
inline std::string coerceString(const Value& v)
{ return (v.isArray || v.type == PinType::String) ? v.s : std::string(); }
inline glm::vec2 coerceVec2(const Value& v)
{ return (v.isArray || v.type == PinType::Vec2) ? v.v2 : zeroOf<glm::vec2>(); }
// Vec3/Vec4/Color are three views of the same numbers, so they interconvert —
// this is what carries a graph authored while Color WAS the vec3 type. The pad
// on widening follows the TARGET: a vector's fourth component is 0, a colour's
// is 1 (opaque). Mirrors HorizonCode.cpp's `coerce` case for case.
inline glm::vec4 coerceColor(const Value& v)
{
    if (v.isArray || v.type == PinType::Color) return v.col;
    if (v.type == PinType::Vec3) return glm::vec4(v.v3, 1.0f);
    if (v.type == PinType::Vec4) return v.v4;
    return zeroOf<glm::vec4>();
}
inline glm::vec3 coerceVec3(const Value& v)
{
    if (v.isArray || v.type == PinType::Vec3) return v.v3;
    if (v.type == PinType::Vec4)  return glm::vec3(v.v4);
    if (v.type == PinType::Color) return glm::vec3(v.col);
    return zeroOf<glm::vec3>();
}
inline Vec4 coerceVec4(const Value& v)
{
    if (v.isArray || v.type == PinType::Vec4) return Vec4{ v.v4 };
    if (v.type == PinType::Vec3)  return Vec4{ glm::vec4(v.v3, 0.0f) };
    if (v.type == PinType::Color) return Vec4{ v.col };
    return zeroOf<Vec4>();
}
inline uint32_t coerceRef(const Value& v)
{ return (v.isArray || v.type == PinType::Ref) ? v.ref : 0u; }
inline Transform coerceTransform(const Value& v)
{ return (v.isArray || v.type == PinType::Transform) ? Transform{ v.tpos, v.trot, v.tscl } : Transform{}; }
template <typename T> inline Array<T> coerceArray(const Value& v)
{ return rawArray<T>(v); }   // arrays pass through; a scalar has no items → empty

// Overload set so generated code can spell hc::coerce<T>(v) generically.
template <typename T> inline T coerce(const Value& v);
template <> inline float       coerce<float>(const Value& v)       { return coerceFloat(v); }
template <> inline bool        coerce<bool>(const Value& v)        { return coerceBool(v); }
template <> inline int         coerce<int>(const Value& v)         { return coerceInt(v); }
template <> inline std::string coerce<std::string>(const Value& v) { return coerceString(v); }
template <> inline glm::vec2   coerce<glm::vec2>(const Value& v)   { return coerceVec2(v); }
template <> inline glm::vec4   coerce<glm::vec4>(const Value& v)   { return coerceColor(v); }
template <> inline glm::vec3   coerce<glm::vec3>(const Value& v)   { return coerceVec3(v); }
template <> inline Vec4        coerce<Vec4>(const Value& v)        { return coerceVec4(v); }
template <> inline uint32_t    coerce<uint32_t>(const Value& v)    { return coerceRef(v); }
template <> inline Transform   coerce<Transform>(const Value& v)   { return coerceTransform(v); }

// ── enums (int-backed values, Value-typed at the boundaries) ────────────────
// An Enum pin lowers to `int` (cppScalar), so handing one back as a Value needs
// the definition path again — a generation-time constant, baked into the call.
// Scalar enum Values carry the typeName; ARRAY enum Values do not, because the
// interpreter's own array builders (variableDefaultValue :702, ArrayAdd) don't
// set one either.
inline Value toEnumValue(int v, const char* typeName)
{ Value r; r.type = PinType::Enum; r.typeName = typeName; r.i = v; return r; }
template <typename E> inline Value toEnumValueArray(const Array<E>& a)
{
    Value v; v.isArray = true; v.type = PinType::Enum;
    v.items.reserve(a.size());
    for (const E e : a) { Value it; it.type = PinType::Enum; it.i = (int)e; v.items.push_back(it); }
    return v;
}
// Struct FIELDS are the exception: TypeRegistry::fieldDefault tags an array
// field's Value AND its items with the field's definition path, so a generated
// struct converter has to do the same to round-trip identically.
inline Value tagArray(Value v, const char* typeName)
{
    if (!typeName || !*typeName) return v;
    v.typeName = typeName;
    for (Value& it : v.items) it.typeName = typeName;
    return v;
}
// coerce(v, Enum) — deliberately NOT coerceInt: a Bool does NOT convert into an
// enum (HorizonCode.cpp's `coerce`, case P::Enum, leaves r.i at 0).
inline int coerceEnum(const Value& v)
{
    if (v.isArray || v.type == PinType::Enum) return v.i;
    if (v.type == PinType::Int)   return v.i;
    if (v.type == PinType::Float) return (int)v.f;
    return 0;
}

// ── struct field reads (used by the generated Value ⇄ struct converters) ────
// One field out of a struct Value's `items`, read the way BreakStruct reads it:
// coerced to the field type, a missing item behaving as Value{} (§3.3).
template <typename T> inline T item(const std::vector<Value>& items, size_t k)
{ return k < items.size() ? coerce<T>(items[k]) : zeroOf<T>(); }
template <typename T> inline Array<T> itemArray(const std::vector<Value>& items, size_t k)
{ return k < items.size() ? rawArray<T>(items[k]) : Array<T>{}; }
// Set/Map are declared further down (they need the array helpers), so these two
// live there — see itemSet / itemMap.
inline int itemEnum(const std::vector<Value>& items, size_t k)
{ return k < items.size() ? coerceEnum(items[k]) : 0; }

// ── math / logic / string (§3.4) ─────────────────────────────────────────────
inline bool feq(float a, float b) { return std::fabs(a - b) < 1e-6f; }
// And/Or must evaluate BOTH sides (the interpreter has no short-circuit; §5.5).
inline bool land(bool a, bool b) { return a && b; }
inline bool lor(bool a, bool b)  { return a || b; }
HE_API std::string toStringG(float v);   // snprintf "%g", buffer 48 — like ToString

// ── arrays (§3.4: pure copy semantics, clamped ops, exact-equality search) ───
HE_API void warnArrayGet(int idx, size_t size);   // the interpreter's out-of-range log
// The element parameters below are NON-DEDUCING (std::type_identity_t): the
// container argument alone fixes T, so a string literal on the value pin
// converts to std::string instead of failing to deduce `const char[2]` against
// T. The Set/Map family (further down) was written this way from the start; the
// Array family predates it and got the same treatment only when a parity fixture
// finally INSTANTIATED these templates — until then `hc::arrAdd(Array<string>{},
// "c")` had never been compiled, and an uninstantiated template body is not
// fully type-checked. That is the whole reason the fixture exists.
template <typename T> using Same = std::type_identity_t<T>;

template <typename T> inline T arrGet(const Array<T>& a, int idx)
{
    if (idx >= 0 && idx < (int)a.size()) return a[(size_t)idx];
    warnArrayGet(idx, a.size());
    return zeroOf<T>();
}
template <typename T> inline Array<T> arrAdd(Array<T> a, const Same<T>& v)
{ a.push_back(v); return a; }
template <typename T> inline Array<T> arrSet(Array<T> a, int idx, const Same<T>& v)
{ if (idx >= 0 && idx < (int)a.size()) a[(size_t)idx] = v; return a; }
template <typename T> inline Array<T> arrInsert(Array<T> a, int idx, const Same<T>& v)
{
    if (idx < 0) idx = 0;
    if (idx > (int)a.size()) idx = (int)a.size();
    a.insert(a.begin() + idx, v);
    return a;
}
template <typename T> inline Array<T> arrRemove(Array<T> a, int idx)
{ if (idx >= 0 && idx < (int)a.size()) a.erase(a.begin() + idx); return a; }
template <typename T> inline bool arrContains(const Array<T>& a, const Same<T>& key)
{
    for (const T& e : a) if (e == key) return true;   // exact equality, like valueEquals
    return false;
}
template <typename T> inline int arrIndexOf(const Array<T>& a, const Same<T>& key)
{
    for (size_t i = 0; i < a.size(); ++i) if (a[i] == key) return (int)i;
    return -1;
}
// Struct elements: `valueEquals` (HorizonCode.cpp) has no Struct case, so its
// default arm makes EVERY comparison false — Contains never hits and IndexOf is
// always -1 for a struct array. Both inputs are still parameters, so they are
// still evaluated (pure-EngineCall dispatch counts are part of the trace, §3.4).
template <typename T> inline bool arrContainsNever(const Array<T>&, const Same<T>&) { return false; }
template <typename T> inline int  arrIndexOfNever (const Array<T>&, const Same<T>&) { return -1; }

// ── Set<T> / Map<K,V> ────────────────────────────────────────────────────────
// INSERTION-ORDERED, and vector-backed for exactly that reason: the interpreter
// keeps a container in `Value::items` (plus a parallel `Value::keys` for a map),
// so laying the generated side out the same way makes the two iterate
// identically by CONSTRUCTION. std::set / std::unordered_map would each iterate
// in an order of their own and the parity harness would (correctly) call that a
// divergence. See ContainerKind in HorizonCode.h for the three ordering rules.
//
// Every op is pure and takes its container BY VALUE, like the array ops: the
// nodes return a new container rather than mutating the one they were handed.
template <typename T>
struct Set
{
    Array<T> items;   // iteration order = the order elements were first added
    size_t size() const { return items.size(); }
};

template <typename K, typename V>
struct Map
{
    Array<K> keys;     // iteration order = the order keys were first inserted
    Array<V> values;   // index-parallel to keys
    size_t size() const { return keys.size(); }
};

// Element identity. `scalarValueEquals` (HorizonCode.cpp) has no Struct case, so
// a struct element never matches anything there — mirrored here by answering
// false for any element type that has no operator== of its own, which is what a
// generated struct is.
template <typename T, typename = void>
struct HasEquality : std::false_type {};
template <typename T>
struct HasEquality<T, std::void_t<decltype(std::declval<const T&>() == std::declval<const T&>())>>
    : std::true_type {};

template <typename T> inline bool sameElem(const T& a, const T& b)
{
    if constexpr (HasEquality<T>::value) return a == b;
    else { (void)a; (void)b; return false; }
}
template <typename T> inline int indexOfElem(const Array<T>& a, const T& key)
{
    for (size_t i = 0; i < a.size(); ++i) if (sameElem(a[i], key)) return (int)i;
    return -1;
}

template <typename T> inline Set<T> setAdd(Set<T> s, const Same<T>& v)
{
    // Already present → unchanged, and NOT moved to the back.
    if (indexOfElem(s.items, v) < 0) s.items.push_back(v);
    return s;
}
template <typename T> inline Set<T> setRemove(Set<T> s, const Same<T>& v)
{
    const int at = indexOfElem(s.items, v);
    if (at >= 0) s.items.erase(s.items.begin() + at);   // the rest keeps its order
    return s;
}
template <typename T> inline bool setContains(const Set<T>& s, const Same<T>& v)
{ return indexOfElem(s.items, v) >= 0; }
template <typename T> inline int setLength(const Set<T>& s) { return (int)s.items.size(); }
template <typename T> inline Set<T> setClear(const Set<T>&) { return Set<T>{}; }
template <typename T> inline Array<T> setToArray(const Set<T>& s) { return s.items; }
// Array → Set: duplicates collapse to the FIRST occurrence, which is both what
// the interpreter's dedupeSetItems does and what folding setAdd over the array
// would leave. Spelled as that fold so the two cannot drift.
template <typename T> inline Set<T> setFromArray(const Array<T>& a)
{
    Set<T> s;
    for (const T& e : a) if (indexOfElem(s.items, e) < 0) s.items.push_back(e);
    return s;
}
// The set algebra. A's ORDER is the result's order in all three (union then
// appends B's extras behind it) — with an insertion-ordered container that is a
// promised property, and the interpreter promises the same one.
template <typename T> inline Set<T> setUnion(const Set<T>& a, const Set<T>& b)
{
    // A is filtered too, not copied wholesale. A hand-fed input — a Set built at
    // a script boundary, or one a caller assembled itself — may carry
    // duplicates, and the OUTPUT has to be a set whatever went in. The
    // interpreter states exactly that rule; the two sides of a node are not
    // allowed to promise different things, which is the whole point of the
    // parity harness.
    Set<T> out;
    for (const T& e : a.items) if (indexOfElem(out.items, e) < 0) out.items.push_back(e);
    for (const T& e : b.items) if (indexOfElem(out.items, e) < 0) out.items.push_back(e);
    return out;
}
template <typename T> inline Set<T> setIntersect(const Set<T>& a, const Set<T>& b)
{
    Set<T> out;
    for (const T& e : a.items)
        if (indexOfElem(b.items, e) >= 0 && indexOfElem(out.items, e) < 0) out.items.push_back(e);
    return out;
}
template <typename T> inline Set<T> setDifference(const Set<T>& a, const Set<T>& b)
{
    Set<T> out;
    for (const T& e : a.items)
        if (indexOfElem(b.items, e) < 0 && indexOfElem(out.items, e) < 0) out.items.push_back(e);
    return out;
}

template <typename K, typename V>
inline Map<K, V> mapSet(Map<K, V> m, const Same<K>& k, const Same<V>& v)
{
    const int at = indexOfElem(m.keys, k);
    if (at >= 0) m.values[(size_t)at] = v;   // update IN PLACE, key keeps its slot
    else { m.keys.push_back(k); m.values.push_back(v); }
    return m;
}
template <typename K, typename V> inline Map<K, V> mapRemove(Map<K, V> m, const Same<K>& k)
{
    const int at = indexOfElem(m.keys, k);
    if (at >= 0)
    {
        m.keys.erase(m.keys.begin() + at);
        if ((size_t)at < m.values.size()) m.values.erase(m.values.begin() + at);
    }
    return m;
}
template <typename K, typename V> inline bool mapContains(const Map<K, V>& m, const Same<K>& k)
{ return indexOfElem(m.keys, k) >= 0; }
template <typename K, typename V> inline int mapLength(const Map<K, V>& m)
{ return (int)m.keys.size(); }
template <typename K, typename V> inline Map<K, V> mapClear(const Map<K, V>&)
{ return Map<K, V>{}; }
template <typename K, typename V>
inline V mapGet(const Map<K, V>& m, const Same<K>& k, const Same<V>& def)
{
    const int at = indexOfElem(m.keys, k);
    // A miss is an ordinary answer, not a warning like arrGet's — the Default
    // pin exists precisely for it.
    return (at >= 0 && (size_t)at < m.values.size()) ? m.values[(size_t)at] : def;
}
template <typename K, typename V> inline Array<K> mapKeys(const Map<K, V>& m)
{
    Array<K> out = m.keys;
    out.resize(m.keys.size() < m.values.size() ? m.keys.size() : m.values.size());
    return out;
}
template <typename K, typename V> inline Array<V> mapValues(const Map<K, V>& m)
{
    Array<V> out = m.values;
    out.resize(m.keys.size() < m.values.size() ? m.keys.size() : m.values.size());
    return out;
}
// Break Map has no helper of its own: it lowers onto mapKeys/mapValues, so the
// truncation rule stays in one place.
//
// Two arrays → one map, paired by INDEX. The SHORTER one wins (a surplus key
// would need an invented value), and a repeated key keeps its first position
// while taking the last value — which is precisely what folding mapSet over the
// pairs does, so that is how it is written.
template <typename K, typename V>
inline Map<K, V> mapFromArrays(const Array<K>& keys, const Array<V>& values)
{
    Map<K, V> m;
    const size_t n = keys.size() < values.size() ? keys.size() : values.size();
    for (size_t i = 0; i < n; ++i)
    {
        const int at = indexOfElem(m.keys, keys[i]);
        if (at >= 0) m.values[(size_t)at] = values[i];
        else { m.keys.push_back(keys[i]); m.values.push_back(values[i]); }
    }
    return m;
}
// The reverse lookup: the FIRST pair holding `v`, in iteration order. Two
// helpers rather than one returning a pair, because the node's two data-outs are
// re-emitted independently at every read (§3.3) and neither may depend on the
// other having been read. A miss answers with K's zero — the same value an
// unwired key pin reads, and what zeroLit emits for it.
template <typename K, typename V>
inline int mapIndexOfValue(const Map<K, V>& m, const Same<V>& v)
{
    const size_t n = m.keys.size() < m.values.size() ? m.keys.size() : m.values.size();
    for (size_t i = 0; i < n; ++i) if (sameElem(m.values[i], v)) return (int)i;
    return -1;
}
template <typename K, typename V> inline K mapFindByValue(const Map<K, V>& m, const Same<V>& v)
{
    const int at = mapIndexOfValue(m, v);
    return at >= 0 ? m.keys[(size_t)at] : K{};
}
template <typename K, typename V> inline bool mapFoundByValue(const Map<K, V>& m, const Same<V>& v)
{ return mapIndexOfValue(m, v) >= 0; }
// Removes EVERY pair holding `v` (mapRemove drops one key; this drops all
// matches), survivors in their original order.
template <typename K, typename V>
inline Map<K, V> mapRemoveByValue(const Map<K, V>& m, const Same<V>& v)
{
    Map<K, V> out;
    const size_t n = m.keys.size() < m.values.size() ? m.keys.size() : m.values.size();
    for (size_t i = 0; i < n; ++i)
        if (!sameElem(m.values[i], v)) { out.keys.push_back(m.keys[i]); out.values.push_back(m.values[i]); }
    return out;
}

// ── Set/Map ⇄ Value ──────────────────────────────────────────────────────────
// Element boxing goes through the SAME toValue/raw the array helpers use, so a
// struct element runs the generated converter and an enum element its ADL
// overload. Container `Value`s carry no element typeName of their own — the
// interpreter's own builders don't set one either (see toEnumValueArray).
// An ENUM element's ADL toValue bakes its definition path; inside a container it
// must not, because the interpreter's own container builders leave it empty
// (same rule as toEnumValueArray). Struct elements keep theirs, again like an
// array of structs.
inline Value untagEnums(Value v)
{
    for (Value& it : v.items) if (it.type == PinType::Enum) it.typeName.clear();
    for (Value& k  : v.keys)  if (k.type  == PinType::Enum) k.typeName.clear();
    return v;
}
template <typename T> inline Value toValue(const Set<T>& s)
{
    Value v; v.isArray = true; v.container = ContainerKind::Set; v.type = tagOf<T>();
    v.items.reserve(s.items.size());
    for (const T& e : s.items) v.items.push_back(toValue(e));
    return untagEnums(std::move(v));
}
template <typename K, typename V> inline Value toValue(const Map<K, V>& m)
{
    Value v; v.isArray = true; v.container = ContainerKind::Map;
    v.type = tagOf<V>(); v.keyType = tagOf<K>();
    const size_t n = m.keys.size() < m.values.size() ? m.keys.size() : m.values.size();
    v.keys.reserve(n); v.items.reserve(n);
    for (size_t i = 0; i < n; ++i)
    { v.keys.push_back(toValue(m.keys[i])); v.items.push_back(toValue(m.values[i])); }
    return untagEnums(std::move(v));
}

// ← Value. A Value that is not the right container reads as an EMPTY one, which
// is what the interpreter's coerce leaves a mismatched pin with. Duplicates that
// arrive through a dynamic boundary (a script's table, a hand-edited save)
// collapse to their FIRST occurrence — the same rule Set Add / Map Set follow.
template <typename T> inline Set<T> rawSet(const Value& v)
{
    Set<T> s;
    if (v.kind() != ContainerKind::Set) return s;
    for (const Value& it : v.items)
    {
        T e = raw<T>(it);
        if (indexOfElem(s.items, e) < 0) s.items.push_back(std::move(e));
    }
    return s;
}
template <typename K, typename V> inline Map<K, V> rawMap(const Value& v)
{
    Map<K, V> m;
    if (v.kind() != ContainerKind::Map) return m;
    const size_t n = v.keys.size() < v.items.size() ? v.keys.size() : v.items.size();
    for (size_t i = 0; i < n; ++i)
    {
        K k = raw<K>(v.keys[i]);
        const int at = indexOfElem(m.keys, k);
        if (at >= 0) m.values[(size_t)at] = raw<V>(v.items[i]);   // last write wins
        else { m.keys.push_back(std::move(k)); m.values.push_back(raw<V>(v.items[i])); }
    }
    return m;
}
template <typename T> inline Set<T> coerceSet(const Value& v) { return rawSet<T>(v); }
template <typename K, typename V> inline Map<K, V> coerceMap(const Value& v)
{ return rawMap<K, V>(v); }
template <typename T> inline Set<T> fromValueSet(const std::vector<Value>& r, size_t k)
{ return k < r.size() ? rawSet<T>(r[k]) : Set<T>{}; }
template <typename K, typename V>
inline Map<K, V> fromValueMap(const std::vector<Value>& r, size_t k)
{ return k < r.size() ? rawMap<K, V>(r[k]) : Map<K, V>{}; }

// Struct field reads, alongside item / itemArray.
template <typename T> inline Set<T> itemSet(const std::vector<Value>& items, size_t k)
{ return k < items.size() ? rawSet<T>(items[k]) : Set<T>{}; }
template <typename K, typename V>
inline Map<K, V> itemMap(const std::vector<Value>& items, size_t k)
{ return k < items.size() ? rawMap<K, V>(items[k]) : Map<K, V>{}; }

// tagArray's map counterpart: TypeRegistry::fieldDefault stamps a container
// field's Value and its elements with the field's definition path, and a map's
// KEYS with the key definition. Empty paths are left alone.
inline Value tagMap(Value v, const char* typeName, const char* keyTypeName)
{
    if (typeName && *typeName)
    {
        v.typeName = typeName;
        for (Value& it : v.items) it.typeName = typeName;
    }
    if (keyTypeName && *keyTypeName)
    {
        v.keyTypeName = keyTypeName;
        for (Value& k : v.keys) k.typeName = keyTypeName;
    }
    return v;
}

// ── host / engine seams (null-tolerant, §3.4 "unbound Context → no-op") ─────
inline Value getProperty(const Context& c, int elem, const char* prop)
{ return c.getProperty ? c.getProperty(elem, prop) : Value{}; }
inline void setProperty(const Context& c, int elem, const char* prop, const Value& v)
{ if (c.setProperty) c.setProperty(elem, prop, v); }
inline Value getVariableCtx(const Context& c, const char* name)
{ return c.getVariable ? c.getVariable(name) : Value{}; }
inline void setVariableCtx(const Context& c, const char* name, const Value& v)
{ if (c.setVariable) c.setVariable(name, v); }
inline void showSelf(const Context& c) { if (c.showSelf) c.showSelf(); }
inline void hideSelf(const Context& c) { if (c.hideSelf) c.hideSelf(); }
inline uint32_t createWidget(const Context& c, const char* path)
{ return c.createWidget ? (uint32_t)c.createWidget(path) : 0u; }
inline void showWidget(const Context& c, int id)    { if (c.showWidget) c.showWidget(id); }
inline void hideWidget(const Context& c, int id)    { if (c.hideWidget) c.hideWidget(id); }
inline void destroyWidget(const Context& c, int id) { if (c.destroyWidget) c.destroyWidget(id); }
// logs the fail like :1019. position/rotationEuler are 3 floats each, or nullptr
// for "place it as the class authored it" (see Context::createObject). The
// DEFAULTS are load-bearing: the codegen emits the plain two-argument call for a
// Create Object whose placement pins are unwired, so every graph written before
// those pins existed still generates byte-identical text.
HE_API uint32_t createObject(const Context& c, const char* classPath,
                             const float* position = nullptr,
                             const float* rotationEuler = nullptr);
inline void destroyObject(const Context& c, uint32_t ref)
{ if (c.destroyObject) c.destroyObject(ref); }
inline Value getExternal(const Context& c, uint32_t target, const char* var)
{ return c.getExternal ? c.getExternal(target, var) : Value{}; }
inline void setExternal(const Context& c, uint32_t target, const char* var, const Value& v)
{ if (c.setExternal) c.setExternal(target, var, v); }
inline void bindEvent(const Context& c, uint32_t target, const char* event)
{ if (c.bindEvent) c.bindEvent(target, event); }
inline void emitEvent(const Context& c, const char* event, const Value& arg)
{ if (c.emitEvent) c.emitEvent(event, arg); }
// By interned id: the generated class holds one constant per event it names, so
// dispatch never hashes a string. The name-taking pair above stays as the
// fallback for a Context that predates the id members.
using HorizonCode::EventId;
inline EventId eventId(const char* name) { return HorizonCode::eventId(name); }
inline void bindEvent(const Context& c, uint32_t target, EventId event)
{
    if (c.bindEventId)    c.bindEventId(target, event);
    else if (c.bindEvent) c.bindEvent(target, HorizonCode::eventName(event));
}
inline void emitEvent(const Context& c, EventId event, const Value& arg)
{
    if (c.emitEventId)    c.emitEventId(event, arg);
    else if (c.emitEvent) c.emitEvent(HorizonCode::eventName(event), arg);
}
inline std::vector<Value> callExternal(const Context& c, uint32_t target, const char* fn,
                                       const std::vector<Value>& args)
{ return c.callExternal ? c.callExternal(target, fn, args) : std::vector<Value>{}; }
inline std::vector<Value> callApi(const Context& c, const char* id, const std::vector<Value>& args)
{ return c.callApi ? c.callApi(id, args) : std::vector<Value>{}; }
inline uint32_t self(const Context& c)         { return c.getSelf ? c.getSelf().ref : 0u; }
inline uint32_t gameInstance(const Context& c) { return c.getGameInstance ? c.getGameInstance().ref : 0u; }
// `realTime` defaults to false so code generated before the Delay node grew its
// Real Time pin still compiles, and still means what it meant: game seconds.
inline void scheduleResume(const Context& c, int nodeId, float seconds, bool realTime = false)
{ if (c.scheduleResume) c.scheduleResume(nodeId, seconds, realTime); }
inline bool isValidRef(const Context& c, uint32_t target)
{ return c.isValid && c.isValid(target); }
HE_API void print(const std::string& s);       // HE::scriptLogLine + Logger Info, like Print

// ── declared variables: one table instead of four name chains ───────────────
// The Runtime reaches a compiled class's variables BY NAME (Get/SetExternal,
// savegames, the GC's Ref scan, reseed) — that seam cannot go away. What can go
// is spelling it out five times: a member pointer type-erased through
// SlotAccess gives varInfos/getVariable/setVariable/reseedVariables/collectRefs
// one table row per variable instead of a branch each.
struct VarSlot
{
    const char* name;
    PinType     type;
    bool        isArray;
    int         access;      // 0 public, 1 private — mirrors Variable::access
    const char* typeName;    // Enum/Struct: the definition path, "" otherwise
    Value       def;         // the declared default, re-assigned on reseed
    Value (*get)(const HorizonCode::CompiledInstance*);
    void  (*set)(HorizonCode::CompiledInstance*, const Value&);
    // Appended last, with defaults, so every slot() call written before Set/Map
    // existed still compiles unchanged. `isArray` says "is a container" and
    // these say which — and, for a map, what keys it. keyType matters to the
    // GC: an object held only as a map KEY is reachable, and `type` describes
    // the value side alone.
    ContainerKind container = ContainerKind::None;
    PinType       keyType = PinType::String;

    ContainerKind kind() const { return containerKindOf(isArray, container); }
};

// coerce<T> is a function template, so it cannot be partially specialized for
// arrays — this class template can, which is what lets SlotAccess stay generic.
template <typename T> struct CoerceTo
{ static T from(const Value& v) { return coerce<T>(v); } };
template <typename T> struct CoerceTo<Array<T>>
{ static Array<T> from(const Value& v) { return coerceArray<T>(v); } };
template <typename T> struct CoerceTo<Set<T>>
{ static Set<T> from(const Value& v) { return coerceSet<T>(v); } };
template <typename K, typename V> struct CoerceTo<Map<K, V>>
{ static Map<K, V> from(const Value& v) { return coerceMap<K, V>(v); } };

template <auto M> struct SlotAccess;
template <class C, class T, T C::*M>
struct SlotAccess<M>
{
    static Value get(const HorizonCode::CompiledInstance* s)
    { return toValue(static_cast<const C*>(s)->*M); }          // ADL finds struct converters
    static void set(HorizonCode::CompiledInstance* s, const Value& v)
    { static_cast<C*>(s)->*M = CoerceTo<T>::from(v); }
};

template <auto M>
inline VarSlot slot(const char* name, PinType type, bool isArray, int access,
                    const char* typeName, Value def,
                    ContainerKind container = ContainerKind::None,
                    PinType keyType = PinType::String)
{
    return VarSlot{ name, type, isArray, access, typeName, std::move(def),
                    &SlotAccess<M>::get, &SlotAccess<M>::set, container, keyType };
}

// Enum members are plain ints in C++, so the Value coming back out has to be
// re-stamped with the identity the declaration carries. Scalars get the
// definition path; ARRAYS deliberately do not — the interpreter's own array
// builders (variableDefaultValue, ArrayAdd) leave it empty too.
inline Value slotRead(const VarSlot& s, const HorizonCode::CompiledInstance* self)
{
    Value v = s.get(self);
    if (s.type != PinType::Enum) return v;
    v.type = PinType::Enum;
    // Array payloads carry no definition path — neither do the interpreter's
    // (variableDefaultValue, ArrayAdd) — so normalize both ways.
    if (v.isArray) for (Value& it : v.items) { it.type = PinType::Enum; it.typeName.clear(); }
    else           v.typeName = s.typeName;
    return v;
}
inline void slotWrite(const VarSlot& s, HorizonCode::CompiledInstance* self, const Value& v)
{
    // coerce(v, Enum) is NOT coerce(v, Int): a Bool converts into an int but not
    // into an enum (§3.3). Normalize first, then the member's own coerce<int>
    // passes the value straight through.
    if (s.type == PinType::Enum && !s.isArray) s.set(self, Value::ofInt(coerceEnum(v)));
    else                                       s.set(self, v);
}

using VarSlots = std::vector<VarSlot>;

inline const VarSlot* findSlot(const VarSlots& slots, const std::string& name)
{
    for (const VarSlot& s : slots) if (name == s.name) return &s;
    return nullptr;
}
inline Value getVar(const VarSlots& slots, const HorizonCode::CompiledInstance* self,
                    const std::string& name)
{
    const VarSlot* s = findSlot(slots, name);
    return s ? slotRead(*s, self) : Value{};
}
inline bool setVar(const VarSlots& slots, HorizonCode::CompiledInstance* self,
                   const std::string& name, const Value& v)
{
    const VarSlot* s = findSlot(slots, name);
    if (!s) return false;                      // unknown → the Runtime's overflow store
    slotWrite(*s, self, v);
    return true;
}
inline void reseedVars(const VarSlots& slots, HorizonCode::CompiledInstance* self)
{ for (const VarSlot& s : slots) slotWrite(s, self, s.def); }
// The GC marks Ref-TYPED entries only — it does not look inside structs, so
// neither does this (Runtime::retainOnlyReachableFrom).
inline void collectVarRefs(const VarSlots& slots, const HorizonCode::CompiledInstance* self,
                           std::vector<uint32_t>& out)
{
    for (const VarSlot& s : slots)
    {
        // A map's KEYS are typed separately from its values (`type` is the value
        // side), so an object held only as a key needs its own look — matching
        // Runtime::retainOnlyReachableFrom on the interpreted side.
        const bool refKeys = s.kind() == ContainerKind::Map && s.keyType == PinType::Ref;
        if (s.type != PinType::Ref && !refKeys) continue;
        const Value v = s.get(self);
        if (refKeys)
            for (const Value& k : v.keys) if (k.ref != 0u) out.push_back(k.ref);
        if (s.type != PinType::Ref) continue;
        if (!v.isArray) { if (v.ref != 0u) out.push_back(v.ref); }
        else for (const Value& it : v.items) if (it.ref != 0u) out.push_back(it.ref);
    }
}
// A derived class's table: its base's entries followed by its own. The base's
// entries keep accessors typed on the BASE (SlotAccess static_casts the
// CompiledInstance* down to it), which is exactly right for a derived object —
// single, non-virtual inheritance, so the base sub-object is right there.
//
// Base first, and the names cannot repeat: the editor refuses a variable whose
// name an ancestor already uses, and the generator refuses to compile a class
// that carries one anyway. That is what makes ONE table stand for the whole
// chain the way the interpreter's single variable store does.
inline VarSlots concatSlots(const VarSlots& base, VarSlots own)
{
    VarSlots out = base;
    out.insert(out.end(), std::make_move_iterator(own.begin()),
               std::make_move_iterator(own.end()));
    return out;
}

inline std::vector<HorizonCode::CompiledVarInfo> varInfosOf(const VarSlots& slots)
{
    std::vector<HorizonCode::CompiledVarInfo> out;
    out.reserve(slots.size());
    for (const VarSlot& s : slots) out.push_back({ s.name, s.type, s.isArray, s.access });
    return out;
}

// ── the compiled-to-compiled call path ──────────────────────────────────────
// A HorizonCode object reference is a Runtime HANDLE, not a pointer, so calling
// a function on another instance always costs one resolve. What it does NOT
// have to cost is the rest of the seam — a name lookup, an access scan, and a
// std::vector<Value> for the arguments and another for the results. `as` turns
// the handle into a typed pointer when the target really is that compiled class,
// and null otherwise: reference 0, a destroyed instance, a DIFFERENT class (a
// Ref variable's declared className is editor metadata, never enforced), or an
// interpreted target. Generated code falls back to the seam on null, which is
// what keeps mixed compiled/interpreted populations behaving identically.
//
// EXACT, deliberately. A generated class derives from its base class in C++, so
// an instance of a derived class IS a T when T is one of its ancestors — but the
// tag is one address per class and says nothing about ancestry, so `as` answers
// null there and the caller takes the seam. That is correct, only slower; making
// it hit would mean carrying a per-class key comparison here, which is the
// Cast node's job (castClass) and not worth putting in every direct call.
template <typename T> inline T* as(const Context& c, uint32_t target)
{
    if (target == 0u || !c.resolveCompiled) return nullptr;
    HorizonCode::CompiledInstance* i = c.resolveCompiled(target);
    return (i && i->classTag() == T::classTag_()) ? static_cast<T*>(i) : nullptr;
}

// ── the Cast node's two lowerings ────────────────────────────────────────────
// Which one the generator emits depends on OnFailure (HcCodegen.h):
//
//   Interpret — the per-asset hybrid. Interpreted instances can live in the
//     same run, so the cast has to go through the Context seam, which lands on
//     the very same Runtime::instanceIsA the interpreter asks. Routing it
//     through resolveCompiled/classTag instead would answer differently for a
//     base class and for an interpreted target, and the parity harness would be
//     comparing two languages.
//
//   Stop — every class is compiled, so by construction there is no interpreted
//     instance to be compatible with. The generator then emits castClass for a
//     HorizonCode class (its own key plus its chain — a class may be an
//     ancestor of the reference's, which one exact tag cannot answer) and
//     castBase for an engine base class, which still walks the chain but
//     reaches the instance directly instead of through the Runtime's map.
//
// Both return the reference on success and 0 on failure, so the caller's shape
// — `if (rs.castN != 0u)` — is identical either way.
inline uint32_t castRef(const Context& c, uint32_t ref, const char* classKey)
{
    return (ref != 0u && c.isA && c.isA(ref, classKey)) ? ref : 0u;
}
// A HorizonCode class target under Stop. Not hc::as: with class inheritance the
// target may be an ANCESTOR of what the reference holds, and classTag is one
// exact address per class. Reads the instance's own key and its baked chain —
// still no Runtime map lookup, still no string built at the call site.
inline uint32_t castClass(const Context& c, uint32_t ref, const char* classKey)
{
    if (ref == 0u || !c.resolveCompiled) return 0u;
    HorizonCode::CompiledInstance* i = c.resolveCompiled(ref);
    if (!i) return 0u;
    const char* own = i->classKey();
    if (own && std::strcmp(own, classKey) == 0) return ref;
    for (const char* a : i->classChain())
        if (a && std::strcmp(a, classKey) == 0) return ref;
    return 0u;
}
inline uint32_t castBase(const Context& c, uint32_t ref, const char* baseClass)
{
    if (ref == 0u || !c.resolveCompiled) return 0u;
    HorizonCode::CompiledInstance* i = c.resolveCompiled(ref);
    if (!i) return 0u;
    const char* base = i->baseClassKey();
    return HorizonCode::engineClassIsA(base ? base : "", baseClass) ? ref : 0u;
}

// The GameInstance is the one reference whose class is known while generating
// ("__game_instance__"), and the Runtime already holds the object — so this
// costs neither a handle lookup nor an id round trip. It is NOT guaranteed to
// exist (a project may have none, and it is null before startup sets one and
// again after it is removed), hence the same checked shape as `as`.
template <typename T> inline T* gameInstanceAs(const Context& c)
{
    if (!c.gameInstanceCompiled) return nullptr;
    HorizonCode::CompiledInstance* i = c.gameInstanceCompiled();
    return (i && i->classTag() == T::classTag_()) ? static_cast<T*>(i) : nullptr;
}

// ── run guards (§3.6, sharpened) ─────────────────────────────────────────────
HE_API void warnStepLimit();   // the interpreter's step-limit warning text
constexpr int kMaxSteps = 4096;
constexpr int kMaxDepth = 64;

} // namespace hc

// One step per executed statement; on overrun set the aborted flag, warn once,
// and unwind (every generated body function checks this first).
#define HC_STEP(rs) \
    do { \
        if ((rs).aborted) return; \
        if (++(rs).steps > hc::kMaxSteps) { (rs).aborted = true; hc::warnStepLimit(); return; } \
    } while (0)
