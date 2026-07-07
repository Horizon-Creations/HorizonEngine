#include "HcClassList.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include <Types/Enums.h>
#include <filesystem>

namespace HcEditorUtil
{
std::vector<ClassRef> listAssets(ContentManager* cm, HE::AssetType type)
{
	std::vector<ClassRef> out;
	if (!cm) return out;
	const std::string root = cm->contentRoot();
	if (root.empty()) return out;

	std::error_code ec;
	std::filesystem::recursive_directory_iterator it(root, ec), end;
	for (; it != end; it.increment(ec))
	{
		if (ec) break;
		if (!it->is_regular_file(ec)) continue;
		if (it->path().extension() != ".hasset") continue;
		HAsset::Reader r;
		if (r.open(it->path().string()) &&
		    r.assetType() == static_cast<uint16_t>(type))
		{
			ClassRef cr;
			cr.label = it->path().stem().string();
			cr.path  = std::filesystem::relative(it->path(), root, ec).generic_string();
			out.push_back(std::move(cr));
		}
	}
	return out;
}

std::vector<ClassRef> listHorizonCodeClasses(ContentManager* cm)
{ return listAssets(cm, HE::AssetType::HorizonCodeClass); }
}
