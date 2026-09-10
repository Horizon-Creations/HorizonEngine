#include "McpToolRegistry.h"

#include "EditorAssetTypeCache.h"     // what a path holds, without loading it
#include "McpToolCommon.h"            // the argument readers, the confinement rule, the walk

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <MaterialGraph/MaterialGraph.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// ─── Tuning a surface from outside the editor ────────────────────────────────
// Why a material needs tools of its own, where a parameter's value really lives
// and why an open tab is refused instead of edited: McpToolRegistry.h, beside
// McpMaterialHooks. What is worth stating HERE is what the handlers promise.
//
//   • A PARAMETER NAME IS CHECKED AGAINST THE MATERIAL'S OWN SLOT LIST.
//     `ContentManager::setMaterialParam` answers false for a name it does not
//     know, which on this interface would be an unreadable "failed". So the name
//     is looked up in `graphParamNames` first and a miss is a refusal carrying
//     the list — and an empty list is its own refusal, because a stub material
//     has no parameters at all and "unknown name" would be the wrong story.
//
//   • THE VALUE'S SHAPE COMES FROM THE PARAMETER'S KIND, not from what the JSON
//     happens to look like: Float → a number, Bool → a boolean, Vec2 → two
//     numbers, Color → three, Vec4 → four. A refusal spells out the shape that
//     was expected, since the client cannot see `graphParamTypes`.
//
//   • ONLY THE COMPONENTS THE KIND CARRIES ARE WRITTEN INTO THE NODE. A
//     ParamFloat node keeps its slider range in `p[1]`/`p[2]`
//     (MaterialGraph.h) — writing four floats over it would eat the range that
//     made it a slider, which nothing would report and nobody would connect to
//     this call.
//
//   • EVERY NODE THAT DECLARES THE NAME IS UPDATED, not the first one found.
//     `paramSlot` collapses repeated Param nodes of one name into ONE slot, so a
//     graph may well hold three nodes feeding it. Leaving two of them on the old
//     value means the next structural edit regenerates whichever the codegen
//     reaches first. Nodes of a DIFFERENT kind under the same name are left
//     alone — the first one seen owns the slot's kind, and writing a colour into
//     a float node's `p[]` is the range-eating mistake above.
//
//   • THE SLOT IS WRITTEN BEFORE THE REGENERATE, AND READ BACK AFTER IT. The
//     value in the result is the one the file now holds, not the one that was
//     asked for.
//
//   • A REFUSAL IS A NO-OP. Nothing is written, nothing is regenerated, no
//     instance is synced and no tab is told to re-read anything.

namespace HE::Ed
{

using nlohmann::json;

namespace
{

// ── Names on the wire ────────────────────────────────────────────────────────

const char* paramKindName(HE::MatParamKind k)
{
	switch (k)
	{
	case HE::MatParamKind::Float: return "float";
	case HE::MatParamKind::Color: return "color";
	case HE::MatParamKind::Vec2:  return "vec2";
	case HE::MatParamKind::Vec4:  return "vec4";
	case HE::MatParamKind::Bool:  return "bool";
	}
	return "float";
}

const char* blendModeName(std::uint8_t m)
{
	switch (static_cast<HE::MatBlendMode>(m))
	{
	case HE::MatBlendMode::Opaque:      return "Opaque";
	case HE::MatBlendMode::Masked:      return "Masked";
	case HE::MatBlendMode::Translucent: return "Translucent";
	}
	return "Opaque";
}

// The [hi, lo] pair, the same shape asset_resolve answers with and
// entity_set_components reads back — a second spelling of a uuid would be a
// second thing to convert.
json uuidJson(const HE::UUID& id)
{
	return json::array({ id.hi, id.lo });
}

// Which Param node type declares which kind of slot. Unknown = not a Param node.
bool paramKindOfNode(HE::MatNodeType t, HE::MatParamKind& out)
{
	switch (t)
	{
	case HE::MatNodeType::ParamFloat: out = HE::MatParamKind::Float; return true;
	case HE::MatNodeType::ParamColor: out = HE::MatParamKind::Color; return true;
	case HE::MatNodeType::ParamVec2:  out = HE::MatParamKind::Vec2;  return true;
	case HE::MatNodeType::ParamVec4:  out = HE::MatParamKind::Vec4;  return true;
	case HE::MatNodeType::ParamBool:  out = HE::MatParamKind::Bool;  return true;
	default: return false;
	}
}

// What a Param node's slot is CALLED. An unnamed node does not drop out of the
// layout — `paramSlot` gives it "param_<id>" — so matching on `s` alone would
// miss exactly the nodes a client is most likely to have just created.
std::string effectiveParamName(const HE::MatGraphNode& n)
{
	return n.s.empty() ? ("param_" + std::to_string(n.id)) : n.s;
}

// ── The addressed material ───────────────────────────────────────────────────
// One struct for the three tools, because the path check, the play gate, the
// lock gate and the unsaved-tab gate are the same four questions every time.
//
// Everything is COPIED out of the asset. `ContentManager`'s material pool is a
// dense vector and anything that loads moves it — and the things this file calls
// (regenerateMaterialFromGraph through its function loader,
// syncMaterialInstancesOf through the parents it loads) load. A MaterialAsset*
// held across any of them points at moved memory, taking the strings it owns
// with it (ContentManager.h).
struct Mat
{
	std::string rel;
	std::string abs;
	HE::UUID    id{};
	bool        ok = false;
	bool        openDirty = false;
	ToolResult  failure = ToolResult::ok(json::object());

