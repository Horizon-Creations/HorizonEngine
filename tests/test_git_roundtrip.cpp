#include "doctest.h"
#include "TestFsUtil.h"

#include <SourceControl/GitCli.h>
#include <SourceControl/GitHubApi.h>
#include <SourceControl/RepoConfig.h>

#include "../src/HE_Editor/GitController.h"
#include <SourceControl/GitService.h>
#include <SourceControl/RepoStatus.h>
#include <Platform/Process.h>
#include "ProjectManager.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// ─── Against a real repository ───────────────────────────────────────────────
// Everything except the remote server is testable with no network at all: a
// `git init --bare` directory is a perfectly good remote, so clone, fetch, push
// and pull all round-trip locally.
//
// This is worth more than any mock. What breaks in a git integration is the
// exact command line, the exact output format and the exact exit code — all of
// which a mock defines to be correct by construction.

namespace fs = std::filesystem;
using namespace HE::Sc;

namespace {

bool gitAvailable()
{
	// Guarded so a build machine without git reports "skipped" rather than a red
	// failure that says nothing about this code.
	static const bool available = HE::Proc::which("git").has_value();
	return available;
}

fs::path uniqueDir(const char* stem)
{
	static const auto salt =
		static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count());
	static int counter = 0;
	const fs::path p = fs::temp_directory_path() /
	                   ("he_git_" + std::string(stem) + "_" + std::to_string(salt) + "_" +
	                    std::to_string(counter++));
	std::error_code ec;
	fs::create_directories(p, ec);
	return p;
}

// A path from UTF-8 BYTES, on every platform.
//
// Two Windows-only traps meet here. std::filesystem::path built from a narrow
// std::string interprets it in the active ANSI code page, not UTF-8 — so a file
// created that way lands under a mangled name and the later lookup, done with
// the original UTF-8 string, finds nothing. And without /utf-8 MSVC reads the
// source file itself in the system code page, so even the literal's bytes are
// not dependable. Going through char8_t fixes the first; spelling the bytes out
// as escapes below fixes the second.
fs::path utf8Path(const std::string& utf8)
{
	return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

void writeFile(const fs::path& p, const std::string& text)
{
	std::error_code ec;
	fs::create_directories(p.parent_path(), ec);
	std::ofstream out(p, std::ios::binary);
	out << text;
}

std::string trimmed(std::string v)
{
	while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' '))
		v.pop_back();
	return v;
}

std::string readFile(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)),
	                   std::istreambuf_iterator<char>());
}

// A repository with an identity configured LOCALLY, so the test never depends on
// (or disturbs) the machine's global git config.
fs::path makeRepo(const char* stem)
{
	const fs::path dir = uniqueDir(stem);
	REQUIRE(GitCli::run(dir, { "init", "--initial-branch=main" }).ok);
	REQUIRE(GitCli::run(dir, { "config", "user.name",  "HorizonEngine Test" }).ok);
	REQUIRE(GitCli::run(dir, { "config", "user.email", "test@example.invalid" }).ok);
	// Commit signing would prompt for a passphrase this test cannot answer.
	REQUIRE(GitCli::run(dir, { "config", "commit.gpgsign", "false" }).ok);
	return dir;
}

void commitAll(const fs::path& repo, const char* message)
{
	REQUIRE(GitCli::run(repo, { "add", "-A" }).ok);
	REQUIRE(GitCli::run(repo, { "commit", "-m", message }).ok);
}

} // namespace

TEST_CASE("Repository discovery finds the working tree, and reports its absence")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path repo = makeRepo("discover");
	writeFile(repo / "Content" / "Deep" / "file.txt", "hello");

	// From the root, and from a nested directory: git walks up, so both must
	// resolve to the same working tree.
	const fs::path fromRoot   = GitCli::findRepoRoot(repo);
	const fs::path fromNested = GitCli::findRepoRoot(repo / "Content" / "Deep");
	std::error_code ec;
	CHECK(fs::equivalent(fromRoot, repo, ec));
	CHECK(fs::equivalent(fromNested, repo, ec));

	// A directory that is not in a repository answers "no", not an error.
	const fs::path plain = uniqueDir("not_a_repo");
	CHECK(GitCli::findRepoRoot(plain).empty());

	he_test::removeAllQuiet(repo);
	he_test::removeAllQuiet(plain);
}

TEST_CASE("Status reflects the working tree through every stage of an edit")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path repo = makeRepo("status");
	RepoStatus st;

	// 1. A fresh repository: no commits yet, and git says so rather than naming
	//    a commit that does not exist.
	REQUIRE(GitCli::status(repo, st));
	CHECK(st.isRepo);
	CHECK(st.initialCommit);
	CHECK(st.files.empty());

	// 2. A new file is untracked.
	writeFile(repo / "Content" / "Note.txt", "one");
	REQUIRE(GitCli::status(repo, st));
	const FileEntry* untracked = st.find("Content/Note.txt");
	REQUIRE(untracked != nullptr);
	CHECK(untracked->worktree == FileState::Untracked);
	CHECK(st.dirtyFolders.count("Content"));

	// 3. Staged, it becomes an addition.
	REQUIRE(GitCli::run(repo, { "add", "Content/Note.txt" }).ok);
	REQUIRE(GitCli::status(repo, st));
	REQUIRE(st.find("Content/Note.txt") != nullptr);
	CHECK(st.find("Content/Note.txt")->index == FileState::Added);
	CHECK(st.hasStagedChanges());

	// 4. Committed, it disappears from status — a clean tree lists nothing,
	//    which is what makes the common case cheap.
	REQUIRE(GitCli::run(repo, { "commit", "-m", "add note" }).ok);
	REQUIRE(GitCli::status(repo, st));
	CHECK(st.files.empty());
	CHECK_FALSE(st.initialCommit);
	CHECK(st.branch == "main");

	// 5. Edited and staged, then edited again: BOTH halves must be visible, or a
	//    commit would silently include less than the UI showed.
	writeFile(repo / "Content" / "Note.txt", "two");
	REQUIRE(GitCli::run(repo, { "add", "Content/Note.txt" }).ok);
	writeFile(repo / "Content" / "Note.txt", "three");
	REQUIRE(GitCli::status(repo, st));
	const FileEntry* both = st.find("Content/Note.txt");
	REQUIRE(both != nullptr);
	CHECK(both->index    == FileState::Modified);
	CHECK(both->worktree == FileState::Modified);

	he_test::removeAllQuiet(repo);
}

TEST_CASE("A path with a space, a quote and UTF-8 round-trips through real git")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path repo = makeRepo("awkward");

	// The paths porcelain v1 would have escaped and quoted. Done against real
	// git rather than a hand-built stream, so the test also proves git emits what
	// the parser expects.
	//
	// The non-ASCII name is spelled as explicit UTF-8 bytes rather than as source
	// characters, so it does not depend on how the compiler read this file:
	// "Grüße/日本語.txt".
	const std::string spaced = "Content/My Meshes/Big Rock.txt";
	const std::string utf8   = "Content/Gr\xC3\xBC\xC3\x9F" "e/"
	                           "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" ".txt";
	writeFile(repo / utf8Path(spaced), "a");
	writeFile(repo / utf8Path(utf8),   "b");
#ifndef _WIN32
	// Windows filenames cannot contain a quote at all, so this half only runs
	// where the filesystem permits it.
	const std::string quoted = "Content/weird\"name.txt";
	writeFile(repo / utf8Path(quoted), "c");
#endif

	RepoStatus st;
	REQUIRE(GitCli::status(repo, st));
	CHECK(st.find(spaced) != nullptr);
	CHECK(st.find(utf8)   != nullptr);
#ifndef _WIN32
	CHECK(st.find(quoted) != nullptr);
	CHECK(st.files.size() == 3);
#else
	CHECK(st.files.size() == 2);
#endif

	he_test::removeAllQuiet(repo);
}

TEST_CASE("A rename is reported with both paths")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path repo = makeRepo("rename");
	// Long enough content that git's rename detection is confident rather than
	// treating it as a delete plus an unrelated add.
	writeFile(repo / "Content" / "Old.txt", std::string(500, 'x'));
	commitAll(repo, "initial");

	REQUIRE(GitCli::run(repo, { "mv", "Content/Old.txt", "Content/New.txt" }).ok);

	RepoStatus st;
	REQUIRE(GitCli::status(repo, st));
	const FileEntry* e = st.find("Content/New.txt");
	REQUIRE(e != nullptr);
	CHECK(e->index    == FileState::Renamed);
	CHECK(e->origPath == "Content/Old.txt");
	// The old path is not a separate entry.
	CHECK(st.find("Content/Old.txt") == nullptr);

	he_test::removeAllQuiet(repo);
}

