#include <Hpak/ProjectExporter.h>
#include <Platform/GameLayout.h>        // data/ + HorizonEngine/ of the export layout
#include <Types/Enums.h>            // HE::AssetType (the packed type indices)
#include <cstdint>
#include <Hpak/ProjectConfig.h>
#include <Hpak/GameBuildConfig.h>           // readGameBuildConfigPakKey (incremental key reuse)
#include <Application/AppIcon.h>           // the generated .icns/.ico/.png
#include <Application/DocumentTypes.h>      // …and the three ways to declare a file type
#include <Renderer/UIFont.h>                // uiParseRichColor for the plate colour
#include <HorizonCode/HcCompiledLoader.h>   // compiledLibraryName (artifact naming)
#include <Hpak/HpakWriter.h>
#include <Hpak/HpakReader.h>
#include <Hpak/HpakFormat.h>
#include <Hpak/PackManifest.h>
#include <Hpak/Aes256Gcm.h>
#include <Types/UUID.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <cstdlib>
#include <cstring>

// ─── Export platforms ─────────────────────────────────────────────────────────

const char* exportPlatformName(ExportPlatform p)
{
    switch (p)
    {
    case ExportPlatform::Windows: return "Windows";
    case ExportPlatform::MacOS:   return "macOS";
    case ExportPlatform::Linux:   return "Linux";
    case ExportPlatform::Host:
    default:                      return "Host";
    }
}

ExportPlatform exportPlatformFromName(const std::string& name)
{
    if (name == "Windows") return ExportPlatform::Windows;
    if (name == "macOS")   return ExportPlatform::MacOS;
    if (name == "Linux")   return ExportPlatform::Linux;
    return ExportPlatform::Host;
}

const char* runtimeFlavorName(RuntimeFlavor f)
{
    switch (f)
    {
    case RuntimeFlavor::AppAdvanced: return "AppAdvanced";
    case RuntimeFlavor::AppBasic:    return "AppBasic";
    case RuntimeFlavor::Game:
    default:                         return "Game";
    }
}

RuntimeFlavor runtimeFlavorFor(bool appMode, bool advancedShaderEffects)
{
    // A game always takes the full runtime: it has a world, and dropping four of
    // the platform's five backends from something that may run on any GPU is not
    // a size saving, it is a support problem.
    if (!appMode) return RuntimeFlavor::Game;
    return advancedShaderEffects ? RuntimeFlavor::AppAdvanced : RuntimeFlavor::AppBasic;
}

std::filesystem::path resolveRuntimeDir(const std::filesystem::path& editorBaseDir,
                                        ExportPlatform p,
                                        RuntimeFlavor f)
{
    // Host keeps its flat layout (…/Game, …/AppAdvanced, …/AppBasic) — that is
    // where the build deploys. Cross-targets nest the flavour under the platform
    // rather than beside it, because what a user drops there is a bundle built on
    // that platform, and all three of a platform's runtimes arrive together.
    if (p == ExportPlatform::Host)
        return editorBaseDir / ".." / runtimeFlavorName(f);
    if (f == RuntimeFlavor::Game)
        return editorBaseDir / ".." / "GameRuntimes" / exportPlatformName(p);
    return editorBaseDir / ".." / "GameRuntimes" / exportPlatformName(p)
                         / runtimeFlavorName(f);
}

// A directory qualifies as a runtime bundle only if the game executable is
// actually in it — a bare/leftover folder must not silently export 0 binaries.
// A previous EXPORT output also contains HorizonGame (plus project.hcfg + pak);
// shipping a stale export as the "runtime" would carry its old patched key and
// old binaries, so anything with a project.hcfg is rejected.
static bool isRuntimeBundle(const std::filesystem::path& dir)
{
    std::error_code ec;
    if (std::filesystem::exists(dir / "project.hcfg", ec)) return false;
    return std::filesystem::is_regular_file(dir / "HorizonGame", ec)
        || std::filesystem::is_regular_file(dir / "HorizonGame.exe", ec);
}

std::filesystem::path findRuntimeBundle(const std::filesystem::path& editorBaseDir,
                                        ExportPlatform p,
                                        RuntimeFlavor  f,
                                        RuntimeFlavor* outFlavor)
{
    if (outFlavor) *outFlavor = f;
    if (editorBaseDir.empty()) return {};

    // Wanted flavour first, then Game as the fallback. Searching flavour-by-
    // flavour rather than directory-by-directory is deliberate: an editor that
    // has BOTH an out/deploy/AppBasic two levels up and a Game right beside it
    // must still ship the app runtime it was asked for — walking the tree first
    // would hand back whichever happened to sit closer.
    RuntimeFlavor wanted[2] = { f, RuntimeFlavor::Game };
    const int tries = (f == RuntimeFlavor::Game) ? 1 : 2;

    for (int t = 0; t < tries; ++t)
    {
        const RuntimeFlavor flavor = wanted[t];
        const std::filesystem::path sub = (p == ExportPlatform::Host)
            ? std::filesystem::path(runtimeFlavorName(flavor))
            : (flavor == RuntimeFlavor::Game
                   ? std::filesystem::path("GameRuntimes") / exportPlatformName(p)
                   : std::filesystem::path("GameRuntimes") / exportPlatformName(p)
                         / runtimeFlavorName(flavor));

        // Walk upward: <dir>/Game next to the editor covers the deploy layout
        // (deploy/Editor + deploy/Game); <dir>/out/deploy/Game covers running the
        // editor from a build tree anywhere inside the repo.
        std::filesystem::path dir = editorBaseDir.lexically_normal();
        for (int depth = 0; depth < 7 && !dir.empty(); ++depth)
        {
            if (isRuntimeBundle(dir / sub))
            {
                if (outFlavor) *outFlavor = flavor;
                return dir / sub;
            }
            if (isRuntimeBundle(dir / "out" / "deploy" / sub))
            {
                if (outFlavor) *outFlavor = flavor;
                return dir / "out" / "deploy" / sub;
            }
            const auto parent = dir.parent_path();
            if (parent == dir) break; // filesystem root
            dir = parent;
        }
    }
    return {};
}

bool runtimeHasEngineDir(const std::filesystem::path& runtimeDir)
{
    std::error_code ec;
    return std::filesystem::is_directory(runtimeDir / HE::GameLayout::kEngineDirName, ec);
}

std::string exportedExecutableName(const std::string& projectName, bool windowsExe)
{
    std::string name;
    for (const char c : projectName)
    {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || std::strchr("/\\:*?\"<>|", c)) continue;
        name += c;
    }
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (name.empty()) name = "Game";
    return windowsExe ? name + ".exe" : name;
}

// ─── Embedded pak key ─────────────────────────────────────────────────────────
// Block layout (ABI, see HE_Game/src/EmbeddedPakKey.h):
//   magic[24] | hasKey(1) | pad(7) | key(32)  — 64 bytes total.

static std::string embeddedKeyMagic()
{
    // Assembled from pieces so the contiguous 24-byte pattern exists in no
    // binary except the game's real key block (a literal here would also live
    // in libHorizonCore and be falsely patched as a "block").
    std::string m = "HE_EMBEDDED_";
    m += "PAKKEY_V1";
    m.append(24 - m.size(), '\0');
    return m;
}

static bool readWholeFile(const std::filesystem::path& p, std::vector<uint8_t>& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return f.good() || f.eof();
}

// Mach-O detection (needed on every host: cross-exports can carry macOS
// runtimes, and a patched Mach-O MUST be re-signed or arm64 kills it on launch).
static bool looksMachO(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() < 4) return false;
    const uint32_t w = static_cast<uint32_t>(bytes[0])
                     | static_cast<uint32_t>(bytes[1]) << 8
                     | static_cast<uint32_t>(bytes[2]) << 16
                     | static_cast<uint32_t>(bytes[3]) << 24;
    return w == 0xFEEDFACFu || w == 0xFEEDFACEu   // MH_MAGIC_64 / MH_MAGIC
        || w == 0xBEBAFECAu || w == 0xCAFEBABEu;  // FAT magics (either order)
}

// A path as one shell argument: wrapped in single quotes, embedded quotes
// escaped as '\''. An apostrophe in an export path must not break a command —
// that would skip a re-sign, not just look ugly.
static std::string shellQuote(const std::filesystem::path& p)
{
    std::string out = "'";
    for (const char c : p.string()) out += (c == '\'') ? "'\\''" : std::string(1, c);
    return out + "'";
}

