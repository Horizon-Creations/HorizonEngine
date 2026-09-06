#include "doctest.h"
#include "TestFsUtil.h"
#include <Hpak/HpakFormat.h>
#include <Hpak/HpakWriter.h>
#include <Hpak/HpakReader.h>
#include <Hpak/ProjectExporter.h>
#include <Hpak/ProjectConfig.h>
#include <Application/SplashScreen.h>
#include <ContentManager/HAsset.h>
#include <Types/Enums.h>
#include <Types/UUID.h>
#include "ProjectManager.h"
#include <UIWidget/UIWidgetTree.h>
#include <HorizonCode/HorizonCode.h>
#include <fstream>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/EnvironmentComponent.h>
#include <HorizonScene/Components/WeatherComponent.h>
#include <cstring>

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
    he_test::removeAllQuiet(dir);
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
    he_test::removeAllQuiet(dir);
}

TEST_CASE("ProjectExporter: excludePatterns reach the pak, progress fires")
{
    const auto dir = fs::temp_directory_path() / "he_excl_export";
    const auto out = fs::temp_directory_path() / "he_excl_export_out";
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(out);
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
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(out);
}

// ─── ExportProfile persistence in .heproj ─────────────────────────────────────

TEST_CASE("ProjectManager: new projects are seeded with Development + Shipping profiles")
{
    const auto dir = fs::temp_directory_path() / "he_prof_new";
    he_test::removeAllQuiet(dir);

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
    he_test::removeAllQuiet(dir);
}

TEST_CASE("ProjectManager: manifest without profiles loads seeded defaults")
{
    const auto dir = fs::temp_directory_path() / "he_prof_legacy";
    he_test::removeAllQuiet(dir);
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
    he_test::removeAllQuiet(dir);
}

TEST_CASE("ProjectManager: profiles round-trip and unknown manifest keys survive save")
{
    const auto dir = fs::temp_directory_path() / "he_prof_rt";
    he_test::removeAllQuiet(dir);

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
    proj.exportProfiles[1].incremental     = false;
    proj.exportProfiles[1].targetPlatform  = "Windows";
    proj.exportProfiles[1].appBundle       = true;
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
    CHECK_FALSE(p2.exportProfiles[1].incremental);
    CHECK(p2.exportProfiles[1].targetPlatform == "Windows");
    CHECK(p2.exportProfiles[1].appBundle);
    CHECK_FALSE(p2.exportProfiles[2].appBundle); // default
    CHECK(p2.exportProfiles[2].name == "DemoDisk");
    CHECK(p2.exportProfiles[2].incremental);              // default true
    CHECK(p2.exportProfiles[2].targetPlatform == "Host"); // default
    // startupScene survives the read-modify-write save (old saveProject lost it).
    CHECK_FALSE(p2.startupScene.empty());

    // The foreign key must still be in the file.
    {
        std::ifstream in(heproj);
        nlohmann::json j = nlohmann::json::parse(in);
        CHECK(j.value("futureField", 0) == 42);
        CHECK(j.contains("preset"));
    }
    he_test::removeAllQuiet(dir);
}

// ─── Incremental packing ──────────────────────────────────────────────────────

// Three-asset content dir + one export call with given settings.
static ExportResult runExport(const fs::path& dir, const fs::path& out,
                              bool compress, bool encrypt, bool incremental)
{
    ExportSettings s;
    s.compress    = compress;
    s.encrypt     = encrypt;
    s.incremental = incremental;
    return ProjectExporter::exportProject(dir, "Inc", "", out, s);
}

TEST_CASE("Incremental export: unchanged assets are reused, changes repack")
{
    const auto dir = fs::temp_directory_path() / "he_inc_src";
    const auto out = fs::temp_directory_path() / "he_inc_out";
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(out);
    writeBlob(dir / "a.hasset", tinyHasset({0x1, 0xA}, "a.hasset"));
    writeBlob(dir / "b.hasset", tinyHasset({0x2, 0xB}, "b.hasset"));
    writeBlob(dir / "c.hasset", tinyHasset({0x3, 0xC}, "c.hasset"));

    // First export: nothing to reuse; manifest gets written.
    auto r1 = runExport(dir, out, /*compress*/true, false, true);
    REQUIRE(r1.success);
    CHECK(r1.assetsPacked == 3);
    CHECK(r1.assetsReused == 0);
    CHECK(fs::exists(out / "Inc.hpak.manifest"));

    // Second export, no changes: everything carried over verbatim.
    auto r2 = runExport(dir, out, true, false, true);
    REQUIRE(r2.success);
    CHECK(r2.assetsPacked == 3);
    CHECK(r2.assetsReused == 3);

    // A reused entry must still decode to the original bytes.
    {
        HpakReader reader;
        REQUIRE(reader.open((out / "Inc.hpak").string()));
        CHECK(reader.readEntry(HE::UUID{0x1, 0xA}) == tinyHasset({0x1, 0xA}, "a.hasset"));
    }

    // Modify one asset → exactly that one repacks.
    writeBlob(dir / "b.hasset", tinyHasset({0x2, 0xB}, "renamed/b.hasset"));
    auto r3 = runExport(dir, out, true, false, true);
    REQUIRE(r3.success);
    CHECK(r3.assetsPacked == 3);
    CHECK(r3.assetsReused == 2);

    // Manifest deleted → full repack (graceful fallback, still succeeds).
    he_test::removeQuiet(out / "Inc.hpak.manifest");
    auto r4 = runExport(dir, out, true, false, true);
    REQUIRE(r4.success);
    CHECK(r4.assetsReused == 0);

    // Settings change (codec) invalidates the manifest → full repack.
    auto r5 = runExport(dir, out, true, false, true);   // rebuild manifest (compress)
    CHECK(r5.assetsReused == 3);
    auto r6 = runExport(dir, out, /*compress*/false, false, true);
    REQUIRE(r6.success);
    CHECK(r6.assetsReused == 0);

    // incremental=false ignores the cache entirely.
    auto r7 = runExport(dir, out, false, false, true);  // manifest now matches store
    CHECK(r7.assetsReused == 3);
    auto r8 = runExport(dir, out, false, false, /*incremental*/false);
    REQUIRE(r8.success);
    CHECK(r8.assetsReused == 0);

    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(out);
}

#ifdef HE_HAVE_CRYPTO
TEST_CASE("Incremental export: encryption reuses the previous key, pak stays readable")
{
    const auto dir = fs::temp_directory_path() / "he_inc_enc_src";
    const auto out = fs::temp_directory_path() / "he_inc_enc_out";
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(out);
    writeBlob(dir / "a.hasset", tinyHasset({0x4, 0xD}, "a.hasset"));
    writeBlob(dir / "b.hasset", tinyHasset({0x5, 0xE}, "b.hasset"));

    auto r1 = runExport(dir, out, true, /*encrypt*/true, true);
    REQUIRE(r1.success);
    ProjectConfig cfg1;
    REQUIRE(ProjectConfigLoader::load(out, cfg1));
    REQUIRE(cfg1.encrypted);

    auto r2 = runExport(dir, out, true, true, true);
    REQUIRE(r2.success);
    CHECK(r2.assetsReused == 2);                       // verbatim incl. nonce+tag
    ProjectConfig cfg2;
    REQUIRE(ProjectConfigLoader::load(out, cfg2));
    CHECK(std::memcmp(cfg1.encKey, cfg2.encKey, 32) == 0); // key carried over

    // The reused encrypted entry decrypts with the shipped key.
    HpakReader reader;
    REQUIRE(reader.open((out / "Inc.hpak").string()));
    CHECK(reader.readEntry(HE::UUID{0x4, 0xD}, cfg2.encKey)
          == tinyHasset({0x4, 0xD}, "a.hasset"));

    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(out);
}
#endif

// ─── Embedded pak key ─────────────────────────────────────────────────────────

// Mirror of the game's EmbeddedPakKeyBlock initializer (magic must match).
static std::vector<uint8_t> fakeGameBinary(bool withBlock)
{
    std::vector<uint8_t> bin;
    for (int i = 0; i < 300; ++i) bin.push_back(static_cast<uint8_t>(i * 7 + 3));
    if (withBlock)
    {
        std::string magic = "HE_EMBEDDED_";
        magic += "PAKKEY_V1";
        magic.append(24 - magic.size(), '\0');
        bin.insert(bin.end(), magic.begin(), magic.end()); // magic[24]
        bin.push_back(0);                                  // hasKey = 0
        bin.insert(bin.end(), 7, 0);                       // pad[7]
        bin.insert(bin.end(), 32, 0);                      // key[32]
    }
    for (int i = 0; i < 300; ++i) bin.push_back(static_cast<uint8_t>(i * 13 + 1));
    return bin;
}

TEST_CASE("patchEmbeddedPakKey: patches the block, readEmbeddedPakKey round-trips")
{
    const auto dir = fs::temp_directory_path() / "he_embed_unit";
    he_test::removeAllQuiet(dir);
    fs::create_directories(dir);

    uint8_t key[32];
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(200 - i);

    SUBCASE("binary with a block")
    {
        writeBlob(dir / "game", fakeGameBinary(true));
        CHECK(patchEmbeddedPakKey(dir / "game", key) == 1);
        uint8_t got[32] = {};
        REQUIRE(readEmbeddedPakKey(dir / "game", got));
        CHECK(std::memcmp(got, key, 32) == 0);
    }
    SUBCASE("binary without a block → 0 patched, read fails")
    {
        writeBlob(dir / "plain", fakeGameBinary(false));
        CHECK(patchEmbeddedPakKey(dir / "plain", key) == 0);
        uint8_t got[32];
        CHECK_FALSE(readEmbeddedPakKey(dir / "plain", got));
    }
    SUBCASE("two blocks (universal binary): both patched")
    {
        auto two = fakeGameBinary(true);
        const auto second = fakeGameBinary(true);
        two.insert(two.end(), second.begin(), second.end());
        writeBlob(dir / "fat", two);
        CHECK(patchEmbeddedPakKey(dir / "fat", key) == 2);
    }
    SUBCASE("unpatched block reads as no key")
    {
        writeBlob(dir / "fresh", fakeGameBinary(true));
        uint8_t got[32];
        CHECK_FALSE(readEmbeddedPakKey(dir / "fresh", got)); // hasKey still 0
    }
    he_test::removeAllQuiet(dir);
}

