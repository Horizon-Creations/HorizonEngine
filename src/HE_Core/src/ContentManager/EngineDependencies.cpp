// Engine-content dependency walk — see EngineDependencies.h.
#include "ContentManager/EngineDependencies.h"

#include "ContentManager/HAsset.h"
#include "Types/Enums.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace HE::EngineDeps
{
namespace
{
namespace fs = std::filesystem;
constexpr char kEnginePrefix[] = "Engine/";
constexpr size_t kEnginePrefixLen = sizeof(kEnginePrefix) - 1;
constexpr char kAssetExt[] = ".hasset";
constexpr size_t kAssetExtLen = sizeof(kAssetExt) - 1;

bool isPathByte(unsigned char c)
{
	// What can sit inside a content path as it is stored: letters, digits and the
	// punctuation a file name may carry. A quote, a backslash, a control byte or a
	// length prefix's zero ends it.
	if (c >= '0' && c <= '9') return true;
	if (c >= 'A' && c <= 'Z') return true;
	if (c >= 'a' && c <= 'z') return true;
	switch (c) {
	case '_': case '-': case '.': case '/': case ' ': case '(': case ')': case '+': case '#': case '@': case '&': case ',':
		return true;
	default: return false;
	}
}

bool isIdByte(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }

std::string lowerExt(const fs::path& p)
{
	std::string e = p.extension().string();
	std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return e;
}

// Reads at most `limit` bytes of a file; false when it cannot be opened.
bool readPrefix(const fs::path& path, size_t limit, std::string& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	f.seekg(0, std::ios::end);
	const std::streamoff size = f.tellg();
	f.seekg(0, std::ios::beg);
	const size_t n = size <= 0 ? 0 : std::min<size_t>(static_cast<size_t>(size), limit);
	out.resize(n);
	if (n) f.read(out.data(), static_cast<std::streamsize>(n));
	out.resize(static_cast<size_t>(f.gcount()));
	return true;
}

// Is this .hasset one that can point at other assets? Reads only its 32-byte header.
bool assetMayReference(const fs::path& path)
{
	std::ifstream f(path, std::ios::binary);
	HAsset::FileHeader hdr{};
	if (!f || !f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) return false;
	if (std::memcmp(hdr.magic, HAsset::k_magic, 4) != 0) return false;
	switch (static_cast<HE::AssetType>(hdr.asset_type))
	{
	case HE::AssetType::Texture:
	case HE::AssetType::Audio:
	case HE::AssetType::Font:
		return false;           // payload only, nothing to follow
	default:
		return true;
	}
}

// Asset ids in a scene: [hi, lo] arrays of two unsigned numbers and {"hi":…,"lo":…}.
void collectIds(const nlohmann::json& j, std::vector<HE::UUID>& out)
{
	if (j.is_array())
	{
		if (j.size() == 2 && j[0].is_number_unsigned() && j[1].is_number_unsigned())
			out.push_back(HE::UUID{ j[0].get<uint64_t>(), j[1].get<uint64_t>() });
		else
			for (const auto& e : j) collectIds(e, out);
	}
	else if (j.is_object())
	{
		const auto hi = j.find("hi"), lo = j.find("lo");
		if (hi != j.end() && lo != j.end() && hi->is_number_unsigned() && lo->is_number_unsigned())
			out.push_back(HE::UUID{ hi->get<uint64_t>(), lo->get<uint64_t>() });
		else
			for (const auto& e : j.items()) collectIds(e.value(), out);
	}
}

struct Walker
{
	const Options&                                   opt;
	Result                                           result;
	std::unordered_map<std::string, const CatalogueEntry*> byPath;
	std::unordered_map<HE::UUID, const CatalogueEntry*>    byId;

	explicit Walker(const Options& o) : opt(o)
	{
		for (const CatalogueEntry& e : opt.catalogue)
		{
			byPath[e.path] = &e;
			if (e.uuid != HE::UUID{}) byId[e.uuid] = &e;
		}
	}

	bool cancelled() { return opt.cancelled && opt.cancelled() ? (result.cancelled = true) : false; }
	void report(const char* stage, const std::string& detail, size_t done = 0, size_t total = 0)
	{
		if (opt.progress) opt.progress(Progress{ stage, detail, done, total });
	}

	// Engine references one file holds → `out` ("Engine/…" spelling), first-seen order.
	void scanFile(const fs::path& path, std::vector<std::string>& out)
	{
		const std::string ext = lowerExt(path);
		std::string bytes;
		const bool isAsset = ext == ".hasset";
		if (isAsset)
		{
			if (!assetMayReference(path)) return;
			if (!readPrefix(path, opt.maxAssetBytes, bytes)) return;
		}
		else if (ext == ".hescene" || ext == ".hcode" || ext == ".heproj" || ext == ".lua" || ext == ".py" ||
		         ext == ".json")
		{
			if (!readPrefix(path, opt.maxTextBytes, bytes)) return;
		}
		else
			return;
		++result.filesScanned;

		for (std::string& p : findEngineReferences(bytes.data(), bytes.size()))
			out.push_back(std::move(p));

		if (ext == ".hescene" && !byId.empty())
		{
			const nlohmann::json j = nlohmann::json::parse(bytes, nullptr, /*allow_exceptions=*/false);
			if (!j.is_discarded())
			{
				std::vector<HE::UUID> ids;
				collectIds(j, ids);
				for (const HE::UUID& id : ids)
					if (const auto it = byId.find(id); it != byId.end())
						out.push_back(std::string(kEnginePrefix) + it->second->path);
			}
		}
	}

	std::string locate(const std::string& enginePath) { return locateEngineFile(enginePath, opt); }
};
} // namespace

std::vector<std::string> findEngineReferences(const char* bytes, std::size_t size)
{
	std::vector<std::string> found;
	std::unordered_set<std::string> seen;
	if (!bytes || size < kEnginePrefixLen + kAssetExtLen) return found;

	const char* end = bytes + size;
	for (const char* p = bytes; p + kEnginePrefixLen < end; ++p)
	{
		p = static_cast<const char*>(memchr(p, 'E', static_cast<size_t>(end - p)));
		if (!p || p + kEnginePrefixLen >= end) break;
		if (std::memcmp(p, kEnginePrefix, kEnginePrefixLen) != 0) continue;
		// "MyEngine/…" and "…_Engine/…" are not references into the library.
		if (p != bytes && isIdByte(static_cast<unsigned char>(p[-1]))) continue;

		// Extend over path bytes (bounded), then cut at the FIRST ".hasset": whatever
		// follows (a closing quote, the next field's bytes) is not part of it.
		const char* q = p + kEnginePrefixLen;
		const char* limit = std::min(end, p + 512);
		while (q < limit && isPathByte(static_cast<unsigned char>(*q))) ++q;
		const std::string token(p, q);
		const size_t at = token.find(kAssetExt);
		if (at == std::string::npos || at <= kEnginePrefixLen) continue;
		std::string path = token.substr(0, at + kAssetExtLen);
		if (seen.insert(path).second) found.push_back(std::move(path));
		p = q - 1;
	}
	return found;
}

std::string locateEngineFile(const std::string& enginePath, const Options& o)
{
	if (enginePath.rfind(kEnginePrefix, 0) != 0) return {};
	const std::string rest = enginePath.substr(kEnginePrefixLen);
	std::error_code ec;
	const fs::path candidates[] = {
		fs::path(o.projectContentRoot) / "Engine" / rest,   // a project override wins
		fs::path(o.engineRoot) / rest,                       // the shipped / checked-out library
		fs::path(o.cacheRoot) / rest,                        // what was downloaded
	};
	const bool use[] = { !o.projectContentRoot.empty(), !o.engineRoot.empty(), !o.cacheRoot.empty() };
	for (int i = 0; i < 3; ++i)
		if (use[i] && fs::is_regular_file(candidates[i], ec)) return candidates[i].string();
	return {};
}

Result resolve(const Options& options)
{
	Walker w(options);
	std::error_code ec;

	// ── 1. The project's own files ───────────────────────────────────────────
	w.report("Reading the project", {});
	std::vector<std::string> pendingList;                  // "Engine/…" paths, first-seen order
	std::unordered_map<std::string, std::string> neededBy; // path → the file that named it first

	auto scanProjectFile = [&](const fs::path& p)
	{
		std::vector<std::string> refs;
		w.scanFile(p, refs);
		for (std::string& r : refs)
			if (neededBy.emplace(r, p.filename().string()).second) pendingList.push_back(std::move(r));
	};
	if (!options.projectFile.empty())
	{
		scanProjectFile(options.projectFile);
		const fs::path dir = fs::path(options.projectFile).parent_path();
		for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), e; !ec && it != e; it.increment(ec))
			if (it->is_regular_file(ec) && lowerExt(it->path()) == ".hcode") scanProjectFile(it->path());
	}
	if (!options.projectContentRoot.empty())
	{
		size_t n = 0;
		for (fs::recursive_directory_iterator it(options.projectContentRoot, fs::directory_options::skip_permission_denied, ec), e;
		     !ec && it != e; it.increment(ec))
		{
			if (w.cancelled()) return w.result;
			std::error_code fe;
			if (!it->is_regular_file(fe)) continue;
			scanProjectFile(it->path());
			if ((++n & 63u) == 0) w.report("Reading the project", it->path().filename().string(), n);
		}
	}

	// ── 2. Follow the references, a level at a time ──────────────────────────
	std::unordered_set<std::string> visited;
	std::vector<std::string> level = std::move(pendingList);
	while (!level.empty())
	{
		if (w.cancelled()) return w.result;
		std::vector<std::string> next;
		std::vector<std::string> toFetch;

		w.report("Checking what the project needs", {}, visited.size(), visited.size() + level.size());
		for (const std::string& path : level)
		{
			if (!visited.insert(path).second) continue;
			++w.result.referenced;
			const std::string here = w.locate(path);
			if (!here.empty())
			{
				++w.result.alreadyHere;
				std::vector<std::string> refs;
				w.scanFile(here, refs);
				for (std::string& r : refs)
					if (neededBy.emplace(r, fs::path(path).filename().string()).second) next.push_back(std::move(r));
				continue;
			}
			const std::string rel = path.substr(kEnginePrefixLen);
			if (!options.catalogueKnown)
				w.result.missing.push_back({ path, MissingReason::NoCatalogue, neededBy[path] });
			else if (!w.byPath.count(rel))
				w.result.missing.push_back({ path, MissingReason::NotOnServer, neededBy[path] });
			else if (!options.fetch)
				w.result.missing.push_back({ path, MissingReason::NoFetcher, neededBy[path] });
			else
				toFetch.push_back(path);
		}

		// All of this level's downloads are asked for at once and awaited together:
		// the sync queue works through them one by one anyway, and the next level can
		// only be read once these are here.
		if (!toFetch.empty())
		{
			std::mutex m;
			std::condition_variable cv;
			size_t finished = 0;
			std::vector<char> ok(toFetch.size(), 0);
			for (size_t i = 0; i < toFetch.size(); ++i)
				options.fetch(toFetch[i].substr(kEnginePrefixLen), [&, i](bool success)
				{
					std::lock_guard<std::mutex> lock(m);
					ok[i] = success ? 1 : 0;
					++finished;
					cv.notify_all();
				});
			{
				std::unique_lock<std::mutex> lock(m);
				size_t lastReported = size_t(-1);
				while (finished < toFetch.size())
				{
					cv.wait_for(lock, std::chrono::milliseconds(100));
					if (finished != lastReported)
					{
						lastReported = finished;
						lock.unlock();
						w.report("Downloading", {}, finished, toFetch.size());
						lock.lock();
					}
					if (options.cancelled && options.cancelled())
					{
						// The downloads already queued run on; this call just stops waiting.
						w.result.cancelled = true;
						return w.result;
					}
				}
			}
			for (size_t i = 0; i < toFetch.size(); ++i)
			{
				const std::string& path = toFetch[i];
				const std::string here = ok[i] ? w.locate(path) : std::string();
				if (here.empty())
				{
					w.result.missing.push_back({ path, MissingReason::DownloadFailed, neededBy[path] });
					continue;
				}
				++w.result.downloaded;
				std::vector<std::string> refs;
				w.scanFile(here, refs);
				for (std::string& r : refs)
					if (neededBy.emplace(r, fs::path(path).filename().string()).second) next.push_back(std::move(r));
			}
		}
		level = std::move(next);
	}
	return w.result;
}

} // namespace HE::EngineDeps
