#include "doctest.h"

#include "CollabDocSync.h"

#include <HorizonCode/HorizonCode.h>
#include <MaterialGraph/MaterialGraph.h>
#include <ParticleGraph/ParticleGraph.h>
#include <AnimatorStateMachine/AnimatorStateMachineGraph.h>
#include <UIWidget/UIWidgetTree.h>

#include <algorithm>
#include <string>
#include <vector>

// ─── Item-level document sync ────────────────────────────────────────────────
// The layer that makes a collaborating graph/UI editor live instead of
// periodically reloaded. Two properties matter and are tested here for every
// document type:
//
//   1. A diff after an edit says exactly what changed — no more (a whole-graph
//      resend defeats the point) and no less (a missed item silently forks the
//      two peers).
//   2. Applying that diff on the other side reproduces the document. This is the
//      one that actually protects the user's work, so every case asserts on the
//      SERIALIZED form rather than on a field or two.

namespace DS = CollabDocSync;
namespace HC = HorizonCode;
using Delta  = HE::Net::CollabSession::DocDelta;

namespace {

// Diff `doc` against `mirror` and return what changed.
std::vector<Delta> diff(DS::IDocAdapter& doc, DS::DocMirror& mirror)
{
    std::vector<Delta> out;
    DS::diffInto(doc, mirror, DS::Scope::Primary, out);
    return out;
}

int countOf(const std::vector<Delta>& ds, DS::Kind kind, std::uint8_t op)
{
    return static_cast<int>(std::count_if(ds.begin(), ds.end(), [&](const Delta& d) {
        return d.kind == static_cast<std::uint8_t>(kind) && d.op == op;
    }));
}
constexpr std::uint8_t kUpsert = 0;
constexpr std::uint8_t kRemove = 1;

} // namespace

TEST_CASE("DocSync: the first diff of a document is a baseline, not a full resend")
{
    HC::Graph g;
    HC::Node n; n.type = HC::NodeType::Add; n.id = g.addNode(n);
    (void)n;

    auto doc = DS::forHorizonCodeGraph(g);
    DS::DocMirror mirror;
    // Both peers already hold this document when the tab opens (the file got
    // there by the whole-file path). Announcing every node as newly created
    // would be a pointless resend at best and a duplicate at worst.
    CHECK(diff(*doc, mirror).empty());
    CHECK(mirror.seeded);
}

TEST_CASE("DocSync: HorizonCode nodes, links and variables round-trip through deltas")
{
    HC::Graph a;
    auto docA = DS::forHorizonCodeGraph(a);
    DS::DocMirror mirrorA;
    diff(*docA, mirrorA);   // seed

    HC::Node ev;  ev.type = HC::NodeType::Event; ev.s = "OnClick";
    const int evId = a.addNode(ev);
    HC::Node pr;  pr.type = HC::NodeType::Print; pr.x = 120.0f; pr.y = 40.0f;
    const int prId = a.addNode(pr);
    a.links.push_back({ evId, 0, prId, 0 });
    HC::Variable v; v.name = "score"; v.type = HC::PinType::Int;
    a.variables.push_back(v);

    const std::vector<Delta> ds = diff(*docA, mirrorA);
    CHECK(countOf(ds, DS::Kind::Node, kUpsert)     == 2);
    CHECK(countOf(ds, DS::Kind::Link, kUpsert)     == 1);
    CHECK(countOf(ds, DS::Kind::Variable, kUpsert) == 1);

    // The peer starts from the same (empty) document and must end up identical.
    HC::Graph b;
    auto docB = DS::forHorizonCodeGraph(b);
    DS::DocMirror mirrorB;
    DS::seed(*docB, mirrorB, DS::Scope::Primary);
    CHECK(DS::applyDeltas(*docB, mirrorB, DS::Scope::Primary, ds));
    CHECK(HC::toJson(b) == HC::toJson(a));

    // ...and having applied them, the peer must not diff them straight back out.
    CHECK(diff(*docB, mirrorB).empty());
}

