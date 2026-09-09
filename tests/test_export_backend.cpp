// The word that walked out of an application export into every later game.
//
// A packaged GAME booted showing nothing but an evenly dark window in
// (18,18,22) — the clear colour of the software renderer, which draws user
// interface and never a scene (docs/he-apps-plan.md Block G). It was not a
// second window and not a layer over the scene: the game was running on that
// renderer, because its config.json said "GameBackend": "Software".
//
// The word got there through the export dialog. Exporting an APPLICATION with
// Advanced Shader Effects off assigned "Software" into the static holding the
// GAME's chosen backend — and that static is persisted in the editor's own
// settings under the very key the game reads, then reloaded whenever the dialog
// opens. One app export, and every later game export shipped it. "Software" is
// in no platform's list of choices, so it could only ever have arrived that way.
//
// These cases pin all three halves of the fix, each one red against the
// behaviour that shipped:
//   * forExport() answers the app's question WITHOUT touching the remembered
//     value, so the next game export is unaffected
//   * isOffered() rejects a value that a poisoned settings file still carries
//   * verdictForShipped() refuses the software renderer for a game, which is
//     what rescues builds that are already in players' hands
//
// …and that was not the end of it. The word had a second way into a packaged
// game, one that needed no poisoned export at all — see the second half of this
// file, below the cases above.
#include "doctest.h"
#include "TestFsUtil.h"

#include <Application/GameBackendRules.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <Diagnostics/GlobalState.h>
#include <Hpak/ProjectConfig.h>
#include <Hpak/ProjectExporter.h>
#include <Types/Enums.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace HE;
namespace BR = HE::BackendRules;

// Every backend this build knows how to create. The runtime's own predicate
// asks RendererFactory; here the question is the DECISION, not the flavour, so
// the answer is handed in.
static bool allAvailable(RendererBackend) { return true; }

// ─── The regression itself, in the order the user hit it ─────────────────────

TEST_CASE("export backend: an app export leaves the game's remembered backend alone")
{
    // The editor remembers a game backend from an earlier export.
    std::string remembered = "Metal";

    // Now an APPLICATION is exported with Advanced Shader Effects off. It ships
    // the software renderer — that is what the switch has always meant.
    CHECK(BR::forExport(/*appProject=*/true, /*advancedShaderEffects=*/false, remembered)
          == "Software");
    // …and the remembered value is untouched. This is the whole bug: the old
    // dialog assigned "Software" here, and this line is what would fail.
    CHECK(remembered == "Metal");

    // The next export is a GAME again. It must ship what the user picked.
    CHECK(BR::forExport(/*appProject=*/false, /*advancedShaderEffects=*/true, remembered)
          == "Metal");
    CHECK(BR::forExport(/*appProject=*/false, /*advancedShaderEffects=*/false, remembered)
          == "Metal");
}

TEST_CASE("export backend: an app with advanced effects on takes the platform default")
{
    // Empty means "no GameBackend key at all", which is a different answer from
    // naming a backend the target might not have.
    CHECK(BR::forExport(true, true, "Metal").empty());
    CHECK(BR::forExport(true, true, "").empty());
}

TEST_CASE("export backend: a game with nothing picked still ships nothing")
{
    CHECK(BR::forExport(false, true, "").empty());
}

// ─── The poisoned settings file already on disk ──────────────────────────────

TEST_CASE("export backend: 'Software' is offered by no platform")
{
    // The dialog's own lists — the reason a hand-edited config could not have
    // produced this and an app export must have.
    CHECK_FALSE(BR::isOffered("Software"));
    CHECK_FALSE(BR::isOffered("Direct3D"));   // not a getRHIName spelling
    CHECK_FALSE(BR::isOffered("software"));   // names are compared verbatim
    CHECK_FALSE(BR::isOffered(""));

    CHECK(BR::isOffered("OpenGL"));
    CHECK(BR::isOffered("Vulkan"));
    CHECK(BR::isOffered("Metal"));
    CHECK(BR::isOffered("D3D11"));
    CHECK(BR::isOffered("D3D12"));
}

TEST_CASE("export backend: per-platform choices never include the software renderer")
{
    for (const char* platform : { "Windows", "macOS", "Linux", "Host" })
        for (const char* name : BR::choicesFor(platform))
            CHECK(std::string(name) != "Software");

    // The target decides: no DirectX on a Linux build, no Metal off macOS.
    const auto linux_ = BR::choicesFor("Linux");
    CHECK(std::vector<std::string>(linux_.begin(), linux_.end())
          == std::vector<std::string>{ "OpenGL", "Vulkan" });
    const auto mac = BR::choicesFor("macOS");
    CHECK(std::vector<std::string>(mac.begin(), mac.end())
          == std::vector<std::string>{ "Metal", "OpenGL" });
}

// ─── The builds already shipped ──────────────────────────────────────────────

