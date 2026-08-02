#pragma once

struct AppContext;

// ── Collaboration window (View ▸ Collaboration) ──────────────────────────────
// Host a live session or join one. A guest needs only the session ID and the
// join code — the address is resolved through the session directory, so nobody
// has to know or type an IP.
//
// The join code is the session's only credential: it authenticates the encrypted
// peer link and never travels over the network itself, so it is shown here to be
// passed on out of band.
namespace CollabPanel
{
	void DrawCollabWindow(AppContext& ctx, bool& open);
}
