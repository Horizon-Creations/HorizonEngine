#include "doctest.h"
#include <HorizonCode/HorizonCode.h>
#include <Types/TypeRegistry.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

// Struct/Enum nodes in the HorizonCode interpreter: Make/Break/SetField build
// and split user-defined struct values, ConstEnum/SwitchOnEnum/conversions run
// against the TypeRegistry definitions, and the whole shape survives JSON.
//
// Unified pin order per node: [execIns][execOuts][dataIns][dataOuts].

using namespace HorizonCode;

namespace
{
constexpr const char* kWeapon = "Content/Types/Weapon.hasset";
constexpr const char* kStats  = "Content/Types/PlayerStats.hasset";

struct TypeFixture
{
    TypeFixture()
    {
        auto& reg = HE::TypeRegistry::instance();
        HE::EnumDef weapon;
        weapon.name = "Weapon"; weapon.assetPath = kWeapon;
        weapon.entries = { { "Sword", 0 }, { "Bow", 7 }, { "Staff", 2 } };
        reg.registerEnum(weapon);

        HE::StructDef stats;
        stats.name = "PlayerStats"; stats.assetPath = kStats;
        {
            HE::StructField hp; hp.name = "hp"; hp.type = PinType::Float;
            hp.defaultValue = Value::ofFloat(100.0f);
            HE::StructField title; title.name = "title"; title.type = PinType::String;
            title.defaultValue = Value::ofString("Rookie");
            HE::StructField weap; weap.name = "weapon"; weap.type = PinType::Enum;
            weap.typeName = kWeapon; weap.defaultValue.s = "Bow";
            stats.fields = { hp, title, weap };
        }
        reg.registerStruct(stats);
    }
    ~TypeFixture()
    {
        HE::TypeRegistry::instance().removeType(kWeapon);
        HE::TypeRegistry::instance().removeType(kStats);
    }
};

// A Runner context backed by a plain map (like the widget/instance stores).
struct VarStore
{
    std::unordered_map<std::string, Value> vars;
    Context ctx()
    {
        Context c;
        c.getVariable = [this](const std::string& n){ auto it = vars.find(n); return it != vars.end() ? it->second : Value{}; };
        c.setVariable = [this](const std::string& n, const Value& v){ vars[n] = v; };
        return c;
    }
};

int addTypedNode(Graph& g, NodeType t, const std::string& typeName)
{
    Node n; n.type = t; n.typeName = typeName;
    const int id = g.addNode(std::move(n));
    syncTypeSignatures(g);   // mirror fields/entries from the registry
    return id;
}
} // namespace

TEST_CASE("MakeStruct: wired fields override, unwired fields keep their defaults")
{
    TypeFixture fx;
    Graph g;
    Variable out; out.name = "out"; out.type = PinType::Struct; out.typeName = kStats;
    g.variables.push_back(out);

    Node ev; ev.type = NodeType::Event; ev.s = "Go";
    const int e = g.addNode(ev);
    const int mk = addTypedNode(g, NodeType::MakeStruct, kStats);
    Node cf; cf.type = NodeType::ConstFloat; cf.f[0] = 5.0f;
    const int c = g.addNode(cf);
    Node sv; sv.type = NodeType::SetVariable; sv.s = "out";
    sv.propType = PinType::Struct; sv.typeName = kStats;
    const int s = g.addNode(sv);

    // MakeStruct pins: dataIns hp=0 title=1 weapon=2, dataOut Struct=3.
    REQUIRE(g.connect(c, 0, mk, 0));               // hp ← 5
    REQUIRE(g.connect(e, 0, s, 0));                // exec
    REQUIRE(g.connect(mk, 3, s, 2));               // Value ← struct

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go", 0, {});

    const Value& v = store.vars["out"];
    REQUIRE(v.type == PinType::Struct);
    CHECK(v.typeName == kStats);
    REQUIRE(v.items.size() == 3);
    CHECK(v.items[0].f == doctest::Approx(5.0f));      // wired
    CHECK(v.items[1].s == "Rookie");                   // default
    CHECK(v.items[2].type == PinType::Enum);
    CHECK(v.items[2].i == 7);                          // default "Bow"
}

