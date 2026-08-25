#pragma once

struct AppContext;

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

	// Once per frame, on either screen. Draws nothing while closed.
	void draw(AppContext& ctx);

	// ── "Show me" ────────────────────────────────────────────────────────────
	// An article about a panel can point at it. Opening a panel means flipping a
	// file-static in EditorUI, so the reader is handed a function that does it
	// rather than reaching across for the flags: given an ImGui window name,
	// make sure it is on screen. Returns false if the name is not one it knows.
	using PanelOpener = bool (*)(const char* window);
	void setPanelOpener(PanelOpener opener);
}
