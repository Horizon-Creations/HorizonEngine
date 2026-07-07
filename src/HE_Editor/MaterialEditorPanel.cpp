#include "MaterialEditorPanel.h"
#include "EditorApplication.h"                 // AppContext
#include "GraphEditor.h"                        // shared node-graph canvas frontend
#include <MaterialGraph/MaterialGraph.h>
#include <material/MaterialShaderLibrary.h> // inline compile check (canvas error banner)
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <Types/Enums.h>
#include <Diagnostics/Logger.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
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
	// Shared node-graph canvas state (pan/zoom/selection/drag/box-select). The component
	// owns it; the host keeps one per material and adapts its graph through a Model.
	GraphEditor::State geState;
	int           viewMode = 0;      // right pane: 0 = graph canvas, 1 = generated shader code
	// Preview orbit camera (drag to rotate, wheel to zoom). Re-rendered only when the
	// camera, size, or material changes — the returned texture handle is reused otherwise.
	float         previewYaw = 0.6f, previewPitch = 0.35f, previewDist = 3.1f;
	void*         previewTex   = nullptr;
	bool          previewDirty = true;
	int           previewPx    = 0;
	int           previewShape = 0;  // 0 sphere / 1 cube / 2 plane (RenderMaterialPreview)
	int           previewNodeId = 0; // 0 = whole material; else preview THAT node's output, unlit
	HE::UUID      fnPreviewMatId{};  // function tabs: lazily registered scratch material that
	                                 // wraps this function in a FunctionCall for the preview
	int           editingComment = 0;// comment id whose title is in edit mode (0 = none)
	std::string   compileLog;        // last cross-compile error ("" = ok) → inline banner
	std::string   complexity;        // "~N ALU · M tex" estimate of the generated shader
	// Material INSTANCE tabs (parentMaterialPath set): no canvas — an override panel.
	bool          isInstance = false;
	std::vector<std::pair<std::string, bool>> parentSwitches; // parent's static switches (name, default)
	// Undo/redo: JSON snapshots of the whole graph (incl. comments). undoPos indexes the
	// snapshot the canvas currently shows; edits truncate the redo tail.
	std::vector<std::string> undo;
	int           undoPos = -1;
	std::string lastGlsl;            // generated fragment (debug view)
	std::string name;                // filename for the header
	bool        isFunction = false;  // editing a MaterialFunction asset (FnInput/FnOutput mode)
	std::string relPath;             // content-root-relative path of this asset
};
std::map<std::string, State> g_states;

// Cache of loaded material-FUNCTION graphs, keyed by content-relative path. Backs both
// the codegen loader and the dynamic pins of FunctionCall nodes. Invalidated when a
// function is saved in this editor.
std::map<std::string, HE::MaterialGraph> g_fnGraphCache;

const HE::MaterialGraph* loadFunctionGraph(AppContext& ctx, const std::string& relPath)
{
	if (relPath.empty() || !ctx.contentManager) return nullptr;
	if (auto it = g_fnGraphCache.find(relPath); it != g_fnGraphCache.end()) return &it->second;
	const HE::UUID id = ctx.contentManager->loadAsset(relPath);
	const MaterialFunctionAsset* fn = ctx.contentManager->getMaterialFunction(id);
	if (!fn) return nullptr;
	HE::MaterialGraph g;
	if (!fn->nodeGraphJson.empty() && !HE::materialGraphFromJson(fn->nodeGraphJson, g))
		return nullptr;
	return &(g_fnGraphCache[relPath] = std::move(g));
}

// Node width used to size the inline value widgets (kept in sync with the shared
// canvas' GraphEditor::kNodeW). The canvas' pin/title/row metrics come from GraphEditor.
constexpr float kNodeW = 172.0f;

ImU32 categoryColor(const char* cat)
{
	const std::string c = cat;
	if (c == "Material") return IM_COL32(140,  60,  60, 255);
	if (c == "Input")    return IM_COL32( 60, 100, 140, 255);
	if (c == "Math")     return IM_COL32( 60, 120,  80, 255);
	if (c == "Texture")    return IM_COL32(120,  90, 150, 255);
	if (c == "Parameter")  return IM_COL32(160, 110,  50, 255);
	if (c == "Procedural") return IM_COL32( 90, 130, 130, 255);
	if (c == "Channels")   return IM_COL32( 90,  90, 130, 255);
	if (c == "Function")   return IM_COL32(150,  70, 110, 255);
	return IM_COL32(110, 110,  70, 255); // Shading & misc
}

ImU32 pinColor(MatPinType t)
{
	switch (t)
	{
		case MatPinType::Float: return IM_COL32(160, 200, 120, 255);
		case MatPinType::Vec2:  return IM_COL32(120, 190, 200, 255);
		case MatPinType::Vec3:  return IM_COL32(230, 200, 110, 255);
		case MatPinType::Vec4:  return IM_COL32(235, 140, 180, 255);
	}
	return IM_COL32_WHITE;
}

// Rough cost estimate of a generated fragment: statements in main() plus weighted
// texture/noise calls. Not a profiler — a relative gauge so authors see when a graph
// change makes the shader meaningfully heavier.
std::string estimateComplexity(const std::string& glsl)
{
	const size_t mainPos = glsl.find("void main()");
	const std::string body = mainPos == std::string::npos ? glsl : glsl.substr(mainPos);
	auto count = [](const std::string& hay, const char* needle)
	{
		size_t c = 0, pos = 0; const size_t len = std::strlen(needle);
		while ((pos = hay.find(needle, pos)) != std::string::npos) { ++c; pos += len; }
		return c;
	};
	const size_t ops   = count(body, ";");
	const size_t tex   = count(body, "texture(");
	const size_t fbm   = count(body, "heFbm(") + count(body, "heFbm3(");
	const size_t noise = count(body, "heValueNoise(") + count(body, "heValueNoise3(");
	const size_t alu   = ops + tex * 8 + fbm * 24 + noise * 6;
	char buf[96];
	if (fbm + noise)
		std::snprintf(buf, sizeof buf, "~%zu ALU · %zu tex · %zu noise", alu, tex, fbm + noise);
	else
		std::snprintf(buf, sizeof buf, "~%zu ALU · %zu tex", alu, tex);
	return buf;
}

// Regenerate the shader from the graph and push it into the live MaterialAsset. The
// renderers re-resolve the material's shader every frame (pipeline cached per source
// hash), so the scene updates immediately; Save persists to disk.
void applyToMaterial(State& st, AppContext& ctx)
{
	if (!ctx.contentManager) return;
	if (st.isFunction)
	{
		// Functions carry only their graph. Saving invalidates the pin/codegen cache so
		// open materials pick up the new interface on their next regenerate.
		if (MaterialFunctionAsset* fn = ctx.contentManager->getMaterialFunctionMutable(st.materialId))
		{
			fn->nodeGraphJson = HE::materialGraphToJson(st.graph);
			g_fnGraphCache.erase(st.relPath);
			st.dirty = true;
		}
		return;
	}
	MaterialAsset* mat = ctx.contentManager->getMaterialMutable(st.materialId);
	if (!mat) return;
	HE::MatFunctionLoader loader = [&ctx](const std::string& path)
	{ return loadFunctionGraph(ctx, path); };
	HE::MatShaderGen gen = HE::generateFragment(st.graph, loader);
	st.lastGlsl = gen.glsl;
	mat->customShaderFragGlsl = st.lastGlsl;
	mat->nodeGraphJson        = HE::materialGraphToJson(st.graph);
	mat->shaderParamData.clear();
	mat->graphParamNames.clear();
	mat->graphParamTypes.clear();
	mat->graphParamMinMax.clear();
	mat->graphParamGroups.clear();
	mat->graphParamTooltips.clear();
	for (const auto& slot : gen.params)
	{
		mat->shaderParamData.insert(mat->shaderParamData.end(),
		                            slot.value, slot.value + 4);
		mat->graphParamNames.push_back(slot.name); // parallel to slots → runtime setMaterialParam
		mat->graphParamTypes.push_back(static_cast<uint8_t>(slot.kind)); // typed editors
		mat->graphParamMinMax.insert(mat->graphParamMinMax.end(), { slot.minV, slot.maxV });
		mat->graphParamGroups.push_back(slot.group);
		mat->graphParamTooltips.push_back(slot.tooltip);
	}
	// Project textures the graph samples, in slot order (heTexP0..) — the renderer
	// binds these on loose materials; packing bakes them to graphTextureIds (MTLU).
	mat->graphTexturePaths = gen.textures;
	mat->blendMode            = gen.blendMode;
	mat->customShaderVertGlsl = gen.vertexBody; // WPO vertex body ("" = standard vertex)
	st.dirty = true;
	st.complexity = estimateComplexity(st.lastGlsl);
	// Live master→variants propagation: re-derive every loaded instance of this material.
	ctx.contentManager->syncMaterialInstancesOf(st.relPath);

	// Inline error check: cross-compile the fresh GLSL (Metal target — host-independent,
	// pure codegen) and keep the log for the canvas banner. The library caches results by
	// hash, so unchanged sources cost nothing. The renderers fall back to magenta on a
	// broken shader; this tells the user WHY instead of leaving them guessing.
	static HE::MaterialShaderLibrary s_checkLib;
	const auto& chk = s_checkLib.fragment(std::hash<std::string>{}(st.lastGlsl),
	                                      st.lastGlsl, HE::MaterialShaderLibrary::Backend::Metal);
	st.compileLog = chk.ok ? std::string() : chk.log;
}

// ── Undo/redo (JSON snapshots — cheap at graph scale, and reuses the asset codec) ──
constexpr size_t kUndoCap = 64;

void pushUndo(State& st)
{
	const std::string snap = HE::materialGraphToJson(st.graph);
	if (st.undoPos >= 0 && st.undoPos < (int)st.undo.size() && st.undo[st.undoPos] == snap)
		return; // no-op edit → don't spam the stack
	st.undo.resize(st.undoPos + 1); // drop the redo tail
	st.undo.push_back(snap);
	if (st.undo.size() > kUndoCap) st.undo.erase(st.undo.begin());
	st.undoPos = (int)st.undo.size() - 1;
}

