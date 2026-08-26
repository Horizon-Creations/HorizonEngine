#pragma once
#include <Types/Enums.h>
#include <Types/UUID.h>
#include <imgui.h>
#include <cstddef>
#include <string>

struct AppContext;
struct SDL_Window;   // opaque — avoids pulling SDL into every panel that includes this

// ── Shared editor widgets ────────────────────────────────────────────────────
// The Content-Browser drop slot ("drag a .hasset onto this row to point the
// component/node at it") was spelled out ~20 times: in every Details-panel
// component section and in half the asset editors. The copies drifted — some
// took an undo snapshot when the slot was CLEARED and some did not, some logged
// a warning on a type mismatch and some swallowed it — so the same gesture
// behaved differently depending on which row the user dropped onto. This is
// that widget, once.
namespace EditorWidgets
{

// What a Content-Browser drag ("HE_ASSET_PATH" payload — an absolute path)
// resolved to. Null id = nothing was dropped, or what was dropped is not an
// asset of the wanted type.
struct AssetDrop
{
	HE::UUID    id;
	std::string relPath;   // content-root-relative path (what path-valued slots store)

	explicit operator bool() const { return id != HE::UUID{}; }
};

// Make the item submitted just above a Content-Browser drop target and resolve a
// drop on it: payload path → content-relative path → loadAsset → type check.
// Opens AND closes the ImGui drag-drop target itself, so callers only branch on
// the result. `rejectNoun` ("static mesh", "material", …) turns a rejected drop
// into the "dropped asset is not a X" warning; nullptr rejects silently.
AssetDrop acceptAssetDrop(AppContext& ctx, HE::AssetType want,
                          const char* rejectNoun = nullptr);

// What the user did to a slot this frame — the caller's cue to run its own
// follow-up (mark the component dirty, invalidate the material, …).
enum class SlotAction { None, Assigned, Cleared };

// `label` + a button showing the assigned asset's name + the drop target +
// (optionally) a Clear button. `idSuffix` scopes the ImGui ids, since a Details
// panel shows many slots in one window. Passing label = nullptr draws the
// button alone.
//
// CLICKING the button opens a searchable picker over every asset of `want` in
// the project — the same scan the Content Browser filters by type with. That
// exists because a drop needs something to drag FROM: the asset tabs (the
// HorizonCode class Viewport, the widget designer) fill the whole tab area and
// have no Content Browser beside them, which left every slot in them empty for
// good. Both routes end in the same assignment, undo snapshot included.
//
// Both the assign and the clear take an undo snapshot BEFORE mutating: the slot
// points a component at an asset, which is a scene edit. `undo = false` is for
// slots that hold panel-local preview state (a scrub clip is not a scene edit).
SlotAction assetDropSlot(AppContext& ctx, const char* label, HE::UUID& target,
                         HE::AssetType want, const char* idSuffix,
                         const char* emptyText  = "(none)",
                         const char* rejectNoun = nullptr,
                         bool        showClear  = false,
                         bool        undo       = true);

// ── Confirm / cancel buttons ─────────────────────────────────────────────────
// Every dialog answers the same question — "do it, or not?" — and until now the
// two answers were drawn identically, so which button commits and which backs
// out had to be read, not seen. Three verbs, styled once:
//
//   primaryButton  the accent-filled "do it" — one per dialog, the same blue
//                  the editor already uses for "armed"
//   dangerButton   the red "do it" for actions that remove or overwrite —
//                  block, restore, delete
//   cancelButton   the quiet way out: ghost-styled (border, no fill), because
//                  a cancel that competes with the confirm defeats the point
//
// One helper per verb rather than colour-pushing at every call site — the ban
// dialog, the restore dialog and the unsaved-changes prompt had each hand-rolled
// their own shade of the same idea, all slightly different.
//
// All three look their label up in the help table, like button() below — a
// dialog's confirm is the button most in need of a sentence, and styling it was
// no reason to lose that. Nothing happens where there is no entry.
bool primaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool dangerButton (const char* label, const ImVec2& size = ImVec2(0, 0));
bool cancelButton (const char* label, const ImVec2& size = ImVec2(0, 0));

// The counterpart for creation: a square, frame-height "+" — big enough to be
// a target, small enough to sit in a section header. The glyph alone carries
// the meaning; what it adds goes in the tooltip, not the label, which is what
// kept these buttons from being three different spellings of "+ Add".
bool addButton(const char* id, const char* tooltip = nullptr);

// The compact variants, for deletion that lives INSIDE content rather than at
// the foot of a dialog. Ghost-red on purpose — red text, fill only under the
// mouse: a list where every row ends in a filled red block has no warning
// colour left, but a row whose × turns out to be red on approach still says
// "this one bites" before the click.
bool dangerSmallButton(const char* label);
// Context-menu entries that delete: same red, menu ergonomics.
bool dangerMenuItem(const char* label, bool enabled = true);

// ── Labelled setting rows ────────────────────────────────────────────────────
// ImGui puts a widget's label to the RIGHT of the control, which works in a wide
// dialog and fails in the narrow panels the editor actually docks: the text runs
// past the panel's edge and is simply cut off. The escape used across the
// Details panel was to hide the label and bake the name into the value format
// ("Coverage: %.2f") — which reads oddly, cannot be styled, and disappears
// entirely on controls that have no format string.
//
// These draw the label on its own line ABOVE a control stretched to the
// available width, so nothing depends on how wide the panel happens to be. Each
// returns what its ImGui counterpart returns, and the CONTROL is the last item
// submitted — so IsItemActivated / IsItemDeactivatedAfterEdit (the Details
// panel's undo tracking) still refer to the right thing.
// `label` takes ImGui's usual "Display##id" form: everything before "##" is
// what the user reads, the whole string is what ImGui hashes. That matters here
// because CollapsingHeader — unlike TreeNode — does NOT open an id scope, so the
// four different "Speed##an/ab/sm/pa" rows in the Details panel would collide
// the moment their visible text matched.
namespace Row
{
	bool sliderFloat(const char* label, float* v, float min, float max,
	                 const char* fmt = "%.2f", ImGuiSliderFlags flags = 0);
	bool sliderInt(const char* label, int* v, int min, int max, const char* fmt = "%d");
	bool dragFloat(const char* label, float* v, float speed, float min = 0.0f,
	               float max = 0.0f, const char* fmt = "%.2f");
	bool dragFloat2(const char* label, float* v, float speed, float min = 0.0f,
	                float max = 0.0f, const char* fmt = "%.2f");
	bool dragFloat3(const char* label, float* v, float speed, float min = 0.0f,
	                float max = 0.0f, const char* fmt = "%.2f");
	bool dragFloat4(const char* label, float* v, float speed, float min = 0.0f,
	                float max = 0.0f, const char* fmt = "%.2f");
	bool dragInt(const char* label, int* v, float speed = 1.0f, int min = 0, int max = 0);
	bool inputInt(const char* label, int* v);
	bool combo(const char* label, int* v, const char* const items[], int count);
	// Zero-separated item list ("A\0B\0C\0"), matching ImGui's other Combo overload.
	bool comboZ(const char* label, int* v, const char* itemsSeparatedByZeros);
	bool colorEdit3(const char* label, float* rgb, ImGuiColorEditFlags flags = 0);
	bool colorEdit4(const char* label, float* rgba, ImGuiColorEditFlags flags = 0);
	bool inputText(const char* label, char* buf, size_t bufSize, ImGuiInputTextFlags flags = 0);
	// Read-only value line, laid out like the editable rows above it.
	void labelText(const char* label, const char* fmt, ...) IM_FMTARGS(2);
}

// An explanatory line under a control. Dimmed, and wrapped to the panel width
// rather than clipped — these are full sentences, and a hint the user can only
// read half of is worse than no hint at all.
void hint(const char* fmt, ...) IM_FMTARGS(1);

// ── Help tooltips ────────────────────────────────────────────────────────────
// "What is this control for?", answered from the table in EditorHelp.h.
//
// The Row helpers below call helpForLabel() themselves, so every labelled
// setting row in the editor gets its explanation from the registry with no call
// site involved: a component section pushes a HE::Ed::Help::Scope, and the rows
// inside it are looked up as "<component>/<label>". Controls drawn by hand ask
// by key instead.
//
// ── Why the tooltip is drawn LATE ────────────────────────────────────────────
// These do not draw anything where they are called; they remember what to show
// and drawQueuedHelp() puts it on screen at the end of the frame. That is not
// tidiness — ImGui::Begin (which BeginTooltip goes through) overwrites
// g.LastItemData with the tooltip window's own, and the Details panel reads
// exactly that immediately after the control it just submitted
// (IsItemActivated / IsItemDeactivatedAfterEdit is what commits an undo step).
// Drawing the tooltip in place would silently break every undo in the panel.
//
// A single queue also settles precedence for free: whichever hovered item
// asked last is the one under the mouse.

// Ask for help for the item that was just submitted, by the label the user can
// see ("Metallic", or the full "Density##fog" — both are tried). Returns true
// if an entry exists AND the item is hovered long enough to want a tooltip.
bool helpForLabel(const char* label);
// The same by explicit key ("viewport.play").
bool helpForKey(const char* key);

// A dimmed "?" after a control that carries the same entry — for the places
// where hovering the control itself is not discoverable enough (a section
// heading, a group of buttons). Draws on the current line.
void helpMarker(const char* key);

// Draw whatever was queued this frame, once, late — after every panel has been
// submitted. Returns the entry's documentation topic when the user pressed F1
// while it was showing, so the caller can open the manual there; null
// otherwise. (The caller does that rather than this, because the reader panel
// is an editor thing and these widgets are deliberately ImGui-only.)
const char* drawQueuedHelp();

// ImGui::Checkbox with the help lookup wired in. Same signature and same
// return; the only difference is that a checkbox drawn with this one can
// explain itself.
bool checkbox(const char* label, bool* v);

// ── The same for the two widgets a menu bar is made of ───────────────────────
// A menu is the densest help surface in the editor — thirty-odd verbs, each of
// which does something the word alone does not fully say ("Save All": every
// unsaved asset, then the scene). These are ImGui::MenuItem and ImGui::Button
// with the label looked up, so a menu covers itself by pushing one
// HE::Ed::Help::Scope and changing nothing else.
//
// The scope is what tells two identical labels apart: "Duplicate" is in the
// Edit menu and in the Outliner's context menu, and they are not the same
// sentence.
bool menuItem(const char* label, const char* shortcut = nullptr,
              bool selected = false, bool enabled = true);
bool button(const char* label, const ImVec2& size = ImVec2(0, 0));
// ImGui::Selectable with the lookup — the Content Browser's create menu and the
// Hub's lists are built from these rather than from menu items.
bool selectable(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0,
                const ImVec2& size = ImVec2(0, 0));
// And the compact one, for buttons that sit inside a table cell or a paragraph:
// the "Fix" in the tool-status table, the copy-this-command buttons in the
// build-tools dialog. Exactly the places where the button is too small to say
// what it does.
bool smallButton(const char* label);

// A section caption inside a component (Details panel) — "Surface",
// "Precipitation". Same role as ImGui::SeparatorText, but styled as a heading
// so the eye can find the group boundaries in a long component.
void subHeading(const char* text);

// ── Dialog placement ─────────────────────────────────────────────────────────
// Multi-viewport is on (ImGuiConfigFlags_ViewportsEnable), so ImGui hands any
// window whose rectangle does not fit inside the editor's OS window a *separate*
// top-level OS window. The window manager orders those independently: switching
// apps, or clicking the editor to bring it forward, buries the dialog behind the
// docked layout. It is then invisible while still being open — and a modal one
// keeps swallowing every click, so the editor looks frozen.
//
// Call this immediately before ImGui::BeginPopupModal() for any dialog that must
// stay reachable. It centres the dialog on the editor window and caps its size to
// the editor's work area, so the dialog always fits and ImGui keeps merging it
// into the main viewport — no second OS window exists, so there is nothing to
// fall behind. Content that no longer fits scrolls instead of growing past the
// editor. The cap also overrides a caller's own SetNextWindowSize/-Constraints,
// so those may stay exactly as they are; only the pin and the cap are added here.
//
// The position is pinned every frame, which is what a modal wants: it cannot be
// dragged out of the editor window and lost again. `minSize` is the floor an
// auto-sized dialog must not shrink below (clamped to the cap).
void pinDialogToEditorWindow(ImVec2 minSize = ImVec2(0.0f, 0.0f),
                             float  margin  = 48.0f);

// Same goal for a floating, movable panel: call right after a successful
// ImGui::Begin() to keep the *current* window inside the editor window. It can
// still be dragged anywhere within the editor — just not out of it, where it
// would get its own OS window and could be buried behind the docked layout.
void clampCurrentWindowToEditorWindow(float margin = 8.0f);

// Safety net for dialogs that ended up in their own OS window anyway — a plain
// window the user dragged out, or a dialog that stopped fitting because the
// editor window was shrunk. Call once per frame, after the platform windows have
// been updated: whenever the editor window comes forward, this puts the OS window
// of any *modal* popup back in front of it. Only modals are raised; raising an
// ordinary floating panel would steal the focus the user just gave the editor.
void raiseDetachedModals(SDL_Window* mainWindow);

// ── Text that wraps instead of running off the edge ──────────────────────────
// Editor panels are narrow and docked, and almost everything they display is a
// content path, an asset name, an error from a compiler or a sentence explaining
// a setting — all of them longer than the column they sit in. Without a wrap
// position ImGui draws such a line straight past the right edge and clips it, so
// the reader gets the beginning of a message and no indication that there is
// more; with a horizontal scrollbar they get the same thing behind a gesture
// nobody performs in a tool window. Both are the same bug wearing different
// clothes, and the answer to both is to wrap.
//
// Construct one inside the window whose contents should wrap. Scoped rather
// than a bare PushTextWrapPos/PopTextWrapPos pair because a panel with an early
// return between the two unbalances ImGui's stack, and the only thing that would
// tell us is an assertion in a build nobody runs with assertions on.
//
// ── The scope rule, which is not optional ────────────────────────────────────
// PopTextWrapPos acts on g.CurrentWindow, and ImGui::End() has by then switched
// that to the PARENT window — for a top-level panel, to the implicit debug
// window, whose wrap stack was never pushed. So a guard whose destructor runs
// AFTER the matching End() pops somebody else's stack, or underflows one.
//
// The question is never "which Begin am I under", it is "does my destructor run
// before the End". Read the call site and check, because the three ImGui pairs
// answer differently:
//
//     ImGui::Begin("Details");            // End() is unconditional, at THIS level
//     { EditorWidgets::WrapText wrap;     // -> needs its own block
//       ... }
//     ImGui::End();
//
//     if (ImGui::Begin("Details", &open)) // End() is still outside the body,
//     { EditorWidgets::WrapText wrap;     // so the body itself is enough
//       ... }
//     ImGui::End();
//
//     if (ImGui::BeginPopupModal(...))    // EndPopup() is INSIDE the body —
//     { ...                               // a guard at the top of it would
//       { EditorWidgets::WrapText wrap;   // outlive the popup, so it needs
//         ... }                           // its own block anyway
//       ImGui::EndPopup(); }
//
//     ImGui::BeginChild("##list", ...);   // EndChild() is at THIS level too:
//     { EditorWidgets::WrapText wrap;     // same as the first case
//       ... }
//     ImGui::EndChild();
//
// Only the `if (ImGui::Begin(...)) { }` form is safe with a bare guard at the
// top of the body. Everything else needs the extra braces. This paragraph got
// it wrong twice while the editor-wide pass was being written; if you are about
// to relax it, check imgui.cpp first.
//
// Deliberately NOT applied to the whole frame from one place: the wrap position
// lives on the window, so it has to be pushed inside each one.
struct WrapText
{
	// 0.0f = wrap at the window's content-region right edge, which is what a
	// docked panel wants. A positive value is an absolute x in WINDOW space, for
	// the cases where the window edge is not the right column: a fixed-width
	// auto-resizing popup (wrapping at the edge of a window that sizes itself to
	// its widest line feeds back into itself and the dialog shrinks frame over
	// frame), a table cell, or a row that must leave room for a button.
	//
	// A NEGATIVE value disables wrapping entirely — that is ImGui's rule, not a
	// convention this could change: CalcWrapWidthForPos returns 0 for a negative
	// position, and a wrap width of 0 means "do not wrap". It is spelled out here
	// because the -FLT_MIN "this far in from the right" idiom is right next door
	// in PushItemWidth/SetNextItemWidth, and borrowing it produces a guard that
	// looks correct and does nothing. To keep an N-pixel gutter, pass an absolute
	// column instead:
	//     ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - N
	explicit WrapText(float wrapPosX = 0.0f) { ImGui::PushTextWrapPos(wrapPosX); }
	~WrapText() { ImGui::PopTextWrapPos(); }

	WrapText(const WrapText&)            = delete;
	WrapText& operator=(const WrapText&) = delete;
};

} // namespace EditorWidgets
