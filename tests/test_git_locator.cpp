#include "doctest.h"

#include <SourceControl/GitInstall.h>
#include <SourceControl/GitLocator.h>
#include <SourceControl/GitProbe.h>
#include <Platform/Process.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

// The git locator and the install-route table. Everything here runs with a PATH
// passed as an argument and a fake "which tools exist" set: the real process
// PATH is never touched, because other test files in this binary run git.

using namespace HE::Sc;
namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kGit = "git.exe";
#else
constexpr const char* kGit = "git";
#endif

struct ScratchDir
{
	fs::path root;
	ScratchDir()
	{
		root = fs::temp_directory_path() /
		       ("he_git_locator_" + std::to_string(
		            std::chrono::steady_clock::now().time_since_epoch().count()));
		fs::create_directories(root);
	}
	~ScratchDir() { std::error_code ec; fs::remove_all(root, ec); }

	fs::path dir(const char* name) const
	{
		fs::path d = root / name;
		fs::create_directories(d);
		return d;
	}

	// A file named like git; `executable` only matters on POSIX.
	static fs::path makeFile(const fs::path& dir, const char* name, bool executable = true)
	{
		const fs::path f = dir / name;
		std::ofstream(f) << "#!/bin/sh\n";
		fs::permissions(f, executable ? fs::perms::owner_all
		                              : (fs::perms::owner_read | fs::perms::owner_write),
		                fs::perm_options::replace);
		return f;
	}
};

GitInstallPlan planWith(HostPlatform platform, std::set<std::string> tools, bool isRoot = false)
{
	return planGitInstall(platform,
	                      [&tools](std::string_view t) { return tools.count(std::string(t)) != 0; },
	                      isRoot);
}

} // namespace

// ─── PATH handling ───────────────────────────────────────────────────────────

TEST_CASE("splitPathList drops empty entries and strips quotes")
{
	const auto win = splitPathList(R"(C:\Windows;;"C:\Program Files\Git\cmd"; C:\Tools ;)", ';');
	REQUIRE(win.size() == 3);
	CHECK(win[0] == fs::path(R"(C:\Windows)"));
	// Windows accepts a quoted PATH entry; the quotes are not part of the folder.
	CHECK(win[1] == fs::path(R"(C:\Program Files\Git\cmd)"));
	CHECK(win[2] == fs::path(R"(C:\Tools)"));

	const auto posix = splitPathList("/usr/local/bin::/usr/bin:", ':');
	REQUIRE(posix.size() == 2);
	CHECK(posix[0] == fs::path("/usr/local/bin"));
	CHECK(posix[1] == fs::path("/usr/bin"));

	CHECK(splitPathList("", ';').empty());
	CHECK(splitPathList(";;;", ';').empty());
}

TEST_CASE("findExecutableInDirs takes the first real file, in PATH order")
{
	ScratchDir s;
	const fs::path empty  = s.dir("empty");
	const fs::path first  = s.dir("first");
	const fs::path second = s.dir("second");
	const fs::path decoy  = s.dir("decoy");
	ScratchDir::makeFile(first, kGit);
	ScratchDir::makeFile(second, kGit);
	// A directory that merely has git's name is not git.
	fs::create_directories(decoy / kGit);

	const auto hit = findExecutableInDirs({ empty, decoy, first, second }, kGit);
	REQUIRE(hit.has_value());
	CHECK(*hit == first / kGit);

	// Order is the caller's: the same folders the other way round give the other git.
	const auto reversed = findExecutableInDirs({ second, first }, kGit);
	REQUIRE(reversed.has_value());
	CHECK(*reversed == second / kGit);

	CHECK_FALSE(findExecutableInDirs({ empty, decoy, s.root / "does-not-exist" }, kGit).has_value());
	CHECK_FALSE(findExecutableInDirs({}, kGit).has_value());
}

#ifndef _WIN32
TEST_CASE("findExecutableInDirs skips a git without the execute bit")
{
	ScratchDir s;
	const fs::path noExec = s.dir("noexec");
	const fs::path real   = s.dir("real");
	ScratchDir::makeFile(noExec, kGit, /*executable=*/false);
	ScratchDir::makeFile(real, kGit);

	const auto hit = findExecutableInDirs({ noExec, real }, kGit);
	REQUIRE(hit.has_value());
	CHECK(*hit == real / kGit);
}
#endif

TEST_CASE("A PATH string with a git in the middle resolves to that git")
{
	ScratchDir s;
	const fs::path a = s.dir("a");
	const fs::path b = s.dir("b");
	ScratchDir::makeFile(b, kGit);
#ifdef _WIN32
	const char sep = ';';
#else
	const char sep = ':';
#endif
	const std::string list = a.string() + sep + sep + "\"" + b.string() + "\"" + sep;
	const auto hit = findExecutableInDirs(splitPathList(list, sep), kGit);
	REQUIRE(hit.has_value());
	CHECK(*hit == b / kGit);
}

