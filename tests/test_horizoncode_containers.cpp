#include "doctest.h"
#include <HorizonCode/HorizonCode.h>
#include <Types/TypeRegistry.h>
#include <string>
#include <unordered_map>
#include <vector>

// Set<T> and Map<K,V> in the HorizonCode interpreter: the node semantics, the
// ORDERING RULES the whole feature rests on (insertion order, in-place update,
// order-preserving removal — docs/horizoncode-containers-plan.md §1.2), the
// wiring rules, and the JSON round trip of declarations and seeded defaults.
//
// Unified pin order per node: [execIns][execOuts][dataIns][dataOuts].

using namespace HorizonCode;

namespace
{
constexpr const char* kWeapon = "Content/Types/CtWeapon.hasset";

struct EnumFixture
{
    EnumFixture()
    {
        HE::EnumDef weapon;
        weapon.name = "CtWeapon"; weapon.assetPath = kWeapon;
        weapon.entries = { { "Sword", 0 }, { "Bow", 7 }, { "Staff", 2 } };
        HE::TypeRegistry::instance().registerEnum(weapon);
    }
    ~EnumFixture() { HE::TypeRegistry::instance().removeType(kWeapon); }
};

struct VarStore
{
    std::unordered_map<std::string, Value> vars;
    Context ctx()
    {
        Context c;
        c.getVariable = [this](const std::string& n)
        { auto it = vars.find(n); return it != vars.end() ? it->second : Value{}; };
        c.setVariable = [this](const std::string& n, const Value& v){ vars[n] = v; };
        return c;
    }
};

// A string constant node.
int constStr(Graph& g, const char* s)
{ Node n; n.type = NodeType::ConstString; n.s = s; return g.addNode(std::move(n)); }
int constInt(Graph& g, int v)
{ Node n; n.type = NodeType::ConstInt; n.f[0] = (float)v; return g.addNode(std::move(n)); }

// A Set node over String elements.
int setNode(Graph& g, NodeType t)
{
    Node n; n.type = t; n.propType = PinType::String;
    n.isArray = true; n.container = ContainerKind::Set;
    return g.addNode(std::move(n));
}
// A Map node with String keys and Int values.
int mapNode(Graph& g, NodeType t)
{
    Node n; n.type = t; n.propType = PinType::Int;
    n.isArray = true; n.container = ContainerKind::Map;
    n.keyType = PinType::String;
    return g.addNode(std::move(n));
}

// A plain Array node: container None, which reads as Array — the shape every
// array node written before Set/Map existed has.
int arrNode(Graph& g, NodeType t, PinType elem)
{ Node n; n.type = t; n.propType = elem; return g.addNode(std::move(n)); }

// Where a built container came out: the node and its data-out pin.
struct Out { int node = 0; int pin = 0; };

// Make Array → Array Append → … over the given strings/ints.
Out buildStrArray(Graph& g, const std::vector<const char*>& items)
{
    Out o{ arrNode(g, NodeType::ArrayMake, PinType::String), 0 };
    for (const char* s : items)
    {
        const int add = arrNode(g, NodeType::ArrayAdd, PinType::String);
        REQUIRE(g.connect(o.node, o.pin, add, 0));
        REQUIRE(g.connect(constStr(g, s), 0, add, 1));
        o = { add, 2 };                 // Array 0, Value 1, Array-out 2
    }
    return o;
}
Out buildIntArray(Graph& g, const std::vector<int>& items)
{
    Out o{ arrNode(g, NodeType::ArrayMake, PinType::Int), 0 };
    for (const int i : items)
    {
        const int add = arrNode(g, NodeType::ArrayAdd, PinType::Int);
        REQUIRE(g.connect(o.node, o.pin, add, 0));
        REQUIRE(g.connect(constInt(g, i), 0, add, 1));
        o = { add, 2 };
    }
    return o;
}
// Make Set → Set Add → … over strings.
Out buildSet(Graph& g, const std::vector<const char*>& items)
{
    Out o{ setNode(g, NodeType::SetMake), 0 };
    for (const char* s : items)
    {
        const int add = setNode(g, NodeType::SetAdd);
        REQUIRE(g.connect(o.node, o.pin, add, 0));
        REQUIRE(g.connect(constStr(g, s), 0, add, 1));
        o = { add, 2 };                 // Set 0, Value 1, Set-out 2
    }
    return o;
}
// Make Map → Map Set → … over String → Int.
struct KV { const char* k; int v; };
Out buildMap(Graph& g, const std::vector<KV>& pairs)
{
    Out o{ mapNode(g, NodeType::MapMake), 0 };
    for (const KV& p : pairs)
    {
        const int st = mapNode(g, NodeType::MapSet);
        REQUIRE(g.connect(o.node, o.pin, st, 0));
        REQUIRE(g.connect(constStr(g, p.k), 0, st, 1));
        REQUIRE(g.connect(constInt(g, p.v), 0, st, 2));
        o = { st, 3 };                  // Map 0, Key 1, Value 2, Map-out 3
    }
    return o;
}

// Store `src`'s data-out 0 into a variable of the given shape, fired by "Go".
void storeInto(Graph& g, int src, int srcPin, const char* varName,
               PinType type, ContainerKind kind, PinType keyType = PinType::String)
{
    Variable v; v.name = varName; v.type = type;
    if (kind != ContainerKind::None) { v.isArray = true; v.container = kind; }
    v.keyType = keyType;
    g.variables.push_back(v);

    Node ev; ev.type = NodeType::Event; ev.s = "Go";
    const int e = g.addNode(std::move(ev));
    Node sv; sv.type = NodeType::SetVariable; sv.s = varName; sv.propType = type;
    if (kind != ContainerKind::None) { sv.isArray = true; sv.container = kind; }
    sv.keyType = keyType;
    const int s = g.addNode(std::move(sv));
    REQUIRE(g.connect(e, 0, s, 0));      // exec
    REQUIRE(g.connect(src, srcPin, s, 2));
}

std::vector<std::string> strings(const std::vector<Value>& vs)
{
    std::vector<std::string> out;
    for (const Value& v : vs) out.push_back(v.s);
    return out;
}
std::vector<int> ints(const std::vector<Value>& vs)
{
    std::vector<int> out;
    for (const Value& v : vs) out.push_back(v.i);
    return out;
}
} // namespace

