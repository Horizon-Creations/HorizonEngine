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

#ifdef HE_IMGUI_ENABLED

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

	// The panel the user last clicked into, "###"-suffix stripped. Used by the
	// tour's "visit these panels" steps; here because it is the same question
	// about the same identity.
	const char* focusedPanel();
}

#endif // HE_IMGUI_ENABLED
