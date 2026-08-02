#pragma once

struct AppContext;

// ── Interactive tutorial ──────────────────────────────────────────────────────
// The guided tour a first-time user is offered on the Project Hub and can reopen
// from Help ▸ Interactive Tutorial. The curriculum and every completion rule live
// in TutorialSteps.{h,cpp}; this file is only the ImGui skin over it plus the
// per-frame sampling of the live editor into a HE::tut::Signals snapshot.
//
// Progress is persisted in the global editor config (as a step *id*, not an
// index), so closing the window — or the editor — resumes where the user was.
namespace TutorialPanel
{
	// Editor-wide window flags the completion checks need. They are file statics
	// in EditorUI.cpp, so the caller passes them in rather than this panel
	// reaching for them.
	struct UiFlags
	{
		bool profilerOpen    = false;
		bool environmentOpen = false;
		bool exportOpen      = false;
		bool preferencesOpen = false;
		int  importDialogOpens = 0;  // monotonic: times the import dialog opened
		int  contentRootKind   = 0;  // Content Browser root: 0 Content, 1 Engine, 2 Source
	};

	// Project Hub, first start only: the welcome card offering the guided tour and
	// the tutorial sandbox project. Draws nothing once the user has answered it
	// (the answer is persisted). Creates the project itself when accepted — the
	// hub's own create form is not involved.
	void renderWelcome(AppContext& ctx);

	// Bring the welcome card back for one showing even though it was already
	// answered. This is what "Interactive Tutorial" does while no project is open:
	// there is nothing to give a tour of yet, so the answer is the sandbox offer.
	void showWelcome();

	// Per-frame, inside the full editor UI. Draws nothing while closed, but the
	// signal sampling still runs so a step whose action the user performs with the
	// window shut is not missed.
	void render(AppContext& ctx, float dt, const UiFlags& flags);

	// Help ▸ Interactive Tutorial. Opens at the persisted position; a finished tour
	// starts over.
	void open();
	bool isOpen();
}
