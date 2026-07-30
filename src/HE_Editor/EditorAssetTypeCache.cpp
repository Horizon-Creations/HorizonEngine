#include "EditorAssetTypeCache.h"
#include <ContentManager/HAsset.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace EditorAssetTypeCache
{
namespace
{
	std::unordered_map<std::string, HE::AssetType> g_cache;

	// Reads ONLY the 32-byte HAsset file header. HAsset::Reader::open() would pull
	// every chunk of the file into memory — megabytes for a mesh — to answer a
	// question that lives in its first 8 bytes; since the cache is invalidated on
	// each content refresh, that sniff has to stay cheap enough to repeat.
	HE::AssetType sniffHeader(const std::string& path)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f.is_open()) return HE::AssetType::Unknown;
		HAsset::FileHeader hdr{};
		f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
		if (!f || std::memcmp(hdr.magic, HAsset::k_magic, 4) != 0)
			return HE::AssetType::Unknown;
		return static_cast<HE::AssetType>(hdr.asset_type);
	}
}

HE::AssetType assetTypeOf(const std::string& path)
{
	if (path.empty()) return HE::AssetType::Unknown;
	if (auto it = g_cache.find(path); it != g_cache.end()) return it->second;

	// Files that are not HAssets (C++ sources, the virtual "::LevelScript::" tab
	// path) are cached as Unknown too, so they cost one open instead of one per frame.
	const HE::AssetType type = sniffHeader(path);
	g_cache.emplace(path, type);
	return type;
}

bool is(const std::string& path, HE::AssetType type)
{
	return assetTypeOf(path) == type;
}

void invalidate(const std::string& path)
{
	g_cache.erase(path);
}

void invalidateAll()
{
	g_cache.clear();
}

} // namespace EditorAssetTypeCache
