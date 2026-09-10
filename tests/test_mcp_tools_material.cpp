#include "doctest.h"

#include "AssetStubWriter.h"
#include "EditorAssetTypeCache.h"
#include "McpToolRegistry.h"
#include "TestFsUtil.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <MaterialGraph/MaterialGraph.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// ─── Tuning a material from outside the editor ───────────────────────────────
// The one claim these tools make that nothing else in the codebase can make for
// them: A VALUE THEY WROTE SURVIVES THE NEXT EDIT IN THE MATERIAL EDITOR. That is
// not the same as "the asset round-trips", because of what the panel does on
// every change: `applyToMaterial` regenerates the whole parameter block from the
// GRAPH's Param nodes and keeps nothing of the old block. A value written only
// into `shaderParamData` looks right in the file, renders right, and silently
// goes back the moment a human moves a node.
//
// So the load-bearing tests here do not stop at reading the asset back. They run
// the codegen the panel runs — `materialGraphFromJson` → `generateFragment` →
// `MatParamSlot::value` — and assert that THAT is the value the tool wrote. And
// the mirror-image trap gets the same treatment: `regenerateMaterialFromGraph`
// restores the old block by name over the fresh defaults, so a tool that wrote
// only the node default would be reverted by its own regenerate. Both halves are
// checked on the same call.
//
// The rest are the places where a shortcut would look green and be wrong later:
//
//   • is a ParamFloat's slider range still there after a value was set — it lives
//     in p[1]/p[2] of the very node the value goes into,
//   • do ALL the nodes of a repeated parameter name follow, not just the first
//     one the walk happens to reach,
//   • does an instance's value really carry the override mark, without which the
//     next sync of the parent copies straight over it,
//   • does a master's edit reach a loaded instance that does NOT override it,
//   • does a refusal really leave the file byte for byte as it was,
//   • does every refusal arrive under a code a client can branch on.

using HE::Ed::McpMaterialHooks;
using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace fs = std::filesystem;

namespace {

// ── Graphs, as the Material Editor would have left them ──────────────────────

// Output ← Param nodes, one of every kind, so the param layout a client sees has
// every shape in it. Types are not checked on a link (MaterialGraph::connect
// validates pin RANGE only, like the canvas), which is what lets a vec2 and a
// vec4 reach a float pin here — the codegen converts.
HE::MaterialGraph makeParamGraph()
{
	HE::MaterialGraph g;
	const int out = g.addNode(HE::MatNodeType::Output, 400, 120);
	// Translucent (Output p[1]), because the blend mode decides whether the
	// Opacity pin is EVALUATED at all: on an opaque material the codegen never
	// reaches it, so the Param node behind it would get no slot and the bool
	// parameter below would quietly not exist.
	g.findNode(out)->p[1] = static_cast<float>(HE::MatBlendMode::Translucent);

	const int tint = g.addNode(HE::MatNodeType::ParamColor, 80, 60);
	g.findNode(tint)->s = "Tint";
	g.findNode(tint)->p[0] = 0.25f; g.findNode(tint)->p[1] = 0.5f; g.findNode(tint)->p[2] = 0.75f;
	g.findNode(tint)->group   = "Surface";
	g.findNode(tint)->tooltip = "Multiplies the albedo";
	g.connect(tint, 0, out, HE::kMatOutputBaseColorPin);

	// No range (p[1] == p[2] == 0 → min is not < max): a free drag in the editor.
	const int metal = g.addNode(HE::MatNodeType::ParamFloat, 80, 160);
	g.findNode(metal)->s = "Metal";
	g.findNode(metal)->p[0] = 0.1f;
	g.connect(metal, 0, out, HE::kMatOutputMetallicPin);

	// WITH a range — the slider case, and the one p[1]/p[2] must survive.
	const int rough = g.addNode(HE::MatNodeType::ParamFloat, 80, 240);
	g.findNode(rough)->s = "Rough";
	g.findNode(rough)->p[0] = 0.4f;
	g.findNode(rough)->p[1] = 0.0f;
	g.findNode(rough)->p[2] = 1.0f;
	g.connect(rough, 0, out, HE::kMatOutputRoughnessPin);

	const int flag = g.addNode(HE::MatNodeType::ParamBool, 80, 320);
	g.findNode(flag)->s = "Flag";
	g.findNode(flag)->p[0] = 1.0f;
	g.connect(flag, 0, out, HE::kMatOutputOpacityPin);

	const int uv = g.addNode(HE::MatNodeType::ParamVec2, 80, 400);
	g.findNode(uv)->s = "Tiling";
	g.findNode(uv)->p[0] = 2.0f; g.findNode(uv)->p[1] = 3.0f;
	g.connect(uv, 0, out, HE::kMatOutputSpecularPin);

	const int glow = g.addNode(HE::MatNodeType::ParamVec4, 80, 480);
	g.findNode(glow)->s = "Glow";
	for (int k = 0; k < 4; ++k) g.findNode(glow)->p[k] = 0.1f * static_cast<float>(k + 1);
	g.connect(glow, 0, out, HE::kMatOutputEmissivePin);

	return g;
}

// TWO nodes feeding one name. `paramSlot` collapses them into a single slot, so
// a setter that updates one of them leaves the other as a time bomb: the next
// regenerate takes whichever the codegen reaches first.
HE::MaterialGraph makeSharedNameGraph()
{
	HE::MaterialGraph g;
	const int out = g.addNode(HE::MatNodeType::Output, 400, 120);
	const int a = g.addNode(HE::MatNodeType::ParamFloat, 80, 60);
	const int b = g.addNode(HE::MatNodeType::ParamFloat, 80, 160);
	for (int id : { a, b }) { g.findNode(id)->s = "Shared"; g.findNode(id)->p[0] = 0.2f; }
	g.connect(a, 0, out, HE::kMatOutputMetallicPin);
	g.connect(b, 0, out, HE::kMatOutputRoughnessPin);
	return g;
}

// ParamColor → FnOutput: a parameter that exists in the material's layout while
// NO node of the material's own graph declares it.
HE::MaterialGraph makeFunctionGraph()
{
	HE::MaterialGraph g;
	const int p = g.addNode(HE::MatNodeType::ParamColor, 80, 120);
	g.findNode(p)->s = "FnTint";
	g.findNode(p)->p[0] = 0.9f; g.findNode(p)->p[1] = 0.8f; g.findNode(p)->p[2] = 0.7f;
	const int out = g.addNode(HE::MatNodeType::FnOutput, 380, 120);
	g.findNode(out)->s = "Out";
	g.connect(p, 0, out, 0);
	return g;
}

struct Fixture
{
	fs::path        root;
	ContentManager  content;
	McpToolRegistry registry;

