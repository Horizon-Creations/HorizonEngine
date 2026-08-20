#pragma once
// The HorizonCode → C++ parity fixtures (plan §10.3): each graph exercises one
// clause of the semantic contract. Built programmatically so the interpreter
// (tests) and the generator (hc_codegen at build time) consume the IDENTICAL
// graph — both after a toJson/fromJson round trip, like production assets.
//
// Shared by tools/hc_codegen (build-time generation) and
// tests/test_horizoncode_codegen.cpp (the parity harness). Header-only.

#include <HorizonScene/HcCodegen.h>
#include <HorizonScene/EngineApi.h>
#include <Types/TypeRegistry.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace hcfix {

using HorizonCode::Graph;
using HorizonCode::Node;
using HorizonCode::NodeType;
using HorizonCode::PinType;
using HorizonCode::Value;
using HorizonCode::Variable;
using NT = NodeType;
using PT = PinType;

// ── user-defined types (the Enum/Struct fixtures) ────────────────────────────
// Registered in the process-global TypeRegistry, which is what BOTH consumers
// read: hc_codegen at build time (resolving entries/fields while it emits) and
// he_tests at run time (where the interpreter resolves the same names). They are
// registered directly rather than loaded from .hasset files so the two stay
// definitionally identical without a ContentManager in the loop.
inline const char* kMoodEnum  = "fix/Types/Mood.hasset";
inline const char* kDupEnum   = "fix/Types/Dup.hasset";
inline const char* kInnerType = "fix/Types/Inner.hasset";
inline const char* kStatsType = "fix/Types/Stats.hasset";

inline void registerTypes()
{
    auto& reg = HE::TypeRegistry::instance();

    HE::EnumDef mood;
    mood.name = "FixMood"; mood.assetPath = kMoodEnum;
    // Deliberately sparse and non-contiguous: entry VALUES (not indices) drive
    // both the interpreter's routing and the generated switch.
    mood.entries = { { "Calm", 0 }, { "Happy", 1 }, { "Angry", 5 } };
    reg.registerEnum(mood);

    // Two entries on the SAME value: nothing in the enum panel prevents it.
    // findValue answers with the first, so "B" is dead — and a generator that
    // emitted both would produce a duplicate `case` label, i.e. a .cpp that
    // does not compile. This fixture is that regression.
    HE::EnumDef dup;
    dup.name = "FixDup"; dup.assetPath = kDupEnum;
    dup.entries = { { "A", 1 }, { "B", 1 }, { "C", 2 } };
    reg.registerEnum(dup);

    auto field = [](const char* name, PT type, Value def, bool isArray = false,
                    const char* typeName = "")
    {
        HE::StructField f;
        f.name = name; f.type = type; f.isArray = isArray; f.typeName = typeName;
        f.defaultValue = std::move(def);
        return f;
    };

    HE::StructDef inner;
    inner.name = "FixInner"; inner.assetPath = kInnerType;
    inner.fields = { field("n", PT::Float, Value::ofFloat(2.5f)),
                     field("tag", PT::String, Value::ofString("in")) };
    reg.registerStruct(inner);

    // Every field kind that changes the lowering: scalar, enum (default by entry
    // NAME), nested struct, array with authored slots, and a Ref (which the GC
    // scan must NOT see through a struct — see collectRefs).
    Value hits; hits.isArray = true; hits.type = PT::Float;
    hits.items = { Value::ofFloat(1.0f), Value::ofFloat(2.0f) };
    HE::StructDef stats;
    stats.name = "FixStats"; stats.assetPath = kStatsType;
    stats.fields = { field("hp", PT::Float, Value::ofFloat(100.0f)),
                     field("lvl", PT::Int, Value::ofInt(3)),
                     field("mood", PT::Enum, Value::ofString("Happy"), false, kMoodEnum),
                     field("inner", PT::Struct, Value{}, false, kInnerType),
                     field("hits", PT::Float, hits, true),
                     field("owner", PT::Ref, Value::ofRef(0)) };
    reg.registerStruct(stats);
}

// ── wiring helpers (unified pin index space, like the interpreter) ───────────
struct Fx
{
    Graph g;

    static void must(bool ok, const char* what)
    {
        if (!ok) { std::fprintf(stderr, "hcodegen fixture: %s failed\n", what); std::abort(); }
    }

    int add(Node n) { return g.addNode(std::move(n)); }

    int pinBase(int id, int which) const   // 0 execIn, 1 execOut, 2 dataIn, 3 dataOut
    {
        const Node* n = g.findNode(id);
        must(n != nullptr, "findNode");
        const auto s = HorizonCode::signatureOf(*n);
        const int execOut0 = (int)s.execIns.size();
        const int dataIn0  = execOut0 + (int)s.execOuts.size();
        const int dataOut0 = dataIn0 + (int)s.dataIns.size();
        switch (which) { case 0: return 0; case 1: return execOut0; case 2: return dataIn0; default: return dataOut0; }
    }
    // exec: src's exec-out k → dst's exec-in 0.
    void exec(int src, int dst, int srcOut = 0)
    { must(g.connect(src, pinBase(src, 1) + srcOut, dst, pinBase(dst, 0)), "exec connect"); }
    // data: src's data-out srcIdx → dst's data-in dstIdx.
    void data(int src, int srcIdx, int dst, int dstIdx)
    { must(g.connect(src, pinBase(src, 3) + srcIdx, dst, pinBase(dst, 2) + dstIdx), "data connect"); }

    // ── node shorthands ──────────────────────────────────────────────────────
    int event(const std::string& name, int elem = 0, bool hasArg = false, PT argType = PT::Float)
    { Node n; n.type = NT::Event; n.s = name; n.elem = elem; n.hasArg = hasArg; n.propType = argType; return add(n); }
    int fnEntry(const std::string& name, int access,
                std::vector<HorizonCode::FuncParam> params,
                std::vector<HorizonCode::FuncParam> results)
    { Node n; n.type = NT::FunctionEntry; n.s = name; n.access = access;
      n.params = std::move(params); n.results = std::move(results); return add(n); }
    int fnCall(const std::string& name)
    { Node n; n.type = NT::FunctionCall; n.s = name; return add(n); }
    int fnReturn(const std::string& name)
    { Node n; n.type = NT::FunctionReturn; n.s = name; return add(n); }
    int setVar(const std::string& name, PT t, bool isArray = false)
    { Node n; n.type = NT::SetVariable; n.s = name; n.propType = t; n.isArray = isArray; return add(n); }
    int getVar(const std::string& name, PT t, bool isArray = false)
    { Node n; n.type = NT::GetVariable; n.s = name; n.propType = t; n.isArray = isArray; return add(n); }
    int constF(float v)      { Node n; n.type = NT::ConstFloat;  n.f[0] = v; return add(n); }
    int constI(int v)        { Node n; n.type = NT::ConstInt;    n.f[0] = (float)v; return add(n); }
    int constB(bool v)       { Node n; n.type = NT::ConstBool;   n.f[0] = v ? 1.0f : 0.0f; return add(n); }
    int constS(const std::string& v) { Node n; n.type = NT::ConstString; n.s = v; return add(n); }
    int op(NT t)             { Node n; n.type = t; return add(n); }
    int arrayOp(NT t, PT elem) { Node n; n.type = t; n.propType = elem; return add(n); }
    int forEach(PT elem)     { Node n; n.type = NT::ForEach; n.propType = elem; return add(n); }
    int branch()             { return op(NT::Branch); }
    int sequence()           { return op(NT::Sequence); }
    // EngineCall with pins mirrored from the HE::api registry (like the editor).
    int engineCall(const std::string& id)
    {
        const HE::api::ApiFn* fn = HE::api::find(id);
        must(fn != nullptr, "engine api id");
        Node n; n.type = NT::EngineCall; n.s = id; n.hasArg = fn->isExec;
        for (const auto& p : fn->params)  n.params.push_back({ p.name, p.type, p.isArray });
        for (const auto& r : fn->results) n.results.push_back({ r.name, r.type, r.isArray });
        return add(n);
    }

    // ── user-defined-type nodes ──────────────────────────────────────────────
    // Pins are mirrored from the definition at construction, exactly like the
    // editor does — connect() checks pin types, so the mirror must be right
    // BEFORE anything is wired (done()'s round trip re-syncs it afterwards).
    int getVarT(const std::string& name, PT t, const std::string& typeName, bool isArray = false)
    { Node n; n.type = NT::GetVariable; n.s = name; n.propType = t; n.typeName = typeName;
      n.isArray = isArray; return add(n); }
    int setVarT(const std::string& name, PT t, const std::string& typeName, bool isArray = false)
    { Node n; n.type = NT::SetVariable; n.s = name; n.propType = t; n.typeName = typeName;
      n.isArray = isArray; return add(n); }
    int constEnum(const std::string& typeName, int value)
    { Node n; n.type = NT::ConstEnum; n.typeName = typeName; n.f[0] = (float)value; return add(n); }
    int enumOp(NT t, const std::string& typeName)
    { Node n; n.type = t; n.typeName = typeName; return add(n); }
    int switchEnum(const std::string& typeName)
    {
        HE::EnumDef def;
        must(HE::TypeRegistry::instance().getEnum(typeName, def), "enum def");
        Node n; n.type = NT::SwitchOnEnum; n.typeName = typeName;
        for (const auto& e : def.entries) n.params.push_back({ e.name, PT::Exec, false, {} });
        return add(n);
    }
    static std::vector<HorizonCode::FuncParam> structParams(const std::string& typeName)
    {
        HE::StructDef def;
        must(HE::TypeRegistry::instance().getStruct(typeName, def), "struct def");
        std::vector<HorizonCode::FuncParam> ps;
        for (const auto& f : def.fields) ps.push_back({ f.name, f.type, f.isArray, f.typeName });
        return ps;
    }
    int structOp(NT t, const std::string& typeName)
    { Node n; n.type = t; n.typeName = typeName; n.params = structParams(typeName); return add(n); }
    int fieldOp(NT t, const std::string& typeName, const std::string& fieldName)
    {
        HE::StructDef def;
        must(HE::TypeRegistry::instance().getStruct(typeName, def), "struct def");
        const HE::StructField* f = def.findField(fieldName);
        must(f != nullptr, "struct field");
        Node n; n.type = t; n.typeName = typeName;
        n.params = { { f->name, f->type, f->isArray, f->typeName } };
        return add(n);
    }
    int arrayOpT(NT t, PT elem, const std::string& typeName)
    { Node n; n.type = t; n.propType = elem; n.typeName = typeName; return add(n); }

    void enumVar(const std::string& name, const std::string& typeName,
                 const std::string& defaultEntry)
    { Variable v; v.name = name; v.type = PT::Enum; v.typeName = typeName; v.s = defaultEntry;
      g.variables.push_back(std::move(v)); }
    void structVar(const std::string& name, const std::string& typeName,
                   std::unordered_map<std::string, Value> overrides = {})
    { Variable v; v.name = name; v.type = PT::Struct; v.typeName = typeName;
      v.structDefaults = std::move(overrides); g.variables.push_back(std::move(v)); }
    void enumArrVar(const std::string& name, const std::string& typeName)
    { Variable v; v.name = name; v.type = PT::Enum; v.typeName = typeName; v.isArray = true;
      g.variables.push_back(std::move(v)); }
    void structArrVar(const std::string& name, const std::string& typeName)
    { Variable v; v.name = name; v.type = PT::Struct; v.typeName = typeName; v.isArray = true;
      g.variables.push_back(std::move(v)); }

    void var(const std::string& name, PT t, float f0 = 0.0f, const std::string& s = {},
             int access = 0, int scope = 0)
    {
        Variable v; v.name = name; v.type = t; v.f[0] = f0; v.s = s;
        v.access = access; v.scope = scope;
        g.variables.push_back(std::move(v));
    }
    void arrVar(const std::string& name, PT t, std::vector<Value> items, int scope = 0)
    {
        Variable v; v.name = name; v.type = t; v.isArray = true;
        v.defaultItems = std::move(items); v.scope = scope;
        g.variables.push_back(std::move(v));
    }

    // ── Set / Map ────────────────────────────────────────────────────────────
    // Both flags are always set together — see ContainerKind in HorizonCode.h.
    using CK = HorizonCode::ContainerKind;
    void setVarDecl(const std::string& name, PT t, std::vector<Value> items = {},
                    const std::string& tn = {})
    {
        Variable v; v.name = name; v.type = t; v.isArray = true; v.container = CK::Set;
        v.typeName = tn; v.defaultItems = std::move(items);
        g.variables.push_back(std::move(v));
    }
    void mapVarDecl(const std::string& name, PT keyT, PT valT,
                    std::vector<Value> keys = {}, std::vector<Value> values = {},
                    const std::string& keyTn = {}, const std::string& tn = {})
    {
        Variable v; v.name = name; v.type = valT; v.isArray = true; v.container = CK::Map;
        v.keyType = keyT; v.keyTypeName = keyTn; v.typeName = tn;
        v.defaultKeys = std::move(keys); v.defaultItems = std::move(values);
        g.variables.push_back(std::move(v));
    }
    int setOp(NT t, PT elem, const std::string& tn = {})
    { Node n; n.type = t; n.propType = elem; n.typeName = tn;
      n.isArray = true; n.container = CK::Set; return add(n); }
    int mapOp(NT t, PT keyT, PT valT, const std::string& keyTn = {}, const std::string& tn = {})
    { Node n; n.type = t; n.propType = valT; n.typeName = tn;
      n.isArray = true; n.container = CK::Map;
      n.keyType = keyT; n.keyTypeName = keyTn; return add(n); }
    int getSetVar(const std::string& name, PT elem, const std::string& tn = {})
    { Node n; n.type = NT::GetVariable; n.s = name; n.propType = elem; n.typeName = tn;
      n.isArray = true; n.container = CK::Set; return add(n); }
    int setSetVar(const std::string& name, PT elem, const std::string& tn = {})
    { Node n; n.type = NT::SetVariable; n.s = name; n.propType = elem; n.typeName = tn;
      n.isArray = true; n.container = CK::Set; return add(n); }
    int getMapVar(const std::string& name, PT keyT, PT valT,
                  const std::string& keyTn = {}, const std::string& tn = {})
    { Node n; n.type = NT::GetVariable; n.s = name; n.propType = valT; n.typeName = tn;
      n.isArray = true; n.container = CK::Map; n.keyType = keyT; n.keyTypeName = keyTn;
      return add(n); }
    int setMapVar(const std::string& name, PT keyT, PT valT,
                  const std::string& keyTn = {}, const std::string& tn = {})
    { Node n; n.type = NT::SetVariable; n.s = name; n.propType = valT; n.typeName = tn;
      n.isArray = true; n.container = CK::Map; n.keyType = keyT; n.keyTypeName = keyTn;
      return add(n); }
    int forEachSet(PT elem, const std::string& tn = {})
    { Node n; n.type = NT::ForEachSet; n.propType = elem; n.typeName = tn;
      n.isArray = true; n.container = CK::Set; return add(n); }
    int forEachMap(PT keyT, PT valT, const std::string& keyTn = {}, const std::string& tn = {})
    { Node n; n.type = NT::ForEachMap; n.propType = valT; n.typeName = tn;
      n.isArray = true; n.container = CK::Map; n.keyType = keyT; n.keyTypeName = keyTn;
      return add(n); }

