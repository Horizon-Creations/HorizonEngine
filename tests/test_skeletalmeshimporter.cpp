#include "doctest.h"
#include "TestFsUtil.h"
#include "SkeletalMeshImporter.h"
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <vector>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
//  Minimal glTF fixture
//
//  Binary buffer layout (232 bytes):
//    offset  0 :  36 bytes — POSITION  vec3 float32 ×3
//    offset 36 :   6 bytes — indices   uint16       ×3
//    offset 42 :   2 bytes — padding
//    offset 44 :  12 bytes — JOINTS_0  vec4 uint8   ×3
//    offset 56 :  48 bytes — WEIGHTS_0 vec4 float32 ×3
//    offset 104: 128 bytes — IBM       mat4 float32 ×2  (column-major)
//
//  Scene: node 0="root" (children=[1]), node 1="hip", node 2=mesh+skin
//  Skin joints: [0, 1]  →  root=joint 0 (parent -1), hip=joint 1 (parent 0)
//  IBM 0: identity;  IBM 1: T(0,-1,0)
// ─────────────────────────────────────────────────────────────────────────────
namespace
{

bool writeTestGltf(const fs::path& dir)
{
    std::vector<uint8_t> buf(232, 0);

    // positions: (0,0,0), (1,0,0), (0,1,0)
    float pos[9] = { 0,0,0,  1,0,0,  0,1,0 };
    std::memcpy(buf.data() +  0, pos, sizeof(pos));

    // indices: 0,1,2 as uint16
    uint16_t idx[3] = { 0, 1, 2 };
    std::memcpy(buf.data() + 36, idx, sizeof(idx));
    // 2 bytes padding to reach offset 44

    // joints: v0→joint0, v1→joint1, v2→joint0
    uint8_t jts[12] = { 0,0,0,0,  1,0,0,0,  0,0,0,0 };
    std::memcpy(buf.data() + 44, jts, sizeof(jts));

    // weights: all 100% on first influence
    float wts[12] = { 1,0,0,0,  1,0,0,0,  1,0,0,0 };
    std::memcpy(buf.data() + 56, wts, sizeof(wts));

    // IBM 0: identity (col-major)
    float ibm0[16] = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1 };
    std::memcpy(buf.data() + 104, ibm0, sizeof(ibm0));

    // IBM 1: inverse of T(0,1,0) = T(0,-1,0);  col3=(0,-1,0,1)
    float ibm1[16] = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,-1,0,1 };
    std::memcpy(buf.data() + 168, ibm1, sizeof(ibm1));

    const fs::path binPath = dir / "sm_skin_test.bin";
    {
        std::ofstream f(binPath, std::ios::binary);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
    }

    const std::string gltf = R"({
"asset":{"version":"2.0"},
"scene":0,
"scenes":[{"nodes":[0,2]}],
"nodes":[
  {"name":"root","children":[1]},
  {"name":"hip"},
  {"mesh":0,"skin":0}
],
"skins":[{"inverseBindMatrices":4,"joints":[0,1],"name":"Armature"}],
"meshes":[{"name":"TestMesh","primitives":[{
  "attributes":{"POSITION":0,"JOINTS_0":2,"WEIGHTS_0":3},
  "indices":1,
  "mode":4
}]}],
"accessors":[
  {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
  {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"},
  {"bufferView":2,"componentType":5121,"count":3,"type":"VEC4"},
  {"bufferView":3,"componentType":5126,"count":3,"type":"VEC4"},
  {"bufferView":4,"componentType":5126,"count":2,"type":"MAT4"}
],
"bufferViews":[
  {"buffer":0,"byteOffset":0,  "byteLength":36},
  {"buffer":0,"byteOffset":36, "byteLength":6},
  {"buffer":0,"byteOffset":44, "byteLength":12},
  {"buffer":0,"byteOffset":56, "byteLength":48},
  {"buffer":0,"byteOffset":104,"byteLength":128}
],
"buffers":[{"uri":"sm_skin_test.bin","byteLength":232}]
})";

    const fs::path gltfPath = dir / "sm_skin_test.gltf";
    std::ofstream f(gltfPath);
    if (!f) return false;
    f << gltf;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Node-transform fixture — regression guard against a transposed node matrix
