#include "doctest.h"
#include <Hpak/HpakFormat.h>
#include <Hpak/HpakWriter.h>
#include <Hpak/HpakReader.h>
#include <Hpak/ProjectExporter.h>
#include <ContentManager/HAsset.h>
#include <Types/Enums.h>
#include <Types/UUID.h>
#include "ProjectManager.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ─── Glob matcher ─────────────────────────────────────────────────────────────

TEST_CASE("globMatch: literal, ?, and * (spanning '/')")
{
    CHECK(Hpak::globMatch("a.hasset", "a.hasset"));
    CHECK_FALSE(Hpak::globMatch("a.hasset", "b.hasset"));

    CHECK(Hpak::globMatch("?.hasset", "a.hasset"));
    CHECK_FALSE(Hpak::globMatch("?.hasset", "ab.hasset"));

    CHECK(Hpak::globMatch("*", "anything/at/all.hasset"));
    CHECK(Hpak::globMatch("Debug/*", "Debug/thing.hasset"));
    CHECK(Hpak::globMatch("Debug/*", "Debug/sub/deep.hasset")); // * spans '/'
    CHECK_FALSE(Hpak::globMatch("Debug/*", "Release/thing.hasset"));

    CHECK(Hpak::globMatch("*_test.hasset", "foo_test.hasset"));
    CHECK(Hpak::globMatch("*_test.hasset", "sub/dir/foo_test.hasset"));
    CHECK_FALSE(Hpak::globMatch("*_test.hasset", "foo_test.hasset.bak"));

    CHECK(Hpak::globMatch("a*b*c", "aXXbYYc"));
    CHECK_FALSE(Hpak::globMatch("a*b*c", "aXXbYY"));
    CHECK(Hpak::globMatch("abc***", "abc"));   // trailing stars match empty
    CHECK_FALSE(Hpak::globMatch("", "x"));
    CHECK(Hpak::globMatch("", ""));
}

// ─── addDirectory exclude patterns + progress ─────────────────────────────────

// Minimal valid .hasset (META only) for a given UUID/relative path.
static std::vector<uint8_t> tinyHasset(HE::UUID id, const std::string& relPath)
{
    std::vector<uint8_t> meta;
    HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(HE::AssetType::Texture));
    HAsset::Writer::appendPOD(meta, id.hi);
    HAsset::Writer::appendPOD(meta, id.lo);
    HAsset::Writer::appendString(meta, fs::path(relPath).stem().string());
    HAsset::Writer::appendString(meta, relPath);
    HAsset::Writer w;
    w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
    return w.toBytes(static_cast<uint16_t>(HE::AssetType::Texture));
}

