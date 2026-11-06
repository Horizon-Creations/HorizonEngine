#include "ContentSync/CacheFreshness.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace HE::Cs {

namespace {

std::mutex g_recordsMutex;   // the records file is read-modified-written by the refresh and by downloads

struct Record { std::uint64_t size = 0; std::int64_t mtime = 0; std::uint64_t hash = 0; };

fs::path recordsPath(const fs::path& cacheDir) { return cacheDir / ".cache-hashes.json"; }

std::map<std::string, Record> loadRecords(const fs::path& cacheDir)
{
	std::map<std::string, Record> out;
	std::ifstream in(recordsPath(cacheDir), std::ios::binary);
	if (!in) return out;
	const json j = json::parse(in, nullptr, /*allow_exceptions=*/false);
	if (!j.is_object()) return out;
	for (auto it = j.begin(); it != j.end(); ++it)
	{
		const json& v = it.value();
		if (!v.is_object() || !v.contains("size") || !v.contains("mtime") || !v.contains("hash")) continue;
		Record r;
		r.size  = v["size"].get<std::uint64_t>();
		r.mtime = v["mtime"].get<std::int64_t>();
		r.hash  = v["hash"].get<std::uint64_t>();
		out[it.key()] = r;
	}
	return out;
}

void saveRecords(const fs::path& cacheDir, const std::map<std::string, Record>& records)
{
	json j = json::object();
	for (const auto& [path, r] : records)
		j[path] = { { "size", r.size }, { "mtime", r.mtime }, { "hash", r.hash } };
	const fs::path finalPath = recordsPath(cacheDir);
	const fs::path tmp = finalPath.string() + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		out << j.dump();
		if (!out) return;
	}
	std::error_code ec;
	fs::rename(tmp, finalPath, ec);
	if (ec) fs::remove(tmp, ec);
}

// Byte-wise FNV-1a, the same function as Hpak::fnv1a64Bytes (the manifest's change key), fed in blocks.
bool fnv1a64File(const fs::path& p, std::uint64_t& out)
{
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	std::uint64_t h = 1469598103934665603ULL;
	std::vector<char> buf(4u << 20);
	while (f)
	{
		f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
		const std::streamsize n = f.gcount();
		for (std::streamsize i = 0; i < n; ++i)
		{
			h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(buf[static_cast<size_t>(i)]));
			h *= 1099511628211ULL;
		}
	}
	if (!f.eof()) return false;
	out = h;
	return true;
}

std::int64_t mtimeOf(const fs::path& p, std::error_code& ec)
{
	return static_cast<std::int64_t>(fs::last_write_time(p, ec).time_since_epoch().count());
}

} // namespace

std::size_t discardStaleCachedFiles(const fs::path& cacheDir, const EngineContentManifest& manifest,
                                    const std::function<bool(const std::string&)>& isBusy,
                                    const std::function<void(const std::string&)>& onLog)
{
	std::lock_guard<std::mutex> lock(g_recordsMutex);
	std::map<std::string, Record> records = loadRecords(cacheDir);
	bool recordsChanged = false;
	std::size_t removed = 0;

	for (const EngineContentManifestEntry& e : manifest.entries)
	{
		if (e.relativePath.empty() || e.contentHash == 0) continue;   // a raw file: nothing to compare
		const fs::path file = cacheDir / e.relativePath;
		std::error_code ec;
		if (!fs::is_regular_file(file, ec)) { if (records.erase(e.relativePath)) recordsChanged = true; continue; }
		if (isBusy && isBusy(e.relativePath)) continue;

		const std::uint64_t size = fs::file_size(file, ec);
		if (ec) continue;
		const std::int64_t mtime = mtimeOf(file, ec);
		if (ec) continue;

		bool stale = false;
		if (e.size != 0 && size != e.size)
			stale = true;
		else
		{
			std::uint64_t hash = 0;
			const auto it = records.find(e.relativePath);
			if (it != records.end() && it->second.size == size && it->second.mtime == mtime)
				hash = it->second.hash;
			else if (fnv1a64File(file, hash))
			{
				records[e.relativePath] = { size, mtime, hash };
				recordsChanged = true;
			}
			else continue;   // unreadable right now: leave it
			stale = hash != e.contentHash;
		}
		if (!stale) continue;

		if (fs::remove(file, ec) && !ec)
		{
			++removed;
			records.erase(e.relativePath);
			recordsChanged = true;
			if (onLog) onLog("Cached '" + e.relativePath + "' is out of date (the server has a newer one): removed, it downloads again when it is used");
		}
	}
	if (recordsChanged) saveRecords(cacheDir, records);
	return removed;
}

void recordCachedFile(const fs::path& cacheDir, const std::string& relativePath, std::uint64_t contentHash)
{
	std::lock_guard<std::mutex> lock(g_recordsMutex);
	std::map<std::string, Record> records = loadRecords(cacheDir);
	if (contentHash == 0)
	{
		if (!records.erase(relativePath)) return;
	}
	else
	{
		const fs::path file = cacheDir / relativePath;
		std::error_code ec;
		const std::uint64_t size = fs::file_size(file, ec);
		if (ec) return;
		const std::int64_t mtime = mtimeOf(file, ec);
		if (ec) return;
		records[relativePath] = { size, mtime, contentHash };
	}
	saveRecords(cacheDir, records);
}

} // namespace HE::Cs
