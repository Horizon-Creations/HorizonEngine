#pragma once
#include <Types/Enums.h>
#include <Types/UUID.h>
#include <imgui.h>
#include <string>

struct AppContext;

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

} // namespace EditorWidgets
