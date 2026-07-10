#pragma once
#include <Types/Defines.h>
#include "HorizonCode.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// ── hc:: — the runtime support library for GENERATED HorizonCode C++ ─────────
// Included by every class the HorizonCode → C++ codegen emits (and by the parity
// tests). Each helper reproduces one clause of the interpreter's semantic
// contract (docs/horizoncode-cpp-codegen-implementation-plan.md §3) — the
// comments below cite the clause they mirror. Nothing here may consult wall
// clock or locale: generated code must be deterministic.

namespace hc {

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
inline float coerceFloat(const Value& v)
{
    if (v.isArray || v.type == PinType::Float) return v.f;
    if (v.type == PinType::Bool) return v.b ? 1.0f : 0.0f;
    if (v.type == PinType::Int)  return (float)v.i;
    return 0.0f;
}
inline int coerceInt(const Value& v)
{
    if (v.isArray || v.type == PinType::Int) return v.i;
    if (v.type == PinType::Float) return (int)v.f;
    if (v.type == PinType::Bool)  return v.b ? 1 : 0;
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
inline glm::vec4 coerceColor(const Value& v)
{ return (v.isArray || v.type == PinType::Color) ? v.col : zeroOf<glm::vec4>(); }
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
template <> inline uint32_t    coerce<uint32_t>(const Value& v)    { return coerceRef(v); }
template <> inline Transform   coerce<Transform>(const Value& v)   { return coerceTransform(v); }

// ── math / logic / string (§3.4) ─────────────────────────────────────────────
inline bool feq(float a, float b) { return std::fabs(a - b) < 1e-6f; }
// And/Or must evaluate BOTH sides (the interpreter has no short-circuit; §5.5).
inline bool land(bool a, bool b) { return a && b; }
inline bool lor(bool a, bool b)  { return a || b; }
HE_API std::string toStringG(float v);   // snprintf "%g", buffer 48 — like ToString

// ── arrays (§3.4: pure copy semantics, clamped ops, exact-equality search) ───
HE_API void warnArrayGet(int idx, size_t size);   // the interpreter's out-of-range log
template <typename T> inline T arrGet(const Array<T>& a, int idx)
{
    if (idx >= 0 && idx < (int)a.size()) return a[(size_t)idx];
    warnArrayGet(idx, a.size());
    return zeroOf<T>();
}
template <typename T> inline Array<T> arrAdd(Array<T> a, const T& v)
{ a.push_back(v); return a; }
template <typename T> inline Array<T> arrSet(Array<T> a, int idx, const T& v)
{ if (idx >= 0 && idx < (int)a.size()) a[(size_t)idx] = v; return a; }
template <typename T> inline Array<T> arrInsert(Array<T> a, int idx, const T& v)
{
    if (idx < 0) idx = 0;
    if (idx > (int)a.size()) idx = (int)a.size();
    a.insert(a.begin() + idx, v);
    return a;
}
template <typename T> inline Array<T> arrRemove(Array<T> a, int idx)
{ if (idx >= 0 && idx < (int)a.size()) a.erase(a.begin() + idx); return a; }
template <typename T> inline bool arrContains(const Array<T>& a, const T& key)
{
    for (const T& e : a) if (e == key) return true;   // exact equality, like valueEquals
    return false;
}
template <typename T> inline int arrIndexOf(const Array<T>& a, const T& key)
{
    for (size_t i = 0; i < a.size(); ++i) if (a[i] == key) return (int)i;
    return -1;
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
HE_API uint32_t createObject(const Context& c, const char* classPath); // logs the fail like :1019
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
inline std::vector<Value> callExternal(const Context& c, uint32_t target, const char* fn,
                                       const std::vector<Value>& args)
{ return c.callExternal ? c.callExternal(target, fn, args) : std::vector<Value>{}; }
inline std::vector<Value> callApi(const Context& c, const char* id, const std::vector<Value>& args)
{ return c.callApi ? c.callApi(id, args) : std::vector<Value>{}; }
inline uint32_t self(const Context& c)         { return c.getSelf ? c.getSelf().ref : 0u; }
inline uint32_t gameInstance(const Context& c) { return c.getGameInstance ? c.getGameInstance().ref : 0u; }
inline void scheduleResume(const Context& c, int nodeId, float seconds)
{ if (c.scheduleResume) c.scheduleResume(nodeId, seconds); }
inline bool isValidRef(const Context& c, uint32_t target)
{ return c.isValid && c.isValid(target); }
HE_API void print(const std::string& s);       // "[Widget] " + Logger Info, like Print

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
