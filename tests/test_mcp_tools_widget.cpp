#include "doctest.h"

#include "AssetStubWriter.h"
#include "EditorAssetTypeCache.h"
#include "McpToolRegistry.h"
#include "TestFsUtil.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <UIWidget/UIElement.h>
#include <UIWidget/UIWidgetTree.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// ─── Authoring a UI widget from outside the editor ───────────────────────────
// These tools have two code paths that can never both be exercised in a running
// editor, because the second one is defined by a tab NOT existing:
//
//   • with a Designer tab open, the edit belongs to that tab — it lands in the
//     tab's tree, it marks the tab dirty and it does NOT touch the file, or the
//     human's next Save writes over it,
//   • without one, the edit belongs to the asset and the file is written at
//     once, because a client has no Save button to press.
//
// So the questions here are the ones where a shortcut would look green and be
// wrong later:
//
//   • does the live path really leave the file alone, and does the disk path
//     really reach the disk (asked with a SECOND content manager, so an answer
//     out of the first one's cache cannot pass for a write),
//   • is a property name checked against the element's own table — setPropAny
//     writes an unknown one nowhere and says nothing, which on this interface
//     would be a silent lie,
//   • is a refused call really a no-op, or did half its properties land,
//   • is a type name checked — uiWidgetTypeFromName answers Panel for anything
//     it does not know,
//   • does a reparent land in the PLACE that was asked for, sibling order being
//     what a box stacks by,
//   • does every refusal arrive under a code a client can branch on.

using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
using HE::Ed::McpWidgetHooks;
using HE::Ed::ToolResult;
using HE::UIElement;
using HE::UIWidgetTree;
using nlohmann::json;

namespace fs = std::filesystem;

namespace {

struct Fixture
{
	fs::path        root;
	ContentManager  content;
	McpToolRegistry registry;

	// The editor half, as flags a test sets.
	bool        playing = false;
	std::string lockedRel;        // non-empty = a peer holds it

	// The Designer tab, played by a tree on the stack. Empty `liveRel` = no tab
	// holds anything, which is the disk path.
	std::string  liveRel;
	UIWidgetTree live;
	bool         liveDirty = false;
	int          markEditedCalls = 0;
	int          saveCalls = 0;
	bool         saveSucceeds = true;

	explicit Fixture(const std::string& name)
	{
		root = fs::temp_directory_path() /
		       ("he_test_mcp_widget_" + name + "_" + std::to_string(::rand()));
		fs::create_directories(root);
		content.setContentRoot(root.string());
		EditorAssetTypeCache::invalidateAll();

		McpWidgetHooks h;
		h.isPlaying     = [this] { return playing; };
		h.lockedByOther = [this](const std::string& rel) {
			return !lockedRel.empty() && rel == lockedRel;
		};
		h.liveTree = [this](const std::string& rel) -> UIWidgetTree* {
			return (!liveRel.empty() && rel == liveRel) ? &live : nullptr;
		};
		h.markEdited = [this](const std::string&) { ++markEditedCalls; liveDirty = true; };
		h.save       = [this](const std::string&) {
			++saveCalls;
			if (saveSucceeds) liveDirty = false;
			return saveSucceeds;
		};
		h.isDirty = [this](const std::string&) { return liveDirty; };

		HE::Ed::registerWidgetTools(registry, content, std::move(h));
	}

	~Fixture()
	{
		EditorAssetTypeCache::invalidateAll();
		he_test::removeAllQuiet(root);
	}

	ToolResult call(const char* name, json args)
	{
		const McpTool* t = registry.find(name);
		REQUIRE(t != nullptr);
		return t->handler(args);
	}

	// A widget straight onto disk, bypassing the tools — the state a test starts
	// FROM. The stub writer is the editor's own, so what is in a newborn widget
	// here is what is in one the create menu made.
	void writeWidget(const std::string& rel)
	{
		const fs::path abs = root / rel;
		fs::create_directories(abs.parent_path());
		REQUIRE(HE::Ed::writeAssetStub(abs.string(), rel, fs::path(rel).stem().string(),
		                               HE::AssetType::Widget));
		EditorAssetTypeCache::invalidate(abs.string());
	}

	void writeAsset(const std::string& rel, HE::AssetType type)
	{
		const fs::path abs = root / rel;
		fs::create_directories(abs.parent_path());
		REQUIRE(HE::Ed::writeAssetStub(abs.string(), rel, fs::path(rel).stem().string(), type));
		EditorAssetTypeCache::invalidate(abs.string());
	}

