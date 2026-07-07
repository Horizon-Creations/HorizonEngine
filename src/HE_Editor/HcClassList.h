#pragma once
#include <Types/Enums.h>
#include <string>
#include <vector>

class ContentManager;
namespace HorizonCode { struct Graph; struct Node; }

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

	// Interface editor for a HorizonCode function: edit the FunctionEntry's typed
	// Inputs (params) and Outputs (results). On any change it re-syncs the matching
	// Call/Return nodes and prunes now-invalid links, then sets `edited`. Shared by
	// the level/GI/class graph editor and the widget graph editor.
	void drawFunctionInterface(HorizonCode::Graph& g, HorizonCode::Node& entry, bool& edited);

	// "Return from <fn>" picker for a FunctionReturn node's details — lists the
	// functions declared in the graph (those with a FunctionEntry). Sets the node's
	// owning function name + mirrors its result pins. Returns true if it changed.
	bool drawReturnFunctionPicker(HorizonCode::Graph& g, HorizonCode::Node& ret);
}
