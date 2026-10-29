// Landscape 3.0, step 2 (Thema 233): texture groups (Mountains / Forest / Grassland / Desert).
//
// A group is a landscape material with its own three arrays. The same graph skeleton builds all of
// them; a group differs by the files its roles read, the paint-layer names and the defaults of three
// parameters. Pinned here:
//   * the group table (ids, paths, lookups) and that Mountains is the legacy material;
//   * Forest / Grassland graphs: same skeleton, own arrays only, own names and defaults, they compile;
//   * the GI finds the albedo array of any group;
//   * the committed Forest / Grassland / Desert assets are what the builder makes, and bake on every backend.
#include "doctest.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/EngineDependencies.h>
#include <HorizonRendering/GiLandscape.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>
#include <MaterialGraph/MaterialGraph.h>
#include <MaterialGraph/ProceduralSurfaceFunctions.h>
#include <MaterialGraph/WeatherMaterialFunctions.h>
#include <Renderer/IRenderer.h>
#include <material/MaterialShaderBake.h>
#include <material/MaterialShaderLibrary.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using HE::AutoLandscapeGroupId;

namespace
{
const HE::AutoLandscapeGroup& row(AutoLandscapeGroupId id) { return HE::autoLandscapeGroup(id); }

const HE::MatGraphNode* paramNode(const HE::MaterialGraph& g, const char* name)
{
	for (const HE::MatGraphNode& n : g.nodes)
		if (n.type == HE::MatNodeType::ParamFloat && n.s == name) return &n;
	return nullptr;
}
} // namespace

