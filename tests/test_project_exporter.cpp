#include "doctest.h"
#include "TestFsUtil.h"
#include <Hpak/ProjectConfig.h>
#include <Hpak/ProjectExporter.h>
#include <Hpak/HpakReader.h>
#include <Hpak/HpakWriter.h>
#include <ContentManager/HAsset.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Types/UUID.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
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

// A minimal .hasset for any of the JSON-payload asset types: META (uint16 type,
// hi, lo, name, path) plus the single chunk the loader reads for that type. The
// HEADER type handed to toBytes is the one HpakWriter records in packedTypes()
// and the one discoverAssets compares against, so it has to be the real thing.
static std::vector<uint8_t> makeJsonAssetBlob(HE::AssetType type, uint32_t chunkId,
                                              const HE::UUID& id, const std::string& name,
                                              const std::string& json)
{
    std::vector<uint8_t> meta;
    const uint16_t typeVal = static_cast<uint16_t>(type);
    HAsset::Writer::appendPOD(meta, typeVal);
    HAsset::Writer::appendPOD(meta, id.hi);
    HAsset::Writer::appendPOD(meta, id.lo);
    HAsset::Writer::appendString(meta, name);
    HAsset::Writer::appendString(meta, "mem://" + name);

    HAsset::Writer w;
    w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
    w.addChunk(chunkId, json.data(), json.size());
    return w.toBytes(typeVal);
}

static bool listed(const std::vector<HE::UUID>& ids, const HE::UUID& id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

static void writeBlob(const std::filesystem::path& p, const std::vector<uint8_t>& b)
{
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), b.size());
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

    he_test::removeAllQuiet(tmpDir);
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

    he_test::removeAllQuiet(tmpDir);
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

    he_test::removeAllQuiet(tmpDir);
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

    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
}

TEST_CASE("Lazy-mounted pak resolves loadAsset by content path via the asset index")
{
    // The regression this guards: a shipped game LAZY-mounts its pak (mountPak
    // registers UUID→mount residency only). An asset the scene's UUID reference
    // closure never reaches — the classic case being a widget a HorizonCode
    // script creates by PATH — must still resolve through loadAsset("<path>").
    // Before the __asset_index__ entry, that path lookup fell through to a disk
    // read that fails in a pak-only build, so the widget's UI silently vanished.
    auto contentDir = std::filesystem::temp_directory_path() / "he_test_pathidx_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_test_pathidx_out";
    std::filesystem::create_directories(contentDir / "UI");

    // Full 64-bit hi/lo so the "hi:lo" index round-trip is exercised past 2^53.
    const HE::UUID matId{0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL};
    const auto blob = makeMinimalMaterialBlob(matId, "menu_mat");
    { std::ofstream f(contentDir / "UI" / "menu_mat.hasset", std::ios::binary);
      f.write(reinterpret_cast<const char*>(blob.data()), blob.size()); }

    ExportSettings settings;
    settings.compress = false;
    const auto result = ProjectExporter::exportProject(
        contentDir, "PathIdxGame", "", outputDir, settings);
    REQUIRE(result.success);

    const auto pakPath = outputDir / "PathIdxGame.hpak";
    REQUIRE(std::filesystem::exists(pakPath));

    ContentManager cm;
    REQUIRE(cm.mountPak(pakPath.string()));      // lazy, like the game — NOT loadPak
    CHECK(cm.getMaterial(matId) == nullptr);     // nothing streamed/registered yet

    // Resolve by the content-relative path the editor + HorizonCode store.
    const HE::UUID resolved = cm.loadAsset("UI/menu_mat.hasset");
    CHECK(resolved == matId);
    CHECK(cm.getMaterial(matId) != nullptr);
    // Second lookup hits the path cache and returns the same id.
    CHECK(cm.loadAsset("UI/menu_mat.hasset") == matId);
    // An unknown path still fails cleanly (no index entry, no loose file).
    CHECK(cm.loadAsset("UI/nope.hasset") == HE::UUID{});

    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
}