    HE::hccg::ClassSource doneKey(const std::string& key, const std::string& name)
    {
        HorizonCode::syncFunctionSignatures(g);
        Graph rt;
        must(HorizonCode::fromJson(HorizonCode::toJson(g), rt), "json round trip");
        return { key, name, std::move(rt) };
    }

    // `baseClass` is the engine taxonomy row the class derives from (empty =
    // Object). It reaches the generator as ClassSource::baseClass, which emits
    // baseClassKey() — what a Cast to a BASE class resolves against.
    HE::hccg::ClassSource done(const std::string& name, const std::string& baseClass = {})
    {
        HorizonCode::syncFunctionSignatures(g);
        // Production graphs arrive through fromJson — round-trip so both
        // backends consume exactly what a shipped asset would contain.
        Graph rt;
        must(HorizonCode::fromJson(HorizonCode::toJson(g), rt), "json round trip");
        return { "fix/" + name, name, std::move(rt), baseClass };
    }

    // A class that derives from ANOTHER fixture class. `chain` is what the
    // generator turns into real C++ inheritance, so `g` must hold this class's
    // OWN nodes only — what it inherits lives in the base's fixture.
    HE::hccg::ClassSource done(const std::string& name, const std::string& baseKey,
                               const std::string& engineBase)
    {
        HE::hccg::ClassSource s = done(name, engineBase);
        s.chain = { baseKey };
        return s;
    }

    // A Cast node pointed at `target`, with the output pin's readable name
    // mirrored on exactly as HcEditorUtil::setCastTarget does in the editor.
    int cast(const std::string& target)
    {
        Node n; n.type = NT::Cast; n.s = target;
        HorizonCode::FuncParam p;
        p.name = "As " + target;
        p.type = PT::Ref;
        n.params.push_back(std::move(p));
        return add(n);
    }
};

// 1 — flow_branch_sequence: nesting, both arms, sequence order (§3.4).
inline HE::hccg::ClassSource fxFlow()
{
    Fx f;
    f.var("trace", PT::String);
    f.var("flag", PT::Bool, 1.0f);

    const int ev = f.event("Run");
    const int seq = f.sequence();
    f.exec(ev, seq);
    // Then 0: trace += "A", then Branch(flag): trace += "T" / "F".
    const int setA = f.setVar("trace", PT::String);
    { const int cat = f.op(NT::Concat), g0 = f.getVar("trace", PT::String), c = f.constS("A");
      f.data(g0, 0, cat, 0); f.data(c, 0, cat, 1); f.data(cat, 0, setA, 0); }
    f.exec(seq, setA, 0);
    const int br = f.branch();
    { const int gf = f.getVar("flag", PT::Bool); f.data(gf, 0, br, 0); }
    f.exec(setA, br);
    const int setT = f.setVar("trace", PT::String);
    { const int cat = f.op(NT::Concat), g0 = f.getVar("trace", PT::String), c = f.constS("T");
      f.data(g0, 0, cat, 0); f.data(c, 0, cat, 1); f.data(cat, 0, setT, 0); }
    f.exec(br, setT, 0);
    const int setF = f.setVar("trace", PT::String);
    { const int cat = f.op(NT::Concat), g0 = f.getVar("trace", PT::String), c = f.constS("F");
      f.data(g0, 0, cat, 0); f.data(c, 0, cat, 1); f.data(cat, 0, setF, 0); }
    f.exec(br, setF, 1);
    // Then 1: trace += "B" (runs after the whole Then-0 chain).
    const int setB = f.setVar("trace", PT::String);
    { const int cat = f.op(NT::Concat), g0 = f.getVar("trace", PT::String), c = f.constS("B");
      f.data(g0, 0, cat, 0); f.data(c, 0, cat, 1); f.data(cat, 0, setB, 0); }
    f.exec(seq, setB, 1);

    // Toggle event so both arms get exercised.
    const int evT = f.event("SetFlag", 0, true, PT::Bool);
    const int setFlag = f.setVar("flag", PT::Bool);
    f.data(evT, 0, setFlag, 0);
    f.exec(evT, setFlag);
    return f.done("flow_branch_sequence");
}

// 2 — coerce_matrix: pin defaults + event args of every convertible and
// inconvertible pairing (§3.3 coerce).
inline HE::hccg::ClassSource fxCoerce()
{
    Fx f;
    f.var("fOut", PT::Float);
    f.var("iOut", PT::Int);
    f.var("bOut", PT::Bool);
    f.var("sOut", PT::String, 0.0f, "keep");

    const int ev = f.event("Defaults");
    // Bool(true) default on a Float pin → 1.0f.
    const int s1 = f.setVar("fOut", PT::Float);
    f.g.findNode(s1)->pinDefaults[0] = Value::ofBool(true);
    f.exec(ev, s1);
    // Float(3.9) default on an Int pin → 3 (C-cast truncation).
    const int s2 = f.setVar("iOut", PT::Int);
    f.g.findNode(s2)->pinDefaults[0] = Value::ofFloat(3.9f);
    f.exec(s1, s2);
    // Int(2) default on a Bool pin → true.
    const int s3 = f.setVar("bOut", PT::Bool);
    f.g.findNode(s3)->pinDefaults[0] = Value::ofInt(2);
    f.exec(s2, s3);
    // Float default on a String pin → the zero value "" (inconvertible).
    const int s4 = f.setVar("sOut", PT::String);
    f.g.findNode(s4)->pinDefaults[0] = Value::ofFloat(1.5f);
    f.exec(s3, s4);

    // Event-arg coercion (§3.3: Event data-out ← arg coerced to propType).
    const int evF = f.event("ArgF", 0, true, PT::Float);
    const int sf = f.setVar("fOut", PT::Float);
    f.data(evF, 0, sf, 0);
    f.exec(evF, sf);
    const int evB = f.event("ArgB", 0, true, PT::Bool);
    const int sb = f.setVar("bOut", PT::Bool);
    f.data(evB, 0, sb, 0);
    f.exec(evB, sb);

    // ── Vector ↔ colour ──────────────────────────────────────────────────────
    // The rule that carries graphs authored while Color WAS the vec3 type: links
    // are restored from JSON without re-checking pin types, so a legacy Color
    // wire lands on a Vec3 pin and only coerce makes it right. The pad follows
    // the TARGET — a vector's fourth component is 0, a colour's is 1 — so both
    // widening directions are pinned here, in both backends.
    {
        Variable v3; v3.name = "v3FromColor"; v3.type = PT::Vec3; f.g.variables.push_back(v3);
        Variable v4; v4.name = "v4FromVec3";  v4.type = PT::Vec4; f.g.variables.push_back(v4);
        Variable cl; cl.name = "colorFromVec3"; cl.type = PT::Color; f.g.variables.push_back(cl);
        Variable v3s; v3s.name = "v3FromVec4"; v3s.type = PT::Vec3; f.g.variables.push_back(v3s);
        Variable v3z; v3z.name = "v3FromFloat"; v3z.type = PT::Vec3; f.g.variables.push_back(v3z);

        const int evV = f.event("Vectors");
        int prev = evV;
        auto setFromDefault = [&](const char* name, PT t, const Value& literal)
        {
            const int s = f.setVar(name, t);
            f.g.findNode(s)->pinDefaults[0] = literal;
            f.exec(prev, s);
            prev = s;
        };
        // Color(1,2,3,4) on a Vec3 pin → (1,2,3), w dropped.
        setFromDefault("v3FromColor", PT::Vec3, Value::ofColor({ 1.0f, 2.0f, 3.0f, 4.0f }));
        // Vec3(1,2,3) on a Vec4 pin → w padded with 0 (a direction has no w).
        setFromDefault("v4FromVec3", PT::Vec4, Value::ofVec3({ 1.0f, 2.0f, 3.0f }));
        // Vec3(1,2,3) on a Color pin → alpha padded with 1 (opaque).
        setFromDefault("colorFromVec3", PT::Color, Value::ofVec3({ 1.0f, 2.0f, 3.0f }));
        // Vec4(5,6,7,8) on a Vec3 pin → (5,6,7).
        setFromDefault("v3FromVec4", PT::Vec3, Value::ofVec4({ 5.0f, 6.0f, 7.0f, 8.0f }));
        // Anything else on a Vec3 pin → the null vector, like every mismatch.
        setFromDefault("v3FromFloat", PT::Vec3, Value::ofFloat(9.0f));
    }
    return f.done("coerce_matrix");
}

// 3 — math_ops: all operators incl. divide-by-zero, Equals epsilon, ToString.
inline HE::hccg::ClassSource fxMath()
{
    Fx f;
    for (const char* v : { "add", "sub", "mul", "d0", "d1" }) f.var(v, PT::Float);
    for (const char* v : { "gt", "lt", "eq", "lg" }) f.var(v, PT::Bool);
    f.var("str", PT::String);

    const int ev = f.event("Calc");
    int prev = ev;
    auto chainSet = [&](const std::string& var, PT t, int valueNode, int valueOut)
    {
        const int s = f.setVar(var, t);
        f.data(valueNode, valueOut, s, 0);
        f.exec(prev, s);
        prev = s;
    };
    { const int n = f.op(NT::Add);      f.data(f.constF(2.5f), 0, n, 0); f.data(f.constF(0.5f), 0, n, 1); chainSet("add", PT::Float, n, 0); }
    { const int n = f.op(NT::Subtract); f.data(f.constF(2.0f), 0, n, 0); f.data(f.constF(5.5f), 0, n, 1); chainSet("sub", PT::Float, n, 0); }
    { const int n = f.op(NT::Multiply); f.data(f.constF(3.0f), 0, n, 0); f.data(f.constF(1.5f), 0, n, 1); chainSet("mul", PT::Float, n, 0); }
    { const int n = f.op(NT::Divide);   f.data(f.constF(5.0f), 0, n, 0); f.data(f.constF(0.0f), 0, n, 1); chainSet("d0", PT::Float, n, 0); }
    { const int n = f.op(NT::Divide);   f.data(f.constF(5.0f), 0, n, 0); f.data(f.constF(2.0f), 0, n, 1); chainSet("d1", PT::Float, n, 0); }
    { const int n = f.op(NT::Greater);  f.data(f.constF(2.0f), 0, n, 0); f.data(f.constF(1.0f), 0, n, 1); chainSet("gt", PT::Bool, n, 0); }
    { const int n = f.op(NT::Less);     f.data(f.constF(2.0f), 0, n, 0); f.data(f.constF(1.0f), 0, n, 1); chainSet("lt", PT::Bool, n, 0); }
    { const int n = f.op(NT::Equals);   f.data(f.constF(0.3000001f), 0, n, 0); f.data(f.constF(0.3f), 0, n, 1); chainSet("eq", PT::Bool, n, 0); }
    { // And(Not(false), Or(false, true)) — both sides always evaluated.
      const int a = f.op(NT::And), nt = f.op(NT::Not), o = f.op(NT::Or);
      f.data(f.constB(false), 0, nt, 0);
      f.data(f.constB(false), 0, o, 0); f.data(f.constB(true), 0, o, 1);
      f.data(nt, 0, a, 0); f.data(o, 0, a, 1);
      chainSet("lg", PT::Bool, a, 0); }
    { // Concat(ToString(3.5), "x") → "3.5x" (%g formatting).
      const int ts = f.op(NT::ToString), c = f.op(NT::Concat);
      f.data(f.constF(3.5f), 0, ts, 0);
      f.data(ts, 0, c, 0); f.data(f.constS("x"), 0, c, 1);
      chainSet("str", PT::String, c, 0); }
    return f.done("math_ops");
}

// 3b — vector_ops: Make/Break Vector at all three widths, plus the angle
// conversions. The point of this fixture is the codegen: expr()'s default case
// emits a zero literal, so a node with no case there compiles SILENTLY to 0 and
// diverges from the interpreter with no error and no fallback. Only a parity
// run catches that.
inline HE::hccg::ClassSource fxVectorOps()
{
    Fx f;
    for (const char* v : { "x2", "y2", "x3", "y3", "z3", "x4", "y4", "z4", "w4",
                           "w3drop", "rad", "deg", "roundTrip",
                           "len3", "dist3", "dot", "nx", "cz" })
        f.var(v, PT::Float);

    const int ev = f.event("Build");
    int prev = ev;
    auto chainSet = [&](const std::string& var, int src, int srcOut)
    {
        const int s = f.setVar(var, PT::Float);
        f.data(src, srcOut, s, 0);
        f.exec(prev, s);
        prev = s;
    };

    // Vec2 round trip.
    {
        const int mk = f.op(NT::MakeVector2);
        f.data(f.constF(1.5f), 0, mk, 0);
        f.data(f.constF(-2.5f), 0, mk, 1);
        const int bk = f.op(NT::BreakVector2);
        f.data(mk, 0, bk, 0);
        chainSet("x2", bk, 0);
        chainSet("y2", bk, 1);
    }
    // Vector 3 → Break 3. Also read pin 3 (W) off a Break 4 fed by Make 3, to
    // pin down that Make Vector 3 really leaves W at zero in BOTH backends.
    {
        const int mk = f.op(NT::MakeVector3);
        f.data(f.constF(3.0f),  0, mk, 0);
        f.data(f.constF(4.0f),  0, mk, 1);
        f.data(f.constF(-5.0f), 0, mk, 2);
        const int bk = f.op(NT::BreakVector3);
        f.data(mk, 0, bk, 0);
        chainSet("x3", bk, 0);
        chainSet("y3", bk, 1);
        chainSet("z3", bk, 2);

        const int bk4 = f.op(NT::BreakVector4);
        f.data(mk, 0, bk4, 0);
        chainSet("w3drop", bk4, 3);
    }
    // Vector 4 round trip.
    {
        const int mk = f.op(NT::MakeVector4);
        f.data(f.constF(0.25f), 0, mk, 0);
        f.data(f.constF(0.5f),  0, mk, 1);
        f.data(f.constF(0.75f), 0, mk, 2);
        f.data(f.constF(1.25f), 0, mk, 3);
        const int bk = f.op(NT::BreakVector4);
        f.data(mk, 0, bk, 0);
        chainSet("x4", bk, 0);
        chainSet("y4", bk, 1);
        chainSet("z4", bk, 2);
        chainSet("w4", bk, 3);
    }
    // Angle conversion, both directions, and degrees(radians(x)) back to x.
    {
        const int r = f.engineCall("math.radians");
        f.g.findNode(r)->pinDefaults[0] = Value::ofFloat(180.0f);
        chainSet("rad", r, 0);

        const int d = f.engineCall("math.degrees");
        f.g.findNode(d)->pinDefaults[0] = Value::ofFloat(3.14159265f);
        chainSet("deg", d, 0);

        const int r2 = f.engineCall("math.radians");
        f.g.findNode(r2)->pinDefaults[0] = Value::ofFloat(57.5f);
        const int d2 = f.engineCall("math.degrees");
        f.data(r2, 0, d2, 0);
        chainSet("roundTrip", d2, 0);
    }
    // Vector maths, fed from Make Vector 3 so the whole chain — build a vector,
    // hand it to an engine call, read a float back — runs in both backends.
    {
        auto mk3 = [&](float x, float y, float z)
        {
            const int n = f.op(NT::MakeVector3);
            f.data(f.constF(x), 0, n, 0);
            f.data(f.constF(y), 0, n, 1);
            f.data(f.constF(z), 0, n, 2);
            return n;
        };
        const int a = mk3(3.0f, 4.0f, 0.0f);     // length 5
        const int b = mk3(0.0f, 0.0f, 2.0f);

        const int len = f.engineCall("math.length3");
        f.data(a, 0, len, 0);
        chainSet("len3", len, 0);

        const int dist = f.engineCall("math.distance3");
        f.data(a, 0, dist, 0); f.data(b, 0, dist, 1);
        chainSet("dist3", dist, 0);

        const int dot = f.engineCall("math.dot3");
        f.data(a, 0, dot, 0); f.data(b, 0, dot, 1);
        chainSet("dot", dot, 0);

        // normalize3 returns a Vec3 → straight back through a Break Vector 3.
        const int nrm = f.engineCall("math.normalize3");
        f.data(a, 0, nrm, 0);
        const int bn = f.op(NT::BreakVector3);
        f.data(nrm, 0, bn, 0);
        chainSet("nx", bn, 0);                   // 3/5

        // cross((3,4,0), (0,0,2)) = (8, -6, 0)
        const int crs = f.engineCall("math.cross");
        f.data(a, 0, crs, 0); f.data(b, 0, crs, 1);
        const int bc = f.op(NT::BreakVector3);
        f.data(crs, 0, bc, 0);
        chainSet("cz", bc, 1);                   // y = -6
    }
    return f.done("vector_ops");
}