#ifdef HE_HAVE_CRYPTO
TEST_CASE("Export with encryption embeds the key in the game binary, not the hcfg")
{
    const auto dir = fs::temp_directory_path() / "he_embed_src";
    const auto rt  = fs::temp_directory_path() / "he_embed_rt";
    const auto out = fs::temp_directory_path() / "he_embed_out";
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
    writeBlob(dir / "a.hasset", tinyHasset({0xE, 0x1}, "a.hasset"));
    writeBlob(rt / "HorizonGame", fakeGameBinary(true));
    writeBlob(rt / "libFake.dylib", fakeGameBinary(false));

    ExportSettings s;
    s.compress = true; s.encrypt = true; s.incremental = true;
    s.gameRuntimeDir = rt;
    auto r1 = ProjectExporter::exportProject(dir, "Emb", "", out, s);
    REQUIRE(r1.success);
    CHECK(r1.binaryFilesCopied == 2);
    CHECK(r1.keyEmbedded);

    // Key is in the shipped binary…
    uint8_t key[32] = {};
    REQUIRE(readEmbeddedPakKey(out / "HorizonGame", key));
    // …and NOT in project.hcfg (encrypted flag stays set, key is zeroed).
    ProjectConfig cfg;
    REQUIRE(ProjectConfigLoader::load(out, cfg));
    CHECK(cfg.encrypted);
    bool anyKeyByte = false;
    for (int i = 0; i < 32; ++i) anyKeyByte |= (cfg.encKey[i] != 0);
    CHECK_FALSE(anyKeyByte);

    // The pak decrypts with the embedded key (what the runtime will use).
    HpakReader reader;
    REQUIRE(reader.open((out / "Emb.hpak").string()));
    CHECK(reader.readEntry(HE::UUID{0xE, 0x1}, key) == tinyHasset({0xE, 0x1}, "a.hasset"));

    // Incremental re-export: key recovered from the patched binary (hcfg has
    // none), entries reused verbatim, key unchanged.
    auto r2 = ProjectExporter::exportProject(dir, "Emb", "", out, s);
    REQUIRE(r2.success);
    CHECK(r2.assetsReused == 1);
    CHECK(r2.keyEmbedded);
    uint8_t key2[32] = {};
    REQUIRE(readEmbeddedPakKey(out / "HorizonGame", key2));
    CHECK(std::memcmp(key, key2, 32) == 0);

    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
}

TEST_CASE("Export with encryption falls back to the hcfg key for a legacy runtime")
{
    const auto dir = fs::temp_directory_path() / "he_embed_legacy_src";
    const auto rt  = fs::temp_directory_path() / "he_embed_legacy_rt";
    const auto out = fs::temp_directory_path() / "he_embed_legacy_out";
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
    writeBlob(dir / "a.hasset", tinyHasset({0xE, 0x2}, "a.hasset"));
    writeBlob(rt / "HorizonGame", fakeGameBinary(false)); // no key block

    ExportSettings s;
    s.compress = true; s.encrypt = true;
    s.gameRuntimeDir = rt;
    auto r = ProjectExporter::exportProject(dir, "Leg", "", out, s);
    REQUIRE(r.success);
    CHECK_FALSE(r.keyEmbedded);

    ProjectConfig cfg;
    REQUIRE(ProjectConfigLoader::load(out, cfg));
    REQUIRE(cfg.encrypted);
    bool anyKeyByte = false;
    for (int i = 0; i < 32; ++i) anyKeyByte |= (cfg.encKey[i] != 0);
    CHECK(anyKeyByte); // key ships in the hcfg as before

    HpakReader reader;
    REQUIRE(reader.open((out / "Leg.hpak").string()));
    CHECK(reader.readEntry(HE::UUID{0xE, 0x2}, cfg.encKey) == tinyHasset({0xE, 0x2}, "a.hasset"));

    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
}
#endif

TEST_CASE("Export fails when the runtime dir is named but missing")
{
    const auto dir = fs::temp_directory_path() / "he_missrt_src";
    const auto out = fs::temp_directory_path() / "he_missrt_out";
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(out);
    writeBlob(dir / "a.hasset", tinyHasset({0xE, 0x4}, "a.hasset"));

    ExportSettings s;
    s.compress = false;
    s.gameRuntimeDir = fs::temp_directory_path() / "he_missrt_DOES_NOT_EXIST";
    auto r = ProjectExporter::exportProject(dir, "Miss", "", out, s);
    CHECK_FALSE(r.success);
    CHECK(r.errorMessage.find("not found") != std::string::npos);
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(out);
}

TEST_CASE("Export fails hard when copying a runtime binary fails")
{
    const auto dir = fs::temp_directory_path() / "he_cpyfail_src";
    const auto rt  = fs::temp_directory_path() / "he_cpyfail_rt";
    const auto out = fs::temp_directory_path() / "he_cpyfail_out";
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
    writeBlob(dir / "a.hasset", tinyHasset({0xE, 0x5}, "a.hasset"));
    writeBlob(rt / "HorizonGame", fakeGameBinary(true));
    writeBlob(rt / "libFake.dylib", fakeGameBinary(false));
    // A NON-EMPTY directory squatting on the destination name makes copy_file
    // fail (and can't be cleared by the pre-copy remove, which only deletes
    // files/empty dirs) — previously a copy failure was silently skipped,
    // shipping a stale/missing exe as OK.
    fs::create_directories(out / "HorizonGame");
    writeBlob(out / "HorizonGame" / "squat", { 0x00 });

    ExportSettings s;
    s.compress = false;
    s.gameRuntimeDir = rt;
    auto r = ProjectExporter::exportProject(dir, "CpyFail", "", out, s);
    CHECK_FALSE(r.success);
    CHECK(r.errorMessage.find("Failed to copy") != std::string::npos);
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
}

#ifdef HE_HAVE_CRYPTO
TEST_CASE("Key patching preserves the executable bit")
{
    const auto dir = fs::temp_directory_path() / "he_perm_src";
    const auto rt  = fs::temp_directory_path() / "he_perm_rt";
    const auto out = fs::temp_directory_path() / "he_perm_out";
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
    writeBlob(dir / "a.hasset", tinyHasset({0xE, 0x6}, "a.hasset"));
    writeBlob(rt / "HorizonGame", fakeGameBinary(true));
    fs::permissions(rt / "HorizonGame",
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read);

    ExportSettings s;
    s.compress = false; s.encrypt = true;
    s.gameRuntimeDir = rt;
    auto r = ProjectExporter::exportProject(dir, "Perm", "", out, s);
    REQUIRE(r.success);
    REQUIRE(r.keyEmbedded);
    // The temp+rename patch path must not strip +x — an exported game the OS
    // refuses to execute is just as broken as a missing one.
    const auto perms = fs::status(out / "HorizonGame").permissions();
    CHECK((perms & fs::perms::owner_exec) != fs::perms::none);
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
}
#endif

TEST_CASE("findRuntimeBundle: a previous export output is not a runtime bundle")
{
    const auto root = fs::temp_directory_path() / "he_bundle_export_root";
    he_test::removeAllQuiet(root);
    // Looks like a bundle (has HorizonGame) but is an old EXPORT (has hcfg) —
    // shipping it would carry a stale patched key + stale binaries.
    writeBlob(root / "deploy" / "Game" / "HorizonGame", fakeGameBinary(false));
    writeBlob(root / "deploy" / "Game" / "project.hcfg", { 0x01, 0x02 });
    fs::create_directories(root / "deploy" / "Editor");
    CHECK(findRuntimeBundle(root / "deploy" / "Editor", ExportPlatform::Host).empty());
    he_test::removeAllQuiet(root);
}

TEST_CASE("Export fails when the runtime dir yields no binaries")
{
    const auto dir = fs::temp_directory_path() / "he_nobin_src";
    const auto rt  = fs::temp_directory_path() / "he_nobin_rt";   // exists, empty
    const auto out = fs::temp_directory_path() / "he_nobin_out";
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
    writeBlob(dir / "a.hasset", tinyHasset({0xE, 0x3}, "a.hasset"));
    fs::create_directories(rt);

    ExportSettings s;
    s.compress = false;
    s.gameRuntimeDir = rt;
    auto r = ProjectExporter::exportProject(dir, "NoBin", "", out, s);
    CHECK_FALSE(r.success);
    CHECK(r.errorMessage.find("no files") != std::string::npos);

    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
}

// ─── E6: no game leftovers in a shipped application ───────────────────────────

namespace
{
// The window half of what the export dialog writes into config.json, in the
// shape GlobalState reads (the "CustomConfig" array — a flat object parses and
// is then ignored).
std::string windowConfigJson(const char* mode)
{
    nlohmann::json j;
    j["CustomConfig"] = nlohmann::json::array({
        nlohmann::json{ { "Key", "GameWindowWidth"  }, { "Value", "1280"  } },
        nlohmann::json{ { "Key", "GameWindowHeight" }, { "Value", "720"   } },
        nlohmann::json{ { "Key", "GameWindowMode"   }, { "Value", mode    } },
    });
    return j.dump(4);
}

// The GameWindowMode the export left in config.json, or "" when it wrote none.
std::string shippedWindowMode(const fs::path& outDir)
{
    std::ifstream in(outDir / "config.json");
    if (!in) return {};
    const auto j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return {};
    const auto entries = j.find("CustomConfig");
    if (entries == j.end() || !entries->is_array()) return {};
    for (const auto& e : *entries)
        if (e.is_object() && e.value("Key", std::string{}) == "GameWindowMode")
            return e.value("Value", std::string{});
    return {};
}

// One export into a fresh directory pair, with a single asset so packing has
// something to do. Returns the directory config.json lands in.
fs::path exportWithWindowMode(const char* tag, bool appProject, const char* mode)
{
    const auto dir = fs::temp_directory_path() / (std::string("he_e6_src_")  + tag);
    const auto out = fs::temp_directory_path() / (std::string("he_e6_out_")  + tag);
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(out);
    writeBlob(dir / "a.hasset", tinyHasset({ 0xE, 0x6 }, "a.hasset"));

    ExportSettings s;
    s.compress        = false;
    s.appProject      = appProject;
    s.gameConfigJson  = windowConfigJson(mode);
    const auto r = ProjectExporter::exportProject(dir, tag, "", out, s);
    REQUIRE_MESSAGE(r.success, r.errorMessage);
    he_test::removeAllQuiet(dir);
    return out;
}
} // namespace

