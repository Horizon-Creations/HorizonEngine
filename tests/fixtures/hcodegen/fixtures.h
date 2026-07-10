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

    HE::hccg::ClassSource done(const std::string& name)
    {
        HorizonCode::syncFunctionSignatures(g);
        // Production graphs arrive through fromJson — round-trip so both
        // backends consume exactly what a shipped asset would contain.
        Graph rt;
        must(HorizonCode::fromJson(HorizonCode::toJson(g), rt), "json round trip");
        return { "fix/" + name, name, std::move(rt) };
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
    const int show = f.op(NT::ShowWidget);
    f.exec(sCopy, show);
    const int hide = f.op(NT::HideWidget);
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
    // save round trip: exec set, pure get (dispatch per read).
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
    f.var("obj", PT::Ref);
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

inline std::vector<HE::hccg::ClassSource> all()
{
    return {
        fxFlow(), fxCoerce(), fxMath(), fxVariables(), fxFunctionsBasic(),
        fxFunctionsRecursive(), fxForeachArrays(), fxEventsMulti(),
        fxWidgetProps(), fxLimitsSmoke(), fxFunctionsLocals(),
        fxEnginePureMultiout(), fxEngineExecCached(),
        fxRefTarget(), fxRefsObjects(), fxDispatchOwner(), fxDispatchListener(),
        fxDispatchSink(),
    };
}

} // namespace hcfix