// ── Set ──────────────────────────────────────────────────────────────────────

TEST_CASE("Set Add keeps the FIRST insertion's position and ignores duplicates")
{
    Graph g;
    // Make → Add "a" → Add "b" → Add "a" (dup) → Add "c"
    const int mk = setNode(g, NodeType::SetMake);
    int prev = mk, prevPin = 0;
    for (const char* s : { "a", "b", "a", "c" })
    {
        const int add = setNode(g, NodeType::SetAdd);
        REQUIRE(g.connect(prev, prevPin, add, 0));
        REQUIRE(g.connect(constStr(g, s), 0, add, 1));
        prev = add; prevPin = 2;   // Set-in 0, Value-in 1, Set-out 2
    }
    storeInto(g, prev, prevPin, "s", PinType::String, ContainerKind::Set);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    const Value& v = store.vars["s"];
    CHECK(v.kind() == ContainerKind::Set);
    CHECK(strings(v.items) == std::vector<std::string>{ "a", "b", "c" });
}

TEST_CASE("Set Remove preserves the order of the rest; Contains and Length agree")
{
    Graph g;
    const int mk = setNode(g, NodeType::SetMake);
    int prev = mk, prevPin = 0;
    for (const char* s : { "a", "b", "c" })
    {
        const int add = setNode(g, NodeType::SetAdd);
        REQUIRE(g.connect(prev, prevPin, add, 0));
        REQUIRE(g.connect(constStr(g, s), 0, add, 1));
        prev = add; prevPin = 2;
    }
    const int rm = setNode(g, NodeType::SetRemove);
    REQUIRE(g.connect(prev, prevPin, rm, 0));
    REQUIRE(g.connect(constStr(g, "b"), 0, rm, 1));

    const int len = setNode(g, NodeType::SetLength);
    REQUIRE(g.connect(rm, 2, len, 0));
    const int hasA = setNode(g, NodeType::SetContains);
    REQUIRE(g.connect(rm, 2, hasA, 0));
    REQUIRE(g.connect(constStr(g, "b"), 0, hasA, 1));
    const int arr = setNode(g, NodeType::SetToArray);
    REQUIRE(g.connect(rm, 2, arr, 0));

    storeInto(g, len, 1, "n", PinType::Int, ContainerKind::None);
    storeInto(g, hasA, 2, "hasB", PinType::Bool, ContainerKind::None);
    storeInto(g, arr, 1, "a", PinType::String, ContainerKind::Array);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    CHECK(store.vars["n"].i == 2);
    CHECK(store.vars["hasB"].b == false);
    CHECK(store.vars["a"].kind() == ContainerKind::Array);
    CHECK(strings(store.vars["a"].items) == std::vector<std::string>{ "a", "c" });
}

TEST_CASE("Set Clear empties the set but keeps its element type")
{
    Graph g;
    const int mk = setNode(g, NodeType::SetMake);
    const int add = setNode(g, NodeType::SetAdd);
    REQUIRE(g.connect(mk, 0, add, 0));
    REQUIRE(g.connect(constStr(g, "a"), 0, add, 1));
    const int cl = setNode(g, NodeType::SetClear);
    REQUIRE(g.connect(add, 2, cl, 0));
    storeInto(g, cl, 1, "s", PinType::String, ContainerKind::Set);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    const Value& v = store.vars["s"];
    CHECK(v.kind() == ContainerKind::Set);
    CHECK(v.type == PinType::String);
    CHECK(v.items.empty());
}

TEST_CASE("For Each Set walks insertion order")
{
    Graph g;
    const int mk = setNode(g, NodeType::SetMake);
    int prev = mk, prevPin = 0;
    for (const char* s : { "z", "a", "z", "m" })
    {
        const int add = setNode(g, NodeType::SetAdd);
        REQUIRE(g.connect(prev, prevPin, add, 0));
        REQUIRE(g.connect(constStr(g, s), 0, add, 1));
        prev = add; prevPin = 2;
    }
    Node loopN; loopN.type = NodeType::ForEachSet; loopN.propType = PinType::String;
    loopN.isArray = true; loopN.container = ContainerKind::Set;
    const int loop = g.addNode(std::move(loopN));
    REQUIRE(g.connect(prev, prevPin, loop, 3));       // Set input

    // Body appends the element to an ordinary array variable, via Array Append.
    Variable acc; acc.name = "seen"; acc.type = PinType::String; acc.isArray = true;
    g.variables.push_back(acc);
    Node getN; getN.type = NodeType::GetVariable; getN.s = "seen";
    getN.propType = PinType::String; getN.isArray = true;
    const int get = g.addNode(std::move(getN));
    Node addN; addN.type = NodeType::ArrayAdd; addN.propType = PinType::String;
    const int aadd = g.addNode(std::move(addN));
    Node setN; setN.type = NodeType::SetVariable; setN.s = "seen";
    setN.propType = PinType::String; setN.isArray = true;
    const int sset = g.addNode(std::move(setN));
    REQUIRE(g.connect(get, 0, aadd, 0));
    REQUIRE(g.connect(loop, 4, aadd, 1));             // Element out → Value in
    REQUIRE(g.connect(aadd, 2, sset, 2));
    REQUIRE(g.connect(loop, 1, sset, 0));             // Body → SetVariable

    Node ev; ev.type = NodeType::Event; ev.s = "Go";
    const int e = g.addNode(std::move(ev));
    REQUIRE(g.connect(e, 0, loop, 0));

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    CHECK(strings(store.vars["seen"].items) == std::vector<std::string>{ "z", "a", "m" });
}