TEST_CASE("E6: an exported application never ships a fullscreen window mode")
{
    // The dialog remembers the window mode across exports, so a Fullscreen left
    // standing from a game export is exactly what reaches an app export. The
    // runtime opens an app windowed by itself, but config.json overrides that
    // default — so this has to be corrected before it is written, not after.
    const auto app = exportWithWindowMode("app", /*appProject=*/true, "Fullscreen");
    CHECK(shippedWindowMode(app) == "Windowed");
    he_test::removeAllQuiet(app);

    // Borderless is NOT touched: a frameless window is what an application with
    // its own title bar asks for, and the rule is about fullscreen.
    const auto brdr = exportWithWindowMode("brdr", /*appProject=*/true, "Borderless");
    CHECK(shippedWindowMode(brdr) == "Borderless");
    he_test::removeAllQuiet(brdr);

    // A GAME keeps what it was given. Fullscreen is the right default there, and
    // this guard must not reach across into it.
    const auto game = exportWithWindowMode("game", /*appProject=*/false, "Fullscreen");
    CHECK(shippedWindowMode(game) == "Fullscreen");
    he_test::removeAllQuiet(game);
}

TEST_CASE("E6: the engine's splash screen is off unless a host asks for it")
{
    // Every host links HorizonCore, a shipped game and a shipped application
    // included. With this default flipped, each of them would open a window
    // carrying the Horizon wordmark ahead of its own first frame — the engine
    // advertising itself inside someone else's product. The editor is the only
    // host that sets it, and it sets it explicitly.
    CHECK_FALSE(HE::SplashConfig{}.enabled);
}

TEST_CASE("findRuntimeBundle: deploy layout, build-tree layout, cross-platform")
{
    const auto root = fs::temp_directory_path() / "he_bundle_root";
    he_test::removeAllQuiet(root);

    // Deploy layout: <root>/deploy/{Editor,Game}
    writeBlob(root / "deploy" / "Game" / "HorizonGame", fakeGameBinary(false));
    fs::create_directories(root / "deploy" / "Editor");
    CHECK(findRuntimeBundle(root / "deploy" / "Editor", ExportPlatform::Host)
          == (root / "deploy" / "Game").lexically_normal());

    // Build-tree layout: editor runs from <root>/cmake-build/src/HE_Editor,
    // runtime deployed to <root>/out/deploy/Game.
    writeBlob(root / "out" / "deploy" / "Game" / "HorizonGame", fakeGameBinary(false));
    fs::create_directories(root / "cmake-build" / "src" / "HE_Editor");
    CHECK(findRuntimeBundle(root / "cmake-build" / "src" / "HE_Editor", ExportPlatform::Host)
          == (root / "out" / "deploy" / "Game").lexically_normal());

    // Cross-platform bundle with a Windows exe.
    writeBlob(root / "out" / "deploy" / "GameRuntimes" / "Windows" / "HorizonGame.exe",
              fakeGameBinary(false));
    CHECK(findRuntimeBundle(root / "cmake-build" / "src" / "HE_Editor", ExportPlatform::Windows)
          == (root / "out" / "deploy" / "GameRuntimes" / "Windows").lexically_normal());

    // A bundle dir WITHOUT the executable does not qualify.
    fs::create_directories(root / "empty" / "Game");
    fs::create_directories(root / "empty" / "Editor");
    CHECK(findRuntimeBundle(root / "empty" / "Editor", ExportPlatform::Linux).empty());

    he_test::removeAllQuiet(root);
}

// ─── Runtime flavours (docs/he-apps-plan.md A3b) ──────────────────────────────
//
// Three runtimes per platform, told apart by one word. That word is spelled in
// four places — this enum, HE_RUNTIME_DIR_NAME in the root CMakeLists.txt,
// FLAVOR_DIRS in scripts/build_runtimes.py and DIR_TO_FLAVOR in
// scripts/runtime_size.py — so the first thing worth pinning is the spelling
// itself: a rename in one place and not the others produces an exporter that
// looks in a directory nobody writes, and nothing else notices.

TEST_CASE("runtimeFlavorName: the directory spelling is the enum's name")
{
    CHECK(std::string(runtimeFlavorName(RuntimeFlavor::Game))        == "Game");
    CHECK(std::string(runtimeFlavorName(RuntimeFlavor::AppAdvanced)) == "AppAdvanced");
    CHECK(std::string(runtimeFlavorName(RuntimeFlavor::AppBasic))    == "AppBasic");
}

TEST_CASE("runtimeFlavorFor: a game always takes the full runtime")
{
    // Advanced Shader Effects is an app-side question. For a game it decides
    // nothing about the runtime, because a game may meet any GPU and dropping
    // four of five backends from it is a support problem, not a size saving.
    CHECK(runtimeFlavorFor(false, true)  == RuntimeFlavor::Game);
    CHECK(runtimeFlavorFor(false, false) == RuntimeFlavor::Game);
    CHECK(runtimeFlavorFor(true,  true)  == RuntimeFlavor::AppAdvanced);
    CHECK(runtimeFlavorFor(true,  false) == RuntimeFlavor::AppBasic);
}

TEST_CASE("resolveRuntimeDir: Host is flat, cross-targets nest under the platform")
{
    const fs::path base = fs::path("/tmp") / "he_editor";
    auto norm = [](const fs::path& p) { return p.lexically_normal(); };

    // Host: the three deploy directories sit beside each other, because that is
    // where the build writes them.
    CHECK(norm(resolveRuntimeDir(base, ExportPlatform::Host, RuntimeFlavor::Game))
          == norm(fs::path("/tmp") / "Game"));
    CHECK(norm(resolveRuntimeDir(base, ExportPlatform::Host, RuntimeFlavor::AppAdvanced))
          == norm(fs::path("/tmp") / "AppAdvanced"));
    CHECK(norm(resolveRuntimeDir(base, ExportPlatform::Host, RuntimeFlavor::AppBasic))
          == norm(fs::path("/tmp") / "AppBasic"));

    // Cross-target: the game runtime keeps the path it always had — an editor
    // updated to this change must still find a bundle a user dropped there
    // before it existed.
    CHECK(norm(resolveRuntimeDir(base, ExportPlatform::Windows, RuntimeFlavor::Game))
          == norm(fs::path("/tmp") / "GameRuntimes" / "Windows"));
    CHECK(norm(resolveRuntimeDir(base, ExportPlatform::Windows))
          == norm(fs::path("/tmp") / "GameRuntimes" / "Windows"));
    // The app flavours nest one level deeper: all three of a platform's runtimes
    // arrive together, so they must not collide on one directory name.
    CHECK(norm(resolveRuntimeDir(base, ExportPlatform::Linux, RuntimeFlavor::AppBasic))
          == norm(fs::path("/tmp") / "GameRuntimes" / "Linux" / "AppBasic"));
}

TEST_CASE("findRuntimeBundle: the wanted flavour wins, Game is the fallback")
{
    const auto root = fs::temp_directory_path() / "he_flavor_root";
    he_test::removeAllQuiet(root);
    const auto editor = root / "deploy" / "Editor";
    fs::create_directories(editor);

    // Only the game runtime exists — the state of every checkout where nobody
    // ran scripts/build_runtimes.py, and of every editor built before the app
    // runtimes did. An app must still export: fatter beats not at all.
    writeBlob(root / "deploy" / "Game" / "HorizonGame", fakeGameBinary(false));
    RuntimeFlavor got = RuntimeFlavor::AppBasic;
    CHECK(findRuntimeBundle(editor, ExportPlatform::Host, RuntimeFlavor::AppAdvanced, &got)
          == (root / "deploy" / "Game").lexically_normal());
    // And it must SAY so, or the fallback is a silent ~20 MB substitution.
    CHECK(got == RuntimeFlavor::Game);

    // Now the app runtime is there. It is preferred, and outFlavor confirms it.
    writeBlob(root / "deploy" / "AppAdvanced" / "HorizonGame", fakeGameBinary(false));
    got = RuntimeFlavor::Game;
    CHECK(findRuntimeBundle(editor, ExportPlatform::Host, RuntimeFlavor::AppAdvanced, &got)
          == (root / "deploy" / "AppAdvanced").lexically_normal());
    CHECK(got == RuntimeFlavor::AppAdvanced);

    // Asking for a flavour that was never built, while a DIFFERENT app flavour
    // exists, still falls back to Game and never to the wrong app runtime — an
    // app-basic tree has no GPU backend at all, shipping it for an advanced
    // project would start and then render nothing.
    got = RuntimeFlavor::AppAdvanced;
    CHECK(findRuntimeBundle(editor, ExportPlatform::Host, RuntimeFlavor::AppBasic, &got)
          == (root / "deploy" / "Game").lexically_normal());
    CHECK(got == RuntimeFlavor::Game);

    // The default argument is Game, so every caller written before the flavours
    // existed keeps behaving exactly as it did.
    CHECK(findRuntimeBundle(editor, ExportPlatform::Host)
          == (root / "deploy" / "Game").lexically_normal());

    he_test::removeAllQuiet(root);
}

