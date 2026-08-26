#include "doctest.h"
#include <algorithm>
#include <cctype>
#include <string>

#include "HcGraphShortcuts.h"
#include "HcEditorUtil.h"   // assetNodeTitle — the header a Create node draws

// The quick-spawn bindings are a hand-maintained table, and a table is exactly the
// kind of thing that rots: two nodes silently claiming the same key, a binding for
// a node type that no longer exists, a key the canvas already spends on something
// else. Nothing about those mistakes is visible in a screenshot — the second
// binding just never fires. So the invariants are asserted here instead, where a
// window is not needed.

namespace HCS = HcGraphShortcuts;
namespace HC  = HorizonCode;

TEST_CASE("HorizonCode graph shortcuts: table invariants")
{
	const auto& binds = HCS::bindings();
	REQUIRE(!binds.empty());

	SUBCASE("every key is an uppercase letter, bound once")
	{
		std::string seen;
		for (const auto& b : binds)
		{
			CHECK(b.key >= 'A');
			CHECK(b.key <= 'Z');
			CHECK(seen.find(b.key) == std::string::npos); // no key bound twice
			seen.push_back(b.key);
		}
	}

	SUBCASE("no node type is bound twice")
	{
		for (size_t i = 0; i < binds.size(); ++i)
			for (size_t j = i + 1; j < binds.size(); ++j)
				CHECK(binds[i].type != binds[j].type);
	}

	SUBCASE("the hint is exactly the key, so the menus advertise what fires")
	{
		for (const auto& b : binds)
		{
			REQUIRE(b.hint != nullptr);
			CHECK(std::string(b.hint) == std::string(1, b.key));
			CHECK(HCS::hintFor(b.type) == b.hint);
		}
	}

	SUBCASE("bound types are real, palette-visible node types")
	{
		const auto& reg = HC::nodeRegistry();
		for (const auto& b : binds)
		{
			CHECK(std::find(reg.begin(), reg.end(), b.type) != reg.end());
			// A node the palette cannot name cannot be taught either.
			CHECK(std::string(HC::nodeDisplayName(b.type)) != "");
		}
	}

	SUBCASE("no binding steals a key the canvas claims for itself")
	{
		// G/E open pickers, Q straightens — see reservedKeys(). F is the one
		// documented overlap (For Each vs. frame-selection), disambiguated by
		// whether a click happens while it is held, so it is NOT reserved.
		for (char r : HCS::reservedKeys())
			for (const auto& b : binds)
				CHECK(b.key != r);
		CHECK(std::find(HCS::reservedKeys().begin(), HCS::reservedKeys().end(), 'F')
		      == HCS::reservedKeys().end());
	}

	SUBCASE("hintFor reports nothing for unbound types")
	{
		// Literals stay unbound on purpose: unwired simple inputs carry their
		// value on the pin, so a literal node is the exception, not the rule.
		CHECK(HCS::hintFor(HC::NodeType::ConstFloat)  == nullptr);
		CHECK(HCS::hintFor(HC::NodeType::ConstString) == nullptr);
		CHECK(HCS::hintFor(HC::NodeType::Event)       == nullptr);
	}
}

TEST_CASE("HorizonCode graph shortcuts: the Blueprint five keep their keys")
{
	// B/S/D/F/O are Unreal's bindings key for key — muscle memory is the entire
	// point of choosing those letters, so changing one is a decision, not a typo.
	struct Expect { char key; HC::NodeType type; };
	const Expect expected[] = {
		{ 'B', HC::NodeType::Branch   },
		{ 'S', HC::NodeType::Sequence },
		{ 'D', HC::NodeType::Delay    },
		{ 'F', HC::NodeType::ForEach  },
		{ 'O', HC::NodeType::DoOnce   },
	};
	for (const Expect& e : expected)
	{
		const auto& binds = HCS::bindings();
		const auto it = std::find_if(binds.begin(), binds.end(),
			[&](const HCS::Binding& b){ return b.key == e.key; });
		REQUIRE(it != binds.end());
		CHECK(it->type == e.type);
	}
}

// ── Node headers that name their asset ───────────────────────────────────────
// Create Widget and Create Object both store an asset path and both used to
// draw as the bare node name, so a graph with five of them said "Create Widget"
// five times and the only way to tell them apart was clicking each one. The
// header now carries the asset, the same answer Cast gives with "Cast To X".
// Asserted here rather than looked at, because the failure mode is a title that
// silently falls back to the base name.
TEST_CASE("Asset nodes name their asset in the header")
{
	using HcEditorUtil::assetNodeTitle;

	CHECK(assetNodeTitle("Create Widget", "Content/UI/PauseMenu.hasset")
	      == "Create Widget: PauseMenu");
	// Nested folders change nothing: the stem is what every other asset picker
	// in the editor shows, so the header agrees with the combo box above it.
	CHECK(assetNodeTitle("Create Object", "Content/Enemies/Boss/Goblin.hasset")
	      == "Create Object: Goblin");
	// Not chosen yet — the node still has to say what KIND it is.
	CHECK(assetNodeTitle("Create Widget", "") == "Create Widget");
	// A path with no stem must not produce a dangling colon.
	CHECK(assetNodeTitle("Create Widget", "Content/UI/") == "Create Widget");
}
