#include "AnimatorStateMachineEditorPanel.h"
#include <cstdint>
#include "EditorApplication.h" // AppContext
#include "GraphEditor.h"       // shared node-graph canvas frontend
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <AnimatorStateMachine/AnimatorStateMachineGraph.h>
#include <HorizonScene/AnimationStateMachineSystem.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <Types/Enums.h>
#include <Diagnostics/Logger.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace AnimatorStateMachineEditorPanel
{

struct State
{
	bool        loaded = false;
	std::string relPath, name;
	HE::UUID    assetId;
	HE::AnimatorStateMachineGraph graph;
	bool        dirty = false;

	GraphEditor::State geState;
};

static std::map<std::string, State> g_states;

static State& stateFor(const std::string& path, AppContext& ctx)
{
	State& st = g_states[path];
	if (st.loaded || !ctx.contentManager) return st;

	st.name = std::filesystem::path(path).filename().string();
	const std::string rel = ctx.contentManager->toContentRelativePath(path);
	st.relPath = rel.empty() ? path : rel;
	st.assetId = ctx.contentManager->loadAsset(st.relPath);

	if (const AnimatorStateMachineAsset* asset = ctx.contentManager->getAnimatorStateMachine(st.assetId);
	    asset && !asset->graphJson.empty())
	{
		HE::AnimatorStateMachineGraph parsed;
		if (HE::animatorStateMachineFromJson(asset->graphJson, parsed)) st.graph = std::move(parsed);
	}

	st.loaded = true;
	return st;
}

bool isAnimatorStateMachineAsset(const std::string& path)
{
	static std::map<std::string, bool> s_typeCache;
	if (auto it = s_typeCache.find(path); it != s_typeCache.end()) return it->second;
	HAsset::Reader r;
	const bool isAsm = r.open(path) &&
		r.assetType() == static_cast<uint16_t>(HE::AssetType::AnimatorStateMachine);
	s_typeCache[path] = isAsm;
	return isAsm;
}

void forget(const std::string& assetPath) { g_states.erase(assetPath); }

namespace
{
HE::AnimationState* findStateById(HE::AnimatorStateMachineGraph& g, int id)
{
	for (auto& s : g.states) if (s.id == id) return &s;
	return nullptr;
}
HE::AnimationState* findStateByName(HE::AnimatorStateMachineGraph& g, const std::string& name)
{
	for (auto& s : g.states) if (s.name == name) return &s;
	return nullptr;
}

// Scale embedded ImGui widgets to the canvas zoom (same technique Material/
// HorizonCode/ParticleGraph node bodies use).
void pushWidgetScale(float z)
{
	const ImGuiStyle& s = ImGui::GetStyle();
	const ImVec2 fp = s.FramePadding, is = s.ItemSpacing, iis = s.ItemInnerSpacing;
	const float  fr = s.FrameRounding, gm = s.GrabMinSize;
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,     ImVec2(fp.x * z, fp.y * z));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(is.x * z, is.y * z));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(iis.x * z, iis.y * z));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,    fr * z);
	ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize,      gm * z);
	ImGui::SetWindowFontScale(z);
}
void popWidgetScale() { ImGui::SetWindowFontScale(1.0f); ImGui::PopStyleVar(5); }
} // namespace

