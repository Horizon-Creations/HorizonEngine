#include "doctest.h"

#include "McpClaudeProbe.h"

#include <filesystem>
#include <string>

// ─── The row that must not be able to lie ────────────────────────────────────
// Preferences ▸ Editor ▸ Tool Status says whether a Claude session can drive
// this editor. Everything about that verdict comes out of text — `claude mcp
// get` exits ZERO whether the server connected or failed to connect, so the exit
// code decides nothing and the parser decides everything.
//
// Which makes this the cheapest place for the check to go quietly wrong: a
// wording that shifts, a tick that changes shape, an entry from a previous
// installation read as current. So the transcripts below are the real ones,
// copied from a real CLI (plan §6.7, §6.9), and each test names the wrong answer
// it exists to prevent:
//
//   • "Failed to connect" read as connected, because the exit code was 0.
//   • "no such entry" read as a failed connection, sending the user to fix a
//     switch when what is missing is the registration.
//   • an entry from a moved installation read as current — the exact case the
//     "Add to Claude" button exists for, and the only place that would say so.
//   • a CLI that never started, or hung, reported as an answer about the config.
//
// Nothing here runs `claude`: every input is a synthetic HE::Proc::Result, so
// the test says the same thing on a build machine with no Claude Code installed
// and never touches anybody's real config.

namespace fs    = std::filesystem;
namespace Probe = HE::Ed::McpClaudeProbe;

namespace
{

// A run that finished normally, with whatever the CLI printed on stdout.
HE::Proc::Result finished(int exitCode, std::string out)
{
	HE::Proc::Result r;
	r.exitCode = exitCode;
	r.out      = std::move(out);
	return r;
}

// The measured shape of a registered, healthy entry. The Unicode tick is in
// here on purpose: it is the character the parser must NOT depend on.
std::string connectedTranscript(const std::string& script, const std::string& endpoint)
{
	return
		"horizon-editor:\n"
		"  Scope: User config (available in all your projects)\n"
		"  Status: \xE2\x9C\x94 Connected\n"
		"  Type: stdio\n"
		"  Command: /usr/bin/python3\n"
		"  Args: " + script + "\n"
		"  Environment:\n"
		"    HE_MCP_ENDPOINT=" + endpoint + "\n"
		"\n"
		"To remove this server, run: claude mcp remove horizon-editor -s user\n";
}

const fs::path kScript   = "/Applications/HorizonEditor.app/Contents/Resources/he_mcp.py";
const fs::path kEndpoint = "/Users/someone/Library/Application Support/Horizon/mcp-endpoint.json";

} // namespace

TEST_CASE("claude mcp get: the command asks about the entry the button writes")
{
	const std::vector<std::string> args = Probe::getArgs();
	REQUIRE(args.size() >= 3);
	CHECK(args[0] == "mcp");
	CHECK(args[1] == "get");
	// The same name "Add to Claude" registers under. Two different names here
	// would be a page reporting on an entry nobody has.
	CHECK(args[2] == std::string(HE::Ed::McpClientSetup::kServerName));

	// And NO scope flag: `claude mcp get` does not take one (unlike add/remove),
	// and passing one would make every check fail on argument parsing.
	for (const std::string& a : args) CHECK(a != "-s");
}

TEST_CASE("claude mcp get: the fields are read out of the real layout")
{
	const Probe::GetFields f =
		Probe::parseGetOutput(connectedTranscript(kScript.string(), kEndpoint.string()));

	CHECK(f.scope   == "User config (available in all your projects)");
	CHECK(f.status  == "\xE2\x9C\x94 Connected");
	CHECK(f.command == "/usr/bin/python3");
	CHECK(f.args    == kScript.string());
	// Found wherever it sits: the environment block's layout is the least
	// documented part of the output, so the variable is looked for by name.
	CHECK(f.endpoint == kEndpoint.string());
	CHECK(f.issue.empty());
}

