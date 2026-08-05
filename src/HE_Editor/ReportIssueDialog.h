#pragma once
#include <cstddef>
#include <string>
#include <vector>

struct AppContext;

// ── Help ▸ Report Issue… ─────────────────────────────────────────────────────
// Collects a short bug description plus the machine's environment and the tail
// of the current engine log, and hands the lot to GitHub as a PRE-FILLED new
// issue in the browser. Nothing is transmitted by the editor itself: the URL is
// opened, the user reads the filled-in form and presses Submit — or does not.
//
// On "attaching the log": GitHub's new-issue URL takes a body, not files, so the
// log ride-along is the last N lines inlined into that body (folded into a
// <details> block). A request line has a hard size limit, so buildIssueUrl drops
// log lines from the oldest end until the URL fits. For the COMPLETE log the
// dialog offers "Show Log File" — GitHub accepts a drag-and-drop of the file
// into the comment box, which is the only way a real attachment can happen.
namespace ReportIssueDialog
{
	// Open the dialog (Help ▸ Report Issue…, both menu bars).
	void open();
	bool isOpen();

	// Draw. Safe to call every frame; draws nothing while closed.
	void DrawReportIssueDialog(AppContext& ctx);

	// ── Report → URL (pure, unit-tested; no ImGui/SDL involved) ──────────────

	// The repository's new-issue endpoint.
	inline constexpr const char* kIssueBaseUrl =
		"https://github.com/Horizon-Creations/HorizonEngine/issues/new";

	// GitHub answers 414 well before 8 KB of request line and intermediaries cut
	// in lower still, so the builder aims comfortably under it. A body that does
	// not fit loses log lines, not user text.
	inline constexpr std::size_t kMaxUrlLength = 6000;

	// How many log lines are offered at most, before any URL-length trimming.
	inline constexpr int kMaxLogLines = 400;

	struct Report
	{
		std::string title;
		std::string what;                   // "What happened?"
		std::string steps;                  // optional
		std::string expected;               // optional
		std::string environment;            // pre-rendered "Key : value" lines
		std::string logPath;                // absolute path of the live log file
		std::string logCounts;              // e.g. "2 errors, 7 warnings"
		std::vector<std::string> logLines;  // oldest first; empty = no log wanted
		// What the lines are, for the fold's summary: "warning/error" when they
		// were filtered to those, "log" when everything is on offer. Only a
		// dozen or two lines survive the URL limit, so a reader has to be told
		// whether the gaps are filtering or truncation.
		std::string logLabel = "log";
	};

	// Percent-encodes to RFC 3986 unreserved characters.
	std::string urlEncode(const std::string& text);

	// The markdown issue body. `logLineLimit` < 0 keeps every line, 0 drops the
	// log section's line dump (the path and counts stay — they are the pointer
	// to the full log).
	std::string buildBody(const Report& report, int logLineLimit);

	// kIssueBaseUrl + encoded title/body, with log lines dropped from the oldest
	// end until the result fits `maxUrlLength`. `outLinesKept` (optional) reports
	// how many log lines survived; `outBodyTruncated` is set when even a
	// log-free body had to be cut, which is the dialog's cue to tell the user
	// the browser is not getting the whole report.
	std::string buildIssueUrl(const Report& report, std::size_t maxUrlLength,
	                          int* outLinesKept = nullptr,
	                          bool* outBodyTruncated = nullptr);
}
