#include "../EditorGuides.h"

// ── Guides: menus, saving, and getting it out of the door ────────────────────
// UI and menus, save games, settings, packaging a build for each platform, and
// what changes between pressing Play in the editor and running the packaged
// game — which is where most of the surprises are.
//
// Fill this in with Guides::page(...) entries. Everything you need is in
// EditorGuides.h; EditorGuides.cpp's own npcChaseGuide() is the worked example
// of the shape. Two rules that decide whether a page is worth having: every
// name must be spelled the way the editor spells it (checked against the code
// that draws it), and every place where the editor does nothing instead of
// complaining gets a Guides::warn() next to the step that causes it.

namespace HE::Ed::Guides
{

std::vector<Docs::Page> shippingPages()
{
	return {};
}

} // namespace HE::Ed::Guides
