#include "AnimatorStateMachineEditorPanel.h"
#include "EditorToolbar.h"   // shared toolbar strip
#include <cstdint>
#include "EditorApplication.h"      // AppContext
#include "EditorAssetTypeCache.h"   // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"       // shared per-tab state map + lazy asset open
#include "EditorWidgets.h"          // shared Content-Browser asset drop target
#include "GraphEditor.h"            // shared node-graph canvas frontend
#include "HcGraphHost.h"            // the HorizonCode half of that canvas (sync graph)
#include "HcEditorUtil.h"
#include <HorizonCode/HorizonCode.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <AnimatorStateMachine/AnimatorStateMachineGraph.h>
#include <HorizonScene/AnimationStateMachineSystem.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <Types/Enums.h>
#include <Diagnostics/Logger.h>
#include <algorithm>
#include <array>
#include <cstdio>
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
	// Last state the peers have seen — see CollabDocSync.
	CollabDocSync::DocMirror collabMirror;

	GraphEditor::State geState;

	// ── The sync graph ───────────────────────────────────────────────────────
	// The second half of the asset: a HorizonCode graph fired once per frame per
	// animated entity, whose one job is to read that character and write this
	// machine's parameters. Unreal keeps the same two things in one Animation
	// Blueprint — EventGraph next to AnimGraph — and for the same reason: the
	// parameters are the seam between them, so authoring them apart would mean
	// keeping two files in step by hand.
	bool               showSync = false;   // which of the two the canvas shows
	HorizonCode::Graph syncGraph;
	GraphEditor::State syncGe;
	int                syncSelected = 0;
};

static AssetPanelState<State> s_states;

static State& stateFor(const std::string& path, AppContext& ctx)
{
	State& st = s_states[path];
	if (st.loaded || !ctx.contentManager) return st;

	st.assetId = openPanelAsset(ctx, path, st.name, st.relPath);

	if (const AnimatorStateMachineAsset* asset = ctx.contentManager->getAnimatorStateMachine(st.assetId);
	    asset && !asset->graphJson.empty())
	{
		HE::AnimatorStateMachineGraph parsed;
		if (HE::animatorStateMachineFromJson(asset->graphJson, parsed)) st.graph = std::move(parsed);
	}
	if (const AnimatorStateMachineAsset* asset = ctx.contentManager->getAnimatorStateMachine(st.assetId);
	    asset && !asset->syncGraphJson.empty())
	{
		HorizonCode::Graph parsed;
		if (HorizonCode::fromJson(asset->syncGraphJson, parsed)) st.syncGraph = std::move(parsed);
	}

	// A sync graph has exactly one entry point and it is not optional, so it is
	// there from the start rather than something to remember. Seeded WITHOUT
	// marking dirty: merely opening the view must not decide to write a chunk
	// into an asset that never had one.
	if (st.syncGraph.nodes.empty())
	{
		HorizonCode::Node ev;
		ev.type = HorizonCode::NodeType::Event;
		ev.s = "Update";
		ev.hasArg = true; ev.propType = HorizonCode::PinType::Float;   // dt
		ev.x = 60.0f; ev.y = 60.0f;
		st.syncGraph.addNode(std::move(ev));
	}

	st.loaded = true;
	return st;
}

bool isAnimatorStateMachineAsset(const std::string& path)
{
	return EditorAssetTypeCache::is(path, HE::AssetType::AnimatorStateMachine);
}

bool isDirty(const std::string& assetPath) { return s_states.dirty(assetPath); }

void appendDirtyPaths(std::vector<std::string>& out) { s_states.appendDirtyPaths(out); }
CollabDocSync::DocBindings collabDocs(const std::string& assetPath)
{
	State* st = s_states.find(assetPath);
	if (!st || !st->loaded) return {};
	CollabDocSync::DocBindings out;
	out.push_back({ CollabDocSync::Scope::Primary,
	                CollabDocSync::forAnimatorGraph(st->graph), &st->collabMirror });
	return out;
}

void forget(const std::string& assetPath) { s_states.forget(assetPath); }