TEST_CASE("A real merge conflict is reported as conflicted")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path repo = makeRepo("conflict");
	writeFile(repo / "shared.txt", "base\n");
	commitAll(repo, "base");

	// Two branches change the same line.
	REQUIRE(GitCli::run(repo, { "checkout", "-b", "other" }).ok);
	writeFile(repo / "shared.txt", "from other\n");
	commitAll(repo, "other side");

	REQUIRE(GitCli::run(repo, { "checkout", "main" }).ok);
	writeFile(repo / "shared.txt", "from main\n");
	commitAll(repo, "main side");

	// The merge is EXPECTED to fail — that is the point, so its exit code is not
	// asserted.
	GitCli::run(repo, { "merge", "other" });

	RepoStatus st;
	REQUIRE(GitCli::status(repo, st));
	const FileEntry* e = st.find("shared.txt");
	REQUIRE(e != nullptr);
	CHECK(e->conflicted());
	CHECK(st.hasConflicts());

	he_test::removeAllQuiet(repo);
}

TEST_CASE("Ahead and behind are read from a real local remote")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	// A bare repository is a perfectly good remote. This is what makes push,
	// fetch and pull testable with no network and no credentials.
	const fs::path bare = uniqueDir("bare");
	REQUIRE(GitCli::run(bare, { "init", "--bare", "--initial-branch=main" }).ok);

	const fs::path repo = makeRepo("ahead");
	writeFile(repo / "a.txt", "one");
	commitAll(repo, "first");

	REQUIRE(GitCli::run(repo, { "remote", "add", "origin", bare.string() }).ok);
	REQUIRE(GitCli::run(repo, { "push", "-u", "origin", "main" }).ok);

	RepoStatus st;
	REQUIRE(GitCli::status(repo, st));
	CHECK(st.upstream == "origin/main");
	CHECK(st.ahead  == 0);
	CHECK(st.behind == 0);

	// One local commit that the remote does not have.
	writeFile(repo / "b.txt", "two");
	commitAll(repo, "second");
	REQUIRE(GitCli::status(repo, st));
	CHECK(st.ahead  == 1);
	CHECK(st.behind == 0);

	// A second clone pushes, so the first is now behind as well as ahead.
	const fs::path other = uniqueDir("clone");
	REQUIRE(GitCli::run(other.parent_path(),
	                    { "clone", bare.string(), other.filename().string() }).ok);
	REQUIRE(GitCli::run(other, { "config", "user.name",  "Other" }).ok);
	REQUIRE(GitCli::run(other, { "config", "user.email", "other@example.invalid" }).ok);
	REQUIRE(GitCli::run(other, { "config", "commit.gpgsign", "false" }).ok);
	writeFile(other / "c.txt", "three");
	commitAll(other, "third");
	REQUIRE(GitCli::run(other, { "push" }).ok);

	REQUIRE(GitCli::run(repo, { "fetch" }).ok);
	REQUIRE(GitCli::status(repo, st));
	CHECK(st.ahead  == 1);
	CHECK(st.behind == 1);

	he_test::removeAllQuiet(repo);
	he_test::removeAllQuiet(other);
	he_test::removeAllQuiet(bare);
}

TEST_CASE("Content Browser badge lookups resolve through the controller")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	// The exact seam the tiles use: absolute paths in the same spelling the
	// browser builds (entry.path().string(), from the PROJECT path) against a
	// repo root git reports with symlinks RESOLVED. On macOS the temp dir sits
	// behind /var → /private/var, so this fixture naturally exercises the
	// two-spellings case that silently blanked every badge; elsewhere the
	// spellings coincide and the direct prefix match covers it.
	const fs::path repo = makeRepo("badges");
	writeFile(repo / "Content" / "Props" / "crate.hmat", "{}");
	commitAll(repo, "first");
	writeFile(repo / "Content" / "Props" / "crate.hmat", "{changed}");
	writeFile(repo / "Content" / "new.hcode", "{}");

	GitController git;
	git.openProject(repo);

	std::uint64_t now = 1;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (git.status().generation == 0 && std::chrono::steady_clock::now() < deadline)
	{
		git.update(now);
		now += 100;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	REQUIRE(git.isRepo());

	const std::string modified  = (repo / "Content" / "Props" / "crate.hmat").string();
	const std::string untracked = (repo / "Content" / "new.hcode").string();
	const std::string clean     = (repo / "Content" / "nothing.here").string();

	const HE::Sc::FileEntry* e = git.entryForAbsolutePath(modified);
	REQUIRE(e != nullptr);
	CHECK(e->dirty());

	e = git.entryForAbsolutePath(untracked);
	REQUIRE(e != nullptr);
	CHECK(e->worktree == FileState::Untracked);

	CHECK(git.entryForAbsolutePath(clean) == nullptr);

	// The folder rollup answers for every ancestor, which is what puts the dot
	// on "Content" without walking its subtree.
	CHECK(git.folderHasChanges((repo / "Content").string()));
	CHECK(git.folderHasChanges((repo / "Content" / "Props").string()));

	git.closeProject();
	he_test::removeQuiet(repo);
}

TEST_CASE("The panel operations round-trip: init, commit, remote, push, pull")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	// Exactly what the Source Control panel drives, minus the ImGui: init with
	// generated config, commit-all, set remote, first push with -u, and a pull
	// that fast-forwards a change made by "someone else" (a second clone).
	const fs::path dir = uniqueDir("panelops");
	std::string err;

	REQUIRE(GitCli::init(dir, &err));
	REQUIRE(RepoConfig::writeInitialFiles(dir, &err));
	CHECK(fs::exists(dir / ".gitignore"));
	CHECK(fs::exists(dir / ".gitattributes"));

	// The generator must never clobber a user's file.
	writeFile(dir / ".gitignore", "# mine\n");
	REQUIRE(RepoConfig::writeInitialFiles(dir, &err));
	{
		std::ifstream in(dir / ".gitignore");
		std::string first;
		std::getline(in, first);
		CHECK(first == "# mine");
	}

	REQUIRE(GitCli::run(dir, { "config", "user.name",  "HorizonEngine Test" }).ok);
	REQUIRE(GitCli::run(dir, { "config", "user.email", "test@example.invalid" }).ok);
	REQUIRE(GitCli::run(dir, { "config", "commit.gpgsign", "false" }).ok);

	writeFile(dir / "Content" / "scene.hescene", "{}");
	REQUIRE(GitCli::addAll(dir, &err));
	REQUIRE(GitCli::commit(dir, "first", &err));

	RepoStatus st;
	REQUIRE(GitCli::status(dir, st));
	CHECK(st.dirtyCount() == 0);
	CHECK_FALSE(st.initialCommit);

	// Remote: absent, then set, then read back.
	CHECK(GitCli::remoteUrl(dir).empty());
	const fs::path bare = uniqueDir("panelbare");
	REQUIRE(GitCli::run(bare, { "init", "--bare", "--initial-branch=main" }).ok);
	REQUIRE(GitCli::setRemote(dir, bare.string(), &err));
	CHECK(GitCli::remoteUrl(dir) == bare.string());
	// setRemote on an existing origin updates rather than fails.
	REQUIRE(GitCli::setRemote(dir, bare.string(), &err));

	// First push: no upstream yet → -u origin HEAD.
	REQUIRE(GitCli::push(dir, /*upstreamConfigured=*/false, &err));
	REQUIRE(GitCli::status(dir, st));
	CHECK(st.upstream == "origin/main");
	CHECK(st.ahead == 0);

	// Someone else pushes; our pull fast-forwards it in.
	const fs::path other = uniqueDir("panelother");
	REQUIRE(GitCli::run(fs::temp_directory_path(),
	                    { "clone", bare.string(), other.string() }).ok);
	REQUIRE(GitCli::run(other, { "config", "user.name",  "Other" }).ok);
	REQUIRE(GitCli::run(other, { "config", "user.email", "other@example.invalid" }).ok);
	REQUIRE(GitCli::run(other, { "config", "commit.gpgsign", "false" }).ok);
	writeFile(other / "theirs.txt", "hello");
	commitAll(other, "theirs");
	REQUIRE(GitCli::run(other, { "push" }).ok);

	REQUIRE(GitCli::pull(dir, &err));
	CHECK(fs::exists(dir / "theirs.txt"));

	// A DIVERGED branch must refuse rather than invent a merge.
	writeFile(other / "theirs2.txt", "more");
	commitAll(other, "theirs 2");
	REQUIRE(GitCli::run(other, { "push" }).ok);
	writeFile(dir / "mine.txt", "mine");
	REQUIRE(GitCli::addAll(dir, &err));
	REQUIRE(GitCli::commit(dir, "mine", &err));
	CHECK_FALSE(GitCli::pull(dir, &err));
	CHECK_FALSE(err.empty());

	he_test::removeQuiet(dir);
	he_test::removeQuiet(bare);
	he_test::removeQuiet(other);
}