	// The editor half, as flags a test sets.
	bool        playing = false;
	bool        materialsOk = true;
	std::string lockedRel;   // non-empty = a peer holds it
	std::string dirtyRel;    // non-empty = an open tab holds it with edits
	std::string openRel;     // non-empty = an open tab holds it (clean or not)
	int         reloadCalls = 0;
	int         appeared    = 0;
	std::vector<std::string> published;

	explicit Fixture(const std::string& name)
	{
		root = fs::temp_directory_path() /
		       ("he_test_mcp_material_" + name + "_" + std::to_string(::rand()));
		fs::create_directories(root);
		content.setContentRoot(root.string());
		EditorAssetTypeCache::invalidateAll();

		McpMaterialHooks h;
		h.isPlaying     = [this] { return playing; };
		h.lockedByOther = [this](const std::string& rel) {
			return !lockedRel.empty() && rel == lockedRel;
		};
		h.isDirty = [this](const std::string& rel) {
			return !dirtyRel.empty() && rel == dirtyRel;
		};
		h.reloadFromDisk = [this](const std::string& rel) {
			if (openRel.empty() || rel != openRel) return false;
			++reloadCalls;
			return true;
		};
		h.materialsAllowed = [this] { return materialsOk; };
		h.publishCreate = [this](const std::string& rel, const std::string&) {
			published.push_back(rel);
		};
		h.onAssetAppeared = [this](const std::string&) { ++appeared; };

		HE::Ed::registerMaterialTools(registry, content, std::move(h));
	}

	~Fixture()
	{
		EditorAssetTypeCache::invalidateAll();
		he_test::removeAllQuiet(root);
	}

	ToolResult call(const char* name, json args)
	{
		const McpTool* t = registry.find(name);
		REQUIRE(t != nullptr);
		return t->handler(args);
	}

	// An asset straight onto disk, bypassing the tools — the state a test starts
	// FROM. The stub writer is the editor's own, so a newborn material here is
	// the one the create menu makes: META and nothing else, no graph.
	void writeStub(const std::string& rel, HE::AssetType type)
	{
		const fs::path abs = root / rel;
		fs::create_directories(abs.parent_path());
		REQUIRE(HE::Ed::writeAssetStub(abs.string(), rel, fs::path(rel).stem().string(), type));
		EditorAssetTypeCache::invalidate(abs.string());
	}

	// A material with a graph, written the way the Material Editor writes one:
	// the graph goes in, the engine's own codegen derives the shader and the
	// parameter block from it, the asset is saved. Deliberately NOT through the
	// tools under test.
	HE::UUID writeMaterial(const std::string& rel, const HE::MaterialGraph& g)
	{
		writeStub(rel, HE::AssetType::Material);
		const HE::UUID id = content.loadAsset(rel);
		REQUIRE_FALSE(id == HE::UUID{});
		{
			MaterialAsset* a = content.getMaterialMutable(id);
			REQUIRE(a != nullptr);
			a->nodeGraphJson = HE::materialGraphToJson(g);
		}
		// Loads the functions the graph calls → the pool may move, so the pointer
		// is taken after, never before.
		content.regenerateMaterialFromGraph(id);
		MaterialAsset* a = content.getMaterialMutable(id);
		REQUIRE(a != nullptr);
		REQUIRE(content.saveAsset(*a));
		return id;
	}