// The three asset types PlayerHost::begin looks for at startup, with the chunk
// each one's loader reads. Shared by the B5 tests below.
static const HE::UUID kB5ClassId {0x1122334455667788ULL, 0x00FFEEDDCCBBAA99ULL};
static const HE::UUID kB5ActionId{0x00000000DEADBEEFULL, 0xFEEDFACE00000001ULL};
static const HE::UUID kB5MapId   {0x7FFFFFFFFFFFFFFFULL, 0x8000000000000001ULL};

static void writeB5StartupAssets(const std::filesystem::path& contentDir)
{
    std::filesystem::create_directories(contentDir / "Input");
    writeBlob(contentDir / "PlayerController.hasset",
              makeJsonAssetBlob(HE::AssetType::HorizonCodeClass, HAsset::CHUNK_HCGR,
                                kB5ClassId, "PlayerController", R"({"nodes":[]})"));
    writeBlob(contentDir / "Input" / "IA_Jump.hasset",
              makeJsonAssetBlob(HE::AssetType::InputAction, HAsset::CHUNK_IACT,
                                kB5ActionId, "IA_Jump", R"({"valueType":"bool"})"));
    writeBlob(contentDir / "Input" / "IMC_Default.hasset",
              makeJsonAssetBlob(HE::AssetType::InputMappingContext, HAsset::CHUNK_IMAP,
                                kB5MapId, "IMC_Default", R"({"mappings":[]})"));
}

TEST_CASE("Packaged game discovers its startup assets from the pak's asset type index")
{
    // BLOCKER B5, end to end. A shipped game LAZY-mounts its pak, and mounting
    // used to contribute residency (UUID→mount) and paths only — no types. The
    // ContentManager learned an asset's type solely by LOADING it, so in a
    // packaged build both halves of discoverAssets came back empty: the type
    // index knew nothing, and the loose-content walk had no content directory to
    // walk. PlayerHost::begin therefore found no HorizonCodeClass to instantiate
    // as a PlayerController, no Construct and no BeginPlay ran, and the game
    // booted into a world with the scenery but no player. Everything worked in
    // the editor, where the content walk carries the whole thing.
    auto contentDir = std::filesystem::temp_directory_path() / "he_b5_types_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_b5_types_out";
    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
    writeB5StartupAssets(contentDir);

    ExportSettings settings;
    settings.compress = true;
    const auto result = ProjectExporter::exportProject(
        contentDir, "B5Game", "", outputDir, settings);
    REQUIRE(result.success);
    CHECK(result.assetsPacked == 3);

    // ── The game-runtime sequence: hcfg → mountPak, nothing else ──
    ProjectConfig cfg;
    REQUIRE(ProjectConfigLoader::load(outputDir, cfg));
    const uint8_t* key = cfg.encrypted ? cfg.encKey : nullptr;

    ContentManager cm;                       // fresh, like GameApplication's
    REQUIRE(cm.contentRoot().empty());       // a shipped game has NO content dir
    REQUIRE(cm.mountPak((outputDir / cfg.hpakFilename).string(), key));

    // Nothing is loaded yet — this is the state PlayerHost::begin asks in.
    CHECK(cm.getHorizonCodeClass(kB5ClassId)    == nullptr);
    CHECK(cm.getInputAction(kB5ActionId)        == nullptr);
    CHECK(cm.getInputMappingContext(kB5MapId)   == nullptr);

    // 1) Discovery must NAME the assets (the type index half) …
    const auto classes = cm.discoverAssets(HE::AssetType::HorizonCodeClass);
    const auto actions = cm.discoverAssets(HE::AssetType::InputAction);
    const auto maps    = cm.discoverAssets(HE::AssetType::InputMappingContext);
    CHECK(listed(classes, kB5ClassId));
    CHECK(listed(actions, kB5ActionId));
    CHECK(listed(maps,    kB5MapId));

    // 2) … and the ids it names must be REDEEMABLE. An index that hands back
    // UUIDs whose accessors all read null has moved B5, not fixed it: PlayerHost
    // would still have no class body to instantiate.
    const auto* cls = cm.getHorizonCodeClass(kB5ClassId);
    REQUIRE(cls != nullptr);
    CHECK(cls->name == "PlayerController");
    CHECK(cls->graphJson == R"({"nodes":[]})");
    const auto* act = cm.getInputAction(kB5ActionId);
    REQUIRE(act != nullptr);
    CHECK(act->name == "IA_Jump");
    const auto* imc = cm.getInputMappingContext(kB5MapId);
    REQUIRE(imc != nullptr);
    CHECK(imc->name == "IMC_Default");

    // The type is answerable without a load having preceded it either way.
    CHECK(cm.assetType(kB5ClassId) == HE::AssetType::HorizonCodeClass);

    // A type nothing in the pak has stays empty — the index is not a wildcard.
    CHECK(!listed(cm.discoverAssets(HE::AssetType::AnimatorStateMachine), kB5ClassId));

    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
}

