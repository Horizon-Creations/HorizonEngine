#pragma once
#include <HorizonCode/HorizonCode.h>
#include <filesystem>
#include <string>
#include <vector>

// ── HE::hccg — HorizonCode → C++ code generation ─────────────────────────────
// Turns HorizonCode graphs into CompiledInstance subclasses (see
// docs/horizoncode-cpp-codegen-implementation-plan.md). Lives in HE_Scene
// because it needs both the graph model (HE_Core) and the HE::api registry
// (EngineCall validation). Consumers: the editor's export worker (in-process),
// the hc_codegen CLI, and the parity tests.
//
// Contract: generated code must be observably identical to the interpreter —
// the semantic contract is plan §3; every lowering cites it. A graph the
// generator can't compile is never an error: it becomes a Fallback entry and
// ships interpreted (per-asset hybrid).

namespace HE::hccg {

struct ClassSource
{
    std::string        key;     // canonical registry key (asset path / "level:…" / "__game_instance__")
    std::string        label;   // for diagnostics ("MainMenu.hasset")
    HorizonCode::Graph graph;   // post-fromJson (signatures synced, subgraphs assigned)
};

struct Options
{
    bool        traceHooks = false;        // reserved (parity tracing; v1 records at the Context seam)
    std::string namespaceName = "hcgen";   // namespace of the generated classes + registry
    std::string engineVersion;             // baked into the manifest, checked at load
};

struct GeneratedFile { std::string name; std::string contents; };

struct Result
{
    bool ok = false;                        // false only on internal errors
    std::vector<GeneratedFile> files;       // hcgen_<Class>.h/.cpp per class + hc_registry.h/.cpp
    struct Fallback { std::string key, reason; };
    std::vector<Fallback> fallbacks;        // validated-out graphs (ship interpreted)
    std::vector<std::string> warnings;
};

Result generate(const std::vector<ClassSource>& sources, const Options& opt);

// The CMake project that builds the generated files into HorizonCodeGen.<dylib>
// (export packaging, plan §8.2). cppFiles = the .cpp names from Result::files.
std::string generateCMakeLists(const Options& opt, const std::vector<std::string>& cppFiles);

// ── toolchain integration (plan §8.3/§8.4) ───────────────────────────────────
// What the generated project needs from the export host: engine headers to
// compile against and the engine libraries to link. Three sources, first hit
// wins: the HE_HCGEN_SDK env override (CI/unusual layouts), a staged
// <editorBase>/SDK/ (deployed editor), or the he_sdk_config.json CMake writes
// beside the editor binary in a development build.
struct SdkInfo
{
    std::vector<std::filesystem::path> includeDirs;
    std::filesystem::path              libDir;
    bool valid() const { return !includeDirs.empty() && !libDir.empty(); }
};
SdkInfo resolveSdk(const std::filesystem::path& editorBaseDir);

// True when a `cmake` executable answers --version (the toolchain probe).
bool toolchainAvailable();

struct BuildOutcome
{
    bool                  ok = false;
    std::string           message;    // one-line failure summary for the export report
    std::filesystem::path artifact;   // the built shared library (set on success)
    std::filesystem::path logFile;    // full cmake/compiler output (genDir/build.log)
};
// Configure + build the generated project (genDir holds the files from
// generate() + generateCMakeLists()). Blocking — run it on the export worker.
BuildOutcome buildDylib(const std::filesystem::path& genDir, const SdkInfo& sdk);

} // namespace HE::hccg
