#pragma once
#include <Types/Enums.h>
#include <cstdint>
#include <string>
#include <vector>

class ContentManager;
namespace HorizonCode { enum class PinType : std::uint8_t; struct Graph; struct Node; }

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

	// Draws the HE::api engine registry as an add-menu section: entries grouped by
	// category, filtered by `lowerQuery` (already lowercased; empty = show all).
	// Returns the picked registry id when a selectable is clicked this frame, else
	// "". The caller resolves it via HE::api::find and builds an EngineCall node
	// (copying the descriptor's isExec → hasArg and params/results onto the node).
	std::string drawEngineApiMenu(const std::string& lowerQuery);

	// "Return from <fn>" picker for a FunctionReturn node's details — lists the
	// functions declared in the graph (those with a FunctionEntry). Sets the node's
	// owning function name + mirrors its result pins. Returns true if it changed.
	bool drawReturnFunctionPicker(HorizonCode::Graph& g, HorizonCode::Node& ret);

	// ── Shared graph colors (ImU32; keep every HC editor consistent) ──────────
	// A stable color per value type — Bool always red, Float green, Ref purple, …
	std::uint32_t pinTypeColor(HorizonCode::PinType t);
	// A node's header/accent color: Events red, Functions purple, Branch/Sequence
	// gray, reference/object nodes purple, and data nodes (Get/Set/Const) colored
	// by their value type so a Bool getter is always red, a Float getter green, …
	std::uint32_t nodeHeaderColor(const HorizonCode::Node& n);
}
