#include "doctest.h"
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

    fs::remove_all(dir);
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

    fs::remove_all(dir);
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

    fs::remove_all(dir);
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

    fs::remove_all(dir);
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

    fs::remove_all(dir);
}
