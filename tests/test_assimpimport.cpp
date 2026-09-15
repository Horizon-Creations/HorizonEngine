// FBX / OBJ / COLLADA import through Assimp (AssimpMeshImport, routed by
// MeshImporter). Compiled out without Assimp: the whole feature is.
#ifdef HE_HAVE_ASSIMP

#include "doctest.h"
#include "TestFsUtil.h"
#include "AssimpMeshImport.h"
#include "MeshImporter.h"
#include "ImporterCommon.h"
#include <ContentManager/Assets.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

	// Two materials → two sections, contiguous and covering the buffer; no
	// material was imported on this path, so both paths are empty ("the mesh's
	// own material") and the mesh binds none either.
	REQUIRE(mesh->sections.size() == 2);
	CHECK(mesh->sections[0].indexOffset == 0);
	CHECK(mesh->sections[0].indexCount  == 3);
	CHECK(mesh->sections[1].indexOffset == 3);
	CHECK(mesh->sections[1].indexCount  == 3);
	CHECK(mesh->sections[0].materialPath.empty());
	CHECK(mesh->sections[1].materialPath.empty());
	CHECK(mesh->materialPath.empty());

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

#endif // HE_HAVE_ASSIMP
