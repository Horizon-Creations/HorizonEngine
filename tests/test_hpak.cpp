#include "doctest.h"
#include <Hpak/HpakWriter.h>
#include <Hpak/HpakReader.h>
#include <Hpak/HpakFormat.h>
#include <Hpak/KeyDerivation.h>
#include <ContentManager/HAsset.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/AudioSourceComponent.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <cstring>

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::vector<uint8_t> makeMaterialBlob(const HE::UUID& id,
                                              const std::string& name = "test_mat",
                                              float r = 1.f, float g = 0.f, float b = 0.f)
{
    std::vector<uint8_t> meta;
    const uint16_t typeVal = static_cast<uint16_t>(HE::AssetType::Material);
    HAsset::Writer::appendPOD(meta, typeVal);
    HAsset::Writer::appendPOD(meta, id.hi);
    HAsset::Writer::appendPOD(meta, id.lo);
    HAsset::Writer::appendString(meta, name);
    HAsset::Writer::appendString(meta, "mem://" + name);

    std::vector<uint8_t> mtrl;
    HAsset::Writer::appendString(mtrl, "");
    const uint64_t texCount = 0;
    HAsset::Writer::appendPOD(mtrl, texCount);
    HAsset::Writer::appendPOD(mtrl, r);
    HAsset::Writer::appendPOD(mtrl, g);
    HAsset::Writer::appendPOD(mtrl, b);
    float metallic = 0.f, roughness = 0.5f, opacity = 1.f;
    HAsset::Writer::appendPOD(mtrl, metallic);
    HAsset::Writer::appendPOD(mtrl, roughness);
    HAsset::Writer::appendPOD(mtrl, opacity);

    HAsset::Writer w;
    w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
    w.addChunk(HAsset::CHUNK_MTRL, mtrl.data(), mtrl.size());
    return w.toBytes(static_cast<uint16_t>(HE::AssetType::Material));
}

// Flip one byte at `offset` in a file (corruption injection).
static void flipByteAt(const std::filesystem::path& p, std::streamoff offset)
{
    std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
    f.seekg(offset); char c; f.read(&c, 1);
    c = static_cast<char>(c ^ 0xFF);
    f.seekp(offset); f.write(&c, 1);
}

// ─── Format layout ─────────────────────────────────────────────────────────────

TEST_CASE("hpak v2 struct sizes are fixed")
{
    CHECK(sizeof(Hpak::FileHeader) == 64);
    CHECK(sizeof(Hpak::EntryDesc)  == 56);
    CHECK(Hpak::k_version == 2);
}

// ─── Round-trip basics ─────────────────────────────────────────────────────────

TEST_CASE("HpakWriter/HpakReader round-trip single entry")
{
    const HE::UUID id{0xDEADBEEF12345678ULL, 0x0102030405060708ULL};
    auto blob = makeMaterialBlob(id, "mat_single");

    HpakWriter packer;
    packer.addEntry(id, blob);
    CHECK(packer.entryCount() == 1);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_single.hpak";
    REQUIRE(packer.write(tmp.string()));

    HpakReader reader;
    REQUIRE(reader.open(tmp.string()));
    REQUIRE(reader.hasEntry(id));

    const auto out = reader.readEntry(id);
    CHECK(out == blob);

    std::filesystem::remove(tmp);
}

TEST_CASE("HpakWriter/HpakReader multiple entries, sorted TOC + binary search")
{
    // Insert deliberately out of UUID order to exercise the sort + binary search.
    const HE::UUID id1{3,3}, id2{1,1}, id3{2,2}, id4{1,5};
    const auto b1 = makeMaterialBlob(id1, "mat1", 1.f,0.f,0.f);
    const auto b2 = makeMaterialBlob(id2, "mat2", 0.f,1.f,0.f);
    const auto b3 = makeMaterialBlob(id3, "mat3", 0.f,0.f,1.f);
    const auto b4 = makeMaterialBlob(id4, "mat4", 0.2f,0.3f,0.4f);

    HpakWriter packer;
    packer.addEntry(id1, b1);
    packer.addEntry(id2, b2);
    packer.addEntry(id3, b3);
    packer.addEntry(id4, b4);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_multi.hpak";
    REQUIRE(packer.write(tmp.string()));

    HpakReader reader;
    REQUIRE(reader.open(tmp.string()));

    const auto ids = reader.enumerate();
    REQUIRE(ids.size() == 4);
    // enumerate() returns stored order → must be ascending by (hi,lo)
    for (size_t i = 1; i < ids.size(); ++i)
    {
        const bool ordered = (ids[i-1].hi < ids[i].hi) ||
                             (ids[i-1].hi == ids[i].hi && ids[i-1].lo <= ids[i].lo);
        CHECK(ordered);
    }

    CHECK(reader.readEntry(id1) == b1);
    CHECK(reader.readEntry(id2) == b2);
    CHECK(reader.readEntry(id3) == b3);
    CHECK(reader.readEntry(id4) == b4);

    std::filesystem::remove(tmp);
}

