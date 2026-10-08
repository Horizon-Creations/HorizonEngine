#include "GitHubSignIn.h"
#include "EditorApplication.h"        // AppContext, GitController
#include "EditorHelp.h"               // "Source Control/<label>" for the row's buttons
#include "EditorWidgets.h"            // pinDialogToEditorWindow, WrapText, buttons
#include "GitHubSignInView.h"

#include <Diagnostics/Log.h>
#include <SourceControl/GitHubApi.h>
#include <SourceControl/GitHubOAuth.h>
#include <SourceControl/GitHubTokenStore.h>

#include <SDL3/SDL.h>                 // SDL_OpenURL, SDL_GetTicks
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace GitHubSignIn
{

namespace fs  = std::filesystem;
namespace SIV = GitHubSignInView;
using HE::Sc::GitHubDeviceLogin;
using HE::Sc::GitHubTokenStore;

namespace {

// Overwrite before releasing: a token that merely went out of scope is still
// sitting in freed heap memory.
void wipe(std::string& s)
{
	std::fill(s.begin(), s.end(), '\0');
	s.clear();
}

// ── The worker: probe, save, sign out ────────────────────────────────────────
// One at a time. Each is a git subprocess or two plus at most one HTTPS round
// trip, so none of it may run on the frame thread.

enum class Job { Probe, Save, SignOut };

std::thread       s_worker;
std::atomic<bool> s_workerRunning{ false };

std::mutex    s_mutex;                 // guards everything down to the next blank line
Account       s_account    = Account::Unknown;
std::string   s_login;
std::string   s_error;
std::uint64_t s_generation = 0;
bool          s_saveDone   = false;    // a Save job has answered; the flow collects it
bool          s_saveOk     = false;
std::string   s_saveLogin;
std::string   s_saveError;

// UI thread only.
fs::path s_checkedRoot;
bool     s_everChecked = false;

void setAccount(Account a, std::string login, std::string error)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	s_account = a;
	s_login   = std::move(login);
	s_error   = std::move(error);
	++s_generation;
}

// Whose is the stored token, if there is one?
void probe(const fs::path& root)
{
	std::string token;
	if (!GitHubTokenStore::load(root, token))
	{
		setAccount(Account::SignedOut, {}, {});
		return;
	}
	HE::Sc::GitHubUser user;
	std::string err;
	const bool ok = HE::Sc::GitHubApi::currentUser(token, user, &err);
	wipe(token);
	if (ok) setAccount(Account::SignedIn, user.login, {});
	else    setAccount(Account::Rejected, {}, err);
}

void runSave(const fs::path& root, std::string token)
{
	std::string err;
	bool ok = GitHubTokenStore::store(root, token, &err);
	wipe(token);

	// Read it back from where every other reader reads it, and ask GitHub with
	// THAT copy: the proof that the sign-in is usable is that the stored token
	// works, not that the one in hand did.
	std::string stored;
	HE::Sc::GitHubUser user;
	if (ok && !GitHubTokenStore::load(root, stored))
	{
		ok  = false;
		err = "The token was handed to git's credential helper but could not be "
		      "read back from it. Check the credential helper under Preferences "
		      "\xe2\x96\xb8 Source Control.";
	}
	if (ok) ok = HE::Sc::GitHubApi::currentUser(stored, user, &err);
	wipe(stored);

	if (ok)
	{
		HE_LOG_INFO(Editor, "GitHub sign-in stored in the credential helper (%s).",
		            user.login.c_str());
		setAccount(Account::SignedIn, user.login, {});
	}
	else
	{
		HE_LOG_WARN(Editor, "GitHub sign-in could not be completed: %s", err.c_str());
		probe(root);   // whatever IS stored now decides the account row
	}

	std::lock_guard<std::mutex> lk(s_mutex);
	s_saveDone  = true;
	s_saveOk    = ok;
	s_saveLogin = ok ? user.login : std::string{};
	s_saveError = ok ? std::string{} : err;
}

void runSignOut(const fs::path& root)
{
	std::string err;
	if (GitHubTokenStore::forget(root, &err))
	{
		HE_LOG_INFO(Editor, "Signed out of GitHub: token removed from the credential helper.");
		setAccount(Account::SignedOut, {}, {});
		return;
	}
	probe(root);
	std::lock_guard<std::mutex> lk(s_mutex);
	s_error = "Signing out did not work: " + err;
	++s_generation;
}

// Must run on the UI thread, and before starting another — a joinable thread
// that gets overwritten terminates the process.
void reapWorker()
{
	if (!s_workerRunning.load(std::memory_order_acquire) && s_worker.joinable())
		s_worker.join();
}

bool startJob(Job job, fs::path root, std::string token = {})
{
	reapWorker();
	if (s_workerRunning.load(std::memory_order_acquire)) { wipe(token); return false; }
	{
		std::lock_guard<std::mutex> lk(s_mutex);
		s_account = Account::Checking;
	}
	s_workerRunning.store(true, std::memory_order_release);
	s_worker = std::thread([job, root = std::move(root), token = std::move(token)]() mutable {
		switch (job)
		{
		case Job::Probe:   probe(root);                      break;
		case Job::Save:    runSave(root, std::move(token));  break;
		case Job::SignOut: runSignOut(root);                 break;
		}
		wipe(token);
		s_workerRunning.store(false, std::memory_order_release);
	});
	return true;
}

// ── The flow (UI thread only) ────────────────────────────────────────────────

// Owned, not a plain static: its destructor joins the poll thread, which must
// happen in joinPendingWork() and not during static destruction.
std::unique_ptr<GitHubDeviceLogin> s_device;

enum class Result { None, SignedIn, Failed };

bool          s_flowActive    = false;
fs::path      s_flowRoot;
bool          s_saving        = false;   // the token went to the worker
Result        s_result        = Result::None;
std::string   s_resultLogin;
std::string   s_resultError;
bool          s_waitStamped   = false;   // countdown start taken
std::uint64_t s_waitStartMs   = 0;
std::uint64_t s_copiedAtMs    = 0;
bool          s_openRequested = false;
bool          s_modalOpen     = false;   // the standalone popup was opened and not yet closed

constexpr const char*   kPopupId     = "##GitHubSignIn";
constexpr float         kDialogWidth = 520.0f;
constexpr std::uint64_t kCopiedShowMs = 2500;

void resetFlowView()
{
	s_result = Result::None;
	s_resultLogin.clear();
	s_resultError.clear();
	s_waitStamped = false;
	s_copiedAtMs  = 0;
}

// Once per frame: hand a granted token to the worker, collect what it said.
void pump()
{
	reapWorker();

	{
		std::lock_guard<std::mutex> lk(s_mutex);
		if (s_saveDone)
		{
			s_saveDone = false;
			s_saving   = false;
			s_result      = s_saveOk ? Result::SignedIn : Result::Failed;
			s_resultLogin = std::move(s_saveLogin);
			s_resultError = std::move(s_saveError);
			s_saveLogin.clear();
			s_saveError.clear();
		}
	}

	// A granted token is saved even when the dialog that showed the flow has
	// closed in the meantime: the user approved on GitHub, and that is the
	// decision that counts. It waits only while the worker is busy.
	if (s_device && !s_saving && s_device->state() == GitHubDeviceLogin::State::Granted &&
	    !s_workerRunning.load(std::memory_order_acquire))
	{
		std::string token = s_device->takeToken();
		if (!token.empty())
		{
			s_saving = startJob(Job::Save, s_flowRoot, std::move(token));
		}
	}
}

SIV::View currentView()
{
	SIV::View v;
	if (!s_flowActive || !s_device) return v;

	if (s_result == Result::SignedIn) { v.phase = SIV::Phase::SignedIn; v.login = s_resultLogin; return v; }
	if (s_result == Result::Failed)   { v.phase = SIV::Phase::Failed;   v.error = s_resultError; return v; }
	if (s_saving) { v.phase = SIV::Phase::Saving; return v; }

	switch (s_device->state())
	{
	case GitHubDeviceLogin::State::Idle:
	case GitHubDeviceLogin::State::RequestingCode:
		v.phase = SIV::Phase::RequestingCode;
		break;
	case GitHubDeviceLogin::State::WaitingForUser:
	{
		const std::uint64_t now = SDL_GetTicks();
		if (!s_waitStamped) { s_waitStamped = true; s_waitStartMs = now; }
		const HE::Sc::DevicePrompt p = s_device->prompt();
		v.phase           = SIV::Phase::WaitingForUser;
		v.userCode        = p.userCode;
		v.verificationUri = p.verificationUri;
		v.secondsLeft     = p.expiresInSeconds - static_cast<int>((now - s_waitStartMs) / 1000);
		v.copied          = s_copiedAtMs != 0 && now - s_copiedAtMs < kCopiedShowMs;
		break;
	}
	case GitHubDeviceLogin::State::Granted:
		v.phase = SIV::Phase::Saving;
		break;
	case GitHubDeviceLogin::State::Denied:
		v.phase = SIV::Phase::Denied;
		v.error = s_device->error();
		break;
	case GitHubDeviceLogin::State::Expired:
		v.phase = SIV::Phase::Expired;
		v.error = s_device->error();
		break;
	case GitHubDeviceLogin::State::Cancelled:
		v.phase = SIV::Phase::Idle;   // nothing left to show
		break;
	case GitHubDeviceLogin::State::Failed:
		v.phase = SIV::Phase::Failed;
		v.error = s_device->error();
		break;
	}
	return v;
}

#ifdef HE_IMGUI_ENABLED
void handle(SIV::Action action, const SIV::View& v)
{
	switch (action)
	{
	case SIV::Action::None:
		break;
	case SIV::Action::Copy:
	case SIV::Action::CopyAndOpen:
		ImGui::SetClipboardText(v.userCode.c_str());
		s_copiedAtMs = SDL_GetTicks();
		// Only ever a github.com page: parseDeviceCodeResponse refuses anything else.
		if (action == SIV::Action::CopyAndOpen && !v.verificationUri.empty())
			SDL_OpenURL(v.verificationUri.c_str());
		break;
	case SIV::Action::Cancel:
	case SIV::Action::Close:
		endFlow();
		break;
	case SIV::Action::Retry:
		resetFlowView();
		if (s_device) s_device->start();
		break;
	}
}

SIV::Fonts fontsOf(const AppContext& ctx)
{
	SIV::Fonts f;
	f.heading = ctx.fontHeading;
	return f;
}
#endif

} // namespace