// ── Set: the way back, and the set algebra ───────────────────────────────────

TEST_CASE("Set From Array drops duplicates, keeping the FIRST occurrence's slot")
{
    // The decision this pins: an array MAY repeat and a set may not, so the
    // later "b" disappears and the earlier one keeps its position — the same
    // answer a chain of Set Add nodes gives, which is the point.
    Graph g;
    const Out a = buildStrArray(g, { "b", "a", "b", "c" });
    const int fs = setNode(g, NodeType::SetFromArray);
    REQUIRE(g.connect(a.node, a.pin, fs, 0));
    storeInto(g, fs, 1, "s", PinType::String, ContainerKind::Set);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    const Value& v = store.vars["s"];
    CHECK(v.kind() == ContainerKind::Set);
    CHECK(strings(v.items) == std::vector<std::string>{ "b", "a", "c" });
}

TEST_CASE("Set From Array and Set To Array are each other's inverse on a set")
{
    Graph g;
    const Out s = buildSet(g, { "z", "a", "m" });
    const int ta = setNode(g, NodeType::SetToArray);
    REQUIRE(g.connect(s.node, s.pin, ta, 0));
    const int fs = setNode(g, NodeType::SetFromArray);
    REQUIRE(g.connect(ta, 1, fs, 0));
    storeInto(g, fs, 1, "s", PinType::String, ContainerKind::Set);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    // Round trip, ORDER INCLUDED — anything sorted on the way would show here.
    CHECK(strings(store.vars["s"].items) == std::vector<std::string>{ "z", "a", "m" });
}

TEST_CASE("Set Union is A's order, then the elements only B has")
{
    Graph g;
    const Out a = buildSet(g, { "b", "a" });
    const Out b = buildSet(g, { "c", "a", "d" });
    const int un = setNode(g, NodeType::SetUnion);
    REQUIRE(g.connect(a.node, a.pin, un, 0));
    REQUIRE(g.connect(b.node, b.pin, un, 1));
    storeInto(g, un, 2, "u", PinType::String, ContainerKind::Set);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    // "a" is in both and keeps A's slot; "c"/"d" follow in B's order. A sorted
    // result would read a,b,c,d and a B-first one c,a,d,b.
    CHECK(strings(store.vars["u"].items) == std::vector<std::string>{ "b", "a", "c", "d" });
}

TEST_CASE("Set Intersect keeps only what both have, in A's order")
{
    Graph g;
    const Out a = buildSet(g, { "c", "b", "a" });
    const Out b = buildSet(g, { "a", "b" });
    const int in = setNode(g, NodeType::SetIntersect);
    REQUIRE(g.connect(a.node, a.pin, in, 0));
    REQUIRE(g.connect(b.node, b.pin, in, 1));
    storeInto(g, in, 2, "i", PinType::String, ContainerKind::Set);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    // B lists a before b; the result follows A, which lists b before a.
    CHECK(strings(store.vars["i"].items) == std::vector<std::string>{ "b", "a" });
}

TEST_CASE("Set Difference is A without B — and swapping the sides is a different answer")
{
    Graph g;
    const Out a = buildSet(g, { "c", "b", "a" });
    const Out b = buildSet(g, { "b", "x" });
    const int ab = setNode(g, NodeType::SetDifference);
    REQUIRE(g.connect(a.node, a.pin, ab, 0));
    REQUIRE(g.connect(b.node, b.pin, ab, 1));
    const int ba = setNode(g, NodeType::SetDifference);
    REQUIRE(g.connect(b.node, b.pin, ba, 0));
    REQUIRE(g.connect(a.node, a.pin, ba, 1));
    storeInto(g, ab, 2, "ab", PinType::String, ContainerKind::Set);
    storeInto(g, ba, 2, "ba", PinType::String, ContainerKind::Set);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    CHECK(strings(store.vars["ab"].items) == std::vector<std::string>{ "c", "a" });
    CHECK(strings(store.vars["ba"].items) == std::vector<std::string>{ "x" });
}

TEST_CASE("A set operation on an UNWIRED side is the identity / the empty set")
{
    // An unwired container pin reads as an empty container, so union leaves A
    // alone and intersect empties it — not "whatever a scalar zero would be".
    Graph g;
    const Out a = buildSet(g, { "a", "b" });
    const int un = setNode(g, NodeType::SetUnion);
    REQUIRE(g.connect(a.node, a.pin, un, 0));            // B left unwired
    const int in = setNode(g, NodeType::SetIntersect);
    REQUIRE(g.connect(a.node, a.pin, in, 0));
    storeInto(g, un, 2, "u", PinType::String, ContainerKind::Set);
    storeInto(g, in, 2, "i", PinType::String, ContainerKind::Set);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    CHECK(strings(store.vars["u"].items) == std::vector<std::string>{ "a", "b" });
    CHECK(store.vars["i"].items.empty());
    CHECK(store.vars["i"].kind() == ContainerKind::Set);
}

// ── Map ──────────────────────────────────────────────────────────────────────

