// glTF materials → MaterialAssets with a PBR node graph (Importer::importGltfMaterials).
//
// What these pin down is the LINKING, not the shading: which assets an import
// writes, under which names, which textures each material's graph samples in which
// slot, and which of them the mesh ends up bound to. The graph's shading semantics
// are test_material_graph.cpp's job.
#include "doctest.h"
#include "TestFsUtil.h"
#include "ImporterCommon.h"
#include "MeshImporter.h"
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <MaterialGraph/MaterialGraph.h>
#include <material/MaterialShaderLibrary.h>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
// A 1x1 red PNG (truecolor, 8 bit) — the smallest real image stb_image decodes.
// The PIXELS are irrelevant here: every assertion below is about paths and slots,
// so the same bytes stand in for base colour, ORM, normal and emissive alike.
const uint8_t kPng1x1[] = {
	0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
	0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
	0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
	0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
	0x00, 0x03, 0x01, 0x01, 0x00, 0xF7, 0x03, 0x41, 0x43, 0x00, 0x00, 0x00,
	0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
};
// The same bytes as a base64 data: URI — cgltf resolves data-URI BUFFERS but
// leaves data-URI IMAGES encoded, so this exercises the importer's own decode.
const char* kPng1x1DataUri =
	"data:image/png;base64,"
	"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mP4z8AAAAMBAQD3A0FDAAAAAElFTkSuQmCC";

void writePng(const fs::path& file)
{
	std::error_code ec;
	fs::create_directories(file.parent_path(), ec);   // the fixture dir was just removed
	std::ofstream png(file, std::ios::binary);
	png.write(reinterpret_cast<const char*>(kPng1x1),
	          static_cast<std::streamsize>(sizeof(kPng1x1)));
}

// One triangle with POSITION + TEXCOORD_0, in the byte layout test_meshimporter.cpp
// uses: 36 B positions, 24 B UVs, 6 B indices.
void writeTriangleBin(const fs::path& dir)
{
	std::vector<uint8_t> buf(66, 0);
	const float pos[9] = { 0,0,0,  1,0,0,  0,1,0 };
	std::memcpy(buf.data() + 0, pos, sizeof(pos));
	const float uv[6] = { 0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f };
	std::memcpy(buf.data() + 36, uv, sizeof(uv));
	const uint16_t idx[3] = { 0, 1, 2 };
	std::memcpy(buf.data() + 60, idx, sizeof(idx));

	std::error_code ec;
	fs::create_directories(dir, ec);
	std::ofstream bin(dir / "tri.bin", std::ios::binary);
	bin.write(reinterpret_cast<const char*>(buf.data()),
	          static_cast<std::streamsize>(buf.size()));
}

// The geometry half every fixture below shares — one primitive, material 0.
const char* kGeometryJson = R"(
  "buffers":    [ { "uri": "tri.bin", "byteLength": 66 } ],
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
  ])";

void writeGltf(const fs::path& dir, const std::string& name, const std::string& materialsAndImages)
{
	writeTriangleBin(dir);
	std::ofstream f(dir / (name + ".gltf"));
	f << R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes":  [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ {
      "attributes": { "POSITION": 0, "TEXCOORD_0": 1 },
      "indices": 2, "material": 0 } ] } ],
)" << materialsAndImages << ",\n" << kGeometryJson << "\n}\n";
}

// Loads a written material back the way the engine does, so the assertions run
// against the DESERIALIZED asset rather than the in-memory struct the importer
// happened to build. Note that ContentManager regenerates the GLSL from the graph
// at load — which is exactly why the importer must also write the graph, and why
// these tests look at nodeGraphJson / graphTexturePaths for the wiring.
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

// The node the Output pin `pin` is driven by, following Multiply/Split hops the
// importer inserts for factors and channel picks. Returns nullptr when the pin is
// unconnected.
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

// Every texture path any node in the graph samples (Texture Sample + Normal Map).
std::vector<std::string> sampledTextures(const HE::MaterialGraph& g)
{
	std::vector<std::string> out;
	for (const HE::MatGraphNode& n : g.nodes)
		if (n.type == HE::MatNodeType::TextureSample || n.type == HE::MatNodeType::NormalMapSample)
			out.push_back(n.s);
	return out;
}

bool contains(const std::vector<std::string>& v, const std::string& s)
{
	return std::find(v.begin(), v.end(), s) != v.end();
}