TEST_CASE("Texture groups: the table is consistent and Mountains is the legacy material")
{
	const HE::AutoLandscapeGroup& m = row(AutoLandscapeGroupId::Mountains);
	CHECK(m.materialId == HE::kAutoLandscapeMaterialId);
	CHECK(std::string(m.materialPath) == HE::kAutoLandscapeMaterialPath);
	CHECK(std::string(m.arrayPaths[0]) == HE::kAutoLandscapeAlbedoArray);
	CHECK(std::string(m.arrayPaths[1]) == HE::kAutoLandscapeNormalArray);
	CHECK(std::string(m.arrayPaths[2]) == HE::kAutoLandscapeMaskArray);
	CHECK(std::string(m.paintLayerNames) == HE::kAutoLandscapePaintLayerNames);
	CHECK(m.rockSlope == 0.12f);
	CHECK(m.dirtAmount == 0.35f);
	CHECK(m.snowHeight == 60.0f);

	std::set<std::string> paths;
	std::set<std::pair<uint64_t, uint64_t>> ids;
	for (int i = 0; i < HE::kAutoLandscapeGroupCount; ++i)
	{
		const HE::AutoLandscapeGroup& g = HE::kAutoLandscapeGroups[i];
		CAPTURE(g.name);
		CHECK(static_cast<int>(g.id) == i);
		CHECK(HE::autoLandscapeGroupOfMaterial(g.materialId) == i);
		CHECK(ids.insert({ g.materialId.hi, g.materialId.lo }).second);
		CHECK(paths.insert(g.materialPath).second);
		CHECK(std::string(g.materialPath).rfind("Engine/Materials/", 0) == 0);
		for (int map = 0; map < 3; ++map)
		{
			CHECK(paths.insert(g.arrayPaths[map]).second);   // no array shared between groups
			CHECK(std::string(g.arrayPaths[map]).rfind("Engine/Textures/Landscape/", 0) == 0);
			int gotMap = -1;
			CHECK(HE::autoLandscapeGroupOfArray(g.arrayPaths[map], &gotMap) == i);
			CHECK(gotMap == map);
			// landscape_tex_gen writes the arrays under materialId.hi + 1 + map (0x400 block).
			CHECK(g.materialId.hi + 1 + map <= g.materialId.hi + 3);
		}
		// Six paint channels in the same role order in every group: Auto first, Puddles last.
		const std::vector<std::string> names = HE::matLandscapeLayerNames(g.paintLayerNames);
		REQUIRE(names.size() == static_cast<size_t>(HE::kAutoLandscapePaintLayerCount));
		CHECK(names.front() == "Auto");
		CHECK(names[static_cast<int>(HE::AutoLandscapePaintLayer::Snow)] == "Snow");
		CHECK(names.back() == "Puddles");
		for (int role = 0; role < HE::kAutoLandscapeRoleCount; ++role)
			CHECK(names[1 + role] == g.roleLabels[role]);
	}
	// One id, one asset: the ContentManager keeps a single entry per UUID whatever its type, so a group's
	// material (or one of its arrays) on the id of the weather functions, of landscape_tex_gen's textures
	// (0x400..0x411, 3 arrays among them) or of another group loads as the wrong thing.
	std::set<uint64_t> used;
	for (uint64_t hi = 0x400; hi <= 0x412; ++hi) used.insert(hi);   // textures, Mountains' material
	CHECK(used.count(HE::kAutoLandscapeMaterialId.hi) == 1);
	CHECK(used.insert(HE::kWeatherPuddlesFunctionId.hi).second);
	CHECK(used.insert(HE::kWeatherSnowFunctionId.hi).second);
	// The procedural surface functions (Thema 329, step 2) took 0x41D / 0x41E: the two ids left below the
	// loose maps (0x420..) after the Forest and Grassland blocks; 0x41F stays free.
	CHECK(used.insert(HE::kProceduralSnowFunctionId.hi).second);
	CHECK(used.insert(HE::kProceduralSandFunctionId.hi).second);
	// landscape_tex_gen's loose maps (0x420..0x42E) and variants (0x430..0x498: 8 variants of 5 layers),
	// which the groups after Grassland must not land on. Forest and Grassland sit below them, Desert above.
	for (uint64_t hi = 0x420; hi <= 0x498; ++hi) used.insert(hi);
	for (int i = 1; i < HE::kAutoLandscapeGroupCount; ++i)
	{
		const HE::AutoLandscapeGroup& g = HE::kAutoLandscapeGroups[i];
		CHECK(g.materialId.hi < 0x500);                     // the weather sounds start at 0x500
		for (int k = 0; k < 4; ++k)
			CHECK_MESSAGE(used.insert(g.materialId.hi + k).second, g.name, ": id ", g.materialId.hi + k, " is taken");
	}
	// landscape_tex_gen's per-group loose Roughness/AO/Height (--loose --group, Thema 233 Schritt 8):
	// kLooseGroupIndexBase 0x4A8..0x4CB (3 groups * 4 roles * 3 maps), right after the 0x4A4..0x4A7 a
	// fifth group's material + arrays would take.
	for (uint64_t hi = 0x4A8; hi <= 0x4CB; ++hi) CHECK(used.insert(hi).second);
	CHECK(HE::autoLandscapeGroupOfArray("Engine/Textures/Landscape/T_Landscape_Rock_Albedo.hasset") == -1);
	CHECK(HE::autoLandscapeGroupOfArray(nullptr) == -1);
	// A terrain that names no landscape group: a custom material, or none at all.
	CHECK(HE::autoLandscapeGroupOfMaterial(HE::UUID{}) == -1);
	CHECK(HE::autoLandscapeGroupOfMaterial(HE::UUID{ 0x0000000000000413ULL, 1 }) == -1);
}