TEST_CASE("BreakStruct + SetStructField resolve fields by name and copy on write")
{
    TypeFixture fx;
    Graph g;
    Variable hpVar; hpVar.name = "hp"; hpVar.type = PinType::Float;
    g.variables.push_back(hpVar);

    Node ev; ev.type = NodeType::Event; ev.s = "Go";
    const int e = g.addNode(ev);
    const int mk = addTypedNode(g, NodeType::MakeStruct, kStats);      // all defaults
    const int sf = addTypedNode(g, NodeType::SetStructField, kStats);
    // Choose the hp field on the SetStructField (what the editor's combo does).
    {
        Node* n = g.findNode(sf);
        n->params.clear();
        n->params.push_back({ "hp", PinType::Float, false, {} });
    }
    Node cf; cf.type = NodeType::ConstFloat; cf.f[0] = 9.0f;
    const int c = g.addNode(cf);
    const int br = addTypedNode(g, NodeType::BreakStruct, kStats);
    Node sv; sv.type = NodeType::SetVariable; sv.s = "hp"; sv.propType = PinType::Float;
    const int s = g.addNode(sv);

    // SetStructField pins: dataIns Struct=0 Value=1, dataOut=2.
    REQUIRE(g.connect(mk, 3, sf, 0));
    REQUIRE(g.connect(c, 0, sf, 1));
    // BreakStruct pins: dataIn Struct=0, dataOuts hp=1 title=2 weapon=3.
    REQUIRE(g.connect(sf, 2, br, 0));
    REQUIRE(g.connect(e, 0, s, 0));
    REQUIRE(g.connect(br, 1, s, 2));               // hp out → variable

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go", 0, {});
    CHECK(store.vars["hp"].f == doctest::Approx(9.0f));
}

TEST_CASE("GetStructField reads one field and survives a definition edit")
{
    TypeFixture fx;
    Graph g;
    Variable title; title.name = "title"; title.type = PinType::String;
    g.variables.push_back(title);

    Node ev; ev.type = NodeType::Event; ev.s = "Go";
    const int e = g.addNode(ev);
    const int mk = addTypedNode(g, NodeType::MakeStruct, kStats);   // all defaults
    const int gf = addTypedNode(g, NodeType::GetStructField, kStats);
    { Node* n = g.findNode(gf);
      n->params.clear();
      n->params.push_back({ "title", PinType::String, false, {} }); }
    syncTypeSignatures(g);
    Node sv; sv.type = NodeType::SetVariable; sv.s = "title"; sv.propType = PinType::String;
    const int s = g.addNode(sv);

    // GetStructField pins: dataIn Struct=0, dataOut field=1.
    REQUIRE(g.connect(mk, 3, gf, 0));
    REQUIRE(g.connect(e, 0, s, 0));
    REQUIRE(g.connect(gf, 1, s, 2));

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go", 0, {});
    CHECK(store.vars["title"].s == "Rookie");

    // A field INSERTED before it must not shift the read (name resolution).
    {
        HE::StructDef edited;
        REQUIRE(HE::TypeRegistry::instance().getStruct(kStats, edited));
        HE::StructField extra; extra.name = "armor"; extra.type = PinType::Float;
        extra.defaultValue = Value::ofFloat(4.0f);
        edited.fields.insert(edited.fields.begin(), extra);
        HE::TypeRegistry::instance().registerStruct(edited);
    }
    syncTypeSignatures(g);   // re-mirrors MakeStruct's inputs (now 4)
    VarStore store2;
    Runner r2(g, store2.ctx());
    r2.fireEvent("Go", 0, {});
    CHECK(store2.vars["title"].s == "Rookie");

    // A REMOVED field clears the choice; the node then reads a typed zero.
    {
        HE::StructDef edited;
        REQUIRE(HE::TypeRegistry::instance().getStruct(kStats, edited));
        edited.fields.erase(std::remove_if(edited.fields.begin(), edited.fields.end(),
            [](const HE::StructField& f){ return f.name == "title"; }), edited.fields.end());
        HE::TypeRegistry::instance().registerStruct(edited);
    }
    syncTypeSignatures(g);
    CHECK(g.findNode(gf)->params.empty());
    VarStore store3;
    Runner r3(g, store3.ctx());
    r3.fireEvent("Go", 0, {});
    CHECK(store3.vars["title"].s.empty());
}