// True when every sampling node's UV input pin is wired. It has to be: codegen
// resolves an unconnected input to the pin's numeric DEFAULT, and Texture Sample's
// UV pin defaults to 0 — so an unwired sampler emits `texture(heTexP0, vec2(0.0))`
// and paints the whole surface with a single texel. (Normal Map is the exception,
// falling back to vUV, but the importer wires it too so a transformed normal map
// tiles with the channels beside it.)
bool everySamplerHasUV(const HE::MaterialGraph& g)
{
	for (const HE::MatGraphNode& n : g.nodes)
	{
		if (n.type != HE::MatNodeType::TextureSample && n.type != HE::MatNodeType::NormalMapSample)
			continue;
		bool wired = false;
		for (const HE::MatGraphLink& l : g.links)
			if (l.dstNode == n.id && l.dstPin == 0) { wired = true; break; }
		if (!wired) return false;
	}
	return true;
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The import used to take the FIRST base-colour texture found anywhere in the
// file, wire it into an unlit material named after the mesh, and drop every other
// material, every other channel and every PBR factor. A mesh exported from a DCC
// tool with a normal map and an ORM map therefore arrived flat and unlit, with no
// error to say so.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("glTF PBR material imports every channel into one graph")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_pbr";
	he_test::removeAllQuiet(dir);
	writePng(dir / "rock_basecolor.png");
	writePng(dir / "rock_orm.png");
	writePng(dir / "rock_normal.png");
	writePng(dir / "rock_emissive.png");

	// Unreal's glTF export shape: one ORM image bound to BOTH the occlusion and the
	// metallic-roughness slot.
	writeGltf(dir, "rock", R"(  "materials": [ {
    "name": "M_Rock",
    "pbrMetallicRoughness": {
      "baseColorTexture": { "index": 0 },
      "baseColorFactor": [0.5, 0.6, 0.7, 1.0],
      "metallicRoughnessTexture": { "index": 1 },
      "metallicFactor": 0.25,
      "roughnessFactor": 0.75
    },
    "normalTexture":    { "index": 2, "scale": 0.5 },
    "occlusionTexture": { "index": 1 },
    "emissiveTexture":  { "index": 3 },
    "emissiveFactor":   [1.0, 0.0, 0.0],
    "doubleSided": true
  } ],
  "textures": [ { "source": 0 }, { "source": 1 }, { "source": 2 }, { "source": 3 } ],
  "images": [ { "uri": "rock_basecolor.png" }, { "uri": "rock_orm.png" },
              { "uri": "rock_normal.png" },    { "uri": "rock_emissive.png" } ])");

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	auto mesh = MeshImporter::import(dir / "rock.gltf", contentRoot, "Imported");
	REQUIRE(mesh != nullptr);

	// Named after the glTF MATERIAL, not the mesh: that name is stable across a
	// mesh rename, and two meshes sharing a material converge on one asset.
	CHECK(mesh->materialPath == "Imported/M_Rock.hasset");
	REQUIRE(fs::exists(contentRoot / mesh->materialPath));

	// Each image became its own texture asset, named after the image FILE.
	for (const char* t : { "rock_basecolor", "rock_orm", "rock_normal", "rock_emissive" })
		CHECK(fs::exists(contentRoot / ("Imported/" + std::string(t) + ".hasset")));

	LoadedMaterial loaded(contentRoot, mesh->materialPath);
	REQUIRE(loaded.mat != nullptr);

	// The scalar fields carry the glTF factors too, not just the graph: they are
	// what the Inspector edits and what any consumer bypassing the graph reads.
	CHECK(loaded.mat->baseColor[0] == doctest::Approx(0.5f));
	CHECK(loaded.mat->baseColor[1] == doctest::Approx(0.6f));
	CHECK(loaded.mat->baseColor[2] == doctest::Approx(0.7f));
	CHECK(loaded.mat->metallic     == doctest::Approx(0.25f));
	CHECK(loaded.mat->roughness    == doctest::Approx(0.75f));
	// glTF's `doubleSided` is read and set on the asset, but NOT asserted here:
	// MaterialAsset::doubleSided is absent from the MTRL chunk (ContentManager's
	// material branch never writes it), so it cannot survive this round-trip — and
	// no renderer reads it either. Adding it is a format change, not an importer one.
	// The legacy heTex0 slot still points at the base colour, for the mesh paths
	// that sample it without a graph shader.
	REQUIRE(loaded.mat->texturePaths.size() == 1);
	CHECK(loaded.mat->texturePaths[0] == "Imported/rock_basecolor.hasset");

	// The graph is the source of truth and must round-trip.
	REQUIRE_FALSE(loaded.mat->nodeGraphJson.empty());
	HE::MaterialGraph g;
	REQUIRE(HE::materialGraphFromJson(loaded.mat->nodeGraphJson, g));
	const int out = outputNodeId(g);
	REQUIRE(out != 0);

	// Every PBR pin is driven — including Metallic and Roughness, which MUST be
	// wired even without a texture: glTF's factor defaults (1, 1) are not the
	// Output node's pin defaults (0, 0.5).
	CHECK(pinSource(g, out, HE::kMatOutputBaseColorPin) != nullptr);
	CHECK(pinSource(g, out, HE::kMatOutputMetallicPin)  != nullptr);
	CHECK(pinSource(g, out, HE::kMatOutputRoughnessPin) != nullptr);
	CHECK(pinSource(g, out, HE::kMatOutputEmissivePin)  != nullptr);
	CHECK(pinSource(g, out, HE::kMatOutputAOPin)        != nullptr);
	// The normal must come from a Normal Map node — a plain Texture Sample would
	// feed raw tangent-space bytes into a world-space pin.
	const HE::MatGraphNode* normalSrc = pinSource(g, out, HE::kMatOutputNormalPin);
	REQUIRE(normalSrc != nullptr);
	CHECK(normalSrc->type == HE::MatNodeType::NormalMapSample);
	CHECK(normalSrc->s    == "Imported/rock_normal.hasset");
	CHECK(normalSrc->p[0] == doctest::Approx(0.5f));   // normalTexture.scale

	// Opaque (the default alphaMode): codegen forces alpha to 1 and never reads the
	// Opacity pin, so wiring it would be a dead branch in every imported material.
	CHECK(loaded.mat->blendMode == static_cast<uint8_t>(HE::MatBlendMode::Opaque));
	CHECK(pinSource(g, out, HE::kMatOutputOpacityPin) == nullptr);

	// ORM shared between occlusion and metallic-roughness: ONE texture slot, and one
	// Texture Sample node. A graph has only four slots (HE::kMatMaxGraphTextures), so
	// a second copy of the same path would cost a real binding.
	const std::vector<std::string> sampled = sampledTextures(g);
	CHECK(std::count(sampled.begin(), sampled.end(), std::string("Imported/rock_orm.hasset")) == 1);
	CHECK(loaded.mat->graphTexturePaths.size() == 4);
	for (const char* t : { "rock_basecolor", "rock_orm", "rock_normal", "rock_emissive" })
		CHECK(contains(loaded.mat->graphTexturePaths, "Imported/" + std::string(t) + ".hasset"));

	// Every sampler reads the mesh UV. Without this the generated GLSL samples at
	// vec2(0.0) — one texel stretched over the whole surface, which looks like a
	// broken texture rather than an unwired pin.
	CHECK(everySamplerHasUV(g));
	// The negative is the load-bearing one: an unwired UV pin emits exactly this.
	CHECK(loaded.mat->customShaderFragGlsl.find("texture(heTexP0, vec2(0.000000))")
	      == std::string::npos);

	// The baked GLSL is written by the IMPORTER, not left to ContentManager's
	// load-time regeneration: the packer reads .hasset chunks raw, so a material
	// carrying only the graph would ship with no shader at all.
	//
	// Asserted on the FILE, not on `loaded.mat`: loading goes through
	// ContentManager, which regenerates the GLSL from the graph — so
	// `loaded.mat->customShaderFragGlsl` is non-empty whether or not the importer
	// ever wrote it, and the obvious assertion here cannot fail.
	{
		std::ifstream f(contentRoot / mesh->materialPath, std::ios::binary);
		REQUIRE(f.is_open());
		const std::string bytes((std::istreambuf_iterator<char>(f)),
		                        std::istreambuf_iterator<char>());
		CHECK(bytes.find("#version 450") != std::string::npos);
		// …and BOTH variants: the forward tail calls heLitP, the deferred one writes
		// oGB0. The packer ships the G-buffer copy verbatim, so a material missing it
		// renders forward-only in a deferred build with nothing logged.
		CHECK(bytes.find("heLitP(") != std::string::npos);
		CHECK(bytes.find("oGB0")    != std::string::npos);
	}

	he_test::removeAllQuiet(dir);
}

// A mesh asset holds exactly ONE material reference (no submesh concept), so a
// multi-material glTF cannot be bound in full. Importing every material anyway —
// and saying which ones went unbound — beats dropping them: the assets are
// complete, the user only has to assign them.
TEST_CASE("a multi-material glTF writes every material and binds the first primitive's")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_multimat";
	he_test::removeAllQuiet(dir);
	writeTriangleBin(dir);

	// Two primitives, two materials; the FIRST one is what the mesh binds.
	std::ofstream f(dir / "two.gltf");
	f << R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes":  [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [
      { "attributes": { "POSITION": 0, "TEXCOORD_0": 1 }, "indices": 2, "material": 0 },
      { "attributes": { "POSITION": 0, "TEXCOORD_0": 1 }, "indices": 2, "material": 1 } ] } ],
  "materials": [
    { "name": "M_Body", "pbrMetallicRoughness": { "baseColorFactor": [1, 0, 0, 1] } },
    { "name": "M Glass", "alphaMode": "BLEND",
      "pbrMetallicRoughness": { "baseColorFactor": [0, 0, 1, 0.4] } } ],
)" << kGeometryJson << "\n}\n";
	f.close();

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	auto mesh = MeshImporter::import(dir / "two.gltf", contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	CHECK(mesh->materialPath == "Imported/M_Body.hasset");

	// The unbound one is still a complete asset on disk. Its name comes from the
	// glTF material with the space sanitised — a raw DCC name can carry characters
	// that are not legal in a file name at all.
	REQUIRE(fs::exists(contentRoot / "Imported/M_Glass.hasset"));

	LoadedMaterial glass(contentRoot, "Imported/M_Glass.hasset");
	REQUIRE(glass.mat != nullptr);
	// alphaMode BLEND routes the material into the sorted alpha-blend pass.
	CHECK(glass.mat->blendMode == static_cast<uint8_t>(HE::MatBlendMode::Translucent));
	CHECK(glass.mat->opacity   == doctest::Approx(0.4f));

	HE::MaterialGraph g;
	REQUIRE(HE::materialGraphFromJson(glass.mat->nodeGraphJson, g));
	// Translucent DOES read the Opacity pin, so it has to be driven — here by the
	// base-colour factor's alpha, since the material has no base-colour texture.
	CHECK(pinSource(g, outputNodeId(g), HE::kMatOutputOpacityPin) != nullptr);

	he_test::removeAllQuiet(dir);
}

