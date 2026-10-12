#include "SelfUpdate/Takeover.h"
#include "SelfUpdate/SelfUpdateApplyCore.h"
#include "SelfUpdate/SelfUpdateInstall.h"
#include "SelfUpdate/SelfUpdateProcess.h"

#include <chrono>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace HE::Su {

namespace {

bool startsWith(const std::string& s, const char* prefix)
{
	const std::size_t n = std::char_traits<char>::length(prefix);
	return s.size() >= n && s.compare(0, n, prefix) == 0;
}

std::string randomSuffix()
{
	std::random_device rd;
	std::ostringstream os;
	os << std::hex << rd() << rd();
	return os.str();
}

std::vector<std::string> preserveListFor(const std::string& platformKey)
{
	// A macOS .app bundle is signed and often read-only (a mounted dmg, a
	// translocated /Applications copy); GlobalState::setLogFile() already falls
	// back to the per-user data dir there instead of writing beside the
	// executable, so there is nothing of the install itself to carry over.
	if (startsWith(platformKey, "macos")) return {};
	return { "config.json", "imgui.ini", "HorizonEngine.log",
	         "HorizonEngine.1.log", "HorizonEngine.2.log", "HorizonEngine.3.log" };
}

std::string updaterExeName()
{
#if defined(_WIN32)
	return "HorizonUpdater.exe";
#else
	return "HorizonUpdater";
#endif
}

bool directoryIsWritable(const fs::path& dir, std::string& error)
{
	std::error_code ec;
	if (!fs::is_directory(dir, ec)) { error = "not a directory: " + utf8FromPath(dir); return false; }
	const fs::path probe = dir / (".he-selfupdate-write-test-" + randomSuffix());
	std::ofstream f(probe, std::ios::binary);
	if (!f) { error = "not writable: " + utf8FromPath(dir); return false; }
	f.close();
	fs::remove(probe, ec);
	return true;
}

} // namespace

TakeoverCheck preflightTakeover(const TakeoverOptions& options)
{
	TakeoverCheck result;

	StagedUpdate staged;
	std::string error;
	if (!readPendingUpdate(options.stagingRoot, staged, error))
	{
		result.error = error.empty() ? "nothing is staged" : error;
		return result;
	}
	if (!verifyStagedUpdate(staged, error))
	{
		result.error = "the staged build failed verification: " + error;
		return result;
	}

	const fs::path currentExe = currentExecutablePath();
	if (currentExe.empty()) { result.error = "could not determine the running executable's path"; return result; }

	const fs::path installRoot = installRootForRunningBuild(currentExe, staged.platform);
	if (installRoot.empty()) { result.error = "the running build is not laid out the way '" + staged.platform + "' expects"; return result; }

	std::string writeErr;
	if (!directoryIsWritable(installRoot.parent_path(), writeErr))
	{
		result.error = "cannot update in place: " + writeErr;
		return result;
	}

	const fs::path updaterSrc = currentExe.parent_path() / updaterExeName();
	std::error_code ec;
	if (!fs::is_regular_file(updaterSrc, ec))
	{
		result.error = "HorizonUpdater is missing next to the editor: " + utf8FromPath(updaterSrc);
		return result;
	}

	result.ok = true;
	return result;
}

