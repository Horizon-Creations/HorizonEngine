#include "MaterialEditorPanel.h"
#include "EditorApplication.h"                 // AppContext
#include <MaterialGraph/MaterialGraph.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <Types/Enums.h>
#include <Diagnostics/Logger.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace
{
using HE::MaterialGraph;
using HE::MatGraphNode;
using HE::MatNodeDesc;
using HE::MatNodeType;
using HE::MatPinType;

// ── Per-material editor state (session-persistent, keyed by asset path) ────────
struct State
{
	MaterialGraph graph;
	HE::UUID      materialId{};
	bool          loaded  = false;
	bool          dirty   = false;   // unsaved-to-disk graph edits
	ImVec2        scroll  = ImVec2(40.0f, 40.0f);
	int           selectedNode = 0;
	// Live link drag: source pin (node, pin index, output side?) or inactive (node == 0).
	int  dragNode = 0, dragPin = 0;
	bool dragFromOutput = true;
	std::string lastGlsl;            // generated fragment (debug view)
	std::string name;                // filename for the header
};
std::map<std::string, State> g_states;

// Pin hit-test data collected while drawing, consumed for link routing + drop targets.
struct PinPos { int node; int pin; bool output; ImVec2 pos; MatPinType type; };

constexpr float kNodeW    = 172.0f;
constexpr float kTitleH   = 24.0f;
constexpr float kRowH     = 20.0f;
constexpr float kPinR     = 5.0f;

ImU32 categoryColor(const char* cat)
{
	const std::string c = cat;
	if (c == "Material") return IM_COL32(140,  60,  60, 255);
	if (c == "Input")    return IM_COL32( 60, 100, 140, 255);
	if (c == "Math")     return IM_COL32( 60, 120,  80, 255);
	if (c == "Texture")  return IM_COL32(120,  90, 150, 255);
	return IM_COL32(110, 110,  70, 255); // Shading & misc
}

ImU32 pinColor(MatPinType t)
{
	switch (t)
	{
		case MatPinType::Float: return IM_COL32(160, 200, 120, 255);
		case MatPinType::Vec2:  return IM_COL32(120, 190, 200, 255);
		case MatPinType::Vec3:  return IM_COL32(230, 200, 110, 255);
	}
	return IM_COL32_WHITE;
}

// Regenerate the shader from the graph and push it into the live MaterialAsset. The
// renderers re-resolve the material's shader every frame (pipeline cached per source
// hash), so the scene updates immediately; Save persists to disk.
void applyToMaterial(State& st, AppContext& ctx)
{
	if (!ctx.contentManager) return;
	MaterialAsset* mat = ctx.contentManager->getMaterialMutable(st.materialId);
	if (!mat) return;
	st.lastGlsl = HE::generateFragmentGlsl(st.graph);
	mat->customShaderFragGlsl = st.lastGlsl;
	mat->nodeGraphJson        = HE::materialGraphToJson(st.graph);
	st.dirty = true;
}

State& stateFor(const std::string& path, AppContext& ctx)
{
	State& st = g_states[path];
	if (st.loaded || !ctx.contentManager) return st;

	st.name = std::filesystem::path(path).filename().string();
	// The ContentManager addresses assets by content-root-relative path.
	std::error_code ec;
	const std::string rel = std::filesystem::relative(
		path, ctx.contentManager->contentRoot(), ec).generic_string();
	st.materialId = ctx.contentManager->loadAsset(ec ? path : rel);

	const MaterialAsset* mat = ctx.contentManager->getMaterial(st.materialId);
	if (mat && !mat->nodeGraphJson.empty() &&
	    HE::materialGraphFromJson(mat->nodeGraphJson, st.graph))
	{
		st.lastGlsl = HE::generateFragmentGlsl(st.graph);
	}
	else
	{
		// No graph yet (fresh material, or one with only a hand-written shader): start
		// from the default graph. Nothing is written to the asset until the first edit.
		st.graph = MaterialGraph::makeDefault();
		st.lastGlsl = HE::generateFragmentGlsl(st.graph);
	}
	st.loaded = true;
	return st;
}

// Inline parameter widgets for a node; returns true when an edit was COMMITTED
// (deactivated-after-edit), so constant drags don't rebuild the pipeline every frame.
bool nodeParamWidgets(MatGraphNode& n)
{
	bool committed = false;
	switch (n.type)
	{
		case MatNodeType::ConstFloat:
			ImGui::SetNextItemWidth(kNodeW - 24.0f);
			ImGui::DragFloat("##v", &n.p[0], 0.01f);
			committed = ImGui::IsItemDeactivatedAfterEdit();
			break;
		case MatNodeType::ConstColor:
			ImGui::SetNextItemWidth(kNodeW - 24.0f);
			ImGui::ColorEdit3("##c", n.p, ImGuiColorEditFlags_Float);
			committed = ImGui::IsItemDeactivatedAfterEdit();
			break;
		case MatNodeType::Fresnel:
			ImGui::SetNextItemWidth(kNodeW - 60.0f);
			ImGui::DragFloat("Pow", &n.p[0], 0.05f, 0.01f, 16.0f);
			committed = ImGui::IsItemDeactivatedAfterEdit();
			break;
		case MatNodeType::Output:
		{
			bool lit = n.p[0] > 0.5f;
			if (ImGui::Checkbox("Lit", &lit)) { n.p[0] = lit ? 1.0f : 0.0f; committed = true; }
			break;
		}
		default: break;
	}
	return committed;
}
} // namespace