	// The copy, as of openMat().
	std::string              nodeGraphJson;
	std::string              parentMaterialPath;
	std::vector<std::string> paramNames;
	std::vector<std::uint8_t> paramTypes;
	std::vector<float>       paramMinMax;
	std::vector<float>       paramData;
	std::vector<std::string> paramGroups;
	std::vector<std::string> paramTooltips;
	std::vector<std::string> overridden;
	std::vector<std::string> switchNames;
	std::vector<std::uint8_t> switchValues;
	std::vector<std::string> textures;
	std::vector<std::string> layers;
	std::uint8_t             blendMode = 0;
	std::uint8_t             domain    = 0;
	float                    baseColor[3] = { 1, 1, 1 };
	float                    metallic = 0.0f, roughness = 0.5f, opacity = 1.0f;
	bool                     doubleSided = false;

	bool isInstance() const { return !parentMaterialPath.empty(); }

	// Slot index of a parameter, -1 = not a parameter of this material.
	int slotOf(const std::string& name) const
	{
		for (std::size_t i = 0; i < paramNames.size(); ++i)
			if (paramNames[i] == name) return static_cast<int>(i);
		return -1;
	}

	HE::MatParamKind kindAt(int slot) const
	{
		return slot >= 0 && slot < static_cast<int>(paramTypes.size())
			? static_cast<HE::MatParamKind>(paramTypes[static_cast<std::size_t>(slot)])
			: HE::MatParamKind::Float;
	}

