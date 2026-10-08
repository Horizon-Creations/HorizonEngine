#pragma once

// ── "That one, there" ────────────────────────────────────────────────────────
// Drawing a pulsing outline around a named editor window. Written for the
// guided tour (which outlines the panel each step is about) and now shared with
// the documentation reader's "Show me" button, because pointing at a panel is
// the same act whether a tour or an article is doing the pointing.
//
// Deliberately by WINDOW NAME rather than a handle: the callers hold curriculum
// data and documentation topics, neither of which can own an ImGuiWindow*, and
// the name is the one identity a panel keeps across docking, closing and
// reopening.
//
// The three cases that make this less obvious than "draw a rectangle around
// w->Pos/w->Size" are spelled out at the implementation.

// Gated on the HEADER, not on HE_IMGUI_ENABLED: this is ImGui and nothing else,
// and the test target compiles the panels that use it without the editor's
// define (see EditorTheme.h for the same rule).
#if __has_include(<imgui.h>)

#include <imgui.h>

#include <string>
#include <vector>

namespace HE::Ed::Spotlight
{
	// Outline `name`'s window. `time` is a rising seconds clock (ImGui::GetTime())
	// and drives the pulse; `dimmed` draws the quiet, already-done variant.
	//
	// Returns false when there was nothing to outline — the window does not
	// exist, is closed, or sits in a hidden host. That is not a failure to
	// swallow: it is how a caller knows to say "the panel is closed" instead of
	// pointing confidently at nothing.
	bool outline(const char* name, float time, bool dimmed = false);

	// The rectangle outline() frames for `name`, in screen coordinates: the
	// whole dock slot (tab bar included) for a docked panel, the window rect for
	// a floating one. False in exactly the cases outline() draws nothing. Shared
	// so that whatever has to keep clear of a panel keeps clear of the box the
	// user SEES around it, tab bar and all. `viewport`, when given, receives the
	// OS window the panel lives in.
	bool panelRect(const char* name, ImVec2& pos, ImVec2& size,
	               ImGuiViewport** viewport = nullptr);

	struct Box { ImVec2 min, max; };

	// Where a floating card of `cardSize`, now at `cardPos`, should sit so that
	// it covers as little of `avoid` as possible while staying inside
	// [areaMin, areaMax] (inset by `margin`). The guided tour uses it so its card
	// never sits on the panel it is pointing at.
	//
	// Stays put when the card already covers none of `avoid`, and when no
	// candidate covers less than where it is now — a card that hops between two
	// equally bad spots is worse than one that stays. Otherwise it picks the spot
	// covering the least, and among equally clear spots the one closest to where
	// the card is, so it steps aside instead of flying across the editor.
	// Candidates are the area's corners and the four sides of every box (and of
	// all of them together), `gap` points away from the box.
	ImVec2 placeClearOf(ImVec2 cardPos, ImVec2 cardSize, ImVec2 areaMin, ImVec2 areaMax,
	                    const std::vector<Box>& avoid, float margin = 8.0f, float gap = 12.0f);

	// Area in square points that a card at `pos`/`size` shares with `avoid`.
	float coveredArea(ImVec2 pos, ImVec2 size, const std::vector<Box>& avoid);

	// A floating card that keeps itself off the panels it points at, frame after
	// frame: placeClearOf() plus the two things that make it bearable to use —
	// it glides instead of jumping, and a card the user dragged stays where they
	// put it until reset(). Call update() every frame BEFORE Begin(cardName); it
	// issues the SetNextWindowPos when the card has to move, and nothing else.
	// The card's previous-frame rect is what is tested, so the first frame ever
	// (no window yet) leaves the caller's default position alone.
	class KeepClear
	{
	public:
		// `panels` are window names as outline() takes them. Panels in another
		// OS window than the card, closed or hidden ones, are skipped.
		void update(const char* cardName, const std::vector<std::string>& panels, float dt);
		// A new subject: forget the user's drag and any glide in progress.
		void reset() { m_dragged = false; m_gliding = false; }
		bool userMoved() const { return m_dragged; }
		bool gliding() const { return m_gliding; }

	private:
		bool   m_dragged = false;
		bool   m_gliding = false;
		int    m_glideFrames = 0;
		ImVec2 m_to      = ImVec2(0.0f, 0.0f);
	};

	// The panel the user last clicked into, "###"-suffix stripped. Used by the
	// tour's "visit these panels" steps; here because it is the same question
	// about the same identity.
	const char* focusedPanel();
}

#endif // __has_include(<imgui.h>)