	// What is REALLY in the file. Read through a content manager of its own:
	// the fixture's has the asset loaded, and an answer out of its cache would
	// pass for a write that never happened.
	UIWidgetTree fromDisk(const std::string& rel) const
	{
		ContentManager cold;
		cold.setContentRoot(root.string());
		UIWidgetTree t;
		const HE::UUID id = cold.loadAsset(rel);
		if (const UIWidgetAsset* a = id == HE::UUID{} ? nullptr : cold.getWidget(id))
			if (!a->treeJson.empty()) HE::uiWidgetTreeFromJson(a->treeJson, t);
		return t;
	}

	// A tab holding `rel`, with one element already in it — the state most of
	// the live-path tests start from. Returns that element's id.
	int openTab(const std::string& rel)
	{
		liveRel = rel;
		live = UIWidgetTree{};
		liveDirty = false;
		return live.add(HE::UIWidgetType::Panel);
	}
};

// The element with this id in a tree, or null. Its own helper because half the
// assertions below end in one.
const UIElement* elem(const UIWidgetTree& t, int id) { return t.find(id); }

} // namespace

// ─── The registry itself ─────────────────────────────────────────────────────

TEST_CASE("widget tools register under legal names with object schemas")
{
	Fixture f("reg");
	for (const char* n : { "widget_tree", "widget_types", "widget_add", "widget_remove",
	                       "widget_move", "widget_set_properties", "widget_set_anchor",
	                       "widget_save" })
	{
		const McpTool* t = f.registry.find(n);
		REQUIRE_MESSAGE(t != nullptr, n);
		CHECK(McpToolRegistry::enforceNameRule(t->name));
		CHECK(t->inputSchema.is_object());
		CHECK(t->inputSchema["type"] == "object");
		CHECK_FALSE(t->description.empty());
	}
	// The two readers are readers: they are not refused in play mode and are not
	// written to the console log, and `mutates` is what says so.
	CHECK_FALSE(f.registry.find("widget_tree")->mutates);
	CHECK_FALSE(f.registry.find("widget_types")->mutates);
	CHECK(f.registry.find("widget_add")->mutates);
	CHECK(f.registry.find("widget_remove")->mutates);
	CHECK(f.registry.find("widget_move")->mutates);
	CHECK(f.registry.find("widget_set_properties")->mutates);
	CHECK(f.registry.find("widget_set_anchor")->mutates);
	CHECK(f.registry.find("widget_save")->mutates);
}

// ─── The catalog ─────────────────────────────────────────────────────────────

TEST_CASE("widget_types describes every element type there is")
{
	Fixture f("types");
	const ToolResult r = f.call("widget_types", json::object());
	REQUIRE_FALSE(r.isError);
	const json& types = r.content["types"];
	REQUIRE(types.is_array());
	// Not "more than a few": the catalog IS the registry, and a type missing
	// from it is a type a client cannot address at all.
	CHECK(types.size() == HE::uiWidgetTypeRegistry().size());

	bool sawButton = false, sawText = false;
	for (const json& t : types)
	{
		CHECK_FALSE(t["name"].get<std::string>().empty());
		CHECK(t["properties"].is_array());
		if (t["name"] == "Button")
		{
			sawButton = true;
			CHECK(t["acceptsChildren"] == true);
		}
		if (t["name"] == "Text")
		{
			sawText = true;
			CHECK(t["acceptsChildren"] == false);
			// The names are an on-disk format, and they are the whole reason
			// this tool exists: a client cannot guess "FontSize".
			bool hasText = false, hasFontSize = false;
			for (const json& p : t["properties"])
			{
				if (p["name"] == "Text")     { hasText = true;     CHECK(p["type"] == "string"); }
				if (p["name"] == "FontSize") { hasFontSize = true; CHECK(p["type"] == "float"); }
				if (p["name"] == "Color")    CHECK(p["type"] == "color");
				if (p["name"] == "Position") CHECK(p["type"] == "vec2");
			}
			CHECK(hasText);
			CHECK(hasFontSize);
		}
	}
	CHECK(sawButton);
	CHECK(sawText);

	// The sixteen anchors, with the rectangle each one names — the numbers are
	// the truth, the name is the convenience.
	const json& presets = r.content["anchorPresets"];
	REQUIRE(presets.size() == 16);
	CHECK(presets[0]["name"] == "TopLeft");
	CHECK(presets[15]["name"] == "Fill");
	CHECK(presets[15]["rect"] == json::array({ 0.0f, 0.0f, 1.0f, 1.0f }));
}