TEST_CASE("HpakReader returns empty for missing entry")
{
    const HE::UUID id{42,42};
    const auto blob = makeMaterialBlob(id, "present");

    HpakWriter packer;
    packer.addEntry(id, blob);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_missing.hpak";
    REQUIRE(packer.write(tmp.string()));

    HpakReader reader;
    REQUIRE(reader.open(tmp.string()));

    const HE::UUID missing{99,99};
    CHECK(!reader.hasEntry(missing));
    CHECK(reader.readEntry(missing).empty());

    std::filesystem::remove(tmp);
}

TEST_CASE("Empty HpakWriter writes and reads back correctly")
{
    HpakWriter packer;
    auto tmp = std::filesystem::temp_directory_path() / "he_test_empty.hpak";
    REQUIRE(packer.write(tmp.string()));

    HpakReader reader;
    REQUIRE(reader.open(tmp.string()));
    CHECK(reader.enumerate().empty());

    std::filesystem::remove(tmp);
}

// ─── Codec matrix ────────────────────────────────────────────────────────────

TEST_CASE("Codec round-trip: store / lz4 / zstd all reproduce the input")
{
    const HE::UUID id{0xC0DEC0DEC0DEC0DEULL, 0x1111222233334444ULL};
    // Highly compressible + structured payload so compression is exercised.
    std::vector<uint8_t> data(8192);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>((i / 64) & 0xFF);

    const Hpak::Codec codecs[] = { Hpak::Codec::Store, Hpak::Codec::LZ4, Hpak::Codec::Zstd };
    for (Hpak::Codec codec : codecs)
    {
        Hpak::PackSettings s; s.codec = codec;
        HpakWriter packer;
        packer.addEntry(id, data, s);

        auto tmp = std::filesystem::temp_directory_path() / "he_test_codec.hpak";
        REQUIRE(packer.write(tmp.string()));

        HpakReader reader;
        REQUIRE(reader.open(tmp.string()));
        const auto out = reader.readEntry(id);
        CHECK(out == data);   // round-trips regardless of codec availability
        std::filesystem::remove(tmp);
    }
}

#ifdef HE_HAVE_LZ4
TEST_CASE("LZ4 codec actually shrinks compressible data")
{
    const HE::UUID id{0xAAAA,0xBBBB};
    std::vector<uint8_t> data(16384, 0x42);
    Hpak::PackSettings s; s.codec = Hpak::Codec::LZ4;
    HpakWriter packer; packer.addEntry(id, data, s);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_lz4.hpak";
    REQUIRE(packer.write(tmp.string()));
    const auto fileSize = std::filesystem::file_size(tmp);
    CHECK(fileSize < sizeof(Hpak::FileHeader) + sizeof(Hpak::EntryDesc) + data.size());

    HpakReader reader; REQUIRE(reader.open(tmp.string()));
    CHECK(reader.readEntry(id) == data);
    std::filesystem::remove(tmp);
}
#endif

#ifdef HE_HAVE_ZSTD
TEST_CASE("zstd codec actually shrinks compressible data")
{
    const HE::UUID id{0xCCCC,0xDDDD};
    std::vector<uint8_t> data(16384, 0x37);
    Hpak::PackSettings s; s.codec = Hpak::Codec::Zstd;
    HpakWriter packer; packer.addEntry(id, data, s);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_zstd.hpak";
    REQUIRE(packer.write(tmp.string()));
    const auto fileSize = std::filesystem::file_size(tmp);
    CHECK(fileSize < sizeof(Hpak::FileHeader) + sizeof(Hpak::EntryDesc) + data.size());

    HpakReader reader; REQUIRE(reader.open(tmp.string()));
    CHECK(reader.readEntry(id) == data);
    std::filesystem::remove(tmp);
}
#endif

// ─── Encryption (AES-256-GCM, Phase B) ─────────────────────────────────────────

#ifdef HE_HAVE_OPENSSL
TEST_CASE("AES-256-GCM round-trip (store + encrypt)")
{
    const HE::UUID id{0xABCD,0xEF01};
    const std::vector<uint8_t> data = {10,20,30,40,50,60,70,80};

    Hpak::PackSettings s;
    s.encrypt = true;
    for (int i = 0; i < 32; ++i) s.key[i] = static_cast<uint8_t>(i + 1);

    HpakWriter packer; packer.addEntry(id, data, s);
    auto tmp = std::filesystem::temp_directory_path() / "he_test_enc.hpak";
    REQUIRE(packer.write(tmp.string()));

    HpakReader reader; REQUIRE(reader.open(tmp.string()));
    CHECK(reader.readEntry(id).empty());                 // no key → cannot decrypt

    uint8_t wrong[32]; for (int i=0;i<32;++i) wrong[i] = static_cast<uint8_t>(i + 2);
    CHECK(reader.readEntry(id, wrong).empty());          // wrong key → GCM tag mismatch

    const auto decrypted = reader.readEntry(id, s.key);  // correct key
    CHECK(decrypted == data);

    std::filesystem::remove(tmp);
}