TEST_CASE("Map Set inserts in order and UPDATES an existing key in place")
{
    Graph g;
    const int mk = mapNode(g, NodeType::MapMake);
    int prev = mk, prevPin = 0;
    // b=1, a=2, b=3 → keys stay [b, a]; b's value becomes 3.
    const struct { const char* k; int v; } pairs[] = { { "b", 1 }, { "a", 2 }, { "b", 3 } };
    for (const auto& p : pairs)
    {
        const int st = mapNode(g, NodeType::MapSet);
        REQUIRE(g.connect(prev, prevPin, st, 0));
        REQUIRE(g.connect(constStr(g, p.k), 0, st, 1));
        REQUIRE(g.connect(constInt(g, p.v), 0, st, 2));
        prev = st; prevPin = 3;   // Map 0, Key 1, Value 2, Map-out 3
    }
    const int keys = mapNode(g, NodeType::MapKeys);
    REQUIRE(g.connect(prev, prevPin, keys, 0));
    const int vals = mapNode(g, NodeType::MapValues);
    REQUIRE(g.connect(prev, prevPin, vals, 0));

    storeInto(g, keys, 1, "k", PinType::String, ContainerKind::Array);
    storeInto(g, vals, 1, "v", PinType::Int, ContainerKind::Array);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    CHECK(strings(store.vars["k"].items) == std::vector<std::string>{ "b", "a" });
    CHECK(ints(store.vars["v"].items) == std::vector<int>{ 3, 2 });
}

TEST_CASE("Map Get falls back to the Default input on a miss")
{
    Graph g;
    const int mk = mapNode(g, NodeType::MapMake);
    const int st = mapNode(g, NodeType::MapSet);
    REQUIRE(g.connect(mk, 0, st, 0));
    REQUIRE(g.connect(constStr(g, "hp"), 0, st, 1));
    REQUIRE(g.connect(constInt(g, 42), 0, st, 2));

    const int hit = mapNode(g, NodeType::MapGet);
    REQUIRE(g.connect(st, 3, hit, 0));
    REQUIRE(g.connect(constStr(g, "hp"), 0, hit, 1));
    REQUIRE(g.connect(constInt(g, -1), 0, hit, 2));
    const int miss = mapNode(g, NodeType::MapGet);
    REQUIRE(g.connect(st, 3, miss, 0));
    REQUIRE(g.connect(constStr(g, "mp"), 0, miss, 1));
    REQUIRE(g.connect(constInt(g, -1), 0, miss, 2));

    storeInto(g, hit, 3, "hit", PinType::Int, ContainerKind::None);
    storeInto(g, miss, 3, "miss", PinType::Int, ContainerKind::None);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    CHECK(store.vars["hit"].i == 42);
    CHECK(store.vars["miss"].i == -1);
}

TEST_CASE("Map Remove drops the pair and keeps the rest in order")
{
    Graph g;
    const int mk = mapNode(g, NodeType::MapMake);
    int prev = mk, prevPin = 0;
    const struct { const char* k; int v; } pairs[] = { { "a", 1 }, { "b", 2 }, { "c", 3 } };
    for (const auto& p : pairs)
    {
        const int st = mapNode(g, NodeType::MapSet);
        REQUIRE(g.connect(prev, prevPin, st, 0));
        REQUIRE(g.connect(constStr(g, p.k), 0, st, 1));
        REQUIRE(g.connect(constInt(g, p.v), 0, st, 2));
        prev = st; prevPin = 3;
    }
    const int rm = mapNode(g, NodeType::MapRemove);
    REQUIRE(g.connect(prev, prevPin, rm, 0));
    REQUIRE(g.connect(constStr(g, "a"), 0, rm, 1));
    const int keys = mapNode(g, NodeType::MapKeys);
    REQUIRE(g.connect(rm, 2, keys, 0));
    const int vals = mapNode(g, NodeType::MapValues);
    REQUIRE(g.connect(rm, 2, vals, 0));
    const int has = mapNode(g, NodeType::MapContains);
    REQUIRE(g.connect(rm, 2, has, 0));
    REQUIRE(g.connect(constStr(g, "a"), 0, has, 1));

    storeInto(g, keys, 1, "k", PinType::String, ContainerKind::Array);
    storeInto(g, vals, 1, "v", PinType::Int, ContainerKind::Array);
    storeInto(g, has, 2, "hasA", PinType::Bool, ContainerKind::None);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    CHECK(strings(store.vars["k"].items) == std::vector<std::string>{ "b", "c" });
    CHECK(ints(store.vars["v"].items) == std::vector<int>{ 2, 3 });
    CHECK(store.vars["hasA"].b == false);
}

TEST_CASE("Map Length of an UNWIRED map input is 0, not garbage")
{
    Graph g;
    const int len = mapNode(g, NodeType::MapLength);   // Map input left unwired
    storeInto(g, len, 1, "n", PinType::Int, ContainerKind::None);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    CHECK(store.vars["n"].i == 0);
}