// 4 — variables: defaults of every type + arrays, set/get, pass-through
// data-out, undeclared-set-then-get (§3.4).
inline HE::hccg::ClassSource fxVariables()
{
    Fx f;
    f.var("f", PT::Float, 1.5f);
    f.var("b", PT::Bool, 1.0f);
    f.var("i", PT::Int, 7.0f);
    f.var("s", PT::String, 0.0f, "hi", /*access=*/1);
    { Variable v; v.name = "v2"; v.type = PT::Vec2; v.f[0] = 1; v.f[1] = 2; f.g.variables.push_back(v); }
    { Variable v; v.name = "col"; v.type = PT::Color; v.f[0] = 0.1f; v.f[1] = 0.2f; v.f[2] = 0.3f; v.f[3] = 0.4f; f.g.variables.push_back(v); }
    f.var("r", PT::Ref);
    { Variable v; v.name = "xf"; v.type = PT::Transform;
      v.tpos = { 1, 2, 3 }; v.trot = { 4, 5, 6 }; v.tscl = { 7, 8, 9 }; f.g.variables.push_back(v); }
    f.arrVar("arrF", PT::Float, { Value::ofFloat(1), Value::ofFloat(2), Value::ofFloat(3) });
    f.arrVar("arrS", PT::String, { Value::ofString("a"), Value::ofString("b") });
    f.var("copied", PT::Float);
    f.var("ghostRead", PT::Int);

    const int ev = f.event("Mut");
    // f += 1.
    const int sf = f.setVar("f", PT::Float);
    { const int a = f.op(NT::Add); f.data(f.getVar("f", PT::Float), 0, a, 0);
      f.data(f.constF(1.0f), 0, a, 1); f.data(a, 0, sf, 0); }
    f.exec(ev, sf);
    // Pass-through: copied = (the Set's data-out re-evaluates the Value input).
    const int sCopy = f.setVar("copied", PT::Float);
    f.data(sf, 0, sCopy, 0);
    f.exec(sf, sCopy);
    // Undeclared name: Set "ghost" creates a store entry; Get reads it back.
    const int sGhost = f.setVar("ghost", PT::Int);
    f.g.findNode(sGhost)->pinDefaults[0] = Value::ofInt(42);
    f.exec(sCopy, sGhost);
    const int sRead = f.setVar("ghostRead", PT::Int);
    f.data(f.getVar("ghost", PT::Int), 0, sRead, 0);
    f.exec(sGhost, sRead);
    // Array mutation via copy semantics.
    const int sArr = f.setVar("arrF", PT::Float, true);
    { const int add = f.arrayOp(NT::ArrayAdd, PT::Float);
      f.data(f.getVar("arrF", PT::Float, true), 0, add, 0);
      f.data(f.constF(4.0f), 0, add, 1);
      f.data(add, 0, sArr, 0); }
    f.exec(sRead, sArr);
    return f.done("variables");
}

// 5 — functions_basic: params/results, returns in both branch arms, missing
// args, private + requirePublic (§3.1).
inline HE::hccg::ClassSource fxFunctionsBasic()
{
    Fx f;
    f.var("out", PT::Float);
    f.var("sec", PT::Float);

    // Sum(a, b) -> total
    const int sum = f.fnEntry("Sum", 0, { { "a", PT::Float }, { "b", PT::Float } },
                              { { "total", PT::Float } });
    const int sumRet = f.fnReturn("Sum");
    { const int a = f.op(NT::Add); f.data(sum, 0, a, 0); f.data(sum, 1, a, 1);
      HorizonCode::syncFunctionSignatures(f.g);   // return pins mirror the entry
      f.data(a, 0, sumRet, 0); }
    f.exec(sum, sumRet);

    // Pick(x) -> r: "pos" / "neg" via returns inside both branch arms.
    const int pick = f.fnEntry("Pick", 0, { { "x", PT::Float } }, { { "r", PT::String } });
    const int br = f.branch();
    { const int g = f.op(NT::Greater); f.data(pick, 0, g, 0); f.data(f.constF(0.0f), 0, g, 1);
      f.data(g, 0, br, 0); }
    f.exec(pick, br);
    const int retP = f.fnReturn("Pick");
    const int retN = f.fnReturn("Pick");
    HorizonCode::syncFunctionSignatures(f.g);
    f.data(f.constS("pos"), 0, retP, 0);
    f.data(f.constS("neg"), 0, retN, 0);
    f.exec(br, retP, 0);
    f.exec(br, retN, 1);

    // Early(n) -> r: a Return inside a Sequence arm. Return has no exec-out, so
    // it ends ONLY its own chain — arm 1 still runs, and its Return wins.
    const int early = f.fnEntry("Early", 0, { { "n", PT::Float } }, { { "r", PT::Float } });
    const int seqE = f.sequence();
    f.exec(early, seqE);
    const int retA = f.fnReturn("Early");
    const int retB = f.fnReturn("Early");
    HorizonCode::syncFunctionSignatures(f.g);
    f.g.findNode(retA)->pinDefaults[0] = Value::ofFloat(1.0f);
    f.g.findNode(retB)->pinDefaults[0] = Value::ofFloat(2.0f);
    const int sideA = f.setVar("sec", PT::Float);
    f.g.findNode(sideA)->pinDefaults[0] = Value::ofFloat(7.0f);
    f.exec(seqE, retA, 0);
    f.exec(seqE, sideA, 1);
    f.exec(sideA, retB);

    // Secret() — private; sets sec = 1.
    const int secret = f.fnEntry("Secret", 1, {}, {});
    const int sSec = f.setVar("sec", PT::Float);
    f.g.findNode(sSec)->pinDefaults[0] = Value::ofFloat(1.0f);
    f.exec(secret, sSec);

    // Event calls Sum(3, 4) internally and stores the result.
    const int ev = f.event("UseSum");
    const int call = f.fnCall("Sum");
    HorizonCode::syncFunctionSignatures(f.g);
    f.data(f.constF(3.0f), 0, call, 0);
    f.data(f.constF(4.0f), 0, call, 1);
    f.exec(ev, call);
    const int sOut = f.setVar("out", PT::Float);
    f.data(call, 0, sOut, 0);
    f.exec(call, sOut);
    return f.done("functions_basic");
}

// 6 — functions_recursive: bounded recursion incl. the stale-exec-cache read
// (§5.4: the per-RUN cache is shared across frames; an inner recursive call
// overwrites the outer invocation's cached call outputs).
inline HE::hccg::ClassSource fxFunctionsRecursive()
{
    Fx f;
    f.var("sum", PT::Float);

    // S(x) -> r = x * 10 + 1 (the +1 keeps S(0) non-zero, so a per-invocation
    // cache would produce a visibly different sum than the per-run one).
    const int s = f.fnEntry("S", 0, { { "x", PT::Float } }, { { "r", PT::Float } });
    const int sRet = f.fnReturn("S");
    { const int m = f.op(NT::Multiply); f.data(s, 0, m, 0); f.data(f.constF(10.0f), 0, m, 1);
      const int a = f.op(NT::Add); f.data(m, 0, a, 0); f.data(f.constF(1.0f), 0, a, 1);
      HorizonCode::syncFunctionSignatures(f.g);
      f.data(a, 0, sRet, 0); }
    f.exec(s, sRet);

    // R(n) -> out: calls S(n) (cached), recurses for n > 0.5, then reads the
    // S-call's cache AFTER the recursion — sees the innermost frame's value.
    const int r = f.fnEntry("R", 0, { { "n", PT::Float } }, { { "out", PT::Float } });
    const int callS = f.fnCall("S");
    HorizonCode::syncFunctionSignatures(f.g);
    f.data(r, 0, callS, 0);
    f.exec(r, callS);
    const int br = f.branch();
    { const int g = f.op(NT::Greater); f.data(r, 0, g, 0); f.data(f.constF(0.5f), 0, g, 1);
      f.data(g, 0, br, 0); }
    f.exec(callS, br);
    // true: recurse R(n - 1), then sum += S-cache, return sum.
    const int callR = f.fnCall("R");
    HorizonCode::syncFunctionSignatures(f.g);
    { const int sub = f.op(NT::Subtract); f.data(r, 0, sub, 0); f.data(f.constF(1.0f), 0, sub, 1);
      f.data(sub, 0, callR, 0); }
    f.exec(br, callR, 0);
    const int accT = f.setVar("sum", PT::Float);
    { const int a = f.op(NT::Add); f.data(f.getVar("sum", PT::Float), 0, a, 0);
      f.data(callS, 0, a, 1); f.data(a, 0, accT, 0); }
    f.exec(callR, accT);
    const int retT = f.fnReturn("R");
    HorizonCode::syncFunctionSignatures(f.g);
    f.data(f.getVar("sum", PT::Float), 0, retT, 0);
    f.exec(accT, retT);
    // false: sum += S-cache, return the S-cache directly.
    const int accF = f.setVar("sum", PT::Float);
    { const int a = f.op(NT::Add); f.data(f.getVar("sum", PT::Float), 0, a, 0);
      f.data(callS, 0, a, 1); f.data(a, 0, accF, 0); }
    f.exec(br, accF, 1);
    const int retF = f.fnReturn("R");
    HorizonCode::syncFunctionSignatures(f.g);
    f.data(callS, 0, retF, 0);
    f.exec(accF, retF);
    return f.done("functions_recursive");
}

// 7 — foreach_arrays: all array ops, clamps, out-of-range, element/index reads
// after Done, nested ForEach (§3.4).
inline HE::hccg::ClassSource fxForeachArrays()
{
    Fx f;
    f.arrVar("arrF", PT::Float, { Value::ofFloat(5), Value::ofFloat(6), Value::ofFloat(7) });
    f.var("total", PT::Float);
    f.var("count", PT::Int);
    f.var("afterElem", PT::Float);
    f.var("afterIdx", PT::Int);
    f.arrVar("built", PT::Float, {});
    f.arrVar("mod", PT::Float, {});
    f.var("has6", PT::Bool);
    f.var("idx7", PT::Int);
    f.var("oob", PT::Float);
    f.var("nestedSum", PT::Float);

    // Loop: total += element per iteration; Done stores count + the LAST
    // iteration's element/index (still cached after the loop).
    const int evL = f.event("Loop");
    const int fe = f.forEach(PT::Float);
    f.data(f.getVar("arrF", PT::Float, true), 0, fe, 0);
    f.exec(evL, fe);
    const int sTot = f.setVar("total", PT::Float);
    { const int a = f.op(NT::Add); f.data(f.getVar("total", PT::Float), 0, a, 0);
      f.data(fe, 0, a, 1); f.data(a, 0, sTot, 0); }
    f.exec(fe, sTot, 0);   // Body
    const int sCount = f.setVar("count", PT::Int);
    { const int len = f.arrayOp(NT::ArrayLength, PT::Float);
      f.data(f.getVar("arrF", PT::Float, true), 0, len, 0);
      f.data(len, 0, sCount, 0); }
    f.exec(fe, sCount, 1); // Done
    const int sAE = f.setVar("afterElem", PT::Float);
    f.data(fe, 0, sAE, 0);
    f.exec(sCount, sAE);
    const int sAI = f.setVar("afterIdx", PT::Int);
    f.data(fe, 1, sAI, 0);
    f.exec(sAE, sAI);

    // Ops: build/insert/set/remove/search/out-of-range.
    const int evO = f.event("Ops");
    const int sBuilt = f.setVar("built", PT::Float, true);
    { const int mk = f.arrayOp(NT::ArrayMake, PT::Float);
      const int a1 = f.arrayOp(NT::ArrayAdd, PT::Float);
      const int a2 = f.arrayOp(NT::ArrayAdd, PT::Float);
      f.data(mk, 0, a1, 0); f.data(f.constF(1.0f), 0, a1, 1);
      f.data(a1, 0, a2, 0); f.data(f.constF(2.0f), 0, a2, 1);
      f.data(a2, 0, sBuilt, 0); }
    f.exec(evO, sBuilt);
    const int sMod = f.setVar("mod", PT::Float, true);
    { const int st = f.arrayOp(NT::ArraySet, PT::Float);      // {5,9,7}
      f.data(f.getVar("arrF", PT::Float, true), 0, st, 0);
      f.data(f.constI(1), 0, st, 1); f.data(f.constF(9.0f), 0, st, 2);
      const int in = f.arrayOp(NT::ArrayInsert, PT::Float);   // index 99 clamps → append 42
      f.data(st, 0, in, 0); f.data(f.constI(99), 0, in, 1); f.data(f.constF(42.0f), 0, in, 2);
      const int rm = f.arrayOp(NT::ArrayRemove, PT::Float);   // remove index 0 → {9,7,42}
      f.data(in, 0, rm, 0); f.data(f.constI(0), 0, rm, 1);
      f.data(rm, 0, sMod, 0); }
    f.exec(sBuilt, sMod);
    const int sHas = f.setVar("has6", PT::Bool);
    { const int c = f.arrayOp(NT::ArrayContains, PT::Float);
      f.data(f.getVar("arrF", PT::Float, true), 0, c, 0); f.data(f.constF(6.0f), 0, c, 1);
      f.data(c, 0, sHas, 0); }
    f.exec(sMod, sHas);
    const int sIdx = f.setVar("idx7", PT::Int);
    { const int c = f.arrayOp(NT::ArrayIndexOf, PT::Float);
      f.data(f.getVar("arrF", PT::Float, true), 0, c, 0); f.data(f.constF(7.0f), 0, c, 1);
      f.data(c, 0, sIdx, 0); }
    f.exec(sHas, sIdx);
    const int sOob = f.setVar("oob", PT::Float);
    { const int gget = f.arrayOp(NT::ArrayGet, PT::Float);
      f.data(f.getVar("arrF", PT::Float, true), 0, gget, 0); f.data(f.constI(99), 0, gget, 1);
      f.data(gget, 0, sOob, 0); }
    f.exec(sIdx, sOob);

    // Nested: sum of outer*inner products.
    const int evN = f.event("Nested");
    const int feO = f.forEach(PT::Float);
    f.data(f.getVar("arrF", PT::Float, true), 0, feO, 0);
    f.exec(evN, feO);
    const int feI = f.forEach(PT::Float);
    f.data(f.getVar("arrF", PT::Float, true), 0, feI, 0);
    f.exec(feO, feI, 0);   // outer Body
    const int sN = f.setVar("nestedSum", PT::Float);
    { const int m = f.op(NT::Multiply); f.data(feO, 0, m, 0); f.data(feI, 0, m, 1);
      const int a = f.op(NT::Add); f.data(f.getVar("nestedSum", PT::Float), 0, a, 0);
      f.data(m, 0, a, 1); f.data(a, 0, sN, 0); }
    f.exec(feI, sN, 0);    // inner Body
    return f.done("foreach_arrays");
}

