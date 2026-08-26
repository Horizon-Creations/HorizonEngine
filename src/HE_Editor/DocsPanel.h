#pragma once

struct AppContext;
class  IRenderer;

// ── Help ▸ Documentation, inside the editor ──────────────────────────────────
// The reader over the shipped documentation bundle (see DocsLibrary.h): a
// searchable manual in a panel, rather than a link that sends the user to a
// browser and loses whatever they were in the middle of.
//
// Everything about WHAT is shown lives in DocsLibrary; this is the window.
//
// Drawn from EditorUI::render for BOTH screens — the Project Hub as well as the
// editor. That is not symmetry for its own sake: "how do I start a project" is
// the one question the user has before a project exists, and the Hub is exactly
// where they are when they ask it.
namespace DocsPanel
{
	// Help ▸ Documentation. Opens at whatever was last read, or at the manual's
	// first page the first time.
	void open();
	// Open at a specific topic ("editor#play-mode") — what F1 over a control and
	// a tooltip's "Learn more" do. An unknown topic opens the reader anyway,
	// rather than doing nothing at all in response to a keypress.
	void openTopic(const char* topic);
	// Open with the search box already filled in and focused.
	void openSearch(const char* query);
	void close();
	bool isOpen();

	// True when one of the three calls above ran during THIS ImGui frame.
	//
	// F1 has two handlers, and they meet in one frame: a node's hover tooltip
	// consumes it inline while the graph canvas is being drawn, and the editor
	// has a global F1 at the end of the frame that toggles the reader. Without
	// this, opening the manual at a node's entry was immediately followed by the
	// toggle seeing the same still-pressed key and closing it again — the one
	// interaction this whole feature exists for, doing nothing.
	bool openedThisFrame();

	// True while the sidebar is still allowed to force its category groups
	// open — the short window after a navigation, when the group holding the
	// section just opened unfolds itself and the others close.
	//
	// Outside that window the groups belong to the user. Forcing them EVERY
	// frame (which is what this did at first) is the same call and a completely
	// different control: a click flips the group, the next frame sets it back,
	// and it cannot be opened by hand at all.
	bool navigatingGroups();

	// ── What the reader needs from the editor ────────────────────────────────
	// Four fonts and a renderer — not an AppContext. The panel used to take one,
	// which meant it could only be drawn by something that had a project, a
	// world and a GPU behind it; and that in turn meant the reader could not be
	// rendered headless, which is the only way anyone was ever going to LOOK at
	// it (tests/test_ui_shot.cpp). A narrow struct costs one adapter and buys
	// that, the same trade TutorialPanel::UiFlags makes.
	//
	// A null renderer is allowed and means "no figures" — the screenshots are
	// skipped, everything else draws.
	struct Host
	{
		void*      fontBody       = nullptr;   // ImFont*, kept opaque so this
		void*      fontSubheading = nullptr;   // header stays ImGui-free
		void*      fontHeading    = nullptr;
		void*      fontCode       = nullptr;
		IRenderer* renderer       = nullptr;
	};

	// Once per frame, on either screen. Draws nothing while closed.
	void draw(const Host& host);
	// The adapter every call site in the editor uses.
	void draw(AppContext& ctx);

	// ── "Show me" ────────────────────────────────────────────────────────────
	// An article about a panel can point at it. Opening a panel means flipping a
	// file-static in EditorUI, so the reader is handed a function that does it
	// rather than reaching across for the flags: given an ImGui window name,
	// make sure it is on screen. Returns false if the name is not one it knows.
	using PanelOpener = bool (*)(const char* window);
	void setPanelOpener(PanelOpener opener);
}