TEST_CASE("GitHub create-repo responses map to actionable messages")
{
	// Pure response interpretation — no network, no token. The status codes are
	// the ones GitHub actually answers, including the deliberately misleading
	// 404 for a fine-grained token without permission.
	CreatedRepo repo;
	std::string err;

	CHECK(GitHubApi::parseCreateRepoResponse(201,
		R"({"clone_url":"https://github.com/anna/proj.git","full_name":"anna/proj"})",
		repo, &err));
	CHECK(repo.cloneUrl == "https://github.com/anna/proj.git");
	CHECK(repo.fullName == "anna/proj");

	CHECK_FALSE(GitHubApi::parseCreateRepoResponse(401, R"({"message":"Bad credentials"})",
	                                               repo, &err));
	CHECK(err.find("token") != std::string::npos);

	CHECK_FALSE(GitHubApi::parseCreateRepoResponse(422,
		R"({"message":"name already exists on this account"})", repo, &err));
	CHECK(err.find("already exists") != std::string::npos);

	CHECK_FALSE(GitHubApi::parseCreateRepoResponse(404, "{}", repo, &err));
	CHECK(err.find("permission") != std::string::npos);

	// Success status with a broken body must not report success.
	CHECK_FALSE(GitHubApi::parseCreateRepoResponse(201, "not json", repo, &err));
}

TEST_CASE("GitHub repository-list pages parse into clone-picker entries")
{
	// Same discipline: one page of GET /user/repos, no network. The pager in
	// listRepos decides "was this the last page?" from rawCount, so that count
	// has to include the entries the parser drops.
	std::vector<RepoListEntry> repos;
	std::string err;
	int raw = -1;

	SUBCASE("a page with public, private and odd entries")
	{
		CHECK(GitHubApi::parseListReposResponse(200, R"([
			{"name":"proj","full_name":"anna/proj","private":false,
			 "clone_url":"https://github.com/anna/proj.git",
			 "default_branch":"main","updated_at":"2026-09-30T12:00:00Z"},
			{"name":"secret","full_name":"anna/secret","private":true,
			 "clone_url":"https://github.com/anna/secret.git",
			 "default_branch":null,"updated_at":"2026-09-01T08:00:00Z",
			 "description":null},
			{"name":"no-url","full_name":"anna/no-url"},
			"not an object"
		])", repos, &err, &raw));
		CHECK(raw == 4);
		REQUIRE(repos.size() == 2);

		CHECK(repos[0].name          == "proj");
		CHECK(repos[0].fullName      == "anna/proj");
		CHECK(repos[0].cloneUrl      == "https://github.com/anna/proj.git");
		CHECK(repos[0].defaultBranch == "main");
		CHECK(repos[0].updatedAt     == "2026-09-30T12:00:00Z");
		CHECK_FALSE(repos[0].isPrivate);

		// null where a string is expected must read as empty, not throw.
		CHECK(repos[1].isPrivate);
		CHECK(repos[1].defaultBranch.empty());
	}

	SUBCASE("an empty page is a valid, final answer")
	{
		CHECK(GitHubApi::parseListReposResponse(200, "[]", repos, &err, &raw));
		CHECK(repos.empty());
		CHECK(raw == 0);
	}

	SUBCASE("a page replaces, never appends")
	{
		repos.push_back({ "stale", "x/stale", "https://github.com/x/stale.git", "", "", false });
		CHECK(GitHubApi::parseListReposResponse(200, "[]", repos, &err));
		CHECK(repos.empty());
	}

	SUBCASE("a success status without a list is not a success")
	{
		CHECK_FALSE(GitHubApi::parseListReposResponse(200, "not json", repos, &err, &raw));
		CHECK(raw == 0);
		CHECK_FALSE(GitHubApi::parseListReposResponse(200, R"({"name":"proj"})", repos, &err));
		CHECK(err.find("repository list") != std::string::npos);
	}

	SUBCASE("failures map to messages a user can act on")
	{
		CHECK_FALSE(GitHubApi::parseListReposResponse(401,
			R"({"message":"Bad credentials"})", repos, &err));
		CHECK(err.find("token") != std::string::npos);
		CHECK(repos.empty());

		CHECK_FALSE(GitHubApi::parseListReposResponse(404, "{}", repos, &err));
		CHECK(err.find("repo") != std::string::npos);

		// Rate limiting arrives as 403 with GitHub's own explanation — quote it.
		CHECK_FALSE(GitHubApi::parseListReposResponse(403,
			R"({"message":"API rate limit exceeded"})", repos, &err));
		CHECK(err.find("rate limit") != std::string::npos);
	}
}

TEST_CASE("listRepos refuses to run without a token")
{
	// No network is reached: the check runs first.
	std::vector<RepoListEntry> repos;
	std::string err;
	CHECK_FALSE(GitHubApi::listRepos("", repos, &err));
	CHECK(repos.empty());
	CHECK(err.find("token") != std::string::npos);
}

TEST_CASE("A credential reaches git's helper via stdin, never argv")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path repo = makeRepo("cred");

	// A shim helper that records what git hands it. The `!shell` helper form
	// runs through sh, which Git for Windows bundles, so this is portable.
	const fs::path sink = repo / "cred_sink.txt";
	const std::string helper =
		"!f() { test \"$1\" = store && cat >> '" + sink.generic_string() + "'; }; f";

	// The shim as the ONLY helper (override), not as a --local one: approve
	// feeds every configured helper, and Apple's git has osxkeychain set
	// system-wide — so a local shim alone wrote this fake github.com token
	// into the developer's real keychain on every run, where the editor's
	// GitHub sign-in then found it.
	std::string err;
	REQUIRE(GitCli::approveCredential(repo, "github.com", "x-access-token",
	                                  "tok_TESTVALUE_123", &err, helper));

	std::ifstream in(sink);
	REQUIRE(in.good());
	std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	CHECK(all.find("host=github.com") != std::string::npos);
	CHECK(all.find("username=x-access-token") != std::string::npos);
	CHECK(all.find("password=tok_TESTVALUE_123") != std::string::npos);

	he_test::removeQuiet(repo);
}

TEST_CASE("A stored credential can be read back out of the helper")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path repo = makeRepo("credfill");

	// A shim helper that answers `get` with fixed values. The host below is
	// deliberately unroutable: git consults the GLOBAL helper before the local
	// one, so a real hostname here would reach into the developer's own keychain
	// and pull out their actual token.
	const std::string helper =
		"!f() { test \"$1\" = get && printf 'username=tester\\npassword=tok_FILL_123\\n'; }; f";
	REQUIRE(GitCli::run(repo, { "config", "--local", "credential.helper", helper }).ok);

	std::string user, secret, err;
	REQUIRE(GitCli::fillCredential(repo, "horizon-test.invalid", user, secret, &err));
	CHECK(user   == "tester");
	CHECK(secret == "tok_FILL_123");

	he_test::removeQuiet(repo);
}

TEST_CASE("Reading a credential nobody stored fails instead of prompting")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	// The case that matters for the editor: with no helper holding anything for
	// this host, git must ANSWER — not sit waiting on a terminal nobody is
	// watching, or raise a dialog behind the editor window. If the prompt
	// suppression in fillCredential ever regresses, this test hangs, which is
	// the correct way to notice.
	const fs::path repo = makeRepo("credfill_none");
	REQUIRE(GitCli::run(repo, { "config", "--local", "credential.helper", "" }).ok);

	std::string user, secret, err;
	CHECK_FALSE(GitCli::fillCredential(repo, "horizon-test.invalid", user, secret, &err));
	CHECK(secret.empty());

	he_test::removeQuiet(repo);
}