bool restoreSnapshot(State& st, int pos)
{
	if (pos < 0 || pos >= (int)st.undo.size()) return false;
	HE::MaterialGraph g;
	if (!HE::materialGraphFromJson(st.undo[pos], g)) return false;
	st.graph  = std::move(g);
	st.undoPos = pos;
	// Prune references to nodes that no longer exist in this snapshot.
	auto& sel = st.geState.selection;
	sel.erase(std::remove_if(sel.begin(), sel.end(),
		[&](int id){ return st.graph.findNode(id) == nullptr; }), sel.end());
	if (st.geState.selected && !st.graph.findNode(st.geState.selected)) st.geState.selected = 0;
	if (st.previewNodeId && !st.graph.findNode(st.previewNodeId)) st.previewNodeId = 0;
	return true;
}

// ── Node clipboard (process-wide → copy/paste works ACROSS material tabs) ──────────
std::string g_matClipboard;

// One-shot open-asset request (double-clicked Material Function node → its editor tab).
std::string s_openAssetRequest;

// Selected nodes + the links fully inside the selection, as graph JSON. Interface
// nodes are excluded: Output/FnOutput are singletons, FnInput defines a function's
// signature — duplicating either would corrupt the target graph.
std::string serializeSelection(const State& st)
{
	HE::MaterialGraph tmp;
	for (int id : st.geState.selection)
		if (const MatGraphNode* n = st.graph.findNode(id))
			if (n->type != MatNodeType::Output && n->type != MatNodeType::FnOutput &&
			    n->type != MatNodeType::FnInput)
				tmp.nodes.push_back(*n);
	if (tmp.nodes.empty()) return {};
	auto inSel = [&](int id){ for (auto& n : tmp.nodes) if (n.id == id) return true; return false; };
	for (const auto& l : st.graph.links)
		if (inSel(l.srcNode) && inSel(l.dstNode)) tmp.links.push_back(l);
	return HE::materialGraphToJson(tmp);
}

// Paste `payload` into st.graph with FRESH ids, the group's top-left at (atX, atY).
// The pasted nodes become the new selection. Returns false on empty/invalid payload.
bool pasteInto(State& st, const std::string& payload, float atX, float atY)
{
	HE::MaterialGraph tmp;
	if (payload.empty() || !HE::materialGraphFromJson(payload, tmp) || tmp.nodes.empty())
		return false;
	float mnx = FLT_MAX, mny = FLT_MAX;
	for (const auto& n : tmp.nodes) { mnx = std::min(mnx, n.x); mny = std::min(mny, n.y); }
	std::map<int, int> remap;
	auto& sel = st.geState.selection;
	sel.clear();
	for (const auto& n : tmp.nodes)
	{
		MatGraphNode c = n;
		c.id = st.graph.nextId++;
		c.x  = n.x - mnx + atX;
		c.y  = n.y - mny + atY;
		remap[n.id] = c.id;
		sel.push_back(c.id);
		st.graph.nodes.push_back(std::move(c));
	}
	for (const auto& l : tmp.links)
		st.graph.links.push_back({ remap[l.srcNode], l.srcPin, remap[l.dstNode], l.dstPin });
	st.geState.selected = sel.empty() ? 0 : sel.front();
	return true;
}

// Function tabs: edit the function's INTERFACE — its FnInput/FnOutput nodes — as one
// list (rename, retype, add, remove) instead of hunting nodes on the canvas. The order
// shown matches the call-node pin order (sorted by node id, same as matFunctionPins).
bool drawFunctionInterfacePanel(MaterialGraph& g)
{
	bool committed = false;
	static const char* kTypes[] = { "Float", "Vec2", "Vec3", "Vec4" };
	int removeId = 0;

	auto section = [&](const char* title, MatNodeType type, const char* addLabel,
	                   const char* namePrefix)
	{
		ImGui::TextDisabled("%s", title);
		ImGui::Separator();
		std::vector<MatGraphNode*> rows;
		for (auto& n : g.nodes) if (n.type == type) rows.push_back(&n);
		std::sort(rows.begin(), rows.end(),
		          [](const MatGraphNode* a, const MatGraphNode* b){ return a->id < b->id; });
		for (MatGraphNode* n : rows)
		{
			ImGui::PushID(n->id);
			ImGui::SetNextItemWidth(118.0f);
			ImGui::InputText("##nm", &n->s);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::SameLine();
			int t = std::clamp(static_cast<int>(n->p[0]), 0, 3);
			ImGui::SetNextItemWidth(66.0f);
			if (ImGui::Combo("##ty", &t, kTypes, 4)) { n->p[0] = (float)t; committed = true; }
			ImGui::SameLine();
			// A function needs at least one output; inputs may go to zero.
			const bool lastOutput = type == MatNodeType::FnOutput && rows.size() <= 1;
			ImGui::BeginDisabled(lastOutput);
			if (ImGui::SmallButton("x")) removeId = n->id;
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		if (ImGui::SmallButton(addLabel))
		{
			// Drop the new node below the lowest sibling so it lands visibly on canvas.
			float y = 40.0f; const float x = type == MatNodeType::FnInput ? 40.0f : 420.0f;
			for (const MatGraphNode* n : rows) y = std::max(y, n->y + 90.0f);
			const int id = g.addNode(type, x, y);
			g.findNode(id)->s = std::string(namePrefix) + std::to_string(rows.size() + 1);
			committed = true;
		}
	};

	section("Inputs",  MatNodeType::FnInput,  "+ Add Input",  "In");
	ImGui::Spacing(); ImGui::Spacing();
	section("Outputs", MatNodeType::FnOutput, "+ Add Output", "Out");

	if (removeId != 0) { g.removeNode(removeId); committed = true; }
	return committed;
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
	st.relPath    = ec ? path : rel;
	st.materialId = ctx.contentManager->loadAsset(st.relPath);
	st.isFunction = ctx.contentManager->assetType(st.materialId) == HE::AssetType::MaterialFunction;

	if (st.isFunction)
	{
		const MaterialFunctionAsset* fn = ctx.contentManager->getMaterialFunction(st.materialId);
		if (!fn || fn->nodeGraphJson.empty() ||
		    !HE::materialGraphFromJson(fn->nodeGraphJson, st.graph))
			st.graph = MaterialGraph::makeDefaultFunction();
	}
	else
	{
		const MaterialAsset* mat = ctx.contentManager->getMaterial(st.materialId);
		if (mat && !mat->parentMaterialPath.empty())
		{
			// Material INSTANCE: no graph of its own — the tab becomes an override
			// panel. Cache the parent's static switches for the switch section.
			st.isInstance = true;
			st.lastGlsl   = mat->customShaderFragGlsl;
			const HE::UUID pid = ctx.contentManager->loadAsset(mat->parentMaterialPath);
			if (const MaterialAsset* par = ctx.contentManager->getMaterial(pid))
			{
				HE::MaterialGraph pg;
				if (!par->nodeGraphJson.empty() && HE::materialGraphFromJson(par->nodeGraphJson, pg))
					for (const auto& n : pg.nodes)
						if (n.type == MatNodeType::StaticSwitch)
							st.parentSwitches.push_back({
								n.s.empty() ? ("switch_" + std::to_string(n.id)) : n.s,
								n.p[0] > 0.5f });
			}
		}
		else if (!mat || mat->nodeGraphJson.empty() ||
		    !HE::materialGraphFromJson(mat->nodeGraphJson, st.graph))
		{
			// No graph yet (fresh material, or one with only a hand-written shader):
			// start from the default. Nothing is written until the first edit.
			st.graph = MaterialGraph::makeDefault();
		}
		if (!st.isInstance) st.lastGlsl = HE::generateFragmentGlsl(st.graph);
	}
	st.loaded = true;
	pushUndo(st); // seed the undo stack with the as-loaded state (undo floor)
	return st;
}

// Scale embedded ImGui widgets to the canvas zoom. Font scale alone is not enough —
// FramePadding/spacing/grab are in pixels and would keep full size, making widgets
// overflow the shrunken node box; scale those too so a node's widgets track its box.
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
void popWidgetScale()
{
	ImGui::SetWindowFontScale(1.0f);
	ImGui::PopStyleVar(5);
}

// Inline parameter widgets for a node; returns true when an edit was COMMITTED
// (deactivated-after-edit), so constant drags don't rebuild the pipeline every frame.
// `scale` = canvas zoom, so the fixed widget widths track the scaled node box.
// `drawName` = draw the name/label field (true in the side panel); the canvas passes
// false because the name is edited in the node's colored header instead.
bool nodeParamWidgets(MatGraphNode& n, float scale = 1.0f, bool drawName = true)
{
	bool committed = false;
	switch (n.type)
	{
		case MatNodeType::ConstFloat:
			ImGui::SetNextItemWidth((kNodeW - 24.0f) * scale);
			ImGui::DragFloat("##v", &n.p[0], 0.01f);
			committed = ImGui::IsItemDeactivatedAfterEdit();
			break;
		case MatNodeType::ConstColor:
			ImGui::SetNextItemWidth((kNodeW - 24.0f) * scale);
			ImGui::ColorEdit3("##c", n.p, ImGuiColorEditFlags_Float);
			committed = ImGui::IsItemDeactivatedAfterEdit();
			break;
		case MatNodeType::Fresnel:
			ImGui::SetNextItemWidth((kNodeW - 60.0f) * scale);
			ImGui::DragFloat("Pow", &n.p[0], 0.05f, 0.01f, 16.0f);
			committed = ImGui::IsItemDeactivatedAfterEdit();
			break;
		case MatNodeType::ConstVec2:
			ImGui::SetNextItemWidth((kNodeW - 24.0f) * scale);
			ImGui::DragFloat2("##v2", n.p, 0.01f);
			committed = ImGui::IsItemDeactivatedAfterEdit();
			break;
		case MatNodeType::ConstVec4:
			ImGui::SetNextItemWidth((kNodeW - 24.0f) * scale);
			ImGui::DragFloat4("##v4", n.p, 0.01f);
			committed = ImGui::IsItemDeactivatedAfterEdit();
			break;
		case MatNodeType::NormalMapSample:
		{
			ImGui::SetNextItemWidth((kNodeW - 76.0f) * scale);
			ImGui::DragFloat("Strength", &n.p[0], 0.05f, 0.0f, 4.0f);
			committed = ImGui::IsItemDeactivatedAfterEdit();
			const std::string label = n.s.empty()
				? std::string("(mesh texture)")
				: std::filesystem::path(n.s).filename().string();
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.85f, 1.0f, 1.0f));
			ImGui::TextWrapped("%s", label.c_str());
			ImGui::PopStyleColor();
			if (!n.s.empty() && ImGui::SmallButton("Clear")) { n.s.clear(); committed = true; }
			break;
		}
		case MatNodeType::TextureSample:
		{
			// A picked texture shows its filename + a clear button; the drop target
			// itself is the whole node body (handled in the node loop). Empty = the
			// material's own base/mesh texture.
			const std::string label = n.s.empty()
				? std::string("(mesh texture)")
				: std::filesystem::path(n.s).filename().string();
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.85f, 1.0f, 1.0f));
			ImGui::TextWrapped("%s", label.c_str());
			ImGui::PopStyleColor();
			if (!n.s.empty() && ImGui::SmallButton("Clear")) { n.s.clear(); committed = true; }
			ImGui::TextDisabled("(drop a texture)");
			break;
		}
		case MatNodeType::Output:
		{
			// Only the Lit toggle lives ON the node; the blend mode (which re-shapes the
			// node's pins) is a MATERIAL-level setting edited in the tab header.
			bool lit = n.p[0] > 0.5f;
			if (ImGui::Checkbox("Lit", &lit)) { n.p[0] = lit ? 1.0f : 0.0f; committed = true; }
			break;
		}
		case MatNodeType::ParamFloat:
			if (drawName) {
				ImGui::SetNextItemWidth((kNodeW - 24.0f) * scale);
				ImGui::InputText("##name", &n.s);
				committed |= ImGui::IsItemDeactivatedAfterEdit();
			}
			ImGui::SetNextItemWidth((kNodeW - 24.0f) * scale);
			// Metadata slider range (p[1]/p[2]): min < max → bounded slider, else free drag.
			if (n.p[1] < n.p[2])
				ImGui::SliderFloat("##v", &n.p[0], n.p[1], n.p[2]);
			else
				ImGui::DragFloat("##v", &n.p[0], 0.01f);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			break;
		case MatNodeType::ParamColor:
			if (drawName) {
				ImGui::SetNextItemWidth((kNodeW - 24.0f) * scale);
				ImGui::InputText("##name", &n.s);
				committed |= ImGui::IsItemDeactivatedAfterEdit();
			}
			ImGui::SetNextItemWidth((kNodeW - 24.0f) * scale);
			ImGui::ColorEdit3("##c", n.p, ImGuiColorEditFlags_Float);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			break;
		case MatNodeType::FnInput:
		case MatNodeType::FnOutput:
		{
			if (drawName) {
				ImGui::SetNextItemWidth((kNodeW - 24.0f) * scale);
				ImGui::InputText("##name", &n.s);
				committed |= ImGui::IsItemDeactivatedAfterEdit();
			}
			static const char* kTypes[] = { "Float", "Vec2", "Vec3", "Vec4" };
			int t = std::clamp(static_cast<int>(n.p[0]), 0, 3);
			ImGui::SetNextItemWidth((kNodeW - 24.0f) * scale);
			if (ImGui::Combo("##type", &t, kTypes, 4)) { n.p[0] = (float)t; committed = true; }
			break;
		}
		// ── v5: baked constant bool + new parameter types ──
		case MatNodeType::ConstBool:
		{
			bool on = n.p[0] > 0.5f;
			if (ImGui::Checkbox("True", &on)) { n.p[0] = on ? 1.0f : 0.0f; committed = true; }
			break;
		}
		case MatNodeType::ParamVec2:
			if (drawName) {
				ImGui::SetNextItemWidth((kNodeW - 24.0f) * scale);
				ImGui::InputText("##name", &n.s);
				committed |= ImGui::IsItemDeactivatedAfterEdit();
			}
			ImGui::SetNextItemWidth((kNodeW - 24.0f) * scale);
			ImGui::DragFloat2("##v2", n.p, 0.01f);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			break;
		case MatNodeType::ParamVec4:
			if (drawName) {
				ImGui::SetNextItemWidth((kNodeW - 24.0f) * scale);
				ImGui::InputText("##name", &n.s);
				committed |= ImGui::IsItemDeactivatedAfterEdit();
			}
			ImGui::SetNextItemWidth((kNodeW - 24.0f) * scale);
			ImGui::DragFloat4("##v4", n.p, 0.01f);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			break;
		case MatNodeType::ParamBool:
		{
			if (drawName) {
				ImGui::SetNextItemWidth((kNodeW - 24.0f) * scale);
				ImGui::InputText("##name", &n.s);
				committed |= ImGui::IsItemDeactivatedAfterEdit();
			}
			bool on = n.p[0] > 0.5f;
			if (ImGui::Checkbox("Default", &on)) { n.p[0] = on ? 1.0f : 0.0f; committed = true; }
			break;
		}
		// ── v8: compile-time switch — toggling REGENERATES the shader (that's the point) ──
		case MatNodeType::StaticSwitch:
		{
			bool on = n.p[0] > 0.5f;
			if (ImGui::Checkbox("On (default)", &on)) { n.p[0] = on ? 1.0f : 0.0f; committed = true; }
			break;
		}
		// ── v6: procedural texture — inline Scale (bigger = finer speckle) ──
		case MatNodeType::NoiseTexture:
			ImGui::SetNextItemWidth((kNodeW - 60.0f) * scale);
			ImGui::DragFloat("Scale", &n.p[0], 0.1f, 0.01f, 256.0f);
			committed = ImGui::IsItemDeactivatedAfterEdit();
			break;
		default: break;
	}
	return committed;
}

