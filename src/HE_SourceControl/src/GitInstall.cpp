#include "SourceControl/GitInstall.h"
#include "SourceControl/GitProbe.h"

#include "ScLog.h"

#include <Platform/Process.h>

#include <chrono>
#include <cstdint>
#include <thread>

namespace HE::Sc {
namespace {

// A package manager downloading git is minutes; one that has not finished in
// 45 it is stuck on something nobody can see (a lock held by another apt, a
// prompt the flags did not suppress), and the dialog must get its answer back.
constexpr std::uint32_t kInstallTimeoutMs = 45u * 60u * 1000u;

// Apple's installer downloads the whole Command Line Tools (about 700 MB) and
// the user may leave its dialog open for a while before pressing Install.
constexpr auto kAppleInstallerWait = std::chrono::minutes(60);

std::string quotedForDisplay(const std::string& s) { return "'" + s + "'"; }

// The Command Line Tools' git, which is what xcode-select --install produces.
// A file check only: running /usr/bin/git before it exists would be the shim.
bool commandLineToolsGitPresent()
{
	std::error_code ec;
	return std::filesystem::exists("/Library/Developer/CommandLineTools/usr/bin/git", ec);
}

std::string trimmedLine(const std::string& s)
{
	std::size_t e = s.size();
	while (e > 0 && (s[e - 1] == '\n' || s[e - 1] == '\r' || s[e - 1] == ' ')) --e;
	return s.substr(0, e);
}

} // namespace

const char* gitDownloadPageUrl(HostPlatform platform)
{
	switch (platform)
	{
	case HostPlatform::Windows: return "https://git-scm.com/download/win";
	case HostPlatform::MacOS:   return "https://git-scm.com/download/mac";
	case HostPlatform::Linux:   return "https://git-scm.com/download/linux";
	}
	return "https://git-scm.com/downloads";
}

std::optional<GitInstallLaunch> gitInstallLaunch(const GitInstallPlan& plan, GitInstallTarget target)
{
	if (!plan.canInstallAutomatically()) return std::nullopt;
	const bool lfs = target == GitInstallTarget::GitLfs;
	// No package the plan can name (the Windows installer, the Command Line
	// Tools) means no automatic route for LFS either.
	if (lfs && plan.lfsInstallCommand.empty()) return std::nullopt;

	GitInstallLaunch l;
	switch (plan.route)
	{
	case GitInstallRoute::Winget:
		// The two --accept flags are what keep winget from stopping at its first
		// run to ask about the source agreement — on stdin, which nobody sees.
		l.exe  = "winget";
		l.args = { "install", "--id", lfs ? "GitHub.GitLFS" : "Git.Git", "-e", "--source", "winget",
		           "--accept-source-agreements", "--accept-package-agreements" };
		l.userPrompt = lfs ? "Windows may ask for permission (UAC) while Git LFS installs."
		                   : "Windows may ask for permission (UAC) while Git for Windows installs.";
		break;

	case GitInstallRoute::Homebrew:
		l.exe  = "brew";
		l.args = { "install", lfs ? "git-lfs" : "git" };
		break;

	case GitInstallRoute::XcodeCommandLineTools:
		if (lfs) return std::nullopt;
		l.exe  = "xcode-select";
		l.args = { "--install" };
		l.returnsBeforeInstalled = true;
		l.userPrompt = "Apple's installer opens in its own window. Choose Install there; "
		               "the editor waits until it has finished.";
		break;

	case GitInstallRoute::Apt:
	case GitInstallRoute::Dnf:
	case GitInstallRoute::Pacman:
	case GitInstallRoute::Zypper:
	{
		const std::string& command = lfs ? plan.lfsInstallCommand : plan.installCommand;
		if (command.empty()) return std::nullopt;
		// pkexec starts its program with a clean environment, so this has to be
		// set inside the shell line: without it a debconf question would wait on
		// a terminal that does not exist until the timeout.
		std::string line = command;
		if (plan.route == GitInstallRoute::Apt)
			line = "export DEBIAN_FRONTEND=noninteractive; " + line;

		// One argument for sh -c, never pkexec <cmd>: apt's line has `&&`.
		if (plan.elevation == GitInstallElevation::Pkexec)
		{
			l.exe  = "pkexec";
			l.args = { "/bin/sh", "-c", line };
			l.userPrompt = "A system dialog asks for an administrator password.";
		}
		else if (plan.elevation == GitInstallElevation::AlreadyRoot)
		{
			l.exe  = "/bin/sh";
			l.args = { "-c", line };
		}
		else
		{
			return std::nullopt;
		}
		break;
	}

	case GitInstallRoute::DownloadInstaller:
	case GitInstallRoute::None:
		return std::nullopt;
	}

	l.display = l.exe.string();
	for (const std::string& a : l.args)
		l.display += " " + (a.find(' ') != std::string::npos ? quotedForDisplay(a) : a);
	return l;
}

GitInstallOutcome runGitInstall(const GitInstallPlan& plan,
                                const std::function<void(const std::string&)>& onLine,
                                const std::atomic<bool>* cancel,
                                GitInstallTarget target)
{
	GitInstallOutcome out;
	const auto emit = [&](const std::string& line) { if (onLine) onLine(line); };
	const bool lfs = target == GitInstallTarget::GitLfs;
	const std::string what = lfs ? "Git LFS" : "git";

	const std::optional<GitInstallLaunch> launch = gitInstallLaunch(plan, target);
	if (!launch)
	{
		out.message = "The editor cannot install " + what + " on this machine by itself (" +
		              plan.reason + ").";
		emit(out.message);
		return out;
	}

	// Resolved now rather than at plan time: the plan may be minutes old, and a
	// Finder-launched editor needs the Homebrew prefixes on PATH to see brew.
	std::filesystem::path exe = launch->exe;
	if (!exe.is_absolute())
	{
		const auto found = HE::Proc::which(exe.string());
		if (!found)
		{
			out.message = exe.string() + " is no longer available, so " + what + " cannot be "
			              "installed automatically.";
			emit(out.message);
			return out;
		}
		exe = *found;
	}

	HE_SC_INFO("Installing %s through %s: %s", what.c_str(), gitInstallRouteName(plan.route),
	           launch->display.c_str());
	emit("Installing " + what + " through " + gitInstallRouteName(plan.route) + ".");
	if (!launch->userPrompt.empty()) emit(launch->userPrompt);
	emit("$ " + launch->display);

	// pkexec exits 127 both when no password dialog could be shown and when
	// polkit refused this account outright; only its own line tells them apart,
	// and the second one is the plain "no administrator rights" case.
	bool pkexecNotAuthorized = false;
	const auto watch = [&](std::string_view l)
	{
		if (l.find("Not authorized") != std::string_view::npos) pkexecNotAuthorized = true;
		emit(std::string(l));
	};

	HE::Proc::Options o;
	o.exe          = exe;
	o.args         = launch->args;
	o.timeoutMs    = kInstallTimeoutMs;
	o.onStdoutLine = watch;
	o.onStderrLine = watch;
	const HE::Proc::Result r = HE::Proc::run(o);

	out.launched = !r.launchFailed;
	out.exitCode = r.exitCode;
	if (r.launchFailed)
	{
		out.message = "The installer could not be started (" + exe.string() + ").";
		emit(out.message);
		HE_SC_WARN("git install: %s could not be started", exe.string().c_str());
		return out;
	}
	if (r.timedOut)
		emit("[stopped: no answer after " + std::to_string(kInstallTimeoutMs / 60000) + " minutes]");
	else
		emit(r.exitCode == 0 ? "[done]" : "[exit code " + std::to_string(r.exitCode) + "]");

	if (launch->returnsBeforeInstalled && !r.timedOut)
	{
		// xcode-select exits at once — 0 when it opened the installer, 1 when the
		// tools are already there. Either way git exists only once the files do.
		emit("Waiting for Apple's installer to finish…");
		const auto until = std::chrono::steady_clock::now() + kAppleInstallerWait;
		while (!commandLineToolsGitPresent() && std::chrono::steady_clock::now() < until)
		{
			if (cancel && cancel->load(std::memory_order_acquire)) break;
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	}

	// The verdict is git itself, not the installer's exit code: winget reports
	// "already installed" as a failure, and a package manager can exit 0 after
	// installing something that is not on any PATH this process can see.
	invalidateGitLocation();
	const auto found = gitExecutable();
	if (lfs)
	{
		// `git lfs version` is what the probe asks, with the PATH repairs it
		// does for it; asking the probe keeps one answer to "is LFS here?".
		const GitProbe after = probeGit();
		out.lfsRunnable = after.lfsFound;
		out.lfsVersion  = after.lfsVersion;
		out.gitRunnable = after.gitFound;
		out.gitVersion  = after.gitVersion;
		out.gitPath     = after.gitPath;
		if (out.lfsRunnable)
		{
			emit("$ git lfs version");
			emit("git-lfs/" + out.lfsVersion);
		}
	}
	else if (found && developerToolsPresent(found))
	{
		HE::Proc::Options v;
		v.exe       = *found;
		v.args      = { "--version" };
		v.timeoutMs = 15000;
		v.env.emplace_back("GIT_TERMINAL_PROMPT", "0");
		const HE::Proc::Result vr = HE::Proc::run(v);
		if (vr.ok())
		{
			out.gitRunnable = true;
			out.gitVersion  = parseGitVersion(vr.out);
			out.gitPath     = *found;
			emit("$ git --version");
			emit(trimmedLine(vr.out));
		}
	}

	if (lfs ? out.lfsRunnable : out.gitRunnable)
	{
		out.message = lfs ? "Git LFS " + out.lfsVersion + " is installed."
		                  : "git " + out.gitVersion + " is installed at " + out.gitPath.string() + ".";
		HE_SC_INFO("%s install finished: %s", what.c_str(), out.message.c_str());
	}
	else
	{
		// The cases that end up here have different fixes, so say which one.
		if (plan.elevation == GitInstallElevation::Pkexec && r.exitCode == 126)
			out.message = "The administrator password dialog was cancelled, so nothing was installed.";
		else if (plan.elevation == GitInstallElevation::Pkexec && r.exitCode == 127 &&
		         pkexecNotAuthorized)
			out.message = "This account is not allowed to install software (pkexec: not "
			              "authorized). Ask an administrator to run the command below in a terminal.";
		else if (plan.elevation == GitInstallElevation::Pkexec && r.exitCode == 127)
			// Also where a translated "Not authorized" lands, so name both causes.
			out.message = "pkexec could not get administrator rights: no password dialog could "
			              "be shown (no authentication agent is running), or this account may "
			              "not install software. Run the command below in a terminal instead.";
		else if (launch->returnsBeforeInstalled)
			out.message = "Apple's installer has not finished. Complete it, then press Recheck.";
		else if (r.timedOut)
			out.message = "The installer did not finish in time. Check it, then press Recheck.";
		else
			out.message = "The installer ended, but " + what + " still does not run. See the log "
			              "above; you can also install " + what + " manually.";
		HE_SC_WARN("%s install did not produce a runnable %s (route %s, exit %d)", what.c_str(),
		           what.c_str(), gitInstallRouteName(plan.route), r.exitCode);
	}
	emit(out.message);
	return out;
}

} // namespace HE::Sc