#if defined(__APPLE__) || defined(__linux__)
// Drop the LOCAL symbol table from a copied binary. `-x` and nothing more: the
// exported symbols stay, so a crash backtrace out of a shipped build still names
// functions, and a game-logic module still finds what it links against. What
// goes is the file-local half nobody outside the build ever reads — on this
// machine about a fifth of every engine library.
//
// Best effort: a host without `strip` leaves the file exactly as copied, which
// is what shipped before this existed. The caller re-signs on Apple, because a
// stripped Mach-O has an invalid signature and arm64 kills those on launch.
static bool stripLocalSymbols(const std::filesystem::path& p)
{
    return std::system(("/usr/bin/strip -x " + shellQuote(p) + " 2>/dev/null").c_str()) == 0;
}
#endif

#ifdef __APPLE__
// Ad-hoc re-sign in place. Patching invalidated the signature; on arm64 macOS
// an invalid signature means the binary is killed on launch, so a failed
// re-sign must FAIL the export — never ship silently unrunnable.
static bool resignMachO(const std::filesystem::path& p)
{
    // Shell-quote the path: wrap in single quotes, escaping embedded single
    // quotes as '\'' (an apostrophe in the export path must not break the
    // command — that would skip the re-sign, not just look ugly).
    const std::string cmd = "/usr/bin/codesign --force --sign - " + shellQuote(p) + " 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
}
#endif

int patchEmbeddedPakKey(const std::filesystem::path& binary, const uint8_t key[32])
{
    std::vector<uint8_t> bytes;
    if (!readWholeFile(binary, bytes)) return -1;

#ifndef __APPLE__
    // A Mach-O runtime patched on a non-Apple host cannot be re-signed here —
    // the result would be killed on launch on Apple Silicon. Leave the binary
    // untouched (signature stays valid); the caller then ships the key in
    // project.hcfg, which the runtime uses as its fallback.
    if (looksMachO(bytes)) return 0;
#endif

    const std::string magic = embeddedKeyMagic();
    if (bytes.size() < 64) return 0;

    int patched = 0;
    // Patch EVERY occurrence: universal (fat) binaries carry one block per
    // architecture slice.
    auto it = bytes.begin();
    while (true)
    {
        it = std::search(it, bytes.end(), magic.begin(), magic.end());
        if (it == bytes.end()) break;
        const size_t off = static_cast<size_t>(it - bytes.begin());
        if (off + 64 > bytes.size()) break;
        bytes[off + 24] = 1;                        // hasKey
        std::memcpy(bytes.data() + off + 32, key, 32);
        ++patched;
        it += 64;
    }
    if (patched == 0) return 0;

    // Write temp + rename: an in-place trunc rewrite that fails mid-stream
    // (disk full) would leave a corrupt half-written executable behind. The
    // temp file inherits fresh permissions, so the original's (notably +x)
    // are copied over before the swap.
    std::error_code ec;
    const auto perms = std::filesystem::status(binary, ec).permissions();
    const std::filesystem::path tmp = binary.string() + ".keytmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return -1;
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        f.close();
        if (f.fail())
        {
            std::filesystem::remove(tmp, ec);
            return -1;
        }
    }
    if (!ec) std::filesystem::permissions(tmp, perms, ec);
    ec.clear();
    std::filesystem::rename(tmp, binary, ec);
    if (ec)
    {
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        return -1;
    }

#ifdef __APPLE__
    if (looksMachO(bytes) && !resignMachO(binary))
        return -2; // patched but unsigned = killed on launch; caller must fail
#endif
    return patched;
}

bool readEmbeddedPakKey(const std::filesystem::path& binary, uint8_t outKey[32])
{
    std::vector<uint8_t> bytes;
    if (!readWholeFile(binary, bytes) || bytes.size() < 64) return false;

    const std::string magic = embeddedKeyMagic();
    auto it = std::search(bytes.begin(), bytes.end(), magic.begin(), magic.end());
    if (it == bytes.end()) return false;
    const size_t off = static_cast<size_t>(it - bytes.begin());
    if (off + 64 > bytes.size() || bytes[off + 24] != 1) return false;
    std::memcpy(outKey, bytes.data() + off + 32, 32);
    return true;
}

// ─── macOS .app bundle ────────────────────────────────────────────────────────

// Reverse-DNS bundle id from the project name: keep [A-Za-z0-9-], collapse the
// rest, lower-case. Empty → "game" so the id is always well-formed.
static std::string bundleIdentifier(const std::string& projectName)
{
    std::string s;
    for (char c : projectName)
    {
        if ((c >= 'A' && c <= 'Z')) s += static_cast<char>(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') s += c;
    }
    if (s.empty()) s = "game";
    return "com.horizonengine." + s;
}

static std::string xmlEscape(const std::string& in)
{
    std::string out;
    for (char c : in)
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c;
        }
    return out;
}

static bool writeInfoPlist(const std::filesystem::path& contentsDir,
                           const std::string& exeName,
                           const std::string& projectName,
                           const std::string& displayName,
                           const std::string& bundleId,
                           const std::string& version,
                           bool hasIcon,
                           const std::vector<HE::AppDocumentType>& docTypes)
{
    // XML-escape the display name (titles can contain & < > " '). The title
    // is what the Dock and Finder show; the identifier below still derives
    // from the PROJECT name, so retitling a game does not change its identity.
    const std::string name = xmlEscape(displayName.empty() ? projectName : displayName);

    // An empty identifier means "derive it", which is what every export did
    // before the field existed; a version that is empty means the same for 1.0.
    const std::string ident = bundleId.empty() ? bundleIdentifier(projectName) : bundleId;
    const std::string ver   = version.empty() ? std::string("1.0") : version;

    const std::string plist =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "  <key>CFBundleExecutable</key><string>" + xmlEscape(exeName) + "</string>\n"
        "  <key>CFBundleIdentifier</key><string>" + ident + "</string>\n"
        "  <key>CFBundleName</key><string>" + name + "</string>\n"
        "  <key>CFBundleDisplayName</key><string>" + name + "</string>\n"
        "  <key>CFBundlePackageType</key><string>APPL</string>\n"
        "  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>\n"
        "  <key>CFBundleVersion</key><string>" + ver + "</string>\n"
        "  <key>CFBundleShortVersionString</key><string>" + ver + "</string>\n" +
        // Named only when the file is actually there: a bundle that points at a
        // missing icon shows the generic one AND makes Finder cache that.
        (hasIcon ? "  <key>CFBundleIconFile</key><string>AppIcon</string>\n" : "") +
        // Which files are this application's, and what the system should call
        // them. Empty when the project claims none, and then the plist reads
        // exactly as it did before document types existed.
        HE::heInfoPlistDocumentTypes(docTypes, ident) +
        "  <key>NSHighResolutionCapable</key><true/>\n"
        "  <key>LSMinimumSystemVersion</key><string>11.0</string>\n"
        "</dict>\n</plist>\n";

    std::ofstream f(contentsDir / "Info.plist", std::ios::trunc);
    if (!f) return false;
    f << plist;
    f.close();
    return !f.fail();
}

#ifdef __APPLE__
// Ad-hoc code-sign the whole bundle (executable + nested dylibs + seal). Without
// a valid signature Apple Silicon kills the app at launch, so a sign failure is
// a hard error — never ship a silently-unrunnable .app.
static bool signAppBundle(const std::filesystem::path& appPath)
{
    std::string quoted = "'";
    for (const char c : appPath.string())
        quoted += (c == '\'') ? "'\\''" : std::string(1, c);
    quoted += "'";
    const std::string cmd =
        "/usr/bin/codesign --force --deep --sign - " + quoted + " 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
}
#endif

HE::UUID sceneUuidForPath(const std::string& projectRelPath)
{
    // Normalize to forward slashes so the SAME path always hashes the same on
    // every platform and caller.
    std::string p = projectRelPath;
    for (char& c : p) if (c == '\\') c = '/';
    auto fnv1a = [](const char* s, size_t n, uint64_t seed) {
        uint64_t h = seed;
        for (size_t i = 0; i < n; ++i) { h ^= (uint8_t)s[i]; h *= 1099511628211ull; }
        return h;
    };
    HE::UUID u{};
    u.hi = fnv1a(p.data(), p.size(), 14695981039346656037ull);
    const std::string r(p.rbegin(), p.rend());
    u.lo = fnv1a(r.data(), r.size(), 1099511628211ull ^ 14695981039346656037ull);
    return u;
}

std::string levelScriptKeyForUuid(const HE::UUID& sceneUuid)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "level:%016llx%016llx",
                  (unsigned long long)sceneUuid.hi, (unsigned long long)sceneUuid.lo);
    return buf;
}

// ─── exportProject phases ─────────────────────────────────────────────────────
// exportProject is one linear pipeline; every phase below is one step of it and
// returns std::nullopt on success, or the FAILED ExportResult that the caller
// hands straight back. Every one of those failures is deliberately HARD: this
// writes shipping builds, where a silently missing executable, a missing key or
// an unsigned bundle costs far more than a failed export.

