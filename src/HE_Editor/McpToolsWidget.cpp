#include "McpToolRegistry.h"

#include "McpToolCommon.h"            // the argument readers and the ONE confinement rule

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <UIWidget/UIElement.h>
#include <UIWidget/UIWidgetTree.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

// ─── Authoring a UI widget from outside the editor ───────────────────────────
// The rationale for the shape of this file — why a widget needs tools of its
// own, and why an open Designer tab and a closed asset are two different places
// to write — is in McpToolRegistry.h beside McpWidgetHooks. What is worth
// stating HERE is what the handlers actually promise:
//
//   • EVERY PROPERTY NAME IS CHECKED AGAINST THE ELEMENT'S OWN TABLE.
//     `UIElement::setPropAny` writes an unknown name NOWHERE and says nothing
//     about it, which on this interface would mean a client believes it set
//     "Text" on a Panel and sees no reason to look again. So the name is looked
//     up in `allProperties()` first and a miss is a refusal carrying the list.
//     The same rule for a type name: `uiWidgetTypeFromName` answers Panel for
//     anything it does not know, so garbage would quietly become a Panel.
//
//   • VALUES ARE READ BY THE PROPERTY'S DECLARED TYPE, not by what the JSON
//     happens to look like. A Color is four numbers, a Vec2 is two, a Slot Fill
//     is one — and 5 is a number where the table says float, because the schema
//     is what a client reads (McpToolCommon's `numArg` rule).
//
//   • ADDING AN ELEMENT GOES THROUGH THE SAME TWO STEPS THE DESIGNER TAKES.
//     `UIWidgetTree::add` neither sets `parentId` nor asks whether the parent
//     takes children — only `moveElement` does — so the parent is validated
//     here, `parentId` is written before the add, and a requested place among
//     the siblings is a `moveElement` afterwards. The anchor follows
//     `addElementAt`: an explicit position anchors top-left, no position
//     anchors middle-centre, which is what a human's drop and a human's palette
//     click respectively do.
//
//   • NOTHING IS SAVED BEHIND A TAB'S BACK. With a Designer tab open the edit
//     lands in that tab (unsaved, dirty, undoable); `widget_save` writes it.
//     Without one there is nothing to be behind and the file is written at once,
//     because a client that cannot press Save would otherwise lose the edit when
//     the asset is unloaded.

