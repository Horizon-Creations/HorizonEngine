#include "doctest.h"

#include "ReportIssueDialog.h"

#include <string>
#include <vector>

// ── Help ▸ Report Issue… ─────────────────────────────────────────────────────
// The dialog itself is ImGui, but the piece that can silently break is not: the
// issue is delivered as a pre-filled GitHub URL, and a URL has a hard length
// limit. Overshoot it and the user's browser gets a 414 instead of a form —
// with no failure visible in the editor, since SDL_OpenURL succeeded. So the
// builder must ALWAYS return something under the budget, whatever it is fed,
// and it must spend the room it has on log lines rather than on the user's text.

namespace {

std::vector<std::string> fakeLog(int lines)
{
	std::vector<std::string> out;
	out.reserve(static_cast<std::size_t>(lines));
	for (int i = 0; i < lines; ++i)
		out.push_back("[14:22:07.318] [ INFO] [Render  ] [Main    ] [f    120] line " +
		              std::to_string(i));
	return out;
}

ReportIssueDialog::Report sampleReport(int logLines)
{
	ReportIssueDialog::Report r;
	r.title       = "Viewport goes black after entering play mode";
	r.what        = "The viewport turns black the moment I press Play.";
	r.steps       = "1. Open the sample scene\n2. Press Play";
	r.expected    = "The scene keeps rendering.";
	r.environment = "Engine   : Horizon Engine 0.3.0\nRenderer : Metal";
	r.logPath     = "/Users/someone/HorizonEditor/HorizonEngine.log";
	r.logCounts   = "2 errors, 7 warnings";
	r.logLines    = fakeLog(logLines);
	return r;
}

} // namespace

TEST_CASE("urlEncode leaves unreserved characters alone and escapes the rest")
{
	using ReportIssueDialog::urlEncode;

	CHECK(urlEncode("abcXYZ019-_.~") == "abcXYZ019-_.~");
	CHECK(urlEncode(" ")   == "%20");
	CHECK(urlEncode("\n")  == "%0A");
	CHECK(urlEncode("&")   == "%26");
	CHECK(urlEncode("#")   == "%23");
	CHECK(urlEncode("+")   == "%2B");
	// Non-ASCII must go out byte-wise, not be mangled or dropped: log lines and
	// bug descriptions carry umlauts and the em dash the engine logs with.
	CHECK(urlEncode("ü") == "%C3%BC");
	CHECK(urlEncode("—") == "%E2%80%94");
}

TEST_CASE("buildBody carries the description, environment and log")
{
	const ReportIssueDialog::Report r = sampleReport(5);
	const std::string body = ReportIssueDialog::buildBody(r, -1);

	CHECK(body.find("### What happened") != std::string::npos);
	CHECK(body.find(r.what) != std::string::npos);
	CHECK(body.find("### Steps to reproduce") != std::string::npos);
	CHECK(body.find("### Environment") != std::string::npos);
	CHECK(body.find("Renderer : Metal") != std::string::npos);
	CHECK(body.find(r.logPath) != std::string::npos);
	CHECK(body.find("2 errors, 7 warnings") != std::string::npos);
	// Every log line, and the fold that keeps them from burying the description.
	CHECK(body.find("<details>") != std::string::npos);
	for (const std::string& line : r.logLines)
		CHECK(body.find(line) != std::string::npos);
}

TEST_CASE("buildBody skips empty sections but keeps the log pointer")
{
	ReportIssueDialog::Report r;
	r.what    = "It crashed.";
	r.logPath = "/tmp/HorizonEngine.log";

	const std::string body = ReportIssueDialog::buildBody(r, -1);
	CHECK(body.find("### What happened") != std::string::npos);
	CHECK(body.find("### Steps to reproduce") == std::string::npos);
	CHECK(body.find("### Expected behaviour") == std::string::npos);
	CHECK(body.find("### Environment") == std::string::npos);
	// No lines to show, but the path to the full log still belongs in the issue.
	CHECK(body.find("### Log") != std::string::npos);
	CHECK(body.find("/tmp/HorizonEngine.log") != std::string::npos);
	CHECK(body.find("<details>") == std::string::npos);
}

TEST_CASE("buildBody keeps the NEWEST log lines when it has to choose")
{
	const ReportIssueDialog::Report r = sampleReport(20);
	const std::string body = ReportIssueDialog::buildBody(r, 3);

	// The tail is what matters — whatever the engine said last is what preceded
	// the problem being reported.
	CHECK(body.find("line 19") != std::string::npos);
	CHECK(body.find("line 18") != std::string::npos);
	CHECK(body.find("line 17") != std::string::npos);
	CHECK(body.find("line 16") == std::string::npos);
	CHECK(body.find("line 0")  == std::string::npos);
	CHECK(body.find("Last 3 log lines") != std::string::npos);
}

