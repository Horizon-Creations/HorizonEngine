#pragma once

namespace HE::Ed::Docs { class Library; }

// ── The editor's own reference, built from its tooltips ──────────────────────
// The manual in the editor used to be the website, converted. That is the right
// shape for prose — "how does rendering work" — and the wrong one for the
// question people actually arrive with: "what does THIS control do?" The
// measurement that settled it (docs/editor-help-audit-2026-08-26.md): 320 help
// entries pointed at 47 topics, so F1 on "Cloud Fluffiness" opened a chapter
// about the sky.
//
// So the same move the node reference made: the pages are not written, they are
// BUILT — here, from the same table the hover tooltips come from
// (EditorHelp.cpp). One page per area of the editor, one section per control,
// and F1 lands on the control itself.
//
// What that buys, structurally:
//
//   * a tooltip cannot exist without an entry to open, and an entry cannot
//     exist without a tooltip — they are the same row;
//   * the website's chapters stay what they are good at, and become the
//     "more about this" link inside each entry instead of its destination;
//   * a control added with a help entry appears in the manual by itself.
namespace HE::Ed::EditorReference
{
	// Build every area page and put them in the library, replacing earlier
	// copies. Idempotent, like the node reference.
	void install(Docs::Library& lib);
}