namespace HE::Ed
{

using nlohmann::json;

namespace
{

// ── The sixteen anchor rectangles, named ─────────────────────────────────────
// The Designer draws this grid as pictures and never had to name a cell. A
// client cannot see pictures, so the names are invented HERE — and the rect
// travels with every one of them (`uiAnchorPresetRect`), so the name is a
// convenience and the numbers are the truth. index = row * 4 + col, col
// 0/1/2/3 = left/centre/right/stretch-across, row 0/1/2/3 = top/middle/bottom/
// stretch-down (UIWidgetTree.h).
std::string anchorPresetName(int preset)
{
	static const char* kRows[] = { "Top", "Middle", "Bottom", "Stretch" };
	static const char* kCols[] = { "Left", "Center", "Right", "Stretch" };
	if (preset < 0 || preset >= kUIAnchorPresetCount) return {};
	if (preset == kUIAnchorFill) return "Fill";
	return std::string(kRows[preset / 4]) + kCols[preset % 4];
}

const char* propTypeName(UIPropType t)
{
	switch (t)
	{
	case UIPropType::Float:      return "float";
	case UIPropType::Int:        return "int";
	case UIPropType::Bool:       return "bool";
	case UIPropType::String:     return "string";
	case UIPropType::Color:      return "color";
	case UIPropType::Vec2:       return "vec2";
	case UIPropType::StringList: return "stringList";
	}
	return "float";
}

json propValueToJson(const UIPropValue& v)
{
	switch (v.type)
	{
	case UIPropType::Float:      return v.f;
	case UIPropType::Int:        return v.i;
	case UIPropType::Bool:       return v.b;
	case UIPropType::String:     return v.s;
	case UIPropType::Color:      return json::array({ v.col.r, v.col.g, v.col.b, v.col.a });
	case UIPropType::Vec2:       return json::array({ v.v2.x, v.v2.y });
	case UIPropType::StringList: return json(v.list);
	}
	return json();
}

// Read a client's value AS the type the property declares. `why` is filled with
// the sentence the refusal carries — the shape that was expected, not just
// "wrong type", because the client cannot see the table.
bool propValueFromJson(const json& j, UIPropType want, const std::string& name,
                       UIPropValue& out, std::string& why)
{
	const auto numbers = [&](size_t least, size_t most, std::vector<double>& v) {
		if (!j.is_array() || j.size() < least || j.size() > most) return false;
		for (const json& e : j)
		{
			if (!e.is_number()) return false;
			v.push_back(e.get<double>());
		}
		return true;
	};

	switch (want)
	{
	case UIPropType::Float:
		if (!j.is_number()) break;
		out = UIPropValue::ofFloat(static_cast<float>(j.get<double>()));
		return true;
	case UIPropType::Int:
		if (!j.is_number()) break;
		out = UIPropValue::ofInt(static_cast<int>(j.get<double>()));
		return true;
	case UIPropType::Bool:
		if (!j.is_boolean()) break;
		out = UIPropValue::ofBool(j.get<bool>());
		return true;
	case UIPropType::String:
		if (!j.is_string()) break;
		out = UIPropValue::ofString(j.get<std::string>());
		return true;
	case UIPropType::Color:
	{
		std::vector<double> v;
		// Three is a colour without a stated alpha, which is opaque — the one
		// abbreviation worth taking, because "#rrggbb" is how colours are
		// usually said and an alpha of 0 would make the element invisible.
		if (!numbers(3, 4, v)) break;
		out = UIPropValue::ofColor({ (float)v[0], (float)v[1], (float)v[2],
		                             v.size() > 3 ? (float)v[3] : 1.0f });
		return true;
	}
	case UIPropType::Vec2:
	{
		std::vector<double> v;
		if (!numbers(2, 2, v)) break;
		out = UIPropValue::ofVec2({ (float)v[0], (float)v[1] });
		return true;
	}
	case UIPropType::StringList:
	{
		if (!j.is_array()) break;
		UIPropValue r;
		r.type = UIPropType::StringList;
		for (const json& e : j)
		{
			if (!e.is_string()) { why = "'" + name + "' takes an array of strings."; return false; }
			r.list.push_back(e.get<std::string>());
		}
		out = std::move(r);
		return true;
	}
	}

	static const char* kShapes[] = { "a number", "a whole number", "true or false",
	                                 "a string", "an array of 3 or 4 numbers (r,g,b[,a] "
	                                 "in 0..1)", "an array of 2 numbers", "an array of strings" };
	why = "'" + name + "' is a " + propTypeName(want) + " property: it takes " +
	      kShapes[static_cast<int>(want)] + ".";
	return false;
}

// ── The addressed widget ─────────────────────────────────────────────────────
// Two places a tree can live and one struct that hides which — see the header.
// `tree` points either at the Designer tab's own tree or at `local`, which is
// this call's parse of the asset's JSON, and `live` is what `commit` branches
// on.
struct Doc
{
	std::string   rel;
	std::string   abs;
	HE::UUID      assetId{};
	bool          live = false;
	UIWidgetTree  local;
	UIWidgetTree* tree = nullptr;
	bool          ok = false;
	ToolResult    failure = ToolResult::ok(json::object());
};

Doc openDoc(ContentManager& content, const McpWidgetHooks& h, const json& args, bool forWrite)
{
	Doc d;
	const PathCheck p = checkPath(content, strArg(args, "path"), /*mustExist=*/true, "path");
	if (!p.ok) { d.failure = p.failure; return d; }
	d.rel = p.rel;
	d.abs = p.abs;

	if (forWrite)
	{
		if (p.engine) { d.failure = failEngineReadOnly(p.rel); return d; }
		if (h.isPlaying && h.isPlaying())
		{
			d.failure = ToolResult::fail("play_mode",
				"Play-in-editor is running. A widget edited now is edited in the very "
				"asset the running session is drawing from, and the Designer's undo "
				"cannot reach it — so it is refused rather than half-applied. Ask the "
				"user to stop play mode.");
			return d;
		}
		if (h.lockedByOther && h.lockedByOther(p.rel))
		{
			d.failure = ToolResult::fail("locked_by_other",
				"Another participant in the collaboration session holds '" + p.rel +
				"' right now. Wait until they let go, or work on something else.");
			return d;
		}
	}

	// An open Designer tab owns the tree. Asking FIRST is the whole rule: a tree
	// loaded from the asset behind that tab would be a second copy, and the
	// human's next Save from the tab would write over everything done to it.
	if (h.liveTree)
		if (UIWidgetTree* t = h.liveTree(p.rel))
		{
			d.live = true;
			d.tree = t;
			d.ok = true;
			return d;
		}

	d.assetId = content.loadAsset(p.rel);
	const UIWidgetAsset* a = d.assetId == HE::UUID{} ? nullptr : content.getWidget(d.assetId);
	if (!a)
	{
		d.failure = ToolResult::fail("invalid_path",
			"'" + p.rel + "' is not a UI Widget asset. asset_resolve reports what a path "
			"holds; asset_create with type 'Widget' makes a new one.");
		return d;
	}
	// Copied out at once and never held: the asset pool is a dense vector and
	// anything that loads moves every asset in it, taking this string with it
	// (ContentManager.h). The pointer is dead from the next line on.
	const std::string treeJson = a->treeJson;
	// A stub is born with a serialised empty tree, but a file written by
	// something older may carry nothing at all — and parsing "" fails, which
	// would make a newborn widget uneditable rather than empty.
	if (!treeJson.empty() && !uiWidgetTreeFromJson(treeJson, d.local))
	{
		d.failure = ToolResult::fail("failed",
			"The widget tree in '" + p.rel + "' could not be parsed. Open the asset in "
			"the editor to see what the Designer makes of it — this tool will not write "
			"over a file it cannot read.");
		return d;
	}
	d.tree = &d.local;
	d.ok = true;
	return d;
}

// Finish a mutation. The live half is the panel's own commitEdit (undo snapshot,
// dirty mark, asset refresh); the disk half re-fetches the asset by UUID —
// never by a pointer taken before the edit — and writes the file.
ToolResult* commit(Doc& d, ContentManager& content, const McpWidgetHooks& h, ToolResult& scratch)
{
	if (d.live)
	{
		if (h.markEdited) h.markEdited(d.rel);
		return nullptr;
	}
	UIWidgetAsset* a = content.getWidgetMutable(d.assetId);
	if (!a)
	{
		scratch = ToolResult::fail("failed",
			"'" + d.rel + "' was unloaded while the edit was being applied — nothing was "
			"written. Retry.");
		return &scratch;
	}
	a->treeJson = uiWidgetTreeToJson(*d.tree);
	if (!content.saveAsset(*a))
	{
		scratch = ToolResult::fail("failed",
			"Could not write '" + d.rel + "'. A read-only file or a full disk is the "
			"usual cause; the editor log carries the reason.");
		return &scratch;
	}
	return nullptr;
}

// What every mutating result carries, so a client never has to ask a second tool
// where its change went: which of the two places was written, and whether that
// place now has something unsaved in it.
json docState(const Doc& d, const McpWidgetHooks& h)
{
	json j = json::object();
	j["path"]   = d.rel;
	j["target"] = d.live ? "editor" : "disk";
	j["dirty"]  = d.live ? (h.isDirty ? h.isDirty(d.rel) : true) : false;
	return j;
}

// The element the tools address, refused with the ids that DO exist rather than
// with a bare "not found" — a client whose numbering drifted has no other way
// to recover.
UIElement* elementArg(const Doc& d, const json& args, const char* key, ToolResult& failure)
{
	if (!hasArg(args, key))
	{
		failure = ToolResult::fail("invalid_payload",
			std::string("'") + key + "' is required and must be the id of an element in "
			"this widget — widget_tree lists them.");
		return nullptr;
	}
	const int id = intArg(args, key, 0);
	if (UIElement* e = d.tree->find(id)) return e;

	std::string ids;
	for (const auto& e : d.tree->elements)
		ids += (ids.empty() ? "" : ", ") + std::to_string(e->id);
	failure = ToolResult::fail("not_found",
		"No element with id " + std::to_string(id) + " in '" + d.rel + "'. " +
		(ids.empty() ? "This widget is empty." : "It holds: " + ids + "."));
	return nullptr;
}

// One element, as the readers report it. Cheap fields only — the property table
// is the expensive half and only widget_tree's single-element form asks for it.
json elementSummary(const UIWidgetTree& tree, const UIElement& e)
{
	json j = json::object();
	j["id"]       = e.id;
	j["name"]     = e.name;
	j["type"]     = e.typeName();
	j["parent"]   = e.parentId;
	j["visible"]  = e.visible;
	j["enabled"]  = e.enabled;
	j["position"] = json::array({ e.posX, e.posY });
	j["size"]     = json::array({ e.sizeX, e.sizeY });
	const int preset = uiAnchorPresetOf(e);
	j["anchorPreset"] = preset;
	if (preset >= 0) j["anchorPresetName"] = anchorPresetName(preset);
	j["anchor"] = json::array({ e.anchorMinX, e.anchorMinY, e.anchorMaxX, e.anchorMaxY });
	if (e.acceptsChildren()) j["acceptsChildren"] = true;
	const std::vector<int> kids = tree.childrenOf(e.id);
	if (!kids.empty()) j["children"] = kids;
	return j;
}

json propertyCatalog(const UIElement& e)
{
	json arr = json::array();
	for (const UIPropDesc& d : e.allProperties())
	{
		json p = json::object();
		p["name"] = d.name;
		p["type"] = propTypeName(d.type);
		if (d.minV < d.maxV) { p["min"] = d.minV; p["max"] = d.maxV; }
		if (d.multiline) p["multiline"] = true;
		arr.push_back(std::move(p));
	}
	return arr;
}

// ── Which of these properties will NOT be in the file ────────────────────────
// A handful of properties are runtime state that the widget format deliberately
// does not carry — a List View's "Item Count" above all, because "an application
// that reopens with the last run's row count would be showing rows for data it
// has not loaded yet" (UIElements.h). Writing one through this interface
// succeeds, reads back as the number that was asked for, and is gone the next
// time the asset is loaded.
//
// A client cannot see that and has no way to find it out. So it is asked here,
// by the only method that cannot go out of step with the writer: serialise the
// tree, read it back, and compare. Anything that did not survive its own round
// trip is named in the result rather than left to be discovered later.
std::vector<std::string> notPersisted(const UIWidgetTree& tree, int id,
                                      const std::vector<std::string>& names)
{
	std::vector<std::string> lost;
	const UIElement* before = tree.find(id);
	if (!before) return lost;

	UIWidgetTree round;
	if (!uiWidgetTreeFromJson(uiWidgetTreeToJson(tree), round)) return lost;
	const UIElement* after = round.find(id);
	if (!after) return lost;

	for (const std::string& n : names)
		if (propValueToJson(before->getPropAny(n)) != propValueToJson(after->getPropAny(n)))
			lost.push_back(n);
	return lost;
}

json pathProp()
{
	return stringProp("Content-relative path of the UI Widget asset, e.g. "
	                  "'UI/MainMenu.hasset'. asset_list with type 'Widget' names them.");
}

json elementProp(const char* what)
{
	return json{ { "type", "integer" }, { "description", what } };
}

} // namespace

// ─── The tools ───────────────────────────────────────────────────────────────

void registerWidgetTools(McpToolRegistry& registry, ContentManager& content,
                         McpWidgetHooks hooks)
{
	ContentManager* cm = &content;
	auto h = std::make_shared<McpWidgetHooks>(std::move(hooks));

	// ── widget_tree ──────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "widget_tree";
		t.description =
			"Read a UI Widget asset's element tree: the canvas it was authored on and "
			"every element in it with its id, type, parent, children, anchor and rect. "
			"Elements come back in TREE ORDER, which is also sibling order — the order a "
			"Vertical Box stacks its children in and the order overlapping elements are "
			"drawn in. Pass 'element' to get one element's full property list (names and "
			"current values) instead, which is what widget_set_properties takes.";
		t.inputSchema = objectSchema(json{
			{ "path",    pathProp() },
			{ "element", elementProp("Report this one element in full — every property "
			                         "it has, with its value. Omit for the whole tree.") },
		}, { "path" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			Doc d = openDoc(*cm, *h, args, /*forWrite=*/false);
			if (!d.ok) return d.failure;

			json out = json::object();
			out["path"]   = d.rel;
			out["source"] = d.live ? "editor" : "asset";
			if (d.live) out["dirty"] = h->isDirty ? h->isDirty(d.rel) : true;
			out["canvas"] = json{ { "width", d.tree->canvasWidth },
			                      { "height", d.tree->canvasHeight },
			                      { "scaleMode", static_cast<int>(d.tree->scaleMode) } };
			if (!d.tree->description.empty()) out["description"] = d.tree->description;
			if (!d.tree->themeAsset.empty())  out["theme"] = d.tree->themeAsset;

			if (hasArg(args, "element"))
			{
				ToolResult failure;
				UIElement* e = elementArg(d, args, "element", failure);
				if (!e) return failure;
				json je = elementSummary(*d.tree, *e);
				je["hasTextureSlot"]  = e->hasTextureSlot();
				je["hasMaterialSlot"] = e->hasMaterialSlot();
				json props = json::object();
				for (const UIPropDesc& pd : e->allProperties())
					props[pd.name] = propValueToJson(e->getPropAny(pd.name));
				je["properties"] = std::move(props);
				je["propertyTypes"] = propertyCatalog(*e);
				out["element"] = std::move(je);
				return ToolResult::ok(std::move(out));
			}

			json arr = json::array();
			for (const auto& e : d.tree->elements) arr.push_back(elementSummary(*d.tree, *e));
			out["elements"] = std::move(arr);
			// The roots, separately: "children of 0" is not something the array
			// above spells out, and a client building a page starts there.
			out["roots"] = d.tree->childrenOf(0);
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── widget_types ─────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "widget_types";
		t.description =
			"Every element type a widget can be built from, with the properties each one "
			"has. This is what makes widget_add and widget_set_properties callable "
			"without guessing: property names are an on-disk format ('Slot Fill', 'Back "
			"Color', 'Text'), they differ per type, and a name that is not in this list "
			"is refused. Also carries the sixteen anchor presets widget_set_anchor takes.";
		t.inputSchema = objectSchema(json{
			{ "type", stringProp("Report only this type (e.g. 'Button'). Omit for all "
			                     "of them — that answer is long.") },
		}, {});
		t.handler = [](const json& args) -> ToolResult {
			const std::string only = strArg(args, "type");
			json types = json::array();
			bool matched = false;
			for (UIWidgetType wt : uiWidgetTypeRegistry())
			{
				const char* name = uiWidgetTypeName(wt);
				if (!only.empty() && only != name) continue;
				matched = true;
				std::unique_ptr<UIElement> e = makeUIElement(wt);
				if (!e) continue;
				json j = json::object();
				j["name"]            = name;
				j["acceptsChildren"] = e->acceptsChildren();
				j["hasTextureSlot"]  = e->hasTextureSlot();
				j["hasMaterialSlot"] = e->hasMaterialSlot();
				j["properties"]      = propertyCatalog(*e);
				types.push_back(std::move(j));
			}
			if (!only.empty() && !matched)
			{
				std::string all;
				for (UIWidgetType wt : uiWidgetTypeRegistry())
					all += (all.empty() ? "" : ", ") + std::string(uiWidgetTypeName(wt));
				return ToolResult::fail("not_found",
					"'" + only + "' is not a widget element type. The types are: " + all + ".");
			}

			json presets = json::array();
			for (int i = 0; i < kUIAnchorPresetCount; ++i)
			{
				float x0, y0, x1, y1;
				uiAnchorPresetRect(i, x0, y0, x1, y1);
				presets.push_back(json{ { "preset", i },
				                        { "name", anchorPresetName(i) },
				                        { "rect", json::array({ x0, y0, x1, y1 }) } });
			}
			return ToolResult::ok(json{ { "types", std::move(types) },
			                            { "anchorPresets", std::move(presets) } });
		};
		registry.add(std::move(t));
	}

	// ── widget_add ───────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "widget_add";
		t.description =
			"Add an element to a widget. Without 'parent' it becomes a root of the "
			"canvas; with one it becomes a child of that element, which has to be a type "
			"that takes children (widget_types says which do). Inside a layout container "
			"— a Vertical Box, a Grid, a Tab Box — the box decides where the child sits "
			"and 'position' is ignored, so use 'before' to say WHICH child it is.";
		t.mutates     = true;
		t.inputSchema = objectSchema(json{
			{ "path",   pathProp() },
			{ "type",   stringProp("Element type, e.g. 'Button', 'Text', 'VerticalBox'. "
			                       "widget_types lists them all.") },
			{ "parent", elementProp("Id of the element to add this one under. Omit or 0 "
			                        "for a root of the canvas.") },
			{ "before", elementProp("Land in FRONT of this sibling. Omit for the end. A "
			                        "sibling order is what a box stacks by and what "
			                        "decides which of two overlapping elements is on "
			                        "top.") },
			{ "name",   stringProp("Name shown in the hierarchy. Omit for the type's "
			                       "name. A Tab Box's pages and an Accordion's sections "
			                       "take their LABEL from this.") },
			{ "position", json{ { "type", "array" },
			                    { "description", "[x, y] in canvas units, measured from "
			                                     "the parent's top-left corner; the "
			                                     "element is anchored top-left. Omit to "
			                                     "place it in the middle of its parent, "
			                                     "anchored middle-centre." },
			                    { "items", json{ { "type", "number" } } } } },
			{ "size",     json{ { "type", "array" },
			                    { "description", "[width, height] in canvas units. Omit "
			                                     "for the type's own default." },
			                    { "items", json{ { "type", "number" } } } } },
			{ "properties", json{ { "type", "object" },
			                      { "description", "Properties to set on the new element, "
			                                       "by name — the same map "
			                                       "widget_set_properties takes." } } },
		}, { "path", "type" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			Doc d = openDoc(*cm, *h, args, /*forWrite=*/true);
			if (!d.ok) return d.failure;

			// `uiWidgetTypeFromName` answers Panel for anything it does not
			// know, so an unchecked name would quietly become a Panel.
			const std::string typeName = strArg(args, "type");
			bool known = false;
			std::string all;
			for (UIWidgetType wt : uiWidgetTypeRegistry())
			{
				const char* n = uiWidgetTypeName(wt);
				if (typeName == n) known = true;
				all += (all.empty() ? "" : ", ") + std::string(n);
			}
			if (!known)
				return ToolResult::fail("invalid_payload",
					"'" + typeName + "' is not a widget element type. The types are: " +
					all + ". widget_types describes what each of them can do.");

			const int parentId = intArg(args, "parent", 0);
			if (parentId != 0)
			{
				const UIElement* p = d.tree->find(parentId);
				if (!p)
				{
					ToolResult failure;
					elementArg(d, args, "parent", failure);
					return failure;
				}
				if (!p->acceptsChildren())
					return ToolResult::fail("invalid_payload",
						"Element " + std::to_string(parentId) + " is a " + p->typeName() +
						", which takes no children. Panels, boxes, grids, tab boxes and "
						"splitters do — widget_types says which.");
			}

			std::unique_ptr<UIElement> e = makeUIElement(uiWidgetTypeFromName(typeName));
			if (!e)
				return ToolResult::fail("failed",
					"The engine could not create a '" + typeName + "'.");
			e->parentId = parentId;
			const std::string name = strArg(args, "name");
			e->name = name.empty() ? std::string(e->typeName()) : name;

			// The Designer's own two placements (UIEditorPanel::addElementAt): a
			// drop point anchors top-left, a palette click anchors middle-centre
			// in the middle of its parent.
			if (args.is_object() && args.contains("position") && args["position"].is_array() &&
			    args["position"].size() == 2 && args["position"][0].is_number() &&
			    args["position"][1].is_number())
			{
				uiSetAnchorPreset(*e, 0);
				e->posX = args["position"][0].get<float>();
				e->posY = args["position"][1].get<float>();
			}
			else
			{
				uiSetAnchorPreset(*e, 5);
				e->posX = 0.0f;
				e->posY = 0.0f;
			}
			if (args.is_object() && args.contains("size") && args["size"].is_array() &&
			    args["size"].size() == 2 && args["size"][0].is_number() &&
			    args["size"][1].is_number())
			{
				e->sizeX = args["size"][0].get<float>();
				e->sizeY = args["size"][1].get<float>();
			}

			// Every property is validated BEFORE the element joins the tree, so
			// a refusal leaves the widget exactly as it was rather than half a
			// button in it.
			std::vector<std::pair<std::string, UIPropValue>> pending;
			if (args.is_object() && args.contains("properties"))
			{
				const json& props = args["properties"];
				if (!props.is_object())
					return ToolResult::fail("invalid_payload",
						"'properties' is an object of property name to value.");
				const std::vector<UIPropDesc> table = e->allProperties();
				for (auto it = props.begin(); it != props.end(); ++it)
				{
					const auto row = std::find_if(table.begin(), table.end(),
						[&](const UIPropDesc& pd) { return pd.name == it.key(); });
					if (row == table.end())
					{
						std::string names;
						for (const UIPropDesc& pd : table)
							names += (names.empty() ? "" : ", ") + pd.name;
						return ToolResult::fail("invalid_payload",
							"A " + std::string(e->typeName()) + " has no property '" +
							it.key() + "'. It has: " + names + ".");
					}
					UIPropValue v;
					std::string why;
					if (!propValueFromJson(it.value(), row->type, it.key(), v, why))
						return ToolResult::fail("invalid_payload", why);
					pending.emplace_back(it.key(), std::move(v));
				}
			}
			for (const auto& [n, v] : pending) e->setPropAny(n, v);

			const int newId = d.tree->add(std::move(e));
			// Only when a place was asked for: `add` appends, which is the end,
			// and moving to the end again is a no-op that would still be a
			// second walk over the vector.
			const int before = intArg(args, "before", 0);
			if (before != 0) d.tree->moveElement(newId, parentId, before);

			std::vector<std::string> names;
			for (const auto& [n, v] : pending) names.push_back(n);
			const std::vector<std::string> lost = notPersisted(*d.tree, newId, names);

			ToolResult scratch;
			if (ToolResult* r = commit(d, *cm, *h, scratch)) return *r;

			json out = docState(d, *h);
			out["id"]      = newId;
			out["element"] = elementSummary(*d.tree, *d.tree->find(newId));
			if (!lost.empty()) out["notPersisted"] = lost;
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── widget_remove ────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "widget_remove";
		t.description =
			"Remove an element and everything under it. The whole subtree goes — a "
			"Vertical Box takes its rows with it — and the ids that went are reported, "
			"so a client holding one of them knows it is stale.";
		t.mutates     = true;
		t.inputSchema = objectSchema(json{
			{ "path",    pathProp() },
			{ "element", elementProp("Id of the element to remove.") },
		}, { "path", "element" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			Doc d = openDoc(*cm, *h, args, /*forWrite=*/true);
			if (!d.ok) return d.failure;

			ToolResult failure;
			UIElement* e = elementArg(d, args, "element", failure);
			if (!e) return failure;

			// Collected before the removal, the same walk removeSubtree does:
			// afterwards there is nothing left to ask.
			std::vector<int> gone{ e->id };
			for (size_t i = 0; i < gone.size(); ++i)
				for (int c : d.tree->childrenOf(gone[i])) gone.push_back(c);

			d.tree->removeSubtree(e->id);

			ToolResult scratch;
			if (ToolResult* r = commit(d, *cm, *h, scratch)) return *r;

			json out = docState(d, *h);
			out["removed"] = gone;
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── widget_move ──────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "widget_move";
		t.description =
			"Move an element to another parent, to another place among its siblings, or "
			"both. Sibling order is what a layout box stacks by, which page a Tab Box "
			"calls its third and which of two overlapping elements covers the other — so "
			"'before' is as much of a move as 'parent' is. Refused, with the widget "
			"untouched, when the move cannot be: into itself or its own descendant, or "
			"into an element that takes no children.";
		t.mutates     = true;
		t.inputSchema = objectSchema(json{
			{ "path",    pathProp() },
			{ "element", elementProp("Id of the element to move.") },
			{ "parent",  elementProp("New parent, 0 for the canvas. Omit to keep the "
			                         "parent it has and only reorder.") },
			{ "before",  elementProp("Land in FRONT of this sibling. Omit or 0 for the "
			                         "end of the new parent's children.") },
		}, { "path", "element" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			Doc d = openDoc(*cm, *h, args, /*forWrite=*/true);
			if (!d.ok) return d.failure;

			ToolResult failure;
			UIElement* e = elementArg(d, args, "element", failure);
			if (!e) return failure;
			const int id = e->id;

			// Absent is not 0 here: 0 is the canvas, and reading "keep the
			// parent" as "make it a root" is a move nobody asked for.
			const int newParent = hasArg(args, "parent") ? intArg(args, "parent", 0)
			                                             : e->parentId;
			if (newParent != 0 && !d.tree->find(newParent))
				return ToolResult::fail("not_found",
					"No element with id " + std::to_string(newParent) + " in '" + d.rel +
					"' to move " + std::to_string(id) + " into. widget_tree lists them.");

			if (!d.tree->canMoveElement(id, newParent))
			{
				const UIElement* p = newParent == 0 ? nullptr : d.tree->find(newParent);
				if (p && !p->acceptsChildren())
					return ToolResult::fail("invalid_payload",
						"Element " + std::to_string(newParent) + " is a " + p->typeName() +
						", which takes no children.");
				return ToolResult::fail("invalid_payload",
					"Element " + std::to_string(id) + " cannot move into " +
					std::to_string(newParent) + ": that is itself or something inside it, "
					"and a tree cannot contain itself.");
			}
			if (!d.tree->moveElement(id, newParent, intArg(args, "before", 0)))
				return ToolResult::fail("failed",
					"The move of element " + std::to_string(id) + " was refused by the "
					"widget tree.");

			ToolResult scratch;
			if (ToolResult* r = commit(d, *cm, *h, scratch)) return *r;

			json out = docState(d, *h);
			out["element"]  = elementSummary(*d.tree, *d.tree->find(id));
			out["siblings"] = d.tree->childrenOf(newParent);
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── widget_set_properties ────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "widget_set_properties";
		t.description =
			"Set properties on one element by name — a Text's 'Text' and 'FontSize', a "
			"Button's 'Normal Color', an Image's 'Texture', a List View's 'Item Count', "
			"and the ones every element has ('Position', 'Size', 'Visible', 'Slot Fill', "
			"'Tooltip'). Names and types come from widget_types, or from widget_tree "
			"with 'element'. A name that element does not have is refused with the list "
			"of the ones it does — nothing is written until every value has been read, "
			"so a refusal leaves the widget exactly as it was.";
		t.mutates     = true;
		t.inputSchema = objectSchema(json{
			{ "path",       pathProp() },
			{ "element",    elementProp("Id of the element to change.") },
			{ "properties", json{ { "type", "object" },
			                      { "description", "Property name to value. A float takes "
			                                       "a number, a color four numbers "
			                                       "(r,g,b,a in 0..1) or three for opaque, "
			                                       "a vec2 two, a stringList an array of "
			                                       "strings." } } },
		}, { "path", "element", "properties" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			Doc d = openDoc(*cm, *h, args, /*forWrite=*/true);
			if (!d.ok) return d.failure;

			ToolResult failure;
			UIElement* e = elementArg(d, args, "element", failure);
			if (!e) return failure;

			if (!args.is_object() || !args.contains("properties") ||
			    !args["properties"].is_object())
				return ToolResult::fail("invalid_payload",
					"'properties' is required and is an object of property name to value. "
					"widget_tree with 'element' reports the names this element has.");
			const json& props = args["properties"];
			if (props.empty())
				return ToolResult::fail("invalid_payload",
					"'properties' is empty, so this call would change nothing.");

			// Read everything first, write nothing yet: half a refused call is
			// the one failure a client cannot recover from, because it does not
			// know which half landed.
			const std::vector<UIPropDesc> table = e->allProperties();
			std::vector<std::pair<std::string, UIPropValue>> pending;
			for (auto it = props.begin(); it != props.end(); ++it)
			{
				const auto row = std::find_if(table.begin(), table.end(),
					[&](const UIPropDesc& pd) { return pd.name == it.key(); });
				if (row == table.end())
				{
					std::string names;
					for (const UIPropDesc& pd : table)
						names += (names.empty() ? "" : ", ") + pd.name;
					return ToolResult::fail("invalid_payload",
						"A " + std::string(e->typeName()) + " has no property '" + it.key() +
						"'. It has: " + names + ".");
				}
				UIPropValue v;
				std::string why;
				if (!propValueFromJson(it.value(), row->type, it.key(), v, why))
					return ToolResult::fail("invalid_payload", why);
				pending.emplace_back(it.key(), std::move(v));
			}

			json written = json::object();
			std::vector<std::string> names;
			for (const auto& [n, v] : pending)
			{
				e->setPropAny(n, v);
				// Read BACK rather than echoed: a property that clamps or
				// rounds says so here instead of leaving a client believing a
				// number that is not in the widget.
				written[n] = propValueToJson(e->getPropAny(n));
				names.push_back(n);
			}
			const std::vector<std::string> lost = notPersisted(*d.tree, e->id, names);
			const int id = e->id;

			ToolResult scratch;
			if (ToolResult* r = commit(d, *cm, *h, scratch)) return *r;

			json out = docState(d, *h);
			out["element"] = id;
			out["set"]     = std::move(written);
			if (!lost.empty())
			{
				out["notPersisted"] = lost;
				out["notPersistedNote"] =
					"These are runtime state, not authored values: they are set on the "
					"widget now and the file does not carry them, so they are back to "
					"their defaults the next time it is loaded. A List View's row count "
					"is the usual one — the owner sets it when the data arrives.";
			}
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── widget_set_anchor ────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "widget_set_anchor";
		t.description =
			"Anchor an element to a part of its parent: one of the sixteen presets "
			"widget_types lists (0 = top-left point, 15 = fill the parent, 3 = the whole "
			"top side, 12 = the whole left side). The anchor is what decides where the "
			"element goes when its parent resizes, which is the whole reason a UI "
			"survives a different screen. By default the element STAYS where it is and "
			"only its behaviour on resize changes, which is what the Designer's anchor "
			"grid does; keepRect=false re-anchors the way UMG does without the modifier "
			"and lets the element move.";
		t.mutates     = true;
		t.inputSchema = objectSchema(json{
			{ "path",    pathProp() },
			{ "element", elementProp("Id of the element to anchor.") },
			{ "preset",  json{ { "type", "integer" },
			                   { "description", "0..15: row * 4 + column, where column "
			                                    "0/1/2/3 is left/centre/right/stretch-"
			                                    "across and row 0/1/2/3 is top/middle/"
			                                    "bottom/stretch-down." } } },
			{ "keepRect", json{ { "type", "boolean" },
			                    { "description", "Keep the element exactly where it is "
			                                     "(default true). False lets the rect "
			                                     "move to the new anchor." } } },
		}, { "path", "element", "preset" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			Doc d = openDoc(*cm, *h, args, /*forWrite=*/true);
			if (!d.ok) return d.failure;

			ToolResult failure;
			UIElement* e = elementArg(d, args, "element", failure);
			if (!e) return failure;

			if (!hasArg(args, "preset"))
				return ToolResult::fail("invalid_payload",
					"'preset' is required — 0..15. widget_types reports all sixteen with "
					"the rectangle each one names.");
			const int preset = intArg(args, "preset", -1);
			if (preset < 0 || preset >= kUIAnchorPresetCount)
				return ToolResult::fail("invalid_payload",
					"'preset' is " + std::to_string(preset) + "; it has to be 0..15. "
					"widget_types reports all sixteen with the rectangle each one names.");

			if (boolArg(args, "keepRect", true)) uiReanchorKeepingRect(*d.tree, *e, preset);
			else                                 uiSetAnchorPreset(*e, preset);

			ToolResult scratch;
			if (ToolResult* r = commit(d, *cm, *h, scratch)) return *r;

			json out = docState(d, *h);
			out["element"] = elementSummary(*d.tree, *e);
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── widget_save ──────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "widget_save";
		t.description =
			"Write a widget the editor has open to disk — the Designer tab's Save "
			"button. Only needed when the widget IS open in the editor: the other tools "
			"report target='editor' in that case and leave the tab dirty, exactly as a "
			"human's edit does. With no tab open they write the file themselves and this "
			"call has nothing left to do, which it reports rather than treating as an "
			"error.";
		t.mutates     = true;
		t.inputSchema = objectSchema(json{ { "path", pathProp() } }, { "path" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			Doc d = openDoc(*cm, *h, args, /*forWrite=*/true);
			if (!d.ok) return d.failure;

			json out = json::object();
			out["path"] = d.rel;
			if (!d.live)
			{
				// Not an error: the widget is not open anywhere, so whatever was
				// done to it is already in the file.
				out["saved"]   = false;
				out["dirty"]   = false;
				out["message"] = "'" + d.rel + "' is not open in the editor, so there is "
				                 "nothing unsaved to write — the widget tools wrote the "
				                 "file directly.";
				return ToolResult::ok(std::move(out));
			}
			if (!h->save || !h->save(d.rel))
				return ToolResult::fail("failed",
					"The editor could not write '" + d.rel + "'. A read-only file or a "
					"full disk is the usual cause; the editor log carries the reason.");
			out["saved"] = true;
			out["dirty"] = h->isDirty ? h->isDirty(d.rel) : false;
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}
}

} // namespace HE::Ed
