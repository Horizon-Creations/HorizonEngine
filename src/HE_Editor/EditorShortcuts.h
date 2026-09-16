#pragma once
#include <string>
#include <string_view>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>   // ImGuiKeyChord

// ── The editor's keyboard shortcuts, as a table ──────────────────────────────
// Every shortcut used to be an IsKeyPressed(ImGuiKey_S) next to the code it
// triggered, with the menu row that advertised it printing "Ctrl+S" as a
// string literal of its own. That worked as long as nobody asked to change
// one: there was no list to show, no place to store a different key, and the
// label in the menu could not follow a rebind it did not know about.
//
// This is that list. An action has a stable id, the name the Shortcuts page
// shows, the scope it fires in, and the chord it ships with; the user's
// overrides live beside it and go to config.json as one string. The code that
// fires an action asks pressed("file.save") instead of reading the keyboard,
// and the menu that advertises it asks label("file.save") — so a rebind lands
// in both places at once, and the table is the only spelling of either.
//
// Two things are deliberately NOT in here:
//   * The shortcuts that belong to one editor (the HorizonCode graph, the
//     Material editor, the Sequencer, the code editor). Those panels own their
//     keys and their conflicts, and pulling them into a global table would be
//     the wrong scope for a key like Delete that means something different
//     in every one of them.
//   * The chords ImGui itself owns (text editing, Tab navigation).
namespace EditorShortcuts
{
	// Where an action listens. Two actions on the same chord only clash
	// inside one scope — W is Move in the viewport and nothing anywhere
	// else — and a Global action clashes with every scope, since it fires
	// regardless of where the pointer is.
	enum class Scope { Global, Viewport };

	struct Action
	{
		const char*   id;         // "file.save": the address code and config use
		const char*   label;      // "Save"
		const char*   category;   // the heading it sits under on the Shortcuts page
		Scope         scope;
		ImGuiKeyChord defaultChord;
		// The native macOS menu bar carries this action as a key equivalent,
		// and the OS delivers the keystroke to the menu before SDL sees it. A
		// different chord bound here still fires on macOS; the DEFAULT one keeps
		// working through the menu, so the page says so rather than offering
		// a change that only half applies.
		bool          nativeOnMac = false;
		// Fires even while a text field has the keyboard (function keys and
		// the like, which cannot be typed). Everything else is suppressed
		// while typing, so a chord can never eat a character.
		bool          whileTyping = false;
	};

	// The whole table, in the order the Shortcuts page lists it.
	const std::vector<Action>& actions();

	// Null for an id that is not in the table.
	const Action* find(std::string_view id);

	// The chord in force: the user's override, or the default. ImGuiKey_None
	// for an action the user unbound.
	ImGuiKeyChord chord(std::string_view id);

	// Rebind. ImGuiKey_None unbinds; the default chord clears the override.
	void setChord(std::string_view id, ImGuiKeyChord chord);
	void reset(std::string_view id);
	void resetAll();
	bool isDefault(std::string_view id);

	// Was the action's chord pressed this frame? Modifiers must match the
	// chord EXACTLY (Ctrl+S does not fire on Ctrl+Shift+S), with one
	// deliberate looseness: Ctrl in a chord is satisfied by either Ctrl or
	// Cmd, which is what every shortcut in the editor has always accepted.
	// Suppressed while a text field has the keyboard unless the action says
	// otherwise. Needs an ImGui frame; the caller decides the scope (hovered
	// viewport, active tab) — this only answers for the keys.
	bool pressed(std::string_view id);

	// What the menus print: "Ctrl+S", "Cmd+S" on macOS, "" when unbound.
	std::string label(std::string_view id);
	std::string chordLabel(ImGuiKeyChord chord);

	// Every OTHER action listening for the same chord where both could fire:
	// same scope, or either of them Global. What the page paints red.
	std::vector<const Action*> conflicts(std::string_view id);

	// ── Persistence ─────────────────────────────────────────────────────────
	// The overrides as one config string — "file.save=Ctrl+Shift+S;edit.undo="
	// — and back. Only differences from the defaults are written, so an
	// action whose default changes in a later build picks the new default up
	// unless the user had rebound it. Unknown ids in the string are dropped.
	std::string encode();
	void        decode(std::string_view text);

	// Chord ⇄ the portable spelling above. Key names are ImGui's own
	// ("S", "F11", "Keypad7", "GraveAccent", "Comma"); modifiers are always
	// written as Ctrl/Shift/Alt/Super regardless of platform, so a config
	// file moves between a Mac and a PC and means the same keys.
	// chordFromText returns ImGuiKey_None for anything it cannot read.
	std::string   chordToText(ImGuiKeyChord chord);
	ImGuiKeyChord chordFromText(std::string_view text);

	// The Shortcuts page is taking the next keystroke as a binding: nothing
	// may fire on it. Called by the page on every frame a capture is armed;
	// pressed() answers false while the last call is at most one frame old.
	// A frame stamp rather than a flag on purpose — a capture the page never
	// gets to close (the user switches tabs mid-capture, the page is no
	// longer drawn) would otherwise leave every shortcut in the editor dead.
	void noteCapturing();
	bool capturingNow();

	// The chord the keyboard is pressing right now, for the Shortcuts page's
	// capture: the first non-modifier key that went down this frame, with the
	// modifiers held alongside it; ImGuiKey_None while only modifiers (or
	// nothing) are down. Mouse buttons and gamepad inputs are never chords.
	ImGuiKeyChord captureChord();
}
#endif // HE_IMGUI_ENABLED
