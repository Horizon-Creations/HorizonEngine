#include "doctest.h"
#include <Hpak/ProjectConfig.h>
#include <Hpak/ProjectExporter.h>
#include <Hpak/HpakReader.h>
#include <ContentManager/HAsset.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Types/UUID.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_set>
#include <cstring>

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::vector<uint8_t> makeMinimalMaterialBlob(const HE::UUID& id,
                                                     const std::string& name)
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
    float r=1.f, g=0.f, b=0.f, met=0.f, rough=0.5f, op=1.f;
    HAsset::Writer::appendPOD(mtrl, r); HAsset::Writer::appendPOD(mtrl, g);
    HAsset::Writer::appendPOD(mtrl, b); HAsset::Writer::appendPOD(mtrl, met);
    HAsset::Writer::appendPOD(mtrl, rough); HAsset::Writer::appendPOD(mtrl, op);

    HAsset::Writer w;
    w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
    w.addChunk(HAsset::CHUNK_MTRL, mtrl.data(), mtrl.size());
    return w.toBytes(typeVal);
}

// ─── ProjectConfigLoader ──────────────────────────────────────────────────────

TEST_CASE("ProjectConfigLoader save/load round-trip")
{
    auto tmpDir = std::filesystem::temp_directory_path() / "he_test_pcfg";
    std::filesystem::create_directories(tmpDir);

    ProjectConfig cfg;
    cfg.projectName   = "TestProject";
    cfg.hpakFilename  = "TestProject.hpak";
    cfg.mainSceneName = "Main.hescene";
    for (int i = 0; i < 16; ++i) cfg.projectUuidBytes[i] = static_cast<uint8_t>(i + 1);
    cfg.enableModSupport = true;

    REQUIRE(ProjectConfigLoader::save(tmpDir, cfg));

    ProjectConfig loaded;
    REQUIRE(ProjectConfigLoader::load(tmpDir, loaded));
    CHECK(loaded.projectName   == cfg.projectName);
    CHECK(loaded.hpakFilename  == cfg.hpakFilename);
    CHECK(loaded.mainSceneName == cfg.mainSceneName);
    CHECK(std::memcmp(loaded.projectUuidBytes, cfg.projectUuidBytes, 16) == 0);
    CHECK(loaded.enableModSupport == cfg.enableModSupport);

    std::filesystem::remove_all(tmpDir);
}

TEST_CASE("ProjectConfigLoader returns false for missing file")
{
    ProjectConfig cfg;
    CHECK(!ProjectConfigLoader::load("/nonexistent/dir/that/does/not/exist", cfg));
}

TEST_CASE("ProjectConfigLoader returns false for corrupt data")
{
    auto tmpDir = std::filesystem::temp_directory_path() / "he_test_pcfg_bad";
    std::filesystem::create_directories(tmpDir);

    { std::ofstream f(tmpDir / "project.hcfg", std::ios::binary); f << "BADDATA"; }

    ProjectConfig cfg;
    CHECK(!ProjectConfigLoader::load(tmpDir, cfg));

    std::filesystem::remove_all(tmpDir);
}

TEST_CASE("ProjectConfigLoader empty strings round-trip")
{
    auto tmpDir = std::filesystem::temp_directory_path() / "he_test_pcfg_empty";
    std::filesystem::create_directories(tmpDir);

    ProjectConfig cfg;
    cfg.projectName   = "";
    cfg.hpakFilename  = "";
    cfg.mainSceneName = "";
    cfg.enableModSupport = false;

    REQUIRE(ProjectConfigLoader::save(tmpDir, cfg));

    ProjectConfig loaded;
    REQUIRE(ProjectConfigLoader::load(tmpDir, loaded));
    CHECK(loaded.projectName.empty());
    CHECK(loaded.hpakFilename.empty());
    CHECK(loaded.mainSceneName.empty());
    CHECK(!loaded.enableModSupport);

    std::filesystem::remove_all(tmpDir);
}

// ─── ProjectExporter ──────────────────────────────────────────────────────────