namespace MaterialEditorPanel
{
bool isMaterialAsset(const std::string& path)
{
	// Called every frame for the active tab (dispatch) — cache the header sniff so we
	// don't re-open the file per frame. An asset's type never changes in place.
	static std::map<std::string, bool> s_typeCache;
	if (auto it = s_typeCache.find(path); it != s_typeCache.end()) return it->second;
	HAsset::Reader r;
	const bool isMat = r.open(path) &&
		r.assetType() == static_cast<uint16_t>(HE::AssetType::Material);
	s_typeCache[path] = isMat;
	return isMat;
}

bool isDirty(const std::string& assetPath)
{
	auto it = g_states.find(assetPath);
	return it != g_states.end() && it->second.dirty;
}

void forget(const std::string& assetPath) { g_states.erase(assetPath); }

void render(AppContext& ctx, const std::string& assetPath,
            const ImVec2& pos, const ImVec2& size)
{
	State& st = stateFor(assetPath, ctx);

	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin("##MaterialGraphTab", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	MaterialAsset* mat = ctx.contentManager
		? ctx.contentManager->getMaterialMutable(st.materialId) : nullptr;

	// ── Header: name, save, hints ──────────────────────────────────────────────
	ImGui::TextUnformatted(st.name.c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("material graph%s", st.dirty ? "  (unsaved)" : "");
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 160.0f);
	if (ImGui::Button("Save Material") && mat)
	{
		applyToMaterial(st, ctx);
		if (ctx.contentManager->saveAsset(*mat)) st.dirty = false;
		Logger::Log(Logger::LogLevel::Info,
			("MaterialEditor: saved '" + st.name + "'").c_str());
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(edits apply live)");
	if (!mat)
		ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Material asset could not be loaded.");
	ImGui::Separator();

	// ── Canvas ──────────────────────────────────────────────────────────────────
	const float glslPaneH = 0.0f; // GLSL view lives in a collapsing header below the canvas
	ImVec2 canvasSize = ImGui::GetContentRegionAvail();
	canvasSize.y -= 28.0f; // keep one row for the GLSL collapsing header
	ImGui::BeginChild("##graphCanvas", canvasSize, ImGuiChildFlags_Borders,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);

	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 avail  = ImGui::GetContentRegionAvail();
	(void)glslPaneH;

	// Grid
	dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(28, 28, 30, 255));
	const float grid = 32.0f;
	for (float x = fmodf(st.scroll.x, grid); x < avail.x; x += grid)
		dl->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + avail.y), IM_COL32(255,255,255,10));
	for (float y = fmodf(st.scroll.y, grid); y < avail.y; y += grid)
		dl->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + avail.x, origin.y + y), IM_COL32(255,255,255,10));

	std::vector<PinPos> pins;
	pins.reserve(st.graph.nodes.size() * 4);
	bool  structuralEdit = false; // connect/disconnect/add/delete → apply immediately
	bool  paramEdit      = false; // committed widget edit → apply
	int   deleteNode     = 0;

	// ── Nodes ──
	for (auto& n : st.graph.nodes)
	{
		const MatNodeDesc& d = HE::matNodeDesc(n.type);
		const ImVec2 p(origin.x + st.scroll.x + n.x, origin.y + st.scroll.y + n.y);
		const int rows = std::max<int>((int)d.inputs.size(), (int)d.outputs.size());
		const float paramH = d.paramCount > 0 ? 26.0f : 0.0f;
		const float h = kTitleH + 6.0f + rows * kRowH + paramH;

		ImGui::PushID(n.id);

		// Body + title
		const bool selected = st.selectedNode == n.id;
		dl->AddRectFilled(p, ImVec2(p.x + kNodeW, p.y + h), IM_COL32(52, 52, 56, 255), 5.0f);
		dl->AddRectFilled(p, ImVec2(p.x + kNodeW, p.y + kTitleH), categoryColor(d.category), 5.0f,
		                  ImDrawFlags_RoundCornersTop);
		dl->AddRect(p, ImVec2(p.x + kNodeW, p.y + h),
		            selected ? IM_COL32(255, 200, 80, 255) : IM_COL32(0, 0, 0, 160), 5.0f, 0,
		            selected ? 2.0f : 1.0f);
		dl->AddText(ImVec2(p.x + 8, p.y + 4), IM_COL32_WHITE, d.name);

		// Title = drag handle (and selection, and node context menu).
		ImGui::SetCursorScreenPos(p);
		ImGui::InvisibleButton("##title", ImVec2(kNodeW, kTitleH));
		if (ImGui::IsItemActivated()) st.selectedNode = n.id;
		if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
		{
			n.x += ImGui::GetIO().MouseDelta.x;
			n.y += ImGui::GetIO().MouseDelta.y;
		}
		if (ImGui::BeginPopupContextItem("##nodeCtx"))
		{
			const bool deletable = n.type != MatNodeType::Output;
			if (ImGui::MenuItem("Delete Node", nullptr, false, deletable)) deleteNode = n.id;
			ImGui::EndPopup();
		}

		// Input pins (left column)
		for (int i = 0; i < (int)d.inputs.size(); ++i)
		{
			const ImVec2 pp(p.x, p.y + kTitleH + 6.0f + i * kRowH + kRowH * 0.5f);
			pins.push_back({ n.id, i, false, pp, d.inputs[i].type });
			dl->AddCircleFilled(pp, kPinR, pinColor(d.inputs[i].type));
			dl->AddText(ImVec2(pp.x + 10, pp.y - 8), IM_COL32(210, 210, 210, 255), d.inputs[i].name);
			ImGui::SetCursorScreenPos(ImVec2(pp.x - 8, pp.y - 8));
			ImGui::InvisibleButton((std::string("##in") + std::to_string(i)).c_str(), ImVec2(16, 16));
			if (ImGui::IsItemActivated())
			{ st.dragNode = n.id; st.dragPin = i; st.dragFromOutput = false; }
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
			{ st.graph.disconnectInput(n.id, i); structuralEdit = true; }
		}
		// Output pins (right column)
		for (int i = 0; i < (int)d.outputs.size(); ++i)
		{
			const ImVec2 pp(p.x + kNodeW, p.y + kTitleH + 6.0f + i * kRowH + kRowH * 0.5f);
			pins.push_back({ n.id, i, true, pp, d.outputs[i].type });
			dl->AddCircleFilled(pp, kPinR, pinColor(d.outputs[i].type));
			const ImVec2 ts = ImGui::CalcTextSize(d.outputs[i].name);
			dl->AddText(ImVec2(pp.x - 10 - ts.x, pp.y - 8), IM_COL32(210, 210, 210, 255), d.outputs[i].name);
			ImGui::SetCursorScreenPos(ImVec2(pp.x - 8, pp.y - 8));
			ImGui::InvisibleButton((std::string("##out") + std::to_string(i)).c_str(), ImVec2(16, 16));
			if (ImGui::IsItemActivated())
			{ st.dragNode = n.id; st.dragPin = i; st.dragFromOutput = true; }
		}

		// Inline parameter widgets under the pins
		if (d.paramCount > 0)
		{
			ImGui::SetCursorScreenPos(ImVec2(p.x + 10, p.y + kTitleH + 6.0f + rows * kRowH));
			if (nodeParamWidgets(n)) paramEdit = true;
		}

		ImGui::PopID();
	}

	// ── Links (bezier, colored by source pin type) ──
	auto pinAt = [&](int node, int pin, bool output) -> const PinPos* {
		for (const auto& pp : pins)
			if (pp.node == node && pp.pin == pin && pp.output == output) return &pp;
		return nullptr;
	};
	for (const auto& l : st.graph.links)
	{
		const PinPos* a = pinAt(l.srcNode, l.srcPin, true);
		const PinPos* b = pinAt(l.dstNode, l.dstPin, false);
		if (!a || !b) continue;
		const float t = std::max(40.0f, fabsf(b->pos.x - a->pos.x) * 0.5f);
		dl->AddBezierCubic(a->pos, ImVec2(a->pos.x + t, a->pos.y),
		                   ImVec2(b->pos.x - t, b->pos.y), b->pos, pinColor(a->type), 2.0f);
	}

	// ── Live link drag ──
	if (st.dragNode != 0)
	{
		if (const PinPos* src = pinAt(st.dragNode, st.dragPin, st.dragFromOutput))
		{
			const ImVec2 m = ImGui::GetIO().MousePos;
			const float t = 60.0f;
			if (st.dragFromOutput)
				dl->AddBezierCubic(src->pos, ImVec2(src->pos.x + t, src->pos.y),
				                   ImVec2(m.x - t, m.y), m, IM_COL32(255, 255, 255, 160), 2.0f);
			else
				dl->AddBezierCubic(m, ImVec2(m.x + t, m.y),
				                   ImVec2(src->pos.x - t, src->pos.y), src->pos, IM_COL32(255, 255, 255, 160), 2.0f);
		}
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			// Drop on a compatible opposite-side pin → connect.
			const ImVec2 m = ImGui::GetIO().MousePos;
			for (const auto& pp : pins)
			{
				const float dx = pp.pos.x - m.x, dy = pp.pos.y - m.y;
				if (dx * dx + dy * dy > 12.0f * 12.0f) continue;
				if (pp.output == st.dragFromOutput || pp.node == st.dragNode) continue;
				const bool ok = st.dragFromOutput
					? st.graph.connect(st.dragNode, st.dragPin, pp.node, pp.pin)
					: st.graph.connect(pp.node, pp.pin, st.dragNode, st.dragPin);
				if (ok) structuralEdit = true;
				break;
			}
			st.dragNode = 0;
		}
	}

	// ── Canvas panning (LMB/MMB drag on empty space) + add-node context menu ──
	if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive() && st.dragNode == 0)
	{
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
		    (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()))
		{
			st.scroll.x += ImGui::GetIO().MouseDelta.x;
			st.scroll.y += ImGui::GetIO().MouseDelta.y;
		}
	}
	if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() &&
	    ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		ImGui::OpenPopup("##addNode");
	if (ImGui::BeginPopup("##addNode"))
	{
		const ImVec2 m = ImGui::GetMousePosOnOpeningCurrentPopup();
		const float gx = m.x - origin.x - st.scroll.x;
		const float gy = m.y - origin.y - st.scroll.y;
		ImGui::TextDisabled("Add Node");
		ImGui::Separator();
		const char* lastCat = "";
		for (const auto& d : HE::matNodeRegistry())
		{
			if (d.type == MatNodeType::Output) continue; // exactly one output exists
			if (std::string(lastCat) != d.category)
			{
				if (*lastCat) ImGui::Separator();
				ImGui::TextDisabled("%s", d.category);
				lastCat = d.category;
			}
			if (ImGui::MenuItem(d.name))
			{
				st.selectedNode = st.graph.addNode(d.type, gx, gy);
				structuralEdit = true;
			}
		}
		ImGui::EndPopup();
	}

	ImGui::EndChild();

	// Structural / committed edits → regenerate + push into the live material.
	if (deleteNode != 0) { st.graph.removeNode(deleteNode); structuralEdit = true; }
	if ((structuralEdit || paramEdit) && mat)
		applyToMaterial(st, ctx);

	// ── Generated GLSL (debug view) ─────────────────────────────────────────────
	if (ImGui::CollapsingHeader("Generated GLSL"))
	{
		ImGui::BeginChild("##glsl", ImVec2(0, 180.0f), ImGuiChildFlags_Borders);
		ImGui::TextUnformatted(st.lastGlsl.c_str());
		ImGui::EndChild();
	}

	ImGui::End();
}
} // namespace MaterialEditorPanel