TEST_CASE("findRuntimeBundle: a distant app runtime beats a close game runtime")
{
    // The reason the search runs flavour-by-flavour and not directory-by-
    // directory. The editor runs from a build tree with out/deploy/Game right
    // beside it and the app runtime two levels up; walking the tree once and
    // taking the first bundle found would hand back Game and silently ship
    // ~20 MB of glslang the app never uses.
    const auto root = fs::temp_directory_path() / "he_flavor_depth";
    he_test::removeAllQuiet(root);
    const auto editor = root / "inner" / "cmake-build" / "src" / "HE_Editor";
    fs::create_directories(editor);
    writeBlob(root / "inner" / "cmake-build" / "src" / "out" / "deploy" / "Game" / "HorizonGame",
              fakeGameBinary(false));
    writeBlob(root / "out" / "deploy" / "AppBasic" / "HorizonGame", fakeGameBinary(false));

    RuntimeFlavor got = RuntimeFlavor::Game;
    CHECK(findRuntimeBundle(editor, ExportPlatform::Host, RuntimeFlavor::AppBasic, &got)
          == (root / "out" / "deploy" / "AppBasic").lexically_normal());
    CHECK(got == RuntimeFlavor::AppBasic);

    he_test::removeAllQuiet(root);
}

TEST_CASE("findRuntimeBundle: cross-platform app runtimes nest under the platform")
{
    const auto root = fs::temp_directory_path() / "he_flavor_cross";
    he_test::removeAllQuiet(root);
    const auto editor = root / "deploy" / "Editor";
    fs::create_directories(editor);
    const auto rts = root / "deploy" / "GameRuntimes" / "Windows";
    writeBlob(rts / "HorizonGame.exe", fakeGameBinary(false));
    writeBlob(rts / "AppBasic" / "HorizonGame.exe", fakeGameBinary(false));

    RuntimeFlavor got = RuntimeFlavor::Game;
    CHECK(findRuntimeBundle(editor, ExportPlatform::Windows, RuntimeFlavor::AppBasic, &got)
          == (rts / "AppBasic").lexically_normal());
    CHECK(got == RuntimeFlavor::AppBasic);

    // The nested one is not visible to a Game search: <platform>/AppBasic is a
    // sub-directory of the game bundle, and isRuntimeBundle only looks at the
    // directory it is handed.
    CHECK(findRuntimeBundle(editor, ExportPlatform::Windows, RuntimeFlavor::Game)
          == rts.lexically_normal());

    // Nothing at all for a platform: empty, and outFlavor is left at the wish
    // rather than at some half-found value.
    got = RuntimeFlavor::Game;
    CHECK(findRuntimeBundle(editor, ExportPlatform::Linux, RuntimeFlavor::AppAdvanced, &got)
          .empty());
    CHECK(got == RuntimeFlavor::AppAdvanced);

    he_test::removeAllQuiet(root);
}

// ─── macOS .app bundle ────────────────────────────────────────────────────────

TEST_CASE("Export .app bundle: layout routes binaries vs data correctly")
{
    const auto dir = fs::temp_directory_path() / "he_app_src";
    const auto rt  = fs::temp_directory_path() / "he_app_rt";
    const auto out = fs::temp_directory_path() / "he_app_out";
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
    writeBlob(dir / "a.hasset", tinyHasset({0xA9, 0x1}, "a.hasset"));
    // A runtime dir with the exe, an engine dylib, GameLogic, and a data file.
    writeBlob(rt / "HorizonGame",           fakeGameBinary(false));
    writeBlob(rt / "libHorizonCore.dylib",  fakeGameBinary(false));
    writeBlob(rt / "GameLogic.dylib",       fakeGameBinary(false));
    writeBlob(rt / "config.json",           { '{', '}' });

    ExportSettings s;
    s.compress = false;
    s.gameRuntimeDir = rt;
    s.appBundle = true;
    // Off-macOS this still builds the structure (only the codesign step is
    // skipped); on macOS the sign runs but our fake exe isn't a Mach-O — so
    // this layout test is guarded to non-Apple. The Apple path is covered by
    // the real-binary end-to-end test below.
#ifndef __APPLE__
    auto r = ProjectExporter::exportProject(dir, "MyGame", "", out, s);
    REQUIRE(r.success);

    const auto app = out / "MyGame.app";
    CHECK(fs::exists(app / "Contents" / "Info.plist"));
    // Executable + engine dylib next to the exe.
    CHECK(fs::exists(app / "Contents" / "MacOS" / "HorizonGame"));
    CHECK(fs::exists(app / "Contents" / "MacOS" / "libHorizonCore.dylib"));
    // GameLogic + data + pak + hcfg in Resources (SDL_GetBasePath).
    CHECK(fs::exists(app / "Contents" / "Resources" / "GameLogic.dylib"));
    CHECK(fs::exists(app / "Contents" / "Resources" / "config.json"));
    CHECK(fs::exists(app / "Contents" / "Resources" / "MyGame.hpak"));
    CHECK(fs::exists(app / "Contents" / "Resources" / "project.hcfg"));
    // The dylib must NOT also be in Resources, nor the pak in MacOS.
    CHECK_FALSE(fs::exists(app / "Contents" / "Resources" / "libHorizonCore.dylib"));
    CHECK_FALSE(fs::exists(app / "Contents" / "MacOS" / "MyGame.hpak"));

    // Info.plist names the executable.
    std::ifstream pf(app / "Contents" / "Info.plist");
    std::string plist((std::istreambuf_iterator<char>(pf)), std::istreambuf_iterator<char>());
    CHECK(plist.find("<key>CFBundleExecutable</key><string>HorizonGame</string>") != std::string::npos);
    CHECK(plist.find("com.horizonengine.mygame") != std::string::npos);
#endif
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
}

#if defined(__APPLE__) && defined(HE_TEST_GAME_EXE)
TEST_CASE("Export .app bundle: real binary is bundled, signed, and (encrypted) key-embedded")
{
    const auto dir = fs::temp_directory_path() / "he_app_real_src";
    const auto rt  = fs::temp_directory_path() / "he_app_real_rt";
    const auto out = fs::temp_directory_path() / "he_app_real_out";
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
    writeBlob(dir / "a.hasset", tinyHasset({0xA9, 0x2}, "a.hasset"));
    fs::create_directories(rt);
    // The genuine HorizonGame Mach-O, so codesign + key patching operate on real
    // structures. A permission copy keeps the +x bit. config.json is a data file
    // (not Mach-O, safe under codesign --deep) → must route to Resources.
    fs::copy_file(HE_TEST_GAME_EXE, rt / "HorizonGame");
    writeBlob(rt / "config.json", { '{', '}' });

    ExportSettings s;
    s.compress = false;
    s.encrypt  = true;                 // exercises key-embed into the bundled exe
    s.gameRuntimeDir = rt;
    s.appBundle = true;
    auto r = ProjectExporter::exportProject(dir, "RealApp", "", out, s);
    REQUIRE_MESSAGE(r.success, r.errorMessage);
    CHECK(r.keyEmbedded);

    const auto app = out / "RealApp.app";
    const auto exe = app / "Contents" / "MacOS" / "HorizonGame";
    REQUIRE(fs::exists(exe));
    // Data-vs-bin split: exe in MacOS, pak/hcfg/config in Resources.
    CHECK(fs::exists(app / "Contents" / "Resources" / "RealApp.hpak"));
    CHECK(fs::exists(app / "Contents" / "Resources" / "config.json"));
    CHECK(fs::exists(app / "Contents" / "Info.plist"));
    CHECK_FALSE(fs::exists(app / "Contents" / "MacOS" / "config.json"));
    CHECK((fs::status(exe).permissions() & fs::perms::owner_exec) != fs::perms::none);

    // The whole bundle carries a valid (ad-hoc) signature.
    const std::string verify = "/usr/bin/codesign --verify '" + app.string() + "' 2>/dev/null";
    CHECK(std::system(verify.c_str()) == 0);

    // The key was patched into the bundled executable and the pak decrypts with it.
    uint8_t key[32] = {};
    REQUIRE(readEmbeddedPakKey(exe, key));
    HpakReader reader;
    REQUIRE(reader.open((app / "Contents" / "Resources" / "RealApp.hpak").string()));
    CHECK(reader.readEntry(HE::UUID{0xA9, 0x2}, key) == tinyHasset({0xA9, 0x2}, "a.hasset"));

    // The hcfg carries only the flag, not the key.
    ProjectConfig cfg;
    REQUIRE(ProjectConfigLoader::load(app / "Contents" / "Resources", cfg));
    CHECK(cfg.encrypted);
    bool anyKeyByte = false;
    for (int i = 0; i < 32; ++i) anyKeyByte |= (cfg.encKey[i] != 0);
    CHECK_FALSE(anyKeyByte);

    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
}

TEST_CASE("Export .app bundle: re-export over an existing signed bundle re-signs cleanly")
{
    const auto dir = fs::temp_directory_path() / "he_app_reexp_src";
    const auto rt  = fs::temp_directory_path() / "he_app_reexp_rt";
    const auto out = fs::temp_directory_path() / "he_app_reexp_out";
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
    writeBlob(dir / "a.hasset", tinyHasset({0xA9, 0x3}, "a.hasset"));
    fs::create_directories(rt);
    fs::copy_file(HE_TEST_GAME_EXE, rt / "HorizonGame");
    // A stray extra dylib present in the FIRST runtime but not the second, to
    // prove stale binaries do not linger in the re-signed bundle.
    fs::copy_file(HE_TEST_GAME_EXE, rt / "libStale.dylib");

    ExportSettings s;
    s.compress = true; s.encrypt = true; s.incremental = true;
    s.gameRuntimeDir = rt;
    s.appBundle = true;

    // First export: builds + signs the .app (with libStale.dylib inside).
    auto r1 = ProjectExporter::exportProject(dir, "ReApp", "", out, s);
    REQUIRE_MESSAGE(r1.success, r1.errorMessage);
    const auto app = out / "ReApp.app";
    REQUIRE(fs::exists(app / "Contents" / "MacOS" / "libStale.dylib"));

    // Drop the stray dylib, then re-export over the existing signed bundle —
    // this used to fail codesign (stale _CodeSignature + leftover binary).
    he_test::removeQuiet(rt / "libStale.dylib");
    auto r2 = ProjectExporter::exportProject(dir, "ReApp", "", out, s);
    REQUIRE_MESSAGE(r2.success, r2.errorMessage);
    CHECK(r2.assetsReused == 1);                       // incremental still worked

    // The stale dylib is gone and the re-signed bundle verifies.
    CHECK_FALSE(fs::exists(app / "Contents" / "MacOS" / "libStale.dylib"));
    const std::string verify = "/usr/bin/codesign --verify --deep '" + app.string() + "' 2>/dev/null";
    CHECK(std::system(verify.c_str()) == 0);

    // The pak still decrypts with the (reused) embedded key.
    uint8_t key[32] = {};
    REQUIRE(readEmbeddedPakKey(app / "Contents" / "MacOS" / "HorizonGame", key));
    HpakReader reader;
    REQUIRE(reader.open((app / "Contents" / "Resources" / "ReApp.hpak").string()));
    CHECK(reader.readEntry(HE::UUID{0xA9, 0x3}, key) == tinyHasset({0xA9, 0x3}, "a.hasset"));

    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(rt); he_test::removeAllQuiet(out);
}
#endif