//
//  Reuses the binary buffer written by writeTestGltf() and adds a SECOND,
//  UN-skinned mesh node whose world transform is deliberately non-symmetric:
//
//    node "prop_parent" : translation (10,0,0)
//      └─ node "prop"   : matrix = rotZ(+90°) with translation (2,3,4)
//
//  world = T(10,0,0) * M  →  columns (0,1,0,0) (-1,0,0,0) (0,0,1,0) (12,3,4,1)
//
//  A symmetric matrix cannot detect a transpose, this one can: transposing it
//  moves the translation out of the 4th column into the bottom row, so the
//  local origin would stay at (0,0,0) instead of landing at (12,3,4).
// ─────────────────────────────────────────────────────────────────────────────
bool writeUnskinnedNodeGltf(const fs::path& dir)
{
    if (!writeTestGltf(dir)) return false; // writes sm_skin_test.bin, reused here

    const std::string gltf = R"({
"asset":{"version":"2.0"},
"scene":0,
"scenes":[{"nodes":[0,2,3]}],
"nodes":[
  {"name":"root","children":[1]},
  {"name":"hip"},
  {"name":"skinned","mesh":0,"skin":0},
  {"name":"prop_parent","translation":[10,0,0],"children":[4]},
  {"name":"prop","mesh":1,"matrix":[0,1,0,0, -1,0,0,0, 0,0,1,0, 2,3,4,1]}
],
"skins":[{"inverseBindMatrices":4,"joints":[0,1],"name":"Armature"}],
"meshes":[
  {"name":"SkinnedMesh","primitives":[{
    "attributes":{"POSITION":0,"JOINTS_0":2,"WEIGHTS_0":3},"indices":1,"mode":4}]},
  {"name":"PropMesh","primitives":[{
    "attributes":{"POSITION":0},"indices":1,"mode":4}]}
],
"accessors":[
  {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
  {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"},
  {"bufferView":2,"componentType":5121,"count":3,"type":"VEC4"},
  {"bufferView":3,"componentType":5126,"count":3,"type":"VEC4"},
  {"bufferView":4,"componentType":5126,"count":2,"type":"MAT4"}
],
"bufferViews":[
  {"buffer":0,"byteOffset":0,  "byteLength":36},
  {"buffer":0,"byteOffset":36, "byteLength":6},
  {"buffer":0,"byteOffset":44, "byteLength":12},
  {"buffer":0,"byteOffset":56, "byteLength":48},
  {"buffer":0,"byteOffset":104,"byteLength":128}
],
"buffers":[{"uri":"sm_skin_test.bin","byteLength":232}]
})";

    std::ofstream f(dir / "sm_prop_test.gltf");
    if (!f) return false;
    f << gltf;
    return true;
}

// A 1x1 red PNG (truecolor, 8 bit) — the smallest real image stb_image decodes,
// so the fixture below can reference an actual base-color texture on disk.
const uint8_t kPng1x1[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
    0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
    0x00, 0x03, 0x01, 0x01, 0x00, 0xF7, 0x03, 0x41, 0x43, 0x00, 0x00, 0x00,
    0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
};