TEST_CASE("GitHub issue/gist/user responses map to actionable messages")
{
	// Same discipline as the create-repo mapping above: pure interpretation, no
	// network. These three are what Help ▸ Report Issue relies on, and the ones
	// a user is most likely to hit are the scope failures — a token stored for
	// pushing a game usually has neither 'gist' nor issue permission on someone
	// else's repository.
	std::string err;

	SUBCASE("current user")
	{
		GitHubUser user;
		CHECK(GitHubApi::parseUserResponse(200, R"({"login":"octocat"})", user, &err));
		CHECK(user.login == "octocat");

		CHECK_FALSE(GitHubApi::parseUserResponse(401, R"({"message":"Bad credentials"})",
		                                         user, &err));
		CHECK(err.find("token") != std::string::npos);
		// A 200 that names nobody is not a success.
		CHECK_FALSE(GitHubApi::parseUserResponse(200, "{}", user, &err));
	}

	SUBCASE("gist")
	{
		CreatedGist gist;
		CHECK(GitHubApi::parseCreateGistResponse(201,
			R"({"html_url":"https://gist.github.com/octocat/abc123"})", gist, &err));
		CHECK(gist.htmlUrl == "https://gist.github.com/octocat/abc123");

		// The common one: a token that can push but was never given 'gist'.
		CHECK_FALSE(GitHubApi::parseCreateGistResponse(404, "{}", gist, &err));
		CHECK(err.find("gist") != std::string::npos);
		CHECK_FALSE(GitHubApi::parseCreateGistResponse(403, "{}", gist, &err));
		CHECK(err.find("gist") != std::string::npos);

		CHECK_FALSE(GitHubApi::parseCreateGistResponse(201, "not json", gist, &err));
	}

	SUBCASE("issue")
	{
		CreatedIssue issue;
		CHECK(GitHubApi::parseCreateIssueResponse(201,
			R"({"html_url":"https://github.com/o/r/issues/42","number":42})", issue, &err));
		CHECK(issue.htmlUrl == "https://github.com/o/r/issues/42");
		CHECK(issue.number == 42);

		CHECK_FALSE(GitHubApi::parseCreateIssueResponse(410, "{}", issue, &err));
		CHECK(err.find("disabled") != std::string::npos);

		CHECK_FALSE(GitHubApi::parseCreateIssueResponse(404, "{}", issue, &err));
		CHECK(err.find("issues") != std::string::npos);

		// 201 without an address is a failure, not a silent success.
		CHECK_FALSE(GitHubApi::parseCreateIssueResponse(201, R"({"number":42})", issue, &err));
	}
}

TEST_CASE("createIssue refuses a repository name that is not one")
{
	// owner/repo are pasted straight into the request path. They are constants
	// at today's only call site, but a path segment built from a string is
	// exactly what stops being constant later — so the guard is checked here
	// rather than trusted. No network is reached: the check runs first.
	CreatedIssue issue;
	std::string  err;
	CHECK_FALSE(GitHubApi::createIssue("tok", "owner/../../etc", "repo", "t", "b",
	                                   issue, &err));
	CHECK(err.find("invalid repository name") != std::string::npos);

	CHECK_FALSE(GitHubApi::createIssue("tok", "owner", "repo?x=1", "t", "b", issue, &err));
	CHECK(err.find("invalid repository name") != std::string::npos);
}

TEST_CASE("ensureCredentialHelper leaves a repo with a usable helper")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path repo = makeRepo("helper");
	std::string chosen, err;
	REQUIRE(GitCli::ensureCredentialHelper(repo, &chosen, &err));
	// Whichever branch ran — pre-existing global helper, or the platform default
	// just configured — the repo must end up with one.
	CHECK_FALSE(GitCli::credentialHelper(repo).empty());

	he_test::removeQuiet(repo);
}

TEST_CASE("Branches can be created from any commit, with or without switching")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path repo = makeRepo("branch");
	writeFile(repo / "a.txt", "v1");
	commitAll(repo, "first");
	const std::string firstOid =
		trimmed(GitCli::run(repo, { "rev-parse", "--short", "HEAD" }).out);
	writeFile(repo / "a.txt", "v2");
	commitAll(repo, "second");

	std::string err;

	// Off an OLD commit, without switching: the ref exists, the working tree is
	// untouched, and we are still where we were.
	REQUIRE(GitCli::createBranch(repo, "from-first", firstOid, /*checkout=*/false, &err));
	CHECK(GitCli::branchExists(repo, "from-first"));
	CHECK(readFile(repo / "a.txt") == "v2");

	std::vector<std::string> names;
	std::string current;
	REQUIRE(GitCli::listBranches(repo, names, current));
	CHECK(std::find(names.begin(), names.end(), "from-first") != names.end());
	CHECK(current == "main");

	// The branch really starts at that commit — one commit of history, not two.
	std::vector<GitCli::CommitInfo> log;
	REQUIRE(GitCli::log(repo, 10, log));
	CHECK(log.size() == 2);   // still on main

	// Now WITH switching: the working tree becomes that commit's.
	REQUIRE(GitCli::createBranch(repo, "work-here", firstOid, /*checkout=*/true, &err));
	REQUIRE(GitCli::listBranches(repo, names, current));
	CHECK(current == "work-here");
	CHECK(readFile(repo / "a.txt") == "v1");

	// Rejections that would otherwise surface as raw git errors.
	CHECK_FALSE(GitCli::createBranch(repo, "work-here", "", false, &err));
	CHECK(err.find("already exists") != std::string::npos);
	CHECK_FALSE(GitCli::createBranch(repo, "bad name with spaces", "", false, &err));
	CHECK_FALSE(err.empty());
	CHECK_FALSE(GitCli::createBranch(repo, "ok-name", "nosuchcommit", false, &err));
	CHECK(err.find("no commit") != std::string::npos);

	CHECK_FALSE(GitCli::isValidBranchName(repo, "has space"));
	CHECK_FALSE(GitCli::isValidBranchName(repo, ""));
	CHECK(GitCli::isValidBranchName(repo, "feature/new-lighting"));

	he_test::removeQuiet(repo);
}

TEST_CASE("Creating a branch works on a dirty tree; switching to it does not")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path repo = makeRepo("branchdirty");
	writeFile(repo / "a.txt", "v1");
	commitAll(repo, "first");

	// Work in progress — the exact moment "park this on a branch" is wanted.
	writeFile(repo / "a.txt", "UNCOMMITTED");

	GitService svc;
	svc.open(repo);
	auto settle = [&] {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while ((svc.busy() || svc.status().generation == 0) &&
		       std::chrono::steady_clock::now() < deadline)
		{
			svc.pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		svc.pump();
	};
	settle();
	REQUIRE(svc.isRepo());

	// Writing a ref touches nothing on disk, so this must be allowed.
	svc.requestCreateBranch("parked", "", /*checkout=*/false);
	settle();
	CHECK(svc.lastError().empty());
	CHECK(GitCli::branchExists(repo, "parked"));
	CHECK(readFile(repo / "a.txt") == "UNCOMMITTED");

	// Switching would carry the changes over or overwrite them — refused.
	svc.requestCreateBranch("switch-me", "", /*checkout=*/true);
	settle();
	CHECK_FALSE(svc.lastError().empty());
	CHECK(svc.lastError().find("uncommitted") != std::string::npos);
	CHECK(readFile(repo / "a.txt") == "UNCOMMITTED");

	svc.close();
	he_test::removeQuiet(repo);
}

TEST_CASE("Restoring to a commit rewinds the folder without losing history")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path repo = makeRepo("restore");

	writeFile(repo / "keep.txt", "v1");
	writeFile(repo / "Content" / "nested" / "asset.txt", "old");
	commitAll(repo, "first");
	const std::string first = GitCli::run(repo, { "rev-parse", "--short", "HEAD" }).out;
	const std::string firstOid = first.substr(0, first.find_first_of("\r\n"));

	// Move on: change one file, add another, delete a third.
	writeFile(repo / "keep.txt", "v2");
	writeFile(repo / "added-later.txt", "new");
	fs::remove(repo / "Content" / "nested" / "asset.txt");
	commitAll(repo, "second");

	std::string err;
	REQUIRE(GitCli::restoreWorktreeTo(repo, firstOid, &err));

	// All three kinds of difference are undone — the one `restore --source`
	// cannot do on its own is the deletion of a file added afterwards.
	CHECK(readFile(repo / "keep.txt") == "v1");
	CHECK_FALSE(fs::exists(repo / "added-later.txt"));
	CHECK(fs::exists(repo / "Content" / "nested" / "asset.txt"));

	// And the history is intact: both commits still there, HEAD still on the
	// branch tip. That is the whole difference from reset --hard.
	std::vector<GitCli::CommitInfo> log;
	REQUIRE(GitCli::log(repo, 10, log));
	CHECK(log.size() == 2);
	CHECK(GitCli::commitExists(repo, "HEAD"));

	// The difference is staged, ready to be recorded as an ordinary commit.
	RepoStatus st;
	REQUIRE(GitCli::status(repo, st));
	CHECK(st.dirtyCount() > 0);

	CHECK_FALSE(GitCli::restoreWorktreeTo(repo, "nosuchcommit", &err));
	CHECK_FALSE(err.empty());

	he_test::removeQuiet(repo);
}

TEST_CASE("A restore refuses to run over uncommitted work")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path repo = makeRepo("restoreguard");
	writeFile(repo / "a.txt", "v1");
	commitAll(repo, "first");
	const std::string oid =
		GitCli::run(repo, { "rev-parse", "--short", "HEAD" }).out.substr(0, 7);
	writeFile(repo / "a.txt", "v2");
	commitAll(repo, "second");

	// Work in progress that exists nowhere else.
	writeFile(repo / "a.txt", "PRECIOUS UNSAVED WORK");

	GitService svc;
	svc.open(repo);
	auto settle = [&] {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while ((svc.busy() || svc.status().generation == 0) &&
		       std::chrono::steady_clock::now() < deadline)
		{
			svc.pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		svc.pump();
	};
	settle();
	REQUIRE(svc.isRepo());

	svc.requestRestoreTo(oid, oid);
	settle();

	// Refused, said why, and the uncommitted file is untouched.
	CHECK_FALSE(svc.lastError().empty());
	CHECK(svc.lastError().find("uncommitted") != std::string::npos);
	CHECK(readFile(repo / "a.txt") == "PRECIOUS UNSAVED WORK");

	svc.close();
	he_test::removeQuiet(repo);
}

TEST_CASE("History reads back newest first and knows what is unpushed")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path repo = makeRepo("hist");
	const fs::path bare = uniqueDir("histbare");
	REQUIRE(GitCli::run(bare, { "init", "--bare", "--initial-branch=main" }).ok);

	writeFile(repo / "a.txt", "one");
	// A subject with the characters that break naive framing: quotes, an
	// umlaut, and a pipe. The 0x1F/0x1E separators must not care.
	commitAll(repo, "erster Commit | \"quoted\" & Umlaut ae");
	REQUIRE(GitCli::run(repo, { "remote", "add", "origin", bare.string() }).ok);
	REQUIRE(GitCli::run(repo, { "push", "-u", "origin", "main" }).ok);

	writeFile(repo / "b.txt", "two");
	commitAll(repo, "zweiter, noch nicht gepusht");

	std::vector<GitCli::CommitInfo> log;
	REQUIRE(GitCli::log(repo, 10, log));
	REQUIRE(log.size() == 2);

	// Newest first, like every git log.
	CHECK(log[0].subject == "zweiter, noch nicht gepusht");
	CHECK(log[0].unpushed);
	CHECK(log[1].subject.find("erster Commit") == 0);
	CHECK_FALSE(log[1].unpushed);
	CHECK(log[0].author == "HorizonEngine Test");
	CHECK_FALSE(log[0].shortOid.empty());
	CHECK_FALSE(log[0].relTime.empty());

	// An empty repository answers an empty history, not an error.
	const fs::path fresh = makeRepo("histempty");
	REQUIRE(GitCli::log(fresh, 10, log));
	CHECK(log.empty());

	he_test::removeQuiet(repo);
	he_test::removeQuiet(bare);
	he_test::removeQuiet(fresh);
}

