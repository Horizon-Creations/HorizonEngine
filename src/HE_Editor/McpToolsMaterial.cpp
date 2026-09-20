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
#include <map>
#include <memory>
#include <set>
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
	// A material FUNCTION, accepted only by the graph reader: it has a graph and
	// nothing else below (no parameters, no blend mode, no parent).
	bool        isFunction = false;
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

// `acceptFunction`: the graph reader takes a material FUNCTION too (its graph is
// the whole point of reading it); the parameter tools refuse one, because a
// function declares no slots. Never combined with `forWrite` — nothing here
// writes a function.
Mat openMat(ContentManager& content, const McpMaterialHooks& h, const json& args,
            bool forWrite, bool acceptFunction = false)
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
	if (sniffed == HE::AssetType::MaterialFunction && acceptFunction)
	{
		m.openDirty = h.isDirty && h.isDirty(p.rel);
		m.id = content.loadAsset(p.rel);
		const MaterialFunctionAsset* fn =
			m.id == HE::UUID{} ? nullptr : content.getMaterialFunction(m.id);
		if (!fn)
		{
			m.failure = ToolResult::fail("failed",
				"'" + p.rel + "' reads as a Material Function asset but could not be "
				"loaded. The editor log carries the reason.");
			return m;
		}
		m.nodeGraphJson = fn->nodeGraphJson;
		m.isFunction    = true;
		m.ok            = true;
		return m;
	}
	if (sniffed == HE::AssetType::MaterialFunction)
	{
		m.failure = ToolResult::fail("invalid_path",
			"'" + p.rel + "' is a Material FUNCTION — a reusable sub-graph, not a "
			"material. It declares no parameter slots of its own; the material whose "
			"graph calls it is what carries them, and that is what to address here. "
			"material_graph_info reads a function's own graph and interface.");
		return m;
	}
	if (sniffed != HE::AssetType::Material)
	{
		m.failure = ToolResult::fail("invalid_path",
			"'" + p.rel + "' is not a Material asset. asset_resolve reports what a path "
			"holds, material_info without a path lists every material in the project, "
			"and material_create makes a new one with a graph and parameters.");
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

// ── material_graph_info ──────────────────────────────────────────────────────
// The STRUCTURE of a material — which nodes, how wired, what feeds each Output
// pin — where material_info answers only the parameter slots. Two forms in one
// tool: the graph (nodes with resolved pin lists, links, comments) for a client
// that is going to address a node, and the summary (one line per Output pin,
// textures, functions, switches, warnings) for the far more common "what does
// this material do".
//
//   • NODE TYPES GO OUT UNDER THEIR ENUM NAME (`TextureSample`), not the
//     display name the file stores (`"Texture Sample"`). The display name is
//     beside it, because that is what the panel shows; the enum name is what a
//     later material_add_node takes, so the vocabulary is fixed here, once.
//     The table below is the only place that spells the names, and the test
//     sweeps the registry to prove it is complete and unambiguous.
//
//   • PIN NAMES ARE RESOLVED THE WAY THE CANVAS RESOLVES THEM: matNodeDesc for
//     the library, matOutputPins for the Output node (so pin 5 is "OpacityMask"
//     on a Masked material and absent on an Opaque one), matFunctionPins for a
//     FunctionCall, matLandscapeLayerNames for a Landscape Layer Blend. The
//     answer cannot disagree with the panel because it is the panel's code.
//
//   • AN INSTANCE ANSWERS WITH ITS PARENT'S GRAPH under its own switch
//     overrides. An instance has no graph, and "what does my material do" is
//     still the right question to ask of one.
//
//   • A FUNCTION IS ACCEPTED, its interface (FnInput / FnOutput) reported in
//     pin order — the one place over this interface where a function is
//     readable at all.
//
//   • EVERYTHING IS COPIED before anything is loaded (a called function, an
//     instance's parent): the pool is a dense vector, and matFunctionPins hands
//     back pin names that point INTO the graph it was given, so that graph is a
//     local copy held until the JSON is built.

// Enum-name table. No default label: a new MatNodeType has to be named here
// deliberately, and the registry sweep in the tests fails until it is.
const char* nodeTypeName(HE::MatNodeType t)
{
	using T = HE::MatNodeType;
	switch (t)
	{
	case T::Output:              return "Output";
	case T::ConstFloat:          return "ConstFloat";
	case T::ConstColor:          return "ConstColor";
	case T::VertexColor:         return "VertexColor";
	case T::NormalWS:            return "NormalWS";
	case T::UV:                  return "UV";
	case T::Time:                return "Time";
	case T::TextureSample:       return "TextureSample";
	case T::Add:                 return "Add";
	case T::Multiply:            return "Multiply";
	case T::Lerp:                return "Lerp";
	case T::OneMinus:            return "OneMinus";
	case T::Power:               return "Power";
	case T::Saturate:            return "Saturate";
	case T::DotProduct:          return "DotProduct";
	case T::Sine:                return "Sine";
	case T::Fresnel:             return "Fresnel";
	case T::Combine3:            return "Combine3";
	case T::WorldPos:            return "WorldPos";
	case T::ViewDir:             return "ViewDir";
	case T::ParamFloat:          return "ParamFloat";
	case T::ParamColor:          return "ParamColor";
	case T::Subtract:            return "Subtract";
	case T::Divide:              return "Divide";
	case T::Absolute:            return "Absolute";
	case T::Fract:               return "Fract";
	case T::Smoothstep:          return "Smoothstep";
	case T::Step:                return "Step";
	case T::Normalize3:          return "Normalize3";
	case T::Panner:              return "Panner";
	case T::ValueNoise:          return "ValueNoise";
	case T::Fbm:                 return "Fbm";
	case T::Checker:             return "Checker";
	case T::SplitRGBA:           return "SplitRGBA";
	case T::CombineRGBA:         return "CombineRGBA";
	case T::FnInput:             return "FnInput";
	case T::FnOutput:            return "FnOutput";
	case T::FunctionCall:        return "FunctionCall";
	case T::ConstVec2:           return "ConstVec2";
	case T::ConstVec4:           return "ConstVec4";
	case T::CameraPos:           return "CameraPos";
	case T::CameraDistance:      return "CameraDistance";
	case T::ScreenPos:           return "ScreenPos";
	case T::ConstBool:           return "ConstBool";
	case T::ParamVec2:           return "ParamVec2";
	case T::ParamVec4:           return "ParamVec4";
	case T::ParamBool:           return "ParamBool";
	case T::If:                  return "If";
	case T::Greater:             return "Greater";
	case T::Less:                return "Less";
	case T::GreaterEqual:        return "GreaterEqual";
	case T::LessEqual:           return "LessEqual";
	case T::Equal:               return "Equal";
	case T::NotEqual:            return "NotEqual";
	case T::And:                 return "And";
	case T::Or:                  return "Or";
	case T::Not:                 return "Not";
	case T::NoiseTexture:        return "NoiseTexture";
	case T::Reroute:             return "Reroute";
	case T::StaticSwitch:        return "StaticSwitch";
	case T::NormalMapSample:     return "NormalMapSample";
	case T::LandscapeLayerBlend: return "LandscapeLayerBlend";
	case T::ElementSize:         return "ElementSize";
	case T::ElementUV:           return "ElementUV";
	case T::RoundedRectSDF:      return "RoundedRectSDF";
	case T::BorderDistance:      return "BorderDistance";
	case T::ElementState:        return "ElementState";
	case T::Backdrop:            return "Backdrop";
	}
	return "";
}

const char* pinTypeName(HE::MatPinType t)
{
	switch (t)
	{
	case HE::MatPinType::Float: return "float";
	case HE::MatPinType::Vec2:  return "vec2";
	case HE::MatPinType::Vec3:  return "vec3";
	case HE::MatPinType::Vec4:  return "vec4";
	}
	return "float";
}

// Function graphs the material calls, parsed once each. Owns the graphs, so
// the pin names matFunctionPins hands out stay valid for the whole build.
struct FnGraphs
{
	ContentManager&                         cm;
	std::map<std::string, HE::MaterialGraph> loaded;
	std::set<std::string>                   missing;

	const HE::MaterialGraph* get(const std::string& path)
	{
		if (auto it = loaded.find(path); it != loaded.end()) return &it->second;
		if (missing.count(path)) return nullptr;
		// Copied out before parsing: the load moves the pool. A call node with
		// no path yet is a call to nothing, not a load of "".
		std::string js;
		if (!path.empty())
		{
			const MaterialFunctionAsset* fn = cm.getMaterialFunction(cm.loadAsset(path));
			if (fn) js = fn->nodeGraphJson;
		}
		HE::MaterialGraph g;
		if (js.empty() || !HE::materialGraphFromJson(js, g))
		{
			missing.insert(path);
			return nullptr;
		}
		return &(loaded[path] = std::move(g));
	}
};

// The input pins of a node, as the canvas lays them out. `blendMode` is the
// Output node's, for the one pin whose name and presence depend on it.
struct ResolvedPins
{
	std::vector<HE::MatPinDesc> inputs;
	std::vector<int>            inputIndex;   // registry pin index per row
	std::vector<HE::MatPinDesc> outputs;
};

ResolvedPins resolvePins(const HE::MatGraphNode& n, int blendMode, FnGraphs& fns,
                         std::vector<std::string>& layerScratch)
{
	ResolvedPins r;
	const HE::MatNodeDesc& d = HE::matNodeDesc(n.type);
	if (n.type == HE::MatNodeType::Output)
	{
		HE::matOutputPins(blendMode, r.inputs, r.inputIndex);
	}
	else if (n.type == HE::MatNodeType::FunctionCall)
	{
		if (const HE::MaterialGraph* fg = fns.get(n.s))
			HE::matFunctionPins(*fg, r.inputs, r.outputs);
		for (int i = 0; i < static_cast<int>(r.inputs.size()); ++i) r.inputIndex.push_back(i);
		return r;
	}
	else if (n.type == HE::MatNodeType::LandscapeLayerBlend)
	{
		layerScratch = HE::matLandscapeLayerNames(n.s);
		for (int i = 0; i < static_cast<int>(layerScratch.size()); ++i)
		{
			r.inputs.push_back({ layerScratch[static_cast<std::size_t>(i)].c_str(),
			                     HE::MatPinType::Vec3, 0.0f });
			r.inputIndex.push_back(i);
		}
	}
	else
	{
		r.inputs = d.inputs;
		for (int i = 0; i < static_cast<int>(r.inputs.size()); ++i) r.inputIndex.push_back(i);
	}
	r.outputs = d.outputs;
	return r;
}

const HE::MatGraphLink* linkInto(const HE::MaterialGraph& g, int node, int pin)
{
	for (const HE::MatGraphLink& l : g.links)
		if (l.dstNode == node && l.dstPin == pin) return &l;
	return nullptr;
}

int connectedInputs(const HE::MaterialGraph& g, int node)
{
	int n = 0;
	for (const HE::MatGraphLink& l : g.links)
		if (l.dstNode == node) ++n;
	return n;
}

// The most useful thing a node carries, for the one-line chain.
std::string nodeDetail(const HE::MatGraphNode& n)
{
	switch (n.type)
	{
	case HE::MatNodeType::TextureSample:
	case HE::MatNodeType::NormalMapSample:
	case HE::MatNodeType::FunctionCall:
		return n.s.empty() ? std::string("no path") : n.s;
	case HE::MatNodeType::ParamFloat:
	case HE::MatNodeType::ParamColor:
	case HE::MatNodeType::ParamVec2:
	case HE::MatNodeType::ParamVec4:
	case HE::MatNodeType::ParamBool:
	case HE::MatNodeType::StaticSwitch:
	case HE::MatNodeType::FnInput:
	case HE::MatNodeType::FnOutput:
		return effectiveParamName(n);
	case HE::MatNodeType::ConstFloat:
	case HE::MatNodeType::ConstBool:
		return std::to_string(n.p[0]);
	case HE::MatNodeType::ConstColor:
		return std::to_string(n.p[0]) + ", " + std::to_string(n.p[1]) + ", " +
		       std::to_string(n.p[2]);
	default:
		return {};
	}
}

// Follow the wire into (node, pin) backwards: "Multiply #9 ← Texture Sample #7
// (Textures/Rock.hasset) ← UV #3". Reroutes are skipped, the walk stops at the
// first node with more than one connected input (a summary, not the graph) or
// at depth 6, and says "…" when it stopped short.
std::string chainInto(const HE::MaterialGraph& g, int node, int pin)
{
	std::string s;
	int depth = 0;
	const HE::MatGraphLink* l = linkInto(g, node, pin);
	while (l)
	{
		const HE::MatGraphNode* src = g.findNode(l->srcNode);
		if (!src) break;
		if (src->type == HE::MatNodeType::Reroute)
		{
			l = linkInto(g, src->id, 0);
			continue;
		}
		if (!s.empty()) s += " <- ";
		s += std::string(HE::matNodeDesc(src->type).name) + " #" + std::to_string(src->id);
		const std::string detail = nodeDetail(*src);
		if (!detail.empty()) s += " (" + detail + ")";

		const int fanIn = connectedInputs(g, src->id);
		if (fanIn == 0) break;
		if (fanIn > 1 || ++depth >= 6) { s += " <- ..."; break; }
		// Exactly one connected input: follow it, whichever pin it is on.
		l = nullptr;
		for (const HE::MatGraphLink& cand : g.links)
			if (cand.dstNode == src->id) { l = &cand; break; }
	}
	return s;
}

// The wire's far end, through Reroutes — the (node, pin) a later
// material_connect would name.
bool sourceOf(const HE::MaterialGraph& g, int node, int pin, json& out)
{
	const HE::MatGraphLink* l = linkInto(g, node, pin);
	for (int guard = 0; l && guard < 64; ++guard)
	{
		const HE::MatGraphNode* src = g.findNode(l->srcNode);
		if (src && src->type == HE::MatNodeType::Reroute)
		{
			l = linkInto(g, src->id, 0);
			continue;
		}
		out = json{ { "node", l->srcNode }, { "pin", l->srcPin } };
		return true;
	}
	return false;
}

json nodeJson(const HE::MaterialGraph& g, const HE::MatGraphNode& n, int blendMode,
              FnGraphs& fns)
{
	const HE::MatNodeDesc& d = HE::matNodeDesc(n.type);
	std::vector<std::string> layerScratch;
	const ResolvedPins pins = resolvePins(n, blendMode, fns, layerScratch);

	json p = json::array();
	for (int k = 0; k < d.paramCount && k < 4; ++k) p.push_back(n.p[k]);

	json inputs = json::array();
	for (std::size_t i = 0; i < pins.inputs.size(); ++i)
	{
		const int idx = pins.inputIndex[i];
		json pin{
			{ "pin",     idx },
			{ "name",    pins.inputs[i].name },
			{ "type",    pinTypeName(pins.inputs[i].type) },
			{ "default", pins.inputs[i].def },
		};
		json src;
		const bool connected = sourceOf(g, n.id, idx, src);
		pin["connected"] = connected;
		if (connected) pin["source"] = std::move(src);
		inputs.push_back(std::move(pin));
	}
	json outputs = json::array();
	for (std::size_t i = 0; i < pins.outputs.size(); ++i)
		outputs.push_back(json{
			{ "pin",  static_cast<int>(i) },
			{ "name", pins.outputs[i].name },
			{ "type", pinTypeName(pins.outputs[i].type) },
		});

	json j{
		{ "id",          n.id },
		{ "type",        nodeTypeName(n.type) },
		{ "displayName", d.name },
		{ "category",    d.category },
		{ "x", n.x }, { "y", n.y },
		{ "p",           std::move(p) },
		{ "inputs",      std::move(inputs) },
		{ "outputs",     std::move(outputs) },
	};
	if (!n.s.empty()) j["s"] = n.s;

	HE::MatParamKind kind{};
	if (paramKindOfNode(n.type, kind))
	{
		j["paramName"] = effectiveParamName(n);
		j["kind"]      = paramKindName(kind);
		if (!n.group.empty())   j["group"]   = n.group;
		if (!n.tooltip.empty()) j["tooltip"] = n.tooltip;
		if (n.type == HE::MatNodeType::ParamFloat && n.p[1] < n.p[2])
		{
			j["min"] = n.p[1];
			j["max"] = n.p[2];
		}
	}
	switch (n.type)
	{
	case HE::MatNodeType::TextureSample:
	case HE::MatNodeType::NormalMapSample:
		j["texture"] = n.s;
		break;
	case HE::MatNodeType::FunctionCall:
		j["function"] = n.s;
		if (fns.missing.count(n.s)) j["missing"] = true;
		break;
	case HE::MatNodeType::LandscapeLayerBlend:
		j["layers"] = layerScratch;
		break;
	case HE::MatNodeType::StaticSwitch:
		j["switchName"]    = n.s;
		j["switchDefault"] = n.p[0] >= 0.5f;
		break;
	case HE::MatNodeType::FnInput:
		j["hasDefault"] = n.p[2] >= 0.5f;
		break;
	default:
		break;
	}
	return j;
}

void addGraphInfo(McpToolRegistry& registry, ContentManager& content,
                  const std::shared_ptr<McpMaterialHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "material_graph_info";
	t.description =
		"The STRUCTURE of one material's graph, where material_info reports only "
		"its parameter slots: every node with its type, position, values and a "
		"resolved pin list (names and indices exactly as the Material Editor shows "
		"them), every link, and for each Output pin what feeds it as a one-line "
		"chain, plus the textures sampled, the functions called, the static "
		"switches and the warnings the canvas would show. Node ids and pin indices "
		"are the ones a graph edit would name. A material INSTANCE answers with its "
		"parent's graph under its own switch overrides; a material FUNCTION is "
		"accepted too and reports its FnInput/FnOutput interface in pin order. "
		"'summary_only' drops nodes, links and comments for the cheap answer to "
		"'what does this material do'. Loads the asset, its parent for an instance, "
		"and every function the graph calls; generates no shader.";
	t.inputSchema = objectSchema(json{
		{ "path", stringProp("Content-relative path of a material or material "
		                     "function, e.g. 'Materials/Rock.hasset'.") },
		{ "summary_only", json{ { "type", "boolean" },
		                        { "description",
		                          "Default false. True: only the Output pin "
		                          "summary, interface, textures, functions, "
		                          "switches and warnings — no node or link list." } } },
	}, { "path" });
	t.handler = [cm, h](const json& args) -> ToolResult {
		if (cm->contentRoot().empty())
			return ToolResult::fail("no_project",
				"No project is open in the editor, so there are no materials. Call "
				"scene_info first.");
		const Mat m = openMat(*cm, *h, args, /*forWrite=*/false, /*acceptFunction=*/true);
		if (!m.ok) return m.failure;
		const bool summaryOnly = boolArg(args, "summary_only", false);

		const char* kind = m.isFunction ? "function" : m.isInstance() ? "instance" : "master";
		json out{
			{ "path",          m.rel },
			{ "uuid",          uuidJson(m.id) },
			{ "kind",          kind },
			{ "graphVersion",  HE::kMatGraphVersion },
			{ "dirtyInEditor", m.openDirty },
		};

		// An instance has no graph; its parent's is the answer. Copied out
		// before anything else is loaded, like everything in this file.
		std::string graphJson = m.nodeGraphJson;
		if (m.isInstance())
		{
			out["parent"] = m.parentMaterialPath;
			const HE::UUID pid = cm->loadAsset(m.parentMaterialPath);
			const MaterialAsset* parent = pid == HE::UUID{} ? nullptr : cm->getMaterial(pid);
			if (!parent)
				return ToolResult::fail("failed",
					"'" + m.rel + "' is an instance of '" + m.parentMaterialPath + "', "
					"which could not be loaded, so there is no graph to report. "
					"asset_resolve reports what that path holds.");
			graphJson = parent->nodeGraphJson;
			json sw = json::array();
			for (std::size_t i = 0; i < m.switchNames.size(); ++i)
				sw.push_back(json{ { "name", m.switchNames[i] },
				                   { "value", i < m.switchValues.size() &&
				                              m.switchValues[i] != 0 } });
			out["switchOverrides"] = std::move(sw);
		}

		HE::MaterialGraph g;
		const bool haveGraph = !graphJson.empty() && HE::materialGraphFromJson(graphJson, g);
		out["hasGraph"] = haveGraph;
		if (!haveGraph)
		{
			// A stub from asset_create, or a hand-written shader material: there
			// is nothing to draw. Not a refusal — the answer to "what is in it" is
			// "nothing", and the note says where a graph comes from.
			out["nodes"] = json::array();
			out["links"] = json::array();
			out["note"]  = m.isFunction
				? "This material function has no graph yet; the Material Editor adds one."
				: "This material has no node graph — it is either a stub from "
				  "asset_create or a hand-written shader material. material_create "
				  "makes a material with a graph and parameters from birth.";
			return ToolResult::ok(std::move(out));
		}

		// The Output node decides the blend mode, and with it pin 5's name.
		const HE::MatGraphNode* outNode = nullptr;
		for (const HE::MatGraphNode& n : g.nodes)
			if (n.type == HE::MatNodeType::Output) { outNode = &n; break; }
		const int blendMode = outNode ? static_cast<int>(outNode->p[1]) : 0;

		FnGraphs fns{ *cm, {}, {} };
		json warnings = json::array();

		// ── Summary half: the Output pins, or the function's interface ─────────
		if (m.isFunction)
		{
			json ins = json::array(), outs = json::array();
			std::vector<HE::MatPinDesc> fi, fo;
			HE::matFunctionPins(g, fi, fo);
			// The pin descs point into g's node strings — g outlives this scope.
			std::vector<const HE::MatGraphNode*> inNodes;
			for (const HE::MatGraphNode& n : g.nodes)
				if (n.type == HE::MatNodeType::FnInput) inNodes.push_back(&n);
			std::sort(inNodes.begin(), inNodes.end(),
			          [](const HE::MatGraphNode* a, const HE::MatGraphNode* b) { return a->id < b->id; });
			for (std::size_t i = 0; i < fi.size(); ++i)
			{
				json pin{ { "pin", static_cast<int>(i) }, { "name", fi[i].name },
				          { "type", pinTypeName(fi[i].type) } };
				if (i < inNodes.size())
				{
					pin["node"] = inNodes[i]->id;
					if (inNodes[i]->p[2] >= 0.5f) pin["default"] = inNodes[i]->p[1];
				}
				ins.push_back(std::move(pin));
			}
			for (std::size_t i = 0; i < fo.size(); ++i)
				outs.push_back(json{ { "pin", static_cast<int>(i) }, { "name", fo[i].name },
				                     { "type", pinTypeName(fo[i].type) } });
			out["interface"] = json{ { "inputs", std::move(ins) }, { "outputs", std::move(outs) } };
			if (fo.empty())
				warnings.push_back("The function has no Function Output node; a call "
				                   "to it emits magenta.");
		}
		else if (!outNode)
		{
			warnings.push_back("The graph has no Output node; the codegen yields the "
			                   "magenta error shader.");
		}
		else
		{
			std::vector<HE::MatPinDesc> pins;
			std::vector<int>            regIndex;
			HE::matOutputPins(blendMode, pins, regIndex);
			json pinList = json::array();
			for (std::size_t i = 0; i < pins.size(); ++i)
			{
				json src;
				const bool connected = sourceOf(g, outNode->id, regIndex[i], src);
				json pin{
					{ "pin",       regIndex[i] },
					{ "name",      pins[i].name },
					{ "type",      pinTypeName(pins[i].type) },
					{ "default",   pins[i].def },
					{ "connected", connected },
				};
				if (connected)
				{
					pin["source"] = std::move(src);
					pin["chain"]  = chainInto(g, outNode->id, regIndex[i]);
				}
				pinList.push_back(std::move(pin));
			}
			// The pin the blend mode hides: a wire into it is silently ignored.
			if (blendMode == static_cast<int>(HE::MatBlendMode::Opaque) &&
			    linkInto(g, outNode->id, HE::kMatOutputOpacityPin))
				warnings.push_back("Output pin 'Opacity' is connected but the blend mode "
				                   "is Opaque, so it is ignored; set the mode to Masked "
				                   "or Translucent for it to take effect.");

			// The CPU fold the GI kernels shade with — the closest thing to "what
			// colour is it" without running the shader. A pin that folds to a
			// Param node reports the slot's LIVE value from the parameter block.
			std::map<std::string, bool> overrides;
			for (std::size_t i = 0; i < m.switchNames.size(); ++i)
				overrides[m.switchNames[i]] = i < m.switchValues.size() && m.switchValues[i] != 0;
			const HE::MatApproxSurface ap =
				HE::matGraphApproxSurface(g, m.isInstance() ? &overrides : nullptr);
			auto liveOrFolded = [&](const std::string& param, const float folded[3]) {
				const int slot = param.empty() ? -1 : m.slotOf(param);
				json c = json::array();
				for (int k = 0; k < 3; ++k)
				{
					const std::size_t at = static_cast<std::size_t>(slot) * 4 + static_cast<std::size_t>(k);
					c.push_back(slot >= 0 && at < m.paramData.size() ? m.paramData[at] : folded[k]);
				}
				return c;
			};
			json approx{
				{ "baseColor", liveOrFolded(ap.baseColorParam, ap.baseColor) },
				{ "emissive",  liveOrFolded(ap.emissiveParam, ap.emissive) },
				{ "metallic",  ap.metallic },
				{ "roughness", ap.roughness },
			};
			if (!ap.baseColorParam.empty()) approx["baseColorParam"] = ap.baseColorParam;
			if (!ap.emissiveParam.empty())  approx["emissiveParam"]  = ap.emissiveParam;

			out["output"] = json{
				{ "node",       outNode->id },
				{ "lit",        outNode->p[0] >= 0.5f },
				{ "blendMode",  blendModeName(static_cast<std::uint8_t>(blendMode)) },
				{ "maskCutoff", outNode->p[2] },
				{ "domain",     HE::matDomainName(static_cast<HE::MatDomain>(
				                    static_cast<int>(outNode->p[3]))) },
				{ "pins",       std::move(pinList) },
				{ "approx",     std::move(approx) },
			};
		}

		// ── What the graph reaches for: textures, functions, switches, params ──
		std::vector<std::string> textures, functions;
		std::set<std::string>    paramNames;
		json switches = json::array();
		for (const HE::MatGraphNode& n : g.nodes)
		{
			HE::MatParamKind pk{};
			if (paramKindOfNode(n.type, pk)) paramNames.insert(effectiveParamName(n));
			switch (n.type)
			{
			case HE::MatNodeType::TextureSample:
			case HE::MatNodeType::NormalMapSample:
				if (!n.s.empty() &&
				    std::find(textures.begin(), textures.end(), n.s) == textures.end())
					textures.push_back(n.s);
				break;
			case HE::MatNodeType::FunctionCall:
				if (std::find(functions.begin(), functions.end(), n.s) == functions.end())
					functions.push_back(n.s);
				break;
			case HE::MatNodeType::StaticSwitch:
				switches.push_back(json{ { "name", n.s }, { "default", n.p[0] >= 0.5f },
				                         { "node", n.id } });
				break;
			default:
				break;
			}
		}
		// The baked slot order, when the asset carries one (a function does not):
		// that is the order heTexP0..3 are bound in, and what a client should name.
		out["textures"]  = m.textures.empty() ? json(textures) : json(m.textures);
		out["functions"] = functions;
		out["switches"]  = std::move(switches);
		if (!m.layers.empty()) out["layers"] = m.layers;
		if (static_cast<int>(textures.size()) > HE::kMatMaxGraphTextures)
			warnings.push_back("The graph samples " + std::to_string(textures.size()) +
			                   " distinct textures; only the first " +
			                   std::to_string(HE::kMatMaxGraphTextures) +
			                   " get a slot, the rest sample nothing.");
		if (static_cast<int>(paramNames.size()) > HE::kMatMaxParams)
			warnings.push_back("The graph declares " + std::to_string(paramNames.size()) +
			                   " distinct parameter names; only " +
			                   std::to_string(HE::kMatMaxParams) +
			                   " get a slot, the rest are baked as constants.");

		// ── The graph itself ──────────────────────────────────────────────────
		out["nodeCount"] = static_cast<int>(g.nodes.size());
		out["linkCount"] = static_cast<int>(g.links.size());
		if (!summaryOnly)
		{
			json nodes = json::array();
			for (const HE::MatGraphNode& n : g.nodes)
				nodes.push_back(nodeJson(g, n, blendMode, fns));
			json links = json::array();
			for (const HE::MatGraphLink& l : g.links)
				links.push_back(json{ { "srcNode", l.srcNode }, { "srcPin", l.srcPin },
				                      { "dstNode", l.dstNode }, { "dstPin", l.dstPin } });
			json comments = json::array();
			for (const HE::MatGraphComment& c : g.comments)
				comments.push_back(json{ { "id", c.id }, { "text", c.text },
				                         { "x", c.x }, { "y", c.y }, { "w", c.w }, { "h", c.h } });
			out["nodes"]    = std::move(nodes);
			out["links"]    = std::move(links);
			out["comments"] = std::move(comments);
		}
		else
		{
			// The function pins are only resolved while building node JSON; the
			// summary still has to know which calls point at nothing.
			for (const std::string& fnPath : functions) fns.get(fnPath);
		}
		for (const std::string& missing : fns.missing)
			warnings.push_back("Function '" + missing + "' could not be loaded; its "
			                   "call emits pin defaults.");
		out["warnings"] = std::move(warnings);
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
				"(Vector2), Param (Vector4), Param (Bool)). A material made by "
				"asset_create has no graph at all yet; material_create makes one whose "
				"PBR inputs are parameters from birth. For an existing graph, ask the "
				"user to open the material and expose what should be tunable.");

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

// ── Making a new material file: the gates the two creators share ─────────────
// `material_create` and `material_create_instance` ask the same questions
// before they write, in the same order, and a second copy of the list is how
// one of them quietly stops asking one of them.

// The gates that need no path: play mode and the project's own "does it author
// materials at all" setting. Empty-ok result = go on.
ToolResult createGate(const McpMaterialHooks& h)
{
	if (h.isPlaying && h.isPlaying())
		return ToolResult::fail("play_mode",
			"Play-in-editor is running. Creating an asset now would register it in "
			"the content manager the running session draws from, so it is refused "
			"rather than half-applied. Ask the user to stop play mode.");
	// The project's own gate, mirrored: a project without Advanced Shader
	// Effects has no Material row in the create menu and no "Create Material
	// Instance" either. MCP must not create what the menu will not offer.
	if (h.materialsAllowed && !h.materialsAllowed())
		return ToolResult::fail("invalid_payload",
			"This project does not author materials — the Content Browser offers "
			"neither the Material row nor 'Create Material Instance'. The gate is "
			"the project's Advanced Shader Effects setting.");
	return ToolResult::ok(json::object());
}

// Where the new file goes. `rawPath` already carries the caller's choice (or
// the sibling name material_create_instance picked); this appends the suffix,
// confines the path, and refuses the reserved namespace, an existing file and
// a peer's lock — every one of them before anything is written. On success the
// parent directory exists.
PathCheck newMaterialPath(ContentManager& cm, const McpMaterialHooks& h,
                          std::string rawPath, const char* what, const char* existsHint)
{
	PathCheck bad;
	if (std::filesystem::path(rawPath).extension().empty())
	{
		rawPath += ".hasset";
	}
	else if (std::filesystem::path(rawPath).extension() != ".hasset")
	{
		bad.failure = ToolResult::fail("invalid_path",
			"'" + rawPath + "' does not end in '.hasset'. A " + what + " is an asset "
			"like any other; the suffix is not negotiable, and a wrong one is refused "
			"rather than corrected.");
		return bad;
	}

	PathCheck dst = checkPath(cm, rawPath, /*mustExist=*/false, "path");
	if (!dst.ok) return dst;
	if (dst.engine) { bad.failure = failEngineReadOnly(dst.rel); return bad; }
	std::error_code ec;
	if (std::filesystem::exists(dst.abs, ec))
	{
		bad.failure = ToolResult::fail("already_exists",
			"'" + dst.rel + "' already exists. A client that named a path explicitly "
			"is more likely to have meant a different one than to have wanted it "
			"overwritten, so nothing was written. Choose another path" + std::string(existsHint) + ".");
		return bad;
	}
	if (h.lockedByOther && h.lockedByOther(dst.rel))
	{
		bad.failure = ToolResult::fail("locked_by_other",
			"Another participant in the collaboration session holds '" + dst.rel +
			"' right now. Wait until they let go, or choose another path.");
		return bad;
	}
	std::filesystem::create_directories(std::filesystem::path(dst.abs).parent_path(), ec);
	return dst;
}

// The editor's bookkeeping for a file that was not there before, and the
// report that lets the very next call be a material_set_param.
json reportCreated(ContentManager& cm, const McpMaterialHooks& h, const PathCheck& dst,
                   HE::UUID id)
{
	EditorAssetTypeCache::invalidate(dst.abs);
	if (h.onAssetAppeared) h.onAssetAppeared(dst.abs);
	// Published, not requested: nothing refers to a brand-new asset yet, so
	// there is nothing for the host to arbitrate.
	if (h.publishCreate) h.publishCreate(dst.rel, dst.abs);

	Mat m;
	m.rel = dst.rel;
	m.abs = dst.abs;
	m.id  = id;
	if (const MaterialAsset* a = cm.getMaterial(id)) copyOut(m, *a);
	json out = materialJson(m, h);
	out["created"] = true;
	return out;
}

// ── material_create: the templates ───────────────────────────────────────────
// Every PBR input a template wires is a PARAM node, not a constant, for the one
// reason that decides it: a ConstColor is unreachable over this interface and a
// ParamColor is a slot material_set_param can address. A template of constants
// would be a material the client cannot tune, only this time with a graph.
//
// The ParamFloat ranges are set here because nothing else sets them: without
// p[1] < p[2] the parameter is a number field in the editor instead of a
// slider, and material_set_param writes values, never ranges.

struct MaterialTemplate
{
	const char*      name;
	const char*      about;   // one line for the schema and for a refusal
	bool             lit;
	HE::MatBlendMode blend;
	HE::MatDomain    domain;
};

const std::array<MaterialTemplate, 5>& materialTemplates()
{
	static const std::array<MaterialTemplate, 5> k{ {
		{ "OpaquePBR",     "lit, opaque: BaseColor, Metallic, Specular, Roughness, Emissive",
		  true,  HE::MatBlendMode::Opaque,      HE::MatDomain::Surface },
		{ "Masked",        "lit, alpha-tested: the PBR set plus OpacityMask (cutoff 0.5)",
		  true,  HE::MatBlendMode::Masked,      HE::MatDomain::Surface },
		{ "Translucent",   "lit, alpha-blended: the PBR set plus Opacity",
		  true,  HE::MatBlendMode::Translucent, HE::MatDomain::Surface },
		{ "Unlit",         "unlit surface: a single Color, no lighting",
		  false, HE::MatBlendMode::Opaque,      HE::MatDomain::Surface },
		{ "UserInterface", "widget material: a single Color in the UI domain",
		  false, HE::MatBlendMode::Opaque,      HE::MatDomain::UserInterface },
	} };
	return k;
}

const MaterialTemplate* findTemplate(const std::string& name)
{
	for (const MaterialTemplate& t : materialTemplates())
		if (name == t.name) return &t;
	return nullptr;
}

std::string templateNameList()
{
	std::string s;
	for (const MaterialTemplate& t : materialTemplates())
	{
		if (!s.empty()) s += ", ";
		s += t.name;
	}
	return s;
}

// Inputs left in one column, Output right — the layout makeDefault() uses, so
// a human opening the file afterwards finds rows, not a knot.
HE::MaterialGraph templateGraph(const MaterialTemplate& t)
{
	HE::MaterialGraph g;
	const int out = g.addNode(HE::MatNodeType::Output, 460, 160);
	{
		HE::MatGraphNode* o = g.findNode(out);
		o->p[0] = t.lit ? 1.0f : 0.0f;
		o->p[1] = static_cast<float>(t.blend);
		// p[2] (mask cutoff 0.5) is what addNode already set.
		o->p[3] = static_cast<float>(t.domain);
	}

	float y = 40.0f;
	auto place = [&](HE::MatNodeType type, const char* name, std::array<float, 4> p,
	                 int outputPin) {
		const int id = g.addNode(type, 80, y);
		HE::MatGraphNode* n = g.findNode(id);
		// addNode names a fresh Param node "MyParam"/"MyColor" — the slot name a
		// client would then have to type, so it is overwritten, not appended to.
		n->s = name;
		for (int k = 0; k < 4; ++k) n->p[k] = p[static_cast<std::size_t>(k)];
		g.connect(id, 0, out, outputPin);
		y += 90.0f;
	};

	if (!t.lit)
	{
		// The unlit tail writes base + emissive without lighting; one colour is
		// the whole surface. Emissive stays unconnected (default 0).
		place(HE::MatNodeType::ParamColor, "Color", { 1.0f, 1.0f, 1.0f, 0.0f },
		      HE::kMatOutputBaseColorPin);
		return g;
	}

	place(HE::MatNodeType::ParamColor, "BaseColor", { 0.8f, 0.8f, 0.8f, 0.0f },
	      HE::kMatOutputBaseColorPin);
	place(HE::MatNodeType::ParamFloat, "Metallic",  { 0.0f, 0.0f, 1.0f, 0.0f },
	      HE::kMatOutputMetallicPin);
	place(HE::MatNodeType::ParamFloat, "Specular",  { 0.5f, 0.0f, 1.0f, 0.0f },
	      HE::kMatOutputSpecularPin);
	place(HE::MatNodeType::ParamFloat, "Roughness", { 0.5f, 0.0f, 1.0f, 0.0f },
	      HE::kMatOutputRoughnessPin);
	place(HE::MatNodeType::ParamColor, "Emissive",  { 0.0f, 0.0f, 0.0f, 0.0f },
	      HE::kMatOutputEmissivePin);
	// The blend-mode pin, under the name the Output node shows for it: the
	// codegen only reaches kMatOutputOpacityPin when the mode makes it mean
	// something, so on Opaque there is no such parameter at all.
	if (t.blend == HE::MatBlendMode::Masked)
		place(HE::MatNodeType::ParamFloat, "OpacityMask", { 1.0f, 0.0f, 1.0f, 0.0f },
		      HE::kMatOutputOpacityPin);
	else if (t.blend == HE::MatBlendMode::Translucent)
		place(HE::MatNodeType::ParamFloat, "Opacity", { 1.0f, 0.0f, 1.0f, 0.0f },
		      HE::kMatOutputOpacityPin);
	return g;
}

// ── material_create ──────────────────────────────────────────────────────────

void addCreate(McpToolRegistry& registry, ContentManager& content,
               const std::shared_ptr<McpMaterialHooks>& h)
{
	ContentManager* cm = &content;
	std::string templates;
	for (const MaterialTemplate& t : materialTemplates())
		templates += std::string("'") + t.name + "' (" + t.about + "); ";

	McpTool t;
	t.name        = "material_create";
	t.description =
		"Create a MASTER material with a graph of its own, built from a template, "
		"and save it. This is the material asset_create cannot make: that one "
		"writes an empty stub with no graph, no shader and no parameters, which "
		"material_set_param then refuses. Every PBR input the template wires is an "
		"exposed parameter (BaseColor, Metallic, Specular, Roughness, Emissive, and "
		"Opacity or OpacityMask by blend mode), so the result's parameter list is "
		"what the very next material_set_param call addresses. The shader is "
		"generated before the file is written, exactly as the Material Editor "
		"would; textures and further nodes are added in the editor. The new asset "
		"may not live under 'Engine/'.";
	t.mutates     = true;
	t.inputSchema = objectSchema(json{
		{ "path",     stringProp("Content-relative path for the new material, e.g. "
		                         "'Materials/Rock.hasset'. A missing '.hasset' suffix "
		                         "is appended; an existing file is refused.") },
		{ "template", stringProp(("Which starting graph, default 'OpaquePBR'. One of: " +
		                          templates).c_str()) },
	}, { "path" });
	t.handler = [cm, h](const json& args) -> ToolResult {
		if (cm->contentRoot().empty())
			return ToolResult::fail("no_project",
				"No project is open in the editor, so there is nowhere to create a "
				"material. Call scene_info first.");
		const ToolResult gate = createGate(*h);
		if (gate.isError) return gate;

		const std::string tplName = hasArg(args, "template") ? strArg(args, "template")
		                                                     : "OpaquePBR";
		const MaterialTemplate* tpl = findTemplate(tplName);
		if (!tpl)
			return ToolResult::fail("invalid_payload",
				"'" + tplName + "' is not a material template. The templates are: " +
				templateNameList() + ". Omit 'template' for OpaquePBR.");

		const std::string rawPath = strArg(args, "path");
		if (rawPath.empty())
			return ToolResult::fail("invalid_payload",
				"'path' is required: the content-relative path of the material to "
				"create, e.g. 'Materials/Rock.hasset'.");
		const PathCheck dst = newMaterialPath(*cm, *h, rawPath, "material", "");
		if (!dst.ok) return dst.failure;

		// The same road a material takes out of the Material Editor and out of
		// material_set_param: graph in, the engine's own codegen derives shader,
		// parameter block, blend mode and domain, then the content manager's
		// writer puts the file down. No second theory of what the file holds.
		MaterialAsset fresh;
		fresh.type          = HE::AssetType::Material;
		fresh.name          = std::filesystem::path(dst.abs).stem().string();
		fresh.path          = dst.rel;
		fresh.nodeGraphJson = HE::materialGraphToJson(templateGraph(*tpl));
		const HE::UUID id = cm->registerMaterial(std::move(fresh));
		if (id == HE::UUID{})
			return ToolResult::fail("failed",
				"The material could not be registered in the content manager. The "
				"editor log carries the reason.");
		// Loads whatever the graph calls (nothing, for a template) — the pointer
		// is taken after, never before.
		cm->regenerateMaterialFromGraph(id);
		MaterialAsset* written = cm->getMaterialMutable(id);
		if (!written || !cm->saveAsset(*written)) return failWrite(dst.rel);

		json out = reportCreated(*cm, *h, dst, id);
		out["template"] = tpl->name;
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
		const ToolResult gate = createGate(*h);
		if (gate.isError) return gate;

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

		const PathCheck dst = newMaterialPath(*cm, *h, rawPath, "material instance",
		                                      ", or omit 'path' for the next free sibling name");
		if (!dst.ok) return dst.failure;

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

		// The full report, so the very next call can be a material_set_param: the
		// parameters the instance inherited are in it.
		return ToolResult::ok(reportCreated(*cm, *h, dst, id));
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
	addGraphInfo(registry, content, h);
	addSetParam(registry, content, h);
	addCreate(registry, content, h);
	addCreateInstance(registry, content, h);
}

} // namespace HE::Ed
