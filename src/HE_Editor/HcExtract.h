#pragma once
#include <HorizonCode/HorizonCode.h>
#include <Types/TypeRegistry.h>
#include <string>
#include <vector>

// ── Extract on Destruct, the editor's half (docs/state-driven-data-exchange-design.md §3.8)
// What the "Extract on Destruct" table shows — one row per member of the chosen
// struct, which variables fit which member, what Auto-Map fills in, which rows
// are broken — and the two "make it for me" buttons (New Struct from
// Variables…, Create OnDestroyed Event), decided WITHOUT ImGui and without the
// content system, so every decision is asserted in he_tests the way HcPull's
// are. HcExtractUi draws it. The red sentences come from
// HorizonCode::extractFailureText, which is what the runtime warns with.
namespace HcExtract
{
	// A variable the class can hand out: its own instance variables and the
	// inherited ones (Graph::inherited), private ones included — the class is
	// giving away its OWN data, so access does not matter here.
	struct ClassVar
	{
		HorizonCode::Variable decl;
		bool                  inherited = false;
	};
	std::vector<ClassVar> classVariables(const HorizonCode::Graph& g);

	// Can `v` fill `f`? Empty = yes; otherwise the tooltip ("Float, needs Int").
	// The runtime's rule (pullShapesCompatible): same shape, numbers convert as
	// scalars, same Enum/Struct definition.
	std::string fitOf(const HE::StructField& f, const HorizonCode::Variable& v);
	// Can Self (the dying instance's Ref) fill `f`? Only a scalar Ref member.
	std::string selfFit(const HE::StructField& f);

	// "Int", "Float[]", "Struct RunStats" — the table's Type column.
	std::string typeText(const HE::StructField& f);
	// The struct asset's display name: its file stem.
	std::string structLabel(const std::string& path);

	// One row of the table.
	struct Row
	{
		std::string member;            // the live field name (the stale one for `removed`)
		std::string type;              // typeText of the field ("" for `removed`)
		std::string var;               // what fills it: a variable, kExtractSelf, or "" = default
		std::string defaultText;       // "default 2.5" — what an unmapped member carries
		bool        removed = false;   // the entry's member is gone from the struct
		std::string error;             // red: why the entry cannot be filled ("" = fine)
		int         field = -1;        // index into the definition, -1 for `removed`
	};
	// Rows in definition order, then one per entry whose member the struct no
	// longer has ("removed from struct", with a delete button) — nothing
	// vanishes silently.
	std::vector<Row> rows(const HorizonCode::Graph& g, const HE::StructDef& def);

	// ── edits (true = the graph changed) ────────────────────────────────────
	// Map `member` to `var` (a variable name or kExtractSelf); an empty `var`
	// clears the row back to the struct default.
	bool assign(HorizonCode::Graph& g, const std::string& member, const std::string& var);
	// Every row that is still at its default gets the variable of the same name
	// (case-sensitive first, then case-insensitive) when it fits; a Ref member
	// named Self/Owner/Who with no variable of that name gets Self. Rows already
	// mapped are left alone. Returns how many rows it filled.
	int  autoMapByName(HorizonCode::Graph& g, const HE::StructDef& def);
	// Pick the struct: replaces the spec (one struct per class) and pre-fills it
	// by name — visible and undoable as one edit, not done behind the back.
	bool chooseStruct(HorizonCode::Graph& g, const HE::StructDef& def);
	// Back to "extracts nothing".
	bool clear(HorizonCode::Graph& g);
	// Clear every row but keep the struct.
	bool clearMap(HorizonCode::Graph& g);

	// ── New Struct from Variables… ───────────────────────────────────────────
	// A struct definition with one field per ticked variable: same name, same
	// shape, the variable's default as the field default. `assetPath` is the
	// content-relative path it will be saved under. Variables that cannot be a
	// field (Exec) are skipped. Pair with chooseStruct to map them 1:1.
	HE::StructDef structFromVariables(const HorizonCode::Graph& g,
	                                  const std::vector<std::string>& vars,
	                                  const std::string& name, const std::string& assetPath);
	// A name for it that is a valid C++/script identifier ("EnemyReport").
	bool isValidStructName(const std::string& name);

	// ── the other direction ─────────────────────────────────────────────────
	// The members `var` is extracted into, in table order.
	std::vector<std::string> extractedTo(const HorizonCode::Graph& g, const std::string& var);
	// "Extracted to EnemyReport.Kills" (empty when it is not).
	std::string describe(const HorizonCode::Graph& g, const std::string& var);
	// A variable of this graph was renamed: the entries follow. Returns the
	// number of entries rewritten.
	int renameVariable(HorizonCode::Graph& g, const std::string& from, const std::string& to);

	// ── the listener side: "Create OnDestroyed Event" ───────────────────────
	enum class Listener
	{
		Created,        // declaration and handler added
		HandlerAdded,   // the declaration was there with the right type, the handler was not
		AlreadyThere,   // both there, right type: nothing to do
		WrongType,      // an OnDestroyed with ANOTHER argument exists (one per class, §3.6)
	};
	// Declare OnDestroyed with `structPath` as its argument ("" = no argument)
	// in the LISTENER's graph and put its Event node on the canvas. Bind Event
	// then offers it, and the runtime fires it. `what` receives the line the
	// editor shows.
	Listener createOnDestroyedEvent(HorizonCode::Graph& listener, const std::string& structPath,
	                                std::string* what = nullptr);
	// What the listener's OnDestroyed currently takes: false = it has none;
	// otherwise `structPath` is its struct ("" = no argument).
	bool onDestroyedArg(const HorizonCode::Graph& listener, std::string& structPath);
}
