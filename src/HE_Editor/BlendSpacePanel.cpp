#include "BlendSpacePanel.h"
#include "EditorApplication.h"     // AppContext
#include "EditorAssetTypeCache.h"  // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"      // shared per-tab state map
#include "EditorToolbar.h"         // shared toolbar strip
#include "EditorHelp.h"            // Help::Scope — "Blend Space Editor/<label>"
#include "EditorWidgets.h"

#include <BlendSpace/BlendSpace.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/AnimationPose.h>
#include <Diagnostics/Log.h>
#include <Types/Enums.h>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

struct PanelState
{
	bool           loaded = false;
	bool           dirty  = false;
	std::string    relPath;
	HE::UUID       assetId;
	HE::BlendSpace space;

	// Where the preview cursor stands in the parameter space, and how far
	// through the shared cycle. Editor state, per tab, never written into the
	// asset: it is a question the author is asking, not a property of the space.
	float cursorX = 0.0f, cursorY = 0.0f;

	// Which sample the cursor is dragging, or -1. Held across frames because a
	// drag is a gesture and ImGui only tells us "the mouse is down here".
	int   draggingSample = -1;

	std::string lastSaveError;
};
AssetPanelState<PanelState> s_states;

bool saveState(PanelState& st, AppContext& ctx)
{
	if (!ctx.contentManager) return false;
	st.lastSaveError.clear();
	BlendSpaceAsset* a = ctx.contentManager->getBlendSpaceMutable(st.assetId);
	if (!a)
	{
		st.lastSaveError = "Not saved: this blend space asset is no longer loaded.";
		return false;
	}
	a->json = HE::blendSpaceToJson(st.space);
	if (!ctx.contentManager->saveAsset(*a))
	{
		st.lastSaveError = "Not saved: the file could not be written.";
		return false;
	}
	st.dirty = false;
	return true;
}

bool isTwoD(const PanelState& st) { return st.space.kind == HE::BlendSpaceKind::TwoD; }

