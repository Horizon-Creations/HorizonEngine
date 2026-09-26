#include "AssetAutosave.h"
#include "SceneAutosave.h"

#include <Diagnostics/Log.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace HE::Ed
{
	namespace
	{
		constexpr const char* kSnapExt     = ".snap";
		constexpr const char* kManifestExt = ".json";
		constexpr const char* kTempExt     = ".tmp";

		constexpr std::uint64_t kMinIntervalMs = 10'000;

		void removeQuiet(const fs::path& p)
		{
			std::error_code ec;
			fs::remove(p, ec);
		}

		bool moveOver(const fs::path& from, const fs::path& to)
		{
			std::error_code ec;
			if (!fs::exists(from, ec)) return false;
			fs::rename(from, to, ec);
			return !ec;
		}

		std::int64_t nowUnix()
		{
			return std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
		}

		// The file's modification time as a plain number, or nothing if it is
		// not there. Only ever compared with another value from this function,
		// so the clock's epoch does not matter.
		std::optional<std::int64_t> mtimeOf(const fs::path& p)
		{
			std::error_code ec;
			const auto t = fs::last_write_time(p, ec);
			if (ec) return std::nullopt;
			return static_cast<std::int64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
		}

		// A key is a file name in our own folders; anything that could climb out
		// of them is not one of ours.
		bool plausibleKey(const std::string& key)
		{
			return !key.empty() && key.find('/') == std::string::npos &&
			       key.find('\\') == std::string::npos && key.find("..") == std::string::npos;
		}

		bool writeManifest(const fs::path& path, const nlohmann::json& j)
		{
			const fs::path tmp = path.string() + kTempExt;
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				if (!out) return false;
				out << j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
				if (!out) { removeQuiet(tmp); return false; }
			}
			std::error_code ec;
			fs::rename(tmp, path, ec);
			if (ec) { removeQuiet(tmp); return false; }
			return true;
		}

		std::optional<nlohmann::json> readManifest(const fs::path& path)
		{
			std::ifstream in(path, std::ios::binary);
			if (!in) return std::nullopt;
			nlohmann::json j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
			if (j.is_discarded() || !j.is_object()) return std::nullopt;
			if (!j.contains("file") || !j["file"].is_string()) return std::nullopt;
			return j;
		}

		// The path, with symlinks resolved as far as it exists: a tab path and the
		// project path may reach the same folder by two spellings (/tmp and
		// /private/tmp on macOS), and the relative path must not depend on which.
		fs::path normalised(const fs::path& p)
		{
			std::error_code ec;
			fs::path w = fs::weakly_canonical(p, ec);
			return ec ? p.lexically_normal() : w;
		}

		std::string stamp()
		{
			const std::time_t t = std::time(nullptr);
			std::tm tm{};
#ifdef _WIN32
			localtime_s(&tm, &t);
#else
			localtime_r(&t, &tm);
#endif
			char buf[32];
			std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
			return buf;
		}
	}

	std::string AssetAutosave::recoveryDirForProject(const std::string& projectFilePath)
	{
		const std::string scene = SceneAutosave::recoveryDirForProject(projectFilePath);
		if (scene.empty()) return {};
		return (fs::path(scene) / "Assets").string();
	}

	std::string AssetAutosave::projectRootFor(const std::string& projectFilePath)
	{
		if (projectFilePath.empty()) return {};
		fs::path p(projectFilePath);
		std::error_code ec;
		if (fs::is_regular_file(p, ec) || p.has_extension()) p = p.parent_path();
		return p.string();
	}

	std::string AssetAutosave::liveDir() const     { return m_dir.empty() ? std::string() : (fs::path(m_dir) / "Live").string(); }
	std::string AssetAutosave::pendingDir() const  { return m_dir.empty() ? std::string() : (fs::path(m_dir) / "Pending").string(); }
	std::string AssetAutosave::replacedDir() const { return m_dir.empty() ? std::string() : (fs::path(m_dir) / "Replaced").string(); }

	void AssetAutosave::configure(const std::string& recoveryDir, const std::string& projectRoot)
	{
		m_dir           = recoveryDir;
		m_root          = projectRoot;
		m_armed         = false;
		m_lastAttemptMs = 0;
		m_live.clear();
		m_keyCache.clear();
	}

	void AssetAutosave::setIntervalMs(std::uint64_t ms)
	{
		m_intervalMs = ms < kMinIntervalMs ? kMinIntervalMs : ms;
	}

	std::string AssetAutosave::relativeOf(const std::string& absoluteFile) const
	{
		if (m_root.empty() || absoluteFile.empty()) return {};
		const fs::path rel = normalised(absoluteFile).lexically_relative(normalised(m_root));
		if (rel.empty()) return {};
		const std::string s = rel.generic_string();
		// Outside the root: the copy could only be put back by writing there.
		if (s == ".." || s.rfind("../", 0) == 0 || rel.is_absolute()) return {};
		return s;
	}

	std::string AssetAutosave::keyFor(const std::string& absoluteFile)
	{
		if (auto it = m_keyCache.find(absoluteFile); it != m_keyCache.end()) return it->second;
		const std::string rel = relativeOf(absoluteFile);
		std::string key;
		if (!rel.empty())
		{
			// FNV-1a over the relative path makes the key unique; the file name
			// after it is only there so a person looking into the folder can tell
			// which copy is which.
			std::uint64_t h = 1469598103934665603ull;
			for (unsigned char c : rel) { h ^= c; h *= 1099511628211ull; }
			char hex[17];
			std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
			std::string name = fs::path(rel).filename().string();
			for (char& c : name)
				if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) c = '_';
			if (name.size() > 48) name.resize(48);
			key = std::string(hex) + "_" + name;
		}
		m_keyCache.emplace(absoluteFile, key);
		return key;
	}

	void AssetAutosave::removeLive(const std::string& key)
	{
		const fs::path live(liveDir());
		// Manifest first: a copy without its manifest is an interrupted write,
		// never an offer, so the pair cannot be left half-described.
		removeQuiet(live / (key + kManifestExt));
		removeQuiet(live / (key + kSnapExt));
		removeQuiet(live / (key + kSnapExt + kTempExt));
	}

	bool AssetAutosave::writeOne(const AssetSnapshotSource& src, const std::string& rel, const std::string& key)
	{
		const fs::path live(liveDir());
		const fs::path tmp  = live / (key + kSnapExt + kTempExt);
		const fs::path snap = live / (key + kSnapExt);
		removeQuiet(tmp);
		if (!src.write || !src.write(tmp.string()))
		{
			removeQuiet(tmp);
			HE_LOG_WARN(Editor, "%s", ("AssetAutosave: writing a copy of " + rel + " failed").c_str());
			return false;
		}
		std::error_code ec;
		fs::rename(tmp, snap, ec);
		if (ec)
		{
			removeQuiet(tmp);
			HE_LOG_WARN(Editor, "%s", ("AssetAutosave: cannot replace " + snap.string() + ": " + ec.message()).c_str());
			return false;
		}
		const auto mtime = mtimeOf(src.file);
		nlohmann::json j = {
			{ "file",          rel },
			{ "savedAt",       nowUnix() },
			{ "targetExisted", mtime.has_value() },
			{ "targetMtime",   mtime.value_or(0) },
		};
		// LAST: it is what the next start trusts, and it may only describe a
		// copy that is completely there.
		if (!writeManifest(live / (key + kManifestExt), j))
		{
			HE_LOG_WARN(Editor, "%s", ("AssetAutosave: cannot write the manifest for " + rel).c_str());
			return false;
		}
		m_live.insert(key);
		return true;
	}

	std::size_t AssetAutosave::update(std::uint64_t nowMs, const std::vector<AssetSnapshotSource>& dirty)
	{
		if (m_dir.empty()) return 0;

		// Prune first, every call: a file that left the dirty list was saved,
		// reverted or dropped, and its copy would otherwise be offered as unsaved
		// work after the next crash.
		std::vector<std::pair<const AssetSnapshotSource*, std::string>> keyed;
		keyed.reserve(dirty.size());
		std::unordered_set<std::string> dirtyKeys;
		for (const AssetSnapshotSource& s : dirty)
		{
			const std::string key = keyFor(s.file);
			if (key.empty()) continue;
			if (dirtyKeys.insert(key).second) keyed.emplace_back(&s, key);
		}
		for (auto it = m_live.begin(); it != m_live.end();)
		{
			if (dirtyKeys.count(*it)) { ++it; continue; }
			removeLive(*it);
			it = m_live.erase(it);
		}

		// The timer starts at the first tick after configure(), not at zero.
		if (!m_armed)
		{
			m_armed         = true;
			m_lastAttemptMs = nowMs;
			return 0;
		}
		if (!m_enabled || keyed.empty()) return 0;
		if (nowMs - m_lastAttemptMs < m_intervalMs) return 0;
		// Success or not, the next try is one interval away.
		m_lastAttemptMs = nowMs;

		std::error_code ec;
		fs::create_directories(liveDir(), ec);
		if (ec)
		{
			HE_LOG_WARN(Editor, "%s", ("AssetAutosave: cannot create " + liveDir() + ": " + ec.message()).c_str());
			return 0;
		}
		std::size_t written = 0;
		for (const auto& [src, key] : keyed)
			if (writeOne(*src, relativeOf(src->file), key)) ++written;
		if (written)
			HE_LOG_DEBUG(Editor, "%s", ("AssetAutosave: " + std::to_string(written) +
			                            " asset cop" + (written == 1 ? "y" : "ies") + " written").c_str());
		return written;
	}

	void AssetAutosave::clear()
	{
		m_live.clear();
		if (m_dir.empty()) return;
		std::error_code ec;
		fs::remove_all(liveDir(), ec);
	}

	std::size_t AssetAutosave::promoteStale()
	{
		if (m_dir.empty()) return 0;
		const fs::path live(liveDir());
		const fs::path pend(pendingDir());
		std::error_code ec;
		if (fs::is_directory(live, ec))
		{
			std::vector<fs::path> manifests;
			for (const auto& e : fs::directory_iterator(live, ec))
			{
				const fs::path p = e.path();
				if (p.extension() == kManifestExt) manifests.push_back(p);
			}
			if (!manifests.empty()) fs::create_directories(pend, ec);
			for (const fs::path& m : manifests)
			{
				const std::string key = m.stem().string();
				const fs::path snap = live / (key + kSnapExt);
				if (!fs::exists(snap, ec)) continue;
				// Older pending pair first, then copy, then manifest — the write
				// order again, so an interruption leaves a copy without a manifest,
				// which pending() treats as nothing.
				removeQuiet(pend / (key + kManifestExt));
				removeQuiet(pend / (key + kSnapExt));
				if (moveOver(snap, pend / (key + kSnapExt)))
					moveOver(m, pend / (key + kManifestExt));
			}
			// Whatever is still here is an orphan (a temp, a copy without its
			// manifest): a write the crash interrupted, worth nothing.
			fs::remove_all(live, ec);
		}
		m_live.clear();
		return pending().size();
	}

	std::vector<AssetRecoveryEntry> AssetAutosave::pending() const
	{
		std::vector<AssetRecoveryEntry> out;
		if (m_dir.empty()) return out;
		const fs::path pend(pendingDir());
		std::error_code ec;
		if (!fs::is_directory(pend, ec)) return out;
		for (const auto& e : fs::directory_iterator(pend, ec))
		{
			const fs::path m = e.path();
			if (m.extension() != kManifestExt) continue;
			const std::string key = m.stem().string();
			const fs::path snap = pend / (key + kSnapExt);
			if (!fs::exists(snap, ec)) continue;
			const auto j = readManifest(m);
			if (!j) continue;
			AssetRecoveryEntry r;
			r.key          = key;
			r.relativePath = (*j)["file"].get<std::string>();
			r.snapshotPath = snap.string();
			r.savedAtUnix  = j->value("savedAt", std::int64_t(0));
			// The manifest is data on disk: a path in it that climbs out of the
			// project is never a place Restore may write to.
			const fs::path relp(r.relativePath);
			const std::string rg = relp.generic_string();
			if (!r.relativePath.empty() && !relp.is_absolute() && !relp.has_root_name() &&
			    rg != ".." && rg.rfind("../", 0) != 0 && rg.find("/../") == std::string::npos &&
			    !m_root.empty())
				r.targetPath = (fs::path(m_root) / relp).lexically_normal().string();
			const auto now = r.targetPath.empty() ? std::nullopt : mtimeOf(r.targetPath);
			r.targetMissing = !now.has_value();
			const bool existed = j->value("targetExisted", false);
			if (existed)
				r.changedSince = now.has_value() && *now != j->value("targetMtime", std::int64_t(0));
			else
				r.changedSince = now.has_value();   // created after the copy was taken
			out.push_back(std::move(r));
		}
		std::sort(out.begin(), out.end(),
			[](const AssetRecoveryEntry& a, const AssetRecoveryEntry& b) { return a.relativePath < b.relativePath; });
		return out;
	}

	void AssetAutosave::discardPending(const std::string& key)
	{
		if (m_dir.empty() || !plausibleKey(key)) return;
		const fs::path pend(pendingDir());
		removeQuiet(pend / (key + kManifestExt));
		removeQuiet(pend / (key + kSnapExt));
	}

	std::optional<std::string> AssetAutosave::restorePending(const std::string& key, std::string* error)
	{
		auto fail = [error](const std::string& why) -> std::optional<std::string>
		{
			if (error) *error = why;
			HE_LOG_WARN(Editor, "%s", ("AssetAutosave: restore failed: " + why).c_str());
			return std::nullopt;
		};
		if (m_dir.empty() || !plausibleKey(key)) return fail("no such recovery copy");

		std::optional<AssetRecoveryEntry> entry;
		for (auto& e : pending())
			if (e.key == key) { entry = std::move(e); break; }
		if (!entry) return fail("no such recovery copy");
		if (entry->targetPath.empty()) return fail(entry->relativePath + " is not inside this project");

		const fs::path target(entry->targetPath);
		std::error_code ec;
		// The version on disk first. If it cannot be kept, nothing is written.
		if (!entry->targetMissing)
		{
			const fs::path backupDir(replacedDir());
			fs::create_directories(backupDir, ec);
			const fs::path backup = backupDir / (stamp() + "_" + target.filename().string());
			ec.clear();
			fs::copy_file(target, backup, fs::copy_options::overwrite_existing, ec);
			if (ec) return fail("cannot keep a copy of " + entry->relativePath + ": " + ec.message());
		}
		fs::create_directories(target.parent_path(), ec);
		const fs::path tmp = target.string() + ".herestore" + kTempExt;
		ec.clear();
		fs::copy_file(entry->snapshotPath, tmp, fs::copy_options::overwrite_existing, ec);
		if (ec) { removeQuiet(tmp); return fail("cannot write " + entry->relativePath + ": " + ec.message()); }
		fs::rename(tmp, target, ec);
		if (ec) { removeQuiet(tmp); return fail("cannot replace " + entry->relativePath + ": " + ec.message()); }

		discardPending(key);
		HE_LOG_INFO(Editor, "%s", ("AssetAutosave: restored " + entry->relativePath + " from its recovery copy").c_str());
		return entry->targetPath;
	}
}
