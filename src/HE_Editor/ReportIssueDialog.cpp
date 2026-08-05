#include "ReportIssueDialog.h"

#include <Diagnostics/Log.h>

#include <algorithm>
#include <cstdio>
#include <string>

#ifdef HE_IMGUI_ENABLED
#include "EditorApplication.h"          // AppContext (renderer backend name)
#include "EditorWidgets.h"              // pinDialogToEditorWindow
#include "HorizonVersion.h"             // HE_VERSION_STRING / HE_VERSION_CODENAME
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>      // InputText overloads for std::string
#include <SDL3/SDL.h>                   // SDL_OpenURL
#include <filesystem>
#include <system_error>
#endif

namespace ReportIssueDialog
{

// ─── Report → markdown → URL (pure; no ImGui, no SDL — unit-tested) ──────────

// GitHub rejects issue titles past 256 characters, and an unbounded title would
// eat the URL budget the log needs.
static constexpr std::size_t kMaxTitleLength = 256;

std::string urlEncode(const std::string& text)
{
	static const char* kHex = "0123456789ABCDEF";
	std::string out;
	out.reserve(text.size() * 2);
	for (const unsigned char c : text)
	{
		// RFC 3986 unreserved. Everything else — including the newlines that make
		// up most of a log — becomes %XX, which is why an encoded body runs about
		// three times the raw size and why the trimming below has to exist.
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		    c == '-' || c == '_' || c == '.' || c == '~')
		{
			out.push_back(static_cast<char>(c));
		}
		else
		{
			out.push_back('%');
			out.push_back(kHex[c >> 4]);
			out.push_back(kHex[c & 0x0F]);
		}
	}
	return out;
}

namespace {

// A markdown section, skipped entirely when the field is blank — an issue full
// of empty headings reads worse than a short one.
void appendSection(std::string& body, const char* heading, const std::string& text)
{
	if (text.empty()) return;
	body += "### ";
	body += heading;
	body += "\n\n";
	body += text;
	body += "\n\n";
}

// Cuts to at most `maxBytes` without splitting a UTF-8 code point (continuation
// bytes are 10xxxxxx).
std::string truncateUtf8(const std::string& text, std::size_t maxBytes)
{
	if (text.size() <= maxBytes) return text;
	std::size_t cut = maxBytes;
	while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
	return text.substr(0, cut);
}

} // namespace

std::string buildBody(const Report& report, int logLineLimit)
{
	std::string body;

	appendSection(body, "What happened", report.what);
	appendSection(body, "Steps to reproduce", report.steps);
	appendSection(body, "Expected behaviour", report.expected);

	if (!report.environment.empty())
	{
		// Fenced rather than a bullet list: the values contain characters markdown
		// would otherwise eat, and a fixed-width block stays aligned.
		body += "### Environment\n\n```text\n";
		body += report.environment;
		body += "\n```\n\n";
	}

	const bool wantLines = logLineLimit != 0 && !report.logLines.empty();
	if (!report.logPath.empty() || wantLines)
	{
		body += "### Log\n\n";
		if (!report.logPath.empty())
		{
			body += "Full log file: `";
			body += report.logPath;
			body += "`\n";
		}
		if (!report.logCounts.empty())
		{
			body += "This run: ";
			body += report.logCounts;
			body += "\n";
		}
		body += "\n";
	}

	if (wantLines)
	{
		const int total = static_cast<int>(report.logLines.size());
		const int keep  = logLineLimit < 0 ? total : std::min(logLineLimit, total);
		const int first = total - keep;

		// <details> because a few hundred log lines would otherwise bury the
		// description, which is what anyone triaging reads first.
		char heading[96];
		std::snprintf(heading, sizeof(heading), "Last %d log line%s", keep,
		              keep == 1 ? "" : "s");
		body += "<details><summary>";
		body += heading;
		body += "</summary>\n\n```text\n";
		for (int i = first; i < total; ++i)
		{
			body += report.logLines[static_cast<std::size_t>(i)];
			body += '\n';
		}
		body += "```\n\n</details>\n";
	}

	return body;
}

