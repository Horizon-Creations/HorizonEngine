#pragma once

// ─── Installing git when the machine has none ────────────────────────────────
// GitLocator answers which route an install WOULD take (GitInstallPlan); this is
// the half that takes it. Kept apart on purpose: the locator is reached from the
// frame thread and must never start a process, while everything here starts an
// installer, waits minutes for it and belongs on a worker thread.
//
// Never called on its own initiative. The editor runs it when the user presses
// "Install Git" (GitMissingDialog and every place that needs git and shows the
// remedy), and streams every line it emits into a visible log — an install is
// several hundred megabytes and usually a password prompt, and neither may
// happen without the user seeing why.
//
// Which routes run here and which only get a manual hint:
//   Winget                 winget install … Git.Git (UAC prompt from the installer)
//   Homebrew               brew install git (as the user, no prompt)
//   XcodeCommandLineTools  xcode-select --install opens Apple's installer and
//                          returns at once; this then waits until the tools land
//   Apt/Dnf/Pacman/Zypper  pkexec sh -c '<command>' (polkit password dialog), or
//                          sh -c directly when the editor already runs as root
//   DownloadInstaller      not run: the engine ships no Git for Windows installer
//                          and does not download executables on its own. The UI
//                          opens the download page instead.
//   elevation Unavailable  not run: only a terminal `sudo` can do it, and the UI
//                          shows that command.
// Git LFS has its own target (GitInstallTarget::GitLfs) over the same routes, with
// the package named in the plan's lfsInstallCommand: GitHub.GitLFS, git-lfs from
// Homebrew, the distro's git-lfs. There is none for DownloadInstaller (LFS is in the
// installer) and none for the Command Line Tools (Apple ships no LFS); those get the
// manual hint. Git for Windows brings LFS itself, see GitInstallPlan::bringsLfs.

#include "SourceControl/GitLocator.h"
#include "SourceControl/ScCommon.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace HE::Sc {

// What a run installs. The routes are the same for both; only the package differs.
enum class GitInstallTarget { Git, GitLfs };

// The process an install route starts, in argv form — never one shell string, so
// apt's `&&` reaches `sh -c` as one argument instead of being split by pkexec.
struct GitInstallLaunch
{
	// A bare tool name ("winget", "brew", "pkexec"…), resolved through PATH when
	// it runs, so a Homebrew under /opt/homebrew is found from a Finder launch.
	std::filesystem::path    exe;
	std::vector<std::string> args;

	// The line the log shows before it runs ("$ winget install …").
	std::string display;

	// xcode-select --install only starts Apple's GUI installer and exits; git is
	// there only once the user has finished that dialog, minutes later.
	bool returnsBeforeInstalled = false;

	// What the user should expect to see while it runs: the UAC prompt, the
	// polkit password dialog, Apple's installer window. Empty when nothing asks.
	std::string userPrompt;
};

// The launch for a plan, or nothing when the route cannot be run by the engine
// (None, DownloadInstaller, or no way to get administrator rights). Pure: decides
// from the plan alone and starts nothing, so every route is testable anywhere.
HE_SC_API std::optional<GitInstallLaunch> gitInstallLaunch(const GitInstallPlan& plan,
                                                            GitInstallTarget target = GitInstallTarget::Git);

// Where the manual fallback points when the engine cannot install by itself.
HE_SC_API const char* gitDownloadPageUrl(HostPlatform platform);

struct GitInstallOutcome
{
	bool        launched = false;   // the installer process actually started
	int         exitCode = -1;      // its exit code; meaningless unless launched
	bool        gitRunnable = false; // `git --version` ran afterwards — the real verdict
	std::string gitVersion;         // "2.47.1" when gitRunnable
	std::filesystem::path gitPath;  // the git that ran

	// For GitInstallTarget::GitLfs: `git lfs version` ran afterwards (the verdict
	// for that target; gitRunnable stays what it was before the run).
	bool        lfsRunnable = false;
	std::string lfsVersion;         // "3.6.1" when lfsRunnable

	// One sentence for the dialog: what happened, and what to do when it failed.
	std::string message;
};

// Runs plan's route, streams every output line (and the engine's own progress
// lines) through onLine, then forgets the cached git location and checks that
// git now runs. Blocking — minutes, for a download — so a worker thread only.
// `cancel` stops the wait for Apple's installer (raised at editor shutdown); it
// cannot interrupt a package manager that is already running.
HE_SC_API GitInstallOutcome runGitInstall(const GitInstallPlan& plan,
                                          const std::function<void(const std::string&)>& onLine,
                                          const std::atomic<bool>* cancel = nullptr,
                                          GitInstallTarget target = GitInstallTarget::Git);

} // namespace HE::Sc
