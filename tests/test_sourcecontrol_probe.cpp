#include "doctest.h"

#include <SourceControl/GitProbe.h>
#include <SourceControl/ScCommon.h>
#include <Platform/Process.h>

// The redaction helper lives in the module's private src/ directory; the test
// target adds it to the include path so this can be covered directly. It is
// worth reaching for: it is the last line of defence against a credential
// reaching a log file that later gets pasted into a bug report.
#include "ScLog.h"

#include <string>

using namespace HE::Sc;

// ─── Version parsing ─────────────────────────────────────────────────────────

TEST_CASE("Git and LFS versions are parsed out of the prose around them")
{
	// Real strings from the platforms this has to run on. The wording differs per
	// OS and per distribution, which is why the number is matched rather than the
	// surrounding words.
	CHECK(parseGitVersion("git version 2.43.0")                     == "2.43.0");
	CHECK(parseGitVersion("git version 2.39.3 (Apple Git-146)")     == "2.39.3");
	// Git for Windows appends a build suffix; the bare numeric version is what is
	// wanted, so parsing stops at the first non-numeric part rather than carrying
	// ".windows.1" into a version comparison later.
	CHECK(parseGitVersion("git version 2.45.1.windows.1")           == "2.45.1");
	CHECK(parseGitVersion("git version 2.43.0\n")                   == "2.43.0");

	CHECK(parseLfsVersion("git-lfs/3.4.1 (GitHub; darwin arm64; go 1.21.0)") == "3.4.1");
	CHECK(parseLfsVersion("git-lfs/3.5.1 (GitHub; linux amd64; go 1.21.6)")  == "3.5.1");

	// Nothing numeric, or nothing dotted, must not produce a half-parsed answer.
	CHECK(parseGitVersion("git: command not found").empty());
	CHECK(parseGitVersion("").empty());
	CHECK(parseLfsVersion("error").empty());
}

TEST_CASE("providerName covers every enumerator")
{
	CHECK(std::string(providerName(ProviderKind::GitHub))      == "GitHub");
	CHECK(std::string(providerName(ProviderKind::GitLab))      == "GitLab");
	CHECK(std::string(providerName(ProviderKind::AzureDevOps)) == "Azure DevOps");
	CHECK(std::string(providerName(ProviderKind::Generic))     == "Generic");
}

// ─── Redaction ───────────────────────────────────────────────────────────────
// Each case is a string this module can genuinely end up logging: a remote URL
// git printed back at us, the output of `git credential fill`, a provider's
// error body. The rule is simple — the secret must not survive, and enough
// context must.

TEST_CASE("Credentials embedded in a URL never reach the log")
{
	using HE::Sc::detail::scrub;

	// The most common leak by far: a token in the remote URL, which then appears
	// in every error message that names the remote, and sits in .git/config.
	const std::string a = scrub("failed to push to https://ghp_AbCdEf0123456789AbCdEf0123456789AbCd@github.com/acme/game.git");
	CHECK(a.find("ghp_AbCdEf0123456789AbCdEf0123456789AbCd") == std::string::npos);
	CHECK(a.find("github.com/acme/game.git") != std::string::npos);   // context survives

	const std::string b = scrub("remote: https://alice:hunter2@gitlab.com/acme/game.git");
	CHECK(b.find("hunter2") == std::string::npos);
	CHECK(b.find("alice")   == std::string::npos);   // the username goes too
	CHECK(b.find("gitlab.com/acme/game.git") != std::string::npos);

	// A URL with no credentials must come through untouched, or every ordinary
	// log line becomes unreadable.
	const std::string clean = "cloning https://github.com/acme/game.git";
	CHECK(scrub(clean) == clean);
}

TEST_CASE("Provider token shapes are redacted wherever they appear")
{
	using HE::Sc::detail::scrub;

	struct Case { const char* token; };
	const Case cases[] = {
		{ "ghp_AbCdEf0123456789AbCdEf0123456789AbCd" },   // GitHub classic PAT
		{ "gho_0123456789abcdef0123456789abcdef0123" },   // GitHub OAuth
		{ "github_pat_11ABCDEFG0aBcDeFgHiJkL_mNoPqRsTuVwXyZ" },
		{ "glpat-xxxxxxxxxxxxxxxxxxxx" },                 // GitLab PAT
	};

	for (const Case& c : cases)
	{
		CAPTURE(c.token);
		const std::string line = scrub(std::string("authenticating with ") + c.token + " ok");
		CHECK(line.find(c.token) == std::string::npos);
		CHECK(line.find("<redacted-token>") != std::string::npos);
		// The surrounding words survive, so the line still says what happened.
		CHECK(line.find("authenticating with") != std::string::npos);
		CHECK(line.find("ok") != std::string::npos);
	}
}

TEST_CASE("git credential's key=value output does not leak the password")
{
	using HE::Sc::detail::scrub;

	// `git credential fill` answers on stdout in this exact shape. Logging its
	// raw output would print the secret in clear.
	const std::string filled = scrub(
		"protocol=https\nhost=github.com\nusername=x-access-token\npassword=ghs_SuperSecretValue123\n");
	CHECK(filled.find("ghs_SuperSecretValue123") == std::string::npos);
	// Everything that is not the secret is still there — that is what makes the
	// log useful for diagnosing a credential problem.
	CHECK(filled.find("host=github.com") != std::string::npos);
	CHECK(filled.find("username=x-access-token") != std::string::npos);

	const std::string header = scrub("Authorization: Bearer abcdefghijklmnop");
	CHECK(header.find("abcdefghijklmnop") == std::string::npos);
}

TEST_CASE("Ordinary text is left alone")
{
	using HE::Sc::detail::scrub;
	// A scrubber that mangles normal lines makes the log useless and pushes
	// people to turn it off — which costs more than it saves.
	const char* untouched[] = {
		"running git status --porcelain=v2",
		"3 files changed, 42 insertions(+), 7 deletions(-)",
		"Content/Meshes/Hero.fbx is 214.5 MB and exceeds the 100 MB limit",
		"branch main is 2 ahead, 0 behind origin/main",
		"C:\\Users\\alice\\Projects\\Game\\Content",
	};
	for (const char* t : untouched)
	{
		CAPTURE(t);
		CHECK(scrub(t) == std::string(t));
	}
}

// ─── The probe itself ────────────────────────────────────────────────────────

TEST_CASE("probeGit reports what is actually installed")
{
	// Guarded so a build machine without git reports "skipped" rather than a
	// failure that says nothing about the code.
	if (!HE::Proc::which("git").has_value())
	{
		MESSAGE("git is not installed on this machine — probe test skipped");
		return;
	}

	const GitProbe p = probeGit();
	CHECK(p.gitFound);
	CHECK_FALSE(p.gitVersion.empty());
	CHECK_FALSE(p.gitPath.empty());
	// The detail text is what the "git missing" dialog shows; an empty one would
	// leave the user with a problem and no information.
	CHECK_FALSE(p.detail.empty());

	// Whatever the outcome, the probe must never put a secret in its detail text.
	CHECK(p.detail.find("password=") == std::string::npos);

	// ready() is the conjunction the editor gates on, so it must not claim
	// readiness while a part is missing.
	if (p.ready())
	{
		CHECK(p.lfsFound);
		CHECK(p.identityConfigured);
	}
}