void render(AppContext& ctx, const std::string& assetPath, const ImVec2& pos, const ImVec2& size)
{
	State& st = stateFor(assetPath, ctx);

	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin("##AnimStateMachineTab", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	AnimatorStateMachineAsset* asset = ctx.contentManager
		? ctx.contentManager->getAnimatorStateMachineMutable(st.assetId) : nullptr;

	// ── Header ───────────────────────────────────────────────────────────────
	ImGui::TextUnformatted(st.name.c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("state machine%s — %zu state(s), %zu transition(s)",
		st.dirty ? "  (unsaved)" : "", st.graph.states.size(), st.graph.transitions.size());
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 100.0f);
	if (ImGui::Button("Save##asmsave") && asset)
	{
		asset->graphJson = HE::animatorStateMachineToJson(st.graph);
		if (ctx.contentManager->saveAsset(*asset)) st.dirty = false;
		// Live entities already using this asset should reflect the edit now,
		// not only the next time their own stateMachineAssetId changes — same
		// idea as InvalidateMaterial after a Material save.
		if (ctx.world)
			for (auto [e, sm] : ctx.world->registry().view<AnimatorStateMachineComponent>().each())
				if (sm.stateMachineAssetId == st.assetId) AnimationStateMachineSystem::markConfigDirty(sm);
		Logger::Log(Logger::LogLevel::Info, ("AnimatorStateMachineEditor: saved '" + st.name + "'").c_str());
	}
	if (!asset)
		ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Asset could not be loaded.");
	ImGui::Separator();

	bool structuralEdit = false;

	// ── Left: node graph canvas (states as nodes, transitions as links) ───────
	const float rightW = 320.0f;
	const float leftW  = std::max(200.0f, ImGui::GetContentRegionAvail().x - rightW);
	ImGui::BeginChild("##asmCanvas", ImVec2(leftW, 0), ImGuiChildFlags_Borders);
	{
		const ImVec2 avail = ImGui::GetContentRegionAvail();

		GraphEditor::Model m;
		m.nodeIds = [&st] {
			std::vector<int> ids; ids.reserve(st.graph.states.size());
			for (const auto& s : st.graph.states) ids.push_back(s.id);
			return ids;
		};
		m.getPos = [&st](int id, float& x, float& y) {
			if (const HE::AnimationState* s = findStateById(st.graph, id)) { x = s->x; y = s->y; }
		};
		m.setPos = [&st](int id, float x, float y) {
			if (HE::AnimationState* s = findStateById(st.graph, id)) { s->x = x; s->y = y; }
		};
		m.title = [&st](int id) -> std::string {
			const HE::AnimationState* s = findStateById(st.graph, id);
			return s ? s->name : std::string();
		};
		m.headerColor = [](int) -> ImU32 { return GraphEditor::categoryColor("Material"); };
		m.pins = [](int) -> std::vector<GraphEditor::Pin> {
			// Generic control-flow pins — transitions carry no data type, just topology.
			return {
				{ 0, "In",  IM_COL32(200, 200, 200, 255), true,  true },
				{ 0, "Out", IM_COL32(200, 200, 200, 255), false, true },
			};
		};
		m.links = [&st] {
			std::vector<std::array<int, 4>> ls;
			ls.reserve(st.graph.transitions.size());
			for (const auto& t : st.graph.transitions)
			{
				const HE::AnimationState* from = findStateByName(st.graph, t.fromState);
				const HE::AnimationState* to   = findStateByName(st.graph, t.toState);
				if (!from || !to) continue; // dangling ref (renamed/hand-edited) — skip, don't crash
				ls.push_back({ from->id, 0, to->id, 0 });
			}
			return ls;
		};
		m.connect = [&st](int outNode, int /*outPin*/, int inNode, int /*inPin*/) -> bool {
			HE::AnimationState* from = findStateById(st.graph, outNode);
			HE::AnimationState* to   = findStateById(st.graph, inNode);
			if (!from || !to) return false;
			HE::AnimationTransition t;
			t.fromState = from->name;
			t.toState   = to->name;
			st.graph.transitions.push_back(std::move(t));
			return true;
		};
		m.removeNode = [&st](int id) {
			auto it = std::find_if(st.graph.states.begin(), st.graph.states.end(),
				[id](const HE::AnimationState& s) { return s.id == id; });
			if (it == st.graph.states.end()) return;
			const std::string name = it->name;
			st.graph.states.erase(it);
			st.graph.transitions.erase(std::remove_if(st.graph.transitions.begin(), st.graph.transitions.end(),
				[&](const HE::AnimationTransition& t) { return t.fromState == name || t.toState == name; }),
				st.graph.transitions.end());
			if (st.graph.startState == name) st.graph.startState.clear();
		};
		m.drawAddMenu = [&st]() -> int {
			int created = 0;
			if (ImGui::Selectable("Add State"))
			{
				int maxId = 0;
				for (const auto& s : st.graph.states) maxId = std::max(maxId, s.id);
				HE::AnimationState s;
				s.id   = maxId + 1;
				s.name = "State" + std::to_string(s.id);
				s.x    = st.geState.addMenuGraphPos.x;
				s.y    = st.geState.addMenuGraphPos.y;
				st.graph.states.push_back(s);
				created = s.id;
				ImGui::CloseCurrentPopup();
			}
			return created;
		};
		m.nodeBodyHeight = [](int) -> float { return 52.0f; }; // name + loop, clip slot
		m.drawNodeBody = [&st, &ctx, &structuralEdit](int id, ImVec2 bodyMin, ImVec2 bodyMax, float zoom)
		{
			HE::AnimationState* s = findStateById(st.graph, id);
			if (!s) return;

			ImGui::SetCursorScreenPos(bodyMin);
			pushWidgetScale(zoom);
			const float w = (GraphEditor::kNodeW - 24.0f) * zoom;

			char nameBuf[128];
			std::snprintf(nameBuf, sizeof(nameBuf), "%s", s->name.c_str());
			ImGui::SetNextItemWidth(w * 0.66f);
			if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
			{
				// Renaming a state must re-point every transition (+ startState)
				// referencing the old name, or links silently go dangling.
				const std::string oldName = s->name;
				s->name = nameBuf;
				for (auto& t : st.graph.transitions)
				{
					if (t.fromState == oldName) t.fromState = s->name;
					if (t.toState   == oldName) t.toState   = s->name;
				}
				if (st.graph.startState == oldName) st.graph.startState = s->name;
			}
			structuralEdit |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::SameLine();
			ImGui::Checkbox("Loop##st", &s->looping);
			structuralEdit |= ImGui::IsItemDeactivatedAfterEdit();

			// Clip drag-drop slot (same whole-row-is-the-drop-target pattern as
			// Material's TextureSample / ParticleGraph's Mesh/Material slots).
			ImGui::SetCursorScreenPos(ImVec2(bodyMin.x, bodyMin.y + 24.0f * zoom));
			const std::string clipState = (s->clipId == HE::UUID{}) ? std::string("(no clip)")
				: (ctx.contentManager && ctx.contentManager->assetType(s->clipId) == HE::AssetType::AnimationClip
					? std::string("clip set") : std::string("(missing)"));
			ImGui::TextDisabled("%s", clipState.c_str());
			ImGui::SetCursorScreenPos(ImVec2(bodyMin.x, bodyMin.y + 24.0f * zoom));
			ImGui::SetNextItemAllowOverlap();
			ImGui::InvisibleButton("##clipslot", ImVec2(std::max(bodyMax.x - bodyMin.x, 1.0f), 22.0f * zoom));
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
				{
					const std::string rel = ctx.contentManager
						? ctx.contentManager->toContentRelativePath(static_cast<const char*>(pl->Data))
						: std::string();
					if (!rel.empty() && ctx.contentManager)
					{
						const HE::UUID dropped = ctx.contentManager->loadAsset(rel);
						if (dropped != HE::UUID{} && ctx.contentManager->assetType(dropped) == HE::AssetType::AnimationClip)
						{
							s->clipId = dropped;
							structuralEdit = true;
						}
					}
				}
				ImGui::EndDragDropTarget();
			}
			popWidgetScale();
		};

		const bool changed = GraphEditor::draw("##asm_graphcanvas", m, st.geState, avail);
		if (changed) structuralEdit = true;
	}
	ImGui::EndChild();

	ImGui::SameLine();

	// ── Right: transitions/params fine-editing ────────────────────────────────
	// GraphEditor links carry no inline widgets — the canvas gives topology +
	// layout, this flat list gives parameter editing (same split Forts. 49's
	// original flat Inspector UI already used).
	ImGui::BeginChild("##asmSide", ImVec2(rightW - 8.0f, 0), ImGuiChildFlags_Borders);
	{
		ImGui::TextDisabled("Start State");
		char startBuf[64];
		std::snprintf(startBuf, sizeof(startBuf), "%s", st.graph.startState.c_str());
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::InputText("##startState", startBuf, sizeof(startBuf)))
		{ st.graph.startState = startBuf; structuralEdit = true; }
		ImGui::TextDisabled("(empty = the first state, if any)");
		ImGui::Separator();

		if (ImGui::TreeNodeEx("Transitions##sm", ImGuiTreeNodeFlags_DefaultOpen))
		{
			const char* opNames[] = { ">", "<", "==" };
			int transToDelete = -1;
			for (int i = 0; i < static_cast<int>(st.graph.transitions.size()); ++i)
			{
				auto& t = st.graph.transitions[i];
				ImGui::PushID(i);
				char fb[64], tb[64], pb[64];
				std::snprintf(fb, sizeof(fb), "%s", t.fromState.c_str());
				std::snprintf(tb, sizeof(tb), "%s", t.toState.c_str());
				std::snprintf(pb, sizeof(pb), "%s", t.paramName.c_str());
				ImGui::TextDisabled("%s -> %s", t.fromState.c_str(), t.toState.c_str());
				if (ImGui::InputText("From##t", fb, sizeof(fb)))  { t.fromState = fb; structuralEdit = true; }
				if (ImGui::InputText("To##t",   tb, sizeof(tb)))  { t.toState   = tb; structuralEdit = true; }
				int opIdx = static_cast<int>(t.op);
				if (ImGui::Combo("Op##t", &opIdx, opNames, 3))
				{ t.op = static_cast<HE::TransitionOp>(opIdx); structuralEdit = true; }
				if (ImGui::InputText("Param##t", pb, sizeof(pb))) { t.paramName = pb; structuralEdit = true; }
				ImGui::DragFloat("Thresh##t", &t.threshold, 0.01f, -999.0f, 999.0f, "%.2f");
				structuralEdit |= ImGui::IsItemDeactivatedAfterEdit();
				ImGui::DragFloat("Duration##t", &t.duration, 0.01f, 0.0f, 10.0f, "%.2f s");
				structuralEdit |= ImGui::IsItemDeactivatedAfterEdit();
				if (ImGui::SmallButton("Remove##t")) transToDelete = i;
				ImGui::Separator();
				ImGui::PopID();
			}
			if (transToDelete >= 0)
			{ st.graph.transitions.erase(st.graph.transitions.begin() + transToDelete); structuralEdit = true; }
			if (ImGui::SmallButton("+ Transition##sm")) { st.graph.transitions.push_back({}); structuralEdit = true; }
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Default Params##sm", ImGuiTreeNodeFlags_None))
		{
			std::string paramToDelete;
			for (auto& [k, v] : st.graph.defaultParams)
			{
				ImGui::PushID(k.c_str());
				ImGui::SetNextItemWidth(-40.0f);
				ImGui::DragFloat(k.c_str(), &v, 0.01f, -999.0f, 999.0f, "%.2f");
				structuralEdit |= ImGui::IsItemDeactivatedAfterEdit();
				ImGui::SameLine();
				if (ImGui::SmallButton("X")) paramToDelete = k;
				ImGui::PopID();
			}
			if (!paramToDelete.empty()) { st.graph.defaultParams.erase(paramToDelete); structuralEdit = true; }
			static char s_newParamName[64] = "";
			ImGui::SetNextItemWidth(-70.0f);
			ImGui::InputText("##newParamName", s_newParamName, sizeof(s_newParamName));
			ImGui::SameLine();
			if (ImGui::SmallButton("+ Param") && s_newParamName[0] != '\0')
			{
				st.graph.defaultParams[s_newParamName] = 0.0f;
				s_newParamName[0] = '\0';
				structuralEdit = true;
			}
			ImGui::TreePop();
		}
	}
	ImGui::EndChild();

	if (structuralEdit) st.dirty = true;

	ImGui::End();
}

} // namespace AnimatorStateMachineEditorPanel
