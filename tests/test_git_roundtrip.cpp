#include "doctest.h"
#include "TestFsUtil.h"

#include <SourceControl/GitCli.h>
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
	const std::string spaced = "Content/My Meshes/Big Rock.txt";
	const std::string utf8   = "Content/Grüße/日本語.txt";
	writeFile(repo / spaced, "a");
	writeFile(repo / utf8,   "b");
#ifndef _WIN32
	// Windows filenames cannot contain a quote at all, so this half only runs
	// where the filesystem permits it.
	const std::string quoted = "Content/weird\"name.txt";
	writeFile(repo / quoted, "c");
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
