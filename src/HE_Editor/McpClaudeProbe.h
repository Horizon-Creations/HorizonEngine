#pragma once

// ─── Does Claude actually reach this editor? ─────────────────────────────────
// The row on Preferences ▸ Editor ▸ Tool Status that must not be able to lie
// (plan §6.7). "Add to Claude" tells you a config line was written; that is not
// the same claim as "a Claude session can drive this editor", and the distance
// between the two is where every real failure lives: an entry pointing at an
// editor that has since moved, a Remote Control switch that is off, a second
// editor holding the same endpoint file.
//
// So the check runs the thing rather than describing it. `claude mcp get
// horizon-editor` STARTS the shim and performs the MCP handshake — a real round
// trip, not an existence test — and the shim connects and authenticates inside
// its `initialize` precisely so that a "Connected" here cannot be produced by a
// closed editor.
//
// Three facts shape the parsing, all measured on a real CLI (§6.7, §6.9):
//
//   1. **The exit code is useless for the verdict.** `claude mcp get` exits ZERO
//      whether the server connected or failed to; the answer is in the `Status:`
//      line. A non-zero exit is a different statement: no such entry.
//   2. **The `Status:` line is decorated with `✔`/`✘`.** Matched on the words
//      `Connected` / `Failed to connect`, never on the glyph — that is the part
//      most likely to differ between CLI versions and platforms.
//   3. **`Connected` alone does not say THIS editor.** Only the bridge knows
//      that, by its handshake counter going up while the check ran. That half is
//      filled in by the caller on the frame thread (see McpBridge::authCount)
//      and is what turns a plausible row into a proven one.
//
// ImGui-free, like McpClientSetup next door and for the same reason: every
// judgement in here is reachable from a test with a synthetic HE::Proc::Result,
// without Claude Code installed on the machine running ctest and without writing
// into anybody's real config.

#include "McpClientSetup.h"

#include <Platform/Process.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace HE::Ed::McpClaudeProbe
{

// Is the editor in Claude's user config, and does the entry still describe THIS
// installation?
enum class Registration
{
	Unknown,     // not asked (no CLI, or the CLI never answered)
	Missing,     // no entry by that name
	Stale,       // an entry, but it names another script or another endpoint file
	Ok,
};

// What the handshake did when `claude` ran it.
enum class Link
{
	Unknown,     // not attempted, or the output could not be read
	Failed,      // the CLI tried and could not talk to the shim
	Connected,
};

// One flat aggregate, filled once per check and read from the frame thread —
// the shape GitProbe and RouterProbe already have, for the same reason: the page
// draws whatever is here and never asks a question that could block it.
struct Probe
{
	// The three paths, and the sentence for whichever is missing. When
	// `tools.ready()` is false nothing below was attempted.
	McpClientSetup::Tools tools;

	Registration registration = Registration::Unknown;
	std::string  registrationDetail;

	Link         link = Link::Unknown;
	std::string  linkDetail;      // the CLI's own `Issue:` line where there is one

	// The cross-check, filled by the caller after the run: did the bridge's
	// handshake counter move while this was going on, and what did the client
	// call itself. Without this, "Connected" is a statement about some editor.
	bool         authSeen = false;
	std::string  authClient;

	// Everything that was run and everything it said. The failure this exists
	// for is a CLI that words its answer differently — and then the only useful
	// thing on the page is the raw text.
	std::string  console;
};

// ── The pieces, each answerable without running anything ────────────────────

// `claude mcp get horizon-editor`. No `-s`: unlike `add` and `remove`, `get`
// takes no scope flag — it searches all of them and prints the scope it found
// the entry in, which is why `Scope:` is read back out below rather than assumed.
std::vector<std::string> getArgs();

// The lines `claude mcp get` prints, pulled out by prefix. Absent fields stay
// empty, which is what an older or newer CLI layout degrades to — a row that
// says "could not read the answer" rather than one that guesses.
struct GetFields
{
	std::string scope;     // the text after "Scope:" — which config the entry is in
	std::string status;    // the text after "Status:", glyph included
	std::string issue;     // the text after "Issue:"
	std::string command;   // the interpreter the entry starts
	std::string args;      // what it passes
	std::string endpoint;  // the value of HE_MCP_ENDPOINT, wherever it appears
};
GetFields parseGetOutput(const std::string& text);

// Reads one run of `claude mcp get` into the two verdicts. `expectedScript` and
// `expectedEndpoint` are what this installation would register today: an entry
// that names something else is Stale, which is the one state that makes the
// "Add to Claude" button the remedy rather than the decoration.
//
// Pure on purpose — `get` is a synthetic HE::Proc::Result in the tests.
void classifyGet(const HE::Proc::Result&      get,
                 const std::filesystem::path& expectedScript,
                 const std::filesystem::path& expectedEndpoint,
                 Probe&                       out);

// ── The piece that touches the machine ──────────────────────────────────────

// Find the tools, then, if they are all there, run `claude mcp get` once.
// Blocking for as long as the CLI's own health check takes (seconds), so it
// belongs on a worker thread; the run carries a timeout, so it cannot hang the
// thread it is on. `authSeen`/`authClient` are NOT filled here — the bridge may
// only be read from the frame thread.
Probe probeClaude(const std::filesystem::path& base,
                  const std::filesystem::path& endpoint);

} // namespace HE::Ed::McpClaudeProbe