std::string buildIssueUrl(const Report& report, std::size_t maxUrlLength,
                          int* outLinesKept, bool* outBodyTruncated)
{
	if (outLinesKept)     *outLinesKept     = 0;
	if (outBodyTruncated) *outBodyTruncated = false;

	const std::string prefix = std::string(kIssueBaseUrl) + "?title=" +
	                           urlEncode(truncateUtf8(report.title, kMaxTitleLength)) + "&body=";

	const auto fits = [&](const std::string& body)
	{
		return prefix.size() + urlEncode(body).size() <= maxUrlLength;
	};

	const int total = static_cast<int>(report.logLines.size());

	// The common case: a short report on a quiet run fits whole.
	const std::string full = buildBody(report, -1);
	if (fits(full))
	{
		if (outLinesKept) *outLinesKept = total;
		return prefix + urlEncode(full);
	}

	const std::string logFree = buildBody(report, 0);
	if (!fits(logFree))
	{
		// Not even the description survives intact. Still produce a usable URL and
		// report it, so the dialog can hand the user the whole text another way.
		if (outBodyTruncated) *outBodyTruncated = true;
		std::size_t keepBytes = logFree.size();
		while (keepBytes > 0 && !fits(truncateUtf8(logFree, keepBytes)))
			keepBytes /= 2;
		// Grow back toward the limit, so the cut lands near what actually fits
		// rather than at whatever power of two the halving happened to stop on.
		for (std::size_t step = std::max<std::size_t>(keepBytes, 64); step >= 32; step /= 2)
			if (fits(truncateUtf8(logFree, keepBytes + step))) keepBytes += step;
		return prefix + urlEncode(truncateUtf8(logFree, keepBytes));
	}

	// Largest number of trailing log lines that still fits. Binary search rather
	// than a walk: this runs on the UI thread, and each probe re-encodes the body.
	int lo = 0, hi = total;   // lo always fits (logFree, just checked); hi does not
	while (lo < hi)
	{
		const int mid = lo + (hi - lo + 1) / 2;
		if (fits(buildBody(report, mid))) lo = mid;
		else                              hi = mid - 1;
	}

	if (outLinesKept) *outLinesKept = lo;
	return prefix + urlEncode(buildBody(report, lo));
}

// ─── Dialog ──────────────────────────────────────────────────────────────────