TEST_CASE("For Each Map walks pairs in insertion order")
{
    Graph g;
    const int mk = mapNode(g, NodeType::MapMake);
    int prev = mk, prevPin = 0;
    const struct { const char* k; int v; } pairs[] = { { "z", 1 }, { "a", 2 } };
    for (const auto& p : pairs)
    {
        const int st = mapNode(g, NodeType::MapSet);
        REQUIRE(g.connect(prev, prevPin, st, 0));
        REQUIRE(g.connect(constStr(g, p.k), 0, st, 1));
        REQUIRE(g.connect(constInt(g, p.v), 0, st, 2));
        prev = st; prevPin = 3;
    }
    Node loopN; loopN.type = NodeType::ForEachMap; loopN.propType = PinType::Int;
    loopN.isArray = true; loopN.container = ContainerKind::Map;
    loopN.keyType = PinType::String;
    const int loop = g.addNode(std::move(loopN));
    REQUIRE(g.connect(prev, prevPin, loop, 3));

    // Collect the keys the loop visits, in order.
    Variable acc; acc.name = "seen"; acc.type = PinType::String; acc.isArray = true;
    g.variables.push_back(acc);
    Node getN; getN.type = NodeType::GetVariable; getN.s = "seen";
    getN.propType = PinType::String; getN.isArray = true;
    const int get = g.addNode(std::move(getN));
    Node addN; addN.type = NodeType::ArrayAdd; addN.propType = PinType::String;
    const int aadd = g.addNode(std::move(addN));
    Node setN; setN.type = NodeType::SetVariable; setN.s = "seen";
    setN.propType = PinType::String; setN.isArray = true;
    const int sset = g.addNode(std::move(setN));
    REQUIRE(g.connect(get, 0, aadd, 0));
    REQUIRE(g.connect(loop, 4, aadd, 1));             // Key out (data-out 0)
    REQUIRE(g.connect(aadd, 2, sset, 2));
    REQUIRE(g.connect(loop, 1, sset, 0));

    Node ev; ev.type = NodeType::Event; ev.s = "Go";
    const int e = g.addNode(std::move(ev));
    REQUIRE(g.connect(e, 0, loop, 0));

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    CHECK(strings(store.vars["seen"].items) == std::vector<std::string>{ "z", "a" });
}

// ── Map: the way back, and searching by VALUE ────────────────────────────────

TEST_CASE("Make Map From Arrays pairs by index; the SHORTER array wins")
{
    // The decision this pins: a surplus key would need a value nobody supplied,
    // so the pair simply does not appear — Map Keys / Map Values truncate to the
    // pairs that exist for the same reason.
    Graph g;
    const Out ks = buildStrArray(g, { "a", "b", "c" });
    const Out vs = buildIntArray(g, { 1, 2 });
    const int mk = mapNode(g, NodeType::MapFromArrays);
    REQUIRE(g.connect(ks.node, ks.pin, mk, 0));
    REQUIRE(g.connect(vs.node, vs.pin, mk, 1));
    const int br = mapNode(g, NodeType::MapBreak);
    REQUIRE(g.connect(mk, 2, br, 0));

    // The half-wired node an author has on the canvas while still building it:
    // an unwired container pin is an EMPTY container, so "shorter wins" makes it
    // an empty map rather than keys with invented values.
    const int half = mapNode(g, NodeType::MapFromArrays);
    REQUIRE(g.connect(ks.node, ks.pin, half, 0));      // Values left unwired
    const int len = mapNode(g, NodeType::MapLength);
    REQUIRE(g.connect(half, 2, len, 0));

    storeInto(g, br, 1, "k", PinType::String, ContainerKind::Array);
    storeInto(g, br, 2, "v", PinType::Int, ContainerKind::Array);
    storeInto(g, len, 1, "halfLen", PinType::Int, ContainerKind::None);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    CHECK(strings(store.vars["k"].items) == std::vector<std::string>{ "a", "b" });
    CHECK(ints(store.vars["v"].items) == std::vector<int>{ 1, 2 });
    CHECK(store.vars["halfLen"].i == 0);
}

TEST_CASE("Make Map From Arrays: a repeated key keeps its slot and takes the LAST value")
{
    // Map Set's house rule, applied to a list — the two ways of building a map
    // from the same pairs have to agree.
    Graph g;
    const Out ks = buildStrArray(g, { "b", "a", "b" });
    const Out vs = buildIntArray(g, { 1, 2, 3 });
    const int mk = mapNode(g, NodeType::MapFromArrays);
    REQUIRE(g.connect(ks.node, ks.pin, mk, 0));
    REQUIRE(g.connect(vs.node, vs.pin, mk, 1));
    const int br = mapNode(g, NodeType::MapBreak);
    REQUIRE(g.connect(mk, 2, br, 0));

    storeInto(g, br, 1, "k", PinType::String, ContainerKind::Array);
    storeInto(g, br, 2, "v", PinType::Int, ContainerKind::Array);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    CHECK(strings(store.vars["k"].items) == std::vector<std::string>{ "b", "a" });
    CHECK(ints(store.vars["v"].items) == std::vector<int>{ 3, 2 });
}

TEST_CASE("Break Map answers exactly what Map Keys and Map Values do")
{
    Graph g;
    const Out m = buildMap(g, { { "z", 9 }, { "a", 1 } });
    const int br = mapNode(g, NodeType::MapBreak);
    REQUIRE(g.connect(m.node, m.pin, br, 0));
    const int keys = mapNode(g, NodeType::MapKeys);
    REQUIRE(g.connect(m.node, m.pin, keys, 0));
    const int vals = mapNode(g, NodeType::MapValues);
    REQUIRE(g.connect(m.node, m.pin, vals, 0));

    storeInto(g, br, 1, "bk", PinType::String, ContainerKind::Array);
    storeInto(g, br, 2, "bv", PinType::Int, ContainerKind::Array);
    storeInto(g, keys, 1, "k", PinType::String, ContainerKind::Array);
    storeInto(g, vals, 1, "v", PinType::Int, ContainerKind::Array);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    CHECK(strings(store.vars["bk"].items) == strings(store.vars["k"].items));
    CHECK(ints(store.vars["bv"].items) == ints(store.vars["v"].items));
    CHECK(strings(store.vars["bk"].items) == std::vector<std::string>{ "z", "a" });
    CHECK(ints(store.vars["bv"].items) == std::vector<int>{ 9, 1 });
}