// MASK is the third blend mode and the one with a threshold: alphaCutoff has to
// reach the Output node, or every masked material discards at the default 0.5.
TEST_CASE("alphaMode MASK carries its cutoff into the graph")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_mask";
	he_test::removeAllQuiet(dir);
	writePng(dir / "leaf.png");
	writeGltf(dir, "leaf", R"(  "materials": [ {
    "name": "M_Leaf", "alphaMode": "MASK", "alphaCutoff": 0.8,
    "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } } ],
  "textures": [ { "source": 0 } ],
  "images":   [ { "uri": "leaf.png" } ])");

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	auto mesh = MeshImporter::import(dir / "leaf.gltf", contentRoot, "Imported");
	REQUIRE(mesh != nullptr);

	LoadedMaterial loaded(contentRoot, "Imported/M_Leaf.hasset");
	REQUIRE(loaded.mat != nullptr);
	CHECK(loaded.mat->blendMode == static_cast<uint8_t>(HE::MatBlendMode::Masked));

	HE::MaterialGraph g;
	REQUIRE(HE::materialGraphFromJson(loaded.mat->nodeGraphJson, g));
	const HE::MatGraphNode* out = g.findNode(outputNodeId(g));
	REQUIRE(out != nullptr);
	CHECK(out->p[2] == doctest::Approx(0.8f));
	// Masked reads the Opacity pin as the MASK — the base texture's alpha here.
	CHECK(pinSource(g, out->id, HE::kMatOutputOpacityPin) != nullptr);

	he_test::removeAllQuiet(dir);
}

// KHR_materials_specular. The two conventions are exactly reconcilable:
//   engine F0 = 0.08 * pin  (MaterialShaderLibrary heLitP — Unreal's rule)
//   glTF   F0 = 0.04 * specularFactor
// so pin = 0.5 * specularFactor holds for every value, and glTF's default of 1.0
// lands on the pin's own default of 0.5. A real Unreal foliage export ships
// specularFactor 0.05 (bark) / 0.03 (leaves); ignoring it leaves both at the
// dielectric 0.04, which reads as wet plastic.
TEST_CASE("KHR_materials_specular maps onto the Specular pin at half the factor")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_specular";
	he_test::removeAllQuiet(dir);
	writeGltf(dir, "spec", R"(  "materials": [
    { "name": "M_Bark", "extensions": {
        "KHR_materials_specular": { "specularFactor": 0.05 } } },
    { "name": "M_Plain" } ],
  "extensionsUsed": [ "KHR_materials_specular" ])");

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	REQUIRE(MeshImporter::import(dir / "spec.gltf", contentRoot, "Imported") != nullptr);

	{
		LoadedMaterial bark(contentRoot, "Imported/M_Bark.hasset");
		REQUIRE(bark.mat != nullptr);
		HE::MaterialGraph g;
		REQUIRE(HE::materialGraphFromJson(bark.mat->nodeGraphJson, g));
		const HE::MatGraphNode* spec = pinSource(g, outputNodeId(g), HE::kMatOutputSpecularPin);
		REQUIRE(spec != nullptr);
		CHECK(spec->type == HE::MatNodeType::ConstFloat);
		CHECK(spec->p[0] == doctest::Approx(0.025f));   // 0.5 * 0.05
		// …and it really reaches the generated shader as heLitP's specular argument.
		CHECK(bark.mat->customShaderFragGlsl.find("0.025000") != std::string::npos);
	}
	{
		// No extension → the pin is left unconnected, so codegen emits its own
		// default of 0.5. Wiring a node there would change nothing and only add
		// clutter to every imported graph.
		LoadedMaterial plain(contentRoot, "Imported/M_Plain.hasset");
		REQUIRE(plain.mat != nullptr);
		HE::MaterialGraph g;
		REQUIRE(HE::materialGraphFromJson(plain.mat->nodeGraphJson, g));
		CHECK(pinSource(g, outputNodeId(g), HE::kMatOutputSpecularPin) == nullptr);
	}

	he_test::removeAllQuiet(dir);
}

// Unreal's glTF exporter BAKES a material into textures addressed by the mesh's
// second, non-overlapping UV set (baking needs a set without overlap — the lightmap
// UV) and declares "texCoord": 1 on every texture view. Reading TEXCOORD_0 there
// samples the baked atlas with the ORIGINAL tiling UVs, which on a real cedar export
// meant v running to 57 instead of staying inside [0,1] — not subtly wrong, but
// unrecognisable. The mesh importer stores the set each primitive's MATERIAL uses.
TEST_CASE("the mesh stores the UV set its material samples, not always TEXCOORD_0")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_uvset";
	he_test::removeAllQuiet(dir);
	writePng(dir / "s_base.png");

	// Two UV sets on one primitive: TEXCOORD_0 tiles far outside [0,1] (the material
	// UV), TEXCOORD_1 stays inside it (the bake UV). The material samples set 1.
	{
		std::vector<uint8_t> buf(90, 0);
		const float pos[9] = { 0,0,0,  1,0,0,  0,1,0 };
		std::memcpy(buf.data() + 0, pos, sizeof(pos));
		const float uv0[6] = { 0.0f, 0.0f,   4.0f, 0.0f,   0.0f, 8.0f };   // TEXCOORD_0
		std::memcpy(buf.data() + 36, uv0, sizeof(uv0));
		const float uv1[6] = { 0.25f, 0.25f, 0.75f, 0.25f, 0.25f, 0.75f }; // TEXCOORD_1
		std::memcpy(buf.data() + 60, uv1, sizeof(uv1));
		const uint16_t idx[3] = { 0, 1, 2 };
		std::memcpy(buf.data() + 84, idx, sizeof(idx));
		std::error_code ec;
		fs::create_directories(dir, ec);
		std::ofstream bin(dir / "two_uv.bin", std::ios::binary);
		bin.write(reinterpret_cast<const char*>(buf.data()),
		          static_cast<std::streamsize>(buf.size()));
	}
	{
		std::ofstream f(dir / "baked.gltf");
		f << R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes":  [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ {
      "attributes": { "POSITION": 0, "TEXCOORD_0": 1, "TEXCOORD_1": 2 },
      "indices": 3, "material": 0 } ] } ],
  "materials": [ { "name": "M_Baked", "pbrMetallicRoughness": {
      "baseColorTexture": { "index": 0, "texCoord": 1 } } } ],
  "textures": [ { "source": 0 } ],
  "images":   [ { "uri": "s_base.png" } ],
  "buffers":    [ { "uri": "two_uv.bin", "byteLength": 90 } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,  "byteLength": 36 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 24 },
    { "buffer": 0, "byteOffset": 60, "byteLength": 24 },
    { "buffer": 0, "byteOffset": 84, "byteLength": 6 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
      "min": [0,0,0], "max": [1,1,0] },
    { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2" },
    { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2" },
    { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" }
  ]
})";
	}

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	auto mesh = MeshImporter::import(dir / "baked.gltf", contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	REQUIRE(mesh->uvs.size() == 6);

	// TEXCOORD_1, with the usual V flip: (0.25,0.25) → (0.25,0.75) etc. Had the
	// importer taken TEXCOORD_0, u would run to 4 and v to -7.
	CHECK(mesh->uvs[0] == doctest::Approx(0.25f));
	CHECK(mesh->uvs[1] == doctest::Approx(0.75f));
	CHECK(mesh->uvs[2] == doctest::Approx(0.75f));
	CHECK(mesh->uvs[4] == doctest::Approx(0.25f));
	CHECK(mesh->uvs[5] == doctest::Approx(0.25f));
	for (size_t i = 0; i < mesh->uvs.size(); ++i)
		CHECK(mesh->uvs[i] <= 1.0f);   // nothing from the tiling set leaked through

	he_test::removeAllQuiet(dir);
}

