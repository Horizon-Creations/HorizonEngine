#include "ParticleGraphEditorPanel.h"
#include "EditorToolbar.h"   // shared toolbar strip

#include <cstdio>
#include <cstdint>
#include "EditorApplication.h"      // AppContext
#include "EditorAssetTypeCache.h"   // shared, invalidatable path → AssetType sniff
#include "EditorInput.h"            // pointer-device grammar (trackpad swipe vs mouse wheel)
#include "EditorPanelState.h"       // shared per-tab state map + lazy asset open
#include "EditorWidgets.h"          // shared Content-Browser asset drop target
#include "GraphEditor.h"            // shared node-graph canvas frontend
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ParticleGraph/ParticleGraph.h>
#include <HorizonScene/ParticleSystem.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <Types/Enums.h>
#include <Diagnostics/Logger.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <random>
#include <string>
#include <vector>

namespace ParticleGraphEditorPanel
{

struct State
{
	bool        loaded = false;
	std::string relPath, name;
	HE::UUID    assetId;
	HE::ParticleGraph graph;
	bool        dirty = false;
	// Last state the peers have seen — see CollabDocSync.
	CollabDocSync::DocMirror collabMirror;

	GraphEditor::State geState;

	// Live preview — scratch simulation + resolved config, re-evaluated only when
	// the graph actually changed (not every frame: RandomRange would otherwise
	// re-roll and visibly flicker — same discipline as ParticleSystem::update's
	// resolvedConfig cache on ParticleSystemComponent).
	std::string               lastEvaluatedJson;
	HE::ParticleEmitterConfig previewConfig;
	std::vector<Particle>     previewParticles;
	float                     previewEmitAccumulator = 0.0f;
	std::mt19937              previewRng{ 1337 };
	bool                      previewPlaying = true;
	float previewYaw = 0.6f, previewPitch = 0.2f, previewDist = 1.6f;
};

static AssetPanelState<State> s_states;

static State& stateFor(const std::string& path, AppContext& ctx)
{
	State& st = s_states[path];
	if (st.loaded || !ctx.contentManager) return st;

	st.assetId = openPanelAsset(ctx, path, st.name, st.relPath);

	st.graph = HE::ParticleGraph::makeDefault();
	if (const ParticleGraphAsset* asset = ctx.contentManager->getParticleGraph(st.assetId);
	    asset && !asset->nodeGraphJson.empty())
	{
		HE::ParticleGraph parsed;
		if (HE::particleGraphFromJson(asset->nodeGraphJson, parsed)) st.graph = std::move(parsed);
	}

	st.loaded = true;
	return st;
}

bool isParticleAsset(const std::string& path)
{
	return EditorAssetTypeCache::is(path, HE::AssetType::ParticleSystem);
}

bool isDirty(const std::string& assetPath) { return s_states.dirty(assetPath); }

void appendDirtyPaths(std::vector<std::string>& out) { s_states.appendDirtyPaths(out); }
CollabDocSync::DocBindings collabDocs(const std::string& assetPath)
{
	State* st = s_states.find(assetPath);
	if (!st || !st->loaded) return {};
	CollabDocSync::DocBindings out;
	out.push_back({ CollabDocSync::Scope::Primary,
	                CollabDocSync::forParticleGraph(st->graph), &st->collabMirror });
	return out;
}

void forget(const std::string& assetPath) { s_states.forget(assetPath); }

// Persist a tab's graph. The header's Save button AND the close/quit prompt's
// "Save All" both come through here, so the two can never drift apart.
static bool saveToDisk(State& st, AppContext& ctx)
{
	if (!ctx.contentManager) return false;
	ParticleGraphAsset* asset = ctx.contentManager->getParticleGraphMutable(st.assetId);
	if (!asset) return false;
	asset->nodeGraphJson = HE::particleGraphToJson(st.graph);
	if (!ctx.contentManager->saveAsset(*asset)) return false;
	st.dirty = false;
	// Live entities already using this asset should reflect the edit now, not only
	// the next time their own particleAssetId changes — same idea as
	// InvalidateMaterial after a Material save.
	if (ctx.world)
		for (auto [e, ps] : ctx.world->registry().view<ParticleSystemComponent>().each())
			if (ps.particleAssetId == st.assetId) ParticleSystem::markConfigDirty(ps);
	HE_LOG_INFO(Editor, "%s", ("ParticleGraphEditor: saved '" + st.name + "'").c_str());
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
ImU32 headerColorFor(HE::ParticleNodeType t)
{
	using T = HE::ParticleNodeType;
	switch (t)
	{
		case T::EmitterOutput: return GraphEditor::categoryColor("Material");   // the sink
		case T::RandomRange:   return GraphEditor::categoryColor("Procedural");
		case T::Add: case T::Multiply: case T::Lerp: return GraphEditor::categoryColor("Math");
		default: return GraphEditor::categoryColor("Literals"); // Const*
	}
}

ImU32 pinColorFor(HE::ParticlePinType t)
{
	return t == HE::ParticlePinType::Vec3 ? IM_COL32(140, 180, 255, 255) : IM_COL32(180, 220, 140, 255);
}

float nodeBodyHeightFor(HE::ParticleNodeType t)
{
	using T = HE::ParticleNodeType;
	switch (t)
	{
		case T::EmitterOutput: return 52.0f; // mesh slot + material slot, stacked
		case T::ConstFloat: case T::ConstVec3: case T::ConstColor: case T::RandomRange: return 26.0f;
		default: return 0.0f; // Add/Multiply/Lerp — pins say it all, no body needed
	}
}

// Scale embedded ImGui widgets to the canvas zoom — FramePadding/ItemSpacing are
// pixel-space and won't track a shrunken node box on their own (same technique
// the Material/HorizonCode editors use). The one copy lives with the canvas.
using GraphEditor::pushWidgetScale;
using GraphEditor::popWidgetScale;
} // namespace

void render(AppContext& ctx, const std::string& assetPath, const ImVec2& pos, const ImVec2& size)
{
	State& st = stateFor(assetPath, ctx);

	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin("##ParticleGraphTab", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	ParticleGraphAsset* asset = ctx.contentManager
		? ctx.contentManager->getParticleGraphMutable(st.assetId) : nullptr;

	// ── Toolbar ──────────────────────────────────────────────────────────────
	// Same strip as the Scene viewport and Source Control: name and unsaved mark
	// on the left, the preview transport in its own well, Save on the right.
	{
		namespace T = EditorToolbar;
		T::Bar bar;
		T::assetHeader(bar, st.name.c_str(), T::iconSparkle, st.dirty);

		bar.group();
		if (bar.item("##ptplay", st.previewPlaying ? T::iconPause : T::iconPlay, nullptr,
		             st.previewPlaying, true,
		             st.previewPlaying ? "Pause the preview" : "Play the preview"))
		{
			st.previewPlaying = !st.previewPlaying;
		}
		bar.endGroup();

		if (!asset) bar.label("Asset could not be loaded", T::kBad);
		if (T::saveButton(bar, asset != nullptr)) saveToDisk(st, ctx);
	}

	bool structuralEdit = false;

	// ── Left: live preview ──────────────────────────────────────────────────
	const float leftW = 280.0f;
	ImGui::BeginChild("##ptLeft", ImVec2(leftW, 0), ImGuiChildFlags_Borders);
	{
		// The transport lives in the toolbar now — one place for "is this
		// running", the same place the Scene bar keeps it.
		ImGui::TextDisabled("Preview");

		// Re-evaluate only on an actual graph change (see State's comment).
		const std::string curJson = HE::particleGraphToJson(st.graph);
		if (curJson != st.lastEvaluatedJson)
		{
			st.previewConfig          = HE::evaluateParticleGraph(st.graph, st.previewRng);
			st.lastEvaluatedJson      = curJson;
			st.previewParticles.clear();
			st.previewEmitAccumulator = 0.0f;
		}

		if (st.previewPlaying)
			ParticleSystem::stepPool(st.previewParticles, st.previewEmitAccumulator, st.previewRng,
			                         st.previewConfig, glm::vec3(0.0f), ImGui::GetIO().DeltaTime);

		ImGui::BeginChild("##ptPreview", ImVec2(0, 240), ImGuiChildFlags_Borders);
		{
			const ImVec2 org = ImGui::GetCursorScreenPos();
			const ImVec2 av  = ImGui::GetContentRegionAvail();
			const float  px  = std::max(64.0f, std::min(av.x, av.y));

			std::vector<ParticlePreviewInstance> instances;
			instances.reserve(st.previewParticles.size());
			for (const auto& p : st.previewParticles)
			{
				const float t01 = 1.0f - p.lifetime / p.maxLifetime; // 0=born, 1=dead
				const float sz  = st.previewConfig.startSize +
					(st.previewConfig.endSize - st.previewConfig.startSize) * t01;
				if (sz <= 0.0f) continue;
				ParticlePreviewInstance inst;
				inst.position = p.position;
				inst.size     = sz;
				inst.color.r  = st.previewConfig.startColor[0] +
					(st.previewConfig.endColor[0] - st.previewConfig.startColor[0]) * t01;
				inst.color.g  = st.previewConfig.startColor[1] +
					(st.previewConfig.endColor[1] - st.previewConfig.startColor[1]) * t01;
				inst.color.b  = st.previewConfig.startColor[2] +
					(st.previewConfig.endColor[2] - st.previewConfig.startColor[2]) * t01;
				inst.alpha    = st.previewConfig.startAlpha +
					(st.previewConfig.endAlpha - st.previewConfig.startAlpha) * t01;
				instances.push_back(inst);
			}

			void* tex = (ctx.renderer && ctx.contentManager && px >= 32.0f)
				? ctx.renderer->RenderParticlePreview(*ctx.contentManager, st.previewConfig.meshAssetId,
					st.previewConfig.materialAssetId, instances, static_cast<uint32_t>(px),
					st.previewYaw, st.previewPitch, st.previewDist)
				: nullptr;

			if (tex)
			{
				const bool flipY = (ctx.backend == HE::RendererBackend::OpenGL);
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (av.x - px) * 0.5f);
				ImGui::Image(reinterpret_cast<ImTextureID>(tex), ImVec2(px, px),
					flipY ? ImVec2(0, 1) : ImVec2(0, 0), flipY ? ImVec2(1, 0) : ImVec2(1, 1));
			}
			else
				ImGui::TextDisabled("(preview unavailable on this backend)");

			// Orbit interaction over the whole preview pane, same feel as the
			// Material/Skeletal-Mesh preview panes.
			ImGui::SetCursorScreenPos(org);
			// Right-drag orbits too — same muscle memory as the RMB-steered viewport.
			ImGui::InvisibleButton("##ptOrbit", ImVec2(std::max(av.x, 1.0f), std::max(av.y, 1.0f)),
				ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
			// Claim the wheel over the preview so a swipe/zoom never also scrolls
			// the surrounding column.
			ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
			ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
			if (ImGui::IsItemActive() &&
			    (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Right)))
			{
				const ImVec2 md = ImGui::GetIO().MouseDelta;
				st.previewYaw   -= md.x * 0.01f;
				st.previewPitch  = std::clamp(st.previewPitch + md.y * 0.01f, -1.45f, 1.45f);
			}
			ImGuiIO& pio = ImGui::GetIO();
			if (ImGui::IsItemHovered() && (pio.MouseWheel != 0.0f || pio.MouseWheelH != 0.0f))
			{
				// Trackpad grammar: two-finger swipe orbits, Cmd/Ctrl+scroll zooms.
				// Mouse grammar: wheel zooms, exactly as before.
				const bool zoomMod = pio.KeyCtrl || pio.KeySuper;
				if (EditorInput::trackpadPointer(ctx) && !zoomMod)
				{
					st.previewYaw   -= pio.MouseWheelH * 0.08f;
					st.previewPitch  = std::clamp(st.previewPitch + pio.MouseWheel * 0.08f, -1.45f, 1.45f);
				}
				else if (pio.MouseWheel != 0.0f)
					st.previewDist = std::clamp(st.previewDist - pio.MouseWheel * 0.1f, 0.3f, 8.0f);
			}
		}
		ImGui::EndChild();

		ImGui::Text("Live particles: %zu", st.previewParticles.size());
		ImGui::TextDisabled("Emit %.1f/s  Life %.2f-%.2fs  Max %d",
			st.previewConfig.emitRate, st.previewConfig.lifetimeMin, st.previewConfig.lifetimeMax,
			st.previewConfig.maxParticles);
	}
	ImGui::EndChild();

	ImGui::SameLine();

	// ── Right: node graph canvas ────────────────────────────────────────────
	ImGui::BeginChild("##ptCanvas", ImVec2(0, 0), ImGuiChildFlags_Borders);
	const ImVec2 avail = ImGui::GetContentRegionAvail();

	GraphEditor::Model m;
	m.nodeIds = [&st]{
		std::vector<int> ids; ids.reserve(st.graph.nodes.size());
		for (const auto& n : st.graph.nodes) ids.push_back(n.id);
		return ids;
	};
	m.getPos = [&st](int id, float& x, float& y) {
		if (const HE::ParticleGraphNode* n = st.graph.findNode(id)) { x = n->x; y = n->y; }
	};
	m.setPos = [&st](int id, float x, float y) {
		if (HE::ParticleGraphNode* n = st.graph.findNode(id)) { n->x = x; n->y = y; }
	};
	m.title = [&st](int id) -> std::string {
		const HE::ParticleGraphNode* n = st.graph.findNode(id);
		return n ? HE::particleNodeDesc(n->type).name : std::string();
	};
	m.headerColor = [&st](int id) -> ImU32 {
		const HE::ParticleGraphNode* n = st.graph.findNode(id);
		return n ? headerColorFor(n->type) : IM_COL32(90, 90, 90, 255);
	};
	m.pins = [&st](int id) -> std::vector<GraphEditor::Pin> {
		const HE::ParticleGraphNode* n = st.graph.findNode(id);
		if (!n) return {};
		const HE::ParticleNodeDesc& d = HE::particleNodeDesc(n->type);
		std::vector<GraphEditor::Pin> pins;
		for (size_t i = 0; i < d.inputs.size(); ++i)
			pins.push_back({ static_cast<int>(i), d.inputs[i].name, pinColorFor(d.inputs[i].type), true, false });
		for (size_t i = 0; i < d.outputs.size(); ++i)
			pins.push_back({ static_cast<int>(i), d.outputs[i].name, pinColorFor(d.outputs[i].type), false, false });
		return pins;
	};
	m.links = [&st]{
		std::vector<std::array<int, 4>> ls;
		ls.reserve(st.graph.links.size());
		for (const auto& l : st.graph.links) ls.push_back({ l.srcNode, l.srcPin, l.dstNode, l.dstPin });
		return ls;
	};
	m.connect = [&st](int outNode, int outPin, int inNode, int inPin) {
		return st.graph.connect(outNode, outPin, inNode, inPin);
	};
	m.clearPinLinks = [&st](int node, int pin, bool input) {
		if (input) { st.graph.disconnectInput(node, pin); return; }
		st.graph.links.erase(std::remove_if(st.graph.links.begin(), st.graph.links.end(),
			[&](const HE::ParticleGraphLink& l) { return l.srcNode == node && l.srcPin == pin; }),
			st.graph.links.end());
	};
	m.removeNode = [&st](int id) { st.graph.removeNode(id); };
	m.drawAddMenu = [&st]() -> int {
		struct Entry { HE::ParticleNodeType type; const char* label; };
		static const Entry kEntries[] = {
			{ HE::ParticleNodeType::ConstFloat,  "Const Float" },
			{ HE::ParticleNodeType::ConstVec3,   "Const Vec3" },
			{ HE::ParticleNodeType::ConstColor,  "Const Color" },
			{ HE::ParticleNodeType::RandomRange, "Random Range" },
			{ HE::ParticleNodeType::Add,         "Add" },
			{ HE::ParticleNodeType::Multiply,    "Multiply" },
			{ HE::ParticleNodeType::Lerp,        "Lerp" },
		};
		int created = 0;
		for (const Entry& e : kEntries)
			if (ImGui::Selectable(e.label))
			{
				created = st.graph.addNode(e.type, st.geState.addMenuGraphPos.x, st.geState.addMenuGraphPos.y);
				ImGui::CloseCurrentPopup();
			}
		return created;
	};
	m.nodeBodyHeight = [&st](int id) -> float {
		const HE::ParticleGraphNode* n = st.graph.findNode(id);
		return n ? nodeBodyHeightFor(n->type) : 0.0f;
	};
	m.drawNodeBody = [&st, &structuralEdit, &ctx](int id, ImVec2 bodyMin, ImVec2 bodyMax, float zoom)
	{
		HE::ParticleGraphNode* n = st.graph.findNode(id);
		if (!n) return;
		using T = HE::ParticleNodeType;

		if (n->type == T::EmitterOutput)
		{
			// Two stacked drag-drop slots (mesh, material) — the whole row is the
			// drop target, matching Material's TextureSample node-body pattern.
			auto slot = [&](float rowY, const char* label, HE::UUID& target, HE::AssetType want)
			{
				ImGui::SetCursorScreenPos(ImVec2(bodyMin.x, rowY));
				pushWidgetScale(zoom);
				const std::string state = (target == HE::UUID{}) ? std::string("(default)")
					: (ctx.contentManager && ctx.contentManager->assetType(target) == want
						? std::string("set") : std::string("(missing)"));
				ImGui::TextDisabled("%s: %s", label, state.c_str());
				popWidgetScale();
				ImGui::SetCursorScreenPos(ImVec2(bodyMin.x, rowY));
				ImGui::PushID(label);
				ImGui::SetNextItemAllowOverlap();
				ImGui::InvisibleButton("##slot", ImVec2(std::max(bodyMax.x - bodyMin.x, 1.0f), 22.0f * zoom));
				// The graph's own JSON undo stack covers this (structuralEdit), so no
				// world snapshot — hence the drop half only, not the whole slot widget.
				if (const EditorWidgets::AssetDrop drop = EditorWidgets::acceptAssetDrop(ctx, want))
				{
					target = drop.id;
					structuralEdit = true;
				}
				ImGui::PopID();
			};
			slot(bodyMin.y,                  "Mesh",     n->meshAssetId,     HE::AssetType::StaticMesh);
			slot(bodyMin.y + 26.0f * zoom,   "Material", n->materialAssetId, HE::AssetType::Material);
			return;
		}

		ImGui::SetCursorScreenPos(bodyMin);
		pushWidgetScale(zoom);
		const float w = (GraphEditor::kNodeW - 24.0f) * zoom;
		switch (n->type)
		{
			case T::ConstFloat:
				ImGui::SetNextItemWidth(w);
				ImGui::DragFloat("##v", &n->p[0], 0.01f);
				structuralEdit |= ImGui::IsItemDeactivatedAfterEdit();
				break;
			case T::ConstVec3:
				ImGui::SetNextItemWidth(w);
				ImGui::DragFloat3("##v3", n->p, 0.01f);
				structuralEdit |= ImGui::IsItemDeactivatedAfterEdit();
				break;
			case T::ConstColor:
				ImGui::SetNextItemWidth(w);
				ImGui::ColorEdit3("##c", n->p, ImGuiColorEditFlags_Float);
				structuralEdit |= ImGui::IsItemDeactivatedAfterEdit();
				break;
			case T::RandomRange:
				ImGui::SetNextItemWidth(w * 0.48f);
				ImGui::DragFloat("##min", &n->p[0], 0.01f);
				structuralEdit |= ImGui::IsItemDeactivatedAfterEdit();
				ImGui::SameLine();
				ImGui::SetNextItemWidth(w * 0.48f);
				ImGui::DragFloat("##max", &n->p[1], 0.01f);
				structuralEdit |= ImGui::IsItemDeactivatedAfterEdit();
				break;
			default: break;
		}
		popWidgetScale();
	};

	const bool changed = GraphEditor::draw("##particle_graphcanvas", m, st.geState, avail);
	if (changed) structuralEdit = true;

	ImGui::EndChild();

	if (structuralEdit)
	{
		st.dirty = true;
		if (ctx.undoSys) ctx.undoSys->snapshotNow();
	}

	ImGui::End();
}

} // namespace ParticleGraphEditorPanel
