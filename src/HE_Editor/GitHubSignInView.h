#pragma once

// ─── "Sign in with GitHub": what the dialog looks like ───────────────────────
// The drawing half of the sign-in (GitHubSignIn.h is the state half). It takes
// a snapshot and returns what the user pressed, and knows nothing about
// threads, git or the AppContext — which is what lets tests/test_ui_shot.cpp
// draw the real thing headless.
//
// Gated on __has_include(<imgui.h>), not HE_IMGUI_ENABLED: the tests compile
// this file without that define.

#include <string>

struct ImFont;

namespace GitHubSignInView
{
	enum class Phase
	{
		Idle,             // nothing to show
		RequestingCode,   // asking GitHub for a code
		WaitingForUser,   // code on screen, polling
		Saving,           // token in hand: handing it to git, asking who it is
		SignedIn,         // login is known
		Denied,           // the user pressed Cancel on GitHub's page
		Expired,          // the code ran out
		Failed,           // error says why
	};

	struct View
	{
		Phase       phase = Phase::Idle;
		std::string userCode;          // "WDJB-MJHT"
		std::string verificationUri;   // "https://github.com/login/device"
		int         secondsLeft = 0;   // of the code's lifetime
		bool        copied      = false;  // the code went to the clipboard just now
		std::string login;             // SignedIn
		std::string error;             // Denied / Expired / Failed
	};

	enum class Action
	{
		None,
		Copy,        // put the code on the clipboard
		CopyAndOpen, // the same, then open the verification page
		Cancel,      // stop waiting
		Retry,       // a fresh code
		Close,       // done looking at the result
	};

	struct Fonts
	{
		ImFont* heading = nullptr;   // the code, drawn large
	};

	// "14:05", or "0:09". Negative counts as zero.
	std::string formatCountdown(int seconds);

	// The flow's body: wraps at the current content width. Draws nothing for
	// Idle. Pushes the "Source Control" help scope itself.
	Action drawBody(const View& view, const Fonts& fonts);
}