// A material that names a UV set the mesh does not carry must not collapse onto
// (0,0) — it falls back to TEXCOORD_0, and the import says so.
TEST_CASE("a material asking for a missing UV set falls back to TEXCOORD_0")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_uvset_missing";
	he_test::removeAllQuiet(dir);
	writePng(dir / "m_base.png");
	writeGltf(dir, "onlyuv0", R"(  "materials": [ { "name": "M_Missing",
    "pbrMetallicRoughness": { "baseColorTexture": { "index": 0, "texCoord": 3 } } } ],
  "textures": [ { "source": 0 } ],
  "images":   [ { "uri": "m_base.png" } ])");

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	// writeGltf's fixture carries TEXCOORD_0 only; the material asks for set 3.
	auto mesh = MeshImporter::import(dir / "onlyuv0.gltf", contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	REQUIRE(mesh->uvs.size() == 6);
	// The fixture's TEXCOORD_0 is (0,0)/(1,0)/(0,1); V-flipped that is (0,1)/(1,1)/(0,0).
	CHECK(mesh->uvs[1] == doctest::Approx(1.0f));
	CHECK(mesh->uvs[5] == doctest::Approx(0.0f));

	he_test::removeAllQuiet(dir);
}

// KHR_texture_transform tiles/offsets a texture in glTF UV space, where V points
// DOWN the image. The engine stores V flipped and samples a flipped image, so the
// numbers cannot be copied across: the UV node must produce
// t = 1 - (v_gltf * sy + oy) = (1 - sy - oy) + sy * w. Getting the sign wrong here
// slides and mirrors every tiled texture along V, which is exactly the kind of
// thing that reads correct in the source.
TEST_CASE("KHR_texture_transform converts into the engine's flipped-V UV node")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_uvxform";
	he_test::removeAllQuiet(dir);
	writePng(dir / "t_base.png");
	// A NON-ZERO offset on purpose. With offset 0 the V term is 1 - sy - 0, which
	// several sign slips reproduce by accident; 0.25 separates them.
	writeGltf(dir, "tiled", R"(  "materials": [ {
    "name": "M_Tiled",
    "pbrMetallicRoughness": { "baseColorTexture": { "index": 0, "extensions": {
      "KHR_texture_transform": { "scale": [2, 4], "offset": [0.125, 0.25] } } } } } ],
  "textures": [ { "source": 0 } ],
  "images":   [ { "uri": "t_base.png" } ],
  "extensionsUsed": [ "KHR_texture_transform" ])");

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	REQUIRE(MeshImporter::import(dir / "tiled.gltf", contentRoot, "Imported") != nullptr);

	LoadedMaterial loaded(contentRoot, "Imported/M_Tiled.hasset");
	REQUIRE(loaded.mat != nullptr);
	HE::MaterialGraph g;
	REQUIRE(HE::materialGraphFromJson(loaded.mat->nodeGraphJson, g));

	const HE::MatGraphNode* uv = nullptr;
	for (const HE::MatGraphNode& n : g.nodes)
		if (n.type == HE::MatNodeType::UV) { uv = &n; break; }
	REQUIRE(uv != nullptr);
	// p[0..1] = tiling (verbatim), p[2] = offset.x (verbatim),
	// p[3] = 1 - scale.y - offset.y = 1 - 4 - 0.25 = -3.25.
	CHECK(uv->p[0] == doctest::Approx(2.0f));
	CHECK(uv->p[1] == doctest::Approx(4.0f));
	CHECK(uv->p[2] == doctest::Approx(0.125f));
	CHECK(uv->p[3] == doctest::Approx(-3.25f));
	// The conversion exists so the glTF's own UV range still lands on [0,1] after
	// the engine's V flip. Check the round trip end to end: a glTF v maps to the
	// stored w = 1 - v, and the UV node must take that back to v * sy + oy.
	for (float v : { 0.0f, 0.3f, 1.0f })
	{
		const float w        = 1.0f - v;                       // what the mesh stores
		const float sampled  = w * uv->p[1] + uv->p[3];        // what the UV node emits
		const float expected = 1.0f - (v * 4.0f + 0.25f);      // 1 - glTF's transformed v
		CHECK(sampled == doctest::Approx(expected));
	}

	he_test::removeAllQuiet(dir);
}

// An untransformed material shares ONE UV node across all of its samplers, so the
// generated shader keeps reading plain vUV instead of one redundant multiply-add
// per texture.
TEST_CASE("an untransformed material shares a single UV node")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_oneuv";
	he_test::removeAllQuiet(dir);
	writePng(dir / "u_base.png");
	writePng(dir / "u_normal.png");
	writeGltf(dir, "plain", R"(  "materials": [ {
    "name": "M_Plain",
    "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } },
    "normalTexture": { "index": 1 } } ],
  "textures": [ { "source": 0 }, { "source": 1 } ],
  "images":   [ { "uri": "u_base.png" }, { "uri": "u_normal.png" } ])");

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	REQUIRE(MeshImporter::import(dir / "plain.gltf", contentRoot, "Imported") != nullptr);

	LoadedMaterial loaded(contentRoot, "Imported/M_Plain.hasset");
	REQUIRE(loaded.mat != nullptr);
	HE::MaterialGraph g;
	REQUIRE(HE::materialGraphFromJson(loaded.mat->nodeGraphJson, g));

	int uvNodes = 0;
	for (const HE::MatGraphNode& n : g.nodes)
		if (n.type == HE::MatNodeType::UV) ++uvNodes;
	CHECK(uvNodes == 1);
	CHECK(everySamplerHasUV(g));

	he_test::removeAllQuiet(dir);
}