TEST_CASE("Texture groups: Mountains is the default, Forest, Grassland and Desert keep the skeleton")
{
	const HE::AutoLandscapeGraph legacy = HE::buildAutoLandscapeGraph();
	const std::string legacyJson = HE::materialGraphToJson(legacy.graph);
	CHECK(HE::materialGraphToJson(HE::buildAutoLandscapeGraph(HE::AutoLandscapeView::Lit, AutoLandscapeGroupId::Mountains).graph)
	      == legacyJson);

	for (AutoLandscapeGroupId id : { AutoLandscapeGroupId::Forest, AutoLandscapeGroupId::Grassland, AutoLandscapeGroupId::Desert })
	{
		const HE::AutoLandscapeGroup& g = row(id);
		CAPTURE(g.name);
		const HE::AutoLandscapeGraph a = HE::buildAutoLandscapeGraph(HE::AutoLandscapeView::Lit, id);
		CHECK(HE::materialGraphToJson(a.graph) != legacyJson);

		if (!g.proceduralSand)
		{
			// Same nodes, same wiring: only strings and three default values differ.
			REQUIRE(a.graph.nodes.size() == legacy.graph.nodes.size());
			CHECK(a.graph.links.size() == legacy.graph.links.size());
			for (size_t i = 0; i < a.graph.nodes.size(); ++i)
				CHECK(a.graph.nodes[i].type == legacy.graph.nodes[i].type);
		}
		else
		{
			// The Desert's ground is MF_ProceduralSand (Thema 329 step 2) instead of three array reads
			// on one bombing grid: fewer nodes, the rest of the skeleton (parameters, paint layers,
			// dirt / rock / snow / puddles, the Dirt and Rock reads) unchanged — checked below.
			CHECK(a.graph.nodes.size() != legacy.graph.nodes.size());
			int sandCalls = 0, grassSlices = 0;
			for (const HE::MatGraphNode& n : a.graph.nodes)
				if (n.type == HE::MatNodeType::FunctionCall && n.s == HE::kProceduralSandFunctionPath) ++sandCalls;
			for (const HE::MatGraphNode& n : a.graph.nodes)
				if (n.type == HE::MatNodeType::TextureArraySample || n.type == HE::MatNodeType::NormalMapArraySample)
					for (const HE::MatGraphLink& l : a.graph.links)
						if (l.dstNode == n.id && l.dstPin == 1)
							grassSlices += static_cast<int>(a.graph.findNode(l.srcNode)->p[0]) == static_cast<int>(HE::AutoLandscapeLayer::Grass);
			CHECK(sandCalls == 1);
			CHECK(grassSlices == 0);   // the ground's slice is never read
		}

		const HE::MatShaderGen gen = HE::generateFragment(a.graph, HE::weatherFunctionLoader());
		REQUIRE_FALSE(gen.glsl.empty());
		CHECK(gen.glsl.find("vec3(1.0, 0.0, 1.0)") == std::string::npos);
		std::vector<std::string> tex = gen.textures, want = { g.arrayPaths[0], g.arrayPaths[1], g.arrayPaths[2] };
		std::sort(tex.begin(), tex.end());
		std::sort(want.begin(), want.end());
		CHECK(tex == want);                       // its own arrays and not one of Mountains'
		CHECK(gen.textureArrayMask == 0x7u);
		CHECK(gen.params.size() == static_cast<size_t>(HE::autoLandscapeParamCount(g)));

		int blends = 0, legacyBlends = 0;
		for (const HE::MatGraphNode& n : a.graph.nodes)
			if (n.type == HE::MatNodeType::LandscapeLayerBlend) { ++blends; CHECK(n.s == g.paintLayerNames); }
		for (const HE::MatGraphNode& n : legacy.graph.nodes)
			if (n.type == HE::MatNodeType::LandscapeLayerBlend) ++legacyBlends;
		CHECK(blends == legacyBlends);
		CHECK(blends > 0);

		REQUIRE(paramNode(a.graph, HE::kAutoLandscapeParamRockSlope));
		CHECK(paramNode(a.graph, HE::kAutoLandscapeParamRockSlope)->p[0] == g.rockSlope);
		CHECK(paramNode(a.graph, HE::kAutoLandscapeParamDirtAmount)->p[0] == g.dirtAmount);
		CHECK(paramNode(a.graph, HE::kAutoLandscapeParamSnowHeight)->p[0] == g.snowHeight);
		// The parameters a group does not tune keep the Mountains value.
		CHECK(paramNode(a.graph, HE::kAutoLandscapeParamRockBlend)->p[0] == paramNode(legacy.graph, HE::kAutoLandscapeParamRockBlend)->p[0]);

		// Every debug view builds and compiles in a group too.
		for (HE::AutoLandscapeView v : { HE::AutoLandscapeView::MasksRockSnowWater, HE::AutoLandscapeView::MasksDirtWet,
		                                 HE::AutoLandscapeView::Normal, HE::AutoLandscapeView::Surface })
			CHECK_FALSE(HE::generateFragment(HE::buildAutoLandscapeGraph(v, id).graph, HE::weatherFunctionLoader()).glsl.empty());
	}
}