TEST_CASE("Map Find By Value answers with the FIRST key, and says when there is none")
{
    Graph g;
    const Out m = buildMap(g, { { "a", 1 }, { "b", 2 }, { "c", 2 } });
    const int hit = mapNode(g, NodeType::MapFindByValue);
    REQUIRE(g.connect(m.node, m.pin, hit, 0));
    REQUIRE(g.connect(constInt(g, 2), 0, hit, 1));
    const int miss = mapNode(g, NodeType::MapFindByValue);
    REQUIRE(g.connect(m.node, m.pin, miss, 0));
    REQUIRE(g.connect(constInt(g, 99), 0, miss, 1));

    storeInto(g, hit, 2, "k", PinType::String, ContainerKind::None);
    storeInto(g, hit, 3, "found", PinType::Bool, ContainerKind::None);
    storeInto(g, miss, 2, "mk", PinType::String, ContainerKind::None);
    storeInto(g, miss, 3, "mfound", PinType::Bool, ContainerKind::None);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    // Two keys hold 2; the FIRST in insertion order wins.
    CHECK(store.vars["k"].s == "b");
    CHECK(store.vars["found"].b == true);
    // A miss: Found says so, and the Key output is the key type's zero. This is
    // why Found is a pin at all — "" is a perfectly good String key.
    CHECK(store.vars["mfound"].b == false);
    CHECK(store.vars["mk"].type == PinType::String);
    CHECK(store.vars["mk"].s.empty());
}

TEST_CASE("Map Remove By Value drops EVERY match and keeps the rest in order")
{
    Graph g;
    const Out m = buildMap(g, { { "a", 1 }, { "b", 2 }, { "c", 2 }, { "d", 3 } });
    const int rm = mapNode(g, NodeType::MapRemoveByValue);
    REQUIRE(g.connect(m.node, m.pin, rm, 0));
    REQUIRE(g.connect(constInt(g, 2), 0, rm, 1));
    const int br = mapNode(g, NodeType::MapBreak);
    REQUIRE(g.connect(rm, 2, br, 0));

    storeInto(g, br, 1, "k", PinType::String, ContainerKind::Array);
    storeInto(g, br, 2, "v", PinType::Int, ContainerKind::Array);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    // Both 2s go — Map Remove would have taken one KEY; this takes every pair.
    CHECK(strings(store.vars["k"].items) == std::vector<std::string>{ "a", "d" });
    CHECK(ints(store.vars["v"].items) == std::vector<int>{ 1, 3 });
}

TEST_CASE("Map Remove By Value with no match returns the map unchanged")
{
    Graph g;
    const Out m = buildMap(g, { { "a", 1 }, { "b", 2 } });
    const int rm = mapNode(g, NodeType::MapRemoveByValue);
    REQUIRE(g.connect(m.node, m.pin, rm, 0));
    REQUIRE(g.connect(constInt(g, 7), 0, rm, 1));
    storeInto(g, rm, 2, "m", PinType::Int, ContainerKind::Map);

    VarStore store;
    Runner r(g, store.ctx());
    r.fireEvent("Go");

    const Value& v = store.vars["m"];
    CHECK(v.kind() == ContainerKind::Map);
    CHECK(strings(v.keys) == std::vector<std::string>{ "a", "b" });
    CHECK(ints(v.items) == std::vector<int>{ 1, 2 });
}

// ── Wiring rules ─────────────────────────────────────────────────────────────

TEST_CASE("Containers of different kinds never connect")
{
    Graph g;
    const int sMake = setNode(g, NodeType::SetMake);          // Set<String> out
    Node arrLen; arrLen.type = NodeType::ArrayLength; arrLen.propType = PinType::String;
    const int aLen = g.addNode(std::move(arrLen));            // Array<String> in
    CHECK_FALSE(g.connect(sMake, 0, aLen, 0));

    const int sLen = setNode(g, NodeType::SetLength);
    CHECK(g.connect(sMake, 0, sLen, 0));                      // Set → Set is fine
}

TEST_CASE("Maps must agree on the key type")
{
    Graph g;
    const int mk = mapNode(g, NodeType::MapMake);             // Map<String,Int>
    Node other; other.type = NodeType::MapLength; other.propType = PinType::Int;
    other.isArray = true; other.container = ContainerKind::Map;
    other.keyType = PinType::Int;                             // Map<Int,Int>
    const int len = g.addNode(std::move(other));
    CHECK_FALSE(g.connect(mk, 0, len, 0));
}

TEST_CASE("A generic For Each Set / For Each Map adopts the container it is wired to")
{
    EnumFixture fx;
    // A loop node dropped from the menu is generic: Float elements, String keys.
    // Wiring a container in has to retype BOTH halves, or every wire off the
    // Key output is silently mistyped.
    Graph g;
    Node loopN; loopN.type = NodeType::ForEachMap;
    loopN.isArray = true; loopN.container = ContainerKind::Map;
    const int loop = g.addNode(std::move(loopN));

    Node src; src.type = NodeType::MapMake; src.propType = PinType::Int;
    src.isArray = true; src.container = ContainerKind::Map;
    src.keyType = PinType::Enum; src.keyTypeName = kWeapon;
    const int m = g.addNode(std::move(src));

    adoptForEachElementType(g, m, 0, loop, 3);
    const Node* l = g.findNode(loop);
    REQUIRE(l != nullptr);
    CHECK(l->keyType == PinType::Enum);
    CHECK(l->keyTypeName == kWeapon);
    CHECK(l->propType == PinType::Int);
    CHECK(g.connect(m, 0, loop, 3));   // …and the wire now typechecks

    // A SET does not teach a For Each Map anything — the kinds differ.
    Graph g2;
    Node ls; ls.type = NodeType::ForEachMap;
    ls.isArray = true; ls.container = ContainerKind::Map;
    const int loop2 = g2.addNode(std::move(ls));
    const int s = setNode(g2, NodeType::SetMake);
    adoptForEachElementType(g2, s, 0, loop2, 3);
    CHECK(g2.findNode(loop2)->propType == PinType::Float);   // untouched
    CHECK_FALSE(g2.connect(s, 0, loop2, 3));
}