// The diagram. Axes, the samples as draggable dots, and the cursor whose weights
// the list below reads out.
//
// It draws the PARAMETER space and not the poses, because the question an author
// has here is "which clips are active at speed 3, and how much" — a question
// about numbers, which a picture of numbers answers and a row of thumbnails does
// not.
void diagram(PanelState& st, const std::vector<float>& weights)
{
	HE::Ed::Help::Scope helpScope("Blend Space Editor");  // the audit reads source

	const bool twoD = isTwoD(st);
	const float h   = twoD ? 240.0f : 120.0f;
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const ImVec2 size(std::max(avail.x, 120.0f), h);
	const ImVec2 p0 = ImGui::GetCursorScreenPos();
	const ImVec2 p1(p0.x + size.x, p0.y + size.y);

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 28, 255), 4.0f);
	dl->AddRect(p0, p1, IM_COL32(70, 70, 78, 255), 4.0f);

	const float pad = 18.0f;
	const ImVec2 a0(p0.x + pad, p0.y + pad);
	const ImVec2 a1(p1.x - pad, p1.y - pad);

	// The ranges are the EDITOR's axes and nothing else — the sampler clamps to
	// the outermost sample, not to these. A degenerate range would divide by
	// zero here, so it is widened rather than trusted.
	const float minX = st.space.minX, maxX = st.space.maxX;
	const float spanX = (maxX - minX) != 0.0f ? (maxX - minX) : 1.0f;
	const float minY = st.space.minY, maxY = st.space.maxY;
	const float spanY = (maxY - minY) != 0.0f ? (maxY - minY) : 1.0f;

	const auto toScreen = [&](float x, float y) -> ImVec2 {
		const float tx = (x - minX) / spanX;
		const float ty = (y - minY) / spanY;
		return ImVec2(a0.x + tx * (a1.x - a0.x),
		              twoD ? a1.y - ty * (a1.y - a0.y) : (a0.y + a1.y) * 0.5f);
	};
	const auto toParam = [&](const ImVec2& s, float& x, float& y) {
		x = minX + (s.x - a0.x) / std::max(a1.x - a0.x, 1.0f) * spanX;
		y = twoD ? minY + (a1.y - s.y) / std::max(a1.y - a0.y, 1.0f) * spanY : 0.0f;
	};

	// Axes
	if (twoD)
	{
		dl->AddLine(ImVec2(a0.x, a1.y), ImVec2(a1.x, a1.y), IM_COL32(90, 90, 100, 255));
		dl->AddLine(ImVec2(a0.x, a0.y), ImVec2(a0.x, a1.y), IM_COL32(90, 90, 100, 255));
	}
	else
	{
		const float mid = (a0.y + a1.y) * 0.5f;
		dl->AddLine(ImVec2(a0.x, mid), ImVec2(a1.x, mid), IM_COL32(90, 90, 100, 255));
	}

	ImGui::InvisibleButton("##bsdiagram", size);
	const bool hovered = ImGui::IsItemHovered();
	const ImVec2 mouse = ImGui::GetIO().MousePos;

	// Dragging a sample beats moving the cursor: the dots are what the author is
	// authoring, the cursor is only how they look at it.
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		st.draggingSample = -1;
		float best = 12.0f * 12.0f;
		for (size_t i = 0; i < st.space.samples.size(); ++i)
		{
			const ImVec2 s = toScreen(st.space.samples[i].x, st.space.samples[i].y);
			const float d2 = (s.x - mouse.x) * (s.x - mouse.x) + (s.y - mouse.y) * (s.y - mouse.y);
			if (d2 < best) { best = d2; st.draggingSample = static_cast<int>(i); }
		}
		if (st.draggingSample < 0) toParam(mouse, st.cursorX, st.cursorY);
	}
	if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && st.draggingSample >= 0 &&
	    st.draggingSample < static_cast<int>(st.space.samples.size()))
	{
		auto& e = st.space.samples[static_cast<size_t>(st.draggingSample)];
		float nx = e.x, ny = e.y;
		toParam(mouse, nx, ny);
		if (nx != e.x || (twoD && ny != e.y)) st.dirty = true;
		e.x = nx;
		if (twoD) e.y = ny;
	}
	else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		st.draggingSample = -1;
	}
	if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right))
		toParam(mouse, st.cursorX, st.cursorY);

	// The samples, sized by their current weight — the readout an author scans
	// for rather than reading the numbers one by one.
	for (size_t i = 0; i < st.space.samples.size(); ++i)
	{
		const auto& e = st.space.samples[i];
		const float w = (i < weights.size()) ? weights[i] : 0.0f;
		const ImVec2 s = toScreen(e.x, e.y);
		const ImU32 col = w > 0.0f ? IM_COL32(240, 180, 80, 255) : IM_COL32(120, 120, 130, 255);
		dl->AddCircleFilled(s, 4.0f + 6.0f * w, col);
		dl->AddCircle(s, 4.0f + 6.0f * w, IM_COL32(20, 20, 24, 255));
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%zu", i);
		dl->AddText(ImVec2(s.x + 8.0f, s.y - 8.0f), IM_COL32(200, 200, 210, 255), buf);
	}

	// The cursor
	const ImVec2 c = toScreen(st.cursorX, st.cursorY);
	dl->AddLine(ImVec2(c.x, p0.y + 4.0f), ImVec2(c.x, p1.y - 4.0f), IM_COL32(120, 200, 255, 160));
	if (twoD)
		dl->AddLine(ImVec2(p0.x + 4.0f, c.y), ImVec2(p1.x - 4.0f, c.y), IM_COL32(120, 200, 255, 160));
	dl->AddCircle(c, 5.0f, IM_COL32(120, 200, 255, 255), 0, 2.0f);

	ImGui::Spacing();
	ImGui::TextDisabled(twoD
		? "Drag a point to move a sample. Click empty space (or right-drag) to move the cursor."
		: "Drag a point to move a sample along the axis. Click empty space to move the cursor.");
}

