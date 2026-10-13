#include "SourceControl/GitProbe.h"

#include "ScLog.h"

#include <Platform/Process.h>

#include <cctype>

namespace HE::Sc {
namespace {

// Every probe call is bounded. `git config` reads local files and is instant,
// but a misconfigured credential helper can block on a network or a keychain
// prompt, and a probe that hangs would freeze the editor's startup check.
constexpr std::uint32_t kProbeTimeoutMs = 5000;

std::string trimmed(const std::string& s)
{
	std::size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
	return s.substr(b, e - b);
}

// The first dotted numeric run in a string. Both tools bury their version in
// prose that differs per platform and per distribution, so pattern-matching the
// number is more robust than matching the surrounding words.
std::string firstVersionNumber(const std::string& text)
{
	for (std::size_t i = 0; i < text.size(); ++i)
	{
		if (!std::isdigit(static_cast<unsigned char>(text[i]))) continue;
		std::size_t j = i;
		bool sawDot = false;
		while (j < text.size() &&
		       (std::isdigit(static_cast<unsigned char>(text[j])) || text[j] == '.'))
		{
			if (text[j] == '.') sawDot = true;
			++j;
		}
		std::string candidate = text.substr(i, j - i);
		// Trim a trailing dot from something like "2.43." so the result is always
		// a clean version string.
		while (!candidate.empty() && candidate.back() == '.') candidate.pop_back();
		if (sawDot) return candidate;
		i = j;
	}
	return {};
}

std::string runCapture(const std::vector<std::string>& args, bool& ok)
{
	HE::Proc::Options o;
	o.exe       = gitLaunchPath();
	o.args      = args;
	o.timeoutMs = kProbeTimeoutMs;
	// git must never stop to ask a question during a probe: with no console to
	// prompt on it would simply hang until the timeout.
	o.env.emplace_back("GIT_TERMINAL_PROMPT", "0");

	const HE::Proc::Result r = HE::Proc::run(o);
	ok = r.ok();
	if (r.launchFailed && o.exe.is_absolute()) invalidateGitLocation();
	return trimmed(r.out);
}

// Only asked on macOS, and only about /usr/bin/git: is there a real git behind
// the xcrun shim? The usual install folders first, then `xcode-select -p`, which
// prints the active developer directory (also a custom Xcode location) and just
// exits non-zero without one — unlike the shim, it never opens an installer.
bool developerToolsPresent(const std::optional<std::filesystem::path>& found)
{
	if (currentPlatform() != HostPlatform::MacOS) return true;
	if (!found || found->lexically_normal() != std::filesystem::path("/usr/bin/git")) return true;

	std::error_code ec;
	for (const char* real : { "/Library/Developer/CommandLineTools/usr/bin/git",
	                          "/Applications/Xcode.app/Contents/Developer/usr/bin/git" })
		if (std::filesystem::exists(real, ec)) return true;

	HE::Proc::Options o;
	o.exe       = "/usr/bin/xcode-select";
	o.args      = { "-p" };
	o.timeoutMs = kProbeTimeoutMs;
	const HE::Proc::Result r = HE::Proc::run(o);
	return r.ok() && std::filesystem::exists(trimmed(r.out) + "/usr/bin/git", ec);
}

} // namespace

const char* providerName(ProviderKind kind)
{
	switch (kind)
	{
	case ProviderKind::GitHub:      return "GitHub";
	case ProviderKind::GitLab:      return "GitLab";
	case ProviderKind::AzureDevOps: return "Azure DevOps";
	case ProviderKind::Generic:     break;
	}
	return "Generic";
}

std::string parseGitVersion(const std::string& versionOutput)
{
	return firstVersionNumber(versionOutput);
}

std::string parseLfsVersion(const std::string& versionOutput)
{
	return firstVersionNumber(versionOutput);
}

GitProbe probeGit()
{
	GitProbe p;

	// Before anything else: a macOS app launched from Finder has a PATH without
	// /opt/homebrew/bin. git usually survives that (it is /usr/bin/git) but
	// git-lfs almost never does, and `git lfs` resolves its subcommand through
	// PATH — so without this the probe would report LFS missing on a machine
	// where it is installed and working.
	HE::Proc::augmentToolPath();

	auto note = [&p](const std::string& line) {
		p.detail += line;
		p.detail += '\n';
	};

	// The probe is what Recheck runs, so it must see a git installed since the
	// last look rather than the cached "not found".
	invalidateGitLocation();
	const auto found = gitExecutable();
	if (found)
	{
		p.gitPath = *found;
		note("git found at " + found->string());
	}
	else
	{
		note("git not found on PATH or in its known install folders");
	}

	p.gitBinary = classifyGitBinary(currentPlatform(), found, developerToolsPresent(found));
	p.install   = detectGitInstallPlan();

	bool ok = false;
	if (p.gitBinary == GitBinaryState::Stub)
	{
		// Not run on purpose: the shim may open Apple's installer, and an install
		// starting because the editor looked is exactly what must not happen.
		note("/usr/bin/git is the macOS shim and the Command Line Tools are missing");
	}
	else if (p.gitBinary == GitBinaryState::Runnable)
	{
		const std::string gitVer = runCapture({ "--version" }, ok);
		p.gitFound   = ok;
		p.gitVersion = parseGitVersion(gitVer);
		note(ok ? ("git --version: " + gitVer) : "git --version failed — git is not runnable");
	}

	if (!p.gitFound)
	{
		note(std::string("install route: ") + gitInstallRouteName(p.install.route) +
		     " — " + p.install.reason);
		// Everything below runs git, so stop rather than emit four identical
		// failures that all mean the same thing.
		HE_SC_WARN("Source control unavailable: git is not installed (install route: %s)",
		           gitInstallRouteName(p.install.route));
		return p;
	}

	const std::string lfsVer = runCapture({ "lfs", "version" }, ok);
	p.lfsFound   = ok;
	p.lfsVersion = parseLfsVersion(lfsVer);
	note(ok ? ("git lfs version: " + lfsVer)
	        : "git lfs version failed — Git LFS is not installed");

	// --get returns exit 1 when the key is unset, which is an answer rather than
	// an error; runCapture reports that as !ok and the empty string, which is
	// exactly what is wanted here.
	p.userName  = runCapture({ "config", "--get", "user.name" },  ok);
	p.userEmail = runCapture({ "config", "--get", "user.email" }, ok);
	p.identityConfigured = !p.userName.empty() && !p.userEmail.empty();
	note(p.identityConfigured
		 ? ("identity: " + p.userName + " <" + p.userEmail + ">")
		 : "user.name / user.email are not both set — git commit would refuse");

	p.credentialHelper = runCapture({ "config", "--get", "credential.helper" }, ok);
	note(p.credentialHelper.empty()
		 ? "no credential.helper configured — a token could not be stored"
		 : ("credential.helper: " + p.credentialHelper));

	HE_SC_INFO("git %s%s, lfs %s, identity %s, credential helper %s",
	           p.gitVersion.empty() ? "?" : p.gitVersion.c_str(),
	           p.gitPath.empty() ? "" : (" (" + p.gitPath.string() + ")").c_str(),
	           p.lfsFound ? p.lfsVersion.c_str() : "MISSING",
	           p.identityConfigured ? "set" : "MISSING",
	           p.credentialHelper.empty() ? "none" : p.credentialHelper.c_str());

	if (!p.lfsFound)
	{
		// Worth its own warning: without LFS the large binaries are exactly the
		// files that fail, which is the opposite of what a user expects from
		// "source control mostly works".
		HE_SC_WARN("Git LFS is missing — large assets could not be versioned, "
		           "which is the main thing source control is needed for here");
	}
	return p;
}

} // namespace HE::Sc