TEST_CASE("ProjectExporter packs .hasset files from content dir")
{
    auto contentDir = std::filesystem::temp_directory_path() / "he_test_export_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_test_export_out";
    std::filesystem::create_directories(contentDir);

    const HE::UUID id1{0xAABB,0xCCDD};
    const HE::UUID id2{0x1122,0x3344};
    const auto blob1 = makeMinimalMaterialBlob(id1, "mat_a");
    const auto blob2 = makeMinimalMaterialBlob(id2, "mat_b");

    { std::ofstream f(contentDir / "mat_a.hasset", std::ios::binary);
      f.write(reinterpret_cast<const char*>(blob1.data()), blob1.size()); }
    { std::ofstream f(contentDir / "mat_b.hasset", std::ios::binary);
      f.write(reinterpret_cast<const char*>(blob2.data()), blob2.size()); }
    // Non-.hasset file should be ignored
    { std::ofstream f(contentDir / "readme.txt"); f << "not an asset"; }

    ExportSettings settings;
    settings.compress         = false;
    settings.enableModSupport = true;
    const auto result = ProjectExporter::exportProject(
        contentDir, "MyGame", "", outputDir, settings);

    REQUIRE(result.success);
    CHECK(result.assetsPacked == 2);

    // .hpak should exist and contain both assets
    const auto pakPath = outputDir / "MyGame.hpak";
    REQUIRE(std::filesystem::exists(pakPath));

    HpakReader reader;
    REQUIRE(reader.open(pakPath.string()));
    CHECK(reader.hasEntry(id1));
    CHECK(reader.hasEntry(id2));
    // The packer bakes additive UUID-ref chunks (MTLU here), so the stored entry
    // is no longer byte-identical to the source blob — verify it parses instead.
    { ContentManager cm; REQUIRE(cm.loadPak(pakPath.string()));
      CHECK(cm.getMaterial(id1) != nullptr);
      CHECK(cm.getMaterial(id2) != nullptr); }

    // project.hcfg should exist
    REQUIRE(std::filesystem::exists(outputDir / "project.hcfg"));
    ProjectConfig cfg;
    REQUIRE(ProjectConfigLoader::load(outputDir, cfg));
    CHECK(cfg.projectName  == "MyGame");
    CHECK(cfg.hpakFilename == "MyGame.hpak");
    CHECK(cfg.mainSceneName.empty()); // no scene specified
    CHECK(cfg.enableModSupport);      // export flag reaches the runtime config

    std::filesystem::remove_all(contentDir);
    std::filesystem::remove_all(outputDir);
}

TEST_CASE("ProjectExporter with empty content dir produces empty pak")
{
    auto contentDir = std::filesystem::temp_directory_path() / "he_test_export_empty_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_test_export_empty_out";
    std::filesystem::create_directories(contentDir);

    ExportSettings settings;
    settings.compress = false;
    const auto result = ProjectExporter::exportProject(
        contentDir, "EmptyGame", "", outputDir, settings);

    REQUIRE(result.success);
    CHECK(result.assetsPacked == 0);
    REQUIRE(std::filesystem::exists(outputDir / "EmptyGame.hpak"));
    REQUIRE(std::filesystem::exists(outputDir / "project.hcfg"));

    std::filesystem::remove_all(contentDir);
    std::filesystem::remove_all(outputDir);
}

TEST_CASE("ProjectExporter copies startup scene file")
{
    auto contentDir = std::filesystem::temp_directory_path() / "he_test_export_scene_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_test_export_scene_out";
    std::filesystem::create_directories(contentDir);

    // Write a fake .hescene
    { std::ofstream f(contentDir / "Main.hescene"); f << "{\"entities\":[]}"; }

    ExportSettings settings;
    settings.compress = false;
    const auto result = ProjectExporter::exportProject(
        contentDir, "SceneGame", "Main.hescene", outputDir, settings);

    REQUIRE(result.success);

    // Scene should be copied to output dir
    CHECK(std::filesystem::exists(outputDir / "Main.hescene"));

    // project.hcfg should reference it
    ProjectConfig cfg;
    REQUIRE(ProjectConfigLoader::load(outputDir, cfg));
    CHECK(cfg.mainSceneName == "Main.hescene");

    std::filesystem::remove_all(contentDir);
    std::filesystem::remove_all(outputDir);
}