TEST_CASE("A pak without an asset type index mounts cleanly and contributes no types")
{
    // Best effort, exactly like the path index next door: every pak built before
    // __asset_types__ existed — and every mod pak a HpakWriter user assembles by
    // hand — has no such entry. Mounting one must succeed and simply teach the
    // manager nothing, never fail the mount or throw on the missing entry.
    // Built through HpakWriter directly because the exporter always writes the
    // entry now; there is no way to un-write it.
    const HE::UUID oldId{0x0BD0, 0x1};
    HpakWriter p;
    p.addEntry(oldId, makeMinimalMaterialBlob(oldId, "legacy_mat"), {Hpak::Codec::Store});
    const auto pak = std::filesystem::temp_directory_path() / "he_b5_legacy.hpak";
    he_test::removeQuiet(pak);
    REQUIRE(p.write(pak.string()));

    ContentManager cm;
    REQUIRE(cm.mountPak(pak.string()));                  // no entry → still mounts
    CHECK(!listed(cm.discoverAssets(HE::AssetType::Material), oldId));
    CHECK(cm.assetType(oldId) == HE::AssetType::Unknown);
    // The asset itself is still reachable the old way — only the TYPE is unknown,
    // so this is a narrower manager, not a broken one.
    CHECK(cm.ensureResident(oldId));
    CHECK(cm.getMaterial(oldId) != nullptr);

    he_test::removeQuiet(pak);
}

TEST_CASE("Unloading a pak asset does not take the pak's type information with it")
{
    // The type index has two origins, and unloadAsset used to erase both. A pak
    // asset's type belongs to the ARCHIVE, not to the load: drop it on unload and
    // discoverAssets stops listing the asset, so nothing ever asks for it to come
    // back — undiscoverable for the rest of the process. Unload → discover →
    // reload has to be a loop, because that is how a game frees a level's classes
    // and picks them up again.
    auto contentDir = std::filesystem::temp_directory_path() / "he_b5_unload_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_b5_unload_out";
    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
    writeB5StartupAssets(contentDir);

    ExportSettings settings;
    settings.compress = false;
    REQUIRE(ProjectExporter::exportProject(contentDir, "UnloadGame", "", outputDir, settings).success);

    ContentManager cm;
    REQUIRE(cm.mountPak((outputDir / "UnloadGame.hpak").string()));
    REQUIRE(listed(cm.discoverAssets(HE::AssetType::HorizonCodeClass), kB5ClassId));
    REQUIRE(cm.getHorizonCodeClass(kB5ClassId) != nullptr);

    REQUIRE(cm.unloadAsset(kB5ClassId));
    CHECK(cm.getHorizonCodeClass(kB5ClassId) == nullptr);   // the payload really went
    CHECK(!cm.isLoaded(kB5ClassId));
    CHECK(cm.assetType(kB5ClassId) == HE::AssetType::HorizonCodeClass); // the type stayed

    // Second time round: still found, and still redeemable.
    CHECK(listed(cm.discoverAssets(HE::AssetType::HorizonCodeClass), kB5ClassId));
    CHECK(cm.getHorizonCodeClass(kB5ClassId) != nullptr);

    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
}

