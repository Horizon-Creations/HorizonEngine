#pragma once

// ── What the session just did to your project, in the footer ─────────────────
// New assets arriving from other participants, and the host's answer to a
// create of your own. Ambient information, in the same corner and the same
// shape as the presence cluster and the EngineContent queue beside it.
//
// Deliberately NOT a dialog, and the reason is arithmetic rather than taste:
// Save All is one keystroke that can produce a dozen creates, so a modal per
// asset would be a dozen modals for one action of somebody else's. Notices are
// therefore COALESCED — "Alice created 3 assets" — and fade on their own.
//
// One exception in tone: a notice about an asset of YOURS (the host renamed it,
// or refused it and it stays local) is not ambient. It is the only way you find
// out, so it is drawn in a warning colour and stays several times longer.

#include <string>

struct AppContext;

namespace CollabActivityBar
{
	// Width the line will take, in pixels; 0 when there is nothing to say. The
	// footer right-aligns, so it needs this before placing the cursor.
	float FooterWidth(AppContext& ctx);

	// The line itself, drawn inside the footer window at the position
	// FooterWidth() was used to compute. Returns true when it was clicked, which
	// the caller answers by revealing the Collaboration window.
	bool DrawFooter(AppContext& ctx);

	// Drop everything pending. Called when a session ends — a notice about a
	// session you have left is noise, and its author is gone.
	void Reset();
}