TEST_CASE("Only Int, String, Enum and Ref may key a map")
{
    CHECK(isValidMapKeyType(PinType::Int));
    CHECK(isValidMapKeyType(PinType::String));
    CHECK(isValidMapKeyType(PinType::Enum));
    CHECK(isValidMapKeyType(PinType::Ref));
    CHECK_FALSE(isValidMapKeyType(PinType::Float));
    CHECK_FALSE(isValidMapKeyType(PinType::Bool));
    CHECK_FALSE(isValidMapKeyType(PinType::Vec2));
    CHECK_FALSE(isValidMapKeyType(PinType::Vec3));
    CHECK_FALSE(isValidMapKeyType(PinType::Color));
    CHECK_FALSE(isValidMapKeyType(PinType::Transform));
    CHECK_FALSE(isValidMapKeyType(PinType::Struct));
}

// ── Persistence ──────────────────────────────────────────────────────────────

TEST_CASE("A seeded Map variable round-trips through JSON with its order intact")
{
    Graph g;
    Variable v;
    v.name = "scores"; v.type = PinType::Int;
    v.isArray = true; v.container = ContainerKind::Map; v.keyType = PinType::String;
    v.defaultKeys  = { Value::ofString("zeta"), Value::ofString("alpha") };
    v.defaultItems = { Value::ofInt(9), Value::ofInt(1) };
    g.variables.push_back(v);

    const std::string json = toJson(g);
    Graph loaded;
    REQUIRE(fromJson(json, loaded));
    const Variable* lv = loaded.findVariable("scores");
    REQUIRE(lv != nullptr);
    CHECK(lv->kind() == ContainerKind::Map);
    CHECK(lv->isArray);                       // normalized on load, never half-set
    CHECK(lv->keyType == PinType::String);
    CHECK(strings(lv->defaultKeys) == std::vector<std::string>{ "zeta", "alpha" });
    CHECK(ints(lv->defaultItems) == std::vector<int>{ 9, 1 });

    // ... and seeding an instance from it keeps that order (NOT alphabetical:
    // a JSON object would have sorted "alpha" first).
    const Value seeded = variableDefaultValue(*lv);
    CHECK(seeded.kind() == ContainerKind::Map);
    CHECK(strings(seeded.keys) == std::vector<std::string>{ "zeta", "alpha" });
    CHECK(ints(seeded.items) == std::vector<int>{ 9, 1 });
}

TEST_CASE("A seeded Set variable drops duplicates, keeping the first occurrence")
{
    Graph g;
    Variable v;
    v.name = "tags"; v.type = PinType::String;
    v.isArray = true; v.container = ContainerKind::Set;
    v.defaultItems = { Value::ofString("b"), Value::ofString("a"), Value::ofString("b") };
    g.variables.push_back(v);

    const Value seeded = variableDefaultValue(v);
    CHECK(seeded.kind() == ContainerKind::Set);
    CHECK(strings(seeded.items) == std::vector<std::string>{ "b", "a" });
}

TEST_CASE("A Set/Map node keeps its kind and key type across a graph round trip")
{
    Graph g;
    const int s = setNode(g, NodeType::SetAdd);
    Node m; m.type = NodeType::MapGet; m.propType = PinType::Int;
    m.isArray = true; m.container = ContainerKind::Map;
    m.keyType = PinType::Enum; m.keyTypeName = kWeapon;
    const int mi = g.addNode(std::move(m));

    Graph loaded;
    REQUIRE(fromJson(toJson(g), loaded));
    const Node* ls = loaded.findNode(s);
    const Node* lm = loaded.findNode(mi);
    REQUIRE(ls); REQUIRE(lm);
    CHECK(ls->kind() == ContainerKind::Set);
    CHECK(lm->kind() == ContainerKind::Map);
    CHECK(lm->keyType == PinType::Enum);
    CHECK(lm->keyTypeName == kWeapon);
}

TEST_CASE("An array declaration written before containers existed still loads as Array")
{
    // The legacy on-disk row: "arr": true and no "ctr" key at all.
    const std::string json =
        R"({"nodes":[],"links":[],"variables":[{"name":"xs","type":4,"arr":true}],"nextId":1})";
    Graph loaded;
    REQUIRE(fromJson(json, loaded));
    const Variable* v = loaded.findVariable("xs");
    REQUIRE(v != nullptr);
    CHECK(v->isArray);
    CHECK(v->container == ContainerKind::None);
    CHECK(v->kind() == ContainerKind::Array);
}

// ── Struct fields ────────────────────────────────────────────────────────────