// Vertical space (graph units) the node reserves for its inline VALUE widgets. The
// editable NAME row (for named nodes) is drawn in the body too now (the header carries
// the node TYPE name via Model.title), so it is added on top of the value height.
float nodeValueHeight(const MatGraphNode& n)
{
	const MatNodeType type = n.type;
	const HE::MatNodeDesc& d = HE::matNodeDesc(type);
	if (d.paramCount == 0 && type != MatNodeType::TextureSample &&
	    type != MatNodeType::NormalMapSample) return 0.0f;
	if (type == MatNodeType::TextureSample ||
	    type == MatNodeType::NormalMapSample) return 44.0f;       // filename + hint rows
	if (type == MatNodeType::ConstVec4 || type == MatNodeType::ParamVec4) return 30.0f; // vec4 drag row
	return 26.0f;                                                 // one value/combo row
}

bool isParamNode(MatNodeType t)
{
	return t == MatNodeType::ParamFloat || t == MatNodeType::ParamColor ||
	       t == MatNodeType::ParamVec2  || t == MatNodeType::ParamVec4  ||
	       t == MatNodeType::ParamBool;
}
bool isConstNode(MatNodeType t)
{
	return t == MatNodeType::ConstFloat || t == MatNodeType::ConstColor ||
	       t == MatNodeType::ConstVec2  || t == MatNodeType::ConstVec4  ||
	       t == MatNodeType::ConstBool;
}
// Nodes that carry an editable NAME/label drawn in the node BODY (params + constants +
// function-interface pins). The name (n.s) drives the uniform name for params and the
// interface name for FnInput/FnOutput; for constants it is a cosmetic label for
// readability (does not affect codegen). The colored header shows the node TYPE name.
bool isNamedNode(MatNodeType t)
{
	return isParamNode(t) || isConstNode(t) ||
	       t == MatNodeType::FnInput || t == MatNodeType::FnOutput ||
	       t == MatNodeType::StaticSwitch;
}

// Total body height (graph units) the shared canvas reserves under the pins: the editable
// name row (named nodes) plus the inline VALUE widgets. GraphEditor scales it by the zoom.
float matNodeBodyHeight(const MatGraphNode& n)
{
	float h = nodeValueHeight(n);
	if (isNamedNode(n.type)) h += 26.0f; // name InputText row
	return h;
}

// Central "Parameters & Constants" panel: every Param/Const node of the graph in
// one list with its typed widget, so values can be set centrally instead of hunting
// nodes on the canvas. Reuses the inline node widgets (rename + value for params,
// value for constants). Returns true if any value was committed (→ regenerate).
bool drawParamConstPanel(MaterialGraph& graph)
{
	std::vector<MatGraphNode*> params, consts;
	for (auto& n : graph.nodes)
	{
		if (isParamNode(n.type))  params.push_back(&n);
		else if (isConstNode(n.type)) consts.push_back(&n);
	}
	if (params.empty() && consts.empty()) return false;

	bool committed = false;
	if (!params.empty())
	{
		ImGui::TextDisabled("Parameters (runtime-settable uniforms)");
		ImGui::Separator();
		// Group by the params' metadata group (first-seen order; "" first as ungrouped).
		std::vector<std::string> groupOrder;
		auto groupOf = [](const MatGraphNode* n){ return n->group; };
		for (MatGraphNode* n : params)
			if (std::find(groupOrder.begin(), groupOrder.end(), groupOf(n)) == groupOrder.end())
				groupOrder.push_back(groupOf(n));
		std::stable_sort(groupOrder.begin(), groupOrder.end(),
			[](const std::string& a, const std::string& b){ return a.empty() && !b.empty(); });
		for (const std::string& grp : groupOrder)
		{
			if (!grp.empty()) { ImGui::Spacing(); ImGui::SeparatorText(grp.c_str()); }
			for (MatGraphNode* n : params)
			{
				if (groupOf(n) != grp) continue;
				ImGui::PushID(n->id);
				committed |= nodeParamWidgets(*n);
				// Tooltip marker + metadata editor ("⋯" popup: slider range/group/tooltip).
				ImGui::SameLine();
				if (ImGui::SmallButton("..")) ImGui::OpenPopup("##pmeta");
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Edit metadata (range / group / tooltip)");
				if (!n->tooltip.empty())
				{
					ImGui::SameLine();
					ImGui::TextDisabled("(?)");
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", n->tooltip.c_str());
				}
				if (ImGui::BeginPopup("##pmeta"))
				{
					ImGui::TextDisabled("Parameter metadata");
					if (n->type == MatNodeType::ParamFloat)
					{
						ImGui::SetNextItemWidth(64.0f);
						ImGui::DragFloat("Min", &n->p[1], 0.05f);
						committed |= ImGui::IsItemDeactivatedAfterEdit();
						ImGui::SameLine();
						ImGui::SetNextItemWidth(64.0f);
						ImGui::DragFloat("Max", &n->p[2], 0.05f);
						committed |= ImGui::IsItemDeactivatedAfterEdit();
						ImGui::TextDisabled("(min < max shows a slider)");
					}
					ImGui::SetNextItemWidth(150.0f);
					ImGui::InputText("Group", &n->group);
					committed |= ImGui::IsItemDeactivatedAfterEdit();
					ImGui::SetNextItemWidth(150.0f);
					ImGui::InputText("Tooltip", &n->tooltip);
					committed |= ImGui::IsItemDeactivatedAfterEdit();
					ImGui::EndPopup();
				}
				ImGui::PopID();
			}
		}
	}
	if (!consts.empty())
	{
		if (!params.empty()) ImGui::Spacing();
		ImGui::TextDisabled("Constants (baked into the shader)");
		ImGui::Separator();
		for (MatGraphNode* n : consts)
		{
			ImGui::PushID(n->id);
			ImGui::TextUnformatted(HE::matNodeDesc(n->type).name);
			ImGui::SameLine(90.0f);
			committed |= nodeParamWidgets(*n);
			ImGui::PopID();
		}
	}
	return committed;
}