namespace
{
// The state the phases hand to each other: the resolved output layout, the pack
// settings derived from the export settings, and the counters the ExportResult
// reports.
struct ExportContext
{
    // Layout: a macOS .app splits the export in two — the executable + engine
    // dylibs live in Contents/MacOS (found via @executable_path rpath) while the
    // pak, project.hcfg, loose scene and GameLogic live in Contents/Resources
    // (where SDL_GetBasePath resolves inside a bundle). A flat export collapses
    // both to outputDir. Everything below routes through binDir / dataDir so the
    // two layouts share one code path.
    bool                  app = false;
    std::filesystem::path appPath;      // <outputDir>/<name>.app, empty unless `app`
    std::filesystem::path binDir;       // game executable + engine dylibs
    std::filesystem::path dataDir;      // pak, project.hcfg, loose scene, GameLogic

    // The runtime has HorizonEngine/ (runtimeHasEngineDir): the export gets
    // <exeName> + data/ + HorizonEngine/ (GameLayout.h). Otherwise the old flat
    // layout, executable named as the runtime names it.
    bool                  engineLayout = false;
    std::filesystem::path engineDir;    // HorizonEngine/ beside the executable
    std::string           exeName = "HorizonGame";

    Hpak::PackSettings    packSettings;
    std::string           hpakFilename;
    std::string           sceneFile;    // loose startup-scene copy (empty when packed)
    HE::UUID              sceneUuid{};  // pak entry of the packed startup scene

    int  assetsPacked = 0;
    int  assetsReused = 0;
    int  binaryCopied = 0;              // game exe + dylibs (+ the HorizonCode lib)
    bool keyEmbedded  = false;
    bool hcGenShipped = false;
};
} // namespace

// Phase 1: resolve the output layout (flat folder vs. .app bundle) and create it.
static std::optional<ExportResult> prepareOutputLayout(
    const std::filesystem::path& outputDir,
    const std::string&           projectName,
    const ExportSettings&        settings,
    ExportContext&               ctx)
{
    std::error_code ec;

    std::filesystem::create_directories(outputDir, ec);
    if (ec) return ExportResult{false, "Cannot create output dir: " + ec.message(), 0};

    ctx.app     = settings.appBundle;
    ctx.appPath = ctx.app ? outputDir / (projectName + ".app") : std::filesystem::path{};
    ctx.binDir  = ctx.app ? ctx.appPath / "Contents" / "MacOS"     : outputDir;
    ctx.dataDir = ctx.app ? ctx.appPath / "Contents" / "Resources" : outputDir;

    // The layout follows the runtime: one with HorizonEngine/ finds its data in
    // data/ (a .app keeps Contents/Resources, which is its base path), one
    // without looks beside its executable and must get the flat layout.
    const bool windowsExe = !settings.gameRuntimeDir.empty()
        && std::filesystem::is_regular_file(settings.gameRuntimeDir / "HorizonGame.exe", ec);
    ec.clear();
    ctx.engineLayout = !settings.gameRuntimeDir.empty()
                    && runtimeHasEngineDir(settings.gameRuntimeDir);
    if (ctx.engineLayout)
    {
        ctx.exeName   = exportedExecutableName(projectName, windowsExe);
        ctx.engineDir = ctx.binDir / HE::GameLayout::kEngineDirName;
        if (!ctx.app) ctx.dataDir = outputDir / HE::GameLayout::kDataDirName;
    }
    else if (windowsExe)
    {
        ctx.exeName = "HorizonGame.exe";
    }

    if (ctx.app || ctx.engineLayout)
    {
        std::filesystem::create_directories(ctx.binDir, ec);
        std::filesystem::create_directories(ctx.dataDir, ec);
        if (ec) return ExportResult{false, (ctx.app ? "Cannot create .app bundle: "
                                                    : "Cannot create the data folder: ")
                                           + ec.message(), 0};
    }
    return std::nullopt;
}

// Phase 2: derive the pak's PackSettings — codec, encryption key (reusing the
// previous export's key for incremental packs) and the cook/precompile knobs.
static std::optional<ExportResult> resolvePackSettings(const ExportSettings& settings,
                                                       ExportContext&        ctx)
{
    Hpak::PackSettings& packSettings = ctx.packSettings;

    // Map the export "compress" toggle to the best codec available at build time
    // (zstd preferred for ship builds, LZ4 otherwise). Store when off.
    if (settings.compress)
    {
#if defined(HE_HAVE_ZSTD)
        packSettings.codec = Hpak::Codec::Zstd;
#elif defined(HE_HAVE_LZ4)
        packSettings.codec = Hpak::Codec::LZ4;
#else
        packSettings.codec = Hpak::Codec::Store;
#endif
    }
    packSettings.encrypt = settings.encrypt;
    if (settings.encrypt && settings.planned)
    {
        // planExport already went through the sources below, and a game may
        // have been compiled with its answer since: that key and no other.
        std::memcpy(packSettings.key, settings.plannedPakKey, 32);
    }
    else if (settings.encrypt)
    {
        // Ship a random 256-bit key in project.hcfg (deriving from a passphrase
        // buys nothing when the key ships with the game anyway). Incremental
        // exports REUSE the previous export's key so unchanged encrypted
        // entries can be carried over byte-verbatim.
        bool haveKey = false;
        if (settings.incremental)
        {
            // Previous key sources, in order: project.hcfg (legacy exports keep
            // the key there) — but only a NON-ZERO key (embedded-key exports
            // deliberately zero it); then the key patched into the previous
            // export's game executable.
            ProjectConfig prevCfg;
            if (ProjectConfigLoader::load(ctx.dataDir, prevCfg) && prevCfg.encrypted)
            {
                bool nonZero = false;
                for (int i = 0; i < 32; ++i) nonZero |= (prevCfg.encKey[i] != 0);
                if (nonZero)
                {
                    std::memcpy(packSettings.key, prevCfg.encKey, 32);
                    haveKey = true;
                }
            }
            if (!haveKey)
            {
                for (const std::string& exe : { ctx.exeName, std::string("HorizonGame"),
                                                std::string("HorizonGame.exe") })
                    if (readEmbeddedPakKey(ctx.binDir / exe, packSettings.key))
                    { haveKey = true; break; }
            }
            // A game compiled from the SDK has neither: its key lives in the
            // header it was built from.
            if (!haveKey && !settings.previousBuildConfigHeader.empty() &&
                HE::readGameBuildConfigPakKey(settings.previousBuildConfigHeader, packSettings.key))
                haveKey = true;
        }
        if (!haveKey && !Hpak::randomBytes(packSettings.key, 32))
            return ExportResult{false, "Crypto backend unavailable — cannot encrypt", 0};
    }

    packSettings.excludePatterns = settings.excludePatterns;
    packSettings.cook = true; // always cook exports into the runtime-optimal form
    packSettings.textureCompression = settings.textureCompression;
    packSettings.textureQuality     = settings.textureQuality;
    packSettings.shaderBackends        = settings.shaderBackends;        // precompile material shaders
    packSettings.compileShaderVariants = settings.compileShaderVariants;
    packSettings.compileParticleShaderVariants = settings.compileParticleShaderVariants;
    packSettings.onAsset = settings.onAsset;
    packSettings.onWarning = settings.onWarning;
    return std::nullopt;
}

// Phase 3a: the scene entries that ride along in the pak — the startup scene, every
// other project scene, the scene index and the app-wide GameInstance graph.
static void addSceneEntries(
    HpakWriter&                  packer,
    const std::vector<uint8_t>&  startupSceneBinary,
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& extraScenes,
    const std::string&           gameInstanceJson,
    ExportContext&               ctx)
{
    const Hpak::PackSettings& packSettings = ctx.packSettings;

    // Pack the startup scene as a binary entry INTO the pak (if the caller
    // serialized one), under a fresh UUID recorded in the hcfg. Same codec +
    // encryption as the assets. Must happen before write().
    if (!startupSceneBinary.empty())
    {
        // A planned export brings its UUID along (planExport): the game built
        // before the pack has it compiled in.
        if (ctx.sceneUuid == HE::UUID{}) ctx.sceneUuid = HE::UUID::generate();
        packer.addEntry(ctx.sceneUuid, startupSceneBinary, packSettings);
    }

    // Every other project scene rides along under a path-derived UUID so the
    // game runtime can scene.load("<project-relative path>") for level
    // transitions. Same codec + encryption as the assets.
    for (const auto& [relPath, bytes] : extraScenes)
        if (!bytes.empty())
            packer.addEntry(sceneUuidForPath(relPath), bytes, packSettings);
    // …plus a scene INDEX (a JSON string array of those paths) under a
    // well-known name, so scene.available() can enumerate scenes in shipped
    // builds (pak entries are UUID-keyed — paths aren't recoverable from them).
    if (!extraScenes.empty())
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& scene : extraScenes)
            arr.push_back(scene.first);   // project-relative path
        // "replace" error handler: a scene path is a filesystem string and may hold
        // bytes that are not valid UTF-8, on which the default dump() THROWS
        // (type_error.316) — the same crash ProjectManager::saveProject hit with a
        // project name. Substitute U+FFFD instead of failing the whole export.
        const std::string index = arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        packer.addEntry(sceneUuidForPath(kSceneIndexEntry),
                        std::vector<uint8_t>(index.begin(), index.end()), packSettings);
    }

    // App-wide GameInstance graph (project GameInstance.hcode). It drives OnInit
    // and the app lifecycle — and, crucially, is where a game commonly creates
    // its UI (OnInit → Create Object → createWidget). Packed into the .hpak it
    // ships with the same codec/encryption/bundle layout; the game loads it via
    // readMountedEntry, falling back to a loose file for dev. Was never shipped
    // before → OnInit ran on an empty graph → no UI in packaged builds.
    if (!gameInstanceJson.empty())
        packer.addEntry(sceneUuidForPath(kGameInstanceEntry),
                        std::vector<uint8_t>(gameInstanceJson.begin(), gameInstanceJson.end()),
                        packSettings);
}