	bool isOverridden(const std::string& name) const
	{
		return std::find(overridden.begin(), overridden.end(), name) != overridden.end();
	}
};

void copyOut(Mat& m, const MaterialAsset& a)
{
	m.nodeGraphJson      = a.nodeGraphJson;
	m.parentMaterialPath = a.parentMaterialPath;
	m.paramNames         = a.graphParamNames;
	m.paramTypes         = a.graphParamTypes;
	m.paramMinMax        = a.graphParamMinMax;
	m.paramData          = a.shaderParamData;
	m.paramGroups        = a.graphParamGroups;
	m.paramTooltips      = a.graphParamTooltips;
	m.overridden         = a.instanceOverriddenParams;
	m.switchNames        = a.instanceSwitchNames;
	m.switchValues       = a.instanceSwitchValues;
	m.textures           = a.graphTexturePaths;
	m.layers             = a.graphLayerNames;
	m.blendMode          = a.blendMode;
	m.domain             = a.domain;
	for (int k = 0; k < 3; ++k) m.baseColor[k] = a.baseColor[k];
	m.metallic    = a.metallic;
	m.roughness   = a.roughness;
	m.opacity     = a.opacity;
	m.doubleSided = a.doubleSided;
}

Mat openMat(ContentManager& content, const McpMaterialHooks& h, const json& args,
            bool forWrite)
{
	Mat m;
	const PathCheck p = checkPath(content, strArg(args, "path"), /*mustExist=*/true, "path");
	if (!p.ok) { m.failure = p.failure; return m; }
	m.rel = p.rel;
	m.abs = p.abs;

	// The sniff FIRST, so a material function gets the answer that tells a client
	// what it actually holds. Going through loadAsset would land it in the
	// function pool and come back as a plain "not a material".
	const HE::AssetType sniffed = EditorAssetTypeCache::assetTypeOf(p.abs);
	if (sniffed == HE::AssetType::MaterialFunction)
	{
		m.failure = ToolResult::fail("invalid_path",
			"'" + p.rel + "' is a Material FUNCTION — a reusable sub-graph, not a "
			"material. It declares no parameter slots of its own; the material whose "
			"graph calls it is what carries them, and that is what to address here.");
		return m;
	}
	if (sniffed != HE::AssetType::Material)
	{
		m.failure = ToolResult::fail("invalid_path",
			"'" + p.rel + "' is not a Material asset. asset_resolve reports what a path "
			"holds, material_info without a path lists every material in the project, "
			"and asset_create with type 'Material' makes a new one.");
		return m;
	}

	m.openDirty = h.isDirty && h.isDirty(p.rel);

	if (forWrite)
	{
		if (p.engine) { m.failure = failEngineReadOnly(p.rel); return m; }
		if (h.isPlaying && h.isPlaying())
		{
			m.failure = ToolResult::fail("play_mode",
				"Play-in-editor is running, and it draws from this very asset — a "
				"parameter changed now would be in effect in a session whose state is "
				"thrown away when it stops, and on disk afterwards. It is refused rather "
				"than half-applied. Ask the user to stop play mode.");
			return m;
		}
		if (h.lockedByOther && h.lockedByOther(p.rel))
		{
			m.failure = ToolResult::fail("locked_by_other",
				"Another participant in the collaboration session holds '" + p.rel +
				"' right now. Wait until they let go, or work on something else.");
			return m;
		}
		if (m.openDirty)
		{
			m.failure = ToolResult::fail("dirty",
				"'" + p.rel + "' is open in the Material Editor with unsaved changes. "
				"That tab holds its own copy of the graph and rebuilds the parameter "
				"block from it on every edit, so a write to the file would be reverted "
				"the moment the human touches a node. Ask the user to save or close that "
				"tab, then call again.");
			return m;
		}
	}

	// The load is what every reader below needs and it is deliberately the LAST
	// thing before the copy: nothing is held across it.
	m.id = content.loadAsset(p.rel);
	const MaterialAsset* a = m.id == HE::UUID{} ? nullptr : content.getMaterial(m.id);
	if (!a)
	{
		m.failure = ToolResult::fail("failed",
			"'" + p.rel + "' reads as a Material asset but could not be loaded. The "
			"editor log carries the reason; a file truncated mid-write is the usual one.");
		return m;
	}
	copyOut(m, *a);
	m.ok = true;
	return m;
}

// ── Reporting ────────────────────────────────────────────────────────────────

json paramJson(const Mat& m, std::size_t i, const HE::MaterialGraph* graph)
{
	const HE::MatParamKind kind = m.kindAt(static_cast<int>(i));
	const int comps = HE::matParamKindComponents(kind);

	json value = json::array();
	for (int k = 0; k < comps; ++k)
	{
		const std::size_t at = i * 4 + static_cast<std::size_t>(k);
		value.push_back(at < m.paramData.size() ? m.paramData[at] : 0.0f);
	}

	json j{
		{ "name", m.paramNames[i] },
		{ "kind", paramKindName(kind) },
		{ "slot", static_cast<int>(i) },
	};
	// A bool and a float answer as the scalar they are rather than as a
	// one-element array — the shape this parameter is SET with, so a client can
	// read a value here and hand it straight back.
	if (comps == 1)
		j["value"] = kind == HE::MatParamKind::Bool
			? json(value[0].get<double>() >= 0.5) : value[0];
	else
		j["value"] = std::move(value);

	if (i * 2 + 1 < m.paramMinMax.size())
	{
		const float lo = m.paramMinMax[i * 2], hi = m.paramMinMax[i * 2 + 1];
		// min < max is what makes it a slider in the editor, and the only case in
		// which the pair means anything at all.
		if (lo < hi) { j["min"] = lo; j["max"] = hi; }
	}
	if (i < m.paramGroups.size()   && !m.paramGroups[i].empty())   j["group"]   = m.paramGroups[i];
	if (i < m.paramTooltips.size() && !m.paramTooltips[i].empty()) j["tooltip"] = m.paramTooltips[i];

	if (m.isInstance()) j["overridden"] = m.isOverridden(m.paramNames[i]);

	// Does a Param node of THIS graph declare the name? A "no" on a master is the
	// material-function case: the slot is settable and the value does not survive
	// the next edit in the Material Editor.
	if (graph)
	{
		bool inGraph = false;
		for (const HE::MatGraphNode& n : graph->nodes)
		{
			HE::MatParamKind nk{};
			if (!paramKindOfNode(n.type, nk)) continue;
			if (effectiveParamName(n) == m.paramNames[i]) { inGraph = true; break; }
		}
		j["inGraph"] = inGraph;
	}
	return j;
}

json materialJson(const Mat& m, const McpMaterialHooks& h)
{
	HE::MaterialGraph graph;
	const bool haveGraph = !m.nodeGraphJson.empty() &&
	                       HE::materialGraphFromJson(m.nodeGraphJson, graph);

	json params = json::array();
	for (std::size_t i = 0; i < m.paramNames.size(); ++i)
		params.push_back(paramJson(m, i, haveGraph ? &graph : nullptr));

	json j{
		{ "path",      m.rel },
		{ "uuid",      uuidJson(m.id) },
		{ "kind",      m.isInstance() ? "instance" : "master" },
		{ "hasGraph",  haveGraph },
		{ "blendMode", blendModeName(m.blendMode) },
		{ "domain",    HE::matDomainName(static_cast<HE::MatDomain>(m.domain)) },
		{ "params",    std::move(params) },
		{ "baseColor", json::array({ m.baseColor[0], m.baseColor[1], m.baseColor[2] }) },
		{ "metallic",    m.metallic },
		{ "roughness",   m.roughness },
		{ "opacity",     m.opacity },
		{ "doubleSided", m.doubleSided },
	};
	if (m.isInstance()) j["parent"] = m.parentMaterialPath;
	if (!m.textures.empty()) j["textures"] = m.textures;
	// A landscape material's paint layers, in weightmap-channel order — what the
	// terrain tools' `layer` argument names.
	if (!m.layers.empty()) j["layers"] = m.layers;
	if (!m.switchNames.empty())
	{
		json sw = json::array();
		for (std::size_t i = 0; i < m.switchNames.size(); ++i)
			sw.push_back(json{ { "name", m.switchNames[i] },
			                   { "value", i < m.switchValues.size() &&
			                              m.switchValues[i] != 0 } });
		j["switchOverrides"] = std::move(sw);
	}
	// Reported whether true or false: "open in the editor with edits" is the one
	// state in which every writing tool here refuses.
	j["dirtyInEditor"] = m.openDirty;
	(void)h;
	return j;
}

// ── Reading a value by the kind the parameter declares ───────────────────────
// `why` is filled with the sentence the refusal carries: the shape that was
// expected, not just "wrong type".
bool valueFromJson(const json& v, HE::MatParamKind kind, const std::string& name,
                   std::array<float, 4>& out, std::string& why)
{
	out = { 0, 0, 0, 0 };
	const int comps = HE::matParamKindComponents(kind);

	if (kind == HE::MatParamKind::Bool)
	{
		if (!v.is_boolean())
		{
			why = "Parameter '" + name + "' is a bool: 'value' has to be true or false. "
			      "The shader reads it as 0.0 or 1.0, but that is the encoding, not the "
			      "interface.";
			return false;
		}
		out[0] = v.get<bool>() ? 1.0f : 0.0f;
		return true;
	}
	if (kind == HE::MatParamKind::Float)
	{
		if (!v.is_number())
		{
			why = "Parameter '" + name + "' is a float: 'value' has to be a number.";
			return false;
		}
		out[0] = static_cast<float>(v.get<double>());
		return true;
	}
	// Color is THREE, not four: the slot's fourth component stays 0, exactly as
	// the codegen leaves it (matParamKindComponents).
	const char* shape = kind == HE::MatParamKind::Vec2 ? "two numbers [x, y]"
	                  : kind == HE::MatParamKind::Vec4 ? "four numbers [x, y, z, w]"
	                                                   : "three numbers [r, g, b]";
	if (!v.is_array() || static_cast<int>(v.size()) != comps)
	{
		why = "Parameter '" + name + "' is a " + paramKindName(kind) + ": 'value' has to "
		      "be " + shape + ".";
		return false;
	}
	for (int k = 0; k < comps; ++k)
	{
		if (!v[static_cast<std::size_t>(k)].is_number())
		{
			why = "Parameter '" + name + "' is a " + paramKindName(kind) + ": every "
			      "element of 'value' has to be a number (" + shape + ").";
			return false;
		}
		out[static_cast<std::size_t>(k)] =
			static_cast<float>(v[static_cast<std::size_t>(k)].get<double>());
	}
	return true;
}

// The names, for a refusal that can be acted on without a second call.
std::string paramNameList(const Mat& m)
{
	std::string s;
	for (const std::string& n : m.paramNames) { if (!s.empty()) s += ", "; s += n; }
	return s;
}

// Write the value into every Param node of `graph` that declares `name` with
// this kind. Returns how many nodes were touched.
int writeNodeDefaults(HE::MaterialGraph& graph, const std::string& name,
                      HE::MatParamKind kind, const std::array<float, 4>& value)
{
	const int comps = HE::matParamKindComponents(kind);
	int touched = 0;
	for (HE::MatGraphNode& n : graph.nodes)
	{
		HE::MatParamKind nk{};
		if (!paramKindOfNode(n.type, nk) || nk != kind) continue;
		if (effectiveParamName(n) != name) continue;
		// Only the components the kind carries: a ParamFloat's p[1]/p[2] are its
		// slider range, and four writes would eat it.
		for (int k = 0; k < comps; ++k) n.p[k] = value[static_cast<std::size_t>(k)];
		++touched;
	}
	return touched;
}

ToolResult failWrite(const std::string& rel)
{
	return ToolResult::fail("failed",
		"Could not write '" + rel + "'. A read-only file or a full disk is the usual "
		"cause; the editor log carries the reason.");
}

// ── material_info ────────────────────────────────────────────────────────────

void addInfo(McpToolRegistry& registry, ContentManager& content,
             const std::shared_ptr<McpMaterialHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "material_info";
	t.description =
		"Without 'path': every material in the project, so a client can find one to "
		"work on. With 'path': one material in full — whether it is a master or an "
		"instance of another, its blend mode and domain, the textures its graph "
		"samples, the landscape paint layers it declares, and above all its exposed "
		"PARAMETERS with name, kind, current value, slider range, group and tooltip. "
		"That parameter list is what material_set_param addresses. The list form "
		"loads nothing; the single-material form loads the asset (and only it).";
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of one material, e.g. "
		                      "'Materials/Rock.hasset'. Omit to list them all.") },
		{ "limit", json{ { "type", "integer" }, { "minimum", 1 },
		                 { "description", "List form: at most this many materials "
		                                  "(default 200)." } } },
	}, {});
	t.handler = [cm, h](const json& args) -> ToolResult {
		if (cm->contentRoot().empty())
			return ToolResult::fail("no_project",
				"No project is open in the editor, so there are no materials. Call "
				"scene_info first.");

		if (hasArg(args, "path"))
		{
			const Mat m = openMat(*cm, *h, args, /*forWrite=*/false);
			if (!m.ok) return m.failure;
			return ToolResult::ok(materialJson(m, *h));
		}

		const int limit = std::max(1, intArg(args, "limit", 200));
		bool truncated = false;
		const std::vector<ContentAsset> found = walkContentAssets(
			*cm, { HE::AssetType::Material, HE::AssetType::MaterialFunction },
			limit, truncated);

		json entries = json::array();
		for (const ContentAsset& a : found)
		{
			json e{
				{ "path", a.rel },
				{ "type", HE::assetTypeName(a.type) },
			};
			// Nothing is loaded to answer this — the list is a question, and a
			// question must not change its answer (a load moves the whole material
			// pool, and an instance's load pulls its parent in after it). A
			// material that is ALREADY resident is free to say more, and that is
			// the one case where master/instance can be told apart here.
			const HE::UUID id = cm->idForPath(a.rel);
			const bool loaded = !(id == HE::UUID{}) && cm->isLoaded(id);
			e["loaded"] = loaded;
			if (loaded && a.type == HE::AssetType::Material)
				if (const MaterialAsset* mat = cm->getMaterial(id))
				{
					e["kind"] = mat->parentMaterialPath.empty() ? "master" : "instance";
					if (!mat->parentMaterialPath.empty())
						e["parent"] = mat->parentMaterialPath;
					e["paramCount"] = static_cast<int>(mat->graphParamNames.size());
				}
			entries.push_back(std::move(e));
		}

		json out{ { "materials", std::move(entries) } };
		if (truncated) out["truncated"] = true;
		return ToolResult::ok(std::move(out));
	};
	registry.add(std::move(t));
}