TEST_CASE("The generated repo config pins its load-bearing lines")
{
	// Golden substrings rather than a byte-exact file: wording may evolve, but
	// losing one of THESE lines silently re-breaks a specific known failure.
	const std::string ign = RepoConfig::gitignoreText();
	CHECK(ign.find("Saved/") != std::string::npos);
	CHECK(ign.find("Export/") != std::string::npos);
	CHECK(ign.find("GameLogic.hot-*.*") != std::string::npos);   // hot-reload copies accumulate
	CHECK(ign.find("Source/build/") != std::string::npos);

	const std::string att = RepoConfig::gitattributesText();
	CHECK(att.find("* text=auto eol=lf") != std::string::npos);
	// NO blanket LFS globs, by explicit decision: routing is per file, by size,
	// at commit time — a 40 KB icon does not belong in LFS for being a .png,
	// nor every .hasset for some holding meshes.
	CHECK(att.find("filter=lfs") == std::string::npos);
	// Scenes are deliberately NOT merge=binary (stable entity ids since CP-A) —
	// the graph formats still are.
	CHECK(att.find(".hescene merge=binary") == std::string::npos);
	CHECK(att.find("*.hcode  merge=binary") != std::string::npos);
}

TEST_CASE("Auto-LFS candidacy is by media category, never by container alone")
{
	using RC = RepoConfig;
	// The three categories the routing covers…
	CHECK(RC::isAutoLfsCandidate("Content/Meshes/rock.fbx"));
	CHECK(RC::isAutoLfsCandidate("Content/Textures/sky.EXR"));   // case-insensitive
	CHECK(RC::isAutoLfsCandidate("Content/Audio/theme.wav"));
	CHECK(RC::isAutoLfsCandidate("Content/Meshes/Rock.hasset"));
	// …and nothing else, whatever its size story turns out to be.
	CHECK_FALSE(RC::isAutoLfsCandidate("Content/Scenes/main.hescene"));
	CHECK_FALSE(RC::isAutoLfsCandidate("Content/Graphs/door.hcode"));
	CHECK_FALSE(RC::isAutoLfsCandidate("Export/game.hpak"));
	CHECK_FALSE(RC::isAutoLfsCandidate("README"));
}

TEST_CASE("A commit routes big media into LFS per file and leaves small media alone")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }
	const fs::path probe = uniqueDir("lfsprobe");
	const bool lfs = GitCli::lfsAvailable(probe);
	he_test::removeQuiet(probe);
	if (!lfs) { MESSAGE("git-lfs not installed — skipped"); return; }

	const fs::path repo = makeRepo("lfssize");
	REQUIRE(GitCli::run(repo, { "lfs", "install", "--local" }).ok);
	REQUIRE(RepoConfig::writeInitialFiles(repo));

	// One texture over the threshold, one under it.
	{
		std::ofstream big(repo / "big.png", std::ios::binary);
		std::vector<char> chunk(1024 * 1024, 'x');
		for (int i = 0; i < 51; ++i) big.write(chunk.data(), chunk.size());
	}
	writeFile(repo / "small.png", "tiny");

	// Drive the same worker the panel uses.
	GitService svc;
	svc.open(repo);
	auto waitIdle = [&] {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while ((svc.busy() || svc.status().generation == 0) &&
		       std::chrono::steady_clock::now() < deadline)
		{
			svc.pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		svc.pump();
	};
	waitIdle();
	REQUIRE(svc.isRepo());

	svc.requestCommitAll("big media");
	waitIdle();
	CHECK(svc.lastError().empty());

	// The big file went through LFS, the small one stayed plain git.
	const GitResult ls = GitCli::run(repo, { "lfs", "ls-files", "--name-only" });
	REQUIRE(ls.ok);
	CHECK(ls.out.find("big.png") != std::string::npos);
	CHECK(ls.out.find("small.png") == std::string::npos);

	// And .gitattributes gained exactly the per-file entry.
	{
		std::ifstream in(repo / ".gitattributes");
		std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		CHECK(all.find("big.png") != std::string::npos);
		CHECK(all.find("*.png") == std::string::npos);
	}

	svc.close();
	he_test::removeQuiet(repo);
}