TEST_CASE("ProjectExporter ships the engine default content under Engine/")
{
    // Regression: the engine's default primitives live next to the EDITOR
    // (EditorDeps/EngineContent), never inside a project — and the exporter only
    // ever packed the project's Content. Every scene reference to a built-in mesh
    // therefore dangled in a shipped game, and the renderers' missing-mesh
    // fallback drew the default cube: sphere, capsule, cone, plane — all cubes.
    auto contentDir = std::filesystem::temp_directory_path() / "he_test_engc_content";
    auto engineDir  = std::filesystem::temp_directory_path() / "he_test_engc_engine";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_test_engc_out";
    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(engineDir);
    he_test::removeAllQuiet(outputDir);
    std::filesystem::create_directories(contentDir);

    // Two engine default meshes, written exactly the way mesh_gen writes them:
    // the Meshes folder IS the content root there, so each META path is the bare
    // "<Name>.hasset" — NOT the "Engine/Meshes/…" the editor addresses them by.
    const HE::UUID cubeId  {0x0000000000000100ULL, 0x0000000000000001ULL};
    const HE::UUID sphereId{0x0000000000000101ULL, 0x0000000000000001ULL};
    {
        ContentManager gen((engineDir / "Meshes").string());
        for (const auto& [name, id] : std::vector<std::pair<std::string, HE::UUID>>{
                 {"Cube", cubeId}, {"Sphere", sphereId} })
        {
            StaticMeshAsset m;
            m.type     = HE::AssetType::StaticMesh;
            m.id       = id;
            m.name     = name;
            m.path     = name + ".hasset";
            m.vertices = { 0,0,0,  1,0,0,  0,1,0 };
            m.normals  = { 0,0,1,  0,0,1,  0,0,1 };
            m.uvs      = { 0,0,  1,0,  0,1 };
            m.indices  = { 0,1,2 };
            REQUIRE(gen.saveAsset(m));
        }
    }

    // One ordinary project asset, so this isn't an engine-only pack.
    const HE::UUID matId{0xAABB, 0xCCDD};
    const auto matBlob = makeMinimalMaterialBlob(matId, "mat");
    { std::ofstream f(contentDir / "mat.hasset", std::ios::binary);
      f.write(reinterpret_cast<const char*>(matBlob.data()), matBlob.size()); }

    ExportSettings settings;
    settings.compress         = false;
    settings.engineContentDir = engineDir;
    const auto result = ProjectExporter::exportProject(
        contentDir, "EngGame", "", outputDir, settings);
    REQUIRE(result.success);
    CHECK(result.assetsPacked == 3);   // project material + both engine meshes

    const auto pakPath = outputDir / "EngGame.hpak";
    REQUIRE(std::filesystem::exists(pakPath));
    {
        HpakReader reader;
        REQUIRE(reader.open(pakPath.string()));
        CHECK(reader.hasEntry(cubeId));
        CHECK(reader.hasEntry(sphereId));   // ← the whole point: NOT just the cube
    }

    // The shipped game resolves them by UUID (what a MeshComponent stores) and by
    // the "Engine/…" path the editor writes into references.
    ContentManager cm;
    REQUIRE(cm.mountPak(pakPath.string()));
    // Checked FIRST, while nothing is registered yet, so this can only be
    // answered by the pak's path index: the bare META path an engine default
    // carries must never enter it — it would hijack a project asset that happens
    // to be called Cube.hasset.
    CHECK(cm.loadAsset("Cube.hasset") == HE::UUID{});
    REQUIRE(cm.ensureResident(sphereId));
    const StaticMeshAsset* sphere = cm.getStaticMesh(sphereId);
    REQUIRE(sphere != nullptr);
    CHECK(sphere->name == "Sphere");
    CHECK(cm.loadAsset("Engine/Meshes/Cube.hasset") == cubeId);

    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(engineDir);
    he_test::removeAllQuiet(outputDir);
}