TEST_CASE("widget_types can name one type, and refuses one that is not a type")
{
	Fixture f("types1");
	const ToolResult one = f.call("widget_types", json{ { "type", "Slider" } });
	REQUIRE_FALSE(one.isError);
	REQUIRE(one.content["types"].size() == 1);
	CHECK(one.content["types"][0]["name"] == "Slider");

	const ToolResult bad = f.call("widget_types", json{ { "type", "Frobnicator" } });
	CHECK(bad.isError);
	CHECK(bad.errorCode == "not_found");
	// The refusal carries the list, because a client that guessed wrong has no
	// other way to recover.
	CHECK(bad.errorMessage.find("Panel") != std::string::npos);
}

// ─── Reading ─────────────────────────────────────────────────────────────────

TEST_CASE("widget_tree reads a newborn widget")
{
	Fixture f("read");
	f.writeWidget("UI/Menu.hasset");

	const ToolResult r = f.call("widget_tree", json{ { "path", "UI/Menu.hasset" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["source"] == "asset");
	CHECK(r.content["canvas"]["width"] == 1920.0f);
	CHECK(r.content["canvas"]["height"] == 1080.0f);
	CHECK(r.content["elements"].is_array());
	CHECK(r.content["elements"].empty());
	CHECK(r.content["roots"].empty());
}

TEST_CASE("widget_tree with an element reports every property it has")
{
	Fixture f("read1");
	f.writeWidget("UI/Menu.hasset");
	const ToolResult add = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                                  { "type", "Text" } });
	REQUIRE_FALSE(add.isError);
	const int id = add.content["id"].get<int>();

	const ToolResult r = f.call("widget_tree", json{ { "path", "UI/Menu.hasset" },
	                                                 { "element", id } });
	REQUIRE_FALSE(r.isError);
	const json& e = r.content["element"];
	CHECK(e["type"] == "Text");
	CHECK(e["id"] == id);
	// The type's own properties AND the base ones every element shares, which is
	// what makes 'Position' settable on anything.
	CHECK(e["properties"].contains("Text"));
	CHECK(e["properties"].contains("FontSize"));
	CHECK(e["properties"].contains("Position"));
	CHECK(e["properties"]["Position"].is_array());
	CHECK(e["properties"]["Color"].size() == 4);
	CHECK(e["propertyTypes"].is_array());
}

TEST_CASE("widget_tree refuses what is not a widget, and what is not in the project")
{
	Fixture f("read2");
	f.writeAsset("Materials/Steel.hasset", HE::AssetType::Material);

	const ToolResult wrong = f.call("widget_tree", json{ { "path", "Materials/Steel.hasset" } });
	CHECK(wrong.isError);
	CHECK(wrong.errorCode == "invalid_path");

	const ToolResult gone = f.call("widget_tree", json{ { "path", "UI/Nope.hasset" } });
	CHECK(gone.isError);

	// The content root is a boundary, not a convention '..' walks through.
	const ToolResult out = f.call("widget_tree", json{ { "path", "../../secrets.hasset" } });
	CHECK(out.isError);
}

// ─── Adding ──────────────────────────────────────────────────────────────────

TEST_CASE("widget_add writes the file when no Designer tab holds the widget")
{
	Fixture f("add");
	f.writeWidget("UI/Menu.hasset");

	const ToolResult r = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                                { "type", "Panel" },
	                                                { "name", "Backdrop" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["target"] == "disk");
	CHECK(r.content["dirty"] == false);
	const int id = r.content["id"].get<int>();
	CHECK(r.content["element"]["name"] == "Backdrop");
	// Not a redraw of what we sent: read back out of the file, through a content
	// manager that has never seen this asset.
	const UIWidgetTree disk = f.fromDisk("UI/Menu.hasset");
	REQUIRE(disk.elements.size() == 1);
	CHECK(disk.elements[0]->id == id);
	CHECK(disk.elements[0]->name == "Backdrop");
	CHECK(disk.elements[0]->parentId == 0);
	// No tab was involved, so nothing was marked dirty anywhere.
	CHECK(f.markEditedCalls == 0);
}

TEST_CASE("widget_add follows the Designer's two placements")
{
	Fixture f("add2");
	f.writeWidget("UI/Menu.hasset");

	// A palette click: middle-centre of its parent.
	const ToolResult mid = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                                  { "type", "Button" } });
	REQUIRE_FALSE(mid.isError);
	CHECK(mid.content["element"]["anchorPreset"] == 5);
	CHECK(mid.content["element"]["position"] == json::array({ 0.0f, 0.0f }));

	// A drop point: top-left, and the position is the offset from the parent's
	// own top-left corner.
	const ToolResult at = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                                 { "type", "Text" },
	                                                 { "position", json::array({ 40.0, 70.0 }) },
	                                                 { "size", json::array({ 200.0, 24.0 }) } });
	REQUIRE_FALSE(at.isError);
	CHECK(at.content["element"]["anchorPreset"] == 0);
	CHECK(at.content["element"]["position"] == json::array({ 40.0f, 70.0f }));
	CHECK(at.content["element"]["size"] == json::array({ 200.0f, 24.0f }));
}