TEST_CASE("ProjectExporter copies game runtime binaries when gameRuntimeDir is set")
{
    auto runtimeDir = std::filesystem::temp_directory_path() / "he_test_export_runtime";
    auto contentDir = std::filesystem::temp_directory_path() / "he_test_export_bin_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_test_export_bin_out";
    std::filesystem::create_directories(runtimeDir);
    std::filesystem::create_directories(contentDir);

    // Simulate game runtime dir: dummy executable + two dylibs
    { std::ofstream f(runtimeDir / "HorizonGame"); f << "ELF_FAKE"; }
    { std::ofstream f(runtimeDir / "libHorizonCore.dylib"); f << "DYLIB_FAKE_A"; }
    { std::ofstream f(runtimeDir / "libSDL3.0.dylib"); f << "DYLIB_FAKE_B"; }

    ExportSettings settings;
    settings.compress       = false;
    settings.gameRuntimeDir = runtimeDir;
    const auto result = ProjectExporter::exportProject(
        contentDir, "MyGame", "", outputDir, settings);

    REQUIRE(result.success);
    CHECK(result.binaryFilesCopied == 3);
    CHECK(std::filesystem::exists(outputDir / "HorizonGame"));
    CHECK(std::filesystem::exists(outputDir / "libHorizonCore.dylib"));
    CHECK(std::filesystem::exists(outputDir / "libSDL3.0.dylib"));

    std::filesystem::remove_all(runtimeDir);
    std::filesystem::remove_all(contentDir);
    std::filesystem::remove_all(outputDir);
}

TEST_CASE("ProjectExporter skips binary copy when gameRuntimeDir is empty")
{
    auto contentDir = std::filesystem::temp_directory_path() / "he_test_export_nobin_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_test_export_nobin_out";
    std::filesystem::create_directories(contentDir);

    ExportSettings settings;
    settings.compress = false;
    // gameRuntimeDir left empty (default)
    const auto result = ProjectExporter::exportProject(
        contentDir, "Game", "", outputDir, settings);

    REQUIRE(result.success);
    CHECK(result.binaryFilesCopied == 0);

    std::filesystem::remove_all(contentDir);
    std::filesystem::remove_all(outputDir);
}

// Contract CHANGED with the runnable-exports work: a runtime dir that is named
// but missing used to be silently skipped (data-only export reported OK — the
// exact defect users hit); it is now a hard error. Empty gameRuntimeDir remains
// the supported "data only" mode.
TEST_CASE("ProjectExporter fails when a named gameRuntimeDir does not exist")
{
    auto contentDir = std::filesystem::temp_directory_path() / "he_test_export_baddrt_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_test_export_baddrt_out";
    std::filesystem::create_directories(contentDir);

    ExportSettings settings;
    settings.compress       = false;
    settings.gameRuntimeDir = "/nonexistent/path/that/does/not/exist/xyz123";
    const auto result = ProjectExporter::exportProject(
        contentDir, "Game", "", outputDir, settings);

    CHECK_FALSE(result.success);
    CHECK(result.errorMessage.find("not found") != std::string::npos);
    CHECK(result.binaryFilesCopied == 0);

    std::filesystem::remove_all(contentDir);
    std::filesystem::remove_all(outputDir);
}

TEST_CASE("ProjectExporter does not copy subdirectories from gameRuntimeDir")
{
    auto runtimeDir = std::filesystem::temp_directory_path() / "he_test_export_subdir_runtime";
    auto contentDir = std::filesystem::temp_directory_path() / "he_test_export_subdir_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_test_export_subdir_out";
    std::filesystem::create_directories(runtimeDir / "subdir");
    std::filesystem::create_directories(contentDir);
    { std::ofstream f(runtimeDir / "HorizonGame"); f << "ELF"; }
    { std::ofstream f(runtimeDir / "subdir" / "nested.txt"); f << "nested"; }

    ExportSettings settings;
    settings.compress       = false;
    settings.gameRuntimeDir = runtimeDir;
    const auto result = ProjectExporter::exportProject(
        contentDir, "Game", "", outputDir, settings);

    REQUIRE(result.success);
    CHECK(result.binaryFilesCopied == 1);  // only HorizonGame, not the subdir
    CHECK(std::filesystem::exists(outputDir / "HorizonGame"));
    CHECK(!std::filesystem::exists(outputDir / "subdir")); // subdirs not copied

    std::filesystem::remove_all(runtimeDir);
    std::filesystem::remove_all(contentDir);
    std::filesystem::remove_all(outputDir);
}