// ─── Is it really git? ───────────────────────────────────────────────────────

TEST_CASE("The macOS /usr/bin/git shim is only a stub without the developer tools")
{
	const fs::path shim("/usr/bin/git");
	const fs::path brewGit("/opt/homebrew/bin/git");

	CHECK(classifyGitBinary(HostPlatform::MacOS, std::nullopt, false) == GitBinaryState::NotFound);
	CHECK(classifyGitBinary(HostPlatform::MacOS, shim, false)         == GitBinaryState::Stub);
	CHECK(classifyGitBinary(HostPlatform::MacOS, shim, true)          == GitBinaryState::Runnable);
	// A Homebrew git is real whether or not the Command Line Tools are there.
	CHECK(classifyGitBinary(HostPlatform::MacOS, brewGit, false)      == GitBinaryState::Runnable);
	// /usr/bin/git on Linux is the distro's git, never a shim.
	CHECK(classifyGitBinary(HostPlatform::Linux, shim, false)         == GitBinaryState::Runnable);
	CHECK(classifyGitBinary(HostPlatform::Windows, fs::path(R"(C:\Program Files\Git\cmd\git.exe)"), false)
	      == GitBinaryState::Runnable);
}

// ─── Install routes ──────────────────────────────────────────────────────────

TEST_CASE("Windows: winget when present, otherwise the downloadable installer")
{
	const GitInstallPlan w = planWith(HostPlatform::Windows, { "winget" });
	CHECK(w.route == GitInstallRoute::Winget);
	REQUIRE(w.available.size() == 2);
	CHECK(w.available[1] == GitInstallRoute::DownloadInstaller);
	CHECK(w.elevation == GitInstallElevation::OsPrompt);
	CHECK(w.bringsLfs);   // Git for Windows bundles git-lfs
	CHECK(w.installCommand.find("Git.Git") != std::string::npos);
	CHECK(w.canInstallAutomatically());

	const GitInstallPlan d = planWith(HostPlatform::Windows, {});
	CHECK(d.route == GitInstallRoute::DownloadInstaller);
	REQUIRE(d.available.size() == 1);
	CHECK(d.installCommand.empty());
	CHECK(d.bringsLfs);
	CHECK(d.canInstallAutomatically());
	CHECK_FALSE(d.reason.empty());

	// Linux package managers mean nothing on Windows.
	CHECK(planWith(HostPlatform::Windows, { "apt-get", "pkexec" }).route == GitInstallRoute::DownloadInstaller);
}

TEST_CASE("macOS: Homebrew before the Command Line Tools, neither means no route")
{
	const GitInstallPlan b = planWith(HostPlatform::MacOS, { "brew", "xcode-select" });
	CHECK(b.route == GitInstallRoute::Homebrew);
	REQUIRE(b.available.size() == 2);
	CHECK(b.available[1] == GitInstallRoute::XcodeCommandLineTools);
	CHECK(b.elevation == GitInstallElevation::NotNeeded);
	CHECK(b.installCommand == "brew install git");
	CHECK_FALSE(b.bringsLfs);

	const GitInstallPlan x = planWith(HostPlatform::MacOS, { "xcode-select" });
	CHECK(x.route == GitInstallRoute::XcodeCommandLineTools);
	CHECK(x.elevation == GitInstallElevation::OsPrompt);
	CHECK(x.installCommand == "xcode-select --install");
	CHECK(x.canInstallAutomatically());

	const GitInstallPlan none = planWith(HostPlatform::MacOS, {});
	CHECK(none.route == GitInstallRoute::None);
	CHECK(none.available.empty());
	CHECK_FALSE(none.canInstallAutomatically());
	CHECK_FALSE(none.reason.empty());
}