TEST_CASE("ConstEnum + SwitchOnEnum route by entry, renumber-safely by name")
{
    TypeFixture fx;
    Graph g;
    Variable hit; hit.name = "hit"; hit.type = PinType::String;
    g.variables.push_back(hit);

    Node ev; ev.type = NodeType::Event; ev.s = "Go";
    const int e = g.addNode(ev);
    const int ce = addTypedNode(g, NodeType::ConstEnum, kWeapon);
    g.findNode(ce)->f[0] = 7.0f;                   // Bow
    const int sw = addTypedNode(g, NodeType::SwitchOnEnum, kWeapon);

    auto setterOn = [&](const char* text) {
        Node cs; cs.type = NodeType::ConstString; cs.s = text;
        const int c = g.addNode(cs);
        Node sv; sv.type = NodeType::SetVariable; sv.s = "hit"; sv.propType = PinType::String;
        const int s = g.addNode(sv);
        REQUIRE(g.connect(c, 0, s, 2));
        return s;
    };
    // SwitchOnEnum pins: execIn 0; execOuts Sword=1 Bow=2 Staff=3 Default=4; dataIn=5.
    REQUIRE(g.connect(e, 0, sw, 0));
    REQUIRE(g.connect(ce, 0, sw, 5));
    REQUIRE(g.connect(sw, 1, setterOn("sword"), 0));
    REQUIRE(g.connect(sw, 2, setterOn("bow"), 0));
    REQUIRE(g.connect(sw, 4, setterOn("default"), 0));

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go", 0, {});
    CHECK(store.vars["hit"].s == "bow");

    // An unmapped value falls through to Default.
    g.findNode(ce)->f[0] = 42.0f;
    VarStore store2;
    Runner r2(g, store2.ctx());
    r2.fireEvent("Go", 0, {});
    CHECK(store2.vars["hit"].s == "default");
}

TEST_CASE("Enum conversions: ToInt, ToString, IntToEnum")
{
    TypeFixture fx;
    Graph g;
    Variable a; a.name = "asInt"; a.type = PinType::Int;
    Variable b; b.name = "asStr"; b.type = PinType::String;
    g.variables.push_back(a); g.variables.push_back(b);

    Node ev; ev.type = NodeType::Event; ev.s = "Go";
    const int e = g.addNode(ev);
    Node ci; ci.type = NodeType::ConstInt; ci.f[0] = 2.0f;   // Staff
    const int c = g.addNode(ci);
    const int i2e = addTypedNode(g, NodeType::IntToEnum, kWeapon);
    const int e2i = addTypedNode(g, NodeType::EnumToInt, kWeapon);
    const int e2s = addTypedNode(g, NodeType::EnumToString, kWeapon);
    Node s1; s1.type = NodeType::SetVariable; s1.s = "asInt"; s1.propType = PinType::Int;
    const int sv1 = g.addNode(s1);
    Node s2; s2.type = NodeType::SetVariable; s2.s = "asStr"; s2.propType = PinType::String;
    const int sv2 = g.addNode(s2);

    REQUIRE(g.connect(c, 0, i2e, 0));              // ConstInt dataOut (pin 0) → IntToEnum in
    REQUIRE(g.connect(i2e, 1, e2i, 0));            // IntToEnum: dataIn=0, dataOut=1
    REQUIRE(g.connect(i2e, 1, e2s, 0));
    REQUIRE(g.connect(e, 0, sv1, 0));
    REQUIRE(g.connect(e2i, 1, sv1, 2));
    REQUIRE(g.connect(sv1, 1, sv2, 0));            // chain exec
    REQUIRE(g.connect(e2s, 1, sv2, 2));

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go", 0, {});
    CHECK(store.vars["asInt"].i == 2);
    CHECK(store.vars["asStr"].s == "Staff");
}

TEST_CASE("Struct/enum nodes and typed variables survive the JSON round-trip")
{
    TypeFixture fx;
    Graph g;
    Variable v; v.name = "stats"; v.type = PinType::Struct; v.typeName = kStats;
    g.variables.push_back(v);
    const int mk = addTypedNode(g, NodeType::MakeStruct, kStats);
    const int ce = addTypedNode(g, NodeType::ConstEnum, kWeapon);
    (void)ce;

    const std::string json = toJson(g);
    Graph back;
    REQUIRE(fromJson(json, back));
    const Node* mkb = back.findNode(mk);
    REQUIRE(mkb != nullptr);
    CHECK(mkb->typeName == kStats);
    REQUIRE(mkb->params.size() == 3);              // re-mirrored on load
    CHECK(mkb->params[2].type == PinType::Enum);
    CHECK(mkb->params[2].typeName == kWeapon);
    REQUIRE(back.variables.size() == 1);
    CHECK(back.variables[0].typeName == kStats);
    CHECK(back.variables[0].type == PinType::Struct);
}

TEST_CASE("connect() refuses to wire mismatched struct/enum definitions")
{
    TypeFixture fx;
    // A second struct to mismatch against.
    HE::StructDef other; other.name = "Other"; other.assetPath = "Content/Types/Other.hasset";
    other.fields.push_back({ "x", PinType::Float });
    HE::TypeRegistry::instance().registerStruct(other);

    Graph g;
    const int mk = addTypedNode(g, NodeType::MakeStruct, kStats);        // PlayerStats out (pin 3)
    const int brOther = addTypedNode(g, NodeType::BreakStruct, "Content/Types/Other.hasset");
    const int brStats = addTypedNode(g, NodeType::BreakStruct, kStats);

    CHECK(!g.connect(mk, 3, brOther, 0));          // different struct → refused
    CHECK(g.connect(mk, 3, brStats, 0));           // same struct → wired

    HE::TypeRegistry::instance().removeType("Content/Types/Other.hasset");
}

