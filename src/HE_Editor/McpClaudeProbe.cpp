#include "McpClaudeProbe.h"

namespace HE::Ed::McpClaudeProbe
{

namespace
{

// Same transcript shape the "Add to Claude" button prints: the program, its
// arguments, unquoted, one line. Nothing here is meant to be pasted into a
// shell — the real call never goes through one.
std::string commandLine(const std::filesystem::path& exe, const std::vector<std::string>& args)
{
	std::string line = exe.string();
	for (const std::string& a : args) { line += ' '; line += a; }
	return line;
}

std::string trimmed(const std::string& s)
{
	const std::size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) return {};
	const std::size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

bool contains(const std::string& haystack, const char* needle)
{
	return haystack.find(needle) != std::string::npos;
}

// The `Key: value` lines the CLI indents under the server name. Matched on the
// trimmed line so the indentation is free to change, and only on the FIRST
// occurrence — `To remove this server, run: …` at the foot is a sentence with a
// colon in it, not a field.
void takeField(const std::string& line, const char* key, std::string& out)
{
	if (!out.empty()) return;
	const std::size_t keyLen = std::char_traits<char>::length(key);
	if (line.size() < keyLen + 1) return;
	if (line.compare(0, keyLen, key) != 0) return;
	if (line[keyLen] != ':') return;
	out = trimmed(line.substr(keyLen + 1));
}

// The one field that is not read by prefix. The environment block's layout is
// the least documented part of the output — it may be one line, it may be
// several, it may be indented under `Environment:` — so the variable is looked
// for in the whole text instead. Its name is unique enough that a false
// positive would have to be somebody registering a second server that carries
// the same variable, and that server would be another copy of this editor.
std::string findEnvValue(const std::string& text, const char* name)
{
	const std::string needle = std::string(name) + "=";
	const std::size_t at = text.find(needle);
	if (at == std::string::npos) return {};
	const std::size_t from = at + needle.size();
	const std::size_t end  = text.find_first_of("\r\n", from);
	return trimmed(text.substr(from, end == std::string::npos ? std::string::npos
	                                                          : end - from));
}

} // namespace

std::vector<std::string> getArgs()
{
	return { "mcp", "get", McpClientSetup::kServerName };
}

GetFields parseGetOutput(const std::string& text)
{
	GetFields f;
	std::size_t pos = 0;
	while (pos <= text.size())
	{
		const std::size_t nl   = text.find('\n', pos);
		const std::string line = trimmed(text.substr(pos, nl == std::string::npos
		                                                      ? std::string::npos
		                                                      : nl - pos));
		takeField(line, "Scope",   f.scope);
		takeField(line, "Status",  f.status);
		takeField(line, "Issue",   f.issue);
		takeField(line, "Command", f.command);
		takeField(line, "Args",    f.args);
		if (nl == std::string::npos) break;
		pos = nl + 1;
	}
	f.endpoint = findEnvValue(text, "HE_MCP_ENDPOINT");
	return f;
}

void classifyGet(const HE::Proc::Result&      get,
                 const std::filesystem::path& expectedScript,
                 const std::filesystem::path& expectedEndpoint,
                 Probe&                       out)
{
	// The two ways there is no answer at all. Kept apart from "not registered":
	// a CLI that could not be started says nothing about what is in the config,
	// and a row that reported "not registered" here would send the user to press
	// a button that is about to fail the same way.
	if (get.launchFailed)
	{
		out.registration       = Registration::Unknown;
		out.registrationDetail = "the claude command could not be started";
		out.link               = Link::Unknown;
		out.linkDetail         = out.registrationDetail;
		return;
	}
	if (get.timedOut)
	{
		out.registration       = Registration::Unknown;
		out.registrationDetail = "claude did not answer in time and was stopped";
		out.link               = Link::Unknown;
		out.linkDetail         = out.registrationDetail;
		return;
	}

	const std::string text   = get.out + get.err;
	const GetFields   fields = parseGetOutput(text);

	// No entry: measured as exit 1 with `No MCP server named "horizon-editor".`.
	// Both are checked, because the wording is a message and the exit code is a
	// contract, and either one alone would be a guess about the other.
	if (get.exitCode != 0 || contains(text, "No MCP server named"))
	{
		out.registration       = Registration::Missing;
		out.registrationDetail = "not registered with Claude — press \"Add to Claude\" on "
		                         "the Remote Control page";
		out.link               = Link::Unknown;
		out.linkDetail         = "nothing to connect to until the editor is registered";
		return;
	}

	// An entry always brings a `Status:` line with it. Without one, the answer
	// is in a layout this parser does not know — said plainly rather than
	// guessed at, with the raw text one block further down the page.
	if (fields.status.empty())
	{
		out.registration       = Registration::Unknown;
		out.registrationDetail = "claude answered in a layout this editor cannot read — see "
		                         "the output below";
		out.link               = Link::Unknown;
		out.linkDetail         = out.registrationDetail;
		return;
	}

	// ── There is an entry. Does it describe THIS installation? ───────────────
	// The button exists to be pressed again after the editor moves, and this is
	// the only place that would ever tell somebody to press it: an entry naming
	// a script that is no longer there starts nothing, and one naming another
	// endpoint file connects to another editor.
	std::string mismatch;
	const std::string registeredArgs = fields.args;
	if (!expectedScript.empty() && !registeredArgs.empty() &&
	    registeredArgs.find(expectedScript.string()) == std::string::npos)
		mismatch = "it starts " + registeredArgs + ", not " + expectedScript.string();
	else if (!expectedEndpoint.empty() && !fields.endpoint.empty() &&
	         fields.endpoint != expectedEndpoint.string())
		mismatch = "it points at " + fields.endpoint + ", not " + expectedEndpoint.string();

	if (!mismatch.empty())
	{
		out.registration       = Registration::Stale;
		out.registrationDetail = "registered, but the entry is from another installation — "
		                         + mismatch + ". Press \"Add to Claude\" again to rewrite it.";
	}
	else
	{
		out.registration = Registration::Ok;
		out.registrationDetail = fields.scope.empty()
		                             ? std::string("registered")
		                             : "registered, " + fields.scope;
		if (!fields.command.empty())
			out.registrationDetail += " — " + fields.command +
			                          (registeredArgs.empty() ? "" : " " + registeredArgs);
	}

	// ── What the handshake did ───────────────────────────────────────────────
	// The words, not the glyph: `✔`/`✘` are the part most likely to differ
	// between CLI versions, and a row that turned amber because a tick changed
	// shape would be the exact kind of false alarm that teaches people to ignore
	// this page.
	// "Failed" rather than the whole of "Failed to connect": the measured wording
	// is the latter, and any other refusal this CLI words as a failure should
	// land here too rather than in the "cannot read" branch. Checked before
	// "Connected" and not after — "Failed to connect" carries a lowercase
	// "connect", which is why the comparison is case-sensitive.
	if (contains(fields.status, "Failed"))
	{
		out.link = Link::Failed;
		// The CLI's own diagnosis wins, because it comes from the side that
		// actually tried. Ours is only the fallback, and it names the switch that
		// is the cause nine times out of ten.
		out.linkDetail = fields.issue.empty()
		                     ? std::string("claude started the script but could not talk to "
		                                   "this editor — is Remote Control switched on?")
		                     : fields.issue;
	}
	else if (contains(fields.status, "Connected"))
	{
		out.link       = Link::Connected;
		out.linkDetail = "claude started the script and completed the handshake";
	}
	else
	{
		out.link       = Link::Unknown;
		out.linkDetail = "claude reported \"" + fields.status + "\", which this editor does "
		                 "not know how to read";
	}
}

Probe probeClaude(const std::filesystem::path& base, const std::filesystem::path& endpoint)
{
	Probe p;
	p.tools = McpClientSetup::findTools(base);
	if (!p.tools.ready())
	{
		// Nothing is run. Without the CLI there is nobody to ask, and inventing a
		// verdict for the two rows below it would be the whole failure this file
		// exists to prevent.
		p.registration       = Registration::Unknown;
		p.registrationDetail = "not checked — " + p.tools.blocker;
		p.link               = Link::Unknown;
		p.linkDetail         = p.registrationDetail;
		return p;
	}

	HE::Proc::Options get;
	get.exe  = p.tools.claude;
	get.args = getArgs();
	// Longer than the button's twenty seconds on purpose: `get` health-checks the
	// server, which means starting Python, opening a socket and running the
	// handshake, and on a cold machine the interpreter start alone is seconds.
	get.timeoutMs = 30000;
	const HE::Proc::Result res = HE::Proc::run(get);

	classifyGet(res, p.tools.script, endpoint, p);

	p.console  = "$ " + commandLine(p.tools.claude, get.args) + "\n";
	p.console += res.out;
	p.console += res.err;
	if (res.launchFailed) p.console += "(the process could not be started)\n";
	if (res.timedOut)     p.console += "(stopped after 30 s without an answer)\n";
	return p;
}

} // namespace HE::Ed::McpClaudeProbe