// Persist a tab's graph. The header's Save button AND the close/quit prompt's
// "Save All" both come through here, so the two can never drift apart.
static bool saveToDisk(State& st, AppContext& ctx)
{
	if (!ctx.contentManager) return false;
	AnimatorStateMachineAsset* asset = ctx.contentManager->getAnimatorStateMachineMutable(st.assetId);
	if (!asset) return false;
	asset->graphJson = HE::animatorStateMachineToJson(st.graph);
	// An UNTOUCHED sync graph is written as no chunk at all, so an asset that
	// never got one stays byte-identical to what it was before sync graphs
	// existed. "Untouched" is not "empty": the view seeds the Update event on
	// open, so the bare seed — one event, nothing wired, nothing declared — has
	// to count as nothing too.
	const bool syncUntouched =
		st.syncGraph.links.empty() && st.syncGraph.variables.empty() &&
		(st.syncGraph.nodes.empty() ||
		 (st.syncGraph.nodes.size() == 1 &&
		  st.syncGraph.nodes[0].type == HorizonCode::NodeType::Event &&
		  st.syncGraph.nodes[0].s == "Update"));
	asset->syncGraphJson = syncUntouched ? std::string() : HorizonCode::toJson(st.syncGraph);
	if (!ctx.contentManager->saveAsset(*asset)) return false;
	st.dirty = false;
	// Live entities already using this asset should reflect the edit now, not only
	// the next time their own stateMachineAssetId changes — same idea as
	// InvalidateMaterial after a Material save.
	if (ctx.world)
		for (auto [e, sm] : ctx.world->registry().view<AnimatorStateMachineComponent>().each())
			if (sm.stateMachineAssetId == st.assetId) AnimationStateMachineSystem::markConfigDirty(sm);
	HE_LOG_INFO(Editor, "%s",
		("AnimatorStateMachineEditor: saved '" + st.name + "'").c_str());
	return true;
}

bool reloadFromDisk(const std::string& assetPath)
{
	// A collaboration peer's change just landed in the file. Dropping `loaded`
	// makes the next frame re-read it while the rest of the State survives.
	// Dirty is cleared deliberately: while a peer holds the asset's lock this
	// panel is read-only anyway, so anything "unsaved" here is stale.
	auto* st = s_states.find(assetPath);
	if (!st) return false;
	st->loaded = false;
	st->dirty = false;
	// The mirror describes the document that is about to be replaced. Leaving
	// it would make the first diff after the reload report the difference
	// between the peer's file and our old graph as OUR edit.
	st->collabMirror = {};
	return true;
}


bool save(AppContext& ctx, const std::string& assetPath)
{
	State* st = s_states.find(assetPath);
	// A tab this panel never opened has nothing to write — the caller asks every
	// panel about every path, so "not mine" must read as success.
	if (!st || !st->dirty) return true;
	return saveToDisk(*st, ctx);
}

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
// HorizonCode/ParticleGraph node bodies use) — the one copy lives with the canvas.
using GraphEditor::pushWidgetScale;
using GraphEditor::popWidgetScale;

// ── What a sync graph is allowed to do ───────────────────────────────────────
// Deliberately narrow. This graph runs inside the animation phase, once per
// frame for every character the machine animates, and its job is one sentence:
// read the character, write the parameters. A graph that can also switch scenes
// or spawn objects is a graph someone will eventually use to do that, from a
// place in the frame where it is the wrong thing to do.
//
// The restriction is data, honoured by both the add menu and the drag-off menu
// (see HcGraphHost::MenuOpts::apiGroups) — filtering one and not the other
// leaves the palette looking restricted while a pin drag hands over everything.
const HcGraphHost::MenuOpts& syncMenus()
{
	static const HcGraphHost::MenuOpts m = []{
		HcGraphHost::MenuOpts o;
		// "Flow" is in, but pared down. Not because control flow is wanted here
		// — the graph only pulls values off its owner — but because Cast is an
		// EXEC node, so a graph with no exec flow at all could not cast. What
		// goes is everything that makes a graph remember something across
		// frames or split into branches of work: this thing runs once a frame
		// and computes numbers.
		o.addCategories = { "Flow", "Reference", "Literals", "Math", "Logic",
		                    "Variables", "Debug" };
		o.addExcluded = { HorizonCode::NodeType::Sequence, HorizonCode::NodeType::Delay,
		                  HorizonCode::NodeType::DoOnce,   HorizonCode::NodeType::FlipFlop };
		// The same list again for the drag-off menu. It walks the registry flat
		// rather than by category, so an exclusion that lives only above is one
		// you get back by dragging off a pin.
		o.dragExcluded = o.addExcluded;
		o.apiGroups = {
			"animator",   // the point of the graph
			"entity",     // entity.self — how it reaches the character it animates
			"movement",   // what that character is doing: speed, grounded, direction
			"transform",  // where it is and how it is turned
			"physics",    // a ray under the feet, when speed and grounded aren't enough
			"math",       // turning those into the numbers a transition wants
			// NOT "locomotion": those move the character, and this graph runs in
			// the animation phase — a transform write from here lands after
			// physics has already run.
		};
		return o;
	}();
	return m;
}
} // namespace