// 8 — events_multi: two Event nodes of one name (order + SHARED per-fire exec
// cache), elem filtering, event-arg coercion (§3.1).
inline HE::hccg::ClassSource fxEventsMulti()
{
    Fx f;
    f.var("trace", PT::String);
    f.var("wRef", PT::Ref);
    f.var("tickSum", PT::Float);

    // Handler A (elem 0 = any): trace += "a", creates a widget (cached).
    const int evA = f.event("Ping", 0);
    const int sA = f.setVar("trace", PT::String);
    { const int cat = f.op(NT::Concat); f.data(f.getVar("trace", PT::String), 0, cat, 0);
      f.data(f.constS("a"), 0, cat, 1); f.data(cat, 0, sA, 0); }
    f.exec(evA, sA);
    Node cw; cw.type = NT::CreateWidget; cw.s = "Content/UI/W.hasset";
    const int create = f.add(cw);
    f.exec(sA, create);

    // Handler B (elem 2 only): trace += "b", reads handler A's cached widget
    // ref out of the SHARED RunState of this fire.
    const int evB = f.event("Ping", 2);
    const int sB = f.setVar("trace", PT::String);
    { const int cat = f.op(NT::Concat); f.data(f.getVar("trace", PT::String), 0, cat, 0);
      f.data(f.constS("b"), 0, cat, 1); f.data(cat, 0, sB, 0); }
    f.exec(evB, sB);
    const int sRef = f.setVar("wRef", PT::Ref);
    f.data(create, 0, sRef, 0);
    f.exec(sB, sRef);

    const int evT = f.event("Tick", 0, true, PT::Float);
    const int sT = f.setVar("tickSum", PT::Float);
    { const int a = f.op(NT::Add); f.data(f.getVar("tickSum", PT::Float), 0, a, 0);
      f.data(evT, 0, a, 1); f.data(a, 0, sT, 0); }
    f.exec(evT, sT);
    return f.done("events_multi");
}

// 13 — widget_props: property get/set + show/hide via HostBindings, pin
// defaults on property values, Set-property pass-through.
inline HE::hccg::ClassSource fxWidgetProps()
{
    Fx f;
    f.var("got", PT::Float);
    f.var("copied", PT::String);

    const int ev = f.event("UI");
    Node sp; sp.type = NT::SetProperty; sp.elem = 3; sp.s = "opacity"; sp.propType = PT::Float;
    sp.pinDefaults[0] = Value::ofFloat(0.5f);
    const int setOp = f.add(sp);
    f.exec(ev, setOp);
    Node gp; gp.type = NT::GetProperty; gp.elem = 3; gp.s = "opacity"; gp.propType = PT::Float;
    const int getOp = f.add(gp);
    const int sGot = f.setVar("got", PT::Float);
    f.data(getOp, 0, sGot, 0);
    f.exec(setOp, sGot);
    Node sp2; sp2.type = NT::SetProperty; sp2.elem = 4; sp2.s = "text"; sp2.propType = PT::String;
    sp2.pinDefaults[0] = Value::ofString("hi");
    const int setTx = f.add(sp2);
    f.exec(sGot, setTx);
    const int sCopy = f.setVar("copied", PT::String);
    f.data(setTx, 0, sCopy, 0);   // pass-through re-evaluates the pin default
    f.exec(setTx, sCopy);
    const int show = f.op(NT::ShowSelf);
    f.exec(sCopy, show);
    const int hide = f.op(NT::HideSelf);
    f.exec(show, hide);
    return f.done("widget_props");
}

// 14 — limits_smoke: trips the step guard (nested ForEach ≫ 4096 steps); the
// assertion is only "both sides terminate cleanly" (§3.6, sharpened).
inline HE::hccg::ClassSource fxLimitsSmoke()
{
    Fx f;
    std::vector<Value> big;
    for (int i = 0; i < 20; ++i) big.push_back(Value::ofFloat((float)i));
    f.arrVar("big", PT::Float, std::move(big));
    f.var("x", PT::Float);

    const int ev = f.event("Burn");
    const int f1 = f.forEach(PT::Float);
    f.data(f.getVar("big", PT::Float, true), 0, f1, 0);
    f.exec(ev, f1);
    const int f2 = f.forEach(PT::Float);
    f.data(f.getVar("big", PT::Float, true), 0, f2, 0);
    f.exec(f1, f2, 0);
    const int f3 = f.forEach(PT::Float);
    f.data(f.getVar("big", PT::Float, true), 0, f3, 0);
    f.exec(f2, f3, 0);
    const int sx = f.setVar("x", PT::Float);
    { const int a = f.op(NT::Add); f.data(f.getVar("x", PT::Float), 0, a, 0);
      f.data(f.constF(1.0f), 0, a, 1); f.data(a, 0, sx, 0); }
    f.exec(f3, sx, 0);
    return f.done("limits_smoke");
}

// 15 — functions_locals (§13.4): per-invocation reset, defaults incl. arrays,
// recursion with a per-FRAME local beside the per-RUN exec cache.
inline HE::hccg::ClassSource fxFunctionsLocals()
{
    Fx f;
    f.var("out", PT::Float);
    f.var("outLen", PT::Int);

    // Work(n) -> r: local acc = 10 (+n), local array tmp = {1} (+n).
    const int work = f.fnEntry("Work", 0, { { "n", PT::Float } }, { { "r", PT::Float } });
    f.var("acc", PT::Float, 10.0f, {}, 0, /*scope=*/work);
    f.arrVar("tmp", PT::Float, { Value::ofFloat(1) }, /*scope=*/work);
    const int sAcc = f.setVar("acc", PT::Float);
    { const int a = f.op(NT::Add); f.data(f.getVar("acc", PT::Float), 0, a, 0);
      f.data(work, 0, a, 1); f.data(a, 0, sAcc, 0); }
    f.exec(work, sAcc);
    const int sTmp = f.setVar("tmp", PT::Float, true);
    { const int add = f.arrayOp(NT::ArrayAdd, PT::Float);
      f.data(f.getVar("tmp", PT::Float, true), 0, add, 0);
      f.data(work, 0, add, 1); f.data(add, 0, sTmp, 0); }
    f.exec(sAcc, sTmp);
    const int sOut = f.setVar("out", PT::Float);   // instance observes the local
    f.data(f.getVar("acc", PT::Float), 0, sOut, 0);
    f.exec(sTmp, sOut);
    const int sLen = f.setVar("outLen", PT::Int);
    { const int len = f.arrayOp(NT::ArrayLength, PT::Float);
      f.data(f.getVar("tmp", PT::Float, true), 0, len, 0); f.data(len, 0, sLen, 0); }
    f.exec(sOut, sLen);
    const int wRet = f.fnReturn("Work");
    HorizonCode::syncFunctionSignatures(f.g);
    f.data(f.getVar("acc", PT::Float), 0, wRet, 0);
    f.exec(sLen, wRet);

    // S2(x) -> r = x * 10 (exec-cache donor for the recursion test).
    const int s2 = f.fnEntry("S2", 0, { { "x", PT::Float } }, { { "r", PT::Float } });
    const int s2Ret = f.fnReturn("S2");
    { const int m = f.op(NT::Multiply); f.data(s2, 0, m, 0); f.data(f.constF(10.0f), 0, m, 1);
      HorizonCode::syncFunctionSignatures(f.g);
      f.data(m, 0, s2Ret, 0); }
    f.exec(s2, s2Ret);

    // R2(n) -> r: local mine = n is PER-FRAME (survives the recursion); the
    // S2-call cache is PER-RUN (the inner call overwrites it). r = mine + cache.
    const int r2 = f.fnEntry("R2", 0, { { "n", PT::Float } }, { { "r", PT::Float } });
    f.var("mine", PT::Float, 0.0f, {}, 0, /*scope=*/r2);
    const int sMine = f.setVar("mine", PT::Float);
    f.data(r2, 0, sMine, 0);
    f.exec(r2, sMine);
    const int callS2 = f.fnCall("S2");
    HorizonCode::syncFunctionSignatures(f.g);
    f.data(r2, 0, callS2, 0);
    f.exec(sMine, callS2);
    const int br = f.branch();
    { const int g = f.op(NT::Greater); f.data(r2, 0, g, 0); f.data(f.constF(0.5f), 0, g, 1);
      f.data(g, 0, br, 0); }
    f.exec(callS2, br);
    const int callR2 = f.fnCall("R2");
    HorizonCode::syncFunctionSignatures(f.g);
    { const int sub = f.op(NT::Subtract); f.data(r2, 0, sub, 0); f.data(f.constF(1.0f), 0, sub, 1);
      f.data(sub, 0, callR2, 0); }
    f.exec(br, callR2, 0);
    const int retT = f.fnReturn("R2");
    HorizonCode::syncFunctionSignatures(f.g);
    { const int a = f.op(NT::Add); f.data(f.getVar("mine", PT::Float), 0, a, 0);
      f.data(callS2, 0, a, 1); f.data(a, 0, retT, 0); }
    f.exec(callR2, retT);
    const int retF = f.fnReturn("R2");
    HorizonCode::syncFunctionSignatures(f.g);
    { const int a = f.op(NT::Add); f.data(f.getVar("mine", PT::Float), 0, a, 0);
      f.data(callS2, 0, a, 1); f.data(a, 0, retF, 0); }
    f.exec(br, retF, 1);
    return f.done("functions_locals");
}

// 9 — engine_pure_multiout: a PURE EngineCall dispatches on EVERY data-out
// read (§3.4) — N wired reads = N dispatches, verified by the callApi trace.
inline HE::hccg::ClassSource fxEnginePureMultiout()
{
    Fx f;
    f.var("hit", PT::Bool);
    f.var("hit2", PT::Bool);
    f.var("dist", PT::Float);
    { Variable v; v.name = "pt"; v.type = PT::Color; f.g.variables.push_back(v); }
    f.var("sinv", PT::Float);

    const int ev = f.event("Query");
    // physics.raycast (pure, 5 results) against a null world → deterministic miss.
    const int rc = f.engineCall("physics.raycast");
    { Node* n = f.g.findNode(rc);
      n->pinDefaults[0] = Value::ofColor({ 0, 1, 0, 0 });   // origin
      n->pinDefaults[1] = Value::ofColor({ 0, -1, 0, 0 });  // direction
      n->pinDefaults[2] = Value::ofFloat(10.0f); }          // maxDistance
    int prev = ev;
    auto chainSet = [&](const std::string& var, PT t, int src, int srcOut)
    {
        const int s = f.setVar(var, t);
        f.data(src, srcOut, s, 0);
        f.exec(prev, s);
        prev = s;
    };
    chainSet("hit", PT::Bool, rc, 0);     // dispatch 1
    chainSet("dist", PT::Float, rc, 4);   // dispatch 2
    chainSet("pt", PT::Color, rc, 2);     // dispatch 3
    chainSet("hit2", PT::Bool, rc, 0);    // SAME output read again → dispatch 4
    const int sin = f.engineCall("math.sin");
    f.g.findNode(sin)->pinDefaults[0] = Value::ofFloat(0.5f);
    chainSet("sinv", PT::Float, sin, 0);  // dispatch 5
    return f.done("engine_pure_multiout");
}

// 10 — engine_exec_cached: exec EngineCalls dispatch ONCE at their exec
// position and cache raw results (reads are free); seeded random keeps the
// process-global RNG deterministic per backend run; save.* round-trips the
// process-global store. Arg coercion to the descriptor's param types.
// An animator SYNC GRAPH, in the shape the export ships one: a single Update
// event that reads its own owner and writes state-machine parameters. It is a
// plain graph with no base class and no chain — like the level script, which is
// the precedent for a chain-less ClassSource compiling at all.
//
// The point of the fixture is parity: a sync graph is dispatched from inside the
// animation phase, so if the compiled form diverged from the interpreted one the
// symptom would be a character that animates differently in the shipped game
// than in the editor — the hardest kind of difference to chase.
inline HE::hccg::ClassSource fxAnimatorSync()
{
    Fx f;
    f.var("wrote", PT::Float);

    const int ev = f.event("Update", 0, /*hasArg=*/true, PT::Float);   // dt

    const int self = f.engineCall("entity.self");
    const int set  = f.engineCall("animator.setParam");
    f.data(self, 0, set, 0);                                  // entity
    { Node* n = f.g.findNode(set);
      n->pinDefaults[1] = Value::ofString("speed");
      n->pinDefaults[2] = Value::ofFloat(1.5f); }
    f.exec(ev, set);

    // Read it straight back through getParam, so the fixture also pins down the
    // round trip rather than just the call reaching the seam. With a null world
    // both sides answer 0 — what matters is that they answer the SAME.
    const int get = f.engineCall("animator.getParam");
    f.data(self, 0, get, 0);
    f.g.findNode(get)->pinDefaults[1] = Value::ofString("speed");
    const int s = f.setVar("wrote", PT::Float);
    f.data(get, 0, s, 0);
    f.exec(set, s);

    return f.done("animator_sync");
}

