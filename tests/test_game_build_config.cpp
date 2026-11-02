// HEGameBuildConfig.h — the project configuration an SDK export compiles into
// the game (Thema 211 step 4, Hpak/GameBuildConfig.h).
//
// The fixture header next to this test (fixtures/game_build_config/) IS the
// generator's output for fixtureConfig()/fixtureSettings() below, and this TU
// includes it: if the generator writes something that is not valid C++, or that
// initialises the wrong fields, this file does not compile or the round trip
// fails. After a deliberate change to the generator, regenerate it with
//   HE_UPDATE_GAME_BUILD_CONFIG_FIXTURE=1 he_tests --source-file='*test_game_build_config.cpp'
// and rebuild.
#include "doctest.h"
#include <Hpak/GameBuildConfig.h>
#include <Hpak/ProjectConfig.h>
#include <Hpak/Aes256Gcm.h>
#include <Project/ProjectSettings.h>
#include <nlohmann/json.hpp>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "HEGameBuildConfig.h"   // fixtures/game_build_config, see the top comment

namespace fs = std::filesystem;

namespace
{

// Names that break a naive literal: a quote, a backslash, a non-ASCII letter,
// and a project-settings title that would close the default raw-string delimiter.
ProjectConfig fixtureConfig()
{
    ProjectConfig c;
    c.projectName         = "Der \"Test\" \\ Spiel \xC3\x84";
    c.hpakFilename        = "DerTest.hpak";
    c.mainSceneName       = "Scenes/Main.hescene";
    c.defaultSaveTemplate = "Saves/Default.hasset";
    c.theme               = "Themes/Night.hasset";
    c.themeMode           = "Dark";
    c.bundleId            = "dev.horizon.dertest";
    c.enableModSupport    = true;
    c.encrypted           = true;
    c.hasPackedScene      = true;
    c.horizonCodeCompiled = true;
    c.advancedShaderEffects = false;
    c.allowFiles          = true;
    c.fontScripts         = 3;
    c.fontWeightBold      = false;
    for (int i = 0; i < 16; ++i) c.startupSceneUuid[i] = static_cast<uint8_t>(0xC0 + i);
    for (int i = 0; i < 32; ++i) c.encKey[i] = static_cast<uint8_t>(i * 7 + 1);
    c.collisionLayers.setLayerName(5, "Projectile");
    c.collisionLayers.setCollides(5, 2, false);
    c.audioBuses.add("Music", 0.5f);
    c.audioBuses.masterVolume = 0.75f;
    return c;
}

HE::ProjectSettings fixtureSettings()
{
    HE::ProjectSettings s;
    // JSON escapes every quote, so only a string that ENDS in )hecfg puts
    // `)hecfg"` into the dump.
    s.game.title = "Der Test )hecfg";
    s.input.enabled = true;
    s.input.stickDeadzone = 0.25f;
    return s;
}

const uint8_t* fixtureMask()
{
    static uint8_t m[32];
    for (int i = 0; i < 32; ++i) m[i] = static_cast<uint8_t>(0xA5 ^ (i * 13));
    return m;
}

const fs::path kFixture = fs::path(HE_TEST_GAME_BUILD_CONFIG_FIXTURE);

std::string readText(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string layersJson(const ProjectConfig& c)
{
    nlohmann::json j = nlohmann::json::object();
    c.collisionLayers.toJson(j);
    return j.dump();
}

std::string busesJson(const ProjectConfig& c)
{
    nlohmann::json j = nlohmann::json::object();
    c.audioBuses.toJson(j);
    return j.dump();
}

void checkSameConfig(const ProjectConfig& a, const ProjectConfig& b)
{
    CHECK(a.projectName == b.projectName);
    CHECK(a.hpakFilename == b.hpakFilename);
    CHECK(a.mainSceneName == b.mainSceneName);
    CHECK(a.defaultSaveTemplate == b.defaultSaveTemplate);
    CHECK(a.theme == b.theme);
    CHECK(a.themeMode == b.themeMode);
    CHECK(a.bundleId == b.bundleId);
    CHECK(a.enableModSupport == b.enableModSupport);
    CHECK(a.devTools == b.devTools);
    CHECK(a.encrypted == b.encrypted);
    CHECK(a.hasPackedScene == b.hasPackedScene);
    CHECK(a.horizonCodeCompiled == b.horizonCodeCompiled);
    CHECK(a.appMode == b.appMode);
    CHECK(a.advancedShaderEffects == b.advancedShaderEffects);
    CHECK(a.allowFiles == b.allowFiles);
    CHECK(a.allowProcesses == b.allowProcesses);
    CHECK(a.allowNetwork == b.allowNetwork);
    CHECK(a.fontScripts == b.fontScripts);
    CHECK(a.fontWeightBold == b.fontWeightBold);
    CHECK(std::memcmp(a.startupSceneUuid, b.startupSceneUuid, 16) == 0);
    CHECK(std::memcmp(a.encKey, b.encKey, 32) == 0);
    CHECK(layersJson(a) == layersJson(b));
    CHECK(busesJson(a) == busesJson(b));
}

// A scratch directory of its own per case (he_tests runs files in parallel).
struct ScratchDir
{
    fs::path path;
    explicit ScratchDir(const char* name)
        : path(fs::temp_directory_path() / name)
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~ScratchDir() { std::error_code ec; fs::remove_all(path, ec); }
};

} // namespace

TEST_CASE("GameBuildConfig: the generator writes the checked-in fixture header")
{
    const ProjectConfig       cfg = fixtureConfig();
    const HE::ProjectSettings ps  = fixtureSettings();
    const std::string text = HE::generateGameBuildConfigHeader(cfg, &ps, "release", fixtureMask());
    REQUIRE_FALSE(text.empty());

    if (std::getenv("HE_UPDATE_GAME_BUILD_CONFIG_FIXTURE"))
    {
        REQUIRE(HE::writeGameBuildConfigHeader(kFixture, cfg, &ps, "release", fixtureMask()));
        MESSAGE("rewrote " << kFixture.string() << ": rebuild he_tests and run again");
    }
    CHECK(text == readText(kFixture));

    // Readable: the project's own names and the field names are in the text.
    CHECK(text.find(".hpakFilename          = \"DerTest.hpak\"") != std::string::npos);
    CHECK(text.find("\\\"Test\\\" \\\\ Spiel \\303\\204") != std::string::npos);
    // The title would close R"hecfg(...)hecfg", so the delimiter had to grow.
    CHECK(text.find("R\"hecfg_(") != std::string::npos);
}

TEST_CASE("GameBuildConfig: the pak key is not in the header in plain text")
{
    const ProjectConfig cfg = fixtureConfig();
    const std::string text = HE::generateGameBuildConfigHeader(cfg, nullptr, "release", fixtureMask());
    // Any four consecutive key bytes, spelt the way the header spells bytes.
    for (int start = 0; start + 4 <= 32; ++start)
    {
        char run[64];
        std::snprintf(run, sizeof run, "0x%02x, 0x%02x, 0x%02x, 0x%02x",
                      cfg.encKey[start], cfg.encKey[start + 1],
                      cfg.encKey[start + 2], cfg.encKey[start + 3]);
        CHECK_MESSAGE(text.find(run) == std::string::npos, "key bytes at " << start);
    }

    SUBCASE("a random mask differs per export and still decodes to the same key")
    {
        if (!Hpak::cryptoAvailable()) return;
        ScratchDir dir("he_game_build_config_mask");
        const std::string a = HE::generateGameBuildConfigHeader(cfg, nullptr, "release");
        const std::string b = HE::generateGameBuildConfigHeader(cfg, nullptr, "release");
        CHECK(a != b);
        REQUIRE(HE::writeGameBuildConfigHeader(dir.path / "HEGameBuildConfig.h", cfg, nullptr, "release"));
        uint8_t key[32] = {};
        REQUIRE(HE::readGameBuildConfigPakKey(dir.path / "HEGameBuildConfig.h", key));
        CHECK(std::memcmp(key, cfg.encKey, 32) == 0);
    }
}

#if defined(HE_HAVE_OPENSSL)   // the mbedTLS build has no such cap and would try to allocate 2 GB
TEST_CASE("Aes256Gcm: a length above INT_MAX is refused before OpenSSL sees it")
{
    if (!Hpak::cryptoAvailable()) return;
    // The guard returns before anything reads the buffers, so a one-byte stand-in
    // is enough; a real 2 GB buffer would only test the allocator.
    const size_t tooBig = static_cast<size_t>(INT_MAX) + 1;
    uint8_t key[32] = {}, nonce[12] = {}, dummy[1] = {};
    std::vector<uint8_t> out(7, 0xAB);

    CHECK_FALSE(Hpak::aesGcmEncrypt(key, nonce, dummy, tooBig, out));
    CHECK(out.size() == 7);   // untouched: no resize(len + tag) before the guard
    CHECK_FALSE(Hpak::aesGcmDecrypt(key, nonce, dummy, tooBig + 16, out));
    CHECK(out.size() == 7);
    CHECK_FALSE(Hpak::randomBytes(dummy, tooBig));

    SUBCASE("a small buffer still round-trips")
    {
        const uint8_t msg[5] = {1, 2, 3, 4, 5};
        std::vector<uint8_t> sealed, opened;
        REQUIRE(Hpak::aesGcmEncrypt(key, nonce, msg, sizeof msg, sealed));
        CHECK(sealed.size() == sizeof msg + 16);
        REQUIRE(Hpak::aesGcmDecrypt(key, nonce, sealed.data(), sealed.size(), opened));
        CHECK(opened == std::vector<uint8_t>(msg, msg + sizeof msg));
    }
}
#endif

TEST_CASE("GameBuildConfig: the compiled fixture reads back as the config it was generated from")
{
    const HE::GameBuildConfig& compiled = HE::GameBuild::kConfig;
    CHECK(compiled.format == HE::kGameBuildConfigFormat);
    CHECK(std::string(compiled.buildProfile) == "release");

    ProjectConfig got;
    HE::projectConfigFromBuildConfig(compiled, got);
    checkSameConfig(got, fixtureConfig());

    HE::ProjectSettings ps;
    REQUIRE(HE::resolveProjectSettings(&compiled, fs::path("does-not-exist"), ps));
    CHECK(ps == fixtureSettings());

    // The exporter's way back to the key (incremental export, SDK-built exe).
    uint8_t key[32] = {};
    REQUIRE(HE::readGameBuildConfigPakKey(kFixture, key));
    CHECK(std::memcmp(key, fixtureConfig().encKey, 32) == 0);
}

TEST_CASE("GameBuildConfig: compiled-in values win over project.hcfg and ProjectSettings.json")
{
    ScratchDir dir("he_game_build_config_precedence");
    // A complete, DIFFERENT project beside the executable, as an export into a
    // folder that held a prebuilt-runtime export before would leave it.
    ProjectConfig onDisk;
    onDisk.projectName  = "Other";
    onDisk.hpakFilename = "Other.hpak";
    onDisk.encrypted    = true;
    for (int i = 0; i < 32; ++i) onDisk.encKey[i] = 0xEE;
    REQUIRE(ProjectConfigLoader::save(dir.path, onDisk));
    HE::ProjectSettings diskSettings;
    diskSettings.game.title = "Other title";
    REQUIRE(HE::saveProjectSettings(dir.path, diskSettings));

    ProjectConfig cfg;
    CHECK(HE::resolveProjectConfig(&HE::GameBuild::kConfig, dir.path, cfg)
          == HE::ProjectConfigSource::Compiled);
    checkSameConfig(cfg, fixtureConfig());

    HE::ProjectSettings ps;
    REQUIRE(HE::resolveProjectSettings(&HE::GameBuild::kConfig, dir.path, ps));
    CHECK(ps.game.title == fixtureSettings().game.title);

    // The compiled key wins over a patched block too: an SDK build has no
    // reason to carry one, but a stale one must not mount the pak with the
    // wrong key.
    uint8_t block[32];
    std::memset(block, 0x11, 32);
    const uint8_t* key = nullptr;
    CHECK(HE::selectPakKey(HE::ProjectConfigSource::Compiled, cfg, block, key)
          == HE::PakKeySource::Compiled);
    REQUIRE(key != nullptr);
    CHECK(std::memcmp(key, fixtureConfig().encKey, 32) == 0);

    SUBCASE("no file at all: the compiled config is still a project")
    {
        ScratchDir empty("he_game_build_config_nofile");
        ProjectConfig c2;
        CHECK(HE::resolveProjectConfig(&HE::GameBuild::kConfig, empty.path, c2)
              == HE::ProjectConfigSource::Compiled);
        CHECK(c2.hpakFilename == "DerTest.hpak");
    }
    SUBCASE("an unencrypted compiled config mounts without a key")
    {
        ProjectConfig plain = fixtureConfig();
        plain.encrypted = false;
        CHECK(HE::selectPakKey(HE::ProjectConfigSource::Compiled, plain, block, key)
              == HE::PakKeySource::None);
        CHECK(key == nullptr);
    }
}

TEST_CASE("GameBuildConfig: without compiled values the old project.hcfg path loads as before")
{
    ScratchDir dir("he_game_build_config_oldpath");
    ProjectConfig onDisk = fixtureConfig();
    REQUIRE(ProjectConfigLoader::save(dir.path, onDisk));
    HE::ProjectSettings diskSettings = fixtureSettings();
    REQUIRE(HE::saveProjectSettings(dir.path, diskSettings));

    ProjectConfig cfg;
    CHECK(HE::resolveProjectConfig(nullptr, dir.path, cfg) == HE::ProjectConfigSource::ProjectFile);
    checkSameConfig(cfg, onDisk);
    HE::ProjectSettings ps;
    REQUIRE(HE::resolveProjectSettings(nullptr, dir.path, ps));
    CHECK(ps == diskSettings);

    // The prebuilt runtime's key order, unchanged: the patched block, then the hcfg.
    uint8_t block[32];
    std::memset(block, 0x11, 32);
    const uint8_t* key = nullptr;
    CHECK(HE::selectPakKey(HE::ProjectConfigSource::ProjectFile, cfg, block, key)
          == HE::PakKeySource::EmbeddedBlock);
    CHECK(key == block);
    CHECK(HE::selectPakKey(HE::ProjectConfigSource::ProjectFile, cfg, nullptr, key)
          == HE::PakKeySource::ProjectFile);
    CHECK(key == cfg.encKey);
    cfg.encrypted = false;
    CHECK(HE::selectPakKey(HE::ProjectConfigSource::ProjectFile, cfg, nullptr, key)
          == HE::PakKeySource::None);
    CHECK(key == nullptr);

    SUBCASE("no hcfg and no compiled values: no project, and a patched block still counts")
    {
        ScratchDir empty("he_game_build_config_none");
        ProjectConfig none;
        CHECK(HE::resolveProjectConfig(nullptr, empty.path, none) == HE::ProjectConfigSource::None);
        CHECK(HE::selectPakKey(HE::ProjectConfigSource::None, none, block, key)
              == HE::PakKeySource::EmbeddedBlock);
        HE::ProjectSettings def;
        REQUIRE(HE::resolveProjectSettings(nullptr, empty.path, def));
        CHECK(def == HE::ProjectSettings{});
    }
    SUBCASE("an unencrypted header has no key to read back")
    {
        ProjectConfig plain = fixtureConfig();
        plain.encrypted = false;
        const fs::path h = dir.path / "HEGameBuildConfig.h";
        REQUIRE(HE::writeGameBuildConfigHeader(h, plain, nullptr, "dev"));
        uint8_t k[32];
        CHECK_FALSE(HE::readGameBuildConfigPakKey(h, k));
        CHECK_FALSE(HE::readGameBuildConfigPakKey(dir.path / "missing.h", k));
    }
}