// The skinned fixture plus a material whose base color texture points at that
// PNG — reuses the binary buffer written by writeTestGltf().
bool writeTexturedSkinnedGltf(const fs::path& dir)
{
    if (!writeTestGltf(dir)) return false;  // writes sm_skin_test.bin, reused here
    {
        std::ofstream png(dir / "sm_tex.png", std::ios::binary);
        if (!png) return false;
        png.write(reinterpret_cast<const char*>(kPng1x1),
                  static_cast<std::streamsize>(sizeof(kPng1x1)));
    }

    const std::string gltf = R"({
"asset":{"version":"2.0"},
"scene":0,
"scenes":[{"nodes":[0,2]}],
"nodes":[
  {"name":"root","children":[1]},
  {"name":"hip"},
  {"mesh":0,"skin":0}
],
"skins":[{"inverseBindMatrices":4,"joints":[0,1],"name":"Armature"}],
"meshes":[{"name":"TestMesh","primitives":[{
  "attributes":{"POSITION":0,"JOINTS_0":2,"WEIGHTS_0":3},
  "indices":1,
  "material":0,
  "mode":4
}]}],
"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],
"textures":[{"source":0}],
"images":[{"uri":"sm_tex.png"}],
"accessors":[
  {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
  {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"},
  {"bufferView":2,"componentType":5121,"count":3,"type":"VEC4"},
  {"bufferView":3,"componentType":5126,"count":3,"type":"VEC4"},
  {"bufferView":4,"componentType":5126,"count":2,"type":"MAT4"}
],
"bufferViews":[
  {"buffer":0,"byteOffset":0,  "byteLength":36},
  {"buffer":0,"byteOffset":36, "byteLength":6},
  {"buffer":0,"byteOffset":44, "byteLength":12},
  {"buffer":0,"byteOffset":56, "byteLength":48},
  {"buffer":0,"byteOffset":104,"byteLength":128}
],
"buffers":[{"uri":"sm_skin_test.bin","byteLength":232}]
})";

    std::ofstream f(dir / "sm_tex_test.gltf");
    if (!f) return false;
    f << gltf;
    return true;
}

} // namespace

TEST_CASE("SkeletalMeshImporter invalid path returns null")
{
    auto result = SkeletalMeshImporter::import(
        "/nonexistent/totally_fake_file.gltf",
        fs::temp_directory_path());
    CHECK(result == nullptr);
}

TEST_CASE("SkeletalMeshImporter skeleton joint count and parent chain")
{
    const fs::path dir     = fs::temp_directory_path() / "he_test_skelimport";
    const fs::path content = dir / "content";
    fs::create_directories(dir);
    fs::create_directories(content);
    REQUIRE(writeTestGltf(dir));

    auto mesh = SkeletalMeshImporter::import(dir / "sm_skin_test.gltf", content);
    REQUIRE(mesh != nullptr);
    REQUIRE(mesh->skeleton.size() == 2);

    CHECK(mesh->skeleton[0].name   == "root");
    CHECK(mesh->skeleton[0].parent == -1);
    CHECK(mesh->skeleton[1].name   == "hip");
    CHECK(mesh->skeleton[1].parent == 0);

    he_test::removeAllQuiet(dir);
}

TEST_CASE("SkeletalMeshImporter inverse bind matrices")
{
    const fs::path dir     = fs::temp_directory_path() / "he_test_skelibm";
    const fs::path content = dir / "content";
    fs::create_directories(dir);
    fs::create_directories(content);
    REQUIRE(writeTestGltf(dir));

    auto mesh = SkeletalMeshImporter::import(dir / "sm_skin_test.gltf", content);
    REQUIRE(mesh != nullptr);
    REQUIRE(mesh->skeleton.size() == 2);

    // Joint 0: identity IBM
    CHECK(mesh->skeleton[0].inverseBindMatrix[0]  == doctest::Approx(1.0f));
    CHECK(mesh->skeleton[0].inverseBindMatrix[5]  == doctest::Approx(1.0f));
    CHECK(mesh->skeleton[0].inverseBindMatrix[10] == doctest::Approx(1.0f));
    CHECK(mesh->skeleton[0].inverseBindMatrix[15] == doctest::Approx(1.0f));
    CHECK(mesh->skeleton[0].inverseBindMatrix[12] == doctest::Approx(0.0f));

    // Joint 1: T(0,-1,0) — column 3 = (0,-1,0,1)
    CHECK(mesh->skeleton[1].inverseBindMatrix[12] == doctest::Approx(0.0f));
    CHECK(mesh->skeleton[1].inverseBindMatrix[13] == doctest::Approx(-1.0f));
    CHECK(mesh->skeleton[1].inverseBindMatrix[14] == doctest::Approx(0.0f));
    CHECK(mesh->skeleton[1].inverseBindMatrix[15] == doctest::Approx(1.0f));

    he_test::removeAllQuiet(dir);
}

