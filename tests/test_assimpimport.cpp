// FBX / OBJ / COLLADA import through Assimp (AssimpMeshImport, routed by
// MeshImporter). Compiled out without Assimp — the whole feature is — except
// for the first case, which pins the Import Asset dialog to the routing in
// EITHER build: with Assimp the dialog must offer the three formats, without
// it it must not, and must say why.
#include "doctest.h"
#include "TestFsUtil.h"
#include "AudioImporter.h"
#include "ImporterCommon.h"
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace
{
// The ';'-separated entries of an SDL dialog pattern.
std::vector<std::string> patternTokens(const char* pattern)
{
	std::vector<std::string> out;
	std::string_view rest(pattern);
	while (!rest.empty())
	{
		const size_t semi = rest.find(';');
		out.emplace_back(rest.substr(0, semi));
		if (semi == std::string_view::npos) break;
		rest.remove_prefix(semi + 1);
	}
	return out;
}
} // namespace

TEST_CASE("Import dialog: every offered extension imports, and the routing offers nothing more")
{
	using Importer::SourceFamily;
	const int count = static_cast<int>(SourceFamily::Count);
	std::vector<std::string> offered;
	for (int i = 0; i < count; ++i)
	{
		const auto family = static_cast<SourceFamily>(i);
		CAPTURE(i);
		CHECK(std::strlen(Importer::sourceFamilyLabel(family)) > 0);
		const std::vector<std::string> tokens = patternTokens(Importer::sourceFamilyPattern(family));
		CHECK_FALSE(tokens.empty());
		for (const std::string& ext : tokens)
		{
			CAPTURE(ext);
			// No dot, no upper case, no empty token — the form SDL filters take.
			CHECK(ext.find('.') == std::string::npos);
			CHECK(std::none_of(ext.begin(), ext.end(), [](unsigned char c) { return std::isupper(c); }));
			// Offered means importable — in this exact build.
			CHECK(Importer::isImportableSource("Some/File." + ext));
			CHECK(std::string(Importer::importBlockedReason("Some/File." + ext)).empty());
			// The audio row is a copy of AudioImporter's own list; keep them equal.
			if (family == SourceFamily::Audio)
				CHECK(AudioImporter::isSupportedSource("Some/File." + ext));
			offered.push_back(ext);
		}
	}
	// "All Supported Assets" is the union, nothing more and nothing less.
	std::vector<std::string> all = patternTokens(Importer::allSourcesPattern());
	std::sort(all.begin(), all.end());
	std::sort(offered.begin(), offered.end());
	CHECK(all == offered);

	// Every mesh extension the routing takes is offered under 3D Models — the
	// drift this test exists for: the dialog said "gltf;glb" long after
	// .fbx/.obj/.dae had become importable.
	const std::vector<std::string> meshes = patternTokens(Importer::sourceFamilyPattern(SourceFamily::Mesh));
	auto offers = [&](const char* ext) { return std::find(meshes.begin(), meshes.end(), ext) != meshes.end(); };
	CHECK(offers("gltf"));
	CHECK(offers("glb"));
#ifdef HE_HAVE_ASSIMP
	CHECK(offers("fbx"));
	CHECK(offers("obj"));
	CHECK(offers("dae"));
#else
	CHECK_FALSE(offers("fbx"));
	CHECK_FALSE(offers("obj"));
	CHECK_FALSE(offers("dae"));
	// Not importable, but not silently so either: the Content Browser greys the
	// Import item out with this sentence.
	for (const char* ext : { ".fbx", ".OBJ", ".dae" })
	{
		CAPTURE(ext);
		CHECK_FALSE(Importer::isImportableSource(fs::path("Some/Model") += ext));
		CHECK_FALSE(std::string(Importer::importBlockedReason(fs::path("Some/Model") += ext)).empty());
	}
#endif
	// A format the engine has never heard of is neither offered nor explained.
	CHECK_FALSE(Importer::isImportableSource("Some/Model.blend"));
	CHECK(std::string(Importer::importBlockedReason("Some/Model.blend")).empty());
	// "tga" must not be found inside "gltf": the pattern lookup is per token.
	CHECK_FALSE(Importer::isImportableSource("Some/File.lt"));
	CHECK_FALSE(Importer::isImportableSource("Some/File.gl"));
}

#ifdef HE_HAVE_ASSIMP

#include "AssimpMeshImport.h"
#include "MeshImporter.h"
#include <ContentManager/AssetRefRetarget.h>   // the rename half the editor does after fs::rename
#include <ContentManager/AssetRefScan.h>       // assetUuidOfFile — identity across a reimport
#include <MaterialGraph/MaterialGraph.h>

