#include "doctest.h"

#include "McpClientSetup.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ─── What "Add to Claude" will actually run ──────────────────────────────────
// The button spawns `claude`, and none of that belongs in a test: it would need
// Claude Code installed on the machine running ctest, and it would write into
// the user's real config. So the split is deliberate — everything decidable
// without a process is a free function in McpClientSetup, and this file holds it
// to the three claims the button makes:
//
//   1. The command line names the endpoint file, the interpreter and the script,
//      as absolute paths, in a user scope. A single wrong flag here is a
//      registration that lands where nobody looks.
//   2. A CLI that could not be started, that hung, or that refused is NOT a
//      success — including the case where it says something on stdout and exits
//      non-zero, which is how npm shims fail.
//   3. The candidate list covers the installs PATH does not mention, and never
//      invents one out of an unset HOME.
//
// The one thing NOT covered here is `claude mcp add` over an existing entry
// exiting zero without changing anything (plan §6.6). That is a fact about the
// CLI, not about this code; what this code does about it — remove first, then
// add — is visible in registerWithClaude and cannot be asserted without running
// the real thing.

namespace fs = std::filesystem;
namespace Setup = HE::Ed::McpClientSetup;

namespace
{

// The index of `needle` in `v`, or -1. Written out rather than using find so a
// failing case prints the position it expected against the one it got.
int indexOf(const std::vector<std::string>& v, const std::string& needle)
{
	for (std::size_t i = 0; i < v.size(); ++i)
		if (v[i] == needle) return static_cast<int>(i);
	return -1;
}

} // namespace

TEST_CASE("mcp client setup: the add command line")
{
	const std::vector<std::string> args =
		Setup::addArgs("/usr/bin/python3", "/Apps/Horizon.app/Contents/Resources/he_mcp.py",
		               "/Users/someone/Library/Application Support/HorizonEngine/mcp-endpoint.json");

	// The shape, in order: it is `claude mcp add <name>`, not `claude add`.
	REQUIRE(args.size() >= 4);
	CHECK(args[0] == "mcp");
	CHECK(args[1] == "add");
	CHECK(args[2] == std::string(Setup::kServerName));

	// User scope, not the default. Without it the entry goes into the config for
	// the current working directory — which for an editor started from Finder is
	// "/", and the user would never see the server again.
	const int scopeAt = indexOf(args, "-s");
	REQUIRE(scopeAt >= 0);
	REQUIRE(scopeAt + 1 < static_cast<int>(args.size()));
	CHECK(args[scopeAt + 1] == "user");

	// The endpoint, handed over rather than guessed. The shim reads
	// HE_MCP_ENDPOINT (scripts/he_mcp.py, resolve_endpoint_path).
	const int envAt = indexOf(args, "-e");
	REQUIRE(envAt >= 0);
	REQUIRE(envAt + 1 < static_cast<int>(args.size()));
	CHECK(args[envAt + 1] ==
	      "HE_MCP_ENDPOINT=/Users/someone/Library/Application Support/HorizonEngine/mcp-endpoint.json");

	// The command to start comes after `--`, in that order, and is the LAST
	// thing on the line: everything after the separator belongs to the child.
	const int dashAt = indexOf(args, "--");
	REQUIRE(dashAt >= 0);
	REQUIRE(args.size() == static_cast<std::size_t>(dashAt) + 3);
	CHECK(args[dashAt + 1] == "/usr/bin/python3");
	CHECK(args[dashAt + 2] == "/Apps/Horizon.app/Contents/Resources/he_mcp.py");

	// A path with a space in it stays ONE argument. This is the whole reason the
	// registration goes through an argv vector: quoting it into a shell string is
	// the classic way "Application Support" breaks a tool.
	CHECK(args[envAt + 1].find(' ') != std::string::npos);
	for (const std::string& a : args) CHECK(a.find('"') == std::string::npos);
}

TEST_CASE("mcp client setup: remove targets the same entry in the same scope")
{
	const std::vector<std::string> rm  = Setup::removeArgs();
	const std::vector<std::string> add = Setup::addArgs("py", "s.py", "e.json");

	CHECK(rm[0] == "mcp");
	CHECK(rm[1] == "remove");
	// Same name and same scope as the add, or the remove-then-add that makes the
	// button idempotent would delete something else and leave the stale entry.
	CHECK(rm[2] == add[2]);
	const int rmScope  = indexOf(rm, "-s");
	const int addScope = indexOf(add, "-s");
	REQUIRE(rmScope >= 0);
	REQUIRE(addScope >= 0);
	CHECK(rm[rmScope + 1] == add[addScope + 1]);
}

