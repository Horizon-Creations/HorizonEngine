#include "doctest.h"
#include "TestFsUtil.h"
#include "MeshImporter.h"
#include "ImporterCommon.h"
#include "AutoLod.h"
#include <ContentManager/Assets.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
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

// A UV sphere, big enough that an Auto-LOD chain of several ratios produces that many
// genuinely distinct levels (same lat/lon AutoLod's own "removeStaleSidecars" test
// proves reaches at least 5 levels with 5 ratios) — unlike the single triangle above,
// which cannot be reduced at all and so can't tell a 3-level cap from a longer chain.
bool writeSphereGltf(const fs::path& dir, int lat, int lon)
{
	std::vector<float> pos, uv;
	for (int y = 0; y <= lat; ++y)
		for (int x = 0; x <= lon; ++x)
		{
			const float v = static_cast<float>(y) / lat, u = static_cast<float>(x) / lon;
			const float th = v * 3.14159265f, ph = u * 6.2831853f;
			pos.insert(pos.end(), { std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph) });
			uv.insert(uv.end(), { u, v });
		}
	std::vector<uint16_t> idx;
	const uint16_t row = static_cast<uint16_t>(lon + 1);
	for (int y = 0; y < lat; ++y)
		for (int x = 0; x < lon; ++x)
		{
			const uint16_t a = static_cast<uint16_t>(y * row + x), b = static_cast<uint16_t>(a + 1);
			const uint16_t c = static_cast<uint16_t>(a + row),     d = static_cast<uint16_t>(c + 1);
			idx.insert(idx.end(), { a, c, b, b, c, d });
		}

	const size_t posBytes = pos.size() * sizeof(float);
	const size_t uvBytes  = uv.size()  * sizeof(float);
	const size_t idxBytes = idx.size() * sizeof(uint16_t);
	std::vector<uint8_t> buf(posBytes + uvBytes + idxBytes, 0);
	std::memcpy(buf.data(), pos.data(), posBytes);
	std::memcpy(buf.data() + posBytes, uv.data(), uvBytes);
	std::memcpy(buf.data() + posBytes + uvBytes, idx.data(), idxBytes);

	std::error_code ec;
	fs::create_directories(dir, ec);
	{
		std::ofstream bin(dir / "sphere.bin", std::ios::binary);
		if (!bin) return false;
		bin.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
	}

	std::ostringstream json;
	json << "{\n"
	        "  \"asset\": { \"version\": \"2.0\" },\n"
	        "  \"scene\": 0,\n"
	        "  \"scenes\": [ { \"nodes\": [0] } ],\n"
	        "  \"nodes\":  [ { \"mesh\": 0 } ],\n"
	        "  \"meshes\": [ { \"primitives\": [ {\n"
	        "      \"attributes\": { \"POSITION\": 0, \"TEXCOORD_0\": 1 },\n"
	        "      \"indices\": 2 } ] } ],\n"
	        "  \"buffers\":    [ { \"uri\": \"sphere.bin\", \"byteLength\": " << buf.size() << " } ],\n"
	        "  \"bufferViews\": [\n"
	        "    { \"buffer\": 0, \"byteOffset\": 0, \"byteLength\": " << posBytes << " },\n"
	        "    { \"buffer\": 0, \"byteOffset\": " << posBytes << ", \"byteLength\": " << uvBytes << " },\n"
	        "    { \"buffer\": 0, \"byteOffset\": " << (posBytes + uvBytes) << ", \"byteLength\": " << idxBytes << " }\n"
	        "  ],\n"
	        "  \"accessors\": [\n"
	        "    { \"bufferView\": 0, \"componentType\": 5126, \"count\": " << (pos.size() / 3) << ", \"type\": \"VEC3\",\n"
	        "      \"min\": [-1,-1,-1], \"max\": [1,1,1] },\n"
	        "    { \"bufferView\": 1, \"componentType\": 5126, \"count\": " << (uv.size() / 2) << ", \"type\": \"VEC2\" },\n"
	        "    { \"bufferView\": 2, \"componentType\": 5123, \"count\": " << idx.size() << ", \"type\": \"SCALAR\" }\n"
	        "  ]\n"
	        "}";
	std::ofstream f(dir / "sphere.gltf");
	if (!f) return false;
	f << json.str();
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