// A material graph samples at most HE::kMatMaxGraphTextures textures. Over budget,
// codegen silently falls the surplus node back to the MESH texture — and because it
// allocates slots in Output-pin order (base, metallic/roughness, emissive, AO, and
// only THEN normal), the channel that would lose out is the NORMAL MAP. The
// importer therefore drops occlusion itself, which is the cheapest channel to lose.
TEST_CASE("a material over the texture budget loses occlusion, never its normal map")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_budget";
	he_test::removeAllQuiet(dir);
	for (const char* t : { "b", "mr", "n", "ao", "e" })
		writePng(dir / (std::string(t) + ".png"));

	// Five DISTINCT images — occlusion and metallic-roughness are separate here.
	writeGltf(dir, "busy", R"(  "materials": [ {
    "name": "M_Busy",
    "pbrMetallicRoughness": {
      "baseColorTexture": { "index": 0 }, "metallicRoughnessTexture": { "index": 1 } },
    "normalTexture":    { "index": 2 },
    "occlusionTexture": { "index": 3 },
    "emissiveTexture":  { "index": 4 },
    "emissiveFactor":   [1, 1, 1] } ],
  "textures": [ { "source": 0 }, { "source": 1 }, { "source": 2 },
                { "source": 3 }, { "source": 4 } ],
  "images": [ { "uri": "b.png" }, { "uri": "mr.png" }, { "uri": "n.png" },
              { "uri": "ao.png" }, { "uri": "e.png" } ])");

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	REQUIRE(MeshImporter::import(dir / "busy.gltf", contentRoot, "Imported") != nullptr);

	LoadedMaterial loaded(contentRoot, "Imported/M_Busy.hasset");
	REQUIRE(loaded.mat != nullptr);
	CHECK(loaded.mat->graphTexturePaths.size() == HE::kMatMaxGraphTextures);

	HE::MaterialGraph g;
	REQUIRE(HE::materialGraphFromJson(loaded.mat->nodeGraphJson, g));
	const int out = outputNodeId(g);
	// The normal map survives; occlusion is the pin that went unconnected.
	const HE::MatGraphNode* normalSrc = pinSource(g, out, HE::kMatOutputNormalPin);
	REQUIRE(normalSrc != nullptr);
	CHECK(normalSrc->s == "Imported/n.hasset");
	// Wired is not enough — it has to hold a real SLOT. Over budget, codegen keeps
	// the node and silently points it at the mesh texture instead, so the pin stays
	// connected while the normal map is gone.
	CHECK(contains(loaded.mat->graphTexturePaths, "Imported/n.hasset"));
	CHECK(loaded.mat->customShaderFragGlsl.find("hePerturbNormal") != std::string::npos);
	CHECK(pinSource(g, out, HE::kMatOutputAOPin) == nullptr);
	CHECK_FALSE(contains(loaded.mat->graphTexturePaths, "Imported/ao.hasset"));

	// The dropped image is still IMPORTED — only the graph slot is gone, so the
	// user can wire it into a channel they care about more.
	CHECK(fs::exists(contentRoot / "Imported/ao.hasset"));

	he_test::removeAllQuiet(dir);
}

// glTF multiplies the emissive texture by emissiveFactor, whose DEFAULT is zero —
// so an exporter that names an emissive texture without a factor describes a
// material that emits nothing. Wiring it would spend one of only four graph
// texture slots on a channel that resolves to black.
TEST_CASE("an emissive texture with a zero factor does not take a texture slot")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_blackemissive";
	he_test::removeAllQuiet(dir);
	writePng(dir / "e_base.png");
	writePng(dir / "e_glow.png");
	writeGltf(dir, "dark", R"(  "materials": [ {
    "name": "M_Dark",
    "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } },
    "emissiveTexture": { "index": 1 } } ],
  "textures": [ { "source": 0 }, { "source": 1 } ],
  "images":   [ { "uri": "e_base.png" }, { "uri": "e_glow.png" } ])");

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	REQUIRE(MeshImporter::import(dir / "dark.gltf", contentRoot, "Imported") != nullptr);

	LoadedMaterial loaded(contentRoot, "Imported/M_Dark.hasset");
	REQUIRE(loaded.mat != nullptr);
	CHECK(loaded.mat->graphTexturePaths.size() == 1);
	CHECK_FALSE(contains(loaded.mat->graphTexturePaths, "Imported/e_glow.hasset"));

	HE::MaterialGraph g;
	REQUIRE(HE::materialGraphFromJson(loaded.mat->nodeGraphJson, g));
	CHECK(pinSource(g, outputNodeId(g), HE::kMatOutputEmissivePin) == nullptr);

	// The image is still imported — only the graph link is skipped, so a user who
	// meant to have emission can wire it up without re-importing.
	CHECK(fs::exists(contentRoot / "Imported/e_glow.hasset"));

	he_test::removeAllQuiet(dir);
}

// Embedded images have no file name to inherit. They used to all be called
// "<meshStem>_basecolor", so the second one overwrote the first — and data-URI
// images were not decoded at all, importing untextured with no error.
TEST_CASE("embedded images are named per image, not per mesh")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_embedded";
	he_test::removeAllQuiet(dir);

	std::string json = R"(  "materials": [ {
    "name": "M_Inline",
    "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } },
    "emissiveTexture": { "index": 1 }, "emissiveFactor": [1, 1, 1] } ],
  "textures": [ { "source": 0 }, { "source": 1 } ],
  "images": [ { "name": "Albedo Map", "uri": ")";
	json += kPng1x1DataUri;
	json += R"(" }, { "uri": ")";
	json += kPng1x1DataUri;
	json += R"(" } ])";
	writeGltf(dir, "inline", json);

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	REQUIRE(MeshImporter::import(dir / "inline.gltf", contentRoot, "Imported") != nullptr);

	// Named image → its own (sanitised) name; unnamed → the mesh stem plus the
	// image INDEX, which is what keeps two of them apart.
	CHECK(fs::exists(contentRoot / "Imported/Albedo_Map.hasset"));
	CHECK(fs::exists(contentRoot / "Imported/inline_img1.hasset"));

	LoadedMaterial loaded(contentRoot, "Imported/M_Inline.hasset");
	REQUIRE(loaded.mat != nullptr);
	CHECK(loaded.mat->graphTexturePaths.size() == 2);
	CHECK(contains(loaded.mat->graphTexturePaths, "Imported/Albedo_Map.hasset"));
	CHECK(contains(loaded.mat->graphTexturePaths, "Imported/inline_img1.hasset"));

	he_test::removeAllQuiet(dir);
}

// An imported material's sidecar textures have to be findable from the MESH, or
// the asset compiler calls the mesh up to date after its normal map was deleted
// and never regenerates it. texturePaths alone only carries the base colour.
TEST_CASE("meshSidecarAssets lists a PBR material's node-graph textures")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_sidecars";
	he_test::removeAllQuiet(dir);
	writePng(dir / "s_base.png");
	writePng(dir / "s_normal.png");
	writeGltf(dir, "side", R"(  "materials": [ {
    "name": "M_Side",
    "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } },
    "normalTexture": { "index": 1 } } ],
  "textures": [ { "source": 0 }, { "source": 1 } ],
  "images":   [ { "uri": "s_base.png" }, { "uri": "s_normal.png" } ])");

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	const fs::path source = dir / "side.gltf";
	auto mesh = MeshImporter::import(source, contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	const fs::path primary = contentRoot / mesh->path;

	const std::vector<std::string> sidecars = Importer::meshSidecarAssets(primary, contentRoot);
	// Material first (reimport() redirects outputs.material off sidecars[0]), then
	// the base colour (the legacy slot), then the graph's remaining textures — each
	// listed once even though the base colour appears in both lists.
	REQUIRE(sidecars.size() == 3);
	CHECK(sidecars[0] == "Imported/M_Side.hasset");
	CHECK(sidecars[1] == "Imported/s_base.hasset");
	CHECK(contains(sidecars, "Imported/s_normal.hasset"));

	CHECK(Importer::importOutputsUpToDate(source, contentRoot, primary));
	// Deleting the normal map — reachable ONLY through the graph list — must make
	// the import look incomplete again.
	fs::remove(contentRoot / "Imported/s_normal.hasset", ec);
	CHECK_FALSE(Importer::importOutputsUpToDate(source, contentRoot, primary));
	REQUIRE(MeshImporter::import(source, contentRoot, "Imported") != nullptr);
	CHECK(fs::exists(contentRoot / "Imported/s_normal.hasset"));

	he_test::removeAllQuiet(dir);
}