TEST_CASE("mcp client setup: reading what the CLI did")
{
	SUBCASE("a clean run is a success and says so")
	{
		HE::Proc::Result r;
		r.exitCode = 0;
		r.out      = "Added stdio MCP server horizon-editor with command: python3 he_mcp.py to user config\n";
		const Setup::Outcome o = Setup::classifyAdd(r);
		CHECK(o.ok);
		CHECK(o.message.find(Setup::kServerName) != std::string::npos);
	}

	SUBCASE("never started is not a success")
	{
		HE::Proc::Result r;
		r.launchFailed = true;
		const Setup::Outcome o = Setup::classifyAdd(r);
		CHECK_FALSE(o.ok);
		CHECK(o.message.find("could not be started") != std::string::npos);
	}

	SUBCASE("a hang is reported as a hang, not as a refusal")
	{
		HE::Proc::Result r;
		r.timedOut = true;
		// The exit code left behind by a killed process is meaningless; a zero in
		// there must not read as "done".
		r.exitCode = 0;
		const Setup::Outcome o = Setup::classifyAdd(r);
		CHECK_FALSE(o.ok);
		CHECK(o.message.find("20 seconds") != std::string::npos);
	}

	SUBCASE("a refusal is repeated in the CLI's own words")
	{
		HE::Proc::Result r;
		r.exitCode = 1;
		r.err      = "error: unknown option '-s'\nUsage: claude mcp add\n";
		const Setup::Outcome o = Setup::classifyAdd(r);
		CHECK_FALSE(o.ok);
		// The first line, and only the first: the rest is usage text.
		CHECK(o.message == "error: unknown option '-s'");
	}

	SUBCASE("a silent failure still says something useful")
	{
		HE::Proc::Result r;
		r.exitCode = 3;
		const Setup::Outcome o = Setup::classifyAdd(r);
		CHECK_FALSE(o.ok);
		CHECK(o.message.find("3") != std::string::npos);
	}

	SUBCASE("output on stdout does not make a non-zero exit a success")
	{
		HE::Proc::Result r;
		r.exitCode = 1;
		r.out      = "MCP server horizon-editor is already in this config\n";
		const Setup::Outcome o = Setup::classifyAdd(r);
		CHECK_FALSE(o.ok);
		CHECK(o.message.find("already") != std::string::npos);
	}
}

TEST_CASE("mcp client setup: where the CLI is looked for")
{
	const std::vector<fs::path> c = Setup::claudeCandidates("/Users/someone", "C:/Users/someone/AppData/Roaming");
	REQUIRE_FALSE(c.empty());

	// Every candidate is absolute and named after the home it was built from —
	// a relative candidate would be resolved against the editor's working
	// directory, which for a Finder launch is "/".
	for (const fs::path& p : c)
	{
		CHECK(p.is_absolute());
		CHECK(p.filename().string().rfind("claude", 0) == 0);
	}

#ifndef _WIN32
	// The two the plan named for POSIX (§6.5): the native installer and the
	// local install. Both are places PATH does not mention.
	bool native = false, local = false;
	for (const fs::path& p : c)
	{
		if (p == fs::path("/Users/someone/.local/bin/claude"))      native = true;
		if (p == fs::path("/Users/someone/.claude/local/claude"))   local  = true;
	}
	CHECK(native);
	CHECK(local);
#endif

	// No home, no candidates. Otherwise a missing HOME would produce paths like
	// "/.local/bin/claude" and the search would stat the root of the disk.
	CHECK(Setup::claudeCandidates("", "").empty());
}

TEST_CASE("mcp client setup: python is asked for by the name that exists everywhere")
{
	const std::vector<std::string> names = Setup::pythonNames();
	REQUIRE_FALSE(names.empty());
	// python3 first on every platform: `python` is Python 2 on old machines and
	// absent on new ones, and the Windows Store stub is a `python` that opens a
	// browser.
	CHECK(names[0] == "python3");
}

TEST_CASE("mcp client setup: finding the shim next to the editor")
{
	const fs::path base = fs::temp_directory_path() / "he_mcp_setup_test";
	fs::remove_all(base);
	fs::create_directories(base);

	// Nothing there yet: a find must be empty rather than a path that happens to
	// be spelled right, because the button greys out on empty and would otherwise
	// register a file that is not there.
	CHECK(Setup::findShimScript(base).empty());

	// The deployed layout: beside the executable, which is also Contents/Resources
	// for a bundled .app.
	{ std::ofstream f(base / "he_mcp.py"); f << "# shim\n"; }
	const fs::path found = Setup::findShimScript(base);
	CHECK_FALSE(found.empty());
	CHECK(found.filename() == "he_mcp.py");
	CHECK(found.is_absolute());

	// An empty base is not a lookup in the current directory.
	CHECK(Setup::scriptCandidates("").empty());

	fs::remove_all(base);
}

TEST_CASE("mcp client setup: a blocked setup refuses to run anything")
{
	Setup::Tools t;
	t.blocker = "Claude Code is not installed on this machine.";
	CHECK_FALSE(t.ready());

	// registerWithClaude with no CLI path must not reach HE::Proc at all — it
	// returns the blocker as its message, which is the same sentence the greyed
	// out button shows.
	const Setup::Outcome o = Setup::registerWithClaude(t, "/tmp/mcp-endpoint.json");
	CHECK_FALSE(o.ok);
	CHECK(o.message == t.blocker);
	CHECK(o.console.empty());
}
