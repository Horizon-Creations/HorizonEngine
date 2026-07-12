#include "SkeletalMeshEditorPanel.h"
#include "EditorApplication.h" // AppContext
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <HorizonScene/AnimationPreview.h>
#include <Types/Enums.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace SkeletalMeshEditorPanel
{

struct State
{
	bool        loaded = false;
	std::string relPath;
	std::string name;
	HE::UUID    meshId;

	// Clip-scrub preview — pure UI state, never touches any ECS entity.
	HE::UUID clipId;
	float    clipTime     = 0.0f;
	bool     playing      = false;
	bool     showSkeleton = true;

	// Orbit camera, same feel as MaterialEditorPanel's preview.
	float previewYaw = 0.6f, previewPitch = 0.35f, previewDist = 2.2f;
};

static std::map<std::string, State> g_states;

static State& stateFor(const std::string& path, AppContext& ctx)
{
	State& st = g_states[path];
	if (st.loaded || !ctx.contentManager) return st;

	st.name = std::filesystem::path(path).filename().string();
	std::error_code ec;
	const std::string rel = std::filesystem::relative(
		path, ctx.contentManager->contentRoot(), ec).generic_string();
	st.relPath = ec ? path : rel;
	st.meshId  = ctx.contentManager->loadAsset(st.relPath);
	st.loaded  = true;
	return st;
}

// Recursive joint tree (SkeletalMeshAsset::skeleton is a flat array with
// per-joint parent indices, -1 = root) — a plain ImGui tree is the right tool
// here: bones form a strict hierarchy with no meaningful 2D layout, unlike the
// node graphs GraphEditor targets.
static void drawBoneNode(const SkeletalMeshAsset& mesh, const std::vector<std::vector<int>>& children, int idx)
{
	const SkeletonJoint& joint = mesh.skeleton[idx];
	const std::string label = joint.name.empty() ? ("Joint " + std::to_string(idx)) : joint.name;
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth
		| ImGuiTreeNodeFlags_DefaultOpen;
	if (children[idx].empty())
		flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Bullet;

	const bool open = ImGui::TreeNodeEx((label + "##joint" + std::to_string(idx)).c_str(), flags);
	if (open)
	{
		for (int c : children[idx]) drawBoneNode(mesh, children, c);
		if (!(flags & ImGuiTreeNodeFlags_NoTreePushOnOpen)) ImGui::TreePop();
	}
}

bool isSkeletalMeshAsset(const std::string& path)
{
	static std::map<std::string, bool> s_typeCache;
	if (auto it = s_typeCache.find(path); it != s_typeCache.end()) return it->second;
	HAsset::Reader r;
	const bool isSkel = r.open(path) &&
		r.assetType() == static_cast<uint16_t>(HE::AssetType::SkeletalMesh);
	s_typeCache[path] = isSkel;
	return isSkel;
}

void forget(const std::string& assetPath)
{
	g_states.erase(assetPath);
}

void render(AppContext& ctx, const std::string& assetPath, const ImVec2& pos, const ImVec2& size)
{
	State& st = stateFor(assetPath, ctx);

	ImGui::SetCursorScreenPos(pos);
	ImGui::BeginChild("##skelMeshEditorRoot", size, false);

	const SkeletalMeshAsset* mesh = ctx.contentManager ? ctx.contentManager->getSkeletalMesh(st.meshId) : nullptr;
	if (!mesh)
	{
		ImGui::TextDisabled("Skeletal mesh not loaded.");
		ImGui::EndChild();
		return;
	}

	// ── Left: bone hierarchy ────────────────────────────────────────────────
	const float leftW = std::max(220.0f, size.x * 0.28f);
	ImGui::BeginChild("##skelBoneTree", ImVec2(leftW, 0.0f), true);
	ImGui::TextDisabled("%s — %zu joint(s)", st.name.c_str(), mesh->skeleton.size());
	ImGui::Separator();
	if (mesh->skeleton.empty())
		ImGui::TextDisabled("(no skeleton data)");
	else
	{
		std::vector<std::vector<int>> children(mesh->skeleton.size());
		std::vector<int> roots;
		for (size_t i = 0; i < mesh->skeleton.size(); ++i)
		{
			const int32_t p = mesh->skeleton[i].parent;
			if (p >= 0 && static_cast<size_t>(p) < mesh->skeleton.size())
				children[static_cast<size_t>(p)].push_back(static_cast<int>(i));
			else
				roots.push_back(static_cast<int>(i));
		}
		for (int r : roots) drawBoneNode(*mesh, children, r);
	}
	ImGui::EndChild();

	// ── Right: clip scrub controls + live preview ──────────────────────────
	ImGui::SameLine();
	ImGui::BeginChild("##skelPreviewPane", ImVec2(0.0f, 0.0f), true);

	const AnimationClipAsset* clip = (st.clipId != HE::UUID{} && ctx.contentManager)
		? ctx.contentManager->getAnimationClip(st.clipId) : nullptr;
	const std::string clipLabel = clip ? clip->name
		: (st.clipId == HE::UUID{} ? "(bind pose — drop a clip)" : "(not loaded)");

	ImGui::TextUnformatted("Clip:");
	ImGui::SameLine();
	ImGui::Button((clipLabel + "##skelClipSlot").c_str());
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
		{
			std::error_code ec;
			const std::string rel = std::filesystem::relative(
				static_cast<const char*>(p->Data),
				ctx.contentManager ? ctx.contentManager->contentRoot() : "", ec).generic_string();
			if (!ec && !rel.empty() && rel.rfind("..", 0) != 0)
			{
				const HE::UUID id = ctx.contentManager->loadAsset(rel);
				if (id != HE::UUID{} && ctx.contentManager->getAnimationClip(id))
				{
					st.clipId   = id;
					st.clipTime = 0.0f;
				}
			}
		}
		ImGui::EndDragDropTarget();
	}
	if (clip)
	{
		ImGui::SameLine();
		if (ImGui::Button(st.playing ? "Pause##skelPlay" : "Play##skelPlay")) st.playing = !st.playing;
		ImGui::SetNextItemWidth(160.0f);
		ImGui::SliderFloat("Time##skelScrub", &st.clipTime, 0.0f, std::max(clip->duration, 0.01f), "%.2fs");
	}
	ImGui::SameLine();
	ImGui::Checkbox("Show Skeleton##skel", &st.showSkeleton);

	if (st.playing && clip && clip->duration > 0.0f)
		st.clipTime = std::fmod(st.clipTime + ImGui::GetIO().DeltaTime, clip->duration);

	std::vector<glm::mat4> boneMatrices; // empty = bind pose
	if (clip) AnimationPreview::evaluateClipPose(*mesh, *clip, st.clipTime, boneMatrices);

	ImGui::Separator();
	const ImVec2 av  = ImGui::GetContentRegionAvail();
	const float  px  = std::max(64.0f, std::min(av.x, av.y));
	const ImVec2 org = ImGui::GetCursorScreenPos();

	void* tex = nullptr;
	if (ctx.renderer && ctx.contentManager && px >= 32.0f)
		tex = ctx.renderer->RenderSkeletalPreview(*ctx.contentManager, st.meshId, boneMatrices,
			static_cast<uint32_t>(px), st.previewYaw, st.previewPitch, st.previewDist, st.showSkeleton);

	if (tex)
	{
		const bool flipY = (ctx.backend == HE::RendererBackend::OpenGL);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (av.x - px) * 0.5f);
		ImGui::Image(reinterpret_cast<ImTextureID>(tex), ImVec2(px, px),
			flipY ? ImVec2(0, 1) : ImVec2(0, 0), flipY ? ImVec2(1, 0) : ImVec2(1, 1));
	}
	else
		ImGui::TextDisabled("(preview unavailable on this backend)");

	// Orbit interaction over the whole preview pane (same feel as Material's).
	ImGui::SetCursorScreenPos(org);
	ImGui::InvisibleButton("##skelOrbit", ImVec2(std::max(av.x, 1.0f), std::max(av.y, 1.0f)));
	if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
	{
		const ImVec2 md = ImGui::GetIO().MouseDelta;
		st.previewYaw   -= md.x * 0.01f;
		st.previewPitch  = std::clamp(st.previewPitch + md.y * 0.01f, -1.45f, 1.45f);
	}
	if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f)
		st.previewDist = std::clamp(st.previewDist - ImGui::GetIO().MouseWheel * 0.1f, 0.5f, 8.0f);

	ImGui::EndChild();
	ImGui::EndChild();
}

} // namespace SkeletalMeshEditorPanel