TEST_CASE("GitService keeps git off the calling thread and delivers on pump")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path repo = makeRepo("service");
	writeFile(repo / "Content" / "Thing.txt", "hi");

	GitService svc;
	int    statusCallbacks = 0;
	svc.setOnStatusChanged([&](const RepoStatus&) { ++statusCallbacks; });

	svc.open(repo);

	// Nothing is applied until pump() runs — the worker never touches the state
	// the main thread reads.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (!svc.isRepo() && std::chrono::steady_clock::now() < deadline)
	{
		svc.pump();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	REQUIRE(svc.isRepo());
	CHECK(statusCallbacks >= 1);
	CHECK(svc.status().find("Content/Thing.txt") != nullptr);
	CHECK(svc.status().generation >= 1);
	CHECK(svc.lastError().empty());

	// A burst of requests must not queue a refresh per call — status is a
	// whole-tree snapshot, so running it ten times yields the same answer ten
	// times and delays the one that matters.
	const std::uint64_t before = svc.status().generation;
	for (int i = 0; i < 10; ++i) svc.requestStatus();

	const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (svc.status().generation == before && std::chrono::steady_clock::now() < deadline2)
	{
		svc.pump();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(svc.status().generation > before);
	// Far fewer than ten refreshes actually ran.
	CHECK(svc.status().generation < before + 10);

	// close() must return promptly rather than waiting on a worker with no bound.
	const auto t0 = std::chrono::steady_clock::now();
	svc.close();
	CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20));
	CHECK_FALSE(svc.isRepo());

	he_test::removeAllQuiet(repo);
}

TEST_CASE("Opening a directory that is not a repository is an answer, not an error")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path plain = uniqueDir("plain");

	GitService svc;
	svc.open(plain);

	// Most projects are simply not under source control yet; the UI offers to
	// create a repository rather than reporting a failure.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (svc.busy() && std::chrono::steady_clock::now() < deadline)
	{
		svc.pump();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	svc.pump();

	CHECK_FALSE(svc.isRepo());
	CHECK(svc.lastError().empty());

	svc.close();
	he_test::removeAllQuiet(plain);
}

// ─── Cloning an existing repository ─────────────────────────────────────────
// Against a local bare repository through a file:// URL: the same clone, LFS
// install and LFS download the editor runs against GitHub, minus the network.

namespace {

std::string fileUrl(const fs::path& p)
{
	const std::string g = p.generic_string();
	// POSIX paths already start with '/'; a Windows "C:/..." needs the third one.
	return "file://" + std::string(g.rfind('/', 0) == 0 ? "" : "/") + g;
}

bool lfsInstalled()
{
	const fs::path probe = uniqueDir("lfsprobe_clone");
	const bool lfs = GitCli::lfsAvailable(probe);
	he_test::removeQuiet(probe);
	return lfs;
}

bool dirIsEmpty(const fs::path& p)
{
	std::error_code ec;
	return fs::is_directory(p, ec) && fs::directory_iterator(p, ec) == fs::directory_iterator();
}

// Drive a service until its queue is drained, like waitIdle in the LFS test.
void pumpUntilIdle(GitService& svc, int seconds = 60)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	while (svc.busy() && std::chrono::steady_clock::now() < deadline)
	{
		svc.pump();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	svc.pump();
}

// A bare remote holding one commit with one LFS-tracked binary and one plain
// text file. `outBinary` receives the binary's exact bytes.
fs::path makeLfsRemote(const char* stem, std::string& outBinary)
{
	const fs::path bare = uniqueDir((std::string(stem) + "_bare").c_str());
	REQUIRE(GitCli::run(bare, { "init", "--bare", "--initial-branch=main" }).ok);

	const fs::path src = makeRepo((std::string(stem) + "_src").c_str());
	REQUIRE(GitCli::run(src, { "lfs", "install", "--local" }).ok);
	REQUIRE(GitCli::run(src, { "lfs", "track", "*.bin" }).ok);
	outBinary.clear();
	for (int i = 0; i < 4096; ++i) outBinary.push_back(static_cast<char>((i * 131 + 7) & 0xff));
	writeFile(src / "Content" / "asset.bin", outBinary);
	writeFile(src / "readme.txt", "plain");
	commitAll(src, "assets");
	REQUIRE(GitCli::run(src, { "remote", "add", "origin", fileUrl(bare) }).ok);
	REQUIRE(GitCli::run(src, { "push", "-u", "origin", "main" }).ok);
	he_test::removeAllQuiet(src);
	return bare;
}

} // namespace

TEST_CASE("Clone URLs with credentials in them or without a helper path are refused")
{
	std::string why;
	CHECK(GitCli::isSafeCloneUrl("https://github.com/owner/repo.git"));
	CHECK(GitCli::isSafeCloneUrl("https://github.example.com:8443/owner/repo.git"));
	CHECK(GitCli::isSafeCloneUrl("file:///tmp/some bare.git"));   // local, spaces fine

	// The rule of the whole module: never a token in the URL.
	CHECK_FALSE(GitCli::isSafeCloneUrl("https://ghp_SECRET@github.com/owner/repo.git", &why));
	CHECK(why.find("credential helper") != std::string::npos);
	CHECK_FALSE(GitCli::isSafeCloneUrl("https://x-access-token:ghp_SECRET@github.com/o/r.git"));

	CHECK_FALSE(GitCli::isSafeCloneUrl("http://github.com/owner/repo.git"));
	CHECK_FALSE(GitCli::isSafeCloneUrl("git@github.com:owner/repo.git"));
	CHECK_FALSE(GitCli::isSafeCloneUrl("ssh://git@github.com/owner/repo.git"));
	CHECK_FALSE(GitCli::isSafeCloneUrl("--upload-pack=touch /tmp/pwned"));
	CHECK_FALSE(GitCli::isSafeCloneUrl("https://github.com/o/r.git\n"));
	CHECK_FALSE(GitCli::isSafeCloneUrl("https:///owner/repo.git"));
	CHECK_FALSE(GitCli::isSafeCloneUrl(""));

	CHECK(GitCli::urlHost("https://github.com/owner/repo.git") == "github.com");
	CHECK(GitCli::urlHost("https://tok@github.com/owner/repo.git").empty());
	CHECK(GitCli::urlHost("file:///tmp/x.git").empty());
}

TEST_CASE("A clone refuses a folder that already holds files and leaves them alone")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path bare = uniqueDir("clone_refuse_bare");
	REQUIRE(GitCli::run(bare, { "init", "--bare", "--initial-branch=main" }).ok);

	const fs::path target = uniqueDir("clone_refuse_target");
	writeFile(target / "keep.txt", "mine");

	std::string err;
	CHECK_FALSE(GitCli::clone(fileUrl(bare), target, {}, &err));
	CHECK(err.find("not empty") != std::string::npos);
	CHECK(readFile(target / "keep.txt") == "mine");
	CHECK_FALSE(fs::exists(target / ".git"));

	he_test::removeAllQuiet(target);
	he_test::removeAllQuiet(bare);
}

TEST_CASE("A failed clone leaves the target exactly as empty as it was")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path missing = uniqueDir("clone_missing_remote") / "does-not-exist.git";

	// A folder that existed (empty) stays, empty; one the clone created goes.
	const fs::path existing = uniqueDir("clone_fail_existing");
	std::string err;
	CHECK_FALSE(GitCli::clone(fileUrl(missing), existing, {}, &err));
	CHECK_FALSE(err.empty());
	CHECK(dirIsEmpty(existing));

	const fs::path fresh = uniqueDir("clone_fail_parent") / "NewProject";
	CHECK_FALSE(GitCli::clone(fileUrl(missing), fresh, {}, &err));
	CHECK_FALSE(fs::exists(fresh));

	he_test::removeAllQuiet(existing);
	he_test::removeAllQuiet(fresh.parent_path());
	he_test::removeAllQuiet(missing.parent_path());
}

