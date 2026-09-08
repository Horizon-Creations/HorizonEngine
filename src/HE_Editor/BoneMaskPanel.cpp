#include "BoneMaskPanel.h"
#include "EditorApplication.h"     // AppContext
#include "EditorAssetTypeCache.h"  // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"      // shared per-tab state map
#include "EditorToolbar.h"         // shared toolbar strip
#include "EditorHelp.h"            // Help::Scope — "Bone Mask Editor/<label>"
#include "EditorWidgets.h"

#include <BoneMask/BoneMask.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Diagnostics/Log.h>
#include <Types/Enums.h>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

struct PanelState
{
	bool         loaded = false;
	bool         dirty  = false;
	std::string  relPath;
	HE::UUID     assetId;
	HE::BoneMask mask;

	// The skeleton the tree is drawn from. Editor state, per tab, never written
	// into the asset — see the header. Empty is a legitimate state: the mask is
	// then edited as the plain name list it is.
	HE::UUID     referenceMeshId;

	std::string  lastSaveError;
};
AssetPanelState<PanelState> s_states;

bool saveState(PanelState& st, AppContext& ctx)
{
	if (!ctx.contentManager) return false;
	st.lastSaveError.clear();
	BoneMaskAsset* a = ctx.contentManager->getBoneMaskMutable(st.assetId);
	if (!a)
	{
		st.lastSaveError = "Not saved: this bone mask asset is no longer loaded.";
		return false;
	}
	a->json = HE::boneMaskToJson(st.mask);
	if (!ctx.contentManager->saveAsset(*a))
	{
		st.lastSaveError = "Not saved: the file could not be written.";
		return false;
	}
	st.dirty = false;
	return true;
}

// The mask's entry for a joint, or null. Entries are a list and not a map
// because the on-disk order is the author's order and a map would sort it.
HE::BoneMaskEntry* entryFor(HE::BoneMask& mask, const std::string& joint)
{
	for (auto& e : mask.entries)
		if (e.joint == joint) return &e;
	return nullptr;
}

void setEntry(HE::BoneMask& mask, const std::string& joint, float weight)
{
	if (auto* e = entryFor(mask, joint)) { e->weight = weight; return; }
	mask.entries.push_back({ joint, weight });
}

void eraseEntry(HE::BoneMask& mask, const std::string& joint)
{
	mask.entries.erase(
		std::remove_if(mask.entries.begin(), mask.entries.end(),
		               [&](const HE::BoneMaskEntry& e) { return e.joint == joint; }),
		mask.entries.end());
}

// Every joint at or below `root`, by index. A subtree action expands to explicit
// NAMES here, at authoring time — the asset itself has no "and its children"
// flag on purpose, because it holds no skeleton to resolve one against and two
// rigs would then read the same mask differently (see BoneMask.h).
void collectSubtree(const SkeletalMeshAsset& mesh, int root, std::vector<int>& out)
{
	out.push_back(root);
	for (size_t i = 0; i < mesh.skeleton.size(); ++i)
		if (mesh.skeleton[i].parent == root) collectSubtree(mesh, static_cast<int>(i), out);
}

// One joint row: a checkbox that decides whether the layer touches it at all,
// and — once it does — how strongly.
void jointRow(PanelState& st, const SkeletalMeshAsset& mesh, int index)
{
	// The scope is already open in render(); it is re-declared in every helper
	// that draws a control because the help audit reads SOURCE and cannot follow
	// a call. A nested Scope of the same name costs nothing and keeps the two
	// answers — what the tooltip resolves against, and what the audit checks —
	// the same one.
	HE::Ed::Help::Scope helpScope("Bone Mask Editor");

	const SkeletonJoint& j = mesh.skeleton[static_cast<size_t>(index)];
	HE::BoneMaskEntry*   e = entryFor(st.mask, j.name);

	ImGui::PushID(index);

	bool on = (e != nullptr);
	if (EditorWidgets::checkbox("Joint", &on))
	{
		if (on) setEntry(st.mask, j.name, 1.0f);
		else    eraseEntry(st.mask, j.name);
		st.dirty = true;
	}
	ImGui::SameLine();
	ImGui::TextUnformatted(j.name.c_str());

	if (e)
	{
		ImGui::SameLine();
		ImGui::SetNextItemWidth(110.0f);
		float w = e->weight;
		if (ImGui::SliderFloat("Weight", &w, 0.0f, 1.0f, "%.2f"))
		{
			// Re-found rather than kept: the slider is drawn after the checkbox
			// above may have rewritten the vector, and a pointer into a vector
			// that just grew is the oldest bug in this codebase.
			if (auto* cur = entryFor(st.mask, j.name)) { cur->weight = w; st.dirty = true; }
		}
	}

	ImGui::PopID();
}