TEST_CASE("SkeletalMeshImporter per-vertex bone IDs and weights")
{
    const fs::path dir     = fs::temp_directory_path() / "he_test_skelbones";
    const fs::path content = dir / "content";
    fs::create_directories(dir);
    fs::create_directories(content);
    REQUIRE(writeTestGltf(dir));

    auto mesh = SkeletalMeshImporter::import(dir / "sm_skin_test.gltf", content);
    REQUIRE(mesh != nullptr);
    REQUIRE(mesh->boneIDs.size()     == 12); // 3 verts × 4
    REQUIRE(mesh->boneWeights.size() == 12);

    // vertex 0 → joint 0
    CHECK(mesh->boneIDs[0] == 0u);
    CHECK(mesh->boneWeights[0] == doctest::Approx(1.0f));
    // vertex 1 → joint 1
    CHECK(mesh->boneIDs[4] == 1u);
    CHECK(mesh->boneWeights[4] == doctest::Approx(1.0f));
    // vertex 2 → joint 0
    CHECK(mesh->boneIDs[8] == 0u);

    he_test::removeAllQuiet(dir);
}

TEST_CASE("SkeletalMeshImporter geometry (vertices and indices)")
{
    const fs::path dir     = fs::temp_directory_path() / "he_test_skelgeom";
    const fs::path content = dir / "content";
    fs::create_directories(dir);
    fs::create_directories(content);
    REQUIRE(writeTestGltf(dir));

    auto mesh = SkeletalMeshImporter::import(dir / "sm_skin_test.gltf", content);
    REQUIRE(mesh != nullptr);

    REQUIRE(mesh->vertices.size() == 9); // 3 verts × xyz
    CHECK(mesh->vertices[0] == doctest::Approx(0.0f)); // v0.x
    CHECK(mesh->vertices[3] == doctest::Approx(1.0f)); // v1.x
    CHECK(mesh->vertices[7] == doctest::Approx(1.0f)); // v2.y

    REQUIRE(mesh->indices.size() == 3);
    CHECK(mesh->indices[0] == 0u);
    CHECK(mesh->indices[1] == 1u);
    CHECK(mesh->indices[2] == 2u);

    he_test::removeAllQuiet(dir);
}

TEST_CASE("SkeletalMeshImporter bakes un-skinned node transforms column-major")
{
    const fs::path dir     = fs::temp_directory_path() / "he_test_skelnodexform";
    const fs::path content = dir / "content";
    fs::create_directories(dir);
    fs::create_directories(content);
    REQUIRE(writeUnskinnedNodeGltf(dir));

    auto mesh = SkeletalMeshImporter::import(dir / "sm_prop_test.gltf", content);
    REQUIRE(mesh != nullptr);

    // Node order in the file: skinned mesh node first (3 verts), prop node second.
    REQUIRE(mesh->vertices.size() == 18); // 6 verts x xyz

    // The skinned node must stay untransformed — the skin/IBM math places it.
    CHECK(mesh->vertices[0] == doctest::Approx(0.0f));
    CHECK(mesh->vertices[3] == doctest::Approx(1.0f));
    CHECK(mesh->vertices[7] == doctest::Approx(1.0f));

    // The un-skinned prop node must be baked with its world matrix read
    // column-major. cgltf hands out a column-major float[16] and glm::make_mat4
    // consumes exactly that, so no transpose may be applied.
    //   local (0,0,0) -> (12,3,4)   (transposed it would wrongly stay at origin)
    //   local (1,0,0) -> (12,4,4)   (transposed: (0,-1,0))
    //   local (0,1,0) -> (11,3,4)   (transposed: (1, 0,0))
    CHECK(mesh->vertices[9]  == doctest::Approx(12.0f));
    CHECK(mesh->vertices[10] == doctest::Approx(3.0f));
    CHECK(mesh->vertices[11] == doctest::Approx(4.0f));

    CHECK(mesh->vertices[12] == doctest::Approx(12.0f));
    CHECK(mesh->vertices[13] == doctest::Approx(4.0f));
    CHECK(mesh->vertices[14] == doctest::Approx(4.0f));

    CHECK(mesh->vertices[15] == doctest::Approx(11.0f));
    CHECK(mesh->vertices[16] == doctest::Approx(3.0f));
    CHECK(mesh->vertices[17] == doctest::Approx(4.0f));

    // Second primitive's indices are rebased onto its own vertex block
    REQUIRE(mesh->indices.size() == 6);
    CHECK(mesh->indices[3] == 3u);
    CHECK(mesh->indices[4] == 4u);
    CHECK(mesh->indices[5] == 5u);

    he_test::removeAllQuiet(dir);
}

