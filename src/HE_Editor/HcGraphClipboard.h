#pragma once
#include <string>
#include <vector>

namespace HorizonCode { struct Graph; }

// ── HorizonCode node clipboard ───────────────────────────────────────────────
// One process-wide clipboard shared by every HorizonCode graph editor (Level
// Script, Game Instance, HC Class, UI Widget graph), so nodes copy from one
// graph and paste into another — the material editor has had this since v3 and
// the HC graphs were the odd ones out.
//
// The payload is a HorizonCode graph JSON holding only the selected nodes plus
// the links whose BOTH endpoints are in the selection (an external wire belongs
// to the original, not the copy — same rule as HC::duplicateNodes).
namespace HcClipboard
{
	// Copy `ids` out of `g`. Event and FunctionEntry nodes are skipped: their
	// handler/function names must stay unique per graph, so cloning one would
	// produce a second handler for the same event. Returns false when the
	// selection held nothing copyable (the clipboard is then left untouched).
	bool copy(const HorizonCode::Graph& g, const std::vector<int>& ids);

	// Paste the clipboard into `g` with fresh ids, the group's top-left placed
	// at (atX, atY) in GRAPH space, every pasted node assigned to `subgraph`
	// (the sub-graph currently being edited — 0 = the main event graph).
	// Returns the new node ids, empty when there was nothing to paste.
	std::vector<int> paste(HorizonCode::Graph& g, float atX, float atY, int subgraph = 0);

	bool empty();
}
