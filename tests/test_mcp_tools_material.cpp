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

#include <algorithm>
#include <cctype>
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
		{ "Foliage",       "Masked",      "Surface",
		  { "BaseColor", "Metallic", "Specular", "Roughness", "Emissive", "OpacityMask",
		    "WindAmount", "WindFrequency", "BendHeight" } },
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
		// Only Foliage moves its vertices, and the asset on disk must carry
		// the WPO body: without it the material would draw standing still.
		const bool wantWind = std::string(e.tpl) == "Foliage";
		CHECK(gen.vertexBody.empty() != wantWind);
		CHECK(a->customShaderVertGlsl.empty() != wantWind);
		if (wantWind)
		{
			CHECK(a->customShaderVertGlsl.find("heLight.camPos.w") != std::string::npos);
			CHECK(a->customShaderVertGlsl.find("pos.y") != std::string::npos);
		}
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
		if (!gen.vertexBody.empty())
		{
			const uint64_t vh = std::hash<std::string>{}(gen.vertexBody);
			const auto& mv = lib.customVertex(vh, gen.vertexBody, B::Metal);
			CHECK_MESSAGE(mv.ok, e.tpl, ": MSL vertex compile failed: ", mv.log);
			const auto& gv = lib.customVertex(vh, gen.vertexBody, B::GLSL410);
			CHECK_MESSAGE(gv.ok, e.tpl, ": GLSL vertex compile failed: ", gv.log);
		}
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
		std::transform(low.begin(), low.end(), low.begin(),
		               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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

// ─── The wiring tools ────────────────────────────────────────────────────────

namespace {

// The ids the wiring tests address, read off material_graph_info on the param
// fixture — never hard-coded, the graph hands out ids in insertion order and a
// test that assumed them would be a test of that.
struct ParamIds
{
	int out = -1, tint = -1, metal = -1, rough = -1;
};

ParamIds paramIdsOf(Fixture& f, const std::string& path)
{
	ParamIds ids;
	const ToolResult info = f.call("material_graph_info", json{ { "path", path } });
	REQUIRE_FALSE(info.isError);
	for (const json& n : info.content.at("nodes"))
	{
		if (n.at("type") == "Output") ids.out = n.at("id");
		if (n.contains("paramName") && n.at("paramName") == "Tint")  ids.tint  = n.at("id");
		if (n.contains("paramName") && n.at("paramName") == "Metal") ids.metal = n.at("id");
		if (n.contains("paramName") && n.at("paramName") == "Rough") ids.rough = n.at("id");
	}
	REQUIRE(ids.out > 0);
	REQUIRE(ids.tint > 0);
	REQUIRE(ids.metal > 0);
	REQUIRE(ids.rough > 0);
	return ids;
}

// The link into (node, pin) as the FILE holds it, or nullptr.
const HE::MatGraphLink* savedLinkInto(const HE::MaterialGraph& g, int node, int pin)
{
	for (const HE::MatGraphLink& l : g.links)
		if (l.dstNode == node && l.dstPin == pin) return &l;
	return nullptr;
}

} // namespace

TEST_CASE("mcp material tools: wiring tools registration")
{
	Fixture f("wire_reg");
	for (const char* n : { "material_connect", "material_disconnect", "material_set_pin_default" })
	{
		const McpTool* t = f.registry.find(n);
		REQUIRE(t != nullptr);
		CHECK(McpToolRegistry::enforceNameRule(t->name));
		CHECK(t->inputSchema.is_object());
		CHECK_FALSE(t->description.empty());
		CHECK(t->mutates);
	}
}

TEST_CASE("mcp material tools: connect wires by name and index, replaces, coerces, and the file has it")
{
	Fixture f("connect");
	const std::string path = "Materials/Rock.hasset";
	f.writeMaterial(path, makeParamGraph());
	const ParamIds ids = paramIdsOf(f, path);
	const int nodes0 = static_cast<int>(makeParamGraph().nodes.size());
	const int links0 = static_cast<int>(makeParamGraph().links.size());

	// A constant into Roughness, BY NAME: the Param wire that was there is
	// replaced and reported, the type pair matches, the link count stays.
	const ToolResult added = f.call("material_add_node", json{
		{ "path", path }, { "type", "ConstFloat" }, { "p", json::array({ 0.9 }) } });
	REQUIRE_FALSE(added.isError);
	const int cid = added.content.at("node").at("id");

	const ToolResult r1 = f.call("material_connect", json{
		{ "path", path }, { "srcNode", cid }, { "srcPin", "Value" },
		{ "dstNode", ids.out }, { "dstPin", "Roughness" } });
	REQUIRE_FALSE(r1.isError);
	CHECK(r1.content.at("connected") == true);
	CHECK(r1.content.at("changed") == true);
	CHECK(r1.content.at("coercion").is_null());
	CHECK(r1.content.at("srcType") == "float");
	CHECK(r1.content.at("dstType") == "float");
	CHECK(r1.content.at("link") == json{
		{ "srcNode", cid },     { "srcPin", 0 },                        { "srcName", "Value" },
		{ "dstNode", ids.out }, { "dstPin", HE::kMatOutputRoughnessPin }, { "dstName", "Roughness" } });
	REQUIRE(r1.content.at("replacedLinks").size() == 1);
	CHECK(r1.content.at("replacedLinks")[0] == json{
		{ "srcNode", ids.rough }, { "srcPin", 0 },
		{ "dstNode", ids.out },   { "dstPin", HE::kMatOutputRoughnessPin } });
	CHECK(r1.content.at("nodeCount") == nodes0 + 1);
	CHECK(r1.content.at("linkCount") == links0);
	CHECK_FALSE(r1.content.contains("parameter"));   // a constant declares nothing

	// The file: the wire leaves the constant, and Rough — no longer reached
	// from Output — has no slot in the codegen any more.
	{
		HE::MaterialGraph g;
		REQUIRE(savedGraphOf(f.root, path, g));
		const HE::MatGraphLink* l = savedLinkInto(g, ids.out, HE::kMatOutputRoughnessPin);
		REQUIRE(l != nullptr);
		CHECK(l->srcNode == cid);
		HE::MatParamSlot slot;
		CHECK_FALSE(codegenValueOf(f.root, path, "Rough", slot));
		CHECK(codegenValueOf(f.root, path, "Metal", slot));
	}

	// The same wire again: nothing to do, nothing written.
	{
		const std::string before = f.bytes(path);
		const ToolResult again = f.call("material_connect", json{
			{ "path", path }, { "srcNode", cid }, { "srcPin", 0 },
			{ "dstNode", ids.out }, { "dstPin", HE::kMatOutputRoughnessPin } });
		REQUIRE_FALSE(again.isError);
		CHECK(again.content.at("changed") == false);
		CHECK_FALSE(again.content.contains("nodeCount"));
		CHECK(f.bytes(path) == before);
	}

	// A vec3 into a float pin (Tint → Ambient Occlusion): the canvas takes it,
	// the codegen coerces, the answer says so — and the Param at the source end
	// reports its slot.
	const ToolResult r2 = f.call("material_connect", json{
		{ "path", path }, { "srcNode", ids.tint }, { "srcPin", "RGB" },
		{ "dstNode", ids.out }, { "dstPin", "Ambient Occlusion" } });
	REQUIRE_FALSE(r2.isError);
	CHECK(r2.content.at("coercion") == "vec3 -> float");
	CHECK(r2.content.at("replacedLinks").empty());
	CHECK(r2.content.at("linkCount") == links0 + 1);
	REQUIRE(r2.content.contains("parameter"));
	CHECK(r2.content.at("parameter").at("name") == "Tint");
	CHECK(r2.content.at("parameter").at("hasSlot") == true);

	// Refused when the client said no coercion: file untouched.
	{
		const std::string before = f.bytes(path);
		const ToolResult no = f.call("material_connect", json{
			{ "path", path }, { "srcNode", ids.metal }, { "srcPin", 0 },
			{ "dstNode", ids.out }, { "dstPin", "Base Color" }, { "allowCoercion", false } });
		CHECK(no.isError);
		CHECK(no.errorCode == "failed");
		CHECK(no.errorMessage.find("float -> vec3") != std::string::npos);
		CHECK(f.bytes(path) == before);
	}

	// BY INDEX, the Param back onto Roughness: the constant's wire is the one
	// replaced now, and Rough owns a slot again.
	const ToolResult r3 = f.call("material_connect", json{
		{ "path", path }, { "srcNode", ids.rough }, { "srcPin", 0 },
		{ "dstNode", ids.out }, { "dstPin", HE::kMatOutputRoughnessPin } });
	REQUIRE_FALSE(r3.isError);
	REQUIRE(r3.content.at("replacedLinks").size() == 1);
	CHECK(r3.content.at("replacedLinks")[0].at("srcNode") == cid);
	CHECK(r3.content.at("parameter").at("name") == "Rough");
	CHECK(r3.content.at("parameter").at("hasSlot") == true);
	HE::MatParamSlot slot;
	CHECK(codegenValueOf(f.root, path, "Rough", slot));

	// A wire that leads nowhere near Output: a Param node feeding a Multiply
	// that feeds nothing has no slot, and the answer says so.
	const ToolResult mul = f.call("material_add_node", json{ { "path", path }, { "type", "Multiply" } });
	REQUIRE_FALSE(mul.isError);
	const ToolResult r4 = f.call("material_connect", json{
		{ "path", path }, { "srcNode", ids.metal }, { "srcPin", "Value" },
		{ "dstNode", mul.content.at("node").at("id") }, { "dstPin", "A" } });
	REQUIRE_FALSE(r4.isError);
	CHECK(r4.content.at("parameter").at("hasSlot") == true);   // still wired to Metallic too
}

TEST_CASE("mcp material tools: connect resolves dynamic pins — the blend mode's, a function's")
{
	Fixture f("connect_dyn");

	// Opaque: the Opacity pin is not drawn and not evaluated, so it is not
	// wirable — by index or by either name. Masked: it is 'OpacityMask'.
	// Translucent (the param fixture): 'Opacity'.
	{
		HE::MaterialGraph opaque = HE::MaterialGraph::makeDefault();
		f.writeMaterial("Materials/Opaque.hasset", opaque);
		HE::MaterialGraph masked = HE::MaterialGraph::makeDefault();
		for (HE::MatGraphNode& n : masked.nodes)
			if (n.type == HE::MatNodeType::Output)
				n.p[1] = static_cast<float>(HE::MatBlendMode::Masked);
		f.writeMaterial("Materials/Masked.hasset", masked);
		f.writeMaterial("Materials/Rock.hasset", makeParamGraph());

		auto idsOf = [&](const std::string& path, int& out, int& col) {
			const ToolResult info = f.call("material_graph_info", json{ { "path", path } });
			REQUIRE_FALSE(info.isError);
			out = nodeOfType(info.content.at("nodes"), "Output")->at("id");
			col = nodeOfType(info.content.at("nodes"), "ConstColor")->at("id");
		};
		int out = -1, col = -1;
		idsOf("Materials/Opaque.hasset", out, col);
		for (const json& pin : { json(HE::kMatOutputOpacityPin), json("Opacity"), json("OpacityMask") })
		{
			const std::string before = f.bytes("Materials/Opaque.hasset");
			const ToolResult r = f.call("material_connect", json{
				{ "path", "Materials/Opaque.hasset" }, { "srcNode", col }, { "srcPin", 0 },
				{ "dstNode", out }, { "dstPin", pin } });
			CAPTURE(pin.dump());
			CHECK(r.isError);
			CHECK(r.errorCode == "invalid_payload");
			CHECK(f.bytes("Materials/Opaque.hasset") == before);
		}
		idsOf("Materials/Masked.hasset", out, col);
		const ToolResult m = f.call("material_connect", json{
			{ "path", "Materials/Masked.hasset" }, { "srcNode", col }, { "srcPin", 0 },
			{ "dstNode", out }, { "dstPin", "OpacityMask" } });
		REQUIRE_FALSE(m.isError);
		CHECK(m.content.at("link").at("dstPin") == HE::kMatOutputOpacityPin);
		CHECK(m.content.at("coercion") == "vec3 -> float");
		CHECK(f.call("material_connect", json{
			{ "path", "Materials/Masked.hasset" }, { "srcNode", col }, { "srcPin", 0 },
			{ "dstNode", out }, { "dstPin", "Opacity" } }).errorCode == "invalid_payload");
	}

	// A FunctionCall's pins are the function's: makeFunctionGraph has one
	// FnOutput 'Out' and no FnInput, so 'Out' resolves as a source and the call
	// has no input to wire into. A call to a function that is not there has no
	// pins at all, and says so.
	{
		const std::string path = "Materials/Rock.hasset";
		f.writeFunction("Materials/Fn.hasset", makeFunctionGraph());
		const ParamIds ids = paramIdsOf(f, path);
		const ToolResult call = f.call("material_add_node", json{
			{ "path", path }, { "type", "FunctionCall" }, { "s", "Materials/Fn.hasset" } });
		REQUIRE_FALSE(call.isError);
		const int fnId = call.content.at("node").at("id");

		const ToolResult r = f.call("material_connect", json{
			{ "path", path }, { "srcNode", fnId }, { "srcPin", "Out" },
			{ "dstNode", ids.out }, { "dstPin", "Emissive" } });
		REQUIRE_FALSE(r.isError);
		CHECK(r.content.at("link").at("srcPin") == 0);
		// A fresh FnOutput is a vec3 (addNode sets p[0] = 2), as Emissive is.
		CHECK(r.content.at("coercion").is_null());
		// The function's own parameter now reaches the material's layout.
		HE::MatParamSlot slot;
		const ToolResult info = f.call("material_info", json{ { "path", path } });
		REQUIRE_FALSE(info.isError);
		CHECK(findParam(info.content.at("params"), "FnTint") != nullptr);

		const ToolResult noIn = f.call("material_connect", json{
			{ "path", path }, { "srcNode", ids.tint }, { "srcPin", 0 },
			{ "dstNode", fnId }, { "dstPin", 0 } });
		CHECK(noIn.isError);
		CHECK(noIn.errorCode == "invalid_payload");
		CHECK(noIn.errorMessage.find("no input pins") != std::string::npos);

		// A call bound to a path that is not there — written by hand, since
		// material_add_node refuses to make one.
		HE::MaterialGraph g = makeParamGraph();
		const int ghost = g.addNode(HE::MatNodeType::FunctionCall, 0, 0);
		g.findNode(ghost)->s = "Materials/Nope.hasset";
		f.writeMaterial("Materials/Ghostly.hasset", g);
		const ToolResult gi = f.call("material_graph_info", json{ { "path", "Materials/Ghostly.hasset" } });
		REQUIRE_FALSE(gi.isError);
		const int ghostId = nodeOfType(gi.content.at("nodes"), "FunctionCall")->at("id");
		const int outId   = nodeOfType(gi.content.at("nodes"), "Output")->at("id");
		const std::string before = f.bytes("Materials/Ghostly.hasset");
		const ToolResult missing = f.call("material_connect", json{
			{ "path", "Materials/Ghostly.hasset" }, { "srcNode", ghostId }, { "srcPin", 0 },
			{ "dstNode", outId }, { "dstPin", "Emissive" } });
		CHECK(missing.isError);
		CHECK(missing.errorCode == "invalid_payload");
		CHECK(missing.errorMessage.find("could not be loaded") != std::string::npos);
		CHECK(f.bytes("Materials/Ghostly.hasset") == before);
	}
}

TEST_CASE("mcp material tools: connect refuses a cycle, a self-wire and a wrong end without writing")
{
	Fixture f("connect_refuse");
	const std::string path = "Materials/Rock.hasset";
	f.writeMaterial(path, makeParamGraph());
	const ParamIds ids = paramIdsOf(f, path);
	const int m1 = f.call("material_add_node", json{ { "path", path }, { "type", "Multiply" } })
	                   .content.at("node").at("id");
	const int m2 = f.call("material_add_node", json{ { "path", path }, { "type", "Multiply" } })
	                   .content.at("node").at("id");
	REQUIRE_FALSE(f.call("material_connect", json{
		{ "path", path }, { "srcNode", m1 }, { "srcPin", 0 }, { "dstNode", m2 }, { "dstPin", "A" } }).isError);
	// m2 feeds Output too, so the cycle m1 → m2 → m1 would sit on the live path.
	REQUIRE_FALSE(f.call("material_connect", json{
		{ "path", path }, { "srcNode", m2 }, { "srcPin", 0 }, { "dstNode", ids.out }, { "dstPin", "Emissive" } }).isError);

	const std::string before = f.bytes(path);
	auto refused = [&](json args, const char* code, const char* says) {
		args["path"] = path;
		const ToolResult r = f.call("material_connect", args);
		CAPTURE(args.dump());
		CHECK(r.isError);
		CHECK(r.errorCode == code);
		CHECK(r.errorMessage.find(says) != std::string::npos);
		CHECK(f.bytes(path) == before);
	};
	// The cycle, direct and through a node.
	refused(json{ { "srcNode", m2 }, { "srcPin", 0 }, { "dstNode", m1 }, { "dstPin", "B" } },
	        "failed", "cycle");
	const int m3 = f.call("material_add_node", json{ { "path", path }, { "type", "Multiply" } })
	                   .content.at("node").at("id");
	REQUIRE_FALSE(f.call("material_connect", json{
		{ "path", path }, { "srcNode", m2 }, { "srcPin", 0 }, { "dstNode", m3 }, { "dstPin", "A" } }).isError);
	const std::string before2 = f.bytes(path);
	{
		const ToolResult r = f.call("material_connect", json{
			{ "path", path }, { "srcNode", m3 }, { "srcPin", 0 }, { "dstNode", m1 }, { "dstPin", "A" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "failed");
		CHECK(f.bytes(path) == before2);
	}
	auto refused2 = [&](json args, const char* code, const char* says) {
		args["path"] = path;
		const ToolResult r = f.call("material_connect", args);
		CAPTURE(args.dump());
		CHECK(r.isError);
		CHECK(r.errorCode == code);
		CHECK(r.errorMessage.find(says) != std::string::npos);
		CHECK(f.bytes(path) == before2);
	};
	// A node onto itself.
	refused2(json{ { "srcNode", m1 }, { "srcPin", 0 }, { "dstNode", m1 }, { "dstPin", "B" } },
	         "invalid_payload", "same node");
	// Unknown nodes, each end.
	refused2(json{ { "srcNode", 9999 }, { "srcPin", 0 }, { "dstNode", ids.out }, { "dstPin", 0 } },
	         "not_found", "9999");
	refused2(json{ { "srcNode", ids.tint }, { "srcPin", 0 }, { "dstNode", 9999 }, { "dstPin", 0 } },
	         "not_found", "9999");
	// Wrong side: Output has no outputs, a Param has no inputs.
	refused2(json{ { "srcNode", ids.out }, { "srcPin", 0 }, { "dstNode", m1 }, { "dstPin", "A" } },
	         "invalid_payload", "no output pins");
	refused2(json{ { "srcNode", m1 }, { "srcPin", 0 }, { "dstNode", ids.tint }, { "dstPin", 0 } },
	         "invalid_payload", "no input pins");
	// Unknown pin: by an index the node lacks, by a name it lacks, by a name
	// spelled loosely, by the empty name, and by a shape that is neither.
	refused2(json{ { "srcNode", ids.tint }, { "srcPin", 3 }, { "dstNode", m1 }, { "dstPin", "A" } },
	         "invalid_payload", "no output pin 3");
	refused2(json{ { "srcNode", ids.tint }, { "srcPin", 0 }, { "dstNode", m1 }, { "dstPin", "C" } },
	         "invalid_payload", "'A' (0), 'B' (1)");
	refused2(json{ { "srcNode", ids.tint }, { "srcPin", 0 }, { "dstNode", ids.out }, { "dstPin", "base color" } },
	         "invalid_payload", "exact");
	refused2(json{ { "srcNode", ids.tint }, { "srcPin", "" }, { "dstNode", m1 }, { "dstPin", "A" } },
	         "invalid_payload", "empty");
	refused2(json{ { "srcNode", ids.tint }, { "srcPin", 1.5 }, { "dstNode", m1 }, { "dstPin", "A" } },
	         "invalid_payload", "index");
	// Missing arguments.
	refused2(json{ { "srcNode", ids.tint }, { "srcPin", 0 }, { "dstNode", m1 } },
	         "invalid_payload", "required");
	refused2(json{ { "srcNode", "1" }, { "srcPin", 0 }, { "dstNode", m1 }, { "dstPin", "A" } },
	         "invalid_payload", "integer node ids");
}

TEST_CASE("mcp material tools: disconnect takes the wire and names the fallback")
{
	Fixture f("disconnect");
	const std::string path = "Materials/Rock.hasset";
	f.writeMaterial(path, makeParamGraph());
	const ParamIds ids = paramIdsOf(f, path);
	const int links0 = static_cast<int>(makeParamGraph().links.size());

	// By the input alone: the wire, what the pin reads now, and that the Param
	// behind it lost its slot.
	const ToolResult r = f.call("material_disconnect", json{
		{ "path", path }, { "dstNode", ids.out }, { "dstPin", "Metallic" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content.at("disconnected") == true);
	CHECK(r.content.at("link") == json{
		{ "srcNode", ids.metal }, { "srcPin", 0 },
		{ "dstNode", ids.out },   { "dstPin", HE::kMatOutputMetallicPin } });
	CHECK(r.content.at("fallback").at("name") == "Metallic");
	CHECK(r.content.at("fallback").at("type") == "float");
	CHECK(r.content.at("fallback").at("default") == 0.0);
	CHECK(r.content.at("linkCount") == links0 - 1);
	CHECK(r.content.at("parameter").at("name") == "Metal");
	CHECK(r.content.at("parameter").at("hasSlot") == false);
	{
		HE::MaterialGraph g;
		REQUIRE(savedGraphOf(f.root, path, g));
		CHECK(savedLinkInto(g, ids.out, HE::kMatOutputMetallicPin) == nullptr);
		CHECK(g.findNode(ids.metal) != nullptr);   // the node stays, only the wire went
		HE::MatParamSlot slot;
		CHECK_FALSE(codegenValueOf(f.root, path, "Metal", slot));
		const ToolResult info = f.call("material_info", json{ { "path", path } });
		REQUIRE_FALSE(info.isError);
		CHECK(findParam(info.content.at("params"), "Metal") == nullptr);
	}

	// Again: nothing there.
	const std::string before = f.bytes(path);
	{
		const ToolResult none = f.call("material_disconnect", json{
			{ "path", path }, { "dstNode", ids.out }, { "dstPin", "Metallic" } });
		CHECK(none.isError);
		CHECK(none.errorCode == "not_found");
		CHECK(none.errorMessage.find("no wire") != std::string::npos);
		CHECK(f.bytes(path) == before);
	}
	// With the source named: it has to be THE wire, and the refusal says which
	// one is there.
	{
		const ToolResult wrong = f.call("material_disconnect", json{
			{ "path", path }, { "dstNode", ids.out }, { "dstPin", "Roughness" },
			{ "srcNode", ids.tint }, { "srcPin", "RGB" } });
		CHECK(wrong.isError);
		CHECK(wrong.errorCode == "not_found");
		CHECK(wrong.errorMessage.find("node " + std::to_string(ids.rough)) != std::string::npos);
		CHECK(f.bytes(path) == before);

		const ToolResult half = f.call("material_disconnect", json{
			{ "path", path }, { "dstNode", ids.out }, { "dstPin", "Roughness" }, { "srcNode", ids.rough } });
		CHECK(half.isError);
		CHECK(half.errorCode == "invalid_payload");
		CHECK(f.bytes(path) == before);

		const ToolResult ghost = f.call("material_disconnect", json{
			{ "path", path }, { "dstNode", ids.out }, { "dstPin", "Roughness" },
			{ "srcNode", 9999 }, { "srcPin", 0 } });
		CHECK(ghost.errorCode == "not_found");
		const ToolResult badPin = f.call("material_disconnect", json{
			{ "path", path }, { "dstNode", ids.out }, { "dstPin", "Roughness" },
			{ "srcNode", ids.rough }, { "srcPin", "Nope" } });
		CHECK(badPin.errorCode == "invalid_payload");
		CHECK(f.bytes(path) == before);
	}
	// The right source, both ends by name — the hc shape.
	{
		const ToolResult ok = f.call("material_disconnect", json{
			{ "path", path }, { "dstNode", ids.out }, { "dstPin", "Roughness" },
			{ "srcNode", ids.rough }, { "srcPin", "Value" } });
		REQUIRE_FALSE(ok.isError);
		CHECK(ok.content.at("link").at("srcNode") == ids.rough);
		CHECK(ok.content.at("linkCount") == links0 - 2);
	}
	// The end that is not there.
	{
		const ToolResult r1 = f.call("material_disconnect", json{ { "path", path }, { "dstNode", 9999 }, { "dstPin", 0 } });
		CHECK(r1.errorCode == "not_found");
		const ToolResult r2 = f.call("material_disconnect", json{ { "path", path }, { "dstNode", ids.out } });
		CHECK(r2.errorCode == "invalid_payload");
		const ToolResult r3 = f.call("material_disconnect", json{ { "path", path }, { "dstNode", ids.out }, { "dstPin", "Nope" } });
		CHECK(r3.errorCode == "invalid_payload");
	}
}

TEST_CASE("mcp material tools: set_pin_default makes one constant, updates it in place and removes it")
{
	Fixture f("pindef");
	const std::string path = "Materials/Rock.hasset";
	f.writeMaterial(path, makeParamGraph());
	const ParamIds ids = paramIdsOf(f, path);
	const int nodes0 = static_cast<int>(makeParamGraph().nodes.size());
	const int links0 = static_cast<int>(makeParamGraph().links.size());

	// A float pin: a ConstFloat appears, wired in, holding the value.
	const ToolResult r1 = f.call("material_set_pin_default", json{
		{ "path", path }, { "node", ids.out }, { "pin", "Ambient Occlusion" }, { "value", 0.25 } });
	REQUIRE_FALSE(r1.isError);
	CHECK(r1.content.at("changed") == true);
	CHECK(r1.content.at("created") == true);
	CHECK(r1.content.at("constantNode").at("type") == "ConstFloat");
	CHECK(r1.content.at("constantNode").at("p")[0].get<float>() == doctest::Approx(0.25f));
	CHECK(r1.content.at("pin") == json{ { "node", ids.out }, { "pin", HE::kMatOutputAOPin },
	                                    { "name", "Ambient Occlusion" }, { "type", "float" } });
	CHECK(r1.content.at("nodeCount") == nodes0 + 1);
	CHECK(r1.content.at("linkCount") == links0 + 1);
	const int cid = r1.content.at("constantNode").at("id");
	{
		HE::MaterialGraph g;
		REQUIRE(savedGraphOf(f.root, path, g));
		const HE::MatGraphLink* l = savedLinkInto(g, ids.out, HE::kMatOutputAOPin);
		REQUIRE(l != nullptr);
		CHECK(l->srcNode == cid);
		const HE::MatGraphNode* c = g.findNode(cid);
		REQUIRE(c != nullptr);
		CHECK(c->type == HE::MatNodeType::ConstFloat);
		CHECK(c->p[0] == doctest::Approx(0.25f));
		// Placed left of the node it feeds, not at the origin.
		CHECK(c->x < g.findNode(ids.out)->x);
	}

	// Again: the SAME constant, updated — no second node.
	const ToolResult r2 = f.call("material_set_pin_default", json{
		{ "path", path }, { "node", ids.out }, { "pin", HE::kMatOutputAOPin }, { "value", 0.75 } });
	REQUIRE_FALSE(r2.isError);
	CHECK(r2.content.at("created") == false);
	CHECK(r2.content.at("constantNode").at("id") == cid);
	CHECK(r2.content.at("constantNode").at("p")[0].get<float>() == doctest::Approx(0.75f));
	CHECK(r2.content.at("nodeCount") == nodes0 + 1);
	{
		HE::MaterialGraph g;
		REQUIRE(savedGraphOf(f.root, path, g));
		CHECK(g.findNode(cid)->p[0] == doctest::Approx(0.75f));
		CHECK(static_cast<int>(g.nodes.size()) == nodes0 + 1);
	}

	// A vec3 pin takes three numbers and gets a ConstColor.
	const ToolResult r3 = f.call("material_set_pin_default", json{
		{ "path", path }, { "node", ids.out }, { "pin", "Normal" }, { "value", json::array({ 0.0, 0.0, 1.0 }) } });
	REQUIRE_FALSE(r3.isError);
	CHECK(r3.content.at("created") == true);
	CHECK(r3.content.at("constantNode").at("type") == "ConstColor");
	CHECK(r3.content.at("constantNode").at("p")[2].get<float>() == doctest::Approx(1.0f));
	CHECK(r3.content.at("nodeCount") == nodes0 + 2);

	// The wrong shape for the pin, and a pin that is wired to something that is
	// not this tool's: refused, file untouched.
	const std::string before = f.bytes(path);
	auto refused = [&](json args, const char* code, const char* says) {
		args["path"] = path;
		const ToolResult r = f.call("material_set_pin_default", args);
		CAPTURE(args.dump());
		CHECK(r.isError);
		CHECK(r.errorCode == code);
		CHECK(r.errorMessage.find(says) != std::string::npos);
		CHECK(f.bytes(path) == before);
	};
	refused(json{ { "node", ids.out }, { "pin", "Normal" }, { "value", 0.5 } },
	        "invalid_payload", "array of 3 numbers");
	refused(json{ { "node", ids.out }, { "pin", "Ambient Occlusion" }, { "value", json::array({ 1.0, 2.0 }) } },
	        "invalid_payload", "a number");
	refused(json{ { "node", ids.out }, { "pin", "Ambient Occlusion" }, { "value", "0.5" } },
	        "invalid_payload", "a number");
	refused(json{ { "node", ids.out }, { "pin", "Base Color" }, { "value", json::array({ 1.0, 1.0, 1.0 }) } },
	        "failed", "material_disconnect");
	refused(json{ { "node", ids.out }, { "pin", "Base Color" } },
	        "failed", "material_disconnect");   // null on a foreign wire is a refusal too
	refused(json{ { "node", ids.out }, { "pin", "Nope" }, { "value", 1.0 } },
	        "invalid_payload", "no input pin named");
	refused(json{ { "node", ids.tint }, { "pin", 0 }, { "value", 1.0 } },
	        "invalid_payload", "no input pins");
	refused(json{ { "node", 9999 }, { "pin", 0 }, { "value", 1.0 } },
	        "not_found", "9999");
	refused(json{ { "pin", 0 }, { "value", 1.0 } },
	        "invalid_payload", "required");

	// The constant stops being this tool's once it feeds a second pin — the
	// human may have wired it — and becomes it again when that wire goes.
	REQUIRE_FALSE(f.call("material_connect", json{
		{ "path", path }, { "srcNode", cid }, { "srcPin", 0 },
		{ "dstNode", ids.out }, { "dstPin", "Roughness" } }).isError);
	{
		const std::string shared = f.bytes(path);
		const ToolResult r = f.call("material_set_pin_default", json{
			{ "path", path }, { "node", ids.out }, { "pin", "Ambient Occlusion" }, { "value", 0.1 } });
		CHECK(r.isError);
		CHECK(r.errorCode == "failed");
		CHECK(f.bytes(path) == shared);
	}
	REQUIRE_FALSE(f.call("material_disconnect", json{
		{ "path", path }, { "dstNode", ids.out }, { "dstPin", "Roughness" } }).isError);
	{
		const ToolResult r = f.call("material_set_pin_default", json{
			{ "path", path }, { "node", ids.out }, { "pin", "Ambient Occlusion" }, { "value", 0.1 } });
		REQUIRE_FALSE(r.isError);
		CHECK(r.content.at("created") == false);
		CHECK(r.content.at("constantNode").at("id") == cid);
	}

	// null removes the constant; the pin reads its default again; null on a
	// pin with no constant is a no-op that writes nothing.
	const ToolResult r4 = f.call("material_set_pin_default", json{
		{ "path", path }, { "node", ids.out }, { "pin", "Ambient Occlusion" }, { "value", nullptr } });
	REQUIRE_FALSE(r4.isError);
	CHECK(r4.content.at("cleared") == true);
	CHECK(r4.content.at("changed") == true);
	CHECK(r4.content.at("removedNode") == cid);
	CHECK(r4.content.at("pin").at("default") == 1.0);   // Ambient Occlusion's registry default
	CHECK(r4.content.at("nodeCount") == nodes0 + 1);    // the Normal constant stays
	{
		HE::MaterialGraph g;
		REQUIRE(savedGraphOf(f.root, path, g));
		CHECK(g.findNode(cid) == nullptr);
		CHECK(savedLinkInto(g, ids.out, HE::kMatOutputAOPin) == nullptr);
	}
	const std::string after = f.bytes(path);
	const ToolResult r5 = f.call("material_set_pin_default", json{
		{ "path", path }, { "node", ids.out }, { "pin", "Ambient Occlusion" }, { "value", nullptr } });
	REQUIRE_FALSE(r5.isError);
	CHECK(r5.content.at("cleared") == true);
	CHECK(r5.content.at("changed") == false);
	CHECK_FALSE(r5.content.contains("nodeCount"));
	CHECK(f.bytes(path) == after);
}

TEST_CASE("mcp material tools: the wiring tools refuse without writing")
{
	Fixture f("wire_gates");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());
	f.writeFunction("Materials/Fn.hasset", makeFunctionGraph());
	f.writeStub("Materials/Stub.hasset", HE::AssetType::Material);
	REQUIRE_FALSE(f.call("material_create_instance", json{
		{ "parent", "Materials/Rock.hasset" }, { "path", "Materials/Rock_Inst.hasset" } }).isError);
	const std::string path = "Materials/Rock.hasset";
	const ParamIds ids = paramIdsOf(f, path);

	// Three calls that would each succeed on the clean master: a wire into an
	// unwired Output pin, the wire out of Emissive, a constant into Normal.
	const std::vector<std::pair<const char*, json>> calls{
		{ "material_connect", json{ { "path", path }, { "srcNode", ids.tint }, { "srcPin", 0 },
		                            { "dstNode", ids.out }, { "dstPin", "Ambient Occlusion" } } },
		{ "material_disconnect", json{ { "path", path }, { "dstNode", ids.out }, { "dstPin", "Emissive" } } },
		{ "material_set_pin_default", json{ { "path", path }, { "node", ids.out }, { "pin", "Normal" },
		                                    { "value", json::array({ 0.0, 0.0, 1.0 }) } } },
	};
	auto all = [&](const char* code, const std::string& atPath = {}) {
		for (const auto& [tool, base] : calls)
		{
			json args = base;
			if (!atPath.empty()) args["path"] = atPath;
			const std::string before = f.bytes(args["path"].get<std::string>());
			const ToolResult r = f.call(tool, args);
			CAPTURE(tool);
			CHECK(r.isError);
			CHECK(r.errorCode == code);
			CHECK(f.bytes(args["path"].get<std::string>()) == before);
		}
	};

	f.playing = true;   all("play_mode");        f.playing = false;
	f.lockedRel = path; all("locked_by_other");  f.lockedRel.clear();
	f.dirtyRel = path; f.openRel = path;
	all("dirty");
	f.dirtyRel.clear(); f.openRel.clear();
	all("no_graph",     "Materials/Rock_Inst.hasset");
	all("no_graph",     "Materials/Stub.hasset");
	all("invalid_path", "Materials/Fn.hasset");
	all("not_found",    "Materials/Ghost.hasset");

	// The engine namespace.
	{
		const fs::path engineRoot = f.root / "__engine";
		fs::create_directories(engineRoot / "Materials");
		f.content.setEngineContentRoot(engineRoot.string());
		const fs::path abs = engineRoot / "Materials/Default.hasset";
		REQUIRE(HE::Ed::writeAssetStub(abs.string(), "Engine/Materials/Default.hasset",
		                               "Default", HE::AssetType::Material));
		EditorAssetTypeCache::invalidate(abs.string());
		all("read_only", "Engine/Materials/Default.hasset");
	}

	// A CLEAN tab is told to re-read, by each of the three.
	f.openRel = path;
	CHECK(f.reloadCalls == 0);
	for (const auto& [tool, args] : calls)
	{
		const ToolResult r = f.call(tool, args);
		CAPTURE(tool);
		REQUIRE_FALSE(r.isError);
		CHECK(r.content.at("reloadedInEditor") == true);
	}
	CHECK(f.reloadCalls == 3);
}

// ─── material_set_node ───────────────────────────────────────────────────────

namespace {

// A function WITH inputs: the FunctionCall bound to it has two input pins and
// one output, so a re-bind to makeFunctionGraph (no inputs) has pins to lose.
HE::MaterialGraph makeTwoInputFunctionGraph()
{
	HE::MaterialGraph g;
	const int a = g.addNode(HE::MatNodeType::FnInput, 40, 60);
	g.findNode(a)->s = "A";
	g.findNode(a)->p[0] = 2.0f;   // Vec3
	const int b = g.addNode(HE::MatNodeType::FnInput, 40, 160);
	g.findNode(b)->s = "B";
	g.findNode(b)->p[0] = 0.0f;   // Float
	const int out = g.addNode(HE::MatNodeType::FnOutput, 380, 120);
	g.findNode(out)->s = "Out";
	g.findNode(out)->p[0] = 2.0f;
	g.connect(a, 0, out, 0);
	return g;
}

// The blend mode the FILE's Output node declares (p[1]), -1 without one.
int blendModeOfSaved(const HE::MaterialGraph& g)
{
	for (const HE::MatGraphNode& n : g.nodes)
		if (n.type == HE::MatNodeType::Output) return static_cast<int>(n.p[1]);
	return -1;
}

} // namespace

TEST_CASE("mcp material tools: set_node registration")
{
	Fixture f("setnode_reg");
	const McpTool* t = f.registry.find("material_set_node");
	REQUIRE(t != nullptr);
	CHECK(McpToolRegistry::enforceNameRule(t->name));
	CHECK(t->inputSchema.is_object());
	CHECK_FALSE(t->description.empty());
	CHECK(t->mutates);
}

TEST_CASE("mcp material tools: set_node changes values, range, metadata and position, and the block follows")
{
	Fixture f("setnode_values");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());
	const std::string path = "Materials/Rock.hasset";
	const ParamIds ids = paramIdsOf(f, path);
	// A resident instance that does not override Metal — the state in which a
	// value written to the node alone would leave a stale block on screen.
	const ToolResult made = f.call("material_create_instance", json{ { "parent", path } });
	REQUIRE_FALSE(made.isError);
	const HE::UUID instId = f.content.idForPath(made.content.at("path").get<std::string>());
	REQUIRE_FALSE(instId == HE::UUID{});

	// ── A wired ParamFloat's value: node default AND block, both to 0.9 ──────
	const ToolResult r1 = f.call("material_set_node", json{
		{ "path", path }, { "id", ids.metal }, { "p", json::array({ 0.9 }) } });
	REQUIRE_FALSE(r1.isError);
	CHECK(r1.content.at("changed") == true);
	CHECK(r1.content.at("node").at("p")[0].get<float>() == doctest::Approx(0.9f));
	CHECK(r1.content.at("parameter").at("name") == "Metal");
	CHECK(r1.content.at("parameter").at("hasSlot") == true);
	CHECK(r1.content.at("parameter").at("blockWritten") == true);
	CHECK(r1.content.at("droppedLinks").empty());
	HE::MatParamSlot slot;
	REQUIRE(codegenValueOf(f.root, path, "Metal", slot));
	CHECK(slot.value[0] == doctest::Approx(0.9f));
	float block[4] = {};
	REQUIRE(savedBlockValueOf(f.root, path, "Metal", block));
	CHECK(block[0] == doctest::Approx(0.9f));   // not put back by the regenerate
	float instV[4] = {};
	REQUIRE(f.content.getMaterialParam(instId, "Metal", instV));
	CHECK(instV[0] == doctest::Approx(0.9f));   // the loaded instance followed

	// ── The same values again: nothing to do, nothing written ────────────────
	const std::string afterFirst = f.bytes(path);
	const ToolResult same = f.call("material_set_node", json{
		{ "path", path }, { "id", ids.metal }, { "p", json::array({ 0.9 }) } });
	REQUIRE_FALSE(same.isError);
	CHECK(same.content.at("changed") == false);
	CHECK(same.content.contains("node"));
	CHECK(f.bytes(path) == afterFirst);

	// ── A slider range on the free ParamFloat, and metadata on the colour ────
	const ToolResult r2 = f.call("material_set_node", json{
		{ "path", path }, { "id", ids.metal }, { "min", 0.0 }, { "max", 2.0 },
		{ "group", "Surface" }, { "tooltip", "How metallic" },
		{ "position", json::array({ 10.0, 20.0 }) } });
	REQUIRE_FALSE(r2.isError);
	CHECK(r2.content.at("node").at("min") == 0.0f);
	CHECK(r2.content.at("node").at("max") == 2.0f);
	CHECK(r2.content.at("node").at("group") == "Surface");
	CHECK(r2.content.at("node").at("tooltip") == "How metallic");
	CHECK(r2.content.at("node").at("x") == 10.0f);
	CHECK(r2.content.at("node").at("y") == 20.0f);
	// The value survived the range — and vice versa in the file.
	CHECK(r2.content.at("node").at("p")[0].get<float>() == doctest::Approx(0.9f));
	HE::MaterialGraph saved;
	REQUIRE(savedGraphOf(f.root, path, saved));
	const HE::MatGraphNode* sn = saved.findNode(ids.metal);
	REQUIRE(sn != nullptr);
	CHECK(sn->p[0] == doctest::Approx(0.9f));
	CHECK(sn->p[1] == 0.0f);
	CHECK(sn->p[2] == 2.0f);
	CHECK(sn->group == "Surface");
	CHECK(sn->tooltip == "How metallic");
	CHECK(sn->x == 10.0f);
	// material_info reports the range the file now declares.
	const ToolResult info = f.call("material_info", json{ { "path", path } });
	REQUIRE_FALSE(info.isError);
	const json* metal = findParam(info.content.at("params"), "Metal");
	REQUIRE(metal != nullptr);
	CHECK(metal->at("group") == "Surface");

	// ── A colour, three components; the fourth slot component is untouched ──
	const ToolResult r3 = f.call("material_set_node", json{
		{ "path", path }, { "id", ids.tint }, { "p", json::array({ 1.0, 0.0, 0.5 }) } });
	REQUIRE_FALSE(r3.isError);
	REQUIRE(savedBlockValueOf(f.root, path, "Tint", block));
	CHECK(block[0] == doctest::Approx(1.0f));
	CHECK(block[1] == doctest::Approx(0.0f));
	CHECK(block[2] == doctest::Approx(0.5f));

	// ── A constant node: p is the value, no parameter in the answer ──────────
	const ToolResult added = f.call("material_add_node", json{
		{ "path", path }, { "type", "ConstFloat" } });
	REQUIRE_FALSE(added.isError);
	const int constId = added.content.at("node").at("id");
	const ToolResult r4 = f.call("material_set_node", json{
		{ "path", path }, { "id", constId }, { "p", json::array({ 3.5 }) } });
	REQUIRE_FALSE(r4.isError);
	CHECK(r4.content.at("node").at("p")[0].get<float>() == doctest::Approx(3.5f));
	CHECK_FALSE(r4.content.contains("parameter"));
	REQUIRE(savedGraphOf(f.root, path, saved));
	REQUIRE(saved.findNode(constId) != nullptr);
	CHECK(saved.findNode(constId)->p[0] == doctest::Approx(3.5f));
}

TEST_CASE("mcp material tools: set_node renames a slot, and an instance's override of the old name is gone")
{
	Fixture f("setnode_rename");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());
	const std::string path = "Materials/Rock.hasset";
	const ParamIds ids = paramIdsOf(f, path);
	const ToolResult made = f.call("material_create_instance", json{ { "parent", path } });
	REQUIRE_FALSE(made.isError);
	const std::string inst = made.content.at("path");
	REQUIRE_FALSE(f.call("material_set_param", json{
		{ "path", inst }, { "name", "Metal" }, { "value", 0.7 } }).isError);

	const ToolResult r = f.call("material_set_node", json{
		{ "path", path }, { "id", ids.metal }, { "s", "Shine" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content.at("node").at("paramName") == "Shine");
	CHECK(r.content.at("parameter").at("name") == "Shine");
	CHECK(r.content.at("parameter").at("renamedFrom") == "Metal");
	CHECK(r.content.at("parameter").at("hasSlot") == true);
	CHECK(r.content.contains("note"));
	// Value given with the rename: the fresh slot takes the node's value, no
	// block pre-write needed (and none claimed).
	CHECK_FALSE(r.content.at("parameter").contains("blockWritten"));

	// The master's layout: Metal gone, Shine there with the old value.
	const ToolResult info = f.call("material_info", json{ { "path", path } });
	REQUIRE_FALSE(info.isError);
	CHECK(findParam(info.content.at("params"), "Metal") == nullptr);
	const json* shine = findParam(info.content.at("params"), "Shine");
	REQUIRE(shine != nullptr);
	CHECK(shine->at("value").get<float>() == doctest::Approx(0.1f));
	HE::MatParamSlot slot;
	CHECK_FALSE(codegenValueOf(f.root, path, "Metal", slot));
	CHECK(codegenValueOf(f.root, path, "Shine", slot));

	// The instance: its override was keyed 'Metal'; 'Shine' follows the parent.
	const ToolResult instInfo = f.call("material_info", json{ { "path", inst } });
	REQUIRE_FALSE(instInfo.isError);
	CHECK(findParam(instInfo.content.at("params"), "Metal") == nullptr);
	const json* instShine = findParam(instInfo.content.at("params"), "Shine");
	REQUIRE(instShine != nullptr);
	CHECK(instShine->at("overridden") == false);
	CHECK(instShine->at("value").get<float>() == doctest::Approx(0.1f));

	// A StaticSwitch rename is reported the same way.
	const ToolResult sw = f.call("material_add_node", json{
		{ "path", path }, { "type", "StaticSwitch" }, { "s", "UseDetail" } });
	REQUIRE_FALSE(sw.isError);
	const int swId = sw.content.at("node").at("id");
	const ToolResult sw2 = f.call("material_set_node", json{
		{ "path", path }, { "id", swId }, { "s", "Detail" }, { "p", json::array({ 1.0 }) } });
	REQUIRE_FALSE(sw2.isError);
	CHECK(sw2.content.at("switch").at("name") == "Detail");
	CHECK(sw2.content.at("switch").at("renamedFrom") == "UseDetail");
	CHECK(sw2.content.at("node").at("switchDefault") == true);
}

TEST_CASE("mcp material tools: set_node on the Output node is the blend mode, the lit flag and the domain")
{
	Fixture f("setnode_output");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());
	const std::string path = "Materials/Rock.hasset";
	const ParamIds ids = paramIdsOf(f, path);
	// The fixture is Translucent with 'Flag' on the Opacity pin — evaluated, so
	// it has a slot.
	HE::MatParamSlot slot;
	REQUIRE(codegenValueOf(f.root, path, "Flag", slot));

	// ── Opaque: the pin is no longer evaluated; the LINK stays in the file ───
	const ToolResult r1 = f.call("material_set_node", json{
		{ "path", path }, { "id", ids.out }, { "blendMode", "Opaque" } });
	REQUIRE_FALSE(r1.isError);
	CHECK(r1.content.at("changed") == true);
	CHECK(r1.content.at("blendMode") == "Opaque");
	CHECK(r1.content.at("lit") == true);
	CHECK(r1.content.at("domain") == "Surface");
	CHECK(r1.content.at("droppedLinks").empty());
	CHECK_FALSE(codegenValueOf(f.root, path, "Flag", slot));
	const ToolResult info = f.call("material_info", json{ { "path", path } });
	REQUIRE_FALSE(info.isError);
	CHECK(info.content.at("blendMode") == "Opaque");
	CHECK(findParam(info.content.at("params"), "Flag") == nullptr);
	HE::MaterialGraph saved;
	REQUIRE(savedGraphOf(f.root, path, saved));
	CHECK(savedLinkInto(saved, ids.out, HE::kMatOutputOpacityPin) != nullptr);
	// The Output node's pins as reported now: no Opacity row.
	const json& outNode = r1.content.at("node");
	CHECK(findPinNamed(outNode.at("inputs"), "Opacity") == nullptr);
	CHECK(findPinNamed(outNode.at("inputs"), "OpacityMask") == nullptr);

	// ── Masked with no cutoff: the editor's 0.5, and the slot is back ────────
	// (addNode gives an Output a cutoff of 0.5 from birth; a graph with 0
	// there is one whose author zeroed it — done here while still Opaque,
	// where the cutoff means nothing.)
	REQUIRE_FALSE(f.call("material_set_node", json{
		{ "path", path }, { "id", ids.out }, { "p", json::array({ 1.0, 0.0, 0.0 }) } }).isError);
	const ToolResult r2 = f.call("material_set_node", json{
		{ "path", path }, { "id", ids.out }, { "p", json::array({ 1.0, 1.0 }) } });
	REQUIRE_FALSE(r2.isError);
	CHECK(r2.content.at("blendMode") == "Masked");
	CHECK(r2.content.at("maskCutoffDefaulted") == 0.5f);
	CHECK(r2.content.at("node").at("p")[2].get<float>() == doctest::Approx(0.5f));
	CHECK(findPinNamed(r2.content.at("node").at("inputs"), "OpacityMask") != nullptr);
	CHECK(codegenValueOf(f.root, path, "Flag", slot));
	REQUIRE(savedGraphOf(f.root, path, saved));
	CHECK(blendModeOfSaved(saved) == static_cast<int>(HE::MatBlendMode::Masked));

	// ── An explicit cutoff is kept; unlit; UI domain ─────────────────────────
	const ToolResult r3 = f.call("material_set_node", json{
		{ "path", path }, { "id", ids.out }, { "p", json::array({ 0.0, 1.0, 0.25, 1.0 }) } });
	REQUIRE_FALSE(r3.isError);
	CHECK(r3.content.at("lit") == false);
	CHECK(r3.content.at("domain") == "User Interface");
	CHECK_FALSE(r3.content.contains("maskCutoffDefaulted"));
	CHECK(r3.content.at("node").at("p")[2].get<float>() == doctest::Approx(0.25f));
	const ToolResult info3 = f.call("material_info", json{ { "path", path } });
	REQUIRE_FALSE(info3.isError);
	CHECK(info3.content.at("domain") == "User Interface");

	// ── The refusals, none of them writing ───────────────────────────────────
	const std::string before = f.bytes(path);
	auto refused = [&](json args, const char* code) {
		const ToolResult r = f.call("material_set_node", args);
		CAPTURE(args.dump());
		CHECK(r.isError);
		CHECK(r.errorCode == code);
		CHECK(f.bytes(path) == before);
	};
	refused(json{ { "path", path }, { "id", ids.out }, { "blendMode", "Glass" } },     "invalid_payload");
	refused(json{ { "path", path }, { "id", ids.out }, { "p", json::array({ 1.0, 7.0 }) } }, "invalid_payload");
	refused(json{ { "path", path }, { "id", ids.out }, { "p", json::array({ 2.0 }) } }, "invalid_payload");
	refused(json{ { "path", path }, { "id", ids.out }, { "p", json::array({ 1.0, 1.0, 0.5, 3.0 }) } },
	        "invalid_payload");
	refused(json{ { "path", path }, { "id", ids.out }, { "blendMode", "Opaque" },
	              { "p", json::array({ 1.0, 2.0 }) } },                                "invalid_payload");
	refused(json{ { "path", path }, { "id", ids.out }, { "s", "x" } },                 "invalid_payload");
	refused(json{ { "path", path }, { "id", ids.metal }, { "blendMode", "Opaque" } }, "invalid_payload");
}

TEST_CASE("mcp material tools: set_node re-binds a FunctionCall and re-lays a layer blend's wires")
{
	Fixture f("setnode_pins");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());
	f.writeFunction("Materials/Fn.hasset", makeFunctionGraph());
	f.writeFunction("Materials/Fn2.hasset", makeTwoInputFunctionGraph());
	f.writeStub("Materials/FnStub.hasset", HE::AssetType::MaterialFunction);
	const std::string path = "Materials/Rock.hasset";
	const ParamIds ids = paramIdsOf(f, path);

	// ── A call to the two-input function, wired on both sides ────────────────
	const ToolResult call = f.call("material_add_node", json{
		{ "path", path }, { "type", "FunctionCall" }, { "s", "Materials/Fn2.hasset" } });
	REQUIRE_FALSE(call.isError);
	const int callId = call.content.at("node").at("id");
	REQUIRE(call.content.at("node").at("inputs").size() == 2);
	REQUIRE_FALSE(f.call("material_connect", json{
		{ "path", path }, { "srcNode", ids.tint }, { "srcPin", 0 },
		{ "dstNode", callId }, { "dstPin", "A" } }).isError);
	REQUIRE_FALSE(f.call("material_connect", json{
		{ "path", path }, { "srcNode", ids.metal }, { "srcPin", 0 },
		{ "dstNode", callId }, { "dstPin", "B" } }).isError);
	REQUIRE_FALSE(f.call("material_connect", json{
		{ "path", path }, { "srcNode", callId }, { "srcPin", "Out" },
		{ "dstNode", ids.out }, { "dstPin", "Emissive" } }).isError);

	// ── Re-bound to the function with NO inputs: both input wires go ─────────
	const ToolResult rb = f.call("material_set_node", json{
		{ "path", path }, { "id", callId }, { "s", "Materials/Fn.hasset" } });
	REQUIRE_FALSE(rb.isError);
	CHECK(rb.content.at("node").at("function") == "Materials/Fn.hasset");
	CHECK(rb.content.at("node").at("inputs").empty());
	REQUIRE(rb.content.at("droppedLinks").size() == 2);
	for (const json& l : rb.content.at("droppedLinks")) CHECK(l.at("dstNode") == callId);
	// The output wire survived: one output before, one after.
	HE::MaterialGraph saved;
	REQUIRE(savedGraphOf(f.root, path, saved));
	CHECK(savedLinkInto(saved, callId, 0) == nullptr);
	CHECK(savedLinkInto(saved, callId, 1) == nullptr);
	const HE::MatGraphLink* emissive = savedLinkInto(saved, ids.out, HE::kMatOutputEmissivePin);
	REQUIRE(emissive != nullptr);
	CHECK(emissive->srcNode == callId);
	// What the asset's own regenerate (with the content manager as function
	// loader) made of it: FnTint, declared inside the function, is a slot now.
	const ToolResult info = f.call("material_info", json{ { "path", path } });
	REQUIRE_FALSE(info.isError);
	const json* fnTint = findParam(info.content.at("params"), "FnTint");
	REQUIRE(fnTint != nullptr);
	CHECK(fnTint->at("inGraph") == false);

	// ── Re-binds that are refused, the file untouched ────────────────────────
	const std::string before = f.bytes(path);
	auto refused = [&](json args, const char* code) {
		const ToolResult r = f.call("material_set_node", args);
		CAPTURE(args.dump());
		CHECK(r.isError);
		CHECK(r.errorCode == code);
		CHECK(f.bytes(path) == before);
	};
	refused(json{ { "path", path }, { "id", callId }, { "s", "" } },                         "invalid_payload");
	refused(json{ { "path", path }, { "id", callId }, { "s", "Materials/Ghost.hasset" } },  "not_found");
	refused(json{ { "path", path }, { "id", callId }, { "s", path } },                       "invalid_path");
	refused(json{ { "path", path }, { "id", callId }, { "s", "Materials/FnStub.hasset" } }, "invalid_payload");

	// ── A layer blend: Grass / Rock / Snow, a constant into each ─────────────
	const ToolResult lb = f.call("material_add_node", json{
		{ "path", path }, { "type", "LandscapeLayerBlend" }, { "s", "Grass\nRock\nSnow" } });
	REQUIRE_FALSE(lb.isError);
	const int lbId = lb.content.at("node").at("id");
	REQUIRE(lb.content.at("node").at("inputs").size() == 3);
	int constOf[3] = { -1, -1, -1 };
	for (int k = 0; k < 3; ++k)
	{
		const ToolResult c = f.call("material_set_pin_default", json{
			{ "path", path }, { "node", lbId }, { "pin", k },
			{ "value", json::array({ 0.1 * (k + 1), 0.0, 0.0 }) } });
		REQUIRE_FALSE(c.isError);
		constOf[k] = c.content.at("constantNode").at("id");
	}

	// Rock removed: its wire goes, Snow's wire slides from pin 2 to pin 1.
	const ToolResult rl = f.call("material_set_node", json{
		{ "path", path }, { "id", lbId }, { "s", "Grass\nSnow" } });
	REQUIRE_FALSE(rl.isError);
	REQUIRE(rl.content.at("node").at("inputs").size() == 2);
	CHECK(rl.content.at("node").at("layers") == json::array({ "Grass", "Snow" }));
	REQUIRE(rl.content.at("droppedLinks").size() == 1);
	CHECK(rl.content.at("droppedLinks")[0].at("srcNode") == constOf[1]);
	CHECK(rl.content.at("droppedLinks")[0].at("dstPin") == 1);
	REQUIRE(rl.content.at("movedLinks").size() == 1);
	CHECK(rl.content.at("movedLinks")[0].at("srcNode") == constOf[2]);
	CHECK(rl.content.at("movedLinks")[0].at("fromPin") == 2);
	CHECK(rl.content.at("movedLinks")[0].at("toPin") == 1);
	CHECK(rl.content.at("movedLinks")[0].at("layer") == "Snow");
	REQUIRE(savedGraphOf(f.root, path, saved));
	const HE::MatGraphLink* grass = savedLinkInto(saved, lbId, 0);
	const HE::MatGraphLink* snow  = savedLinkInto(saved, lbId, 1);
	REQUIRE(grass != nullptr);
	REQUIRE(snow != nullptr);
	CHECK(grass->srcNode == constOf[0]);
	CHECK(snow->srcNode == constOf[2]);
	CHECK(savedLinkInto(saved, lbId, 2) == nullptr);
	// The orphaned constant is still a node (the delete is not this tool's).
	CHECK(saved.findNode(constOf[1]) != nullptr);

	// A pure rename keeps every wire where it is — the panel leaves a wire on
	// its pin through a rename too; nobody claimed index 1 by name, so Snow's
	// wire is now Ice's.
	const ToolResult rn = f.call("material_set_node", json{
		{ "path", path }, { "id", lbId }, { "s", "Grass\nIce" } });
	REQUIRE_FALSE(rn.isError);
	CHECK(rn.content.at("droppedLinks").empty());
	CHECK_FALSE(rn.content.contains("movedLinks"));
	REQUIRE(rn.content.at("renamedLayers").size() == 1);
	CHECK(rn.content.at("renamedLayers")[0].at("srcNode") == constOf[2]);
	CHECK(rn.content.at("renamedLayers")[0].at("pin") == 1);
	CHECK(rn.content.at("renamedLayers")[0].at("from") == "Snow");
	CHECK(rn.content.at("renamedLayers")[0].at("to") == "Ice");
	REQUIRE(savedGraphOf(f.root, path, saved));
	REQUIRE(savedLinkInto(saved, lbId, 0) != nullptr);
	CHECK(savedLinkInto(saved, lbId, 0)->srcNode == constOf[0]);
	REQUIRE(savedLinkInto(saved, lbId, 1) != nullptr);
	CHECK(savedLinkInto(saved, lbId, 1)->srcNode == constOf[2]);

	// Dropping the last layer: its pin is gone, so its wire is.
	const ToolResult rd = f.call("material_set_node", json{
		{ "path", path }, { "id", lbId }, { "s", "Grass" } });
	REQUIRE_FALSE(rd.isError);
	REQUIRE(rd.content.at("droppedLinks").size() == 1);
	CHECK(rd.content.at("droppedLinks")[0].at("srcNode") == constOf[2]);
	CHECK_FALSE(rd.content.contains("renamedLayers"));
	REQUIRE(savedGraphOf(f.root, path, saved));
	CHECK(savedLinkInto(saved, lbId, 1) == nullptr);
	REQUIRE(savedLinkInto(saved, lbId, 0) != nullptr);
}

TEST_CASE("mcp material tools: set_node refuses the wrong payloads and every gate, without writing")
{
	Fixture f("setnode_gates");
	f.writeMaterial("Materials/Rock.hasset", makeParamGraph());
	f.writeFunction("Materials/Fn.hasset", makeFunctionGraph());
	f.writeStub("Materials/Stub.hasset", HE::AssetType::Material);
	f.writeStub("Widgets/HUD.hasset", HE::AssetType::Widget);
	REQUIRE_FALSE(f.call("material_create_instance", json{
		{ "parent", "Materials/Rock.hasset" }, { "path", "Materials/Rock_Inst.hasset" } }).isError);
	const std::string path = "Materials/Rock.hasset";
	const ParamIds ids = paramIdsOf(f, path);
	const ToolResult add = f.call("material_add_node", json{ { "path", path }, { "type", "Multiply" } });
	REQUIRE_FALSE(add.isError);
	const int mulId = add.content.at("node").at("id");
	const ToolResult tex = f.call("material_add_node", json{ { "path", path }, { "type", "TextureSample" } });
	REQUIRE_FALSE(tex.isError);
	const int texId = tex.content.at("node").at("id");

	const std::string before = f.bytes(path);
	auto refused = [&](json args, const char* code) {
		if (!args.contains("path")) args["path"] = path;
		const ToolResult r = f.call("material_set_node", args);
		CAPTURE(args.dump());
		CHECK(r.isError);
		CHECK(r.errorCode == code);
		CHECK(f.bytes(path) == before);
	};
	refused(json{ { "id", 9999 }, { "p", json::array({ 1.0 }) } },                   "not_found");
	refused(json{ { "p", json::array({ 1.0 }) } },                                   "invalid_payload");
	refused(json{ { "id", ids.metal } },                                             "invalid_payload");   // nothing to set
	refused(json{ { "id", ids.metal }, { "type", "ConstFloat" }, { "p", json::array({ 1.0 }) } },
	        "invalid_payload");   // a type does not change
	refused(json{ { "id", mulId }, { "p", json::array({ 1.0 }) } },                  "invalid_payload");   // carries no values
	refused(json{ { "id", ids.metal }, { "p", json::array() } },                     "invalid_payload");
	refused(json{ { "id", ids.metal }, { "p", json::array({ 1, 2, 3, 4, 5 }) } },    "invalid_payload");
	refused(json{ { "id", ids.metal }, { "p", "one" } },                             "invalid_payload");
	refused(json{ { "id", ids.tint }, { "min", 0.0 }, { "max", 1.0 } },              "invalid_payload");   // no range on a colour
	refused(json{ { "id", ids.metal }, { "min", 1.0 }, { "max", 1.0 } },             "invalid_payload");   // min < max
	refused(json{ { "id", ids.metal }, { "min", 0.0 } },                             "invalid_payload");   // both or neither
	refused(json{ { "id", ids.metal }, { "min", 0.0 }, { "max", 1.0 }, { "p", json::array({ 0.5, 0.0 }) } },
	        "invalid_payload");   // the range twice
	refused(json{ { "id", mulId }, { "group", "Math" } },                            "invalid_payload");   // metadata on a non-parameter
	refused(json{ { "id", mulId }, { "position", json::array({ 1.0 }) } },           "invalid_payload");
	refused(json{ { "id", texId }, { "s", "Widgets/HUD.hasset" } },                  "invalid_path");
	refused(json{ { "id", texId }, { "s", "Textures/Ghost.hasset" } },               "not_found");
	// The same type name is accepted — a client echoing what graph_info said.
	{
		const ToolResult ok = f.call("material_set_node", json{
			{ "path", path }, { "id", mulId }, { "type", "Multiply" },
			{ "position", json::array({ 5.0, 5.0 }) } });
		REQUIRE_FALSE(ok.isError);
		CHECK(ok.content.at("node").at("x") == 5.0f);
	}

	// ── The gates ────────────────────────────────────────────────────────────
	const json base{ { "path", path }, { "id", ids.metal }, { "p", json::array({ 0.55 }) } };
	auto gate = [&](const char* code, const std::string& atPath = {}) {
		json args = base;
		if (!atPath.empty()) args["path"] = atPath;
		const std::string b = f.bytes(args["path"].get<std::string>());
		const ToolResult r = f.call("material_set_node", args);
		CAPTURE(code);
		CHECK(r.isError);
		CHECK(r.errorCode == code);
		CHECK(f.bytes(args["path"].get<std::string>()) == b);
	};
	f.playing = true;   gate("play_mode");        f.playing = false;
	f.lockedRel = path; gate("locked_by_other");  f.lockedRel.clear();
	f.dirtyRel = path; f.openRel = path;
	gate("dirty");
	f.dirtyRel.clear(); f.openRel.clear();
	gate("no_graph",     "Materials/Rock_Inst.hasset");
	gate("no_graph",     "Materials/Stub.hasset");
	gate("invalid_path", "Materials/Fn.hasset");
	gate("not_found",    "Materials/Ghost.hasset");
	{
		const fs::path engineRoot = f.root / "__engine";
		fs::create_directories(engineRoot / "Materials");
		f.content.setEngineContentRoot(engineRoot.string());
		const fs::path abs = engineRoot / "Materials/Default.hasset";
		REQUIRE(HE::Ed::writeAssetStub(abs.string(), "Engine/Materials/Default.hasset",
		                               "Default", HE::AssetType::Material));
		EditorAssetTypeCache::invalidate(abs.string());
		gate("read_only", "Engine/Materials/Default.hasset");
	}

	// A CLEAN tab is told to re-read.
	f.openRel = path;
	CHECK(f.reloadCalls == 0);
	const ToolResult r = f.call("material_set_node", base);
	REQUIRE_FALSE(r.isError);
	CHECK(r.content.at("reloadedInEditor") == true);
	CHECK(f.reloadCalls == 1);
}

#if defined(HE_TESTS_HAVE_SHADERC)
TEST_CASE("mcp material tools: a rewired template still cross-compiles")
{
	// The wiring tools change what the codegen emits more than add/remove do:
	// a coerced wire and a constant both land in the shader text. The panel's
	// own check, on a shipped template rewired three ways.
	Fixture f("wire_compile");
	const ToolResult made = f.call("material_create", json{
		{ "path", "Materials/Rewired.hasset" }, { "template", "OpaquePBR" } });
	REQUIRE_FALSE(made.isError);
	const std::string path = "Materials/Rewired.hasset";
	const ToolResult info = f.call("material_graph_info", json{ { "path", path } });
	REQUIRE_FALSE(info.isError);
	const int out = nodeOfType(info.content.at("nodes"), "Output")->at("id");
	int baseColor = -1;
	for (const json& n : info.content.at("nodes"))
		if (n.contains("paramName") && n.at("paramName") == "BaseColor") baseColor = n.at("id");
	REQUIRE(baseColor > 0);

	// The colour parameter (vec3) onto Metallic (float) — coerced; Roughness
	// unwired and given a constant; Normal given a constant vector.
	REQUIRE_FALSE(f.call("material_connect", json{
		{ "path", path }, { "srcNode", baseColor }, { "srcPin", 0 },
		{ "dstNode", out }, { "dstPin", "Metallic" } }).isError);
	REQUIRE_FALSE(f.call("material_disconnect", json{
		{ "path", path }, { "dstNode", out }, { "dstPin", "Roughness" } }).isError);
	REQUIRE_FALSE(f.call("material_set_pin_default", json{
		{ "path", path }, { "node", out }, { "pin", "Roughness" }, { "value", 0.3 } }).isError);
	REQUIRE_FALSE(f.call("material_set_pin_default", json{
		{ "path", path }, { "node", out }, { "pin", "Normal" },
		{ "value", json::array({ 0.0, 0.0, 1.0 }) } }).isError);

	HE::MaterialGraph g;
	REQUIRE(savedGraphOf(f.root, path, g));
	const HE::MatShaderGen gen = HE::generateFragment(g);
	REQUIRE_FALSE(gen.glsl.empty());
	bool sawRough = false, sawMetal = false;
	for (const HE::MatParamSlot& s : gen.params)
	{
		if (s.name == "Roughness") sawRough = true;
		if (s.name == "Metallic")  sawMetal = true;
	}
	CHECK_FALSE(sawRough);
	CHECK_FALSE(sawMetal);
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	const uint64_t hash = std::hash<std::string>{}(gen.glsl);
	const auto& msl = lib.fragment(hash, gen.glsl, B::Metal);
	CHECK_MESSAGE(msl.ok, "MSL compile failed: ", msl.log);
	const auto& gl = lib.fragment(hash, gen.glsl, B::GLSL410);
	CHECK_MESSAGE(gl.ok, "GLSL compile failed: ", gl.log);
}

TEST_CASE("mcp material tools: a template re-moded and re-valued by set_node still cross-compiles")
{
	// set_node's Output branch is the one that changes the shader's SHAPE (a
	// Masked material gains the discard, an unlit one loses the lighting tail):
	// the shipped Opaque template switched to Masked + unlit, a Param node's
	// default moved, then the panel's own cross-compile check.
	Fixture f("setnode_compile");
	const ToolResult made = f.call("material_create", json{
		{ "path", "Materials/Remoded.hasset" }, { "template", "OpaquePBR" } });
	REQUIRE_FALSE(made.isError);
	const std::string path = "Materials/Remoded.hasset";
	const ToolResult info = f.call("material_graph_info", json{ { "path", path } });
	REQUIRE_FALSE(info.isError);
	const int out = nodeOfType(info.content.at("nodes"), "Output")->at("id");
	int roughness = -1;
	for (const json& n : info.content.at("nodes"))
		if (n.contains("paramName") && n.at("paramName") == "Roughness") roughness = n.at("id");
	REQUIRE(roughness > 0);

	REQUIRE_FALSE(f.call("material_set_node", json{
		{ "path", path }, { "id", out }, { "blendMode", "Masked" }, { "p", json::array({ 0.0 }) } }).isError);
	REQUIRE_FALSE(f.call("material_set_node", json{
		{ "path", path }, { "id", roughness }, { "p", json::array({ 0.15 }) } }).isError);

	HE::MaterialGraph g;
	REQUIRE(savedGraphOf(f.root, path, g));
	const HE::MatShaderGen gen = HE::generateFragment(g);
	REQUIRE_FALSE(gen.glsl.empty());
	CHECK(gen.blendMode == static_cast<int>(HE::MatBlendMode::Masked));
	bool sawRough = false;
	for (const HE::MatParamSlot& s : gen.params)
		if (s.name == "Roughness") { sawRough = true; CHECK(s.value[0] == doctest::Approx(0.15f)); }
	CHECK(sawRough);
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	const uint64_t hash = std::hash<std::string>{}(gen.glsl);
	const auto& msl = lib.fragment(hash, gen.glsl, B::Metal);
	CHECK_MESSAGE(msl.ok, "MSL compile failed: ", msl.log);
	const auto& gl = lib.fragment(hash, gen.glsl, B::GLSL410);
	CHECK_MESSAGE(gl.ok, "GLSL compile failed: ", gl.log);
}
#endif

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
