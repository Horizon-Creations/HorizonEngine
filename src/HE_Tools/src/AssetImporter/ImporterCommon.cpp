#include "ImporterCommon.h"
#include "ContentManager/ContentManager.h"
#include "ContentManager/HAsset.h"
#include "Diagnostics/Logger.h"

namespace Importer
{

std::string toAssetPath(const std::filesystem::path& relativePath)
{
	std::string s = relativePath.generic_string();
	// Strip a leading "./" that std::filesystem::relative sometimes produces
	if (s.rfind("./", 0) == 0)
		s.erase(0, 2);
	return s;
}

static HE::UUID existingUUID(const std::filesystem::path& file)
{
	HAsset::Reader reader;
	if (!reader.open(file.string()))
		return HE::UUID{};

	const auto* meta = reader.findChunk(HAsset::CHUNK_META);
	if (!meta || reader.header().version < 2)
		return HE::UUID{};

	HE::UUID id;
	size_t   off = sizeof(uint16_t); // skip asset type
	if (!HAsset::Reader::readPOD(meta->data, off, id.hi)) return HE::UUID{};
	if (!HAsset::Reader::readPOD(meta->data, off, id.lo)) return HE::UUID{};
	return id;
}

bool writeAsset(RuntimeAsset& asset, const std::filesystem::path& contentRoot)
{
	const std::filesystem::path target = contentRoot / asset.path;

	std::error_code ec;
	std::filesystem::create_directories(target.parent_path(), ec);

	// Re-import: keep the identity the asset already has on disk.
	if (asset.id == HE::UUID{})
		asset.id = existingUUID(target);

	ContentManager cm(contentRoot.string());
	if (!cm.saveAsset(asset))
	{
		Logger::Log(Logger::LogLevel::Error,
			("Importer: failed to write " + target.string()).c_str());
		return false;
	}
	return true;
}

} // namespace Importer
