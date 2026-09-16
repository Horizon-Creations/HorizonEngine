#include "doctest.h"
#include <HorizonCode/HorizonCode.h>
#include <string>
#include <unordered_map>
#include <vector>

// Reroute nodes (a knot in a wire) and comment boxes (editor chrome) in the
// HorizonCode graph: the pin shape a reroute takes, how it ADOPTS a type from
// whatever it is wired to (in either direction, and down a chain), that the
// interpreter reads straight through it, that a chain resolves to its real
// source, and that both survive the JSON round trip every asset makes.
//
// Unified pin order per node: [execIns][execOuts][dataIns][dataOuts]. A
// reroute has one pin per side, so its input is 0 and its output 1 on both
// shapes.

using namespace HorizonCode;

namespace
{
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

int reroute(Graph& g, bool exec = false)
{ Node n; n.type = NodeType::Reroute; n.hasArg = exec; return g.addNode(std::move(n)); }
int constInt(Graph& g, int v)
{ Node n; n.type = NodeType::ConstInt; n.f[0] = (float)v; return g.addNode(std::move(n)); }
int constStr(Graph& g, const char* s)
{ Node n; n.type = NodeType::ConstString; n.s = s; return g.addNode(std::move(n)); }
int setVar(Graph& g, const char* name, PinType t, bool isArray = false)
{ Node n; n.type = NodeType::SetVariable; n.s = name; n.propType = t; n.isArray = isArray;
  return g.addNode(std::move(n)); }
int event(Graph& g, const char* name)
{ Node n; n.type = NodeType::Event; n.s = name; return g.addNode(std::move(n)); }
int arrMake(Graph& g, PinType elem)
{ Node n; n.type = NodeType::ArrayMake; n.propType = elem; return g.addNode(std::move(n)); }

// Unified pin ranges of a node: [execIns][execOuts][dataIns][dataOuts].
struct Ranges { int execIn0, execOut0, dataIn0, dataOut0; };
Ranges pinRanges(const Node& n)
{
    const NodeSigCounts c = signatureCountsOf(n);
    return { 0, c.execIns, c.execIns + c.execOuts, c.execIns + c.execOuts + c.dataIns };
}
// Data-out 0 / data-in 0 of a node, in unified pin numbers.
int out0(const Graph& g, int id) { return pinRanges(*g.findNode(id)).dataOut0; }
int in0(const Graph& g, int id)  { return pinRanges(*g.findNode(id)).dataIn0; }
} // namespace

TEST_CASE("reroute: one pin per side, in the shape hasArg says")
{
    Node data; data.type = NodeType::Reroute; data.propType = PinType::String;
    const NodeSig ds = signatureOf(data);
    CHECK(ds.execIns.empty());
    CHECK(ds.execOuts.empty());
    REQUIRE(ds.dataIns.size() == 1);
    REQUIRE(ds.dataOuts.size() == 1);
    CHECK(ds.dataIns[0].type == PinType::String);
    CHECK(ds.dataOuts[0].type == PinType::String);
    CHECK(std::string(ds.dataIns[0].name).empty());   // a dot has no label

    Node exec; exec.type = NodeType::Reroute; exec.hasArg = true;
    const NodeSig es = signatureOf(exec);
    CHECK(es.execIns.size() == 1);
    CHECK(es.execOuts.size() == 1);
    CHECK(es.dataIns.empty());
    CHECK(es.dataOuts.empty());

    // Both shapes: input at 0, output at 1 — what the editor splices by.
    CHECK(pinRanges(data).dataIn0 == 0);
    CHECK(pinRanges(data).dataOut0 == 1);
    CHECK(pinRanges(exec).execIn0 == 0);
    CHECK(pinRanges(exec).execOut0 == 1);

    CHECK(std::string(nodeCategory(NodeType::Reroute)) == "Flow");
    CHECK(std::string(nodeDisplayName(NodeType::Reroute)) == "Reroute");
}