inline HE::hccg::ClassSource fxEngineExecCached()
{
    Fx f;
    f.var("v1", PT::Float);
    f.var("v1b", PT::Float);
    f.var("v2", PT::Float);
    f.var("saved", PT::Float);

    const int ev = f.event("Roll");
    const int seed = f.engineCall("random.seed");
    f.g.findNode(seed)->pinDefaults[0] = Value::ofFloat(42.7f);   // Float → Int arg coercion
    f.exec(ev, seed);
    const int roll = f.engineCall("random.value");
    f.exec(seed, roll);
    const int s1 = f.setVar("v1", PT::Float);
    f.data(roll, 0, s1, 0);      // cached read — no re-dispatch
    f.exec(roll, s1);
    const int s1b = f.setVar("v1b", PT::Float);
    f.data(roll, 0, s1b, 0);     // second cached read — still no dispatch
    f.exec(s1, s1b);
    const int range = f.engineCall("random.range");
    { Node* n = f.g.findNode(range);
      n->pinDefaults[0] = Value::ofFloat(0.0f);
      n->pinDefaults[1] = Value::ofFloat(10.0f); }
    f.exec(s1b, range);
    const int s2 = f.setVar("v2", PT::Float);
    f.data(range, 0, s2, 0);
    f.exec(range, s2);
    // save v2: with no active save both calls take the loud-failure path —
    // set returns false, get returns the default. Parity still must hold
    // (identical dispatch through the same registry lambda on both backends);
    // the VALUE round-trip is covered by test_engine_api's save-v2 cases.
    const int save = f.engineCall("save.setNumber");
    { Node* n = f.g.findNode(save);
      n->pinDefaults[0] = Value::ofString("hc.parity");
      n->pinDefaults[1] = Value::ofFloat(3.5f); }
    f.exec(s2, save);
    const int load = f.engineCall("save.getNumber");
    { Node* n = f.g.findNode(load);
      n->pinDefaults[0] = Value::ofString("hc.parity");
      n->pinDefaults[1] = Value::ofFloat(-1.0f); }
    const int s3 = f.setVar("saved", PT::Float);
    f.data(load, 0, s3, 0);
    f.exec(save, s3);
    return f.done("engine_exec_cached");
}

// 11a — ref_target: the class fixture 11 instantiates via Create Object.
// Public hp + Damage(amount)->left, private secret + Heal(), Construct marker.
inline HE::hccg::ClassSource fxRefTarget()
{
    Fx f;
    f.var("hp", PT::Float, 100.0f);
    f.var("secret", PT::Float, 5.0f, {}, /*access=*/1);
    f.var("constructed", PT::Float);

    const int ctor = f.event("Construct");
    const int sc = f.setVar("constructed", PT::Float);
    f.g.findNode(sc)->pinDefaults[0] = Value::ofFloat(1.0f);
    f.exec(ctor, sc);

    const int dmg = f.fnEntry("Damage", 0, { { "amount", PT::Float } }, { { "left", PT::Float } });
    const int sHp = f.setVar("hp", PT::Float);
    { const int sub = f.op(NT::Subtract); f.data(f.getVar("hp", PT::Float), 0, sub, 0);
      f.data(dmg, 0, sub, 1); f.data(sub, 0, sHp, 0); }
    f.exec(dmg, sHp);
    const int ret = f.fnReturn("Damage");
    HorizonCode::syncFunctionSignatures(f.g);
    f.data(f.getVar("hp", PT::Float), 0, ret, 0);
    f.exec(sHp, ret);

    const int heal = f.fnEntry("Heal", 1, {}, {});   // private
    const int sFull = f.setVar("hp", PT::Float);
    f.g.findNode(sFull)->pinDefaults[0] = Value::ofFloat(100.0f);
    f.exec(heal, sFull);
    return f.done("ref_target");
}

// 11 — refs_objects: Create/DestroyObject, Get/Set/CallExternal incl. the
// null-target and private-member warn paths, GetSelf/GetGameInstance (§3.5).
inline HE::hccg::ClassSource fxRefsObjects()
{
    Fx f;
    // className is what lets codegen see WHICH class the ref holds — the direct
    // call path is only emittable when it does (and still checks at run time).
    { Variable v; v.name = "obj"; v.type = PT::Ref; v.className = "fix/ref_target";
      f.g.variables.push_back(v); }
    f.var("objNull", PT::Ref);
    f.var("seen", PT::Float);
    f.var("left", PT::Float);
    f.var("sneak", PT::Float);
    f.var("nseen", PT::Float);
    f.var("meRef", PT::Ref);
    f.var("giRef", PT::Ref);

    // Spawn: create the target, keep the ref.
    const int evS = f.event("Spawn");
    Node co; co.type = NT::CreateObject; co.s = "fix/ref_target";
    const int create = f.add(co);
    f.exec(evS, create);
    const int sObj = f.setVar("obj", PT::Ref);
    f.data(create, 0, sObj, 0);
    f.exec(create, sObj);

    // Poke: public set/get/call, then the private warn paths.
    const int evP = f.event("Poke");
    Node se; se.type = NT::SetExternal; se.s = "hp"; se.propType = PT::Float;
    se.pinDefaults[1] = Value::ofFloat(50.0f);
    const int setHp = f.add(se);
    f.data(f.getVar("obj", PT::Ref), 0, setHp, 0);
    f.exec(evP, setHp);
    const int sSeen = f.setVar("seen", PT::Float);
    { Node ge; ge.type = NT::GetExternal; ge.s = "hp"; ge.propType = PT::Float;
      const int getHp = f.add(ge);
      f.data(f.getVar("obj", PT::Ref), 0, getHp, 0);
      f.data(getHp, 0, sSeen, 0); }
    f.exec(setHp, sSeen);
    Node ce; ce.type = NT::CallExternal; ce.s = "Damage";
    ce.params = { { "amount", PT::Float } }; ce.results = { { "left", PT::Float } };
    ce.pinDefaults[1] = Value::ofFloat(20.0f);   // data-in 0 = Target, 1 = amount
    const int callDmg = f.add(ce);
    f.data(f.getVar("obj", PT::Ref), 0, callDmg, 0);
    f.exec(sSeen, callDmg);
    const int sLeft = f.setVar("left", PT::Float);
    f.data(callDmg, 0, sLeft, 0);
    f.exec(callDmg, sLeft);
    // Private member/function → warn + zero / empty results.
    const int sSneak = f.setVar("sneak", PT::Float);
    { Node ge; ge.type = NT::GetExternal; ge.s = "secret"; ge.propType = PT::Float;
      const int getSec = f.add(ge);
      f.data(f.getVar("obj", PT::Ref), 0, getSec, 0);
      f.data(getSec, 0, sSneak, 0); }
    f.exec(sLeft, sSneak);
    Node ch; ch.type = NT::CallExternal; ch.s = "Heal";
    const int callHeal = f.add(ch);
    f.data(f.getVar("obj", PT::Ref), 0, callHeal, 0);
    f.exec(sSneak, callHeal);

    // NullPoke: Get on a null ref → error log + zero.
    const int evN = f.event("NullPoke");
    const int sN = f.setVar("nseen", PT::Float);
    { Node ge; ge.type = NT::GetExternal; ge.s = "hp"; ge.propType = PT::Float;
      const int getN = f.add(ge);
      f.data(f.getVar("objNull", PT::Ref), 0, getN, 0);
      f.data(getN, 0, sN, 0); }
    f.exec(evN, sN);

    // Who: self + game-instance refs.
    const int evW = f.event("Who");
    const int sMe = f.setVar("meRef", PT::Ref);
    f.data(f.op(NT::GetSelf), 0, sMe, 0);
    f.exec(evW, sMe);
    const int sGi = f.setVar("giRef", PT::Ref);
    f.data(f.op(NT::GetGameInstance), 0, sGi, 0);
    f.exec(sMe, sGi);

    // Kill: destroy the held object.
    const int evK = f.event("Kill");
    Node dob; dob.type = NT::DestroyObject;
    const int destroy = f.add(dob);
    f.data(f.getVar("obj", PT::Ref), 0, destroy, 0);
    f.exec(evK, destroy);
    return f.done("refs_objects");
}

// 12a — dispatch_owner: fires/emits "Sig" (dispatchers, §3.5).
inline HE::hccg::ClassSource fxDispatchOwner()
{
    Fx f;
    f.var("ownGot", PT::Float);
    const int evSig = f.event("Sig", 0, true, PT::Float);
    const int s = f.setVar("ownGot", PT::Float);
    f.data(evSig, 0, s, 0);
    f.exec(evSig, s);
    const int evGo = f.event("Go");
    Node em; em.type = NT::EmitEvent; em.s = "Sig"; em.hasArg = true; em.propType = PT::Float;
    em.pinDefaults[0] = Value::ofFloat(7.0f);
    const int emit = f.add(em);
    f.exec(evGo, emit);
    return f.done("dispatch_owner");
}

// 12b — dispatch_listener: binds itself to "Sig" on a ref passed via the Setup
// arg, accumulates received args, and RELAYS by emitting "Sig" itself — so
// listener chains (and, with a bind cycle, the depth-32 guard) get exercised.
inline HE::hccg::ClassSource fxDispatchListener()
{
    Fx f;
    f.var("got", PT::Float);
    const int evSetup = f.event("Setup", 0, true, PT::Ref);
    Node be; be.type = NT::BindEvent; be.s = "Sig";
    const int bind = f.add(be);
    f.data(evSetup, 0, bind, 0);
    f.exec(evSetup, bind);
    const int evSig = f.event("Sig", 0, true, PT::Float);
    const int s = f.setVar("got", PT::Float);
    { const int a = f.op(NT::Add); f.data(f.getVar("got", PT::Float), 0, a, 0);
      f.data(evSig, 0, a, 1); f.data(a, 0, s, 0); }
    f.exec(evSig, s);
    Node em; em.type = NT::EmitEvent; em.s = "Sig"; em.hasArg = true; em.propType = PT::Float;
    const int relay = f.add(em);
    f.data(evSig, 0, relay, 0);
    f.exec(s, relay);
    return f.done("dispatch_listener");
}

// 12c — dispatch_sink: accumulates "Sig" WITHOUT relaying. Used for the bind-
// CYCLE depth-guard test: a relaying listener in a cycle branches the dispatch
// tree (EmitEvent + fireEvent's trailing listener dispatch) into 2^32 fires —
// the guard bounds DEPTH, not total work. Sinks keep the cycle a single
// trailing-dispatch chain, which the depth guard cuts after 32 hops.
inline HE::hccg::ClassSource fxDispatchSink()
{
    Fx f;
    f.var("got", PT::Float);
    const int evSetup = f.event("Setup", 0, true, PT::Ref);
    Node be; be.type = NT::BindEvent; be.s = "Sig";
    const int bind = f.add(be);
    f.data(evSetup, 0, bind, 0);
    f.exec(evSetup, bind);
    const int evSig = f.event("Sig", 0, true, PT::Float);
    const int s = f.setVar("got", PT::Float);
    { const int a = f.op(NT::Add); f.data(f.getVar("got", PT::Float), 0, a, 0);
      f.data(evSig, 0, a, 1); f.data(a, 0, s, 0); }
    f.exec(evSig, s);
    return f.done("dispatch_sink");
}

// 16 — latent_flow: Delay (latent continuation via Runtime::update, retrigger
// ignored), Do Once (fires once per instance, reset by reseed), Flip Flop
// (alternating A/B + IsA data-out), Is Valid (Ref liveness).
inline HE::hccg::ClassSource fxLatentFlow()
{
    Fx f;
    f.var("n", PT::Float);
    f.var("once", PT::Float);
    f.var("flip", PT::String);
    f.var("isA", PT::Bool);
    f.var("valid", PT::Bool);
    f.var("r", PT::Float);       // the real-time Delay's counter

    // Once: DoOnce → once += 1 (only the first fire).
    const int evO = f.event("Once");
    const int doOnce = f.op(NT::DoOnce);
    f.exec(evO, doOnce);
    const int sOnce = f.setVar("once", PT::Float);
    { const int a = f.op(NT::Add); f.data(f.getVar("once", PT::Float), 0, a, 0);
      f.data(f.constF(1.0f), 0, a, 1); f.data(a, 0, sOnce, 0); }
    f.exec(doOnce, sOnce, 0);

    // Flip: FlipFlop → A appends "A" (and stores IsA), B appends "B".
    const int evF = f.event("Flip");
    const int ff = f.op(NT::FlipFlop);
    f.exec(evF, ff);
    const int sA = f.setVar("flip", PT::String);
    { const int cat = f.op(NT::Concat); f.data(f.getVar("flip", PT::String), 0, cat, 0);
      f.data(f.constS("A"), 0, cat, 1); f.data(cat, 0, sA, 0); }
    f.exec(ff, sA, 0);
    const int sIsA = f.setVar("isA", PT::Bool);
    f.data(ff, 0, sIsA, 0);   // IsA data-out (state of the side just taken)
    f.exec(sA, sIsA);
    const int sB = f.setVar("flip", PT::String);
    { const int cat = f.op(NT::Concat); f.data(f.getVar("flip", PT::String), 0, cat, 0);
      f.data(f.constS("B"), 0, cat, 1); f.data(cat, 0, sB, 0); }
    f.exec(ff, sB, 1);

    // Check(arg Ref): valid = Is Valid(arg).
    const int evC = f.event("Check", 0, true, PT::Ref);
    const int iv = f.op(NT::IsValid);
    f.data(evC, 0, iv, 0);
    const int sV = f.setVar("valid", PT::Bool);
    f.data(iv, 0, sV, 0);
    f.exec(evC, sV);

    // Wait: n += 1, then Delay(1s) → n += 10 (the latent continuation).
    const int evW = f.event("Wait");
    const int s1 = f.setVar("n", PT::Float);
    { const int a = f.op(NT::Add); f.data(f.getVar("n", PT::Float), 0, a, 0);
      f.data(f.constF(1.0f), 0, a, 1); f.data(a, 0, s1, 0); }
    f.exec(evW, s1);
    const int delay = f.op(NT::Delay);
    f.g.findNode(delay)->pinDefaults[0] = Value::ofFloat(1.0f);
    f.exec(s1, delay);
    const int s2 = f.setVar("n", PT::Float);
    { const int a = f.op(NT::Add); f.data(f.getVar("n", PT::Float), 0, a, 0);
      f.data(f.constF(10.0f), 0, a, 1); f.data(a, 0, s2, 0); }
    f.exec(delay, s2, 0);   // Completed

    // WaitReal: the same shape on the REAL-TIME clock (Delay's second data-in),
    // so the parity harness proves both backends read the new pin — and that a
    // frame whose game time stands still (a pause) still expires this one.
    const int evR = f.event("WaitReal");
    const int delayR = f.op(NT::Delay);
    f.g.findNode(delayR)->pinDefaults[0] = Value::ofFloat(1.0f);
    f.g.findNode(delayR)->pinDefaults[1] = Value::ofBool(true);
    f.exec(evR, delayR);
    const int s3 = f.setVar("r", PT::Float);
    { const int a = f.op(NT::Add); f.data(f.getVar("r", PT::Float), 0, a, 0);
      f.data(f.constF(1.0f), 0, a, 1); f.data(a, 0, s3, 0); }
    f.exec(delayR, s3, 0);   // Completed
    return f.done("latent_flow");
}

