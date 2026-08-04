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

namespace {

// Local commands answer in well under this; only a wedged filesystem exceeds it.
constexpr std::uint32_t kLocalTimeoutMs   = 60'000;
// Push/pull move real data — multi-GB LFS objects at residential upload speed.
constexpr std::uint32_t kNetworkTimeoutMs = 15 * 60'000;

bool runChecked(const std::filesystem::path& cwd, const std::vector<std::string>& args,
                std::uint32_t timeoutMs, std::string* err)
{
	const GitResult r = GitCli::run(cwd, args, timeoutMs);
	if (r.ok) return true;
	if (err)
	{
		// git writes its useful text to stderr; stdout as the fallback covers the
		// odd command that reports there instead.
		*err = !r.err.empty() ? trimTrailing(r.err)
		     : !r.out.empty() ? trimTrailing(r.out)
		                      : "git failed with exit code " + std::to_string(r.exitCode);
	}
	return false;
}

} // namespace

bool GitCli::init(const std::filesystem::path& dir, std::string* err)
{
	// -b main: the default branch name is a config lottery across machines, and
	// a project that starts on "master" here and "main" there guarantees the
	// first push goes somewhere surprising.
	return runChecked(dir, { "init", "-b", "main" }, kLocalTimeoutMs, err);
}

bool GitCli::addAll(const std::filesystem::path& root, std::string* err)
{
	return runChecked(root, { "add", "-A" }, kLocalTimeoutMs, err);
}

bool GitCli::commit(const std::filesystem::path& root, const std::string& message,
                    std::string* err)
{
	return runChecked(root, { "commit", "-m", message }, kLocalTimeoutMs, err);
}

bool GitCli::push(const std::filesystem::path& root, bool upstreamConfigured, std::string* err)
{
	if (upstreamConfigured)
		return runChecked(root, { "push" }, kNetworkTimeoutMs, err);
	// First push: bind the branch to its remote counterpart while we are at it,
	// so ahead/behind starts meaning something.
	return runChecked(root, { "push", "-u", "origin", "HEAD" }, kNetworkTimeoutMs, err);
}

bool GitCli::pull(const std::filesystem::path& root, std::string* err)
{
	// --ff-only on purpose: an editor button must never quietly create a merge
	// commit. If the branch diverged, the error says so and the user decides —
	// with a working tree full of binary assets that is the only honest option.
	return runChecked(root, { "pull", "--ff-only" }, kNetworkTimeoutMs, err);
}

std::string GitCli::remoteUrl(const std::filesystem::path& root)
{
	const GitResult r = run(root, { "remote", "get-url", "origin" }, 5000);
	return r.ok ? trimTrailing(r.out) : std::string{};
}

bool GitCli::setRemote(const std::filesystem::path& root, const std::string& url,
                       std::string* err)
{
	if (remoteUrl(root).empty())
		return runChecked(root, { "remote", "add", "origin", url }, kLocalTimeoutMs, err);
	return runChecked(root, { "remote", "set-url", "origin", url }, kLocalTimeoutMs, err);
}

bool GitCli::commitExists(const std::filesystem::path& root, const std::string& commit)
{
	if (commit.empty()) return false;
	// ^{commit} makes this fail for a tag or tree that merely resolves — the
	// caller means a commit.
	return run(root, { "rev-parse", "--verify", "--quiet", commit + "^{commit}" }, 10000).ok;
}

bool GitCli::restoreWorktreeTo(const std::filesystem::path& root,
                               const std::string& commit, std::string* err)
{
	if (!commitExists(root, commit))
	{
		if (err) *err = "no commit named \"" + commit + "\" in this repository";
		return false;
	}
	// -u writes the result to the working tree, --reset lets it overwrite the
	// files that differ. HEAD is untouched, so the branch and every commit on
	// it survive; the difference simply shows up staged.
	return runChecked(root, { "read-tree", "-u", "--reset", commit }, kLocalTimeoutMs, err);
}