// ── material_set_param ───────────────────────────────────────────────────────

void addSetParam(McpToolRegistry& registry, ContentManager& content,
                 const std::shared_ptr<McpMaterialHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "material_set_param";
	t.description =
		"Set one exposed shader parameter of a material and save the asset. The name "
		"and the shape of 'value' come from material_info's parameter list: a float "
		"takes a number, a bool takes true/false, a vec2 two numbers, a color three "
		"(r, g, b), a vec4 four. On a MASTER material both the graph's Param node "
		"default and the live parameter block are written, so the value survives the "
		"next edit in the Material Editor, and every loaded instance that does not "
		"override this parameter follows. On an INSTANCE the value is marked as an "
		"override, which is what keeps it from being re-derived from the parent; pass "
		"'override': false (without a value) to drop the override and follow the "
		"parent again. Refused while play-in-editor runs, or while the material is "
		"open in the editor with unsaved changes.";
	t.mutates     = true;
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of the material, e.g. "
		                      "'Materials/Rock.hasset'.") },
		{ "name",  stringProp("Parameter name, exactly as material_info reports it.") },
		{ "value", json{ { "description",
		                   "The new value, in the shape the parameter's kind asks for: "
		                   "number (float), true/false (bool), [x, y] (vec2), "
		                   "[r, g, b] (color), [x, y, z, w] (vec4). Required unless "
		                   "'override' is false." } } },
		{ "override", json{ { "type", "boolean" },
		                    { "description",
		                      "Material INSTANCES only. Default true: keep this "
		                      "instance's own value for the parameter. False: drop the "
		                      "override, so the parameter follows the parent material "
		                      "again (no 'value' needed)." } } },
	}, { "path", "name" });
	t.handler = [cm, h](const json& args) -> ToolResult {
		Mat m = openMat(*cm, *h, args, /*forWrite=*/true);
		if (!m.ok) return m.failure;

		const std::string name = strArg(args, "name");
		if (name.empty())
			return ToolResult::fail("invalid_payload",
				"'name' is required: the parameter to set, spelled as material_info "
				"reports it.");

		if (m.paramNames.empty())
			return ToolResult::fail("no_params",
				"'" + m.rel + "' exposes no shader parameters. A material gets them from "
				"Param nodes in its graph (Param (Float), Param (Color), Param "
				"(Vector2), Param (Vector4), Param (Bool)), and a freshly created one "
				"has no graph at all yet — which is something only the Material Editor "
				"can add. Ask the user to open the material and expose what should be "
				"tunable.");

		const int slot = m.slotOf(name);
		if (slot < 0)
			return ToolResult::fail("unknown_param",
				"'" + m.rel + "' has no parameter called '" + name + "'. Its parameters "
				"are: " + paramNameList(m) + ". Names are case-sensitive and come from "
				"the graph's Param nodes.");
		const HE::MatParamKind kind = m.kindAt(slot);

		// ── The un-override branch: an instance going back to its parent ──────
		const bool wantOverride = boolArg(args, "override", true);
		if (!wantOverride)
		{
			if (!m.isInstance())
				return ToolResult::fail("invalid_payload",
					"'" + m.rel + "' is a master material, not an instance, so there is "
					"no override to drop and nothing to follow. 'override' is only "
					"meaningful on a material created with material_create_instance.");
			if (!m.isOverridden(name))
				return ToolResult::fail("invalid_payload",
					"'" + name + "' is not overridden on '" + m.rel + "' — it already "
					"follows the parent material. material_info reports 'overridden' per "
					"parameter.");

			std::vector<std::string> kept;
			for (const std::string& n : m.overridden)
				if (n != name) kept.push_back(n);
			if (MaterialAsset* a = cm->getMaterialMutable(m.id))
				a->instanceOverriddenParams = std::move(kept);
			else
				return failWrite(m.rel);
			// Re-derive from the parent: dropping the name alone would leave this
			// instance's own value sitting in the slot, which is not "following the
			// parent", it is the same value with the bookkeeping removed. The sync
			// LOADS the parent, so nothing may be held across it.
			cm->syncMaterialInstance(m.id);

			MaterialAsset* after = cm->getMaterialMutable(m.id);
			if (!after || !cm->saveAsset(*after)) return failWrite(m.rel);
			Mat fresh = m;
			if (const MaterialAsset* a2 = cm->getMaterial(m.id)) copyOut(fresh, *a2);

			json out{
				{ "path",       m.rel },
				{ "name",       name },
				{ "kind",       paramKindName(kind) },
				{ "target",     "instance" },
				{ "overridden", false },
			};
			const int newSlot = fresh.slotOf(name);
			if (newSlot >= 0) out["value"] = paramJson(fresh, static_cast<std::size_t>(newSlot),
			                                           nullptr)["value"];
			out["reloadedInEditor"] = h->reloadFromDisk ? h->reloadFromDisk(m.rel) : false;
			return ToolResult::ok(std::move(out));
		}

		// ── The ordinary branch: a new value ─────────────────────────────────
		if (!hasArg(args, "value"))
			return ToolResult::fail("invalid_payload",
				"'value' is required. Parameter '" + name + "' is a " +
				paramKindName(kind) + "; material_info reports the shape, and "
				"'override': false is the only call that needs no value.");

		std::array<float, 4> value{};
		std::string why;
		if (!valueFromJson(args["value"], kind, name, value, why))
			return ToolResult::fail("invalid_payload", why);

		// The slider range, where the material declares one. Refused rather than
		// clamped: a clamp is a value the client did not ask for, reported as a
		// success, and the editor's own slider cannot be dragged out of the range
		// either.
		if (kind == HE::MatParamKind::Float &&
		    static_cast<std::size_t>(slot) * 2 + 1 < m.paramMinMax.size())
		{
			const float lo = m.paramMinMax[static_cast<std::size_t>(slot) * 2];
			const float hi = m.paramMinMax[static_cast<std::size_t>(slot) * 2 + 1];
			if (lo < hi && (value[0] < lo || value[0] > hi))
				return ToolResult::fail("out_of_range",
					"Parameter '" + name + "' is declared with a range of " +
					std::to_string(lo) + " to " + std::to_string(hi) + ", and the editor's "
					"slider for it cannot leave that range either. Nothing was written.");
		}

		json out{
			{ "path", m.rel },
			{ "name", name },
			{ "kind", paramKindName(kind) },
			{ "slot", slot },
		};

		if (m.isInstance())
		{
			MaterialAsset* a = cm->getMaterialMutable(m.id);
			if (!a) return failWrite(m.rel);
			for (int k = 0; k < 4; ++k)
			{
				const std::size_t at = static_cast<std::size_t>(slot) * 4 +
				                       static_cast<std::size_t>(k);
				if (at < a->shaderParamData.size()) a->shaderParamData[at] = value[k];
			}
			// The override mark is not decoration: without it the next
			// syncMaterialInstance — which runs whenever the parent is edited —
			// copies the parent's value straight back over this one.
			if (!m.isOverridden(name)) a->instanceOverriddenParams.push_back(name);
			if (!cm->saveAsset(*a)) return failWrite(m.rel);
			out["target"]     = "instance";
			out["overridden"] = true;
			out["parent"]     = m.parentMaterialPath;
		}
		else
		{
			// The graph's Param node default AND the live slot, in that order —
			// McpToolRegistry.h says why either alone is reverted later.
			HE::MaterialGraph graph;
			const bool haveGraph = !m.nodeGraphJson.empty() &&
			                       HE::materialGraphFromJson(m.nodeGraphJson, graph);
			int nodes = 0;
			std::string graphJson;
			if (haveGraph)
			{
				nodes = writeNodeDefaults(graph, name, kind, value);
				if (nodes > 0) graphJson = HE::materialGraphToJson(graph);
			}

			// In its own scope, because the two calls after it LOAD and the pointer
			// must not outlive them — see Mat's comment.
			{
				MaterialAsset* a = cm->getMaterialMutable(m.id);
				if (!a) return failWrite(m.rel);
				if (!graphJson.empty()) a->nodeGraphJson = graphJson;
				for (int k = 0; k < 4; ++k)
				{
					const std::size_t at = static_cast<std::size_t>(slot) * 4 +
					                       static_cast<std::size_t>(k);
					if (at < a->shaderParamData.size()) a->shaderParamData[at] = value[k];
				}
			}

			// Re-run the codegen, exactly as the Material Editor does after an edit:
			// the generated GLSL is unchanged by a value, but the GI approximation
			// colours and the param metadata are derived from the graph, and the
			// value is carried over by name (so the slot write above is what it
			// carries). Skipped for a material with no graph — a hand-written
			// escape-hatch shader has no codegen to re-run.
			if (haveGraph) cm->regenerateMaterialFromGraph(m.id);
			// Live master → variants, the same call applyToMaterial makes: an
			// instance that does not override this parameter must not keep showing
			// the old value.
			cm->syncMaterialInstancesOf(m.rel);

			MaterialAsset* after = cm->getMaterialMutable(m.id);
			if (!after || !cm->saveAsset(*after)) return failWrite(m.rel);

			out["target"]              = "master";
			out["graphDefaultUpdated"] = nodes > 0;
			out["graphNodesUpdated"]   = nodes;
			out["syncedLoadedInstances"] = true;
			if (haveGraph && nodes == 0)
				out["note"] =
					"No Param node of this material's own graph declares '" + name + "' — "
					"it comes from a material FUNCTION the graph calls. The value is in "
					"the asset and the renderer uses it, but the next edit in the Material "
					"Editor regenerates the parameter block from the function's own "
					"default and resets it. Change it in that function to make it stick.";
		}

		// Read back from the asset rather than echoing the argument: after the
		// regenerate this is what the file holds.
		Mat fresh = m;
		if (const MaterialAsset* a2 = cm->getMaterial(m.id)) copyOut(fresh, *a2);
		const int newSlot = fresh.slotOf(name);
		if (newSlot >= 0)
			out["value"] = paramJson(fresh, static_cast<std::size_t>(newSlot),
			                         nullptr)["value"];

		// The tab was clean — openMat refused otherwise — so this is the
		// collab-peer path: drop what it had and read the file again next frame.
		out["reloadedInEditor"] = h->reloadFromDisk ? h->reloadFromDisk(m.rel) : false;
		return ToolResult::ok(std::move(out));
	};
	registry.add(std::move(t));
}