namespace
{
bool writeText(const fs::path& file, const std::string& text)
{
	std::error_code ec;
	fs::create_directories(file.parent_path(), ec);
	std::ofstream f(file);
	if (!f) return false;
	f << text;
	return true;
}

// A 1x1 red PNG (truecolor, 8 bit) — the smallest real image stb_image decodes.
// The PIXELS are irrelevant here: every material assertion below is about
// paths, pins and slots, so the same bytes stand in for every channel.
const uint8_t kPng1x1[] = {
	0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
	0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
	0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
	0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
	0x00, 0x03, 0x01, 0x01, 0x00, 0xF7, 0x03, 0x41, 0x43, 0x00, 0x00, 0x00,
	0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
};

bool writePng(const fs::path& file)
{
	std::error_code ec;
	fs::create_directories(file.parent_path(), ec);
	std::ofstream png(file, std::ios::binary);
	if (!png) return false;
	png.write(reinterpret_cast<const char*>(kPng1x1), static_cast<std::streamsize>(sizeof(kPng1x1)));
	return true;
}

// Loads a written material back the way the engine does, so the assertions run
// against the DESERIALIZED asset rather than the in-memory struct the importer
// happened to build.
struct LoadedMaterial
{
	ContentManager       cm;
	const MaterialAsset* mat = nullptr;
	explicit LoadedMaterial(const fs::path& contentRoot, const std::string& rel)
		: cm(contentRoot.string())
	{
		mat = cm.getMaterial(cm.loadAsset(rel));
	}
};

// The node the Output pin `pin` is driven by, or nullptr when unconnected.
const HE::MatGraphNode* pinSource(const HE::MaterialGraph& g, int outputNode, int pin)
{
	for (const HE::MatGraphLink& l : g.links)
		if (l.dstNode == outputNode && l.dstPin == pin)
			return g.findNode(l.srcNode);
	return nullptr;
}

int outputNodeId(const HE::MaterialGraph& g)
{
	for (const HE::MatGraphNode& n : g.nodes)
		if (n.type == HE::MatNodeType::Output) return n.id;
	return 0;
}

// The graph of a loaded material, or an empty one (with a failed REQUIRE).
HE::MaterialGraph graphOf(const LoadedMaterial& loaded)
{
	HE::MaterialGraph g;
	REQUIRE(loaded.mat != nullptr);
	REQUIRE(HE::materialGraphFromJson(loaded.mat->nodeGraphJson, g));
	return g;
}

// Index of the vertex sitting at `p` (±eps), or -1. Assimp's identical-vertex
// merge may reorder vertices, so tests locate them by position rather than
// assuming file order.
int vertexAt(const StaticMeshAsset& m, float x, float y, float z, float eps = 1e-4f)
{
	for (size_t v = 0; v + 2 < m.vertices.size(); v += 3)
		if (std::fabs(m.vertices[v] - x) < eps && std::fabs(m.vertices[v+1] - y) < eps
		    && std::fabs(m.vertices[v+2] - z) < eps)
			return static_cast<int>(v / 3);
	return -1;
}

void boundsOf(const StaticMeshAsset& m, float lo[3], float hi[3])
{
	for (int k = 0; k < 3; ++k) { lo[k] = 1e30f; hi[k] = -1e30f; }
	for (size_t v = 0; v + 2 < m.vertices.size(); v += 3)
		for (int k = 0; k < 3; ++k)
		{
			lo[k] = std::min(lo[k], m.vertices[v + k]);
			hi[k] = std::max(hi[k], m.vertices[v + k]);
		}
}

// Two triangles, each under its own `usemtl`, sharing one set of UVs. The UVs
// are deliberately asymmetric (0.25, 0.75) so a V flip would show.
const char* kTwoMaterialObj = R"(# HorizonEngine test fixture
mtllib two.mtl
v 0 0 0
v 1 0 0
v 0 1 0
v 2 0 0
v 3 0 0
v 2 1 0
vt 0.25 0.75
vt 1 0
vt 0 1
usemtl MatA
f 1/1 2/2 3/3
usemtl MatB
f 4/1 5/2 6/3
)";

const char* kTwoMaterialMtl = R"(newmtl MatA
Kd 1 0 0
newmtl MatB
Kd 0 0 1
)";

// One triangle in a Z-up document: the vertex on +Z must arrive on +Y.
const char* kZUpDae = R"(<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset>
    <unit name="meter" meter="1"/>
    <up_axis>Z_UP</up_axis>
  </asset>
  <library_geometries>
    <geometry id="tri" name="tri">
      <mesh>
        <source id="tri-positions">
          <float_array id="tri-positions-array" count="9">0 0 0 1 0 0 0 0 1</float_array>
          <technique_common>
            <accessor source="#tri-positions-array" count="3" stride="3">
              <param name="X" type="float"/>
              <param name="Y" type="float"/>
              <param name="Z" type="float"/>
            </accessor>
          </technique_common>
        </source>
        <vertices id="tri-vertices">
          <input semantic="POSITION" source="#tri-positions"/>
        </vertices>
        <triangles count="1">
          <input semantic="VERTEX" source="#tri-vertices" offset="0"/>
          <p>0 1 2</p>
        </triangles>
      </mesh>
    </geometry>
  </library_geometries>
  <library_visual_scenes>
    <visual_scene id="Scene" name="Scene">
      <node id="tri-node" name="tri">
        <instance_geometry url="#tri"/>
      </node>
    </visual_scene>
  </library_visual_scenes>
  <scene>
    <instance_visual_scene url="#Scene"/>
  </scene>
</COLLADA>
)";

// A minimal ASCII FBX 7.4 (the structure of Assimp's own test models, cut down
// to one Geometry + one Model): one triangle whose coordinates are `unit`
// long, in a document whose UnitScaleFactor is `unitScaleFactor`.
std::string fbxTriangle(double unitScaleFactor, double unit)
{
	const std::string u   = std::to_string(unit);
	const std::string usf = std::to_string(unitScaleFactor);
	return
"; FBX 7.4.0 project file\n"
"; ----------------------------------------------------\n"
"\n"
"FBXHeaderExtension:  {\n"
"\tFBXHeaderVersion: 1003\n"
"\tFBXVersion: 7400\n"
"\tCreator: \"HorizonEngine test fixture\"\n"
"}\n"
"GlobalSettings:  {\n"
"\tVersion: 1000\n"
"\tProperties70:  {\n"
"\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
"\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
"\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
"\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
"\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
"\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
"\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\"," + usf + "\n"
"\t}\n"
"}\n"
"Objects:  {\n"
"\tGeometry: 1000, \"Geometry::Tri\", \"Mesh\" {\n"
"\t\tVertices: *9 {\n"
"\t\t\ta: 0,0,0," + u + ",0,0,0," + u + ",0\n"
"\t\t}\n"
"\t\tPolygonVertexIndex: *3 {\n"
"\t\t\ta: 0,1,-3\n"
"\t\t}\n"
"\t\tGeometryVersion: 124\n"
"\t\tLayerElementUV: 0 {\n"
"\t\t\tVersion: 101\n"
"\t\t\tName: \"UVMap\"\n"
"\t\t\tMappingInformationType: \"ByPolygonVertex\"\n"
"\t\t\tReferenceInformationType: \"Direct\"\n"
"\t\t\tUV: *6 {\n"
"\t\t\t\ta: 0.25,0.75,1,0,0,1\n"
"\t\t\t}\n"
"\t\t}\n"
"\t\tLayer: 0 {\n"
"\t\t\tVersion: 100\n"
"\t\t\tLayerElement:  {\n"
"\t\t\t\tType: \"LayerElementUV\"\n"
"\t\t\t\tTypedIndex: 0\n"
"\t\t\t}\n"
"\t\t}\n"
"\t}\n"
"\tModel: 2000, \"Model::Tri\", \"Mesh\" {\n"
"\t\tVersion: 232\n"
"\t\tShading: T\n"
"\t\tCulling: \"CullingOff\"\n"
"\t}\n"
"}\n"
"Connections:  {\n"
"\tC: \"OO\",2000,0\n"
"\tC: \"OO\",1000,2000\n"
"}\n";
}
} // namespace