TEST_CASE("DocSync: moving one node sends one delta, not the graph")
{
    HC::Graph a;
    HC::Node n1; n1.type = HC::NodeType::Add;     const int id1 = a.addNode(n1);
    HC::Node n2; n2.type = HC::NodeType::Multiply; (void)a.addNode(n2);
    HC::Node n3; n3.type = HC::NodeType::Print;    (void)a.addNode(n3);

    auto doc = DS::forHorizonCodeGraph(a);
    DS::DocMirror mirror;
    diff(*doc, mirror);

    a.findNode(id1)->x = 400.0f;   // one drag

    const std::vector<Delta> ds = diff(*doc, mirror);
    REQUIRE(ds.size() == 1);
    CHECK(ds[0].itemId == id1);
    CHECK(ds[0].op == kUpsert);
}

TEST_CASE("DocSync: deleting a node removes it and its links on the peer")
{
    HC::Graph a;
    HC::Node ev; ev.type = HC::NodeType::Event;  const int evId = a.addNode(ev);
    HC::Node pr; pr.type = HC::NodeType::Print;  const int prId = a.addNode(pr);
    a.links.push_back({ evId, 0, prId, 0 });

    HC::Graph b = a;   // both sides start in step
    auto docA = DS::forHorizonCodeGraph(a);
    auto docB = DS::forHorizonCodeGraph(b);
    DS::DocMirror mirrorA, mirrorB;
    diff(*docA, mirrorA);
    DS::seed(*docB, mirrorB, DS::Scope::Primary);

    a.removeNode(prId);   // takes the link with it

    const std::vector<Delta> ds = diff(*docA, mirrorA);
    CHECK(countOf(ds, DS::Kind::Node, kRemove) == 1);
    CHECK(countOf(ds, DS::Kind::Link, kRemove) == 1);

    CHECK(DS::applyDeltas(*docB, mirrorB, DS::Scope::Primary, ds));
    CHECK(b.links.empty());              // no link left pointing at a dead node
    CHECK(b.nodes.size() == 1);
    CHECK(HC::toJson(b) == HC::toJson(a));
}

TEST_CASE("DocSync: a link key is stable when unrelated nodes come and go")
{
    // The key is built from node IDS, not vector positions, so deleting the node
    // in front of a link must not make the link look like a different one — that
    // would send a spurious remove+add pair and, worse, briefly break the link on
    // the peer.
    const std::int64_t before = DS::linkKey(7, 0, 9, 1);

    HC::Graph a;
    HC::Node n0; n0.type = HC::NodeType::Add;   const int spare = a.addNode(n0);
    HC::Node n1; n1.type = HC::NodeType::Event; n1.id = 7;  a.nodes.push_back(n1);
    HC::Node n2; n2.type = HC::NodeType::Print; n2.id = 9;  a.nodes.push_back(n2);
    a.links.push_back({ 7, 0, 9, 1 });

    auto doc = DS::forHorizonCodeGraph(a);
    DS::DocMirror mirror;
    diff(*doc, mirror);

    a.removeNode(spare);

    const std::vector<Delta> ds = diff(*doc, mirror);
    CHECK(countOf(ds, DS::Kind::Link, kRemove) == 0);   // the link is untouched
    CHECK(countOf(ds, DS::Kind::Link, kUpsert) == 0);
    CHECK(DS::linkKey(7, 0, 9, 1) == before);
}

TEST_CASE("DocSync: a peer's node id does not collide with the next local one")
{
    HC::Graph b;                       // an empty local graph — nextId is 1
    auto docB = DS::forHorizonCodeGraph(b);
    DS::DocMirror mirrorB;
    DS::seed(*docB, mirrorB, DS::Scope::Primary);

    std::vector<Delta> ds;
    Delta d;
    d.kind = static_cast<std::uint8_t>(DS::Kind::Node);
    d.op = kUpsert; d.itemId = 40;
    HC::Node n; n.id = 40; n.type = HC::NodeType::Print;
    d.json = HC::nodeToJson(n);
    ds.push_back(d);
    CHECK(DS::applyDeltas(*docB, mirrorB, DS::Scope::Primary, ds));

    // Without the allocator bump the next locally created node would be id 1 —
    // fine here, but the same trap reappears the moment ids run past each other,
    // and a reused id silently overwrites the peer's node.
    HC::Node local; local.type = HC::NodeType::Add;
    const int localId = b.addNode(local);
    CHECK(localId > 40);
}

