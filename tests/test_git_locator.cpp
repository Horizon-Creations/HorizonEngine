#include "doctest.h"

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