TEST_CASE("Assimp routing: .fbx/.obj/.dae are importable mesh sources, any case")
{
	for (const char* ext : { ".fbx", ".obj", ".dae", ".FBX", ".Obj", ".DAE" })
	{
		CHECK(Importer::isAssimpSource(fs::path("Some/Model") += ext));
		CHECK(Importer::isImportableSource(fs::path("Some/Model") += ext));
	}
	CHECK_FALSE(Importer::isAssimpSource("Some/Model.gltf"));
	CHECK_FALSE(Importer::isAssimpSource("Some/Model.blend"));
	// The skin probe is glTF-only and must not open other formats at all — a
	// path that does not exist would otherwise be a parse failure, not a "no".
	CHECK_FALSE(Importer::gltfHasSkin("Does/Not/Exist.fbx"));
}

TEST_CASE("OBJ import: one section per usemtl, UVs taken verbatim (no V flip)")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_assimp_obj";
	he_test::removeAllQuiet(dir);
	REQUIRE(writeText(dir / "two.obj", kTwoMaterialObj));
	REQUIRE(writeText(dir / "two.mtl", kTwoMaterialMtl));
	const fs::path contentRoot = dir / "Content";

	auto mesh = MeshImporter::import(dir / "two.obj", contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	CHECK(mesh->path == "Imported/two.hasset");
	CHECK(mesh->vertices.size() == 6 * 3);
	CHECK(mesh->indices.size()  == 6);
	CHECK(mesh->normals.size()  == 6 * 3);   // generated: the OBJ has none
	CHECK(mesh->uvs.size()      == 6 * 2);

	// Two materials → two sections, contiguous and covering the buffer, each
	// bound to the MaterialAsset written for its usemtl — named after the OBJ
	// material — and the mesh binds section 0's, exactly the glTF rule.
	REQUIRE(mesh->sections.size() == 2);
	CHECK(mesh->sections[0].indexOffset == 0);
	CHECK(mesh->sections[0].indexCount  == 3);
	CHECK(mesh->sections[1].indexOffset == 3);
	CHECK(mesh->sections[1].indexCount  == 3);
	CHECK(mesh->sections[0].materialPath == "Imported/MatA.hasset");
	CHECK(mesh->sections[1].materialPath == "Imported/MatB.hasset");
	CHECK(mesh->materialPath == "Imported/MatA.hasset");
	CHECK(fs::exists(contentRoot / "Imported/MatA.hasset"));
	CHECK(fs::exists(contentRoot / "Imported/MatB.hasset"));

	// A Phong material's Kd is the base colour, and the absence of any PBR key
	// makes it a DIELECTRIC — Assimp's model has no metallic default, and glTF's
	// (metallic 1) would turn every OBJ into chrome.
	{
		LoadedMaterial a(contentRoot, "Imported/MatA.hasset");
		REQUIRE(a.mat != nullptr);
		CHECK(a.mat->baseColor[0] == doctest::Approx(1.0f));
		CHECK(a.mat->baseColor[1] == doctest::Approx(0.0f));
		CHECK(a.mat->baseColor[2] == doctest::Approx(0.0f));
		CHECK(a.mat->metallic  == doctest::Approx(0.0f));
		CHECK(a.mat->roughness == doctest::Approx(0.5f));   // no Ns → the middle of the range
		CHECK(a.mat->texturePaths.empty());
		const HE::MaterialGraph g = graphOf(a);
		const int out = outputNodeId(g);
		REQUIRE(out != 0);
		// Metallic and roughness are wired even without a texture: the Output
		// node's pin defaults are not the material's values.
		const HE::MatGraphNode* metal = pinSource(g, out, HE::kMatOutputMetallicPin);
		REQUIRE(metal != nullptr);
		CHECK(metal->type == HE::MatNodeType::ConstFloat);
		CHECK(metal->p[0] == doctest::Approx(0.0f));
		CHECK(pinSource(g, out, HE::kMatOutputRoughnessPin) != nullptr);
		const HE::MatGraphNode* base = pinSource(g, out, HE::kMatOutputBaseColorPin);
		REQUIRE(base != nullptr);
		CHECK(base->type == HE::MatNodeType::ConstColor);
		CHECK(base->p[0] == doctest::Approx(1.0f));
	}

	// The OBJ's vt (0.25, 0.75) on the origin vertex arrives unchanged: Assimp's
	// UV origin is bottom-left like the engine's, unlike glTF's.
	const int v0 = vertexAt(*mesh, 0, 0, 0);
	REQUIRE(v0 >= 0);
	CHECK(mesh->uvs[v0 * 2 + 0] == doctest::Approx(0.25f));
	CHECK(mesh->uvs[v0 * 2 + 1] == doctest::Approx(0.75f));

	// Generated normals: the triangles lie in the XY plane, CCW → +Z.
	CHECK(mesh->normals[v0 * 3 + 2] == doctest::Approx(1.0f));

	CHECK(fs::exists(contentRoot / "Imported" / "two.hasset"));
	he_test::removeAllQuiet(dir);
}

TEST_CASE("COLLADA import: Z_UP documents arrive Y-up")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_assimp_dae";
	he_test::removeAllQuiet(dir);
	REQUIRE(writeText(dir / "zup.dae", kZUpDae));
	const fs::path contentRoot = dir / "Content";

	auto mesh = MeshImporter::import(dir / "zup.dae", contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	CHECK(mesh->vertices.size() == 3 * 3);
	CHECK(mesh->indices.size()  == 3);
	REQUIRE(mesh->sections.size() == 1);
	CHECK(mesh->sections[0].indexCount == 3);

	// (0,0,1) in the Z-up file is (0,1,0) in the engine; nothing lands on +Z.
	CHECK(vertexAt(*mesh, 0, 1, 0) >= 0);
	CHECK(vertexAt(*mesh, 0, 0, 1) < 0);
	CHECK(vertexAt(*mesh, 1, 0, 0) >= 0);
	he_test::removeAllQuiet(dir);
}

