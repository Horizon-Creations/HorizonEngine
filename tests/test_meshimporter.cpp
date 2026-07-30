#include "doctest.h"
#include "TestFsUtil.h"
#include "MeshImporter.h"
#include "ImporterCommon.h"
#include <ContentManager/Assets.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
//  Minimal static-mesh glTF fixture: one triangle with POSITION + TEXCOORD_0.
//
//  Binary buffer layout (66 bytes):
//    offset  0 : 36 bytes — POSITION   vec3 float32 ×3
//    offset 36 : 24 bytes — TEXCOORD_0 vec2 float32 ×3
//    offset 60 :  6 bytes — indices    uint16       ×3
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
bool writeUvGltf(const fs::path& dir)
{
	std::vector<uint8_t> buf(66, 0);
	const float pos[9] = { 0,0,0,  1,0,0,  0,1,0 };
	std::memcpy(buf.data() + 0, pos, sizeof(pos));
	// glTF convention: V = 0 is the TOP of the image.
	const float uv[6] = { 0.0f, 0.0f,   1.0f, 0.0f,   0.0f, 1.0f };
	std::memcpy(buf.data() + 36, uv, sizeof(uv));
	const uint16_t idx[3] = { 0, 1, 2 };
	std::memcpy(buf.data() + 60, idx, sizeof(idx));

	std::error_code ec;
	fs::create_directories(dir, ec);
	{
		std::ofstream bin(dir / "uv.bin", std::ios::binary);
		if (!bin) return false;
		bin.write(reinterpret_cast<const char*>(buf.data()),
		          static_cast<std::streamsize>(buf.size()));
	}

	const char* json = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes":  [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ {
      "attributes": { "POSITION": 0, "TEXCOORD_0": 1 },
      "indices": 2 } ] } ],
  "buffers":    [ { "uri": "uv.bin", "byteLength": 66 } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,  "byteLength": 36 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 24 },
    { "buffer": 0, "byteOffset": 60, "byteLength": 6 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
      "min": [0,0,0], "max": [1,1,0] },
    { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2" },
    { "bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR" }
  ]
})";
	std::ofstream f(dir / "uv.gltf");
	if (!f) return false;
	f << json;
	return true;
}

// Same triangle, but the primitive is skinned: two joint nodes, a skin, and
// JOINTS_0/WEIGHTS_0 attributes. Only the JSON matters here — Importer::gltfHasSkin
// parses the document without loading buffers — so no .bin is written.
bool writeSkinnedGltf(const fs::path& dir)
{
	std::error_code ec;
	fs::create_directories(dir, ec);
	const char* json = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0, 2] } ],
  "nodes":  [ { "name": "root", "children": [1] }, { "name": "hip" },
              { "mesh": 0, "skin": 0 } ],
  "skins":  [ { "joints": [0, 1], "name": "Armature" } ],
  "meshes": [ { "primitives": [ {
      "attributes": { "POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2 },
      "mode": 4 } ] } ],
  "buffers":    [ { "uri": "skin.bin", "byteLength": 96 } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,  "byteLength": 36 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 12 },
    { "buffer": 0, "byteOffset": 48, "byteLength": 48 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
      "min": [0,0,0], "max": [1,1,0] },
    { "bufferView": 1, "componentType": 5121, "count": 3, "type": "VEC4" },
    { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4" }
  ]
})";
	std::ofstream f(dir / "skinned.gltf");
	if (!f) return false;
	f << json;
	return true;
}
} // namespace

// glTF stores UVs with the origin at the TOP-left; the engine is GL-style
// BOTTOM-left (TextureImporter flips images on import, Metal flips V at sample
// time). Reading glTF's V verbatim rendered every imported mesh's texture
// vertically mirrored.
TEST_CASE("MeshImporter flips glTF V into the engine's bottom-left UV origin")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_uv_gltf";
	he_test::removeAllQuiet(dir);
	REQUIRE(writeUvGltf(dir));

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	auto mesh = MeshImporter::import(dir / "uv.gltf", contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	REQUIRE(mesh->uvs.size() == 6);

	// Source V (0, 0, 1) must arrive as (1, 1, 0); U is untouched.
	CHECK(mesh->uvs[0] == doctest::Approx(0.0f)); // v0.u
	CHECK(mesh->uvs[1] == doctest::Approx(1.0f)); // v0.v  (was 0 in the file)
	CHECK(mesh->uvs[2] == doctest::Approx(1.0f)); // v1.u
	CHECK(mesh->uvs[3] == doctest::Approx(1.0f)); // v1.v
	CHECK(mesh->uvs[4] == doctest::Approx(0.0f)); // v2.u
	CHECK(mesh->uvs[5] == doctest::Approx(0.0f)); // v2.v  (was 1 in the file)

	he_test::removeAllQuiet(dir);
}

// Every import entry point (asset_compiler, File ▸ Import Asset, Content Browser
// ▸ Import) used to send every .gltf to MeshImporter, which ignores skins — so a
// rigged character imported as bind-pose geometry registered as a StaticMesh,
// reported as a success, and never showed up in the SkeletalMesh picker. They now
// all ask this one helper which importer a glTF belongs to.
TEST_CASE("gltfHasSkin routes a skinned glTF away from the static MeshImporter")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_skin_probe_gltf";
	he_test::removeAllQuiet(dir);
	REQUIRE(writeUvGltf(dir));
	REQUIRE(writeSkinnedGltf(dir));

	CHECK(Importer::gltfHasSkin(dir / "skinned.gltf") == true);
	CHECK(Importer::gltfHasSkin(dir / "uv.gltf")      == false);
	// Unreadable / non-glTF input must not claim a skin — the caller falls back to
	// the static path and reports that importer's error.
	CHECK(Importer::gltfHasSkin(dir / "does_not_exist.gltf") == false);

	he_test::removeAllQuiet(dir);
}