// Phase 3b: the pak's asset path index.
static void addAssetPathIndex(HpakWriter& packer, const ExportContext& ctx)
{
    // Asset path index: content-relative path → "hi:lo" UUID for every packed
    // asset, so the game can resolve loadAsset("<path>") to a mounted-pak entry.
    // Without it, an asset the scene's UUID reference closure never reaches — the
    // classic case being a widget a HorizonCode script creates by path — can't be
    // found in a pak-only build (pak entries are UUID-keyed; the path is gone),
    // and its UI silently never appears. Same codec + encryption as the assets.
    if (!packer.packedPaths().empty())
    {
        nlohmann::json idx = nlohmann::json::object();
        for (const auto& [relPath, id] : packer.packedPaths())
            idx[relPath] = std::to_string(id.hi) + ":" + std::to_string(id.lo);
        const std::string s = idx.dump();
        packer.addEntry(sceneUuidForPath(kAssetPathIndexEntry),
                        std::vector<uint8_t>(s.begin(), s.end()), ctx.packSettings);
    }
}

// Phase 3c: the pak's asset TYPE index — "hi:lo" → HE::AssetType for every
// packed asset (see kAssetTypeIndexEntry).
static void addAssetTypeIndex(HpakWriter& packer, const ExportContext& ctx)
{
    // A mounted pak contributes residency and paths, but no types — and a
    // ContentManager only learns an asset's type by loading it. So in a shipped
    // game discoverAssets(HorizonCodeClass), the call PlayerHost uses to find the
    // startup controller classes, asked about assets nothing had touched yet and
    // came back empty: no controller, no BeginPlay, no player in the world.
    //
    // Every packed asset is listed, not just the types PlayerHost happens to ask
    // for — discoverAssets is a general question and the next caller should not
    // have to come back here. The reserved entries (path/scene/type index, scenes,
    // GameInstance) are excluded by construction: packedTypes() is filled by the
    // content scan only, and those are JSON metadata, not assets.
    // Same codec + encryption as the assets.
    if (packer.packedTypes().empty()) return;
    nlohmann::json idx = nlohmann::json::object();
    for (const auto& [id, type] : packer.packedTypes())
        idx[std::to_string(id.hi) + ":" + std::to_string(id.lo)] =
            static_cast<uint32_t>(type);
    const std::string s = idx.dump();   // keys are digits + ':', values numbers: always valid UTF-8
    packer.addEntry(sceneUuidForPath(kAssetTypeIndexEntry),
                    std::vector<uint8_t>(s.begin(), s.end()), ctx.packSettings);
}

// Phase 3d: the pak's user-type index — the paths of every packed Struct/Enum/
// SaveGameTemplate definition asset, so the game can load them eagerly before the
// script backends bootstrap (see kTypeIndexEntry). The types come from the pack
// scan, which already read each asset's header; re-opening every file of the
// project here to sniff it a second time bought nothing.
static void addTypeIndex(HpakWriter& packer, const ExportContext& ctx)
{
    nlohmann::json idx = nlohmann::json::array();
    for (const auto& [relPath, id] : packer.packedPaths())
    {
        const auto it = packer.packedTypes().find(id);
        if (it == packer.packedTypes().end()) continue;
        if (it->second == static_cast<uint16_t>(HE::AssetType::StructType) ||
            it->second == static_cast<uint16_t>(HE::AssetType::EnumType) ||
            it->second == static_cast<uint16_t>(HE::AssetType::SaveGameTemplate))
            idx.push_back(relPath);
    }
    if (idx.empty()) return;
    const std::string s = idx.dump();
    packer.addEntry(sceneUuidForPath(kTypeIndexEntry),
                    std::vector<uint8_t>(s.begin(), s.end()), ctx.packSettings);
}

// Phase 3: pack the content directory (reusing unchanged entries from the previous
// export when possible), add the scene/index entries, write the .hpak and persist
// the manifest the NEXT incremental export reads.
static std::optional<ExportResult> packContent(
    const std::filesystem::path& contentDir,
    const std::string&           projectName,
    const ExportSettings&        settings,
    const std::vector<uint8_t>&  startupSceneBinary,
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& extraScenes,
    const std::string&           gameInstanceJson,
    ExportContext&               ctx)
{
    ctx.hpakFilename        = projectName + ".hpak";
    const auto pakPath      = ctx.dataDir / ctx.hpakFilename;
    const auto manifestPath = ctx.dataDir / (ctx.hpakFilename + ".manifest");

    // Incremental cache: previous pak + its manifest, gated on the manifest
    // describing exactly that pak (tocHash) built with these settings
    // (Hpak/PackManifest.h, shared with the hpak_packer CLI).
    auto prevPak = std::make_unique<HpakReader>();
    Hpak::IncrementalCache cache;
    const bool haveCache = settings.incremental
        && HE::openIncrementalCache(pakPath, manifestPath, ctx.packSettings, *prevPak, cache);

    // The project's own Content first, then the engine defaults under "Engine/",
    // then the EngineContent download cache behind them — the three tiers of
    // ContentManager::resolveAbsolutePath, in its order. Order matters: a
    // project-local override (Content/Engine/…) carries the same UUID as the
    // default it shadows and must be the one that ships.
    std::vector<HpakWriter::SourceRoot> roots{ { contentDir, {} } };
    if (!settings.engineContentDir.empty())
    {
        roots.push_back({ settings.engineContentDir, "Engine/" });
        if (!settings.engineContentCacheDir.empty())
            roots.push_back({ settings.engineContentCacheDir, "Engine/", /*fallbackOnly*/ true });
    }

    HpakWriter packer;
    ctx.assetsPacked = packer.addDirectories(roots, ctx.packSettings, settings.progress,
                                             haveCache ? &cache : nullptr);
    ctx.assetsReused = packer.reusedCount();
    prevPak.reset(); // release the read handle BEFORE write() replaces the file

    addSceneEntries(packer, startupSceneBinary, extraScenes, gameInstanceJson, ctx);
    addAssetPathIndex(packer, ctx);
    addAssetTypeIndex(packer, ctx);
    addTypeIndex(packer, ctx);

    if (!packer.write(pakPath.string()))
        return ExportResult{false, "Failed to write " + ctx.hpakFilename, 0};

    // Persist the manifest for the NEXT incremental export. Best-effort cache:
    // if the reopen or write fails, the next export simply packs everything.
    HE::writeIncrementalManifest(pakPath, manifestPath, ctx.packSettings, packer.entryInfos());
    return std::nullopt;
}

// Phase 4: loose startup-scene fallback — only when no binary scene was packed.
static void copyLooseStartupScene(const std::filesystem::path& contentDir,
                                  const std::string&           startupSceneName,
                                  const std::vector<uint8_t>&  startupSceneBinary,
                                  ExportContext&               ctx)
{
    std::error_code ec;

    if (startupSceneBinary.empty() && !startupSceneName.empty())
    {
        const auto sceneSrc = contentDir / startupSceneName;
        ctx.sceneFile = std::filesystem::path(startupSceneName).filename().string();
        if (std::filesystem::exists(sceneSrc, ec))
            std::filesystem::copy_file(sceneSrc, ctx.dataDir / ctx.sceneFile,
                std::filesystem::copy_options::overwrite_existing, ec);
    }
}