// A 1x1 red PNG (truecolor, 8 bit) — the smallest real image stb_image decodes,
// so a fixture glTF can reference an actual base-color texture on disk.
const uint8_t kPng1x1[] = {
	0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
	0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
	0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
	0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
	0x00, 0x03, 0x01, 0x01, 0x00, 0xF7, 0x03, 0x41, 0x43, 0x00, 0x00, 0x00,
	0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
};

// The same triangle, but with a material whose base color texture points at the
// PNG above — importing this produces the two SIDECAR assets (the texture and
// the material referencing it) next to the mesh.
bool writeTexturedGltf(const fs::path& dir)
{
	if (!writeUvGltf(dir)) return false;  // writes uv.bin, reused here
	{
		std::ofstream png(dir / "tex1x1.png", std::ios::binary);
		if (!png) return false;
		png.write(reinterpret_cast<const char*>(kPng1x1),
		          static_cast<std::streamsize>(sizeof(kPng1x1)));
	}

	const char* json = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes":  [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ {
      "attributes": { "POSITION": 0, "TEXCOORD_0": 1 },
      "indices": 2, "material": 0 } ] } ],
  "materials": [ { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } } ],
  "textures":  [ { "source": 0 } ],
  "images":    [ { "uri": "tex1x1.png" } ],
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
	std::ofstream f(dir / "textured.gltf");
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

// Auto-LOD (Thema 331, step 17): a re-import with LOD generation on writes fewer levels
// than the file had before (here none: one triangle cannot be reduced). The old tail
// would otherwise stay beside the mesh and the _LOD<n> name convention would keep
// handing it back as levels of the new geometry.
TEST_CASE("MeshImporter: a re-import with LODs drops the stale _LOD<n> tail it no longer writes")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_stale_lod_gltf";
	he_test::removeAllQuiet(dir);
	REQUIRE(writeUvGltf(dir));
	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	MeshImporter::ImportSettings settings;
	settings.generateLods = true;
	auto mesh = MeshImporter::import(dir / "uv.gltf", contentRoot, {}, settings);
	REQUIRE(mesh != nullptr);
	const fs::path lod1 = contentRoot / AutoLod::lodPath(mesh->path, 1);
	const fs::path lod2 = contentRoot / AutoLod::lodPath(mesh->path, 2);
	const fs::path lod4 = contentRoot / AutoLod::lodPath(mesh->path, 4);   // behind a gap
	// A leftover chain from an earlier, longer run, plus one file past a gap.
	for (const fs::path& f : { lod1, lod2, lod4 }) std::ofstream(f, std::ios::binary) << "old";
	REQUIRE(fs::exists(lod1));

	auto again = MeshImporter::import(dir / "uv.gltf", contentRoot, {}, settings);
	REQUIRE(again != nullptr);
	CHECK_FALSE(fs::exists(lod1));
	CHECK_FALSE(fs::exists(lod2));
	CHECK(fs::exists(lod4));   // the walk ends at the first gap, a file past it is not part of the chain
	CHECK(AutoLod::findLodSidecars(contentRoot, again->path).empty());

	// With LOD generation off the importer leaves sidecars alone.
	std::ofstream(lod1, std::ios::binary) << "old";
	MeshImporter::ImportSettings off;
	REQUIRE(MeshImporter::import(dir / "uv.gltf", contentRoot, {}, off) != nullptr);
	CHECK(fs::exists(lod1));

	he_test::removeAllQuiet(dir);
}

// Step 20: Importer::reimport() used to build a fresh ImportOptions (default-constructed
// lodRatios = 3 entries) and only ever shrink it to std::min(3, levels.size()) — so a
// chain longer than the default 3 (built through the editor dialog, which allows up to 6)
// came back from a re-import truncated to 3, and MeshImporter's own stale-sidecar cleanup
// then deleted the now-unreferenced LOD4+ files as if they belonged to an older, longer run.
TEST_CASE("Importer::reimport regenerates a longer-than-default Auto-LOD chain instead of capping it to 3")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_long_lod_chain_gltf";
	he_test::removeAllQuiet(dir);
	REQUIRE(writeSphereGltf(dir, 24, 32));
	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	// A chain built with 4 ratios (more than ImportOptions' default 3), on the dialog's own
	// step/floor curve (ratiosFor(4, 0.5, 0.1) = 0.5/0.25/0.125/0.1 — four distinct values,
	// so none of the four generated levels collide with each other on this sphere).
	const std::vector<float> fourRatios = AutoLod::ratiosFor(4, 0.5f, 0.1f);
	REQUIRE(fourRatios.size() == 4u);
	MeshImporter::ImportSettings settings;
	settings.generateLods = true;
	settings.lodRatios     = fourRatios;
	auto mesh = MeshImporter::import(dir / "sphere.gltf", contentRoot, {}, settings);
	REQUIRE(mesh != nullptr);
	const size_t before = AutoLod::findLodSidecars(contentRoot, mesh->path).size();
	REQUIRE(before == 4u);

	// Re-import without the caller choosing ratios: reimport() must read how many
	// sidecars the chain already has and regenerate that many on the same curve, not its
	// own 3-entry default — which would land on the exact same four ratios used above and
	// so reproduce the same four levels deterministically.
	REQUIRE(Importer::reimport(contentRoot / mesh->path, contentRoot));
	const size_t after = AutoLod::findLodSidecars(contentRoot, mesh->path).size();
	CHECK(after > 3u);         // not silently capped to the default run's length
	CHECK(after == before);    // the whole chain survives, not just its first 3 levels

	he_test::removeAllQuiet(dir);
}

