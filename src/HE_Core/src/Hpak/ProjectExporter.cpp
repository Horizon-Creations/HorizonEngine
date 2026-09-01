#include <Hpak/ProjectExporter.h>
#include <Types/Enums.h>            // HE::AssetType (the packed type indices)
#include <cstdint>
#include <Hpak/ProjectConfig.h>
#include <HorizonCode/HcCompiledLoader.h>   // compiledLibraryName (artifact naming)
#include <Hpak/HpakWriter.h>
#include <Hpak/HpakReader.h>
#include <Hpak/HpakFormat.h>
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

std::filesystem::path resolveRuntimeDir(const std::filesystem::path& editorBaseDir,
                                        ExportPlatform p)
{
    if (p == ExportPlatform::Host)
        return editorBaseDir / ".." / "Game";
    return editorBaseDir / ".." / "GameRuntimes" / exportPlatformName(p);
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
                                        ExportPlatform p)
{
    if (editorBaseDir.empty()) return {};

    const std::filesystem::path sub = (p == ExportPlatform::Host)
        ? std::filesystem::path("Game")
        : std::filesystem::path("GameRuntimes") / exportPlatformName(p);

    // Walk upward: <dir>/Game next to the editor covers the deploy layout
    // (deploy/Editor + deploy/Game); <dir>/out/deploy/Game covers running the
    // editor from a build tree anywhere inside the repo.
    std::error_code ec;
    std::filesystem::path dir = editorBaseDir.lexically_normal();
    for (int depth = 0; depth < 7 && !dir.empty(); ++depth)
    {
        if (isRuntimeBundle(dir / sub))                    return dir / sub;
        if (isRuntimeBundle(dir / "out" / "deploy" / sub)) return dir / "out" / "deploy" / sub;
        const auto parent = dir.parent_path();
        if (parent == dir) break; // filesystem root
        dir = parent;
    }
    return {};
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

// ─── Incremental-pack manifest ────────────────────────────────────────────────
// Sidecar "<name>.hpak.manifest" written next to the pak: per entry the hash of
// the rewritten blob it was packed from, plus two fingerprints that gate reuse —
// the pak's tocHash (manifest must describe exactly this pak, not a stale or
// hand-swapped one) and a settings fingerprint (codec/level/encrypt/key).

static uint64_t settingsFingerprint(const Hpak::PackSettings& s)
{
    uint8_t buf[8 + 32];
    buf[0] = static_cast<uint8_t>(s.codec);
    buf[1] = static_cast<uint8_t>(s.level);
    buf[2] = s.encrypt ? 1 : 0;
    // cook toggle (bit0) + texture-compression target (bits1-3) → changing either re-packs.
    buf[3] = static_cast<uint8_t>((s.cook ? 1 : 0) | ((s.textureCompression & 0x7) << 1));
    std::memcpy(buf + 4, &s.shaderBackends, 4); // precompiled-shader backend set → re-pack materials
    std::memcpy(buf + 8, s.key, 32);  // all-zero when not encrypting
    return Hpak::hash64(buf, sizeof(buf));
}

static bool loadPakManifest(const std::filesystem::path& path,
                            uint64_t expectPakTocHash, uint64_t expectSettingsFp,
                            std::unordered_map<HE::UUID, uint64_t>& outHashes)
{
    std::ifstream in(path);
    if (!in.is_open()) return false;
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;

    auto u64 = [&](const char* key, uint64_t& out) -> bool {
        auto it = j.find(key);
        if (it == j.end() || !it->is_number_unsigned()) return false;
        out = it->get<uint64_t>();
        return true;
    };
    uint64_t tocHash = 0, fp = 0;
    if (!u64("pakTocHash", tocHash) || tocHash != expectPakTocHash) return false;
    if (!u64("settingsFp", fp) || fp != expectSettingsFp) return false;

    auto it = j.find("entries");
    if (it == j.end() || !it->is_array()) return false;
    for (const auto& e : *it)
    {
        if (!e.is_object()) continue;
        auto hi = e.find("hi"); auto lo = e.find("lo"); auto h = e.find("srcHash");
        if (hi == e.end() || lo == e.end() || h == e.end()) continue;
        if (!hi->is_number_unsigned() || !lo->is_number_unsigned() || !h->is_number_unsigned())
            continue;
        outHashes[HE::UUID{hi->get<uint64_t>(), lo->get<uint64_t>()}] = h->get<uint64_t>();
    }
    return !outHashes.empty();
}

static void savePakManifest(const std::filesystem::path& path,
                            uint64_t pakTocHash, uint64_t settingsFp,
                            const std::vector<std::pair<HE::UUID, uint64_t>>& hashes)
{
    nlohmann::json j;
    j["pakTocHash"] = pakTocHash;
    j["settingsFp"] = settingsFp;
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& [id, h] : hashes)
        entries.push_back({ {"hi", id.hi}, {"lo", id.lo}, {"srcHash", h} });
    j["entries"] = std::move(entries);

    // Best-effort cache metadata: failures are ignored (next export packs full).
    std::ofstream out(path, std::ios::trunc);
    if (out.is_open()) out << j.dump();
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

static bool writeInfoPlist(const std::filesystem::path& contentsDir,
                           const std::string& projectName)
{
    // XML-escape the display name (project names can contain & < > " ').
    std::string name;
    for (char c : projectName)
        switch (c)
        {
        case '&': name += "&amp;"; break;
        case '<': name += "&lt;"; break;
        case '>': name += "&gt;"; break;
        case '"': name += "&quot;"; break;
        case '\'': name += "&apos;"; break;
        default: name += c;
        }

    const std::string plist =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "  <key>CFBundleExecutable</key><string>HorizonGame</string>\n"
        "  <key>CFBundleIdentifier</key><string>" + bundleIdentifier(projectName) + "</string>\n"
        "  <key>CFBundleName</key><string>" + name + "</string>\n"
        "  <key>CFBundleDisplayName</key><string>" + name + "</string>\n"
        "  <key>CFBundlePackageType</key><string>APPL</string>\n"
        "  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>\n"
        "  <key>CFBundleVersion</key><string>1.0</string>\n"
        "  <key>CFBundleShortVersionString</key><string>1.0</string>\n"
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
    if (ctx.app)
    {
        std::filesystem::create_directories(ctx.binDir, ec);
        std::filesystem::create_directories(ctx.dataDir, ec);
        if (ec) return ExportResult{false, "Cannot create .app bundle: " + ec.message(), 0};
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
    if (settings.encrypt)
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
                for (const char* exe : { "HorizonGame", "HorizonGame.exe" })
                    if (readEmbeddedPakKey(ctx.binDir / exe, packSettings.key))
                    { haveKey = true; break; }
            }
        }
        if (!haveKey && !Hpak::randomBytes(packSettings.key, 32))
            return ExportResult{false, "Crypto backend unavailable — cannot encrypt", 0};
    }

    packSettings.excludePatterns = settings.excludePatterns;
    packSettings.cook = true; // always cook exports into the runtime-optimal form
    packSettings.textureCompression = settings.textureCompression;
    packSettings.shaderBackends        = settings.shaderBackends;        // precompile material shaders
    packSettings.compileShaderVariants = settings.compileShaderVariants;
    packSettings.compileParticleShaderVariants = settings.compileParticleShaderVariants;
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
        ctx.sceneUuid = HE::UUID::generate();
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
    std::error_code ec;

    ctx.hpakFilename        = projectName + ".hpak";
    const auto pakPath      = ctx.dataDir / ctx.hpakFilename;
    const auto manifestPath = ctx.dataDir / (ctx.hpakFilename + ".manifest");
    const uint64_t settingsFp = settingsFingerprint(ctx.packSettings);

    // Incremental cache: previous pak + its manifest, gated on the manifest
    // describing exactly that pak (tocHash) built with these settings.
    auto prevPak = std::make_unique<HpakReader>();
    Hpak::IncrementalCache cache;
    bool haveCache = false;
    if (settings.incremental && std::filesystem::exists(pakPath, ec)
        && prevPak->open(pakPath.string()))
    {
        if (loadPakManifest(manifestPath, prevPak->tocHash(), settingsFp, cache.srcHashes))
        {
            cache.previousPak = prevPak.get();
            haveCache = true;
        }
    }

    // The project's own Content first, then the engine defaults under "Engine/".
    // Order matters: a project-local override (Content/Engine/…) carries the same
    // UUID as the default it shadows and must be the one that ships.
    std::vector<HpakWriter::SourceRoot> roots{ { contentDir, {} } };
    if (!settings.engineContentDir.empty())
        roots.push_back({ settings.engineContentDir, "Engine/" });

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
    {
        HpakReader newPak;
        if (newPak.open(pakPath.string()))
            savePakManifest(manifestPath, newPak.tocHash(), settingsFp,
                            packer.sourceHashes());
    }
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

        // Route each runtime file to the right place for the .app layout: the
        // executable and engine dylibs go next to the exe (binDir); GameLogic and
        // anything else (config.json) goes with the data (dataDir) so the running
        // game finds it via SDL_GetBasePath. Flat exports collapse both to
        // outputDir, so the routing is a no-op there.
        auto routeRuntime = [&](const std::string& n) -> std::filesystem::path {
            const bool gameLogic = n.rfind("GameLogic.", 0) == 0; // loaded from base path
            // The bundled CPython runtime (python3XY.dll/.zip + python3XY._pth) must sit next
            // to the executable so embedded Python's isolated-path lookup finds it; route any
            // "python*" file to binDir. (Flat exports collapse binDir==dataDir, so this only
            // matters for the macOS .app split.)
            const bool pythonRuntime = n.rfind("python", 0) == 0;
            // The POSIX stdlib path file is named after the executable (HorizonGame._pth), so
            // it doesn't match the "python*" rule — but it too must sit next to the exe.
            const bool pthFile = n.size() > 5 && n.compare(n.size() - 5, 5, "._pth") == 0;
            const bool engineBin = !gameLogic
                && (n == "HorizonGame" || n == "HorizonGame.exe" || pythonRuntime || pthFile
                    || n.size() > 4 && (n.compare(n.size() - 4, 4, ".dll") == 0)
                    || n.size() > 3 && (n.compare(n.size() - 3, 3, ".so") == 0)
                    || n.size() > 6 && (n.compare(n.size() - 6, 6, ".dylib") == 0));
            return (engineBin ? ctx.binDir : ctx.dataDir) / n;
        };

        // Clear STALE code before copying fresh binaries. Re-signing over a
        // previous export's leftovers is the classic codesign failure ("bundle
        // format unrecognized" / "code object is not signed"): an old
        // _CodeSignature seal no longer matches, and stale dylibs from an older
        // runtime linger in the bundle. For a .app, Contents/MacOS is disjoint
        // from Contents/Resources, so wiping it does NOT touch the pak/hcfg the
        // incremental cache already read above — recreate it empty. (Flat
        // exports share one dir with the just-written pak, so they only remove
        // each destination individually, below.)
        if (ctx.app)
        {
            std::filesystem::remove_all(ctx.appPath / "Contents" / "_CodeSignature", ec); ec.clear();
            std::filesystem::remove_all(ctx.binDir, ec); ec.clear();
            std::filesystem::create_directories(ctx.binDir, ec); ec.clear();
        }

        // Every file in the bundle is required (executable AND its libraries):
        // any copy failure is a hard error. A silently skipped executable is
        // the worst case — the output would keep a STALE previously-exported
        // exe whose embedded key no longer matches this pak.
        std::vector<std::filesystem::path> copied;
        std::filesystem::directory_iterator dit(settings.gameRuntimeDir, ec);
        const std::filesystem::directory_iterator dend;
        while (!ec && dit != dend)
        {
            const bool regular = dit->is_regular_file(ec);
            if (ec) { ec.clear(); dit.increment(ec); continue; }
            if (regular)
            {
                const std::string fname = dit->path().filename().string();
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
                if (!settings.bundlePython)
                {
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
                    if (isZip || isPth || isPyLib || isPlugin) { dit.increment(ec); continue; }
                }
                const auto dst = routeRuntime(fname);
                // Delete any existing file first (fresh inode): copying over a
                // code-signed / currently-mapped binary in place can leave a
                // stale signature or a busy-file error on re-export.
                std::filesystem::remove(dst, ec); ec.clear();
                std::filesystem::copy_file(dit->path(), dst,
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (ec)
                    return ExportResult{false, "Failed to copy runtime binary "
                                               + dit->path().filename().string() + ": "
                                               + ec.message(), ctx.assetsPacked};
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
            }
            dit.increment(ec);
        }
        ec.clear();

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
                if (name != "HorizonGame" && name != "HorizonGame.exe") continue;
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

// Phase 6: compiled HorizonCode classes — ship the generated library with the data
// (base path — same load location as GameLogic), under the canonical name the
// runtime loader probes. A copy failure is a hard error: hcfg would otherwise
// claim "compiled" for a library that never shipped.
static std::optional<ExportResult> copyHorizonCodeLib(const ExportSettings& settings,
                                                      ExportContext&        ctx)
{
    std::error_code ec;

    if (!settings.horizonCodeGenLib.empty())
    {
        const auto dst = ctx.dataDir / HorizonCode::compiledLibraryName();
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
static std::optional<ExportResult> writeProjectConfig(const std::string&          projectName,
                                                      const ExportSettings&       settings,
                                                      const std::vector<uint8_t>& startupSceneBinary,
                                                      const ExportContext&        ctx)
{
    ProjectConfig cfg;
    cfg.projectName   = projectName;
    cfg.hpakFilename  = ctx.hpakFilename;
    cfg.mainSceneName = ctx.sceneFile;
    std::memset(cfg.projectUuidBytes, 0, 16);
    cfg.enableModSupport = settings.enableModSupport;
    cfg.encrypted = settings.encrypt;
    cfg.horizonCodeCompiled = ctx.hcGenShipped;
    cfg.defaultSaveTemplate = settings.defaultSaveTemplate;
    cfg.appMode               = settings.appProject;
    cfg.advancedShaderEffects = settings.advancedShaderEffects;
    cfg.allowFiles            = settings.allowFiles;
    cfg.allowProcesses        = settings.allowProcesses;
    cfg.allowNetwork          = settings.allowNetwork;
    cfg.theme                 = settings.theme;
    cfg.themeMode             = settings.themeMode;
    // Key placement: inside the game executable when the patch succeeded (the
    // hcfg then carries only the encrypted flag), in the hcfg otherwise.
    if (settings.encrypt && !ctx.keyEmbedded)
        std::memcpy(cfg.encKey, ctx.packSettings.key, 32);
    if (!startupSceneBinary.empty())
    {
        cfg.hasPackedScene = true;
        std::memcpy(cfg.startupSceneUuid,      &ctx.sceneUuid.hi, 8);
        std::memcpy(cfg.startupSceneUuid + 8,  &ctx.sceneUuid.lo, 8);
    }

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
    const auto parsed = nlohmann::json::parse(settings.gameConfigJson, nullptr,
                                              /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object())
        return ExportResult{false, "Game settings are not a JSON object", ctx.assetsPacked};

    const auto dst = ctx.dataDir / "config.json";
    std::ofstream out(dst, std::ios::trunc);
    out << settings.gameConfigJson;
    out.flush();
    if (!out.good())
        return ExportResult{false, "Failed to write " + dst.string(), ctx.assetsPacked};
    return std::nullopt;
}

// Phase 8: finalize the .app — Info.plist makes Contents/ a real bundle (so
// SDL_GetBasePath resolves Resources), then an ad-hoc codesign of the whole
// thing — the key patch already re-signed the bare executable, but adding
// Info.plist and dylibs invalidates that; the bundle must be sealed last.
static std::optional<ExportResult> finalizeAppBundle(const std::string&   projectName,
                                                     const ExportContext& ctx)
{
    if (ctx.app)
    {
        if (!writeInfoPlist(ctx.appPath / "Contents", projectName))
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
    stage("bundle");
    if (auto fail = finalizeAppBundle(projectName, ctx))                        return *fail;

    // The runtime always ships under this name (routeRuntime above keys on it),
    // so the executable is derived, not searched for. Reported only if it is
    // really there — a data-only export (no gameRuntimeDir) has none.
    std::error_code exeEc;
    std::filesystem::path exe = ctx.binDir / "HorizonGame";
    if (!std::filesystem::exists(exe, exeEc)) exe = ctx.binDir / "HorizonGame.exe";
    if (!std::filesystem::exists(exe, exeEc)) exe.clear();

    return {true, "", ctx.assetsPacked, ctx.binaryCopied, ctx.assetsReused, ctx.keyEmbedded,
            exe};
}
