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
#include "doctest.h"

#include <Application/GameBackendRules.h>
#include <Types/Enums.h>

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