TEST_CASE("buildIssueUrl stays under the budget and keeps as much log as fits")
{
	const ReportIssueDialog::Report r = sampleReport(ReportIssueDialog::kMaxLogLines);

	int  kept = -1;
	bool cut  = true;
	const std::string url =
		ReportIssueDialog::buildIssueUrl(r, ReportIssueDialog::kMaxUrlLength, &kept, &cut);

	CHECK(url.size() <= ReportIssueDialog::kMaxUrlLength);
	CHECK(url.rfind(ReportIssueDialog::kIssueBaseUrl, 0) == 0);
	CHECK(url.find("?title=") != std::string::npos);
	CHECK(url.find("&body=")  != std::string::npos);

	// The user's own text is never what gets dropped — only log lines are.
	CHECK_FALSE(cut);
	CHECK(kept > 0);
	CHECK(kept < static_cast<int>(r.logLines.size()));

	// And the kept count is really the most that fits: one more must not.
	const std::string oneMore =
		std::string(ReportIssueDialog::kIssueBaseUrl) + "?title=" +
		ReportIssueDialog::urlEncode(r.title) + "&body=" +
		ReportIssueDialog::urlEncode(ReportIssueDialog::buildBody(r, kept + 1));
	CHECK(oneMore.size() > ReportIssueDialog::kMaxUrlLength);
}

TEST_CASE("buildIssueUrl keeps every log line when the whole report fits")
{
	const ReportIssueDialog::Report r = sampleReport(3);

	int  kept = -1;
	bool cut  = true;
	const std::string url =
		ReportIssueDialog::buildIssueUrl(r, ReportIssueDialog::kMaxUrlLength, &kept, &cut);

	CHECK(url.size() <= ReportIssueDialog::kMaxUrlLength);
	CHECK(kept == 3);
	CHECK_FALSE(cut);
}

TEST_CASE("buildIssueUrl survives a report that cannot fit at all")
{
	// A pasted stack trace in the description: even with no log the body is over
	// budget. The URL must still be valid and under the limit, and the caller
	// must be told, so the dialog can put the full text on the clipboard instead
	// of letting a silently shortened report go out.
	ReportIssueDialog::Report r;
	r.title = std::string(4000, 'T');
	r.what  = std::string(40000, 'x');

	int  kept = -1;
	bool cut  = false;
	const std::string url =
		ReportIssueDialog::buildIssueUrl(r, ReportIssueDialog::kMaxUrlLength, &kept, &cut);

	CHECK(url.size() <= ReportIssueDialog::kMaxUrlLength);
	CHECK(url.rfind(ReportIssueDialog::kIssueBaseUrl, 0) == 0);
	CHECK(cut);
	CHECK(kept == 0);
}

TEST_CASE("buildIssueUrl never splits a multi-byte character")
{
	// Truncation happens on the raw body, which is UTF-8. Cutting mid-code-point
	// would emit a lone continuation byte — a broken percent sequence that some
	// browsers reject outright.
	ReportIssueDialog::Report r;
	r.title = "ü";
	for (int i = 0; i < 4000; ++i) r.what += "äöü—";

	bool cut = false;
	const std::string url =
		ReportIssueDialog::buildIssueUrl(r, ReportIssueDialog::kMaxUrlLength, nullptr, &cut);
	CHECK(cut);
	CHECK(url.size() <= ReportIssueDialog::kMaxUrlLength);

	// Walk the encoded body back to bytes and check it is well-formed UTF-8.
	const std::size_t bodyAt = url.find("&body=") + 6;
	std::string bytes;
	for (std::size_t i = bodyAt; i < url.size(); )
	{
		if (url[i] == '%' && i + 2 < url.size())
		{
			bytes.push_back(static_cast<char>(std::stoi(url.substr(i + 1, 2), nullptr, 16)));
			i += 3;
		}
		else
		{
			bytes.push_back(url[i]);
			++i;
		}
	}

	std::size_t i = 0;
	bool wellFormed = true;
	while (i < bytes.size())
	{
		const unsigned char c = static_cast<unsigned char>(bytes[i]);
		std::size_t extra = 0;
		if      (c < 0x80)          extra = 0;
		else if ((c & 0xE0) == 0xC0) extra = 1;
		else if ((c & 0xF0) == 0xE0) extra = 2;
		else if ((c & 0xF8) == 0xF0) extra = 3;
		else { wellFormed = false; break; }

		if (i + extra >= bytes.size() + (extra ? 0 : 1)) { wellFormed = false; break; }
		for (std::size_t k = 1; k <= extra; ++k)
		{
			if (i + k >= bytes.size() ||
			    (static_cast<unsigned char>(bytes[i + k]) & 0xC0) != 0x80)
			{
				wellFormed = false;
				break;
			}
		}
		if (!wellFormed) break;
		i += extra + 1;
	}
	CHECK(wellFormed);
}
