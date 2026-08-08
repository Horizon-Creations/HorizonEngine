#include "ContentSync/EngineContentManifest.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace HE::Cs {

const EngineContentManifestEntry* EngineContentManifest::findByUuid(HE::UUID id) const
{
	for (const auto& e : entries)
		if (e.uuid == id) return &e;
	return nullptr;
}

const EngineContentManifestEntry* EngineContentManifest::findByPath(const std::string& relativePath) const
{
	for (const auto& e : entries)
		if (e.relativePath == relativePath) return &e;
	return nullptr;
}

std::string serializeManifest(const EngineContentManifest& manifest)
{
	json arr = json::array();
	for (const auto& e : manifest.entries)
	{
		arr.push_back({
			{ "path", e.relativePath },
			{ "uuid", { { "hi", e.uuid.hi }, { "lo", e.uuid.lo } } },
			{ "hash", e.contentHash },
			{ "size", e.size },
			{ "mtime", e.mtime },
		});
	}
	json root;
	root["entries"] = std::move(arr);
	return root.dump(2);
}

bool parseManifest(const std::string& jsonText, EngineContentManifest& out)
{
	json root;
	try
	{
		root = json::parse(jsonText);
	}
	catch (const json::parse_error&)
	{
		return false;
	}

	if (!root.contains("entries") || !root["entries"].is_array()) return false;

	EngineContentManifest result;
	for (const auto& e : root["entries"])
	{
		if (!e.contains("path")) continue;

		EngineContentManifestEntry entry;
		entry.relativePath = e.value("path", "");
		entry.contentHash  = e.value("hash", std::uint64_t{ 0 });
		entry.size         = e.value("size", std::uint64_t{ 0 });
		entry.mtime        = e.value("mtime", std::uint64_t{ 0 });

		// "uuid" is absent for a raw/loose entry (see the field comment in the
		// header) — defaults to {0,0}, same as HE::UUID's own default.
		if (e.contains("uuid"))
		{
			const auto& u = e["uuid"];
			entry.uuid.hi = u.value("hi", std::uint64_t{ 0 });
			entry.uuid.lo = u.value("lo", std::uint64_t{ 0 });
		}

		if (entry.relativePath.empty()) continue;
		result.entries.push_back(std::move(entry));
	}

	out = std::move(result);
	return true;
}

} // namespace HE::Cs
