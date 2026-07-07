#pragma once
#include <string>
#include <vector>

class ContentManager;

// Small editor helper: enumerate the project's HorizonCode class assets (for the
// Create Object node's class picker). Walks the content root and sniffs each
// .hasset header — cheap enough to call while a combo is open.
namespace HcEditorUtil
{
	struct ClassRef
	{
		std::string label; // display name (file stem)
		std::string path;  // content-relative path (what CreateObject stores)
	};
	std::vector<ClassRef> listHorizonCodeClasses(ContentManager* cm);
}