// FBX geometry is in the file's units, UnitScaleFactor centimetres each: a
// metre-authored file (UnitScaleFactor 100, 1-unit triangle) and a
// centimetre-authored one (UnitScaleFactor 1, 100-unit triangle) are both 1 m
// long and must import as 1 m. This also PINS that Assimp v6.0.5 leaves the
// unit out of its root transform (AssimpMeshImport.cpp explains the float/double
// slip); an upgrade that starts baking it in shows up here as 100 m triangles.
TEST_CASE("FBX import: centimetre and metre documents both land in metres")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_assimp_fbx";
	he_test::removeAllQuiet(dir);
	REQUIRE(writeText(dir / "cm.fbx", fbxTriangle(/*unitScaleFactor=*/1.0,   /*unit=*/100.0)));
	REQUIRE(writeText(dir / "m.fbx",  fbxTriangle(/*unitScaleFactor=*/100.0, /*unit=*/1.0)));
	const fs::path contentRoot = dir / "Content";

	for (const char* name : { "cm.fbx", "m.fbx" })
	{
		CAPTURE(name);
		auto mesh = MeshImporter::import(dir / name, contentRoot, "Imported");
		REQUIRE(mesh != nullptr);
		CHECK(mesh->vertices.size() == 3 * 3);
		CHECK(mesh->indices.size()  == 3);
		REQUIRE(mesh->sections.size() == 1);
		// No material in the file: Assimp binds its own "DefaultMaterial", which is
		// written under the MESH's name — two such meshes in one folder must not
		// share (and keep rewriting) one DefaultMaterial.hasset.
		CHECK(mesh->materialPath == "Imported/" + fs::path(name).stem().string() + "_mat.hasset");
		CHECK(mesh->sections[0].materialPath == mesh->materialPath);
		CHECK_FALSE(fs::exists(contentRoot / "Imported/DefaultMaterial.hasset"));

		float lo[3], hi[3];
		boundsOf(*mesh, lo, hi);
		CHECK(lo[0] == doctest::Approx(0.0f)); CHECK(hi[0] == doctest::Approx(1.0f));
		CHECK(lo[1] == doctest::Approx(0.0f)); CHECK(hi[1] == doctest::Approx(1.0f));
		CHECK(lo[2] == doctest::Approx(0.0f)); CHECK(hi[2] == doctest::Approx(0.0f));

		// The UV layer is read and, as for OBJ, not flipped.
		const int v0 = vertexAt(*mesh, 0, 0, 0);
		REQUIRE(v0 >= 0);
		CHECK(mesh->uvs[v0 * 2 + 0] == doctest::Approx(0.25f));
		CHECK(mesh->uvs[v0 * 2 + 1] == doctest::Approx(0.75f));
	}

	// uniformScale multiplies the unit correction rather than replacing it.
	MeshImporter::ImportSettings settings;
	settings.uniformScale = 2.0f;
	auto scaled = MeshImporter::import(dir / "m.fbx", contentRoot, "Scaled", settings);
	REQUIRE(scaled != nullptr);
	float lo[3], hi[3];
	boundsOf(*scaled, lo, hi);
	CHECK(hi[0] == doctest::Approx(2.0f));
	CHECK(hi[1] == doctest::Approx(2.0f));
	he_test::removeAllQuiet(dir);
}

TEST_CASE("Assimp import: an unreadable file fails without an asset")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_assimp_bad";
	he_test::removeAllQuiet(dir);
	REQUIRE(writeText(dir / "garbage.fbx", "this is not an FBX file\n"));
	const fs::path contentRoot = dir / "Content";

	CHECK(MeshImporter::import(dir / "garbage.fbx", contentRoot, "Imported") == nullptr);
	CHECK_FALSE(fs::exists(contentRoot / "Imported" / "garbage.hasset"));
	CHECK(MeshImporter::import(dir / "missing.obj", contentRoot, "Imported") == nullptr);
	he_test::removeAllQuiet(dir);
}