#ifdef HE_IMGUI_ENABLED
namespace {

constexpr const char* kPopupId = "Report an Issue##reportissue";

bool s_openRequested = false;
bool s_isOpen        = false;

// Deliberately NOT cleared when the dialog closes: pressing Escape on a
// half-written report and losing it would be worse than a stale field.
std::string s_title;
std::string s_what;
std::string s_steps;
std::string s_expected;
bool        s_includeLog = true;
std::string s_status;          // outcome line under the buttons

// Snapshotted at open time, not per frame: background work keeps logging while
// the dialog is up, and a report should describe the moment it was raised.
std::vector<std::string> s_logLines;
std::string              s_logPath;
std::string              s_logCounts;
std::string              s_environment;

std::string environmentBlock(AppContext& ctx)
{
	std::string out = "Engine   : Horizon Engine " HE_VERSION_STRING
	                  " \"" HE_VERSION_CODENAME "\"\n";
	out += "Renderer : ";
	out += ctx.backendName.empty() ? std::string("unknown") : ctx.backendName;
	out += "\n";
	out += HE::Log::systemInfoBlock();
	return out;
}

std::string logCountsLine()
{
	const unsigned long long errors =
		HE::Log::messageCount(HE::LogLevel::Error) +
		HE::Log::messageCount(HE::LogLevel::Critical);
	const unsigned long long warnings = HE::Log::messageCount(HE::LogLevel::Warning);
	char buf[96];
	std::snprintf(buf, sizeof(buf), "%llu error%s, %llu warning%s",
	              errors,   errors   == 1 ? "" : "s",
	              warnings, warnings == 1 ? "" : "s");
	return buf;
}

Report currentReport()
{
	Report r;
	r.title       = s_title;
	r.what        = s_what;
	r.steps       = s_steps;
	r.expected    = s_expected;
	r.environment = s_environment;
	if (s_includeLog)
	{
		r.logPath   = s_logPath;
		r.logCounts = s_logCounts;
		r.logLines  = s_logLines;
	}
	return r;
}

// Building the URL means percent-encoding a few hundred log lines several times
// over (the binary search), which is far too much to redo every frame just to
// keep a preview label honest. Recomputed only when an input actually changed.
struct Preview
{
	std::string body;
	std::string url;
	int         linesKept = 0;
	bool        bodyCut   = false;
};

const Preview& preview()
{
	static Preview s_preview;
	static std::string s_key;
	static bool s_valid = false;

	// The log snapshot is part of the key via its size: it only changes when the
	// dialog is reopened, which also re-reads every other field.
	const std::string key = s_title + '\x01' + s_what + '\x01' + s_steps + '\x01' +
	                        s_expected + '\x01' + (s_includeLog ? "1" : "0") + '\x01' +
	                        std::to_string(s_logLines.size());
	if (!s_valid || key != s_key)
	{
		const Report report = currentReport();
		s_preview.body = buildBody(report, -1);
		s_preview.url  = buildIssueUrl(report, kMaxUrlLength,
		                               &s_preview.linesKept, &s_preview.bodyCut);
		s_key   = key;
		s_valid = true;
	}
	return s_preview;
}

// Opens the platform file browser at the log's folder. There is no portable
// "reveal this file" call, so the folder is the honest target — the file is
// named right below the button.
void revealLogFile()
{
	if (s_logPath.empty()) return;
	std::error_code ec;
	const std::filesystem::path dir = std::filesystem::path(s_logPath).parent_path();
	if (dir.empty() || !std::filesystem::exists(dir, ec)) return;
	// Windows paths start with a drive letter, which needs the third slash.
	std::string url  = "file://";
	std::string path = dir.generic_string();
	if (!path.empty() && path.front() != '/') url += '/';
	SDL_OpenURL((url + path).c_str());
}

} // namespace
#endif // HE_IMGUI_ENABLED

void open()
{
#ifdef HE_IMGUI_ENABLED
	s_openRequested = true;
#endif
}

bool isOpen()
{
#ifdef HE_IMGUI_ENABLED
	return s_isOpen;
#else
	return false;
#endif
}

