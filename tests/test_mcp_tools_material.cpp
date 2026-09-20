#include "doctest.h"

#include "AssetStubWriter.h"
#include "EditorAssetTypeCache.h"
#include "McpBridge.h"
#include "McpToolRegistry.h"
#include "TestFsUtil.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <MaterialGraph/MaterialGraph.h>
// Linked into he_tests in every flavour; only the cross-compile of the
// material_create templates is gated on HE_TESTS_HAVE_SHADERC.
#include <material/MaterialShaderLibrary.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
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
	                       "material_create", "material_create_instance" })
	{
		const McpTool* t = f.registry.find(n);
		REQUIRE(t != nullptr);
		CHECK(McpToolRegistry::enforceNameRule(t->name));
		CHECK(t->inputSchema.is_object());
		CHECK_FALSE(t->description.empty());
	}
	CHECK_FALSE(f.registry.find("material_info")->mutates);
	CHECK(f.registry.find("material_set_param")->mutates);
	CHECK(f.registry.find("material_create")->mutates);
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

// ═══ material_create ═════════════════════════════════════════════════════════
// The chain this tool exists to mend: over MCP, make a material → tune it. With
// asset_create the second link is refused (`no_params`); with material_create
// the very next call can be a material_set_param, and the value it writes is
// what the Material Editor's own regenerate produces from the file.

TEST_CASE("mcp material tools: material_create mends the create-then-tune chain")
{
	Fixture f("mcreate");

	// The control: what asset_create leaves behind is a material the tools
	// cannot tune. That is the gap, stated as a test so its closing is visible.
	f.writeStub("Materials/Stub.hasset", HE::AssetType::Material);
	const ToolResult stubSet = f.call("material_set_param", json{
		{ "path", "Materials/Stub.hasset" }, { "name", "BaseColor" },
		{ "value", json::array({ 1.0, 0.0, 0.0 }) } });
	CHECK(stubSet.isError);
	CHECK(stubSet.errorCode == "no_params");

	const ToolResult made = f.call("material_create", json{ { "path", "Materials/Rock" } });
	REQUIRE_FALSE(made.isError);
	CHECK(made.content.at("path") == "Materials/Rock.hasset");
	CHECK(made.content.at("created") == true);
	CHECK(made.content.at("kind") == "master");
	CHECK(made.content.at("template") == "OpaquePBR");
	CHECK(made.content.at("hasGraph") == true);
	CHECK(made.content.at("blendMode") == "Opaque");
	CHECK(made.content.at("domain") == "Surface");
	CHECK(fs::exists(f.root / "Materials/Rock.hasset"));
	CHECK(f.appeared == 1);
	REQUIRE(f.published.size() == 1);
	CHECK(f.published[0] == "Materials/Rock.hasset");

	// The parameter list IS the template's PBR set — the names a client types
	// next — and every one of them is declared by the material's own graph, so
	// a value set on it survives the panel's regenerate.
	const json& params = made.content.at("params");
	REQUIRE(params.size() == 5);
	for (const char* n : { "BaseColor", "Metallic", "Specular", "Roughness", "Emissive" })
	{
		const json* p = findParam(params, n);
		REQUIRE_MESSAGE(p != nullptr, n);
		CHECK(p->at("inGraph") == true);
	}
	// Opaque: no Opacity parameter, because the codegen never reaches the pin.
	CHECK(findParam(params, "Opacity") == nullptr);
	CHECK(findParam(params, "OpacityMask") == nullptr);
	// The floats are sliders, not number fields — the range was authored.
	const json* rough = findParam(params, "Roughness");
	CHECK(rough->at("min") == 0.0);
	CHECK(rough->at("max") == 1.0);
	CHECK(rough->at("value") == doctest::Approx(0.5));

	// The chain: set a value on the result, read it back through the panel's
	// own codegen with a FRESH content manager.
	const ToolResult set = f.call("material_set_param", json{
		{ "path", "Materials/Rock.hasset" }, { "name", "BaseColor" },
		{ "value", json::array({ 0.1, 0.2, 0.3 }) } });
	REQUIRE_FALSE(set.isError);
	CHECK(set.content.at("graphDefaultUpdated") == true);
	HE::MatParamSlot slot;
	REQUIRE(codegenValueOf(f.root, "Materials/Rock.hasset", "BaseColor", slot));
	CHECK(slot.value[0] == doctest::Approx(0.1f));
	CHECK(slot.value[1] == doctest::Approx(0.2f));
	CHECK(slot.value[2] == doctest::Approx(0.3f));

	// And the file is a material the list form and the single form both read.
	const ToolResult info = f.call("material_info", json{ { "path", "Materials/Rock.hasset" } });
	REQUIRE_FALSE(info.isError);
	CHECK(info.content.at("params").size() == 5);
	// An instance of it inherits the five slots — the master is a real parent.
	const ToolResult inst = f.call("material_create_instance",
	                              json{ { "parent", "Materials/Rock.hasset" } });
	REQUIRE_FALSE(inst.isError);
	CHECK(inst.content.at("params").size() == 5);
}

TEST_CASE("mcp material tools: every template compiles and exposes its own parameters")
{
	Fixture f("templates");

	struct Expect { const char* tpl; const char* blend; const char* domain;
	                std::vector<std::string> params; };
	const std::vector<Expect> table{
		{ "OpaquePBR",     "Opaque",      "Surface",
		  { "BaseColor", "Metallic", "Specular", "Roughness", "Emissive" } },
		{ "Masked",        "Masked",      "Surface",
		  { "BaseColor", "Metallic", "Specular", "Roughness", "Emissive", "OpacityMask" } },
		{ "Translucent",   "Translucent", "Surface",
		  { "BaseColor", "Metallic", "Specular", "Roughness", "Emissive", "Opacity" } },
		{ "Unlit",         "Opaque",      "Surface",        { "Color" } },
		{ "UserInterface", "Opaque",      "User Interface", { "Color" } },
	};

	for (const Expect& e : table)
	{
		CAPTURE(e.tpl);
		const std::string rel = std::string("Materials/T_") + e.tpl + ".hasset";
		const ToolResult made = f.call("material_create",
		                              json{ { "path", rel }, { "template", e.tpl } });
		REQUIRE_FALSE(made.isError);
		CHECK(made.content.at("template") == e.tpl);
		CHECK(made.content.at("blendMode") == e.blend);
		CHECK(made.content.at("domain") == e.domain);
		const json& params = made.content.at("params");
		REQUIRE(params.size() == e.params.size());
		for (const std::string& n : e.params)
		{
			const json* p = findParam(params, n);
			REQUIRE_MESSAGE(p != nullptr, n);
			CHECK(p->at("inGraph") == true);
		}

		// What the file holds, read by a fresh manager and run through the real
		// codegen: a shader, and not the magenta one a broken graph yields.
		ContentManager fresh;
		fresh.setContentRoot(f.root.string());
		const HE::UUID id = fresh.loadAsset(rel);
		const MaterialAsset* a = id == HE::UUID{} ? nullptr : fresh.getMaterial(id);
		REQUIRE(a != nullptr);
		REQUIRE_FALSE(a->nodeGraphJson.empty());
		CHECK_FALSE(a->customShaderFragGlsl.empty());
		HE::MaterialGraph g;
		REQUIRE(HE::materialGraphFromJson(a->nodeGraphJson, g));
		const HE::MatShaderGen gen = HE::generateFragment(g);
		CHECK_FALSE(gen.glsl.empty());
		CHECK(gen.glsl.find("vec3(1.0, 0.0, 1.0)") == std::string::npos);
		CHECK(gen.glsl.find("no Output node") == std::string::npos);
		CHECK(gen.params.size() == e.params.size());
#if defined(HE_TESTS_HAVE_SHADERC)
		// The cross-compile the Material Editor runs inline: a template that
		// only generates but does not compile would render magenta on a real
		// backend, and no test above would notice.
		using B = HE::MaterialShaderLibrary::Backend;
		HE::MaterialShaderLibrary lib;
		const uint64_t hash = std::hash<std::string>{}(gen.glsl);
		const auto& msl = lib.fragment(hash, gen.glsl, B::Metal);
		CHECK_MESSAGE(msl.ok, e.tpl, ": MSL compile failed: ", msl.log);
		const auto& gl = lib.fragment(hash, gen.glsl, B::GLSL410);
		CHECK_MESSAGE(gl.ok, e.tpl, ": GLSL compile failed: ", gl.log);
#endif
	}

	// The default is OpaquePBR, and a name that is not a template is refused
	// with the list.
	const ToolResult unknown = f.call("material_create", json{
		{ "path", "Materials/Nope.hasset" }, { "template", "Glass" } });
	CHECK(unknown.isError);
	CHECK(unknown.errorCode == "invalid_payload");
	CHECK(unknown.errorMessage.find("OpaquePBR") != std::string::npos);
	CHECK_FALSE(fs::exists(f.root / "Materials/Nope.hasset"));
}

