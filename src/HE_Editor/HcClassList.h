#pragma once
#include <Types/Enums.h>
#include <string>
#include <vector>

class ContentManager;

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
}
