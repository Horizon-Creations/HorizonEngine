#pragma once

// ─── Live document sync: item-level deltas for graphs and the UI designer ────
// Turns "this panel mutated its document" into per-item deltas, and applies
// incoming ones back onto a live document.
//
// WHY DIFFING, and not per-action deltas: every graph host already reports only
// THAT it changed — GraphEditor::draw's return value, HcGraphHost::Host::onEdit,
// the designer's dirty flag. Threading a per-action delta through five editors
// would mean touching the interaction code of each, and re-doing it for every
// action added afterwards (the add menu, paste, the pin-drag menu, box-select
// drag, every context menu, undo/redo). Diffing a shadow copy of the document
// gets all of those for free, cannot drift from what the document actually
// holds, and is cheap: a graph is hundreds of items, and the diff only runs on
// the frames a panel reported a change.
//
// The mirror is also the echo guard. Applying a received delta updates it in the
// same step, so the next diff does not see the peer's own edit as a local change
// and bounce it straight back.
//
// Everything here is UI-thread only, like the panels it serves.

#include <Net/CollabSession.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace HorizonCode { struct Graph; }
namespace HE {
struct MaterialGraph;
struct ParticleGraph;
struct UIWidgetTree;
struct AnimatorStateMachineGraph;
}

namespace CollabDocSync
{

// What an item is. The wire carries this as an opaque byte (HorizonNet does not
// interpret it); the adapters below give it meaning per document type. Kinds are
// deliberately shared across document types where they mean the same thing — a
// node is a node — so the diff and apply loops are type-agnostic.
enum class Kind : std::uint8_t
{
    Node = 0,       // graph node / animator state
    Link,           // graph link / animator transition
    Variable,       // HorizonCode graph variable
    Comment,        // material graph comment box
    Element,        // UI designer element
    COUNT
};
inline constexpr int kKindCount = static_cast<int>(Kind::COUNT);

// Which document inside one asset. A UI widget file holds two: the element tree
// the designer edits and the HorizonCode graph its Graph tab edits. They share
// one lock (deleting an element breaks the nodes that reference it) but are
// addressed separately so a delta lands in the right one.
enum class Scope : std::uint8_t { Primary = 0, LogicGraph = 1 };

// Links and animator transitions carry no id of their own — they are identified
// by their endpoints. This folds those into the 64-bit item id the wire uses.
// Stable under node reordering, because it is built from the ids, not indices.
std::int64_t linkKey(int srcNode, int srcPin, int dstNode, int dstPin);
// Animator transitions name their endpoints by state NAME (see the wart note in
// AnimatorStateMachineGraph.h), so theirs is a hash of from/to/param instead.
std::int64_t transitionKey(const std::string& from, const std::string& to,
                           const std::string& param);

// ── Document adapter ────────────────────────────────────────────────────────
// The one thing a document type has to provide. Everything else — diffing,
// applying, batching, the echo guard — is shared.
struct IDocAdapter
{
    virtual ~IDocAdapter() = default;

    // Every item of `kind`, as (id, item JSON). The JSON must be the SAME form
    // the document serializer writes (HorizonCode::nodeToJson and friends), so
    // what travels and what is saved cannot diverge.
    using ItemFn = std::function<void(std::int64_t id, std::string json)>;
    virtual void enumerate(Kind kind, const ItemFn& fn) const = 0;

    // Create or replace `id`. False = the payload was not usable (an unknown
    // node type from a newer peer), which is skipped rather than fatal.
    virtual bool upsert(Kind kind, std::int64_t id, const std::string& json) = 0;
    virtual bool remove(Kind kind, std::int64_t id) = 0;

    // Put the items of `kind` into this order. Ids the document does not have are
    // ignored; ones the sender did not mention keep their relative position at
    // the end. Sibling order in the UI designer IS draw order, so a pure reorder
    // is a real edit that no per-item payload would carry; and for the graphs it
    // keeps both peers' saved files byte-identical, which matters because the
    // whole-file autosave still runs underneath and would otherwise ping-pong.
    virtual bool reorder(Kind, const std::vector<std::int64_t>&) { return false; }

    // Ran once after a batch is applied, for whatever the document needs to be
    // coherent again: HorizonCode reconciles call/return pins with their entries,
    // the material graph marks its preview dirty.
    virtual void afterApply() {}
};

// ── Mirror ──────────────────────────────────────────────────────────────────
// The last state we know both sides agree on: id → item JSON, per kind.
struct DocMirror
{
    std::unordered_map<std::int64_t, std::string> items[kKindCount];
    // Document order per kind, so a pure reorder is detectable at all — no
    // per-item payload changes when two siblings swap places.
    std::vector<std::int64_t> order[kKindCount];
    bool seeded = false;   // false until the first capture — see seed()
};

// Record the document as-is WITHOUT emitting deltas. Called when a tab opens (or
// when a peer's whole-file update lands): the two sides already agree at that
// point, and diffing against an empty mirror would otherwise announce every node
// in the document as newly created.
void seed(const IDocAdapter& doc, DocMirror& mirror, Scope scope);

// Deltas that take `mirror` to the document's current state, appended to `out`.
// Updates the mirror. Empty out = nothing changed, which is the common case.
void diffInto(const IDocAdapter& doc, DocMirror& mirror, Scope scope,
              std::vector<HE::Net::CollabSession::DocDelta>& out);

// Apply a peer's batch. Only deltas whose scope matches are considered, so one
// call per document of a multi-document asset. Returns true when anything was
// applied (the caller repaints / marks dirty). Updates the mirror, which is what
// stops the applied edit from being diffed back out as ours.
bool applyDeltas(IDocAdapter& doc, DocMirror& mirror, Scope scope,
                 const std::vector<HE::Net::CollabSession::DocDelta>& batch);

// ── What a panel hands out ──────────────────────────────────────────────────
// The live documents behind one open tab, each paired with the mirror tracking
// what the peers have seen. The PANEL owns the mirror (it has to survive between
// frames, alongside the document); the adapter is a per-call view over it.
//
// A vector because one asset can hold more than one document: a UI widget file
// has the element tree AND its HorizonCode graph.
struct DocBinding
{
    Scope                        scope = Scope::Primary;
    std::unique_ptr<IDocAdapter> adapter;
    DocMirror*                   mirror = nullptr;
};
using DocBindings = std::vector<DocBinding>;

// ── Concrete adapters ───────────────────────────────────────────────────────
// Each wraps a live document the panel owns; none of them owns it.
std::unique_ptr<IDocAdapter> forHorizonCodeGraph(HorizonCode::Graph& g);
// `onChanged` fires after a peer's batch lands, for whatever the host has to
// invalidate (the material preview, the generated shader). May be null.
std::unique_ptr<IDocAdapter> forMaterialGraph(HE::MaterialGraph& g,
                                              std::function<void()> onChanged = nullptr);
std::unique_ptr<IDocAdapter> forParticleGraph(HE::ParticleGraph& g);
std::unique_ptr<IDocAdapter> forAnimatorGraph(HE::AnimatorStateMachineGraph& g);
std::unique_ptr<IDocAdapter> forUIWidgetTree(HE::UIWidgetTree& t);

} // namespace CollabDocSync