// The tree, drawn from a reference skeleton. Recursive because a skeleton is,
// and because "select this arm and everything below it" is the action an author
// actually wants and it needs the hierarchy to exist on screen.
void jointTree(PanelState& st, const SkeletalMeshAsset& mesh, int index)
{
	HE::Ed::Help::Scope helpScope("Bone Mask Editor");   // see jointRow

	std::vector<int> children;
	for (size_t i = 0; i < mesh.skeleton.size(); ++i)
		if (mesh.skeleton[i].parent == index) children.push_back(static_cast<int>(i));

	const SkeletonJoint& j = mesh.skeleton[static_cast<size_t>(index)];
	ImGui::PushID(index);

	const bool open = children.empty()
		? false
		: ImGui::TreeNodeEx("##node", ImGuiTreeNodeFlags_DefaultOpen |
		                              ImGuiTreeNodeFlags_SpanAvailWidth |
		                              ImGuiTreeNodeFlags_AllowOverlap, "%s", j.name.c_str());
	if (children.empty())
	{
		ImGui::Indent();
		jointRow(st, mesh, index);
		ImGui::Unindent();
	}
	else
	{
		ImGui::SameLine();
		jointRow(st, mesh, index);
		// Subtree actions sit on the branch they act on rather than in a toolbar:
		// a humanoid upper body is twenty joints, and ticking twenty boxes to say
		// "the arm" is how a mask ends up half-authored.
		ImGui::SameLine();
		if (EditorWidgets::smallButton("Add Subtree"))
		{
			std::vector<int> sub; collectSubtree(mesh, index, sub);
			for (int s : sub) setEntry(st.mask, mesh.skeleton[static_cast<size_t>(s)].name, 1.0f);
			st.dirty = true;
		}
		ImGui::SameLine();
		if (EditorWidgets::smallButton("Clear Subtree"))
		{
			std::vector<int> sub; collectSubtree(mesh, index, sub);
			for (int s : sub) eraseEntry(st.mask, mesh.skeleton[static_cast<size_t>(s)].name);
			st.dirty = true;
		}
		if (open)
		{
			for (int c : children) jointTree(st, mesh, c);
			ImGui::TreePop();
		}
	}

	ImGui::PopID();
}

// Names in the mask that the reference skeleton does not have. Shown, and shown
// separately, because the runtime gives them weight 0 and a mask that silently
// stopped covering the joint it was written for looks exactly like a layer that
// stopped working for no reason.
void strayNames(PanelState& st, const SkeletalMeshAsset* mesh)
{
	HE::Ed::Help::Scope helpScope("Bone Mask Editor");   // see jointRow

	std::vector<std::string> strays;
	for (const auto& e : st.mask.entries)
	{
		bool found = false;
		if (mesh)
			for (const auto& j : mesh->skeleton)
				if (j.name == e.joint) { found = true; break; }
		if (!found) strays.push_back(e.joint);
	}
	if (strays.empty()) return;

	ImGui::Spacing();
	ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 180, 90, 255));
	ImGui::TextWrapped(mesh
		? "%zu name(s) in this mask are not in the reference skeleton. At run time they "
		  "weigh nothing — a mask never falls back to affecting everything."
		: "%zu name(s), and no reference skeleton to check them against.",
		strays.size());
	ImGui::PopStyleColor();

	for (const std::string& name : strays)
	{
		ImGui::PushID(name.c_str());
		ImGui::BulletText("%s", name.c_str());
		ImGui::SameLine();
		if (EditorWidgets::smallButton("Drop")) { eraseEntry(st.mask, name); st.dirty = true; }
		ImGui::PopID();
	}
}

} // namespace