bool GitCli::log(const std::filesystem::path& root, std::size_t maxCount,
                 std::vector<CommitInfo>& out, std::string* err)
{
	out.clear();

	const GitResult r = run(root, {
		"log", "-n", std::to_string(maxCount),
		// 0x1F between fields, 0x1E after each record: the one framing a commit
		// message can never contain, unlike newlines or any printable character.
		"--pretty=format:%h%x1f%s%x1f%an%x1f%ar%x1e",
	}, 15000);
	if (!r.ok)
	{
		// A repository with no commits yet answers non-zero; that is an empty
		// history, not an error.
		if (r.err.find("does not have any commits") != std::string::npos ||
		    r.err.find("bad default revision") != std::string::npos)
			return true;
		if (err) *err = trimTrailing(r.err);
		return false;
	}

	std::size_t pos = 0;
	while (pos < r.out.size())
	{
		const std::size_t end = r.out.find('\x1e', pos);
		const std::string rec = r.out.substr(pos, end == std::string::npos
		                                          ? std::string::npos : end - pos);
		pos = end == std::string::npos ? r.out.size() : end + 1;
		// git separates records with \n after our 0x1E; strip it.
		std::size_t begin = 0;
		while (begin < rec.size() && (rec[begin] == '\n' || rec[begin] == '\r')) ++begin;

		std::vector<std::string> fields;
		std::size_t f = begin;
		while (f <= rec.size())
		{
			const std::size_t sep = rec.find('\x1f', f);
			fields.push_back(rec.substr(f, sep == std::string::npos
			                                  ? std::string::npos : sep - f));
			if (sep == std::string::npos) break;
			f = sep + 1;
		}
		if (fields.size() < 4 || fields[0].empty()) continue;

		CommitInfo c;
		c.shortOid = fields[0];
		c.subject  = fields[1];
		c.author   = fields[2];
		c.relTime  = fields[3];
		out.push_back(std::move(c));
	}

	// Which of these the upstream does not have yet. Only meaningful (and only
	// answerable) when an upstream exists — absence is not an error.
	const GitResult ahead = run(root, { "rev-list", "--abbrev-commit", "@{upstream}..HEAD" },
	                            10000);
	if (ahead.ok)
	{
		for (CommitInfo& c : out)
		{
			if (ahead.out.find(c.shortOid) != std::string::npos) c.unpushed = true;
		}
	}
	return true;
}

bool GitCli::lfsAvailable(const std::filesystem::path& root)
{
	return run(root, { "lfs", "version" }, 10000).ok;
}

bool GitCli::lfsTrack(const std::filesystem::path& root,
                      const std::string& repoRelativePath, std::string* err)
{
	return runChecked(root, { "lfs", "track", "--filename", repoRelativePath },
	                  kLocalTimeoutMs, err);
}

std::string GitCli::credentialHelper(const std::filesystem::path& root)
{
	const GitResult r = run(root, { "config", "--get", "credential.helper" }, 5000);
	return r.ok ? trimTrailing(r.out) : std::string{};
}

bool GitCli::ensureCredentialHelper(const std::filesystem::path& root,
                                    std::string* outConfigured, std::string* err)
{
	if (outConfigured) outConfigured->clear();
	if (!credentialHelper(root).empty()) return true;   // someone already chose

#if defined(__APPLE__)
	const char* helper = "osxkeychain";       // the macOS keychain
#elif defined(_WIN32)
	const char* helper = "manager";           // Git Credential Manager, ships with Git for Windows
#else
	// No universal secure store on Linux; a bounded in-memory cache is the only
	// default that never writes a plaintext file. NEVER `store` — that is a
	// token in a world-readable file, silently.
	const char* helper = "cache --timeout=3600";
#endif

	// --local: this decision is scoped to the repository that asked for it, not
	// imposed on the user's global git config.
	if (!runChecked(root, { "config", "--local", "credential.helper", helper },
	                kLocalTimeoutMs, err))
	{
		return false;
	}
	if (outConfigured) *outConfigured = helper;
	HE_SC_INFO("Configured repo-local credential.helper: %s", helper);
	return true;
}

bool GitCli::approveCredential(const std::filesystem::path& root,
                               const std::string& host,
                               const std::string& username,
                               const std::string& secret,
                               std::string* err)
{
	HE::Proc::Options o;
	o.exe       = "git";
	o.args      = { "credential", "approve" };
	o.cwd       = root;
	o.timeoutMs = 15000;
	o.env.emplace_back("GIT_TERMINAL_PROMPT", "0");
	// The credential format git defines: key=value lines, blank line to end.
	// stdin, never argv — argv is visible to every process on the machine.
	o.stdinData = "protocol=https\nhost=" + host + "\nusername=" + username +
	              "\npassword=" + secret + "\n\n";

	const HE::Proc::Result r = HE::Proc::run(o);
	if (!r.ok())
	{
		// Whatever git printed — its own messages never echo the password field.
		if (err) *err = r.err.empty() ? "git credential approve failed" : trimTrailing(r.err);
		return false;
	}
	return true;
}

} // namespace HE::Sc