TEST_CASE("A freshly created, empty remote clones into a repository with no commits")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	// What a brand-new GitHub repository looks like: no commits at all.
	const fs::path bare = uniqueDir("clone_empty_bare");
	REQUIRE(GitCli::run(bare, { "init", "--bare", "--initial-branch=main" }).ok);
	const fs::path target = uniqueDir("clone_empty_parent") / "Empty";

	std::string err;
	REQUIRE(GitCli::clone(fileUrl(bare), target, {}, &err));
	RepoStatus st;
	REQUIRE(GitCli::status(target, st, &err));
	CHECK(st.isRepo);
	std::vector<GitCli::CommitInfo> commits;
	CHECK(GitCli::log(target, 5, commits, &err));
	CHECK(commits.empty());
	CHECK_FALSE(GitCli::usesLfs(target));
	CHECK(GitCli::remoteUrl(target) == fileUrl(bare));

	he_test::removeAllQuiet(target.parent_path());
	he_test::removeAllQuiet(bare);
}

TEST_CASE("A credential override replaces the configured helpers for one command")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	// Before a clone there is no repository to hold a --local helper, so the
	// helper is passed per command. Outside any repository here, on purpose.
	// The host is unroutable and the override REPLACES every configured helper,
	// so a developer's real keychain never receives this test value.
	const fs::path dir  = uniqueDir("cred_override");
	const fs::path sink = dir / "sink.txt";
	const std::string helper =
		"!f() { test \"$1\" = store && cat >> '" + sink.generic_string() + "'; }; f";

	std::string err;
	REQUIRE(GitCli::approveCredential(dir, "horizon-test.invalid", "x-access-token",
	                                  "tok_CLONE_123", &err, helper));
	const std::string all = readFile(sink);
	CHECK(all.find("host=horizon-test.invalid") != std::string::npos);
	CHECK(all.find("password=tok_CLONE_123") != std::string::npos);

	CHECK_FALSE(GitCli::defaultCredentialHelper().empty());

	he_test::removeAllQuiet(dir);
}

TEST_CASE("The service refuses a clone URL with a token in it before storing anything")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path target = uniqueDir("clone_tokenurl_parent") / "Project";

	// No open() first: a clone is what happens before any project is open.
	GitService svc;
	svc.requestClone("https://ghp_SECRET@github.com/owner/repo.git", target, "tok_UNUSED");
	pumpUntilIdle(svc);

	CHECK(svc.lastError().find("credential helper") != std::string::npos);
	CHECK(svc.lastClonedRoot().empty());
	CHECK_FALSE(fs::exists(target));

	svc.close();
	he_test::removeAllQuiet(target.parent_path());
}

TEST_CASE("Cloning through the service brings the working tree and its LFS assets")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }
	if (!lfsInstalled()) { MESSAGE("git-lfs not installed — skipped"); return; }

	std::string binary;
	const fs::path bare   = makeLfsRemote("clone_lfs", binary);
	const fs::path target = uniqueDir("clone_lfs_parent") / "Project";
	const std::string url = fileUrl(bare);

	GitService svc;
	svc.requestClone(url, target);
	pumpUntilIdle(svc);

	CHECK(svc.lastError().empty());
	CHECK(svc.lastInfo().find("Cloned") != std::string::npos);
	CHECK(svc.lastClonedRoot() == target);
	CHECK(svc.isRepo());
	CHECK(svc.remoteUrl() == url);

	// Real bytes, not the LFS pointer text the skip-smudge clone left behind.
	const std::string got = readFile(target / "Content" / "asset.bin");
	CHECK(got.size() == binary.size());
	CHECK(got == binary);
	CHECK(got.find("git-lfs.github.com/spec") == std::string::npos);
	CHECK(readFile(target / "readme.txt") == "plain");
	CHECK(GitCli::usesLfs(target));

	// The filter lives in the repository itself, so later pulls smudge too.
	CHECK(GitCli::run(target, { "config", "--local", "--get", "filter.lfs.process" }).ok);
	// And a helper is reachable for push/pull (system, global or repo-local).
	CHECK_FALSE(GitCli::credentialHelper(target).empty());
	// Clean: the download did not leave the tree looking modified.
	CHECK(svc.status().dirtyCount() == 0);

	svc.close();
	he_test::removeAllQuiet(target.parent_path());
	he_test::removeAllQuiet(bare);
}

TEST_CASE("A clone whose LFS download fails still reports the repository it made")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }
	if (!lfsInstalled()) { MESSAGE("git-lfs not installed — skipped"); return; }

	std::string binary;
	const fs::path bare = makeLfsRemote("clone_lfsfail", binary);
	// The objects vanish from the remote: git history is intact, LFS is not.
	he_test::removeAllQuiet(bare / "lfs" / "objects");
	REQUIRE_FALSE(fs::exists(bare / "lfs" / "objects"));

	const fs::path target = uniqueDir("clone_lfsfail_parent") / "Project";
	GitService svc;
	svc.requestClone(fileUrl(bare), target);
	pumpUntilIdle(svc);

	// Partial success, said as such: the error names LFS, and the working tree
	// is handed over anyway so the UI can open it and retry the download.
	CHECK(svc.lastError().find("LFS") != std::string::npos);
	CHECK(svc.lastClonedRoot() == target);
	CHECK(svc.isRepo());
	CHECK(readFile(target / "readme.txt") == "plain");

	// The retry reports the same missing objects instead of pretending.
	svc.requestLfsPull();
	pumpUntilIdle(svc);
	CHECK_FALSE(svc.lastError().empty());

	svc.close();
	he_test::removeAllQuiet(target.parent_path());
	he_test::removeAllQuiet(bare);
}

TEST_CASE("The controller clones beside the open project without re-targeting it")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	// What the clone dialog drives: a project is open (no remote), and a
	// repository from elsewhere is cloned as a NEW project. GitService's clone
	// re-targets the service it runs on, so the controller must run it on one
	// of its own — the open project keeps its status and its (absent) remote.
	const fs::path open = makeRepo("clonectl_open");
	writeFile(open / "Open.heproj", "{}");
	commitAll(open, "open project");

	const fs::path bare = uniqueDir("clonectl_bare");
	REQUIRE(GitCli::run(bare, { "init", "--bare", "--initial-branch=main" }).ok);
	const fs::path src = makeRepo("clonectl_src");
	writeFile(src / "Cloned.heproj", "{}");
	writeFile(src / "Content" / "readme.txt", "cloned");
	commitAll(src, "cloned project");
	REQUIRE(GitCli::run(src, { "remote", "add", "origin", fileUrl(bare) }).ok);
	REQUIRE(GitCli::run(src, { "push", "-u", "origin", "main" }).ok);
	he_test::removeAllQuiet(src);

	GitController git;
	git.openProject(open);
	std::uint64_t now = 1;
	const auto frames = [&](auto until, int seconds) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
		while (!until() && std::chrono::steady_clock::now() < deadline)
		{
			git.update(now);
			now += 100;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	};
	frames([&] { return git.status().generation != 0; }, 10);
	REQUIRE(git.isRepo());
	const std::string openRoot = git.status().root;

	const fs::path parent = uniqueDir("clonectl_parent");
	const fs::path target = parent / "Cloned";
	git.requestClone(fileUrl(bare), target, {});
	// Busy from the call itself, not from the first frame after it.
	CHECK(git.cloneBusy());
	frames([&] { return !git.cloneBusy(); }, 60);
	REQUIRE_FALSE(git.cloneBusy());

	// The first frame that says "done" already carries the result — the worker
	// reports idle only after queuing it, and the controller reads idle before
	// it pumps. A dialog reading this state never sees "finished, no root".
	CHECK(git.cloneError().empty());
	CHECK(git.clonedRoot() == target);
	CHECK(fs::exists(target / "Cloned.heproj"));
	CHECK(readFile(target / "Content" / "readme.txt") == "cloned");

	// The open project is untouched: same repository, still no remote.
	frames([&] { return false; }, 1);   // let a status poll land, if one would
	CHECK(git.projectRoot() == open);
	CHECK(git.status().root == openRoot);
	CHECK(git.remoteUrl().empty());

	// A second clone starts with a clean slate — no "Cloned … into …" from the
	// first standing in as its progress.
	const fs::path target2 = parent / "Again";
	git.requestClone(fileUrl(bare), target2, {});
	CHECK(git.cloneInfo().empty());
	CHECK(git.cloneError().empty());
	CHECK(git.clonedRoot().empty());
	frames([&] { return !git.cloneBusy(); }, 60);
	CHECK(git.clonedRoot() == target2);

	git.finishClone();
	CHECK_FALSE(git.cloneBusy());
	git.closeProject();
	he_test::removeAllQuiet(parent);
	he_test::removeAllQuiet(bare);
	he_test::removeQuiet(open);
}