TEST_CASE("widget_add can set properties in the same call, and refuses one that is not there")
{
	Fixture f("add3");
	f.writeWidget("UI/Menu.hasset");

	const ToolResult ok = f.call("widget_add", json{
		{ "path", "UI/Menu.hasset" }, { "type", "Text" },
		{ "properties", json{ { "Text", "Play" }, { "FontSize", 28 },
		                      { "Color", json::array({ 1.0, 0.5, 0.25, 1.0 }) } } } });
	REQUIRE_FALSE(ok.isError);
	const UIWidgetTree disk = f.fromDisk("UI/Menu.hasset");
	REQUIRE(disk.elements.size() == 1);
	CHECK(disk.elements[0]->getPropAny("Text").s == "Play");
	// 28 is a number where the table says float — a client that sends the whole
	// number it meant must not be refused for it.
	CHECK(disk.elements[0]->getPropAny("FontSize").f == doctest::Approx(28.0f));

	const ToolResult bad = f.call("widget_add", json{
		{ "path", "UI/Menu.hasset" }, { "type", "Panel" },
		{ "properties", json{ { "Text", "nope" } } } });
	CHECK(bad.isError);
	CHECK(bad.errorCode == "invalid_payload");
	CHECK(bad.errorMessage.find("Color") != std::string::npos);   // it names what a Panel HAS
	// And the refusal left the widget alone: the Panel never joined the tree.
	CHECK(f.fromDisk("UI/Menu.hasset").elements.size() == 1);
}

TEST_CASE("widget_add refuses a type that is not a type, rather than making a Panel of it")
{
	Fixture f("add4");
	f.writeWidget("UI/Menu.hasset");
	const ToolResult r = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                                { "type", "Buton" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");
	CHECK(r.errorMessage.find("Button") != std::string::npos);
	CHECK(f.fromDisk("UI/Menu.hasset").elements.empty());
}

TEST_CASE("widget_add refuses a parent that takes no children, and one that is not there")
{
	Fixture f("add5");
	f.writeWidget("UI/Menu.hasset");
	const ToolResult text = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                                   { "type", "Text" } });
	REQUIRE_FALSE(text.isError);
	const int textId = text.content["id"].get<int>();

	const ToolResult under = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                                    { "type", "Button" },
	                                                    { "parent", textId } });
	CHECK(under.isError);
	CHECK(under.errorCode == "invalid_payload");
	CHECK(under.errorMessage.find("no children") != std::string::npos);

	const ToolResult ghost = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                                    { "type", "Button" },
	                                                    { "parent", 9999 } });
	CHECK(ghost.isError);
	CHECK(ghost.errorCode == "not_found");

	CHECK(f.fromDisk("UI/Menu.hasset").elements.size() == 1);
}

TEST_CASE("widget_add lands in the place among the siblings that was asked for")
{
	Fixture f("add6");
	f.writeWidget("UI/Menu.hasset");
	const int box = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                           { "type", "VerticalBox" } }).content["id"];
	const int a = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                         { "type", "Text" }, { "parent", box },
	                                         { "name", "A" } }).content["id"];
	const int c = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                         { "type", "Text" }, { "parent", box },
	                                         { "name", "C" } }).content["id"];
	// In FRONT of C, which is what makes the row order the author's and not the
	// order they happened to type things in.
	const ToolResult mid = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                                  { "type", "Text" }, { "parent", box },
	                                                  { "name", "B" }, { "before", c } });
	REQUIRE_FALSE(mid.isError);
	const int b = mid.content["id"].get<int>();

	const UIWidgetTree disk = f.fromDisk("UI/Menu.hasset");
	CHECK(disk.childrenOf(box) == std::vector<int>{ a, b, c });
}

// ─── Properties ──────────────────────────────────────────────────────────────

TEST_CASE("widget_set_properties writes by name, and reads back what landed")
{
	Fixture f("prop");
	f.writeWidget("UI/Menu.hasset");
	const int list = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                            { "type", "ListView" } }).content["id"];

	const ToolResult r = f.call("widget_set_properties", json{
		{ "path", "UI/Menu.hasset" }, { "element", list },
		{ "properties", json{ { "Row Height", 40.0 },
		                      { "Position", json::array({ 10.0, 20.0 }) },
		                      { "Visible", false },
		                      { "Tooltip", "The saves you have" } } } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["set"]["Row Height"] == doctest::Approx(40.0f));
	CHECK_FALSE(r.content.contains("notPersisted"));

	const UIWidgetTree disk = f.fromDisk("UI/Menu.hasset");
	const UIElement* e = elem(disk, list);
	REQUIRE(e != nullptr);
	CHECK(e->getPropAny("Row Height").f == doctest::Approx(40.0f));
	CHECK(e->posX == doctest::Approx(10.0f));
	CHECK(e->posY == doctest::Approx(20.0f));
	CHECK_FALSE(e->visible);
	CHECK(e->tooltip == "The saves you have");
}

