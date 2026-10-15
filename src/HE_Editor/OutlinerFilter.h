#pragma once
#include <HorizonScene/HorizonWorld.h>
#include <string>
#include <string_view>
#include <cstddef>
#include <vector>

// ── The Outliner's search box, without the box ───────────────────────────────
// A scene with a few hundred entities could be SHOWN in the Outliner but not
// FOUND in it: the only way to reach "Torch_07" was to remember which subtree
// it hung under and open the way down to it. The header row the panel now
// carries — a text search and a row of type chips, the pair the Content
// Browser has — answers that, and this is the rule behind it, kept ImGui-free
// so a test can ask it with a list instead of a window.
//
// Two decisions live here:
//  * What a row IS, for the type chips. Entities have no type of their own;
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
	// One kind of entity, a chip in the header. `test` reads the registry every frame
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

	// ── What a row IS, at a glance ───────────────────────────────────────────
	// The type chips and the icon on every row are the dropdown's kinds drawn
	// instead of listed, so they ask the same tests. An entity can be several
	// kinds at once (a crate is a Mesh, a Collider and a Rigid Body); the icon
	// shows the one that tells it apart best, by a fixed priority — a Light on a
	// mesh is a light first — and 0 (the "group" icon) when it carries none.
	int         primaryKind(const entt::registry& reg, Entity e);
	// The badge's glyph (one or two characters) and its colour as 0xRRGGBB.
	// Index 0 (All types) has a glyph of its own for the "All" chip.
	const char* kindGlyph(int i);
	unsigned    kindColor(int i);
	// Adds one to counts[i] for every kind `e` satisfies, and to counts[0] — the
	// number the "All" chip wears. `counts` holds kindCount() ints. The panel
	// calls this a slice of the scene per frame rather than the whole of it, so
	// a large scene is counted over a few frames instead of in one hitch.
	void        tally(const entt::registry& reg, Entity e, int* counts);

	// Case-insensitive substring — what a box that says "search" means, not a
	// prefix and not a glob. `needle` is taken as typed; an empty one matches.
	bool nameMatches(std::string_view name, std::string_view needle);
	// Where nameMatches found it, for drawing the hit in the name: the first
	// match's start and length, or {npos, 0} for none (and for an empty needle).
	struct Span { size_t pos = std::string_view::npos; size_t len = 0; };
	Span matchSpan(std::string_view name, std::string_view needle);

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