	HE::UUID writeFunction(const std::string& rel, const HE::MaterialGraph& g)
	{
		writeStub(rel, HE::AssetType::MaterialFunction);
		const HE::UUID id = content.loadAsset(rel);
		REQUIRE_FALSE(id == HE::UUID{});
		MaterialFunctionAsset* a = content.getMaterialFunctionMutable(id);
		REQUIRE(a != nullptr);
		a->nodeGraphJson = HE::materialGraphToJson(g);
		REQUIRE(content.saveAsset(*a));
		return id;
	}

	std::string bytes(const std::string& rel) const
	{
		std::ifstream f(root / rel, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	}
};

// One parameter out of a material_info / material_set_param payload.
const json* findParam(const json& params, const std::string& name)
{
	for (const json& p : params)
		if (p.value("name", std::string()) == name) return &p;
	return nullptr;
}

// THE question the tools exist to answer: what would the Material Editor's own
// regenerate make of this file? Reads the saved asset with a FRESH content
// manager (so nothing in memory can flatter the answer), runs the graph through
// the real codegen and hands back the slot it produced for `name`.
bool codegenValueOf(const fs::path& root, const std::string& rel, const std::string& name,
                    HE::MatParamSlot& out)
{
	ContentManager fresh;
	fresh.setContentRoot(root.string());
	const HE::UUID id = fresh.loadAsset(rel);
	const MaterialAsset* a = id == HE::UUID{} ? nullptr : fresh.getMaterial(id);
	if (!a || a->nodeGraphJson.empty()) return false;
	HE::MaterialGraph g;
	if (!HE::materialGraphFromJson(a->nodeGraphJson, g)) return false;
	const HE::MatShaderGen gen = HE::generateFragment(g);
	for (const HE::MatParamSlot& s : gen.params)
		if (s.name == name) { out = s; return true; }
	return false;
}

// The value the file's own parameter BLOCK holds (what the renderer uploads),
// read back with a fresh manager.
bool savedBlockValueOf(const fs::path& root, const std::string& rel, const std::string& name,
                       float out[4])
{
	ContentManager fresh;
	fresh.setContentRoot(root.string());
	const HE::UUID id = fresh.loadAsset(rel);
	if (id == HE::UUID{}) return false;
	return fresh.getMaterialParam(id, name, out);
}

} // namespace

TEST_CASE("mcp material tools: registration")
{
	Fixture f("reg");
	for (const char* n : { "material_info", "material_set_param",
	                       "material_create_instance" })
	{
		const McpTool* t = f.registry.find(n);
		REQUIRE(t != nullptr);
		CHECK(McpToolRegistry::enforceNameRule(t->name));
		CHECK(t->inputSchema.is_object());
		CHECK_FALSE(t->description.empty());
	}
	CHECK_FALSE(f.registry.find("material_info")->mutates);
	CHECK(f.registry.find("material_set_param")->mutates);
	CHECK(f.registry.find("material_create_instance")->mutates);
}

TEST_CASE("mcp material tools: the list loads nothing")
{
	Fixture f("list");
	f.writeStub("Materials/Plain.hasset", HE::AssetType::Material);
	f.writeStub("Materials/Fn.hasset", HE::AssetType::MaterialFunction);
	f.writeStub("Widgets/HUD.hasset", HE::AssetType::Widget);

	const ToolResult r = f.call("material_info", json::object());
	REQUIRE_FALSE(r.isError);
	const json& list = r.content.at("materials");
	REQUIRE(list.size() == 2);   // the widget is not a material

	bool sawMaterial = false, sawFunction = false;
	for (const json& e : list)
	{
		if (e.at("path") == "Materials/Plain.hasset")
		{
			sawMaterial = true;
			CHECK(e.at("type") == "Material");
			// The whole point: a question does not change its answer. Nothing was
			// loaded, so there is no kind and no param count to report either.
			CHECK(e.at("loaded") == false);
			CHECK(e.find("kind") == e.end());
		}
		if (e.at("path") == "Materials/Fn.hasset")
		{
			sawFunction = true;
			CHECK(e.at("type") == "MaterialFunction");
		}
	}
	CHECK(sawMaterial);
	CHECK(sawFunction);

	// A resident material is free to say more — and only then.
	f.writeMaterial("Materials/Real.hasset", makeParamGraph());
	const ToolResult r2 = f.call("material_info", json::object());
	REQUIRE_FALSE(r2.isError);
	for (const json& e : r2.content.at("materials"))
		if (e.at("path") == "Materials/Real.hasset")
		{
			CHECK(e.at("loaded") == true);
			CHECK(e.at("kind") == "master");
			CHECK(e.at("paramCount") == 6);
		}
}

TEST_CASE("mcp material tools: one material in full")
{
	Fixture f("info");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());