// ── material_create_instance ─────────────────────────────────────────────────

void addCreateInstance(McpToolRegistry& registry, ContentManager& content,
                       const std::shared_ptr<McpMaterialHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "material_create_instance";
	t.description =
		"Create a material INSTANCE of an existing material — one master, many "
		"variants: the child has no graph of its own, shares the parent's compiled "
		"shader and differs only in the parameter values it overrides. This is the one "
		"kind of material asset_create cannot make (that one writes a plain, empty "
		"master). The parent may be an engine default; the new asset may not live "
		"under 'Engine/'. Omit 'path' for the sibling name the Content Browser would "
		"pick ('<parent>_Inst.hasset'). Set values on the result with "
		"material_set_param.";
	t.mutates     = true;
	t.inputSchema = objectSchema(json{
		{ "parent", stringProp("Content-relative path of the material to derive from, "
		                       "e.g. 'Materials/Rock.hasset' or "
		                       "'Engine/Materials/Default.hasset'.") },
		{ "path",   stringProp("Content-relative path for the new instance, e.g. "
		                       "'Materials/Rock_Mossy.hasset'. A missing '.hasset' "
		                       "suffix is appended. Omit for '<parent>_Inst.hasset' "
		                       "beside the parent, uniquified.") },
	}, { "parent" });
	t.handler = [cm, h](const json& args) -> ToolResult {
		if (h->isPlaying && h->isPlaying())
			return ToolResult::fail("play_mode",
				"Play-in-editor is running. Creating an asset now would register it in "
				"the content manager the running session draws from, so it is refused "
				"rather than half-applied. Ask the user to stop play mode.");
		// The project's own gate, mirrored: a project without Advanced Shader
		// Effects has no Material row in the create menu and no "Create Material
		// Instance" either. MCP must not create what the menu will not offer.
		if (h->materialsAllowed && !h->materialsAllowed())
			return ToolResult::fail("invalid_payload",
				"This project does not author materials — the Content Browser offers "
				"neither the Material row nor 'Create Material Instance'. The gate is "
				"the project's Advanced Shader Effects setting.");

		const PathCheck parent = checkPath(*cm, strArg(args, "parent"),
		                                   /*mustExist=*/true, "parent");
		if (!parent.ok) return parent.failure;
		const HE::AssetType parentType = EditorAssetTypeCache::assetTypeOf(parent.abs);
		if (parentType != HE::AssetType::Material)
			return ToolResult::fail("invalid_path",
				"'" + parent.rel + "' is not a Material asset, so there is nothing to "
				"derive from. A Material FUNCTION is a sub-graph a material's graph "
				"calls, not a material; asset_resolve reports what a path holds.");

		// The default name is the Content Browser's, uniquified the same way:
		// <stem>_Inst[.N].hasset beside the parent.
		std::string rawPath = strArg(args, "path");
		if (rawPath.empty())
		{
			const std::filesystem::path pp(parent.rel);
			const std::string dir  = pp.parent_path().generic_string();
			const std::string stem = pp.stem().string();
			// Beside the parent — except under 'Engine/', where a child is refused
			// outright (it is the shipped, shared content): deriving from an engine
			// default is a normal thing to want, so the fallback lands the child in
			// the project's own content root rather than refusing a call that named
			// no path at all.
			const std::string base = (parent.engine || dir.empty()) ? stem : dir + "/" + stem;
			rawPath = base + "_Inst.hasset";
			for (int k = 2; k < 100; ++k)
			{
				const std::string abs = cm->resolveAbsolutePath(rawPath);
				std::error_code existsEc;
				if (abs.empty() || !std::filesystem::exists(abs, existsEc)) break;
				rawPath = base + "_Inst" + std::to_string(k) + ".hasset";
			}
		}
		else if (std::filesystem::path(rawPath).extension().empty())
		{
			rawPath += ".hasset";
		}
		else if (std::filesystem::path(rawPath).extension() != ".hasset")
		{
			return ToolResult::fail("invalid_path",
				"'" + rawPath + "' does not end in '.hasset'. A material instance is an "
				"asset like any other; the suffix is not negotiable, and a wrong one is "
				"refused rather than corrected.");
		}

		const PathCheck dst = checkPath(*cm, rawPath, /*mustExist=*/false, "path");
		if (!dst.ok) return dst.failure;
		if (dst.engine) return failEngineReadOnly(dst.rel);
		std::error_code ec;
		if (std::filesystem::exists(dst.abs, ec))
			return ToolResult::fail("already_exists",
				"'" + dst.rel + "' already exists. A client that named a path explicitly "
				"is more likely to have meant a different one than to have wanted it "
				"overwritten, so nothing was written. Choose another path, or omit 'path' "
				"for the next free sibling name.");
		if (h->lockedByOther && h->lockedByOther(dst.rel))
			return ToolResult::fail("locked_by_other",
				"Another participant in the collaboration session holds '" + dst.rel +
				"' right now. Wait until they let go, or choose another path.");

		std::filesystem::create_directories(std::filesystem::path(dst.abs).parent_path(), ec);

		// The Content Browser's own flow (ContentBrowserPanel, "Create Material
		// Instance"): registered with the parent path, derived by the content
		// manager, then written. No stub writer — a stub is a MASTER, and a file
		// with neither graph nor parent is the one thing an instance must not be.
		MaterialAsset inst;
		inst.type = HE::AssetType::Material;
		inst.name = std::filesystem::path(dst.abs).stem().string();
		inst.path = dst.rel;
		inst.parentMaterialPath = parent.rel;
		const HE::UUID id = cm->registerMaterial(std::move(inst));
		if (id == HE::UUID{})
			return ToolResult::fail("failed",
				"The instance could not be registered in the content manager. The editor "
				"log carries the reason.");
		// Derives the shader and the whole parameter layout from the parent — this
		// is what makes the new asset a usable material rather than an empty file.
		// It LOADS the parent, so no pointer may be held across it.
		cm->syncMaterialInstance(id);
		MaterialAsset* written = cm->getMaterialMutable(id);
		if (!written || !cm->saveAsset(*written)) return failWrite(dst.rel);

		EditorAssetTypeCache::invalidate(dst.abs);
		if (h->onAssetAppeared) h->onAssetAppeared(dst.abs);
		// Published, not requested: nothing refers to a brand-new asset yet, so
		// there is nothing for the host to arbitrate.
		if (h->publishCreate) h->publishCreate(dst.rel, dst.abs);

		// The full report, so the very next call can be a material_set_param: the
		// parameters the instance inherited are in it.
		Mat m;
		m.rel = dst.rel;
		m.abs = dst.abs;
		m.id  = id;
		if (const MaterialAsset* a = cm->getMaterial(id)) copyOut(m, *a);
		json out = materialJson(m, *h);
		out["created"] = true;
		return ToolResult::ok(std::move(out));
	};
	registry.add(std::move(t));
}

} // namespace

void registerMaterialTools(McpToolRegistry& registry, ContentManager& content,
                           McpMaterialHooks hooks)
{
	// Shared rather than copied into each handler: the hooks hold std::functions
	// that capture the editor, and three copies of them would be three chances to
	// let one go stale.
	auto h = std::make_shared<McpMaterialHooks>(std::move(hooks));
	addInfo(registry, content, h);
	addSetParam(registry, content, h);
	addCreateInstance(registry, content, h);
}

} // namespace HE::Ed