TEST_CASE("claude mcp get: a failure is a failure even though the CLI exits zero")
{
	// The measured failing shape. If the verdict ever came off the exit code,
	// THIS is the case that would be reported as a working connection.
	const std::string failing =
		"horizon-editor:\n"
		"  Scope: User config (available in all your projects)\n"
		"  Status: \xE2\x9C\x98 Failed to connect\n"
		"  Issue: CONNECTION_CLOSED: Connection closed\n"
		"  Type: stdio\n"
		"  Command: /usr/bin/python3\n"
		"  Args: " + kScript.string() + "\n"
		"  Environment:\n"
		"    HE_MCP_ENDPOINT=" + kEndpoint.string() + "\n";

	Probe::Probe p;
	Probe::classifyGet(finished(0, failing), kScript, kEndpoint, p);

	CHECK(p.registration == Probe::Registration::Ok);   // the entry is fine
	CHECK(p.link == Probe::Link::Failed);               // what it starts is not
	// The CLI's own diagnosis is kept verbatim: it comes from the side that
	// actually tried, and paraphrasing it loses the only detail that helps.
	CHECK(p.linkDetail == "CONNECTION_CLOSED: Connection closed");
}

TEST_CASE("claude mcp get: a healthy entry is connected and current")
{
	Probe::Probe p;
	Probe::classifyGet(finished(0, connectedTranscript(kScript.string(), kEndpoint.string())),
	                   kScript, kEndpoint, p);

	CHECK(p.registration == Probe::Registration::Ok);
	CHECK(p.link == Probe::Link::Connected);
	// Not filled in by the parser: whether the handshake reached THIS editor is
	// the bridge's answer, taken on the frame thread, and a classifier that set
	// it here would be inventing the half that carries the proof.
	CHECK_FALSE(p.authSeen);
	CHECK(p.registrationDetail.find("User config") != std::string::npos);
}

TEST_CASE("claude mcp get: no entry is not a broken connection")
{
	// Measured: exit 1 and a one-line message. Reported as "not registered", so
	// the remedy the row points at is the button — not the Remote Control switch,
	// which is what a "connection failed" verdict would send the user to instead.
	Probe::Probe p;
	Probe::classifyGet(
		finished(1, "No MCP server named \"horizon-editor\". Configured servers: hive\n"),
		kScript, kEndpoint, p);

	CHECK(p.registration == Probe::Registration::Missing);
	CHECK(p.link == Probe::Link::Unknown);
	CHECK(p.registrationDetail.find("Add to Claude") != std::string::npos);
}

TEST_CASE("claude mcp get: an entry from a moved installation is not current")
{
	// The case the button exists for (§6.6): the .app was moved, so the entry
	// still names a script that is no longer there. `claude mcp get` would happily
	// call this registered, and Claude would start nothing.
	const std::string stale =
		connectedTranscript("/Users/someone/Desktop/old/he_mcp.py", kEndpoint.string());

	Probe::Probe p;
	Probe::classifyGet(finished(0, stale), kScript, kEndpoint, p);

	CHECK(p.registration == Probe::Registration::Stale);
	// It has to name BOTH paths: "stale" without them is a verdict the user
	// cannot check, and this row is the only place the mismatch is visible.
	CHECK(p.registrationDetail.find("/Users/someone/Desktop/old/he_mcp.py") != std::string::npos);
	CHECK(p.registrationDetail.find(kScript.string()) != std::string::npos);
}

TEST_CASE("claude mcp get: an entry pointing at another editor's endpoint is not current")
{
	// Two editors, one machine. The script is right, the endpoint file is
	// somebody else's — so a "Connected" here would be about the other window.
	const std::string other =
		connectedTranscript(kScript.string(), "/Users/someone/other/mcp-endpoint.json");

	Probe::Probe p;
	Probe::classifyGet(finished(0, other), kScript, kEndpoint, p);

	CHECK(p.registration == Probe::Registration::Stale);
	CHECK(p.registrationDetail.find("/Users/someone/other/mcp-endpoint.json")
	      != std::string::npos);
}

