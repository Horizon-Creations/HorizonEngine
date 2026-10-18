#pragma once
#include <UIWidget/UIWidgetTree.h>
#include <Types/Defines.h>
#include <memory>

namespace HE {

// ── One layout pass per frame (Thema 213, CORE-601/701/702/703) ──────────────
// The layout in UIWidgetTree.h is a set of stateless questions: where is this
// element, is it on screen, is it enabled. Each answer walks the parent chain,
// and every step of that walk used to be a linear id lookup; a box child's
// slot rescanned the whole tree twice, a Tab Box asked "which child is this"
// by counting through every element. Asked for every element every frame,
// that is O(n²) and worse — 45 ms a frame for an 1873-element shell.
//
// A scope answers the same questions from tables built once, for as long as
// it lives. The functions themselves do not change their signatures or their
// arithmetic: each one asks whether a scope is active for this tree (on this
// thread) and, if so, reads the table instead of walking. Nothing outside a
// scope is affected — the designer, the MCP tools and every test that asks
// one question at a time keep the old walk.
//
// What a scope holds, and when it stops being true:
//
//   STRUCTURE — id → element, each element's position among its siblings and
//   each parent's children in tree order. True until an element is added,
//   removed, reordered or reparented. Writes to sizes, flags or text leave it
//   true, which is why auto-size can run inside one.
//
//   VALUES (memoizeValues) — the resolved rect, unit scale, visibility,
//   enabled state and opacity of every element asked about, and each grid's
//   solved tracks. True only while NOTHING in the tree is written. Turned on
//   for the draw and hit-test loops, after auto-size and the scroll clamp have
//   had their say; never while those run, because a box child's rect depends
//   on the size its sibling just wrote.
//
// So the rule for a caller: open a scope after the last structural change of
// the frame (syncLists grafts rows), turn values on after the last write, and
// let it close before anything that may run a script. A scope is bound to one
// tree and one canvas; a question about another tree, or on another canvas,
// goes the old way.
class HE_API UILayoutScope
{
public:
    explicit UILayoutScope(const UIWidgetTree& tree, const UIWidgetCanvas* canvas = nullptr);
    ~UILayoutScope();
    UILayoutScope(const UILayoutScope&) = delete;
    UILayoutScope& operator=(const UILayoutScope&) = delete;

    // From here until the scope closes, nothing in the tree is written.
    void memoizeValues();

    struct Impl;
private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace HE