TEST_CASE("Struct variables: per-graph field overrides win, by NAME, and survive JSON")
{
    TypeFixture fx;
    Variable v; v.name = "stats"; v.type = PinType::Struct; v.typeName = kStats;
    // hp (Float 100), title (String "Rookie"), weapon (Enum, "Bow" = 7).
    v.structDefaults["hp"]     = Value::ofFloat(55.0f);
    v.structDefaults["weapon"] = Value::ofString("Staff");   // enum override = entry NAME

    const Value seeded = variableDefaultValue(v);
    REQUIRE(seeded.items.size() == 3);
    CHECK(seeded.items[0].f == doctest::Approx(55.0f));   // overridden
    CHECK(seeded.items[1].s == "Rookie");                 // untouched → definition default
    CHECK(seeded.items[2].i == 2);                        // "Staff" resolved

    // A field INSERTED before the overridden ones must not shift them.
    {
        HE::StructDef edited;
        REQUIRE(HE::TypeRegistry::instance().getStruct(kStats, edited));
        HE::StructField extra; extra.name = "armor"; extra.type = PinType::Float;
        extra.defaultValue = Value::ofFloat(4.0f);
        edited.fields.insert(edited.fields.begin(), extra);
        HE::TypeRegistry::instance().registerStruct(edited);
    }
    const Value after = variableDefaultValue(v);
    REQUIRE(after.items.size() == 4);
    CHECK(after.items[0].f == doctest::Approx(4.0f));     // the new field's own default
    CHECK(after.items[1].f == doctest::Approx(55.0f));    // the override followed its NAME
    CHECK(after.items[3].i == 2);

    // A stale enum entry name falls back to the definition default, silently.
    v.structDefaults["weapon"] = Value::ofString("Nonexistent");
    CHECK(variableDefaultValue(v).items[3].i == 7);       // "Bow"

    // Overrides round-trip through the graph JSON.
    Graph g;
    g.variables.push_back(v);
    Graph back;
    REQUIRE(fromJson(toJson(g), back));
    REQUIRE(back.variables.size() == 1);
    const auto& sd = back.variables[0].structDefaults;
    REQUIRE(sd.size() == 2);
    CHECK(sd.at("hp").f == doctest::Approx(55.0f));
    CHECK(sd.at("weapon").s == "Nonexistent");
}

TEST_CASE("Struct variables seed from definition defaults; enum defaults resolve by name")
{
    TypeFixture fx;
    Variable sv; sv.name = "stats"; sv.type = PinType::Struct; sv.typeName = kStats;
    const Value seeded = variableDefaultValue(sv);
    REQUIRE(seeded.type == PinType::Struct);
    REQUIRE(seeded.items.size() == 3);
    CHECK(seeded.items[0].f == doctest::Approx(100.0f));
    CHECK(seeded.items[2].i == 7);                 // weapon default "Bow"

    Variable ev; ev.name = "weapon"; ev.type = PinType::Enum; ev.typeName = kWeapon;
    ev.s = "Staff";
    const Value en = variableDefaultValue(ev);
    CHECK(en.type == PinType::Enum);
    CHECK(en.i == 2);

    ev.s = "Nonexistent";                          // stale name → first entry
    CHECK(variableDefaultValue(ev).i == 0);
}

// ── Pins of user-defined types carry WHICH definition ───────────────────────
// Without it a Struct/Enum pin is an unresolvable type: no field pins, no entry
// list, "definition missing" in every panel — and the codegen has no C++ type.

TEST_CASE("Array/ForEach pins expose their element definition")
{
    TypeFixture fx;
    Node arr; arr.type = NodeType::ArrayGet;
    arr.propType = PinType::Struct; arr.typeName = kStats;
    const NodeSig s = signatureOf(arr);
    REQUIRE(s.dataIns.size() == 2);
    REQUIRE(s.dataOuts.size() == 1);
    CHECK(std::string(s.dataIns[0].typeName ? s.dataIns[0].typeName : "") == kStats);
    CHECK(std::string(s.dataOuts[0].typeName ? s.dataOuts[0].typeName : "") == kStats);
    // A built-in element type still reports null, as PinDesc documents.
    Node plain; plain.type = NodeType::ArrayGet; plain.propType = PinType::Float;
    CHECK(signatureOf(plain).dataOuts[0].typeName == nullptr);
}