// OBJ's PBR extension: separate metallic and roughness maps (map_Pm / map_Pr,
// each a grey image read from R), `norm` for the normal map, Ke as a constant
// emissive. Four distinct images fill the graph's four texture slots exactly.
TEST_CASE("OBJ PBR material: separate metallic/roughness maps, normal map, Kd not applied as tint")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_assimp_objpbr";
	he_test::removeAllQuiet(dir);
	for (const char* t : { "crate_base", "crate_normal", "crate_rough", "crate_metal" })
		REQUIRE(writePng(dir / (std::string(t) + ".png")));
	REQUIRE(writeText(dir / "crate.obj",
		"mtllib crate.mtl\n"
		"v 0 0 0\nv 1 0 0\nv 0 1 0\n"
		"vt 0 0\nvt 1 0\nvt 0 1\n"
		"usemtl M_Crate\n"
		"f 1/1 2/2 3/3\n"));
	REQUIRE(writeText(dir / "crate.mtl",
		"newmtl M_Crate\n"
		"Kd 0.8 0.8 0.8\n"          // Blender's lighting coefficient next to a map: NOT a tint
		"Ke 1 0 0\n"
		"Ns 250\n"
		"Pm 0.5\n"                  // explicit PBR factor: multiplies the metallic map
		"map_Kd crate_base.png\n"
		"norm crate_normal.png\n"
		"map_Pr crate_rough.png\n"
		"map_Pm crate_metal.png\n"));
	const fs::path contentRoot = dir / "Content";

	auto mesh = MeshImporter::import(dir / "crate.obj", contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	CHECK(mesh->materialPath == "Imported/M_Crate.hasset");
	REQUIRE(mesh->sections.size() == 1);
	CHECK(mesh->sections[0].materialPath == "Imported/M_Crate.hasset");

	// Every image became its own texture asset, named after the image FILE.
	for (const char* t : { "crate_base", "crate_normal", "crate_rough", "crate_metal" })
		CHECK(fs::exists(contentRoot / ("Imported/" + std::string(t) + ".hasset")));

	LoadedMaterial loaded(contentRoot, "Imported/M_Crate.hasset");
	REQUIRE(loaded.mat != nullptr);
	// The legacy heTex0 slot points at the base colour.
	REQUIRE(loaded.mat->texturePaths.size() == 1);
	CHECK(loaded.mat->texturePaths[0] == "Imported/crate_base.hasset");
	// Kd is dropped under a base-colour map (a Phong coefficient, not a tint);
	// the explicit Pm survives as the metallic factor.
	CHECK(loaded.mat->baseColor[0] == doctest::Approx(1.0f));
	CHECK(loaded.mat->metallic     == doctest::Approx(0.5f));
	// With a roughness MAP and no Pr, the factor is 1 so the map is not darkened
	// by the Ns-derived fallback.
	CHECK(loaded.mat->roughness    == doctest::Approx(1.0f));

	const HE::MaterialGraph g = graphOf(loaded);
	const int out = outputNodeId(g);
	REQUIRE(out != 0);

	// Base colour straight from the sampler — no Multiply by a 0.8 tint in between.
	const HE::MatGraphNode* base = pinSource(g, out, HE::kMatOutputBaseColorPin);
	REQUIRE(base != nullptr);
	CHECK(base->type == HE::MatNodeType::TextureSample);
	CHECK(base->s    == "Imported/crate_base.hasset");

	// Metallic: map × 0.5 (Multiply fed by the map's SplitRGBA); roughness: the
	// map's split directly (factor 1).
	const HE::MatGraphNode* metal = pinSource(g, out, HE::kMatOutputMetallicPin);
	REQUIRE(metal != nullptr);
	CHECK(metal->type == HE::MatNodeType::Multiply);
	const HE::MatGraphNode* rough = pinSource(g, out, HE::kMatOutputRoughnessPin);
	REQUIRE(rough != nullptr);
	CHECK(rough->type == HE::MatNodeType::SplitRGBA);

	const HE::MatGraphNode* normal = pinSource(g, out, HE::kMatOutputNormalPin);
	REQUIRE(normal != nullptr);
	CHECK(normal->type == HE::MatNodeType::NormalMapSample);
	CHECK(normal->s    == "Imported/crate_normal.hasset");

	const HE::MatGraphNode* emissive = pinSource(g, out, HE::kMatOutputEmissivePin);
	REQUIRE(emissive != nullptr);
	CHECK(emissive->type == HE::MatNodeType::ConstColor);
	CHECK(emissive->p[0] == doctest::Approx(1.0f));
	CHECK(emissive->p[1] == doctest::Approx(0.0f));

	// Four distinct images, four slots — separate metallic and roughness maps
	// each cost one, unlike glTF's packed ORM.
	CHECK(loaded.mat->graphTexturePaths.size() == 4);
	CHECK(loaded.mat->blendMode == static_cast<uint8_t>(HE::MatBlendMode::Opaque));
	// Every sampler reads the mesh UV (an unwired UV pin samples one texel).
	for (const HE::MatGraphNode& n : g.nodes)
	{
		if (n.type != HE::MatNodeType::TextureSample && n.type != HE::MatNodeType::NormalMapSample)
			continue;
		bool wired = false;
		for (const HE::MatGraphLink& l : g.links)
			if (l.dstNode == n.id && l.dstPin == 0) { wired = true; break; }
		CHECK(wired);
	}
	he_test::removeAllQuiet(dir);
}

// A constant opacity below 1 (OBJ `d`) is a translucent surface with the
// Opacity pin wired; a missing texture file loses the channel with a warning
// but never the material.
TEST_CASE("OBJ material: d < 1 imports translucent, a missing map drops only its channel")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_assimp_objalpha";
	he_test::removeAllQuiet(dir);
	REQUIRE(writeText(dir / "glass.obj",
		"mtllib glass.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl Glass\nf 1 2 3\n"));
	REQUIRE(writeText(dir / "glass.mtl",
		"newmtl Glass\nKd 0.2 0.4 0.6\nd 0.25\nmap_Kd not_there.png\n"));
	const fs::path contentRoot = dir / "Content";

	auto mesh = MeshImporter::import(dir / "glass.obj", contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	// "Glass" vs the mesh's "glass": one file on macOS and Windows, so the
	// material must step aside — on every platform, so the outputs match.
	CHECK(mesh->materialPath == "Imported/Glass_2.hasset");
	// Assimp's OBJ reader prepends a "DefaultMaterial" nothing here uses; it is
	// not written out as a stray asset.
	CHECK_FALSE(fs::exists(contentRoot / "Imported/DefaultMaterial.hasset"));

	LoadedMaterial loaded(contentRoot, "Imported/Glass_2.hasset");
	REQUIRE(loaded.mat != nullptr);
	CHECK(loaded.mat->blendMode == static_cast<uint8_t>(HE::MatBlendMode::Translucent));
	CHECK(loaded.mat->opacity == doctest::Approx(0.25f));
	CHECK(loaded.mat->texturePaths.empty());   // the map was not found: no channel, no crash
	// Kd IS the colour here — there is no map to defer to.
	CHECK(loaded.mat->baseColor[0] == doctest::Approx(0.2f));
	CHECK(loaded.mat->baseColor[2] == doctest::Approx(0.6f));

	const HE::MaterialGraph g = graphOf(loaded);
	const int out = outputNodeId(g);
	REQUIRE(out != 0);
	const HE::MatGraphNode* opacity = pinSource(g, out, HE::kMatOutputOpacityPin);
	REQUIRE(opacity != nullptr);
	CHECK(opacity->type == HE::MatNodeType::ConstFloat);
	CHECK(opacity->p[0] == doctest::Approx(0.25f));
	he_test::removeAllQuiet(dir);
}