// Every output of one import lands in the same flat folder under a stem taken from
// an untrusted glTF string, so a material can be named exactly like a texture file
// or like the source mesh. writeAsset recovers the uuid from whatever file is
// already at the target, so the loser did not merely get overwritten — the winner
// INHERITED its identity, and every reference resolved to the wrong kind of asset
// with nothing logged.
TEST_CASE("an import never writes one asset over another of a different type")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_collide";
	he_test::removeAllQuiet(dir);
	writePng(dir / "Wood.png");

	// The material is called "Wood" and so is the texture file; the source is
	// "Wood.gltf", so the mesh wants that stem too — all three collide at once.
	writeGltf(dir, "Wood", R"(  "materials": [ { "name": "Wood",
    "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } } ],
  "textures": [ { "source": 0 } ],
  "images":   [ { "uri": "Wood.png" } ])");

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	auto mesh = MeshImporter::import(dir / "Wood.gltf", contentRoot, "Imported");
	REQUIRE(mesh != nullptr);

	// The mesh keeps the source stem; the other two moved aside.
	CHECK(mesh->path == "Imported/Wood.hasset");
	REQUIRE_FALSE(mesh->materialPath.empty());
	CHECK(mesh->materialPath != mesh->path);

	LoadedMaterial mat(contentRoot, mesh->materialPath);
	REQUIRE(mat.mat != nullptr);                       // …and it really is a MATERIAL
	REQUIRE(mat.mat->texturePaths.size() == 1);
	CHECK(mat.mat->texturePaths[0] != mesh->materialPath);
	CHECK(mat.mat->texturePaths[0] != mesh->path);

	// Three files, three identities, each still the kind it claims to be.
	ContentManager cm(contentRoot.string());
	const HE::UUID meshId = cm.loadAsset(mesh->path);
	const HE::UUID matId  = cm.loadAsset(mesh->materialPath);
	const HE::UUID texId  = cm.loadAsset(mat.mat->texturePaths[0]);
	CHECK(cm.getStaticMesh(meshId) != nullptr);
	CHECK(cm.getMaterial(matId)    != nullptr);
	CHECK(cm.getTexture(texId)     != nullptr);
	CHECK(meshId != matId);
	CHECK(matId  != texId);

	he_test::removeAllQuiet(dir);
}

// Two images with the SAME basename in different sub-folders used to resolve to one
// asset — the second silently replaced the first and both materials then sampled
// the same pixels.
TEST_CASE("two images with the same basename import as two assets")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_samebase";
	he_test::removeAllQuiet(dir);
	writePng(dir / "wood" / "basecolor.png");
	writePng(dir / "metal" / "basecolor.png");

	writeGltf(dir, "two_tex", R"(  "materials": [
    { "name": "M_Wood",  "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } },
    { "name": "M_Metal", "pbrMetallicRoughness": { "baseColorTexture": { "index": 1 } } } ],
  "textures": [ { "source": 0 }, { "source": 1 } ],
  "images":   [ { "uri": "wood/basecolor.png" }, { "uri": "metal/basecolor.png" } ])");

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	REQUIRE(MeshImporter::import(dir / "two_tex.gltf", contentRoot, "Imported") != nullptr);

	LoadedMaterial wood(contentRoot, "Imported/M_Wood.hasset");
	LoadedMaterial metal(contentRoot, "Imported/M_Metal.hasset");
	REQUIRE(wood.mat != nullptr);
	REQUIRE(metal.mat != nullptr);
	REQUIRE(wood.mat->texturePaths.size() == 1);
	REQUIRE(metal.mat->texturePaths.size() == 1);
	CHECK(wood.mat->texturePaths[0] != metal.mat->texturePaths[0]);
	CHECK(fs::exists(contentRoot / wood.mat->texturePaths[0]));
	CHECK(fs::exists(contentRoot / metal.mat->texturePaths[0]));

	he_test::removeAllQuiet(dir);
}

// The don't-clobber guard used to be gated on a re-import redirect, which is only
// ever set for the BOUND material of a single-material glTF. Every other material
// was therefore regenerated on every import: a leaf material the artist had opened
// and given a graph came back flat, and the loss first showed on the next load.
TEST_CASE("re-importing a multi-material glTF leaves the unbound materials alone")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_unbound_authored";
	he_test::removeAllQuiet(dir);
	writeTriangleBin(dir);
	{
		std::ofstream f(dir / "tree.gltf");
		f << R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes":  [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [
      { "attributes": { "POSITION": 0, "TEXCOORD_0": 1 }, "indices": 2, "material": 0 },
      { "attributes": { "POSITION": 0, "TEXCOORD_0": 1 }, "indices": 2, "material": 1 } ] } ],
  "materials": [ { "name": "M_Bark" }, { "name": "M_Leaves" } ],
)" << kGeometryJson << "\n}\n";
	}

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	const fs::path source = dir / "tree.gltf";
	auto mesh = MeshImporter::import(source, contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	CHECK(mesh->materialPath == "Imported/M_Bark.hasset");
	REQUIRE(fs::exists(contentRoot / "Imported/M_Leaves.hasset"));

	// The artist opens the unbound leaf material and gives it a graph of its own.
	{
		MaterialAsset authored;
		authored.type          = HE::AssetType::Material;
		authored.name          = "M_Leaves";
		authored.path          = "Imported/M_Leaves.hasset";
		authored.nodeGraphJson = "AUTHORED-BY-HAND";
		REQUIRE(Importer::writeAsset(authored, contentRoot));
	}

	// …and the mesh is imported again (the DCC file changed, the menu was used a
	// second time, or the asset compiler ran).
	REQUIRE(MeshImporter::import(source, contentRoot, "Imported") != nullptr);

	LoadedMaterial leaves(contentRoot, "Imported/M_Leaves.hasset");
	REQUIRE(leaves.mat != nullptr);
	CHECK(leaves.mat->nodeGraphJson == "AUTHORED-BY-HAND");

	he_test::removeAllQuiet(dir);
}

// Before materials were named after the glTF, a single-material import always wrote
// "<meshStem>_mat". Re-importing such an asset must refresh THAT file rather than
// write a second material beside it and orphan whatever the artist changed.
TEST_CASE("an existing <mesh>_mat sidecar keeps its name on re-import")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_legacyname";
	he_test::removeAllQuiet(dir);
	writePng(dir / "l_base.png");
	writeGltf(dir, "crate", R"(  "materials": [ { "name": "M_Crate",
    "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } } ],
  "textures": [ { "source": 0 } ],
  "images":   [ { "uri": "l_base.png" } ])");

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);
	fs::create_directories(contentRoot / "Imported", ec);

	// Stand in for a project imported by an older build: the sidecar already exists
	// under the historical name.
	{
		MaterialAsset legacy;
		legacy.type = HE::AssetType::Material;
		legacy.name = "crate_mat";
		legacy.path = "Imported/crate_mat.hasset";
		REQUIRE(Importer::writeAsset(legacy, contentRoot));
	}

	auto mesh = MeshImporter::import(dir / "crate.gltf", contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	// The historical name wins over the glTF material name, so the mesh keeps
	// pointing at the file the project already has.
	CHECK(mesh->materialPath == "Imported/crate_mat.hasset");
	CHECK_FALSE(fs::exists(contentRoot / "Imported/M_Crate.hasset"));

	he_test::removeAllQuiet(dir);
}

