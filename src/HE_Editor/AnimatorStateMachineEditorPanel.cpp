#include "AnimatorStateMachineEditorPanel.h"
#include "EditorApplication.h" // AppContext
#include "GraphEditor.h"       // shared node-graph canvas frontend
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace AnimatorStateMachineEditorPanel
{

std::string tabPathFor(Entity entity)
{
	return std::string(kTabPrefix) + std::to_string(static_cast<uint32_t>(entity));
}

bool isStateMachineTab(const std::string& path)
{
	return path.rfind(kTabPrefix, 0) == 0;
}

namespace
{
AnimationState* findStateById(AnimatorStateMachineComponent& sm, int id)
{
	for (auto& s : sm.states) if (s.id == id) return &s;
	return nullptr;
}
AnimationState* findStateByName(AnimatorStateMachineComponent& sm, const std::string& name)
{
	for (auto& s : sm.states) if (s.name == name) return &s;
	return nullptr;
}

// Scale embedded ImGui widgets to the canvas zoom (same technique the Material/
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

void render(AppContext& ctx, const std::string& tabPath, const ImVec2& pos, const ImVec2& size)
{
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin("##AnimStateMachineTab", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	Entity entity = entt::null;
	if (tabPath.size() > std::char_traits<char>::length(kTabPrefix))
	{
		const std::string idStr = tabPath.substr(std::char_traits<char>::length(kTabPrefix));
		char* end = nullptr;
		const unsigned long id = std::strtoul(idStr.c_str(), &end, 10);
		if (end && *end == '\0') entity = static_cast<Entity>(id);
	}

	if (!ctx.world || entity == entt::null || !ctx.world->registry().valid(entity))
	{
		ImGui::TextDisabled("This entity no longer exists — close this tab.");
		ImGui::End();
		return;
	}
	auto& registry = ctx.world->registry();
	AnimatorStateMachineComponent* sm = registry.try_get<AnimatorStateMachineComponent>(entity);
	if (!sm)
	{
		ImGui::TextDisabled("This entity no longer has an Animator State Machine — close this tab.");
		ImGui::End();
		return;
	}

	// ── Header ───────────────────────────────────────────────────────────────
	const NameComponent* nm = registry.try_get<NameComponent>(entity);
	ImGui::TextUnformatted(nm && !nm->name.empty() ? nm->name.c_str() : "(unnamed entity)");
	ImGui::SameLine();
	ImGui::TextDisabled("animator state machine — %zu state(s), %zu transition(s)",
		sm->states.size(), sm->transitions.size());
	ImGui::Separator();

	bool structuralEdit = false;

	// ── Left: node graph canvas (states as nodes, transitions as links) ───────
	static std::unordered_map<uint32_t, GraphEditor::State> s_geStates;
	GraphEditor::State& geState = s_geStates[static_cast<uint32_t>(entity)];

	const float rightW = 320.0f;
	const float leftW  = std::max(200.0f, ImGui::GetContentRegionAvail().x - rightW);
	ImGui::BeginChild("##asmCanvas", ImVec2(leftW, 0), ImGuiChildFlags_Borders);
	{
		const ImVec2 avail = ImGui::GetContentRegionAvail();

		GraphEditor::Model m;
		m.nodeIds = [sm] {
			std::vector<int> ids; ids.reserve(sm->states.size());
			for (const auto& s : sm->states) ids.push_back(s.id);
			return ids;
		};
		m.getPos = [sm](int id, float& x, float& y) {
			if (const AnimationState* s = findStateById(*sm, id)) { x = s->x; y = s->y; }
		};
		m.setPos = [sm](int id, float x, float y) {
			if (AnimationState* s = findStateById(*sm, id)) { s->x = x; s->y = y; }
		};
		m.title = [sm](int id) -> std::string {
			const AnimationState* s = findStateById(*sm, id);
			return s ? s->name : std::string();
		};
		m.headerColor = [](int) -> ImU32 { return GraphEditor::categoryColor("Material"); };
		m.nodeOutline = [sm](int id) -> ImU32 {
			const AnimationState* s = findStateById(*sm, id);
			// Highlight the state currently running (meaningful during Play mode).
			return (s && s->name == sm->currentStateName) ? IM_COL32(90, 230, 130, 255) : 0;
		};
		m.pins = [](int) -> std::vector<GraphEditor::Pin> {
			// Generic control-flow pins — transitions carry no data type, just topology.
			return {
				{ 0, "In",  IM_COL32(200, 200, 200, 255), true,  true },
				{ 0, "Out", IM_COL32(200, 200, 200, 255), false, true },
			};
		};
		m.links = [sm] {
			std::vector<std::array<int, 4>> ls;
			ls.reserve(sm->transitions.size());
			for (const auto& t : sm->transitions)
			{
				const AnimationState* from = findStateByName(*sm, t.fromState);
				const AnimationState* to   = findStateByName(*sm, t.toState);
				if (!from || !to) continue; // dangling ref (renamed/hand-edited) — skip, don't crash
				ls.push_back({ from->id, 0, to->id, 0 });
			}
			return ls;
		};
		m.connect = [sm](int outNode, int /*outPin*/, int inNode, int /*inPin*/) -> bool {
			AnimationState* from = findStateById(*sm, outNode);
			AnimationState* to   = findStateById(*sm, inNode);
			if (!from || !to) return false;
			AnimationTransition t;
			t.fromState = from->name;
			t.toState   = to->name;
			sm->transitions.push_back(std::move(t));
			return true;
		};
		m.removeNode = [sm](int id) {
			auto it = std::find_if(sm->states.begin(), sm->states.end(),
				[id](const AnimationState& s) { return s.id == id; });
			if (it == sm->states.end()) return;
			const std::string name = it->name;
			sm->states.erase(it);
			sm->transitions.erase(std::remove_if(sm->transitions.begin(), sm->transitions.end(),
				[&](const AnimationTransition& t) { return t.fromState == name || t.toState == name; }),
				sm->transitions.end());
			if (sm->currentStateName == name) sm->currentStateName.clear();
		};
		m.drawAddMenu = [sm, &geState]() -> int {
			int created = 0;
			if (ImGui::Selectable("Add State"))
			{
				int maxId = 0;
				for (const auto& s : sm->states) maxId = std::max(maxId, s.id);
				AnimationState s;
				s.id   = maxId + 1;
				s.name = "State" + std::to_string(s.id);
				s.x    = geState.addMenuGraphPos.x;
				s.y    = geState.addMenuGraphPos.y;
				sm->states.push_back(s);
				created = s.id;
				ImGui::CloseCurrentPopup();
			}
			return created;
		};
		m.nodeBodyHeight = [](int) -> float { return 52.0f; }; // name + loop, clip slot
		m.drawNodeBody = [sm, &ctx, &structuralEdit](int id, ImVec2 bodyMin, ImVec2 bodyMax, float zoom)
		{
			AnimationState* s = findStateById(*sm, id);
			if (!s) return;

			ImGui::SetCursorScreenPos(bodyMin);
			pushWidgetScale(zoom);
			const float w = (GraphEditor::kNodeW - 24.0f) * zoom;

			char nameBuf[128];
			std::snprintf(nameBuf, sizeof(nameBuf), "%s", s->name.c_str());
			ImGui::SetNextItemWidth(w * 0.66f);
			if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
			{
				// Renaming a state must re-point every transition referencing the old
				// name, or links silently go dangling (findStateByName above).
				const std::string oldName = s->name;
				s->name = nameBuf;
				for (auto& t : sm->transitions)
				{
					if (t.fromState == oldName) t.fromState = s->name;
					if (t.toState   == oldName) t.toState   = s->name;
				}
				if (sm->currentStateName == oldName) sm->currentStateName = s->name;
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
					std::error_code ec;
					const std::string rel = std::filesystem::relative(
						static_cast<const char*>(pl->Data),
						ctx.contentManager ? ctx.contentManager->contentRoot() : "", ec).generic_string();
					if (!ec && !rel.empty() && rel.rfind("..", 0) != 0 && ctx.contentManager)
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

		const bool changed = GraphEditor::draw("##asm_graphcanvas", m, geState, avail);
		if (changed) structuralEdit = true;
	}
	ImGui::EndChild();

	ImGui::SameLine();

	// ── Right: runtime readout + transitions/params fine-editing ─────────────
	ImGui::BeginChild("##asmSide", ImVec2(rightW - 8.0f, 0), ImGuiChildFlags_Borders);
	{
		ImGui::TextDisabled("Runtime");
		ImGui::LabelText("Current##sm", "%s", sm->currentStateName.empty() ? "(none)" : sm->currentStateName.c_str());
		ImGui::DragFloat("Speed##sm", &sm->playbackSpeed, 0.01f, -4.0f, 4.0f, "%.2f");
		structuralEdit |= ImGui::IsItemDeactivatedAfterEdit();
		if (sm->inTransition)
		{
			ImGui::LabelText("-> ##sm", "%s", sm->transitionTarget.c_str());
			const float pct = sm->transitionDuration > 0.0f
				? sm->transitionElapsed / sm->transitionDuration : 0.0f;
			ImGui::ProgressBar(std::min(pct, 1.0f), ImVec2(-1, 0), "crossfade");
		}
		ImGui::Separator();

		if (ImGui::TreeNodeEx("Transitions##sm", ImGuiTreeNodeFlags_DefaultOpen))
		{
			const char* opNames[] = { ">", "<", "==" };
			int transToDelete = -1;
			for (int i = 0; i < static_cast<int>(sm->transitions.size()); ++i)
			{
				auto& t = sm->transitions[i];
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
				{ t.op = static_cast<TransitionOp>(opIdx); structuralEdit = true; }
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
			{ sm->transitions.erase(sm->transitions.begin() + transToDelete); structuralEdit = true; }
			if (ImGui::SmallButton("+ Transition##sm")) { sm->transitions.push_back({}); structuralEdit = true; }
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Params##sm", ImGuiTreeNodeFlags_None))
		{
			std::string paramToDelete;
			for (auto& [k, v] : sm->params)
			{
				ImGui::PushID(k.c_str());
				ImGui::SetNextItemWidth(-40.0f);
				ImGui::DragFloat(k.c_str(), &v, 0.01f, -999.0f, 999.0f, "%.2f");
				structuralEdit |= ImGui::IsItemDeactivatedAfterEdit();
				ImGui::SameLine();
				if (ImGui::SmallButton("X")) paramToDelete = k;
				ImGui::PopID();
			}
			if (!paramToDelete.empty()) { sm->params.erase(paramToDelete); structuralEdit = true; }
			static char s_newParamName[64] = "";
			ImGui::SetNextItemWidth(-70.0f);
			ImGui::InputText("##newParamName", s_newParamName, sizeof(s_newParamName));
			ImGui::SameLine();
			if (ImGui::SmallButton("+ Param") && s_newParamName[0] != '\0')
			{
				sm->params[s_newParamName] = 0.0f;
				s_newParamName[0] = '\0';
				structuralEdit = true;
			}
			ImGui::TreePop();
		}
	}
	ImGui::EndChild();

	if (structuralEdit && ctx.undoSys) ctx.undoSys->snapshotNow();

	ImGui::End();
}

} // namespace AnimatorStateMachineEditorPanel