TEST_CASE("shipped config: a game refuses the software renderer")
{
    RendererBackend out = RendererBackend::Metal;   // what the runtime defaulted to

    // The exact config.json the user's broken export carried.
    CHECK(BR::verdictForShipped("Software", /*appMode=*/false, out, allAvailable)
          == BR::Shipped::UiOnlyForGame);
    CHECK(out == RendererBackend::Metal);           // default kept, scene drawn

    // An APPLICATION asked for it on purpose — Block G — and gets it.
    CHECK(BR::verdictForShipped("Software", /*appMode=*/true, out, allAvailable)
          == BR::Shipped::Use);
    CHECK(out == RendererBackend::Software);
}

TEST_CASE("shipped config: names the runtime cannot use fall back, they do not abort")
{
    RendererBackend out = RendererBackend::OpenGL;

    CHECK(BR::verdictForShipped("Nonsense", false, out, allAvailable)
          == BR::Shipped::UnknownName);
    CHECK(out == RendererBackend::OpenGL);

    // A runtime flavour that links only the software rasterizer (A3b): a config
    // naming Metal must fall back rather than throw in RendererFactory.
    auto softwareOnly = [](RendererBackend b) { return b == RendererBackend::Software; };
    CHECK(BR::verdictForShipped("Metal", false, out, softwareOnly)
          == BR::Shipped::NotBuilt);
    CHECK(out == RendererBackend::OpenGL);

    // …and an app-flavour runtime still gets its software renderer.
    CHECK(BR::verdictForShipped("Software", true, out, softwareOnly)
          == BR::Shipped::Use);
    CHECK(out == RendererBackend::Software);
}

TEST_CASE("shipped config: every name the dialog can write parses back")
{
    // The two halves have to agree on the spelling, or an export would ship a
    // name its own runtime warns about.
    for (const char* platform : { "Windows", "macOS", "Linux" })
        for (const char* name : BR::choicesFor(platform))
        {
            RendererBackend out{};
            CAPTURE(name);
            CHECK(BR::fromName(name, out));
            CHECK(BR::verdictForShipped(name, false, out, allAvailable) == BR::Shipped::Use);
        }
}

// ═══ The second half of the same bug ═════════════════════════════════════════
//
// Everything above is about the word the export dialog WRITES. It was fixed,
// merged, and the user rebuilt, re-exported — and the packaged game still said
// software renderer in its boot log. The word had a second way in.
//
// A shipped game read the per-user settings file. `Application`'s constructor
// calls `GlobalState::readConfig()`, and `configFilePath()` lands in
// ~/Library/Application Support/HorizonEngine (or %APPDATA%, or $XDG_CONFIG_HOME)
// for anything whose working directory is not its own — a macOS .app launched
// from Finder runs in "/". On a developer's machine that file belongs to the
// EDITOR. The shipped config.json was then laid OVER it, which covers the keys
// it carries and no others — and "GameBackend" is deliberately absent whenever
// the dialog says "(platform default)", which is what a fresh game project has.
// So the one key that decides whether anything is drawn at all was taken from
// the editor's preferences, where an application export had once left
// "Software".
//
// Reproduced by hand before it was written down, with a runtime built from the
// already-fixed main: a config.json with no GameBackend, a per-user file with
// one, and the game logged
//
//   [WARN] the software renderer draws user interface only — a game would show
//          an empty window; using the default instead
//
// The rescue from the first fix held, which is why the window was no longer
// black — but the leaked value still reached the decision, and before that fix
// it WAS the decision.
//
// The two cases below are the two halves of that path, and both are red on the
// build that shipped:
//   * a full export of a fresh GAME project writes no GameBackend at all
//   * a runtime pinned to that export cannot see the editor's value either

namespace {

// A whole export of a project that has never been anything but a game: no
// appProject, no app runtime, and the exact config.json body the fixed dialog
// produces for "(platform default)" — every graphics key it always ships, and
// no GameBackend, because effectiveExportBackend() returned empty.
std::filesystem::path exportFreshGameProject(const std::string& tag)
{
    const auto proj = std::filesystem::temp_directory_path() / ("he_fresh_game_proj_" + tag);
    const auto out  = std::filesystem::temp_directory_path() / ("he_fresh_game_out_" + tag);
    he_test::removeAllQuiet(proj);
    he_test::removeAllQuiet(out);
    std::filesystem::create_directories(proj);

    // One ordinary asset, so this is a real pak and not an empty directory.
    ContentManager cm(proj.string());
    MaterialAsset mat;
    mat.type = HE::AssetType::Material;
    mat.name = "Ground";
    mat.path = "Ground.hasset";
    REQUIRE(cm.saveAsset(mat));

    nlohmann::json cfg;
    cfg["CustomConfig"] = nlohmann::json::array({
        nlohmann::json{ { "Key", "BloomEnabled"    }, { "Value", true  } },
        nlohmann::json{ { "Key", "SSAOEnabled"     }, { "Value", true  } },
        nlohmann::json{ { "Key", "GameWindowWidth" }, { "Value", 1280  } },
        nlohmann::json{ { "Key", "GameWindowHeight"}, { "Value", 720   } },
        nlohmann::json{ { "Key", "GameWindowMode"  }, { "Value", "Fullscreen" } },
        nlohmann::json{ { "Key", "GameVSync"       }, { "Value", true  } },
        // …and no "GameBackend". That absence is the whole point: it is how the
        // dialog says "the platform's own", and it is what let the editor's
        // value through from the file underneath.
    });

    ExportSettings settings;
    settings.compress            = false;
    settings.appProject          = false;   // a GAME. Never anything else.
    settings.advancedShaderEffects = true;
    settings.gameConfigJson      = cfg.dump(4);
    // The binaries too, when this configure has a deployed runtime — that is
    // what makes it a full export rather than a data drop. Without one the
    // export is still valid and every assertion below still holds.
    const std::filesystem::path runtimeDir = HE_TEST_GAME_RUNTIME_DIR;
    if (std::filesystem::is_directory(runtimeDir))
        settings.gameRuntimeDir = runtimeDir;

    const auto res = ProjectExporter::exportProject(
        proj, "FreshGame", /*startupSceneName=*/"", out, settings);
    REQUIRE_MESSAGE(res.success, res.errorMessage);
    return out;
}

// The one config.json an export produced. It goes next to project.hcfg, which
// inside a .app is Contents/Resources — so it is searched for rather than
// assumed at the top.
std::filesystem::path findShippedConfig(const std::filesystem::path& out)
{
    for (const auto& e : std::filesystem::recursive_directory_iterator(out))
        if (e.is_regular_file() && e.path().filename() == "config.json")
            return e.path();
    return {};
}

} // namespace

