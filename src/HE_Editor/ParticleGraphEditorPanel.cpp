#include "ParticleGraphEditorPanel.h"
#include "EditorApplication.h" // AppContext
#include "GraphEditor.h"       // shared node-graph canvas frontend
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
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
#include <filesystem>
#include <map>
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
	st.assetId = ctx.contentManager->loadAsset(st.relPath);

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
	static std::map<std::string, bool> s_typeCache;
	if (auto it = s_typeCache.find(path); it != s_typeCache.end()) return it->second;
	HAsset::Reader r;
	const bool isPt = r.open(path) &&
		r.assetType() == static_cast<uint16_t>(HE::AssetType::ParticleSystem);
	s_typeCache[path] = isPt;
	return isPt;
}

void forget(const std::string& assetPath) { g_states.erase(assetPath); }

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
// the Material/HorizonCode editors use for their own node-body widgets).
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
	ImGui::Begin("##ParticleGraphTab", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	ParticleGraphAsset* asset = ctx.contentManager
		? ctx.contentManager->getParticleGraphMutable(st.assetId) : nullptr;

	// ── Header ───────────────────────────────────────────────────────────────
	ImGui::TextUnformatted(st.name.c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("particle system%s", st.dirty ? "  (unsaved)" : "");
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 100.0f);
	if (ImGui::Button("Save##ptsave") && asset)
	{
		asset->nodeGraphJson = HE::particleGraphToJson(st.graph);
		if (ctx.contentManager->saveAsset(*asset)) st.dirty = false;
		// Live entities already using this asset should reflect the edit now,
		// not only the next time their own particleAssetId changes — same idea
		// as InvalidateMaterial after a Material save.
		if (ctx.world)
			for (auto [e, ps] : ctx.world->registry().view<ParticleSystemComponent>().each())
				if (ps.particleAssetId == st.assetId) ParticleSystem::markConfigDirty(ps);
		Logger::Log(Logger::LogLevel::Info, ("ParticleGraphEditor: saved '" + st.name + "'").c_str());
	}
	if (!asset)
		ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Asset could not be loaded.");
	ImGui::Separator();

	bool structuralEdit = false;

	// ── Left: live preview ──────────────────────────────────────────────────
	const float leftW = 280.0f;
	ImGui::BeginChild("##ptLeft", ImVec2(leftW, 0), ImGuiChildFlags_Borders);
	{
		ImGui::TextDisabled("Preview");
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
		if (ImGui::Button(st.previewPlaying ? "Pause" : "Play")) st.previewPlaying = !st.previewPlaying;

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
			ImGui::InvisibleButton("##ptOrbit", ImVec2(std::max(av.x, 1.0f), std::max(av.y, 1.0f)));
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
			{
				const ImVec2 md = ImGui::GetIO().MouseDelta;
				st.previewYaw   -= md.x * 0.01f;
				st.previewPitch  = std::clamp(st.previewPitch + md.y * 0.01f, -1.45f, 1.45f);
			}
			if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f)
				st.previewDist = std::clamp(st.previewDist - ImGui::GetIO().MouseWheel * 0.1f, 0.3f, 8.0f);
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
							if (dropped != HE::UUID{} && ctx.contentManager->assetType(dropped) == want)
							{
								target = dropped;
								structuralEdit = true;
							}
						}
					}
					ImGui::EndDragDropTarget();
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