// ─── Live: a real repository on the own GitHub account ──────────────────────
// Skipped unless asked for. What the clone dialog does, call for call, against
// github.com: list the account's repositories with a token, pick one, clone it
// with that token into an empty folder (LFS assets included), open the .heproj
// the way the Project Hub does. The dialog itself cannot be drawn here —
// AppContext hangs on HE_IMGUI_ENABLED — so this drives the controller under it.
//
//   HE_GITHUB_LIVE_TOKEN_FILE  a file holding a token with 'repo' scope
//   HE_GITHUB_LIVE_REPO        owner/name of a PRIVATE repository made from the seed
//
// Run it with a git config that holds NO github.com credential of its own:
//   GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=<file: [credential] helper = cache --timeout=900>
// A machine-wide helper (gh auth git-credential, the keychain) would otherwise
// answer for the token, and the test would prove nothing about the path the
// token takes. It refuses to run in that case, and its first clone, the one
// without a token, must fail.
//
// Seed, once: HE_GITHUB_LIVE_SEED_DIR=<new folder> writes a fresh project plus
// the LFS probe file; commit it with the probe tracked by LFS, push it to a
// private repository.

namespace {

// Deterministic bytes, so the seed and the check agree without sharing a file.
// Binary on purpose (NULs, high bytes): neither a pointer file nor a text
// conversion survives the comparison.
std::string lfsProbeBytes()
{
	std::string out(192 * 1024, '\0');
	std::uint32_t x = 0x48454C46u;
	for (char& c : out)
	{
		x = x * 1664525u + 1013904223u;
		c = static_cast<char>(x >> 24);
	}
	return out;
}

constexpr const char* kLfsProbePath = "Content/Textures/lfs_probe.bin";

std::string envOr(const char* name)
{
	const char* v = std::getenv(name);
	return v ? std::string(v) : std::string{};
}

} // namespace

TEST_CASE("Live seed: a fresh project with an LFS probe file for the clone check")
{
	const std::string dir = envOr("HE_GITHUB_LIVE_SEED_DIR");
	if (dir.empty()) { MESSAGE("HE_GITHUB_LIVE_SEED_DIR not set — skipped"); return; }

	ProjectManager pm;
	REQUIRE(pm.createNewProject(dir, "CloneVerify"));
	writeFile(fs::path(dir) / kLfsProbePath, lfsProbeBytes());
	MESSAGE("seeded " << pm.currentProject().path);
}

TEST_CASE("Live: a repository from the own GitHub account clones and opens as a project")
{
	const std::string tokenFile = envOr("HE_GITHUB_LIVE_TOKEN_FILE");
	const std::string repoName  = envOr("HE_GITHUB_LIVE_REPO");
	if (tokenFile.empty() || repoName.empty())
	{
		MESSAGE("HE_GITHUB_LIVE_TOKEN_FILE / HE_GITHUB_LIVE_REPO not set — skipped");
		return;
	}
	REQUIRE(gitAvailable());
	REQUIRE(lfsInstalled());

	std::string token = trimmed(readFile(tokenFile));
	REQUIRE_MESSAGE(!token.empty(), "the token file is empty");

	const fs::path parent = uniqueDir("live_clone");

	// No credential for github.com anywhere in the helper chain, or everything
	// below would pass on someone else's.
	{
		HE::Proc::Options o;
		o.exe       = "git";
		o.args      = { "credential", "fill" };
		o.cwd       = parent;
		o.timeoutMs = 15000;
		o.env.emplace_back("GIT_TERMINAL_PROMPT", "0");
		o.stdinData = "protocol=https\nhost=github.com\n\n";
		const HE::Proc::Result r = HE::Proc::run(o);
		REQUIRE_MESSAGE(!r.ok(), "the git config already answers for github.com — run with "
		                         "GIT_CONFIG_NOSYSTEM=1 and a GIT_CONFIG_GLOBAL holding only "
		                         "a cache helper");
	}

	GitController git;
	std::uint64_t now = 1;
	const auto frames = [&](auto until, int seconds) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
		while (!until() && std::chrono::steady_clock::now() < deadline)
		{
			git.update(now);
			now += 100;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	};

	// ── 1. "Load my repositories" ───────────────────────────────────────────
	git.requestListRepos(token);
	frames([&] { return git.repoListLoaded() && !git.listingRepos(); }, 60);
	REQUIRE(git.repoListLoaded());
	REQUIRE_MESSAGE(git.repoListError().empty(), git.repoListError());
	MESSAGE("repositories listed: " << git.repoList().size());

	// ── 2. Pick one ─────────────────────────────────────────────────────────
	const RepoListEntry* chosen = nullptr;
	for (const auto& r : git.repoList())
		if (r.fullName == repoName) { chosen = &r; break; }
	REQUIRE_MESSAGE(chosen != nullptr, repoName << " is not in the account's list");
	const RepoListEntry entry = *chosen;
	CHECK(entry.isPrivate);             // so it is the token that gets the clone in
	CHECK_FALSE(entry.defaultBranch.empty());
	CHECK(entry.cloneUrl.rfind("https://github.com/", 0) == 0);
	CHECK(entry.cloneUrl.find('@') == std::string::npos);
	MESSAGE("picked " << entry.fullName << " (" << entry.cloneUrl << ", default branch "
	                  << entry.defaultBranch << ")");

	// ── Negative control: the same clone without a token does not get in ────
	const fs::path refused = parent / "NoToken";
	git.requestClone(entry.cloneUrl, refused, {});
	frames([&] { return !git.cloneBusy(); }, 120);
	REQUIRE_FALSE(git.cloneBusy());
	CHECK_FALSE(git.cloneError().empty());
	CHECK(git.clonedRoot().empty());
	{
		std::error_code ec;
		CHECK((!fs::exists(refused, ec) || fs::is_empty(refused, ec)));
	}
	MESSAGE("without a token: " << git.cloneError());
	git.finishClone();

	// ── 3. Clone into an empty folder, with the token ───────────────────────
	const fs::path target = parent / entry.name;
	git.requestClone(entry.cloneUrl, target, token);
	std::string lastPhase;
	frames([&] {
		if (git.cloneInfo() != lastPhase)
		{
			lastPhase = git.cloneInfo();
			if (!lastPhase.empty()) MESSAGE("phase: " << lastPhase);
		}
		return !git.cloneBusy();
	}, 600);
	REQUIRE_FALSE(git.cloneBusy());
	REQUIRE_MESSAGE(git.cloneError().empty(), git.cloneError());
	REQUIRE(git.clonedRoot() == target);
	MESSAGE("result: " << git.cloneInfo());

	// The LFS asset is the real file, not its pointer.
	const std::string probe = readFile(target / kLfsProbePath);
	CHECK(probe.rfind("version https://git-lfs", 0) != 0);
	CHECK(probe.size() == lfsProbeBytes().size());
	CHECK(probe == lfsProbeBytes());
	const GitResult lfsFiles = GitCli::run(target, { "lfs", "ls-files" });
	REQUIRE(lfsFiles.ok);
	MESSAGE("git lfs ls-files: " << trimmed(lfsFiles.out));
	CHECK(lfsFiles.out.find(std::string("* ") + kLfsProbePath) != std::string::npos);

	// The token went to the helper, not into the repository.
	const GitResult remote = GitCli::run(target, { "remote", "get-url", "origin" });
	REQUIRE(remote.ok);
	CHECK(trimmed(remote.out) == entry.cloneUrl);
	CHECK(readFile(target / ".git" / "config").find(token) == std::string::npos);

	// ── 4. Open it: the .heproj at the top, loaded the way the Hub does ─────
	fs::path heproj;
	for (const auto& e : fs::directory_iterator(target))
		if (e.is_regular_file() && e.path().extension() == ".heproj") { heproj = e.path(); break; }
	REQUIRE_MESSAGE(!heproj.empty(), "no .heproj at the top of the clone");
	ProjectManager pm;
	REQUIRE(pm.loadProject(heproj.string()));
	CHECK(fs::equivalent(fs::path(pm.projectRoot()), target));
	MESSAGE("opened " << heproj.filename().string() << " as \"" << pm.currentProject().name
	                  << "\" from " << pm.projectRoot());

	std::fill(token.begin(), token.end(), '\0');
	pm.closeProject();
	git.finishClone();
	he_test::removeAllQuiet(parent);
}
