#pragma once

#include <cstddef>
#include <functional>
#include <string>

struct ImFont;

// ─── The console's buffer and its drawing ────────────────────────────────────
// ConsolePanel.h explains what the console is for and what it can not show.
// This is the half that knows nothing about the AppContext: the sink that
// collects HE_LOG records, the ring they land in, and the ImGui body of the
// window. It takes what it needs to LEAD somewhere (a node, a script line)
// as callbacks, which is what lets tests/test_ui_shot.cpp draw the real
// panel headless.
//
// Gated on __has_include(<imgui.h>), not HE_IMGUI_ENABLED: the tests compile
// this file without that define.
namespace ConsoleView
{
	// Start/stop forwarding HE_LOG records into the buffer. ImGui-free.
	void attachToEngineLog();
	void detachFromEngineLog();

	// What a row can lead to. All three are optional; a null one means the row
	// has nowhere to go, and the link that would use it is not offered.
	struct Hooks
	{
		ImFont* codeFont = nullptr;   // the monospace face the rows are set in
		// A HorizonCode runtime key ("level:<uuid>") as the user knows the tab
		// ("the Level Script"). Null = the key itself.
		std::function<std::string(const std::string& hcKey)>                    tabLabelFor;
		std::function<void(const std::string& hcKey, int node)>                 goToNode;
		// The script is the module name it runs under; the asset behind it is
		// the caller's to look up, on the click, because the world can have
		// changed since the line was written.
		std::function<void(const std::string& script, int line, bool python)>   goToLine;
	};

	// Takes what was logged since the last frame into the ring. draw() does it
	// too; the window calls this BEFORE Begin so a rolled-up window keeps its
	// history current instead of coming back with a gap in it.
	void pump();

	// The window's body: toolbar and rows. The caller owns Begin/End.
	void draw(const Hooks& hooks);

	// What the buffer holds, in OCCURRENCES unless it says rows: a row folded
	// from 38 repeats is one row and 38 occurrences. byLevel ignores the level
	// filter (it is what each chip shows), shown applies all of them.
	struct Stats
	{
		std::size_t rows     = 0;
		long        total    = 0;
		long        shown    = 0;
		long        byLevel[6] = {};   // Trace, Debug, Info, Warning, Error, Critical
	};
	Stats stats();

	// Empties the buffer, the filters and the selection. For tests, which share
	// one process (and therefore one buffer) between scenes.
	void resetForTest();
}