// ─── Platform targets ─────────────────────────────────────────────────────────

TEST_CASE("ExportPlatform: name mapping and runtime-dir resolution")
{
    CHECK(std::string(exportPlatformName(ExportPlatform::Host))    == "Host");
    CHECK(std::string(exportPlatformName(ExportPlatform::Windows)) == "Windows");
    CHECK(std::string(exportPlatformName(ExportPlatform::MacOS))   == "macOS");
    CHECK(std::string(exportPlatformName(ExportPlatform::Linux))   == "Linux");

    CHECK(exportPlatformFromName("Windows") == ExportPlatform::Windows);
    CHECK(exportPlatformFromName("macOS")   == ExportPlatform::MacOS);
    CHECK(exportPlatformFromName("Linux")   == ExportPlatform::Linux);
    CHECK(exportPlatformFromName("Host")    == ExportPlatform::Host);
    CHECK(exportPlatformFromName("")        == ExportPlatform::Host);   // unknown → Host
    CHECK(exportPlatformFromName("Amiga")   == ExportPlatform::Host);

    const fs::path base = "/opt/editor";
    CHECK(resolveRuntimeDir(base, ExportPlatform::Host).lexically_normal()
          == fs::path("/opt/Game"));
    CHECK(resolveRuntimeDir(base, ExportPlatform::Windows).lexically_normal()
          == fs::path("/opt/GameRuntimes/Windows"));
    CHECK(resolveRuntimeDir(base, ExportPlatform::Linux).lexically_normal()
          == fs::path("/opt/GameRuntimes/Linux"));
}

// ─── Review-fix regressions ───────────────────────────────────────────────────

TEST_CASE("ProjectManager: type-malformed profile values load without throwing")
{
    const auto dir = fs::temp_directory_path() / "he_prof_badtypes";
    he_test::removeAllQuiet(dir);
    fs::create_directories(dir);
    {
        // name as number, compress as string, excludePatterns with mixed types,
        // one non-object entry, activeExportProfile as number.
        std::ofstream out(dir / "B.heproj");
        out << R"({"name":123,"startupScene":7,"exportProfiles":[
                    42,
                    {"name":"Odd","compress":"yes","encrypt":1,
                     "excludePatterns":["ok",5,true,"also_ok"]},
                    {"compress":true}
                  ],"activeExportProfile":9})";
    }
    ProjectManager pm;
    REQUIRE(pm.loadProject((dir / "B.heproj").string())); // must not throw
    const auto& p = pm.currentProject();
    REQUIRE(p.exportProfiles.size() == 1);               // only "Odd" has a name
    CHECK(p.exportProfiles[0].compress == true);          // wrong type → default
    CHECK(p.exportProfiles[0].excludePatterns
          == std::vector<std::string>{ "ok", "also_ok" });
    CHECK(p.activeExportProfile == "Odd");                // number → fallback
    CHECK(p.name == "B");                                 // number → filename stem
    he_test::removeAllQuiet(dir);
}

TEST_CASE("ProjectManager: saveProject refuses to clobber a corrupt manifest")
{
    const auto dir = fs::temp_directory_path() / "he_prof_corrupt";
    he_test::removeAllQuiet(dir);
    fs::create_directories(dir);
    const auto heproj = dir / "C.heproj";
    {
        std::ofstream out(heproj);
        out << "{ this is not json";
    }
    ProjectManager pm;
    pm.currentProject().name = "C";
    pm.currentProject().exportProfiles = defaultExportProfiles();
    CHECK_FALSE(pm.saveProject(heproj.string()));         // refuse, don't overwrite
    {
        std::ifstream in(heproj);
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        CHECK(content == "{ this is not json");           // untouched
    }
    CHECK_FALSE(fs::exists(heproj.string() + ".tmp"));    // no temp left behind
    he_test::removeAllQuiet(dir);
}

TEST_CASE("HpakWriter: unreadable subdirectory does not throw out of addDirectory")
{
    const auto dir = fs::temp_directory_path() / "he_excl_denied";
    he_test::removeAllQuiet(dir);
    writeBlob(dir / "ok.hasset", tinyHasset({0x7, 0x7}, "ok.hasset"));
    fs::create_directories(dir / "locked");
    writeBlob(dir / "locked" / "hidden.hasset", tinyHasset({0x8, 0x8}, "locked/hidden.hasset"));
    fs::permissions(dir / "locked", fs::perms::none);     // chmod 000

    HpakWriter packer;
    int added = -1;
    // The old range-for iteration threw filesystem_error here (== std::terminate
    // on the export worker thread). Must complete and pack the readable asset.
    CHECK_NOTHROW(added = packer.addDirectory(dir, Hpak::PackSettings{}));
    CHECK(added >= 1);

    fs::permissions(dir / "locked", fs::perms::owner_all); // restore for cleanup
    he_test::removeAllQuiet(dir);
}

TEST_CASE("HAsset::Reader: corrupt chunk size fails cleanly instead of allocating")
{
    // Header claims one chunk whose size field is bogus-huge: openData must
    // return false (bounds check) rather than resize(huge) → length_error. All 8
    // size bytes are set to 0xFF so `offset + size` would WRAP size_t — the
    // overflow-safe remaining-bytes comparison must still reject it.
    auto blob = tinyHasset({0x9, 0x9}, "x.hasset");
    // Chunk layout: FileHeader(32B), then ChunkHeader { uint32 id; uint64 size; }.
    REQUIRE(blob.size() > sizeof(HAsset::FileHeader) + 12);
    const size_t sizeOff = sizeof(HAsset::FileHeader) + 4; // after chunk id
    for (int i = 0; i < 8; ++i) blob[sizeOff + i] = 0xFF;

    HAsset::Reader r;
    bool ok = true;
    CHECK_NOTHROW(ok = r.openData(blob));
    CHECK_FALSE(ok);
}

TEST_CASE("ProjectManager: unknown active profile falls back to the first")
{
    const auto dir = fs::temp_directory_path() / "he_prof_fallback";
    he_test::removeAllQuiet(dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "F.heproj");
        out << R"({"name":"F","exportProfiles":[{"name":"Only"}],"activeExportProfile":"Ghost"})";
    }
    ProjectManager pm;
    REQUIRE(pm.loadProject((dir / "F.heproj").string()));
    CHECK(pm.currentProject().activeExportProfile == "Only");
    he_test::removeAllQuiet(dir);
}

// The Game/Simulation templates seed a Sky + Weather entity into their StartupScene;
// Empty/Tool start bare. Verify by loading the generated scene into a world.
static void checkTemplateScene(ProjectPreset preset, const char* name, bool expectSky)
{
    const auto dir = fs::temp_directory_path() / name;
    he_test::removeAllQuiet(dir);

    ProjectManager pm;
    REQUIRE(pm.createNewProject(dir.string(), "T", preset));
    const fs::path scene = dir / "Content" / "StartupScene.hescene";
    REQUIRE(fs::exists(scene));

    HorizonWorld world;
    SceneSerializer ser;
    REQUIRE(ser.load(world, scene.string(), SerializeFormat::JSON));

    if (expectSky)
    {
        const Entity sky = world.environmentEntity();
        const Entity weather = world.weatherEntity();
        CHECK((sky != entt::null));
        CHECK((weather != entt::null));
        CHECK(sky != world.rootEntity());        // dedicated entities, not the root
        CHECK(weather != world.rootEntity());
    }
    else
    {
        CHECK((world.environmentEntity() == entt::null));
        CHECK((world.weatherEntity()     == entt::null));
    }
    he_test::removeAllQuiet(dir);
}

TEST_CASE("Game/Simulation templates seed Sky+Weather; Empty/Tool start bare")
{
    checkTemplateScene(ProjectPreset::Game,       "he_tpl_game",  true);
    checkTemplateScene(ProjectPreset::Simulation, "he_tpl_sim",   true);
    checkTemplateScene(ProjectPreset::Empty,      "he_tpl_empty", false);
    checkTemplateScene(ProjectPreset::Tool,       "he_tpl_tool",  false);
}

// ─── Pack-time GI-hit approx re-fold ──────────────────────────────────────────
// Files saved BEFORE the approx feature carry no approxBaseColor/... tail; the
// editor only bakes it on a re-save. The packer must re-fold the values from
// the node graph so packaged reflections keep their colours (the bug: every
// graph material reflected white in shipped games until manually re-saved).

#include <MaterialGraph/MaterialGraph.h>

