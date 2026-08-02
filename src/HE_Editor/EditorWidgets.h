#pragma once
#include <Types/Enums.h>
#include <Types/UUID.h>
#include <imgui.h>
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