// A re-import must land on the material the mesh already references, and must NOT
// rewrite its contents: the generated material carries one graph and a few
// textures, so writing it over one the artist has since authored in the Material
// Editor silently destroys that graph (writeAsset keeps the UUID, so nothing
// dangles and the loss only surfaces on the next project load).
TEST_CASE("re-import refreshes an existing material's textures but not the material")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_reimport";
	he_test::removeAllQuiet(dir);
	writePng(dir / "r_base.png");
	writeGltf(dir, "rock2", R"(  "materials": [ {
    "name": "M_Rock2",
    "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } } ],
  "textures": [ { "source": 0 } ],
  "images":   [ { "uri": "r_base.png" } ])");

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	const fs::path source = dir / "rock2.gltf";
	auto mesh = MeshImporter::import(source, contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	CHECK(mesh->materialPath == "Imported/M_Rock2.hasset");

	// Stand in for "the artist opened this in the Material Editor": rewrite the
	// material with a graph of its own.
	{
		ContentManager cm(contentRoot.string());
		MaterialAsset authored;
		authored.type          = HE::AssetType::Material;
		authored.name          = "M_Rock2";
		authored.path          = "Imported/M_Rock2.hasset";
		authored.nodeGraphJson = "AUTHORED-BY-HAND";
		REQUIRE(cm.saveAsset(authored));
	}

	REQUIRE(Importer::reimport(contentRoot / mesh->path, contentRoot));

	LoadedMaterial after(contentRoot, "Imported/M_Rock2.hasset");
	REQUIRE(after.mat != nullptr);
	CHECK(after.mat->nodeGraphJson == "AUTHORED-BY-HAND");
	// …while the texture, which has nothing authorable in it, was refreshed.
	CHECK(fs::exists(contentRoot / "Imported/r_base.hasset"));

	he_test::removeAllQuiet(dir);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Normal-map orientation
//
//  Two flips meet on an imported normal map and it is not obvious that they
//  cancel — an inverted green channel reads as lighting from the wrong side, and
//  is easy to ship because nothing errors:
//    1. the mesh importer stores V flipped (glTF's UV origin is TOP-left, the
//       engine's is GL-style BOTTOM-left),
//    2. TextureImporter flips the image rows to match,
//    3. the Normal Map node builds its cotangent frame (hePerturbNormal, Mikkelsen)
//       from screen-space derivatives of the STORED UV — so the bitangent follows
//       the stored V, not glTF's.
//
//  This replicates hePerturbNormal against a real imported mesh and checks that a
//  glTF normal map's green channel (+Y, OpenGL convention: bright green tilts the
//  surface toward the TOP of the image) really does tilt toward the top of the
//  image in world space. If it did not, the importer would have to bake a green
//  flip into every normal map it imports.
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
struct V3 { float x, y, z; };
V3 sub(V3 a, V3 b)        { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
V3 scale(V3 a, float s)   { return { a.x*s, a.y*s, a.z*s }; }
V3 add(V3 a, V3 b)        { return { a.x+b.x, a.y+b.y, a.z+b.z }; }
float dot(V3 a, V3 b)     { return a.x*b.x + a.y*b.y + a.z*b.z; }
V3 cross(V3 a, V3 b)
{ return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x }; }
V3 normalize(V3 a)        { const float l = std::sqrt(dot(a, a)); return scale(a, l > 0 ? 1.0f/l : 0.0f); }

// hePerturbNormal, transcribed from HE::generateFragment's emitted preamble.
// dp1/duv1 are the screen-X derivatives, dp2/duv2 the screen-Y ones.
V3 perturbNormal(V3 N, V3 mapN, V3 dp1, V3 dp2, float duv1[2], float duv2[2])
{
	const V3 dp2perp = cross(dp2, N);
	const V3 dp1perp = cross(N, dp1);
	const V3 T = add(scale(dp2perp, duv1[0]), scale(dp1perp, duv2[0]));
	const V3 B = add(scale(dp2perp, duv1[1]), scale(dp1perp, duv2[1]));
	const float invmax = 1.0f / std::sqrt(std::max(dot(T, T), dot(B, B)));
	// mat3(T*invmax, B*invmax, N) * mapN — columns, GLSL convention.
	return normalize(add(add(scale(T, invmax * mapN.x), scale(B, invmax * mapN.y)),
	                     scale(N, mapN.z)));
}
} // namespace

TEST_CASE("an imported glTF normal map's green channel points to the top of the image")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_normal_orient";
	he_test::removeAllQuiet(dir);

	// A triangle in the world XY plane, facing +Z. The glTF UVs pin the image so
	// that its TOP edge (v = 0) sits at world +Y: v0 at the image's bottom-left,
	// v1 bottom-right, v2 top-left.
	{
		std::vector<uint8_t> buf(66, 0);
		const float pos[9] = { 0,0,0,  1,0,0,  0,1,0 };
		std::memcpy(buf.data() + 0, pos, sizeof(pos));
		const float uv[6] = { 0.0f, 1.0f,   1.0f, 1.0f,   0.0f, 0.0f };
		std::memcpy(buf.data() + 36, uv, sizeof(uv));
		const uint16_t idx[3] = { 0, 1, 2 };
		std::memcpy(buf.data() + 60, idx, sizeof(idx));
		std::error_code ec;
		fs::create_directories(dir, ec);
		std::ofstream bin(dir / "tri.bin", std::ios::binary);
		bin.write(reinterpret_cast<const char*>(buf.data()),
		          static_cast<std::streamsize>(buf.size()));
	}
	{
		std::ofstream f(dir / "quad.gltf");
		f << R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes":  [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ {
      "attributes": { "POSITION": 0, "TEXCOORD_0": 1 }, "indices": 2 } ] } ],
)" << kGeometryJson << "\n}\n";
	}

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	auto mesh = MeshImporter::import(dir / "quad.gltf", contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	REQUIRE(mesh->vertices.size() == 9);
	REQUIRE(mesh->uvs.size() == 6);

	const auto P = [&](int i) { return V3{ mesh->vertices[i*3], mesh->vertices[i*3+1],
	                                       mesh->vertices[i*3+2] }; };

	// The screen basis of a camera looking down -Z at this front-facing triangle:
	// screen X is world +X, screen Y is world +Y. It has to be a proper (unmirrored)
	// view — hePerturbNormal's frame is the true tangent frame times the UV
	// Jacobian's DETERMINANT, so feeding it a mirrored basis flips the frame, which
	// on screen only happens when you are looking at the back face.
	const V3 dp1 = sub(P(1), P(0));   // +X
	const V3 dp2 = sub(P(2), P(0));   // +Y
	float duv1[2] = { mesh->uvs[2] - mesh->uvs[0], mesh->uvs[3] - mesh->uvs[1] };
	float duv2[2] = { mesh->uvs[4] - mesh->uvs[0], mesh->uvs[5] - mesh->uvs[1] };

	const V3 N = { 0.0f, 0.0f, 1.0f };
	// A glTF normal map texel of pure +Y (green = 255, i.e. 2*1-1 = +1 after the
	// node's decode).
	const V3 greenUp = perturbNormal(N, { 0.0f, 1.0f, 0.0f }, dp1, dp2, duv1, duv2);

	// It must tilt toward the top of the image, which this layout puts at world +Y.
	CHECK(greenUp.y == doctest::Approx(1.0f));
	CHECK(greenUp.x == doctest::Approx(0.0f));

	// And the red channel (+X, toward the RIGHT of the image) toward world +X.
	const V3 redRight = perturbNormal(N, { 1.0f, 0.0f, 0.0f }, dp1, dp2, duv1, duv2);
	CHECK(redRight.x == doctest::Approx(1.0f));

	// The V flip is what makes that true: with glTF's raw V the bitangent would run
	// down the image and the same texel would tilt the surface the other way — the
	// classic inverted-green look. This is the case the importer would have to
	// compensate for by baking a green flip into every normal map.
	float rawDuv1[2] = { duv1[0], -duv1[1] };
	float rawDuv2[2] = { duv2[0], -duv2[1] };
	const V3 unflipped = perturbNormal(N, { 0.0f, 1.0f, 0.0f }, dp1, dp2, rawDuv1, rawDuv2);
	CHECK(unflipped.y == doctest::Approx(-1.0f));

	he_test::removeAllQuiet(dir);
}