// An ASCII FBX with a Phong material and a texture connected to DiffuseColor:
// the material asset is named after the FBX material, the texture is found by
// its RelativeFilename next to the source, and the Phong ShininessExponent
// becomes a roughness (Assimp's own FBX rule, 1 - sqrt(exp) / 10).
TEST_CASE("FBX Phong material: texture resolved next to the source, shininess becomes roughness")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_assimp_fbxmat";
	he_test::removeAllQuiet(dir);
	REQUIRE(writePng(dir / "tri_diffuse.png"));
	const std::string fbx =
"; FBX 7.4.0 project file\n"
"FBXHeaderExtension:  {\n"
"\tFBXHeaderVersion: 1003\n"
"\tFBXVersion: 7400\n"
"\tCreator: \"HorizonEngine test fixture\"\n"
"}\n"
"GlobalSettings:  {\n"
"\tVersion: 1000\n"
"\tProperties70:  {\n"
"\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
"\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
"\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
"\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
"\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
"\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
"\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",100\n"
"\t}\n"
"}\n"
"Objects:  {\n"
"\tGeometry: 1000, \"Geometry::Tri\", \"Mesh\" {\n"
"\t\tVertices: *9 {\n"
"\t\t\ta: 0,0,0,1,0,0,0,1,0\n"
"\t\t}\n"
"\t\tPolygonVertexIndex: *3 {\n"
"\t\t\ta: 0,1,-3\n"
"\t\t}\n"
"\t\tGeometryVersion: 124\n"
"\t\tLayerElementUV: 0 {\n"
"\t\t\tVersion: 101\n"
"\t\t\tName: \"UVMap\"\n"
"\t\t\tMappingInformationType: \"ByPolygonVertex\"\n"
"\t\t\tReferenceInformationType: \"Direct\"\n"
"\t\t\tUV: *6 {\n"
"\t\t\t\ta: 0,0,1,0,0,1\n"
"\t\t\t}\n"
"\t\t}\n"
"\t\tLayerElementMaterial: 0 {\n"
"\t\t\tVersion: 101\n"
"\t\t\tName: \"\"\n"
"\t\t\tMappingInformationType: \"AllSame\"\n"
"\t\t\tReferenceInformationType: \"IndexToDirect\"\n"
"\t\t\tMaterials: *1 {\n"
"\t\t\t\ta: 0\n"
"\t\t\t}\n"
"\t\t}\n"
"\t\tLayer: 0 {\n"
"\t\t\tVersion: 100\n"
"\t\t\tLayerElement:  {\n"
"\t\t\t\tType: \"LayerElementUV\"\n"
"\t\t\t\tTypedIndex: 0\n"
"\t\t\t}\n"
"\t\t\tLayerElement:  {\n"
"\t\t\t\tType: \"LayerElementMaterial\"\n"
"\t\t\t\tTypedIndex: 0\n"
"\t\t\t}\n"
"\t\t}\n"
"\t}\n"
"\tModel: 2000, \"Model::Tri\", \"Mesh\" {\n"
"\t\tVersion: 232\n"
"\t\tShading: T\n"
"\t\tCulling: \"CullingOff\"\n"
"\t}\n"
"\tMaterial: 3000, \"Material::M_Tri\", \"\" {\n"
"\t\tVersion: 102\n"
"\t\tShadingModel: \"phong\"\n"
"\t\tMultiLayer: 0\n"
"\t\tProperties70:  {\n"
"\t\t\tP: \"DiffuseColor\", \"Color\", \"\", \"A\",0.2,0.4,0.6\n"
"\t\t\tP: \"ShininessExponent\", \"double\", \"Number\", \"\",16\n"
"\t\t}\n"
"\t}\n"
"\tTexture: 4000, \"Texture::T_Diffuse\", \"\" {\n"
"\t\tType: \"TextureVideoClip\"\n"
"\t\tVersion: 202\n"
"\t\tTextureName: \"Texture::T_Diffuse\"\n"
"\t\tFileName: \"C:\\\\somewhere\\\\else\\\\tri_diffuse.png\"\n"
"\t\tRelativeFilename: \"tri_diffuse.png\"\n"
"\t\tModelUVTranslation: 0,0\n"
"\t\tModelUVScaling: 1,1\n"
"\t\tTexture_Alpha_Source: \"None\"\n"
"\t\tCropping: 0,0,0,0\n"
"\t}\n"
"}\n"
"Connections:  {\n"
"\tC: \"OO\",2000,0\n"
"\tC: \"OO\",1000,2000\n"
"\tC: \"OO\",3000,2000\n"
"\tC: \"OP\",4000,3000, \"DiffuseColor\"\n"
"}\n";
	REQUIRE(writeText(dir / "tri.fbx", fbx));
	const fs::path contentRoot = dir / "Content";

	auto mesh = MeshImporter::import(dir / "tri.fbx", contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	CHECK(mesh->materialPath == "Imported/M_Tri.hasset");
	REQUIRE(mesh->sections.size() == 1);
	CHECK(mesh->sections[0].materialPath == "Imported/M_Tri.hasset");
	CHECK(fs::exists(contentRoot / "Imported/tri_diffuse.hasset"));

	LoadedMaterial loaded(contentRoot, "Imported/M_Tri.hasset");
	REQUIRE(loaded.mat != nullptr);
	REQUIRE(loaded.mat->texturePaths.size() == 1);
	CHECK(loaded.mat->texturePaths[0] == "Imported/tri_diffuse.hasset");
	CHECK(loaded.mat->metallic  == doctest::Approx(0.0f));
	CHECK(loaded.mat->roughness == doctest::Approx(1.0f - 4.0f / 10.0f));   // 1 - sqrt(16)/10
	// DiffuseColor under a diffuse map is a coefficient, not a tint.
	CHECK(loaded.mat->baseColor[0] == doctest::Approx(1.0f));

	const HE::MaterialGraph g = graphOf(loaded);
	const int out = outputNodeId(g);
	REQUIRE(out != 0);
	const HE::MatGraphNode* base = pinSource(g, out, HE::kMatOutputBaseColorPin);
	REQUIRE(base != nullptr);
	CHECK(base->type == HE::MatNodeType::TextureSample);
	CHECK(base->s    == "Imported/tri_diffuse.hasset");

	// The sidecar bookkeeping reads the material and its textures back off the
	// mesh, so a Reimport and the asset compiler's up-to-date probe see them.
	const auto sidecars = Importer::meshSidecarAssets(contentRoot / "Imported/tri.hasset", contentRoot);
	REQUIRE(sidecars.size() >= 2);
	CHECK(sidecars[0] == "Imported/M_Tri.hasset");
	CHECK(std::find(sidecars.begin(), sidecars.end(), "Imported/tri_diffuse.hasset") != sidecars.end());
	he_test::removeAllQuiet(dir);
}

