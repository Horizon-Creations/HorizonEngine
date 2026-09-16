#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HorizonCode { class Runtime; struct Value; struct Graph; }

// ── What a stopped HorizonCode run is holding, as rows ───────────────────────
// A run stopped at a breakpoint carries its whole state by value
// (HorizonCode::SuspendedRun): the event argument it was fired with, the call
// frames with their arguments and function locals, and the outputs every exec
// node before the stop produced. The instance it belongs to holds the
// variables. This module reads all of that out of the runtime and turns it
// into named, typed, formatted rows in sections — the watch window draws them,
// a test asserts them.
//
// Read-only by design: a value on the list is what the run SAW; changing it
// under a stopped run is a later step, if ever. Values are formatted into
// strings here rather than handed out as HorizonCode::Value, so the window
// has nothing to interpret: an enum reads as its entry name, a struct as its
// fields, a reference as the class it points at — every Value spelling in one
// place (formatValue), which is also what a tooltip or a log line would use.
//
// Nothing here is kept between frames. build() walks the runtime's stopped
// run every time it is asked; Continue and Step replace that run between
// frames, and a snapshot taken before is simply out of date.
//
// ImGui-free on purpose: this is what the test drives.
namespace HcWatch
{
	// One value on the list.
	struct Row
	{
		std::string name;
		std::string type;    // as the editor spells it: "Float", "Array of Int", "Goblin"
		std::string value;   // formatValue
		// A reference that names a live instance (type == Object), else 0 —
		// the window can offer "the object this points at".
		uint32_t    ref = 0;
	};

	// One group of rows: the event argument, one call frame, the variables,
	// the outputs. `kind` lets the window pick a header style; `title` is
	// what it says.
	struct Section
	{
		enum class Kind { EventArg, Frame, Variables, Outputs };
		Kind             kind = Kind::Variables;
		std::string      title;
		std::vector<Row> rows;
	};

	// The stopped run as the window shows it. `valid` false = no run is
	// stopped at that index (every other field is then empty).
	struct Snapshot
	{
		bool        valid    = false;
		uint32_t    instance = 0;
		std::string classKey;      // the runtime's spelling (HcExecTrace::tabKeyFor maps it)
		size_t      level    = 0;
		int         nodeId   = 0;  // the node the run is stopped at
		std::string nodeLabel;     // "Print", "Set speed", "Call Function Attack"
		size_t      runCount = 0;  // how many runs are stopped in all (the index-th is this)
		std::vector<Section> sections;
	};

	// Everything the index-th stopped run holds (0 = the one Step acts on).
	// Sections in reading order: the event argument (when the run's entry has
	// one), the call frames innermost first, the instance's variables, the
	// outputs the run produced so far. Rows within a section are sorted by
	// name (frames: arguments in declaration order, then locals by name), so
	// the list holds still from frame to frame.
	Snapshot build(const HorizonCode::Runtime& rt, size_t runIndex = 0);

	// The variables of one live instance (stopped or not), sorted by name —
	// what the window shows for the Game Instance while the world runs and
	// nothing is stopped. Empty for an unknown id.
	std::vector<Row> variableRows(const HorizonCode::Runtime& rt, uint32_t instance);

	// ── Spelling ─────────────────────────────────────────────────────────────
	// A Value as one line: `1.5`, `true`, `"text"`, `(1, 2, 3)`, `Goblin #12`,
	// `Idle` (an enum entry by name), `{hp: 10, name: "x"}` (a struct by its
	// definition's fields), `[3] {1, 2, 3}` (a container, first elements only
	// past kMaxItems). `rt` resolves references to their class (null: `#12`).
	std::string formatValue(const HorizonCode::Value& v, const HorizonCode::Runtime* rt = nullptr);
	// A Value's type as the editor spells it, container and definition
	// included: "Float", "Object", "Array of Int", "Map of String to Goblin".
	std::string typeLabel(const HorizonCode::Value& v);
	// A class key as a name: "Goblin" for "Content/Enemies/Goblin.hasset",
	// "Level Script", "Game Instance". Empty in → empty out.
	std::string classLabel(const std::string& classKey);
	// A node as one word or two, without the graph editor's table: the type's
	// display name plus what it names ("Set Variable speed", "Event Ping").
	// Unknown id → "Node N".
	std::string nodeLabel(const HorizonCode::Graph& g, int nodeId);

	// How many container elements formatValue spells out before "…".
	constexpr size_t kMaxItems = 8;
}