// ── Pin list for the shared canvas ────────────────────────────────────────────
// Build the GraphEditor::Pin list for one material node, reusing the same pin-index
// semantics the graph links use: input pin `id` = the registry pin index (identity
// except the Output node, whose blend mode hides/renames pin 4 — matOutputPins gives
// the row→registry map); output pin `id` = the sequential output index. FunctionCall
// pins come from the referenced function graph's interface.
std::vector<GraphEditor::Pin> matNodePins(AppContext& ctx, const MatGraphNode& n)
{
	const MatNodeDesc& d = HE::matNodeDesc(n.type);
	std::vector<HE::MatPinDesc> dynIn, dynOut;
	const std::vector<HE::MatPinDesc>* nodeIns  = &d.inputs;
	const std::vector<HE::MatPinDesc>* nodeOuts = &d.outputs;
	std::vector<int> inPinIndex; // row → registry pin index (Output only differs)
	if (n.type == MatNodeType::Output)
	{
		HE::matOutputPins(std::clamp(static_cast<int>(n.p[1]), 0, 2), dynIn, inPinIndex);
		nodeIns = &dynIn;
	}
	if (n.type == MatNodeType::FunctionCall)
		if (const HE::MaterialGraph* fn = loadFunctionGraph(ctx, n.s))
		{
			HE::matFunctionPins(*fn, dynIn, dynOut);
			nodeIns = &dynIn; nodeOuts = &dynOut;
		}

	std::vector<GraphEditor::Pin> pins;
	pins.reserve(nodeIns->size() + nodeOuts->size());
	for (int i = 0; i < (int)nodeIns->size(); ++i)
	{
		const int pinIdx = i < (int)inPinIndex.size() ? inPinIndex[i] : i;
		pins.push_back({ pinIdx, (*nodeIns)[i].name, pinColor((*nodeIns)[i].type), true,  false });
	}
	for (int i = 0; i < (int)nodeOuts->size(); ++i)
		pins.push_back({ i, (*nodeOuts)[i].name, pinColor((*nodeOuts)[i].type), false, false });
	return pins;
}

// The header title shown by the shared canvas: the node TYPE name (FunctionCall shows
// the referenced function's file stem). The editable instance NAME lives in the body.
std::string matNodeTitle(const MatGraphNode& n)
{
	if (n.type == MatNodeType::FunctionCall)
	{
		std::string t = std::filesystem::path(n.s).stem().string();
		return t.empty() ? std::string("Material Function") : t;
	}
	return HE::matNodeDesc(n.type).name;
}