TEST_CASE("Procedural snow and sand: the look is an exposed parameter, packed into the slot budget")
{
	for (int i = 0; i < HE::kAutoLandscapeGroupCount; ++i)
	{
		const HE::AutoLandscapeGroup& g = HE::kAutoLandscapeGroups[i];
		CAPTURE(g.name);
		const HE::AutoLandscapeGraph a = HE::buildAutoLandscapeGraph(HE::AutoLandscapeView::Lit, static_cast<AutoLandscapeGroupId>(i));
		const HE::MatShaderGen gen = HE::generateFragment(a.graph, HE::weatherFunctionLoader());
		// Nothing baked for being over budget: every parameter is a real HeParams slot.
		CHECK(gen.params.size() == static_cast<size_t>(HE::autoLandscapeParamCount(g)));
		CHECK(gen.params.size() <= static_cast<size_t>(HE::kMatMaxParams));
		auto find = [&](const char* name) -> const HE::MatParamSlot* {
			for (const HE::MatParamSlot& s : gen.params) if (s.name == name) return &s;
			return nullptr;
		};
		const HE::MatParamSlot* color = find(HE::kAutoLandscapeParamSnowColor);
		const HE::MatParamSlot* look  = find(HE::kAutoLandscapeParamSnowLook);
		REQUIRE(color);
		REQUIRE(look);
		CHECK(color->kind == HE::MatParamKind::Color);
		CHECK(look->kind == HE::MatParamKind::Vec4);
		// The defaults are the functions' own (Grain, Drift, Glitter, Softness).
		CHECK(look->value[0] == 0.5f);
		CHECK(look->value[1] == 0.5f);
		CHECK(look->value[2] == 0.35f);
		CHECK(look->value[3] == 0.5f);
		const HE::MatParamSlot* sand = find(HE::kAutoLandscapeParamSandLook);
		CHECK((sand != nullptr) == g.proceduralSand);
		if (sand)
		{
			CHECK(sand->kind == HE::MatParamKind::Vec4);
			CHECK(sand->value[0] == 0.03f);
			CHECK(sand->value[1] == 0.5f);
			CHECK(sand->value[2] == 0.3f);
			CHECK(sand->value[3] == 0.3f);
		}
		// The functions' pins are fed by the parameters (through Split RGBA), not by constants.
		CHECK(gen.glsl.find("// param: Snow Look") != std::string::npos);
	}
}

TEST_CASE("Texture groups: the GI finds the albedo array of any group")
{
	for (int i = 0; i < HE::kAutoLandscapeGroupCount; ++i)
	{
		const HE::AutoLandscapeGroup& g = HE::kAutoLandscapeGroups[i];
		CAPTURE(g.name);
		MaterialAsset ma;
		for (const char* name : { HE::kAutoLandscapeParamRockSlope, HE::kAutoLandscapeParamRockBlend,
		                          HE::kAutoLandscapeParamDirtAmount, HE::kAutoLandscapeParamSnowHeight,
		                          HE::kAutoLandscapeParamSnowBlend, HE::kAutoLandscapeParamSnowMaxSlope,
		                          HE::kAutoLandscapeParamPuddleAmount, HE::kAutoLandscapeParamPuddleMaxSlope })
		{
			ma.graphParamNames.push_back(name);
			ma.shaderParamData.insert(ma.shaderParamData.end(), { 0.2f, 0.0f, 0.0f, 0.0f });
		}
		ma.graphTexturePaths = { g.arrayPaths[1], g.arrayPaths[0], g.arrayPaths[2] };
		HE::GiLandscape L;
		CHECK(HE::giAutoLandscapeParams(ma, {}, L) == 1);          // wherever the albedo array sits
		// The normal / mask arrays alone are not the albedo array.
		ma.graphTexturePaths = { g.arrayPaths[1], g.arrayPaths[2] };
		CHECK(HE::giAutoLandscapeParams(ma, {}, L) == -1);
		// Another group's albedo array is still an auto landscape's.
		ma.graphTexturePaths = { row(AutoLandscapeGroupId::Forest).arrayPaths[0] };
		CHECK(HE::giAutoLandscapeParams(ma, {}, L) == 0);
	}
}