// Graph material .hasset in the PRE-approx MTRL layout: every field through
// customShaderGBufGlsl, then EOF (no approx block).
static std::vector<uint8_t> preApproxGraphMaterial(HE::UUID id, const std::string& relPath,
                                                   const std::string& graphJson,
                                                   const std::string& parentPath = {},
                                                   const std::vector<std::string>& switchNames = {},
                                                   const std::vector<uint8_t>& switchValues = {})
{
    std::vector<uint8_t> meta;
    HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(HE::AssetType::Material));
    HAsset::Writer::appendPOD(meta, id.hi);
    HAsset::Writer::appendPOD(meta, id.lo);
    HAsset::Writer::appendString(meta, fs::path(relPath).stem().string());
    HAsset::Writer::appendString(meta, relPath);

    std::vector<uint8_t> mtrl;
    HAsset::Writer::appendString(mtrl, std::string{});               // shaderPath
    HAsset::Writer::appendVec(mtrl, std::vector<std::string>{});     // texturePaths
    const float pbr[6] = { 1.f, 1.f, 1.f, 0.f, 0.5f, 1.f };          // baseColor+met+rough+opacity
    for (float f : pbr) HAsset::Writer::appendPOD(mtrl, f);
    HAsset::Writer::appendString(mtrl, std::string("void main(){}")); // customShaderFragGlsl
    HAsset::Writer::appendString(mtrl, graphJson);                    // nodeGraphJson
    HAsset::Writer::appendPOD(mtrl, static_cast<uint32_t>(0));        // shaderParamData count (u32 + floats — NOT appendVec)
    HAsset::Writer::appendVec(mtrl, std::vector<std::string>{});      // graphTexturePaths
    HAsset::Writer::appendVec(mtrl, std::vector<std::string>{});      // graphParamNames
    HAsset::Writer::appendVec(mtrl, std::vector<uint8_t>{});          // graphParamTypes
    HAsset::Writer::appendVec(mtrl, std::vector<float>{});            // minmax
    HAsset::Writer::appendVec(mtrl, std::vector<std::string>{});      // groups
    HAsset::Writer::appendVec(mtrl, std::vector<std::string>{});      // tooltips
    HAsset::Writer::appendString(mtrl, parentPath);                   // parentMaterialPath
    HAsset::Writer::appendVec(mtrl, std::vector<std::string>{});      // overridden params
    HAsset::Writer::appendVec(mtrl, switchNames);                     // switch names
    HAsset::Writer::appendVec(mtrl, switchValues);                    // switch values
    HAsset::Writer::appendPOD(mtrl, static_cast<uint8_t>(0));         // blend mode
    HAsset::Writer::appendString(mtrl, std::string{});                // customShaderVertGlsl
    HAsset::Writer::appendVec(mtrl, std::vector<std::string>{});      // graphLayerNames
    HAsset::Writer::appendString(mtrl, std::string{});                // customShaderGBufGlsl
    // deliberately NO approx tail — the pre-feature on-disk layout

    HAsset::Writer w;
    w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
    w.addChunk(HAsset::CHUNK_MTRL, mtrl.data(), mtrl.size());
    return w.toBytes(static_cast<uint16_t>(HE::AssetType::Material));
}

TEST_CASE("Pack-time approx re-fold: pre-approx graph material gains fresh GI-hit colours")
{
    // Graph: ConstColor(0.1, 0.8, 0.2) → BaseColor, ConstColor(2, 0, 0) → Emissive.
    HE::MaterialGraph g = HE::MaterialGraph::makeDefault();
    for (auto& n : g.nodes)
        if (n.type == HE::MatNodeType::ConstColor)
        { n.p[0] = 0.1f; n.p[1] = 0.8f; n.p[2] = 0.2f; }
    int outId = 0;
    for (auto& n : g.nodes)
        if (n.type == HE::MatNodeType::Output) { outId = n.id; break; }
    const int em = g.addNode(HE::MatNodeType::ConstColor);
    HE::MatGraphNode* emn = g.findNode(em);
    REQUIRE(emn != nullptr);
    emn->p[0] = 2.0f; emn->p[1] = 0.0f; emn->p[2] = 0.0f;
    REQUIRE(g.connect(em, 0, outId, HE::kMatOutputEmissivePin));

    const auto dir = fs::temp_directory_path() / "he_approx_refold";
    he_test::removeAllQuiet(dir);
    const HE::UUID id{ 0xC0FFEE, 0xC0DE };
    writeBlob(dir / "m.hasset",
              preApproxGraphMaterial(id, "m.hasset", HE::materialGraphToJson(g)));

    HpakWriter packer;
    Hpak::PackSettings s;
    s.codec = Hpak::Codec::Store;
    REQUIRE(packer.addDirectory(dir, s, nullptr) == 1);
    const auto pak = (dir / "out.hpak").string();
    REQUIRE(packer.write(pak));

    HpakReader reader;
    REQUIRE(reader.open(pak));
    const std::vector<uint8_t> blob = reader.readEntry(id);
    REQUIRE(!blob.empty());

    // Walk the packed MTRL to the approx tail (same field order the runtime reads).
    HAsset::Reader r;
    REQUIRE(r.openData(blob));
    const auto* c = r.findChunk(HAsset::CHUNK_MTRL);
    REQUIRE(c != nullptr);
    size_t o = 0;
    { std::string str; std::vector<std::string> vs; std::vector<float> vf;
      std::vector<uint8_t> vb; float f; uint8_t b8;
      HAsset::Reader::readString(c->data, o, str);   // shaderPath (dropped → empty)
      HAsset::Reader::readVec(c->data, o, vs);       // texturePaths
      for (int i = 0; i < 6; ++i) HAsset::Reader::readPOD(c->data, o, f); // PBR
      HAsset::Reader::readString(c->data, o, str);   // customShaderFragGlsl
      HAsset::Reader::readString(c->data, o, str);   // nodeGraphJson
      { uint32_t n = 0; HAsset::Reader::readPOD(c->data, o, n); // shaderParamData (u32 count)
        for (uint32_t i = 0; i < n; ++i) HAsset::Reader::readPOD(c->data, o, f); }
      HAsset::Reader::readVec(c->data, o, vs);       // graphTexturePaths
      HAsset::Reader::readVec(c->data, o, vs);       // graphParamNames
      HAsset::Reader::readVec(c->data, o, vb);       // graphParamTypes
      HAsset::Reader::readVec(c->data, o, vf);       // minmax
      HAsset::Reader::readVec(c->data, o, vs);       // groups
      HAsset::Reader::readVec(c->data, o, vs);       // tooltips
      HAsset::Reader::readString(c->data, o, str);   // parentMaterialPath
      HAsset::Reader::readVec(c->data, o, vs);       // overridden
      HAsset::Reader::readVec(c->data, o, vs);       // switch names
      HAsset::Reader::readVec(c->data, o, vb);       // switch values
      HAsset::Reader::readPOD(c->data, o, b8);       // blend mode
      HAsset::Reader::readString(c->data, o, str);   // customShaderVertGlsl
      HAsset::Reader::readVec(c->data, o, vs);       // graphLayerNames
      HAsset::Reader::readString(c->data, o, str);   // customShaderGBufGlsl
    }
    float bc[3] = {}, emv[3] = {}; int32_t slotB = 0, slotE = 0; float met = -1.f, rgh = -1.f;
    for (int k = 0; k < 3; ++k) REQUIRE(HAsset::Reader::readPOD(c->data, o, bc[k]));
    for (int k = 0; k < 3; ++k) REQUIRE(HAsset::Reader::readPOD(c->data, o, emv[k]));
    REQUIRE(HAsset::Reader::readPOD(c->data, o, slotB));
    REQUIRE(HAsset::Reader::readPOD(c->data, o, slotE));
    REQUIRE(HAsset::Reader::readPOD(c->data, o, met));
    REQUIRE(HAsset::Reader::readPOD(c->data, o, rgh));
    CHECK(bc[0]  == doctest::Approx(0.1f)); // the fold, not the white default
    CHECK(bc[1]  == doctest::Approx(0.8f));
    CHECK(bc[2]  == doctest::Approx(0.2f));
    CHECK(emv[0] == doctest::Approx(2.0f));
    CHECK(emv[1] == doctest::Approx(0.0f));
    CHECK(slotB == -1);
    CHECK(slotE == -1);

    he_test::removeAllQuiet(dir);
}

TEST_CASE("Pack-time approx re-fold: graph-less INSTANCE folds from its parent chain")
{
    // Parent graph: ConstColor(0.9, 0.1, 0.7) → BaseColor.
    HE::MaterialGraph g = HE::MaterialGraph::makeDefault();
    for (auto& n : g.nodes)
        if (n.type == HE::MatNodeType::ConstColor)
        { n.p[0] = 0.9f; n.p[1] = 0.1f; n.p[2] = 0.7f; }

    const auto dir = fs::temp_directory_path() / "he_approx_inst";
    he_test::removeAllQuiet(dir);
    const HE::UUID parentId{ 0xFA11, 0x1 };
    const HE::UUID midId   { 0xFA11, 0x2 };
    const HE::UUID instId  { 0xFA11, 0x3 };
    writeBlob(dir / "parent.hasset",
              preApproxGraphMaterial(parentId, "parent.hasset", HE::materialGraphToJson(g)));
    // Instance CHAIN: inst → mid (graph-less) → parent (graph). Both instance
    // files are pre-approx and carry no graph of their own.
    writeBlob(dir / "mid.hasset",
              preApproxGraphMaterial(midId, "mid.hasset", "", "parent.hasset"));
    writeBlob(dir / "inst.hasset",
              preApproxGraphMaterial(instId, "inst.hasset", "", "mid.hasset"));

    HpakWriter packer;
    Hpak::PackSettings s;
    s.codec = Hpak::Codec::Store;
    REQUIRE(packer.addDirectory(dir, s, nullptr) == 3);
    const auto pak = (dir / "out.hpak").string();
    REQUIRE(packer.write(pak));

    HpakReader reader;
    REQUIRE(reader.open(pak));
    const std::vector<uint8_t> blob = reader.readEntry(instId);
    REQUIRE(!blob.empty());

    HAsset::Reader r;
    REQUIRE(r.openData(blob));
    const auto* c = r.findChunk(HAsset::CHUNK_MTRL);
    REQUIRE(c != nullptr);
    size_t o = 0;
    { std::string str; std::vector<std::string> vs; std::vector<float> vf;
      std::vector<uint8_t> vb; float f; uint8_t b8;
      HAsset::Reader::readString(c->data, o, str);   // shaderPath
      HAsset::Reader::readVec(c->data, o, vs);       // texturePaths
      for (int i = 0; i < 6; ++i) HAsset::Reader::readPOD(c->data, o, f); // PBR
      HAsset::Reader::readString(c->data, o, str);   // customShaderFragGlsl
      HAsset::Reader::readString(c->data, o, str);   // nodeGraphJson
      { uint32_t n = 0; HAsset::Reader::readPOD(c->data, o, n);
        for (uint32_t i = 0; i < n; ++i) HAsset::Reader::readPOD(c->data, o, f); }
      HAsset::Reader::readVec(c->data, o, vs);       // graphTexturePaths
      HAsset::Reader::readVec(c->data, o, vs);       // graphParamNames
      HAsset::Reader::readVec(c->data, o, vb);       // graphParamTypes
      HAsset::Reader::readVec(c->data, o, vf);       // minmax
      HAsset::Reader::readVec(c->data, o, vs);       // groups
      HAsset::Reader::readVec(c->data, o, vs);       // tooltips
      HAsset::Reader::readString(c->data, o, str);   // parentMaterialPath
      HAsset::Reader::readVec(c->data, o, vs);       // overridden
      HAsset::Reader::readVec(c->data, o, vs);       // switch names
      HAsset::Reader::readVec(c->data, o, vb);       // switch values
      HAsset::Reader::readPOD(c->data, o, b8);       // blend mode
      HAsset::Reader::readString(c->data, o, str);   // customShaderVertGlsl
      HAsset::Reader::readVec(c->data, o, vs);       // graphLayerNames
      HAsset::Reader::readString(c->data, o, str);   // customShaderGBufGlsl
    }
    float bc[3] = {};
    for (int k = 0; k < 3; ++k) REQUIRE(HAsset::Reader::readPOD(c->data, o, bc[k]));
    CHECK(bc[0] == doctest::Approx(0.9f)); // parent's fold reached through the chain
    CHECK(bc[1] == doctest::Approx(0.1f));
    CHECK(bc[2] == doctest::Approx(0.7f));

    he_test::removeAllQuiet(dir);
}