// One sample row: its clip, where it sits, and what it weighs right now.
void sampleRow(PanelState& st, AppContext& ctx, size_t index,
               const std::vector<float>& weights, int& removeIndex)
{
	HE::Ed::Help::Scope helpScope("Blend Space Editor");  // see diagram()

	auto& e = st.space.samples[index];
	ImGui::PushID(static_cast<int>(index));
	ImGui::Separator();

	const float w = (index < weights.size()) ? weights[index] : 0.0f;
	ImGui::Text("Sample %zu", index);
	ImGui::SameLine();
	ImGui::TextDisabled("weight %.3f", static_cast<double>(w));

	// No undo snapshot: this is an ASSET editor with its own dirty flag and save
	// button, like every other tab editor here — the world's undo stack has
	// nothing to do with it.
	if (EditorWidgets::assetDropSlot(ctx, "Clip", e.clipId,
			HE::AssetType::AnimationClip, "bsclip",
			"(none — this sample poses nothing)", "animation clip",
			/*showClear=*/true, /*undo=*/false) != EditorWidgets::SlotAction::None)
		st.dirty = true;

	if (EditorWidgets::Row::dragFloat("X##bs", &e.x, 0.01f, -10000.0f, 10000.0f, "%.3f"))
		st.dirty = true;
	if (isTwoD(st))
		if (EditorWidgets::Row::dragFloat("Y##bs", &e.y, 0.01f, -10000.0f, 10000.0f, "%.3f"))
			st.dirty = true;
	if (EditorWidgets::Row::dragFloat("Speed Scale##bs", &e.speedScale, 0.01f, 0.01f, 10.0f, "%.2f"))
		st.dirty = true;

	if (EditorWidgets::dangerSmallButton("Remove")) removeIndex = static_cast<int>(index);
	ImGui::PopID();
}

} // namespace

bool BlendSpacePanel::isBlendSpaceAsset(const std::string& path)
{ return EditorAssetTypeCache::is(path, HE::AssetType::BlendSpace); }

bool BlendSpacePanel::isDirty(const std::string& path) { return s_states.dirty(path); }

bool BlendSpacePanel::reloadFromDisk(const std::string& assetPath)
{
	auto* st = s_states.find(assetPath);
	if (!st) return false;
	st->loaded = false;
	st->dirty  = false;
	return true;
}

void BlendSpacePanel::appendDirtyPaths(std::vector<std::string>& out)
{ s_states.appendDirtyPaths(out); }

bool BlendSpacePanel::save(AppContext& ctx, const std::string& path)
{
	PanelState* st = s_states.find(path);
	if (!st || !st->dirty) return true;   // "not mine" reads as success
	return saveState(*st, ctx);
}

void BlendSpacePanel::forget(const std::string& path) { s_states.forget(path); }