// asset_compiler used to compare only the PRIMARY .hasset against the source, so
// a deleted sidecar (the base-color texture, or the material referencing it) was
// never regenerated without --force. The up-to-date probe now walks every output
// an import produces.
TEST_CASE("importOutputsUpToDate notices a deleted sidecar asset")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_sidecar_gltf";
	he_test::removeAllQuiet(dir);
	REQUIRE(writeTexturedGltf(dir));

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	const fs::path source = dir / "textured.gltf";
	auto mesh = MeshImporter::import(source, contentRoot, "Imported");
	REQUIRE(mesh != nullptr);

	// The mesh's material reference names a MATERIAL, and that material names the
	// texture — both written by this import, next to the mesh.
	const fs::path primary = contentRoot / mesh->path;
	const auto sidecars = Importer::meshSidecarAssets(primary, contentRoot);
	REQUIRE(sidecars.size() == 2);
	CHECK(sidecars[0] == mesh->materialPath);
	CHECK(sidecars[0] == "Imported/textured_mat.hasset");
	CHECK(sidecars[1] == "Imported/tex1x1.hasset");

	CHECK(Importer::importOutputsUpToDate(source, contentRoot, primary));

	// Delete the imported texture: the mesh itself is still newer than the
	// source, but the import is no longer complete.
	fs::remove(contentRoot / sidecars[1], ec);
	CHECK_FALSE(Importer::importOutputsUpToDate(source, contentRoot, primary));

	// …and a re-run brings it back.
	REQUIRE(MeshImporter::import(source, contentRoot, "Imported") != nullptr);
	CHECK(fs::exists(contentRoot / sidecars[1]));
	CHECK(Importer::importOutputsUpToDate(source, contentRoot, primary));

	// Same for the material itself.
	fs::remove(contentRoot / sidecars[0], ec);
	CHECK_FALSE(Importer::importOutputsUpToDate(source, contentRoot, primary));

	// A listed extra output (the asset compiler passes a rigged glTF's animation
	// clips here) counts the same way.
	REQUIRE(MeshImporter::import(source, contentRoot, "Imported") != nullptr);
	CHECK(Importer::importOutputsUpToDate(source, contentRoot, primary,
	                                      { "Imported/tex1x1.hasset" }));
	CHECK_FALSE(Importer::importOutputsUpToDate(source, contentRoot, primary,
	                                            { "Imported/never_written.hasset" }));

	he_test::removeAllQuiet(dir);
}

// The sidecars are read back off the mesh instead of guessed, so a glTF that
// carries no texture at all yields an empty list — otherwise every such asset
// would look permanently out of date and be re-imported on every run.
TEST_CASE("importOutputsUpToDate stays true for a glTF without a material")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_nosidecar_gltf";
	he_test::removeAllQuiet(dir);
	REQUIRE(writeUvGltf(dir));

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	const fs::path source = dir / "uv.gltf";
	auto mesh = MeshImporter::import(source, contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	CHECK(mesh->materialPath.empty());

	const fs::path primary = contentRoot / mesh->path;
	CHECK(Importer::meshSidecarAssets(primary, contentRoot).empty());
	CHECK(Importer::importOutputsUpToDate(source, contentRoot, primary));

	// A missing primary is still out of date, of course.
	fs::remove(primary, ec);
	CHECK_FALSE(Importer::importOutputsUpToDate(source, contentRoot, primary));

	he_test::removeAllQuiet(dir);
}
