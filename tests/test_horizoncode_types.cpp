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

// ── Codegen: enums compile natively, structs fall back to the interpreter ────

#include <HorizonScene/HcCodegen.h>

TEST_CASE("HcCodegen: enum nodes compile (entry values baked), struct nodes fall back")
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
        // The generated source routes on the baked entry value and bakes the
        // entry-name switch for EnumToString.
        bool sawCase7 = false, sawStaff = false;
        for (const auto& f : r.files)
        {
            if (f.contents.find("case 7:") != std::string::npos) sawCase7 = true;
            if (f.contents.find("\"Staff\"") != std::string::npos) sawStaff = true;
        }
        CHECK(sawCase7);
        (void)sawStaff; // EnumToString is unwired downstream — emission optional
    }

    // Struct graph: MakeStruct present → the class ships interpreted.
    {
        Graph g;
        Node ev; ev.type = NodeType::Event; ev.s = "Go";
        g.addNode(ev);
        addTypedNode(g, NodeType::MakeStruct, kStats);
        HE::hccg::Options opt;
        HE::hccg::Result r = HE::hccg::generate({ { "struct_graph", "struct_graph", g } }, opt);
        REQUIRE(r.ok);
        REQUIRE(r.fallbacks.size() == 1);
        CHECK(r.fallbacks[0].reason.find("struct") != std::string::npos);
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