TEST_CASE("reroute: adopts the type of what is wired INTO it, and drops wires that stop fitting")
{
    Graph g;
    const int r  = reroute(g);                 // born Float
    const int s  = constStr(g, "hi");
    const int sv = setVar(g, "name", PinType::String);
    const int iv = setVar(g, "count", PinType::Int);

    // Backwards first: an unfed reroute wired into a String input becomes one.
    adoptRerouteType(g, r, 1, sv, in0(g, sv));
    REQUIRE(g.connect(r, 1, sv, in0(g, sv)));
    CHECK(g.findNode(r)->propType == PinType::String);

    // Then a String source into it: still String, the wire below still holds.
    adoptRerouteType(g, s, out0(g, s), r, 0);
    REQUIRE(g.connect(s, out0(g, s), r, 0));
    CHECK(g.findNode(r)->propType == PinType::String);
    CHECK(g.links.size() == 2);

    // Re-feed it from an Int: the knot retypes, and the String reader's wire
    // is dropped (Int → String is not a wire convert allows), while an Int
    // reader wired afterwards is accepted.
    const int i = constInt(g, 3);
    adoptRerouteType(g, i, out0(g, i), r, 0);
    REQUIRE(g.connect(i, out0(g, i), r, 0));
    CHECK(g.findNode(r)->propType == PinType::Int);
    bool stringWireLeft = false;
    for (const Link& l : g.links) if (l.dstNode == sv) stringWireLeft = true;
    CHECK_FALSE(stringWireLeft);
    REQUIRE(g.connect(r, 1, iv, in0(g, iv)));

    // Once fed, the FEED is the authority: wiring the output into a String
    // input no longer retypes it (and the typed connect refuses).
    adoptRerouteType(g, r, 1, sv, in0(g, sv));
    CHECK(g.findNode(r)->propType == PinType::Int);
    CHECK_FALSE(g.connect(r, 1, sv, in0(g, sv)));
}

TEST_CASE("reroute: a container shape rides along, and a chain follows its head")
{
    Graph g;
    const int arr = arrMake(g, PinType::String);
    const int r1 = reroute(g);
    const int r2 = reroute(g);
    const int r3 = reroute(g);
    // Chain first (all still Float scalars), then feed the head.
    REQUIRE(g.connect(r1, 1, r2, 0));
    REQUIRE(g.connect(r2, 1, r3, 0));
    adoptRerouteType(g, arr, out0(g, arr), r1, 0);
    REQUIRE(g.connect(arr, out0(g, arr), r1, 0));
    for (int id : { r1, r2, r3 })
    {
        INFO("reroute ", id);
        CHECK(g.findNode(id)->propType == PinType::String);
        CHECK(g.findNode(id)->isArray);
        CHECK(g.findNode(id)->kind() == ContainerKind::Array);
    }
    // The chain's tail feeds an Array<String> reader and nothing else.
    const int sv = setVar(g, "names", PinType::String, /*isArray=*/true);
    const int sc = setVar(g, "one", PinType::String);
    CHECK(g.connect(r3, 1, sv, in0(g, sv)));
    CHECK_FALSE(g.connect(r3, 1, sc, in0(g, sc)));

    // rerouteOrigin sees through the whole chain to the Make Array.
    int originPin = -1;
    const Node* o = rerouteOrigin(g, *g.findNode(r3), 1, originPin);
    REQUIRE(o != nullptr);
    CHECK(o->id == arr);
    CHECK(originPin == out0(g, arr));
    // …and an unfed knot is its own origin.
    const int lone = reroute(g);
    o = rerouteOrigin(g, *g.findNode(lone), 1, originPin);
    REQUIRE(o != nullptr);
    CHECK(o->id == lone);
}

