#pragma once

// ── Footer widget: is anything driving this editor from outside? ─────────────
// The visible half of the MCP bridge's security model. The gate itself is in
// McpBridge (off by default, loopback only, token in a 0600 endpoint file, a
// closed tool list); this is the part that answers the question a human has to
// be able to answer at a glance, without opening a window: *is it on right now,
// and is anybody on it?*
//
// That matters more here than for the neighbouring footer widgets. A download
// queue that is running is visible in the project; a collaborator who is in the
// scene has a name and a cursor. A remote client is a program with no face, and
// the edits it makes look exactly like the ones the user just made themselves.
// If the only place that says so is a Preferences page, then the honest answer
// to "did I do that or did it?" costs three clicks — and nobody pays them.
//
// Same shape as EngineContentSyncBar and CollabPresenceBar next to it:
// FooterWidth() reports the horizontal space to reserve and returns 0 when
// there is nothing to say, DrawFooter() draws it at that position. Zero is the
// normal case here — the bridge is off in almost every session — and that is
// deliberate: a strip that is always present is a strip nobody reads, so the
// footer stays empty until the answer stops being "no".
//
// Three states, because two would be a lie (see McpBridge::setEnabled): the
// listener can be ON, it can be OFF, and it can be *wanted but not up* — a
// start() that failed is never retried, so the setting says yes while nothing
// is listening. That third state is the one worth a colour.

struct AppContext;

namespace McpStatusBar
{
	float FooterWidth(AppContext& ctx);
	// Draws the line. Clicking it opens Preferences on the Remote Control page —
	// the one control a user reaches for when this bar says something they did
	// not expect.
	void  DrawFooter(AppContext& ctx);
}