// Declared here because render() dispatches to it before the definition below.
static void drawSyncGraph(AppContext& ctx, State& st);

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

	// ── Toolbar ──────────────────────────────────────────────────────────────
	{
		namespace T = EditorToolbar;
		T::Bar bar;
		T::assetHeader(bar, st.name.c_str(), T::iconBone, st.dirty);

		// The two counts that say whether the machine is worth looking at, as a
		// readout rather than a sentence — nothing here is clickable and it
		// should not pretend to be.
		char counts[64];
		std::snprintf(counts, sizeof(counts), "%zu state%s, %zu transition%s",
		              st.graph.states.size(), st.graph.states.size() == 1 ? "" : "s",
		              st.graph.transitions.size(),
		              st.graph.transitions.size() == 1 ? "" : "s");
		bar.group();
		bar.readout(T::iconLayers, counts, T::kFgDim);
		bar.endGroup();

		// The asset's two halves. Same radio pair the class editor uses for
		// Graph/Components, for the same reason: they are two views of one
		// document, not two documents.
		bar.group();
		if (bar.item("##asmmodestates", nullptr, "States", !st.showSync, true,
		             "The machine itself — states, transitions, parameters"))
			st.showSync = false;
		if (bar.item("##asmmodesync", nullptr, "Sync Graph", st.showSync, true,
		             "Runs once per frame per animated character: read it, write the parameters"))
			st.showSync = true;
		bar.endGroup();

		if (!asset) bar.label("Asset could not be loaded", T::kBad);
		if (T::saveButton(bar, asset != nullptr)) saveToDisk(st, ctx);
	}

	if (st.showSync)
	{
		drawSyncGraph(ctx, st);
		ImGui::End();
		return;
	}

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
			// The graph's own dirty flag covers this (structuralEdit), so no world
			// snapshot — hence the drop half only, not the whole slot widget.
			if (const EditorWidgets::AssetDrop drop =
					EditorWidgets::acceptAssetDrop(ctx, HE::AssetType::AnimationClip))
			{
				s->clipId = drop.id;
				structuralEdit = true;
			}
			popWidgetScale();
		};

		const bool changed = GraphEditor::draw("##asm_graphcanvas", m, st.geState, avail);
		// liveEdit is the same answer mid-gesture: a node being dragged has
		// already moved. Folded in here because structuralEdit only marks the
		// graph dirty — no undo snapshot to spam.
		if (changed || st.geState.liveEdit) structuralEdit = true;
	}
	ImGui::EndChild();

	ImGui::SameLine();

	// ── Right: transitions/params fine-editing ────────────────────────────────
	// GraphEditor links carry no inline widgets — the canvas gives topology +
	// layout, this flat list gives parameter editing (same split Forts. 49's
	// original flat Inspector UI already used).
	ImGui::BeginChild("##asmSide", ImVec2(rightW - 8.0f, 0), ImGuiChildFlags_Borders);
	{
		// This column is 312 px wide and full of things that are longer than that:
		// a transition reads "SomeLongStateName -> SomeOtherLongStateName" and the
		// note under the start-state field is a whole sentence. Without a wrap
		// position ImGui draws them straight past the right edge and clips them, so
		// the reader gets "SomeLongStateName -> SomeOth" and no sign that anything
		// was cut — the same defect as a sideways scrollbar, only quieter. The guard
		// is pushed HERE and not on the tab window because the wrap position lives on
		// the window it was pushed in: it would never reach into this child, and it
		// must never reach into the canvas child next door, where node bodies are
		// placed at computed positions and a wrap would shred the layout.
		EditorWidgets::WrapText wrap;

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
				if (EditorWidgets::dangerSmallButton("Remove##t")) transToDelete = i;
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
				if (EditorWidgets::dangerSmallButton("\xc3\x97")) paramToDelete = k;
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

// ── The sync graph view ──────────────────────────────────────────────────────
// A HorizonCode canvas on the same shared GraphEditor every other graph editor
// uses, with the restricted palette from syncMenus(). The side column lists the
// machine's parameters — the things this graph exists to write — and the
// selected node's details.
static void drawSyncGraph(AppContext& ctx, State& st)
{
	namespace HC = HorizonCode;

	bool edited = false;
	HcGraphHost::Host h;
	h.graph         = &st.syncGraph;
	h.ge            = &st.syncGe;
	h.selectedNode  = &st.syncSelected;
	h.currentGraph  = 0;
	h.content       = ctx.contentManager;
	h.menus         = &syncMenus();
	h.title         = [](const HC::Node& n){ return HcGraphHost::defaultNodeTitle(n); };
	h.onEdit        = [&edited](bool){ edited = true; };

	const float rightW = 300.0f;
	const float leftW  = std::max(200.0f, ImGui::GetContentRegionAvail().x - rightW);

	ImGui::BeginChild("##asmSyncCanvas", ImVec2(leftW, 0), ImGuiChildFlags_Borders);
	{
		GraphEditor::Model model = HcGraphHost::buildModel(h);
		model.drawAddMenu = [&st, &h]() -> int {
			int created = 0;
			const std::string q = HcGraphHost::beginAddMenu();
			// The one event this graph has. Not a catalog, because there is
			// nothing else to pick: a sync graph is called once a frame, and
			// this is that call.
			if (q.empty() || HcGraphHost::lower("Update").find(q) != std::string::npos)
			{
				ImGui::TextDisabled("Events");
				const bool used = [&]{
					for (const auto& n : st.syncGraph.nodes)
						if (n.type == HC::NodeType::Event && n.s == "Update") return true;
					return false;
				}();
				if (HcEditorUtil::searchMenuItem("Update", used))
				{
					const int id = HcGraphHost::addNode(st.syncGraph, HC::NodeType::Event,
					                                    st.syncGe.addMenuGraphPos, 0);
					HC::Node* n = st.syncGraph.findNode(id);
					n->s = "Update";
					n->hasArg = true; n->propType = HC::PinType::Float;   // dt
					created = id;
					ImGui::CloseCurrentPopup();
				}
				if (used) { ImGui::SameLine(); ImGui::TextDisabled("(added)"); }
				ImGui::Spacing();
			}
			if (const int id = HcGraphHost::drawAddMenuTail(h, q)) created = id;
			HcGraphHost::endAddMenu();
			return created;
		};

		const ImVec2 avail = ImGui::GetContentRegionAvail();
		if (GraphEditor::draw("##asmSyncGe", model, st.syncGe, avail) || st.syncGe.liveEdit)
			edited = true;
	}
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild("##asmSyncSide", ImVec2(0, 0), ImGuiChildFlags_Borders);
	{
		ImGui::TextDisabled("%s", "Runs once per frame, for every character this");
		ImGui::TextDisabled("%s", "machine animates. Reach the character with");
		ImGui::TextDisabled("%s", "Get Owning Entity — never \"the player\", or the");
		ImGui::TextDisabled("%s", "same machine animates an NPC with the");
		ImGui::TextDisabled("%s", "player's numbers.");
		ImGui::Separator();

		// The parameters, so the names this graph has to write are in front of
		// you while you write them. Authored on the States side; listed here.
		if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (st.graph.defaultParams.empty())
				ImGui::TextDisabled("None declared — add them under States.");
			for (const auto& [name, value] : st.graph.defaultParams)
				ImGui::BulletText("%s  (default %.3g)", name.c_str(), value);
		}
		ImGui::Separator();

		if (HC::Node* n = st.syncGraph.findNode(st.syncSelected))
		{
			ImGui::TextUnformatted(HC::nodeDisplayName(n->type));
			ImGui::Separator();
			if (HcGraphHost::drawCommonNodeDetails(h, *n)) edited = true;
		}
		else ImGui::TextDisabled("Select a node.");
	}
	ImGui::EndChild();

	if (edited) st.dirty = true;
}

} // namespace AnimatorStateMachineEditorPanel