TEST_CASE("A struct field may be a Set or a Map, and survives the definition JSON")
{
    EnumFixture fx;
    HE::StructDef def;
    def.name = "CtLoadout"; def.assetPath = "Content/Types/CtLoadout.hasset";
    {
        HE::StructField tags;
        tags.name = "tags"; tags.type = PinType::String;
        tags.isArray = true; tags.container = ContainerKind::Set;
        tags.defaultValue.isArray = true; tags.defaultValue.container = ContainerKind::Set;
        tags.defaultValue.items = { Value::ofString("fire"), Value::ofString("ice") };

        HE::StructField ammo;
        ammo.name = "ammo"; ammo.type = PinType::Int;
        ammo.isArray = true; ammo.container = ContainerKind::Map;
        ammo.keyType = PinType::Enum; ammo.keyTypeName = kWeapon;
        ammo.defaultValue.isArray = true; ammo.defaultValue.container = ContainerKind::Map;
        Value bow;   bow.type = PinType::Enum;   bow.s = "Bow";
        Value sword; sword.type = PinType::Enum; sword.s = "Sword";
        ammo.defaultValue.keys  = { bow, sword };
        ammo.defaultValue.items = { Value::ofInt(30), Value::ofInt(1) };

        def.fields = { tags, ammo };
    }

    HE::StructDef back;
    REQUIRE(HE::TypeRegistry::structFromJson(HE::TypeRegistry::structToJson(def), back));
    REQUIRE(back.fields.size() == 2);
    CHECK(back.fields[0].kind() == ContainerKind::Set);
    CHECK(back.fields[1].kind() == ContainerKind::Map);
    CHECK(back.fields[1].keyType == PinType::Enum);
    CHECK(back.fields[1].keyTypeName == kWeapon);

    back.assetPath = def.assetPath; back.name = def.name;
    HE::TypeRegistry::instance().registerStruct(back);
    const Value seeded = HE::TypeRegistry::instance().makeDefaultValue(def.assetPath);
    REQUIRE(seeded.items.size() == 2);
    CHECK(strings(seeded.items[0].items) == std::vector<std::string>{ "fire", "ice" });
    // Enum keys persist as entry NAMES and resolve to their current values,
    // in the order they were authored — 7 ("Bow") before 0 ("Sword").
    CHECK(ints(seeded.items[1].keys)  == std::vector<int>{ 7, 0 });
    CHECK(ints(seeded.items[1].items) == std::vector<int>{ 30, 1 });
    HE::TypeRegistry::instance().removeType(def.assetPath);
}

TEST_CASE("A map key whose enum entry is gone drops the pair instead of merging it")
{
    EnumFixture fx;
    HE::StructDef def;
    def.name = "CtBag"; def.assetPath = "Content/Types/CtBag.hasset";
    HE::StructField ammo;
    ammo.name = "ammo"; ammo.type = PinType::Int;
    ammo.isArray = true; ammo.container = ContainerKind::Map;
    ammo.keyType = PinType::Enum; ammo.keyTypeName = kWeapon;
    ammo.defaultValue.isArray = true; ammo.defaultValue.container = ContainerKind::Map;
    Value bow;  bow.type = PinType::Enum;  bow.s = "Bow";
    Value gone; gone.type = PinType::Enum; gone.s = "Crossbow";   // no such entry
    ammo.defaultValue.keys  = { bow, gone };
    ammo.defaultValue.items = { Value::ofInt(30), Value::ofInt(5) };
    def.fields = { ammo };
    HE::TypeRegistry::instance().registerStruct(def);

    const Value seeded = HE::TypeRegistry::instance().makeDefaultValue(def.assetPath);
    REQUIRE(seeded.items.size() == 1);
    // Falling back to the first entry would have made "Crossbow" a second
    // "Sword" — two keys silently becoming one. The pair is dropped instead.
    CHECK(ints(seeded.items[0].keys)  == std::vector<int>{ 7 });
    CHECK(ints(seeded.items[0].items) == std::vector<int>{ 30 });
    HE::TypeRegistry::instance().removeType(def.assetPath);
}

// ── Container wires carry their element type, they do not convert it ─────────
// A wire between two containers passes the values through untouched: coerce
// leaves a container alone, by design. Accepting Array<Float> into an Array<Int>
// pin would therefore hand the reader elements still tagged Float while every
// comparison it makes runs on Int — and scalarValueEquals(Int) reads Value::i,
// which a Float value leaves at 0. Every element would look equal to every
// other, so a five-element array would dedupe to one. That is a silent wrong
// answer; a refused wire is a question the author can still answer.
TEST_CASE("Containers only join when the ELEMENT types match, not merely convert")
{
    Graph g;
    auto setMake = [&](PinType elem) {
        Node n; n.type = NodeType::SetMake; n.propType = elem;
        n.isArray = true; n.container = ContainerKind::Set;
        return g.addNode(std::move(n));
    };
    auto setAdd = [&](PinType elem) {
        Node n; n.type = NodeType::SetAdd; n.propType = elem;
        n.isArray = true; n.container = ContainerKind::Set;
        return g.addNode(std::move(n));
    };

    // Float and Int convert freely as SCALARS. That is what makes this worth
    // pinning: the permissive rule is right one level down and wrong here.
    CHECK(canConvertPinType(PinType::Float, PinType::Int));

    const int intAdd = setAdd(PinType::Int);
    // Set<Float> into a Set<Int> pin: refused. Accepting it would hand the
    // reader values still tagged Float while every comparison it makes runs on
    // Int, and scalarValueEquals(Int) reads Value::i, which a Float value
    // leaves at 0 — every element would look equal to every other.
    CHECK_FALSE(g.connect(setMake(PinType::Float), 0, intAdd, 0));
    // The same wire with matching elements is fine, which is what shows the
    // refusal is about the TYPE and not about container wires in general.
    CHECK(g.connect(setMake(PinType::Int), 0, intAdd, 0));

    const int floatAdd = setAdd(PinType::Float);
    CHECK(g.connect(setMake(PinType::Float), 0, floatAdd, 0));
}