// 20 — enums: sparse entry VALUES, Const/ToInt/IntTo/ToString, Switch routing,
// enum arrays, and a definition with DUPLICATE entry values (§3.4).
inline HE::hccg::ClassSource fxEnums()
{
    Fx f;
    f.enumVar("m", kMoodEnum, "Angry");     // 5
    f.enumVar("d", kDupEnum, "A");          // 1
    f.enumVar("back", kMoodEnum, "Calm");
    f.enumVar("first", kMoodEnum, "Calm");
    f.enumArrVar("marr", kMoodEnum);
    f.var("mi", PT::Int);
    f.var("ms", PT::String);
    f.var("ds", PT::String);
    f.var("route", PT::String);

    // Init: entry name ⇄ underlying int, both directions.
    const int ev = f.event("Init");
    const int sMs = f.setVar("ms", PT::String);
    { const int e2s = f.enumOp(NT::EnumToString, kMoodEnum);
      f.data(f.getVarT("m", PT::Enum, kMoodEnum), 0, e2s, 0);
      f.data(e2s, 0, sMs, 0); }
    f.exec(ev, sMs);
    const int sMi = f.setVar("mi", PT::Int);
    { const int e2i = f.enumOp(NT::EnumToInt, kMoodEnum);
      f.data(f.getVarT("m", PT::Enum, kMoodEnum), 0, e2i, 0);
      f.data(e2i, 0, sMi, 0); }
    f.exec(sMs, sMi);
    const int sBack = f.setVarT("back", PT::Enum, kMoodEnum);
    { const int i2e = f.enumOp(NT::IntToEnum, kMoodEnum);
      f.data(f.constI(1), 0, i2e, 0);
      f.data(i2e, 0, sBack, 0); }
    f.exec(sMi, sBack);

    // Pick(arg): a dynamic Value boundary into an enum pin, then routing.
    const int evP = f.event("Pick", 0, true, PT::Enum);
    const int sM = f.setVarT("m", PT::Enum, kMoodEnum);
    f.data(evP, 0, sM, 0);
    f.exec(evP, sM);
    const int sw = f.switchEnum(kMoodEnum);
    f.data(f.getVarT("m", PT::Enum, kMoodEnum), 0, sw, 0);
    f.exec(sM, sw);
    { const char* names[] = { "calm", "happy", "angry", "other" };
      for (int i = 0; i < 4; ++i)
      {
          const int s = f.setVar("route", PT::String);
          f.g.findNode(s)->pinDefaults[0] = Value::ofString(names[i]);
          f.exec(sw, s, i);
      } }

    // A literal entry from the palette.
    const int evC = f.event("Calm");
    const int sC = f.setVarT("m", PT::Enum, kMoodEnum);
    f.data(f.constEnum(kMoodEnum, 0), 0, sC, 0);
    f.exec(evC, sC);

    // Enum arrays: built at run time so the elements carry real entry values.
    const int evA = f.event("Push");
    const int sArr = f.setVarT("marr", PT::Enum, kMoodEnum, true);
    { const int add = f.arrayOpT(NT::ArrayAdd, PT::Enum, kMoodEnum);
      f.data(f.getVarT("marr", PT::Enum, kMoodEnum, true), 0, add, 0);
      f.data(f.constEnum(kMoodEnum, 5), 0, add, 1);
      f.data(add, 0, sArr, 0); }
    f.exec(evA, sArr);
    const int sFirst = f.setVarT("first", PT::Enum, kMoodEnum);
    { const int get = f.arrayOpT(NT::ArrayGet, PT::Enum, kMoodEnum);
      f.data(f.getVarT("marr", PT::Enum, kMoodEnum, true), 0, get, 0);
      f.data(f.constI(0), 0, get, 1);
      f.data(get, 0, sFirst, 0); }
    f.exec(sArr, sFirst);

    // Duplicate entry VALUES: ToString and Switch must both follow findValue
    // (first match wins) — and must not emit two `case 1:` labels.
    const int evD = f.event("Dup");
    const int sDs = f.setVar("ds", PT::String);
    { const int e2s = f.enumOp(NT::EnumToString, kDupEnum);
      f.data(f.getVarT("d", PT::Enum, kDupEnum), 0, e2s, 0);
      f.data(e2s, 0, sDs, 0); }
    f.exec(evD, sDs);
    const int swD = f.switchEnum(kDupEnum);
    f.data(f.getVarT("d", PT::Enum, kDupEnum), 0, swD, 0);
    f.exec(sDs, swD);
    { const char* names[] = { "A", "B", "C", "?" };
      for (int i = 0; i < 4; ++i)
      {
          const int s = f.setVar("route", PT::String);
          f.g.findNode(s)->pinDefaults[0] = Value::ofString(names[i]);
          f.exec(swD, s, i);
      } }
    return f.done("enums");
}

// 21 — structs: definition defaults + per-graph overrides, Make/Break/Get/Set,
// nested structs, enum + array + Ref fields, struct arrays, struct params (§3.4).
inline HE::hccg::ClassSource fxStructs()
{
    Fx f;
    f.structVar("s", kStatsType, { { "hp", Value::ofFloat(42.0f) } });
    f.structArrVar("arr", kStatsType);
    f.var("hp", PT::Float);
    f.var("lvl", PT::Int);
    f.var("tag", PT::String);
    f.var("moodName", PT::String);
    f.var("hit0", PT::Float);
    f.var("found", PT::Bool);
    f.var("idx", PT::Int);

    // Read: one field, a nested field, a whole break, an enum field by name.
    const int ev = f.event("Read");
    const int sHp = f.setVar("hp", PT::Float);
    { const int get = f.fieldOp(NT::GetStructField, kStatsType, "hp");
      f.data(f.getVarT("s", PT::Struct, kStatsType), 0, get, 0);
      f.data(get, 0, sHp, 0); }
    f.exec(ev, sHp);
    const int sTag = f.setVar("tag", PT::String);
    { const int inner = f.fieldOp(NT::GetStructField, kStatsType, "inner");
      const int tag   = f.fieldOp(NT::GetStructField, kInnerType, "tag");
      f.data(f.getVarT("s", PT::Struct, kStatsType), 0, inner, 0);
      f.data(inner, 0, tag, 0);
      f.data(tag, 0, sTag, 0); }
    f.exec(sHp, sTag);
    const int brk = f.structOp(NT::BreakStruct, kStatsType);
    f.data(f.getVarT("s", PT::Struct, kStatsType), 0, brk, 0);
    const int sLvl = f.setVar("lvl", PT::Int);
    f.data(brk, 1, sLvl, 0);                       // field 1 = lvl
    f.exec(sTag, sLvl);
    const int sMood = f.setVar("moodName", PT::String);
    { const int e2s = f.enumOp(NT::EnumToString, kMoodEnum);
      f.data(brk, 2, e2s, 0);                      // field 2 = mood (Enum)
      f.data(e2s, 0, sMood, 0); }
    f.exec(sLvl, sMood);
    const int sHit = f.setVar("hit0", PT::Float);
    { const int get = f.arrayOp(NT::ArrayGet, PT::Float);
      f.data(brk, 4, get, 0);                      // field 4 = hits (Float[])
      f.data(f.constI(1), 0, get, 1);
      f.data(get, 0, sHit, 0); }
    f.exec(sMood, sHit);

    // Write: a copy with one field replaced (pure — the source is untouched).
    const int evB = f.event("Bump");
    const int sS = f.setVarT("s", PT::Struct, kStatsType);
    { const int set = f.fieldOp(NT::SetStructField, kStatsType, "hp");
      const int add = f.op(NT::Add);
      const int get = f.fieldOp(NT::GetStructField, kStatsType, "hp");
      f.data(f.getVarT("s", PT::Struct, kStatsType), 0, get, 0);
      f.data(get, 0, add, 0);
      f.data(f.constF(1.0f), 0, add, 1);
      f.data(f.getVarT("s", PT::Struct, kStatsType), 0, set, 0);
      f.data(add, 0, set, 1);
      f.data(set, 0, sS, 0); }
    f.exec(evB, sS);

    // An UNWIRED struct input is not "the zero struct": the interpreter sees a
    // Value that is not of this definition and re-seeds from makeDefaultValue
    // before writing the field (§3.4).
    const int evU = f.event("Fresh");
    const int sU = f.setVarT("s", PT::Struct, kStatsType);
    { const int set = f.fieldOp(NT::SetStructField, kStatsType, "hp");
      f.g.findNode(set)->pinDefaults[1] = Value::ofFloat(5.0f);
      f.data(set, 0, sU, 0); }
    f.exec(evU, sU);

    // Make: one wired field, every other at the DEFINITION's default (not zero).
    const int evM = f.event("Make");
    const int sM = f.setVarT("s", PT::Struct, kStatsType);
    { const int mk = f.structOp(NT::MakeStruct, kStatsType);
      f.data(f.constF(7.0f), 0, mk, 0);            // hp
      f.data(mk, 0, sM, 0); }
    f.exec(evM, sM);

    // Struct arrays: Add copies; Contains/IndexOf can never match, because
    // valueEquals has no Struct case (§3.4).
    const int evA = f.event("Push");
    const int sArr = f.setVarT("arr", PT::Struct, kStatsType, true);
    { const int add = f.arrayOpT(NT::ArrayAdd, PT::Struct, kStatsType);
      f.data(f.getVarT("arr", PT::Struct, kStatsType, true), 0, add, 0);
      f.data(f.getVarT("s", PT::Struct, kStatsType), 0, add, 1);
      f.data(add, 0, sArr, 0); }
    f.exec(evA, sArr);
    const int sFound = f.setVar("found", PT::Bool);
    { const int c = f.arrayOpT(NT::ArrayContains, PT::Struct, kStatsType);
      f.data(f.getVarT("arr", PT::Struct, kStatsType, true), 0, c, 0);
      f.data(f.getVarT("s", PT::Struct, kStatsType), 0, c, 1);
      f.data(c, 0, sFound, 0); }
    f.exec(sArr, sFound);
    const int sIdx = f.setVar("idx", PT::Int);
    { const int c = f.arrayOpT(NT::ArrayIndexOf, PT::Struct, kStatsType);
      f.data(f.getVarT("arr", PT::Struct, kStatsType, true), 0, c, 0);
      f.data(f.getVarT("s", PT::Struct, kStatsType), 0, c, 1);
      f.data(c, 0, sIdx, 0); }
    f.exec(sFound, sIdx);

    // A function taking AND returning a struct — callFunction's Value seam.
    const int fn = f.fnEntry("Scale", 0,
                             { { "in", PT::Struct, false, kStatsType }, { "k", PT::Float } },
                             { { "out", PT::Struct, false, kStatsType } });
    const int ret = f.fnReturn("Scale");
    HorizonCode::syncFunctionSignatures(f.g);
    { const int set = f.fieldOp(NT::SetStructField, kStatsType, "hp");
      const int mul = f.op(NT::Multiply);
      const int get = f.fieldOp(NT::GetStructField, kStatsType, "hp");
      f.data(fn, 0, get, 0);
      f.data(get, 0, mul, 0);
      f.data(fn, 1, mul, 1);
      f.data(fn, 0, set, 0);
      f.data(mul, 0, set, 1);
      f.data(set, 0, ret, 0); }
    f.exec(fn, ret);
    return f.done("structs");
}

// 22 — the GameInstance: the one reference whose class is fixed at generation
// time, so calls through Get Game Instance resolve without a handle lookup.
inline HE::hccg::ClassSource fxGameInstance()
{
    Fx f;
    f.var("score", PT::Float);
    f.var("hidden", PT::Float, 0.0f, {}, /*access=*/1);

    const int fn = f.fnEntry("AddScore", 0, { { "amount", PT::Float } },
                                            { { "total", PT::Float } });
    const int sAdd = f.setVar("score", PT::Float);
    { const int a = f.op(NT::Add); f.data(f.getVar("score", PT::Float), 0, a, 0);
      f.data(fn, 0, a, 1); f.data(a, 0, sAdd, 0); }
    f.exec(fn, sAdd);
    const int ret = f.fnReturn("AddScore");
    HorizonCode::syncFunctionSignatures(f.g);
    f.data(f.getVar("score", PT::Float), 0, ret, 0);
    f.exec(sAdd, ret);
    return f.doneKey("__game_instance__", "game_instance");
}

// 23 — reaching the GameInstance: public variable and public function, plus the
// private ones that must stay on the Runtime's seam (warn paths).
inline HE::hccg::ClassSource fxGiCaller()
{
    Fx f;
    f.var("seen", PT::Float);
    f.var("total", PT::Float);
    f.var("sneak", PT::Float);

    const int ev = f.event("Bump");
    Node se; se.type = NT::SetExternal; se.s = "score"; se.propType = PT::Float;
    se.pinDefaults[1] = Value::ofFloat(3.0f);
    const int setSc = f.add(se);
    f.data(f.op(NT::GetGameInstance), 0, setSc, 0);
    f.exec(ev, setSc);
    const int sSeen = f.setVar("seen", PT::Float);
    { Node ge; ge.type = NT::GetExternal; ge.s = "score"; ge.propType = PT::Float;
      const int getSc = f.add(ge);
      f.data(f.op(NT::GetGameInstance), 0, getSc, 0);
      f.data(getSc, 0, sSeen, 0); }
    f.exec(setSc, sSeen);
    Node ce; ce.type = NT::CallExternal; ce.s = "AddScore";
    ce.params = { { "amount", PT::Float } }; ce.results = { { "total", PT::Float } };
    ce.pinDefaults[1] = Value::ofFloat(10.0f);
    const int call = f.add(ce);
    f.data(f.op(NT::GetGameInstance), 0, call, 0);
    f.exec(sSeen, call);
    const int sTotal = f.setVar("total", PT::Float);
    f.data(call, 0, sTotal, 0);
    f.exec(call, sTotal);
    // Private variable → the seam warns and yields zero, exactly as before.
    const int sSneak = f.setVar("sneak", PT::Float);
    { Node ge; ge.type = NT::GetExternal; ge.s = "hidden"; ge.propType = PT::Float;
      const int getH = f.add(ge);
      f.data(f.op(NT::GetGameInstance), 0, getH, 0);
      f.data(getH, 0, sSneak, 0); }
    f.exec(sTotal, sSneak);
    return f.done("gi_caller");
}

