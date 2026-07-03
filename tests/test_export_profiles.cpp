#include "doctest.h"
#include <Hpak/HpakFormat.h>
#include <Hpak/HpakWriter.h>
#include <Hpak/HpakReader.h>
#include <Hpak/ProjectExporter.h>
#include <Hpak/ProjectConfig.h>
#include <ContentManager/HAsset.h>
#include <Types/Enums.h>
#include <Types/UUID.h>
#include "ProjectManager.h"
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
    proj.exportProfiles[1].incremental     = false;
    proj.exportProfiles[1].targetPlatform  = "Windows";
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
    fs::remove_all(dir);
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
    fs::remove_all(dir); fs::remove_all(out);
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
    fs::remove(out / "Inc.hpak.manifest");
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

    fs::remove_all(dir); fs::remove_all(out);
}

#ifdef HE_HAVE_OPENSSL
TEST_CASE("Incremental export: encryption reuses the previous key, pak stays readable")
{
    const auto dir = fs::temp_directory_path() / "he_inc_enc_src";
    const auto out = fs::temp_directory_path() / "he_inc_enc_out";
    fs::remove_all(dir); fs::remove_all(out);
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

    fs::remove_all(dir); fs::remove_all(out);
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
    fs::remove_all(dir);
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
    fs::remove_all(dir);
}

#ifdef HE_HAVE_OPENSSL
TEST_CASE("Export with encryption embeds the key in the game binary, not the hcfg")
{
    const auto dir = fs::temp_directory_path() / "he_embed_src";
    const auto rt  = fs::temp_directory_path() / "he_embed_rt";
    const auto out = fs::temp_directory_path() / "he_embed_out";
    fs::remove_all(dir); fs::remove_all(rt); fs::remove_all(out);
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

    fs::remove_all(dir); fs::remove_all(rt); fs::remove_all(out);
}

TEST_CASE("Export with encryption falls back to the hcfg key for a legacy runtime")
{
    const auto dir = fs::temp_directory_path() / "he_embed_legacy_src";
    const auto rt  = fs::temp_directory_path() / "he_embed_legacy_rt";
    const auto out = fs::temp_directory_path() / "he_embed_legacy_out";
    fs::remove_all(dir); fs::remove_all(rt); fs::remove_all(out);
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

    fs::remove_all(dir); fs::remove_all(rt); fs::remove_all(out);
}
#endif

TEST_CASE("Export fails when the runtime dir yields no binaries")
{
    const auto dir = fs::temp_directory_path() / "he_nobin_src";
    const auto rt  = fs::temp_directory_path() / "he_nobin_rt";   // exists, empty
    const auto out = fs::temp_directory_path() / "he_nobin_out";
    fs::remove_all(dir); fs::remove_all(rt); fs::remove_all(out);
    writeBlob(dir / "a.hasset", tinyHasset({0xE, 0x3}, "a.hasset"));
    fs::create_directories(rt);

    ExportSettings s;
    s.compress = false;
    s.gameRuntimeDir = rt;
    auto r = ProjectExporter::exportProject(dir, "NoBin", "", out, s);
    CHECK_FALSE(r.success);
    CHECK(r.errorMessage.find("no files") != std::string::npos);

    fs::remove_all(dir); fs::remove_all(rt); fs::remove_all(out);
}

TEST_CASE("findRuntimeBundle: deploy layout, build-tree layout, cross-platform")
{
    const auto root = fs::temp_directory_path() / "he_bundle_root";
    fs::remove_all(root);

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

    fs::remove_all(root);
}

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
    fs::remove_all(dir);
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
    fs::remove_all(dir);
}

TEST_CASE("ProjectManager: saveProject refuses to clobber a corrupt manifest")
{
    const auto dir = fs::temp_directory_path() / "he_prof_corrupt";
    fs::remove_all(dir);
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
    fs::remove_all(dir);
}

TEST_CASE("HpakWriter: unreadable subdirectory does not throw out of addDirectory")
{
    const auto dir = fs::temp_directory_path() / "he_excl_denied";
    fs::remove_all(dir);
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
    fs::remove_all(dir);
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