TEST_CASE("Linux: every package manager, with pkexec, as root, and with neither")
{
	struct Case { const char* tool; GitInstallRoute route; const char* command; };
	const Case cases[] = {
		{ "apt-get", GitInstallRoute::Apt,    "apt-get update && apt-get install -y git" },
		{ "dnf",     GitInstallRoute::Dnf,    "dnf install -y git" },
		{ "pacman",  GitInstallRoute::Pacman, "pacman -S --needed --noconfirm git" },
		{ "zypper",  GitInstallRoute::Zypper, "zypper --non-interactive install git" },
	};
	for (const Case& c : cases)
	{
		CAPTURE(std::string(c.tool));

		const GitInstallPlan pk = planWith(HostPlatform::Linux, { c.tool, "pkexec" });
		CHECK(pk.route == c.route);
		CHECK(pk.installCommand == c.command);
		CHECK(pk.elevation == GitInstallElevation::Pkexec);
		CHECK(pk.canInstallAutomatically());
		CHECK_FALSE(pk.bringsLfs);
		// The terminal fallback needs sudo on every part of the command.
		CHECK(pk.manualCommand.rfind("sudo ", 0) == 0);

		const GitInstallPlan root = planWith(HostPlatform::Linux, { c.tool }, /*isRoot=*/true);
		CHECK(root.elevation == GitInstallElevation::AlreadyRoot);
		CHECK(root.canInstallAutomatically());
		CHECK(root.manualCommand == c.command);   // no sudo for root

		// The fallback case: a package manager, but no way to get rights.
		const GitInstallPlan stuck = planWith(HostPlatform::Linux, { c.tool });
		CHECK(stuck.route == c.route);
		CHECK(stuck.elevation == GitInstallElevation::Unavailable);
		CHECK_FALSE(stuck.canInstallAutomatically());
		CHECK(stuck.manualCommand.find("sudo") != std::string::npos);
		CHECK_FALSE(stuck.reason.empty());
	}

	const GitInstallPlan apt = planWith(HostPlatform::Linux, { "apt-get", "pkexec" });
	CHECK(apt.manualCommand == "sudo apt-get update && sudo apt-get install -y git");
}

TEST_CASE("Linux: the first known manager wins, all are listed, none is reported")
{
	const GitInstallPlan many = planWith(HostPlatform::Linux, { "zypper", "dnf", "pkexec" });
	CHECK(many.route == GitInstallRoute::Dnf);
	REQUIRE(many.available.size() == 2);
	CHECK(many.available[0] == GitInstallRoute::Dnf);
	CHECK(many.available[1] == GitInstallRoute::Zypper);

	const GitInstallPlan none = planWith(HostPlatform::Linux, { "pkexec" }, true);
	CHECK(none.route == GitInstallRoute::None);
	CHECK(none.available.empty());
	CHECK(none.installCommand.empty());
	CHECK_FALSE(none.canInstallAutomatically());
	CHECK(none.reason.find("package manager") != std::string::npos);
}

TEST_CASE("gitInstallRouteName covers every route")
{
	const GitInstallRoute all[] = {
		GitInstallRoute::None, GitInstallRoute::Winget, GitInstallRoute::DownloadInstaller,
		GitInstallRoute::Homebrew, GitInstallRoute::XcodeCommandLineTools, GitInstallRoute::Apt,
		GitInstallRoute::Dnf, GitInstallRoute::Pacman, GitInstallRoute::Zypper,
	};
	std::set<std::string> names;
	for (GitInstallRoute r : all) names.insert(gitInstallRouteName(r));
	CHECK(names.size() == std::size(all));
}

// ─── From plan to installer process (GitInstall.h) ───────────────────────────
// Only the pure half: runGitInstall would really install git, so it is run here
// only for plans that must NOT start anything.

TEST_CASE("Install launch: winget gets the agreement flags so it never waits on stdin")
{
	const auto l = gitInstallLaunch(planWith(HostPlatform::Windows, { "winget" }));
	REQUIRE(l.has_value());
	CHECK(l->exe == fs::path("winget"));
	const std::vector<std::string> want = {
		"install", "--id", "Git.Git", "-e", "--source", "winget",
		"--accept-source-agreements", "--accept-package-agreements" };
	CHECK(l->args == want);
	CHECK_FALSE(l->returnsBeforeInstalled);
	CHECK_FALSE(l->userPrompt.empty());   // UAC: the user is told before it pops up
	CHECK(l->display.rfind("winget install --id Git.Git", 0) == 0);

	// No winget: the downloadable installer is a route, but not one the engine
	// runs — the UI opens the download page instead.
	const GitInstallPlan d = planWith(HostPlatform::Windows, {});
	CHECK(d.route == GitInstallRoute::DownloadInstaller);
	CHECK_FALSE(gitInstallLaunch(d).has_value());
}

TEST_CASE("Install launch: Homebrew runs as the user, the Command Line Tools are waited for")
{
	const auto brew = gitInstallLaunch(planWith(HostPlatform::MacOS, { "brew", "xcode-select" }));
	REQUIRE(brew.has_value());
	CHECK(brew->exe == fs::path("brew"));
	CHECK(brew->args == std::vector<std::string>{ "install", "git" });
	CHECK_FALSE(brew->returnsBeforeInstalled);
	CHECK(brew->userPrompt.empty());

	const auto clt = gitInstallLaunch(planWith(HostPlatform::MacOS, { "xcode-select" }));
	REQUIRE(clt.has_value());
	CHECK(clt->exe == fs::path("xcode-select"));
	CHECK(clt->args == std::vector<std::string>{ "--install" });
	CHECK(clt->returnsBeforeInstalled);   // exits at once; git comes minutes later
	CHECK_FALSE(clt->userPrompt.empty());

	CHECK_FALSE(gitInstallLaunch(planWith(HostPlatform::MacOS, {})).has_value());
}

