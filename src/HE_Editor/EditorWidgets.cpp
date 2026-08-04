#include "EditorWidgets.h"
#include "EditorApplication.h"      // AppContext
#include "EditorUndo.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Diagnostics/Logger.h>
#include <imgui_internal.h>          // OpenPopupStack — which windows are modal
#include <SDL3/SDL.h>
#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <cstring>
#include <string>

namespace EditorWidgets
{
namespace
{
	// "Is this UUID a loaded asset of `want`, and what is it called?" — the typed
	// getters are exactly the per-slot checks the copies used (getStaticMesh(id)
	// != nullptr and friends), so which drops are accepted is unchanged. Types
	// with no typed getter fall back to the type index, which is what the graph
	// panels' copies already used.
	bool resolveAsset(ContentManager& cm, HE::UUID id, HE::AssetType want, std::string* nameOut)
	{
		const auto take = [&](const auto* asset)
		{
			if (asset && nameOut) *nameOut = asset->name;
			return asset != nullptr;
		};
		switch (want)
		{
			case HE::AssetType::StaticMesh:           return take(cm.getStaticMesh(id));
			case HE::AssetType::SkeletalMesh:         return take(cm.getSkeletalMesh(id));
			case HE::AssetType::Material:             return take(cm.getMaterial(id));
			case HE::AssetType::Audio:                return take(cm.getAudio(id));
			case HE::AssetType::AnimationClip:        return take(cm.getAnimationClip(id));
			case HE::AssetType::PropertyAnimClip:     return take(cm.getPropertyAnimClip(id));
			case HE::AssetType::ParticleSystem:       return take(cm.getParticleGraph(id));
			case HE::AssetType::AnimatorStateMachine: return take(cm.getAnimatorStateMachine(id));
			default:                                  return cm.assetType(id) == want;
		}
	}
}

AssetDrop acceptAssetDrop(AppContext& ctx, HE::AssetType want, const char* rejectNoun)
{
	AssetDrop drop;
	if (!ImGui::BeginDragDropTarget()) return drop;

	if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"); p && ctx.contentManager)
	{
		// The payload is the absolute path the Content Browser dragged. Assets are
		// addressed by content-root-relative path (or the reserved "Engine/" one);
		// anything outside both roots resolves to empty and is not an asset at all.
		const std::string rel = ctx.contentManager->toContentRelativePath(
			static_cast<const char*>(p->Data));
		const HE::UUID id = rel.empty() ? HE::UUID{} : ctx.contentManager->loadAsset(rel);
		if (id != HE::UUID{} && resolveAsset(*ctx.contentManager, id, want, nullptr))
		{
			drop.id      = id;
			drop.relPath = rel;
		}
		else if (rejectNoun)
		{
			HE_LOG_WARN(Editor, "%s",
				(std::string("Editor: dropped asset is not a ") + rejectNoun).c_str());
		}
	}
	ImGui::EndDragDropTarget();
	return drop;
}

SlotAction assetDropSlot(AppContext& ctx, const char* label, HE::UUID& target,
                         HE::AssetType want, const char* idSuffix,
                         const char* emptyText, const char* rejectNoun,
                         bool showClear, bool undo)
{
	// Slot text: the asset's name when it resolves, the caller's hint when the
	// slot is empty, and "(not loaded)" for a reference whose asset is gone —
	// the state that tells the user the scene points at a deleted/renamed asset.
	std::string shown;
	if (target == HE::UUID{})
		shown = emptyText;
	else if (!ctx.contentManager || !resolveAsset(*ctx.contentManager, target, want, &shown))
		shown = "(not loaded)";

	if (label)
	{
		ImGui::TextUnformatted(label);
		ImGui::SameLine();
	}
	ImGui::Button((shown + "##" + idSuffix).c_str());

	SlotAction action = SlotAction::None;
	if (const AssetDrop drop = acceptAssetDrop(ctx, want, rejectNoun))
	{
		if (undo && ctx.undoSys) ctx.undoSys->snapshotNow();
		target = drop.id;
		action = SlotAction::Assigned;
	}

	if (showClear && target != HE::UUID{})
	{
		ImGui::SameLine();
		if (ImGui::SmallButton((std::string("Clear##") + idSuffix).c_str()))
		{
			if (undo && ctx.undoSys) ctx.undoSys->snapshotNow();
			target = HE::UUID{};
			action = SlotAction::Cleared;
		}
	}
	return action;
}

// ── Dialog placement (see the header for why this exists) ────────────────────
void pinDialogToEditorWindow(ImVec2 minSize, float margin)
{
	const ImGuiViewport* vp = ImGui::GetMainViewport();

	// Never let a dialog grow past the editor window: the moment its rectangle
	// protrudes, ImGui gives it its own OS window and it can be buried behind us.
	// Constraints are applied after SetNextWindowSize and after auto-resize, so
	// this caps whatever the caller (or the content) asked for.
	const ImVec2 maxSize(std::max(240.0f, vp->WorkSize.x - margin * 2.0f),
	                     std::max(180.0f, vp->WorkSize.y - margin * 2.0f));

	// Min of 0 = "keep whatever you would have picked". The cap wins over the
	// caller's minimum: a dialog wider than the editor is the bug being fixed.
	ImGui::SetNextWindowSizeConstraints(
		ImVec2(std::min(minSize.x, maxSize.x), std::min(minSize.y, maxSize.y)), maxSize);
	ImGui::SetNextWindowPos(vp->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
}

void clampCurrentWindowToEditorWindow(float margin)
{
	const ImGuiViewport* vp   = ImGui::GetMainViewport();
	const ImVec2         pos  = ImGui::GetWindowPos();
	const ImVec2         size = ImGui::GetWindowSize();

	// max() guards the case where the panel is larger than the editor window: the
	// top-left corner then wins, so the title bar (and its close button) stays
	// reachable instead of the window being pushed off to the left.
	const float minX = vp->WorkPos.x + margin;
	const float minY = vp->WorkPos.y + margin;
	const float maxX = vp->WorkPos.x + std::max(margin, vp->WorkSize.x - size.x - margin);
	const float maxY = vp->WorkPos.y + std::max(margin, vp->WorkSize.y - size.y - margin);

	const ImVec2 want(std::clamp(pos.x, minX, maxX), std::clamp(pos.y, minY, maxY));
	if (want.x != pos.x || want.y != pos.y)
		ImGui::SetWindowPos(want);
}

void raiseDetachedModals(SDL_Window* mainWindow)
{
	if (!mainWindow) return;

	// Only act on the transition into focus. Raising every frame would fight the
	// user for the focus they may be trying to give another window of ours.
	static bool s_hadFocus = true;
	const bool  hasFocus   = (SDL_GetWindowFlags(mainWindow) & SDL_WINDOW_INPUT_FOCUS) != 0;
	const bool  cameForward = hasFocus && !s_hadFocus;
	s_hadFocus = hasFocus;
	if (!cameForward) return;

	ImGuiContext* g = ImGui::GetCurrentContext();
	if (!g) return;
	const ImGuiViewport* mainVp = ImGui::GetMainViewport();

	for (const ImGuiPopupData& popup : g->OpenPopupStack)
	{
		const ImGuiWindow* w = popup.Window;
		if (!w || !w->Active || (w->Flags & ImGuiWindowFlags_Modal) == 0) continue;
		const ImGuiViewport* vp = w->Viewport;
		if (!vp || vp == mainVp || vp->PlatformHandle == nullptr) continue;
		const SDL_WindowID id = static_cast<SDL_WindowID>(
			reinterpret_cast<intptr_t>(vp->PlatformHandle));
		if (SDL_Window* win = SDL_GetWindowFromID(id))
			SDL_RaiseWindow(win);
	}
}

} // namespace EditorWidgets
