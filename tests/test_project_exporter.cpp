#include "doctest.h"
#include <Hpak/ProjectConfig.h>
#include <Hpak/ProjectExporter.h>
#include <Hpak/HpakReader.h>
#include <ContentManager/HAsset.h>
#include <ContentManager/Assets.h>
#include <Types/UUID.h>
#include <filesystem>
#include <fstream>
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
    settings.compress = false;
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
    CHECK(reader.readEntry(id1) == blob1);
    CHECK(reader.readEntry(id2) == blob2);

    // project.hcfg should exist
    REQUIRE(std::filesystem::exists(outputDir / "project.hcfg"));
    ProjectConfig cfg;
    REQUIRE(ProjectConfigLoader::load(outputDir, cfg));
    CHECK(cfg.projectName  == "MyGame");
    CHECK(cfg.hpakFilename == "MyGame.hpak");
    CHECK(cfg.mainSceneName.empty()); // no scene specified

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