// ── Public ───────────────────────────────────────────────────────────────────

fs::path credentialRoot(const AppContext& ctx)
{
	if (ctx.projectLoaded && ctx.git && !ctx.git->projectRoot().empty())
		return ctx.git->projectRoot();
#ifdef _WIN32
	const char* home = std::getenv("USERPROFILE");
#else
	const char* home = std::getenv("HOME");
#endif
	if (home && *home) return fs::path(home);
	std::error_code ec;
	return fs::current_path(ec);
}

Account account()
{
	std::lock_guard<std::mutex> lk(s_mutex);
	return s_account;
}

std::string accountLogin()
{
	std::lock_guard<std::mutex> lk(s_mutex);
	return s_login;
}

std::string accountError()
{
	std::lock_guard<std::mutex> lk(s_mutex);
	return s_error;
}

std::uint64_t accountGeneration()
{
	std::lock_guard<std::mutex> lk(s_mutex);
	return s_generation;
}

void refreshAccount(const fs::path& root)
{
	if (startJob(Job::Probe, root))
	{
		s_checkedRoot = root;
		s_everChecked = true;
	}
}

void ensureChecked(const fs::path& root)
{
	if (!s_everChecked || s_checkedRoot != root) refreshAccount(root);
}

void signOut(const fs::path& root)
{
	if (startJob(Job::SignOut, root))
	{
		s_checkedRoot = root;
		s_everChecked = true;
	}
}