TEST_CASE("DocSync: material graph nodes, links and comments round-trip")
{
    HE::MaterialGraph a = HE::MaterialGraph::makeDefault();
    auto docA = DS::forMaterialGraph(a);
    DS::DocMirror mirrorA;
    diff(*docA, mirrorA);

    HE::MaterialGraph b = a;
    auto docB = DS::forMaterialGraph(b);
    DS::DocMirror mirrorB;
    DS::seed(*docB, mirrorB, DS::Scope::Primary);

    const int mul = a.addNode(HE::MatNodeType::Multiply, 60.0f, 20.0f);
    HE::MatGraphComment cm;
    cm.id = a.nextId++; cm.text = "lighting"; cm.x = 10.0f; cm.y = 12.0f;
    a.comments.push_back(cm);
    (void)mul;

    const std::vector<Delta> ds = diff(*docA, mirrorA);
    CHECK(countOf(ds, DS::Kind::Node, kUpsert)    == 1);
    CHECK(countOf(ds, DS::Kind::Comment, kUpsert) == 1);

    CHECK(DS::applyDeltas(*docB, mirrorB, DS::Scope::Primary, ds));
    CHECK(HE::materialGraphToJson(b) == HE::materialGraphToJson(a));
}

TEST_CASE("DocSync: particle graph nodes round-trip")
{
    HE::ParticleGraph a = HE::ParticleGraph::makeDefault();
    auto docA = DS::forParticleGraph(a);
    DS::DocMirror mirrorA;
    diff(*docA, mirrorA);

    HE::ParticleGraph b = a;
    auto docB = DS::forParticleGraph(b);
    DS::DocMirror mirrorB;
    DS::seed(*docB, mirrorB, DS::Scope::Primary);

    a.addNode(HE::ParticleNodeType::ConstFloat, 30.0f, 30.0f);

    const std::vector<Delta> ds = diff(*docA, mirrorA);
    CHECK(countOf(ds, DS::Kind::Node, kUpsert) == 1);
    CHECK(DS::applyDeltas(*docB, mirrorB, DS::Scope::Primary, ds));
    CHECK(HE::particleGraphToJson(b) == HE::particleGraphToJson(a));
}

TEST_CASE("DocSync: animator states and transitions round-trip")
{
    HE::AnimatorStateMachineGraph a;
    HE::AnimationState idle; idle.id = 1; idle.name = "Idle";
    HE::AnimationState run;  run.id  = 2; run.name  = "Run";
    a.states = { idle, run };
    a.startState = "Idle";

    auto docA = DS::forAnimatorGraph(a);
    DS::DocMirror mirrorA;
    diff(*docA, mirrorA);

    HE::AnimatorStateMachineGraph b = a;
    auto docB = DS::forAnimatorGraph(b);
    DS::DocMirror mirrorB;
    DS::seed(*docB, mirrorB, DS::Scope::Primary);

    HE::AnimationTransition t;
    t.fromState = "Idle"; t.toState = "Run"; t.paramName = "speed"; t.threshold = 0.3f;
    a.transitions.push_back(t);
    a.states[1].x = 220.0f;

    const std::vector<Delta> ds = diff(*docA, mirrorA);
    CHECK(countOf(ds, DS::Kind::Link, kUpsert) == 1);
    CHECK(countOf(ds, DS::Kind::Node, kUpsert) == 1);   // only the moved state

    CHECK(DS::applyDeltas(*docB, mirrorB, DS::Scope::Primary, ds));
    CHECK(HE::animatorStateMachineToJson(b) == HE::animatorStateMachineToJson(a));
}

