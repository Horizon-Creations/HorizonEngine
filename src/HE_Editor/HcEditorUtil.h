#pragma once
#include <Types/Enums.h>
#include <cstdint>
#include <string>
#include <vector>

class ContentManager;
namespace HorizonCode { enum class PinType : std::uint8_t; enum class NodeType : std::uint8_t;
                        struct Graph; struct Node; struct Variable; }
namespace HE::api { struct ApiFn; }

// Small editor helper: enumerate the project's assets of a given type (for the
// asset/object picker dropdowns — HorizonCode classes, widgets, textures, …).
// Walks the content root and sniffs each .hasset header — cheap enough to call
// while a combo is open.
namespace HcEditorUtil
{
	struct ClassRef
	{
		std::string label; // display name (file stem)
		std::string path;  // content-relative path (what nodes store)
	};
	std::vector<ClassRef> listAssets(ContentManager* cm, HE::AssetType type);
	// Convenience wrapper for the Create Object class picker.
	std::vector<ClassRef> listHorizonCodeClasses(ContentManager* cm);
	// Every .hescene under the project root, as project-relative paths (e.g.
	// "Content/123.hescene") — the exact string scene.load expects. Feeds the
	// scene-path dropdown on scene.* Engine Call nodes.
	std::vector<ClassRef> listScenes(ContentManager* cm);

	// If `n` is an Engine Call with a String "scene"/"path" param (scene.load /
	// scene.loadAdditive), draw a dropdown of project scenes and write the pick to
	// that param's inline pin default — so the level to load is CHOSEN, not typed
	// (a bare "123.hescene" mismatches the packed/resolved project-relative path).
	// Returns true when changed; no-op (returns false, draws nothing) otherwise.
	bool drawSceneParamPicker(HorizonCode::Node& n, ContentManager* cm);

	// ── HC class registry ─────────────────────────────────────────────────────
	// The public interface of one HorizonCode class the editor knows about — an
	// asset (widget / HC class) or an in-memory graph (level / GameInstance). This
	// is what the drag-off menu and the object-type pickers read.
	struct MemberFn  { std::string name; std::vector<HorizonCode::PinType> paramTypes; bool hasResult = false; };
	struct MemberVar { std::string name; HorizonCode::PinType type; std::string className; };
	struct ClassInfo
	{
		std::string label;                 // display name
		std::string path;                  // content-relative asset path ("" for level/GI)
		enum Kind { Class, Widget, Level, GameInstance } kind = Class;
		std::vector<MemberFn>  functions;  // public FunctionEntry nodes
		std::vector<MemberVar> variables;  // public variables
	};
	// Reduce a graph to its public interface.
	ClassInfo classInfoFromGraph(const HorizonCode::Graph& g, const std::string& label,
	                             const std::string& path, ClassInfo::Kind kind);
	// Load an asset (HC class or widget) and extract its public interface. False if
	// the path resolves to no HorizonCode graph.
	bool classInfoForPath(ContentManager* cm, const std::string& path, ClassInfo& out);
	// Every HC class in the project (class + widget assets), plus the level and
	// GameInstance graphs when supplied.
	std::vector<ClassInfo> listClasses(ContentManager* cm,
	                                   const HorizonCode::Graph* levelGraph,
	                                   const HorizonCode::Graph* giGraph);

	// Searchable type dropdown: default value types (Float/Bool/Int/…) plus object
	// types (the project's HC classes). Writes `type` and, for an object type,
	// `className` (the class path); clears className for a default type. Shows the
	// class name for an object type instead of a bare "Object". Returns true when
	// changed. Pass className=nullptr where object types aren't allowed.
	bool drawTypePicker(const char* label, ContentManager* cm,
	                    HorizonCode::PinType& type, std::string* className);

	// Interface editor for a HorizonCode function: edit the FunctionEntry's typed
	// Inputs (params) and Outputs (results). On any change it re-syncs the matching
	// Call/Return nodes and prunes now-invalid links, then sets `edited`. Shared by
	// the level/GI/class graph editor and the widget graph editor.
	void drawFunctionInterface(HorizonCode::Graph& g, HorizonCode::Node& entry, bool& edited);