// 24 — the engine's own events: element filters, typed arguments, and names the
// engine spells out itself. Drives the CompiledInstance hooks.
inline HE::hccg::ClassSource fxEngineEvents()
{
    Fx f;
    f.var("trace", PT::String);
    f.var("text", PT::String);
    f.var("sum", PT::Float);
    f.var("built", PT::Float);

    auto append = [&f](int ev, const char* what) {
        const int s = f.setVar("trace", PT::String);
        const int cat = f.op(NT::Concat);
        f.data(f.getVar("trace", PT::String), 0, cat, 0);
        f.data(f.constS(what), 0, cat, 1);
        f.data(cat, 0, s, 0);
        f.exec(ev, s);
    };
    // Any element, then element 2 only — the same rule fireEvent applies.
    append(f.event("OnClicked", 0), "a");
    append(f.event("OnClicked", 2), "b");

    // A typed argument straight from the engine.
    const int evT = f.event("OnTextChanged", 0, true, PT::String);
    const int sText = f.setVar("text", PT::String);
    f.data(evT, 0, sText, 0);
    f.exec(evT, sText);

    const int evTick = f.event("Tick", 0, true, PT::Float);
    const int sSum = f.setVar("sum", PT::Float);
    { const int a = f.op(NT::Add); f.data(f.getVar("sum", PT::Float), 0, a, 0);
      f.data(evTick, 0, a, 1); f.data(a, 0, sSum, 0); }
    f.exec(evTick, sSum);

    // An event with no argument at all.
    const int evC = f.event("Construct");
    const int sB = f.setVar("built", PT::Float);
    f.g.findNode(sB)->pinDefaults[0] = Value::ofFloat(1.0f);
    f.exec(evC, sB);
    return f.done("engine_events");
}

// 25a — cast_target: the class fixture 25 casts references to. Its baseClass is
// what makes a Cast to "PlayerCharacter" (and, up the chain, to "Entity")
// meaningful, and it is what the generator turns into baseClassKey().
inline HE::hccg::ClassSource fxCastTarget()
{
    Fx f;
    f.var("constructed", PT::Float);
    const int ctor = f.event("Construct");
    const int sc = f.setVar("constructed", PT::Float);
    f.g.findNode(sc)->pinDefaults[0] = Value::ofFloat(1.0f);
    f.exec(ctor, sc);
    return f.done("cast_target", "PlayerCharacter");
}

// 25 — casts: every branch a Cast can take, against one live object.
// The fixtures are generated with OnFailure::Stop, so THIS is what proves the
// direct lowerings (hc::castClass for a HorizonCode class, hc::castBase for an
// engine base) agree with the interpreter's Runtime::instanceIsA — the fast
// path's own parity case, which would otherwise be the untested one.
inline HE::hccg::ClassSource fxCasts()
{
    Fx f;
    { Variable v; v.name = "obj"; v.type = PT::Ref; v.className = "fix/cast_target";
      f.g.variables.push_back(v); }
    f.var("objNull", PT::Ref);
    for (const char* n : { "exact", "base", "up", "side", "other", "null", "gotRef" })
        f.var(n, std::string(n) == "gotRef" ? PT::Ref : PT::Float);

    // Spawn the object the casts run against.
    const int evS = f.event("Spawn");
    Node co; co.type = NT::CreateObject; co.s = "fix/cast_target";
    const int create = f.add(co);
    f.exec(evS, create);
    const int sObj = f.setVar("obj", PT::Ref);
    f.data(create, 0, sObj, 0);
    f.exec(create, sObj);

    // One cast per case, chained: Success writes its marker, Failure the next
    // node's, so BOTH exec-outs of every Cast are reachable and the generated
    // if/else is exercised from both sides across the run.
    const int evG = f.event("Go");
    int prev = evG;
    int prevOut = 0;
    auto step = [&](const std::string& target, const char* marker, bool expectHit,
                    const char* refVar)
    {
        const int c = f.cast(target);
        f.exec(prev, c, prevOut);
        f.data(f.getVar("obj", PT::Ref), 0, c, 0);
        const int sm = f.setVar(marker, PT::Float);
        f.g.findNode(sm)->pinDefaults[0] = Value::ofFloat(1.0f);
        // Wire the marker onto the branch we expect, so a wrong answer shows up
        // as a MISSING trace entry rather than a merely different variable.
        f.exec(c, sm, expectHit ? 0 : 1);
        if (refVar)
        {
            const int sr = f.setVar(refVar, PT::Ref);
            f.data(c, 0, sr, 0);      // the cast's "As …" output
            f.exec(sm, sr);
            prev = sr;
        }
        else prev = sm;
        prevOut = 0;
    };
    step("fix/cast_target",  "exact", true,  "gotRef");  // exact HC class → hc::as
    step("PlayerCharacter",  "base",  true,  nullptr);   // its own engine base
    step("Entity",           "up",    true,  nullptr);   // up the chain
    step("PlayerController", "side",  false, nullptr);   // sideways → Failure
    step("fix/ref_target",   "other", false, nullptr);   // another class → Failure

    // A null reference takes Failure too — Cast doubles as the Is Valid guard.
    const int cNull = f.cast("Entity");
    f.exec(prev, cNull, prevOut);
    f.data(f.getVar("objNull", PT::Ref), 0, cNull, 0);
    const int sNull = f.setVar("null", PT::Float);
    f.g.findNode(sNull)->pinDefaults[0] = Value::ofFloat(1.0f);
    f.exec(cNull, sNull, 1);
    return f.done("casts");
}

// 26a — inherit_base: the base half of the inheritance pair. Everything a
// derived class is supposed to be able to reach lives here and NOWHERE in the
// derived fixture: an instance variable, a handler it overrides, a handler it
// does not, a public function and a private one.
inline HE::hccg::ClassSource fxInheritBase()
{
    Fx f;
    f.var("hp", PT::Float, 100.0f);
    f.var("hits", PT::Float);
    f.var("baseOnly", PT::Float);
    f.var("secret", PT::Float, 0.0f, {}, /*access=*/1);

    // Construct: the derived class overrides it, so on a derived instance this
    // must NOT run — baseOnly staying 0 is what proves it.
    const int ctor = f.event("Construct");
    const int sBase = f.setVar("baseOnly", PT::Float);
    f.g.findNode(sBase)->pinDefaults[0] = Value::ofFloat(1.0f);
    f.exec(ctor, sBase);

    // Ping is overridden too; Pong is not, so Pong on a derived instance runs
    // THIS handler — the "the base still answers what the child ignores" half.
    const int ping = f.event("Ping");
    const int sPing = f.setVar("hits", PT::Float);
    f.g.findNode(sPing)->pinDefaults[0] = Value::ofFloat(1.0f);
    f.exec(ping, sPing);

    const int pong = f.event("Pong");
    const int sPong = f.setVar("hits", PT::Float);
    { const int a = f.op(NT::Add);
      f.data(f.getVar("hits", PT::Float), 0, a, 0);
      f.data(f.constF(10.0f), 0, a, 1);
      f.data(a, 0, sPong, 0); }
    f.exec(pong, sPong);

    // A public function the derived class calls, results included: hp -= amount,
    // and the remainder comes back out.
    {
        const int fe = f.fnEntry("Damage", 0, { { "amount", PT::Float, false, {} } },
                                             { { "left",   PT::Float, false, {} } });
        f.g.findNode(fe)->subgraph = fe;
        const int sub = f.setVar("hp", PT::Float);
        f.g.findNode(sub)->subgraph = fe;
        const int a = f.op(NT::Subtract);
        f.g.findNode(a)->subgraph = fe;
        const int gh = f.getVar("hp", PT::Float);
        f.g.findNode(gh)->subgraph = fe;
        f.data(gh, 0, a, 0);
        f.data(fe, 0, a, 1);          // the "amount" parameter
        f.data(a, 0, sub, 0);
        f.exec(fe, sub);
        const int ret = f.fnReturn("Damage");
        f.g.findNode(ret)->subgraph = fe;
        f.g.findNode(ret)->results = f.g.findNode(fe)->results;
        const int gh2 = f.getVar("hp", PT::Float);
        f.g.findNode(gh2)->subgraph = fe;
        f.data(gh2, 0, ret, 0);
        f.exec(sub, ret);
    }
    // A PRIVATE one: a call from the derived class must reach nothing, in both
    // backends — Runtime::callFunction refuses a private member across a class
    // boundary, and the generator lowers that call to the same no-op.
    {
        const int fe = f.fnEntry("Secret", 1, {}, {});
        f.g.findNode(fe)->subgraph = fe;
        const int ss = f.setVar("secret", PT::Float);
        f.g.findNode(ss)->subgraph = fe;
        f.g.findNode(ss)->pinDefaults[0] = Value::ofFloat(1.0f);
        f.exec(fe, ss);
    }
    return f.done("inherit_base", "Entity");
}

// 26 — inherit_derived: `C_inherit_derived : public C_inherit_base`. Its graph
// holds ONLY its own nodes; everything else it reaches is inherited.
inline HE::hccg::ClassSource fxInheritDerived()
{
    Fx f;
    f.var("mine", PT::Float);
    f.var("left", PT::Float);
    f.var("isBase", PT::Float);

    // Overrides Construct: the base's handler must not also run.
    const int ctor = f.event("Construct");
    const int sMine = f.setVar("mine", PT::Float);
    f.g.findNode(sMine)->pinDefaults[0] = Value::ofFloat(5.0f);
    f.exec(ctor, sMine);

    // Overrides Ping, and writes an INHERITED variable while doing it — one
    // store in the interpreter, one C++ member here.
    const int ping = f.event("Ping");
    const int sHits = f.setVar("hits", PT::Float);
    f.g.findNode(sHits)->pinDefaults[0] = Value::ofFloat(99.0f);
    f.exec(ping, sHits);
    // Pong is deliberately absent — the base's handler answers it.

    // Calls the base's public function and keeps its result.
    {
        const int go = f.event("Go");
        const int call = f.fnCall("Damage");
        // No local FunctionEntry to mirror from, so the call carries the base's
        // signature itself — which is exactly what syncFunctionSignatures leaves
        // alone for a function declared in another graph.
        f.g.findNode(call)->params  = { { "amount", PT::Float, false, {} } };
        f.g.findNode(call)->results = { { "left",   PT::Float, false, {} } };
        f.exec(go, call);
        f.data(f.constF(30.0f), 0, call, 0);
        const int sLeft = f.setVar("left", PT::Float);
        f.data(call, 0, sLeft, 0);
        f.exec(call, sLeft);
        // …and the base's PRIVATE one, which reaches nothing on either backend.
        const int priv = f.fnCall("Secret");
        f.exec(sLeft, priv);
    }

    // Cast to the base CLASS: one exact tag per class cannot answer this, so it
    // is the case that proves the chain-aware lowering.
    {
        const int ev = f.event("Check");
        const int cast = f.cast("fix/inherit_base");
        f.exec(ev, cast);
        f.data(f.op(NT::GetSelf), 0, cast, 0);
        const int sIs = f.setVar("isBase", PT::Float);
        f.g.findNode(sIs)->pinDefaults[0] = Value::ofFloat(1.0f);
        f.exec(cast, sIs, 0);   // Success
    }
    return f.done("inherit_derived", "fix/inherit_base", "Entity");
}

// 27a/27 — a base class with NO instance variables at all, and a child that has
// one. slots() is a generated static rather than a virtual, so a child that
// concatenated its base's table here would name a function the base never
// emitted — a .cpp that fails to compile at EXPORT time, in front of the user.
// A class of nothing but events and functions is an ordinary shape (a
// controller, say), so this pair exists to keep that path compiling.
inline HE::hccg::ClassSource fxInheritNovarsBase()
{
    Fx f;
    const int fe = f.fnEntry("Bump", 0, {}, { { "out", PT::Float, false, {} } });
    f.g.findNode(fe)->subgraph = fe;
    const int ret = f.fnReturn("Bump");
    f.g.findNode(ret)->subgraph = fe;
    f.g.findNode(ret)->results = f.g.findNode(fe)->results;
    const int k = f.constF(7.0f);
    f.g.findNode(k)->subgraph = fe;
    f.data(k, 0, ret, 0);
    f.exec(fe, ret);
    return f.done("inherit_novars_base", "Entity");
}

inline HE::hccg::ClassSource fxInheritNovars()
{
    Fx f;
    f.var("got", PT::Float);
    const int go = f.event("Go");
    const int call = f.fnCall("Bump");
    f.g.findNode(call)->results = { { "out", PT::Float, false, {} } };
    f.exec(go, call);
    const int s = f.setVar("got", PT::Float);
    f.data(call, 0, s, 0);
    f.exec(call, s);
    return f.done("inherit_novars", "fix/inherit_novars_base", "Entity");
}

// 28 — input_actions: the Input Action node, which is the one ENTRY node with
// more than one chain. Pressed and Released are separate bodies reached from the
// same node, and an axis action is a third shape (one chain plus a value). The
// host fires all three as ordinary named events, so the harness drives both
// backends with exactly what PlayerHost sends.
inline HE::hccg::ClassSource fxInputActions()
{
    Fx f;
    f.var("downs", PT::Float);
    f.var("ups", PT::Float);
    f.var("axis", PT::Float);
    f.var("look", PT::Vec2);

    // Digital: two exec-outs off ONE node, counting separately so a chain that
    // ran the wrong body shows up as the wrong number rather than not at all.
    {
        Node ia; ia.type = NT::InputAction; ia.s = "Jump";
        const int a = f.add(ia);
        auto bump = [&](int outPin, const char* var)
        {
            const int s = f.setVar(var, PT::Float);
            const int add = f.op(NT::Add);
            f.data(f.getVar(var, PT::Float), 0, add, 0);
            f.data(f.constF(1.0f), 0, add, 1);
            f.data(add, 0, s, 0);
            f.exec(a, s, outPin);
        };
        bump(0, "downs");
        bump(1, "ups");
    }
    // Axis: one chain, and the value arrives on the event argument.
    {
        Node ia; ia.type = NT::InputAction; ia.s = "Move"; ia.hasArg = true;
        const int a = f.add(ia);
        const int s = f.setVar("axis", PT::Float);
        f.exec(a, s);
        f.data(a, 0, s, 0);   // the node's Value data-out
    }
    // A TWO-dimensional axis: same one chain, but the Value is a Vec2 and the
    // event it answers to is ".Axis2D". Coercing that to a Float somewhere in
    // the generated code would hand the graph one component or a zero, and this
    // is the case that would catch it.
    {
        Node ia; ia.type = NT::InputAction; ia.s = "Look";
        ia.hasArg = true; ia.propType = PT::Vec2;
        const int a = f.add(ia);
        const int s = f.setVar("look", PT::Vec2);
        f.exec(a, s);
        f.data(a, 0, s, 0);
    }
    return f.done("input_actions", "PlayerCharacter");
}