TEST_CASE("mcp material tools: material_create refuses without writing")
{
	Fixture f("mcreate_gates");
	const std::string rel = "Materials/New.hasset";
	const json good{ { "path", rel } };
	auto refused = [&](const char* code) {
		const ToolResult r = f.call("material_create", good);
		CHECK(r.isError);
		CHECK(r.errorCode == code);
		CHECK_FALSE(fs::exists(f.root / rel));
		CHECK(f.appeared == 0);
		CHECK(f.published.empty());
	};

	f.playing = true;   refused("play_mode");        f.playing = false;
	f.materialsOk = false; refused("invalid_payload"); f.materialsOk = true;
	f.lockedRel = rel;  refused("locked_by_other");  f.lockedRel.clear();

	// No path at all, the wrong suffix, escaping and absolute paths.
	ToolResult r = f.call("material_create", json::object());
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");
	r = f.call("material_create", json{ { "path", "Materials/New.png" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_path");
	for (const char* bad : { "/Materials/New.hasset", "../outside.hasset" })
	{
		r = f.call("material_create", json{ { "path", bad } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_path");
	}
	CHECK(f.appeared == 0);

	// An existing file is refused, byte for byte untouched.
	f.writeMaterial(rel, makeParamGraph());
	const std::string before = f.bytes(rel);
	r = f.call("material_create", good);
	CHECK(r.isError);
	CHECK(r.errorCode == "already_exists");
	CHECK(f.bytes(rel) == before);

	// The reserved namespace.
	const fs::path engineRoot = f.root / "__engine";
	fs::create_directories(engineRoot / "Materials");
	f.content.setEngineContentRoot(engineRoot.string());
	r = f.call("material_create", json{ { "path", "Engine/Materials/Mine.hasset" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "read_only");
	CHECK_FALSE(fs::exists(engineRoot / "Materials/Mine.hasset"));
}

// ═══ material_graph_info ═════════════════════════════════════════════════════
// The structure material_info throws away. What is worth proving here is that
// the answer is the CANVAS's answer (pin names from the same functions, the
// Output pin that depends on the blend mode), that every node type has an
// unambiguous name on the wire, and that the two things a material cannot
// answer for itself — an instance, a function — get a real answer too.

namespace {

const json* findNode(const json& nodes, int id)
{
	for (const json& n : nodes)
		if (n.at("id") == id) return &n;
	return nullptr;
}

const json* findPinNamed(const json& pins, const std::string& name)
{
	for (const json& p : pins)
		if (p.at("name") == name) return &p;
	return nullptr;
}

} // namespace

TEST_CASE("mcp material tools: graph_info registration")
{
	Fixture f("ginfo_reg");
	const McpTool* t = f.registry.find("material_graph_info");
	REQUIRE(t != nullptr);
	CHECK(McpToolRegistry::enforceNameRule(t->name));
	CHECK(t->inputSchema.is_object());
	CHECK_FALSE(t->description.empty());
	CHECK_FALSE(t->mutates);
}

TEST_CASE("mcp material tools: graph_info reports the param fixture as the canvas sees it")
{
	Fixture f("ginfo");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());

	const ToolResult r = f.call("material_graph_info", json{ { "path", "Materials/Rock.hasset" } });
	REQUIRE_FALSE(r.isError);
	const json& j = r.content;
	CHECK(j.at("kind") == "master");
	CHECK(j.at("hasGraph") == true);
	CHECK(j.at("graphVersion") == HE::kMatGraphVersion);
	REQUIRE(j.at("nodes").size() == 7);
	REQUIRE(j.at("links").size() == 6);
	CHECK(j.at("nodeCount") == 7);
	CHECK(j.at("linkCount") == 6);
	CHECK(j.at("warnings").empty());

	// The Output block: Translucent, so pin 5 is "Opacity" and present.
	const json& out = j.at("output");
	CHECK(out.at("lit") == true);
	CHECK(out.at("blendMode") == "Translucent");
	CHECK(out.at("domain") == "Surface");
	CHECK(out.at("maskCutoff") == doctest::Approx(0.5));
	const json& pins = out.at("pins");
	REQUIRE(pins.size() == 9);
	const json* opacity = findPinNamed(pins, "Opacity");
	REQUIRE(opacity != nullptr);
	CHECK(opacity->at("pin") == HE::kMatOutputOpacityPin);
	CHECK(opacity->at("connected") == true);
	CHECK(opacity->at("source").at("node") == 5);   // the ParamBool "Flag"
	CHECK(opacity->at("chain").get<std::string>().find("Param (Bool) #5 (Flag)") == 0);
	const json* normal = findPinNamed(pins, "Normal");
	REQUIRE(normal != nullptr);
	CHECK(normal->at("connected") == false);
	CHECK_FALSE(normal->contains("chain"));

	// The fold: Base Color is a Param node, so it reports the LIVE slot value
	// and says which slot.
	CHECK(out.at("approx").at("baseColorParam") == "Tint");
	const json& bc = out.at("approx").at("baseColor");
	CHECK(bc[0] == doctest::Approx(0.25));
	CHECK(bc[1] == doctest::Approx(0.5));
	CHECK(bc[2] == doctest::Approx(0.75));

	// A node, in full: enum name on the wire, display name beside it, the Param
	// metadata the panel shows, and the slider range only where it is one.
	const json* tint = findNode(j.at("nodes"), 2);
	REQUIRE(tint != nullptr);
	CHECK(tint->at("type") == "ParamColor");
	CHECK(tint->at("displayName") == "Param (Color)");
	CHECK(tint->at("category") == "Parameter");
	CHECK(tint->at("paramName") == "Tint");
	CHECK(tint->at("kind") == "color");
	CHECK(tint->at("group") == "Surface");
	CHECK(tint->at("tooltip") == "Multiplies the albedo");
	CHECK(tint->at("x") == doctest::Approx(80));
	REQUIRE(tint->at("p").size() == 3);
	CHECK(tint->at("p")[2] == doctest::Approx(0.75));
	REQUIRE(tint->at("outputs").size() == 1);
	CHECK(tint->at("outputs")[0].at("type") == "vec3");

	const json* rough = findNode(j.at("nodes"), 4);
	REQUIRE(rough != nullptr);
	CHECK(rough->at("type") == "ParamFloat");
	CHECK(rough->at("min") == doctest::Approx(0.0));
	CHECK(rough->at("max") == doctest::Approx(1.0));
	const json* metal = findNode(j.at("nodes"), 3);
	REQUIRE(metal != nullptr);
	CHECK_FALSE(metal->contains("min"));   // no range → no slider

	// The Output node's input list is the blend-mode-dependent one, and each
	// row says what feeds it.
	const json* outNode = findNode(j.at("nodes"), 1);
	REQUIRE(outNode != nullptr);
	CHECK(outNode->at("type") == "Output");
	REQUIRE(outNode->at("inputs").size() == 9);
	const json* baseIn = findPinNamed(outNode->at("inputs"), "Base Color");
	REQUIRE(baseIn != nullptr);
	CHECK(baseIn->at("pin") == HE::kMatOutputBaseColorPin);
	CHECK(baseIn->at("connected") == true);
	CHECK(baseIn->at("source") == json{ { "node", 2 }, { "pin", 0 } });

	// Links, verbatim.
	bool sawTintLink = false;
	for (const json& l : j.at("links"))
		if (l.at("srcNode") == 2 && l.at("dstNode") == 1 && l.at("dstPin") == HE::kMatOutputBaseColorPin)
			sawTintLink = true;
	CHECK(sawTintLink);

	// The summary form drops the graph and keeps the rest.
	const ToolResult s = f.call("material_graph_info",
	                           json{ { "path", "Materials/Rock.hasset" }, { "summary_only", true } });
	REQUIRE_FALSE(s.isError);
	CHECK_FALSE(s.content.contains("nodes"));
	CHECK_FALSE(s.content.contains("links"));
	CHECK(s.content.at("output").at("pins").size() == 9);
	CHECK(s.content.at("nodeCount") == 7);
}

TEST_CASE("mcp material tools: graph_info names the Output pin by the blend mode")
{
	Fixture f("ginfo_blend");
	// Opaque with a wire into the hidden Opacity pin: the pin is absent from
	// the list and the warning says the wire does nothing.
	HE::MaterialGraph g;
	const int out = g.addNode(HE::MatNodeType::Output, 400, 120);
	const int c = g.addNode(HE::MatNodeType::ConstFloat, 80, 120);
	g.connect(c, 0, out, HE::kMatOutputOpacityPin);
	f.writeMaterial("Materials/Opaque.hasset", g);
	const ToolResult r = f.call("material_graph_info", json{ { "path", "Materials/Opaque.hasset" } });
	REQUIRE_FALSE(r.isError);
	const json& pins = r.content.at("output").at("pins");
	CHECK(pins.size() == 8);
	CHECK(findPinNamed(pins, "Opacity") == nullptr);
	CHECK(findPinNamed(pins, "OpacityMask") == nullptr);
	REQUIRE(r.content.at("warnings").size() == 1);
	CHECK(r.content.at("warnings")[0].get<std::string>().find("Opaque") != std::string::npos);

	// Masked: the same pin index, under the other name.
	g.findNode(out)->p[1] = static_cast<float>(HE::MatBlendMode::Masked);
	f.writeMaterial("Materials/Masked.hasset", g);
	const ToolResult m = f.call("material_graph_info", json{ { "path", "Materials/Masked.hasset" } });
	REQUIRE_FALSE(m.isError);
	CHECK(m.content.at("output").at("blendMode") == "Masked");
	const json* mask = findPinNamed(m.content.at("output").at("pins"), "OpacityMask");
	REQUIRE(mask != nullptr);
	CHECK(mask->at("pin") == HE::kMatOutputOpacityPin);
	CHECK(mask->at("connected") == true);
	CHECK(m.content.at("warnings").empty());
}

TEST_CASE("mcp material tools: graph_info gives every node type an unambiguous name")
{
	Fixture f("ginfo_sweep");
	// One node of every registry type, wired to nothing — the same sweep the
	// codegen tests run, seen from the wire. A type without a name here is a
	// type material_add_node could never take.
	HE::MaterialGraph g;
	for (const HE::MatNodeDesc& d : HE::matNodeRegistry())
		g.addNode(d.type, 0, 0);
	f.writeMaterial("Materials/All.hasset", g);

	const ToolResult r = f.call("material_graph_info", json{ { "path", "Materials/All.hasset" } });
	REQUIRE_FALSE(r.isError);
	const json& nodes = r.content.at("nodes");
	REQUIRE(nodes.size() == HE::matNodeRegistry().size());
	std::set<std::string> seen;
	for (const json& n : nodes)
	{
		const std::string type = n.at("type");
		CAPTURE(n.at("displayName").get<std::string>());
		CHECK_FALSE(type.empty());
		CHECK(seen.insert(type).second);   // unique
		// The enum spelling: no spaces, no parentheses — the display name is
		// the other field.
		CHECK(type.find(' ') == std::string::npos);
		CHECK(type.find('(') == std::string::npos);
		CHECK(n.at("inputs").is_array());
		CHECK(n.at("outputs").is_array());
	}
	// The dynamic-pin nodes resolve their rows from their own strings.
	bool sawLayers = false;
	for (const json& n : nodes)
		if (n.at("type") == "LandscapeLayerBlend")
		{
			sawLayers = true;
			REQUIRE(n.at("layers").size() == 2);   // addNode's "Layer 1\nLayer 2"
			REQUIRE(n.at("inputs").size() == 2);
			CHECK(n.at("inputs")[1].at("name") == "Layer 2");
		}
	CHECK(sawLayers);
	// A FunctionCall with no path is a call to nothing, and the summary says so.
	bool sawMissing = false;
	for (const json& w : r.content.at("warnings"))
		if (w.get<std::string>().find("could not be loaded") != std::string::npos) sawMissing = true;
	CHECK(sawMissing);
}

TEST_CASE("mcp material tools: graph_info reads a function, an instance and a stub")
{
	Fixture f("ginfo_kinds");
	f.writeFunction("Materials/Fn.hasset", makeFunctionGraph());

	// A function: accepted here (material_info refuses it), with its interface.
	const ToolResult fn = f.call("material_graph_info", json{ { "path", "Materials/Fn.hasset" } });
	REQUIRE_FALSE(fn.isError);
	CHECK(fn.content.at("kind") == "function");
	CHECK_FALSE(fn.content.contains("output"));
	const json& iface = fn.content.at("interface");
	CHECK(iface.at("inputs").empty());
	REQUIRE(iface.at("outputs").size() == 1);
	CHECK(iface.at("outputs")[0].at("name") == "Out");
	CHECK(iface.at("outputs")[0].at("pin") == 0);
	REQUIRE(fn.content.at("nodes").size() == 2);
	bool sawFnOut = false;
	for (const json& n : fn.content.at("nodes"))
		if (n.at("type") == "FnOutput") sawFnOut = true;
	CHECK(sawFnOut);

	// A master that calls it: the call node's pins come from the function's
	// graph, and the function is listed.
	HE::MaterialGraph g;
	const int out  = g.addNode(HE::MatNodeType::Output, 400, 120);
	const int call = g.addNode(HE::MatNodeType::FunctionCall, 80, 120);
	g.findNode(call)->s = "Materials/Fn.hasset";
	g.connect(call, 0, out, HE::kMatOutputBaseColorPin);
	const int sw = g.addNode(HE::MatNodeType::StaticSwitch, 80, 300);
	g.findNode(sw)->s = "Glossy";
	f.writeMaterial("Materials/Caller.hasset", g);
	const ToolResult caller = f.call("material_graph_info", json{ { "path", "Materials/Caller.hasset" } });
	REQUIRE_FALSE(caller.isError);
	REQUIRE(caller.content.at("functions").size() == 1);
	CHECK(caller.content.at("functions")[0] == "Materials/Fn.hasset");
	const json* callNode = findNode(caller.content.at("nodes"), call);
	REQUIRE(callNode != nullptr);
	CHECK(callNode->at("type") == "FunctionCall");
	CHECK(callNode->at("function") == "Materials/Fn.hasset");
	CHECK_FALSE(callNode->contains("missing"));
	REQUIRE(callNode->at("outputs").size() == 1);
	CHECK(callNode->at("outputs")[0].at("name") == "Out");
	REQUIRE(caller.content.at("switches").size() == 1);
	CHECK(caller.content.at("switches")[0].at("name") == "Glossy");
	CHECK(caller.content.at("switches")[0].at("default") == true);
	CHECK(caller.content.at("warnings").empty());
	const json* base = findPinNamed(caller.content.at("output").at("pins"), "Base Color");
	REQUIRE(base != nullptr);
	// The chain speaks in display names (what the panel shows), the node list
	// in enum names (what an edit would type).
	CHECK(base->at("chain") == "Material Function #2 (Materials/Fn.hasset)");
	CHECK(callNode->at("displayName") == "Material Function");

	// An instance: the parent's graph under the instance's own overrides.
	const ToolResult made = f.call("material_create_instance",
	                              json{ { "parent", "Materials/Caller.hasset" } });
	REQUIRE_FALSE(made.isError);
	const ToolResult inst = f.call("material_graph_info",
	                              json{ { "path", made.content.at("path") } });
	REQUIRE_FALSE(inst.isError);
	CHECK(inst.content.at("kind") == "instance");
	CHECK(inst.content.at("parent") == "Materials/Caller.hasset");
	CHECK(inst.content.at("hasGraph") == true);
	CHECK(inst.content.at("nodes").size() == 3);
	CHECK(inst.content.at("switchOverrides").is_array());

	// A stub: no graph, no refusal, and the note says where one comes from.
	f.writeStub("Materials/Stub.hasset", HE::AssetType::Material);
	const ToolResult stub = f.call("material_graph_info", json{ { "path", "Materials/Stub.hasset" } });
	REQUIRE_FALSE(stub.isError);
	CHECK(stub.content.at("hasGraph") == false);
	CHECK(stub.content.at("nodes").empty());
	CHECK(stub.content.at("note").get<std::string>().find("material_create") != std::string::npos);

	// The same gates as every reader: a missing file, the wrong kind of asset.
	CHECK(f.call("material_graph_info", json{ { "path", "Materials/Ghost.hasset" } }).errorCode == "not_found");
	f.writeStub("Widgets/HUD.hasset", HE::AssetType::Widget);
	CHECK(f.call("material_graph_info", json{ { "path", "Widgets/HUD.hasset" } }).errorCode == "invalid_path");
}

TEST_CASE("mcp material tools: graph_info's chain skips reroutes and stops at fan-in")
{
	Fixture f("ginfo_chain");
	HE::MaterialGraph g;
	const int out = g.addNode(HE::MatNodeType::Output, 600, 120);
	const int mul = g.addNode(HE::MatNodeType::Multiply, 400, 120);
	const int rr  = g.addNode(HE::MatNodeType::Reroute, 500, 120);
	const int tex = g.addNode(HE::MatNodeType::TextureSample, 200, 60);
	g.findNode(tex)->s = "Textures/Rock.hasset";
	const int uv  = g.addNode(HE::MatNodeType::UV, 80, 60);
	const int col = g.addNode(HE::MatNodeType::ConstColor, 200, 200);
	g.connect(uv, 0, tex, 0);
	g.connect(tex, 0, mul, 0);
	g.connect(col, 0, mul, 1);
	g.connect(mul, 0, rr, 0);
	g.connect(rr, 0, out, HE::kMatOutputBaseColorPin);
	f.writeMaterial("Materials/Chain.hasset", g);

	const ToolResult r = f.call("material_graph_info", json{ { "path", "Materials/Chain.hasset" } });
	REQUIRE_FALSE(r.isError);
	const json* base = findPinNamed(r.content.at("output").at("pins"), "Base Color");
	REQUIRE(base != nullptr);
	// The source is the node BEHIND the reroute — what a connect would name.
	CHECK(base->at("source") == json{ { "node", mul }, { "pin", 0 } });
	// Multiply has two connected inputs: the chain names it and stops.
	CHECK(base->at("chain") == "Multiply #2 <- ...");
	// The texture slot order is the baked one.
	REQUIRE(r.content.at("textures").size() == 1);
	CHECK(r.content.at("textures")[0] == "Textures/Rock.hasset");
	// And the texture node says what it samples.
	const json* texNode = findNode(r.content.at("nodes"), tex);
	REQUIRE(texNode != nullptr);
	CHECK(texNode->at("texture") == "Textures/Rock.hasset");
	REQUIRE(texNode->at("inputs").size() == 1);
	CHECK(texNode->at("inputs")[0].at("source") == json{ { "node", uv }, { "pin", 0 } });
}

// ═══ material_node_types / material_add_node / material_remove_node ══════════
// The write half of the graph. What these tests have to prove that the readers
// above do not: that a node ADDED over the wire is the node the Material Editor
// would have added (same defaults, same pins), that a node REMOVED takes its
// links and its parameter slot with it all the way through the regenerate, and
// that every refusal — including the new `no_graph` and the function refusal —
// leaves the file byte for byte as it was.

namespace {

const json* nodeOfType(const json& nodes, const std::string& type)
{
	for (const json& n : nodes)
		if (n.at("type") == type) return &n;
	return nullptr;
}

const json* nodeById(const json& nodes, int id)
{
	for (const json& n : nodes)
		if (n.at("id") == id) return &n;
	return nullptr;
}

// The graph as the FILE holds it, read with a fresh manager.
bool savedGraphOf(const fs::path& root, const std::string& rel, HE::MaterialGraph& out)
{
	ContentManager fresh;
	fresh.setContentRoot(root.string());
	const HE::UUID id = fresh.loadAsset(rel);
	const MaterialAsset* a = id == HE::UUID{} ? nullptr : fresh.getMaterial(id);
	if (!a || a->nodeGraphJson.empty()) return false;
	return HE::materialGraphFromJson(a->nodeGraphJson, out);
}

} // namespace

TEST_CASE("mcp material tools: graph editors registration")
{
	Fixture f("gedit_reg");
	for (const char* n : { "material_node_types", "material_add_node", "material_remove_node" })
	{
		const McpTool* t = f.registry.find(n);
		REQUIRE(t != nullptr);
		CHECK(McpToolRegistry::enforceNameRule(t->name));
		CHECK(t->inputSchema.is_object());
		CHECK_FALSE(t->description.empty());
	}
	CHECK_FALSE(f.registry.find("material_node_types")->mutates);
	CHECK(f.registry.find("material_add_node")->mutates);
	CHECK(f.registry.find("material_remove_node")->mutates);
}

TEST_CASE("mcp material tools: node_types is the registry minus Output, with the editor's defaults")
{
	Fixture f("ntypes");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());
	f.writeFunction("Materials/Fn.hasset", makeFunctionGraph());
	f.writeStub("Materials/EmptyFn.hasset", HE::AssetType::MaterialFunction);

	// ── No path: the whole library, function-only types flagged ──────────────
	const ToolResult all = f.call("material_node_types", json::object());
	REQUIRE_FALSE(all.isError);
	const json& types = all.content.at("nodeTypes");
	REQUIRE(types.size() == HE::matNodeRegistry().size() - 1);   // every type but Output
	std::set<std::string> seen;
	for (const json& t : types)
	{
		const std::string type = t.at("type");
		CHECK(seen.insert(type).second);
		CHECK(type != "Output");
		CHECK(t.at("inputs").is_array());
		CHECK(t.at("outputs").is_array());
		CHECK(t.at("defaults").at("p").size() == 4);
		// The spelling material_add_node takes is the one graph_info reports.
		CHECK(type.find(' ') == std::string::npos);
	}
	const json* fnIn = nodeOfType(types, "FnInput");
	REQUIRE(fnIn != nullptr);
	CHECK(fnIn->at("functionOnly") == true);
	CHECK(nodeOfType(types, "Multiply")->contains("functionOnly") == false);
	// The defaults are MaterialGraph::addNode's, not a restatement.
	const json* pf = nodeOfType(types, "ParamFloat");
	REQUIRE(pf != nullptr);
	CHECK(pf->at("defaults").at("s") == "MyParam");
	CHECK(pf->at("defaults").at("p")[0] == 1.0f);
	CHECK(pf->at("paramKind") == "float");
	CHECK(pf->at("paramCount") == 1);
	const json* tex = nodeOfType(types, "TextureSample");
	REQUIRE(tex != nullptr);
	CHECK(tex->at("inputs")[0].at("name") == "UV");
	CHECK(tex->at("outputs").size() == 2);
	CHECK(tex->contains("requires"));
	CHECK(nodeOfType(types, "FunctionCall")->at("dynamicPins") == true);

	// ── The functions a FunctionCall can call, with their interface ──────────
	const json& fns = all.content.at("functions");
	REQUIRE(fns.size() == 2);
	CHECK(all.content.at("functionsTruncated") == false);
	const json* fn = nullptr;
	const json* empty = nullptr;
	for (const json& e : fns)
	{
		if (e.at("path") == "Materials/Fn.hasset")      fn = &e;
		if (e.at("path") == "Materials/EmptyFn.hasset") empty = &e;
	}
	REQUIRE(fn != nullptr);
	REQUIRE(empty != nullptr);
	CHECK(fn->at("loadable") == true);
	CHECK(fn->at("inputs").size() == 0);          // makeFunctionGraph has no FnInput
	REQUIRE(fn->at("outputs").size() == 1);
	CHECK(fn->at("outputs")[0].at("name") == "Out");
	CHECK(empty->at("loadable") == false);        // a stub: no graph, a call emits defaults

	// ── With a master's path: the function interface is not offered ──────────
	const ToolResult forMat = f.call("material_node_types", json{ { "path", "Materials/Rock.hasset" } });
	REQUIRE_FALSE(forMat.isError);
	CHECK(forMat.content.at("scope") == "material");
	CHECK(forMat.content.at("kind") == "master");
	CHECK(nodeOfType(forMat.content.at("nodeTypes"), "FnInput") == nullptr);
	CHECK(nodeOfType(forMat.content.at("nodeTypes"), "FnOutput") == nullptr);
	CHECK(nodeOfType(forMat.content.at("nodeTypes"), "Multiply") != nullptr);

	// ── With a function's path: it is, and the function cannot call itself ───
	const ToolResult forFn = f.call("material_node_types", json{ { "path", "Materials/Fn.hasset" } });
	REQUIRE_FALSE(forFn.isError);
	CHECK(forFn.content.at("scope") == "function");
	CHECK(nodeOfType(forFn.content.at("nodeTypes"), "FnInput") != nullptr);
	for (const json& e : forFn.content.at("functions"))
		CHECK(e.at("path") != "Materials/Fn.hasset");
	CHECK(forFn.content.at("functions").size() == 1);

	// ── query + limit ────────────────────────────────────────────────────────
	const ToolResult q = f.call("material_node_types", json{ { "query", "param" } });
	REQUIRE_FALSE(q.isError);
	CHECK(q.content.at("nodeTypes").size() >= 5);   // the five Param kinds
	for (const json& t : q.content.at("nodeTypes"))
	{
		const std::string hay = t.at("type").get<std::string>() + " " +
		                        t.at("displayName").get<std::string>() + " " +
		                        t.at("category").get<std::string>();
		std::string low = hay;
		std::transform(low.begin(), low.end(), low.begin(), ::tolower);
		CHECK(low.find("param") != std::string::npos);
	}
	CHECK(q.content.at("functions").empty());   // no function path contains "param"
	const ToolResult lim = f.call("material_node_types", json{ { "limit", 1 } });
	REQUIRE_FALSE(lim.isError);
	CHECK(lim.content.at("functions").size() == 1);
	CHECK(lim.content.at("functionsTruncated") == true);
	CHECK(lim.content.at("nodeTypes").size() == types.size());   // never cut

	// A path that is not a material is the same refusal the readers give.
	f.writeStub("Widgets/HUD.hasset", HE::AssetType::Widget);
	const ToolResult widget = f.call("material_node_types", json{ { "path", "Widgets/HUD.hasset" } });
	CHECK(widget.isError);
	CHECK(widget.errorCode == "invalid_path");
}

TEST_CASE("mcp material tools: every listed type goes through add_node, and the file has it")
{
	Fixture f("add_sweep");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());
	f.writeFunction("Materials/Fn.hasset", makeFunctionGraph());

	const ToolResult cat = f.call("material_node_types", json{ { "path", "Materials/Rock.hasset" } });
	REQUIRE_FALSE(cat.isError);
	std::set<int> ids;
	for (const json& t : cat.content.at("nodeTypes"))
	{
		json args{ { "path", "Materials/Rock.hasset" }, { "type", t.at("type") } };
		// The one type whose payload is required.
		if (t.at("type") == "FunctionCall") args["s"] = "Materials/Fn.hasset";
		const ToolResult r = f.call("material_add_node", args);
		CAPTURE(t.at("type").get<std::string>());
		REQUIRE_FALSE(r.isError);
		const json& node = r.content.at("node");
		CHECK(node.at("type") == t.at("type"));
		CHECK(node.at("displayName") == t.at("displayName"));
		CHECK(ids.insert(node.at("id").get<int>()).second);   // fresh id every time
		CHECK(r.content.at("reloadedInEditor") == false);      // no tab in a test
		// The pins the answer reports are the pins the catalogue promised —
		// except where the catalogue said they come from the payload.
		if (!t.contains("dynamicPins"))
		{
			CHECK(node.at("inputs").size() == t.at("inputs").size());
			CHECK(node.at("outputs").size() == t.at("outputs").size());
		}
		else if (t.at("type") == "LandscapeLayerBlend")
			CHECK(node.at("inputs").size() == 2);   // addNode's "Layer 1\nLayer 2"
	}
	// A FunctionCall's pins are the function's interface.
	const ToolResult info = f.call("material_graph_info", json{ { "path", "Materials/Rock.hasset" } });
	REQUIRE_FALSE(info.isError);
	const json* call = nodeOfType(info.content.at("nodes"), "FunctionCall");
	REQUIRE(call != nullptr);
	CHECK(call->at("function") == "Materials/Fn.hasset");
	CHECK(call->at("inputs").size() == 0);
	REQUIRE(call->at("outputs").size() == 1);
	CHECK(call->at("outputs")[0].at("name") == "Out");

	// Every one of them is in the FILE, under the id the answer gave.
	HE::MaterialGraph saved;
	REQUIRE(savedGraphOf(f.root, "Materials/Rock.hasset", saved));
	for (int id : ids) CHECK(saved.findNode(id) != nullptr);
	CHECK(saved.nodes.size() == makeParamGraph().nodes.size() + ids.size());

	// The display name is taken too — it is what the file stores.
	const ToolResult byDisplay = f.call("material_add_node", json{
		{ "path", "Materials/Rock.hasset" }, { "type", "Texture Sample" } });
	REQUIRE_FALSE(byDisplay.isError);
	CHECK(byDisplay.content.at("node").at("type") == "TextureSample");
}

TEST_CASE("mcp material tools: add_node takes the payloads and refuses the wrong ones")
{
	Fixture f("add_payload");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());
	f.writeFunction("Materials/Fn.hasset", makeFunctionGraph());
	f.writeStub("Textures/Rock.hasset", HE::AssetType::Texture);
	f.writeStub("Widgets/HUD.hasset", HE::AssetType::Widget);
	const std::string path = "Materials/Rock.hasset";

	// ── A ParamFloat with everything: value, range, group, tooltip, position ─
	const ToolResult pf = f.call("material_add_node", json{
		{ "path", path }, { "type", "ParamFloat" }, { "s", "Shine" },
		{ "p", json::array({ 0.3 }) }, { "min", 0.0 }, { "max", 2.0 },
		{ "group", "Surface" }, { "tooltip", "How shiny" },
		{ "position", json::array({ 120.0, 640.0 }) } });
	REQUIRE_FALSE(pf.isError);
	const json& node = pf.content.at("node");
	CHECK(node.at("paramName") == "Shine");
	CHECK(node.at("p")[0] == 0.3f);
	CHECK(node.at("min") == 0.0f);
	CHECK(node.at("max") == 2.0f);
	CHECK(node.at("group") == "Surface");
	CHECK(node.at("tooltip") == "How shiny");
	CHECK(node.at("x") == 120.0f);
	CHECK(node.at("y") == 640.0f);
	// Not wired towards Output: the codegen never reaches it, so no slot — and
	// the answer says so instead of claiming the parameter is tunable.
	CHECK(pf.content.at("parameter").at("name") == "Shine");
	CHECK(pf.content.at("parameter").at("hasSlot") == false);
	CHECK(pf.content.contains("note"));
	HE::MatParamSlot slot;
	CHECK_FALSE(codegenValueOf(f.root, path, "Shine", slot));
	const ToolResult info = f.call("material_info", json{ { "path", path } });
	REQUIRE_FALSE(info.isError);
	CHECK(findParam(info.content.at("params"), "Shine") == nullptr);
	// …but the node, with its range, is in the file for the human to wire.
	HE::MaterialGraph saved;
	REQUIRE(savedGraphOf(f.root, path, saved));
	const HE::MatGraphNode* sn = saved.findNode(node.at("id").get<int>());
	REQUIRE(sn != nullptr);
	CHECK(sn->s == "Shine");
	CHECK(sn->p[1] == 0.0f);
	CHECK(sn->p[2] == 2.0f);
	CHECK(sn->group == "Surface");

	// ── A texture sample bound to a texture, and to the mesh's own ───────────
	const ToolResult tex = f.call("material_add_node", json{
		{ "path", path }, { "type", "TextureSample" }, { "s", "Textures/Rock.hasset" } });
	REQUIRE_FALSE(tex.isError);
	CHECK(tex.content.at("node").at("texture") == "Textures/Rock.hasset");
	const ToolResult meshTex = f.call("material_add_node", json{
		{ "path", path }, { "type", "TextureSample" } });
	REQUIRE_FALSE(meshTex.isError);
	CHECK(meshTex.content.at("node").contains("s") == false);

	// ── The refusals, each under its code, none of them writing ──────────────
	const std::string before = f.bytes(path);
	auto refused = [&](json args, const char* code) {
		const ToolResult r = f.call("material_add_node", args);
		CAPTURE(args.dump());
		CHECK(r.isError);
		CHECK(r.errorCode == code);
		CHECK(f.bytes(path) == before);
	};
	refused(json{ { "path", path }, { "type", "texture sample" } },        "invalid_payload");
	refused(json{ { "path", path } },                                       "invalid_payload");
	refused(json{ { "path", path }, { "type", "Output" } },                "refused_by_policy");
	refused(json{ { "path", path }, { "type", "FnInput" } },               "refused_by_policy");
	refused(json{ { "path", path }, { "type", "FnOutput" } },              "refused_by_policy");
	refused(json{ { "path", path }, { "type", "FunctionCall" } },          "invalid_payload");
	refused(json{ { "path", path }, { "type", "FunctionCall" }, { "s", path } }, "invalid_path");
	refused(json{ { "path", path }, { "type", "FunctionCall" },
	              { "s", "Materials/Ghost.hasset" } },                      "not_found");
	refused(json{ { "path", path }, { "type", "TextureSample" },
	              { "s", "Widgets/HUD.hasset" } },                          "invalid_path");
	refused(json{ { "path", path }, { "type", "TextureSample" },
	              { "s", "Textures/Ghost.hasset" } },                       "not_found");
	refused(json{ { "path", path }, { "type", "Add" }, { "p", json::array({ 1.0 }) } },
	        "invalid_payload");   // carries no values
	refused(json{ { "path", path }, { "type", "ConstFloat" }, { "p", json::array() } },
	        "invalid_payload");
	refused(json{ { "path", path }, { "type", "ConstFloat" }, { "p", json::array({ 1, 2, 3, 4, 5 }) } },
	        "invalid_payload");
	refused(json{ { "path", path }, { "type", "ConstFloat" }, { "p", "one" } },
	        "invalid_payload");
	refused(json{ { "path", path }, { "type", "ParamColor" }, { "min", 0.0 }, { "max", 1.0 } },
	        "invalid_payload");   // no range on a colour
	refused(json{ { "path", path }, { "type", "ParamFloat" }, { "min", 1.0 }, { "max", 1.0 } },
	        "invalid_payload");   // min < max
	refused(json{ { "path", path }, { "type", "ParamFloat" }, { "min", 0.0 } },
	        "invalid_payload");   // both or neither
	refused(json{ { "path", path }, { "type", "Multiply" }, { "group", "Math" } },
	        "invalid_payload");   // metadata on a non-parameter
	refused(json{ { "path", path }, { "type", "Multiply" }, { "position", json::array({ 1.0 }) } },
	        "invalid_payload");
}

TEST_CASE("mcp material tools: remove_node takes the links and the slot with it")
{
	Fixture f("remove");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());
	const std::string path = "Materials/Rock.hasset";
	// A loaded instance that does NOT override Metal: after the removal the
	// slot must be gone from it too, the same road a master's value takes.
	const ToolResult inst = f.call("material_create_instance", json{
		{ "parent", path }, { "path", "Materials/Rock_Inst.hasset" } });
	REQUIRE_FALSE(inst.isError);
	REQUIRE(findParam(inst.content.at("params"), "Metal") != nullptr);

	const ToolResult before = f.call("material_graph_info", json{ { "path", path } });
	REQUIRE_FALSE(before.isError);
	const json* metal = nullptr;
	int outId = -1;
	for (const json& n : before.content.at("nodes"))
	{
		if (n.at("type") == "ParamFloat" && n.at("paramName") == "Metal") metal = &n;
		if (n.at("type") == "Output") outId = n.at("id");
	}
	REQUIRE(metal != nullptr);
	REQUIRE(outId > 0);
	const int metalId = metal->at("id");

	const ToolResult r = f.call("material_remove_node", json{ { "path", path }, { "id", metalId } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content.at("removed") == metalId);
	CHECK(r.content.at("removedType") == "ParamFloat");
	REQUIRE(r.content.at("removedLinks").size() == 1);
	CHECK(r.content.at("removedLinks")[0] == json{
		{ "srcNode", metalId }, { "srcPin", 0 },
		{ "dstNode", outId },   { "dstPin", HE::kMatOutputMetallicPin } });
	CHECK(r.content.at("parameter").at("name") == "Metal");
	CHECK(r.content.at("parameter").at("slotRemoved") == true);
	CHECK(r.content.at("nodeCount") == static_cast<int>(makeParamGraph().nodes.size()) - 1);
	CHECK(r.content.at("linkCount") == static_cast<int>(makeParamGraph().links.size()) - 1);

	// The slot is gone from the file, from the codegen, from the canvas' view of
	// the Output pin and from the loaded instance.
	const ToolResult info = f.call("material_info", json{ { "path", path } });
	REQUIRE_FALSE(info.isError);
	CHECK(findParam(info.content.at("params"), "Metal") == nullptr);
	CHECK(findParam(info.content.at("params"), "Rough") != nullptr);
	HE::MatParamSlot slot;
	CHECK_FALSE(codegenValueOf(f.root, path, "Metal", slot));
	CHECK(codegenValueOf(f.root, path, "Rough", slot));
	const ToolResult after = f.call("material_graph_info", json{ { "path", path } });
	REQUIRE_FALSE(after.isError);
	CHECK(nodeById(after.content.at("nodes"), metalId) == nullptr);
	const json* out = nodeById(after.content.at("nodes"), outId);
	REQUIRE(out != nullptr);
	bool sawMetallicPin = false;
	for (const json& pin : out->at("inputs"))
		if (pin.at("pin") == HE::kMatOutputMetallicPin)
		{
			sawMetallicPin = true;
			CHECK(pin.at("connected") == false);
		}
	CHECK(sawMetallicPin);
	CHECK(after.content.at("links").size() == before.content.at("links").size() - 1);
	const ToolResult instAfter = f.call("material_info", json{ { "path", "Materials/Rock_Inst.hasset" } });
	REQUIRE_FALSE(instAfter.isError);
	CHECK(findParam(instAfter.content.at("params"), "Metal") == nullptr);

	// The Output node is the fixed sink; an unknown id and a comment id are
	// not nodes.
	const std::string bytes = f.bytes(path);
	const ToolResult sink = f.call("material_remove_node", json{ { "path", path }, { "id", outId } });
	CHECK(sink.isError);
	CHECK(sink.errorCode == "refused_by_policy");
	const ToolResult ghost = f.call("material_remove_node", json{ { "path", path }, { "id", 9999 } });
	CHECK(ghost.isError);
	CHECK(ghost.errorCode == "not_found");
	const ToolResult noId = f.call("material_remove_node", json{ { "path", path }, { "id", "three" } });
	CHECK(noId.isError);
	CHECK(noId.errorCode == "invalid_payload");
	CHECK(f.bytes(path) == bytes);

	// A comment box shares the id counter but is not a node.
	{
		HE::MaterialGraph g = makeParamGraph();
		HE::MatGraphComment c;
		c.id = g.nextId++;
		c.text = "Notes";
		g.comments.push_back(c);
		f.writeMaterial("Materials/Commented.hasset", g);
		const ToolResult cm = f.call("material_remove_node", json{
			{ "path", "Materials/Commented.hasset" }, { "id", c.id } });
		CHECK(cm.isError);
		CHECK(cm.errorCode == "not_found");
		CHECK(cm.errorMessage.find("comment") != std::string::npos);
	}
}

TEST_CASE("mcp material tools: a shared name keeps its slot while one node remains")
{
	Fixture f("remove_shared");
	f.writeMaterial("Materials/Shared.hasset", makeSharedNameGraph());
	const std::string path = "Materials/Shared.hasset";
	const ToolResult before = f.call("material_graph_info", json{ { "path", path } });
	REQUIRE_FALSE(before.isError);
	int first = -1;
	for (const json& n : before.content.at("nodes"))
		if (n.at("type") == "ParamFloat") { first = n.at("id"); break; }
	REQUIRE(first > 0);

	const ToolResult r = f.call("material_remove_node", json{ { "path", path }, { "id", first } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content.at("parameter").at("name") == "Shared");
	CHECK(r.content.at("parameter").at("slotRemoved") == false);
	HE::MatParamSlot slot;
	CHECK(codegenValueOf(f.root, path, "Shared", slot));
}

#if defined(HE_TESTS_HAVE_SHADERC)
TEST_CASE("mcp material tools: an edited template still cross-compiles")
{
	// A graph that generates but does not compile renders magenta; the
	// readers cannot notice, so the editors get the panel's own check.
	Fixture f("edit_compile");
	const ToolResult made = f.call("material_create", json{
		{ "path", "Materials/Edited.hasset" }, { "template", "OpaquePBR" } });
	REQUIRE_FALSE(made.isError);
	const std::string path = "Materials/Edited.hasset";

	// Take the Roughness parameter out (the pin falls back to its default) and
	// add a Fresnel — the codegen now differs from every shipped template.
	const ToolResult info = f.call("material_graph_info", json{ { "path", path } });
	REQUIRE_FALSE(info.isError);
	int roughId = -1;
	for (const json& n : info.content.at("nodes"))
		if (n.contains("paramName") && n.at("paramName") == "Roughness") roughId = n.at("id");
	REQUIRE(roughId > 0);
	REQUIRE_FALSE(f.call("material_remove_node", json{ { "path", path }, { "id", roughId } }).isError);
	REQUIRE_FALSE(f.call("material_add_node", json{ { "path", path }, { "type", "Fresnel" },
	                                                { "p", json::array({ 5.0 }) } }).isError);

	HE::MaterialGraph g;
	REQUIRE(savedGraphOf(f.root, path, g));
	const HE::MatShaderGen gen = HE::generateFragment(g);
	REQUIRE_FALSE(gen.glsl.empty());
	bool sawRough = false;
	for (const HE::MatParamSlot& s : gen.params) if (s.name == "Roughness") sawRough = true;
	CHECK_FALSE(sawRough);
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	const uint64_t hash = std::hash<std::string>{}(gen.glsl);
	const auto& msl = lib.fragment(hash, gen.glsl, B::Metal);
	CHECK_MESSAGE(msl.ok, "MSL compile failed: ", msl.log);
	const auto& gl = lib.fragment(hash, gen.glsl, B::GLSL410);
	CHECK_MESSAGE(gl.ok, "GLSL compile failed: ", gl.log);
}
#endif

TEST_CASE("mcp material tools: the graph editors refuse without writing")
{
	Fixture f("gedit_gates");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());
	f.writeFunction("Materials/Fn.hasset", makeFunctionGraph());
	f.writeStub("Materials/Stub.hasset", HE::AssetType::Material);
	REQUIRE_FALSE(f.call("material_create_instance", json{
		{ "parent", "Materials/Rock.hasset" }, { "path", "Materials/Rock_Inst.hasset" } }).isError);
	const std::string path = "Materials/Rock.hasset";
	const ToolResult info = f.call("material_graph_info", json{ { "path", path } });
	REQUIRE_FALSE(info.isError);
	const int someId = nodeOfType(info.content.at("nodes"), "ParamFloat")->at("id");

	const json add{ { "path", path }, { "type", "ConstFloat" } };
	const json rem{ { "path", path }, { "id", someId } };
	auto both = [&](const char* code, const std::string& atPath = {}) {
		for (const char* tool : { "material_add_node", "material_remove_node" })
		{
			json args = std::string(tool) == "material_add_node" ? add : rem;
			if (!atPath.empty()) args["path"] = atPath;
			const std::string before = f.bytes(args["path"].get<std::string>());
			const ToolResult r = f.call(tool, args);
			CAPTURE(tool);
			CHECK(r.isError);
			CHECK(r.errorCode == code);
			CHECK(f.bytes(args["path"].get<std::string>()) == before);
		}
	};

	f.playing = true;   both("play_mode");        f.playing = false;
	f.lockedRel = path; both("locked_by_other");  f.lockedRel.clear();
	f.dirtyRel = path; f.openRel = path;
	both("dirty");
	f.dirtyRel.clear(); f.openRel.clear();

	// The two `no_graph` cases and the function.
	both("no_graph",     "Materials/Rock_Inst.hasset");
	both("no_graph",     "Materials/Stub.hasset");
	both("invalid_path", "Materials/Fn.hasset");
	both("not_found",    "Materials/Ghost.hasset");
	// The no-graph refusals name where a graph comes from.
	const ToolResult inst = f.call("material_add_node", json{
		{ "path", "Materials/Rock_Inst.hasset" }, { "type", "ConstFloat" } });
	CHECK(inst.errorMessage.find("Materials/Rock.hasset") != std::string::npos);
	const ToolResult stub = f.call("material_add_node", json{
		{ "path", "Materials/Stub.hasset" }, { "type", "ConstFloat" } });
	CHECK(stub.errorMessage.find("material_create") != std::string::npos);

	// The engine namespace.
	{
		const fs::path engineRoot = f.root / "__engine";
		fs::create_directories(engineRoot / "Materials");
		f.content.setEngineContentRoot(engineRoot.string());
		const fs::path abs = engineRoot / "Materials/Default.hasset";
		REQUIRE(HE::Ed::writeAssetStub(abs.string(), "Engine/Materials/Default.hasset",
		                               "Default", HE::AssetType::Material));
		EditorAssetTypeCache::invalidate(abs.string());
		const ToolResult r = f.call("material_add_node", json{
			{ "path", "Engine/Materials/Default.hasset" }, { "type", "ConstFloat" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "read_only");
	}

	// A CLEAN tab is told to re-read, for both.
	f.openRel = path;
	CHECK(f.reloadCalls == 0);
	const ToolResult added = f.call("material_add_node", add);
	REQUIRE_FALSE(added.isError);
	CHECK(added.content.at("reloadedInEditor") == true);
	const ToolResult removed = f.call("material_remove_node", rem);
	REQUIRE_FALSE(removed.isError);
	CHECK(removed.content.at("reloadedInEditor") == true);
	CHECK(f.reloadCalls == 2);
}

// ─── Serving the material tools to a real client, by hand ────────────────────
// Every case above calls a handler in-process. That proves what a tool DOES, but
// not what a client SEES: the bridge wraps a ToolResult into MCP's wire shape
// (`content`/`isError`/`structuredContent`), the shim (scripts/he_mcp.py) hands
// it across, and where `errorCode` ends up on the wire is a fact of that
// wrapping, not of the handler. The editor cannot serve this check without a
// project on the machine, so this case IS the running server: the real
// McpBridge with the real registerMaterialTools on a temp root, plus the
// engine's own EngineContent under `Engine/`, pumped until told to stop.
//
// Off unless HE_MCP_SERVE_MATERIAL names the endpoint file to write — then
// drive it with `scripts/he_mcp.py --endpoint <that file>` from another
// process, and touch `<that file>.stop` to end it. HE_MCP_SERVE_SECONDS caps the
// wait (default 120) so a forgotten run cannot hang a suite.
TEST_CASE("mcp material tools: serve to a client (HE_MCP_SERVE_MATERIAL)")
{
	const char* endpoint = std::getenv("HE_MCP_SERVE_MATERIAL");
	if (!endpoint || !*endpoint) return;
	int seconds = 120;
	if (const char* s = std::getenv("HE_MCP_SERVE_SECONDS"); s && *s) seconds = std::atoi(s);

	// What a client finds on the root: a master with every parameter kind
	// (the graph_info fixture), a function, a stub, and the shipped engine
	// content under Engine/ — read-only there, like in the editor.
	Fixture f("serve");
	f.writeMaterial("Materials/Params.hasset", makeParamGraph());
	f.writeFunction("Materials/Fn.hasset", makeFunctionGraph());
	f.writeStub("Materials/Stub.hasset", HE::AssetType::Material);
	if (const char* eng = std::getenv("HE_MCP_SERVE_ENGINE_CONTENT"); eng && *eng)
		f.content.setEngineContentRoot(eng);

	HE::Ed::McpBridge bridge;
	bridge.setEndpointFile(fs::path(endpoint));
	HE::Ed::registerMaterialTools(bridge.registry(), f.content, McpMaterialHooks{});
	REQUIRE(bridge.start());
	std::cerr << "he_tests: serving material tools on port " << bridge.port()
	          << ", root " << f.root << ", stop file " << endpoint << ".stop\n";

	const fs::path stopFile = fs::path(std::string(endpoint) + ".stop");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	std::uint64_t now = 0;
	while (std::chrono::steady_clock::now() < deadline && !fs::exists(stopFile))
	{
		bridge.update(now);
		now += 16;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	CHECK_MESSAGE(fs::exists(stopFile), "no client stopped the server before the cap");
	bridge.stop();
	he_test::removeQuiet(stopFile);
}