bool BoneMaskPanel::isBoneMaskAsset(const std::string& path)
{ return EditorAssetTypeCache::is(path, HE::AssetType::BoneMask); }

bool BoneMaskPanel::isDirty(const std::string& path) { return s_states.dirty(path); }

bool BoneMaskPanel::reloadFromDisk(const std::string& assetPath)
{
	auto* st = s_states.find(assetPath);
	if (!st) return false;
	st->loaded = false;
	st->dirty  = false;
	return true;
}

void BoneMaskPanel::appendDirtyPaths(std::vector<std::string>& out)
{ s_states.appendDirtyPaths(out); }

bool BoneMaskPanel::save(AppContext& ctx, const std::string& path)
{
	PanelState* st = s_states.find(path);
	if (!st || !st->dirty) return true;   // "not mine" reads as success
	return saveState(*st, ctx);
}

void BoneMaskPanel::forget(const std::string& path) { s_states.forget(path); }

void BoneMaskPanel::render(AppContext& ctx, const std::string& assetPath,
                           const ImVec2& pos, const ImVec2& size)
{
	PanelState& st = s_states[assetPath];
	if (!st.loaded && ctx.contentManager)
	{
		st.relPath = ctx.contentManager->toContentRelativePath(assetPath);
		st.assetId = ctx.contentManager->loadAsset(st.relPath);
		st.mask    = HE::BoneMask{};
		if (const BoneMaskAsset* a = ctx.contentManager->getBoneMask(st.assetId))
			HE::boneMaskFromJson(a->json, st.mask);
		if (st.mask.name.empty())
			st.mask.name = std::filesystem::path(assetPath).stem().string();
		st.loaded = true;
	}

	HE::Ed::Help::Scope helpScope("Bone Mask Editor");
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin(("##bonemask_" + assetPath).c_str(), nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

	{
		namespace T = EditorToolbar;
		T::Bar bar;
		T::assetHeader(bar, st.mask.name.c_str(), T::iconBranch, st.dirty);
		bar.group();
		bar.readout(nullptr, "Bone Mask", T::kFgDim);
		bar.endGroup();
		if (T::saveButton(bar, true)) saveState(st, ctx);
	}
	if (!st.lastSaveError.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 80, 80, 255));
		ImGui::TextWrapped("%s", st.lastSaveError.c_str());
		ImGui::PopStyleColor();
	}

	{
		EditorWidgets::WrapText wrap;
		ImGui::TextDisabled("Which joints an animation layer is allowed to touch. A joint that is");
		ImGui::TextDisabled("not ticked weighs nothing — a mask is an allow-list, and one that");
		ImGui::TextDisabled("matches no joint means the layer does nothing, never everything.");
	}

	ImGui::Spacing();
	EditorWidgets::assetDropSlot(ctx, "Reference Skeleton", st.referenceMeshId,
		HE::AssetType::SkeletalMesh, "bmskref",
		"(none — drop a skeletal mesh here)", "skeletal mesh",
		/*showClear=*/true, /*undo=*/false);

	const SkeletalMeshAsset* mesh = (st.referenceMeshId == HE::UUID{} || !ctx.contentManager)
		? nullptr : ctx.contentManager->getSkeletalMesh(st.referenceMeshId);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	if (mesh && !mesh->skeleton.empty())
	{
		ImGui::Text("%zu of %zu joints in the mask",
		            st.mask.entries.size(), mesh->skeleton.size());
		ImGui::Spacing();
		// Every root. A glTF skeleton may have more than one, and a tree that
		// only walked the first would hide half a rig.
		for (size_t i = 0; i < mesh->skeleton.size(); ++i)
			if (mesh->skeleton[i].parent < 0) jointTree(st, *mesh, static_cast<int>(i));
	}
	else
	{
		EditorWidgets::WrapText wrap;
		ImGui::TextDisabled("Drop a skeletal mesh above to pick joints from its skeleton. The mesh");
		ImGui::TextDisabled("is only a reference for this editor and is not saved into the mask —");
		ImGui::TextDisabled("a mask works on every rig that spells its joints the same way.");
	}

	strayNames(st, mesh);

	ImGui::End();
}
