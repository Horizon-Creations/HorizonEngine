#include "SourceControl/GitCli.h"

#include "ScLog.h"

#include <Platform/Process.h>

#include <algorithm>

namespace HE::Sc {
namespace {

std::string trimTrailing(std::string s)
{
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
	return s;
}

std::string joinForLog(const std::vector<std::string>& args)
{
	std::string s;
	for (const std::string& a : args) { s += ' '; s += a; }
	return s;
}

} // namespace

GitResult GitCli::run(const std::filesystem::path& cwd,
                      const std::vector<std::string>& args,
                      std::uint32_t timeoutMs)
{
	HE::Proc::Options o;
	o.exe       = "git";
	o.args      = args;
	o.cwd       = cwd;
	o.timeoutMs = timeoutMs;

	// git must never stop to ask a question. With no console to prompt on it
	// would sit there until the timeout, which looks like a hang rather than a
	// missing credential — and these are the exact variables that turn an
	// interactive prompt into an immediate, diagnosable failure.
	o.env.emplace_back("GIT_TERMINAL_PROMPT", "0");
	o.env.emplace_back("GIT_ASKPASS", "");
	o.env.emplace_back("SSH_ASKPASS", "");
	// Machine-readable output regardless of the user's locale: git translates its
	// messages, and a parser matching English words breaks on a German install.
	o.env.emplace_back("LC_ALL", "C");
	o.env.emplace_back("LANG", "C");

	HE_SC_TRACE("git%s (in %s)", joinForLog(args).c_str(), cwd.string().c_str());

	const HE::Proc::Result r = HE::Proc::run(o);

	GitResult g;
	g.exitCode = r.exitCode;
	g.out      = r.out;
	g.err      = r.err;
	g.ok       = r.ok();

	if (r.launchFailed)
	{
		g.err = "git could not be started — is it installed and on PATH?";
		HE_SC_ERROR("git could not be started for:%s", joinForLog(args).c_str());
	}
	else if (r.timedOut)
	{
		g.err = "git did not finish in time and was stopped";
		HE_SC_WARN("git timed out after %u ms:%s", timeoutMs, joinForLog(args).c_str());
	}
	else if (!g.ok)
	{
		// Not logged as an error: a non-zero exit is a legitimate answer for
		// several commands (rev-parse on a non-repository, diff --quiet on a
		// dirty tree). The caller decides whether it was a failure.
		HE_SC_DEBUG("git exited %d:%s — %s", g.exitCode, joinForLog(args).c_str(),
		            trimTrailing(g.err).c_str());
	}
	return g;
}

std::filesystem::path GitCli::findRepoRoot(const std::filesystem::path& anyPathInside)
{
	if (anyPathInside.empty()) return {};

	std::error_code ec;
	// git needs an existing directory to start from; a path to a file (or one
	// that was just deleted) would make it fail for the wrong reason.
	std::filesystem::path dir = anyPathInside;
	if (!std::filesystem::is_directory(dir, ec)) dir = dir.parent_path();
	if (dir.empty() || !std::filesystem::exists(dir, ec)) return {};

	// Short timeout: this only walks up the directory tree and reads a config
	// file, so anything slower means something is wrong (a stalled network
	// filesystem, most likely) and waiting longer will not help.
	const GitResult r = run(dir, { "rev-parse", "--show-toplevel" }, 5000);
	if (!r.ok) return {};

	const std::string path = trimTrailing(r.out);
	if (path.empty()) return {};
	return std::filesystem::path(path);
}

bool GitCli::status(const std::filesystem::path& root, RepoStatus& out, std::string* err)
{
	out = RepoStatus{};
	if (root.empty()) return false;

	const GitResult r = run(root, {
		// --no-optional-locks is the important one: a plain `git status`
		// refreshes the index and takes index.lock, so a status poll racing any
		// other git command produces "fatal: Unable to create index.lock". This
		// makes status genuinely read-only.
		"--no-optional-locks", "status",
		// v2 with -z emits raw NUL-separated bytes. v1 C-escapes and quotes any
		// path with a space, quote or non-ASCII character, so parsing it means
		// reimplementing git's unquoting — and getting that subtly wrong is how
		// a file with a quote in its name corrupts every record after it.
		"--porcelain=v2", "-z",
		// Ahead/behind in the same invocation rather than a second one.
		"--branch",
		// Without =all, a new folder full of assets collapses into a single
		// directory entry and the user sees one badge for fifty new files.
		"--untracked-files=all",
		// Ignored files are excluded: the tree is full of them (Saved/, Export/,
		// build output) and listing them would dwarf the real changes.
		"--ignored=no",
	});

	if (!r.ok)
	{
		if (err) *err = r.err.empty() ? "git status failed" : trimTrailing(r.err);
		return false;
	}

	if (!parsePorcelainV2(r.out, out))
	{
		if (err) *err = "could not interpret git status output";
		return false;
	}

	out.isRepo = true;
	out.root   = root.generic_string();
	return true;
}

} // namespace HE::Sc
