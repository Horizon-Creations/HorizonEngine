#include "AssetIdentity.h"

#include <ContentManager/AssetRefScan.h>
#include <ContentManager/ContentManager.h>
#include <Types/UUID.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <unordered_map>

namespace fs = std::filesystem;

namespace HE::Ed::AssetIdentity
{
namespace
{
using Clock = std::chrono::steady_clock;

// How long an answer is trusted before the file is looked at again. A rename or a
// replaced file shows up within this; asking again inside it costs one hash lookup,
// which matters because every panel row is asked for every tab every frame.
constexpr std::chrono::milliseconds kRecheck{ 250 };
// A runaway guard, not a working set: a few dozen tabs are ever asked about.
constexpr std::size_t kMaxCached = 4096;

struct Cached
{
	bool                  valid = false;
	HE::UUID              uuid{};
	std::string           key;
	fs::file_time_type    mtime{};
	std::uintmax_t        size = 0;
	bool                  hadFile = false;
	Clock::time_point     checkedAt{};
	std::uint64_t         epoch = 0;
};

const ContentManager*                        s_content = nullptr;
std::unordered_map<std::string, Cached>      s_cache;

bool isVirtualTab(const std::string& p) { return p.empty() || p[0] == ':'; }

std::string normalise(const std::string& p)
{
	return fs::path(p).lexically_normal().generic_string();
}

// A trailing separator is the one thing lexically_normal leaves different between
// "a/b" and "a/b/".
std::string normalisedPathKey(const std::string& p)
{
	std::string n = normalise(p);
	while (n.size() > 1 && n.back() == '/') n.pop_back();
	return "path:" + n;
}

std::string uuidKey(const HE::UUID& id)
{
	char buf[40];
	std::snprintf(buf, sizeof(buf), "uuid:%016llx%016llx",
	              static_cast<unsigned long long>(id.hi),
	              static_cast<unsigned long long>(id.lo));
	return buf;
}

bool isAssetFile(const std::string& p)
{
	return fs::path(p).extension() == ".hasset";
}

// Is `uuid` at this file, as far as the ContentManager can tell? It answers "no"
// only when it positively places the id somewhere else: a copy made outside the
// editor carries the original's id, and two files must not share a panel state.
// A registry that does not know the id yet (a file written a moment ago, before
// the next scan) is no evidence against it.
bool contentManagerAgrees(const std::string& path, const HE::UUID& uuid)
{
	if (!s_content) return true;
	const std::string owner = s_content->pathForId(uuid);
	if (owner.empty()) return true;
	const std::string here = s_content->toContentRelativePath(path);
	return here.empty() || here == owner;
}

std::uint64_t currentEpoch() { return s_content ? s_content->contentEpoch() : 0; }
} // namespace

void bind(const ContentManager* content)
{
	s_content = content;
	s_cache.clear();
}

void resetCache() { s_cache.clear(); }

std::string pathKeyFor(const std::string& assetPath)
{
	if (isVirtualTab(assetPath)) return assetPath;
	return normalisedPathKey(assetPath);
}

bool samePathSpelling(const std::string& a, const std::string& b)
{
	if (a == b) return true;
	if (isVirtualTab(a) || isVirtualTab(b)) return false;
	return normalise(a) == normalise(b);
}

std::string contentRelative(const std::string& assetPath)
{
	if (!s_content || isVirtualTab(assetPath)) return {};
	return s_content->toContentRelativePath(assetPath);
}

HE::UUID uuidFor(const std::string& assetPath)
{
	if (isVirtualTab(assetPath)) return {};
	const std::string key = keyFor(assetPath);
	if (key.rfind("uuid:", 0) != 0) return {};
	const auto it = s_cache.find(assetPath);
	return it == s_cache.end() ? HE::UUID{} : it->second.uuid;
}

std::string keyFor(const std::string& assetPath)
{
	if (isVirtualTab(assetPath)) return assetPath;

	if (s_cache.size() > kMaxCached) s_cache.clear();
	Cached&       c   = s_cache[assetPath];
	const auto    now = Clock::now();
	const std::uint64_t epoch = currentEpoch();
	if (c.valid && c.epoch == epoch && now - c.checkedAt < kRecheck) return c.key;

	std::error_code ec;
	const fs::file_time_type mtime = fs::last_write_time(assetPath, ec);
	const bool                  fileOk = !ec;
	std::uintmax_t              size = 0;
	if (fileOk) { ec.clear(); size = fs::file_size(assetPath, ec); }

	if (!fileOk)
	{
		// The file is not there: deleted, or moved away a moment ago. Whoever asks
		// is usually closing or forgetting a tab that WAS there, and its state sits
		// under the identity we gave it then — keep answering with that, rather
		// than with a path key that no state was ever filed under.
		if (c.valid && c.hadFile) { c.checkedAt = now; c.epoch = epoch; return c.key; }
		c.valid = true; c.hadFile = false; c.uuid = {};
		c.key = normalisedPathKey(assetPath);
		c.checkedAt = now; c.epoch = epoch;
		return c.key;
	}

	// The file's header is read only when the file changed (or on first sight).
	const bool sameFile = c.valid && c.hadFile && c.mtime == mtime && c.size == size;
	if (!sameFile)
		c.uuid = isAssetFile(assetPath) ? HE::AssetRefs::assetUuidOfFile(assetPath) : HE::UUID{};

	c.valid = true; c.hadFile = true; c.mtime = mtime; c.size = size;
	c.checkedAt = now; c.epoch = epoch;
	c.key = (c.uuid == HE::UUID{} || !contentManagerAgrees(assetPath, c.uuid))
		? normalisedPathKey(assetPath)
		: uuidKey(c.uuid);
	return c.key;
}
} // namespace HE::Ed::AssetIdentity