void DrawReportIssueDialog(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	if (s_openRequested)
	{
		s_openRequested = false;
		s_status.clear();
		s_logLines    = HE::Log::recentLines(kMaxLogLines);
		s_logPath     = HE::Log::logFilePath();
		s_logCounts   = logCountsLine();
		s_environment = environmentBlock(ctx);
		s_isOpen      = true;
		ImGui::OpenPopup(kPopupId);
	}

	if (!s_isOpen) return;

	ImGui::SetNextWindowSize(ImVec2(660.0f, 600.0f), ImGuiCond_Appearing);
	EditorWidgets::pinDialogToEditorWindow(ImVec2(520.0f, 0.0f));
	if (!ImGui::BeginPopupModal(kPopupId, nullptr, ImGuiWindowFlags_NoCollapse))
	{
		// Dismissed with Escape rather than through a button.
		s_isOpen = false;
		return;
	}

	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(
		"Describe what went wrong. The editor adds your engine version, this "
		"machine and the recent log, then opens the issue form on GitHub — "
		"nothing leaves the editor until you press Submit there.");
	ImGui::PopTextWrapPos();
	ImGui::Spacing();

	ImGui::TextUnformatted("Title");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputTextWithHint("##ri_title", "Short summary of the problem", &s_title);

	ImGui::Spacing();
	ImGui::TextUnformatted("What happened?");
	ImGui::InputTextMultiline("##ri_what", &s_what, ImVec2(-1.0f, 90.0f));

	ImGui::Spacing();
	ImGui::TextUnformatted("Steps to reproduce");
	ImGui::SameLine(); ImGui::TextDisabled("(optional)");
	ImGui::InputTextMultiline("##ri_steps", &s_steps, ImVec2(-1.0f, 70.0f));

	ImGui::Spacing();
	ImGui::TextUnformatted("What did you expect instead?");
	ImGui::SameLine(); ImGui::TextDisabled("(optional)");
	ImGui::InputTextMultiline("##ri_expected", &s_expected, ImVec2(-1.0f, 50.0f));

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::Checkbox("Attach the engine log", &s_includeLog);
	ImGui::SameLine();
	ImGui::TextDisabled("(%s this run)", s_logCounts.c_str());

	if (s_includeLog)
	{
		ImGui::Indent();
		// Spelled out because it is the one thing here that could surprise
		// someone: a link cannot carry a file, so the issue gets the tail of the
		// log inline and the user drags the file in for the rest.
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled(
			"The most recent log lines travel inside the issue text (%d captured). "
			"A link cannot carry a file, so for the complete log open the folder "
			"below and drag HorizonEngine.log into the GitHub issue.",
			static_cast<int>(s_logLines.size()));
		ImGui::PopTextWrapPos();
		if (!s_logPath.empty())
		{
			if (ImGui::SmallButton("Show Log File")) revealLogFile();
			ImGui::SameLine();
			if (ImGui::SmallButton("Copy Log Path")) ImGui::SetClipboardText(s_logPath.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("%s", s_logPath.c_str());
		}
		ImGui::Unindent();
	}

	ImGui::Spacing();
	if (ImGui::TreeNode("Preview what gets sent"))
	{
		const Preview& p = preview();
		ImGui::TextDisabled("%d of %d log lines fit into the link (%d of %d characters).",
		                    p.linesKept, static_cast<int>(s_includeLog ? s_logLines.size() : 0),
		                    static_cast<int>(p.url.size()), static_cast<int>(kMaxUrlLength));
		std::string body = p.body;   // ReadOnly, but the widget wants a mutable target
		ImGui::InputTextMultiline("##ri_preview", &body, ImVec2(-1.0f, 200.0f),
		                          ImGuiInputTextFlags_ReadOnly);
		ImGui::TreePop();
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Something has to be said; the rest can be filled in on the web form.
	const bool submittable = !s_title.empty() || !s_what.empty();
	if (!submittable) ImGui::BeginDisabled();
	if (ImGui::Button("Open on GitHub", ImVec2(150.0f, 0.0f)))
	{
		const Preview& p = preview();
		SDL_OpenURL(p.url.c_str());

		const int totalLines = static_cast<int>(s_includeLog ? s_logLines.size() : 0);
		if (p.bodyCut)
		{
			// The browser did not get everything — hand over the whole thing rather
			// than let a silently shortened report be submitted.
			ImGui::SetClipboardText((s_title + "\n\n" + p.body).c_str());
			s_status = "Opened GitHub. The report was too long for a link — the full "
			           "text is on your clipboard, paste it over the form.";
		}
		else if (p.linesKept < totalLines)
		{
			char buf[192];
			std::snprintf(buf, sizeof(buf),
			              "Opened GitHub with the last %d of %d log lines. Drag the log "
			              "file into the issue for the rest.", p.linesKept, totalLines);
			s_status = buf;
		}
		else
		{
			s_status = "Opened GitHub in your browser.";
		}
	}
	if (!submittable) ImGui::EndDisabled();

	ImGui::SameLine();
	if (ImGui::Button("Copy Report", ImVec2(120.0f, 0.0f)))
	{
		ImGui::SetClipboardText((s_title + "\n\n" + preview().body).c_str());
		s_status = "Full report copied to the clipboard.";
	}

	ImGui::SameLine();
	if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
	{
		s_isOpen = false;
		ImGui::CloseCurrentPopup();
	}

	if (!s_status.empty())
	{
		ImGui::Spacing();
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(s_status.c_str());
		ImGui::PopTextWrapPos();
	}

	ImGui::EndPopup();
#else
	(void)ctx;
#endif
}

} // namespace ReportIssueDialog