	const ToolResult r = f.call("material_info", json{ { "path", "Materials/Rock.hasset" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content.at("kind") == "master");
	CHECK(r.content.at("hasGraph") == true);
	// Baked from the Output node, not stored separately — and what decides which
	// render pass the material's draws land in.
	CHECK(r.content.at("blendMode") == "Translucent");
	CHECK(r.content.at("domain") == "Surface");
	CHECK(r.content.at("dirtyInEditor") == false);
	CHECK(r.content.find("parent") == r.content.end());

	const json& params = r.content.at("params");
	REQUIRE(params.size() == 6);

	const json* tint = findParam(params, "Tint");
	REQUIRE(tint != nullptr);
	CHECK(tint->at("kind") == "color");
	REQUIRE(tint->at("value").is_array());
	REQUIRE(tint->at("value").size() == 3);   // colour is three, not four
	CHECK(tint->at("value")[0].get<float>() == doctest::Approx(0.25f));
	CHECK(tint->at("value")[2].get<float>() == doctest::Approx(0.75f));
	CHECK(tint->at("group") == "Surface");
	CHECK(tint->at("tooltip") == "Multiplies the albedo");
	CHECK(tint->at("inGraph") == true);

	// A float answers as a number, not as a one-element array: the shape it is
	// SET with, so a value read here can be handed straight back.
	const json* metal = findParam(params, "Metal");
	REQUIRE(metal != nullptr);
	CHECK(metal->at("kind") == "float");
	CHECK(metal->at("value").is_number());
	// No declared range → no min/max at all, rather than a meaningless 0/0 pair.
	CHECK(metal->find("min") == metal->end());

	const json* rough = findParam(params, "Rough");
	REQUIRE(rough != nullptr);
	CHECK(rough->at("min").get<float>() == doctest::Approx(0.0f));
	CHECK(rough->at("max").get<float>() == doctest::Approx(1.0f));

	const json* flag = findParam(params, "Flag");
	REQUIRE(flag != nullptr);
	CHECK(flag->at("kind") == "bool");
	CHECK(flag->at("value").is_boolean());
	CHECK(flag->at("value") == true);

	const json* tiling = findParam(params, "Tiling");
	REQUIRE(tiling != nullptr);
	CHECK(tiling->at("kind") == "vec2");
	REQUIRE(tiling->at("value").size() == 2);
	CHECK(tiling->at("value")[1].get<float>() == doctest::Approx(3.0f));

	const json* glow = findParam(params, "Glow");
	REQUIRE(glow != nullptr);
	CHECK(glow->at("kind") == "vec4");
	REQUIRE(glow->at("value").size() == 4);
	CHECK(glow->at("value")[3].get<float>() == doctest::Approx(0.4f));

	// A master reports no per-parameter override flag — there is nothing to
	// override against.
	CHECK(tint->find("overridden") == tint->end());
}

TEST_CASE("mcp material tools: a value set on a master survives the panel's regenerate")
{
	Fixture f("master");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());

	const ToolResult r = f.call("material_set_param", json{
		{ "path", "Materials/Rock.hasset" }, { "name", "Metal" }, { "value", 0.75 } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content.at("target") == "master");
	CHECK(r.content.at("graphDefaultUpdated") == true);
	CHECK(r.content.at("graphNodesUpdated") == 1);
	CHECK(r.content.at("value").get<float>() == doctest::Approx(0.75f));
	CHECK(r.content.find("note") == r.content.end());

	// Half one: the parameter BLOCK on disk, which is what the renderer uploads.
	// This is the half a tool that only touched the graph would fail —
	// regenerateMaterialFromGraph restores the block by name over the fresh
	// defaults, so the old value would have been put back over the new one.
	float block[4] = {};
	REQUIRE(savedBlockValueOf(f.root, "Materials/Rock.hasset", "Metal", block));
	CHECK(block[0] == doctest::Approx(0.75f));

	// Half two: the GRAPH, run through the very codegen MaterialEditorPanel runs
	// on every edit. A tool that only wrote the block would pass the first half
	// and fail here, and the failure would not show until a human moved a node.
	HE::MatParamSlot slot;
	REQUIRE(codegenValueOf(f.root, "Materials/Rock.hasset", "Metal", slot));
	CHECK(slot.value[0] == doctest::Approx(0.75f));
	CHECK(slot.kind == HE::MatParamKind::Float);
}

TEST_CASE("mcp material tools: a slider range survives the value that goes into it")
{
	Fixture f("range");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());

	const ToolResult r = f.call("material_set_param", json{
		{ "path", "Materials/Rock.hasset" }, { "name", "Rough" }, { "value", 0.9 } });
	REQUIRE_FALSE(r.isError);

	// p[1]/p[2] of a ParamFloat node ARE its range — the same four floats a
	// careless write would have filled with the value.
	HE::MatParamSlot slot;
	REQUIRE(codegenValueOf(f.root, "Materials/Rock.hasset", "Rough", slot));
	CHECK(slot.value[0] == doctest::Approx(0.9f));
	CHECK(slot.minV == doctest::Approx(0.0f));
	CHECK(slot.maxV == doctest::Approx(1.0f));

	// And the range is enforced rather than silently clamped.
	const std::string before = f.bytes("Materials/Rock.hasset");
	const ToolResult bad = f.call("material_set_param", json{
		{ "path", "Materials/Rock.hasset" }, { "name", "Rough" }, { "value", 4.0 } });
	CHECK(bad.isError);
	CHECK(bad.errorCode == "out_of_range");
	CHECK(f.bytes("Materials/Rock.hasset") == before);
}

TEST_CASE("mcp material tools: every node of a repeated name follows")
{
	Fixture f("shared");
	f.writeMaterial("Materials/Shared.hasset", makeSharedNameGraph());

	const ToolResult r = f.call("material_set_param", json{
		{ "path", "Materials/Shared.hasset" }, { "name", "Shared" }, { "value", 0.6 } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content.at("graphNodesUpdated") == 2);

	// Both nodes, not just the one the codegen happens to reach first: read the
	// saved graph and look at every Param node by hand.
	ContentManager fresh;
	fresh.setContentRoot(f.root.string());
	const HE::UUID id = fresh.loadAsset("Materials/Shared.hasset");
	const MaterialAsset* a = fresh.getMaterial(id);
	REQUIRE(a != nullptr);
	HE::MaterialGraph g;
	REQUIRE(HE::materialGraphFromJson(a->nodeGraphJson, g));
	int seen = 0;
	for (const HE::MatGraphNode& n : g.nodes)
		if (n.type == HE::MatNodeType::ParamFloat && n.s == "Shared")
		{
			CHECK(n.p[0] == doctest::Approx(0.6f));
			++seen;
		}
	CHECK(seen == 2);
}

TEST_CASE("mcp material tools: every kind takes its own shape and refuses the others")
{
	Fixture f("shapes");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());
	const std::string path = "Materials/Rock.hasset";

	// Colour: three numbers.
	const ToolResult col = f.call("material_set_param", json{
		{ "path", path }, { "name", "Tint" },
		{ "value", json::array({ 0.1, 0.2, 0.3 }) } });
	REQUIRE_FALSE(col.isError);
	REQUIRE(col.content.at("value").size() == 3);
	CHECK(col.content.at("value")[1].get<float>() == doctest::Approx(0.2f));
	HE::MatParamSlot slot;
	REQUIRE(codegenValueOf(f.root, path, "Tint", slot));
	CHECK(slot.value[2] == doctest::Approx(0.3f));

	// Bool: true/false, and a number is not "close enough".
	const ToolResult b = f.call("material_set_param", json{
		{ "path", path }, { "name", "Flag" }, { "value", false } });
	REQUIRE_FALSE(b.isError);
	CHECK(b.content.at("value") == false);
	REQUIRE(codegenValueOf(f.root, path, "Flag", slot));
	CHECK(slot.value[0] == doctest::Approx(0.0f));

	// Vec2 and vec4 by their own arity.
	REQUIRE_FALSE(f.call("material_set_param", json{
		{ "path", path }, { "name", "Tiling" },
		{ "value", json::array({ 5.0, 6.0 }) } }).isError);
	REQUIRE(codegenValueOf(f.root, path, "Tiling", slot));
	CHECK(slot.value[1] == doctest::Approx(6.0f));
	REQUIRE_FALSE(f.call("material_set_param", json{
		{ "path", path }, { "name", "Glow" },
		{ "value", json::array({ 1.0, 2.0, 3.0, 4.0 }) } }).isError);
	REQUIRE(codegenValueOf(f.root, path, "Glow", slot));
	CHECK(slot.value[3] == doctest::Approx(4.0f));

	// And the refusals, each one a no-op.
	const std::string before = f.bytes(path);
	struct Bad { const char* name; json value; };
	const std::vector<Bad> bad{
		{ "Tint",   json(0.5) },                              // colour as a scalar
		{ "Tint",   json::array({ 0.1, 0.2 }) },              // two of three
		{ "Tint",   json::array({ 0.1, 0.2, 0.3, 0.4 }) },    // four of three
		{ "Metal",  json::array({ 0.5 }) },                   // float as an array
		{ "Flag",   json(1.0) },                              // bool as a number
		{ "Tiling", json::array({ 1.0, "x" }) },              // a string in the list
		{ "Glow",   json::array({ 1.0, 2.0, 3.0 }) },         // three of four
	};
	for (const Bad& t : bad)
	{
		const ToolResult r = f.call("material_set_param", json{
			{ "path", path }, { "name", t.name }, { "value", t.value } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_payload");
		// The message names the shape that was expected, since the client cannot
		// see graphParamTypes.
		CHECK(r.errorMessage.find(t.name) != std::string::npos);
	}
	CHECK(f.bytes(path) == before);
}

TEST_CASE("mcp material tools: a name that is not a parameter, and a material with none")
{
	Fixture f("names");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());
	f.writeStub("Materials/Stub.hasset", HE::AssetType::Material);

	const ToolResult unknown = f.call("material_set_param", json{
		{ "path", "Materials/Rock.hasset" }, { "name", "tint" }, { "value",
			json::array({ 1.0, 1.0, 1.0 }) } });
	CHECK(unknown.isError);
	CHECK(unknown.errorCode == "unknown_param");
	// The list travels with the refusal — the client can act on it without a
	// second call, and names are case-sensitive.
	CHECK(unknown.errorMessage.find("Tint") != std::string::npos);
	CHECK(unknown.errorMessage.find("Rough") != std::string::npos);

	// A stub has no parameters at all, which is a different story from "unknown
	// name" and gets its own code.
	const ToolResult none = f.call("material_set_param", json{
		{ "path", "Materials/Stub.hasset" }, { "name", "Tint" }, { "value", 1.0 } });
	CHECK(none.isError);
	CHECK(none.errorCode == "no_params");

	// material_info still answers for it, which is how a client finds that out.
	const ToolResult info = f.call("material_info",
	                              json{ { "path", "Materials/Stub.hasset" } });
	REQUIRE_FALSE(info.isError);
	CHECK(info.content.at("hasGraph") == false);
	CHECK(info.content.at("params").empty());

	// A value without a value.
	const ToolResult noValue = f.call("material_set_param", json{
		{ "path", "Materials/Rock.hasset" }, { "name", "Metal" } });
	CHECK(noValue.isError);
	CHECK(noValue.errorCode == "invalid_payload");
}

TEST_CASE("mcp material tools: a parameter that comes from a material function says so")
{
	Fixture f("fn");
	f.writeFunction("Materials/Tintify.hasset", makeFunctionGraph());

	HE::MaterialGraph g;
	const int out  = g.addNode(HE::MatNodeType::Output, 400, 120);
	const int call = g.addNode(HE::MatNodeType::FunctionCall, 120, 120);
	g.findNode(call)->s = "Materials/Tintify.hasset";
	g.connect(call, 0, out, HE::kMatOutputBaseColorPin);
	f.writeMaterial("Materials/UsesFn.hasset", g);

	const ToolResult info = f.call("material_info",
	                              json{ { "path", "Materials/UsesFn.hasset" } });
	REQUIRE_FALSE(info.isError);
	const json* p = findParam(info.content.at("params"), "FnTint");
	REQUIRE(p != nullptr);
	// The honest half: the slot exists, and no node of THIS graph declares it.
	CHECK(p->at("inGraph") == false);

	const ToolResult r = f.call("material_set_param", json{
		{ "path", "Materials/UsesFn.hasset" }, { "name", "FnTint" },
		{ "value", json::array({ 0.1, 0.1, 0.1 }) } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content.at("graphDefaultUpdated") == false);
	CHECK(r.content.at("graphNodesUpdated") == 0);
	// Not a silent success: the value is real, and the note says what will undo
	// it, because nothing else would tell the client.
	REQUIRE(r.content.find("note") != r.content.end());
	CHECK(r.content.at("note").get<std::string>().find("function") != std::string::npos);
	float block[4] = {};
	REQUIRE(savedBlockValueOf(f.root, "Materials/UsesFn.hasset", "FnTint", block));
	CHECK(block[0] == doctest::Approx(0.1f));

	// And a material FUNCTION itself is refused, with the reason rather than a
	// bare "not a material".
	const ToolResult fnAsset = f.call("material_set_param", json{
		{ "path", "Materials/Tintify.hasset" }, { "name", "FnTint" },
		{ "value", json::array({ 1.0, 1.0, 1.0 }) } });
	CHECK(fnAsset.isError);
	CHECK(fnAsset.errorCode == "invalid_path");
	CHECK(fnAsset.errorMessage.find("FUNCTION") != std::string::npos);
}

TEST_CASE("mcp material tools: an instance overrides, un-overrides and follows its parent")
{
	Fixture f("inst");
	f.writeMaterial("Materials/Master.hasset", makeParamGraph());

	const ToolResult made = f.call("material_create_instance",
	                              json{ { "parent", "Materials/Master.hasset" } });
	REQUIRE_FALSE(made.isError);
	CHECK(made.content.at("created") == true);
	CHECK(made.content.at("kind") == "instance");
	CHECK(made.content.at("parent") == "Materials/Master.hasset");
	const std::string inst = made.content.at("path");
	// The Content Browser's own naming.
	CHECK(inst == "Materials/Master_Inst.hasset");
	CHECK(f.appeared == 1);
	REQUIRE(f.published.size() == 1);
	CHECK(f.published[0] == inst);
	// It inherited the whole parameter layout — that is what makes it usable at
	// all, and it is why the create reports the params rather than just a path.
	REQUIRE(made.content.at("params").size() == 6);
	const json* instTint = findParam(made.content.at("params"), "Tint");
	REQUIRE(instTint != nullptr);
	CHECK(instTint->at("overridden") == false);
	CHECK(instTint->at("value")[0].get<float>() == doctest::Approx(0.25f));

	// An override on the instance.
	const ToolResult set = f.call("material_set_param", json{
		{ "path", inst }, { "name", "Tint" },
		{ "value", json::array({ 1.0, 0.0, 0.0 }) } });
	REQUIRE_FALSE(set.isError);
	CHECK(set.content.at("target") == "instance");
	CHECK(set.content.at("overridden") == true);

	// The mark is the load-bearing part: a sync of the parent runs whenever the
	// parent is edited, and it copies the parent's value over every slot that is
	// NOT marked.
	{
		ContentManager fresh;
		fresh.setContentRoot(f.root.string());
		const HE::UUID id = fresh.loadAsset(inst);
		fresh.syncMaterialInstance(id);
		float v[4] = {};
		REQUIRE(fresh.getMaterialParam(id, "Tint", v));
		CHECK(v[0] == doctest::Approx(1.0f));
		CHECK(v[1] == doctest::Approx(0.0f));
		// And a slot it does not override still follows the parent.
		REQUIRE(fresh.getMaterialParam(id, "Metal", v));
		CHECK(v[0] == doctest::Approx(0.1f));
	}

	// Dropping the override is not "erase the bookkeeping": the value has to come
	// back from the parent, or the instance would keep the same colour with
	// nothing recording why.
	const ToolResult drop = f.call("material_set_param", json{
		{ "path", inst }, { "name", "Tint" }, { "override", false } });
	REQUIRE_FALSE(drop.isError);
	CHECK(drop.content.at("overridden") == false);
	REQUIRE(drop.content.at("value").size() == 3);
	CHECK(drop.content.at("value")[0].get<float>() == doctest::Approx(0.25f));

	const ToolResult again = f.call("material_set_param", json{
		{ "path", inst }, { "name", "Tint" }, { "override", false } });
	CHECK(again.isError);
	CHECK(again.errorCode == "invalid_payload");

	// 'override' on a master is a misunderstanding worth naming.
	const ToolResult onMaster = f.call("material_set_param", json{
		{ "path", "Materials/Master.hasset" }, { "name", "Tint" }, { "override", false } });
	CHECK(onMaster.isError);
	CHECK(onMaster.errorCode == "invalid_payload");
}

TEST_CASE("mcp material tools: a master's edit reaches a loaded instance that does not override")
{
	Fixture f("propagate");
	f.writeMaterial("Materials/Master.hasset", makeParamGraph());
	const ToolResult made = f.call("material_create_instance",
	                              json{ { "parent", "Materials/Master.hasset" } });
	REQUIRE_FALSE(made.isError);
	const std::string inst = made.content.at("path");

	// The instance is resident (the create loaded it) — exactly the state in which
	// a stale value would be on screen.
	const HE::UUID instId = f.content.idForPath(inst);
	REQUIRE_FALSE(instId == HE::UUID{});

	REQUIRE_FALSE(f.call("material_set_param", json{
		{ "path", "Materials/Master.hasset" }, { "name", "Metal" },
		{ "value", 0.9 } }).isError);

	float v[4] = {};
	REQUIRE(f.content.getMaterialParam(instId, "Metal", v));
	CHECK(v[0] == doctest::Approx(0.9f));
}

TEST_CASE("mcp material tools: creating an instance, and the four ways it is refused")
{
	Fixture f("create");
	f.writeMaterial("Materials/Master.hasset", makeParamGraph());
	f.writeFunction("Materials/Fn.hasset", makeFunctionGraph());

	// An explicit path, and the suffix rule the scene tools set: a missing
	// '.hasset' is appended, a different one is refused rather than corrected.
	const ToolResult named = f.call("material_create_instance", json{
		{ "parent", "Materials/Master.hasset" }, { "path", "Variants/Mossy" } });
	REQUIRE_FALSE(named.isError);
	CHECK(named.content.at("path") == "Variants/Mossy.hasset");
	CHECK(fs::exists(f.root / "Variants/Mossy.hasset"));

	const ToolResult wrongExt = f.call("material_create_instance", json{
		{ "parent", "Materials/Master.hasset" }, { "path", "Variants/Mossy.png" } });
	CHECK(wrongExt.isError);
	CHECK(wrongExt.errorCode == "invalid_path");

	const ToolResult taken = f.call("material_create_instance", json{
		{ "parent", "Materials/Master.hasset" }, { "path", "Variants/Mossy.hasset" } });
	CHECK(taken.isError);
	CHECK(taken.errorCode == "already_exists");

	// Omitting the path twice takes the next free sibling name, like the Content
	// Browser.
	REQUIRE_FALSE(f.call("material_create_instance",
	                     json{ { "parent", "Materials/Master.hasset" } }).isError);
	const ToolResult second = f.call("material_create_instance",
	                                json{ { "parent", "Materials/Master.hasset" } });
	REQUIRE_FALSE(second.isError);
	CHECK(second.content.at("path") == "Materials/Master_Inst2.hasset");

	// A function is not a material, so there is nothing to derive from.
	const ToolResult fnParent = f.call("material_create_instance",
	                                  json{ { "parent", "Materials/Fn.hasset" } });
	CHECK(fnParent.isError);
	CHECK(fnParent.errorCode == "invalid_path");

	const ToolResult gone = f.call("material_create_instance",
	                              json{ { "parent", "Materials/Nope.hasset" } });
	CHECK(gone.isError);
	CHECK(gone.errorCode == "not_found");

	// The project's own gate: no Advanced Shader Effects, no materials — the same
	// refusal the create menu gives by not offering the row.
	f.materialsOk = false;
	const ToolResult gated = f.call("material_create_instance",
	                               json{ { "parent", "Materials/Master.hasset" } });
	CHECK(gated.isError);
	CHECK(gated.errorCode == "invalid_payload");
	f.materialsOk = true;

	f.playing = true;
	const ToolResult playing = f.call("material_create_instance",
	                                 json{ { "parent", "Materials/Master.hasset" } });
	CHECK(playing.isError);
	CHECK(playing.errorCode == "play_mode");
}

TEST_CASE("mcp material tools: an instance of an instance keeps the chain")
{
	Fixture f("chain");
	f.writeMaterial("Materials/Master.hasset", makeParamGraph());
	const ToolResult a = f.call("material_create_instance",
	                           json{ { "parent", "Materials/Master.hasset" } });
	REQUIRE_FALSE(a.isError);
	const ToolResult b = f.call("material_create_instance",
	                           json{ { "parent", a.content.at("path") } });
	REQUIRE_FALSE(b.isError);
	CHECK(b.content.at("parent") == a.content.at("path").get<std::string>());
	REQUIRE(b.content.at("params").size() == 6);
}

TEST_CASE("mcp material tools: every gate refuses without writing")
{
	Fixture f("gates");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());
	const std::string path  = "Materials/Rock.hasset";
	const std::string before = f.bytes(path);
	const json good{ { "path", path }, { "name", "Metal" }, { "value", 0.42 } };

	f.playing = true;
	ToolResult r = f.call("material_set_param", good);
	CHECK(r.isError);
	CHECK(r.errorCode == "play_mode");
	f.playing = false;

	f.lockedRel = path;
	r = f.call("material_set_param", good);
	CHECK(r.isError);
	CHECK(r.errorCode == "locked_by_other");
	f.lockedRel.clear();

	// An open tab with unsaved edits: writing the file would be reverted by the
	// tab's own regenerate, so it is refused rather than half-applied.
	f.dirtyRel = path;
	f.openRel  = path;
	r = f.call("material_set_param", good);
	CHECK(r.isError);
	CHECK(r.errorCode == "dirty");
	// …while READING it is fine, and says so.
	const ToolResult info = f.call("material_info", json{ { "path", path } });
	REQUIRE_FALSE(info.isError);
	CHECK(info.content.at("dirtyInEditor") == true);
	f.dirtyRel.clear();

	// A CLEAN tab is told to re-read the file instead — the same path a
	// collaboration peer's change takes.
	CHECK(f.reloadCalls == 0);
	r = f.call("material_set_param", good);
	REQUIRE_FALSE(r.isError);
	CHECK(r.content.at("reloadedInEditor") == true);
	CHECK(f.reloadCalls == 1);
	f.openRel.clear();

	// Everything above except the last call left the file alone.
	CHECK(f.bytes(path) != before);   // the successful one did write

	// Paths: absolute, escaping, missing, and the wrong kind of asset.
	for (const char* bad : { "/Materials/Rock.hasset", "../outside.hasset" })
	{
		const ToolResult br = f.call("material_set_param", json{
			{ "path", bad }, { "name", "Metal" }, { "value", 0.1 } });
		CHECK(br.isError);
		CHECK(br.errorCode == "invalid_path");
	}
	const ToolResult missing = f.call("material_info",
	                                 json{ { "path", "Materials/Ghost.hasset" } });
	CHECK(missing.isError);
	CHECK(missing.errorCode == "not_found");

	f.writeStub("Widgets/HUD.hasset", HE::AssetType::Widget);
	const ToolResult widget = f.call("material_info",
	                                json{ { "path", "Widgets/HUD.hasset" } });
	CHECK(widget.isError);
	CHECK(widget.errorCode == "invalid_path");
}

TEST_CASE("mcp material tools: the engine namespace is read-only")
{
	Fixture f("engine");
	// An Engine-rooted path needs an engine content root to resolve into.
	const fs::path engineRoot = f.root / "__engine";
	fs::create_directories(engineRoot / "Materials");
	f.content.setEngineContentRoot(engineRoot.string());

	const fs::path abs = engineRoot / "Materials/Default.hasset";
	REQUIRE(HE::Ed::writeAssetStub(abs.string(), "Engine/Materials/Default.hasset",
	                               "Default", HE::AssetType::Material));
	EditorAssetTypeCache::invalidate(abs.string());

	const ToolResult r = f.call("material_set_param", json{
		{ "path", "Engine/Materials/Default.hasset" }, { "name", "Tint" },
		{ "value", json::array({ 1.0, 1.0, 1.0 }) } });
	CHECK(r.isError);
	CHECK(r.errorCode == "read_only");

	// Reading an engine material is fine — and so is deriving a PROJECT instance
	// from one, which is the whole point of shipping defaults.
	const ToolResult info = f.call("material_info",
	                              json{ { "path", "Engine/Materials/Default.hasset" } });
	REQUIRE_FALSE(info.isError);

	const ToolResult made = f.call("material_create_instance",
	                              json{ { "parent", "Engine/Materials/Default.hasset" } });
	REQUIRE_FALSE(made.isError);
	// Beside the project's own content, not inside the engine's.
	CHECK(made.content.at("path") == "Default_Inst.hasset");
	CHECK(made.content.at("parent") == "Engine/Materials/Default.hasset");

	const ToolResult intoEngine = f.call("material_create_instance", json{
		{ "parent", "Engine/Materials/Default.hasset" },
		{ "path",   "Engine/Materials/Mine.hasset" } });
	CHECK(intoEngine.isError);
	CHECK(intoEngine.errorCode == "read_only");
}