TEST_CASE("Pack-time approx re-fold: switch-override instance folds its own permutation")
{
    // Parent: StaticSwitch "ColorSwitch" (default OFF) — True = blue, False = red.
    HE::MaterialGraph g = HE::MaterialGraph::makeDefault();
    int outId = 0, redId = 0;
    for (auto& n : g.nodes)
    {
        if (n.type == HE::MatNodeType::Output)     outId = n.id;
        if (n.type == HE::MatNodeType::ConstColor) redId = n.id;
    }
    HE::MatGraphNode* red = g.findNode(redId);
    REQUIRE(red != nullptr);
    red->p[0] = 1.0f; red->p[1] = 0.0f; red->p[2] = 0.0f;
    g.disconnectInput(outId, HE::kMatOutputBaseColorPin);
    const int swId = g.addNode(HE::MatNodeType::StaticSwitch);
    HE::MatGraphNode* sw = g.findNode(swId);
    REQUIRE(sw != nullptr);
    sw->s = "ColorSwitch"; sw->p[0] = 0.0f;
    const int blueId = g.addNode(HE::MatNodeType::ConstColor);
    HE::MatGraphNode* blue = g.findNode(blueId);
    REQUIRE(blue != nullptr);
    blue->p[0] = 0.0f; blue->p[1] = 0.0f; blue->p[2] = 1.0f;
    REQUIRE(g.connect(blueId, 0, swId, 0));
    REQUIRE(g.connect(redId,  0, swId, 1));
    REQUIRE(g.connect(swId, 0, outId, HE::kMatOutputBaseColorPin));

    const auto dir = fs::temp_directory_path() / "he_approx_switch";
    he_test::removeAllQuiet(dir);
    const HE::UUID parentId{ 0x5117, 0x1 };
    const HE::UUID onId    { 0x5117, 0x2 };
    const HE::UUID defId   { 0x5117, 0x3 };
    writeBlob(dir / "swparent.hasset",
              preApproxGraphMaterial(parentId, "swparent.hasset", HE::materialGraphToJson(g)));
    // Instance with the switch flipped ON, and a control instance without overrides.
    writeBlob(dir / "on.hasset",
              preApproxGraphMaterial(onId, "on.hasset", "", "swparent.hasset",
                                     { "ColorSwitch" }, { 1 }));
    writeBlob(dir / "def.hasset",
              preApproxGraphMaterial(defId, "def.hasset", "", "swparent.hasset"));

    HpakWriter packer;
    Hpak::PackSettings s;
    s.codec = Hpak::Codec::Store;
    REQUIRE(packer.addDirectory(dir, s, nullptr) == 3);
    const auto pak = (dir / "out.hpak").string();
    REQUIRE(packer.write(pak));

    HpakReader reader;
    REQUIRE(reader.open(pak));

    // Skim a packed MTRL to its approx baseColor (shared field walk).
    auto approxBaseColor = [&](HE::UUID id, float rgb[3])
    {
        const std::vector<uint8_t> blob = reader.readEntry(id);
        REQUIRE(!blob.empty());
        HAsset::Reader r;
        REQUIRE(r.openData(blob));
        const auto* c = r.findChunk(HAsset::CHUNK_MTRL);
        REQUIRE(c != nullptr);
        size_t o = 0;
        std::string str; std::vector<std::string> vs; std::vector<float> vf;
        std::vector<uint8_t> vb; float f; uint8_t b8;
        HAsset::Reader::readString(c->data, o, str);   // shaderPath
        HAsset::Reader::readVec(c->data, o, vs);       // texturePaths
        for (int i = 0; i < 6; ++i) HAsset::Reader::readPOD(c->data, o, f); // PBR
        HAsset::Reader::readString(c->data, o, str);   // customShaderFragGlsl
        HAsset::Reader::readString(c->data, o, str);   // nodeGraphJson
        { uint32_t n = 0; HAsset::Reader::readPOD(c->data, o, n);
          for (uint32_t i = 0; i < n; ++i) HAsset::Reader::readPOD(c->data, o, f); }
        HAsset::Reader::readVec(c->data, o, vs);       // graphTexturePaths
        HAsset::Reader::readVec(c->data, o, vs);       // graphParamNames
        HAsset::Reader::readVec(c->data, o, vb);       // graphParamTypes
        HAsset::Reader::readVec(c->data, o, vf);       // minmax
        HAsset::Reader::readVec(c->data, o, vs);       // groups
        HAsset::Reader::readVec(c->data, o, vs);       // tooltips
        HAsset::Reader::readString(c->data, o, str);   // parentMaterialPath
        HAsset::Reader::readVec(c->data, o, vs);       // overridden
        HAsset::Reader::readVec(c->data, o, vs);       // switch names
        HAsset::Reader::readVec(c->data, o, vb);       // switch values
        HAsset::Reader::readPOD(c->data, o, b8);       // blend mode
        HAsset::Reader::readString(c->data, o, str);   // customShaderVertGlsl
        HAsset::Reader::readVec(c->data, o, vs);       // graphLayerNames
        HAsset::Reader::readString(c->data, o, str);   // customShaderGBufGlsl
        for (int k = 0; k < 3; ++k) REQUIRE(HAsset::Reader::readPOD(c->data, o, rgb[k]));
    };

    float onRgb[3] = {}, defRgb[3] = {};
    approxBaseColor(onId, onRgb);
    approxBaseColor(defId, defRgb);
    CHECK(onRgb[0]  == doctest::Approx(0.0f)); // ON permutation → blue branch
    CHECK(onRgb[2]  == doctest::Approx(1.0f));
    CHECK(defRgb[0] == doctest::Approx(1.0f)); // no override → parent default (red)
    CHECK(defRgb[2] == doctest::Approx(0.0f));

    he_test::removeAllQuiet(dir);
}

// ─── Type index + hcfg v3: the seam that ships user types + save templates ────

// Like tinyHasset, but with an arbitrary asset type and one JSON chunk.
static std::vector<uint8_t> tinyJsonAsset(HE::UUID id, const std::string& relPath,
                                          HE::AssetType type, uint32_t chunkId,
                                          const std::string& json)
{
    std::vector<uint8_t> meta;
    HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(type));
    HAsset::Writer::appendPOD(meta, id.hi);
    HAsset::Writer::appendPOD(meta, id.lo);
    HAsset::Writer::appendString(meta, fs::path(relPath).stem().string());
    HAsset::Writer::appendString(meta, relPath);
    HAsset::Writer w;
    w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
    w.addChunk(chunkId, json.data(), json.size());
    return w.toBytes(static_cast<uint16_t>(type));
}

TEST_CASE("Export bakes a __type_index__ naming every struct/enum/template asset")
{
    const auto dir = fs::temp_directory_path() / "he_typeidx_export";
    const auto out = fs::temp_directory_path() / "he_typeidx_export_out";
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(out);
    writeBlob(dir / "mesh.hasset", tinyHasset({0x1, 0x1}, "mesh.hasset"));
    writeBlob(dir / "Types" / "Weapon.hasset",
              tinyJsonAsset({0x2, 0x2}, "Types/Weapon.hasset", HE::AssetType::EnumType,
                            HAsset::CHUNK_ENDF, "{\"entries\":[{\"name\":\"Bow\",\"value\":7}]}"));
    writeBlob(dir / "MainSave.hasset",
              tinyJsonAsset({0x3, 0x3}, "MainSave.hasset", HE::AssetType::SaveGameTemplate,
                            HAsset::CHUNK_SGTP, "{\"fields\":[]}"));

    ExportSettings settings;
    settings.compress = false;
    const auto res = ProjectExporter::exportProject(dir, "TypeIdx", "", out, settings);
    REQUIRE(res.success);

    HpakReader reader;
    REQUIRE(reader.open((out / "TypeIdx.hpak").string()));
    const auto bytes = reader.readEntry(sceneUuidForPath(kTypeIndexEntry));
    REQUIRE(!bytes.empty());
    const auto idx = nlohmann::json::parse(bytes.begin(), bytes.end(),
                                           nullptr, /*allow_exceptions=*/false);
    REQUIRE(idx.is_array());
    std::vector<std::string> paths;
    for (const auto& e : idx) if (e.is_string()) paths.push_back(e.get<std::string>());
    std::sort(paths.begin(), paths.end());
    CHECK(paths == std::vector<std::string>({ "MainSave.hasset", "Types/Weapon.hasset" }));
    // Ordinary assets stay OUT of the index (it drives eager loads).
    CHECK(std::find(paths.begin(), paths.end(), "mesh.hasset") == paths.end());
    he_test::removeAllQuiet(dir); he_test::removeAllQuiet(out);
}