TEST_CASE("Install launch: Linux goes through pkexec sh -c with the whole line as ONE argument")
{
	const auto apt = gitInstallLaunch(planWith(HostPlatform::Linux, { "apt-get", "pkexec" }));
	REQUIRE(apt.has_value());
	CHECK(apt->exe == fs::path("pkexec"));
	REQUIRE(apt->args.size() == 3);
	CHECK(apt->args[0] == "/bin/sh");
	CHECK(apt->args[1] == "-c");
	// `&&` stays inside the shell line; pkexec <cmd> would hand it to apt-get.
	CHECK(apt->args[2] == "export DEBIAN_FRONTEND=noninteractive; "
	                      "apt-get update && apt-get install -y git");
	CHECK_FALSE(apt->userPrompt.empty());   // the polkit password dialog

	const auto dnf = gitInstallLaunch(planWith(HostPlatform::Linux, { "dnf", "pkexec" }));
	REQUIRE(dnf.has_value());
	CHECK(dnf->args.back() == "dnf install -y git");   // the debconf variable is apt's alone

	const auto root = gitInstallLaunch(planWith(HostPlatform::Linux, { "pacman" }, /*isRoot=*/true));
	REQUIRE(root.has_value());
	CHECK(root->exe == fs::path("/bin/sh"));
	CHECK(root->args == std::vector<std::string>{ "-c", "pacman -S --needed --noconfirm git" });
	CHECK(root->userPrompt.empty());

	// The fallback cases: a package manager without a way to get rights, and
	// no package manager at all. Neither starts anything.
	CHECK_FALSE(gitInstallLaunch(planWith(HostPlatform::Linux, { "zypper" })).has_value());
	CHECK_FALSE(gitInstallLaunch(planWith(HostPlatform::Linux, { "pkexec" })).has_value());
}

TEST_CASE("runGitInstall without a runnable route explains itself and starts nothing")
{
	for (const GitInstallPlan& plan : { planWith(HostPlatform::Linux, { "apt-get" }),
	                                    planWith(HostPlatform::Windows, {}),
	                                    GitInstallPlan{} })
	{
		std::vector<std::string> lines;
		const GitInstallOutcome o =
			runGitInstall(plan, [&lines](const std::string& l) { lines.push_back(l); });
		CHECK_FALSE(o.launched);
		CHECK_FALSE(o.gitRunnable);
		CHECK_FALSE(o.message.empty());
		// The reason reaches the visible log too, not only the return value.
		REQUIRE(lines.size() == 1);
		CHECK(lines[0] == o.message);
	}
	for (HostPlatform p : { HostPlatform::Windows, HostPlatform::MacOS, HostPlatform::Linux })
		CHECK(std::string(gitDownloadPageUrl(p)).rfind("https://git-scm.com/", 0) == 0);
}

// ─── This machine ────────────────────────────────────────────────────────────

TEST_CASE("The locator finds the git the OS finds, and the probe agrees")
{
	invalidateGitLocation();
	const auto located = gitExecutable();
	const auto osGit   = HE::Proc::which("git");

	// Anything the OS finds by name, the locator finds too (it may find more:
	// on Windows also the registry PATH and Git's install folders).
	if (osGit.has_value())
	{
		REQUIRE(located.has_value());
		CHECK(located->is_absolute());
		CHECK(gitLaunchPath() == *located);
	}
	if (!located.has_value())
	{
		CHECK(gitLaunchPath() == fs::path("git"));
		MESSAGE("git is not installed on this machine — locator smoke test skipped");
		return;
	}

	// Cached: a second call returns the same answer without looking again.
	CHECK(gitExecutable() == located);

	const GitProbe p = probeGit();
	MESSAGE("git " << p.gitVersion << " at " << located->string() << "; install route here: "
	        << std::string(gitInstallRouteName(p.install.route)) << " ("<< p.install.reason << ")");
	CHECK(p.gitPath == *located);
	if (p.gitFound) CHECK(p.gitBinary == GitBinaryState::Runnable);
	// The plan is filled even when git is present: it is what LFS will reuse.
	if (currentPlatform() == HostPlatform::Windows)
		CHECK(p.install.route != GitInstallRoute::None);
}