TEST_CASE("DocSync: UI elements round-trip, including a type change")
{
    HE::UIWidgetTree a;
    const int panel = a.add(HE::UIWidgetType::Panel);
    const int btn   = a.add(HE::UIWidgetType::Button);
    a.find(btn)->parentId = panel;

    auto docA = DS::forUIWidgetTree(a);
    DS::DocMirror mirrorA;
    diff(*docA, mirrorA);

    HE::UIWidgetTree b = a;
    auto docB = DS::forUIWidgetTree(b);
    DS::DocMirror mirrorB;
    DS::seed(*docB, mirrorB, DS::Scope::Primary);

    // A designer drag, plus a rename.
    a.find(btn)->posX = 64.0f;
    a.find(btn)->name = "Start";

    std::vector<Delta> ds = diff(*docA, mirrorA);
    REQUIRE(ds.size() == 1);
    CHECK(ds[0].itemId == btn);
    CHECK(DS::applyDeltas(*docB, mirrorB, DS::Scope::Primary, ds));
    CHECK(HE::uiWidgetTreeToJson(b) == HE::uiWidgetTreeToJson(a));

    // Replacing an element with one of a DIFFERENT widget type has to work:
    // the type is part of an element's identity, so this cannot be a field
    // patch. Its position in the vector — the designer's draw order — must hold.
    const int slot = static_cast<int>(std::distance(
        a.elements.begin(),
        std::find_if(a.elements.begin(), a.elements.end(),
                     [btn](const std::unique_ptr<HE::UIElement>& e) { return e->id == btn; })));
    auto slider = HE::makeUIElement(HE::UIWidgetType::Slider);
    slider->id = btn;
    slider->parentId = panel;
    a.elements[slot] = std::move(slider);

    ds = diff(*docA, mirrorA);
    REQUIRE(ds.size() == 1);
    CHECK(DS::applyDeltas(*docB, mirrorB, DS::Scope::Primary, ds));
    CHECK(HE::uiWidgetTreeToJson(b) == HE::uiWidgetTreeToJson(a));
    CHECK(b.find(btn)->type() == HE::UIWidgetType::Slider);
}

TEST_CASE("DocSync: deleting a UI parent deletes exactly the elements the sender deleted")
{
    HE::UIWidgetTree a;
    const int panel = a.add(HE::UIWidgetType::Panel);
    const int child = a.add(HE::UIWidgetType::Text);
    a.find(child)->parentId = panel;
    const int loose = a.add(HE::UIWidgetType::Image);

    HE::UIWidgetTree b = a;
    auto docA = DS::forUIWidgetTree(a);
    auto docB = DS::forUIWidgetTree(b);
    DS::DocMirror mirrorA, mirrorB;
    diff(*docA, mirrorA);
    DS::seed(*docB, mirrorB, DS::Scope::Primary);

    a.removeSubtree(panel);   // panel + its child

    const std::vector<Delta> ds = diff(*docA, mirrorA);
    CHECK(countOf(ds, DS::Kind::Element, kRemove) == 2);
    CHECK(DS::applyDeltas(*docB, mirrorB, DS::Scope::Primary, ds));
    CHECK(b.elements.size() == 1);
    CHECK(b.find(loose) != nullptr);
    CHECK(HE::uiWidgetTreeToJson(b) == HE::uiWidgetTreeToJson(a));
}

TEST_CASE("DocSync: the two documents of a widget asset do not bleed into each other")
{
    // A UI widget file holds an element tree AND a HorizonCode graph. They share
    // one lock but are separate documents, so a delta addressed to one must be
    // inert for the other — otherwise a node id and an element id, both small
    // integers, would collide.
    HE::UIWidgetTree tree;
    const int el = tree.add(HE::UIWidgetType::Panel);
    HC::Graph graph;

    auto docTree  = DS::forUIWidgetTree(tree);
    auto docGraph = DS::forHorizonCodeGraph(graph);
    DS::DocMirror mTree, mGraph;
    DS::seed(*docTree,  mTree,  DS::Scope::Primary);
    DS::seed(*docGraph, mGraph, DS::Scope::LogicGraph);

    // A graph-scope delta that names the same integer as the element.
    HC::Node n; n.id = el; n.type = HC::NodeType::Print;
    Delta d;
    d.scope  = static_cast<std::uint8_t>(DS::Scope::LogicGraph);
    d.kind   = static_cast<std::uint8_t>(DS::Kind::Node);
    d.op     = kUpsert;
    d.itemId = el;
    d.json   = HC::nodeToJson(n);
    const std::vector<Delta> ds{ d };

    CHECK_FALSE(DS::applyDeltas(*docTree,  mTree,  DS::Scope::Primary,    ds));
    CHECK(      DS::applyDeltas(*docGraph, mGraph, DS::Scope::LogicGraph, ds));
    CHECK(tree.elements.size() == 1);
    CHECK(graph.nodes.size()   == 1);
}