void startFlow(const fs::path& root)
{
	if (s_saving) return;
	if (s_device && s_device->running()) return;
	if (!s_device) s_device = std::make_unique<GitHubDeviceLogin>();
	s_flowRoot   = root;
	s_flowActive = true;
	resetFlowView();
	HE_LOG_INFO(Editor, "GitHub sign-in started (device flow).");
	s_device->start();
}

void endFlow()
{
	if (s_device && s_device->running()) s_device->cancel();
	s_flowActive = false;
	resetFlowView();
}

bool flowVisible()
{
	return currentView().phase != SIV::Phase::Idle;
}

bool drawInline(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	pump();
	const SIV::View v = currentView();
	if (v.phase == SIV::Phase::Idle) return false;
	handle(SIV::drawBody(v, fontsOf(ctx)), v);
	return true;
#else
	(void)ctx;
	return false;
#endif
}

void drawAccountRow(AppContext& ctx, bool inlineFlow)
{
#ifdef HE_IMGUI_ENABLED
	HE::Ed::Help::Scope helpScope("Source Control");
	pump();

	const fs::path root = credentialRoot(ctx);
	ensureChecked(root);

	const bool gitMissing = ctx.gitProbe && !ctx.gitProbe->gitFound;
	const bool busy       = s_workerRunning.load(std::memory_order_acquire) || flowVisible();

	ImGui::PushID("github-account");
	switch (account())
	{
	case Account::Unknown:
	case Account::Checking:
		ImGui::TextDisabled("Checking your GitHub sign-in\xe2\x80\xa6");
		break;

	case Account::SignedIn:
	{
		const std::string login = accountLogin();
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "Signed in to GitHub as %s.",
		                   login.c_str());
		ImGui::SameLine();
		ImGui::BeginDisabled(busy);
		if (EditorWidgets::button("Sign out")) signOut(root);
		ImGui::EndDisabled();
		break;
	}

	case Account::SignedOut:
	case Account::Rejected:
	{
		if (account() == Account::Rejected)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
			// Rejected covers "offline" too: currentUser cannot tell the two apart
			// for us, and its message says which it was.
			ImGui::TextWrapped("A GitHub token is stored, but GitHub did not confirm it: %s",
			                   accountError().c_str());
			ImGui::PopStyleColor();
		}
		else
		{
			ImGui::TextWrapped("Not signed in to GitHub. Signing in takes a code and your "
			                   "browser \xe2\x80\x94 no token to create by hand.");
		}
		ImGui::BeginDisabled(busy || gitMissing);
		if (EditorWidgets::primaryButton("Sign in with GitHub...", ImVec2(220.0f, 0.0f)))
		{
			if (inlineFlow) startFlow(root);
			else            requestOpen();
		}
		if (account() == Account::Rejected)
		{
			ImGui::SameLine();
			if (EditorWidgets::button("Sign out")) signOut(root);
		}
		ImGui::EndDisabled();
		if (gitMissing)
			ImGui::TextDisabled("Needs git: the sign-in is kept by git's credential helper.");
		break;
	}
	}

	// A failed sign-out says so here, whatever the account then turned out to be.
	if (account() != Account::Rejected)
	{
		const std::string err = accountError();
		if (!err.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
			ImGui::TextWrapped("%s", err.c_str());
			ImGui::PopStyleColor();
		}
	}
	ImGui::PopID();
