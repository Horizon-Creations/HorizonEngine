#pragma once
#include <functional>
#include <string>

class ContentManager;
struct AppContext;
namespace HorizonCode { struct Graph; struct Variable; struct Node; }

// ── Extract on Destruct in the class sidebar (design §3.8) ───────────────────
// One drawing for both class editors — the level-script panel's class tabs and
// the widget editor's graph view — so the two cannot drift. The decisions live
// in HcExtract; this file draws them and writes the one thing that leaves the
// graph: a struct asset made by "New Struct from Variables…".
namespace HcExtractUi
{
	// What the section needs from the application, set once per frame by the
	// panel entry points (HcExtractUiBind.cpp), like HcPullUi::Targets.
	struct Targets
	{
		ContentManager* content = nullptr;
		// A struct asset was written: refresh the content browser, tell a
		// collaboration session.
		std::function<void(const std::string& contentRel, const std::string& absPath)> onAssetCreated;
	};
	void setTargets(Targets t);
	void bindFrom(AppContext& ctx);

	// The sidebar section: a header and ONE entry ("EnemyReport" or "(none)").
	// `unavailableWhy` non-null greys it out with that tooltip (the level script
	// and the Game Instance have no Destruct, design §3.5). True when the entry
	// was clicked — the caller makes it the selection, like a variable row.
	bool drawSidebarEntry(const HorizonCode::Graph& g, bool selected, const char* unavailableWhy);

	// The details pane: struct picker, one row per member, Auto-Map/Clear,
	// New Struct from Variables…, and the line listeners see. `varPayload` is
	// the drag payload the panel's variable list sends (a variable dropped on
	// a row maps it). True when the graph changed — one undo step per call.
	bool drawDetails(HorizonCode::Graph& g, const char* varPayload);

	// The variable list: `pullNote` (HcPullUi::listNote, may be empty) widened
	// by "extracted" when the variable is in the table; empty = draw the row as
	// always. listTooltip right after the row.
	std::string listNote(const HorizonCode::Graph& g, const HorizonCode::Variable& v,
	                     const std::string& pullNote);
	void        listTooltip(const HorizonCode::Graph& g, const HorizonCode::Variable& v);
	// The variable details: "Extracted to EnemyReport.Kills" with a button that
	// opens the table. True when the button was pressed.
	bool drawVariableLine(const HorizonCode::Graph& g, const HorizonCode::Variable& v);

	// Under Bind Event's picker: "Create OnDestroyed Event", typed with the
	// struct the target class extracts when the panel can say which class that
	// is (`targetClassPath`, "" = unknown → a struct combo). True when `g`
	// changed.
	bool drawBindEventHelper(HorizonCode::Graph& g, const HorizonCode::Node& bind,
	                         const std::string& targetClassPath);
}
