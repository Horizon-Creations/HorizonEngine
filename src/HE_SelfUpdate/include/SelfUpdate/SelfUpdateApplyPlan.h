#pragma once

// ─── What the editor tells the updater, and what the updater tells back ──────
// HorizonUpdater is a separate, tiny program: the editor cannot replace its own
// files while it is running (on Windows the loader holds every DLL and the exe),
// and whatever replaces them has to be running from somewhere else. The editor
// copies HorizonUpdater to a temporary directory, starts it detached with the
// plan below on its command line, and quits; the updater waits for the editor's
// process to be gone, swaps the directories, starts the new editor and records
// what happened in a result file the next editor start reads.
//
// Header-only and free of any engine type: HorizonUpdater links no engine
// library (a copy of its executable alone has to run from a temp directory), yet
// the editor builds the command line and reads the result with the same code, so
// the two ends cannot drift apart.
//
// Every path is UTF-8 (see SelfUpdateProcess.h for why that is spelled out).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace HE::Su {

struct ApplyPlan
{
	// The editor process to wait for. 0 = do not wait (tests).
	std::int64_t  waitPid = 0;
	// How long to wait for it. Past that the updater changes NOTHING and does not
	// start anything: an editor that is still running is not one to replace.
	std::uint32_t waitTimeoutMs = 120'000;

	// The installed editor, the prepared new one, and where the old one goes. All
	// three are siblings (same parent directory), so each move is one rename on one
	// volume — atomic, and never a half-copied install.
	std::string installRoot;
	std::string payloadRoot;
	std::string backupRoot;

	// The editor's executable inside a root, '/'-separated: "HorizonEditor.exe",
	// "Contents/MacOS/HorizonEditor". The payload is refused without it.
	std::string mainExe;

	// Names inside installRoot that are the user's, not the build's (config.json
	// and imgui.ini beside a portable exe, the logs). Copied from the old install
	// into the new one unless the new one has its own.
	std::vector<std::string> preserve;

	std::string fromVersion;
	std::string toVersion;

	// "exec": start mainExe as a program (Windows, Linux).
	// "open": hand the bundle to LaunchServices (macOS) — `open -n <bundle> --args ...`.
	std::string launcher = "exec";
	// Passed to the new editor: the project to reopen.
	std::vector<std::string> relaunchArgs;

	// A new editor that EXITS within this window failed to start (a missing DLL, a
	// wrong architecture, a crash on the first line) and the old one is put back.
	// Only meaningful for "exec"; LaunchServices does not report what an app does
	// after it was launched. 0 = do not watch.
	std::uint32_t healthMs = 8'000;
	// How long a rename may be retried when something holds a file in the way (a
	// virus scanner, an indexer, a stray handle). Windows only really needs it.
	std::uint32_t renameRetryMs = 30'000;

	std::string resultFile;   // JSON, see ApplyResult
	std::string logFile;      // appended to; also receives the new editor's stdout/stderr

	std::vector<std::string> toArgs() const
	{
		std::vector<std::string> a{ "apply" };
		auto flag = [&a](const char* name, const std::string& value) { a.push_back(name); a.push_back(value); };
		if (waitPid) flag("--wait-pid", std::to_string(waitPid));
		flag("--wait-timeout-ms",   std::to_string(waitTimeoutMs));
		flag("--install",           installRoot);
		flag("--payload",           payloadRoot);
		flag("--backup",            backupRoot);
		flag("--main",              mainExe);
		for (const std::string& p : preserve) flag("--preserve", p);
		flag("--from",              fromVersion);
		flag("--to",                toVersion);
		flag("--launcher",          launcher);
		flag("--health-ms",         std::to_string(healthMs));
		flag("--rename-retry-ms",   std::to_string(renameRetryMs));
		flag("--result",            resultFile);
		flag("--log",               logFile);
		for (const std::string& r : relaunchArgs) flag("--relaunch-arg", r);
		return a;
	}

	// False with `error` set for an unknown flag, a flag without its value or a
	// number that is not one. args[0] must be "apply".
	static bool fromArgs(const std::vector<std::string>& args, ApplyPlan& out, std::string& error)
	{
		if (args.empty() || args[0] != "apply") { error = "expected the command 'apply'"; return false; }
		ApplyPlan p;
		auto number = [&error](const std::string& flag, const std::string& v, std::uint64_t& n) {
			if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos || v.size() > 18)
			{
				error = flag + " needs a number, got '" + v + "'";
				return false;
			}
			n = std::stoull(v);
			return true;
		};
		for (std::size_t i = 1; i < args.size(); i += 2)
		{
			const std::string& f = args[i];
			if (i + 1 >= args.size()) { error = "missing value for " + f; return false; }
			const std::string& v = args[i + 1];
			std::uint64_t n = 0;
			if      (f == "--wait-pid")        { if (!number(f, v, n)) return false; p.waitPid = static_cast<std::int64_t>(n); }
			else if (f == "--wait-timeout-ms") { if (!number(f, v, n)) return false; p.waitTimeoutMs = static_cast<std::uint32_t>(n); }
			else if (f == "--health-ms")       { if (!number(f, v, n)) return false; p.healthMs = static_cast<std::uint32_t>(n); }
			else if (f == "--rename-retry-ms") { if (!number(f, v, n)) return false; p.renameRetryMs = static_cast<std::uint32_t>(n); }
			else if (f == "--install")         p.installRoot = v;
			else if (f == "--payload")         p.payloadRoot = v;
			else if (f == "--backup")          p.backupRoot  = v;
			else if (f == "--main")            p.mainExe     = v;
			else if (f == "--preserve")        p.preserve.push_back(v);
			else if (f == "--from")            p.fromVersion = v;
			else if (f == "--to")              p.toVersion   = v;
			else if (f == "--launcher")        p.launcher    = v;
			else if (f == "--result")          p.resultFile  = v;
			else if (f == "--log")             p.logFile     = v;
			else if (f == "--relaunch-arg")    p.relaunchArgs.push_back(v);
			else { error = "unknown flag " + f; return false; }
		}
		out = std::move(p);
		return true;
	}
};

// How an apply ended. Written by HorizonUpdater, read by the next editor start.
enum class ApplyStatus
{
	Applied,      // the new build is in place and was started
	RolledBack,   // the new build could not be started; the old one is back and was started
	Aborted,      // nothing was changed (the editor never exited, the plan was refused, a rename kept failing)
	Failed,       // something is in an inconsistent state — `backup` says where the old install is
};

inline const char* applyStatusName(ApplyStatus s)
{
	switch (s)
	{
	case ApplyStatus::Applied:    return "applied";
	case ApplyStatus::RolledBack: return "rolled-back";
	case ApplyStatus::Aborted:    return "aborted";
	case ApplyStatus::Failed:     return "failed";
	}
	return "failed";
}

// What `applyUpdate` (SelfUpdateApplyCore.h) found when it finished, written to
// `ApplyPlan::resultFile` and read back by the next editor start — including an
// Aborted/Failed one, so a user who reopens the OLD editor still sees why the
// update did not happen.
struct ApplyResult
{
	ApplyStatus   status = ApplyStatus::Aborted;
	std::string   error;
	std::string   fromVersion;
	std::string   toVersion;
	std::string   installRoot;   // where the (possibly restored) install ended up
	std::string   backupRoot;    // set while the previous install may still be here
	std::int64_t  newPid = 0;    // the process that was (re)started, 0 if none
};

} // namespace HE::Su