#if defined(HE_TESTS_HAVE_SHADERC)
// The end of the line for an imported material is a compiled pipeline. Everything
// above asserts on graph topology and on the emitted GLSL text — but a graph the
// importer builds wrongly (a pin fed the wrong type, a channel swizzled off a
// scalar) produces GLSL that reads plausibly and fails to compile, and the
// renderers answer a failed material compile by logging once and falling back to
// the built-in shader for the rest of the session. So the import is only really
// verified once its output survives glslang.
//
// The fixture is shaped after a real Unreal Engine 5.7 tree export: base colour +
// metallic-roughness + normal, all on TEXCOORD_1 with a KHR_texture_transform,
// alphaMode MASK for the leaf cards, and KHR_materials_specular.
TEST_CASE("an imported Unreal-shaped material cross-compiles for Metal and GL")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_gltf_compile";
	he_test::removeAllQuiet(dir);
	for (const char* t : { "bc", "mr", "nm" })
		writePng(dir / (std::string(t) + ".png"));

	// Two UV sets, textures bound to the second — the Unreal bake shape.
	{
		std::vector<uint8_t> buf(90, 0);
		const float pos[9] = { 0,0,0,  1,0,0,  0,1,0 };
		std::memcpy(buf.data() + 0, pos, sizeof(pos));
		const float uv0[6] = { 0.0f, 0.0f,  9.0f, 0.0f,  0.0f, 9.0f };
		std::memcpy(buf.data() + 36, uv0, sizeof(uv0));
		const float uv1[6] = { 0.1f, 0.1f,  0.9f, 0.1f,  0.1f, 0.9f };
		std::memcpy(buf.data() + 60, uv1, sizeof(uv1));
		const uint16_t idx[3] = { 0, 1, 2 };
		std::memcpy(buf.data() + 84, idx, sizeof(idx));
		std::error_code ec;
		fs::create_directories(dir, ec);
		std::ofstream bin(dir / "tree.bin", std::ios::binary);
		bin.write(reinterpret_cast<const char*>(buf.data()),
		          static_cast<std::streamsize>(buf.size()));
	}
	{
		std::ofstream f(dir / "tree.gltf");
		f << R"({
  "asset": { "version": "2.0", "generator": "Unreal Engine 5.7 glTF Exporter" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes":  [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ {
      "attributes": { "POSITION": 0, "TEXCOORD_0": 1, "TEXCOORD_1": 2 },
      "indices": 3, "material": 0 } ] } ],
  "materials": [ {
    "name": "MI_CedarLeaves",
    "pbrMetallicRoughness": {
      "baseColorTexture": { "index": 0, "texCoord": 1, "extensions": {
        "KHR_texture_transform": { "offset": [-0.008, -0.019], "scale": [1.026, 2.364] } } },
      "metallicRoughnessTexture": { "index": 1, "texCoord": 1, "extensions": {
        "KHR_texture_transform": { "offset": [-0.008, -0.019], "scale": [1.026, 2.364] } } } },
    "normalTexture": { "index": 2, "texCoord": 1, "extensions": {
      "KHR_texture_transform": { "offset": [-0.008, -0.019], "scale": [1.026, 2.364] } } },
    "alphaMode": "MASK", "alphaCutoff": 0.3333, "doubleSided": true,
    "extensions": { "KHR_materials_specular": { "specularFactor": 0.03 } } } ],
  "textures": [ { "source": 0 }, { "source": 1 }, { "source": 2 } ],
  "images":   [ { "uri": "bc.png" }, { "uri": "mr.png" }, { "uri": "nm.png" } ],
  "extensionsUsed": [ "KHR_texture_transform", "KHR_materials_specular" ],
  "buffers":    [ { "uri": "tree.bin", "byteLength": 90 } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,  "byteLength": 36 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 24 },
    { "buffer": 0, "byteOffset": 60, "byteLength": 24 },
    { "buffer": 0, "byteOffset": 84, "byteLength": 6 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
      "min": [0,0,0], "max": [1,1,0] },
    { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2" },
    { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2" },
    { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" }
  ]
})";
	}

	const fs::path contentRoot = dir / "Content";
	std::error_code ec;
	fs::create_directories(contentRoot, ec);

	auto mesh = MeshImporter::import(dir / "tree.gltf", contentRoot, "Imported");
	REQUIRE(mesh != nullptr);
	CHECK(mesh->materialPath == "Imported/MI_CedarLeaves.hasset");

	LoadedMaterial loaded(contentRoot, "Imported/MI_CedarLeaves.hasset");
	REQUIRE(loaded.mat != nullptr);
	REQUIRE_FALSE(loaded.mat->customShaderFragGlsl.empty());
	REQUIRE_FALSE(loaded.mat->customShaderGBufGlsl.empty());
	CHECK(loaded.mat->blendMode == static_cast<uint8_t>(HE::MatBlendMode::Masked));
	CHECK(loaded.mat->graphTexturePaths.size() == 3);

	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;

	// Forward pass — the one every backend builds.
	const uint64_t hFwd = std::hash<std::string>{}(loaded.mat->customShaderFragGlsl);
	const auto& msl = lib.fragment(hFwd, loaded.mat->customShaderFragGlsl, B::Metal);
	CHECK_MESSAGE(msl.ok, "imported material failed to compile for Metal: ", msl.log);
	const auto& gl = lib.fragment(hFwd, loaded.mat->customShaderFragGlsl, B::GLSL410);
	CHECK_MESSAGE(gl.ok, "imported material failed to compile for GL: ", gl.log);

	// …and the deferred G-buffer variant, which the packer ships verbatim and which
	// no other assertion here would notice was broken.
	const uint64_t hGB = std::hash<std::string>{}(loaded.mat->customShaderGBufGlsl);
	const auto& mslGB = lib.fragment(hGB, loaded.mat->customShaderGBufGlsl, B::Metal);
	CHECK_MESSAGE(mslGB.ok, "imported G-buffer variant failed for Metal: ", mslGB.log);
	const auto& glGB = lib.fragment(hGB, loaded.mat->customShaderGBufGlsl, B::GLSL410);
	CHECK_MESSAGE(glGB.ok, "imported G-buffer variant failed for GL: ", glGB.log);

	he_test::removeAllQuiet(dir);
}
#endif // HE_TESTS_HAVE_SHADERC
