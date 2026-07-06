#include "MaterialEditorPanel.h"
#include "EditorApplication.h"                 // AppContext
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
	float         zoom    = 1.0f;    // canvas zoom (graph-space → screen scale)
	int           selectedNode = 0;  // primary selection (context menu / last clicked)
	std::vector<int> selection;      // all selected node ids (multi-select move / box-select)
	bool          boxSel   = false;  // rubber-band box-select in progress
	ImVec2        boxStart;          // box-select anchor (screen space)
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
	// Undo/redo: JSON snapshots of the whole graph (incl. comments). undoPos indexes the
	// snapshot the canvas currently shows; edits truncate the redo tail.
	std::vector<std::string> undo;
	int           undoPos = -1;
	// Live link drag: source pin (node, pin index, output side?) or inactive (node == 0).
	int  dragNode = 0, dragPin = 0;
	bool dragFromOutput = true;
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

// Pin hit-test data collected while drawing, consumed for link routing + drop targets.
struct PinPos { int node; int pin; bool output; ImVec2 pos; MatPinType type; };
// Screen-space node box, collected while drawing, consumed by box-select.
struct NodeBox { int id; ImVec2 mn, mx; };

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
	for (const auto& slot : gen.params)
	{
		mat->shaderParamData.insert(mat->shaderParamData.end(),
		                            slot.value, slot.value + 4);
		mat->graphParamNames.push_back(slot.name); // parallel to slots → runtime setMaterialParam
		mat->graphParamTypes.push_back(static_cast<uint8_t>(slot.kind)); // typed editors
	}
	// Project textures the graph samples, in slot order (heTexP0..) — the renderer
	// binds these on loose materials; packing bakes them to graphTextureIds (MTLU).
	mat->graphTexturePaths = gen.textures;
	st.dirty = true;

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
	st.selection.erase(std::remove_if(st.selection.begin(), st.selection.end(),
		[&](int id){ return st.graph.findNode(id) == nullptr; }), st.selection.end());
	if (st.selectedNode  && !st.graph.findNode(st.selectedNode))  st.selectedNode  = 0;
	if (st.previewNodeId && !st.graph.findNode(st.previewNodeId)) st.previewNodeId = 0;
	return true;
}

// ── Node clipboard (process-wide → copy/paste works ACROSS material tabs) ──────────
std::string g_matClipboard;

// Link dragged into empty canvas: the source pin awaiting the node picked from the
// add-popup (0 = none pending). Transient UI state, shared across tabs harmlessly.
int  s_pendingLinkNode = 0, s_pendingLinkPin = 0;
bool s_pendingLinkFromOutput = true;

// One-shot open-asset request (double-clicked Material Function node → its editor tab).
std::string s_openAssetRequest;

