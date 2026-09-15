#include "SceneAutosave.h"

#include <Diagnostics/Log.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace HE::Ed
{
	namespace
	{
		constexpr const char* kLiveScene       = "autosave.hescene";
		constexpr const char* kLiveManifest    = "autosave.json";
		constexpr const char* kPendingScene    = "recovery.hescene";
		constexpr const char* kPendingManifest = "recovery.json";
		// The temporary the writer fills; renamed over the live name once whole.
		constexpr const char* kLiveTemp        = "autosave.hescene.tmp";

		constexpr std::uint64_t kMinIntervalMs = 10'000;

		void removeQuiet(const fs::path& p)
		{
			std::error_code ec;
			fs::remove(p, ec);
		}

		bool writeManifest(const fs::path& path, const RecoveryInfo& info)
		{
			nlohmann::json j = {
				{ "scene",    info.scenePath },
				{ "project",  info.projectPath },
				{ "revision", info.revision },
				{ "savedAt",  info.savedAtUnix },
			};
			// Temp + rename here too: a manifest is what the next start trusts,
			// and a truncated one would be read as "nothing to offer".
			const fs::path tmp = path.string() + ".tmp";
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				if (!out) return false;
				// The replace handler keeps a path with a stray byte from throwing
				// inside the frame loop.
				out << j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
				if (!out) { removeQuiet(tmp); return false; }
			}
			std::error_code ec;
			fs::rename(tmp, path, ec);
			if (ec) { removeQuiet(tmp); return false; }
			return true;
		}

		// Moves `from` over `to` (replacing it), and reports whether it did.
		bool moveOver(const fs::path& from, const fs::path& to)
		{
			std::error_code ec;
			if (!fs::exists(from, ec)) return false;
			fs::rename(from, to, ec);
			return !ec;
		}
	}

	std::string SceneAutosave::recoveryDirForProject(const std::string& projectFilePath)
	{
		if (projectFilePath.empty()) return {};
		fs::path p(projectFilePath);
		// The project path is the .hproject file in the editor, but a caller
		// may hand the folder itself — both mean the same project.
		std::error_code ec;
		if (fs::is_regular_file(p, ec) || p.has_extension()) p = p.parent_path();
		return (p / "Saved" / "Autosave").string();
	}

	std::string SceneAutosave::livePath(const std::string& dir)            { return (fs::path(dir) / kLiveScene).string(); }
	std::string SceneAutosave::liveManifestPath(const std::string& dir)    { return (fs::path(dir) / kLiveManifest).string(); }
	std::string SceneAutosave::pendingPath(const std::string& dir)         { return (fs::path(dir) / kPendingScene).string(); }
	std::string SceneAutosave::pendingManifestPath(const std::string& dir) { return (fs::path(dir) / kPendingManifest).string(); }

	void SceneAutosave::configure(const std::string& recoveryDir)
	{
		m_dir           = recoveryDir;
		m_armed         = false;
		m_lastAttemptMs = 0;
		m_haveRevision  = false;
		m_lastRevision  = 0;
	}

	void SceneAutosave::setIntervalMs(std::uint64_t ms)
	{
		m_intervalMs = ms < kMinIntervalMs ? kMinIntervalMs : ms;
	}

	bool SceneAutosave::update(std::uint64_t nowMs, bool dirty, std::uint64_t revision,
	                           const RecoveryInfo& info, const Writer& writer)
	{
		if (!m_enabled || m_dir.empty() || !writer) return false;

		// The timer starts at the first tick after configure(), not at zero:
		// otherwise the first frame of every project would snapshot at once,
		// and "interval" would mean nothing for the first write.
		if (!m_armed)
		{
			m_armed         = true;
			m_lastAttemptMs = nowMs;
			return false;
		}
		if (!dirty) return false;
		if (m_haveRevision && revision == m_lastRevision) return false;
		if (nowMs - m_lastAttemptMs < m_intervalMs) return false;

		// Whether it works or not, the next try is one interval away: a scene
		// that cannot be written (disk full, folder gone) must not be retried
		// every frame.
		m_lastAttemptMs = nowMs;

		std::error_code ec;
		fs::create_directories(m_dir, ec);
		if (ec)
		{
			HE_LOG_WARN(Editor, "%s", ("SceneAutosave: cannot create " + m_dir + ": " + ec.message()).c_str());
			return false;
		}

		const fs::path tmp  = fs::path(m_dir) / kLiveTemp;
		const fs::path live = fs::path(m_dir) / kLiveScene;
		removeQuiet(tmp);
		if (!writer(tmp.string()))
		{
			// The previous snapshot (if any) is still whole and its manifest
			// still describes it — nothing to undo here beyond the temp.
			removeQuiet(tmp);
			HE_LOG_WARN(Editor, "%s", ("SceneAutosave: writing " + tmp.string() + " failed").c_str());
			return false;
		}
		fs::rename(tmp, live, ec);
		if (ec)
		{
			removeQuiet(tmp);
			HE_LOG_WARN(Editor, "%s", ("SceneAutosave: cannot replace " + live.string() + ": " + ec.message()).c_str());
			return false;
		}

		// The manifest LAST: it is the thing the next start reads, and it may
		// only ever describe a file that is completely there.
		RecoveryInfo stamped = info;
		stamped.revision    = revision;
		if (stamped.savedAtUnix == 0)
			stamped.savedAtUnix = std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
		if (!writeManifest(fs::path(m_dir) / kLiveManifest, stamped))
		{
			HE_LOG_WARN(Editor, "%s", ("SceneAutosave: cannot write manifest in " + m_dir).c_str());
			return false;
		}

		m_lastRevision = revision;
		m_haveRevision = true;
		HE_LOG_DEBUG(Editor, "%s", ("SceneAutosave: snapshot written to " + live.string()).c_str());
		return true;
	}

	void SceneAutosave::clear()
	{
		m_haveRevision = false;
		m_lastRevision = 0;
		if (m_dir.empty()) return;
		removeQuiet(fs::path(m_dir) / kLiveManifest);
		removeQuiet(fs::path(m_dir) / kLiveScene);
		removeQuiet(fs::path(m_dir) / kLiveTemp);
	}

	bool SceneAutosave::hasLiveSnapshot() const
	{
		if (m_dir.empty()) return false;
		std::error_code ec;
		return fs::exists(fs::path(m_dir) / kLiveManifest, ec) &&
		       fs::exists(fs::path(m_dir) / kLiveScene, ec);
	}

	std::optional<RecoveryInfo> SceneAutosave::promoteStale()
	{
		if (m_dir.empty()) return std::nullopt;
		const fs::path dir(m_dir);
		// A half-written temp from the crash is worthless — the live file beside
		// it is the last COMPLETE state.
		removeQuiet(dir / kLiveTemp);

		if (hasLiveSnapshot())
		{
			// The older pending pair goes first, then scene, then manifest — the
			// write order again. Interrupted anywhere in between, what is left is
			// a scene without a manifest, which pending() reports as nothing:
			// a missing offer, never an offer whose label belongs to another file.
			discardPending();
			if (moveOver(dir / kLiveScene, dir / kPendingScene))
				moveOver(dir / kLiveManifest, dir / kPendingManifest);
		}
		else
		{
			// A scene without its manifest (or the other way round) is a write
			// the crash interrupted; the previous pending pair, if any, is the
			// better offer, so only the orphan goes.
			removeQuiet(dir / kLiveScene);
			removeQuiet(dir / kLiveManifest);
		}
		return pending();
	}

	std::optional<RecoveryInfo> SceneAutosave::pending() const
	{
		if (m_dir.empty()) return std::nullopt;
		const fs::path dir(m_dir);
		std::error_code ec;
		if (!fs::exists(dir / kPendingScene, ec)) return std::nullopt;
		auto info = readManifest((dir / kPendingManifest).string());
		if (!info) return std::nullopt;
		info->snapshotPath = (dir / kPendingScene).string();
		return info;
	}

	void SceneAutosave::discardPending()
	{
		if (m_dir.empty()) return;
		removeQuiet(fs::path(m_dir) / kPendingManifest);
		removeQuiet(fs::path(m_dir) / kPendingScene);
	}

	std::optional<RecoveryInfo> SceneAutosave::readManifest(const std::string& manifestPath)
	{
		std::ifstream in(manifestPath, std::ios::binary);
		if (!in) return std::nullopt;
		nlohmann::json j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
		if (j.is_discarded() || !j.is_object()) return std::nullopt;
		RecoveryInfo info;
		info.scenePath   = j.value("scene", std::string());
		info.projectPath = j.value("project", std::string());
		info.revision    = j.value("revision", std::uint64_t(0));
		info.savedAtUnix = j.value("savedAt", std::int64_t(0));
		return info;
	}
}