TEST_CASE("fresh game export: the shipped config.json names no backend at all")
{
    const auto out = exportFreshGameProject("nokey");

    const auto cfgPath = findShippedConfig(out);
    REQUIRE_FALSE(cfgPath.empty());

    std::ifstream in(cfgPath);
    REQUIRE(in);
    nlohmann::json j;
    in >> j;
    REQUIRE(j.contains("CustomConfig"));
    for (const auto& kv : j.at("CustomConfig"))
    {
        CHECK(kv.at("Key") != "GameBackend");
        CHECK(kv.at("Value") != "Software");
    }

    // …and the build really is a game, so the runtime's own software-renderer
    // refusal applies to it. m_appMode is latched off exactly this flag, and a
    // true here would let "Software" through verdictForShipped unchallenged.
    ProjectConfig shipped;
    REQUIRE(ProjectConfigLoader::load(cfgPath.parent_path(), shipped));
    CHECK_FALSE(shipped.appMode);
    CHECK(runtimeFlavorFor(shipped.appMode, shipped.advancedShaderEffects)
          == RuntimeFlavor::Game);

    he_test::removeAllQuiet(out);
}

TEST_CASE("shipped game: the editor's settings file cannot reach it")
{
    // HE_CONFIG_DIR is a deliberate developer override and outranks the pin —
    // an environment that sets it is asking for a different file on purpose.
    if (std::getenv("HE_CONFIG_DIR"))
    {
        MESSAGE("HE_CONFIG_DIR is set — the pin is overridden on purpose, skipped");
        return;
    }

    const auto out = exportFreshGameProject("pin");
    const auto cfgPath = findShippedConfig(out);
    REQUIRE_FALSE(cfgPath.empty());

    GlobalState& gs = GlobalState::getInstance();

    // What readConfig() had already loaded from the per-user file on a
    // developer's machine by the time the game got a word in: the editor's
    // remembered export settings, under the very keys the game reads them by.
    gs.setCustomConfigEntry("GameBackend", "Software");
    REQUIRE(gs.getCustomConfigString("GameBackend") == "Software");

    // The game's main() does this before the Application constructor runs.
    GlobalState::useShippedConfig(cfgPath.parent_path());
    CHECK(GlobalState::configFilePath() == cfgPath.parent_path() / "config.json");
    // Nothing may be persisted from here on: the file that write would land in
    // is shared with the editor, and on a machine that has none, readConfig()'s
    // "there is no file" branch would CREATE one in the player's home.
    CHECK_FALSE(gs.configPersistent());

    gs.readConfig();

    // The line that was red: the leaked word must not be there to be read.
    CHECK(gs.getCustomConfigString("GameBackend").empty());
    // …while everything the export really shipped is.
    CHECK(gs.getCustomConfigInt("GameWindowWidth") == 1280);
    CHECK(gs.getCustomConfigString("GameWindowMode") == "Fullscreen");

    // And what GameApplication::applyShippedConfig makes of that: an absent key
    // means the platform default is kept, so the decision is never even asked to
    // refuse the software renderer.
    RendererBackend backend = RendererBackend::Metal;   // RendererFactory::Default()
    const std::string name = gs.getCustomConfigString("GameBackend");
    CHECK(name.empty());
    if (!name.empty())   // never taken; here so a regression says WHAT it chose
        BR::verdictForShipped(name, /*appMode=*/false, backend, allAvailable);
    CHECK(backend == RendererBackend::Metal);

    // Leave the singleton as it was found — one process runs every test.
    GlobalState::useShippedConfig({});
    gs.setConfigPersistent(true);
    he_test::removeAllQuiet(out);
}
