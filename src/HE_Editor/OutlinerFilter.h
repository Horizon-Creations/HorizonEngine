#pragma once
#include <HorizonScene/HorizonWorld.h>
#include <string>
#include <string_view>
#include <vector>

// ── The Outliner's search box, without the box ───────────────────────────────
// A scene with a few hundred entities could be SHOWN in the Outliner but not
// FOUND in it: the only way to reach "Torch_07" was to remember which subtree
// it hung under and open the way down to it. The header row the panel now
// carries — a text search and a type dropdown, the same pair the Content
// Browser has — answers that, and this is the rule behind it, kept ImGui-free
// so a test can ask it with a list instead of a window.
//
// Two decisions live here:
//  * What a row IS, for the type dropdown. Entities have no type of their own;
//    they are the sum of their components, so "type" means "carries the
//    component that makes it that kind of thing" — a Light is anything with a
//    LightComponent. The labels are the Details panel's section names, so the
//    dropdown and the panel that opens on the hit read the same way.
//  * Which rows stay on screen. A hit is shown; every ancestor of a hit is
//    shown too, dimmed, because a row without its path says nothing about
//    WHERE the thing is — and where is the whole question the search answers.
//    Siblings, and everything under a row that neither hits nor leads to one,
//    go. The result is therefore closed under "parent of": no shown row ever
//    hangs under a hidden one, which is what lets the panel draw it with the
//    same depth-walk it draws the unfiltered tree with.
namespace OutlinerFilter
{
	// One entry of the type dropdown. `test` reads the registry every frame
	// rather than a cached flag: a Light added in the Details panel does not
	// dirty the hierarchy, and a filter that showed it a rebuild late would be
	// a filter the user learns not to trust.
	struct Kind
	{
		const char* label;
		bool (*test)(const entt::registry&, Entity);
	};

	// The dropdown, in the order the "Create ▸" menu offers things and the
	// Details panel names them. Index 0 is "All types" and matches everything.
	int         kindCount();
	const Kind& kindAt(int i);
	constexpr int kAllKinds = 0;

	// Case-insensitive substring — what a box that says "search" means, not a
	// prefix and not a glob. `needle` is taken as typed; an empty one matches.
	bool nameMatches(std::string_view name, std::string_view needle);

	// What became of a row once the filter ran over the tree.
	enum class Show : unsigned char
	{
		Hidden,   // neither a hit nor on the way to one
		Context,  // an ancestor of a hit, shown dimmed for the path it gives
		Hit,      // what the user asked for
	};

	// One row of the hierarchy as the panel caches it: its depth, and whether
	// it satisfies the search AND the type on its own.
	struct Row
	{
		int  depth;
		bool matches;
	};

	// What the panel needs to know per row once the filter ran.
	struct Shown
	{
		Show show       = Show::Hidden;
		// Whether anything directly under this row is shown — the panel opens
		// such a row and draws the arrow; a hit whose children all fell away
		// is drawn as a leaf, so an arrow never opens onto nothing.
		bool childShown = false;
	};

	// The closure rule above, over rows in depth-first order (each row's
	// children follow it, one deeper — the order the Outliner's cache is
	// built in). Returns one entry per row, same order.
	std::vector<Shown> apply(const std::vector<Row>& rows);
}
