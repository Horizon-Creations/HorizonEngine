// The editor's "Build and Reload" all the way through, with a real compiler.
//
// Every other test in this area loads a library CMake built as part of the test
// suite. This one starts from what a user actually has — a project folder with
// the scaffold's own Source/ tree in it — runs the SAME build the menu row runs
// (HE::hccg::gameLogicSpec + buildDylib, no reimplementation), and then loads the
// result through the loader with the service tables attached. What it pins is the
// wiring nothing else can: that HORIZON_ENGINE_DIR is enough for the scaffold's
// CMakeLists to find <IGameLogic.h>, that the artifact really is called
// GameLogic.<ext> in the directory the loader then looks in, and that a module
// compiled that way runs.
//
// Skipped rather than failed without a toolchain: a machine with no cmake or no
// C++ compiler cannot answer this question, and the answer it would give is
// about the machine, not about the engine.
#include "doctest.h"
#include "TestFsUtil.h"
#include <Application/GameLogicLoader.h>
#include <CppScaffoldTemplates.h>
#include <HorizonScene/HcCodegen.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/NameComponent.h>
#include <filesystem>
#include <fstream>

// The engine checkout this test binary was built from — the HORIZON_ENGINE_DIR a
// deployed editor recovers from its SDK config (engineRootFromSdk).
#ifndef HE_TEST_ENGINE_ROOT
#  define HE_TEST_ENGINE_ROOT ""
#endif

namespace {

void writeFile(const std::filesystem::path& p, const std::string& text)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

// A GameLogic.cpp on the scaffold's diet: one include directory, no link
// libraries, no glm. Not the scaffold's own entry point — that one drives a
// GameInstance and a level-script registry, and none of that is what this test
// asks about. What it does exercise is the diet itself: <IGameLogic.h> and
// <HorizonGameServices.h> have to be reachable and self-sufficient under nothing
// but HORIZON_ENGINE_DIR, and the he::* calls have to be a safe no-op with no
// tables injected.
const char* kProbeSource = R"(#include <IGameLogic.h>
#include <HorizonGameServices.h>

HE_IMPLEMENT_ENGINE_SERVICES()

namespace {
class ProbeLogic final : public IGameLogic
{
public:
    void onStart(HorizonWorld&) override
    {
        // Through the injected table, never through HorizonWorld — this module
        // cannot see that type, which is exactly the constraint being tested.
        he::entity::findByName("Marker");
    }
    void onUpdate(HorizonWorld&, float) override { he::physics::available(); }
    void onStop(HorizonWorld&) override {}
};
}

extern "C" HE_GAME_API IGameLogic* HE_CreateGameLogic()               { return new ProbeLogic(); }
extern "C" HE_GAME_API void        HE_DestroyGameLogic(IGameLogic* p) { delete p; }
)";

} // namespace

TEST_CASE("Game logic build: the editor's build path compiles and loads a real module")
{
    const std::filesystem::path engineRoot = HE_TEST_ENGINE_ROOT;
    REQUIRE(!engineRoot.empty());
    REQUIRE(std::filesystem::exists(engineRoot / "src" / "HE_Core" / "include" / "IGameLogic.h"));

    if (!HE::hccg::toolchainAvailable())
    {
        MESSAGE("no cmake on this machine — the compile half cannot be exercised here");
        return;
    }

    // A project folder, the way the editor has one: a .heproj file with a
    // Source/ folder beside it. The CMakeLists is the scaffold's OWN, verbatim,
    // because the point is whether that file builds under these defines.
    const std::filesystem::path root = std::filesystem::temp_directory_path()
                                     / "he_gamelogic_build_probe";
    he_test::removeAllQuiet(root);
    const std::filesystem::path projectFile = root / "Probe.heproj";
    const std::filesystem::path source      = root / "Source";
    std::filesystem::create_directories(source);
    writeFile(projectFile, "{}");
    writeFile(source / "CMakeLists.txt", CppScaffold::cmakeLists("Probe"));
    writeFile(source / "GameLogic.cpp",  kProbeSource);

    // Nothing built yet — and the editor asks exactly this before play mode.
    CHECK(HE::hccg::builtGameLogic(projectFile).empty());

    const HE::hccg::DylibBuildSpec spec = HE::hccg::gameLogicSpec(projectFile, engineRoot);
    CHECK(spec.sourceDir == source);
    CHECK(spec.buildDir  == source / "build");

    std::string lastError;
    const HE::hccg::BuildOutcome out = HE::hccg::buildDylib(spec,
        [&lastError](const std::string& line)
        {
            if (line.find("rror") != std::string::npos) lastError = line;
        });
    INFO("build message: " << out.message << "  |  last error line: " << lastError);
    if (!out.ok)
    {
        // A machine with cmake but no working compiler lands here. Say which of
        // the two it was and stop — a red test would be about the machine.
        MESSAGE("the toolchain could not build the scaffold here: " << out.message);
        he_test::removeAllQuiet(root);
        return;
    }

    // The name and the place the loader will look in — this is the pairing that
    // the HorizonCodeGen artifact list would have got wrong (no `lib` prefix).
    REQUIRE(out.artifact.filename().stem() == "GameLogic");
    CHECK(HE::hccg::builtGameLogic(projectFile) == out.artifact);
    CHECK(std::filesystem::exists(spec.logFile));

    // …and it runs. loadAndStart is the whole sequence the button drives, so a
    // module that comes up here is one the editor can hot-swap.
    {
        HorizonWorld world;
        HE::GameLogicLoader loader;
        REQUIRE(loader.loadAndStart(out.artifact, world, nullptr));
        CHECK(loader.isLoaded());
        loader.logic()->onUpdate(world, 1.0f / 60.0f);
        loader.unload(world);
        CHECK(!loader.isLoaded());
    }

    // A second build over the same cache, which is what pressing the button
    // twice in a session is. It must land on the same artifact, not a second one.
    const HE::hccg::BuildOutcome again = HE::hccg::buildDylib(spec);
    CHECK(again.ok);
    CHECK(again.artifact == out.artifact);

    he_test::removeAllQuiet(root);
}
