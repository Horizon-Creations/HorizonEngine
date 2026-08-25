#pragma once

struct AppContext;

// ── The editor's console (View ▸ Console) ────────────────────────────────────
// Until this panel existed the editor had exactly one way to say something went
// wrong — the footer bell — and that channel filters hard on Error by design
// (NotificationStore.h: "A log answers 'what happened'; this answers 'what still
// needs you'"). Everything below Error, and every deliberate print a script
// makes, went to stdout. An editor started from Finder or Explorer has no
// terminal attached to catch it, so print-debugging — the first method anybody
// reaches for — produced nothing at all.
//
// This is the other half: a plain reading of HE_LOG, ALL levels, nothing
// collapsed away by severity.
//
// Deliberately NOT the notification store's throttling. That surface drops a
// repeated error inside a cooldown and caps a burst, because a bell that fires
// four thousand times is a bell nobody reads. A console that drops lines is a
// console that lies about what the program did, so here a repeat is COUNTED
// (consecutive identical lines collapse into one row with a ×N badge) rather
// than dropped — which bounds the per-frame-error case without losing the fact
// that it happened, and keeps the ring from being flushed by a single bad
// shader compile.
//
// What it can NOT show: records the log filtered out upstream. Trace and Debug
// are off for most categories by default (HE_LOG env var / logVerbosity in
// config.json set them), and a record that never reaches a sink cannot reach a
// panel either. The level filter here narrows what the log emitted; it cannot
// widen it.
namespace ConsolePanel
{
	// Start/stop forwarding HE_LOG records into the panel's buffer. Called once
	// each from EditorApplication (OnInit / OnShutdown) rather than from the
	// panel itself: the buffer has to fill whether or not anybody ever opens the
	// window, or opening it after something went wrong would show an empty list.
	//
	// Lines logged BEFORE the attach (renderer creation, window setup) are not in
	// it — they are in HorizonEngine.log, and the log's own ring buffer keeps
	// them as formatted strings with no level attached, which the level filter
	// here could only lie about.
	void attachToEngineLog();
	void detachFromEngineLog();

	// `open` is the View-menu toggle; the window clears it when closed. Drawn
	// from the editor's overlay pass like the other tool windows, so it stays
	// readable while an asset tab (a script, a material graph) is in front —
	// which is exactly when someone is watching for output.
	void DrawConsoleWindow(AppContext& ctx, bool& open);
}
