#pragma once

// ─── "Sign in with GitHub" (OAuth Device Flow) ───────────────────────────────
// The editor's way to a GitHub token without anyone creating one by hand: a
// code on screen, the user approves it on github.com, the token lands in git's
// credential helper (GitHubTokenStore — the same entry the token forms write).
// The state half; GitHubSignInView draws it.
//
// Three surfaces show the flow:
//   • its own modal, raised from Preferences ▸ Source Control (requestOpen);
//   • INLINE inside the Clone and Report Issue dialogs (drawInline) — those
//     are modals themselves, and a second root-level modal would close them;
//   • the account row (drawAccountRow): "Signed in as …  [Sign out]".
//
// Not per project on purpose: the keychain entry is the user's, so the login
// shown is the machine's. What IS per project is where git is asked from — a
// repo-local helper is only seen with that repository as working directory — so
// the account is re-checked whenever credentialRoot() changes. Nothing here
// goes into config.json.
//
// Threading: the device flow runs on GitHubDeviceLogin's own thread; storing,
// reading back, asking GitHub who it is and signing out run on one worker here.
// The UI thread only reads snapshots. The token passes through exactly once:
// takeToken() → the worker → the helper → wiped.

#include <cstdint>
#include <filesystem>
#include <string>

struct AppContext;

namespace GitHubSignIn
{
	// Where the sign-in is stored and read: the open project's folder (so a
	// repo-local helper is seen), otherwise the home folder.
	std::filesystem::path credentialRoot(const AppContext& ctx);

	// ── Who is signed in ─────────────────────────────────────────────────────
	enum class Account : std::uint8_t {
		Unknown,     // not asked yet
		Checking,    // a probe, a save or a sign-out is running
		SignedOut,   // nothing stored for github.com
		SignedIn,    // accountLogin() says who
		Rejected,    // a token is stored but GitHub did not accept it (accountError)
	};
	Account     account();
	std::string accountLogin();
	std::string accountError();
	// Bumped whenever the answer changes, so a dialog that probes on its own
	// (Report Issue) knows to look again after a sign-in or sign-out.
	std::uint64_t accountGeneration();

	// Ask git and GitHub again. ensureChecked only asks when `root` differs from
	// the last one asked about (a project was opened or closed).
	void refreshAccount(const std::filesystem::path& root);
	void ensureChecked(const std::filesystem::path& root);
	void signOut(const std::filesystem::path& root);

	// ── The flow ─────────────────────────────────────────────────────────────
	// Begin a sign-in that stores its token at `root`. Ignored while one runs.
	void startFlow(const std::filesystem::path& root);
	// Stop it and hide it (a dialog that showed it inline is closing). A token
	// already being saved is still saved.
	void endFlow();
	// Something to show: running, or a result not yet dismissed.
	bool flowVisible();

	// The flow's body inside another dialog. Returns false when there is nothing
	// to show (then the caller draws its own content).
	bool drawInline(AppContext& ctx);

	// "Signed in to GitHub as X [Sign out]" / "[Sign in with GitHub...]". With
	// `inlineFlow` the button starts the flow for drawInline instead of raising
	// the modal (for callers that are modals themselves).
	void drawAccountRow(AppContext& ctx, bool inlineFlow);

	// ── The standalone dialog ────────────────────────────────────────────────
	void requestOpen();
	// Every frame, over the Hub and the editor alike: also where a granted token
	// is picked up and finished workers are reaped.
	void Draw(AppContext& ctx);

	// Shutdown: cancel the flow and join every thread. A joinable std::thread
	// destroyed at exit terminates the process.
	void joinPendingWork();
}
