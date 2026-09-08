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
    // Engine base class (HorizonCode.h engineClasses()); "" = Object. Emitted as
    // baseClassKey(), which is what lets a Cast to a BASE class answer the same
    // way for a compiled instance as for an interpreted one. Widgets, level
    // scripts and the GameInstance leave it empty.
    std::string        baseClass;
    // The HorizonCode classes this one derives from, nearest first. Emitted as
    // classChain(), which is what lets a Cast to a PARENT class be answered
    // without the Runtime's map — classTag is one exact address per class and
    // knows nothing about an ancestry.
    //
    // It is also the INHERITANCE the generator emits: chain.front() must appear
    // in the same run as its own source, and the class comes out as
    // `C_Goblin : public C_Enemy`. `graph` is therefore this class's OWN level
    // (ResolvedClass::levels.back()) — NOT the flattened chain. Handing in a
    // flattened graph together with a chain would compile every inherited member
    // twice: once in the base's class and once more here.
    std::vector<std::string> chain;
};

// What a graph the generator cannot compile means for the build.
enum class OnFailure
{
    // Ship it interpreted (the per-asset hybrid): the packaged game carries both
    // backends and they interoperate. Compiling is an optimization, never a gate.
    Interpret,
    // Fail the build instead. The point is a build in which EVERY class is
    // native, so nothing silently falls back to the interpreter — which is also
    // what makes the direct cross-class call paths always hit.
    Stop,
};

struct Options
{
    OnFailure   onFailure = OnFailure::Interpret;
    bool        traceHooks = false;        // reserved (parity tracing; v1 records at the Context seam)
    std::string namespaceName = "hcgen";   // namespace of the generated classes + registry
    std::string engineVersion;             // baked into the manifest, checked at load
    // Progress: invoked before each class is translated (build-output UI).
    std::function<void(const std::string& label, size_t index, size_t count)> onClass;
};

struct GeneratedFile { std::string name; std::string contents; };

struct Result
{
    // "The result is usable." False on an internal error (emitter slip / out of
    // memory) and — with OnFailure::Stop — on any graph that could not be
    // compiled, since the whole point of that mode is that none may fall back.
    // With OnFailure::Interpret a fallback leaves ok true: shipping it
    // interpreted IS the answer. generate() never lets an exception escape; the
    // reason lands in `warnings`.
    bool ok = false;
    // Per compiled class: declarations in hcgen_<Class>.h (so other C++ can
    // include it and call the graph's events and functions directly),
    // definitions in hcgen_<Class>.cpp. Plus hc_registry.h/.cpp, hcgen.h (the
    // whole library in one include, for YOUR code), and — per Struct/Enum
    // definition the classes touch — one hcgen_type_<Name>.h, with
    // hcgen_types.h as their umbrella. Generated files include each other
    // narrowly: a changed graph or type rebuilds only what depends on it.
    std::vector<GeneratedFile> files;
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
// system GUI installer, so its progress is limited). When Homebrew itself is
// absent, a single Terminal.app session installs Homebrew (which also pulls in the
// Command Line Tools compiler) AND then cmake, all in one automated chain — it
// needs interactive admin rights a windowless pipe can't service, so the user just
// enters their password once in Terminal and clicks Recheck when it's done.
// Windows → winget;
// Linux → pkexec + the distro's apt/dnf/pacman/zypper. When no route is available
// it returns attempted=false with an explanatory message instead of failing hard.
// macOS note: a Finder-launched app inherits a minimal PATH without the Homebrew
// prefixes, so this (and probeToolchain) first augment PATH to make an installed
// brew/cmake visible in the first place.
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

// The same cmake run, told what it is building. The overload above is the
// HorizonCode-codegen case expressed through this one; the editor's
// "Build and Reload" uses it for a project's own Source/ folder, which produces
// a DIFFERENTLY NAMED library (GameLogic.dylib — the scaffold sets PREFIX "", so
// there is no `lib` in front) in a build directory that must not sit inside the
// folder the project's CMakeLists globs its sources from.
struct DylibBuildSpec
{
    std::filesystem::path    sourceDir;        // -S: the folder holding CMakeLists.txt
    std::filesystem::path    buildDir;         // -B; empty ⇒ sourceDir/"build"
    std::filesystem::path    logFile;          // empty ⇒ buildDir/"build.log"
    // Looked for under buildDir and buildDir/Release, in this order. Empty ⇒ the
    // three HorizonCodeGen names.
    std::vector<std::string> artifactNames;
    // Extra cmake cache entries as "NAME=value" (no -D). Everything the project
    // being built needs to find its headers goes here — the caller decides,
    // because a generated codegen project and a user's game project are told
    // about the engine in different ways.
    std::vector<std::string> defines;
    // What the "nothing was produced" message calls the missing library.
    std::string              what = "HorizonCodeGen library";
};
BuildOutcome buildDylib(const DylibBuildSpec& spec,
                        const std::function<void(const std::string& line)>& onLine = {});

// Where buildDylib would find an already-built artifact, without building one:
// the same two directories (flat, then Release/) and the same name list. Empty
// path when none of them is there. The editor needs this to load a module that
// was built earlier in the session — or in a previous one.
std::filesystem::path findBuiltArtifact(const std::filesystem::path& buildDir,
                                        const std::vector<std::string>& artifactNames);

// ── A C++ project's native GameLogic module ─────────────────────────────────
// Where it is, what it is called and how it is built. It lives HERE rather than
// in the editor panel that has the button, because "how this project's module is
// built" is one answer that the build path, the play-mode loader and the test
// that compiles a real scaffold all have to give identically. Every function
// takes the .heproj FILE (ProjectManager::Project::path), not its folder.

// GameLogic.{dylib,so,dll} — no `lib` prefix: the scaffold sets PREFIX "" so the
// engine finds a library by the one name it looks for.
const std::vector<std::string>& gameLogicArtifactNames();
// <project>/Source — the folder the scaffold wrote, holding the CMakeLists that
// globs it.
std::filesystem::path gameLogicSourceDir(const std::filesystem::path& projectFile);
// <project>/Source/build — the SAME directory the scaffold's README tells a user
// to configure by hand, deliberately: one cmake cache, and a module built in a
// terminal is the one the editor then loads.
std::filesystem::path gameLogicBuildDir(const std::filesystem::path& projectFile);
// The built module, or empty when nothing has been built yet.
std::filesystem::path builtGameLogic(const std::filesystem::path& projectFile);

// The engine ROOT a scaffold CMakeLists needs as HORIZON_ENGINE_DIR (it looks
// for ${HORIZON_ENGINE_DIR}/src/HE_Core/include). Recovered from the SDK's own
// include list: the entry that IS that directory names the root three parents
// up. Empty when there is no such entry — a staged SDK has a flat include/ with
// no src/ layout, and a caller that gets an empty path must refuse the build
// rather than configure a project that cannot find its one header.
std::filesystem::path engineRootFromSdk(const SdkInfo& sdk);

// The build a "Build and Reload" runs, as data. `engineRoot` comes from
// engineRootFromSdk (or is a checkout root, in a test).
DylibBuildSpec gameLogicSpec(const std::filesystem::path& projectFile,
                             const std::filesystem::path& engineRoot);

} // namespace HE::hccg
