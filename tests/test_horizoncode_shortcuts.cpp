#include "doctest.h"
#include <algorithm>
#include <cctype>
#include <string>

#include "HcGraphShortcuts.h"
#include "HcEditorUtil.h"   // assetNodeTitle — the header a Create node draws
#include <HorizonCode/HorizonCode.h>
#include <UIWidget/UIWidgetAnim.h>   // the two vocabularies the dropdowns offer
#include <vector>

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

// ── The add menu's engine-call sections ──────────────────────────────────────
// The palette used to draw registry rows under "Engine · <Category>" headers,
// in a second pass below the node categories — so "UI" appeared twice and a
// reader had to know which half a call lived in. The sections are merged now,
// which makes the category list the menu's backbone: it decides which headings
// exist at all.
TEST_CASE("Add menu: engine-call categories are listed once, filtered, and hide what the nodes cover")
{
	const auto all = HcEditorUtil::engineApiCategories("");
	REQUIRE(!all.empty());

	// No duplicates: one heading per category, or the merge gains nothing.
	for (size_t i = 0; i < all.size(); ++i)
		for (size_t j = i + 1; j < all.size(); ++j)
			CHECK(std::string(all[i]) != std::string(all[j]));

	auto has = [](const std::vector<const char*>& v, const char* c) {
		for (const char* s : v) if (std::string(s) == c) return true;
		return false;
	};
	CHECK(has(all, "Transform"));
	CHECK(has(all, "Physics"));

	// The group filter reaches this list too. Filtering only the rows and not
	// the headings would leave empty sections behind.
	const auto onlyPhysics = HcEditorUtil::engineApiCategories("", { "physics" });
	CHECK(has(onlyPhysics, "Physics"));
	CHECK_FALSE(has(onlyPhysics, "Transform"));

	// "log" is deliberately not in the palette: the Print node does that job,
	// and two entries for one thing is what the hidden list exists to stop. The
	// row itself stays — Lua and Python call it as horizon.log — so this can only
	// be checked through the menu, which is what makes the assertion worth having.
	// The proof is that "Debug" does not appear. It used to be "the result is
	// empty", which stopped being true the day a Dialog group arrived: "dialog"
	// contains "log", and matching it is correct behaviour, not a regression.
	CHECK_FALSE(has(HcEditorUtil::engineApiCategories("log"), "Debug"));
	CHECK_FALSE(HcEditorUtil::engineApiCategories("physics").empty());
}

// ── Which string pins are a LIST, and which stay a text box ─────────────────
// A typed string is the most error-prone input a graph has: a misspelled easing
// plays Linear, a misspelled animation plays nothing, a misspelled scene loads
// nothing, and not one of them says so. This is the table that decides which
// pins get a dropdown instead — the half that needs no project, so it can be
// checked here rather than looked at.
TEST_CASE("Engine call parameters: the ones with a closed list of values")
{
	auto call = [](const char* id, std::vector<std::pair<const char*, HC::PinType>> params)
	{
		HC::Node n;
		n.type = HC::NodeType::EngineCall;
		n.s = id;
		for (const auto& [name, type] : params) n.params.push_back({ name, type });
		return n;
	};
	auto choices = [](const HC::Node& n, const char* param)
	{ return HcEditorUtil::engineParamChoices(n, param, nullptr); };
	auto has = [](const std::vector<std::string>& v, const std::string& s)
	{ return std::find(v.begin(), v.end(), s) != v.end(); };

	const HC::Node play = call("widget.playAnimation",
		{ { "widget", HC::PinType::Ref }, { "animation", HC::PinType::String },
		  { "restoreAfterCompleted", HC::PinType::Bool }, { "direction", HC::PinType::String } });

	// The play direction: the enum, spelled the way it is stored.
	const std::vector<std::string> dirs = choices(play, "direction");
	CHECK(dirs.size() == (size_t)HE::UIAnimDirection::COUNT);
	CHECK(has(dirs, "Forward"));
	CHECK(has(dirs, "Backward"));
	CHECK(has(dirs, "Ping Pong"));

	// The easing curves, likewise — every one of them, or the dropdown would be
	// a shorter vocabulary than the engine's.
	const HC::Node anim = call("widget.animate",
		{ { "widget", HC::PinType::Ref }, { "element", HC::PinType::String },
		  { "property", HC::PinType::String }, { "to", HC::PinType::Float },
		  { "seconds", HC::PinType::Float }, { "easing", HC::PinType::String } });
	CHECK(choices(anim, "easing").size() == (size_t)HE::UIEase::COUNT);
	CHECK(has(choices(anim, "easing"), "Out Back"));

	// Three words and the third is the point: System is a rule, not a colour.
	const HC::Node mode = call("theme.setMode", { { "mode", HC::PinType::String } });
	CHECK(choices(mode, "mode") == std::vector<std::string>{ "Light", "Dark", "System" });

	// SDL's own spelling for the pad, asked of SDL so a second copy cannot drift.
	const HC::Node pad = call("input.gamepadButton", { { "button", HC::PinType::String } });
	CHECK(has(choices(pad, "button"), "a"));
	CHECK(has(choices(pad, "button"), "dpup"));
	const HC::Node axis = call("input.gamepadAxis", { { "axis", HC::PinType::String } });
	CHECK(has(choices(axis, "axis"), "leftx"));

	// …and what must stay a text box, because there is no list to draw: a save
	// key, a window title, a URL. An empty answer is the signal for that, so it
	// is as load-bearing as the lists above.
	const HC::Node pref = call("prefs.setString",
		{ { "key", HC::PinType::String }, { "value", HC::PinType::String } });
	CHECK(choices(pref, "key").empty());
	CHECK(choices(pref, "value").empty());
	const HC::Node title = call("app.setTitle", { { "title", HC::PinType::String } });
	CHECK(choices(title, "title").empty());

	// The row decides, not the parameter name alone: "mode" on the theme call is
	// a list, and the same word elsewhere is not.
	const HC::Node other = call("input.mode", { { "mode", HC::PinType::String } });
	CHECK(choices(other, "mode").empty());
	// A node that is not an engine call has no parameters to answer about.
	HC::Node plain; plain.type = HC::NodeType::ConstString;
	CHECK(HcEditorUtil::engineParamChoices(plain, "easing", nullptr).empty());

	// The project-backed lists (scenes, classes, assets, savegame fields) are
	// asked of the ContentManager; with none there is nothing to offer, and the
	// pin falls back to the text box rather than to an empty dropdown.
	const HC::Node scene = call("scene.load", { { "scene", HC::PinType::String } });
	CHECK(choices(scene, "scene").empty());
	CHECK(HcEditorUtil::engineParamHint(scene) != nullptr);
	CHECK(HcEditorUtil::engineParamHint(title) == nullptr);
}
