#include "../EditorGuides.h"

// ── Guides: building the world ───────────────────────────────────────────────
// Terrain, sky and weather, lighting and shadows, materials, props and
// foliage, physics bodies, audio — the level itself rather than the things
// moving through it.
//
// Fill this in with Guides::page(...) entries. Everything you need is in
// EditorGuides.h; EditorGuides.cpp's own npcChaseGuide() is the worked example
// of the shape. Two rules that decide whether a page is worth having: every
// name must be spelled the way the editor spells it (checked against the code
// that draws it), and every place where the editor does nothing instead of
// complaining gets a Guides::warn() next to the step that causes it.

namespace HE::Ed::Guides
{

std::vector<Docs::Page> worldPages()
{
	return {};
}

} // namespace HE::Ed::Guides
