#pragma once

// ─── Running git ─────────────────────────────────────────────────────────────
// A concrete class, not an interface. There will only ever be one
// implementation, and a test double would be worse than the real thing: a
// `git init` fixture in a temp directory exercises the actual command lines,
// the actual output format and the actual exit codes, all of which are what
// break. The seam for tests is the fixture, not a mock.
//
// Every call blocks on a subprocess and belongs on a worker thread — GitService
// owns that thread; nothing here should be called from the frame loop.

#include "SourceControl/RepoStatus.h"
#include "SourceControl/ScCommon.h"

#include <filesystem>
#include <string>
#include <vector>

namespace HE::Sc {

struct GitResult
{
	bool        ok       = false;
	int         exitCode = -1;
	std::string out;
	std::string err;      // git's own message, already scrubbed of credentials
};

class HE_SC_API GitCli {
public:
	// Run git in `cwd`. The environment is fixed up so git can never block on a
	// prompt it has no way to display.
	static GitResult run(const std::filesystem::path& cwd,
	                     const std::vector<std::string>& args,
	                     std::uint32_t timeoutMs = 30000);

	// The working tree containing `anyPathInside`, or an empty path when it is
	// not in a repository. Uses git rather than looking for a .git directory,
	// because .git can be a file (worktrees, submodules) and the answer for a
	// path inside a submodule is not simply "the nearest .git".
	static std::filesystem::path findRepoRoot(const std::filesystem::path& anyPathInside);

	// Full working-tree status. Fills `out` completely, including root, branch,
	// ahead/behind and the dirty-folder rollup.
	static bool status(const std::filesystem::path& root, RepoStatus& out, std::string* err = nullptr);

	// ── Mutating operations ──────────────────────────────────────────────────
	// Each is one git invocation with the timeout that matches its nature:
	// local commands get seconds, network transfers get minutes — a large LFS
	// push legitimately runs that long, and cutting it off mid-transfer is
	// strictly worse than waiting.

	// git init -b main. `err` is filled from git's own message on failure.
	static bool init(const std::filesystem::path& dir, std::string* err = nullptr);

	// git add -A: stage everything, including deletions and untracked files.
	static bool addAll(const std::filesystem::path& root, std::string* err = nullptr);

	// git commit -m. Fails cleanly when there is nothing staged or no identity
	// is configured — both come back as git's message, not as a mystery.
	static bool commit(const std::filesystem::path& root, const std::string& message,
	                   std::string* err = nullptr);

	// git push, with -u origin HEAD on the first one (no upstream yet) so the
	// branch tracks its remote counterpart from then on.
	static bool push(const std::filesystem::path& root, bool upstreamConfigured,
	                 std::string* err = nullptr);

	// git pull --ff-only: never invents a merge commit on its own. A diverged
	// branch comes back as an error telling the user what to decide.
	static bool pull(const std::filesystem::path& root, std::string* err = nullptr);

	// The URL of `origin`, or empty when no remote is configured.
	static std::string remoteUrl(const std::filesystem::path& root);

	// Point `origin` at `url`, creating or updating it.
	static bool setRemote(const std::filesystem::path& root, const std::string& url,
	                      std::string* err = nullptr);
};

} // namespace HE::Sc
