#include "SkeletalMeshEditorPanel.h"
#include "EditorToolbar.h"   // shared toolbar strip

#include <cstdio>
#include <cstdint>
#include "EditorApplication.h"      // AppContext
#include "EditorAssetTypeCache.h"   // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"       // shared per-tab state map + lazy asset open
#include "EditorHelp.h"             // "Mesh Viewer/<label>" scope for the tooltips
#include "EditorWidgets.h"          // asset drop slot + WrapText (text wraps, never runs off)
#include "EditorInput.h"            // pointer-device grammar (trackpad swipe vs mouse wheel)
#include "UITimelineMath.h"         // seconds ⇄ pixels, shared with the UI designer's strip
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/AnimationPreview.h>
#include <HorizonScene/RootMotion.h>
#include <Diagnostics/Log.h>
#include <Types/Enums.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
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
	bool     showRootPath = true;

	// Which notify the rows under the lane edit. -1 = none. An index rather than a
	// pointer or a name: notifies are a plain vector with no identity of their own,
	// and two of them may legitimately carry the same name.
	int selectedNotify = -1;

	// The root-motion path, cached. It depends on nothing this panel edits except
	// the per-clip switch, so recomputing it every frame would be sixty-odd clip
	// samplings a second for a picture that cannot have changed.
	HE::UUID               pathClipId;
	bool                   pathHadRootMotion = false;
	std::vector<glm::vec3> path;

	// Orbit camera, same feel as MaterialEditorPanel's preview.
	float previewYaw = 0.6f, previewPitch = 0.35f, previewDist = 2.2f;
};

static AssetPanelState<State> s_states;

// Clips with unsaved notify / root-motion edits, path → id. Keyed by the clip
// because that is the asset being edited; see the note in the header. The id is
// carried along so save() does not have to turn a path back into a UUID — the
// panel had one in its hand when it marked the clip dirty.
static std::map<std::string, HE::UUID> s_dirtyClips;

static State& stateFor(const std::string& path, AppContext& ctx)
{
	State& st = s_states[path];
	if (st.loaded || !ctx.contentManager) return st;

	st.meshId = openPanelAsset(ctx, path, st.name, st.relPath);
	st.loaded = true;
	return st;
}

// The loaded clip IS the edit buffer (see ContentManager::getAnimationClipMutable),
// so "dirty" is a note about a file, not a second copy of the data.
static void markClipDirty(const AnimationClipAsset& clip)
{
	// clip.path is a string owned by the asset vector; copying it here is not
	// tidiness — the next loadAsset moves that vector and the string with it.
	s_dirtyClips[clip.path] = clip.id;
}

bool isDirty(const std::string& assetPath) { return s_dirtyClips.count(assetPath) != 0; }

void appendDirtyPaths(std::vector<std::string>& out)
{
	for (const auto& [path, id] : s_dirtyClips) out.push_back(path);
}

bool save(AppContext& ctx, const std::string& assetPath)
{
	// Every panel's save() answers true for a path it is not holding — that is
	// what lets EditorUI::saveAsset ask all of them without a path→panel map.
	const auto it = s_dirtyClips.find(assetPath);
	if (it == s_dirtyClips.end()) return true;
	if (!ctx.contentManager) return false;

	AnimationClipAsset* clip = ctx.contentManager->getAnimationClipMutable(it->second);
	if (!clip) return false;
	if (!ctx.contentManager->saveAsset(*clip)) return false;

	// Erased only after the write reported success: a failed save that cleared
	// the flag would let the editor quit over edits it never wrote.
	s_dirtyClips.erase(assetPath);
	HE_LOG_INFO(Editor, "%s", ("SkeletalMeshEditor: saved clip '" + assetPath + "'").c_str());
	return true;
}

std::string dirtyClipForTab(const std::string& tabPath)
{
	const State* st = s_states.find(tabPath);
	if (!st || st->clipId == HE::UUID{}) return {};
	// By id rather than by remembering the path on the State: a rename moves the
	// path and the id is what both maps already agree on.
	for (const auto& [path, id] : s_dirtyClips)
		if (id == st->clipId) return path;
	return {};
}