// Selected nodes + the links fully inside the selection, as graph JSON. Interface
// nodes are excluded: Output/FnOutput are singletons, FnInput defines a function's
// signature — duplicating either would corrupt the target graph.
std::string serializeSelection(const State& st)
{
	HE::MaterialGraph tmp;
	for (int id : st.selection)
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
	st.selection.clear();
	for (const auto& n : tmp.nodes)
	{
		MatGraphNode c = n;
		c.id = st.graph.nextId++;
		c.x  = n.x - mnx + atX;
		c.y  = n.y - mny + atY;
		remap[n.id] = c.id;
		st.selection.push_back(c.id);
		st.graph.nodes.push_back(std::move(c));
	}
	for (const auto& l : tmp.links)
		st.graph.links.push_back({ remap[l.srcNode], l.srcPin, remap[l.dstNode], l.dstPin });
	st.selectedNode = st.selection.empty() ? 0 : st.selection.front();
	return true;
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
		if (!mat || mat->nodeGraphJson.empty() ||
		    !HE::materialGraphFromJson(mat->nodeGraphJson, st.graph))
		{
			// No graph yet (fresh material, or one with only a hand-written shader):
			// start from the default. Nothing is written until the first edit.
			st.graph = MaterialGraph::makeDefault();
		}
		st.lastGlsl = HE::generateFragmentGlsl(st.graph);
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

// Vertical space the node reserves for its inline VALUE widgets (the name, for named
// nodes, lives in the colored header now — not in the body).
float nodeParamHeight(MatNodeType type)
{
	const HE::MatNodeDesc& d = HE::matNodeDesc(type);
	if (d.paramCount == 0 && type != MatNodeType::TextureSample) return 0.0f;
	if (type == MatNodeType::TextureSample) return 44.0f;         // filename + hint rows
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
// Nodes whose title is an editable NAME/label shown in the colored header (params +
// constants + function-interface pins). The name (n.s) drives the uniform name for
// params and the interface name for FnInput/FnOutput; for constants it is a cosmetic
// label for readability (does not affect codegen).
bool isNamedNode(MatNodeType t)
{
	return isParamNode(t) || isConstNode(t) ||
	       t == MatNodeType::FnInput || t == MatNodeType::FnOutput;
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
		for (MatGraphNode* n : params)
		{
			ImGui::PushID(n->id);
			committed |= nodeParamWidgets(*n);
			ImGui::PopID();
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
	ImGui::TextDisabled("%s%s", st.isFunction ? "material function" : "material graph",
	                    st.dirty ? "  (unsaved)" : "");
	// Graph / Shader-code toggle for the right pane (functions have no shader).
	if (!st.isFunction)
	{
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 330.0f);
		if (ImGui::RadioButton("Graph", st.viewMode == 0)) st.viewMode = 0;
		ImGui::SameLine();
		if (ImGui::RadioButton("Shader Code", st.viewMode == 1)) st.viewMode = 1;
	}
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 140.0f);
	if (ImGui::Button(st.isFunction ? "Save Function" : "Save Material") && assetOk)
	{
		applyToMaterial(st, ctx);
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
		ImGui::TextDisabled("Properties");
		ImGui::BeginChild("##matProps", ImVec2(0, 0), ImGuiChildFlags_Borders);
		if (!st.isFunction)
			panelEdit = drawParamConstPanel(st.graph);
		else
			ImGui::TextDisabled("(function graph — no exposed parameters)");
		ImGui::EndChild();
	}
	ImGui::EndChild();
	ImGui::SameLine();

	// ── Right column: graph canvas OR generated shader code ──────────────────────
	ImGui::BeginChild("##matRight", ImVec2(0, 0), ImGuiChildFlags_Borders);
	const bool showGraph = st.isFunction || st.viewMode == 0;
	if (!showGraph)
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

	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 avail  = ImGui::GetContentRegionAvail();

	// ── Scroll wheel: Cmd/Ctrl+wheel ZOOMS about the cursor; plain two-finger scroll
	// (or wheel) PANS — the trackpad-natural way to move the canvas around. ──────────
	if (ImGui::IsWindowHovered() && st.dragNode == 0 && !st.boxSel)
	{
		const ImGuiIO& io = ImGui::GetIO();
		const bool zoomMod = io.KeyCtrl || io.KeySuper;
		if (zoomMod && io.MouseWheel != 0.0f)
		{
			const float oldZoom = st.zoom;
			st.zoom = std::clamp(st.zoom * (1.0f + io.MouseWheel * 0.12f), 0.35f, 2.5f);
			const ImVec2 mp = io.MousePos; // keep the graph point under the cursor fixed
			const float gxr = (mp.x - origin.x - st.scroll.x) / oldZoom;
			const float gyr = (mp.y - origin.y - st.scroll.y) / oldZoom;
			st.scroll.x = mp.x - origin.x - gxr * st.zoom;
			st.scroll.y = mp.y - origin.y - gyr * st.zoom;
		}
		else if (!zoomMod && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f))
		{
			st.scroll.x += io.MouseWheelH * 28.0f;
			st.scroll.y += io.MouseWheel  * 28.0f;
		}
	}
	const float Z = st.zoom;
	ImFont* const font   = ImGui::GetFont();
	const float   fsz    = ImGui::GetFontSize() * Z; // draw-list text size at this zoom

	// Grid (spacing scales with zoom so the canvas feels anchored)
	dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(28, 28, 30, 255));
	const float grid = 32.0f * Z;
	for (float x = fmodf(st.scroll.x, grid); x < avail.x; x += grid)
		dl->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + avail.y), IM_COL32(255,255,255,10));
	for (float y = fmodf(st.scroll.y, grid); y < avail.y; y += grid)
		dl->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + avail.x, origin.y + y), IM_COL32(255,255,255,10));

	// Full-canvas background button FIRST (nodes draw on top). It owns all empty-space
	// interaction — pan + box-select — so this no longer depends on the fragile
	// "is any item hovered" heuristic that the per-node drag handles broke. AllowOverlap
	// lets the node buttons submitted afterwards take priority where they overlap.
	ImGui::SetCursorScreenPos(origin);
	ImGui::SetNextItemAllowOverlap();
	ImGui::InvisibleButton("##canvasbg", ImVec2(std::max(avail.x, 1.0f), std::max(avail.y, 1.0f)),
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
	const bool canvasActive  = ImGui::IsItemActive();
	const bool canvasHovered = ImGui::IsItemHovered();

	// ── Comment boxes (drawn + interacted BEFORE nodes → they sit behind them). ──
	// Dragging the header moves the box AND every node whose center lies inside it
	// (group semantics); the bottom-right grip resizes; the header text is editable.
	{
		int deleteComment = 0;
		for (auto& cb : st.graph.comments)
		{
			const ImVec2 cp(origin.x + st.scroll.x + cb.x * Z, origin.y + st.scroll.y + cb.y * Z);
			const ImVec2 cs(cb.w * Z, cb.h * Z);
			const float  headH = 24.0f * Z;
			ImGui::PushID(cb.id);
			dl->AddRectFilled(cp, ImVec2(cp.x + cs.x, cp.y + cs.y), IM_COL32(255, 210, 110, 16), 6.0f);
			dl->AddRectFilled(cp, ImVec2(cp.x + cs.x, cp.y + headH), IM_COL32(255, 210, 110, 48), 6.0f,
			                  ImDrawFlags_RoundCornersTop);
			dl->AddRect(cp, ImVec2(cp.x + cs.x, cp.y + cs.y), IM_COL32(255, 210, 110, 130), 6.0f);

			// Header: NORMALLY a plain drag handle showing the title (so the whole strip
			// moves the box); a DOUBLE-CLICK swaps in an InputText until it's committed.
			// An always-on text field used to eat the drag — that's why this is modal.
			const bool editingTitle = st.editingComment == cb.id;
			if (!editingTitle)
			{
				ImGui::SetCursorScreenPos(cp);
				ImGui::InvisibleButton("##cmove", ImVec2(std::max(cs.x, 1.0f), headH));
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					st.editingComment = cb.id; // enter title-edit mode
				else if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
				{
					const float dxg = ImGui::GetIO().MouseDelta.x / Z;
					const float dyg = ImGui::GetIO().MouseDelta.y / Z;
					for (auto& nn : st.graph.nodes)
					{
						const float cxn = nn.x + kNodeW * 0.5f, cyn = nn.y + 40.0f; // ≈ node center
						if (cxn >= cb.x && cxn <= cb.x + cb.w && cyn >= cb.y && cyn <= cb.y + cb.h)
						{ nn.x += dxg; nn.y += dyg; }
					}
					cb.x += dxg; cb.y += dyg;
				}
				if (ImGui::IsItemDeactivated()) commentEdit = true; // move finished → persist
				if (ImGui::BeginPopupContextItem("##cmtCtx"))
				{
					if (ImGui::MenuItem("Rename")) st.editingComment = cb.id;
					if (ImGui::MenuItem("Delete Comment")) deleteComment = cb.id;
					ImGui::EndPopup();
				}
				const char* title = cb.text.empty() ? "(double-click to name)" : cb.text.c_str();
				dl->AddText(font, fsz, ImVec2(cp.x + 6.0f * Z, cp.y + 4.0f * Z),
				            cb.text.empty() ? IM_COL32(230, 210, 160, 130)
				                            : IM_COL32(240, 225, 190, 255), title);
			}
			else
			{
				ImGui::SetCursorScreenPos(ImVec2(cp.x + 6.0f * Z, cp.y + 2.0f * Z));
				ImGui::SetNextItemWidth(std::max(cs.x - 12.0f * Z, 40.0f));
				ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0, 0, 0, 0.25f));
				ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0.30f));
				ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0, 0, 0, 0.35f));
				pushWidgetScale(Z);
				if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive())
					ImGui::SetKeyboardFocusHere(); // grab focus when edit mode starts
				ImGui::InputTextWithHint("##ctitle", "comment", &cb.text);
				if (ImGui::IsItemDeactivated()) // committed OR clicked away → leave edit mode
				{
					st.editingComment = 0;
					commentEdit = true;
				}
				popWidgetScale();
				ImGui::PopStyleColor(3);
			}
			// Resize grip (bottom-right corner).
			const float grip = 14.0f * Z;
			ImGui::SetCursorScreenPos(ImVec2(cp.x + cs.x - grip, cp.y + cs.y - grip));
			ImGui::InvisibleButton("##cresize", ImVec2(grip, grip));
			if (ImGui::IsItemHovered() || ImGui::IsItemActive())
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
			{
				cb.w = std::max(80.0f, cb.w + ImGui::GetIO().MouseDelta.x / Z);
				cb.h = std::max(60.0f, cb.h + ImGui::GetIO().MouseDelta.y / Z);
			}
			if (ImGui::IsItemDeactivated()) commentEdit = true; // resize finished → persist
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
	}

	std::vector<PinPos> pins;
	pins.reserve(st.graph.nodes.size() * 4);
	// Screen rects of each node this frame → box-select hit-testing at mouse-release.
	std::vector<NodeBox> nodeRects;
	nodeRects.reserve(st.graph.nodes.size());
	auto isSelected = [&](int id){ return std::find(st.selection.begin(), st.selection.end(), id) != st.selection.end(); };

	// ── Nodes ──
	for (auto& n : st.graph.nodes)
	{
		const MatNodeDesc& d = HE::matNodeDesc(n.type);
		// FunctionCall pins are dynamic — the referenced function graph's interface.
		std::vector<HE::MatPinDesc> dynIn, dynOut;
		const std::vector<HE::MatPinDesc>* nodeIns  = &d.inputs;
		const std::vector<HE::MatPinDesc>* nodeOuts = &d.outputs;
		std::string titleOverride;
		if (n.type == MatNodeType::FunctionCall)
		{
			if (const HE::MaterialGraph* fn = loadFunctionGraph(ctx, n.s))
			{
				HE::matFunctionPins(*fn, dynIn, dynOut);
				nodeIns = &dynIn; nodeOuts = &dynOut;
			}
			titleOverride = std::filesystem::path(n.s).stem().string();
			if (titleOverride.empty()) titleOverride = "Material Function";
		}
		const ImVec2 p(origin.x + st.scroll.x + n.x * Z, origin.y + st.scroll.y + n.y * Z);
		// Reroute nodes are deliberately tiny — just a dot with one in- and one out-pin.
		const bool  isReroute = n.type == MatNodeType::Reroute;
		const float nodeW  = (isReroute ? 42.0f : kNodeW)  * Z;
		const float titleH = (isReroute ?  6.0f : kTitleH) * Z;
		const float rowH   = kRowH   * Z;
		const float pad6   = 6.0f    * Z;
		const int rows = std::max<int>((int)nodeIns->size(), (int)nodeOuts->size());
		const float paramH = nodeParamHeight(n.type) * Z;
		const float h = titleH + pad6 + rows * rowH + paramH;
		nodeRects.push_back({ n.id, p, ImVec2(p.x + nodeW, p.y + h) });

		ImGui::PushID(n.id);

		// Body + colored header + border.
		const bool selected = isSelected(n.id);
		const bool named    = isNamedNode(n.type);
		dl->AddRectFilled(p, ImVec2(p.x + nodeW, p.y + h), IM_COL32(52, 52, 56, 255), 5.0f);
		dl->AddRectFilled(p, ImVec2(p.x + nodeW, p.y + titleH), categoryColor(d.category), 5.0f,
		                  ImDrawFlags_RoundCornersTop);
		dl->AddRect(p, ImVec2(p.x + nodeW, p.y + h),
		            selected ? IM_COL32(255, 200, 80, 255) : IM_COL32(0, 0, 0, 160), 5.0f, 0,
		            selected ? 2.0f : 1.0f);
		// Cyan halo + dot on the node whose output the preview ball is showing.
		if (st.previewNodeId == n.id)
		{
			dl->AddRect(p, ImVec2(p.x + nodeW, p.y + h), IM_COL32(80, 200, 255, 220), 5.0f, 0, 2.0f);
			dl->AddCircleFilled(ImVec2(p.x + nodeW - 2.0f * Z, p.y - 2.0f * Z), 5.0f * Z,
			                    IM_COL32(80, 200, 255, 255));
		}

		// Full-node drag handle behind the title field / pins / widgets (AllowOverlap so
		// those on-top items still get clicks; this only drags on empty node space) —
		// lets you drag named nodes whose header is now an editable text field.
		ImGui::SetCursorScreenPos(p);
		ImGui::SetNextItemAllowOverlap();
		ImGui::InvisibleButton("##nodedrag", ImVec2(nodeW, h));
		if (ImGui::IsItemActivated())
		{
			st.selectedNode = n.id;
			const bool shift = ImGui::GetIO().KeyShift;
			if (shift)
			{
				if (isSelected(n.id))
					st.selection.erase(std::remove(st.selection.begin(), st.selection.end(), n.id), st.selection.end());
				else
					st.selection.push_back(n.id);
			}
			else if (!isSelected(n.id))
				st.selection = { n.id }; // fresh single-select (keep group if already selected)
		}
		if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
		{
			const float dxg = ImGui::GetIO().MouseDelta.x / Z;
			const float dyg = ImGui::GetIO().MouseDelta.y / Z;
			if (isSelected(n.id) && st.selection.size() > 1)
			{
				for (int sid : st.selection)
					if (MatGraphNode* sn = st.graph.findNode(sid)) { sn->x += dxg; sn->y += dyg; }
			}
			else { n.x += dxg; n.y += dyg; }
		}
		// Double-click a Material Function node → open that function's own editor tab.
		if (n.type == MatNodeType::FunctionCall && !n.s.empty() && ctx.contentManager &&
		    ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			s_openAssetRequest =
				(std::filesystem::path(ctx.contentManager->contentRoot()) / n.s).string();
		}
		if (ImGui::BeginPopupContextItem("##nodeCtx"))
		{
			const bool deletable = n.type != MatNodeType::Output;
			// Open the referenced function from the context menu too (discoverable).
			if (n.type == MatNodeType::FunctionCall && !n.s.empty() && ctx.contentManager)
				if (ImGui::MenuItem("Open Function"))
					s_openAssetRequest =
						(std::filesystem::path(ctx.contentManager->contentRoot()) / n.s).string();
			// Per-node preview: route THIS node's first output (unlit) onto the preview
			// mesh — invaluable for debugging what an intermediate value looks like.
			if (!st.isFunction && n.type != MatNodeType::Output && !nodeOuts->empty())
			{
				const bool on = st.previewNodeId == n.id;
				if (ImGui::MenuItem("Preview This Node", nullptr, on))
				{ st.previewNodeId = on ? 0 : n.id; st.previewDirty = true; }
			}
			if (ImGui::MenuItem("Delete Node", nullptr, false, deletable)) deleteNode = n.id;
			ImGui::EndPopup();
		}
		// Texture Sample: accept a texture dropped from the Content Browser onto the node.
		if (n.type == MatNodeType::TextureSample && ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
			{
				const std::string abs(static_cast<const char*>(pl->Data));
				if (MaterialEditorPanel::isTextureAsset(abs) && ctx.contentManager)
				{
					std::error_code ec;
					const std::string rel = std::filesystem::relative(
						abs, ctx.contentManager->contentRoot(), ec).generic_string();
					n.s = ec ? abs : rel;
					structuralEdit = true; // texture list changed → regenerate
				}
			}
			ImGui::EndDragDropTarget();
		}

		// Header title: an editable name for Param/Const/Fn nodes, otherwise static text.
		// Reroute dots are too small for any title.
		if (isReroute) {}
		else if (named)
		{
			ImGui::SetCursorScreenPos(ImVec2(p.x + 5.0f * Z, p.y + 3.0f * Z));
			ImGui::SetNextItemWidth(nodeW - 10.0f * Z);
			ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0, 0, 0, 0.0f));  // blend into the header
			ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1, 1, 1, 0.12f));
			ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0, 0, 0, 0.30f));
			ImGui::PushStyleColor(ImGuiCol_Text,           ImVec4(1, 1, 1, 1));
			pushWidgetScale(Z);
			const char* hint = isConstNode(n.type) ? d.name : "name"; // constants default to their type name
			ImGui::InputTextWithHint("##hdrname", hint, &n.s);
			if (ImGui::IsItemDeactivatedAfterEdit()) paramEdit = true; // rename → regenerate
			popWidgetScale();
			ImGui::PopStyleColor(4);
		}
		else
			dl->AddText(font, fsz, ImVec2(p.x + 8.0f * Z, p.y + 4.0f * Z), IM_COL32_WHITE,
			            titleOverride.empty() ? d.name : titleOverride.c_str());

		const float pinR = kPinR * Z;
		const float hit  = 16.0f * Z;
		// Input pins (left column)
		for (int i = 0; i < (int)nodeIns->size(); ++i)
		{
			const ImVec2 pp(p.x, p.y + titleH + pad6 + i * rowH + rowH * 0.5f);
			pins.push_back({ n.id, i, false, pp, (*nodeIns)[i].type });
			dl->AddCircleFilled(pp, pinR, pinColor((*nodeIns)[i].type));
			dl->AddText(font, fsz, ImVec2(pp.x + 10.0f * Z, pp.y - fsz * 0.5f), IM_COL32(210, 210, 210, 255), (*nodeIns)[i].name);
			ImGui::SetCursorScreenPos(ImVec2(pp.x - hit * 0.5f, pp.y - hit * 0.5f));
			ImGui::InvisibleButton((std::string("##in") + std::to_string(i)).c_str(), ImVec2(hit, hit));
			if (ImGui::IsItemActivated())
			{ st.dragNode = n.id; st.dragPin = i; st.dragFromOutput = false; }
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
			{ st.graph.disconnectInput(n.id, i); structuralEdit = true; }
		}
		// Output pins (right column)
		for (int i = 0; i < (int)nodeOuts->size(); ++i)
		{
			const ImVec2 pp(p.x + nodeW, p.y + titleH + pad6 + i * rowH + rowH * 0.5f);
			pins.push_back({ n.id, i, true, pp, (*nodeOuts)[i].type });
			dl->AddCircleFilled(pp, pinR, pinColor((*nodeOuts)[i].type));
			const ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, (*nodeOuts)[i].name);
			dl->AddText(font, fsz, ImVec2(pp.x - 10.0f * Z - ts.x, pp.y - fsz * 0.5f), IM_COL32(210, 210, 210, 255), (*nodeOuts)[i].name);
			ImGui::SetCursorScreenPos(ImVec2(pp.x - hit * 0.5f, pp.y - hit * 0.5f));
			ImGui::InvisibleButton((std::string("##out") + std::to_string(i)).c_str(), ImVec2(hit, hit));
			if (ImGui::IsItemActivated())
			{ st.dragNode = n.id; st.dragPin = i; st.dragFromOutput = true; }
		}

		// Inline parameter widgets under the pins (also for Texture Sample, which has
		// no numeric params but shows its picked-texture label + clear). Font-scaled to
		// the zoom so they track the node box.
		if (d.paramCount > 0 || n.type == MatNodeType::TextureSample)
		{
			ImGui::SetCursorScreenPos(ImVec2(p.x + 10.0f * Z, p.y + titleH + pad6 + rows * rowH));
			pushWidgetScale(Z);
			// Name lives in the header for named nodes → only the value widget here.
			if (nodeParamWidgets(n, Z, /*drawName=*/false)) paramEdit = true;
			popWidgetScale();
		}

		ImGui::PopID();
	}

	// ── Links (bezier, colored by source pin type) ──
	auto pinAt = [&](int node, int pin, bool output) -> const PinPos* {
		for (const auto& pp : pins)
			if (pp.node == node && pp.pin == pin && pp.output == output) return &pp;
		return nullptr;
	};
	// Point→segment distance + a cubic-bezier sampler, used to hit-test the mouse
	// against a link so it can be hovered and clicked away.
	auto segDist = [](ImVec2 pt, ImVec2 a, ImVec2 b) -> float {
		const ImVec2 ab(b.x - a.x, b.y - a.y), ap(pt.x - a.x, pt.y - a.y);
		const float len2 = ab.x * ab.x + ab.y * ab.y;
		float u = len2 > 0.0f ? (ap.x * ab.x + ap.y * ab.y) / len2 : 0.0f;
		u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
		const float dx = pt.x - (a.x + ab.x * u), dy = pt.y - (a.y + ab.y * u);
		return std::sqrt(dx * dx + dy * dy);
	};
	auto bez = [](ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, float t) -> ImVec2 {
		const float w = 1.0f - t, w0 = w * w * w, w1 = 3 * w * w * t, w2 = 3 * w * t * t, w3 = t * t * t;
		return ImVec2(w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x,
		              w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y);
	};

	// A link is "pickable" only over the empty canvas (not while dragging/box-selecting).
	const bool  canPickLink = canvasHovered && st.dragNode == 0 && !st.boxSel;
	const ImVec2 lmouse = ImGui::GetIO().MousePos;
	int   hoverLink  = -1;
	float hoverLinkD = 1e9f;
	bool  linkHot    = false; // gates box-select / add-node so a link click isn't stolen

	for (size_t li = 0; li < st.graph.links.size(); ++li)
	{
		const HE::MatGraphLink& l = st.graph.links[li];
		const PinPos* a = pinAt(l.srcNode, l.srcPin, true);
		const PinPos* b = pinAt(l.dstNode, l.dstPin, false);
		if (!a || !b) continue;
		const float t = std::max(40.0f, fabsf(b->pos.x - a->pos.x) * 0.5f);
		const ImVec2 c1(a->pos.x + t, a->pos.y), c2(b->pos.x - t, b->pos.y);
		dl->AddBezierCubic(a->pos, c1, c2, b->pos, pinColor(a->type), 2.0f);
		if (canPickLink)
		{
			ImVec2 prev = a->pos;
			for (int s = 1; s <= 20; ++s)
			{
				const ImVec2 cur = bez(a->pos, c1, c2, b->pos, s / 20.0f);
				const float dd = segDist(lmouse, prev, cur);
				if (dd < hoverLinkD) { hoverLinkD = dd; hoverLink = (int)li; }
				prev = cur;
			}
		}
	}
	// Hovered link → highlight + interact: Alt+Click severs it, double-click splices a
	// Reroute dot into it (both ends re-wired through the new node).
	if (canPickLink && hoverLink >= 0 && hoverLinkD <= 7.0f)
	{
		const HE::MatGraphLink l = st.graph.links[hoverLink]; // COPY — edits below mutate links
		if (const PinPos* a = pinAt(l.srcNode, l.srcPin, true))
		if (const PinPos* b = pinAt(l.dstNode, l.dstPin, false))
		{
			linkHot = true;
			const float t = std::max(40.0f, fabsf(b->pos.x - a->pos.x) * 0.5f);
			dl->AddBezierCubic(a->pos, ImVec2(a->pos.x + t, a->pos.y),
			                   ImVec2(b->pos.x - t, b->pos.y), b->pos, IM_COL32(255, 90, 90, 255), 3.5f);
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			ImGui::SetTooltip("Alt+Click: remove link\nDouble-click: insert reroute");
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				// Splice a reroute dot at the mouse: src → reroute → dst. connect() on the
				// dst pin auto-drops the original link.
				const float gx = (lmouse.x - origin.x - st.scroll.x) / Z - 21.0f;
				const float gy = (lmouse.y - origin.y - st.scroll.y) / Z - 14.0f;
				const int rid = st.graph.addNode(MatNodeType::Reroute, gx, gy);
				st.graph.connect(l.srcNode, l.srcPin, rid, 0);
				st.graph.connect(rid, 0, l.dstNode, l.dstPin);
				structuralEdit = true;
			}
			else if (ImGui::GetIO().KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				st.graph.disconnectInput(l.dstNode, l.dstPin);
				structuralEdit = true;
				linkHot = false; // link is gone this frame
			}
		}
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
			bool onPin = false;
			for (const auto& pp : pins)
			{
				const float dx = pp.pos.x - m.x, dy = pp.pos.y - m.y;
				if (dx * dx + dy * dy > 12.0f * 12.0f) continue;
				if (pp.output == st.dragFromOutput || pp.node == st.dragNode) continue;
				const bool ok = st.dragFromOutput
					? st.graph.connect(st.dragNode, st.dragPin, pp.node, pp.pin)
					: st.graph.connect(pp.node, pp.pin, st.dragNode, st.dragPin);
				if (ok) structuralEdit = true;
				onPin = true;
				break;
			}
			// Dropped over EMPTY canvas → open the add-node palette filtered to nodes
			// with a compatible opposite-side pin, and auto-wire the one that's picked.
			if (!onPin &&
			    m.x >= origin.x && m.x <= origin.x + avail.x &&
			    m.y >= origin.y && m.y <= origin.y + avail.y)
			{
				s_pendingLinkNode = st.dragNode;
				s_pendingLinkPin  = st.dragPin;
				s_pendingLinkFromOutput = st.dragFromOutput;
				ImGui::OpenPopup("##addNode");
			}
			st.dragNode = 0;
		}
	}

	// ── Empty-canvas interaction (driven by the ##canvasbg button, robust vs nodes) ─
	// Middle-drag pans (mouse users); trackpad users pan with two-finger scroll above.
	if ((canvasActive || ImGui::IsWindowHovered()) && st.dragNode == 0 && !st.boxSel &&
	    ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
	{
		st.scroll.x += ImGui::GetIO().MouseDelta.x;
		st.scroll.y += ImGui::GetIO().MouseDelta.y;
	}
	// Left-press on empty canvas begins a rubber-band box-select; a plain click (no
	// drag) just clears the selection. Shift keeps the existing selection (additive).
	if (canvasActive && st.dragNode == 0 && !st.boxSel && !linkHot &&
	    ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		st.boxSel   = true;
		st.boxStart = ImGui::GetIO().MousePos;
		if (!ImGui::GetIO().KeyShift) st.selection.clear();
	}
	if (st.boxSel)
	{
		const ImVec2 m = ImGui::GetIO().MousePos;
		const ImVec2 a(std::min(st.boxStart.x, m.x), std::min(st.boxStart.y, m.y));
		const ImVec2 b(std::max(st.boxStart.x, m.x), std::max(st.boxStart.y, m.y));
		dl->AddRectFilled(a, b, IM_COL32(255, 200, 80, 30));
		dl->AddRect(a, b, IM_COL32(255, 200, 80, 180));
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			for (const NodeBox& nb : nodeRects)
			{
				const bool hit = nb.mn.x <= b.x && nb.mx.x >= a.x && nb.mn.y <= b.y && nb.mx.y >= a.y;
				if (hit && std::find(st.selection.begin(), st.selection.end(), nb.id) == st.selection.end())
					st.selection.push_back(nb.id);
			}
			if (st.selection.size() == 1) st.selectedNode = st.selection[0];
			st.boxSel = false;
		}
	}
	// Delete key removes the whole selection (never the material Output node).
	if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive() && !st.selection.empty() &&
	    ImGui::IsKeyPressed(ImGuiKey_Delete))
	{
		for (int sid : st.selection)
			if (const MatGraphNode* sn = st.graph.findNode(sid); sn && sn->type != MatNodeType::Output)
				st.graph.removeNode(sid);
		st.selection.clear();
		st.selectedNode = 0;
		structuralEdit = true;
	}
	// ── Keyboard: undo/redo + node clipboard (Cmd on macOS, Ctrl elsewhere) ──────────
	{
		const ImGuiIO& kio = ImGui::GetIO();
		const bool mod  = kio.KeyCtrl || kio.KeySuper;
		const bool kbOk = ImGui::IsWindowHovered() && !kio.WantTextInput && !ImGui::IsAnyItemActive();
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
		if (kbOk && mod && ImGui::IsKeyPressed(ImGuiKey_C) && !st.selection.empty())
		{
			const std::string payload = serializeSelection(st);
			if (!payload.empty()) g_matClipboard = payload;
		}
		if (kbOk && mod && ImGui::IsKeyPressed(ImGuiKey_X) && !st.selection.empty())
		{
			// Cut = copy + delete (interface nodes are excluded from BOTH, so a cut can
			// never remove the Output while still copying it).
			const std::string payload = serializeSelection(st);
			if (!payload.empty())
			{
				g_matClipboard = payload;
				for (int sid : st.selection)
					if (const MatGraphNode* sn = st.graph.findNode(sid);
					    sn && sn->type != MatNodeType::Output &&
					    sn->type != MatNodeType::FnOutput && sn->type != MatNodeType::FnInput)
						st.graph.removeNode(sid);
				st.selection.clear();
				st.selectedNode = 0;
				structuralEdit = true;
			}
		}
		if (kbOk && mod && ImGui::IsKeyPressed(ImGuiKey_V) && !g_matClipboard.empty())
		{
			// Paste at the mouse when it's over the canvas, else into the visible center.
			const ImVec2 mp = kio.MousePos;
			const bool overCanvas = mp.x >= origin.x && mp.x <= origin.x + avail.x &&
			                        mp.y >= origin.y && mp.y <= origin.y + avail.y;
			const float gx = ((overCanvas ? mp.x : origin.x + avail.x * 0.5f) - origin.x - st.scroll.x) / Z;
			const float gy = ((overCanvas ? mp.y : origin.y + avail.y * 0.5f) - origin.y - st.scroll.y) / Z;
			if (pasteInto(st, g_matClipboard, gx, gy)) structuralEdit = true;
		}
		if (kbOk && mod && ImGui::IsKeyPressed(ImGuiKey_D) && !st.selection.empty())
		{
			// Duplicate in place (slight offset), without touching the shared clipboard.
			float mnx = FLT_MAX, mny = FLT_MAX;
			for (int sid : st.selection)
				if (const MatGraphNode* sn = st.graph.findNode(sid))
				{ mnx = std::min(mnx, sn->x); mny = std::min(mny, sn->y); }
			const std::string payload = serializeSelection(st);
			if (mnx != FLT_MAX && pasteInto(st, payload, mnx + 28.0f, mny + 28.0f))
				structuralEdit = true;
		}
	}
	// Right-click over empty canvas (the bg button is hovered, not a node/link) → add-node.
	if (canvasHovered && !linkHot && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		ImGui::OpenPopup("##addNode");
	// Fixed-size popup with an internal scroll region: the size never depends on the
	// filtered result count, so the window can't grow/reposition (which shifted the
	// item hitboxes) as you type. Results live in a scrolling child.
	ImGui::SetNextWindowSize(ImVec2(250.0f, 340.0f), ImGuiCond_Always);
	if (ImGui::BeginPopup("##addNode"))
	{
		const ImVec2 m = ImGui::GetMousePosOnOpeningCurrentPopup();
		const float gx = (m.x - origin.x - st.scroll.x) / st.zoom;
		const float gy = (m.y - origin.y - st.scroll.y) / st.zoom;
		// Searchable palette: filters the node library AND the project's material
		// functions by substring (name or category, case-insensitive).
		static std::string s_search;
		if (ImGui::IsWindowAppearing()) { s_search.clear(); ImGui::SetKeyboardFocusHere(); }
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##nodeSearch", "Search nodes...", &s_search);
		ImGui::Separator();
		auto lower = [](std::string v){ std::transform(v.begin(), v.end(), v.begin(),
			[](unsigned char ch){ return (char)std::tolower(ch); }); return v; };
		const std::string q = lower(s_search);
		auto matches = [&](const char* name, const char* cat)
		{ return q.empty() || lower(name).find(q) != std::string::npos
		      || lower(cat).find(q) != std::string::npos; };

		// Auto-wire helper: when this popup was opened by dragging a link into empty
		// canvas, connect the freshly created node to the dragged pin — preferring an
		// exact type match on the opposite side (coercion makes any pairing legal).
		auto autoWire = [&](int newNode)
		{
			if (s_pendingLinkNode == 0) return;
			const MatGraphNode* nn = st.graph.findNode(newNode);
			if (!nn) { s_pendingLinkNode = 0; return; }
			std::vector<HE::MatPinDesc> dIn, dOut;
			const std::vector<HE::MatPinDesc>* ins  = &HE::matNodeDesc(nn->type).inputs;
			const std::vector<HE::MatPinDesc>* outs = &HE::matNodeDesc(nn->type).outputs;
			if (nn->type == MatNodeType::FunctionCall)
				if (const HE::MaterialGraph* fn = loadFunctionGraph(ctx, nn->s))
				{ HE::matFunctionPins(*fn, dIn, dOut); ins = &dIn; outs = &dOut; }
			MatPinType want = MatPinType::Vec3;
			if (const MatGraphNode* src = st.graph.findNode(s_pendingLinkNode))
			{
				std::vector<HE::MatPinDesc> sIn, sOut;
				const std::vector<HE::MatPinDesc>* sp = s_pendingLinkFromOutput
					? &HE::matNodeDesc(src->type).outputs : &HE::matNodeDesc(src->type).inputs;
				if (src->type == MatNodeType::FunctionCall)
					if (const HE::MaterialGraph* fn = loadFunctionGraph(ctx, src->s))
					{ HE::matFunctionPins(*fn, sIn, sOut); sp = s_pendingLinkFromOutput ? &sOut : &sIn; }
				if (s_pendingLinkPin >= 0 && s_pendingLinkPin < (int)sp->size())
					want = (*sp)[s_pendingLinkPin].type;
			}
			const auto& cand = s_pendingLinkFromOutput ? *ins : *outs;
			int pick = cand.empty() ? -1 : 0;
			for (int i = 0; i < (int)cand.size(); ++i)
				if (cand[i].type == want) { pick = i; break; }
			if (pick >= 0)
			{
				if (s_pendingLinkFromOutput)
					st.graph.connect(s_pendingLinkNode, s_pendingLinkPin, newNode, pick);
				else
					st.graph.connect(newNode, pick, s_pendingLinkNode, s_pendingLinkPin);
			}
			s_pendingLinkNode = 0;
		};

		ImGui::BeginChild("##nodeList", ImVec2(0, 0), ImGuiChildFlags_None);
		// Comment boxes are pure editor chrome — offered only in the plain palette,
		// not when the popup is completing a dragged link (a comment has no pins).
		if (s_pendingLinkNode == 0 && matches("Comment Box", "Editor"))
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
			// Exactly one material Output; FunctionCall is inserted via the functions
			// list below. FnInput/FnOutput exist ONLY in function graphs; every other
			// node is available in both materials and functions.
			if (d.type == MatNodeType::Output || d.type == MatNodeType::FunctionCall) continue;
			const bool fnInterface = d.type == MatNodeType::FnInput || d.type == MatNodeType::FnOutput;
			if (fnInterface && !st.isFunction) continue;
			// Completing a dragged link → only nodes with a compatible opposite side.
			if (s_pendingLinkNode != 0 &&
			    (s_pendingLinkFromOutput ? d.inputs.empty() : d.outputs.empty())) continue;
			if (!matches(d.name, d.category)) continue;
			if (std::string(lastCat) != d.category)
			{
				if (*lastCat) ImGui::Spacing();
				ImGui::TextDisabled("%s", d.category);
				lastCat = d.category;
			}
			if (ImGui::Selectable(d.name))
			{
				st.selectedNode = st.graph.addNode(d.type, gx, gy);
				st.selection = { st.selectedNode };
				autoWire(st.selectedNode);
				structuralEdit = true;
				ImGui::CloseCurrentPopup();
			}
		}

		// Project-wide material functions (insert as FunctionCall).
		if (ctx.contentManager)
		{
			bool headerShown = false;
			for (const HE::UUID& fnId :
			     ctx.contentManager->enumerateIds(HE::AssetType::MaterialFunction))
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
					st.selectedNode = id;
					st.selection = { id };
					autoWire(id);
					structuralEdit = true;
					ImGui::CloseCurrentPopup();
				}
			}
		}
		ImGui::EndChild();
		ImGui::EndPopup();
	}
	else
		s_pendingLinkNode = 0; // popup closed without a pick → drop the pending link

	ImGui::EndChild(); // ##graphCanvas
	}                  // end graph-view branch

	ImGui::EndChild(); // ##matRight

	// Structural / committed edits (either column) → regenerate + push into the live material.
	if (deleteNode != 0) { st.graph.removeNode(deleteNode); structuralEdit = true; }
	if ((structuralEdit || paramEdit || panelEdit || commentEdit) && assetOk)
	{
		applyToMaterial(st, ctx);
		pushUndo(st);           // every committed edit becomes an undo step
		st.previewDirty = true; // material changed → refresh the preview
	}

	ImGui::End();
}
} // namespace MaterialEditorPanel