TEST_CASE("reroute: the interpreter reads straight through data and exec knots")
{
    Graph g;
    const int ev = event(g, "Go");
    const int k  = reroute(g, /*exec=*/true);
    const int sv = setVar(g, "n", PinType::Int);
    const int r1 = reroute(g);
    const int r2 = reroute(g);
    const int c  = constInt(g, 41);
    adoptRerouteType(g, c, out0(g, c), r1, 0);
    REQUIRE(g.connect(c, out0(g, c), r1, 0));
    REQUIRE(g.connect(r1, 1, r2, 0));
    REQUIRE(g.connect(r2, 1, sv, in0(g, sv)));
    REQUIRE(g.connect(ev, pinRanges(*g.findNode(ev)).execOut0, k, 0));
    REQUIRE(g.connect(k, 1, sv, pinRanges(*g.findNode(sv)).execIn0));

    VarStore store;
    Runner run(g, store.ctx());
    run.fireEvent("Go");
    REQUIRE(store.vars.count("n") == 1);
    CHECK(store.vars["n"].type == PinType::Int);
    CHECK(store.vars["n"].i == 41);
}

TEST_CASE("reroute + comments: both survive the JSON round trip, and a stale knot retypes on load")
{
    Graph g;
    const int ev = event(g, "Go");
    const int k  = reroute(g, /*exec=*/true);
    const int r  = reroute(g);
    const int c  = constStr(g, "x");
    adoptRerouteType(g, c, out0(g, c), r, 0);
    REQUIRE(g.connect(c, out0(g, c), r, 0));
    REQUIRE(g.connect(ev, pinRanges(*g.findNode(ev)).execOut0, k, 0));

    GraphComment cb;
    cb.id = g.nextId++; cb.text = "Setup"; cb.x = 10; cb.y = 20; cb.w = 300; cb.h = 150;
    cb.subgraph = 0;
    g.comments.push_back(cb);
    GraphComment cb2;
    cb2.id = g.nextId++; cb2.text = ""; cb2.x = -5; cb2.y = 7; cb2.subgraph = 3;
    g.comments.push_back(cb2);

    Graph back;
    REQUIRE(fromJson(toJson(g), back));
    const Node* kb = back.findNode(k);
    const Node* rb = back.findNode(r);
    REQUIRE(kb != nullptr);
    REQUIRE(rb != nullptr);
    CHECK(kb->type == NodeType::Reroute);
    CHECK(kb->hasArg);                          // still an exec knot
    CHECK(rb->type == NodeType::Reroute);
    CHECK_FALSE(rb->hasArg);
    CHECK(rb->propType == PinType::String);
    CHECK(back.links.size() == 2);

    REQUIRE(back.comments.size() == 2);
    CHECK(back.comments[0].id == cb.id);
    CHECK(back.comments[0].text == "Setup");
    CHECK(back.comments[0].x == 10.0f);
    CHECK(back.comments[0].y == 20.0f);
    CHECK(back.comments[0].w == 300.0f);
    CHECK(back.comments[0].h == 150.0f);
    CHECK(back.comments[0].subgraph == 0);
    CHECK(back.comments[1].id == cb2.id);
    CHECK(back.comments[1].text.empty());
    CHECK(back.comments[1].subgraph == 3);
    // A comment id is a graph id: the counter must be past it.
    CHECK(back.nextId > cb2.id);

    // A graph with no comments writes none — older readers see the same bytes.
    Graph plain;
    event(plain, "Go");
    CHECK(toJson(plain).find("comments") == std::string::npos);

    // A knot saved as Float but fed by a String on disk (the source retyped
    // after the save, say) comes back as a String: fromJson re-propagates.
    Graph stale;
    const int sc = constStr(stale, "y");
    const int sr = reroute(stale);                 // Float, never adopted
    stale.links.push_back({ sc, out0(stale, sc), sr, 0 });
    Graph fixed;
    REQUIRE(fromJson(toJson(stale), fixed));
    REQUIRE(fixed.findNode(sr) != nullptr);
    CHECK(fixed.findNode(sr)->propType == PinType::String);
}