TEST_CASE("widget_set_properties reports the value the element ENDED UP with")
{
	Fixture f("prop2");
	f.writeWidget("UI/Menu.hasset");
	const int list = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                            { "type", "ListView" } }).content["id"];
	// A list of minus five rows is clamped to none by the element itself. Echoing
	// the request back would leave a client believing a number that is not in the
	// widget, so the answer is read out of the element after the write.
	const ToolResult r = f.call("widget_set_properties", json{
		{ "path", "UI/Menu.hasset" }, { "element", list },
		{ "properties", json{ { "Item Count", -5 } } } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["set"]["Item Count"] == 0);
}

TEST_CASE("widget_set_properties says which values the file will not carry")
{
	Fixture f("prop2b");
	f.writeWidget("UI/Menu.hasset");
	const int list = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                            { "type", "ListView" } }).content["id"];

	// A List View's row count is runtime state the widget format deliberately
	// does not persist ("an application that reopens with the last run's row
	// count would be showing rows for data it has not loaded yet"). The write
	// succeeds and the element really holds 12 — and then the file does not, so
	// the tool has to say so instead of leaving a client to find out on the next
	// load.
	const ToolResult r = f.call("widget_set_properties", json{
		{ "path", "UI/Menu.hasset" }, { "element", list },
		{ "properties", json{ { "Item Count", 12 }, { "Row Height", 33.0 } } } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["set"]["Item Count"] == 12);
	REQUIRE(r.content.contains("notPersisted"));
	CHECK(r.content["notPersisted"] == json::array({ "Item Count" }));

	const UIWidgetTree disk = f.fromDisk("UI/Menu.hasset");
	// …and the claim is true in both directions: the row height did survive.
	CHECK(elem(disk, list)->getPropAny("Item Count").i == 0);
	CHECK(elem(disk, list)->getPropAny("Row Height").f == doctest::Approx(33.0f));
}

TEST_CASE("widget_set_properties refuses a name the element does not have, and writes nothing")
{
	Fixture f("prop3");
	f.writeWidget("UI/Menu.hasset");
	const int panel = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                             { "type", "Panel" } }).content["id"];

	const ToolResult r = f.call("widget_set_properties", json{
		{ "path", "UI/Menu.hasset" }, { "element", panel },
		// The first one is real and would land if the call were applied as it is
		// read — which is exactly the half-applied failure a client cannot
		// recover from, because it does not know which half went in.
		{ "properties", json{ { "Tooltip", "hello" }, { "Not A Property", 1 } } } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");
	CHECK(r.errorMessage.find("Not A Property") != std::string::npos);

	const UIWidgetTree disk = f.fromDisk("UI/Menu.hasset");
	REQUIRE(elem(disk, panel) != nullptr);
	CHECK(elem(disk, panel)->tooltip.empty());
}

TEST_CASE("widget_set_properties reads a value by the property's declared type")
{
	Fixture f("prop4");
	f.writeWidget("UI/Menu.hasset");
	const int text = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                            { "type", "Text" } }).content["id"];

	// A colour is not a number, and saying so beats writing r=1 and calling it a
	// colour.
	const ToolResult wrong = f.call("widget_set_properties", json{
		{ "path", "UI/Menu.hasset" }, { "element", text },
		{ "properties", json{ { "Color", 1.0 } } } });
	CHECK(wrong.isError);
	CHECK(wrong.errorMessage.find("r,g,b") != std::string::npos);

	// Three numbers is a colour without a stated alpha, which is opaque — the
	// one abbreviation worth taking.
	const ToolResult rgb = f.call("widget_set_properties", json{
		{ "path", "UI/Menu.hasset" }, { "element", text },
		{ "properties", json{ { "Color", json::array({ 0.2, 0.4, 0.6 }) } } } });
	REQUIRE_FALSE(rgb.isError);
	CHECK(rgb.content["set"]["Color"][3] == doctest::Approx(1.0f));

	const ToolResult vec = f.call("widget_set_properties", json{
		{ "path", "UI/Menu.hasset" }, { "element", text },
		{ "properties", json{ { "Position", json::array({ 1.0 }) } } } });
	CHECK(vec.isError);

	const ToolResult b = f.call("widget_set_properties", json{
		{ "path", "UI/Menu.hasset" }, { "element", text },
		{ "properties", json{ { "WordWrap", "yes" } } } });
	CHECK(b.isError);
}

