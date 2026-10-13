#pragma once

// ─── Where is git, and how would this machine get it? ────────────────────────
// The single place the engine asks "which git do I run?". Every launch of git in
// the editor (GitCli, the credential calls, GitProbe, the identity writer) takes
// its executable from gitExecutable() instead of handing the bare word "git" to
// the OS, so there is exactly one answer and one spot to change it.
//
// Why a path and not a bool: on Windows a freshly installed Git is invisible to
// a process that was already running. The new PATH entry lands in the registry,
// the editor keeps the environment block it started with (augmentToolPath is a
// no-op there), and "git" by name keeps failing until a restart. Resolving an
// absolute path from the registry PATH and Git for Windows' known install
// folders is what lets the original action run straight after an install.
//
// The second half answers what an install would go through, per platform —
// winget or the downloadable Git for Windows installer; Homebrew or the Xcode
// Command Line Tools; apt/dnf/pacman/zypper behind pkexec. This module only
// REPORTS. It never installs anything and never asks the user; that is the
// editor's job (see docs/git-auto-install-callsites-2026-10-13.md, section 3).

#include "SourceControl/ScCommon.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace HE::Sc {

// ── The executable ───────────────────────────────────────────────────────────

// The absolute path of the git to launch, or nothing when there is none.
// Cached after the first call and thread-safe. NEVER starts a process — only
// PATH, the registry and file-existence checks — because it is reached from the
// frame thread (SourceControlPanel's branch-name check runs git there).
HE_SC_API std::optional<std::filesystem::path> gitExecutable();

// Forget the cached answer, so the next gitExecutable() looks again. Called by
// probeGit() (which is what Recheck runs), after an install, and when a located
// git suddenly cannot be started (it was uninstalled underneath the editor).
HE_SC_API void invalidateGitLocation();

// What a launch should put in Proc::Options::exe: the located path, or the bare
// word "git" when nothing was located — so a git the locator does not know
// about but the OS loader would still find keeps working exactly as before.
HE_SC_API std::filesystem::path gitLaunchPath();

// ── Pure helpers, exposed so the lookup is testable without touching PATH ─────

// A PATH-style list split into directories: empty entries dropped, surrounding
// double quotes removed (Windows tolerates `"C:\Program Files\Git\cmd"` in PATH).
HE_SC_API std::vector<std::filesystem::path> splitPathList(std::string_view list, char separator);

// The first `<dir>/<fileName>` that is a regular file the platform could run
// (on POSIX: has an execute bit for this user). Directories named like the file
// and broken entries are skipped. Order is the order of `dirs`.
HE_SC_API std::optional<std::filesystem::path>
findExecutableInDirs(const std::vector<std::filesystem::path>& dirs, std::string_view fileName);

// ── Is the binary that was found actually git? ───────────────────────────────

enum class GitBinaryState
{
	NotFound,   // nothing named git anywhere we look
	Stub,       // macOS /usr/bin/git without Command Line Tools: an xcrun shim
	            // that fails, and may open Apple's installer when run. Not run.
	Runnable,   // found, not a known stub — `git --version` decides the rest
};

enum class HostPlatform { Windows, MacOS, Linux };

HE_SC_API HostPlatform currentPlatform();

// `developerToolsPresent`: the Command Line Tools or Xcode are installed (on
// macOS that is what makes /usr/bin/git real). Ignored on other platforms. Only
// /usr/bin/git is the shim — a Homebrew git under /opt/homebrew is always real.
HE_SC_API GitBinaryState classifyGitBinary(HostPlatform platform,
                                           const std::optional<std::filesystem::path>& found,
                                           bool developerToolsPresent);

// ── How would git be installed here? ─────────────────────────────────────────

enum class GitInstallRoute
{
	None,               // nothing known on this machine
	Winget,             // winget install --id Git.Git -e  (brings Git LFS along)
	DownloadInstaller,  // the Git for Windows installer from git-scm.com; the engine
	                    // ships none, so this needs a download (brings Git LFS)
	Homebrew,           // brew install git
	XcodeCommandLineTools, // xcode-select --install (Apple's own GUI installer)
	Apt,                // apt-get install -y git
	Dnf,                // dnf install -y git
	Pacman,             // pacman -S --needed --noconfirm git
	Zypper,             // zypper --non-interactive install git
};

// How the install gets the rights it needs.
enum class GitInstallElevation
{
	NotNeeded,    // Homebrew installs as the user
	OsPrompt,     // the installer asks itself: UAC on Windows, Apple's dialog on macOS
	Pkexec,       // Linux, a graphical polkit prompt
	AlreadyRoot,  // Linux, the editor runs as root
	Unavailable,  // Linux, neither pkexec nor root: only a terminal `sudo` can do it
};

struct GitInstallPlan
{
	GitInstallRoute     route     = GitInstallRoute::None;   // the preferred route
	GitInstallElevation elevation = GitInstallElevation::NotNeeded;

	// Every route that exists here, preferred first. `route` is front() or None.
	std::vector<GitInstallRoute> available;

	// The command the route runs, without any elevation prefix, and the one line
	// a user would type in a terminal — with `sudo` where it is required. Both
	// empty for DownloadInstaller/None. installCommand can contain `&&` (apt runs
	// update first), so it is a shell line: run it as `pkexec sh -c '<cmd>'`,
	// never as `pkexec <cmd>`, which would hand `&&` to apt-get as an argument.
	std::string installCommand;
	std::string manualCommand;

	// Whether the route installs Git LFS along with git. Git for Windows bundles
	// it; Homebrew, the Command Line Tools and distro `git` packages do not, and
	// GitProbe::ready() requires LFS.
	bool bringsLfs = false;

	// One sentence for the details section and the log: what was found and why
	// this route, or why there is none.
	std::string reason;

	// The engine could run it without the user opening a terminal.
	bool canInstallAutomatically() const
	{
		return route != GitInstallRoute::None && elevation != GitInstallElevation::Unavailable;
	}
};

HE_SC_API const char* gitInstallRouteName(GitInstallRoute route);

// The decision table, with the machine abstracted away: `toolExists` answers
// "is this program available?" for "winget", "brew", "xcode-select", "apt-get",
// "dnf", "pacman", "zypper", "pkexec"; `isRoot` is the effective uid 0 on Linux.
HE_SC_API GitInstallPlan planGitInstall(HostPlatform platform,
                                        const std::function<bool(std::string_view)>& toolExists,
                                        bool isRoot);

// planGitInstall for this machine. Starts no process (lookups only), so it is
// cheap, but it is still meant for the probe's worker thread.
HE_SC_API GitInstallPlan detectGitInstallPlan();

} // namespace HE::Sc