#ifdef HE_EDITOR_DEPS_DIR
TEST_CASE("The shipped Forest, Grassland and Desert materials are exactly what the builder makes, and bake on every backend")
{
	// Generated by `landscape_tex_gen <dir> --material --group <G>` and committed.
	const std::filesystem::path root = std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent";
	const std::filesystem::path dir  = root / "Materials";
	for (AutoLandscapeGroupId id : { AutoLandscapeGroupId::Mountains, AutoLandscapeGroupId::Forest, AutoLandscapeGroupId::Grassland, AutoLandscapeGroupId::Desert })
	{
		const HE::AutoLandscapeGroup& g = row(id);
		CAPTURE(g.name);
		const std::string file = std::filesystem::path(g.materialPath).filename().string();
		REQUIRE(std::filesystem::exists(dir / file));
		ContentManager cm(dir.string());
		cm.setEngineContentRoot(root.string());
		const HE::UUID loaded = cm.loadAsset(file);
		CHECK(loaded == g.materialId);
		const MaterialAsset* m = cm.getMaterial(loaded);
		REQUIRE(m);
		CHECK_MESSAGE(m->nodeGraphJson == HE::materialGraphToJson(HE::buildAutoLandscapeGraph(HE::AutoLandscapeView::Lit, id).graph),
		              "re-run: landscape_tex_gen EditorDeps/EngineContent/Materials --material --group ", g.name);
		std::vector<std::string> tex = m->graphTexturePaths, want = { g.arrayPaths[0], g.arrayPaths[1], g.arrayPaths[2] };
		std::sort(tex.begin(), tex.end());
		std::sort(want.begin(), want.end());
		CHECK(tex == want);
		CHECK(m->graphLayerNames == HE::matLandscapeLayerNames(g.paintLayerNames));
		std::string have;
		for (const std::string& pn : m->graphParamNames) have += pn + "; ";
		CHECK_MESSAGE(m->graphParamNames.size() == static_cast<size_t>(HE::autoLandscapeParamCount(g)), have);
		REQUIRE(!m->customShaderFragGlsl.empty());

		// The packer ships the fragment text stored in the file: it must be what the codegen makes today.
		std::ifstream in(dir / file, std::ios::binary);
		const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		CHECK_MESSAGE(bytes.find(m->customShaderFragGlsl) != std::string::npos,
		              "re-run: landscape_tex_gen EditorDeps/EngineContent/Materials --material --group ", g.name);

		// The EngineContent resolver (editor prefetch, project export) finds the arrays by scanning the
		// material's bytes for "Engine/….hasset" strings: every one of this group's arrays has to be in there.
		const std::vector<std::string> refs = HE::EngineDeps::findEngineReferences(bytes.data(), bytes.size());
		for (const char* arr : g.arrayPaths)
			CHECK_MESSAGE(std::find(refs.begin(), refs.end(), arr) != refs.end(), arr, " is not referenced by ", file);
		// ... and so is every material function the graph calls (Thema 329 step 2: snow in every group, sand in
		// the Desert): a function the resolver does not find would not be fetched, and the call is magenta.
		std::vector<const char*> fns = { HE::kWeatherPuddlesFunctionPath, HE::kWeatherSnowFunctionPath, HE::kProceduralSnowFunctionPath };
		if (g.proceduralSand) fns.push_back(HE::kProceduralSandFunctionPath);
		for (const char* fn : fns)
			CHECK_MESSAGE(std::find(refs.begin(), refs.end(), fn) != refs.end(), fn, " is not referenced by ", file);

		// One variant per backend, the way the exporter bakes them (no GPU involved).
		HE::MaterialShaderLibrary lib;
		for (uint8_t v = 0; v <= static_cast<uint8_t>(HE::RendererBackend::Metal); ++v)
		{
			MaterialShaderVariant var;
			std::string error;
			std::vector<std::string> warnings;
			CHECK_MESSAGE(HE::bakeMaterialShaderVariant(lib, static_cast<HE::RendererBackend>(v),
			                                            m->customShaderFragGlsl, "", var, error, warnings),
			              "backend ", static_cast<int>(v), ": ", error);
		}
	}
}
#endif // HE_EDITOR_DEPS_DIR
