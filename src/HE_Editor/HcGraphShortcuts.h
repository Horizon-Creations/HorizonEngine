#pragma once
#include <HorizonCode/HorizonCode.h>
#include <vector>

// ── HcGraphShortcuts ─────────────────────────────────────────────────────────
// The "hold a key and click" node bindings of the HorizonCode graphs: holding
// one of these keys and left-clicking empty canvas drops that node at the
// cursor (and, mid link-drag, drops it already wired). Blueprint users know
// B/S/D/F/O from Unreal; P/V/N cover the HorizonCode nodes that are just as
// frequent here.
//
// Deliberately plain data and deliberately ImGui-free, so the table is one
// reviewable list and the unit test can assert its invariants (no duplicate
// keys, no collision with the keys the canvas claims for itself) without a
// window. HcGraphHost turns a `key` into an ImGuiKey and into a menu hint.
namespace HcGraphShortcuts
{
namespace HC = HorizonCode;

struct Binding
{
	char         key;   // uppercase ASCII letter ('B' … 'Z')
	HC::NodeType type;
	const char*  hint;  // what the palettes show next to the entry ("B")
};

// The bindings, in the order the reference doc lists them.
const std::vector<Binding>& bindings();

// The hint for a node type ("B"), or nullptr when the type has no binding.
const char* hintFor(HC::NodeType t);

// Keys the canvas claims for something other than dropping a fixed node type:
// G (variable picker), E (engine-call picker), Q (straighten connections). A
// node binding must never reuse one of them.
//
// F is the ONE deliberate overlap: F + click drops a For Each, while a plain F
// tap (key up without a click) frames the selection — the click disambiguates,
// so both fit on the key Unreal already trained into people's hands.
const std::vector<char>& reservedKeys();

} // namespace HcGraphShortcuts