// ── The material node canvas, on the shared GraphEditor component ──────────────
// Mirrors UIEditorPanel::drawGraphCanvas: builds a Model adapter over st.graph, syncs
// the host selection into geState, and maps every material-specific feature (comments,
// on-node params, per-node preview, context menu, function open, clipboard) through the
// component's hooks. Sets the caller's edit flags so the shared post-canvas block can
// regenerate + snapshot.
void drawMaterialCanvas(State& st, AppContext& ctx, bool assetOk,
                        const ImVec2& avail, bool& structuralEdit, bool& paramEdit,
                        bool& commentEdit, int& deleteNode)
{
	// Per-node preview toggle picked from the context menu (0 = none this frame).
	int togglePreviewNode = 0;

	GraphEditor::Model m;
	m.multiSelect = true;
	m.nodeIds = [&st]{ std::vector<int> ids; ids.reserve(st.graph.nodes.size());
		for (const auto& n : st.graph.nodes) ids.push_back(n.id); return ids; };
	m.getPos = [&st](int id, float& x, float& y){ if (const MatGraphNode* n = st.graph.findNode(id)) { x = n->x; y = n->y; } };
	m.setPos = [&st](int id, float x, float y){ if (MatGraphNode* n = st.graph.findNode(id)) { n->x = x; n->y = y; } };
	m.title  = [&st](int id){ const MatGraphNode* n = st.graph.findNode(id); return n ? matNodeTitle(*n) : std::string(); };
	m.headerColor = [&st](int id){ const MatGraphNode* n = st.graph.findNode(id);
		return categoryColor(n ? HE::matNodeDesc(n->type).category : ""); };
	m.pins = [&st, &ctx](int id){ const MatGraphNode* n = st.graph.findNode(id);
		return n ? matNodePins(ctx, *n) : std::vector<GraphEditor::Pin>{}; };
	m.links = [&st]{ std::vector<std::array<int,4>> ls; ls.reserve(st.graph.links.size());
		for (const auto& l : st.graph.links) ls.push_back({ l.srcNode, l.srcPin, l.dstNode, l.dstPin }); return ls; };
	m.connect = [&st](int oN, int oP, int iN, int iP){ return st.graph.connect(oN, oP, iN, iP); };
	// Alt+click a pin severs its link(s): an input drops its single incoming link; an
	// output drops every link leaving it.
	m.clearPinLinks = [&st](int node, int pin, bool input)
	{
		if (input) st.graph.disconnectInput(node, pin);
		else st.graph.links.erase(std::remove_if(st.graph.links.begin(), st.graph.links.end(),
			[&](const HE::MatGraphLink& l){ return l.srcNode == node && l.srcPin == pin; }),
			st.graph.links.end());
	};
	// Delete removes any node except the material Output (which is a singleton).
	m.removeNode = [&st](int id){ if (const MatGraphNode* n = st.graph.findNode(id); n && n->type != MatNodeType::Output) st.graph.removeNode(id); };

	// ── On-node body: editable name (named nodes) + inline value widgets ──────────
	m.nodeBodyHeight = [&st](int id){ const MatGraphNode* n = st.graph.findNode(id); return n ? matNodeBodyHeight(*n) : 0.0f; };
	m.drawNodeBody = [&st, &paramEdit, &structuralEdit, &ctx](int id, ImVec2 bodyMin, ImVec2 bodyMax, float zoom)
	{
		MatGraphNode* n = st.graph.findNode(id);
		if (!n) return;
		// Texture Sample / Normal Map: the whole body is a Content-Browser drop target.
		// Submit it FIRST (behind, AllowOverlap) so the label + Clear button drawn on top
		// still take their clicks.
		if (n->type == MatNodeType::TextureSample || n->type == MatNodeType::NormalMapSample)
		{
			ImGui::SetCursorScreenPos(bodyMin);
			ImGui::SetNextItemAllowOverlap();
			ImGui::InvisibleButton("##texdrop", ImVec2(std::max((bodyMax.x - bodyMin.x), 1.0f),
			                                           std::max((bodyMax.y - bodyMin.y), 1.0f)));
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
				{
					const std::string abs(static_cast<const char*>(pl->Data));
					if (MaterialEditorPanel::isTextureAsset(abs) && ctx.contentManager)
					{
						std::error_code ec;
						const std::string rel = std::filesystem::relative(
							abs, ctx.contentManager->contentRoot(), ec).generic_string();
						n->s = ec ? abs : rel;
						structuralEdit = true; // texture list changed → regenerate
					}
				}
				ImGui::EndDragDropTarget();
			}
		}
		ImGui::SetCursorScreenPos(ImVec2(bodyMin.x + 4.0f * zoom, bodyMin.y));
		pushWidgetScale(zoom);
		// Editable instance NAME (params/consts/fn-interface). The header carries the type.
		if (isNamedNode(n->type))
		{
			ImGui::SetNextItemWidth((kNodeW - 20.0f) * zoom);
			const char* hint = isConstNode(n->type) ? HE::matNodeDesc(n->type).name : "name";
			ImGui::InputTextWithHint("##hdrname", hint, &n->s);
			if (ImGui::IsItemDeactivatedAfterEdit()) paramEdit = true; // rename → regenerate
		}
		// Inline VALUE widgets (name drawn above, so drawName=false).
		if (HE::matNodeDesc(n->type).paramCount > 0 || n->type == MatNodeType::TextureSample)
			if (nodeParamWidgets(*n, zoom, /*drawName=*/false)) paramEdit = true;
		popWidgetScale();
	};

	// ── Comments: boxes drawn behind the nodes, dragged/resized/renamed in front. ──
	m.drawBehind = [&st](ImDrawList* dl, ImVec2 origin, ImVec2 pan, float zoom)
	{
		ImFont* const font = ImGui::GetFont();
		const float fsz = ImGui::GetFontSize() * zoom;
		for (const auto& cb : st.graph.comments)
		{
			const ImVec2 cp(origin.x + pan.x + cb.x * zoom, origin.y + pan.y + cb.y * zoom);
			const ImVec2 cs(cb.w * zoom, cb.h * zoom);
			const float  headH = 24.0f * zoom;
			dl->AddRectFilled(cp, ImVec2(cp.x + cs.x, cp.y + cs.y), IM_COL32(255, 210, 110, 16), 6.0f);
			dl->AddRectFilled(cp, ImVec2(cp.x + cs.x, cp.y + headH), IM_COL32(255, 210, 110, 48), 6.0f,
			                  ImDrawFlags_RoundCornersTop);
			dl->AddRect(cp, ImVec2(cp.x + cs.x, cp.y + cs.y), IM_COL32(255, 210, 110, 130), 6.0f);
			if (st.editingComment == cb.id) continue; // title drawn by the InputText below
			const char* title = cb.text.empty() ? "(double-click to name)" : cb.text.c_str();
			dl->AddText(font, fsz, ImVec2(cp.x + 6.0f * zoom, cp.y + 4.0f * zoom),
			            cb.text.empty() ? IM_COL32(230, 210, 160, 130) : IM_COL32(240, 225, 190, 255), title);
		}
	};

	// Comment interaction runs BEFORE node interaction so grabbing a comment header (or
	// its resize grip) does not also drag a node underneath. Returns true when a comment
	// is being grabbed → the component skips its own node/selection/pan handling.
	m.interactBehind = [&st, &commentEdit](ImVec2 origin, ImVec2 pan, float zoom, bool hovered) -> bool
	{
		bool grabbed = false;
		int  deleteComment = 0;
		for (auto& cb : st.graph.comments)
		{
			const ImVec2 cp(origin.x + pan.x + cb.x * zoom, origin.y + pan.y + cb.y * zoom);
			const ImVec2 cs(cb.w * zoom, cb.h * zoom);
			const float  headH = 24.0f * zoom;
			ImGui::PushID(cb.id);
			const bool editingTitle = st.editingComment == cb.id;
			if (!editingTitle)
			{
				// Header: a drag handle that moves the box AND every node whose center lies
				// inside it (group semantics); double-click swaps in an InputText.
				ImGui::SetCursorScreenPos(cp);
				ImGui::InvisibleButton("##cmove", ImVec2(std::max(cs.x, 1.0f), headH),
				                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
				// Hovering the header hands ALL its interaction to the comment (move on
				// left-drag, context menu on right-click, rename on double-click) and keeps
				// the shared canvas from also box-selecting / opening the add-node popup here.
				if (ImGui::IsItemHovered()) grabbed = true;
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					st.editingComment = cb.id;
				else if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
				{
					const float dxg = ImGui::GetIO().MouseDelta.x / zoom;
					const float dyg = ImGui::GetIO().MouseDelta.y / zoom;
					for (auto& nn : st.graph.nodes)
					{
						const float cxn = nn.x + kNodeW * 0.5f, cyn = nn.y + 40.0f; // ≈ node center
						if (cxn >= cb.x && cxn <= cb.x + cb.w && cyn >= cb.y && cyn <= cb.y + cb.h)
						{ nn.x += dxg; nn.y += dyg; }
					}
					cb.x += dxg; cb.y += dyg;
				}
				if (ImGui::IsItemActive() || ImGui::IsItemActivated()) grabbed = true;
				if (ImGui::IsItemDeactivated()) commentEdit = true; // move finished → persist
				if (ImGui::BeginPopupContextItem("##cmtCtx"))
				{
					if (ImGui::MenuItem("Rename")) st.editingComment = cb.id;
					if (ImGui::MenuItem("Delete Comment")) deleteComment = cb.id;
					ImGui::EndPopup();
				}
			}
			else
			{
				ImGui::SetCursorScreenPos(ImVec2(cp.x + 6.0f * zoom, cp.y + 2.0f * zoom));
				ImGui::SetNextItemWidth(std::max(cs.x - 12.0f * zoom, 40.0f));
				ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0, 0, 0, 0.25f));
				ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0.30f));
				ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0, 0, 0, 0.35f));
				pushWidgetScale(zoom);
				if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive())
					ImGui::SetKeyboardFocusHere();
				ImGui::InputTextWithHint("##ctitle", "comment", &cb.text);
				if (ImGui::IsItemDeactivated()) { st.editingComment = 0; commentEdit = true; }
				popWidgetScale();
				ImGui::PopStyleColor(3);
				grabbed = true; // editing the title consumes the mouse
			}
			// Resize grip (bottom-right corner).
			const float grip = 14.0f * zoom;
			ImGui::SetCursorScreenPos(ImVec2(cp.x + cs.x - grip, cp.y + cs.y - grip));
			ImGui::InvisibleButton("##cresize", ImVec2(grip, grip));
			if (ImGui::IsItemHovered() || ImGui::IsItemActive())
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
			{
				cb.w = std::max(80.0f, cb.w + ImGui::GetIO().MouseDelta.x / zoom);
				cb.h = std::max(60.0f, cb.h + ImGui::GetIO().MouseDelta.y / zoom);
			}
			if (ImGui::IsItemHovered() || ImGui::IsItemActive()) grabbed = true;
			if (ImGui::IsItemDeactivated()) commentEdit = true; // resize finished → persist
			// Grip decoration.
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddTriangleFilled(ImVec2(cp.x + cs.x - 2, cp.y + cs.y - grip + 2),
			                      ImVec2(cp.x + cs.x - 2, cp.y + cs.y - 2),
			                      ImVec2(cp.x + cs.x - grip + 2, cp.y + cs.y - 2),
			                      IM_COL32(255, 210, 110, 150));
			ImGui::PopID();
		}
		if (deleteComment != 0)
		{
			st.graph.comments.erase(std::remove_if(st.graph.comments.begin(), st.graph.comments.end(),
				[&](const HE::MatGraphComment& c){ return c.id == deleteComment; }),
				st.graph.comments.end());
			commentEdit = true;
		}
		(void)hovered;
		return grabbed;
	};

	// ── Per-node preview highlight: cyan halo + dot on the previewed node. ──
	m.drawFront = [&st, &ctx](ImDrawList* dl, ImVec2 origin, ImVec2 pan, float zoom)
	{
		if (st.previewNodeId == 0) return;
		const MatGraphNode* n = st.graph.findNode(st.previewNodeId);
		if (!n) return;
		// Match the shared canvas box height (title + pin rows + body, in graph units).
		const std::vector<GraphEditor::Pin> pins = matNodePins(ctx, *n);
		int left = 0, right = 0;
		for (const auto& p : pins) (p.input ? left : right)++;
		const ImVec2 p(origin.x + pan.x + n->x * zoom, origin.y + pan.y + n->y * zoom);
		const float bodyH = matNodeBodyHeight(*n);
		const float h = (GraphEditor::kTitleH + std::max(left, right) * GraphEditor::kRowH + bodyH + 6.0f) * zoom;
		const float w = GraphEditor::kNodeW * zoom;
		dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(80, 200, 255, 220), 5.0f, 0, 2.0f);
		dl->AddCircleFilled(ImVec2(p.x + w - 2.0f * zoom, p.y - 2.0f * zoom), 5.0f * zoom, IM_COL32(80, 200, 255, 255));
	};

	// ── Per-node context menu (Open Function / Preview This Node / Delete). ──
	m.drawNodeContextMenu = [&st, &deleteNode, &togglePreviewNode, &ctx](int nodeId)
	{
		const MatGraphNode* n = st.graph.findNode(nodeId);
		if (!n) return;
		const bool deletable = n->type != MatNodeType::Output;
		if (n->type == MatNodeType::FunctionCall && !n->s.empty() && ctx.contentManager)
			if (ImGui::MenuItem("Open Function"))
				s_openAssetRequest =
					(std::filesystem::path(ctx.contentManager->contentRoot()) / n->s).string();
		// Route THIS node's first output (unlit) onto the preview mesh.
		std::vector<HE::MatPinDesc> dIn, dOut;
		const std::vector<HE::MatPinDesc>* outs = &HE::matNodeDesc(n->type).outputs;
		if (n->type == MatNodeType::FunctionCall)
			if (const HE::MaterialGraph* fn = loadFunctionGraph(ctx, n->s))
			{ HE::matFunctionPins(*fn, dIn, dOut); outs = &dOut; }
		if (!st.isFunction && n->type != MatNodeType::Output && !outs->empty())
		{
			const bool on = st.previewNodeId == nodeId;
			if (ImGui::MenuItem("Preview This Node", nullptr, on)) togglePreviewNode = nodeId;
		}
		if (ImGui::MenuItem("Delete Node", nullptr, false, deletable)) deleteNode = nodeId;
	};

	// ── Double-click a FunctionCall node → open that function's editor tab. ──
	m.onNodeDoubleClick = [&st, &ctx](int nodeId)
	{
		const MatGraphNode* n = st.graph.findNode(nodeId);
		if (n && n->type == MatNodeType::FunctionCall && !n->s.empty() && ctx.contentManager)
			s_openAssetRequest =
				(std::filesystem::path(ctx.contentManager->contentRoot()) / n->s).string();
	};

	// ── Add-node palette (searchable; ports the original popup body). ──
	m.drawAddMenu = [&st, &ctx, &commentEdit]() -> int
	{
		int created = 0;
		const ImVec2 gp = st.geState.addMenuGraphPos;
		const float gx = gp.x, gy = gp.y;
		static std::string s_search;
		if (ImGui::IsWindowAppearing()) { s_search.clear(); ImGui::SetKeyboardFocusHere(); }
		ImGui::SetNextItemWidth(230.0f);
		ImGui::InputTextWithHint("##nodeSearch", "Search nodes...", &s_search);
		ImGui::Separator();
		auto lower = [](std::string v){ std::transform(v.begin(), v.end(), v.begin(),
			[](unsigned char ch){ return (char)std::tolower(ch); }); return v; };
		const std::string q = lower(s_search);
		auto matches = [&](const char* name, const char* cat)
		{ return q.empty() || lower(name).find(q) != std::string::npos
		      || lower(cat).find(q) != std::string::npos; };

		ImGui::BeginChild("##nodeList", ImVec2(232.0f, 300.0f), ImGuiChildFlags_None);
		// Comment boxes are pure editor chrome.
		if (matches("Comment Box", "Editor"))
		{
			ImGui::TextDisabled("Editor");
			if (ImGui::Selectable("Comment Box"))
			{
				HE::MatGraphComment cbx;
				cbx.id   = st.graph.nextId++;
				cbx.text = "Comment";
				cbx.x = gx; cbx.y = gy;
				st.graph.comments.push_back(std::move(cbx));
				commentEdit = true;
				ImGui::CloseCurrentPopup();
			}
			ImGui::Spacing();
		}
		const char* lastCat = "";
		for (const auto& d : HE::matNodeRegistry())
		{
			// Exactly one material Output; FunctionCall inserted from the functions list.
			if (d.type == MatNodeType::Output || d.type == MatNodeType::FunctionCall) continue;
			const bool fnInterface = d.type == MatNodeType::FnInput || d.type == MatNodeType::FnOutput;
			if (fnInterface && !st.isFunction) continue;
			if (!matches(d.name, d.category)) continue;
			if (std::string(lastCat) != d.category)
			{
				if (*lastCat) ImGui::Spacing();
				ImGui::TextDisabled("%s", d.category);
				lastCat = d.category;
			}
			if (ImGui::Selectable(d.name))
			{
				created = st.graph.addNode(d.type, gx, gy);
				ImGui::CloseCurrentPopup();
			}
		}
		// Project-wide material functions (insert as FunctionCall).
		if (ctx.contentManager)
		{
			bool headerShown = false;
			for (const HE::UUID& fnId : ctx.contentManager->enumerateIds(HE::AssetType::MaterialFunction))
			{
				const MaterialFunctionAsset* fn = ctx.contentManager->getMaterialFunction(fnId);
				if (!fn) continue;
				const std::string fnName = std::filesystem::path(fn->path).stem().string();
				if (!matches(fnName.c_str(), "Function")) continue;
				if (st.isFunction && fn->path == st.relPath) continue; // no direct self-call
				if (!headerShown)
				{
					if (*lastCat) ImGui::Spacing();
					ImGui::TextDisabled("Material Functions");
					headerShown = true;
				}
				if (ImGui::Selectable(fnName.c_str()))
				{
					const int id = st.graph.addNode(MatNodeType::FunctionCall, gx, gy);
					st.graph.findNode(id)->s = fn->path;
					created = id;
					ImGui::CloseCurrentPopup();
				}
			}
		}
		ImGui::EndChild();
		return created;
	};

	// ── Draw + interact ──────────────────────────────────────────────────────────
	// The component takes its origin from the cursor screen-pos at entry — capture the
	// same point so paste/duplicate can map the mouse into graph space afterwards.
	const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
	const bool changed = GraphEditor::draw("##mat_graphcanvas", m, st.geState, avail);
	if (changed) structuralEdit = true; // add / connect / delete / move → snapshot

	// Per-node preview toggle (deferred out of the context-menu lambda).
	if (togglePreviewNode != 0)
	{
		st.previewNodeId = (st.previewNodeId == togglePreviewNode) ? 0 : togglePreviewNode;
		st.previewDirty = true;
	}

	// ── Keyboard: undo/redo + node clipboard (Cmd on macOS, Ctrl elsewhere). ──────
	{
		const ImGuiIO& kio = ImGui::GetIO();
		const bool mod  = kio.KeyCtrl || kio.KeySuper;
		const bool kbOk = ImGui::IsWindowHovered() && !kio.WantTextInput && !ImGui::IsAnyItemActive();
		auto& sel = st.geState.selection;
		if (kbOk && mod && ImGui::IsKeyPressed(ImGuiKey_Z))
		{
			// Undo steps back one snapshot; Shift+Z (redo) steps forward.
			const int target = kio.KeyShift ? st.undoPos + 1 : st.undoPos - 1;
			if (restoreSnapshot(st, target) && assetOk)
			{
				applyToMaterial(st, ctx); // push into the live material WITHOUT a new snapshot
				st.previewDirty = true;
			}
		}
		if (kbOk && mod && ImGui::IsKeyPressed(ImGuiKey_Y)) // redo (Ctrl+Y)
		{
			if (restoreSnapshot(st, st.undoPos + 1) && assetOk)
			{
				applyToMaterial(st, ctx);
				st.previewDirty = true;
			}
		}
		if (kbOk && mod && ImGui::IsKeyPressed(ImGuiKey_C) && !sel.empty())
		{
			const std::string payload = serializeSelection(st);
			if (!payload.empty()) g_matClipboard = payload;
		}
		if (kbOk && mod && ImGui::IsKeyPressed(ImGuiKey_X) && !sel.empty())
		{
			// Cut = copy + delete (interface nodes excluded from BOTH).
			const std::string payload = serializeSelection(st);
			if (!payload.empty())
			{
				g_matClipboard = payload;
				for (int sid : sel)
					if (const MatGraphNode* sn = st.graph.findNode(sid);
					    sn && sn->type != MatNodeType::Output &&
					    sn->type != MatNodeType::FnOutput && sn->type != MatNodeType::FnInput)
						st.graph.removeNode(sid);
				sel.clear();
				st.geState.selected = 0;
				structuralEdit = true;
			}
		}
		if (kbOk && mod && ImGui::IsKeyPressed(ImGuiKey_V) && !g_matClipboard.empty())
		{
			// Paste at the mouse when it's over the canvas, else into the visible center.
			const ImVec2 org = canvasOrigin;
			const ImVec2 mp  = kio.MousePos;
			const bool overCanvas = mp.x >= org.x && mp.x <= org.x + avail.x &&
			                        mp.y >= org.y && mp.y <= org.y + avail.y;
			const float Z = st.geState.zoom;
			const float gx = ((overCanvas ? mp.x : org.x + avail.x * 0.5f) - org.x - st.geState.pan.x) / Z;
			const float gy = ((overCanvas ? mp.y : org.y + avail.y * 0.5f) - org.y - st.geState.pan.y) / Z;
			if (pasteInto(st, g_matClipboard, gx, gy)) structuralEdit = true;
		}
		if (kbOk && mod && ImGui::IsKeyPressed(ImGuiKey_D) && !sel.empty())
		{
			// Duplicate in place (slight offset), without touching the shared clipboard.
			float mnx = FLT_MAX, mny = FLT_MAX;
			for (int sid : sel)
				if (const MatGraphNode* sn = st.graph.findNode(sid))
				{ mnx = std::min(mnx, sn->x); mny = std::min(mny, sn->y); }
			const std::string payload = serializeSelection(st);
			if (mnx != FLT_MAX && pasteInto(st, payload, mnx + 28.0f, mny + 28.0f))
				structuralEdit = true;
		}
	}
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