TEST_CASE("widget_set_properties refuses an element that is not there, naming the ones that are")
{
	Fixture f("prop5");
	f.writeWidget("UI/Menu.hasset");
	const int panel = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                             { "type", "Panel" } }).content["id"];
	const ToolResult r = f.call("widget_set_properties", json{
		{ "path", "UI/Menu.hasset" }, { "element", panel + 500 },
		{ "properties", json{ { "Tooltip", "x" } } } });
	CHECK(r.isError);
	CHECK(r.errorCode == "not_found");
	CHECK(r.errorMessage.find(std::to_string(panel)) != std::string::npos);
}

// ─── Anchors ─────────────────────────────────────────────────────────────────

TEST_CASE("widget_set_anchor re-anchors without moving the element")
{
	Fixture f("anchor");
	f.writeWidget("UI/Menu.hasset");
	const ToolResult add = f.call("widget_add", json{
		{ "path", "UI/Menu.hasset" }, { "type", "Panel" },
		{ "position", json::array({ 100.0, 60.0 }) },
		{ "size", json::array({ 300.0, 120.0 }) } });
	const int id = add.content["id"].get<int>();

	const UIWidgetTree before = f.fromDisk("UI/Menu.hasset");
	const HE::UIWidgetRect was = HE::uiElementRect(before, *elem(before, id));

	const ToolResult r = f.call("widget_set_anchor", json{
		{ "path", "UI/Menu.hasset" }, { "element", id }, { "preset", 15 } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["element"]["anchorPreset"] == 15);
	CHECK(r.content["element"]["anchorPresetName"] == "Fill");

	const UIWidgetTree after = f.fromDisk("UI/Menu.hasset");
	const UIElement* e = elem(after, id);
	REQUIRE(e != nullptr);
	CHECK(e->anchorMaxX == doctest::Approx(1.0f));
	CHECK(e->anchorMaxY == doctest::Approx(1.0f));
	// The whole promise of the default: only what happens on a resize changed.
	const HE::UIWidgetRect now = HE::uiElementRect(after, *e);
	CHECK(now.x == doctest::Approx(was.x));
	CHECK(now.y == doctest::Approx(was.y));
	CHECK(now.w == doctest::Approx(was.w));
	CHECK(now.h == doctest::Approx(was.h));
}

TEST_CASE("widget_set_anchor with keepRect=false lets the rect move, and refuses a preset that is not one")
{
	Fixture f("anchor2");
	f.writeWidget("UI/Menu.hasset");
	const int id = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                          { "type", "Panel" },
	                                          { "position", json::array({ 100.0, 60.0 }) } })
	                   .content["id"];

	const ToolResult r = f.call("widget_set_anchor", json{
		{ "path", "UI/Menu.hasset" }, { "element", id },
		{ "preset", 15 }, { "keepRect", false } });
	REQUIRE_FALSE(r.isError);
	const UIWidgetTree disk = f.fromDisk("UI/Menu.hasset");
	const UIElement* e = elem(disk, id);
	REQUIRE(e != nullptr);
	// uiSetAnchorPreset leaves pos/size alone, so the rect is the anchored span
	// offset by the old position rather than the old rectangle.
	CHECK(e->posX == doctest::Approx(100.0f));
	CHECK(e->anchorMaxX == doctest::Approx(1.0f));

	for (int bad : { -1, 16, 99 })
	{
		const ToolResult t = f.call("widget_set_anchor", json{
			{ "path", "UI/Menu.hasset" }, { "element", id }, { "preset", bad } });
		CHECK(t.isError);
		CHECK(t.errorCode == "invalid_payload");
	}
}

// ─── Moving ──────────────────────────────────────────────────────────────────

TEST_CASE("widget_move reparents and reorders")
{
	Fixture f("move");
	f.writeWidget("UI/Menu.hasset");
	const int box = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                           { "type", "VerticalBox" } }).content["id"];
	const int a = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                         { "type", "Text" }, { "parent", box } }).content["id"];
	const int b = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                         { "type", "Text" }, { "parent", box } }).content["id"];
	const int loose = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                             { "type", "Button" } }).content["id"];

	// Into the box, in front of A — a reparent AND a place, which is the pair
	// the old "write parentId and leave the vector alone" could not express.
	const ToolResult r = f.call("widget_move", json{ { "path", "UI/Menu.hasset" },
	                                                 { "element", loose },
	                                                 { "parent", box }, { "before", a } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["siblings"] == json::array({ loose, a, b }));
	CHECK(f.fromDisk("UI/Menu.hasset").childrenOf(box) == std::vector<int>{ loose, a, b });

	// No parent given = keep the parent, only reorder. Reading that as "make it
	// a root" would be a move nobody asked for.
	const ToolResult re = f.call("widget_move", json{ { "path", "UI/Menu.hasset" },
	                                                  { "element", loose } });
	REQUIRE_FALSE(re.isError);
	CHECK(f.fromDisk("UI/Menu.hasset").childrenOf(box) == std::vector<int>{ a, b, loose });
}

