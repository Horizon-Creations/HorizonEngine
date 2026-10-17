// ─── End-to-end check of the git auto-install on a machine without git ───────
// Not part of the build and not a test: it REALLY installs git when asked to
// (runGitInstall), which no he_tests case may do. It replays what the editor does
// between opening a project and the Source Control panel showing the repository
// again, with the same HorizonSourceControl calls in the same order:
//
//   EditorApplication::startGitProbe     -> probeGit()                (plan + verdict)
//   project open (GitController::open)   -> GitService::open(project) (gitMissing, not "no repo")
//   "Install Git" (startGitInstall)      -> runGitInstall(probe.install), probeGit() again
//   m_gitReopenRepo in the frame loop    -> GitService::requestStatus() while gitMissing()
//
// Usage:  git_install_e2e <path inside a repository> [--install] [--expect-route <name>]
//                         [--expect-elevation <name>]
// Exit 0 when every expectation held, 1 otherwise; each line says what it checked.
// Build and run: docs/git-auto-install-e2e/linux-fresh-distro.sh.

#include "Diagnostics/Log.h"
#include "SourceControl/GitInstall.h"
#include "SourceControl/GitLocator.h"
#include "SourceControl/GitProbe.h"
#include "SourceControl/GitService.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what)
{
	std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
	if (!ok) ++g_failures;
}

const char* elevationName(HE::Sc::GitInstallElevation e)
{
	using E = HE::Sc::GitInstallElevation;
	switch (e)
	{
	case E::NotNeeded:   return "NotNeeded";
	case E::OsPrompt:    return "OsPrompt";
	case E::Pkexec:      return "Pkexec";
	case E::AlreadyRoot: return "AlreadyRoot";
	case E::Unavailable: return "Unavailable";
	}
	return "?";
}

void printProbe(const char* when, const HE::Sc::GitProbe& p)
{
	const HE::Sc::GitInstallPlan& plan = p.install;
	std::printf("── probe %s\n", when);
	std::printf("   gitFound=%d version='%s' path='%s'\n", p.gitFound ? 1 : 0,
	            p.gitVersion.c_str(), p.gitPath.string().c_str());
	std::printf("   route=%s elevation=%s automatic=%d bringsLfs=%d\n",
	            HE::Sc::gitInstallRouteName(plan.route), elevationName(plan.elevation),
	            plan.canInstallAutomatically() ? 1 : 0, plan.bringsLfs ? 1 : 0);
	std::printf("   installCommand='%s'\n   manualCommand='%s'\n   reason='%s'\n",
	            plan.installCommand.c_str(), plan.manualCommand.c_str(), plan.reason.c_str());
	if (const auto l = HE::Sc::gitInstallLaunch(plan))
		std::printf("   launch='%s' prompt='%s'\n", l->display.c_str(), l->userPrompt.c_str());
	else
		std::printf("   launch=<none: manual fallback, download page %s>\n",
		            HE::Sc::gitDownloadPageUrl(HE::Sc::currentPlatform()));
}

// pump() until the worker has answered the last request (or 60 s passed).
void settle(HE::Sc::GitService& svc)
{
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	do
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		svc.pump();
	} while (svc.busy() && std::chrono::steady_clock::now() < until);
	svc.pump();
}

void printService(const char* when, const HE::Sc::GitService& svc)
{
	std::printf("── GitService %s: gitMissing=%d isRepo=%d root='%s' branch='%s' head='%s' err='%s'\n",
	            when, svc.gitMissing() ? 1 : 0, svc.isRepo() ? 1 : 0, svc.status().root.c_str(),
	            svc.status().branch.c_str(), svc.status().headOid.c_str(), svc.lastError().c_str());
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s <path inside a repository> [--install] "
		                     "[--expect-route R] [--expect-elevation E]\n", argv[0]);
		return 2;
	}
	const std::filesystem::path project = argv[1];
	bool install = false;
	std::string expectRoute, expectElevation;
	for (int i = 2; i < argc; ++i)
	{
		if (!std::strcmp(argv[i], "--install")) install = true;
		else if (!std::strcmp(argv[i], "--expect-route") && i + 1 < argc) expectRoute = argv[++i];
		else if (!std::strcmp(argv[i], "--expect-elevation") && i + 1 < argc) expectElevation = argv[++i];
	}

	HE::Log::configureFromString("SourceControl=Debug");
	HE::Log::setConsoleEnabled(true);

	// 1. The probe the editor runs at start.
	const HE::Sc::GitProbe before = HE::Sc::probeGit();
	printProbe("at start", before);
	if (!expectRoute.empty())
		check(expectRoute == HE::Sc::gitInstallRouteName(before.install.route),
		      "route is " + expectRoute);
	if (!expectElevation.empty())
		check(expectElevation == elevationName(before.install.elevation),
		      "elevation is " + expectElevation);

	// 2. The project opens: an existing repository must read as "git missing",
	//    never as "no repository" (which would offer git init).
	HE::Sc::GitService svc;
	svc.open(project);
	settle(svc);
	printService("after open", svc);
	if (!before.gitFound)
		check(svc.gitMissing() && !svc.isRepo(), "open without git reports gitMissing, not 'no repo'");

	if (!install)
		return g_failures ? 1 : 0;

	// 3. "Install Git", exactly as EditorApplication::startGitInstall runs it.
	std::printf("── runGitInstall\n");
	const auto t0 = std::chrono::steady_clock::now();
	const HE::Sc::GitInstallOutcome res = HE::Sc::runGitInstall(
		before.install, [](const std::string& l) { std::printf("   | %s\n", l.c_str()); });
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::printf("   launched=%d exit=%d gitRunnable=%d version='%s' path='%s' (%.1f s)\n"
	            "   message='%s'\n", res.launched ? 1 : 0, res.exitCode, res.gitRunnable ? 1 : 0,
	            res.gitVersion.c_str(), res.gitPath.string().c_str(), secs, res.message.c_str());

	const HE::Sc::GitProbe after = HE::Sc::probeGit();
	printProbe("after install", after);

	// 4. The frame loop's m_gitReopenRepo: a refresh, no reopen, no restart.
	if (after.gitFound && svc.gitMissing()) svc.requestStatus();
	settle(svc);
	printService("after refresh", svc);

	// What the UI's Install Git button keys on too: canInstallAutomatically() is
	// also true for DownloadInstaller, which the engine never runs itself.
	if (HE::Sc::gitInstallLaunch(before.install))
	{
		check(res.launched, "the installer started");
		check(res.gitRunnable && after.gitFound, "git --version runs after the install");
		check(!svc.gitMissing() && svc.isRepo() && !svc.status().branch.empty(),
		      "the original function (repository detection + status) runs without a restart");
	}
	else
	{
		check(!res.launched, "no installer started for a manual-only route");
		check(!res.message.empty(), "the outcome explains why: " + res.message);
		check(svc.gitMissing(), "still reported as gitMissing (no false 'no repo')");
	}
	return g_failures ? 1 : 0;
}