// The section core underneath both the glTF wrapper and the Assimp path.
TEST_CASE("buildMeshSections (index core): groups ranges by material, primary absorbs unassigned")
{
	// Four ranges: material 1, none, material 0, material 1 — with primary = 1
	// the unassigned one joins material 1's group. Order of first appearance:
	// 1 then 0.
	std::vector<uint32_t> indices = { 0,1,2,  3,4,5,  6,7,8,  9,10,11 };
	std::vector<Importer::BakedRange> ranges = {
		{ 0, 3, 1 }, { 3, 3, Importer::BakedRange::kNoMaterial }, { 6, 3, 0 }, { 9, 3, 1 } };
	std::vector<std::string> paths = { "Mat/Zero.hasset", "Mat/One.hasset" };

	auto sections = Importer::buildMeshSections(ranges, /*primaryIndex=*/1, paths, indices);
	REQUIRE(sections.size() == 2);
	CHECK(sections[0].indexOffset == 0);
	CHECK(sections[0].indexCount  == 9);
	CHECK(sections[0].materialPath == "Mat/One.hasset");
	CHECK(sections[1].indexOffset == 9);
	CHECK(sections[1].indexCount  == 3);
	CHECK(sections[1].materialPath == "Mat/Zero.hasset");
	CHECK(indices == std::vector<uint32_t>{ 0,1,2, 3,4,5, 9,10,11, 6,7,8 });

	// No materials declared at all: one section, empty path, buffer untouched.
	std::vector<uint32_t> plain = { 0,1,2, 3,4,5 };
	std::vector<Importer::BakedRange> none = {
		{ 0, 3, Importer::BakedRange::kNoMaterial }, { 3, 3, Importer::BakedRange::kNoMaterial } };
	auto one = Importer::buildMeshSections(none, Importer::BakedRange::kNoMaterial, {}, plain);
	REQUIRE(one.size() == 1);
	CHECK(one[0].indexCount == 6);
	CHECK(one[0].materialPath.empty());

	// A material index beyond the path table is an empty path, not a crash.
	std::vector<uint32_t> few = { 0,1,2 };
	std::vector<Importer::BakedRange> beyond = { { 0, 3, 7 } };
	auto shortTable = Importer::buildMeshSections(beyond, 7, paths, few);
	REQUIRE(shortTable.size() == 1);
	CHECK(shortTable[0].materialPath.empty());

	// A range that does not describe the buffer → no table at all.
	std::vector<uint32_t> small = { 0,1,2 };
	std::vector<Importer::BakedRange> bogus = { { 0, 6, 0 } };
	CHECK(Importer::buildMeshSections(bogus, 0, paths, small).empty());
}

// ─── The editor's entry points ───────────────────────────────────────────────
// The Content Browser's Import and the Import Asset dialog both go through
// Importer::importSource; Reimport goes through Importer::reimport. Everything
// above drives MeshImporter directly — these drive the two doors the editor
// actually opens.

TEST_CASE("importSource routes an OBJ to a StaticMesh asset; empty and face-less sources fail without one")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_assimp_importsource";
	he_test::removeAllQuiet(dir);
	const fs::path contentRoot = dir / "Content";
	REQUIRE(writeText(dir / "two.obj", kTwoMaterialObj));
	REQUIRE(writeText(dir / "two.mtl", kTwoMaterialMtl));

	REQUIRE(Importer::importSource(dir / "two.obj", contentRoot, "Imported"));
	REQUIRE(fs::exists(contentRoot / "Imported/two.hasset"));
	{
		ContentManager cm(contentRoot.string());
		const StaticMeshAsset* m = cm.getStaticMesh(cm.loadAsset("Imported/two.hasset"));
		REQUIRE(m != nullptr);
		CHECK(m->name == "two");
		CHECK(m->sections.size() == 2);
		CHECK(m->materialPath == "Imported/MatA.hasset");
		// The provenance Reimport needs was recorded, absolute.
		const std::string recorded = Importer::sourceFileOf(contentRoot / "Imported/two.hasset");
		CHECK_FALSE(recorded.empty());
		CHECK(fs::path(recorded).is_absolute());
		CHECK(fs::path(recorded).filename() == "two.obj");
	}

	// The failure shapes an artist actually produces: an export that wrote
	// nothing, a point cloud, a curve. Each fails as an import (false, logged)
	// and leaves no half-written .hasset behind — the Content Browser would list
	// one, and the mesh would load as nothing.
	REQUIRE(writeText(dir / "empty.obj", ""));
	REQUIRE(writeText(dir / "empty.fbx", ""));
	REQUIRE(writeText(dir / "empty.dae", ""));
	REQUIRE(writeText(dir / "points.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\n"));
	REQUIRE(writeText(dir / "lines.obj",  "v 0 0 0\nv 1 0 0\nv 0 1 0\nl 1 2\nl 2 3\n"));
	REQUIRE(writeText(dir / "notxml.dae", "<COLLADA><asset></COLLADA>\n"));
	for (const char* name : { "empty.obj", "empty.fbx", "empty.dae", "points.obj", "lines.obj", "notxml.dae" })
	{
		CAPTURE(name);
		CHECK_FALSE(Importer::importSource(dir / name, contentRoot, "Imported"));
		CHECK_FALSE(fs::exists(contentRoot / "Imported" / (fs::path(name).stem().string() + ".hasset")));
	}

	// A material library that is not there is a warning, not a failure: the
	// geometry still comes in, under a material of the declared name, so the
	// artist can fix the .mtl later instead of getting nothing.
	REQUIRE(writeText(dir / "nolib.obj",
		"mtllib nowhere.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl Missing\nf 1 2 3\n"));
	REQUIRE(Importer::importSource(dir / "nolib.obj", contentRoot, "Imported"));
	{
		ContentManager cm(contentRoot.string());
		const StaticMeshAsset* m = cm.getStaticMesh(cm.loadAsset("Imported/nolib.hasset"));
		REQUIRE(m != nullptr);
		CHECK(m->indices.size() == 3);
		CHECK_FALSE(m->materialPath.empty());
		CHECK(fs::exists(contentRoot / m->materialPath));
	}
	he_test::removeAllQuiet(dir);
}

