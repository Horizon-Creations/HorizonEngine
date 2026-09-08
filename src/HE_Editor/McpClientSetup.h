#pragma once

// ─── Registering this editor with the Claude CLI ─────────────────────────────
// What the "Add to Claude" button in Preferences ▸ Editor ▸ Remote Control needs
// in order to be a button that works rather than a button that claims to: the
// three absolute paths the registration is made of (the `claude` CLI, a Python
// interpreter, the stdio shim `he_mcp.py`), the exact argument vector, and the
// reading of what came back.
//
// Free of ImGui on purpose (docs/mcp-editor-integration-plan.md §6.8). Every
// question this file answers — "would the button be greyed out on a machine
// without npm", "does the command name the endpoint file", "is a CLI that says
// nothing on stdout a success" — is answerable in a test, and none of them needs
// a window or a `claude` on the machine running the test.
//
// The two facts that shape all of it (§6.5, §6.6):
//
//   1. A GUI app launched from Finder inherits a minimal PATH. `HE::Proc::which`
//      calls augmentToolPath() itself, so Homebrew is covered; the native and
//      npm installers put the CLI in places PATH never mentions, so those are
//      checked by name afterwards.
//   2. `claude mcp add` on an existing entry prints "already exists" and exits
//      ZERO. The exit code alone therefore cannot tell "registered" from "left
//      the old, wrong path in place" — which is why registration removes first
//      and adds second, and why the button rewrites rather than refuses.

#include <Platform/Process.h>

#include <filesystem>
#include <string>
#include <vector>

namespace HE::Ed::McpClientSetup
{

// The name the editor is registered under. One name, so a second press replaces
// the first registration instead of growing a second one next to it.
inline constexpr const char* kServerName = "horizon-editor";

// The three paths the command line is built from, plus the reason it cannot be
// built. Exactly one of those states is true at a time: `blocker` empty means
// all three paths are filled in.
struct Tools
{
	std::filesystem::path claude;   // absolute path to the CLI, empty when not found
	std::filesystem::path python;   // absolute path to the interpreter that runs the shim
	std::filesystem::path script;   // absolute path to he_mcp.py
	std::string           blocker;  // one sentence for the greyed-out button, empty when ready

	bool ready() const { return blocker.empty(); }
};

// What the two CLI runs added up to. `console` is the transcript — both command
// lines with their output — because when this fails, the thing that says why is
// the CLI's own words, and paraphrasing them loses the only detail that helps.
struct Outcome
{
	bool        ok = false;
	std::string message;   // one line, shown next to the button
	std::string console;   // what was run and what it said, verbatim
};

// ── The pieces, each answerable without running anything ────────────────────

// The places an installer puts the CLI, in the order §6.5 fixed: what a terminal
// would find comes first (that is `which`, not this list), then the native
// installer, the local install, and the two npm-without-Homebrew layouts.
// `home` and `appData` are passed in rather than read from the environment so a
// test can ask about a Windows layout from a Mac.
std::vector<std::filesystem::path> claudeCandidates(const std::filesystem::path& home,
                                                    const std::filesystem::path& appData);

// The interpreter names to try, in order. POSIX has exactly one answer worth
// having; Windows has three, and `py` last because the launcher may resolve to
// a Python the user did not mean.
std::vector<std::string> pythonNames();

// Where the shim sits next to the editor. `base` is SDL_GetBasePath() — the
// directory beside the executable in a flat deploy, Contents/Resources in a
// bundled .app. The build copies the script to both.
std::vector<std::filesystem::path> scriptCandidates(const std::filesystem::path& base);

// `claude mcp add horizon-editor -s user -e HE_MCP_ENDPOINT=… -- <py> <script>`,
// as an argv vector. Never a shell string: a user directory with a space in it
// is then not a quoting problem, and cannot be one.
//
// -s user, not the default: `claude mcp add` writes to the LOCAL (per working
// directory) config unless told otherwise, and an editor started from Finder has
// "/" as its working directory. The entry would land where nobody ever looks.
std::vector<std::string> addArgs(const std::filesystem::path& python,
                                 const std::filesystem::path& script,
                                 const std::filesystem::path& endpoint);

// `claude mcp remove horizon-editor -s user`. Run before every add, and its
// failure ignored: "there was nothing to remove" is the normal case.
std::vector<std::string> removeArgs();

// Read one CLI run. Kept separate from running it so the four ways it ends —
// never started, hung, refused, done — can be checked against a synthetic
// Result instead of against whatever `claude` happens to do on this machine.
Outcome classifyAdd(const HE::Proc::Result& add);

// ── The pieces that touch the machine ───────────────────────────────────────

// `which` first (that is what the user would get in a terminal), then the
// candidates. Empty when there is none.
std::filesystem::path findClaudeCli();
std::filesystem::path findPython();
std::filesystem::path findShimScript(const std::filesystem::path& base);

// All three at once, with `blocker` filled in for whichever is missing — the
// text the greyed-out button shows, which names the remedy and not just the
// lack.
Tools findTools(const std::filesystem::path& base);

// Remove, then add. Blocking (two Node startups, roughly a second each), so it
// belongs on a worker thread; both runs carry a timeout, so it cannot hang the
// thread it is on.
Outcome registerWithClaude(const Tools& tools, const std::filesystem::path& endpoint);

} // namespace HE::Ed::McpClientSetup