static void writeBlob(const fs::path& p, const std::vector<uint8_t>& bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

TEST_CASE("addDirectory: excludePatterns skip matching assets, progress reports the rest")
{
    const auto dir = fs::temp_directory_path() / "he_excl_test";
    fs::remove_all(dir);
    writeBlob(dir / "keep.hasset",            tinyHasset({0x1, 0x1}, "keep.hasset"));
    writeBlob(dir / "skip_test.hasset",       tinyHasset({0x2, 0x2}, "skip_test.hasset"));
    writeBlob(dir / "Debug" / "tool.hasset",  tinyHasset({0x3, 0x3}, "Debug/tool.hasset"));
    writeBlob(dir / "Deep" / "keep2.hasset",  tinyHasset({0x4, 0x4}, "Deep/keep2.hasset"));

    Hpak::PackSettings s;
    s.excludePatterns = { "*_test.hasset", "Debug/*" };

    int calls = 0, lastDone = -1, lastTotal = -1;
    std::vector<std::string> seen;
    HpakWriter packer;
    const int added = packer.addDirectory(dir, s,
        [&](int done, int total, const std::string& cur)
        { ++calls; lastDone = done; lastTotal = total; if (!cur.empty()) seen.push_back(cur); });

    CHECK(added == 2);                       // keep + Deep/keep2; the two excluded are gone
    CHECK(lastDone == 2);                    // final call reports (total, total, "")
    CHECK(lastTotal == 2);
    CHECK(calls == 3);                       // 2 per-file + 1 final
    REQUIRE(seen.size() == 2);
    for (const auto& f : seen)
    {
        CHECK(f.find("_test") == std::string::npos);
        CHECK(f.rfind("Debug/", 0) != 0);
    }
    fs::remove_all(dir);
}

TEST_CASE("ProjectExporter: excludePatterns reach the pak, progress fires")
{
    const auto dir = fs::temp_directory_path() / "he_excl_export";
    const auto out = fs::temp_directory_path() / "he_excl_export_out";
    fs::remove_all(dir); fs::remove_all(out);
    writeBlob(dir / "ship.hasset",      tinyHasset({0xA, 0xA}, "ship.hasset"));
    writeBlob(dir / "wip_test.hasset",  tinyHasset({0xB, 0xB}, "wip_test.hasset"));

    ExportSettings settings;
    settings.compress = false;
    settings.excludePatterns = { "*_test.hasset" };
    int progressCalls = 0;
    settings.progress = [&](int, int, const std::string&) { ++progressCalls; };

    const auto res = ProjectExporter::exportProject(dir, "Excl", "", out, settings);
    REQUIRE(res.success);
    CHECK(res.assetsPacked == 1);
    CHECK(progressCalls >= 2); // one per file + final

    HpakReader reader;
    REQUIRE(reader.open((out / "Excl.hpak").string()));
    CHECK(reader.hasEntry(HE::UUID{0xA, 0xA}));
    CHECK_FALSE(reader.hasEntry(HE::UUID{0xB, 0xB}));
    fs::remove_all(dir); fs::remove_all(out);
}

// ─── ExportProfile persistence in .heproj ─────────────────────────────────────

TEST_CASE("ProjectManager: new projects are seeded with Development + Shipping profiles")
{
    const auto dir = fs::temp_directory_path() / "he_prof_new";
    fs::remove_all(dir);

    ProjectManager pm;
    REQUIRE(pm.createNewProject(dir.string(), "ProfProj", ProjectPreset::Empty));
    auto& proj = pm.currentProject();
    REQUIRE(proj.exportProfiles.size() == 2);
    CHECK(proj.exportProfiles[0].name == "Development");
    CHECK_FALSE(proj.exportProfiles[0].compress);
    CHECK(proj.exportProfiles[0].enableModSupport);
    CHECK(proj.exportProfiles[1].name == "Shipping");
    CHECK(proj.exportProfiles[1].compress);
    CHECK(proj.exportProfiles[1].encrypt);
    CHECK(proj.activeExportProfile == "Development");
    fs::remove_all(dir);
}

TEST_CASE("ProjectManager: manifest without profiles loads seeded defaults")
{
    const auto dir = fs::temp_directory_path() / "he_prof_legacy";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // Legacy .heproj as written before profiles existed.
    {
        std::ofstream out(dir / "Old.heproj");
        out << R"({"name":"Old","version":"1.0","preset":0})";
    }

    ProjectManager pm;
    REQUIRE(pm.loadProject((dir / "Old.heproj").string()));
    REQUIRE(pm.currentProject().exportProfiles.size() == 2);
    CHECK(pm.currentProject().activeExportProfile == "Development");
    fs::remove_all(dir);
}

TEST_CASE("ProjectManager: profiles round-trip and unknown manifest keys survive save")
{
    const auto dir = fs::temp_directory_path() / "he_prof_rt";
    fs::remove_all(dir);

    ProjectManager pm;
    REQUIRE(pm.createNewProject(dir.string(), "RT", ProjectPreset::Empty));
    const std::string heproj = pm.currentProject().path;

    // Inject a foreign key the way a future engine version might.
    {
        std::ifstream in(heproj);
        nlohmann::json j = nlohmann::json::parse(in);
        in.close();
        j["futureField"] = 42;
        std::ofstream out(heproj);
        out << j.dump(4);
    }

    // Customize a profile + add a new one, then save.
    REQUIRE(pm.loadProject(heproj)); // re-read (picks up futureField's file state)
    auto& proj = pm.currentProject();
    proj.exportProfiles[1].excludePatterns = { "Debug/*", "*_wip.hasset" };
    proj.exportProfiles[1].startupScene    = "Content/Menu.hescene";
    proj.exportProfiles[1].outputDir       = "/tmp/ship_out";
    ExportProfile extra;
    extra.name = "DemoDisk";
    extra.compress = true;
    proj.exportProfiles.push_back(extra);
    proj.activeExportProfile = "DemoDisk";
    REQUIRE(pm.saveProject(heproj));

    // Reload into a fresh manager and verify everything.
    ProjectManager pm2;
    REQUIRE(pm2.loadProject(heproj));
    const auto& p2 = pm2.currentProject();
    REQUIRE(p2.exportProfiles.size() == 3);
    CHECK(p2.activeExportProfile == "DemoDisk");
    CHECK(p2.exportProfiles[1].excludePatterns
          == std::vector<std::string>{ "Debug/*", "*_wip.hasset" });
    CHECK(p2.exportProfiles[1].startupScene == "Content/Menu.hescene");
    CHECK(p2.exportProfiles[1].outputDir == "/tmp/ship_out");
    CHECK(p2.exportProfiles[2].name == "DemoDisk");
    // startupScene survives the read-modify-write save (old saveProject lost it).
    CHECK_FALSE(p2.startupScene.empty());

    // The foreign key must still be in the file.
    {
        std::ifstream in(heproj);
        nlohmann::json j = nlohmann::json::parse(in);
        CHECK(j.value("futureField", 0) == 42);
        CHECK(j.contains("preset"));
    }
    fs::remove_all(dir);
}

TEST_CASE("ProjectManager: unknown active profile falls back to the first")
{
    const auto dir = fs::temp_directory_path() / "he_prof_fallback";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "F.heproj");
        out << R"({"name":"F","exportProfiles":[{"name":"Only"}],"activeExportProfile":"Ghost"})";
    }
    ProjectManager pm;
    REQUIRE(pm.loadProject((dir / "F.heproj").string()));
    CHECK(pm.currentProject().activeExportProfile == "Only");
    fs::remove_all(dir);
}
