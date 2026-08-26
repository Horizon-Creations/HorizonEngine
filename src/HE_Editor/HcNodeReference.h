#pragma once

namespace HE::Ed::Docs { class Library; }

// ── The node reference, built from the engine ────────────────────────────────
// "HorizonCode Node Reference" used to be a page on the website: two sections, a
// couple of long tables, hand-maintained, and already behind the engine — a
// call added to HE::api::registry() appeared in the editor's add menu and
// nowhere in the documentation.
//
// So it is not written any more, it is BUILT: one section per callable thing,
// from the registries themselves plus the descriptions in HcNodeDocs. Which
// means the reference cannot list a call that does not exist, cannot miss one
// that does, and shows each one's real pins — the same signature the node on the
// canvas has, in the same colours.
//
// It joins the manual as an ordinary page (DocsLibrary::appendPage), under the
// id the website page had: every cross-link and F1 anchor written against
// "horizoncode-nodes" keeps resolving, and search, navigation and history need
// no special case for it.
namespace HE::Ed::NodeReference
{
	// The page id it is published under — the same one the website used.
	inline constexpr const char* kPageId = "horizoncode-nodes";

	// Build the page and put it in the library, replacing any earlier copy
	// (including the one the bundle may still carry). Idempotent.
	void install(Docs::Library& lib);
}