TEST_CASE("DocSync: an item a peer cannot understand is skipped, not fatal to the batch")
{
    HC::Graph b;
    auto docB = DS::forHorizonCodeGraph(b);
    DS::DocMirror mirrorB;
    DS::seed(*docB, mirrorB, DS::Scope::Primary);

    std::vector<Delta> ds;
    Delta bad;
    bad.kind = static_cast<std::uint8_t>(DS::Kind::Node);
    bad.op = kUpsert; bad.itemId = 1;
    bad.json = R"({"id":1,"type":"SomeNodeFromANewerBuild"})";
    ds.push_back(bad);

    HC::Node good; good.id = 2; good.type = HC::NodeType::Print;
    Delta ok;
    ok.kind = static_cast<std::uint8_t>(DS::Kind::Node);
    ok.op = kUpsert; ok.itemId = 2; ok.json = HC::nodeToJson(good);
    ds.push_back(ok);

    CHECK(DS::applyDeltas(*docB, mirrorB, DS::Scope::Primary, ds));
    CHECK(b.nodes.size() == 1);          // the good one landed
    CHECK(b.findNode(2) != nullptr);
    CHECK(b.findNode(1) == nullptr);
}

TEST_CASE("DocSync: a HorizonCode function interface propagates to its calls on the peer")
{
    HC::Graph a;
    HC::Node entry; entry.type = HC::NodeType::FunctionEntry; entry.s = "Reset";
    const int entryId = a.addNode(entry);
    HC::Node call;  call.type  = HC::NodeType::FunctionCall;  call.s  = "Reset";
    (void)a.addNode(call);

    HC::Graph b = a;
    auto docA = DS::forHorizonCodeGraph(a);
    auto docB = DS::forHorizonCodeGraph(b);
    DS::DocMirror mirrorA, mirrorB;
    diff(*docA, mirrorA);
    DS::seed(*docB, mirrorB, DS::Scope::Primary);

    // Give the function a parameter. Only the ENTRY changes; the call node's
    // pins are derived, and a peer that skipped that propagation would draw a
    // call with no input pin and silently drop the argument.
    a.findNode(entryId)->params.push_back({ "amount", HC::PinType::Float, false });

    const std::vector<Delta> ds = diff(*docA, mirrorA);
    CHECK(DS::applyDeltas(*docB, mirrorB, DS::Scope::Primary, ds));

    const HC::Node* peerCall = nullptr;
    for (const HC::Node& n : b.nodes)
        if (n.type == HC::NodeType::FunctionCall) peerCall = &n;
    REQUIRE(peerCall != nullptr);
    REQUIRE(peerCall->params.size() == 1);
    CHECK(peerCall->params[0].name == "amount");
}

TEST_CASE("DocSync: a pure sibling reorder in the designer reaches the peer")
{
    // Nothing about any element CHANGES here — only their order does, and in the
    // designer that is the draw order. A per-item diff alone cannot see it.
    HE::UIWidgetTree a;
    const int back  = a.add(HE::UIWidgetType::Panel);
    const int front = a.add(HE::UIWidgetType::Image);

    HE::UIWidgetTree b = a;
    auto docA = DS::forUIWidgetTree(a);
    auto docB = DS::forUIWidgetTree(b);
    DS::DocMirror mirrorA, mirrorB;
    diff(*docA, mirrorA);
    DS::seed(*docB, mirrorB, DS::Scope::Primary);

    std::swap(a.elements[0], a.elements[1]);   // "bring to front"

    const std::vector<Delta> ds = diff(*docA, mirrorA);
    REQUIRE(ds.size() == 1);
    CHECK(ds[0].op == 2);                       // reorder, not a payload change
    CHECK(DS::applyDeltas(*docB, mirrorB, DS::Scope::Primary, ds));
    CHECK(b.elements[0]->id == front);
    CHECK(b.elements[1]->id == back);
    CHECK(HE::uiWidgetTreeToJson(b) == HE::uiWidgetTreeToJson(a));

    // Applying it must not look like a local edit on the way back.
    CHECK(diff(*docB, mirrorB).empty());
}

TEST_CASE("DocSync: appending an item is not mistaken for a reorder")
{
    HE::UIWidgetTree a;
    a.add(HE::UIWidgetType::Panel);
    auto docA = DS::forUIWidgetTree(a);
    DS::DocMirror mirrorA;
    diff(*docA, mirrorA);

    a.add(HE::UIWidgetType::Text);

    const std::vector<Delta> ds = diff(*docA, mirrorA);
    // One upsert. A reorder delta on top would be pure noise on every add — the
    // apply path already appends in batch order.
    REQUIRE(ds.size() == 1);
    CHECK(ds[0].op == 0);
}