TEST_CASE("ProjectExporter returns error for invalid output dir (file in the way)")
{
    auto contentDir = std::filesystem::temp_directory_path() / "he_test_export_err_content";
    std::filesystem::create_directories(contentDir);

    // Create a file where output dir should go
    auto outputPath = std::filesystem::temp_directory_path() / "he_test_export_err_file";
    { std::ofstream f(outputPath); f << "I am a file, not a dir"; }

    ExportSettings settings;
    settings.compress = false;
    // This may or may not fail depending on OS behaviour (create_directories over file).
    // On most systems, writing into "a file treated as dir" will fail at hpak write stage.
    const auto result = ProjectExporter::exportProject(
        contentDir, "Game", "", outputPath, settings);
    // We don't assert success/failure here — just that it doesn't crash.
    (void)result;

    std::filesystem::remove_all(contentDir);
    std::filesystem::remove(outputPath);
}

// Mirrors the GameApplication runtime sequence: export a project, then read the
// hcfg, mount the pak with its key, stream the assets in on background workers,
// and drain via pollAsyncResults until they are resident — exactly what the game
// runtime now does instead of an eager blocking load.
TEST_CASE("ProjectExporter output mounts + streams like the game runtime")
{
    auto contentDir = std::filesystem::temp_directory_path() / "he_export_stream_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_export_stream_out";
    std::filesystem::remove_all(contentDir);
    std::filesystem::remove_all(outputDir);
    std::filesystem::create_directories(contentDir);

    const HE::UUID id1{0x5711,0x01}, id2{0x5711,0x02};
    for (auto [id, name] : {std::pair{id1, "s1"}, std::pair{id2, "s2"}})
    {
        const auto blob = makeMinimalMaterialBlob(id, name);
        std::ofstream f(contentDir / (std::string(name) + ".hasset"), std::ios::binary);
        f.write(reinterpret_cast<const char*>(blob.data()), blob.size());
    }

    ExportSettings settings;
    settings.compress = true;
    const auto result = ProjectExporter::exportProject(contentDir, "StreamGame", "", outputDir, settings);
    REQUIRE(result.success);
    CHECK(result.assetsPacked == 2);

    // ── The exact game-runtime sequence ──
    ProjectConfig cfg;
    REQUIRE(ProjectConfigLoader::load(outputDir, cfg));
    const uint8_t* key = cfg.encrypted ? cfg.encKey : nullptr;

    ContentManager cm;
    REQUIRE(cm.mountPak((outputDir / cfg.hpakFilename).string(), key));
    CHECK(!cm.isLoaded(id1));                 // mounted, not yet parsed
    CHECK(cm.streamMountedAssets() == 2);     // both stream in the background

    bool done = false;
    for (int i = 0; i < 500 && !done; ++i)
    {
        cm.pollAsyncResults();
        done = cm.isLoaded(id1) && cm.isLoaded(id2);
        if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(done);
    CHECK(cm.getMaterial(id1) != nullptr);
    CHECK(cm.getMaterial(id2) != nullptr);

    std::filesystem::remove_all(contentDir);
    std::filesystem::remove_all(outputDir);
}

TEST_CASE("ProjectExporter packs a binary startup scene into the pak")
{
    auto contentDir = std::filesystem::temp_directory_path() / "he_scene_pak_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_scene_pak_out";
    std::filesystem::remove_all(contentDir);
    std::filesystem::remove_all(outputDir);
    std::filesystem::create_directories(contentDir);

    // One real asset so the pak isn't scene-only.
    const HE::UUID matId{0xA55E, 0x7};
    { const auto blob = makeMinimalMaterialBlob(matId, "with_scene");
      std::ofstream f(contentDir / "with_scene.hasset", std::ios::binary);
      f.write(reinterpret_cast<const char*>(blob.data()), blob.size()); }

    // Stand-in for SceneSerializer::saveToMemory output (arbitrary binary blob).
    std::vector<uint8_t> sceneBinary(256);
    for (size_t i = 0; i < sceneBinary.size(); ++i) sceneBinary[i] = static_cast<uint8_t>(i * 7 + 3);

    ExportSettings settings; settings.compress = true;
    const auto result = ProjectExporter::exportProject(
        contentDir, "SceneGame", "Main.hescene", outputDir, settings, sceneBinary);
    REQUIRE(result.success);
    CHECK(result.assetsPacked == 1);   // the scene is packed separately, not counted as an asset

    // hcfg records the packed scene; the loose-scene fallback is skipped.
    ProjectConfig cfg;
    REQUIRE(ProjectConfigLoader::load(outputDir, cfg));
    CHECK(cfg.hasPackedScene);
    CHECK(cfg.mainSceneName.empty());
    uint8_t zero[16] = {};
    CHECK(std::memcmp(cfg.startupSceneUuid, zero, 16) != 0);

    HE::UUID sceneUuid{};
    std::memcpy(&sceneUuid.hi, cfg.startupSceneUuid,     8);
    std::memcpy(&sceneUuid.lo, cfg.startupSceneUuid + 8, 8);

    ContentManager cm;
    REQUIRE(cm.mountPak((outputDir / cfg.hpakFilename).string(),
                        cfg.encrypted ? cfg.encKey : nullptr));
    // Raw scene bytes round-trip through pack + compression.
    CHECK(cm.readMountedEntry(sceneUuid) == sceneBinary);
    // Streaming skips the scene entry (only the material asset streams).
    std::unordered_set<HE::UUID> exclude{sceneUuid};
    CHECK(cm.streamMountedAssets(exclude) == 1);

    std::filesystem::remove_all(contentDir);
    std::filesystem::remove_all(outputDir);
}

#ifdef HE_HAVE_OPENSSL
// End-to-end: exporting with encryption generates a random key, ships it in
// project.hcfg, and the runtime path (loadPak with that key) decrypts correctly.
// This is the fix for the bug where the game never passed a key to loadPak.
TEST_CASE("ProjectExporter encrypts and the hcfg key decrypts the pak")
{
    auto contentDir = std::filesystem::temp_directory_path() / "he_test_export_enc_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_test_export_enc_out";
    std::filesystem::remove_all(contentDir);
    std::filesystem::remove_all(outputDir);
    std::filesystem::create_directories(contentDir);

    const HE::UUID id{0xFEED,0xBEEF};
    const auto blob = makeMinimalMaterialBlob(id, "secret_mat");
    { std::ofstream f(contentDir / "secret_mat.hasset", std::ios::binary);
      f.write(reinterpret_cast<const char*>(blob.data()), blob.size()); }

    ExportSettings settings;
    settings.compress = true;
    settings.encrypt  = true;
    const auto result = ProjectExporter::exportProject(
        contentDir, "SecretGame", "", outputDir, settings);
    REQUIRE(result.success);
    CHECK(result.assetsPacked == 1);

    // hcfg records encryption + a non-zero random key.
    ProjectConfig cfg;
    REQUIRE(ProjectConfigLoader::load(outputDir, cfg));
    CHECK(cfg.encrypted);
    uint8_t zero[32] = {};
    CHECK(std::memcmp(cfg.encKey, zero, 32) != 0);

    const auto pakPath = (outputDir / "SecretGame.hpak").string();

    // Without the key → asset does not load. With the hcfg key → it does.
    { ContentManager cm; REQUIRE(cm.loadPak(pakPath)); CHECK(cm.getMaterial(id) == nullptr); }
    { ContentManager cm; REQUIRE(cm.loadPak(pakPath, cfg.encKey)); CHECK(cm.getMaterial(id) != nullptr); }

    std::filesystem::remove_all(contentDir);
    std::filesystem::remove_all(outputDir);
}
#endif // HE_HAVE_OPENSSL