TEST_CASE("widget_move refuses a cycle and a parent that takes no children, untouched")
{
	Fixture f("move2");
	f.writeWidget("UI/Menu.hasset");
	const int box = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                           { "type", "VerticalBox" } }).content["id"];
	const int inner = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                             { "type", "Panel" },
	                                             { "parent", box } }).content["id"];
	const int text = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                            { "type", "Text" } }).content["id"];

	const ToolResult cycle = f.call("widget_move", json{ { "path", "UI/Menu.hasset" },
	                                                     { "element", box },
	                                                     { "parent", inner } });
	CHECK(cycle.isError);
	CHECK(cycle.errorCode == "invalid_payload");

	const ToolResult self = f.call("widget_move", json{ { "path", "UI/Menu.hasset" },
	                                                    { "element", box },
	                                                    { "parent", box } });
	CHECK(self.isError);

	const ToolResult into = f.call("widget_move", json{ { "path", "UI/Menu.hasset" },
	                                                    { "element", box },
	                                                    { "parent", text } });
	CHECK(into.isError);
	CHECK(into.errorMessage.find("no children") != std::string::npos);

	const ToolResult ghost = f.call("widget_move", json{ { "path", "UI/Menu.hasset" },
	                                                     { "element", box },
	                                                     { "parent", 4242 } });
	CHECK(ghost.isError);
	CHECK(ghost.errorCode == "not_found");

	// Every one of those refusals left the hierarchy exactly as it was.
	const UIWidgetTree disk = f.fromDisk("UI/Menu.hasset");
	CHECK(elem(disk, box)->parentId == 0);
	CHECK(elem(disk, inner)->parentId == box);
}

// ─── Removing ────────────────────────────────────────────────────────────────

TEST_CASE("widget_remove takes the whole subtree and says which ids went")
{
	Fixture f("rm");
	f.writeWidget("UI/Menu.hasset");
	const int box = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                           { "type", "VerticalBox" } }).content["id"];
	const int row = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                           { "type", "Panel" },
	                                           { "parent", box } }).content["id"];
	const int leaf = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                            { "type", "Text" },
	                                            { "parent", row } }).content["id"];
	const int keep = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                            { "type", "Button" } }).content["id"];

	const ToolResult r = f.call("widget_remove", json{ { "path", "UI/Menu.hasset" },
	                                                   { "element", box } });
	REQUIRE_FALSE(r.isError);
	std::vector<int> gone = r.content["removed"].get<std::vector<int>>();
	std::sort(gone.begin(), gone.end());
	std::vector<int> want{ box, row, leaf };
	std::sort(want.begin(), want.end());
	CHECK(gone == want);

	const UIWidgetTree disk = f.fromDisk("UI/Menu.hasset");
	CHECK(disk.elements.size() == 1);
	CHECK(disk.elements[0]->id == keep);
}

// ─── The two places a widget can live ────────────────────────────────────────

TEST_CASE("an open Designer tab owns the tree: the edit lands there and the file is untouched")
{
	Fixture f("live");
	f.writeWidget("UI/Menu.hasset");
	// Something already in the FILE, so "the file did not change" is a claim
	// about a document with content rather than about two empty ones.
	f.call("widget_add", json{ { "path", "UI/Menu.hasset" }, { "type", "Panel" },
	                           { "name", "OnDisk" } });
	const UIWidgetTree before = f.fromDisk("UI/Menu.hasset");
	REQUIRE(before.elements.size() == 1);

	const int held = f.openTab("UI/Menu.hasset");

	const ToolResult r = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                                { "type", "Text" },
	                                                { "parent", held },
	                                                { "name", "InTab" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["target"] == "editor");
	CHECK(r.content["dirty"] == true);
	CHECK(f.markEditedCalls == 1);

	// It went into the tab…
	REQUIRE(f.live.elements.size() == 2);
	CHECK(f.live.childrenOf(held).size() == 1);
	// …and nowhere near the file, which still holds only what was written before
	// the tab opened. Anything else would be thrown away by the human's Save.
	const UIWidgetTree after = f.fromDisk("UI/Menu.hasset");
	REQUIRE(after.elements.size() == 1);
	CHECK(after.elements[0]->name == "OnDisk");
}

