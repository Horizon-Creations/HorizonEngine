#include "EditorWidgets.h"
#include "EditorApplication.h"      // AppContext
#include "EditorUndo.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Diagnostics/Logger.h>
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
			Logger::Log(Logger::LogLevel::Warning,
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

} // namespace EditorWidgets
