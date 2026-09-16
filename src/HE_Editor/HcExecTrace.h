#pragma once
#include <cstdint>
#include <string>

namespace HorizonCode { class Runtime; }

// ── Where HorizonCode is running, as the editor sees it ──────────────────────
// The interpreter reports every exec node it runs (Runtime::setExecListener)
// and stamps the node that is current around every log line it writes
// (HorizonCode::currentExecSite). This is the editor-side keeper of both:
//
//   • HITS — which node of which graph ran, and how long ago. The canvas asks
//     glowOf() per node every frame and draws a halo that fades over
//     kFadeSeconds, so a running graph lights up along its exec chain while
//     play mode is on. Nothing here is ever "current" in the sense of a paused
//     program counter — that is what breakpoints are for, and they come later.
//     A hit is a moment in the past with a timestamp.
//
//   • REVEAL — a request to show one node: from a console line that carries a
//     site ("take me to the Print that wrote this"), or from anything else that
//     holds a (class key, node id) pair. It is two one-shots because two
//     different parties act on it: the editor shell opens the TAB (it owns the
//     tab bar), then the panel that draws that tab selects and frames the NODE
//     (it owns the canvas state). Neither knows the other's business.
//
// Keys. The runtime spells a class as its ClassIdentity::key: a content-relative
// asset path for a class or widget, "level:<uuid>" for the level script,
// "__game_instance__" for the GameInstance. The two editor-owned graphs are
// normalised to their reserved tab paths at record time (tabKeyFor), because
// the level uuid changes per scene and the editor only ever has one Level
// Script tab; asset paths pass through and match what the class panel and the
// widget editor already key their state on (ClassState::path, State::relPath).
//
// Threading: HorizonCode runs on the main thread (the game loop ticks the
// runtime; the editor draws on the same thread), and the listener is called
// synchronously from inside execution. So this is plain frame-thread state
// with no lock — the log SINK, which does run on whatever thread logs, only
// reads the thread-local site and never touches this module.
//
// ImGui-free on purpose: this is what the test drives.
namespace HcExecTrace
{
	// How long a hit stays visible on the canvas (1.0 at the moment it ran,
	// 0.0 this many seconds later).
	constexpr float kFadeSeconds = 0.8f;

	// The tab that shows the graph a runtime class key names: the reserved tab
	// path for the level script / GameInstance, the content-relative path
	// itself for everything else. Empty in → empty out.
	std::string tabKeyFor(const std::string& runtimeKey);

	// ── Hits ─────────────────────────────────────────────────────────────────
	// Install the listener on `rt` (replaces any previous one). Called once at
	// editor start on the app-wide runtime, which also runs widget previews —
	// so a previewed widget's graph lights up the same way a played level's does.
	void attach(HorizonCode::Runtime& rt);

	// Record that `nodeId` of the graph `runtimeKey` ran, now (steady clock) or
	// at an explicit time in seconds — the explicit form is for the tests, which
	// cannot wait for a fade.
	void recordHit(const std::string& runtimeKey, int nodeId, uint32_t instance);
	void recordHitAt(const std::string& runtimeKey, int nodeId, uint32_t instance, double atSeconds);

	// 0..1: how brightly the node's halo should show — 1 right after it ran,
	// fading linearly to 0 over kFadeSeconds; 0 when it never ran or the fade is
	// over. `tabKey` is what tabKeyFor returns (the panels pass their own key).
	float glowOf(const std::string& tabKey, int nodeId);
	float glowAt(const std::string& tabKey, int nodeId, double nowSeconds);

	// The instance that last ran anything in `tabKey` (0 = none). The watch
	// window that comes later will want "the object this tab is showing"; the
	// tooltip today can already say which instance a highlighted node ran on.
	uint32_t lastInstanceOf(const std::string& tabKey);

	// Forget every hit (play stopped, project switched). Pending reveals stay:
	// a click on a console line from the last session still names a node that
	// exists.
	void clearHits();

	// ── Reveal ───────────────────────────────────────────────────────────────
	// Ask the editor to show `nodeId` of the graph `runtimeKey` (or a tab key —
	// both spellings are accepted). Replaces a pending request.
	void requestReveal(const std::string& runtimeKey, int nodeId);
	// The shell's half: which tab to open or focus. One-shot. Empty = nothing
	// pending. Content-relative paths still need resolving to the tab's full
	// path by the caller (it has the ContentManager); the reserved tab paths
	// are used as they are.
	bool takeRevealTab(std::string& tabKey);
	// The panel's half: the node to select and frame, if the pending reveal is
	// for `tabKey`. One-shot on a match; a request for another tab is left
	// alone, so every panel can ask every frame.
	bool takeRevealNode(const std::string& tabKey, int& nodeId);
	// Is a reveal pending for `tabKey`? For the shell, which wants to know
	// whether the tab it just opened still owes the panel a node.
	bool revealPendingFor(const std::string& tabKey);
	// Drop a pending reveal (the tab could not be opened).
	void cancelReveal();
}