#else
	(void)ctx;
	(void)inlineFlow;
#endif
}

void requestOpen() { s_openRequested = true; }

void Draw(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	pump();

	if (s_openRequested)
	{
		s_openRequested = false;
		if (!flowVisible()) startFlow(credentialRoot(ctx));
		ImGui::OpenPopup(kPopupId);
		s_modalOpen = true;
	}

	ImGui::SetNextWindowSize(ImVec2(kDialogWidth, 0.0f), ImGuiCond_Appearing);
	EditorWidgets::pinDialogToEditorWindow();
	if (!ImGui::BeginPopupModal(kPopupId, nullptr,
	                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
	{
		// Closed from outside (Escape, another root-level modal): stop polling
		// rather than keep a code alive nobody can see.
		if (s_modalOpen)
		{
			s_modalOpen = false;
			endFlow();
		}
		return;
	}

	{
		// Fixed column, as in the clone dialog: an auto-resizing popup that wraps
		// at its own edge makes its width its own input.
		EditorWidgets::WrapText wrap(kDialogWidth - ImGui::GetStyle().WindowPadding.x);
		if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
		ImGui::TextUnformatted("Sign in with GitHub");
		if (ctx.fontSubheading) ImGui::PopFont();
		ImGui::Separator();
		ImGui::Spacing();

		const SIV::View v = currentView();
		handle(SIV::drawBody(v, fontsOf(ctx)), v);
	}

	// Cancelled, closed, or ended from elsewhere: nothing left to show.
	if (!flowVisible())
	{
		s_modalOpen = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
#else
	(void)ctx;
#endif
}

void joinPendingWork()
{
	if (s_device)
	{
		s_device->cancel();
		s_device.reset();   // joins its poll thread
	}
	s_flowActive = false;
	if (s_worker.joinable()) s_worker.join();
}

} // namespace GitHubSignIn