TEST_CASE("claude mcp get: a CLI that never ran says nothing about the config")
{
	SUBCASE("could not be started")
	{
		HE::Proc::Result r;
		r.launchFailed = true;
		Probe::Probe p;
		Probe::classifyGet(r, kScript, kEndpoint, p);
		// Unknown, NOT Missing: nothing was asked, so "not registered" would be a
		// claim about a config file this run never opened.
		CHECK(p.registration == Probe::Registration::Unknown);
		CHECK(p.link == Probe::Link::Unknown);
	}

	SUBCASE("hung and was stopped")
	{
		HE::Proc::Result r;
		r.timedOut = true;
		Probe::Probe p;
		Probe::classifyGet(r, kScript, kEndpoint, p);
		CHECK(p.registration == Probe::Registration::Unknown);
		CHECK(p.link == Probe::Link::Unknown);
		CHECK(p.linkDetail.find("did not answer") != std::string::npos);
	}
}

TEST_CASE("claude mcp get: an unreadable layout is admitted, not guessed at")
{
	// A future CLI that drops the Status line. The wrong answer here would be a
	// green row; the right one is a row that says it could not read the answer
	// and leaves the raw text on the page.
	Probe::Probe p;
	Probe::classifyGet(finished(0, "horizon-editor:\n  Type: stdio\n"), kScript, kEndpoint, p);

	CHECK(p.registration == Probe::Registration::Unknown);
	CHECK(p.link == Probe::Link::Unknown);
}

TEST_CASE("claude mcp get: a status worded some other way is not read as connected")
{
	// "Needs authentication" is a real third state `claude mcp list` prints. It is
	// neither of the two words the parser knows, and the row must say so rather
	// than fall through to either verdict.
	const std::string odd =
		"horizon-editor:\n"
		"  Scope: User config\n"
		"  Status: ! Needs authentication\n"
		"  Command: /usr/bin/python3\n"
		"  Args: " + kScript.string() + "\n"
		"  Environment:\n"
		"    HE_MCP_ENDPOINT=" + kEndpoint.string() + "\n";

	Probe::Probe p;
	Probe::classifyGet(finished(0, odd), kScript, kEndpoint, p);

	CHECK(p.registration == Probe::Registration::Ok);
	CHECK(p.link == Probe::Link::Unknown);
	CHECK(p.linkDetail.find("Needs authentication") != std::string::npos);
}

TEST_CASE("claude mcp get: the verdict does not depend on the tick")
{
	// §6.9's warning, made into a test: the Status line is decorated, and the
	// decoration is the part most likely to differ between versions and
	// platforms. An ASCII-only CLI must reach the same two verdicts.
	Probe::Probe ok;
	Probe::classifyGet(finished(0,
		"horizon-editor:\n  Scope: User config\n  Status: Connected\n  Args: " +
		kScript.string() + "\n  Environment:\n    HE_MCP_ENDPOINT=" + kEndpoint.string() + "\n"),
		kScript, kEndpoint, ok);
	CHECK(ok.link == Probe::Link::Connected);

	Probe::Probe bad;
	Probe::classifyGet(finished(0,
		"horizon-editor:\n  Scope: User config\n  Status: Failed to connect\n  Args: " +
		kScript.string() + "\n  Environment:\n    HE_MCP_ENDPOINT=" + kEndpoint.string() + "\n"),
		kScript, kEndpoint, bad);
	CHECK(bad.link == Probe::Link::Failed);
}

TEST_CASE("claude mcp get: with no CLI on the machine, nothing is claimed")
{
	// probeClaude on a base directory that holds nothing. Whatever this machine
	// has installed, an empty base means no shim script, so the tools are never
	// ready and the two lower rows must stay Unknown rather than inventing a
	// verdict — the check runs no process at all in this state.
	const Probe::Probe p = Probe::probeClaude(fs::path("/nonexistent-he-base"), fs::path());

	CHECK_FALSE(p.tools.ready());
	CHECK(p.registration == Probe::Registration::Unknown);
	CHECK(p.link == Probe::Link::Unknown);
	CHECK(p.registrationDetail.find("not checked") != std::string::npos);
	// Nothing was run, so there is no transcript to show.
	CHECK(p.console.empty());
}
