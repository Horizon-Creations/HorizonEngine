#pragma once

struct AppContext;

// ── Watch (View ▸ Watch) ─────────────────────────────────────────────────────
// What a HorizonCode run stopped at a breakpoint is holding: the event
// argument it was fired with, the function frames it is inside of (arguments
// and locals, innermost first), the instance's variables, and the outputs the
// nodes before the stop produced. Every value is spelled by HcWatch; this
// window only lays the rows out in sections and a filter.
//
// While nothing is stopped and the world runs, it shows the Game Instance's
// variables live instead — a score or a state machine changing as the game
// goes is worth a glance without a breakpoint. Read-only either way.
namespace HcWatchPanel
{
	// `open` is the View-menu toggle; the window clears it when closed. Drawn
	// from the editor's overlay pass like the other tool windows: a stop is
	// looked at while the graph tab that shows the node is in front.
	void DrawWatchWindow(AppContext& ctx, bool& open);
}
