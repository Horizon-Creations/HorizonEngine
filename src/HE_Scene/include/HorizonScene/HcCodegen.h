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
    // Progress: invoked before each class is translated (build-output UI).
    std::function<void(const std::string& label, size_t index, size_t count)> onClass;
};

struct GeneratedFile { std::string name; std::string contents; };

struct Result
{
    bool ok = false;                        // false only on internal errors
    std::vector<GeneratedFile> files;       // hcgen_<Class>.h/.cpp per class + hc_registry.h/.cpp
    // node = the graph node the reason anchors to (0 = whole graph) — lets the
    // editor highlight the offending node ("compile error in the graph").
    struct Fallback { std::string key, reason; int node = 0; };
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

// Point the codegen at a cmake bundled next to the editor (<dir>/bin/cmake[.exe]).
// Call once at startup (before probeToolchain/buildDylib): cmake resolution then
// prefers the bundle over a system cmake on PATH, so a user only needs a C++ compiler.
// Empty/unset ⇒ system cmake only. Resolution is cached on first use.
void setBundledCmakeDir(const std::filesystem::path& dir);

// True when a cmake executable (bundled or on PATH) answers --version.
bool toolchainAvailable();

// ── startup toolchain diagnostics ────────────────────────────────────────────
// Richer probe for the editor's startup check (EditorApplication): unlike
// toolchainAvailable() this also verifies a WORKING C++ compiler by running a
// real (buildless) CMake configure of a throwaway project — the only reliable
// cross-platform way to know a compiler will actually be found (Windows in
// particular: cmake locates MSVC itself, independent of what's on PATH).
// Slow-ish (compiler detection, up to ~1-2s) — call off the UI thread.
struct ToolchainProbe
{
    bool        cmakeFound    = false;
    std::string cmakeVersion;    // e.g. "3.28.3", empty if not found
    bool        compilerFound = false;
    std::string compilerId;      // e.g. "AppleClang 15.0.0", empty if not detected
    std::string detail;          // tail of the probe log when compilerFound == false
};
ToolchainProbe probeToolchain();

// ── automatic toolchain install ──────────────────────────────────────────────
// Best-effort, platform-specific install of the missing toolchain pieces so the
// user doesn't have to copy a command into a terminal. Installer output is
// streamed line-by-line to `onLine` as it runs (for a live progress view).
// BLOCKING — run on a background thread. Only installs what's missing
// (needCmake/needCompiler come from a prior probeToolchain()). Package managers:
// macOS → Homebrew (cmake) + `xcode-select --install` (clang; hands off to the
// system GUI installer, so its progress is limited); Windows → winget; Linux →
// pkexec + the distro's apt/dnf/pacman/zypper. When no route is available it
// returns attempted=false with an explanatory message instead of failing hard.
struct ToolchainInstall
{
    bool        attempted = false; // did we actually launch any installer?
    int         exitCode  = 0;     // last installer's exit code (0 = success)
    std::string message;           // human summary / why nothing could run
};
ToolchainInstall installToolchain(bool needCmake, bool needCompiler,
                                  const std::function<void(const std::string&)>& onLine);

struct BuildOutcome
{
    bool                  ok = false;
    std::string           message;    // one-line failure summary for the export report
    std::filesystem::path artifact;   // the built shared library (set on success)
    std::filesystem::path logFile;    // full cmake/compiler output (genDir/build.log)
};
// Configure + build the generated project (genDir holds the files from
// generate() + generateCMakeLists()). Blocking — run it on the export worker.
// `onLine` (optional) streams every toolchain output line as it appears (the
// build-output UI); the full output is written to genDir/build.log either way.
BuildOutcome buildDylib(const std::filesystem::path& genDir, const SdkInfo& sdk,
                        const std::function<void(const std::string& line)>& onLine = {});

} // namespace HE::hccg