TEST_CASE("AES-256-GCM uses a unique nonce per entry")
{
    // Two entries with identical plaintext + key must produce different stored
    // bytes (fresh random nonce each) — the property XOR lacked.
    const HE::UUID id1{1,1}, id2{2,2};
    const std::vector<uint8_t> data(64, 0x5A);
    Hpak::PackSettings s; s.encrypt = true;
    for (int i = 0; i < 32; ++i) s.key[i] = static_cast<uint8_t>(i + 5);

    HpakWriter packer; packer.addEntry(id1, data, s); packer.addEntry(id2, data, s);
    auto tmp = std::filesystem::temp_directory_path() / "he_test_nonce.hpak";
    REQUIRE(packer.write(tmp.string()));

    // Read the two data blocks straight off disk and confirm they differ.
    std::ifstream f(tmp, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t dataStart = sizeof(Hpak::FileHeader) + 2 * sizeof(Hpak::EntryDesc);
    const size_t blockLen  = data.size() + 16; // ciphertext + tag
    REQUIRE(bytes.size() >= dataStart + 2 * blockLen);
    const std::vector<uint8_t> b1(bytes.begin()+dataStart, bytes.begin()+dataStart+blockLen);
    const std::vector<uint8_t> b2(bytes.begin()+dataStart+blockLen, bytes.begin()+dataStart+2*blockLen);
    CHECK(b1 != b2);

    HpakReader reader; REQUIRE(reader.open(tmp.string()));
    CHECK(reader.readEntry(id1, s.key) == data);
    CHECK(reader.readEntry(id2, s.key) == data);
    std::filesystem::remove(tmp);
}

TEST_CASE("Compression + encryption round-trip")
{
    const HE::UUID id{0xE0E0,0xF0F0};
    std::vector<uint8_t> data(2048);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i & 0xFF);

    Hpak::PackSettings s;
    s.codec   = Hpak::Codec::Zstd;
    s.encrypt = true;
    for (int i = 0; i < 32; ++i) s.key[i] = static_cast<uint8_t>(i + 7);

    HpakWriter packer; packer.addEntry(id, data, s);
    auto tmp = std::filesystem::temp_directory_path() / "he_test_encomp.hpak";
    REQUIRE(packer.write(tmp.string()));

    HpakReader reader; REQUIRE(reader.open(tmp.string()));
    CHECK(reader.readEntry(id).empty());                 // no key → cannot decode
    const auto out = reader.readEntry(id, s.key);        // key → decrypt then decompress
    REQUIRE(out.size() == data.size());
    CHECK(out == data);

    std::filesystem::remove(tmp);
}
#endif // HE_HAVE_OPENSSL

// ─── Integrity ──────────────────────────────────────────────────────────────

TEST_CASE("Corrupt TOC byte → open() fails (tocHash mismatch)")
{
    const HE::UUID id{7,7};
    const auto blob = makeMaterialBlob(id, "toc_guard");
    HpakWriter packer; packer.addEntry(id, blob);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_toc_corrupt.hpak";
    REQUIRE(packer.write(tmp.string()));

    // Flip a byte inside the first EntryDesc (TOC region starts right after the 64B header)
    flipByteAt(tmp, static_cast<std::streamoff>(sizeof(Hpak::FileHeader)));

    HpakReader reader;
    CHECK(!reader.open(tmp.string()));  // TOC hash no longer matches

    std::filesystem::remove(tmp);
}

TEST_CASE("Corrupt data byte → readEntry() returns empty (contentHash mismatch)")
{
    const HE::UUID id{8,8};
    const auto blob = makeMaterialBlob(id, "data_guard");
    HpakWriter packer; packer.addEntry(id, blob);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_data_corrupt.hpak";
    REQUIRE(packer.write(tmp.string()));

    // Flip a byte in the data region (immediately after header + 1 EntryDesc)
    const auto dataStart = static_cast<std::streamoff>(sizeof(Hpak::FileHeader) + sizeof(Hpak::EntryDesc));
    flipByteAt(tmp, dataStart);

    HpakReader reader;
    REQUIRE(reader.open(tmp.string()));      // TOC still valid
    CHECK(reader.readEntry(id).empty());     // stored bytes fail their content hash

    std::filesystem::remove(tmp);
}

TEST_CASE("A failed read does not poison subsequent reads on the same reader")
{
    // Regression: the reader holds one persistent file handle. A failed read
    // leaves a sticky failbit; if the entry guard is checked before clear(), one
    // truncated/corrupt entry disables every later read on that reader instance.
    const HE::UUID lo{1,1}, hi{2,2};              // hi sorts last → highest offset
    const auto bLo = makeMaterialBlob(lo, "lo");
    const auto bHi = makeMaterialBlob(hi, "hi");
    HpakWriter packer; packer.addEntry(lo, bLo); packer.addEntry(hi, bHi);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_poison.hpak";
    REQUIRE(packer.write(tmp.string()));

    // Cut the tail so the last (higher-UUID) entry is short; the lower one stays whole.
    const auto full = std::filesystem::file_size(tmp);
    std::filesystem::resize_file(tmp, full - 16);

    HpakReader reader;
    REQUIRE(reader.open(tmp.string()));      // header + TOC intact
    CHECK(reader.readEntry(hi).empty());     // truncated entry → short read fails
    CHECK(reader.readEntry(lo) == bLo);      // MUST still succeed (no failbit poisoning)

    std::filesystem::remove(tmp);
}

// ─── KeyDerivation (unchanged in Phase A) ──────────────────────────────────────