void forget(const std::string& assetPath)
{
	s_states.forget(assetPath);
	// Also as a CLIP path: this is the "the asset is gone" route, and pending
	// edits to a deleted clip are edits to nothing. Keeping them would let Save
	// All write the file back and quietly undo the deletion.
	s_dirtyClips.erase(assetPath);
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

// ── The notify lane ──────────────────────────────────────────────────────────
// A ruler and one row of events, drawn by hand: a notify is a moment on a line
// and a notify STATE is a span of it, and no ImGui widget says either. The
// arithmetic underneath is UITimelineView, shared with the UI designer's strip,
// at zoom 1 — the whole clip spans the lane, so there is no scroll to get lost in
// and no zoom control to explain.
namespace
{
constexpr ImU32 kLaneBg     = IM_COL32( 28,  26,  24, 255);
constexpr ImU32 kLaneLine   = IM_COL32( 60,  56,  51, 255);
constexpr ImU32 kNotify     = IM_COL32(230, 176,  86, 255);   // EditorToolbar::kWarn
constexpr ImU32 kNotifySel  = IM_COL32(255, 252, 245, 255);
constexpr ImU32 kStateFill  = IM_COL32(230, 176,  86,  70);
constexpr ImU32 kPlayhead   = IM_COL32(255, 250, 240, 190);
constexpr ImU32 kPathLine   = IM_COL32(120, 200, 255, 220);
constexpr float kHitPx      = 6.0f;

// The diamond that marks one instant on the lane.
void diamond(ImDrawList* dl, const ImVec2& c, float r, ImU32 col)
{
	const ImVec2 p[4] = { { c.x, c.y - r }, { c.x + r, c.y }, { c.x, c.y + r }, { c.x - r, c.y } };
	dl->AddConvexPolyFilled(p, 4, col);
}

// Which notify the mouse is over, or -1. Walked backwards so the LAST drawn (and
// therefore topmost) of two overlapping events is the one that answers.
int notifyAt(const AnimationClipAsset& clip, const HE::Ed::UITimelineView& view,
             float mouseX, float laneY, float laneH, float mouseY)
{
	if (mouseY < laneY || mouseY > laneY + laneH) return -1;
	for (int i = static_cast<int>(clip.notifies.size()) - 1; i >= 0; --i)
	{
		const AnimationNotify& n = clip.notifies[static_cast<size_t>(i)];
		const float x0 = view.xOf(n.time);
		const float x1 = view.xOf(n.time + std::max(n.duration, 0.0f));
		if (mouseX >= x0 - kHitPx && mouseX <= x1 + kHitPx) return i;
	}
	return -1;
}
} // namespace

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
		// The dirty asset is the CLIP, and the clip is what this bar's Save writes.
		const bool clipDirty = barClip != nullptr && isDirty(barClip->path);
		// Its path, copied before anything below can invalidate the asset vector.
		const std::string clipPath = barClip ? barClip->path : std::string();

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
		if (bar.item("##skelRootPath", T::iconBranch, nullptr, st.showRootPath, barClip != nullptr,
		             "Show where this clip's root motion travels", "anim.root-path"))
		{
			st.showRootPath = !st.showRootPath;
		}
		bar.endGroup();

		// Only the clip can be dirty here, and only when one is loaded — the mesh
		// itself is still view-only in this tab.
		bar.rightGroup(bar.iconGroupWidth(1));
		if (bar.item("##skelSaveClip", T::iconSave, nullptr, false, clipDirty,
		             "Save the clip's notifies and root-motion switch",
		             "anim.clip-save"))
		{
			save(ctx, clipPath);
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
		HE::Ed::Help::Scope helpScope("Mesh Viewer");

		// undo = false: the scrub clip is preview state on this tab, not a scene edit,
		// so it must not push an undo snapshot of the world.
		if (EditorWidgets::assetDropSlot(ctx, "Clip:", st.clipId, HE::AssetType::AnimationClip,
				"skelClipSlot", "(bind pose — drop a clip)", /*rejectNoun=*/nullptr,
				/*showClear=*/false, /*undo=*/false) == EditorWidgets::SlotAction::Assigned)
		{
			st.clipTime       = 0.0f;
			st.selectedNotify = -1;
		}
		// The slot draws its label and then its button; the lookup lands on the
		// button, which is the thing being pointed at anyway.
		EditorWidgets::helpForLabel("Clip:");

		// Fetched AFTER the slot, never before and never cached on the State: the
		// picker inside assetDropSlot can load an asset, and a load moves the dense
		// vector every clip pointer points into.
		AnimationClipAsset* clip = (st.clipId != HE::UUID{} && ctx.contentManager)
			? ctx.contentManager->getAnimationClipMutable(st.clipId) : nullptr;
		if (clip)
		{
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::SliderFloat("##skelScrub", &st.clipTime, 0.0f,
			                   std::max(clip->duration, 0.01f), "%.2fs");

			// The per-clip half of root motion. The entity's RootMotionComponent
			// says whether it acts on the motion; this says whether the clip is
			// meant to carry any at all — an idle with a pinned root and a roll
			// should not need the entity to be reconfigured between them.
			if (EditorWidgets::checkbox("Root Motion", &clip->hasRootMotion))
				markClipDirty(*clip);
		}

		if (st.playing && clip && clip->duration > 0.0f)
			st.clipTime = std::fmod(st.clipTime + ImGui::GetIO().DeltaTime, clip->duration);

		// ── Notify timeline ─────────────────────────────────────────────────
		if (clip)
		{
			ImGui::Spacing();
			const float laneW  = std::max(64.0f, ImGui::GetContentRegionAvail().x);
			const float rulerH = ImGui::GetTextLineHeight();
			const float laneH  = ImGui::GetTextLineHeightWithSpacing() * 1.3f;
			const ImVec2 org   = ImGui::GetCursorScreenPos();

			ImGui::InvisibleButton("##notifyLane", ImVec2(laneW, rulerH + laneH),
				ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
			const bool laneHovered = ImGui::IsItemHovered();
			EditorWidgets::helpForKey("anim.notify-lane");

			HE::Ed::UITimelineView view;
			view.laneX    = org.x;
			view.laneW    = laneW;
			view.duration = std::max(clip->duration, 0.01f);
			view.zoom     = 1.0f;
			view.scroll   = 0.0f;

			ImDrawList* dl = ImGui::GetWindowDrawList();
			const float laneY = org.y + rulerH;
			dl->AddRectFilled(ImVec2(org.x, laneY), ImVec2(org.x + laneW, laneY + laneH), kLaneBg, 3.0f);

			// Ruler: the 1-2-5 ladder, so consecutive labels stand far enough apart
			// to read at any lane width.
			const float step = HE::Ed::uiTimelineTickStep(view.pixelsPerSecond());
			for (float t = 0.0f; t <= view.duration + 1e-4f; t += step)
			{
				const float x = view.xOf(t);
				dl->AddLine(ImVec2(x, org.y + rulerH * 0.55f), ImVec2(x, laneY), kLaneLine);
				char lbl[24];
				std::snprintf(lbl, sizeof(lbl), "%.2fs", t);
				dl->AddText(ImVec2(x + 2.0f, org.y), EditorToolbar::kFgDim, lbl);
			}

			// The events themselves. A state is a bar (it occupies time), a notify
			// a diamond (it does not) — and a state gets its diamond too, at its
			// Begin, so "where does it start" is answered the same way for both.
			const int selected = st.selectedNotify;
			for (int i = 0; i < static_cast<int>(clip->notifies.size()); ++i)
			{
				const AnimationNotify& n = clip->notifies[static_cast<size_t>(i)];
				const bool  sel = (i == selected);
				const float x0  = view.xOf(n.time);
				if (n.duration > 0.0f)
				{
					// Clamped the way the firing walk clamps it: a state authored
					// past the end of its own clip ends WITH the clip.
					const float x1 = view.xOf(std::min(n.time + n.duration, view.duration));
					dl->AddRectFilled(ImVec2(x0, laneY + 3.0f), ImVec2(x1, laneY + laneH - 3.0f),
					                  kStateFill, 2.0f);
					dl->AddRect(ImVec2(x0, laneY + 3.0f), ImVec2(x1, laneY + laneH - 3.0f),
					            sel ? kNotifySel : kNotify, 2.0f);
				}
				diamond(dl, ImVec2(x0, laneY + laneH * 0.5f), laneH * 0.24f, sel ? kNotifySel : kNotify);
			}

			// Playhead last, over everything: it is the one line that has to stay
			// findable in a lane full of events.
			const float px = view.xOf(std::clamp(st.clipTime, 0.0f, view.duration));
			dl->AddLine(ImVec2(px, org.y), ImVec2(px, laneY + laneH), kPlayhead, 1.5f);

			// ── Lane interaction ────────────────────────────────────────────
			const ImVec2 mouse = ImGui::GetIO().MousePos;
			// Right-click first, and the left branch as an `else`: a right press
			// activates the item too (the button was told about both), so without
			// the order the menu would also scrub the playhead out from under it.
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
			{
				const int hit = notifyAt(*clip, view, mouse.x, laneY, laneH, mouse.y);
				if (hit >= 0) st.selectedNotify = hit;
				// Opened by hand rather than through BeginPopupContextItem, so the
				// menu itself can be drawn further down: it pushes a help scope,
				// and a scope opened above the rows would claim them.
				ImGui::OpenPopup("##notifyLaneMenu");
			}
			else if (ImGui::IsItemActivated())
			{
				const int hit = notifyAt(*clip, view, mouse.x, laneY, laneH, mouse.y);
				if (hit >= 0) st.selectedNotify = hit;
				else
				{
					st.selectedNotify = -1;
					// Empty lane scrubs. That is the reason the timeline sits under
					// a preview at all: an artist places a footstep by looking at
					// the foot, not at a number.
					st.clipTime = view.tOf(mouse.x);
					st.playing  = false;
				}
			}
			// Dragging the selection moves it. Bounded by the clip: a notify past
			// the end never fires, and a lane that lets one be dragged there is a
			// lane that hides events.
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
			    st.selectedNotify >= 0 && st.selectedNotify < static_cast<int>(clip->notifies.size()))
			{
				const float dt = ImGui::GetIO().MouseDelta.x / view.pixelsPerSecond();
				if (dt != 0.0f)
				{
					AnimationNotify& n = clip->notifies[static_cast<size_t>(st.selectedNotify)];
					n.time = std::clamp(n.time + dt, 0.0f, view.duration);
					markClipDirty(*clip);
				}
			}
			// A double-click on empty lane adds one where it landed — the gesture
			// every key editor has, and the reason there is no "add at 0 then drag".
			if (laneHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
			    notifyAt(*clip, view, mouse.x, laneY, laneH, mouse.y) < 0)
			{
				clip->notifies.push_back(AnimationNotify{ "Notify", view.tOf(mouse.x), 0.0f });
				st.selectedNotify = static_cast<int>(clip->notifies.size()) - 1;
				markClipDirty(*clip);
			}

			// ── The selected event's own values ─────────────────────────────
			// An index survives everything above except a delete, which resets it —
			// but a clip reloaded from disk under a stale index would not, so the
			// bound is re-checked here rather than trusted.
			if (st.selectedNotify >= static_cast<int>(clip->notifies.size())) st.selectedNotify = -1;
			if (st.selectedNotify >= 0)
			{
				AnimationNotify& n = clip->notifies[static_cast<size_t>(st.selectedNotify)];
				ImGui::Spacing();
				// Row::inputText writes into the string on every keystroke, which is
				// exactly what "the loaded clip is the edit buffer" means here: there
				// is no undo step to lose, so no scratch buffer is needed either.
				if (EditorWidgets::Row::inputText("Name##notify", &n.name)) markClipDirty(*clip);
				if (EditorWidgets::Row::dragFloat("Time##notify", &n.time, 0.01f,
				                                  0.0f, view.duration, "%.3fs"))
					markClipDirty(*clip);
				if (EditorWidgets::Row::dragFloat("Duration##notify", &n.duration, 0.01f,
				                                  0.0f, view.duration, "%.3fs"))
					markClipDirty(*clip);
				EditorWidgets::hint(n.duration > 0.0f
					? "A notify STATE: Begin at Time, End at Time + Duration."
					: "A notify: fires once as the playhead passes Time.");
			}
			else if (clip->notifies.empty())
				ImGui::TextDisabled("No notifies. Double-click the lane to add one.");

			// Last, for the reason above: the scope it pushes must not reach the
			// rows, whose entries live under this tab's own "Mesh Viewer".
			if (ImGui::BeginPopup("##notifyLaneMenu"))
			{
				HE::Ed::Help::Scope menuScope("Notify Timeline");
				if (EditorWidgets::menuItem("Add Notify"))
				{
					clip->notifies.push_back(AnimationNotify{ "Notify", st.clipTime, 0.0f });
					st.selectedNotify = static_cast<int>(clip->notifies.size()) - 1;
					markClipDirty(*clip);
				}
				if (EditorWidgets::menuItem("Add Notify State"))
				{
					const float rest = std::max(view.duration - st.clipTime, 0.0f);
					clip->notifies.push_back(AnimationNotify{
						"State", st.clipTime, std::max(rest * 0.25f, 0.05f) });
					st.selectedNotify = static_cast<int>(clip->notifies.size()) - 1;
					markClipDirty(*clip);
				}
				const bool hasSel = st.selectedNotify >= 0 &&
				                    st.selectedNotify < static_cast<int>(clip->notifies.size());
				if (EditorWidgets::menuItem("Delete", nullptr, false, hasSel))
				{
					clip->notifies.erase(clip->notifies.begin() + st.selectedNotify);
					st.selectedNotify = -1;
					markClipDirty(*clip);
				}
				ImGui::EndPopup();
			}
		}

		std::vector<glm::mat4> boneMatrices; // empty = bind pose
		if (clip)
		{
			// Locked, not raw: the pose a character with root motion actually wears
			// has the motion taken OUT of it. A preview that skipped the lock would
			// slide the mesh away from the path drawn beside it.
			const HE::RootMotionOptions opt;   // first root joint, the ordinary rig
			AnimationPreview::evaluateClipPoseLocked(*mesh, *clip, st.clipTime, opt, boneMatrices);

			if (st.pathClipId != st.clipId || st.pathHadRootMotion != clip->hasRootMotion)
			{
				AnimationPreview::rootMotionPath(*mesh, *clip, opt, 96, st.path);
				st.pathClipId        = st.clipId;
				st.pathHadRootMotion = clip->hasRootMotion;
			}
		}
		else
			st.path.clear();

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

		// ── Root-motion path, seen from above ───────────────────────────────
		// A corner plot rather than a line in the 3D preview: the preview is
		// rendered by the backend and this panel has no projection to draw into.
		// From above is also the view that answers the question anyway — how far
		// and which way — and the marker on it says where the playhead stands.
		if (st.showRootPath && st.path.size() > 1)
		{
			const float plot = std::min(std::min(av.x, av.y) * 0.34f, 150.0f);
			const ImVec2 p0(org.x + 8.0f, org.y + av.y - plot - 8.0f);
			const ImVec2 p1(p0.x + plot, p0.y + plot);

			glm::vec3 mn = st.path[0], mx = st.path[0];
			for (const glm::vec3& p : st.path)
			{
				mn = glm::min(mn, p);
				mx = glm::max(mx, p);
			}
			// One scale for both axes, and a floor on it: a clip that only walks
			// forward has zero width, and fitting that to the box would stretch a
			// straight line into whatever the box happens to be.
			const float span = std::max(std::max(mx.x - mn.x, mx.z - mn.z), 0.25f);
			const float sc   = (plot - 16.0f) / span;
			const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
			const float  cx = (mn.x + mx.x) * 0.5f, cz = (mn.z + mx.z) * 0.5f;
			// +Z is forward and screens count Y downwards, so forward is UP here.
			auto at = [&](const glm::vec3& p) {
				return ImVec2(c.x + (p.x - cx) * sc, c.y - (p.z - cz) * sc);
			};

			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(p0, p1, IM_COL32(18, 17, 16, 200), 4.0f);
			dl->AddRect(p0, p1, kLaneLine, 4.0f);
			for (size_t i = 1; i < st.path.size(); ++i)
				dl->AddLine(at(st.path[i - 1]), at(st.path[i]), kPathLine, 1.5f);
			dl->AddCircleFilled(at(st.path.front()), 3.0f, IM_COL32(96, 196, 124, 255));
			dl->AddCircleFilled(at(st.path.back()),  3.0f, kNotify);

			// Where the playhead stands on that path, by the same fraction the
			// path was sampled at.
			const AnimationClipAsset* c2 = ctx.contentManager
				? ctx.contentManager->getAnimationClip(st.clipId) : nullptr;
			if (c2 && c2->duration > 0.0f)
			{
				const float f  = std::clamp(st.clipTime / c2->duration, 0.0f, 1.0f);
				const size_t i = static_cast<size_t>(f * static_cast<float>(st.path.size() - 1));
				dl->AddCircle(at(st.path[i]), 4.5f, kPlayhead, 0, 1.5f);
			}

			char dist[48];
			std::snprintf(dist, sizeof(dist), "%.2f m",
			              glm::length(glm::vec3(st.path.back().x, 0.0f, st.path.back().z) -
			                          glm::vec3(st.path.front().x, 0.0f, st.path.front().z)));
			dl->AddText(ImVec2(p0.x + 5.0f, p1.y - ImGui::GetTextLineHeight() - 3.0f),
			            EditorToolbar::kFgDim, dist);
		}

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
