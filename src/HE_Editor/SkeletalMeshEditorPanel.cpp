#include "SkeletalMeshEditorPanel.h"
#include "EditorToolbar.h"   // shared toolbar strip

#include <cstdio>
#include <cstdint>
#include "EditorApplication.h"      // AppContext
#include "EditorAssetTypeCache.h"   // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"       // shared per-tab state map + lazy asset open
#include "EditorWidgets.h"          // asset drop slot + WrapText (text wraps, never runs off)
#include "EditorInput.h"            // pointer-device grammar (trackpad swipe vs mouse wheel)
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/AnimationPreview.h>
#include <Types/Enums.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
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

static AssetPanelState<State> s_states;

static State& stateFor(const std::string& path, AppContext& ctx)
{
	State& st = s_states[path];
	if (st.loaded || !ctx.contentManager) return st;

	st.meshId = openPanelAsset(ctx, path, st.name, st.relPath);
	st.loaded = true;
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
	return EditorAssetTypeCache::is(path, HE::AssetType::SkeletalMesh);
}

void forget(const std::string& assetPath) { s_states.forget(assetPath); }

void render(AppContext& ctx, const std::string& assetPath, const ImVec2& pos, const ImVec2& size)
{
	State& st = stateFor(assetPath, ctx);

	// A REAL host window pinned to the tab area, not a bare BeginChild: with no
	// window open, every ImGui call lands in the implicit "Debug" window — which
	// has a title bar and is user-movable, so the whole tab appeared inside a
	// draggable floating window. Same setup as ScriptEditorPanel.
	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(size, ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(0.0f, 0.0f));
	ImGui::Begin("##SkeletalMeshEditor", nullptr,
		ImGuiWindowFlags_NoTitleBar         | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove             | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoScrollbar        | ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoSavedSettings    | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoDocking);
	ImGui::PopStyleVar(2);

	const SkeletalMeshAsset* mesh = ctx.contentManager ? ctx.contentManager->getSkeletalMesh(st.meshId) : nullptr;
	if (!mesh)
	{
		// This tab is two narrow panes side by side, and what it prints into them
		// is asset names, joint names and whole sentences — all of them longer
		// than the column they land in. Left unwrapped, ImGui draws such a line
		// past the right edge and clips it there: the reader gets its opening
		// words and nothing that says the rest exists. The wrap position is per
		// window and BeginChild opens a fresh one, so it is pushed three times
		// here — once for this line, once inside each pane — and never around the
		// toolbar strip, which paints through the draw list and ignores it.
		{
			EditorWidgets::WrapText wrap;
			ImGui::TextDisabled("Skeletal mesh not loaded.");
		}
		ImGui::End();
		return;
	}

	// ── Toolbar ─────────────────────────────────────────────────────────────
	// The clip transport and the skeleton overlay were tucked in next to the drop
	// slot in the preview pane, where a slider and two controls fought over one
	// row. They are tools, so they belong on the strip.
	{
		namespace T = EditorToolbar;
		const AnimationClipAsset* barClip = (st.clipId != HE::UUID{} && ctx.contentManager)
			? ctx.contentManager->getAnimationClip(st.clipId) : nullptr;

		char joints[48];
		std::snprintf(joints, sizeof(joints), "%zu joint%s", mesh->skeleton.size(),
		              mesh->skeleton.size() == 1 ? "" : "s");

		T::Bar bar;
		bar.group();
		bar.readout(T::iconBone, st.name.c_str());
		bar.readout(nullptr, joints, T::kFgDim);
		bar.endGroup();

		bar.group();
		if (bar.item("##skelPlay", st.playing ? T::iconPause : T::iconPlay, nullptr,
		             st.playing, barClip != nullptr,
		             barClip ? (st.playing ? "Pause the clip" : "Play the clip")
		                     : "Drop an animation clip below to play one"))
		{
			st.playing = !st.playing;
		}
		bar.endGroup();

		bar.group();
		if (bar.item("##skelBones", T::iconBone, nullptr, st.showSkeleton, true,
		             "Draw the skeleton over the mesh"))
		{
			st.showSkeleton = !st.showSkeleton;
		}
		bar.endGroup();
	}

	// ── Left: bone hierarchy ────────────────────────────────────────────────
	const float leftW = std::max(220.0f, size.x * 0.28f);
	ImGui::BeginChild("##skelBoneTree", ImVec2(leftW, 0.0f), true);
	{
		// For the header line: the asset name plus a joint count, in a pane barely
		// 220 px wide. The tree below is unaffected either way — ImGui draws a
		// TreeNode's label without consulting the wrap position.
		EditorWidgets::WrapText wrap;

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
	}
	ImGui::EndChild();

	// ── Right: clip scrub controls + live preview ──────────────────────────
	ImGui::SameLine();
	ImGui::BeginChild("##skelPreviewPane", ImVec2(0.0f, 0.0f), true);
	{
		// For the drop slot's "(bind pose — drop a clip)" and the backend excuse
		// below it. The preview image and the orbit hit-area are laid out by hand
		// and touch no window text, so nothing here can be pushed out of place.
		EditorWidgets::WrapText wrap;

		// undo = false: the scrub clip is preview state on this tab, not a scene edit,
		// so it must not push an undo snapshot of the world.
		if (EditorWidgets::assetDropSlot(ctx, "Clip:", st.clipId, HE::AssetType::AnimationClip,
				"skelClipSlot", "(bind pose — drop a clip)", /*rejectNoun=*/nullptr,
				/*showClear=*/false, /*undo=*/false) == EditorWidgets::SlotAction::Assigned)
			st.clipTime = 0.0f;

		const AnimationClipAsset* clip = (st.clipId != HE::UUID{} && ctx.contentManager)
			? ctx.contentManager->getAnimationClip(st.clipId) : nullptr;
		if (clip)
		{
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::SliderFloat("##skelScrub", &st.clipTime, 0.0f,
			                   std::max(clip->duration, 0.01f), "%.2fs");
		}

		if (st.playing && clip && clip->duration > 0.0f)
			st.clipTime = std::fmod(st.clipTime + ImGui::GetIO().DeltaTime, clip->duration);

		std::vector<glm::mat4> boneMatrices; // empty = bind pose
		if (clip) AnimationPreview::evaluateClipPose(*mesh, *clip, st.clipTime, boneMatrices);

		ImGui::Separator();
		// The preview target matches the pane, so the render fills it edge to edge —
		// it used to be a centred square, which left a dead strip beside it in any
		// non-square pane.
		const ImVec2 av  = ImVec2(std::max(64.0f, ImGui::GetContentRegionAvail().x),
		                          std::max(64.0f, ImGui::GetContentRegionAvail().y));
		const ImVec2 org = ImGui::GetCursorScreenPos();

		void* tex = nullptr;
		if (ctx.renderer && ctx.contentManager)
			tex = ctx.renderer->RenderSkeletalPreview(*ctx.contentManager, st.meshId, boneMatrices,
				static_cast<uint32_t>(av.x), static_cast<uint32_t>(av.y),
				st.previewYaw, st.previewPitch, st.previewDist, st.showSkeleton);

		if (tex)
		{
			const bool flipY = (ctx.backend == HE::RendererBackend::OpenGL);
			ImGui::Image(reinterpret_cast<ImTextureID>(tex), av,
				flipY ? ImVec2(0, 1) : ImVec2(0, 0), flipY ? ImVec2(1, 0) : ImVec2(1, 1));
		}
		else
			ImGui::TextDisabled("(preview unavailable on this backend)");

		// Orbit interaction over the whole preview pane (same feel as Material's).
		ImGui::SetCursorScreenPos(org);
		// Right-drag orbits too: the main viewport steers with RMB, and that muscle
		// memory lands here. InvisibleButton only reacts to buttons it was told
		// about, so without the flag a right-drag never even activates the item.
		ImGui::InvisibleButton("##skelOrbit", ImVec2(std::max(av.x, 1.0f), std::max(av.y, 1.0f)),
			ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		// Claim the wheel over the preview so a swipe/zoom never also scrolls the pane.
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
		if (ImGui::IsItemActive() &&
		    (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Right)))
		{
			const ImVec2 md = ImGui::GetIO().MouseDelta;
			st.previewYaw   -= md.x * 0.01f;
			st.previewPitch  = std::clamp(st.previewPitch + md.y * 0.01f, -1.45f, 1.45f);
		}
		// Scroll = zoom on every pointer device — a 3D orbit view zooms far more
		// often than it orbits, and taking the bare scroll away for a swipe-orbit
		// (tried once) just made zooming feel broken on a pad. Orbiting is the
		// drags above; 2D canvases are where swipe pans.
		if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f)
			st.previewDist = std::clamp(st.previewDist - ImGui::GetIO().MouseWheel * 0.1f, 0.5f, 8.0f);
	}
	ImGui::EndChild();
	ImGui::End();
}

} // namespace SkeletalMeshEditorPanel