TEST_CASE("KeyDerivation is deterministic")
{
    const uint8_t salt[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    uint8_t key1[32] = {}, key2[32] = {};
    KeyDerivation::derive("my_secret_passphrase", salt, key1);
    KeyDerivation::derive("my_secret_passphrase", salt, key2);
    CHECK(std::memcmp(key1, key2, 32) == 0);
}

TEST_CASE("KeyDerivation different secrets yield different keys")
{
    const uint8_t salt[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    uint8_t key1[32] = {}, key2[32] = {};
    KeyDerivation::derive("secret_alpha", salt, key1);
    KeyDerivation::derive("secret_beta",  salt, key2);
    CHECK(std::memcmp(key1, key2, 32) != 0);
}

TEST_CASE("HpakWriter::addDirectory skips non-.hasset files")
{
    auto tmpDir = std::filesystem::temp_directory_path() / "he_test_hpak_dir";
    std::filesystem::remove_all(tmpDir);
    std::filesystem::create_directories(tmpDir);
    { std::ofstream f(tmpDir / "notes.txt"); f << "not an asset"; }
    { std::ofstream f(tmpDir / "dummy.hasset", std::ios::binary); f << "FAKE"; }

    HpakWriter packer;
    const int added = packer.addDirectory(tmpDir);
    CHECK(added == 0);
    std::filesystem::remove_all(tmpDir);
}

// ─── All asset types round-trip through ContentManager × codec ─────────────────
//
// The core guarantee the user asked for: every pak-serializable asset type is
// encoded into a .hpak and decoded back correctly, under each codec.

namespace {

struct TypeIds {
    HE::UUID mesh, skel, tex, mat, scene, script, shader, audio, font, anim;
};

// Author one asset of every serializable type into `dir` via ContentManager,
// returning their (minted) UUIDs. Values are distinctive so we can verify them.
static TypeIds authorAllTypes(const std::filesystem::path& dir)
{
    ContentManager cm(dir.string());
    TypeIds ids;

    StaticMeshAsset mesh; mesh.type = HE::AssetType::StaticMesh; mesh.name = "mesh"; mesh.path = "mesh.hasset";
    mesh.vertices = {0.f,0.f,0.f, 1.f,0.f,0.f, 0.f,1.f,0.f};
    mesh.indices  = {0,1,2};
    mesh.normals  = {0.f,0.f,1.f, 0.f,0.f,1.f, 0.f,0.f,1.f};
    mesh.uvs      = {0.f,0.f, 1.f,0.f, 0.f,1.f};
    REQUIRE(cm.saveAsset(mesh)); ids.mesh = mesh.id;

    SkeletalMeshAsset skel; skel.type = HE::AssetType::SkeletalMesh; skel.name = "skel"; skel.path = "skel.hasset";
    skel.vertices = {0.f,0.f,0.f, 1.f,0.f,0.f};
    skel.indices  = {0,1};
    skel.normals  = {0.f,1.f,0.f, 0.f,1.f,0.f};
    skel.uvs      = {0.f,0.f, 1.f,1.f};
    skel.boneIDs  = {0,0,0,0, 1,0,0,0};
    skel.boneWeights = {1.f,0.f,0.f,0.f, 1.f,0.f,0.f,0.f};
    { SkeletonJoint j; j.name = "root"; j.parent = -1; skel.skeleton.push_back(j); }
    REQUIRE(cm.saveAsset(skel)); ids.skel = skel.id;

    TextureAsset tex; tex.type = HE::AssetType::Texture; tex.name = "tex"; tex.path = "tex.hasset";
    tex.width = 2; tex.height = 2; tex.channels = 4;
    tex.data = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255};
    REQUIRE(cm.saveAsset(tex)); ids.tex = tex.id;

    MaterialAsset mat; mat.type = HE::AssetType::Material; mat.name = "mat"; mat.path = "mat.hasset";
    mat.shaderPath = "shaders/pbr"; mat.texturePaths = {"a.png","b.png"};
    mat.baseColor[0]=0.1f; mat.baseColor[1]=0.2f; mat.baseColor[2]=0.3f;
    mat.metallic=0.4f; mat.roughness=0.6f; mat.opacity=0.8f;
    REQUIRE(cm.saveAsset(mat)); ids.mat = mat.id;

    SceneAsset scene; scene.type = HE::AssetType::Scene; scene.name = "scene"; scene.path = "scene.hasset";
    scene.objectPaths = {"obj/a","obj/b","obj/c"};
    REQUIRE(cm.saveAsset(scene)); ids.scene = scene.id;

    ScriptAsset script; script.type = HE::AssetType::Script; script.name = "script"; script.path = "script.hasset";
    script.sourceCode = "function on_update(dt) end";
    REQUIRE(cm.saveAsset(script)); ids.script = script.id;

    ShaderAsset shader; shader.type = HE::AssetType::Shader; shader.name = "shader"; shader.path = "shader.hasset";
    shader.sourceCode = "#version 410\nvoid main(){}";
    REQUIRE(cm.saveAsset(shader)); ids.shader = shader.id;

    AudioAsset audio; audio.type = HE::AssetType::Audio; audio.name = "audio"; audio.path = "audio.hasset";
    audio.sampleRate = 44100; audio.channels = 2;
    audio.audioData = {1,2,3,4,5,6,7,8};
    REQUIRE(cm.saveAsset(audio)); ids.audio = audio.id;

    FontAsset font; font.type = HE::AssetType::Font; font.name = "font"; font.path = "font.hasset";
    font.size = 24; font.fontData = {0xAA,0xBB,0xCC,0xDD};
    REQUIRE(cm.saveAsset(font)); ids.font = font.id;

    AnimationClipAsset anim; anim.type = HE::AssetType::AnimationClip; anim.name = "anim"; anim.path = "anim.hasset";
    anim.duration = 2.5f;
    { AnimationChannel ch; ch.jointIndex = 0; ch.path = AnimPathType::Translation;
      ch.times = {0.f,1.f,2.5f}; ch.values = {0,0,0, 1,0,0, 2,0,0}; anim.channels.push_back(ch); }
    REQUIRE(cm.saveAsset(anim)); ids.anim = anim.id;

    return ids;
}

static void verifyAllTypes(Hpak::Codec codec)
{
    auto dir = std::filesystem::temp_directory_path() /
               ("he_types_" + std::to_string(static_cast<int>(codec)));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const TypeIds ids = authorAllTypes(dir);

    Hpak::PackSettings s; s.codec = codec;
    HpakWriter packer;
    const int added = packer.addDirectory(dir, s);
    CHECK(added == 10);

    auto pak = std::filesystem::temp_directory_path() /
               ("he_types_" + std::to_string(static_cast<int>(codec)) + ".hpak");
    REQUIRE(packer.write(pak.string()));

    ContentManager cm;                       // fresh manager (only default assets)
    REQUIRE(cm.loadPak(pak.string()));

    // Types with typed getters → verify identity + a representative field.
    const StaticMeshAsset* m = cm.getStaticMesh(ids.mesh);
    REQUIRE(m != nullptr);
    CHECK(m->indices == std::vector<uint32_t>{0,1,2});
    CHECK(m->vertices.size() == 9);

    const SkeletalMeshAsset* sk = cm.getSkeletalMesh(ids.skel);
    REQUIRE(sk != nullptr);
    CHECK(sk->boneIDs.size() == 8);
    REQUIRE(sk->skeleton.size() == 1);
    CHECK(sk->skeleton[0].name == "root");

    const TextureAsset* t = cm.getTexture(ids.tex);
    REQUIRE(t != nullptr);
    CHECK(t->width == 2); CHECK(t->height == 2); CHECK(t->channels == 4);
    CHECK(t->data.size() == 16);

    const MaterialAsset* mt = cm.getMaterial(ids.mat);
    REQUIRE(mt != nullptr);
    CHECK(mt->baseColor[0] == doctest::Approx(0.1f));
    CHECK(mt->baseColor[2] == doctest::Approx(0.3f));
    CHECK(mt->texturePaths.size() == 2);

    const ScriptAsset* scr = cm.getScript(ids.script);
    REQUIRE(scr != nullptr);
    CHECK(scr->sourceCode == "function on_update(dt) end");

    const ShaderAsset* sh = cm.getShader(ids.shader);
    REQUIRE(sh != nullptr);
    CHECK(sh->sourceCode.rfind("#version 410", 0) == 0);

    const AudioAsset* au = cm.getAudio(ids.audio);
    REQUIRE(au != nullptr);
    CHECK(au->sampleRate == 44100); CHECK(au->channels == 2);
    CHECK(au->audioData.size() == 8);

    const AnimationClipAsset* an = cm.getAnimationClip(ids.anim);
    REQUIRE(an != nullptr);
    CHECK(an->duration == doctest::Approx(2.5f));
    REQUIRE(an->channels.size() == 1);
    CHECK(an->channels[0].times.size() == 3);

    // Types without a typed getter (Scene, Font) → verify presence + type.
    CHECK(cm.isLoaded(ids.scene));
    CHECK(cm.assetType(ids.scene) == HE::AssetType::Scene);
    CHECK(cm.isLoaded(ids.font));
    CHECK(cm.assetType(ids.font)  == HE::AssetType::Font);

    std::filesystem::remove(pak);
    std::filesystem::remove_all(dir);
}

} // namespace

TEST_CASE("All asset types round-trip through a Store pak")  { verifyAllTypes(Hpak::Codec::Store); }
TEST_CASE("All asset types round-trip through an LZ4 pak")   { verifyAllTypes(Hpak::Codec::LZ4);   }
TEST_CASE("All asset types round-trip through a zstd pak")   { verifyAllTypes(Hpak::Codec::Zstd);  }

#ifdef HE_HAVE_OPENSSL
TEST_CASE("All asset types round-trip through an encrypted zstd pak")
{
    auto dir = std::filesystem::temp_directory_path() / "he_types_enc";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const TypeIds ids = authorAllTypes(dir);

    Hpak::PackSettings s; s.codec = Hpak::Codec::Zstd; s.encrypt = true;
    for (int i = 0; i < 32; ++i) s.key[i] = static_cast<uint8_t>(i * 3 + 1);

    HpakWriter packer;
    CHECK(packer.addDirectory(dir, s) == 10);
    auto pak = std::filesystem::temp_directory_path() / "he_types_enc.hpak";
    REQUIRE(packer.write(pak.string()));

    ContentManager cm;
    REQUIRE(cm.loadPak(pak.string(), s.key));   // pass the key
    CHECK(cm.getStaticMesh(ids.mesh) != nullptr);
    CHECK(cm.getMaterial(ids.mat)    != nullptr);
    CHECK(cm.getAudio(ids.audio)     != nullptr);
    CHECK(cm.isLoaded(ids.font));

    std::filesystem::remove(pak);
    std::filesystem::remove_all(dir);
}
#endif // HE_HAVE_OPENSSL

// ─── On-demand mounting + overlay ──────────────────────────────────────────────

TEST_CASE("mountPak loads assets on demand, not eagerly")
{
    const HE::UUID id{0x0D,0x0E};
    const auto blob = makeMaterialBlob(id, "ondemand", 0.3f, 0.6f, 0.9f);
    HpakWriter packer; packer.addEntry(id, blob, {Hpak::Codec::Zstd});
    auto pak = std::filesystem::temp_directory_path() / "he_mount.hpak";
    REQUIRE(packer.write(pak.string()));

    ContentManager cm;
    REQUIRE(cm.mountPak(pak.string()));
    CHECK(cm.mountedPakCount() == 1);

    // Mounted but NOT parsed yet.
    CHECK(!cm.isLoaded(id));
    CHECK(cm.getMaterial(id) == nullptr);

    // acquire triggers the on-demand load; afterwards it is resident.
    {
        auto ref = cm.acquireMaterial(id);
        REQUIRE(ref);
        CHECK(ref->baseColor[1] == doctest::Approx(0.6f));
    }
    CHECK(cm.isLoaded(id));
    CHECK(cm.getMaterial(id) != nullptr);

    // ensureResident is idempotent and returns false for unknown UUIDs.
    CHECK(cm.ensureResident(id));
    CHECK(!cm.ensureResident(HE::UUID{0xDEAD, 0xDEAD}));

    std::filesystem::remove(pak);
}

TEST_CASE("mountPak overlay: later mount shadows earlier by UUID, adds new UUIDs")
{
    const HE::UUID shared{0x100, 0x1};   // present in both paks
    const HE::UUID baseOnly{0x100, 0x2};
    const HE::UUID modOnly{0x100, 0x3};

    // Base pak: shared (red) + baseOnly.
    { HpakWriter p;
      p.addEntry(shared,   makeMaterialBlob(shared,   "base_shared", 1.f, 0.f, 0.f));
      p.addEntry(baseOnly, makeMaterialBlob(baseOnly, "base_only",   0.f, 0.f, 1.f));
      REQUIRE(p.write((std::filesystem::temp_directory_path() / "he_base.hpak").string())); }
    // Mod pak: shared (green, different content) + modOnly.
    { HpakWriter p;
      p.addEntry(shared,  makeMaterialBlob(shared,  "mod_shared", 0.f, 1.f, 0.f));
      p.addEntry(modOnly, makeMaterialBlob(modOnly, "mod_only",   1.f, 1.f, 0.f));
      REQUIRE(p.write((std::filesystem::temp_directory_path() / "he_mod.hpak").string())); }

    ContentManager cm;
    REQUIRE(cm.mountPak((std::filesystem::temp_directory_path() / "he_base.hpak").string()));
    REQUIRE(cm.mountPak((std::filesystem::temp_directory_path() / "he_mod.hpak").string())); // higher priority
    CHECK(cm.mountedPakCount() == 2);

    // shared resolves to the MOD version (green), not the base (red).
    { auto ref = cm.acquireMaterial(shared); REQUIRE(ref);
      CHECK(ref->baseColor[0] == doctest::Approx(0.f));
      CHECK(ref->baseColor[1] == doctest::Approx(1.f)); }
    // base-only still resolves; mod-only is an addition.
    CHECK(cm.ensureResident(baseOnly));
    CHECK(cm.ensureResident(modOnly));
    CHECK(cm.getMaterial(baseOnly) != nullptr);
    CHECK(cm.getMaterial(modOnly)  != nullptr);

    std::filesystem::remove(std::filesystem::temp_directory_path() / "he_base.hpak");
    std::filesystem::remove(std::filesystem::temp_directory_path() / "he_mod.hpak");
}

TEST_CASE("SceneSystems::collectAssetRefs gathers component asset UUIDs (the stream seed)")
{
    HorizonWorld world;
    const HE::UUID meshId{0x11,1}, matId{0x22,2}, audioId{0x33,3};

    auto e1 = world.createEntity("meshed");
    { MeshComponent mc; mc.meshAssetId = meshId; world.addComponent(e1, mc); }
    { MaterialComponent mc; mc.materialAssetId = matId; world.addComponent(e1, mc); }
    auto e2 = world.createEntity("sound");
    { AudioSourceComponent ac; ac.assetId = audioId; world.addComponent(e2, ac); }
    // An entity whose mesh ref is null must contribute nothing.
    auto e3 = world.createEntity("empty");
    { MeshComponent mc; world.addComponent(e3, mc); }

    const auto refs = SceneSystems::collectAssetRefs(world);
    auto has = [&](HE::UUID id){ return std::find(refs.begin(), refs.end(), id) != refs.end(); };
    CHECK(has(meshId));
    CHECK(has(matId));
    CHECK(has(audioId));
    CHECK(std::count(refs.begin(), refs.end(), HE::UUID{}) == 0);  // no null seeds
}

TEST_CASE("Pack-time UUID-ref baking: mesh->material and material->texture")
{
    auto dir = std::filesystem::temp_directory_path() / "he_refbake";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // Author a texture, a material referencing it by path, and a mesh referencing
    // the material by path — all as loose .hasset files.
    ContentManager cmSrc(dir.string());
    TextureAsset tex; tex.type = HE::AssetType::Texture; tex.name = "tex"; tex.path = "tex.hasset";
    tex.width = 2; tex.height = 2; tex.channels = 4; tex.data = std::vector<uint8_t>(16, 0x77);
    REQUIRE(cmSrc.saveAsset(tex));
    MaterialAsset mat; mat.type = HE::AssetType::Material; mat.name = "mat"; mat.path = "mat.hasset";
    mat.texturePaths = {"tex.hasset"};
    REQUIRE(cmSrc.saveAsset(mat));
    StaticMeshAsset mesh; mesh.type = HE::AssetType::StaticMesh; mesh.name = "mesh"; mesh.path = "mesh.hasset";
    mesh.materialPath = "mat.hasset"; mesh.vertices = {0,0,0, 1,0,0, 0,1,0}; mesh.indices = {0,1,2};
    REQUIRE(cmSrc.saveAsset(mesh));

    // Loose parse (no pack) → UUID-ref fields stay empty (paths only, for the editor).
    {
        std::ifstream f(dir / "mesh.hasset", std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ContentManager loose;
        REQUIRE(loose.loadAssetFromMemory(bytes) == mesh.id);
        const StaticMeshAsset* lm = loose.getStaticMesh(mesh.id);
        REQUIRE(lm != nullptr);
        CHECK(lm->materialId == HE::UUID{});   // no baking on loose assets
    }

    // Pack (bakes refs) + load → UUID-ref fields resolved to the target UUIDs.
    HpakWriter packer;
    CHECK(packer.addDirectory(dir, {Hpak::Codec::Zstd}) == 3);
    auto pak = std::filesystem::temp_directory_path() / "he_refbake.hpak";
    REQUIRE(packer.write(pak.string()));

    ContentManager cm;
    REQUIRE(cm.loadPak(pak.string()));
    const StaticMeshAsset* m = cm.getStaticMesh(mesh.id);
    REQUIRE(m != nullptr);
    CHECK(m->materialId == mat.id);            // mesh -> material baked
    const MaterialAsset* mt = cm.getMaterial(mat.id);
    REQUIRE(mt != nullptr);
    REQUIRE(mt->textureIds.size() == 1);
    CHECK(mt->textureIds[0] == tex.id);        // material -> texture baked

    std::filesystem::remove(pak);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Reference-graph streaming: seeding a mesh streams its closure only")
{
    auto dir = std::filesystem::temp_directory_path() / "he_closure";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    ContentManager cmSrc(dir.string());
    TextureAsset tex; tex.type = HE::AssetType::Texture; tex.name = "tex"; tex.path = "tex.hasset";
    tex.width = 2; tex.height = 2; tex.channels = 4; tex.data = std::vector<uint8_t>(16, 0x33);
    REQUIRE(cmSrc.saveAsset(tex));
    MaterialAsset mat; mat.type = HE::AssetType::Material; mat.name = "mat"; mat.path = "mat.hasset";
    mat.texturePaths = {"tex.hasset"};
    REQUIRE(cmSrc.saveAsset(mat));
    StaticMeshAsset mesh; mesh.type = HE::AssetType::StaticMesh; mesh.name = "mesh"; mesh.path = "mesh.hasset";
    mesh.materialPath = "mat.hasset"; mesh.vertices = {0,0,0, 1,0,0, 0,1,0}; mesh.indices = {0,1,2};
    REQUIRE(cmSrc.saveAsset(mesh));
    // An unreferenced material — must NOT be pulled by the closure.
    MaterialAsset orphan; orphan.type = HE::AssetType::Material; orphan.name = "orphan"; orphan.path = "orphan.hasset";
    REQUIRE(cmSrc.saveAsset(orphan));

    HpakWriter packer;
    CHECK(packer.addDirectory(dir, {Hpak::Codec::Zstd}) == 4);
    auto pak = std::filesystem::temp_directory_path() / "he_closure.hpak";
    REQUIRE(packer.write(pak.string()));

    ContentManager cm;
    REQUIRE(cm.mountPak(pak.string()));
    cm.loadAssetAsync(mesh.id);   // seed ONLY the mesh

    bool done = false;
    for (int i = 0; i < 500 && !done; ++i)
    {
        cm.pollAsyncResults();    // frontier expands as each asset registers
        done = cm.isLoaded(mesh.id) && cm.isLoaded(mat.id) && cm.isLoaded(tex.id);
        if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(done);                        // mesh -> material -> texture all streamed in
    // Drain a few more times to be sure nothing else sneaks in.
    for (int i = 0; i < 5; ++i) { cm.pollAsyncResults(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    CHECK(!cm.isLoaded(orphan.id));       // unreferenced asset never loaded

    std::filesystem::remove(pak);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Async streaming from a mounted pak (loadAssetAsync by UUID)")
{
    const HE::UUID a{0xA1,1}, b{0xA2,2}, c{0xA3,3};
    HpakWriter p;
    p.addEntry(a, makeMaterialBlob(a, "stream_a", 1.f,0.f,0.f), {Hpak::Codec::Zstd});
    p.addEntry(b, makeMaterialBlob(b, "stream_b", 0.f,1.f,0.f), {Hpak::Codec::LZ4});
    p.addEntry(c, makeMaterialBlob(c, "stream_c", 0.f,0.f,1.f));
    auto pak = std::filesystem::temp_directory_path() / "he_stream.hpak";
    REQUIRE(p.write(pak.string()));

    ContentManager cm;
    REQUIRE(cm.mountPak(pak.string()));
    const size_t jobs = cm.streamMountedAssets();
    CHECK(jobs == 3);
    CHECK(!cm.isLoaded(a));   // async → nothing registered until we pump the drain

    bool done = false;
    for (int i = 0; i < 500 && !done; ++i)
    {
        cm.pollAsyncResults();  // main-thread registration point
        done = cm.isLoaded(a) && cm.isLoaded(b) && cm.isLoaded(c);
        if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(done);
    CHECK(cm.getMaterial(a) != nullptr);
    CHECK(cm.getMaterial(b) != nullptr);
    CHECK(cm.getMaterial(c) != nullptr);
    CHECK(cm.streamMountedAssets() == 0);  // all resident → nothing to submit

    std::filesystem::remove(pak);
}

TEST_CASE("Binary scene round-trips through a mounted pak (readMountedEntry)")
{
    // Build a small world, serialize it to CBOR, pack it as a raw pak entry, then
    // mount + read it back and re-deserialize — the D8 binary-scene-in-pak path.
    HorizonWorld world;
    for (int i = 0; i < 3; ++i)
    {
        auto e = world.createEntity("Obj" + std::to_string(i));
        world.addComponent(e, TransformComponent{});
    }
    SceneSerializer ser;
    std::vector<uint8_t> sceneBytes;
    REQUIRE(ser.saveToMemory(world, sceneBytes));
    REQUIRE(!sceneBytes.empty());

    const HE::UUID sceneUuid{0x5CE, 0x11};
    HpakWriter p;
    p.addEntry(sceneUuid, sceneBytes, {Hpak::Codec::Zstd});
    auto pak = std::filesystem::temp_directory_path() / "he_scene.hpak";
    REQUIRE(p.write(pak.string()));

    ContentManager cm;
    REQUIRE(cm.mountPak(pak.string()));

    // Raw read returns the exact scene bytes (survived compression + the pak).
    const auto got = cm.readMountedEntry(sceneUuid);
    CHECK(got == sceneBytes);

    // And it is NOT registered as an asset (it's scene data, not a .hasset).
    CHECK(!cm.isLoaded(sceneUuid));

    // Re-deserialize into a fresh world; the entities came through.
    HorizonWorld world2;
    REQUIRE(ser.loadFromMemory(world2, got));
    size_t transforms = 0;
    for (auto e : world2.registry().view<TransformComponent>()) { (void)e; ++transforms; }
    CHECK(transforms >= 3);

    std::filesystem::remove(pak);
}

TEST_CASE("pollAsyncResults honors the per-frame registration budget")
{
    HpakWriter p;
    std::vector<HE::UUID> ids;
    for (uint64_t i = 0; i < 6; ++i)
    {
        HE::UUID id{0xB0, i + 1};
        ids.push_back(id);
        p.addEntry(id, makeMaterialBlob(id, "budget" + std::to_string(i)), {Hpak::Codec::Zstd});
    }
    auto pak = std::filesystem::temp_directory_path() / "he_budget.hpak";
    REQUIRE(p.write(pak.string()));

    ContentManager cm;
    REQUIRE(cm.mountPak(pak.string()));
    CHECK(cm.streamMountedAssets() == 6);

    // Budget 0 → registers nothing, even if results are already waiting.
    CHECK(cm.pollAsyncResults(0).empty());

    // Drain at most 2 per call: every call respects the cap, all load eventually.
    size_t resident = 0;
    for (int i = 0; i < 500 && resident < 6; ++i)
    {
        const auto got = cm.pollAsyncResults(2);
        CHECK(got.size() <= 2);
        resident = 0;
        for (const auto& id : ids) if (cm.isLoaded(id)) ++resident;
        if (resident < 6) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(resident == 6);

    std::filesystem::remove(pak);
}

#ifdef HE_HAVE_OPENSSL
TEST_CASE("mountPak with an encrypted pak loads on demand with the key")
{
    const HE::UUID id{0x0E,0x0C};
    const auto blob = makeMaterialBlob(id, "enc_ondemand", 0.2f, 0.4f, 0.8f);
    Hpak::PackSettings s; s.codec = Hpak::Codec::Zstd; s.encrypt = true;
    for (int i = 0; i < 32; ++i) s.key[i] = static_cast<uint8_t>(i + 11);
    HpakWriter packer; packer.addEntry(id, blob, s);
    auto pak = std::filesystem::temp_directory_path() / "he_mount_enc.hpak";
    REQUIRE(packer.write(pak.string()));

    ContentManager cm;
    REQUIRE(cm.mountPak(pak.string(), s.key));
    CHECK(!cm.isLoaded(id));
    auto ref = cm.acquireMaterial(id);
    REQUIRE(ref);
    CHECK(ref->baseColor[2] == doctest::Approx(0.8f));

    std::filesystem::remove(pak);
}
#endif // HE_HAVE_OPENSSL