TEST_CASE("A loaded graph recovers Enum/Struct definitions its nodes never stored")
{
    TypeFixture fx;
    Graph g;
    Variable v; v.name = "list"; v.type = PinType::Struct; v.typeName = kStats; v.isArray = true;
    g.variables.push_back(v);

    Node get; get.type = NodeType::GetVariable; get.s = "list";
    get.propType = PinType::Struct; get.typeName = kStats; get.isArray = true;
    const int gv = g.addNode(std::move(get));
    // Spawned by an older editor build: knows it holds structs, not WHICH.
    Node add; add.type = NodeType::ArrayAdd; add.propType = PinType::Struct;
    const int aa = g.addNode(std::move(add));
    REQUIRE(g.connect(gv, 0, aa, 0));       // list → Array in

    Graph loaded;
    REQUIRE(fromJson(toJson(g), loaded));   // the load path repairs it
    const Node* fixed = nullptr;
    for (const Node& n : loaded.nodes) if (n.type == NodeType::ArrayAdd) fixed = &n;
    REQUIRE(fixed != nullptr);
    CHECK(fixed->typeName == kStats);

    // An existing name is never overwritten, and a node with nothing to learn
    // from stays untouched rather than guessing.
    Graph lone;
    Node solo; solo.type = NodeType::ArrayAdd; solo.propType = PinType::Struct;
    lone.addNode(std::move(solo));
    inferUserTypeNames(lone);
    CHECK(lone.nodes[0].typeName.empty());
}

TEST_CASE("Repairing a definition moves the links with the pins it creates")
{
    TypeFixture fx;
    // The shape an older editor build persisted: user-type nodes that know their
    // KIND but not their definition, so they carry none of the pins the
    // definition implies — and the wires address that bare layout.
    Graph g;
    Variable hit; hit.name = "hit"; hit.type = PinType::String;
    g.variables.push_back(hit);
    Variable out; out.name = "out"; out.type = PinType::Struct; out.typeName = kStats;
    g.variables.push_back(out);

    Node ev; ev.type = NodeType::Event; ev.s = "Go";
    const int e = g.addNode(ev);
    // Via Int to Enum, not Const Enum: syncTypeSignatures clamps a Const Enum
    // onto a live entry, so it could never reach the Default branch.
    Node ci; ci.type = NodeType::ConstInt; ci.f[0] = 99.0f;
    const int cint = g.addNode(std::move(ci));
    Node ie; ie.type = NodeType::IntToEnum; ie.typeName = kWeapon;
    const int cen = g.addNode(std::move(ie));
    REQUIRE(g.connect(cint, 0, cen, 0));
    Node sw; sw.type = NodeType::SwitchOnEnum;            // bare: execIn 0, Default 1, Value 2
    const int swn = g.addNode(std::move(sw));
    Node sv; sv.type = NodeType::SetVariable; sv.s = "hit"; sv.propType = PinType::String;
    const int hitSet = g.addNode(std::move(sv));
    g.findNode(hitSet)->pinDefaults[0] = Value::ofString("default");

    REQUIRE(g.connect(e, 0, swn, 0));      // exec → Switch
    REQUIRE(g.connect(cen, 1, swn, 2));    // enum out (after its Int in) → Value
    REQUIRE(g.connect(swn, 1, hitSet, 0)); // Default → Set hit

    Node mk; mk.type = NodeType::MakeStruct;              // bare: one Struct data-out at 0
    const int mkn = g.addNode(std::move(mk));
    Node so; so.type = NodeType::SetVariable; so.s = "out";
    so.propType = PinType::Struct; so.typeName = kStats;
    const int outSet = g.addNode(std::move(so));
    REQUIRE(g.connect(mkn, 0, outSet, 2));
    REQUIRE(g.connect(hitSet, 1, outSet, 0));             // chain the exec along

    Graph loaded;
    REQUIRE(fromJson(toJson(g), loaded));

    // Both nodes learned their definition from the wire...
    CHECK(loaded.findNode(swn)->typeName == kWeapon);
    CHECK(loaded.findNode(mkn)->typeName == kStats);
    // ...and the links moved with the pins that appeared. Switch keeps Default
    // LAST, so it slid past the three entries: 1 → 4, and Value 2 → 5.
    int defaultPin = -1, valuePin = -1, structOutPin = -1;
    for (const Link& l : loaded.links)
    {
        if (l.srcNode == swn && l.dstNode == hitSet) defaultPin  = l.srcPin;
        if (l.dstNode == swn)                        valuePin    = l.dstPin;
        if (l.srcNode == mkn)                        structOutPin = l.srcPin;
    }
    CHECK(defaultPin == 4);       // 1 execIn + 3 entries → Default
    CHECK(valuePin == 5);
    CHECK(structOutPin == 3);     // 3 fields now precede the Struct out

    // And it still behaves: 99 matches no entry, so Default runs.
    VarStore store;
    Runner r(loaded, store.ctx());
    r.fireEvent("Go", 0, {});
    CHECK(store.vars["hit"].s == "default");
    REQUIRE(store.vars["out"].items.size() == 3);   // a real struct, not an empty Value
    CHECK(store.vars["out"].items[0].f == doctest::Approx(100.0f));
}

