#include "doctest.h"
#include "TestFsUtil.h"

#include <SourceControl/GitCli.h>
#include <SourceControl/GitService.h>
#include <SourceControl/RepoStatus.h>
#include <Platform/Process.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// ─── The Source Control panel's per-file operations ──────────────────────────
// Stage, unstage, discard, commit-staged, amend, switch (with stash), commit
// contents. Against a real repository like test_git_roundtrip.cpp - what breaks
// in these is the exact command line and exit code.

namespace fs = std::filesystem;
using namespace HE::Sc;

namespace {

bool gitAvailable()
{
	static const bool available = HE::Proc::which("git").has_value();
	return available;
}

fs::path uniqueDir(const char* stem)
{
	static const auto salt =
		static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count());
	static int counter = 0;
	const fs::path p = fs::temp_directory_path() /
	                   ("he_gitops_" + std::string(stem) + "_" + std::to_string(salt) + "_" +
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

std::string readFile(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string fileUrl(const fs::path& p)
{
	const std::string g = p.generic_string();
	return "file://" + std::string(g.rfind('/', 0) == 0 ? "" : "/") + g;
}

fs::path makeRepo(const char* stem)
{
	const fs::path dir = uniqueDir(stem);
	REQUIRE(GitCli::run(dir, { "init", "--initial-branch=main" }).ok);
	REQUIRE(GitCli::run(dir, { "config", "user.name",  "HorizonEngine Test" }).ok);
	REQUIRE(GitCli::run(dir, { "config", "user.email", "test@example.invalid" }).ok);
	REQUIRE(GitCli::run(dir, { "config", "commit.gpgsign", "false" }).ok);
	return dir;
}

void commitAll(const fs::path& repo, const char* message)
{
	REQUIRE(GitCli::run(repo, { "add", "-A" }).ok);
	REQUIRE(GitCli::run(repo, { "commit", "-m", message }).ok);
}

RepoStatus statusOf(const fs::path& repo)
{
	RepoStatus st;
	REQUIRE(GitCli::status(repo, st));
	return st;
}

// Runs a service request to completion and returns when its answer was pumped.
struct Svc
{
	GitService svc;
	explicit Svc(const fs::path& repo)
	{
		svc.open(repo);
		settle();
		REQUIRE(svc.isRepo());
	}
	~Svc() { svc.close(); }
	void settle()
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while ((svc.busy() || svc.status().generation == 0) &&
		       std::chrono::steady_clock::now() < deadline)
		{
			svc.pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		svc.pump();
	}
};

} // namespace

TEST_CASE("Staging takes exactly the named files, and unstaging gives them back")
{
	if (!gitAvailable()) { MESSAGE("git not installed - skipped"); return; }
	const fs::path repo = makeRepo("stage");
	writeFile(repo / "keep.txt", "v1");
	writeFile(repo / "gone.txt", "v1");
	commitAll(repo, "first");

	writeFile(repo / "keep.txt", "v2");
	fs::remove(repo / "gone.txt");
	writeFile(repo / "new one.txt", "n");
	writeFile(repo / "[draft]*.txt", "glob");   // a name git would read as a pathspec pattern

	std::string err;
	REQUIRE(GitCli::stage(repo, { "keep.txt", "gone.txt", "[draft]*.txt" }, &err));
	RepoStatus st = statusOf(repo);
	REQUIRE(st.find("keep.txt"));
	CHECK(st.find("keep.txt")->staged());
	CHECK(st.find("gone.txt")->index == FileState::Deleted);    // a deletion stages too
	REQUIRE(st.find("[draft]*.txt"));
	CHECK(st.find("[draft]*.txt")->staged());
	CHECK_FALSE(st.find("new one.txt")->staged());              // literal: not swept in by the glob

	REQUIRE(GitCli::unstage(repo, { "keep.txt", "gone.txt", "[draft]*.txt" }, &err));
	st = statusOf(repo);
	CHECK_FALSE(st.hasStagedChanges());
	CHECK(readFile(repo / "keep.txt") == "v2");                 // the working tree is untouched

	he_test::removeAllQuiet(repo);
}

TEST_CASE("Unstaging works before the first commit, and undoes a rename as a pair")
{
	if (!gitAvailable()) { MESSAGE("git not installed - skipped"); return; }
	const fs::path repo = makeRepo("unstage_initial");
	writeFile(repo / "a.txt", "a");
	std::string err;
	REQUIRE(GitCli::stage(repo, { "a.txt" }, &err));
	CHECK(statusOf(repo).hasStagedChanges());
	REQUIRE(GitCli::unstage(repo, { "a.txt" }, &err));       // no HEAD yet
	CHECK_FALSE(statusOf(repo).hasStagedChanges());
	CHECK(fs::exists(repo / "a.txt"));

	commitAll(repo, "first");
	REQUIRE(GitCli::run(repo, { "mv", "a.txt", "b.txt" }).ok);
	RepoStatus st = statusOf(repo);
	REQUIRE(st.find("b.txt"));
	CHECK(st.find("b.txt")->index == FileState::Renamed);
	REQUIRE(GitCli::unstage(repo, { "b.txt" }, &err));
	st = statusOf(repo);
	CHECK_FALSE(st.hasStagedChanges());                       // the old name is no longer a staged delete
	he_test::removeAllQuiet(repo);
}

TEST_CASE("Discarding throws a change away: edited, deleted, new and untracked")
{
	if (!gitAvailable()) { MESSAGE("git not installed - skipped"); return; }
	const fs::path repo = makeRepo("discard");
	writeFile(repo / "edit.txt", "v1");
	writeFile(repo / "del.txt", "v1");
	writeFile(repo / "staged_edit.txt", "v1");
	commitAll(repo, "first");

	writeFile(repo / "edit.txt", "v2");
	fs::remove(repo / "del.txt");
	writeFile(repo / "staged_edit.txt", "v2");
	std::string err;
	REQUIRE(GitCli::stage(repo, { "staged_edit.txt" }, &err));
	writeFile(repo / "added.txt", "new");
	REQUIRE(GitCli::stage(repo, { "added.txt" }, &err));
	writeFile(repo / "untracked.txt", "u");

	REQUIRE(GitCli::discard(repo, { "edit.txt", "del.txt", "staged_edit.txt", "added.txt",
	                                "untracked.txt" }, &err));
	CHECK(readFile(repo / "edit.txt") == "v1");
	CHECK(readFile(repo / "del.txt") == "v1");
	CHECK(readFile(repo / "staged_edit.txt") == "v1");
	CHECK_FALSE(fs::exists(repo / "added.txt"));
	CHECK_FALSE(fs::exists(repo / "untracked.txt"));
	CHECK(statusOf(repo).dirtyCount() == 0);
	he_test::removeAllQuiet(repo);
}

TEST_CASE("Discarding a conflicted file is refused; resolving keeps one side")
{
	if (!gitAvailable()) { MESSAGE("git not installed - skipped"); return; }
	const fs::path repo = makeRepo("conflict");
	writeFile(repo / "c.txt", "base\n");
	commitAll(repo, "base");
	REQUIRE(GitCli::createBranch(repo, "other", "", /*checkout=*/true));
	writeFile(repo / "c.txt", "other\n");
	commitAll(repo, "other");
	REQUIRE(GitCli::switchBranch(repo, "main"));
	writeFile(repo / "c.txt", "main\n");
	commitAll(repo, "main");
	CHECK_FALSE(GitCli::run(repo, { "merge", "other" }).ok);    // conflicts
	REQUIRE(statusOf(repo).hasConflicts());

	std::string err;
	CHECK_FALSE(GitCli::discard(repo, { "c.txt" }, &err));
	CHECK(err.find("conflict") != std::string::npos);

	REQUIRE(GitCli::resolveConflict(repo, "c.txt", /*ours=*/true, &err));
	CHECK(readFile(repo / "c.txt") == "main\n");
	CHECK_FALSE(statusOf(repo).hasConflicts());
	he_test::removeAllQuiet(repo);
}

TEST_CASE("Commit-staged commits only the staged files, and can amend")
{
	if (!gitAvailable()) { MESSAGE("git not installed - skipped"); return; }
	const fs::path repo = makeRepo("commit_staged");
	writeFile(repo / "a.txt", "1");
	writeFile(repo / "b.txt", "1");
	commitAll(repo, "first");
	writeFile(repo / "a.txt", "2");
	writeFile(repo / "b.txt", "2");

	Svc s(repo);
	s.svc.requestStage({ "a.txt" });
	s.settle();
	CHECK(s.svc.lastError().empty());
	s.svc.requestCommitStaged("only a", /*push=*/false, /*amend=*/false);
	s.settle();
	CHECK(s.svc.lastError().empty());
	REQUIRE_FALSE(s.svc.recentCommits().empty());
	CHECK(s.svc.recentCommits().front().subject == "only a");
	CHECK(s.svc.status().find("b.txt") != nullptr);             // b is still a pending change
	CHECK(s.svc.status().find("a.txt") == nullptr);

	// Nothing staged: refused with a sentence, no empty commit.
	s.svc.requestCommitStaged("empty", false, false);
	s.settle();
	CHECK_FALSE(s.svc.lastError().empty());
	CHECK(s.svc.recentCommits().front().subject == "only a");

	// Amend folds b in and renames the commit.
	s.svc.requestStage({ "b.txt" });
	s.settle();
	s.svc.requestCommitStaged("a and b", false, /*amend=*/true);
	s.settle();
	CHECK(s.svc.lastError().empty());
	CHECK(s.svc.recentCommits().front().subject == "a and b");
	CHECK(s.svc.recentCommits().size() == 2);                   // rewritten, not added
	CHECK(s.svc.status().dirtyCount() == 0);
	he_test::removeAllQuiet(repo);
}

TEST_CASE("Switching branches can stash local work first, and the stash comes back")
{
	if (!gitAvailable()) { MESSAGE("git not installed - skipped"); return; }
	const fs::path repo = makeRepo("switch");
	writeFile(repo / "a.txt", "main-v1");
	commitAll(repo, "first");
	REQUIRE(GitCli::createBranch(repo, "feature", "", /*checkout=*/true));
	writeFile(repo / "a.txt", "feature-v1");
	commitAll(repo, "feature change");
	REQUIRE(GitCli::switchBranch(repo, "main"));

	writeFile(repo / "a.txt", "main-WIP");
	writeFile(repo / "wip.txt", "untracked wip");

	Svc s(repo);
	// Carrying the edit over fails (a.txt differs on the branch) - and says so.
	s.svc.requestSwitchBranch("feature", /*stashFirst=*/false);
	s.settle();
	CHECK_FALSE(s.svc.lastError().empty());
	CHECK(s.svc.status().branch == "main");
	CHECK(readFile(repo / "a.txt") == "main-WIP");

	s.svc.requestSwitchBranch("feature", /*stashFirst=*/true);
	s.settle();
	CHECK(s.svc.lastError().empty());
	CHECK(s.svc.status().branch == "feature");
	CHECK(readFile(repo / "a.txt") == "feature-v1");
	CHECK_FALSE(fs::exists(repo / "wip.txt"));
	REQUIRE(s.svc.stashes().size() == 1);

	s.svc.requestSwitchBranch("main", false);
	s.settle();
	s.svc.requestStashPop();
	s.settle();
	CHECK(s.svc.lastError().empty());
	CHECK(readFile(repo / "a.txt") == "main-WIP");
	CHECK(readFile(repo / "wip.txt") == "untracked wip");
	CHECK(s.svc.stashes().empty());
	he_test::removeAllQuiet(repo);
}

TEST_CASE("A remote-only branch is checked out as a local one that tracks it")
{
	if (!gitAvailable()) { MESSAGE("git not installed - skipped"); return; }
	const fs::path bare = uniqueDir("remote_bare");
	REQUIRE(GitCli::run(bare, { "init", "--bare", "--initial-branch=main" }).ok);

	const fs::path a = makeRepo("remote_a");
	writeFile(a / "f.txt", "1");
	commitAll(a, "first");
	REQUIRE(GitCli::setRemote(a, fileUrl(bare)));
	REQUIRE(GitCli::run(a, { "push", "-u", "origin", "main" }).ok);
	REQUIRE(GitCli::createBranch(a, "release", "", /*checkout=*/true));
	writeFile(a / "f.txt", "release");
	commitAll(a, "release work");
	REQUIRE(GitCli::run(a, { "push", "-u", "origin", "release" }).ok);

	const fs::path b = uniqueDir("remote_b");
	fs::remove_all(b);
	REQUIRE(GitCli::clone(fileUrl(bare), b, {}));
	REQUIRE(GitCli::run(b, { "config", "user.name", "T" }).ok);

	std::vector<std::string> remotes;
	REQUIRE(GitCli::listRemoteBranches(b, remotes));
	CHECK(std::find(remotes.begin(), remotes.end(), "origin/release") != remotes.end());
	CHECK(std::find(remotes.begin(), remotes.end(), "origin/HEAD") == remotes.end());

	std::string err;
	REQUIRE(GitCli::switchBranch(b, "origin/release", &err));
	std::vector<std::string> locals;
	std::string current;
	REQUIRE(GitCli::listBranches(b, locals, current));
	CHECK(current == "release");
	CHECK(readFile(b / "f.txt") == "release");
	CHECK(statusOf(b).upstream == "origin/release");

	he_test::removeAllQuiet(a);
	he_test::removeAllQuiet(b);
	he_test::removeAllQuiet(bare);
}

TEST_CASE("A commit lists the files it touched, renames included")
{
	if (!gitAvailable()) { MESSAGE("git not installed - skipped"); return; }
	const fs::path repo = makeRepo("commit_files");
	writeFile(repo / "a.txt", "line one\nline two\nline three\nline four\n");
	writeFile(repo / "sub dir/b.txt", "b");
	commitAll(repo, "first");

	std::vector<GitCli::ChangedFile> files;
	std::string err;
	REQUIRE(GitCli::commitFiles(repo, "HEAD", files, &err));       // --root: the first commit lists its files
	REQUIRE(files.size() == 2);
	CHECK(files[0].state == 'A');

	writeFile(repo / "sub dir/b.txt", "b2");
	REQUIRE(GitCli::run(repo, { "mv", "a.txt", "renamed.txt" }).ok);
	fs::remove(repo / "sub dir/b.txt");
	commitAll(repo, "second");
	REQUIRE(GitCli::commitFiles(repo, "HEAD", files, &err));
	bool sawRename = false, sawDelete = false;
	for (const auto& f : files)
	{
		if (f.state == 'R' && f.path == "renamed.txt" && f.origPath == "a.txt") sawRename = true;
		if (f.state == 'D' && f.path == "sub dir/b.txt") sawDelete = true;
	}
	CHECK(sawRename);
	CHECK(sawDelete);
	CHECK_FALSE(GitCli::commitFiles(repo, "nosuchcommit", files, &err));
	he_test::removeAllQuiet(repo);
}

TEST_CASE("Commit-all still takes everything, staged or not (the size pass is shared now)")
{
	if (!gitAvailable()) { MESSAGE("git not installed - skipped"); return; }
	const fs::path repo = makeRepo("commit_all");
	writeFile(repo / "a.txt", "1");
	commitAll(repo, "first");
	writeFile(repo / "a.txt", "2");
	writeFile(repo / "b.txt", "new");

	Svc s(repo);
	s.svc.requestCommitAll("everything");
	s.settle();
	CHECK(s.svc.lastError().empty());
	CHECK(s.svc.recentCommits().front().subject == "everything");
	CHECK(s.svc.status().dirtyCount() == 0);
	he_test::removeAllQuiet(repo);
}
