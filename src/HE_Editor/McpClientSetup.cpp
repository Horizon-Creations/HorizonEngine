#include "McpClientSetup.h"

#include <cstdlib>
#include <system_error>

namespace HE::Ed::McpClientSetup
{

namespace
{

// Readable in a log and in the console block the button prints: the program and
// its arguments, one line, with nothing quoted. It is a transcript, not
// something to paste into a shell — the real call never goes through one.
std::string commandLine(const std::filesystem::path& exe, const std::vector<std::string>& args)
{
	std::string line = exe.string();
	for (const std::string& a : args) { line += ' '; line += a; }
	return line;
}

// The environment, read the way a candidate list needs it: an unset variable and
// an empty one are the same answer, and both mean "no candidates from here".
std::filesystem::path envPath(const char* name)
{
	const char* v = std::getenv(name);
	return (v && *v) ? std::filesystem::path(v) : std::filesystem::path();
}

// Exists and is a file we could start. The executable bit is checked because a
// half-finished install leaves the name there without it, and a candidate that
// cannot run is not a find — it would turn a clear "no CLI" into a launch
// failure with a worse message.
bool isRunnable(const std::filesystem::path& p)
{
	std::error_code ec;
	if (!std::filesystem::is_regular_file(p, ec)) return false;
#ifdef _WIN32
	return true;   // no executable bit; the extension is what decides
#else
	const std::filesystem::perms m = std::filesystem::status(p, ec).permissions();
	if (ec) return false;
	using std::filesystem::perms;
	return (m & (perms::owner_exec | perms::group_exec | perms::others_exec)) != perms::none;
#endif
}

// The first line of whatever the CLI said, for the one-line message next to the
// button. The whole of it stays in `console`; this is only the headline.
std::string firstLine(const std::string& text)
{
	const std::size_t end = text.find_first_of("\r\n");
	std::string line = (end == std::string::npos) ? text : text.substr(0, end);
	// Trim, because a CLI that decorates its output leaves the line indented.
	const std::size_t b = line.find_first_not_of(" \t");
	if (b == std::string::npos) return {};
	const std::size_t e = line.find_last_not_of(" \t");
	return line.substr(b, e - b + 1);
}

// Neither run may hang the worker. Twenty seconds is far past what a Node
// startup plus a config write needs and far short of "the user thinks the
// editor froze".
constexpr std::uint32_t kCliTimeoutMs = 20000;

} // namespace

std::vector<std::filesystem::path> claudeCandidates(const std::filesystem::path& home,
                                                    const std::filesystem::path& appData)
{
	std::vector<std::filesystem::path> out;
#ifdef _WIN32
	// npm's global bin on Windows is %APPDATA%\npm, and the entry point there is
	// a .cmd shim — which is why the extension is spelled out rather than left to
	// PATH resolution, this being a direct path and not a lookup.
	if (!appData.empty()) out.push_back(appData / "npm" / "claude.cmd");
	if (!home.empty())    out.push_back(home / ".local" / "bin" / "claude.exe");
#else
	(void)appData;
	if (!home.empty())
	{
		out.push_back(home / ".local" / "bin" / "claude");        // native installer
		out.push_back(home / ".claude" / "local" / "claude");      // local install
		out.push_back(home / ".npm-global" / "bin" / "claude");    // npm prefix moved out of /usr
		out.push_back(home / ".bun" / "bin" / "claude");           // bun install -g
	}
#endif
	return out;
}

std::vector<std::string> pythonNames()
{
#ifdef _WIN32
	return { "python3", "python", "py" };
#else
	return { "python3", "python" };
#endif
}

std::vector<std::filesystem::path> scriptCandidates(const std::filesystem::path& base)
{
	std::vector<std::filesystem::path> out;
	if (base.empty()) return out;
	// Where the build puts it: next to the executable in a flat deploy, and in
	// Contents/Resources for a bundled .app — both of which ARE `base`.
	out.push_back(base / "he_mcp.py");
	// And where it lives in the source tree, for an editor started out of a build
	// directory that has not been deployed. Two levels up covers the usual
	// <repo>/build/bin layout; one level up covers <repo>/build.
	out.push_back(base / "scripts" / "he_mcp.py");
	out.push_back(base / ".." / "scripts" / "he_mcp.py");
	out.push_back(base / ".." / ".." / "scripts" / "he_mcp.py");
	return out;
}

std::vector<std::string> addArgs(const std::filesystem::path& python,
                                 const std::filesystem::path& script,
                                 const std::filesystem::path& endpoint)
{
	// -e, because the editor knows the endpoint path exactly and the shim would
	// otherwise have to guess it from a per-user data directory it derives on its
	// own. -- separates the CLI's own flags from the command it is to start.
	return {
		"mcp", "add", kServerName,
		"-s", "user",
		"-e", "HE_MCP_ENDPOINT=" + endpoint.string(),
		"--",
		python.string(),
		script.string(),
	};
}

std::vector<std::string> removeArgs()
{
	return { "mcp", "remove", kServerName, "-s", "user" };
}

Outcome classifyAdd(const HE::Proc::Result& add)
{
	Outcome o;
	if (add.launchFailed)
	{
		o.message = "The claude command could not be started.";
		return o;
	}
	if (add.timedOut)
	{
		o.message = "claude did not answer within 20 seconds and was stopped.";
		return o;
	}
	if (add.exitCode != 0)
	{
		// The CLI's own words first, because they name the actual refusal; the
		// exit code is only the fallback for a program that failed silently.
		const std::string said = firstLine(add.err.empty() ? add.out : add.err);
		o.message = said.empty() ? ("claude exited with code " + std::to_string(add.exitCode) + ".")
		                         : said;
		return o;
	}
	o.ok      = true;
	o.message = std::string("Registered as \"") + kServerName + "\" in your user config. "
	            "Restart Claude to pick it up.";
	return o;
}

std::filesystem::path findClaudeCli()
{
	// What a terminal would find, first — including the package-manager prefixes
	// a Finder launch does not inherit, since which() augments PATH itself.
	if (auto p = HE::Proc::which("claude")) return *p;

	for (const std::filesystem::path& c : claudeCandidates(envPath("HOME").empty()
	                                                           ? envPath("USERPROFILE")
	                                                           : envPath("HOME"),
	                                                       envPath("APPDATA")))
		if (isRunnable(c)) return c;
	return {};
}

std::filesystem::path findPython()
{
	for (const std::string& name : pythonNames())
		if (auto p = HE::Proc::which(name)) return *p;
	return {};
}

std::filesystem::path findShimScript(const std::filesystem::path& base)
{
	std::error_code ec;
	for (const std::filesystem::path& c : scriptCandidates(base))
		if (std::filesystem::is_regular_file(c, ec))
		{
			// Normalised, because a candidate carries ".." and the path is written
			// into a config file that another program reads back.
			std::filesystem::path abs = std::filesystem::weakly_canonical(c, ec);
			return ec ? c : abs;
		}
	return {};
}

Tools findTools(const std::filesystem::path& base)
{
	Tools t;
	t.claude = findClaudeCli();
	t.python = findPython();
	t.script = findShimScript(base);

	// One blocker at a time, and the one that is worth acting on first: without
	// the CLI nothing else matters, and a missing interpreter is the user's to
	// fix while a missing script is ours.
	if (t.claude.empty())
		t.blocker = "Claude Code is not installed on this machine, or its command "
		            "line tool is not where an installer puts it. Install it, then "
		            "reopen this page.";
	else if (t.python.empty())
		t.blocker = "No Python interpreter was found. The bridge itself needs none, "
		            "but the small script that connects Claude to it is Python.";
	else if (t.script.empty())
		t.blocker = "he_mcp.py was not found next to the editor. This is a broken "
		            "installation rather than something to configure.";
	return t;
}

Outcome registerWithClaude(const Tools& tools, const std::filesystem::path& endpoint)
{
	Outcome o;
	if (!tools.ready())
	{
		o.message = tools.blocker;
		return o;
	}

	// Remove first. Not tidiness: `claude mcp add` over an existing entry says
	// "already exists" and exits ZERO without changing anything, so an entry
	// pointing at a moved .app would survive every press of a button whose whole
	// purpose is to fix it.
	HE::Proc::Options rm;
	rm.exe       = tools.claude;
	rm.args      = removeArgs();
	rm.timeoutMs = kCliTimeoutMs;
	const HE::Proc::Result rmRes = HE::Proc::run(rm);

	HE::Proc::Options add;
	add.exe       = tools.claude;
	add.args      = addArgs(tools.python, tools.script, endpoint);
	add.timeoutMs = kCliTimeoutMs;
	const HE::Proc::Result addRes = HE::Proc::run(add);

	o = classifyAdd(addRes);

	// The transcript, both runs, in the order they happened. The remove is in
	// here even though its outcome is ignored, because when the add then fails
	// for a reason the CLI words badly, what the remove said is often the half
	// that explains it.
	o.console  = "$ " + commandLine(tools.claude, rm.args) + "\n";
	o.console += rmRes.out;
	o.console += rmRes.err;
	o.console += "$ " + commandLine(tools.claude, add.args) + "\n";
	o.console += addRes.out;
	o.console += addRes.err;
	if (addRes.launchFailed) o.console += "(the process could not be started)\n";
	if (addRes.timedOut)     o.console += "(stopped after 20 s without an answer)\n";
	return o;
}

} // namespace HE::Ed::McpClientSetup