TEST_CASE("A project override of an engine default is the one that ships")
{
    // Editing an engine default writes a project-local override under
    // Content/Engine/… with the SAME UUID (see ContentManager::resolveSavePath).
    // Packing both roots must therefore yield ONE entry — the project's.
    auto contentDir = std::filesystem::temp_directory_path() / "he_test_engovr_content";
    auto engineDir  = std::filesystem::temp_directory_path() / "he_test_engovr_engine";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_test_engovr_out";
    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(engineDir);
    he_test::removeAllQuiet(outputDir);

    const HE::UUID cubeId{0x0000000000000100ULL, 0x0000000000000001ULL};
    auto writeCube = [&](const std::filesystem::path& root, const std::string& relPath,
                         const std::string& name)
    {
        ContentManager cm(root.string());
        StaticMeshAsset m;
        m.type     = HE::AssetType::StaticMesh;
        m.id       = cubeId;
        m.name     = name;
        m.path     = relPath;
        m.vertices = { 0,0,0,  1,0,0,  0,1,0 };
        m.normals  = { 0,0,1,  0,0,1,  0,0,1 };
        m.uvs      = { 0,0,  1,0,  0,1 };
        m.indices  = { 0,1,2 };
        REQUIRE(cm.saveAsset(m));
    };
    writeCube(engineDir,  "Meshes/Cube.hasset",        "EngineCube");
    writeCube(contentDir, "Engine/Meshes/Cube.hasset", "ProjectCube");

    ExportSettings settings;
    settings.compress         = false;
    settings.engineContentDir = engineDir;
    const auto result = ProjectExporter::exportProject(
        contentDir, "OvrGame", "", outputDir, settings);
    REQUIRE(result.success);
    CHECK(result.assetsPacked == 1);   // one UUID → one entry, not two

    ContentManager cm;
    REQUIRE(cm.loadPak((outputDir / "OvrGame.hpak").string()));
    const StaticMeshAsset* cube = cm.getStaticMesh(cubeId);
    REQUIRE(cube != nullptr);
    CHECK(cube->name == "ProjectCube");

    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(engineDir);
    he_test::removeAllQuiet(outputDir);
}

TEST_CASE("ProjectExporter packs the GameInstance graph into the pak")
{
    // Regression: GameInstance.hcode (project root, .hcode ext) was never shipped,
    // so the packaged game ran OnInit on an empty graph — and any UI the
    // GameInstance creates (OnInit → Create Object → createWidget) never appeared.
    auto contentDir = std::filesystem::temp_directory_path() / "he_test_gi_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_test_gi_out";
    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
    std::filesystem::create_directories(contentDir);

    const HE::UUID matId{0xBEEF, 0xF00D};
    { const auto blob = makeMinimalMaterialBlob(matId, "gi_mat");
      std::ofstream f(contentDir / "gi_mat.hasset", std::ios::binary);
      f.write(reinterpret_cast<const char*>(blob.data()), blob.size()); }

    const std::string giJson =
        R"({"nodes":[{"id":1,"type":"Event","s":"OnInit"}],"links":[],"variables":[],"nextId":2})";

    ExportSettings settings; settings.compress = false;
    const auto result = ProjectExporter::exportProject(
        contentDir, "GiGame", "", outputDir, settings, /*startupSceneBinary=*/{},
        /*extraScenes=*/{}, giJson);
    REQUIRE(result.success);

    ContentManager cm;
    REQUIRE(cm.mountPak((outputDir / "GiGame.hpak").string()));
    const auto giBytes = cm.readMountedEntry(sceneUuidForPath(kGameInstanceEntry));
    REQUIRE(!giBytes.empty());
    CHECK(std::string(giBytes.begin(), giBytes.end()) == giJson);

    // Empty GameInstance → no entry packed (game falls back / runs empty).
    auto outputDir2 = std::filesystem::temp_directory_path() / "he_test_gi_out2";
    he_test::removeAllQuiet(outputDir2);
    const auto result2 = ProjectExporter::exportProject(
        contentDir, "GiGame2", "", outputDir2, settings);
    REQUIRE(result2.success);
    ContentManager cm2;
    REQUIRE(cm2.mountPak((outputDir2 / "GiGame2.hpak").string()));
    CHECK(cm2.readMountedEntry(sceneUuidForPath(kGameInstanceEntry)).empty());

    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
    he_test::removeAllQuiet(outputDir2);
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

    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
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

    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
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

    he_test::removeAllQuiet(runtimeDir);
    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
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

    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
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

    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
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

    he_test::removeAllQuiet(runtimeDir);
    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
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

    he_test::removeAllQuiet(contentDir);
    he_test::removeQuiet(outputPath);
}