// Phase 5: copy game runtime binaries (executable + dylibs) so the export is
// runnable. This happens BEFORE project.hcfg is written: with encryption the key
// is patched into the copied game executable, and only if that succeeds is the
// key omitted from the hcfg. Non-throwing iteration: this runs on the editor's
// export worker thread, where an escaped filesystem_error would be std::terminate.
static std::optional<ExportResult> copyRuntimeBinaries(const ExportSettings& settings,
                                                       ExportContext&        ctx)
{
    std::error_code ec;

    if (!settings.gameRuntimeDir.empty())
    {
        // A named-but-missing runtime dir must FAIL, not silently ship a
        // data-only export (an existence gate here previously skipped the
        // whole block, including all its error checks).
        if (!std::filesystem::is_directory(settings.gameRuntimeDir, ec))
            return ExportResult{false, "Game runtime dir not found: "
                                       + settings.gameRuntimeDir.string(), ctx.assetsPacked};

        const auto isRuntimeExe = [](const std::string& n) {
            return n == "HorizonGame" || n == "HorizonGame.exe";
        };
        // The POSIX stdlib path file is named after the executable
        // (HorizonGame._pth), so it is renamed along with it.
        const auto isExePth = [](const std::string& n) { return n == "HorizonGame._pth"; };

        // Route each runtime file to the right place for the .app layout: the
        // executable and engine dylibs go next to the exe (binDir); GameLogic and
        // anything else (config.json) goes with the data (dataDir) so the running
        // game finds it via HE::GameLayout::dataDir. Flat exports of an old runtime
        // collapse both to outputDir, so the routing is a no-op there.
        auto routeRuntime = [&](const std::string& n) -> std::filesystem::path {
            if (isRuntimeExe(n)) return ctx.binDir / ctx.exeName;
            if (isExePth(n))
                return ctx.binDir / (std::filesystem::path(ctx.exeName).stem().string() + "._pth");
            const bool gameLogic = n.rfind("GameLogic.", 0) == 0; // loaded from base path
            // The bundled CPython runtime (python3XY.dll/.zip + python3XY._pth) must sit next
            // to the executable so embedded Python's isolated-path lookup finds it; route any
            // "python*" file to binDir. (Flat exports collapse binDir==dataDir, so this only
            // matters for the macOS .app split and the data/ folder.)
            const bool pythonRuntime = n.rfind("python", 0) == 0;
            const bool pthFile = n.size() > 5 && n.compare(n.size() - 5, 5, "._pth") == 0;
            const bool engineBin = !gameLogic
                && (pythonRuntime || pthFile
                    || n.size() > 4 && (n.compare(n.size() - 4, 4, ".dll") == 0)
                    || n.size() > 3 && (n.compare(n.size() - 3, 3, ".so") == 0)
                    || n.size() > 6 && (n.compare(n.size() - 6, 6, ".dylib") == 0));
            return (engineBin ? ctx.binDir : ctx.dataDir) / n;
        };

        // Everything Python ships only for Python-language projects: the
        // stdlib (pythonXY.zip + <Exe>._pth), the interpreter itself
        // (python3XY.dll / libpython3.X.so / .dylib) AND the backend
        // plugin that loads it.
        //
        // The interpreter can be skipped now, which it could not before.
        // It used to be a LOAD-TIME dependency of HorizonScene, so a game
        // without it did not start at all — every non-Python game carried
        // 5-9 MB of CPython to satisfy a link it never used. The backend
        // is a runtime-loaded plugin now (HorizonPython), so leaving both
        // out costs exactly the feature the project does not use.
        const auto skippedPython = [&settings](const std::string& fname) {
            if (settings.bundlePython) return false;
            const auto endsWith = [&fname](const char* suf) {
                const size_t n = std::strlen(suf);
                return fname.size() > n && fname.compare(fname.size() - n, n, suf) == 0;
            };
            const bool isZip    = endsWith(".zip");
            const bool isPth    = endsWith("._pth");
            // "python314.dll", "libpython3.14.dylib", "libpython3.12.so.1.0"
            const bool isPyLib  = fname.find("python") != std::string::npos
                               && fname.find("Horizon") == std::string::npos;
            const bool isPlugin = fname.find("HorizonPython") != std::string::npos;
            return isZip || isPth || isPyLib || isPlugin;
        };

        // Clear STALE code before copying fresh binaries. Re-signing over a
        // previous export's leftovers is the classic codesign failure ("bundle
        // format unrecognized" / "code object is not signed"): an old
        // _CodeSignature seal no longer matches, and stale dylibs from an older
        // runtime linger in the bundle. For a .app, Contents/MacOS is disjoint
        // from Contents/Resources, so wiping it does NOT touch the pak/hcfg the
        // incremental cache already read above — recreate it empty. (Flat
        // exports share one dir with the just-written pak, so they only remove
        // each destination individually, below; HorizonEngine/ holds nothing
        // but runtime files and is replaced as a whole.)
        if (ctx.app)
        {
            std::filesystem::remove_all(ctx.appPath / "Contents" / "_CodeSignature", ec); ec.clear();
            std::filesystem::remove_all(ctx.binDir, ec); ec.clear();
            std::filesystem::create_directories(ctx.binDir, ec); ec.clear();
        }
        if (ctx.engineLayout)
        {
            std::filesystem::remove_all(ctx.engineDir, ec); ec.clear();
            // A previous export of the old flat layout into the same folder left
            // its executable there; two games side by side, one of them stale.
            if (ctx.exeName != "HorizonGame" && ctx.exeName != "HorizonGame.exe")
                for (const char* old : { "HorizonGame", "HorizonGame.exe" })
                {
                    std::filesystem::remove(ctx.binDir / old, ec); ec.clear();
                }
        }

        // Every file in the bundle is required (executable AND its libraries):
        // any copy failure is a hard error. A silently skipped executable is
        // the worst case — the output would keep a STALE previously-exported
        // exe whose embedded key no longer matches this pak.
        std::vector<std::filesystem::path> copied;
        const auto shipFile = [&](const std::filesystem::path& src,
                                  const std::filesystem::path& dst) -> std::optional<ExportResult>
        {
            std::error_code fec;
            // Delete any existing file first (fresh inode): copying over a
            // code-signed / currently-mapped binary in place can leave a
            // stale signature or a busy-file error on re-export.
            std::filesystem::remove(dst, fec); fec.clear();
            std::filesystem::copy_file(src, dst,
                std::filesystem::copy_options::overwrite_existing, fec);
            if (fec)
                return ExportResult{false, "Failed to copy runtime binary "
                                           + src.filename().string() + ": "
                                           + fec.message(), ctx.assetsPacked};
            // Symbols the shipped build has no use for, dropped from the
            // COPY (the developer's own binaries keep theirs). Only the
            // file-local half — see stripLocalSymbols.
#if defined(__APPLE__) || defined(__linux__)
            {
                // Four bytes, not the whole file: the biggest thing here is
                // a 14 MB interpreter and all this asks is the magic.
                std::vector<uint8_t> head(4, 0);
                {
                    std::ifstream f(dst, std::ios::binary);
                    f.read(reinterpret_cast<char*>(head.data()), 4);
                    if (!f) head.clear();
                }
                const bool machO = looksMachO(head);
                if (stripLocalSymbols(dst))
                {
#ifdef __APPLE__
                    // Stripping invalidates the signature, and arm64 kills a
                    // binary with a broken one on launch — so a re-sign that
                    // fails has to fail the export, exactly like the key
                    // patch's does. The .app's later --deep sign would cover
                    // the bundle case, but a plain-folder export has no such
                    // second chance.
                    if (machO && !resignMachO(dst))
                        return ExportResult{false, "Failed to re-sign "
                                                   + dst.filename().string()
                                                   + " after stripping symbols",
                                            ctx.assetsPacked};
#else
                    (void)machO;
#endif
                }
            }
#endif
            ++ctx.binaryCopied;
            copied.push_back(dst);
            return std::nullopt;
        };

        std::filesystem::directory_iterator dit(settings.gameRuntimeDir, ec);
        const std::filesystem::directory_iterator dend;
        while (!ec && dit != dend)
        {
            const bool regular = dit->is_regular_file(ec);
            if (ec) { ec.clear(); dit.increment(ec); continue; }
            if (regular)
            {
                const std::string fname = dit->path().filename().string();
                // The log of a run from the deploy folder is not part of the runtime.
                const bool runLog = fname.size() > 4 && fname.compare(fname.size() - 4, 4, ".log") == 0;
                // With the HorizonEngine/ layout the engine is that one folder: a
                // Horizon*.dll beside the executable is a leftover of an older
                // runtime (HorizonAsset/HorizonTools were never part of the game)
                // that the editor's wholesale copy of the runtime folder carried
                // along. The game's loader never looks at it.
                const bool strayEngineDll = ctx.engineLayout && fname.rfind("Horizon", 0) == 0
                    && fname.size() > 4 && fname.compare(fname.size() - 4, 4, ".dll") == 0;
                if (runLog || strayEngineDll || skippedPython(fname)) { dit.increment(ec); continue; }
                if (auto fail = shipFile(dit->path(), routeRuntime(fname))) return fail;
            }
            dit.increment(ec);
        }
        ec.clear();

        // The engine folder: the HorizonEngine library, SDL3, the bundled native
        // deps, the Python plugin, Shaders/. Shipped as it is, Python filtered
        // like above. Inside a .app the runtime shaders go to Resources/Shaders,
        // where the backends look from SDL_GetBasePath.
        if (ctx.engineLayout)
        {
            const auto srcEngine = settings.gameRuntimeDir / HE::GameLayout::kEngineDirName;
            std::filesystem::create_directories(ctx.engineDir, ec);
            if (ec)
                return ExportResult{false, "Cannot create " + ctx.engineDir.string() + ": "
                                           + ec.message(), ctx.assetsPacked};
            std::filesystem::recursive_directory_iterator rit(srcEngine, ec);
            const std::filesystem::recursive_directory_iterator rend;
            while (!ec && rit != rend)
            {
                const auto rel = rit->path().lexically_relative(srcEngine);
                const bool shader = !rel.empty() && *rel.begin() == "Shaders";
                const auto dst = (shader && ctx.app ? ctx.dataDir : ctx.engineDir) / rel;
                std::error_code fec;
                if (rit->is_directory(fec))
                {
                    std::filesystem::create_directories(dst, fec);
                }
                else if (!fec && rit->is_regular_file(fec))
                {
                    const std::string fname = rit->path().filename().string();
                    if (shader)
                    {
                        std::filesystem::create_directories(dst.parent_path(), fec);
                        std::filesystem::copy_file(rit->path(), dst,
                            std::filesystem::copy_options::overwrite_existing, fec);
                        if (fec)
                            return ExportResult{false, "Failed to copy runtime shader "
                                                       + fname + ": " + fec.message(),
                                                ctx.assetsPacked};
                    }
                    else if (!skippedPython(fname))
                    {
                        if (auto fail = shipFile(rit->path(), dst)) return fail;
                    }
                }
                rit.increment(ec);
            }
            if (ec)
                return ExportResult{false, "Failed to read " + srcEngine.string() + ": "
                                           + ec.message(), ctx.assetsPacked};
        }

        // A runtime dir that yields nothing is a broken export (data without an
        // executable) — the exact failure mode this parameter exists to prevent.
        if (ctx.binaryCopied == 0)
            return ExportResult{false, "Game runtime dir contained no files: "
                                       + settings.gameRuntimeDir.string(), ctx.assetsPacked};

        // Patch the pak key into the game executable's embedded key block.
        // Only the game exe carries the block; other copied files are skipped
        // cheaply by name. A runtime without the block (built before the block
        // existed) falls back to shipping the key in project.hcfg. Patch/sign
        // FAILURES are hard errors — the alternatives are a corrupt or
        // killed-on-launch executable shipped as "OK".
        if (settings.encrypt)
        {
            for (const auto& dst : copied)
            {
                const auto name = dst.filename().string();
                if (dst != ctx.binDir / ctx.exeName) continue;
                const int patched = patchEmbeddedPakKey(dst, ctx.packSettings.key);
                if (patched == -1)
                    return ExportResult{false, "Failed to embed the pak key into " + name
                                               + " (write error)", ctx.assetsPacked};
                if (patched == -2)
                    return ExportResult{false, "Failed to re-sign " + name
                                               + " after embedding the pak key (codesign)",
                                        ctx.assetsPacked};
                if (patched > 0) ctx.keyEmbedded = true;
            }
        }

        // Offline-compiled GPU shaders of a runtime from before HorizonEngine/
        // live in a Shaders/ SUBDIRECTORY beside the executable, which the flat
        // file loop above skipped: Vulkan's SPIR-V (*.spv) and D3D12's DXR
        // kernels (*.cso). Both backends load them from <base path>/Shaders/, so a
        // Vulkan game without it drew nothing at all ("scene shaders missing") and
        // D3D12 lost hardware GI. Routed like every non-binary runtime file (dataDir
        // = where SDL_GetBasePath points); replaced wholesale so a shader dropped
        // from the build does not linger from an older export. Missing source dir =
        // a runtime without these backends (Metal/GL-only), nothing to ship.
        {
            const auto srcShaders = settings.gameRuntimeDir / "Shaders";
            if (std::filesystem::is_directory(srcShaders, ec))
            {
                const auto dstShaders = ctx.dataDir / "Shaders";
                std::filesystem::remove_all(dstShaders, ec); ec.clear();
                std::filesystem::copy(srcShaders, dstShaders,
                    std::filesystem::copy_options::recursive
                    | std::filesystem::copy_options::overwrite_existing, ec);
                if (ec)
                    return ExportResult{false, "Failed to copy runtime Shaders/: "
                                               + ec.message(), ctx.assetsPacked};
            }
            ec.clear();
        }

        // Python C-extension modules live in a lib-dynload/ SUBDIRECTORY, which the
        // flat file loop above skipped. Ship it next to the executable (the
        // <Exe>._pth lists "lib-dynload") — only for Python-language projects, so a
        // non-Python game doesn't carry ~15 MB of unused .so. macOS: these are
        // signed framework binaries copied verbatim (signature intact); the .app's
        // final --deep sign re-seals them.
        if (settings.bundlePython)
        {
            const auto srcDyn = settings.gameRuntimeDir / "lib-dynload";
            if (std::filesystem::is_directory(srcDyn, ec))
            {
                const auto dstDyn = ctx.binDir / "lib-dynload";
                std::filesystem::remove_all(dstDyn, ec); ec.clear();
                std::filesystem::copy(srcDyn, dstDyn,
                    std::filesystem::copy_options::recursive
                    | std::filesystem::copy_options::overwrite_existing, ec);
                if (ec)
                    return ExportResult{false, "Failed to copy Python lib-dynload: "
                                               + ec.message(), ctx.assetsPacked};
            }
            ec.clear();
        }
    }
    return std::nullopt;
}

