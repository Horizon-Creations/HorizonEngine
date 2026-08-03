#include "doctest.h"

#include <SourceControl/RepoStatus.h>

#include <string>
#include <vector>

// ─── porcelain=v2 -z parsing ─────────────────────────────────────────────────
// The reason for v2 with -z rather than the more familiar v1: v1 C-escapes and
// quotes any path containing a space, a quote or a non-ASCII byte, so parsing it
// means reimplementing git's unquoting — and getting that subtly wrong corrupts
// not just the offending record but every record after it. v2 with -z emits raw
// bytes with NUL terminators and no escaping at all.
//
// These build the exact byte stream git produces, so the parser is tested
// against the format rather than against a convenient approximation.

using namespace HE::Sc;

namespace {

// Records are NUL-terminated, not newline-separated.
struct Stream
{
	std::string data;
	Stream& operator<<(const std::string& record)
	{
		data += record;
		data.push_back('\0');
		return *this;
	}
};

// "1 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <path>" — the placeholder fields are
// never read by the parser but must be present, or the field count shifts and
// the path is taken from the wrong position.
std::string ordinary(const char* xy, const std::string& path)
{
	return std::string("1 ") + xy + " N... 100644 100644 100644 abc123 def456 " + path;
}

std::string unmerged(const char* xy, const std::string& path)
{
	return std::string("u ") + xy +
	       " N... 100644 100644 100644 100644 aaa bbb ccc " + path;
}

} // namespace

TEST_CASE("Branch, upstream and ahead/behind come from the header records")
{
	Stream s;
	s << "# branch.oid 1a2b3c4d"
	  << "# branch.head main"
	  << "# branch.upstream origin/main"
	  << "# branch.ab +3 -2";

	RepoStatus st;
	REQUIRE(parsePorcelainV2(s.data, st));
	CHECK(st.headOid  == "1a2b3c4d");
	CHECK(st.branch   == "main");
	CHECK(st.upstream == "origin/main");
	CHECK(st.ahead    == 3);
	// Reported by git as "-2"; a caller wants a positive count of commits behind,
	// not a negative number to remember the sign of.
	CHECK(st.behind   == 2);
	CHECK_FALSE(st.detached);
	CHECK_FALSE(st.initialCommit);
}

TEST_CASE("A repository with no commits yet is recognised, not mistaken for an oid")
{
	Stream s;
	s << "# branch.oid (initial)" << "# branch.head main";

	RepoStatus st;
	REQUIRE(parsePorcelainV2(s.data, st));
	CHECK(st.initialCommit);
	// Treating "(initial)" as a commit id would later produce diffs against a
	// commit that does not exist.
	CHECK(st.headOid.empty());
}

TEST_CASE("A detached HEAD is reported as detached rather than as a branch named (detached)")
{
	Stream s;
	s << "# branch.oid 9f8e7d" << "# branch.head (detached)";

	RepoStatus st;
	REQUIRE(parsePorcelainV2(s.data, st));
	CHECK(st.detached);
	CHECK(st.branch.empty());
}

TEST_CASE("Staged and unstaged changes to one file are both reported")
{
	// The case a single status value cannot express: edited, staged, then edited
	// again. Collapsing the two would make the UI claim a commit includes changes
	// it will not.
	Stream s;
	s << ordinary("MM", "Content/Materials/Rock.hmat");

	RepoStatus st;
	REQUIRE(parsePorcelainV2(s.data, st));
	const FileEntry* e = st.find("Content/Materials/Rock.hmat");
	REQUIRE(e != nullptr);
	CHECK(e->index    == FileState::Modified);
	CHECK(e->worktree == FileState::Modified);
	CHECK(e->staged());
	CHECK(e->dirty());
}

TEST_CASE("Each index/worktree letter maps to its own state")
{
	Stream s;
	s << ordinary("A.", "added.txt")
	  << ordinary(".M", "worktree_only.txt")
	  << ordinary("D.", "staged_delete.txt")
	  << ordinary(".D", "worktree_delete.txt")
	  << ordinary("T.", "typechange.txt")
	  << "? untracked.txt";

	RepoStatus st;
	REQUIRE(parsePorcelainV2(s.data, st));

	CHECK(st.find("added.txt")->index            == FileState::Added);
	CHECK(st.find("added.txt")->worktree         == FileState::Unmodified);
	CHECK(st.find("worktree_only.txt")->index    == FileState::Unmodified);
	CHECK(st.find("worktree_only.txt")->worktree == FileState::Modified);
	CHECK(st.find("staged_delete.txt")->index    == FileState::Deleted);
	CHECK(st.find("worktree_delete.txt")->worktree == FileState::Deleted);
	CHECK(st.find("typechange.txt")->index       == FileState::TypeChanged);
	CHECK(st.find("untracked.txt")->worktree     == FileState::Untracked);

	// Only what a commit would care about counts as staged.
	CHECK(st.find("added.txt")->staged());
	CHECK_FALSE(st.find("worktree_only.txt")->staged());
	CHECK_FALSE(st.find("untracked.txt")->staged());
}

TEST_CASE("A rename carries both paths and does not swallow the next record")
{
	// The one record type that spans TWO NUL-terminated fields. A parser that
	// reads one field per record consumes the following entry as the rename's
	// original path — silently losing a file and mislabelling another.
	Stream s;
	s << "2 R. N... 100644 100644 100644 aaa bbb R100 Content/New.hmat"
	  << "Content/Old.hmat"
	  << ordinary(".M", "Content/Untouched.hmat");

	RepoStatus st;
	REQUIRE(parsePorcelainV2(s.data, st));

	const FileEntry* renamed = st.find("Content/New.hmat");
	REQUIRE(renamed != nullptr);
	CHECK(renamed->index    == FileState::Renamed);
	CHECK(renamed->origPath == "Content/Old.hmat");

	// The record after the rename must still be there and still be itself.
	const FileEntry* next = st.find("Content/Untouched.hmat");
	REQUIRE(next != nullptr);
	CHECK(next->worktree == FileState::Modified);
	CHECK(next->origPath.empty());

	// And the original path is not a separate entry of its own.
	CHECK(st.find("Content/Old.hmat") == nullptr);
	CHECK(st.files.size() == 2);
}