TEST_CASE("adoptForEachElementType carries the element's definition, not just its kind")
{
    TypeFixture fx;
    Graph g;
    Variable v; v.name = "list"; v.type = PinType::Struct; v.typeName = kStats; v.isArray = true;
    g.variables.push_back(v);

    Node get; get.type = NodeType::GetVariable; get.s = "list";
    get.propType = PinType::Struct; get.typeName = kStats; get.isArray = true;
    const int gv = g.addNode(std::move(get));
    Node fe; fe.type = NodeType::ForEach;   // born untyped, like the editor spawns it
    const int each = g.addNode(std::move(fe));

    // ForEach unified pins: execIn 0, Body 1, Done 2, Array-in 3, Element-out 4.
    adoptForEachElementType(g, gv, /*srcPin=*/0, each, /*dstPin=*/3);
    CHECK(g.findNode(each)->propType == PinType::Struct);
    CHECK(g.findNode(each)->typeName == kStats);
    CHECK(std::string(signatureOf(*g.findNode(each)).dataOuts[0].typeName) == kStats);
}

// ── Codegen: calls between compiled classes go direct ───────────────────────
// Parity (test_horizoncode_codegen.cpp) proves the fast path is CORRECT — it
// runs a fully compiled world against a fully interpreted one. Nothing there
// proves it is TAKEN, because the fallback would produce the same answers. That
// is what this asserts.

#include <HorizonScene/HcCodegen.h>

TEST_CASE("HcCodegen: a call into another compiled class is emitted direct")
{
    // Callee: a public function and a public variable, plus a private one each.
    Graph callee;
    {
        Variable hp;  hp.name  = "hp";  hp.type = PinType::Float;                 // public
        Variable sec; sec.name = "sec"; sec.type = PinType::Float; sec.access = 1;
        callee.variables.push_back(hp);
        callee.variables.push_back(sec);
        Node fn; fn.type = NodeType::FunctionEntry; fn.s = "Damage"; fn.access = 0;
        fn.params  = { { "amount", PinType::Float, false, {} } };
        fn.results = { { "left",   PinType::Float, false, {} } };
        callee.addNode(std::move(fn));
        Node hidden; hidden.type = NodeType::FunctionEntry; hidden.s = "Secret"; hidden.access = 1;
        callee.addNode(std::move(hidden));
    }

    // Caller: an Object variable that names the callee's class.
    Graph caller;
    {
        Variable obj; obj.name = "obj"; obj.type = PinType::Ref; obj.className = "callee";
        caller.variables.push_back(obj);
        Node ev; ev.type = NodeType::Event; ev.s = "Go";
        const int e = caller.addNode(ev);
        Node get; get.type = NodeType::GetVariable; get.s = "obj"; get.propType = PinType::Ref;
        const int gv = caller.addNode(std::move(get));
        Node call; call.type = NodeType::CallExternal; call.s = "Damage";
        call.params  = { { "amount", PinType::Float, false, {} } };
        call.results = { { "left",   PinType::Float, false, {} } };
        const int cn = caller.addNode(std::move(call));
        REQUIRE(caller.connect(e, 0, cn, 0));      // exec
        REQUIRE(caller.connect(gv, 0, cn, 2));     // obj → Target
        // A private function on the same target must NOT go direct.
        Node hid; hid.type = NodeType::CallExternal; hid.s = "Secret";
        const int hn = caller.addNode(std::move(hid));
        REQUIRE(caller.connect(cn, 1, hn, 0));
        REQUIRE(caller.connect(gv, 0, hn, 2));
    }

    HE::hccg::Options opt;
    HE::hccg::Result r = HE::hccg::generate(
        { { "callee", "callee", callee }, { "caller", "caller", caller } }, opt);
    REQUIRE(r.ok);
    REQUIRE(r.fallbacks.empty());

    std::string callerCpp;
    for (const auto& f : r.files)
        if (f.name == "hcgen_C_caller.cpp") callerCpp = f.contents;
    REQUIRE_FALSE(callerCpp.empty());

    // The public function resolves to a checked downcast and a typed call...
    CHECK(callerCpp.find("hc::as<C_callee>") != std::string::npos);
    CHECK(callerCpp.find("->Damage(") != std::string::npos);
    // ...with the seam kept as the fallback for a null/foreign/interpreted ref.
    CHECK(callerCpp.find("hc::callExternal(m_ctx, t") != std::string::npos);
    // The PRIVATE one stays entirely on the seam: going direct would skip the
    // access check and the warning the interpreter produces.
    CHECK(callerCpp.find("->Secret(") == std::string::npos);
}