TakeoverResult beginTakeover(const TakeoverOptions& options)
{
	TakeoverResult result;

	StagedUpdate staged;
	std::string error;
	if (!readPendingUpdate(options.stagingRoot, staged, error))
	{
		result.error = error.empty() ? "nothing is staged" : error;
		return result;
	}
	if (!verifyStagedUpdate(staged, error))
	{
		result.error = "the staged build failed verification: " + error;
		return result;
	}

	const fs::path currentExe = currentExecutablePath();
	if (currentExe.empty()) { result.error = "could not determine the running executable's path"; return result; }

	const fs::path installRoot = installRootForRunningBuild(currentExe, staged.platform);
	if (installRoot.empty()) { result.error = "the running build is not laid out the way '" + staged.platform + "' expects"; return result; }

	const fs::path updaterSrc = currentExe.parent_path() / updaterExeName();
	std::error_code ec;
	if (!fs::is_regular_file(updaterSrc, ec))
	{
		result.error = "HorizonUpdater is missing next to the editor: " + utf8FromPath(updaterSrc);
		return result;
	}

	// installRoot, payloadRoot and backupRoot must be siblings on one volume (see
	// SelfUpdateApplyPlan.h) — workRoot lives right beside the install for exactly
	// that reason, named uniquely so a retried takeover never collides with a
	// still-running or half-cleaned-up earlier one.
	const std::string suffix = staged.version + "-" + randomSuffix();
	const fs::path workRoot = installRoot.parent_path() / (".heselfupdate-" + suffix);
	fs::create_directories(workRoot, ec);

	const auto cleanupAndFail = [&](std::string err) {
		std::error_code rec;
		fs::remove_all(workRoot, rec);
		result.ok    = false;
		result.error = std::move(err);
		return result;
	};

	const ExtractResult extracted = extractStagedBuild(staged.filePath, workRoot / "payload", staged.platform);
	if (!extracted.ok) return cleanupAndFail("could not lay out the staged build: " + extracted.error);

	const fs::path updaterTmp = fs::temp_directory_path(ec) / ("HorizonUpdater-" + suffix + (
#if defined(_WIN32)
		".exe"
#else
		""
#endif
	));
	fs::copy_file(updaterSrc, updaterTmp, fs::copy_options::overwrite_existing, ec);
	if (ec) return cleanupAndFail("could not copy HorizonUpdater to a temp location: " + ec.message());
#if !defined(_WIN32)
	fs::permissions(updaterTmp,
	                 fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
	                 fs::perms::others_read | fs::perms::others_exec,
	                 fs::perm_options::add, ec);
#endif

	ApplyPlan plan;
	plan.waitPid       = currentProcessId();
	plan.waitTimeoutMs = options.waitTimeoutMs;
	plan.installRoot   = utf8FromPath(installRoot);
	plan.payloadRoot   = utf8FromPath(extracted.payloadRoot);
	plan.backupRoot    = utf8FromPath(workRoot / "backup");
	plan.mainExe       = mainExecutableRelativePath(staged.platform);
	plan.preserve      = preserveListFor(staged.platform);
	plan.fromVersion   = runningBuild().version;
	plan.toVersion     = staged.version;
	plan.launcher      = startsWith(staged.platform, "macos") ? "open" : "exec";
	plan.relaunchArgs  = options.relaunchArgs;
	plan.healthMs      = options.healthMs;
	plan.renameRetryMs = options.renameRetryMs;
	plan.resultFile    = utf8FromPath(options.stagingRoot / "last-apply.json");
	plan.logFile       = utf8FromPath(options.stagingRoot / "selfupdate-apply.log");

	SpawnOptions so;
	so.exe           = updaterTmp;
	so.args          = plan.toArgs();
	so.cwd           = fs::temp_directory_path(ec);
	so.waitForExitMs = 500;   // a crash before it even reaches the wait loop (missing dependency, bad arg)
	const SpawnResult started = spawnDetached(so);
	if (!started.ok) return cleanupAndFail("could not start HorizonUpdater: " + started.error);
	if (started.exited)
		return cleanupAndFail("HorizonUpdater exited immediately (code " + std::to_string(started.exitCode) + ") -- nothing was touched");

	result.ok = true;
	return result;
}

PruneResult pruneTakeoverLeftovers(const PruneOptions& options)
{
	PruneResult result;
	if (options.installRoot.empty()) return result;
	const fs::path parent = options.installRoot.parent_path();

	struct WorkDir { fs::path path; fs::file_time_type mtime; bool hasBackup = false; };
	std::vector<WorkDir> dirs;
	std::error_code ec;
	for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec))
	{
		const fs::path p = it->path();
		std::error_code sec;
		// Never follow a link out of the install's parent directory.
		if (!startsWith(utf8FromPath(p.filename()), ".heselfupdate-") ||
		    !fs::is_directory(fs::symlink_status(p, sec)))
			continue;
		WorkDir d;
		d.path      = p;
		d.mtime     = fs::last_write_time(p, sec);
		if (sec) continue;
		d.hasBackup = fs::is_directory(p / "backup", sec);
		dirs.push_back(std::move(d));
	}
	if (dirs.empty()) return result;

	const WorkDir* keep = nullptr;
	ApplyResult last;
	std::string readErr;
	if (!options.lastApplyFile.empty() && readApplyResult(utf8FromPath(options.lastApplyFile), last, readErr) &&
	    last.status == ApplyStatus::Applied && last.toVersion == options.runningVersion && !last.backupRoot.empty())
	{
		const fs::path named = pathFromUtf8(last.backupRoot).parent_path();
		for (const WorkDir& d : dirs)
		{
			std::error_code eec;
			if (d.hasBackup && fs::equivalent(d.path, named, eec)) { keep = &d; break; }
		}
	}
	if (!keep)
		for (const WorkDir& d : dirs)
			if (d.hasBackup && (!keep || d.mtime > keep->mtime)) keep = &d;
	if (keep) result.kept = utf8FromPath(keep->path);

	const fs::file_time_type now = fs::file_time_type::clock::now();
	for (const WorkDir& d : dirs)
	{
		if (&d == keep) continue;
		if (now - d.mtime < options.grace)
		{
			result.young.push_back(utf8FromPath(d.path));
			continue;
		}
		std::error_code rec;
		fs::remove_all(d.path, rec);
		if (rec) result.errors.push_back(utf8FromPath(d.path) + ": " + rec.message());
		else     result.removed.push_back(utf8FromPath(d.path));
	}
	return result;
}

PruneResult pruneTakeoverLeftoversForRunningBuild(const fs::path& stagingRoot)
{
	const LocalBuild running = runningBuild();
	const fs::path currentExe = currentExecutablePath();
	if (currentExe.empty()) return {};
	PruneOptions o;
	o.installRoot    = installRootForRunningBuild(currentExe, running.platform);
	o.lastApplyFile  = stagingRoot.empty() ? fs::path() : stagingRoot / "last-apply.json";
	o.runningVersion = running.version;
	return pruneTakeoverLeftovers(o);
}

} // namespace HE::Su