namespace
{
std::vector<std::string> fileNamesIn(const fs::path& dir)
{
	std::vector<std::string> names;
	for (const auto& e : fs::directory_iterator(dir))
		names.push_back(e.path().filename().string());
	std::sort(names.begin(), names.end());
	return names;
}

// fs::rename plus the reference retarget the editor runs after it — a rename
// in the Content Browser is both, and a re-import has to survive both.
void renameAssetLikeTheEditor(const fs::path& contentRoot,
                              const std::string& oldRel, const std::string& newRel)
{
	std::error_code ec;
	fs::rename(contentRoot / oldRel, contentRoot / newRel, ec);
	REQUIRE_FALSE(ec);
	HE::AssetRefs::retargetTree(
		contentRoot.string(),
		HE::AssetRefs::moveRules(oldRel, newRel, /*folder=*/false, "Content"));
}
} // namespace

TEST_CASE("Reimport of a renamed OBJ asset keeps its uuid and redirects the material and texture sidecars")
{
	// The glTF twin lives in test_contentmanager; this one walks the Assimp path
	// through the same OutputTargets plumbing, which steps 3/4 never exercised end
	// to end: importViaAssimp → importAssimpMaterials → the shared PBR core.
	const fs::path dir = fs::temp_directory_path() / "he_test_assimp_reimport";
	he_test::removeAllQuiet(dir);
	const fs::path contentRoot = dir / "Content";
	const fs::path src = dir / "src";
	REQUIRE(writePng(src / "crate_base.png"));
	REQUIRE(writeText(src / "crate.mtl", "newmtl M_Crate\nmap_Kd crate_base.png\n"));
	auto writeObj = [&](float v1x)
	{
		return writeText(src / "crate.obj",
			"mtllib crate.mtl\n"
			"v 0 0 0\nv " + std::to_string(v1x) + " 0 0\nv 0 1 0\n"
			"vt 0 0\nvt 1 0\nvt 0 1\n"
			"usemtl M_Crate\n"
			"f 1/1 2/2 3/3\n");
	};
	REQUIRE(writeObj(1.0f));

	REQUIRE(Importer::importSource(src / "crate.obj", contentRoot, "Meshes"));
	const fs::path meshFile = contentRoot / "Meshes" / "crate.hasset";
	REQUIRE(fs::exists(meshFile));
	const auto sidecars = Importer::meshSidecarAssets(meshFile, contentRoot);
	REQUIRE(sidecars.size() == 2);
	CHECK(sidecars[0] == "Meshes/M_Crate.hasset");
	CHECK(sidecars[1] == "Meshes/crate_base.hasset");

	const HE::UUID meshId = HE::AssetRefs::assetUuidOfFile(meshFile.string());
	const HE::UUID matId  = HE::AssetRefs::assetUuidOfFile((contentRoot / sidecars[0]).string());
	const HE::UUID texId  = HE::AssetRefs::assetUuidOfFile((contentRoot / sidecars[1]).string());
	REQUIRE(meshId != HE::UUID{});
	REQUIRE(matId  != HE::UUID{});
	REQUIRE(texId  != HE::UUID{});

	renameAssetLikeTheEditor(contentRoot, "Meshes/crate.hasset",      "Meshes/Rock.hasset");
	renameAssetLikeTheEditor(contentRoot, "Meshes/M_Crate.hasset",    "Meshes/RockMat.hasset");
	renameAssetLikeTheEditor(contentRoot, "Meshes/crate_base.hasset", "Meshes/RockTex.hasset");
	const fs::path renamedMesh = contentRoot / "Meshes" / "Rock.hasset";
	const std::vector<std::string> before = fileNamesIn(contentRoot / "Meshes");

	// The artist moves a vertex and hits Reimport.
	REQUIRE(writeObj(2.0f));
	REQUIRE(Importer::reimport(renamedMesh, contentRoot));

	// All three kept their identity; no crate/M_Crate/crate_base reappeared.
	CHECK(HE::AssetRefs::assetUuidOfFile(renamedMesh.string()) == meshId);
	CHECK(HE::AssetRefs::assetUuidOfFile((contentRoot / "Meshes" / "RockMat.hasset").string()) == matId);
	CHECK(HE::AssetRefs::assetUuidOfFile((contentRoot / "Meshes" / "RockTex.hasset").string()) == texId);
	CHECK(fileNamesIn(contentRoot / "Meshes") == before);

	const auto after = Importer::meshSidecarAssets(renamedMesh, contentRoot);
	REQUIRE(after.size() == 2);
	CHECK(after[0] == "Meshes/RockMat.hasset");
	CHECK(after[1] == "Meshes/RockTex.hasset");

	ContentManager cm(contentRoot.string());
	const StaticMeshAsset* m = cm.getStaticMesh(cm.loadAsset("Meshes/Rock.hasset"));
	REQUIRE(m != nullptr);
	CHECK(m->name == "Rock");
	CHECK(m->materialPath == "Meshes/RockMat.hasset");
	REQUIRE(m->sections.size() == 1);
	CHECK(m->sections[0].materialPath == "Meshes/RockMat.hasset");
	// …and it is the NEW geometry.
	float lo[3], hi[3];
	boundsOf(*m, lo, hi);
	CHECK(hi[0] == doctest::Approx(2.0f));

	// The Reimport of a source that has since gone is a clean refusal.
	he_test::removeQuiet(src / "crate.obj");
	CHECK_FALSE(Importer::reimport(renamedMesh, contentRoot));
	CHECK(HE::AssetRefs::assetUuidOfFile(renamedMesh.string()) == meshId);
	he_test::removeAllQuiet(dir);
}

#endif // HE_HAVE_ASSIMP
