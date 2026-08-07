#pragma once

// ─── Footer widget: EngineContent download queue ──────────────────────────────
// Ambient status for the SFTP download queue (see HE::Cs::EngineContentSync),
// mirrored on SourceControlPanel::DrawFooterStatus / CollabPresenceBar's footer
// pair: FooterWidth() reports how much horizontal space to reserve (0 when
// there is nothing to show — the footer draws NOTHING while the queue is idle,
// same "don't train people to ignore a bar that is always there" reasoning the
// git status bar follows), DrawFooter() draws it.
//
// Only declared when HE_HAVE_LIBSSH2 is defined — see EditorUI.cpp's call site.

struct AppContext;

namespace EngineContentSyncBar
{
	float FooterWidth(AppContext& ctx);
	void  DrawFooter(AppContext& ctx);
}