// Phase 6: compiled HorizonCode classes — ship the generated library under the
// canonical name the runtime loader probes. In the engine layout it goes into
// HorizonEngine/, beside the engine library it links (Thema 211 step 13); the
// flat layout keeps it with the data (base path, same place as GameLogic).
// A copy failure is a hard error: hcfg would otherwise claim "compiled" for a
// library that never shipped.
static std::optional<ExportResult> copyHorizonCodeLib(const ExportSettings& settings,
                                                      ExportContext&        ctx)
{
    std::error_code ec;

    // data/ is not wiped between exports: a copy an earlier export left there
    // would be hashed by the integrity probe and is never loaded.
    if (ctx.engineLayout)
    {
        std::filesystem::remove(ctx.dataDir / HorizonCode::compiledLibraryName(), ec);
        ec.clear();
    }
    if (!settings.horizonCodeGenLib.empty())
    {
        const auto dst = (ctx.engineLayout ? ctx.engineDir : ctx.dataDir)
                       / HorizonCode::compiledLibraryName();
        std::filesystem::remove(dst, ec); ec.clear();
        std::filesystem::copy_file(settings.horizonCodeGenLib, dst,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            return ExportResult{false, "Failed to copy the compiled HorizonCode library: "
                                       + ec.message(), ctx.assetsPacked};
        ++ctx.binaryCopied;
        ctx.hcGenShipped = true;
    }
    return std::nullopt;
}

// Phase 7: project.hcfg — the manifest the shipped game boots from.
// The configuration itself, without the key: where that goes depends on whether
// the runtime took it (writeProjectConfig) or on who asks (planExport).
static ProjectConfig makeProjectConfig(const std::string&    projectName,
                                       const ExportSettings& settings,
                                       bool                  packedScene,
                                       const ExportContext&  ctx)
{
    ProjectConfig cfg;
    cfg.projectName   = projectName;
    cfg.hpakFilename  = ctx.hpakFilename;
    cfg.mainSceneName = ctx.sceneFile;
    std::memset(cfg.projectUuidBytes, 0, 16);
    cfg.enableModSupport = settings.enableModSupport;
    cfg.devTools         = settings.devTools;
    cfg.encrypted = settings.encrypt;
    cfg.horizonCodeCompiled = ctx.hcGenShipped;
    cfg.defaultSaveTemplate = settings.defaultSaveTemplate;
    cfg.appMode               = settings.appProject;
    cfg.advancedShaderEffects = settings.advancedShaderEffects;
    cfg.allowFiles            = settings.allowFiles;
    cfg.allowProcesses        = settings.allowProcesses;
    cfg.allowNetwork          = settings.allowNetwork;
    cfg.fontScripts           = settings.fontScripts;
    cfg.fontWeightBold        = settings.fontWeightBold;
    cfg.collisionLayers       = settings.collisionLayers;
    cfg.audioBuses            = settings.audioBuses;
    // RESOLVED here, so the runtime never derives it a second time and reaches a
    // different answer than the bundle around it — but only when the answer
    // could not be derived anyway. A filled bundle id makes the writer choose
    // v5, and a runtime bundle from an older engine rejects every version it
    // does not know and boots WITHOUT its pak. Dropping a prebuilt runtime under
    // GameRuntimes/<Platform>/ is a documented way to work, so a game whose id
    // is exactly what the name derives to leaves the field empty and keeps
    // emitting the version that runtime can read. An APPLICATION always writes
    // it: it is what the autostart entry and the document types are filed under,
    // and there is no older runtime for applications to be compatible with.
    const std::string derivedId = bundleIdentifier(projectName);
    const std::string resolvedId = settings.bundleId.empty() ? derivedId : settings.bundleId;
    cfg.bundleId              = (settings.appProject || resolvedId != derivedId)
                                    ? resolvedId : std::string();
    cfg.theme                 = settings.theme;
    cfg.themeMode             = settings.themeMode;
    if (packedScene)
    {
        cfg.hasPackedScene = true;
        std::memcpy(cfg.startupSceneUuid,      &ctx.sceneUuid.hi, 8);
        std::memcpy(cfg.startupSceneUuid + 8,  &ctx.sceneUuid.lo, 8);
    }
    return cfg;
}

static std::optional<ExportResult> writeProjectConfig(const std::string&          projectName,
                                                      const ExportSettings&       settings,
                                                      const std::vector<uint8_t>& startupSceneBinary,
                                                      const ExportContext&        ctx)
{
    ProjectConfig cfg = makeProjectConfig(projectName, settings, !startupSceneBinary.empty(), ctx);
    // Key placement: inside the game executable when the patch succeeded (the
    // hcfg then carries only the encrypted flag), in the hcfg otherwise.
    if (settings.encrypt && !ctx.keyEmbedded)
        std::memcpy(cfg.encKey, ctx.packSettings.key, 32);

    if (!ProjectConfigLoader::save(ctx.dataDir, cfg))
        return ExportResult{false, "Failed to write project.hcfg", 0};
    return std::nullopt;
}

// Phase 7b: config.json — the graphics + window settings the game boots with.
// It goes to dataDir, next to project.hcfg, because that is the one directory a
// shipped game can find on every platform (SDL_GetBasePath, so Contents/
// Resources inside a bundle). Before finalizeAppBundle on purpose: the .app is
// codesigned last, and a file added afterwards breaks the seal.
static std::optional<ExportResult> writeGameConfig(const ExportSettings& settings,
                                                   const ExportContext&  ctx)
{
    if (settings.gameConfigJson.empty()) return std::nullopt;

    // Validated, not copied blind: this writes a shipping build, and a malformed
    // config.json does not fail loudly in the game — it silently resets every
    // graphics setting to the engine default, which is exactly the bug this
    // file exists to close.
    auto parsed = nlohmann::json::parse(settings.gameConfigJson, nullptr,
                                        /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object())
        return ExportResult{false, "Game settings are not a JSON object", ctx.assetsPacked};

    // ── An application does not take over the display (plan E6) ──────────────
    // The runtime already opens an app windowed, but this file is the LAST word:
    // GameWindowMode read from here overrides that default, so a "Fullscreen" in
    // it undoes the whole thing. Enforced at the exporter rather than only in
    // the dialog that fills gameConfigJson, because there the mode is a setting
    // remembered across exports — export a game, then an app, and the game's
    // Fullscreen is what the app would have shipped with.
    //
    // ONLY Fullscreen is rewritten, and Borderless deliberately passes through:
    // a window without a frame is what an application with its own title bar
    // asks for (plan F3), and clamping everything to "Windowed" would take that
    // away in the name of a rule about something else.
    std::string configText = settings.gameConfigJson;
    if (settings.appProject)
    {
        bool rewritten = false;
        if (const auto entries = parsed.find("CustomConfig");
            entries != parsed.end() && entries->is_array())
        {
            for (auto& e : *entries)
            {
                if (!e.is_object()) continue;
                const auto key = e.find("Key");
                if (key == e.end() || !key->is_string() || *key != "GameWindowMode") continue;
                const auto value = e.find("Value");
                if (value == e.end() || !value->is_string() || *value != "Fullscreen") continue;
                *value    = "Windowed";
                rewritten = true;
            }
        }
        // Re-serialized only when something actually changed, so an export that
        // was already right writes the caller's bytes through untouched.
        if (rewritten)
            configText = parsed.dump(4, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    const auto dst = ctx.dataDir / "config.json";
    std::ofstream out(dst, std::ios::trunc);
    out << configText;
    out.flush();
    if (!out.good())
        return ExportResult{false, "Failed to write " + dst.string(), ctx.assetsPacked};
    return std::nullopt;
}

// Phase 7c: Config/ProjectSettings.json — the project's own word on shadows,
// physics rate and title, copied VERBATIM next to project.hcfg (ProjectSettings.h
// explains why it ships as the file and not as hcfg fields). Same directory and
// same "before the bundle is sealed" rule as config.json above.
//
// A project WITHOUT the file ships without it, and that is an answer, not an
// omission: the game reads "no file" as the defaults. So a copy left in this
// output by an earlier export is removed — otherwise a project that went back
// to the defaults (the saver deletes nothing, but a versioned project can be
// checked out at a revision without the file) would ship the old numbers.
static std::optional<ExportResult> copyProjectSettingsFile(const ExportSettings& settings,
                                                           const ExportContext&  ctx)
{
    if (settings.projectSettingsFile.empty()) return std::nullopt;

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dstDir = ctx.dataDir / "Config";
    const fs::path dst    = dstDir / "ProjectSettings.json";

    if (!fs::is_regular_file(settings.projectSettingsFile, ec))
    {
        fs::remove(dst, ec);
        return std::nullopt;
    }
    fs::create_directories(dstDir, ec);
    if (ec)
        return ExportResult{false, "Failed to create " + dstDir.string(), ctx.assetsPacked};
    fs::copy_file(settings.projectSettingsFile, dst, fs::copy_options::overwrite_existing, ec);
    if (ec)
        return ExportResult{false, "Failed to copy " + settings.projectSettingsFile.string()
                                       + " to " + dst.string() + ": " + ec.message(),
                            ctx.assetsPacked};
    return std::nullopt;
}

// The splash picture, copied under the fixed name the packaged game looks for.
// Same shape as the settings file above it: an absent source REMOVES a stale
// copy, because "no picture" is the project's answer and a leftover from an
// earlier export of this output would quietly overrule it.
static std::optional<ExportResult> copySplashImage(const ExportSettings& settings,
                                                   const ExportContext&  ctx)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dst = ctx.dataDir / "Splash.png";
    if (settings.splashImageFile.empty() || !fs::is_regular_file(settings.splashImageFile, ec))
    {
        fs::remove(dst, ec);
        return std::nullopt;
    }
    fs::copy_file(settings.splashImageFile, dst, fs::copy_options::overwrite_existing, ec);
    if (ec)
        return ExportResult{false, "Failed to copy " + settings.splashImageFile.string()
                                       + " to " + dst.string() + ": " + ec.message(),
                            ctx.assetsPacked};
    return std::nullopt;
}

// Phase 7b: the application's icon, generated rather than demanded. One name
// from the built-in icon face on a coloured plate becomes the three files three
// systems each insist on — nobody draws the same picture three times, and a
// project has an icon on the day it is created.
//
// Reports whether the macOS container was written, which is what decides
// CFBundleIconFile below: a bundle that names a missing icon shows the generic
// one and gets that cached.
static bool writeAppIcons(const ExportSettings& settings, const ExportContext& ctx)
{
    glm::vec4 bg(0.12f, 0.44f, 0.78f, 1.0f);
    HE::uiParseRichColor(settings.appIconColor, bg);   // unparsable → the default plate
    const glm::vec4 fg = HE::heAppIconForeground(bg);
    const std::vector<int> sizes = { 16, 32, 64, 128, 256, 512 };

    // The project's own picture first; the generated glyph is what it falls
    // back to, so a PNG that went missing still leaves the project an icon.
    std::vector<HE::AppIconImage> set;
    if (!settings.appIconFile.empty())
    {
        std::vector<std::uint8_t> rgba;
        int w = 0, h = 0;
        if (HE::heLoadPngRGBA(settings.appIconFile, rgba, w, h))
            set = HE::heAppIconSetFromImage(rgba.data(), w, h, sizes);
    }
    if (set.empty() && !settings.appIconName.empty())
        set = HE::heRenderAppIconSet(settings.appIconName, bg, fg, sizes);
    if (set.empty()) return false;   // no picture, or a name the face does not have

    // The PNG goes everywhere: it is what the runtime loads to set its window
    // icon, which is the taskbar entry on Windows and Linux both.
    const auto at = [&set](int px) -> const HE::AppIconImage* {
        for (const HE::AppIconImage& i : set) if (i.px == px) return &i;
        return nullptr;
    };
    if (const HE::AppIconImage* png = at(256))
        HE::hePngWrite(ctx.dataDir / "AppIcon.png", png->rgba.data(), png->px, png->px);

    // "Host" means the machine this editor runs on, resolved here so the phase
    // never has to ask the caller what it meant.
    ExportPlatform target = settings.iconPlatform;
    if (target == ExportPlatform::Host)
    {
#ifdef _WIN32
        target = ExportPlatform::Windows;
#elif defined(__APPLE__)
        target = ExportPlatform::MacOS;
#else
        target = ExportPlatform::Linux;
#endif
    }
    const bool mac = ctx.app || target == ExportPlatform::MacOS;
    const bool win = target == ExportPlatform::Windows;
    bool icns = false;
    if (mac && ctx.app)
        icns = HE::heIcnsWrite(ctx.appPath / "Contents" / "Resources" / "AppIcon.icns", set);
    if (win)
        HE::heIcoWrite(ctx.binDir / "AppIcon.ico", set);

    // A document type gets an icon of its own when it names one, on the same
    // plate as the application: a folder of files that all wear the app's icon
    // tells nobody which is which, and asking for a second drawing is how the
    // whole feature would go unused.
    for (const HE::AppDocumentType& t : settings.documentTypes)
    {
        if (t.iconName.empty() || !HE::heValidDocumentExtension(t.extension)) continue;
        const std::vector<HE::AppIconImage> docSet =
            HE::heRenderAppIconSet(t.iconName, bg, fg, { 16, 32, 64, 128, 256, 512 });
        if (docSet.empty()) continue;
        if (mac && ctx.app)
            HE::heIcnsWrite(ctx.appPath / "Contents" / "Resources" / ("Doc-" + t.extension + ".icns"),
                            docSet);
        if (win)
            HE::heIcoWrite(ctx.binDir / ("Doc-" + t.extension + ".ico"), docSet);
    }
    return icns;
}

// Phase 7c: the declarations the OTHER two systems want. macOS carries its own
// inside Info.plist; Linux and Windows want files beside the executable, and
// INSTALLING them is an installer's job — an export writes what would be
// installed, never the registry or the user's mime database.
static void writeDocumentTypeFiles(const std::string& projectName,
                                   const ExportSettings& settings, const ExportContext& ctx)
{
    if (settings.documentTypes.empty() || ctx.app) return;

    const std::string ident = settings.bundleId.empty()
                                  ? bundleIdentifier(projectName) : settings.bundleId;
    // The name a launcher shows — the title if the project set one. Only ever
    // a label in these files; the identifier above is what keys them.
    const std::string& shown = settings.displayName.empty() ? projectName : settings.displayName;
    ExportPlatform target = settings.iconPlatform;
    if (target == ExportPlatform::Host)
    {
#ifdef _WIN32
        target = ExportPlatform::Windows;
#elif defined(__APPLE__)
        target = ExportPlatform::MacOS;
#else
        target = ExportPlatform::Linux;
#endif
    }

    const auto write = [](const std::filesystem::path& p, const std::string& text) {
        std::ofstream f(p, std::ios::trunc);
        if (f) f << text;
    };

    if (target == ExportPlatform::Windows)
    {
        write(ctx.binDir / "RegisterFileTypes.reg",
              HE::heWindowsRegistration(shown, ctx.exeName, ident,
                                        settings.documentTypes));
    }
    else if (target == ExportPlatform::Linux)
    {
        write(ctx.binDir / (ident + ".desktop"),
              HE::heDesktopEntry(shown, ctx.exeName, ident, settings.documentTypes));
        write(ctx.binDir / (ident + ".xml"),
              HE::heSharedMimeInfo(ident, settings.documentTypes));
    }
}

// Phase 8: finalize the .app — Info.plist makes Contents/ a real bundle (so
// SDL_GetBasePath resolves Resources), then an ad-hoc codesign of the whole
// thing — the key patch already re-signed the bare executable, but adding
// Info.plist and dylibs invalidates that; the bundle must be sealed last.
static std::optional<ExportResult> finalizeAppBundle(const std::string&    projectName,
                                                     const ExportSettings& settings,
                                                     bool                  hasIcon,
                                                     const ExportContext&  ctx)
{
    if (ctx.app)
    {
        if (!writeInfoPlist(ctx.appPath / "Contents", ctx.exeName, projectName, settings.displayName,
                            settings.bundleId, settings.appVersion, hasIcon,
                            settings.documentTypes))
            return ExportResult{false, "Failed to write Info.plist", ctx.assetsPacked};
#ifdef __APPLE__
        if (!signAppBundle(ctx.appPath))
            return ExportResult{false, "Failed to codesign the .app bundle", ctx.assetsPacked};
#endif
    }
    return std::nullopt;
}

ExportResult ProjectExporter::exportProject(
    const std::filesystem::path& contentDir,
    const std::string&           projectName,
    const std::string&           startupSceneName,
    const std::filesystem::path& outputDir,
    const ExportSettings&        settings,
    const std::vector<uint8_t>&  startupSceneBinary,
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& extraScenes,
    const std::string&           gameInstanceJson)
{
    ExportContext ctx;

    // Announce each phase as it starts, so a caller with a progress display can
    // show what is actually happening instead of one bar for the whole export.
    const auto stage = [&](const char* name) { if (settings.onStage) settings.onStage(name); };

    stage("layout");
    if (auto fail = prepareOutputLayout(outputDir, projectName, settings, ctx)) return *fail;
    if (auto fail = resolvePackSettings(settings, ctx))                         return *fail;
    if (settings.planned) ctx.sceneUuid = settings.plannedSceneUuid;
    stage("pack");
    if (auto fail = packContent(contentDir, projectName, settings, startupSceneBinary,
                                extraScenes, gameInstanceJson, ctx))            return *fail;

    copyLooseStartupScene(contentDir, startupSceneName, startupSceneBinary, ctx);

    stage("binaries");
    if (auto fail = copyRuntimeBinaries(settings, ctx))                         return *fail;
    stage("hclib");
    if (auto fail = copyHorizonCodeLib(settings, ctx))                          return *fail;
    stage("config");
    if (auto fail = writeProjectConfig(projectName, settings, startupSceneBinary, ctx))
        return *fail;
    if (auto fail = writeGameConfig(settings, ctx))                             return *fail;
    if (auto fail = copyProjectSettingsFile(settings, ctx))                     return *fail;
    if (auto fail = copySplashImage(settings, ctx))                             return *fail;
    stage("icon");
    const bool hasIcon = writeAppIcons(settings, ctx);
    writeDocumentTypeFiles(projectName, settings, ctx);
    stage("bundle");
    if (auto fail = finalizeAppBundle(projectName, settings, hasIcon, ctx))     return *fail;

    // The runtime ships under ctx.exeName (routeRuntime above renames it), so
    // the executable is derived, not searched for. Reported only if it is
    // really there — a data-only export (no gameRuntimeDir) has none.
    std::error_code exeEc;
    std::filesystem::path exe = ctx.binDir / ctx.exeName;
    if (!std::filesystem::exists(exe, exeEc)) exe.clear();

    return {true, "", ctx.assetsPacked, ctx.binaryCopied, ctx.assetsReused, ctx.keyEmbedded,
            exe};
}

ExportPlan ProjectExporter::planExport(
    const std::string&           projectName,
    const std::string&           startupSceneName,
    const std::filesystem::path& outputDir,
    ExportSettings&              settings,
    bool                         packedScene,
    bool                         horizonCodeCompiled)
{
    // The same phases exportProject starts with, so the key comes from the same
    // sources in the same order (the previous export's hcfg, its executable,
    // the previous SDK build's header) while all of them are still there.
    ExportContext ctx;
    ExportPlan plan;
    if (auto fail = prepareOutputLayout(outputDir, projectName, settings, ctx))
    {
        plan.errorMessage = fail->errorMessage;
        return plan;
    }
    if (auto fail = resolvePackSettings(settings, ctx))
    {
        plan.errorMessage = fail->errorMessage;
        return plan;
    }
    // What packContent and copyLooseStartupScene will name, decided the way
    // they decide it.
    ctx.hpakFilename = projectName + ".hpak";
    if (packedScene)
        ctx.sceneUuid = settings.planned ? settings.plannedSceneUuid : HE::UUID::generate();
    else if (!startupSceneName.empty())
        ctx.sceneFile = std::filesystem::path(startupSceneName).filename().string();
    ctx.hcGenShipped = horizonCodeCompiled;

    plan.config = makeProjectConfig(projectName, settings, packedScene, ctx);
    if (settings.encrypt) std::memcpy(plan.config.encKey, ctx.packSettings.key, 32);

    settings.planned = true;
    std::memcpy(settings.plannedPakKey, ctx.packSettings.key, 32);
    settings.plannedSceneUuid = ctx.sceneUuid;
    plan.success = true;
    return plan;
}