TEST_CASE("SkeletalMeshImporter output is written to disk and loadable")
{
    const fs::path dir     = fs::temp_directory_path() / "he_test_skelwrite";
    const fs::path content = dir / "content";
    fs::create_directories(dir);
    fs::create_directories(content);
    REQUIRE(writeTestGltf(dir));

    auto mesh = SkeletalMeshImporter::import(dir / "sm_skin_test.gltf", content);
    REQUIRE(mesh != nullptr);
    REQUIRE_FALSE(mesh->path.empty());

    // The .hasset file must exist on disk
    const fs::path onDisk = content / mesh->path;
    CHECK(fs::exists(onDisk));

    // Reload via ContentManager and verify skeleton round-trips through disk
    ContentManager cm(content.string());
    HE::UUID id = cm.loadAsset(mesh->path);
    const SkeletalMeshAsset* loaded = cm.getSkeletalMesh(id);
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->skeleton.size() == 2);
    CHECK(loaded->skeleton[0].name == "root");
    CHECK(loaded->skeleton[1].name == "hip");
    CHECK(loaded->skeleton[1].parent == 0);

    he_test::removeAllQuiet(dir);
}

// SkeletalMeshAsset::materialPath lands in chunk MREF, which every renderer
// resolves through ContentManager::resolveMaterialRef — as a MATERIAL. The
// importer used to store the base-color TEXTURE path there instead, so the
// lookup came back empty and skinned meshes rendered untextured. It now writes
// the same texture + material pair the static MeshImporter does.
TEST_CASE("SkeletalMeshImporter's material reference resolves as a material")
{
    const fs::path dir     = fs::temp_directory_path() / "he_test_skelmat";
    const fs::path content = dir / "content";
    he_test::removeAllQuiet(dir);
    fs::create_directories(dir);
    fs::create_directories(content);
    REQUIRE(writeTexturedSkinnedGltf(dir));

    auto mesh = SkeletalMeshImporter::import(dir / "sm_tex_test.gltf", content, "Imported");
    REQUIRE(mesh != nullptr);
    CHECK(mesh->path == "Imported/sm_tex_test_skeletal.hasset");
    REQUIRE(mesh->materialPath == "Imported/sm_tex_test_mat.hasset");

    ContentManager cm(content.string());
    const MaterialAsset* mat = cm.resolveMaterialRef(HE::UUID{}, mesh->materialPath);
    REQUIRE(mat != nullptr);                      // was nullptr with a texture path
    REQUIRE(mat->texturePaths.size() == 1);
    CHECK(mat->texturePaths[0] == "Imported/sm_tex.hasset");
    CHECK(cm.resolveTextureRef(HE::UUID{}, mat->texturePaths[0]) != nullptr);

    // The mesh survives a round trip through disk with that reference intact.
    const SkeletalMeshAsset* loaded = cm.getSkeletalMesh(cm.loadAsset(mesh->path));
    REQUIRE(loaded != nullptr);
    CHECK(loaded->materialPath == mesh->materialPath);

    he_test::removeAllQuiet(dir);
}