TEST_CASE("project.hcfg: defaultSaveTemplate round-trips as v3, empty stays v2-compatible")
{
    const auto dir = fs::temp_directory_path() / "he_hcfg_v3";
    he_test::removeAllQuiet(dir);
    fs::create_directories(dir);

    // With a template: v3 tail round-trips.
    ProjectConfig cfg;
    cfg.projectName = "Cfg";
    cfg.hpakFilename = "Cfg.hpak";
    cfg.defaultSaveTemplate = "Content/MainSave.hasset";
    REQUIRE(ProjectConfigLoader::save(dir, cfg));
    ProjectConfig back;
    REQUIRE(ProjectConfigLoader::load(dir, back));
    CHECK(back.defaultSaveTemplate == "Content/MainSave.hasset");
    CHECK(back.projectName == "Cfg");

    // Without a template the file is emitted as PLAIN v2 — a stale prebuilt
    // runtime bundle (which rejects unknown versions and would boot pak-less)
    // keeps reading it. Byte check: the version word after the 4-byte magic.
    cfg.defaultSaveTemplate.clear();
    REQUIRE(ProjectConfigLoader::save(dir, cfg));
    {
        std::ifstream f(dir / "project.hcfg", std::ios::binary);
        REQUIRE(f.is_open());
        char magic[4]; uint16_t version = 0;
        f.read(magic, 4);
        f.read(reinterpret_cast<char*>(&version), 2);
        CHECK(version == 2);
    }
    ProjectConfig back2;
    REQUIRE(ProjectConfigLoader::load(dir, back2));
    CHECK(back2.defaultSaveTemplate.empty());
    he_test::removeAllQuiet(dir);
}

// The version word after the 4-byte magic of an exported project.hcfg.
static uint16_t hcfgVersion(const fs::path& dir)
{
    std::ifstream f(dir / "project.hcfg", std::ios::binary);
    REQUIRE(f.is_open());
    char magic[4]; uint16_t version = 0;
    f.read(magic, 4);
    f.read(reinterpret_cast<char*>(&version), 2);
    return version;
}

TEST_CASE("Export: a game keeps its project.hcfg readable by an older runtime bundle")
{
    const auto dir = fs::temp_directory_path() / "he_bundleid_src";
    he_test::removeAllQuiet(dir);
    writeBlob(dir / "a.hasset", tinyHasset({0x11, 0x22}, "a.hasset"));

    const auto run = [&](const fs::path& out, const std::string& bundleId, bool appProject) {
        he_test::removeAllQuiet(out);
        ExportSettings s;
        s.bundleId   = bundleId;
        s.appProject = appProject;
        return ProjectExporter::exportProject(dir, "Inc", "", out, s);
    };

    // A plain game: the id the exporter would derive is the id it has, so the
    // field stays empty and the file is the v2 every runtime can read.
    // BEFORE THE FIX: the exporter filled bundleId unconditionally, the writer
    // chose v5, and a prebuilt runtime under GameRuntimes/<Platform>/ rejected
    // it and booted without its pak.
    const auto out1 = fs::temp_directory_path() / "he_bundleid_plain";
    REQUIRE(run(out1, "", false).success);
    CHECK(hcfgVersion(out1) == 2);
    ProjectConfig plain;
    REQUIRE(ProjectConfigLoader::load(out1, plain));
    CHECK(plain.bundleId.empty());

    // The same for a game that TYPED the derived id: it says nothing the
    // runtime could not work out, so it costs nothing to leave out.
    const auto out2 = fs::temp_directory_path() / "he_bundleid_same";
    REQUIRE(run(out2, "com.horizonengine.inc", false).success);
    CHECK(hcfgVersion(out2) == 2);

    // A game with an id of its own has to say it, and pays the version for it.
    const auto out3 = fs::temp_directory_path() / "he_bundleid_custom";
    REQUIRE(run(out3, "dev.horizoncreations.inc", false).success);
    CHECK(hcfgVersion(out3) == 5);
    ProjectConfig custom;
    REQUIRE(ProjectConfigLoader::load(out3, custom));
    CHECK(custom.bundleId == "dev.horizoncreations.inc");

    // An application always says it, derived or not: the autostart entry and
    // the document types are filed under it, and no older runtime exists for
    // applications to stay compatible with.
    const auto out4 = fs::temp_directory_path() / "he_bundleid_app";
    REQUIRE(run(out4, "", true).success);
    CHECK(hcfgVersion(out4) == 5);
    ProjectConfig app;
    REQUIRE(ProjectConfigLoader::load(out4, app));
    CHECK(app.bundleId == "com.horizonengine.inc");
    CHECK(app.appMode);

    he_test::removeAllQuiet(dir);
    for (const auto& o : { out1, out2, out3, out4 }) he_test::removeAllQuiet(o);
}

// ─── What a project inherits that it never asked for ─────────────────────────
// Two defaults from the application work reached game projects as well, and the
// merge analysis (6.1 #7 and #8) says they should not: a new GAME still draws
// bold body text, and a project written before the icon field existed still
// exports without an icon instead of acquiring a generated one.

TEST_CASE("A new game keeps bold body text, a new application does not")
{
    const auto dir = std::filesystem::temp_directory_path() / "he_test_weightproj";
    he_test::removeAllQuiet(dir);

    ProjectManager pm;
    REQUIRE(pm.createNewProject(dir.string(), "WeightGame", ProjectPreset::Empty));
    // BEFORE THE FIX: false — every new project was written regular, so a game
    // author's next project looked different from every one already on disk.
    // Checked twice on purpose: what the editor is holding, and what it wrote.
    CHECK(pm.currentProject().fontWeightBold);
    {
        ProjectManager reopened;
        REQUIRE(reopened.loadProject(pm.currentProject().path));
        CHECK(reopened.currentProject().fontWeightBold);
    }

    const auto appDir = std::filesystem::temp_directory_path() / "he_test_weightapp";
    he_test::removeAllQuiet(appDir);
    ProjectManager app;
    REQUIRE(app.createNewProject(appDir.string(), "WeightApp", ProjectPreset::Application,
                                 ProjectScriptLanguage::HorizonCode, /*appProject=*/true));
    CHECK_FALSE(app.currentProject().fontWeightBold);
    {
        ProjectManager reopened;
        REQUIRE(reopened.loadProject(app.currentProject().path));
        CHECK_FALSE(reopened.currentProject().fontWeightBold);
    }

    he_test::removeAllQuiet(dir);
    he_test::removeAllQuiet(appDir);
}

TEST_CASE("A project written before appIconName loads with no icon, not with one")
{
    const auto dir = std::filesystem::temp_directory_path() / "he_test_iconproj";
    he_test::removeAllQuiet(dir);

    ProjectManager pm;
    REQUIRE(pm.createNewProject(dir.string(), "OldGame", ProjectPreset::Empty));
    const std::string heproj = pm.currentProject().path;

    // Take the key back out — this is what every .heproj written before the
    // field existed looks like.
    {
        std::ifstream in(heproj);
        nlohmann::json j = nlohmann::json::parse(in);
        in.close();
        j.erase("appIconName");
        std::ofstream out(heproj, std::ios::trunc);
        out << j.dump(4);
    }
    ProjectManager reopened;
    REQUIRE(reopened.loadProject(heproj));
    // BEFORE THE FIX: "widgets" — and the exporter draws an icon for any name
    // that is not empty, so the next export of an existing game acquired a
    // generated plate nobody had chosen.
    CHECK(reopened.currentProject().appIconName.empty());

    he_test::removeAllQuiet(dir);
}

// ─── Application template ────────────────────────────────────────────────────
// An app project that opens with an empty preview is indistinguishable from a
// broken one, so the template has to lay down BOTH halves: the root widget, and
// the GameInstance that creates it. Checked together, because either alone
// still leaves a black window.

TEST_CASE("Application template ships a root widget and a GameInstance that creates it")
{
    const auto dir = std::filesystem::temp_directory_path() / "he_test_appproj";
    he_test::removeAllQuiet(dir);

    ProjectManager pm;
    REQUIRE(pm.createNewProject(dir.string(), "AppProj", ProjectPreset::Application));

    // The manifest says what it is, and has no scene to start in.
    CHECK(pm.currentProject().appProject);
    CHECK(pm.currentProject().startupScene.empty());

    // The root widget is a real .hasset: readable, typed, and carrying a tree
    // with something on it.
    const auto widgetPath = dir / "Content" / "UI" / "RootWidget.hasset";
    REQUIRE(std::filesystem::exists(widgetPath));
    {
        HAsset::Reader r;
        REQUIRE(r.open(widgetPath.string()));
        const auto* tree = r.findChunk(HAsset::CHUNK_UIWT);
        REQUIRE(tree != nullptr);
        HE::UIWidgetTree parsed;
        REQUIRE(HE::uiWidgetTreeFromJson(
            std::string(reinterpret_cast<const char*>(tree->data.data()), tree->data.size()),
            parsed));
        CHECK(parsed.elements.size() >= 2);   // a panel and a label on it
    }

    // …and the GameInstance is a parseable graph whose OnInit reaches a Create
    // Widget pointing at exactly that path. A graph with the two nodes but no
    // link between them would draw nothing, so the LINK is the assertion.
    const auto gi = dir / "GameInstance.hcode";
    REQUIRE(std::filesystem::exists(gi));
    {
        std::ifstream f(gi);
        const std::string text((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        HorizonCode::Graph g;
        REQUIRE(HorizonCode::fromJson(text, g));

        int evId = 0, createId = 0;
        for (const auto& n : g.nodes)
        {
            if (n.type == HorizonCode::NodeType::Event && n.s == "OnInit") evId = n.id;
            if (n.type == HorizonCode::NodeType::CreateWidget)
            {
                createId = n.id;
                CHECK(n.s == "UI/RootWidget.hasset");
            }
        }
        REQUIRE(evId != 0);
        REQUIRE(createId != 0);
        bool linked = false;
        for (const auto& l : g.links)
            if (l.srcNode == evId && l.dstNode == createId) linked = true;
        CHECK(linked);
    }

    he_test::removeAllQuiet(dir);
}
