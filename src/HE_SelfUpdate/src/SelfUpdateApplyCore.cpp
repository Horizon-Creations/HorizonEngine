#include "SelfUpdate/SelfUpdateApplyCore.h"
#include "SelfUpdate/SelfUpdateProcess.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;
using Clock  = std::chrono::steady_clock;

namespace HE::Su {

namespace {

std::string timestampUtc()
{
	const auto now = std::chrono::system_clock::now();
	const std::time_t t = std::chrono::system_clock::to_time_t(now);
	std::tm tmBuf{};
#if defined(_WIN32)
	gmtime_s(&tmBuf, &t);
#else
	gmtime_r(&t, &tmBuf);
#endif
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
	              tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
	              tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
	return buf;
}

// installRoot, payloadRoot and backupRoot are documented as siblings (one
// volume), but a stray filesystem boundary or a file still held open (a virus
// scanner, an indexer) turns a single fs::rename into ERROR_SHARING_VIOLATION /
// EBUSY / EXDEV. Retrying is the only thing that helps with the first two; EXDEV
// (a real cross-volume move) never heals, so it is reported as soon as it is seen.
bool retryRename(const fs::path& from, const fs::path& to, std::uint32_t retryMs, std::string& error)
{
	const auto deadline = Clock::now() + std::chrono::milliseconds(retryMs);
	for (;;)
	{
		std::error_code ec;
		fs::rename(from, to, ec);
		if (!ec) return true;
		error = ec.message();
		if (ec == std::errc::cross_device_link) return false;
		if (Clock::now() >= deadline) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

void copyPreserved(const fs::path& fromRoot, const fs::path& toRoot, const std::vector<std::string>& preserve)
{
	for (const std::string& rel : preserve)
	{
		if (rel.empty() || rel.find("..") != std::string::npos) continue;   // defensive; see Takeover.cpp
		const fs::path src = fromRoot / pathFromUtf8(rel);
		const fs::path dst = toRoot / pathFromUtf8(rel);
		std::error_code ec;
		if (!fs::exists(src, ec) || fs::exists(dst, ec)) continue;   // nothing to carry, or the new build brought its own
		fs::create_directories(dst.parent_path(), ec);
		fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
	}
}

SpawnResult relaunch(const fs::path& root, const std::string& mainExe, const std::string& launcher,
                      const std::vector<std::string>& relaunchArgs, const std::string& logFile)
{
	SpawnOptions so;
	so.cwd     = root;
	so.logFile = logFile.empty() ? fs::path() : pathFromUtf8(logFile);
	if (launcher == "open")
	{
		so.exe = "/usr/bin/open";
		so.args.push_back("-n");
		so.args.push_back(utf8FromPath(root));
		if (!relaunchArgs.empty())
		{
			so.args.push_back("--args");
			for (const std::string& a : relaunchArgs) so.args.push_back(a);
		}
	}
	else
	{
		so.exe  = root / pathFromUtf8(mainExe);
		so.args = relaunchArgs;
	}
	return spawnDetached(so);
}

} // namespace

ApplyResult applyUpdate(const ApplyPlan& plan)
{
	ApplyResult result;
	result.fromVersion = plan.fromVersion;
	result.toVersion   = plan.toVersion;
	result.installRoot = plan.installRoot;
	result.backupRoot  = plan.backupRoot;

	const auto finish = [&](ApplyStatus s, std::string err) {
		result.status = s;
		result.error  = std::move(err);
		if (!plan.resultFile.empty())
		{
			std::string writeErr;
			writeApplyResult(plan.resultFile, result, writeErr);
		}
		appendApplyLog(plan.logFile, std::string("apply finished: ") + applyStatusName(result.status) +
		                             (result.error.empty() ? std::string() : (" -- " + result.error)));
		return result;
	};

	appendApplyLog(plan.logFile, "apply starting: " + plan.fromVersion + " -> " + plan.toVersion +
	                             "  install=" + plan.installRoot + "  payload=" + plan.payloadRoot);

	if (plan.waitPid != 0 && !waitForProcessExit(plan.waitPid, plan.waitTimeoutMs))
		return finish(ApplyStatus::Aborted,
		              "the editor (pid " + std::to_string(plan.waitPid) + ") was still running after " +
		              std::to_string(plan.waitTimeoutMs) + " ms -- nothing touched");

	// From here the editor process is confirmed gone. Any path below that ends
	// without a running process is therefore a user left with nothing to click —
	// `relaunchFromUnchangedInstall` covers every such Aborted/Failed exit by
	// starting whatever currently sits at plan.installRoot before returning.
	const fs::path install = pathFromUtf8(plan.installRoot);
	const fs::path payload = pathFromUtf8(plan.payloadRoot);
	const fs::path backup  = pathFromUtf8(plan.backupRoot);
	const fs::path mainExeRel = pathFromUtf8(plan.mainExe);

	const auto abortButRelaunchInstall = [&](std::string err) {
		SpawnResult r = relaunch(install, plan.mainExe, plan.launcher, plan.relaunchArgs, plan.logFile);
		result.newPid = r.ok ? r.pid : 0;
		if (!r.ok) err += "  (and could not relaunch the existing install either: " + r.error + ")";
		return finish(ApplyStatus::Aborted, err);
	};

	if (plan.mainExe.empty() || mainExeRel.is_absolute() || plan.mainExe.find("..") != std::string::npos)
		return abortButRelaunchInstall("unsafe main-executable path in the plan: '" + plan.mainExe + "'");

	std::error_code ec;
	if (!fs::is_directory(payload, ec) || !fs::is_regular_file(payload / mainExeRel, ec))
		return abortButRelaunchInstall("payload is missing or incomplete: " + plan.payloadRoot);

	if (fs::exists(backup, ec))
		return abortButRelaunchInstall("backup path already exists, refusing to overwrite it: " + plan.backupRoot);

	copyPreserved(install, payload, plan.preserve);

	std::string renameErr;
	if (!retryRename(install, backup, plan.renameRetryMs, renameErr))
		return abortButRelaunchInstall("could not move the current install aside: " + renameErr);

	if (!retryRename(payload, install, plan.renameRetryMs, renameErr))
	{
		std::string restoreErr;
		if (!retryRename(backup, install, plan.renameRetryMs, restoreErr))
			return finish(ApplyStatus::Failed,
			              "could not move the new build into place (" + renameErr + ") AND could not restore the "
			              "old one (" + restoreErr + ") -- the previous install is at " + plan.backupRoot);
		return abortButRelaunchInstall("could not move the new build into place: " + renameErr + " -- old install restored");
	}

	// install now holds the NEW build. A rollback from here puts it back where
	// payload came from (that slot is free again) and restores backup -> install.
	const auto rollback = [&](std::string why) {
		std::error_code mec;
		fs::rename(install, payload, mec);   // best-effort; install is left as-is if this fails too
		std::string restoreErr;
		if (!retryRename(backup, install, plan.renameRetryMs, restoreErr))
			return finish(ApplyStatus::Failed,
			              why + " -- AND could not restore the previous install (" + restoreErr +
			              "); the broken new build may be at " + utf8FromPath(payload) +
			              ", the old one at " + plan.backupRoot);
		SpawnResult r = relaunch(install, plan.mainExe, plan.launcher, plan.relaunchArgs, plan.logFile);
		result.newPid = r.ok ? r.pid : 0;
		std::string note = why + " -- rolled back to the previous install";
		if (!r.ok) note += " (could not relaunch it automatically: " + r.error + ")";
		return finish(ApplyStatus::RolledBack, note);
	};

	SpawnResult newProc;
	if (plan.launcher == "open")
	{
		// LaunchServices does not report what the app does after this call
		// returns, so "open itself ran" is the only health signal there is here.
		newProc = relaunch(install, plan.mainExe, plan.launcher, plan.relaunchArgs, plan.logFile);
		if (!newProc.ok) return rollback("could not launch the new build (" + newProc.error + ")");
	}
	else
	{
		SpawnOptions so;
		so.exe           = install / mainExeRel;
		so.args          = plan.relaunchArgs;
		so.cwd           = install;
		so.logFile       = plan.logFile.empty() ? fs::path() : pathFromUtf8(plan.logFile);
		so.waitForExitMs = plan.healthMs;
		newProc = spawnDetached(so);
		if (!newProc.ok) return rollback("could not start the new build (" + newProc.error + ")");
		if (newProc.exited)
			return rollback("the new build exited within " + std::to_string(plan.healthMs) +
			                 " ms of starting (code " + std::to_string(newProc.exitCode) + ")");
	}

	result.newPid = newProc.pid;
	return finish(ApplyStatus::Applied, "");
}

bool writeApplyResult(const std::string& path, const ApplyResult& r, std::string& error)
{
	if (path.empty()) return true;
	nlohmann::json j;
	j["schema"]      = 1;
	j["status"]      = applyStatusName(r.status);
	j["error"]       = r.error;
	j["fromVersion"] = r.fromVersion;
	j["toVersion"]   = r.toVersion;
	j["installRoot"] = r.installRoot;
	j["backupRoot"]  = r.backupRoot;
	j["newPid"]      = r.newPid;

	const fs::path p = pathFromUtf8(path);
	std::error_code ec;
	if (!p.parent_path().empty()) fs::create_directories(p.parent_path(), ec);

	fs::path tmp = p;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f)
		{
			error = "could not open " + path + ".tmp for writing";
			return false;
		}
		f << j.dump(2);
	}
	fs::rename(tmp, p, ec);
	if (ec)
	{
		error = ec.message();
		return false;
	}
	return true;
}

bool readApplyResult(const std::string& path, ApplyResult& out, std::string& error)
{
	std::ifstream f(pathFromUtf8(path), std::ios::binary);
	if (!f)
	{
		error = "no result file at " + path;
		return false;
	}
	nlohmann::json j;
	try
	{
		f >> j;
	}
	catch (const std::exception& e)
	{
		error = std::string("malformed result file: ") + e.what();
		return false;
	}

	const std::string status = j.value("status", std::string("failed"));
	if (status == "applied")          out.status = ApplyStatus::Applied;
	else if (status == "rolled-back") out.status = ApplyStatus::RolledBack;
	else if (status == "aborted")     out.status = ApplyStatus::Aborted;
	else                              out.status = ApplyStatus::Failed;

	out.error       = j.value("error", std::string());
	out.fromVersion = j.value("fromVersion", std::string());
	out.toVersion   = j.value("toVersion", std::string());
	out.installRoot = j.value("installRoot", std::string());
	out.backupRoot  = j.value("backupRoot", std::string());
	out.newPid      = j.value("newPid", static_cast<std::int64_t>(0));
	return true;
}

void appendApplyLog(const std::string& logFile, const std::string& line)
{
	if (logFile.empty()) return;
	const fs::path p = pathFromUtf8(logFile);
	std::error_code ec;
	if (!p.parent_path().empty()) fs::create_directories(p.parent_path(), ec);
	std::ofstream f(p, std::ios::binary | std::ios::app);
	if (!f) return;
	f << "[" << timestampUtc() << "] " << line << "\n";
}

} // namespace HE::Su
