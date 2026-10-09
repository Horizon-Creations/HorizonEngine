#include "SourceControl/GitCli.h"

#include "ScLog.h"

#include <Platform/Process.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>

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

// `-c` arguments that make `helper` the ONLY credential helper for one command.
// The empty value first clears whatever list system/global config built up;
// git exports both to its children, so git-lfs sees the same single helper.
std::vector<std::string> helperOverrideArgs(const std::string& helper)
{
	if (helper.empty()) return {};
	return { "-c", "credential.helper=", "-c", "credential.helper=" + helper };
}

using EnvList = std::vector<std::pair<std::string, std::string>>;

GitResult runImpl(const std::filesystem::path& cwd,
                  const std::vector<std::string>& args,
                  std::uint32_t timeoutMs,
                  const EnvList& extraEnv);

} // namespace

GitResult GitCli::run(const std::filesystem::path& cwd,
                      const std::vector<std::string>& args,
                      std::uint32_t timeoutMs)
{
	return runImpl(cwd, args, timeoutMs, {});
}

namespace {

GitResult runImpl(const std::filesystem::path& cwd,
                  const std::vector<std::string>& args,
                  std::uint32_t timeoutMs,
                  const EnvList& extraEnv)
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
	for (const auto& kv : extraEnv) o.env.push_back(kv);

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

} // namespace

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
                std::uint32_t timeoutMs, std::string* err, const EnvList& extraEnv = {})
{
	const GitResult r = runImpl(cwd, args, timeoutMs, extraEnv);
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

bool GitCli::fetch(const std::filesystem::path& root, std::string* err)
{
	return runChecked(root, { "fetch", "--prune" }, kNetworkTimeoutMs, err);
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

namespace {

constexpr std::string_view kHttpsScheme = "https://";
constexpr std::string_view kFileScheme  = "file://";

// Everything between "https://" and the first '/', '?' or '#'.
std::string httpsAuthority(const std::string& url)
{
	if (url.rfind(kHttpsScheme, 0) != 0) return {};
	const std::size_t begin = kHttpsScheme.size();
	const std::size_t end   = url.find_first_of("/?#", begin);
	return url.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

// A path from the UTF-8 bytes git prints. Built through char8_t because a
// narrow std::string is read in the ANSI code page on Windows.
std::filesystem::path pathFromUtf8(const std::string& utf8)
{
	return std::filesystem::path(
		std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

} // namespace

bool GitCli::isSafeCloneUrl(const std::string& url, std::string* why)
{
	const auto refuse = [why](const char* reason) {
		if (why) *why = reason;
		return false;
	};
	if (url.empty()) return refuse("no clone URL given");
	for (const unsigned char ch : url)
	{
		if (ch < 0x20 || ch == 0x7f)
			return refuse("the clone URL contains control characters");
	}

	// A local bare repository. No credentials are involved at all.
	if (url.rfind(kFileScheme, 0) == 0) return true;

	if (url.rfind(kHttpsScheme, 0) != 0)
	{
		return refuse("only https:// clone URLs are supported — the token reaches git "
		              "through the credential helper, which http:// would send in the "
		              "clear and ssh does not use at all");
	}
	const std::string authority = httpsAuthority(url);
	if (authority.empty()) return refuse("the clone URL has no host");
	if (authority.find('@') != std::string::npos)
	{
		// The rule the whole module keeps: a token in the URL lands in
		// .git/config, in the process table and in every error message git
		// prints about that remote.
		return refuse("the clone URL carries a user name or token before the host — "
		              "credentials go through the credential helper, never into the URL");
	}
	if (url.find(' ') != std::string::npos)
		return refuse("the clone URL contains a space");
	return true;
}

std::string GitCli::urlHost(const std::string& url)
{
	const std::string authority = httpsAuthority(url);
	// A userinfo part is not a host; isSafeCloneUrl refuses such URLs anyway.
	if (authority.find('@') != std::string::npos) return {};
	return authority;
}

bool GitCli::clone(const std::string& url, const std::filesystem::path& targetDir,
                   const std::string& helperOverride, std::string* err)
{
	namespace fs = std::filesystem;

	std::string why;
	if (!isSafeCloneUrl(url, &why))
	{
		if (err) *err = why;
		return false;
	}
	if (targetDir.empty())
	{
		if (err) *err = "no target folder given for the clone";
		return false;
	}

	// ── The target must be empty ─────────────────────────────────────────────
	// Checked here rather than left to git so the refusal names the folder and
	// what is in it, and — more importantly — so the cleanup below is safe:
	// everything inside the target after a failed clone was put there by it.
	std::error_code ec;
	bool createdDir = false;
	if (fs::exists(targetDir, ec))
	{
		if (!fs::is_directory(targetDir, ec))
		{
			if (err) *err = "\"" + targetDir.string() + "\" exists and is not a folder";
			return false;
		}
		std::size_t entries = 0;
		for (fs::directory_iterator it(targetDir, ec), end; !ec && it != end; it.increment(ec))
			++entries;
		if (ec)
		{
			if (err) *err = "cannot read \"" + targetDir.string() + "\": " + ec.message();
			return false;
		}
		if (entries != 0)
		{
			if (err) *err = "\"" + targetDir.string() + "\" is not empty (" +
			                std::to_string(entries) + " entries) — choose an empty folder "
			                "or a new one to clone into";
			return false;
		}
	}
	else
	{
		fs::create_directories(targetDir, ec);
		if (ec)
		{
			if (err) *err = "cannot create \"" + targetDir.string() + "\": " + ec.message();
			return false;
		}
		createdDir = true;
	}

	std::vector<std::string> args = helperOverrideArgs(helperOverride);
	// "--" so a URL can never be read as an option, "." because the working
	// directory IS the target.
	args.insert(args.end(), { "clone", "--", url, "." });

	const bool ok = runChecked(targetDir, args, kNetworkTimeoutMs, err,
	                           { { "GIT_LFS_SKIP_SMUDGE", "1" } });
	if (ok) return true;

	// git removes a failed clone itself — unless it was killed on the timeout,
	// which leaves a partial .git behind and makes every retry fail with "not
	// empty". The folder was empty (or absent) before, so clearing it again
	// removes only what this clone wrote.
	if (createdDir)
	{
		fs::remove_all(targetDir, ec);
	}
	else
	{
		for (fs::directory_iterator it(targetDir, ec), end; !ec && it != end; it.increment(ec))
		{
			std::error_code rmEc;
			fs::remove_all(it->path(), rmEc);
		}
	}
	return false;
}

bool GitCli::isValidBranchName(const std::filesystem::path& root, const std::string& name)
{
	if (name.empty()) return false;
	// git's own rules, asked of git — reimplementing them here would drift.
	return run(root, { "check-ref-format", "--branch", name }, 10000).ok;
}

bool GitCli::branchExists(const std::filesystem::path& root, const std::string& name)
{
	if (name.empty()) return false;
	return run(root, { "rev-parse", "--verify", "--quiet",
	                   "refs/heads/" + name }, 10000).ok;
}

bool GitCli::createBranch(const std::filesystem::path& root, const std::string& name,
                          const std::string& startCommit, bool checkout, std::string* err)
{
	if (!isValidBranchName(root, name))
	{
		if (err) *err = "\"" + name + "\" is not a valid branch name — no spaces, no "
		                "\"..\", and it cannot start or end with a slash or a dot.";
		return false;
	}
	if (branchExists(root, name))
	{
		if (err) *err = "a branch named \"" + name + "\" already exists";
		return false;
	}
	if (!startCommit.empty() && !commitExists(root, startCommit))
	{
		if (err) *err = "no commit named \"" + startCommit + "\" in this repository";
		return false;
	}

	std::vector<std::string> args;
	if (checkout) args = { "switch", "-c", name };   // create AND move onto it
	else          args = { "branch", name };        // write the ref only
	if (!startCommit.empty()) args.push_back(startCommit);

	return runChecked(root, args, kLocalTimeoutMs, err);
}

bool GitCli::listBranches(const std::filesystem::path& root,
                          std::vector<std::string>& out, std::string& outCurrent,
                          std::string* err)
{
	out.clear();
	outCurrent.clear();

	// --format keeps this parseable without the decoration `git branch` adds;
	// refname:short is the plain name.
	const GitResult r = run(root, { "for-each-ref", "--sort=refname",
	                                "--format=%(refname:short)", "refs/heads" }, 10000);
	if (!r.ok)
	{
		if (err) *err = trimTrailing(r.err);
		return false;
	}
	std::size_t pos = 0;
	while (pos < r.out.size())
	{
		const std::size_t nl = r.out.find('\n', pos);
		std::string line = r.out.substr(pos, nl == std::string::npos
		                                     ? std::string::npos : nl - pos);
		pos = nl == std::string::npos ? r.out.size() : nl + 1;
		line = trimTrailing(line);
		if (!line.empty()) out.push_back(std::move(line));
	}

	const GitResult cur = run(root, { "branch", "--show-current" }, 10000);
	if (cur.ok) outCurrent = trimTrailing(cur.out);
	return true;
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

bool GitCli::usesLfs(const std::filesystem::path& root)
{
	// Every tracked .gitattributes, at any depth (**/ matches zero directories,
	// so the root one is included). -z: paths are raw bytes, no quoting.
	const GitResult r = run(root, { "ls-files", "-z", "--", ":(glob)**/.gitattributes" }, 10000);
	if (!r.ok) return false;

	std::size_t pos = 0;
	while (pos < r.out.size())
	{
		const std::size_t nul = r.out.find('\0', pos);
		const std::string rel = r.out.substr(pos, nul == std::string::npos
		                                          ? std::string::npos : nul - pos);
		pos = nul == std::string::npos ? r.out.size() : nul + 1;
		if (rel.empty()) continue;

		std::ifstream in(root / pathFromUtf8(rel), std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(in)),
		                       std::istreambuf_iterator<char>());
		if (text.find("filter=lfs") != std::string::npos) return true;
	}
	return false;
}

bool GitCli::lfsPull(const std::filesystem::path& root, std::string* err)
{
	// A network transfer like push/pull, with the same bound: a cut-off
	// download is resumed by simply running this again.
	return runChecked(root, { "lfs", "pull" }, kNetworkTimeoutMs, err);
}

std::string GitCli::credentialHelper(const std::filesystem::path& root)
{
	const GitResult r = run(root, { "config", "--get", "credential.helper" }, 5000);
	return r.ok ? trimTrailing(r.out) : std::string{};
}

std::string GitCli::defaultCredentialHelper()
{
#if defined(__APPLE__)
	return "osxkeychain";       // the macOS keychain
#elif defined(_WIN32)
	return "manager";           // Git Credential Manager, ships with Git for Windows
#else
	// No universal secure store on Linux; a bounded in-memory cache is the only
	// default that never writes a plaintext file. NEVER `store` — that is a
	// token in a world-readable file, silently.
	return "cache --timeout=3600";
#endif
}

bool GitCli::ensureCredentialHelper(const std::filesystem::path& root,
                                    std::string* outConfigured, std::string* err)
{
	if (outConfigured) outConfigured->clear();
	if (!credentialHelper(root).empty()) return true;   // someone already chose

	const std::string helper = defaultCredentialHelper();

	// --local: this decision is scoped to the repository that asked for it, not
	// imposed on the user's global git config.
	if (!runChecked(root, { "config", "--local", "credential.helper", helper },
	                kLocalTimeoutMs, err))
	{
		return false;
	}
	if (outConfigured) *outConfigured = helper;
	HE_SC_INFO("Configured repo-local credential.helper: %s", helper.c_str());
	return true;
}

bool GitCli::approveCredential(const std::filesystem::path& root,
                               const std::string& host,
                               const std::string& username,
                               const std::string& secret,
                               std::string* err,
                               const std::string& helperOverride)
{
	HE::Proc::Options o;
	o.exe       = "git";
	o.args      = helperOverrideArgs(helperOverride);
	o.args.insert(o.args.end(), { "credential", "approve" });
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

bool GitCli::fillCredential(const std::filesystem::path& root,
                            const std::string& host,
                            std::string& outUsername,
                            std::string& outSecret,
                            std::string* err,
                            const std::string& helperOverride)
{
	outUsername.clear();
	outSecret.clear();

	HE::Proc::Options o;
	o.exe       = "git";
	o.args      = helperOverrideArgs(helperOverride);
	o.args.insert(o.args.end(), { "credential", "fill" });
	o.cwd       = root;
	o.timeoutMs = 15000;
	// Every door a prompt could come through. Without these, a machine with no
	// stored credential does not answer "no" — it stops and waits, either on a
	// terminal nobody is looking at or in a dialog behind the editor window.
	o.env.emplace_back("GIT_TERMINAL_PROMPT", "0");
	o.env.emplace_back("GIT_ASKPASS", "");
	o.env.emplace_back("SSH_ASKPASS", "");
	o.env.emplace_back("GCM_INTERACTIVE", "never");     // Git Credential Manager
	o.stdinData = "protocol=https\nhost=" + host + "\n\n";

	const HE::Proc::Result r = HE::Proc::run(o);
	if (!r.ok())
	{
		// The ordinary "nothing stored" outcome, not a failure to report: git
		// exits non-zero when it cannot produce a credential without asking.
		return false;
	}

	// key=value lines, terminated by a blank line — the same format approve
	// consumes. Anything after the first blank line is not ours.
	std::string line;
	for (std::size_t i = 0; i <= r.out.size(); ++i)
	{
		if (i < r.out.size() && r.out[i] != '\n') { line += r.out[i]; continue; }
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty()) break;

		const std::size_t eq = line.find('=');
		if (eq != std::string::npos)
		{
			const std::string key   = line.substr(0, eq);
			const std::string value = line.substr(eq + 1);
			if      (key == "username") outUsername = value;
			else if (key == "password") outSecret   = value;
		}
		line.clear();
	}

	if (outSecret.empty())
	{
		// git answered, but with no secret in it. Worth saying, because it means
		// a helper IS configured and simply holds nothing for this host.
		if (err) *err = "no stored credential for " + host;
		return false;
	}
	return true;
}

bool GitCli::rejectCredential(const std::filesystem::path& root,
                              const std::string& host,
                              const std::string& username,
                              const std::string& secret,
                              std::string* err,
                              const std::string& helperOverride)
{
	HE::Proc::Options o;
	o.exe       = "git";
	o.args      = helperOverrideArgs(helperOverride);
	o.args.insert(o.args.end(), { "credential", "reject" });
	o.cwd       = root;
	o.timeoutMs = 15000;
	o.env.emplace_back("GIT_TERMINAL_PROMPT", "0");
	o.env.emplace_back("GCM_INTERACTIVE", "never");
	// Only the fields we know: an empty username or password line would be a
	// value to match, not a wildcard.
	o.stdinData = "protocol=https\nhost=" + host + "\n";
	if (!username.empty()) o.stdinData += "username=" + username + "\n";
	if (!secret.empty())   o.stdinData += "password=" + secret + "\n";
	o.stdinData += "\n";

	const HE::Proc::Result r = HE::Proc::run(o);
	if (!r.ok())
	{
		if (err) *err = r.err.empty() ? "git credential reject failed" : trimTrailing(r.err);
		return false;
	}
	return true;
}

} // namespace HE::Sc
