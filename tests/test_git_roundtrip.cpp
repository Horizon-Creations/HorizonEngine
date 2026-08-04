#include "doctest.h"
#include "TestFsUtil.h"

#include <SourceControl/GitCli.h>
#include <SourceControl/GitHubApi.h>
#include <SourceControl/RepoConfig.h>

#include "../src/HE_Editor/GitController.h"
#include <SourceControl/GitService.h>
#include <SourceControl/RepoStatus.h>
#include <Platform/Process.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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

TEST_CASE("A credential reaches git's helper via stdin, never argv")
{
	if (!gitAvailable()) { MESSAGE("git not installed — skipped"); return; }

	const fs::path repo = makeRepo("cred");

	// A shim helper that records what git hands it. The `!shell` helper form
	// runs through sh, which Git for Windows bundles, so this is portable.
	const fs::path sink = repo / "cred_sink.txt";
	const std::string helper =
		"!f() { test \"$1\" = store && cat >> '" + sink.generic_string() + "'; }; f";
	REQUIRE(GitCli::run(repo, { "config", "--local", "credential.helper", helper }).ok);

	std::string err;
	REQUIRE(GitCli::approveCredential(repo, "github.com", "x-access-token",
	                                  "tok_TESTVALUE_123", &err));

	std::ifstream in(sink);
	REQUIRE(in.good());
	std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	CHECK(all.find("host=github.com") != std::string::npos);
	CHECK(all.find("username=x-access-token") != std::string::npos);
	CHECK(all.find("password=tok_TESTVALUE_123") != std::string::npos);

	he_test::removeQuiet(repo);
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
	CHECK(att.find("*.hpak filter=lfs") != std::string::npos);
	CHECK(att.find("*.hasset filter=lfs") != std::string::npos);
	// Scenes are deliberately NOT merge=binary (stable entity ids since CP-A) —
	// the graph formats still are.
	CHECK(att.find(".hescene merge=binary") == std::string::npos);
	CHECK(att.find("*.hcode  merge=binary") != std::string::npos);
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
