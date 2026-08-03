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
};

} // namespace HE::Sc