TEST_CASE("HcCodegen: a duplicate function name is reported as dead code")
{
    Graph g;
    Node ev; ev.type = NodeType::Event; ev.s = "Go";
    g.addNode(ev);
    for (int i = 0; i < 2; ++i)
    {
        Node fn; fn.type = NodeType::FunctionEntry; fn.s = "Reset";
        g.addNode(std::move(fn));
    }
    HE::hccg::Options opt;
    HE::hccg::Result r = HE::hccg::generate({ { "dup", "dup", g } }, opt);
    REQUIRE(r.ok);
    CHECK(r.fallbacks.empty());          // it still compiles — the first one wins
    bool warned = false;
    for (const auto& w : r.warnings)
        if (w.find("duplicate function 'Reset'") != std::string::npos) warned = true;
    CHECK(warned);
}

// ── Codegen: enums and structs both compile against their definitions ───────

#include <HorizonScene/HcCodegen.h>

TEST_CASE("HcCodegen: enum + struct nodes compile against their definitions")
{
    TypeFixture fx;

    // Enum graph: Event → SwitchOnEnum(ConstEnum Bow) with Bow → SetVariable.
    {
        Graph g;
        Variable hit; hit.name = "hit"; hit.type = PinType::String;
        g.variables.push_back(hit);
        Node ev; ev.type = NodeType::Event; ev.s = "Go";
        const int e = g.addNode(ev);
        const int ce = addTypedNode(g, NodeType::ConstEnum, kWeapon);
        g.findNode(ce)->f[0] = 7.0f;
        const int sw = addTypedNode(g, NodeType::SwitchOnEnum, kWeapon);
        Node cs; cs.type = NodeType::ConstString; cs.s = "bow";
        const int c = g.addNode(cs);
        Node sv; sv.type = NodeType::SetVariable; sv.s = "hit"; sv.propType = PinType::String;
        const int s = g.addNode(sv);
        REQUIRE(g.connect(e, 0, sw, 0));
        REQUIRE(g.connect(ce, 0, sw, 5));
        REQUIRE(g.connect(c, 0, s, 2));
        REQUIRE(g.connect(sw, 2, s, 0));            // Bow branch
        const int e2s = addTypedNode(g, NodeType::EnumToString, kWeapon);
        REQUIRE(g.connect(ce, 0, e2s, 0));

        HE::hccg::Options opt;
        HE::hccg::Result r = HE::hccg::generate({ { "enum_graph", "enum_graph", g } }, opt);
        REQUIRE(r.ok);
        CHECK(r.fallbacks.empty());
        // The enum is a real C++ type in the shared header, and the switch
        // routes on its enumerator — not on a bare 7 nobody can read.
        bool sawEnumType = false, sawCaseBow = false, sawStaff = false;
        for (const auto& f : r.files)
        {
            if (f.name == "hcgen_types.h" &&
                f.contents.find("enum class E_Weapon : int") != std::string::npos)
                sawEnumType = true;
            if (f.contents.find("case E_Weapon::Bow:") != std::string::npos) sawCaseBow = true;
            if (f.contents.find("\"Staff\"") != std::string::npos) sawStaff = true;
        }
        CHECK(sawEnumType);
        CHECK(sawCaseBow);
        (void)sawStaff; // EnumToString is unwired downstream — emission optional
    }

    // Struct graph: MakeStruct compiles into a real C++ aggregate, emitted once
    // for the whole run into the shared hcgen_types.h.
    {
        Graph g;
        Node ev; ev.type = NodeType::Event; ev.s = "Go";
        g.addNode(ev);
        addTypedNode(g, NodeType::MakeStruct, kStats);
        HE::hccg::Options opt;
        HE::hccg::Result r = HE::hccg::generate({ { "struct_graph", "struct_graph", g } }, opt);
        REQUIRE(r.ok);
        CHECK(r.fallbacks.empty());
        bool sawTypes = false, sawStruct = false;
        for (const auto& f : r.files)
        {
            if (f.name == "hcgen_types.h")
            {
                sawTypes = true;
                sawStruct = f.contents.find("struct S_PlayerStats") != std::string::npos;
            }
        }
        CHECK(sawTypes);
        CHECK(sawStruct);
    }

    // A struct ARRAY through a ForEach: the ForEach node has no definition of
    // its own (nothing in the editor ever writes one), so codegen has to take it
    // from the wire — otherwise every struct-array graph would ship interpreted.
    {
        Graph g;
        Variable v; v.name = "list"; v.type = PinType::Struct; v.typeName = kStats; v.isArray = true;
        g.variables.push_back(v);
        Variable one; one.name = "cur"; one.type = PinType::Struct; one.typeName = kStats;
        g.variables.push_back(one);

        Node ev; ev.type = NodeType::Event; ev.s = "Go";
        const int e = g.addNode(ev);
        Node get; get.type = NodeType::GetVariable; get.s = "list";
        get.propType = PinType::Struct; get.typeName = kStats; get.isArray = true;
        const int gv = g.addNode(std::move(get));
        Node fe; fe.type = NodeType::ForEach; fe.propType = PinType::Struct;   // NO typeName
        const int each = g.addNode(std::move(fe));
        Node sv; sv.type = NodeType::SetVariable; sv.s = "cur";
        sv.propType = PinType::Struct; sv.typeName = kStats;
        const int set = g.addNode(std::move(sv));

        REQUIRE(g.connect(e, 0, each, 0));      // exec
        REQUIRE(g.connect(gv, 0, each, 3));     // list → Array
        REQUIRE(g.connect(each, 1, set, 0));    // Body → exec
        REQUIRE(g.connect(each, 4, set, 2));    // Element → Value

        HE::hccg::Options opt;
        HE::hccg::Result r = HE::hccg::generate({ { "foreach_struct", "foreach_struct", g } }, opt);
        REQUIRE(r.ok);
        CHECK(r.fallbacks.empty());
    }

    // OnFailure::Stop: the same graph is an ERROR instead of a fallback, and
    // every offending class is still reported so they can be fixed in one pass.
    {
        Graph a, b;
        Node ev; ev.type = NodeType::Event; ev.s = "Go";
        a.addNode(ev); b.addNode(ev);
        Node m1; m1.type = NodeType::MakeStruct; m1.typeName = "Content/Gone.hasset";
        a.addNode(std::move(m1));
        Node m2; m2.type = NodeType::ConstEnum; m2.typeName = "Content/AlsoGone.hasset";
        b.addNode(std::move(m2));

        HE::hccg::Options interp;             // the default: ship them interpreted
        HE::hccg::Result r1 = HE::hccg::generate({ { "a", "a", a }, { "b", "b", b } }, interp);
        CHECK(r1.ok);
        CHECK(r1.fallbacks.size() == 2);

        HE::hccg::Options stop;
        stop.onFailure = HE::hccg::OnFailure::Stop;
        HE::hccg::Result r2 = HE::hccg::generate({ { "a", "a", a }, { "b", "b", b } }, stop);
        CHECK_FALSE(r2.ok);
        CHECK(r2.fallbacks.size() == 2);      // both, not just the first
    }

    // Struct node whose definition is missing → fallback, not a miscompile
    // (the emitter has no field list to resolve names against).
    {
        Graph g;
        Node ev; ev.type = NodeType::Event; ev.s = "Go";
        g.addNode(ev);
        Node ms; ms.type = NodeType::MakeStruct; ms.typeName = "Content/Missing.hasset";
        g.addNode(std::move(ms));
        HE::hccg::Options opt;
        HE::hccg::Result r = HE::hccg::generate({ { "missing_struct", "missing_struct", g } }, opt);
        REQUIRE(r.ok);
        REQUIRE(r.fallbacks.size() == 1);
        CHECK(r.fallbacks[0].reason.find("not registered") != std::string::npos);
    }

    // Enum node whose definition is missing → fallback, not a miscompile.
    {
        Graph g;
        Node ev; ev.type = NodeType::Event; ev.s = "Go";
        g.addNode(ev);
        Node ce; ce.type = NodeType::ConstEnum; ce.typeName = "Content/Missing.hasset";
        g.addNode(std::move(ce));
        HE::hccg::Options opt;
        HE::hccg::Result r = HE::hccg::generate({ { "missing_enum", "missing_enum", g } }, opt);
        REQUIRE(r.ok);
        REQUIRE(r.fallbacks.size() == 1);
        CHECK(r.fallbacks[0].reason.find("not registered") != std::string::npos);
    }
}
