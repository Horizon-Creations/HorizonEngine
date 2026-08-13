#include "ReportIssueDialog.h"

#include <Diagnostics/Log.h>

#include <algorithm>
#include <cstdio>
#include <string>

#ifdef HE_IMGUI_ENABLED
#include "EditorApplication.h"          // AppContext (renderer backend name)
#include "EditorWidgets.h"              // pinDialogToEditorWindow
#include "HorizonVersion.h"             // HE_VERSION_STRING / HE_VERSION_CODENAME
#include <Diagnostics/GlobalState.h>    // project path — cwd for the credential probe
#include <SourceControl/GitCli.h>       // reading a stored GitHub token back
#include <SourceControl/GitHubApi.h>    // gist upload + issue creation
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>      // InputText overloads for std::string
#include <SDL3/SDL.h>                   // SDL_OpenURL
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>
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
	if (!report.logPath.empty() || wantLines || !report.logGistUrl.empty())
	{
		body += "### Log\n\n";
		// The uploaded copy first: it is the one a reader can actually open,
		// and it makes the inlined lines below a summary rather than the story.
		if (!report.logGistUrl.empty())
		{
			body += "Full log: ";
			body += report.logGistUrl;
			body += "\n";
		}
		if (!report.logPath.empty())
		{
			body += report.logGistUrl.empty() ? "Full log file: `" : "Local path: `";
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
		char heading[128];
		std::snprintf(heading, sizeof(heading), "Last %d %s line%s", keep,
		              report.logLabel.empty() ? "log" : report.logLabel.c_str(),
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

// Which lines ride along inside the issue body. Only a dozen or two survive the
// URL length limit, so the default spends them on the records that say
// something went wrong rather than on startup chatter — "Everything" is there
// for the bugs that produce no warning at all.
bool s_problemsOnly = true;

// Snapshotted at open time, not per frame: background work keeps logging while
// the dialog is up, and a report should describe the moment it was raised.
std::vector<std::string> s_logAll;        // every level, newest kMaxLogLines
std::vector<std::string> s_logProblems;   // Warning and above only
std::string              s_logPath;
std::string              s_logCounts;
std::string              s_environment;

const std::vector<std::string>& chosenLogLines()
{
	return s_problemsOnly ? s_logProblems : s_logAll;
}

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
		r.logLines  = chosenLogLines();
		r.logLabel  = s_problemsOnly ? "warning/error" : "log";
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
	                        s_expected + '\x01' + (s_includeLog ? "1" : "0") +
	                        (s_problemsOnly ? "p" : "a") + '\x01' +
	                        std::to_string(chosenLogLines().size());
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

// ── Direct submission through the GitHub API ────────────────────────────────
// Both the token probe and the submit itself are HTTPS round trips, so both run
// on a worker thread; the UI thread only ever reads the atomics and the
// mutex-guarded strings below, and joins once running flips false.
//
// The token is deliberately NOT kept between the probe and the submit. It is
// fetched from git's credential helper, used, and dropped — the same rule the
// source-control layer follows, and the reason a report cannot leak one from a
// dialog someone left open.

enum class Phase { Idle, Probing, Submitting, Succeeded, Failed };

std::atomic<bool> s_workerRunning{false};
std::thread       s_worker;
std::mutex        s_workerMutex;      // guards everything in this block

Phase       s_phase = Phase::Idle;
std::string s_githubLogin;            // "" = no usable stored token
std::string s_workerStep;             // what the worker is doing right now
std::string s_workerError;
std::string s_issueUrl;               // set on success
std::string s_gistNote;               // why the log upload did not happen, if so

// Only ever holds what the user typed, and only while the dialog is open.
std::string s_pastedToken;

// Read on the UI thread at open time and handed to the worker: git's credential
// helper is often configured repo-locally (ensureCredentialHelper writes
// --local), so the probe has to run with the project as its working directory
// or it will not see the token the Source Control panel stored.
std::filesystem::path s_credentialRoot;

struct WorkerView   // a UI-thread snapshot, taken under one lock
{
	Phase       phase = Phase::Idle;
	std::string login, step, error, issueUrl, gistNote;
};

WorkerView workerView()
{
	std::lock_guard<std::mutex> lk(s_workerMutex);
	return WorkerView{ s_phase, s_githubLogin, s_workerStep, s_workerError,
	                   s_issueUrl, s_gistNote };
}

void setStep(const char* step)
{
	std::lock_guard<std::mutex> lk(s_workerMutex);
	s_workerStep = step;
}

// Overwrite before releasing: a token that merely went out of scope is still
// sitting in freed heap memory for anyone who looks.
void wipe(std::string& secret)
{
	for (char& c : secret) c = '\0';
	secret.clear();
}

// Reap a finished worker. Must run on the UI thread, and before starting
// another — a joinable thread that gets overwritten terminates the process.
void reapWorker()
{
	if (!s_workerRunning.load(std::memory_order_acquire) && s_worker.joinable())
		s_worker.join();
}

// The tail of the log file, which is the whole file unless it has grown past
// the cap. Read fresh from disk rather than from the ring: the point of the
// upload is to carry MORE than the ring holds.
std::string readLogTail(const std::string& path, std::size_t maxBytes)
{
	if (path.empty()) return {};
	HE::Log::flush();

	std::error_code ec;
	const std::uintmax_t size = std::filesystem::file_size(path, ec);
	if (ec) return {};

	std::FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return {};

	std::string out;
	std::size_t want = static_cast<std::size_t>(std::min<std::uintmax_t>(size, maxBytes));
	if (size > want && std::fseek(f, static_cast<long>(size - want), SEEK_SET) != 0)
	{
		std::fclose(f);
		return {};
	}
	out.resize(want);
	const std::size_t got = std::fread(out.data(), 1, want, f);
	std::fclose(f);
	out.resize(got);

	if (size > got)
		out.insert(0, "[… earlier lines omitted; log is larger than the upload limit …]\n");
	return out;
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

// Whatever git's helper holds for github.com, or empty. Not an error when there
// is nothing: most users have never signed in, and the browser path is the
// answer for them.
//
// `root` is passed rather than read from the static: reopening the dialog
// rewrites s_credentialRoot, and a worker still reading it would be a race.
std::string storedGitHubToken(const std::filesystem::path& root)
{
	std::string user, secret;
	if (!HE::Sc::GitCli::fillCredential(root, "github.com", user, secret))
		return {};
	return secret;
}

// Is a token stored, and whose is it? Runs once when the dialog opens so the
// user can see which account would speak for them BEFORE writing a report
// against it — and so an expired token is caught now rather than at submit.
void startTokenProbe()
{
	reapWorker();
	if (s_workerRunning.load(std::memory_order_acquire)) return;

	{
		std::lock_guard<std::mutex> lk(s_workerMutex);
		s_phase = Phase::Probing;
		s_githubLogin.clear();
		s_workerError.clear();
		s_workerStep = "Looking for a stored GitHub sign-in…";
	}
	s_workerRunning.store(true, std::memory_order_release);

	s_worker = std::thread([root = s_credentialRoot]
	{
		std::string token = storedGitHubToken(root);
		std::string login, error;
		if (!token.empty())
		{
			HE::Sc::GitHubUser user;
			if (HE::Sc::GitHubApi::currentUser(token, user, &error)) login = user.login;
		}
		wipe(token);

		{
			std::lock_guard<std::mutex> lk(s_workerMutex);
			s_githubLogin = login;
			s_workerStep.clear();
			// A stored token that GitHub rejects is worth saying out loud; having
			// none at all is not a problem, it is just the other path.
			s_workerError = login.empty() ? error : std::string();
			s_phase       = Phase::Idle;
		}
		s_workerRunning.store(false, std::memory_order_release);
	});
}

// Upload the log, file the issue. `pastedToken` wins over the stored one so a
// user with the wrong token in their keychain can still get a report out.
void startSubmit(Report report, std::string pastedToken, bool attachFullLog)
{
	reapWorker();
	if (s_workerRunning.load(std::memory_order_acquire)) return;

	{
		std::lock_guard<std::mutex> lk(s_workerMutex);
		s_phase = Phase::Submitting;
		s_workerError.clear();
		s_issueUrl.clear();
		s_gistNote.clear();
		s_workerStep = "Signing in to GitHub…";
	}
	s_workerRunning.store(true, std::memory_order_release);

	const std::string logPath = s_logPath;

	s_worker = std::thread(
		[report = std::move(report), pastedToken = std::move(pastedToken),
		 attachFullLog, logPath, root = s_credentialRoot]() mutable
	{
		std::string token = pastedToken.empty() ? storedGitHubToken(root) : pastedToken;
		wipe(pastedToken);

		const auto fail = [&](const std::string& why)
		{
			wipe(token);
			// Logged as well as shown: the message names the missing scope, and
			// that is exactly what a user pastes into a follow-up question.
			HE_LOG_WARN(Editor, "Report Issue: %s", why.c_str());
			std::lock_guard<std::mutex> lk(s_workerMutex);
			s_workerError = why;
			s_workerStep.clear();
			s_phase = Phase::Failed;
		};

		if (token.empty())
		{
			fail("No GitHub token available. Paste one, or use \"Open in Browser\".");
			s_workerRunning.store(false, std::memory_order_release);
			return;
		}

		// Verify before uploading anything: a dead token should not cost the user
		// a gist that then has no issue pointing at it.
		std::string       error;
		HE::Sc::GitHubUser user;
		if (!HE::Sc::GitHubApi::currentUser(token, user, &error))
		{
			fail(error);
			s_workerRunning.store(false, std::memory_order_release);
			return;
		}

		std::string gistNote;
		if (attachFullLog)
		{
			setStep("Uploading the log…");
			const std::string contents = readLogTail(logPath, kMaxGistBytes);
			if (contents.empty())
			{
				gistNote = "The log file could not be read, so only the lines below are attached.";
			}
			else
			{
				HE::Sc::CreatedGist gist;
				std::string gistErr;
				if (HE::Sc::GitHubApi::createGist(
						token, "Horizon Engine log for a bug report", "HorizonEngine.log",
						contents, /*isPublic=*/false, gist, &gistErr))
				{
					report.logGistUrl = gist.htmlUrl;
				}
				else
				{
					// Not fatal, and common: a token stored for pushing a game
					// usually has no gist permission. File the issue anyway rather
					// than throwing away a written report over an attachment.
					gistNote = "The log could not be uploaded (" + gistErr +
					           "). The issue was filed with the lines below only.";
				}
			}
		}

		setStep("Filing the issue…");
		HE::Sc::CreatedIssue issue;
		if (!HE::Sc::GitHubApi::createIssue(token, kIssueOwner, kIssueRepo, report.title,
		                                    buildBody(report, -1), issue, &error))
		{
			// Say what DID happen — an uploaded gist is a real side effect, and a
			// user who is told nothing will upload a second one on the next try.
			if (!report.logGistUrl.empty())
				error += "\nThe log was already uploaded to " + report.logGistUrl;
			fail(error);
			s_workerRunning.store(false, std::memory_order_release);
			return;
		}

		wipe(token);
		// The address goes in the log too. The dialog can be dismissed with
		// Escape the moment it appears, and then this is the only record the
		// user has of where their report went.
		HE_LOG_INFO(Editor, "Report Issue: filed #%d — %s", issue.number,
		            issue.htmlUrl.c_str());
		if (!report.logGistUrl.empty())
			HE_LOG_INFO(Editor, "Report Issue: log uploaded to %s", report.logGistUrl.c_str());

		{
			std::lock_guard<std::mutex> lk(s_workerMutex);
			s_issueUrl = issue.htmlUrl;
			s_gistNote = gistNote;
			s_workerStep.clear();
			s_phase = Phase::Succeeded;
		}
		s_workerRunning.store(false, std::memory_order_release);
	});
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
	// Join a worker that finished while the dialog was closed, so a shut dialog
	// never leaves a joinable thread lying around.
	reapWorker();

	if (s_openRequested)
	{
		s_openRequested = false;
		s_status.clear();
		s_logAll      = HE::Log::recentLines(kMaxLogLines);
		s_logProblems = HE::Log::recentProblemLines(kMaxLogLines, HE::LogLevel::Warning);
		s_logPath     = HE::Log::logFilePath();
		s_logCounts   = logCountsLine();
		s_environment = environmentBlock(ctx);
		// A clean run has nothing to filter TO, and an empty fold would be worse
		// than the tail: plenty of bugs (wrong pixels, a frozen gizmo) never log
		// a word. Fall back to everything rather than attaching nothing.
		s_problemsOnly = !s_logProblems.empty();
		s_isOpen       = true;

		// git's credential helper is often configured repo-locally, so the probe
		// has to run with the project as its working directory or it will not see
		// the token the Source Control panel stored.
		s_credentialRoot.clear();
		if (ctx.globalState)
		{
			std::error_code ec;
			std::filesystem::path p = ctx.globalState->getLastProjectPath();
			if (!p.empty())
			{
				if (std::filesystem::is_regular_file(p, ec)) p = p.parent_path();
				s_credentialRoot = p;
			}
		}
		if (s_credentialRoot.empty())
		{
			std::error_code ec;
			s_credentialRoot = std::filesystem::current_path(ec);
		}

		{
			std::lock_guard<std::mutex> lk(s_workerMutex);
			s_phase = Phase::Idle;
			s_issueUrl.clear();
			s_workerError.clear();
			s_gistNote.clear();
		}
		wipe(s_pastedToken);
		startTokenProbe();

		ImGui::OpenPopup(kPopupId);
	}

	if (!s_isOpen) return;

	// Escape closes an ImGui modal, and there is no flag to stop it. While a
	// submit is in flight that would throw away the one thing the user is
	// waiting for — whether their report landed, and its address — so the popup
	// is put straight back. (ImGui also force-closes a modal when another
	// same-level popup opens, which this covers too.)
	const bool workBusy = s_workerRunning.load(std::memory_order_acquire);
	if (workBusy && !ImGui::IsPopupOpen(kPopupId)) ImGui::OpenPopup(kPopupId);

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
		"machine and the recent log. File it directly if you are signed in to "
		"GitHub, or open the pre-filled form in your browser and send it "
		"yourself.");
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

		// Only a dozen or two lines survive the URL limit, so which lines they
		// are matters more than how many: warnings and errors first.
		if (ImGui::RadioButton("Warnings & errors", s_problemsOnly)) s_problemsOnly = true;
		ImGui::SameLine();
		if (ImGui::RadioButton("Everything", !s_problemsOnly)) s_problemsOnly = false;
		ImGui::SameLine();
		ImGui::TextDisabled("(%d of %d lines)",
		                    static_cast<int>(chosenLogLines().size()),
		                    static_cast<int>(s_logAll.size()));
		if (s_problemsOnly && s_logProblems.empty())
		{
			// Wrapped, not clipped. This dialog is where a user goes when
			// something is already wrong, and every explanatory line in it is a
			// whole sentence — the one below explains why the attachment they
			// just chose is empty. Clipped at the right edge it reads as a
			// shorter, different sentence, with nothing to say that more was
			// meant; the dialog is pinned to the editor window and capped to its
			// work area (pinDialogToEditorWindow), so on a small display it is
			// genuinely narrower than these sentences are long.
			//
			// Pushed in small scopes like this one rather than once around the
			// whole dialog: the body has two exits, each with its own EndPopup,
			// and a guard living across either of them would pop the wrap off the
			// PARENT window. The hand-written PushTextWrapPos/PopTextWrapPos pairs
			// elsewhere in this function already do the same job and are left as
			// they are — this only fills in the lines that had none.
			EditorWidgets::WrapText wrap;
			ImGui::TextDisabled("Nothing was logged as a warning or an error this run.");
		}

		// The two routes attach genuinely different amounts, and which one the
		// user is about to take changes what "attach the log" means — worth
		// saying here rather than letting them find out afterwards.
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled(
			"Those lines travel inside the issue text. Filing directly also uploads "
			"the COMPLETE log file as a secret gist and links it. Through the "
			"browser a link cannot carry a file — drag HorizonEngine.log into the "
			"issue yourself for the rest.");
		ImGui::PopTextWrapPos();
		if (!s_logPath.empty())
		{
			if (ImGui::SmallButton("Show Log File")) revealLogFile();
			ImGui::SameLine();
			if (ImGui::SmallButton("Copy Log Path")) ImGui::SetClipboardText(s_logPath.c_str());
			ImGui::SameLine();
			// An absolute log path never fits after two buttons. It wraps onto
			// further lines rather than being cut at the edge — the tail of a path
			// (the file name) is the part that identifies it.
			{
				EditorWidgets::WrapText wrap;
				ImGui::TextDisabled("%s", s_logPath.c_str());
			}
		}
		ImGui::Unindent();
	}

	ImGui::Spacing();
	if (ImGui::TreeNode("Preview what gets sent"))
	{
		const Preview& p = preview();
		// Only the browser route is length-bound; filing directly sends all of it.
		{
			EditorWidgets::WrapText wrap;
			ImGui::TextDisabled(
				"Filing directly sends all of this. Through the browser %d of %d log "
				"lines fit into the link (%d of %d characters).",
				p.linesKept, static_cast<int>(s_includeLog ? chosenLogLines().size() : 0),
				static_cast<int>(p.url.size()), static_cast<int>(kMaxUrlLength));
		}
		std::string body = p.body;   // ReadOnly, but the widget wants a mutable target
		ImGui::InputTextMultiline("##ri_preview", &body, ImVec2(-1.0f, 200.0f),
		                          ImGuiInputTextFlags_ReadOnly);
		ImGui::TreePop();
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// ── Filing it ────────────────────────────────────────────────────────────
	const WorkerView w = workerView();
	const bool busy = s_workerRunning.load(std::memory_order_acquire);

	if (w.phase == Phase::Succeeded && !w.issueUrl.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.85f, 0.45f, 1.0f));
		ImGui::TextUnformatted("The issue has been filed.");
		ImGui::PopStyleColor();
		// The address of the report that was just filed — the one line here worth
		// reading in full even though "Copy Link" sits right below it.
		{
			EditorWidgets::WrapText wrap;
			ImGui::TextDisabled("%s", w.issueUrl.c_str());
		}
		if (!w.gistNote.empty())
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("%s", w.gistNote.c_str());
			ImGui::PopTextWrapPos();
		}
		ImGui::Spacing();
		if (ImGui::Button("Open Issue", ImVec2(150.0f, 0.0f))) SDL_OpenURL(w.issueUrl.c_str());
		ImGui::SameLine();
		if (ImGui::Button("Copy Link", ImVec2(120.0f, 0.0f)))
			ImGui::SetClipboardText(w.issueUrl.c_str());
		ImGui::SameLine();
		if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
		{
			// A filed report is finished business — leaving the text behind would
			// make the next one start as an edit of the last.
			s_title.clear(); s_what.clear(); s_steps.clear(); s_expected.clear();
			s_isOpen = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
		return;
	}

	if (busy)
	{
		// The step text is a sentence ("Looking for a stored GitHub sign-in…"),
		// and it is the only feedback there is while a submit is in flight.
		EditorWidgets::WrapText wrap;
		ImGui::TextUnformatted(w.step.empty() ? "Working…" : w.step.c_str());
	}
	else if (!w.login.empty())
	{
		// Named explicitly: this posts publicly under the user's own account, and
		// they should see whose before they press anything — which is precisely
		// the sentence that must not lose its second half to the window edge.
		EditorWidgets::WrapText wrap;
		ImGui::Text("Signed in to GitHub as %s.", w.login.c_str());
		ImGui::TextDisabled("Filing directly posts the issue under that account.");
	}
	else
	{
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled(
			"Not signed in to GitHub. The browser route needs no account. To file "
			"directly — and upload the whole log — paste a token with 'issues' and "
			"'gist' access; it is used once and never stored.");
		ImGui::PopTextWrapPos();
		if (!w.error.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(w.error.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
		}
		ImGui::SetNextItemWidth(320.0f);
		ImGui::InputTextWithHint("##ri_token", "GitHub token (optional)", &s_pastedToken,
		                         ImGuiInputTextFlags_Password);
		ImGui::SameLine();
		if (ImGui::SmallButton("Create a token"))
			SDL_OpenURL("https://github.com/settings/tokens");
	}

	if (w.phase == Phase::Failed && !w.error.empty() && !busy)
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(w.error.c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
	}

	ImGui::Spacing();

	// GitHub requires a title of its own; the browser form can be finished by
	// hand, but an API call with no title is simply rejected.
	const bool haveToken   = !w.login.empty() || !s_pastedToken.empty();
	const bool canFile     = !busy && haveToken && !s_title.empty();
	if (!canFile) ImGui::BeginDisabled();
	if (ImGui::Button("File on GitHub", ImVec2(150.0f, 0.0f)))
	{
		s_status.clear();
		Report report = currentReport();
		startSubmit(std::move(report), s_pastedToken, s_includeLog);
		wipe(s_pastedToken);
	}
	if (!canFile) ImGui::EndDisabled();
	if (!busy && haveToken && s_title.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("GitHub needs a title for the issue.");
	ImGui::SameLine();

	if (busy) ImGui::BeginDisabled();

	// Something has to be said; the rest can be filled in on the web form.
	const bool submittable = !s_title.empty() || !s_what.empty();
	if (!submittable) ImGui::BeginDisabled();
	if (ImGui::Button("Open in Browser", ImVec2(150.0f, 0.0f)))
	{
		const Preview& p = preview();
		SDL_OpenURL(p.url.c_str());

		const int totalLines = static_cast<int>(s_includeLog ? chosenLogLines().size() : 0);
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

	if (busy) ImGui::EndDisabled();

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

void joinPendingWork()
{
#ifdef HE_IMGUI_ENABLED
	// Editor shutdown. A submit in flight has already POSTED or is about to, so
	// waiting is also the only way the user learns whether their report landed.
	if (s_worker.joinable()) s_worker.join();
#endif
}

} // namespace ReportIssueDialog