	// ── Literal node bodies (inline value editors on the node) ────────────────
	// Const/literal nodes show their value right on the node body: a checkbox for
	// Bool, a number field for Int/Float, two fields for Vec2, a swatch for Color,
	// and a multi-line entry for String that grows with the text up to a cap then
	// scrolls. Height is in graph units (px at zoom 1); 0 for non-literal nodes so
	// the node stays compact/body-less.
	float literalNodeBodyHeight(const HorizonCode::Node& n);
	// Draws the inline editor into the current body child. Returns true if the value
	// changed this frame; sets `committed` when the edit finished (undo snapshot).
	bool  drawLiteralNodeBody(HorizonCode::Node& n, bool& committed);

	// Draws the HE::api engine registry as an add-menu section: entries grouped by
	// category, filtered by `lowerQuery` (already lowercased; empty = show all).
	// Returns the picked registry id when a selectable is clicked this frame, else
	// "". The caller resolves it via HE::api::find and builds an EngineCall node
	// (copying the descriptor's isExec → hasArg and params/results onto the node).
	std::string drawEngineApiMenu(const std::string& lowerQuery);
	// Readable title for an EngineCall node ("Sine" for math.sin) — the registry's
	// displayName, falling back to the raw id.
	std::string engineCallTitle(const std::string& apiId);

	// Default-value slots for an ARRAY variable: add/remove/edit elements, each
	// with the element type's editor (drag-float, checkbox, color swatch, …).
	// Returns true when anything changed (the caller commits for undo).
	bool drawArrayDefaultEditor(HorizonCode::Variable& v);

	// Inline pin defaults: simple UNWIRED data inputs (Bool/Int/Float/String, no
	// arrays) show a small entry right on the node — no literal node needed.
	// `pinSupportsInlineDefault` gates per unified pin; the editor draws inside
	// the GraphEditor's positioned per-pin child. `committed` = snapshot time.
	bool pinSupportsInlineDefault(const HorizonCode::Node& n, int unifiedPin);
	void drawPinDefaultEditor(HorizonCode::Node& n, int unifiedPin, bool& committed);

	// ── Drag-off compatibility ────────────────────────────────────────────────
	// First unified pin index on a FRESH node of `t` (propType seeded with the
	// dragged type) that accepts the dragged pin, or -1. srcIsInput = the drag
	// started on an input pin (so the new node must OUTPUT into it); srcIsExec =
	// the dragged pin is an exec pin (data type is ignored then).
	int dragMatchPin(HorizonCode::NodeType t, HorizonCode::PinType dragType,
	                 bool dragArray, bool srcIsInput, bool srcIsExec);
	// Same for an HE::api registry entry (an EngineCall node built from it).
	int dragMatchApiPin(const HE::api::ApiFn& fn, HorizonCode::PinType dragType,
	                    bool dragArray, bool srcIsInput, bool srcIsExec);

	// "Return from <fn>" picker for a FunctionReturn node's details — lists the
	// functions declared in the graph (those with a FunctionEntry). Sets the node's
	// owning function name + mirrors its result pins. Returns true if it changed.
	bool drawReturnFunctionPicker(HorizonCode::Graph& g, HorizonCode::Node& ret);

	// ── Hover tooltips ────────────────────────────────────────────────────────
	// Full hover-tooltip text for a node instance: what it does (HC::nodeTooltip,
	// or the engine registry entry for an Engine Call), then its inputs and
	// outputs with their value types, derived from HC::signatureOf. Shared by the
	// level/class and widget graph editors so every node documents itself the
	// same way.
	std::string nodeTooltipText(const HorizonCode::Node& n);
	// Same, for a bare node type (add-menu items — no configured instance yet).
	std::string nodeTooltipText(HorizonCode::NodeType t);

	// ── Shared graph colors (ImU32; keep every HC editor consistent) ──────────
	// A stable color per value type — Bool always red, Float green, Ref purple, …
	std::uint32_t pinTypeColor(HorizonCode::PinType t);
	// A node's header/accent color: Events red, Functions purple, Branch/Sequence
	// gray, reference/object nodes purple, and data nodes (Get/Set/Const) colored
	// by their value type so a Bool getter is always red, a Float getter green, …
	std::uint32_t nodeHeaderColor(const HorizonCode::Node& n);
}