TEST_CASE("Paths with spaces, quotes and UTF-8 survive intact")
{
	// Exactly the paths that porcelain v1 would have escaped and quoted. The
	// quote is the important one: a v1 parser that unquotes naively desynchronises
	// on it and mangles everything after.
	const std::string spaced = "Content/My Meshes/Big Rock.fbx";
	const std::string quoted = "Content/weird\"name.png";
	const std::string utf8   = "Content/Grüße/日本語/🎮.png";

	Stream s;
	s << ordinary(".M", spaced)
	  << ordinary("A.", quoted)
	  << ordinary(".M", utf8)
	  << ordinary(".M", "Content/After.txt");

	RepoStatus st;
	REQUIRE(parsePorcelainV2(s.data, st));
	CHECK(st.find(spaced) != nullptr);
	CHECK(st.find(quoted) != nullptr);
	CHECK(st.find(utf8)   != nullptr);
	// The record following the awkward ones is untouched — proof the stream did
	// not desynchronise.
	CHECK(st.find("Content/After.txt") != nullptr);
	CHECK(st.files.size() == 4);
}

TEST_CASE("Unmerged files are reported as conflicted regardless of the exact pair")
{
	// git distinguishes DD, AU, UD, UA, DU, AA and UU. They differ in how the
	// conflict arose; to a caller they all mean the same thing — this cannot be
	// committed until a human resolves it.
	for (const char* xy : { "UU", "AA", "DD", "AU", "UA", "DU", "UD" })
	{
		CAPTURE(xy);
		Stream s;
		s << unmerged(xy, "Content/Scenes/Level.hescene");

		RepoStatus st;
		REQUIRE(parsePorcelainV2(s.data, st));
		const FileEntry* e = st.find("Content/Scenes/Level.hescene");
		REQUIRE(e != nullptr);
		CHECK(e->conflicted());
		CHECK(st.hasConflicts());
	}
}

TEST_CASE("An unknown record type is skipped instead of breaking the parse")
{
	// A newer git adding a record type should cost one missing badge, not a
	// broken status.
	Stream s;
	s << "x 1 2 3 something-new"
	  << ordinary(".M", "Content/Real.txt");

	RepoStatus st;
	REQUIRE(parsePorcelainV2(s.data, st));
	CHECK(st.find("Content/Real.txt") != nullptr);
	CHECK(st.files.size() == 1);
}

TEST_CASE("Empty output parses as a clean repository")
{
	RepoStatus st;
	REQUIRE(parsePorcelainV2("", st));
	CHECK(st.files.empty());
	CHECK(st.dirtyFolders.empty());
	CHECK_FALSE(st.hasConflicts());
	CHECK_FALSE(st.hasStagedChanges());
	CHECK(st.dirtyCount() == 0);
}

TEST_CASE("Dirty folders roll up to every ancestor")
{
	Stream s;
	s << ordinary(".M", "Content/Meshes/Props/Barrel.fbx")
	  << "? Content/Textures/New.png";

	RepoStatus st;
	REQUIRE(parsePorcelainV2(s.data, st));

	// Every ancestor of every dirty file, so a folder tile costs one hash lookup
	// rather than a walk of its subtree.
	CHECK(st.dirtyFolders.count("Content"));
	CHECK(st.dirtyFolders.count("Content/Meshes"));
	CHECK(st.dirtyFolders.count("Content/Meshes/Props"));
	CHECK(st.dirtyFolders.count("Content/Textures"));
	// Not a folder — the file itself must not appear.
	CHECK_FALSE(st.dirtyFolders.count("Content/Meshes/Props/Barrel.fbx"));
	// A folder nobody touched stays clean.
	CHECK_FALSE(st.dirtyFolders.count("Content/Audio"));
}

TEST_CASE("Ignored files do not count as changes")
{
	Stream s;
	s << "! Saved/Thumbnails/cache.bin"
	  << ordinary(".M", "Content/Real.txt");

	RepoStatus st;
	REQUIRE(parsePorcelainV2(s.data, st));

	const FileEntry* ignored = st.find("Saved/Thumbnails/cache.bin");
	REQUIRE(ignored != nullptr);
	CHECK(ignored->worktree == FileState::Ignored);
	CHECK_FALSE(ignored->dirty());
	// One dirty file, and the ignored one contributes no folder badge.
	CHECK(st.dirtyCount() == 1);
	CHECK_FALSE(st.dirtyFolders.count("Saved"));
}

TEST_CASE("Lookup tolerates a case difference between git and the filesystem")
{
	// git records paths case-sensitively; macOS and Windows filesystems are not.
	// A path that came from the filesystem can therefore differ in case from the
	// one git stored, and an exact-only lookup would silently find nothing —
	// which shows up as a badge that is simply missing.
	Stream s;
	s << ordinary(".M", "Content/Meshes/Hero.fbx");

	RepoStatus st;
	REQUIRE(parsePorcelainV2(s.data, st));

	CHECK(st.find("Content/Meshes/Hero.fbx") != nullptr);   // exact
	CHECK(st.find("content/meshes/hero.fbx") != nullptr);   // differing case
	CHECK(st.find("Content/Meshes/Missing.fbx") == nullptr);
}