void BlendSpacePanel::render(AppContext& ctx, const std::string& assetPath,
                             const ImVec2& pos, const ImVec2& size)
{
	PanelState& st = s_states[assetPath];
	if (!st.loaded && ctx.contentManager)
	{
		st.relPath = ctx.contentManager->toContentRelativePath(assetPath);
		st.assetId = ctx.contentManager->loadAsset(st.relPath);
		st.space   = HE::BlendSpace{};
		if (const BlendSpaceAsset* a = ctx.contentManager->getBlendSpace(st.assetId))
			HE::blendSpaceFromJson(a->json, st.space);
		if (st.space.name.empty())
			st.space.name = std::filesystem::path(assetPath).stem().string();
		st.loaded = true;
	}

	HE::Ed::Help::Scope helpScope("Blend Space Editor");
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin(("##blendspace_" + assetPath).c_str(), nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

	{
		namespace T = EditorToolbar;
		T::Bar bar;
		T::assetHeader(bar, st.space.name.c_str(), T::iconBranch, st.dirty);
		bar.group();
		bar.readout(nullptr, "Blend Space", T::kFgDim);
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
		ImGui::TextDisabled("Several clips, placed by parameter and mixed by where the parameters");
		ImGui::TextDisabled("stand. All samples share one normalised cycle, so a 1.2 s walk and a");
		ImGui::TextDisabled("0.8 s run stay in step instead of drifting apart mid-blend.");
	}

	ImGui::Spacing();

	static const char* kKinds[] = { "1D (one parameter)", "2D (two parameters)" };
	int kind = static_cast<int>(st.space.kind);
	if (EditorWidgets::Row::combo("Kind", &kind, kKinds, IM_ARRAYSIZE(kKinds)))
	{ st.space.kind = HE::blendSpaceKindFromInt(kind); st.dirty = true; }

	if (EditorWidgets::Row::inputText("Parameter X", &st.space.paramX)) st.dirty = true;
	if (EditorWidgets::Row::dragFloat("Range X Min", &st.space.minX, 0.05f, -10000.0f, 10000.0f, "%.2f")) st.dirty = true;
	if (EditorWidgets::Row::dragFloat("Range X Max", &st.space.maxX, 0.05f, -10000.0f, 10000.0f, "%.2f")) st.dirty = true;
	if (isTwoD(st))
	{
		if (EditorWidgets::Row::inputText("Parameter Y", &st.space.paramY)) st.dirty = true;
		if (EditorWidgets::Row::dragFloat("Range Y Min", &st.space.minY, 0.05f, -10000.0f, 10000.0f, "%.2f")) st.dirty = true;
		if (EditorWidgets::Row::dragFloat("Range Y Max", &st.space.maxY, 0.05f, -10000.0f, 10000.0f, "%.2f")) st.dirty = true;
	}
	if (EditorWidgets::checkbox("Looping", &st.space.looping)) st.dirty = true;

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// The same weights the runtime computes, from the same function. A preview
	// that mixed differently from the game would be worse than none.
	std::vector<float> weights;
	HE::blendSpaceWeights(st.space, st.cursorX, isTwoD(st) ? st.cursorY : 0.0f, weights);

	diagram(st, weights);

	ImGui::Spacing();
	if (EditorWidgets::Row::dragFloat("Cursor X", &st.cursorX, 0.01f, -10000.0f, 10000.0f, "%.3f")) {}
	if (isTwoD(st))
		if (EditorWidgets::Row::dragFloat("Cursor Y", &st.cursorY, 0.01f, -10000.0f, 10000.0f, "%.3f")) {}

	int active = 0;
	for (float w : weights) if (w > 0.0f) ++active;
	ImGui::TextDisabled("%d of %zu sample(s) active — at most %zu are ever evaluated.",
	                    active, st.space.samples.size(), HE::kBlendSpaceMaxActiveSamples);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	int removeIndex = -1;
	for (size_t i = 0; i < st.space.samples.size(); ++i)
		sampleRow(st, ctx, i, weights, removeIndex);
	if (removeIndex >= 0)
	{
		st.space.samples.erase(st.space.samples.begin() + removeIndex);
		st.dirty = true;
	}

	ImGui::Separator();
	if (EditorWidgets::button("Add Sample", ImVec2(130.0f, 0.0f)))
	{
		HE::BlendSpaceSample e;
		// At the cursor, which is where the author is looking. A new sample at
		// the origin would land on top of whatever is already there.
		e.x = st.cursorX;
		e.y = isTwoD(st) ? st.cursorY : 0.0f;
		st.space.samples.push_back(e);
		st.dirty = true;
	}

	if (st.space.samples.empty())
	{
		EditorWidgets::WrapText wrap;
		ImGui::TextDisabled("No samples yet. Add one per clip — walk at 0, jog at 3, run at 6 —");
		ImGui::TextDisabled("and a state or an animation layer pointing at this space will mix");
		ImGui::TextDisabled("them by its parameter.");
	}

	ImGui::End();
}
