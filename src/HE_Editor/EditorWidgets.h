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
bool primaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool dangerButton (const char* label, const ImVec2& size = ImVec2(0, 0));
bool cancelButton (const char* label, const ImVec2& size = ImVec2(0, 0));

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

} // namespace EditorWidgets