// Mirrors the GameApplication runtime sequence: export a project, then read the
// hcfg, mount the pak with its key, stream the assets in on background workers,
// and drain via pollAsyncResults until they are resident — exactly what the game
// runtime now does instead of an eager blocking load.
TEST_CASE("ProjectExporter output mounts + streams like the game runtime")
{
    auto contentDir = std::filesystem::temp_directory_path() / "he_export_stream_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_export_stream_out";
    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
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

    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
}

TEST_CASE("ProjectExporter packs a binary startup scene into the pak")
{
    auto contentDir = std::filesystem::temp_directory_path() / "he_scene_pak_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_scene_pak_out";
    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
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

    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
}

TEST_CASE("ProjectExporter's scene index is valid JSON for awkward scene paths")
{
    // The index used to be assembled by a hand-rolled escaper that only escaped
    // " and \ — a control character in a path (legal on POSIX) went in RAW and
    // made the whole array unparseable, so the shipped game enumerated NO scenes.
    // It goes through nlohmann now; this pins that every path survives verbatim.
    auto contentDir = std::filesystem::temp_directory_path() / "he_scene_index_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_scene_index_out";
    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
    std::filesystem::create_directories(contentDir);

    const HE::UUID matId{0x1DE, 0x1};
    { const auto blob = makeMinimalMaterialBlob(matId, "idx_mat");
      std::ofstream f(contentDir / "idx_mat.hasset", std::ios::binary);
      f.write(reinterpret_cast<const char*>(blob.data()), blob.size()); }

    const std::vector<std::string> paths = {
        "Scenes/Main.hescene",
        "Scenes/quote\"and\\backslash.hescene",
        "Scenes/tab\tand\nnewline.hescene",
    };
    std::vector<std::pair<std::string, std::vector<uint8_t>>> extraScenes;
    for (const auto& p : paths) extraScenes.push_back({ p, std::vector<uint8_t>{1,2,3,4} });

    const auto result = ProjectExporter::exportProject(
        contentDir, "IndexGame", "", outputDir, {}, {}, extraScenes);
    REQUIRE(result.success);

    ProjectConfig cfg;
    REQUIRE(ProjectConfigLoader::load(outputDir, cfg));
    ContentManager cm;
    REQUIRE(cm.mountPak((outputDir / cfg.hpakFilename).string(),
                        cfg.encrypted ? cfg.encKey : nullptr));

    const std::vector<uint8_t> idx = cm.readMountedEntry(sceneUuidForPath(kSceneIndexEntry));
    REQUIRE(!idx.empty());
    const nlohmann::json j = nlohmann::json::parse(
        std::string(idx.begin(), idx.end()), nullptr, false);
    REQUIRE(!j.is_discarded());          // raw control chars would have failed here
    REQUIRE(j.is_array());
    REQUIRE(j.size() == paths.size());
    for (size_t i = 0; i < paths.size(); ++i)
        CHECK(j[i].get<std::string>() == paths[i]);

    // …and each of those scenes is really addressable under its path-derived UUID.
    for (const auto& p : paths)
        CHECK(cm.readMountedEntry(sceneUuidForPath(p)) == std::vector<uint8_t>{1,2,3,4});

    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
}

#ifdef HE_HAVE_CRYPTO
// End-to-end: exporting with encryption generates a random key, ships it in
// project.hcfg, and the runtime path (loadPak with that key) decrypts correctly.
// This is the fix for the bug where the game never passed a key to loadPak.
TEST_CASE("ProjectExporter encrypts and the hcfg key decrypts the pak")
{
    auto contentDir = std::filesystem::temp_directory_path() / "he_test_export_enc_content";
    auto outputDir  = std::filesystem::temp_directory_path() / "he_test_export_enc_out";
    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
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

    he_test::removeAllQuiet(contentDir);
    he_test::removeAllQuiet(outputDir);
}
#endif // HE_HAVE_CRYPTO