// ── Set<T> / Map<K,V> ────────────────────────────────────────────────────────
// The fixture that pins the ORDERING CONTRACT (docs/horizoncode-containers-plan.md
// §1.2). Every case here is one where a hash-backed or sorted container would
// give a different answer than the interpreter's vectors — which is exactly the
// divergence this harness exists to catch:
//   • re-adding an element a set already has does not move it to the back
//   • re-inserting a key a map already has updates in place and keeps its slot
//   • removal preserves the relative order of everything else
//   • Keys and Values come out index-parallel, in insertion order
//   • the loops walk that same order
// Deliberately built with keys whose insertion order is NOT their sorted order
// ("z" before "a", Angry(5) before Calm(0)), so "it happens to match" is not a
// way for a wrong implementation to pass.
inline HE::hccg::ClassSource fxContainers()
{
    Fx f;
    f.setVarDecl("tags", PT::String);
    f.setVarDecl("tagsAfter", PT::String);
    f.arrVar("tagsArr", PT::String, {});
    f.var("tagCount", PT::Int);
    f.var("hasA", PT::Bool);
    f.var("clearedTags", PT::Int);
    f.mapVarDecl("scores", PT::String, PT::Int);
    f.arrVar("keys", PT::String, {});
    f.arrVar("vals", PT::Int, {});
    f.var("hitB", PT::Int);
    f.var("missZ", PT::Int);
    f.arrVar("keysAfter", PT::String, {});
    f.var("stillHasB", PT::Bool);
    f.var("emptyLen", PT::Int);
    f.var("clearedLen", PT::Int);
    // Seeded declarations: the duplicate in the set collapses to its FIRST
    // occurrence, and the map's authored order survives (an alphabetical
    // "alpha, zeta" would be the JSON-object bug this feature must not have).
    f.setVarDecl("seedSet", PT::String,
                 { Value::ofString("zeta"), Value::ofString("alpha"), Value::ofString("zeta") });
    f.mapVarDecl("seedMap", PT::String, PT::Int,
                 { Value::ofString("zeta"), Value::ofString("alpha") },
                 { Value::ofInt(9), Value::ofInt(1) });
    f.var("setOrder", PT::String);
    f.var("mapOrder", PT::String);
    // Enum elements/keys: the one type that is a plain int in generated C++ but
    // a tagged Value at the boundary, so its container boxing has its own path.
    f.setVarDecl("moods", PT::Enum, {}, kMoodEnum);
    f.mapVarDecl("moodHp", PT::Enum, PT::Int, {}, {}, kMoodEnum);
    f.var("angryHp", PT::Int);

    // ── Set ops ──────────────────────────────────────────────────────────────
    const int evS = f.event("SetOps");
    const int sTags = f.setSetVar("tags", PT::String);
    {
        const int mk = f.setOp(NT::SetMake, PT::String);
        int prev = mk;
        for (const char* s : { "b", "a", "b", "c" })   // the second "b" is a no-op
        {
            const int a = f.setOp(NT::SetAdd, PT::String);
            f.data(prev, 0, a, 0);
            f.data(f.constS(s), 0, a, 1);
            prev = a;
        }
        f.data(prev, 0, sTags, 0);
    }
    f.exec(evS, sTags);
    const int sAfter = f.setSetVar("tagsAfter", PT::String);
    {
        const int rm = f.setOp(NT::SetRemove, PT::String);
        f.data(f.getSetVar("tags", PT::String), 0, rm, 0);
        f.data(f.constS("a"), 0, rm, 1);              // → b, c
        f.data(rm, 0, sAfter, 0);
    }
    f.exec(sTags, sAfter);
    const int sArr = f.setVar("tagsArr", PT::String, true);
    {
        const int ta = f.setOp(NT::SetToArray, PT::String);
        f.data(f.getSetVar("tagsAfter", PT::String), 0, ta, 0);
        f.data(ta, 0, sArr, 0);
    }
    f.exec(sAfter, sArr);
    const int sCount = f.setVar("tagCount", PT::Int);
    {
        const int len = f.setOp(NT::SetLength, PT::String);
        f.data(f.getSetVar("tags", PT::String), 0, len, 0);
        f.data(len, 0, sCount, 0);
    }
    f.exec(sArr, sCount);
    const int sHas = f.setVar("hasA", PT::Bool);
    {
        const int c = f.setOp(NT::SetContains, PT::String);
        f.data(f.getSetVar("tagsAfter", PT::String), 0, c, 0);
        f.data(f.constS("a"), 0, c, 1);               // removed above → false
        f.data(c, 0, sHas, 0);
    }
    f.exec(sCount, sHas);
    const int sCleared = f.setVar("clearedTags", PT::Int);
    {
        const int cl = f.setOp(NT::SetClear, PT::String);
        f.data(f.getSetVar("tags", PT::String), 0, cl, 0);
        const int len = f.setOp(NT::SetLength, PT::String);
        f.data(cl, 0, len, 0);
        f.data(len, 0, sCleared, 0);
    }
    f.exec(sHas, sCleared);

    // ── Map ops ──────────────────────────────────────────────────────────────
    const int evM = f.event("MapOps");
    const int sScores = f.setMapVar("scores", PT::String, PT::Int);
    {
        const int mk = f.mapOp(NT::MapMake, PT::String, PT::Int);
        int prev = mk;
        const struct { const char* k; int v; } pairs[] =
            { { "b", 1 }, { "a", 2 }, { "b", 3 } };   // "b" updates IN PLACE
        for (const auto& p : pairs)
        {
            const int st = f.mapOp(NT::MapSet, PT::String, PT::Int);
            f.data(prev, 0, st, 0);
            f.data(f.constS(p.k), 0, st, 1);
            f.data(f.constI(p.v), 0, st, 2);
            prev = st;
        }
        f.data(prev, 0, sScores, 0);
    }
    f.exec(evM, sScores);
    const int sKeys = f.setVar("keys", PT::String, true);
    {
        const int k = f.mapOp(NT::MapKeys, PT::String, PT::Int);
        f.data(f.getMapVar("scores", PT::String, PT::Int), 0, k, 0);
        f.data(k, 0, sKeys, 0);
    }
    f.exec(sScores, sKeys);
    const int sVals = f.setVar("vals", PT::Int, true);
    {
        const int v = f.mapOp(NT::MapValues, PT::String, PT::Int);
        f.data(f.getMapVar("scores", PT::String, PT::Int), 0, v, 0);
        f.data(v, 0, sVals, 0);
    }
    f.exec(sKeys, sVals);
    const int sHit = f.setVar("hitB", PT::Int);
    {
        const int gg = f.mapOp(NT::MapGet, PT::String, PT::Int);
        f.data(f.getMapVar("scores", PT::String, PT::Int), 0, gg, 0);
        f.data(f.constS("b"), 0, gg, 1);
        f.data(f.constI(-1), 0, gg, 2);
        f.data(gg, 0, sHit, 0);
    }
    f.exec(sVals, sHit);
    const int sMiss = f.setVar("missZ", PT::Int);
    {
        const int gg = f.mapOp(NT::MapGet, PT::String, PT::Int);
        f.data(f.getMapVar("scores", PT::String, PT::Int), 0, gg, 0);
        f.data(f.constS("zz"), 0, gg, 1);
        f.data(f.constI(-1), 0, gg, 2);               // absent → the Default pin
        f.data(gg, 0, sMiss, 0);
    }
    f.exec(sHit, sMiss);
    const int sKA = f.setVar("keysAfter", PT::String, true);
    {
        const int rm = f.mapOp(NT::MapRemove, PT::String, PT::Int);
        f.data(f.getMapVar("scores", PT::String, PT::Int), 0, rm, 0);
        f.data(f.constS("b"), 0, rm, 1);              // → just "a"
        const int k = f.mapOp(NT::MapKeys, PT::String, PT::Int);
        f.data(rm, 0, k, 0);
        f.data(k, 0, sKA, 0);
        const int has = f.mapOp(NT::MapContains, PT::String, PT::Int);
        f.data(rm, 0, has, 0);
        f.data(f.constS("b"), 0, has, 1);
        const int sSt = f.setVar("stillHasB", PT::Bool);
        f.data(has, 0, sSt, 0);
        f.exec(sMiss, sKA);
        f.exec(sKA, sSt);
        // An UNWIRED Map input: its zero is an empty map, so Length is 0 —
        // not whatever a scalar zero would read out of the wrong field.
        const int len = f.mapOp(NT::MapLength, PT::String, PT::Int);
        const int sEl = f.setVar("emptyLen", PT::Int);
        f.data(len, 0, sEl, 0);
        f.exec(sSt, sEl);
        const int cl = f.mapOp(NT::MapClear, PT::String, PT::Int);
        f.data(f.getMapVar("scores", PT::String, PT::Int), 0, cl, 0);
        const int len2 = f.mapOp(NT::MapLength, PT::String, PT::Int);
        f.data(cl, 0, len2, 0);
        const int sCl = f.setVar("clearedLen", PT::Int);
        f.data(len2, 0, sCl, 0);
        f.exec(sEl, sCl);
    }

    // ── Iteration ────────────────────────────────────────────────────────────
    // Both loops append what they visit to a string, so the ASSERTION is the
    // order itself: "zetaalpha", not a multiset that any order would satisfy.
    const int evI = f.event("Iterate");
    const int fes = f.forEachSet(PT::String);
    f.data(f.getSetVar("seedSet", PT::String), 0, fes, 0);
    f.exec(evI, fes);
    {
        const int s = f.setVar("setOrder", PT::String);
        const int cat = f.op(NT::Concat);
        f.data(f.getVar("setOrder", PT::String), 0, cat, 0);
        f.data(fes, 0, cat, 1);           // Element
        f.data(cat, 0, s, 0);
        f.exec(fes, s, 0);                // Body
    }
    const int fem = f.forEachMap(PT::String, PT::Int);
    f.data(f.getMapVar("seedMap", PT::String, PT::Int), 0, fem, 0);
    f.exec(fes, fem, 1);                  // Done → the map loop
    {
        const int s = f.setVar("mapOrder", PT::String);
        const int cat1 = f.op(NT::Concat);
        f.data(f.getVar("mapOrder", PT::String), 0, cat1, 0);
        f.data(fem, 0, cat1, 1);          // Key
        const int ts = f.op(NT::ToString);
        f.data(fem, 1, ts, 0);            // Value
        const int cat2 = f.op(NT::Concat);
        f.data(cat1, 0, cat2, 0);
        f.data(ts, 0, cat2, 1);
        f.data(cat2, 0, s, 0);
        f.exec(fem, s, 0);                // Body
    }

    // ── Enum elements and enum keys ──────────────────────────────────────────
    const int evE = f.event("EnumContainers");
    const int sMoods = f.setSetVar("moods", PT::Enum, kMoodEnum);
    {
        const int mk = f.setOp(NT::SetMake, PT::Enum, kMoodEnum);
        const int a1 = f.setOp(NT::SetAdd, PT::Enum, kMoodEnum);
        f.data(mk, 0, a1, 0);
        f.data(f.constEnum(kMoodEnum, 5), 0, a1, 1);      // Angry
        const int a2 = f.setOp(NT::SetAdd, PT::Enum, kMoodEnum);
        f.data(a1, 0, a2, 0);
        f.data(f.constEnum(kMoodEnum, 0), 0, a2, 1);      // Calm — AFTER Angry
        const int a3 = f.setOp(NT::SetAdd, PT::Enum, kMoodEnum);
        f.data(a2, 0, a3, 0);
        f.data(f.constEnum(kMoodEnum, 5), 0, a3, 1);      // Angry again → no-op
        f.data(a3, 0, sMoods, 0);
    }
    f.exec(evE, sMoods);
    const int sMoodHp = f.setMapVar("moodHp", PT::Enum, PT::Int, kMoodEnum);
    {
        const int mk = f.mapOp(NT::MapMake, PT::Enum, PT::Int, kMoodEnum);
        const int s1 = f.mapOp(NT::MapSet, PT::Enum, PT::Int, kMoodEnum);
        f.data(mk, 0, s1, 0);
        f.data(f.constEnum(kMoodEnum, 5), 0, s1, 1);
        f.data(f.constI(11), 0, s1, 2);
        const int s2 = f.mapOp(NT::MapSet, PT::Enum, PT::Int, kMoodEnum);
        f.data(s1, 0, s2, 0);
        f.data(f.constEnum(kMoodEnum, 1), 0, s2, 1);      // Happy
        f.data(f.constI(22), 0, s2, 2);
        f.data(s2, 0, sMoodHp, 0);
    }
    f.exec(sMoods, sMoodHp);
    const int sAngry = f.setVar("angryHp", PT::Int);
    {
        const int gg = f.mapOp(NT::MapGet, PT::Enum, PT::Int, kMoodEnum);
        f.data(f.getMapVar("moodHp", PT::Enum, PT::Int, kMoodEnum), 0, gg, 0);
        f.data(f.constEnum(kMoodEnum, 5), 0, gg, 1);
        f.data(f.constI(-1), 0, gg, 2);
        f.data(gg, 0, sAngry, 0);
    }
    f.exec(sMoodHp, sAngry);

    return f.done("containers");
}

inline std::vector<HE::hccg::ClassSource> all()
{
    registerTypes();   // the fixtures' Struct/Enum definitions, for both consumers
    return {
        fxFlow(), fxCoerce(), fxMath(), fxVectorOps(), fxVariables(), fxFunctionsBasic(),
        fxFunctionsRecursive(), fxForeachArrays(), fxEventsMulti(),
        fxWidgetProps(), fxLimitsSmoke(), fxFunctionsLocals(),
        fxEnginePureMultiout(), fxEngineExecCached(), fxAnimatorSync(),
        fxRefTarget(), fxRefsObjects(), fxDispatchOwner(), fxDispatchListener(),
        fxDispatchSink(), fxLatentFlow(), fxEnums(), fxStructs(),
        fxGameInstance(), fxGiCaller(), fxEngineEvents(),
        fxCastTarget(), fxCasts(),
        fxInheritBase(), fxInheritDerived(),
        fxInheritNovarsBase(), fxInheritNovars(),
        fxInputActions(), fxContainers(),
    };
}

} // namespace hcfix
