#include <Hpak/ProjectExporter.h>
#include <Hpak/ProjectConfig.h>
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

#ifdef __APPLE__
// Ad-hoc re-sign in place. Patching invalidated the signature; on arm64 macOS
// an invalid signature means the binary is killed on launch, so a failed
// re-sign must FAIL the export — never ship silently unrunnable.
static bool resignMachO(const std::filesystem::path& p)
{
    // Shell-quote the path: wrap in single quotes, escaping embedded single
    // quotes as '\'' (an apostrophe in the export path must not break the
    // command — that would skip the re-sign, not just look ugly).
    std::string quoted = "'";
    for (const char c : p.string())
        quoted += (c == '\'') ? "'\\''" : std::string(1, c);
    quoted += "'";
    const std::string cmd = "/usr/bin/codesign --force --sign - " + quoted + " 2>/dev/null";
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
    uint8_t buf[3 + 32];
    buf[0] = static_cast<uint8_t>(s.codec);
    buf[1] = static_cast<uint8_t>(s.level);
    buf[2] = s.encrypt ? 1 : 0;
    std::memcpy(buf + 3, s.key, 32); // all-zero when not encrypting
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

ExportResult ProjectExporter::exportProject(
    const std::filesystem::path& contentDir,
    const std::string&           projectName,
    const std::string&           startupSceneName,
    const std::filesystem::path& outputDir,
    const ExportSettings&        settings,
    const std::vector<uint8_t>&  startupSceneBinary)
{
    std::error_code ec;

    std::filesystem::create_directories(outputDir, ec);
    if (ec) return {false, "Cannot create output dir: " + ec.message(), 0};

    Hpak::PackSettings packSettings;
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
            if (ProjectConfigLoader::load(outputDir, prevCfg) && prevCfg.encrypted)
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
                    if (readEmbeddedPakKey(outputDir / exe, packSettings.key))
                    { haveKey = true; break; }
            }
        }
        if (!haveKey && !Hpak::randomBytes(packSettings.key, 32))
            return {false, "Crypto backend unavailable — cannot encrypt", 0};
    }

    packSettings.excludePatterns = settings.excludePatterns;

    const std::string hpakFilename = projectName + ".hpak";
    const auto pakPath      = outputDir / hpakFilename;
    const auto manifestPath = outputDir / (hpakFilename + ".manifest");
    const uint64_t settingsFp = settingsFingerprint(packSettings);

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

    HpakWriter packer;
    const int added = packer.addDirectory(contentDir, packSettings, settings.progress,
                                          haveCache ? &cache : nullptr);
    const int reused = packer.reusedCount();
    prevPak.reset(); // release the read handle BEFORE write() replaces the file

    // Pack the startup scene as a binary entry INTO the pak (if the caller
    // serialized one), under a fresh UUID recorded in the hcfg. Same codec +
    // encryption as the assets. Must happen before write().
    HE::UUID sceneUuid{};
    if (!startupSceneBinary.empty())
    {
        sceneUuid = HE::UUID::generate();
        packer.addEntry(sceneUuid, startupSceneBinary, packSettings);
    }

    if (!packer.write(pakPath.string()))
        return {false, "Failed to write " + hpakFilename, 0};

    // Persist the manifest for the NEXT incremental export. Best-effort cache:
    // if the reopen or write fails, the next export simply packs everything.
    {
        HpakReader newPak;
        if (newPak.open(pakPath.string()))
            savePakManifest(manifestPath, newPak.tocHash(), settingsFp,
                            packer.sourceHashes());
    }

    // Loose startup-scene fallback: only when no binary scene was packed.
    std::string sceneFile;
    if (startupSceneBinary.empty() && !startupSceneName.empty())
    {
        const auto sceneSrc = contentDir / startupSceneName;
        sceneFile = std::filesystem::path(startupSceneName).filename().string();
        if (std::filesystem::exists(sceneSrc, ec))
            std::filesystem::copy_file(sceneSrc, outputDir / sceneFile,
                std::filesystem::copy_options::overwrite_existing, ec);
    }

    // Copy game runtime binaries (executable + dylibs) so the export is runnable.
    // This happens BEFORE project.hcfg is written: with encryption the key is
    // patched into the copied game executable, and only if that succeeds is the
    // key omitted from the hcfg. Non-throwing iteration: this runs on the
    // editor's export worker thread, where an escaped filesystem_error would be
    // std::terminate.
    int  binaryCopied = 0;
    bool keyEmbedded  = false;
    if (!settings.gameRuntimeDir.empty())
    {
        // A named-but-missing runtime dir must FAIL, not silently ship a
        // data-only export (an existence gate here previously skipped the
        // whole block, including all its error checks).
        if (!std::filesystem::is_directory(settings.gameRuntimeDir, ec))
            return {false, "Game runtime dir not found: "
                           + settings.gameRuntimeDir.string(), added};

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
                const auto dst = outputDir / dit->path().filename();
                std::filesystem::copy_file(dit->path(), dst,
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (ec)
                    return {false, "Failed to copy runtime binary "
                                   + dit->path().filename().string() + ": "
                                   + ec.message(), added};
                ++binaryCopied;
                copied.push_back(dst);
            }
            dit.increment(ec);
        }
        ec.clear();

        // A runtime dir that yields nothing is a broken export (data without an
        // executable) — the exact failure mode this parameter exists to prevent.
        if (binaryCopied == 0)
            return {false, "Game runtime dir contained no files: "
                           + settings.gameRuntimeDir.string(), added};

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
                const int patched = patchEmbeddedPakKey(dst, packSettings.key);
                if (patched == -1)
                    return {false, "Failed to embed the pak key into " + name
                                   + " (write error)", added};
                if (patched == -2)
                    return {false, "Failed to re-sign " + name
                                   + " after embedding the pak key (codesign)", added};
                if (patched > 0) keyEmbedded = true;
            }
        }
    }

    ProjectConfig cfg;
    cfg.projectName   = projectName;
    cfg.hpakFilename  = hpakFilename;
    cfg.mainSceneName = sceneFile;
    std::memset(cfg.projectUuidBytes, 0, 16);
    cfg.enableModSupport = settings.enableModSupport;
    cfg.encrypted = settings.encrypt;
    // Key placement: inside the game executable when the patch succeeded (the
    // hcfg then carries only the encrypted flag), in the hcfg otherwise.
    if (settings.encrypt && !keyEmbedded)
        std::memcpy(cfg.encKey, packSettings.key, 32);
    if (!startupSceneBinary.empty())
    {
        cfg.hasPackedScene = true;
        std::memcpy(cfg.startupSceneUuid,      &sceneUuid.hi, 8);
        std::memcpy(cfg.startupSceneUuid + 8,  &sceneUuid.lo, 8);
    }

    if (!ProjectConfigLoader::save(outputDir, cfg))
        return {false, "Failed to write project.hcfg", 0};

    return {true, "", added, binaryCopied, reused, keyEmbedded};
}