TEST_CASE("widget_tree reads the tab's unsaved state, not the file")
{
	Fixture f("live2");
	f.writeWidget("UI/Menu.hasset");
	const int held = f.openTab("UI/Menu.hasset");
	f.live.find(held)->name = "Unsaved";
	f.liveDirty = true;

	const ToolResult r = f.call("widget_tree", json{ { "path", "UI/Menu.hasset" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["source"] == "editor");
	CHECK(r.content["dirty"] == true);
	REQUIRE(r.content["elements"].size() == 1);
	CHECK(r.content["elements"][0]["name"] == "Unsaved");
}

TEST_CASE("widget_save writes an open tab, and has nothing to do without one")
{
	Fixture f("save");
	f.writeWidget("UI/Menu.hasset");
	f.openTab("UI/Menu.hasset");
	f.liveDirty = true;

	const ToolResult r = f.call("widget_save", json{ { "path", "UI/Menu.hasset" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["saved"] == true);
	CHECK(r.content["dirty"] == false);
	CHECK(f.saveCalls == 1);

	// A refused write is a failure and says so, rather than reporting a save
	// that did not happen.
	f.liveDirty = true;
	f.saveSucceeds = false;
	const ToolResult bad = f.call("widget_save", json{ { "path", "UI/Menu.hasset" } });
	CHECK(bad.isError);
	CHECK(bad.errorCode == "failed");

	// No tab: the tools already wrote the file, so there is nothing unsaved —
	// which is reported, not treated as an error.
	f.liveRel.clear();
	const ToolResult none = f.call("widget_save", json{ { "path", "UI/Menu.hasset" } });
	REQUIRE_FALSE(none.isError);
	CHECK(none.content["saved"] == false);
}

// ─── Refusals ────────────────────────────────────────────────────────────────

TEST_CASE("every mutating tool is refused while play-in-editor runs")
{
	Fixture f("play");
	f.writeWidget("UI/Menu.hasset");
	const int id = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                          { "type", "Panel" } }).content["id"];
	f.playing = true;

	const json base = json{ { "path", "UI/Menu.hasset" }, { "element", id } };
	for (const char* name : { "widget_add", "widget_remove", "widget_move",
	                          "widget_set_properties", "widget_set_anchor", "widget_save" })
	{
		json args = base;
		args["type"] = "Text";
		args["preset"] = 0;
		args["properties"] = json{ { "Tooltip", "x" } };
		const ToolResult r = f.call(name, args);
		CHECK_MESSAGE(r.isError, name);
		CHECK_MESSAGE(r.errorCode == "play_mode", name);
	}

	// The readers are not: looking at a widget while the game runs changes
	// nothing, and a client refused everything cannot even find out why.
	CHECK_FALSE(f.call("widget_tree", json{ { "path", "UI/Menu.hasset" } }).isError);
	CHECK_FALSE(f.call("widget_types", json::object()).isError);
	// And nothing landed.
	CHECK(f.fromDisk("UI/Menu.hasset").elements.size() == 1);
}

TEST_CASE("a widget a peer holds is refused under the gateway's own code")
{
	Fixture f("lock");
	f.writeWidget("UI/Menu.hasset");
	f.lockedRel = "UI/Menu.hasset";

	const ToolResult r = f.call("widget_add", json{ { "path", "UI/Menu.hasset" },
	                                                { "type", "Panel" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "locked_by_other");
	// Reading is still allowed — a lock is about writing.
	CHECK_FALSE(f.call("widget_tree", json{ { "path", "UI/Menu.hasset" } }).isError);
	CHECK(f.fromDisk("UI/Menu.hasset").elements.empty());
}

TEST_CASE("the reserved Engine namespace is read-only")
{
	Fixture f("engine");
	// A real file, so the refusal is about the NAMESPACE and not about the file
	// missing — those are two different mistakes and only one of them is this
	// one. With no engine root set, "Engine/…" resolves to the project's own
	// override location, which is under the content root and therefore passes
	// the confinement check on its way to this gate.
	f.writeWidget("Engine/UI/Default.hasset");

	const ToolResult r = f.call("widget_add", json{ { "path", "Engine/UI/Default.hasset" },
	                                                { "type", "Panel" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "read_only");
	// Reading a shipped default is fine — it is the writing that is refused.
	CHECK_FALSE(f.call("widget_tree", json{ { "path", "Engine/UI/Default.hasset" } }).isError);
	CHECK(f.fromDisk("Engine/UI/Default.hasset").elements.empty());
}
