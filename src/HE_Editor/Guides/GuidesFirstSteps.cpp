#include "../EditorGuides.h"

// ── Guides: getting something on the screen ──────────────────────────────────
// The first hour. A project, a scene, something visible, something you can walk
// around in — each one a page that ends with the reader having a thing, not
// understanding a subsystem.
//
// Fill this in with Guides::page(...) entries. Everything you need is in
// EditorGuides.h; EditorGuides.cpp's own npcChaseGuide() is the worked example
// of the shape. Two rules that decide whether a page is worth having: every
// name must be spelled the way the editor spells it (checked against the code
// that draws it), and every place where the editor does nothing instead of
// complaining gets a Guides::warn() next to the step that causes it.

namespace HE::Ed::Guides
{

std::vector<Docs::Page> firstStepsPages()
{
	return {};
}

} // namespace HE::Ed::Guides