bool isTextureAsset(const std::string& path)
{
	static std::map<std::string, bool> s_texCache;
	if (auto it = s_texCache.find(path); it != s_texCache.end()) return it->second;
	HAsset::Reader r;
	const bool isTex = r.open(path) &&
		r.assetType() == static_cast<uint16_t>(HE::AssetType::Texture);
	s_texCache[path] = isTex;
	return isTex;
}

bool isMaterialFunctionAsset(const std::string& path)
{
	static std::map<std::string, bool> s_typeCache;
	if (auto it = s_typeCache.find(path); it != s_typeCache.end()) return it->second;
	HAsset::Reader r;
	const bool isFn = r.open(path) &&
		r.assetType() == static_cast<uint16_t>(HE::AssetType::MaterialFunction);
	s_typeCache[path] = isFn;
	return isFn;
}

bool isDirty(const std::string& assetPath)
{
	auto it = g_states.find(assetPath);
	return it != g_states.end() && it->second.dirty;
}

void forget(const std::string& assetPath) { g_states.erase(assetPath); }

std::string takeOpenRequest()
{
	std::string r = std::move(s_openAssetRequest);
	s_openAssetRequest.clear();
	return r;
}

// Material-INSTANCE editor pane: one row per parent parameter (override checkbox +
// typed widget honoring slider metadata + tooltip), grouped like the central panel,
// plus the parent's static switches (overriding one REBUILDS this instance's shader —
// its own permutation; that recompile is the feature, not an accident).
bool drawInstanceOverridePanel(AppContext& ctx, State& st, MaterialAsset& inst)
{
	bool valueEdit = false, structureEdit = false;
	auto ovIndex = [&](const std::string& nm) -> int
	{
		for (size_t i = 0; i < inst.instanceOverriddenParams.size(); ++i)
			if (inst.instanceOverriddenParams[i] == nm) return (int)i;
		return -1;
	};

	ImGui::TextDisabled("Parameter Overrides");
	ImGui::SameLine();
	ImGui::TextDisabled("(checked = this instance's own value)");
	ImGui::Separator();

	// Same grouping as the master's panel, from the synced metadata arrays.
	std::vector<std::string> groupOrder;
	auto groupOf = [&](size_t i) -> std::string
	{ return i < inst.graphParamGroups.size() ? inst.graphParamGroups[i] : std::string(); };
	for (size_t i = 0; i < inst.graphParamNames.size(); ++i)
		if (std::find(groupOrder.begin(), groupOrder.end(), groupOf(i)) == groupOrder.end())
			groupOrder.push_back(groupOf(i));
	std::stable_sort(groupOrder.begin(), groupOrder.end(),
		[](const std::string& a, const std::string& b){ return a.empty() && !b.empty(); });

	for (const std::string& grp : groupOrder)
	{
		if (!grp.empty()) { ImGui::Spacing(); ImGui::SeparatorText(grp.c_str()); }
		for (size_t i = 0; i < inst.graphParamNames.size(); ++i)
		{
			if (groupOf(i) != grp) continue;
			if (i * 4 + 3 >= inst.shaderParamData.size()) continue;
			const std::string& nm = inst.graphParamNames[i];
			float* v = &inst.shaderParamData[i * 4];
			const auto kind = i < inst.graphParamTypes.size()
				? static_cast<HE::MatParamKind>(inst.graphParamTypes[i]) : HE::MatParamKind::Float;
			ImGui::PushID((int)i);
			bool ov = ovIndex(nm) >= 0;
			if (ImGui::Checkbox("##ov", &ov))
			{
				if (ov) inst.instanceOverriddenParams.push_back(nm);
				else    inst.instanceOverriddenParams.erase(
					        inst.instanceOverriddenParams.begin() + ovIndex(nm));
				structureEdit = true; // un-override → value falls back to the parent (sync)
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(!ov);
			ImGui::SetNextItemWidth(190.0f);
			switch (kind)
			{
				case HE::MatParamKind::Color: ImGui::ColorEdit3("##v", v, ImGuiColorEditFlags_Float); break;
				case HE::MatParamKind::Vec2:  ImGui::DragFloat2("##v", v, 0.01f); break;
				case HE::MatParamKind::Vec4:  ImGui::DragFloat4("##v", v, 0.01f); break;
				case HE::MatParamKind::Bool:
				{
					bool on = v[0] > 0.5f;
					if (ImGui::Checkbox("##v", &on)) { v[0] = on ? 1.0f : 0.0f; valueEdit = true; }
					break;
				}
				default: // Float — bounded slider when the parent authored a range
				{
					const float mn = i * 2 + 1 < inst.graphParamMinMax.size() ? inst.graphParamMinMax[i*2]   : 0.0f;
					const float mx = i * 2 + 1 < inst.graphParamMinMax.size() ? inst.graphParamMinMax[i*2+1] : 0.0f;
					if (mn < mx) ImGui::SliderFloat("##v", v, mn, mx);
					else         ImGui::DragFloat("##v", v, 0.01f);
					break;
				}
			}
			if (ImGui::IsItemDeactivatedAfterEdit()) valueEdit = true;
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextUnformatted(nm.empty() ? "param" : nm.c_str());
			if (i < inst.graphParamTooltips.size() && !inst.graphParamTooltips[i].empty())
			{
				ImGui::SameLine();
				ImGui::TextDisabled("(?)");
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", inst.graphParamTooltips[i].c_str());
			}
			ImGui::PopID();
		}
	}

	// ── Static switches: overriding one bakes a different permutation. ──
	bool switchEdit = false;
	if (!st.parentSwitches.empty())
	{
		ImGui::Spacing();
		ImGui::SeparatorText("Static Switches");
		ImGui::TextDisabled("(compile-time — toggling rebuilds this instance's shader)");
		for (const auto& [swName, swDefault] : st.parentSwitches)
		{
			ImGui::PushID(swName.c_str());
			int idx = -1;
			for (size_t i = 0; i < inst.instanceSwitchNames.size(); ++i)
				if (inst.instanceSwitchNames[i] == swName) { idx = (int)i; break; }
			bool ov = idx >= 0;
			if (ImGui::Checkbox("##sov", &ov))
			{
				if (ov)
				{
					inst.instanceSwitchNames.push_back(swName);
					inst.instanceSwitchValues.push_back(swDefault ? 1 : 0);
				}
				else
				{
					inst.instanceSwitchNames.erase(inst.instanceSwitchNames.begin() + idx);
					inst.instanceSwitchValues.erase(inst.instanceSwitchValues.begin() + idx);
				}
				switchEdit = true;
			}
			ImGui::SameLine();
			bool val = idx >= 0 ? inst.instanceSwitchValues[idx] != 0 : swDefault;
			ImGui::BeginDisabled(idx < 0 && !ov);
			if (ImGui::Checkbox(swName.c_str(), &val) && idx >= 0)
			{
				inst.instanceSwitchValues[idx] = val ? 1 : 0;
				switchEdit = true;
			}
			ImGui::EndDisabled();
			ImGui::PopID();
		}
	}

	if (structureEdit || switchEdit)
		ctx.contentManager->syncMaterialInstance(st.materialId); // re-derive from the parent
	if (valueEdit || structureEdit || switchEdit)
	{
		st.dirty = true;
		st.lastGlsl = inst.customShaderFragGlsl; // shader view follows permutation changes
		return true;
	}
	return false;
}

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

	MaterialAsset* mat = (!st.isFunction && ctx.contentManager)
		? ctx.contentManager->getMaterialMutable(st.materialId) : nullptr;
	MaterialFunctionAsset* fnAsset = (st.isFunction && ctx.contentManager)
		? ctx.contentManager->getMaterialFunctionMutable(st.materialId) : nullptr;
	const bool assetOk = mat || fnAsset;

	// ── Header: name, view toggle, save ────────────────────────────────────────
	ImGui::TextUnformatted(st.name.c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("%s%s",
	                    st.isInstance ? "material instance"
	                                  : (st.isFunction ? "material function" : "material graph"),
	                    st.dirty ? "  (unsaved)" : "");
	// Shader complexity gauge (updated on every regenerate).
	if (!st.isFunction && !st.complexity.empty())
	{
		ImGui::SameLine();
		ImGui::TextDisabled("·  %s", st.complexity.c_str());
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Estimated cost of the generated shader\n"
			                  "(statements + weighted texture/noise calls)");
	}
	// Blend mode — a MATERIAL-level setting (changes the Output node's pins + which
	// render pass the material uses), so it lives in the header, not on the canvas.
	bool headerEdit = false;
	if (!st.isFunction && !st.isInstance)
	{
		MatGraphNode* outN = nullptr;
		for (auto& n : st.graph.nodes)
			if (n.type == MatNodeType::Output) { outN = &n; break; }
		if (outN)
		{
			const int  bmCur  = std::clamp(static_cast<int>(outN->p[1]), 0, 2);
			const bool masked = bmCur == 1;
			ImGui::SameLine(ImGui::GetContentRegionAvail().x - (masked ? 610.0f : 480.0f));
			ImGui::TextDisabled("Blend");
			ImGui::SameLine();
			static const char* kBlend[] = { "Opaque", "Masked", "Translucent" };
			int bm = bmCur;
			ImGui::SetNextItemWidth(110.0f);
			if (ImGui::Combo("##blend", &bm, kBlend, 3))
			{
				outN->p[1] = (float)bm;
				if (bm == 1 && outN->p[2] <= 0.0f) outN->p[2] = 0.5f; // sane default cutoff
				headerEdit = true;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Opaque: solid (no Opacity pin)\n"
				                  "Masked: OpacityMask pin — fragments below Clip discard\n"
				                  "Translucent: Opacity pin — sorted alpha-blend pass");
			if (masked)
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth(110.0f);
				ImGui::DragFloat("Clip", &outN->p[2], 0.01f, 0.01f, 1.0f);
				headerEdit |= ImGui::IsItemDeactivatedAfterEdit();
			}
		}
	}
	// Graph|Overrides / Shader-code toggle for the right pane (functions have no shader).
	if (!st.isFunction)
	{
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 330.0f);
		if (ImGui::RadioButton(st.isInstance ? "Overrides" : "Graph", st.viewMode == 0)) st.viewMode = 0;
		ImGui::SameLine();
		if (ImGui::RadioButton("Shader Code", st.viewMode == 1)) st.viewMode = 1;
	}
	if (st.isInstance && mat)
	{
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 250.0f);
		if (ImGui::Button("Open Parent") && ctx.contentManager)
			s_openAssetRequest = (std::filesystem::path(ctx.contentManager->contentRoot())
			                      / mat->parentMaterialPath).string();
	}
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 140.0f);
	if (ImGui::Button(st.isFunction ? "Save Function" : "Save Material") && assetOk)
	{
		// Instances have no graph — their state is already live on the asset; masters
		// regenerate first so the saved file always matches the canvas.
		if (!st.isInstance) applyToMaterial(st, ctx);
		RuntimeAsset* toSave = st.isFunction ? static_cast<RuntimeAsset*>(fnAsset)
		                                     : static_cast<RuntimeAsset*>(mat);
		if (toSave && ctx.contentManager->saveAsset(*toSave)) st.dirty = false;
		Logger::Log(Logger::LogLevel::Info,
			("MaterialEditor: saved '" + st.name + "'").c_str());
	}
	if (!assetOk)
		ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Asset could not be loaded.");
	ImGui::Separator();

	// Edit flags — both columns contribute; applied once at the end.
	bool structuralEdit = false; // connect/disconnect/add/delete → apply immediately
	bool paramEdit      = false; // committed inline widget edit → apply
	bool panelEdit      = false; // committed edit in the side properties panel
	bool commentEdit    = false; // comment box moved/resized/renamed → persist (no shader change)
	int  deleteNode     = 0;
	if (headerEdit) paramEdit = true; // blend-mode change in the header → regenerate

	// ── Left column: material preview (top) + properties panel (scrollable) ──────
	const float leftW = 300.0f;
	ImGui::BeginChild("##matLeft", ImVec2(leftW, 0), ImGuiChildFlags_Borders);
	{
		ImGui::TextDisabled("Preview");
		// Preview primitive selector (sphere shows curvature, cube face seams, plane UVs).
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 92.0f);
		ImGui::SetNextItemWidth(96.0f);
		static const char* kShapes[] = { "Sphere", "Cube", "Plane" };
		if (ImGui::Combo("##pshape", &st.previewShape, kShapes, 3)) st.previewDirty = true;
		ImGui::BeginChild("##matPreview", ImVec2(0, 240), ImGuiChildFlags_Borders);
		{
			// Live material preview: a sphere shaded with THIS material, rendered
			// offscreen (transparent background) and composited over a gradient — an
			// orbit camera you drag to rotate and wheel to zoom, like a mini viewport.
			const ImVec2 org = ImGui::GetCursorScreenPos();
			const ImVec2 av  = ImGui::GetContentRegionAvail();
			ImDrawList* pdl = ImGui::GetWindowDrawList();
			// Vertical gradient backdrop.
			pdl->AddRectFilledMultiColor(org, ImVec2(org.x + av.x, org.y + av.y),
				IM_COL32(58, 62, 74, 255), IM_COL32(58, 62, 74, 255),
				IM_COL32(22, 24, 30, 255), IM_COL32(22, 24, 30, 255));

			const int px = (int)std::min(av.x, av.y);
			if (px != st.previewPx) { st.previewPx = px; st.previewDirty = true; }
			// The preview target is shared across material tabs, so re-render whenever the
			// last-drawn material isn't ours (e.g. after a tab switch), plus on any local
			// change (camera/size/material edit). Reuse the handle otherwise.
			static HE::UUID s_lastPreviewMat{};
			if (s_lastPreviewMat != st.materialId) st.previewDirty = true;
			if (st.previewDirty && !st.isFunction && ctx.renderer && ctx.contentManager &&
			    st.materialId != HE::UUID{} && px >= 32)
			{
				// Per-node preview: route the flagged node's first output straight into an
				// UNLIT BaseColor on a COPY of the graph, and swap the generated shader into
				// the material just for this render. Pipelines are cached by source hash, so
				// both variants coexist and swapping back costs nothing.
				bool swapped = false;
				std::string origGlsl; std::vector<float> origData; std::vector<std::string> origTex;
				if (st.previewNodeId != 0 && !st.graph.findNode(st.previewNodeId))
					st.previewNodeId = 0; // node got deleted → fall back to the material
				if (st.previewNodeId != 0 && mat)
				{
					HE::MaterialGraph pg = st.graph;
					int outId = 0;
					for (auto& nn : pg.nodes)
						if (nn.type == MatNodeType::Output) { outId = nn.id; nn.p[0] = 0.0f; break; }
					if (outId)
					{
						for (int pin = 0; pin < 5; ++pin) pg.disconnectInput(outId, pin);
						pg.connect(st.previewNodeId, 0, outId, 0);
						HE::MatFunctionLoader loader = [&ctx](const std::string& path)
						{ return loadFunctionGraph(ctx, path); };
						const HE::MatShaderGen pgen = HE::generateFragment(pg, loader);
						origGlsl = mat->customShaderFragGlsl;
						origData = mat->shaderParamData;
						origTex  = mat->graphTexturePaths;
						mat->customShaderFragGlsl = pgen.glsl;
						mat->shaderParamData.clear();
						for (const auto& slot : pgen.params)
							mat->shaderParamData.insert(mat->shaderParamData.end(),
							                            slot.value, slot.value + 4);
						mat->graphTexturePaths = pgen.textures;
						swapped = true;
					}
				}
				st.previewTex = ctx.renderer->RenderMaterialPreview(*ctx.contentManager, st.materialId,
					(uint32_t)px, st.previewYaw, st.previewPitch, st.previewDist, st.previewShape);
				if (swapped && mat)
				{
					mat->customShaderFragGlsl = std::move(origGlsl);
					mat->shaderParamData      = std::move(origData);
					mat->graphTexturePaths    = std::move(origTex);
				}
				st.previewDirty  = false;
				s_lastPreviewMat = st.materialId;
			}
			// Material FUNCTIONS preview too: wrap the function in a lazily registered
			// scratch material (Output ← FunctionCall) and render that. The function's
			// codegen cache is invalidated on every committed edit, so the scratch
			// always reflects the current graph.
			else if (st.previewDirty && st.isFunction && ctx.renderer && ctx.contentManager &&
			         px >= 32)
			{
				bool hasOut = false;
				for (const auto& nn : st.graph.nodes)
					if (nn.type == MatNodeType::FnOutput) { hasOut = true; break; }
				if (hasOut && !st.relPath.empty())
				{
					if (st.fnPreviewMatId == HE::UUID{})
					{
						MaterialAsset scratch;
						scratch.type = HE::AssetType::Material;
						scratch.name = "__fnPreview_" + st.name;
						st.fnPreviewMatId = ctx.contentManager->registerMaterial(std::move(scratch));
					}
					HE::MaterialGraph pg;
					const int out  = pg.addNode(MatNodeType::Output);
					pg.findNode(out)->p[0] = 1.0f; // lit, so the function reads like a surface
					const int call = pg.addNode(MatNodeType::FunctionCall);
					pg.findNode(call)->s = st.relPath;
					pg.connect(call, 0, out, 0);   // first FnOutput → BaseColor
					HE::MatFunctionLoader loader = [&ctx](const std::string& path)
					{ return loadFunctionGraph(ctx, path); };
					const HE::MatShaderGen gen = HE::generateFragment(pg, loader);
					if (MaterialAsset* sm = ctx.contentManager->getMaterialMutable(st.fnPreviewMatId))
					{
						sm->customShaderFragGlsl = gen.glsl;
						sm->shaderParamData.clear();
						for (const auto& slot : gen.params)
							sm->shaderParamData.insert(sm->shaderParamData.end(),
							                           slot.value, slot.value + 4);
						sm->graphTexturePaths = gen.textures;
						st.previewTex = ctx.renderer->RenderMaterialPreview(*ctx.contentManager,
							st.fnPreviewMatId, (uint32_t)px, st.previewYaw, st.previewPitch,
							st.previewDist, st.previewShape);
					}
				}
				st.previewDirty  = false;
				s_lastPreviewMat = st.materialId;
			}
			void* tex = st.previewTex;

			if (tex)
			{
				const bool flipY = (ctx.backend == HE::RendererBackend::OpenGL);
				const float side = (float)px;
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (av.x - side) * 0.5f);
				ImGui::Image(reinterpret_cast<ImTextureID>(tex), ImVec2(side, side),
					flipY ? ImVec2(0, 1) : ImVec2(0, 0),
					flipY ? ImVec2(1, 0) : ImVec2(1, 1));
			}
			else
				pdl->AddText(ImVec2(org.x + av.x * 0.5f - 28.0f, org.y + av.y * 0.5f - 6.0f),
					IM_COL32(150, 150, 150, 255), "(preview)");

			// Orbit interaction over the whole preview area (an invisible button on top).
			ImGui::SetCursorScreenPos(org);
			ImGui::InvisibleButton("##orbit", ImVec2(std::max(av.x, 1.0f), std::max(av.y, 1.0f)));
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
			{
				const ImVec2 md = ImGui::GetIO().MouseDelta;
				if (md.x != 0.0f || md.y != 0.0f)
				{
					st.previewYaw   -= md.x * 0.01f;
					st.previewPitch  = std::clamp(st.previewPitch + md.y * 0.01f, -1.45f, 1.45f);
					st.previewDirty  = true;
				}
			}
			if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f)
			{
				st.previewDist  = std::clamp(st.previewDist - ImGui::GetIO().MouseWheel * 0.25f, 1.6f, 8.0f);
				st.previewDirty = true;
			}
		}
		ImGui::EndChild();
		// Per-node preview indicator: shows WHICH node output the ball is displaying
		// (unlit) and offers the way back to previewing the whole material.
		if (st.previewNodeId != 0)
		{
			const MatGraphNode* pn = st.graph.findNode(st.previewNodeId);
			ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "Previewing: %s",
				pn ? HE::matNodeDesc(pn->type).name : "?");
			ImGui::SameLine();
			if (ImGui::SmallButton("Show Material"))
			{ st.previewNodeId = 0; st.previewDirty = true; }
		}
		ImGui::Spacing();
		ImGui::TextDisabled(st.isFunction ? "Interface" : "Properties");
		ImGui::BeginChild("##matProps", ImVec2(0, 0), ImGuiChildFlags_Borders);
		if (st.isInstance)
			ImGui::TextDisabled("(instance — edit overrides on the right)");
		else if (!st.isFunction)
			panelEdit = drawParamConstPanel(st.graph);
		else
			// Functions: edit the interface (named, typed inputs/outputs) centrally.
			panelEdit = drawFunctionInterfacePanel(st.graph);
		ImGui::EndChild();
	}
	ImGui::EndChild();
	ImGui::SameLine();

	// ── Right column: graph canvas OR generated shader code ──────────────────────
	ImGui::BeginChild("##matRight", ImVec2(0, 0), ImGuiChildFlags_Borders);
	const bool showGraph = !st.isInstance && (st.isFunction || st.viewMode == 0);
	if (st.isInstance && st.viewMode == 0)
	{
		// Instance tabs replace the canvas with the override panel; edits are applied
		// straight to the live asset (no graph → no regenerate/undo machinery here).
		if (mat && drawInstanceOverridePanel(ctx, st, *mat))
			st.previewDirty = true;
	}
	else if (!showGraph)
	{
		// Read-only generated fragment GLSL.
		ImGui::TextDisabled("Generated fragment GLSL (read-only)");
		ImGui::InputTextMultiline("##shaderView", &st.lastGlsl, ImGui::GetContentRegionAvail(),
			ImGuiInputTextFlags_ReadOnly);
	}
	else
	{
	// Inline compile-error banner: the renderers fall back to magenta on a broken
	// shader — this SHOWS the compiler log instead of leaving the user guessing.
	if (!st.compileLog.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.42f, 0.10f, 0.10f, 1.0f));
		ImGui::BeginChild("##shaderErr", ImVec2(0, 26), ImGuiChildFlags_None);
		ImGui::SetCursorPos(ImVec2(8, 5));
		ImGui::TextUnformatted("Shader compile error — hover for details");
		if (ImGui::IsWindowHovered())
			ImGui::SetTooltip("%s", st.compileLog.c_str());
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}
	ImGui::BeginChild("##graphCanvas", ImGui::GetContentRegionAvail(), ImGuiChildFlags_None,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
	// The node canvas is now the shared GraphEditor component (same frontend as the
	// HorizonCode graph). drawMaterialCanvas builds the Model adapter over st.graph and
	// maps every material-specific feature (comments, on-node params, per-node preview,
	// context menu, function open, clipboard, undo) onto its hooks, setting our edit flags.
	drawMaterialCanvas(st, ctx, assetOk, ImGui::GetContentRegionAvail(),
	                   structuralEdit, paramEdit, commentEdit, deleteNode);
	ImGui::EndChild(); // ##graphCanvas
	}                  // end graph-view branch

	ImGui::EndChild(); // ##matRight

	// Structural / committed edits (either column) → regenerate + push into the live material.
	if (deleteNode != 0) { st.graph.removeNode(deleteNode); structuralEdit = true; }
	if ((structuralEdit || paramEdit || panelEdit || commentEdit) && assetOk && !st.isInstance)
	{
		applyToMaterial(st, ctx);
		pushUndo(st);           // every committed edit becomes an undo step
		st.previewDirty = true; // material changed → refresh the preview
	}

	ImGui::End();
}
} // namespace MaterialEditorPanel
