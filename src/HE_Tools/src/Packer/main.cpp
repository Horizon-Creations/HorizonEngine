// hpak_packer — pack a project's .hasset files into a single .hpak archive
// Usage: hpak_packer <project_root> <output.hpak> [--secret <passphrase>]
//        [--cook] [--texture-format none|astc|bc7|bc3] [--texture-quality 0|1|2]
//        [--cook-arrays] [--engine-content-cache <dir>]
//        [--only-referenced --startup-scene <file> [--always-include <glob>]...]
//
// Without --cook the archive holds the editor's raw asset form (what it always did);
// --cook runs the same pack-time cook the editor's export runs (meshes interleaved,
// textures mipped and, with --texture-format, block-compressed; --cook-arrays also
// compresses texture ARRAYS). That makes the tool usable to size an export without
// the editor. Material shaders are NOT precompiled here (the compiler is the editor's).
//
// --only-referenced packs only what the game reaches (Hpak/ReachFilter.h). <project_root> is
// the Content folder; --startup-scene names the startup .hescene (a path relative to it), every
// other .hescene under it is a scene the game may load and is kept only when something
// reachable names it. Scripts and the manifest in the folder above are roots.
//
// If --secret is provided the entries are AES-256-GCM encrypted with a key
// derived from the passphrase + a zero salt (KeyDerivation::derive). Reading the
// archive back needs that same 32-byte key: the runtime hands it to
// ContentManager::mountPak() (GameApplication does the mounting for a packaged
// game); loadPak() is the eager tools/tests variant.
//
// --cache <manifest> makes a re-pack incremental, like the editor's export: the
// previous <output.hpak> plus the manifest written next to it after the last
// pack let every unchanged asset be copied over as stored (no cook, no texture
// encode, no compression or encryption). The manifest is only trusted for
// exactly that pak and the same settings (codec, key, ...); anything else, or a
// missing file, packs from scratch. Hpak/PackManifest.h has the rules.

#include "HpakWriter.h"
#include <cstdint>
#include "KeyDerivation.h"
#include <Hpak/HpakReader.h>
#include <Hpak/PackManifest.h>
#include <Hpak/ReachFilter.h>
#include <fstream>
#include <iterator>
#include <memory>
#include <iostream>
#include <string>
#include <filesystem>
#include <vector>

namespace
{
// Prints what was wrong with the command line (if anything) plus the usage, and
// returns the process exit code. Argument errors are HARD here: a packer that
// quietly ignores "--secrt <pass>" or "--codec zstdd" writes an archive the game
// then cannot open, and the mistake only surfaces at runtime.
int usage(const std::string& problem = {})
{
    if (!problem.empty())
        std::cerr << "hpak_packer: " << problem << "\n";
    std::cerr << "Usage: hpak_packer <project_root> <output.hpak> "
                 "[--codec store|lz4|zstd] [--secret <passphrase>] "
                 "[--engine-content <dir>] [--engine-content-cache <dir>] [--exclude <glob>]... "
                 "[--cache <manifest>] [--cook] [--texture-format none|astc|bc7|bc3] "
                 "[--texture-quality 0|1|2] [--cook-arrays] [--only-referenced] "
                 "[--startup-scene <file>] [--always-include <glob>]...\n";
    return 1;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
        return usage();

    const std::string inputPath  = argv[1];
    const std::string outputFile = argv[2];

    std::string   secret;
    std::string   engineContent;  // EditorDeps/EngineContent → packed as "Engine/…"
    Hpak::Codec   codec = Hpak::Codec::Zstd; // sensible ship default
    std::vector<std::string> excludes;
    std::string   cachePath;      // incremental-pack manifest (read, then rewritten)
    std::string   engineContentCache; // EngineContent download cache, fills gaps only
    bool          cook = false, cookArrays = false;
    bool          onlyReferenced = false;
    std::string   startupScene;
    std::vector<std::string> alwaysInclude;
    uint8_t       textureCompression = 0, textureQuality = 0;
    for (int i = 3; i < argc; ++i)
    {
        const std::string arg = argv[i];

        // Every flag below takes exactly one value. The loop used to stop at
        // argc - 1, which silently dropped a trailing flag along with its value.
        const auto takeValue = [&](std::string& out)
        {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };

        if (arg == "--secret")
        {
            if (!takeValue(secret))
                return usage("--secret needs a passphrase");
        }
        else if (arg == "--codec")
        {
            std::string c;
            if (!takeValue(c))
                return usage("--codec needs one of store|lz4|zstd");
            if      (c == "store") codec = Hpak::Codec::Store;
            else if (c == "lz4")   codec = Hpak::Codec::LZ4;
            else if (c == "zstd")  codec = Hpak::Codec::Zstd;
            else return usage("unknown codec '" + c + "' (expected store|lz4|zstd)");
        }
        else if (arg == "--engine-content")
        {
            // The engine-wide default content root. Its assets are packed
            // alongside the project's under the "Engine/" prefix the editor
            // addresses them by — a game shipped without them falls back to the
            // renderer's default cube for every built-in mesh.
            if (!takeValue(engineContent))
                return usage("--engine-content needs a directory");
            if (!std::filesystem::is_directory(engineContent))
                return usage("--engine-content is not a directory: " + engineContent);
        }
        else if (arg == "--exclude")
        {
            // Repeatable. Glob vs the project-root-relative path ('*' spans '/').
            std::string glob;
            if (!takeValue(glob))
                return usage("--exclude needs a glob");
            excludes.emplace_back(std::move(glob));
        }
        else if (arg == "--engine-content-cache")
        {
            if (!takeValue(engineContentCache))
                return usage("--engine-content-cache needs a directory");
            if (!std::filesystem::is_directory(engineContentCache))
                return usage("--engine-content-cache is not a directory: " + engineContentCache);
        }
        else if (arg == "--cook")
            cook = true;
        else if (arg == "--only-referenced")
            onlyReferenced = true;
        else if (arg == "--startup-scene")
        {
            if (!takeValue(startupScene))
                return usage("--startup-scene needs a file");
        }
        else if (arg == "--always-include")
        {
            std::string glob;
            if (!takeValue(glob))
                return usage("--always-include needs a glob");
            alwaysInclude.emplace_back(std::move(glob));
        }
        else if (arg == "--cook-arrays")
            cookArrays = true;
        else if (arg == "--texture-format")
        {
            std::string f;
            if (!takeValue(f))
                return usage("--texture-format needs one of none|astc|bc7|bc3");
            // HE::TextureFormat values, see Hpak::PackSettings::textureCompression.
            if      (f == "none") textureCompression = 0;
            else if (f == "astc") textureCompression = 1;
            else if (f == "bc7")  textureCompression = 2;
            else if (f == "bc3")  textureCompression = 3;
            else return usage("unknown texture format '" + f + "' (expected none|astc|bc7|bc3)");
        }
        else if (arg == "--texture-quality")
        {
            std::string q;
            if (!takeValue(q) || (q != "0" && q != "1" && q != "2"))
                return usage("--texture-quality needs 0, 1 or 2");
            textureQuality = static_cast<uint8_t>(q[0] - '0');
        }
        else if (arg == "--cache")
        {
            if (!takeValue(cachePath))
                return usage("--cache needs a manifest file");
        }
        else
        {
            return usage("unknown argument '" + arg + "'");
        }
    }

    if (!std::filesystem::exists(inputPath))
    {
        std::cerr << "Input path does not exist: " << inputPath << "\n";
        return 1;
    }

    Hpak::PackSettings settings;
    settings.codec = codec;
    settings.excludePatterns = std::move(excludes);
    settings.cook = cook;
    settings.textureCompression = textureCompression;
    settings.textureQuality = textureQuality;
    settings.cookTextureArrays = cookArrays;
    if (!secret.empty())
    {
        settings.encrypt = true;
        uint8_t salt[16] = {};  // zero salt for the packer tool
        KeyDerivation::derive(secret, salt, settings.key);
    }

    if (onlyReferenced)
    {
        namespace fs = std::filesystem;
        settings.onlyReferenced = true;
        settings.alwaysIncludePatterns = alwaysInclude;
        settings.onNotice = [](const std::string& m) { std::cout << m << "\n"; };
        const fs::path content = fs::path(inputPath);
        const auto readAll = [](const fs::path& f) {
            std::ifstream in(f, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        HE::reach::collectProjectTextRoots(content, settings.referenceRoots);
        const fs::path startup = startupScene.empty() ? fs::path() : (content / startupScene).lexically_normal();
        std::error_code ec;
        for (fs::recursive_directory_iterator it(content, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            if (!it->is_regular_file(ec) || it->path().extension() != ".hescene") continue;
            std::vector<uint8_t> bytes = readAll(it->path());
            if (!startup.empty() && it->path().lexically_normal() == startup)
                settings.referenceRoots.push_back(std::move(bytes));
            else if (startup.empty())
                settings.referenceRoots.push_back(std::move(bytes));   // no startup named: all scenes are roots
            else
                settings.reachScenes.push_back({ it->path().lexically_relative(content.parent_path()).generic_string(),
                                                 std::move(bytes) });
        }
    }

    std::vector<HpakWriter::SourceRoot> roots{ { inputPath, {} } };
    if (!engineContent.empty())
        roots.push_back({ engineContent, "Engine/" });
    if (!engineContentCache.empty())
        roots.push_back({ engineContentCache, "Engine/", /*fallbackOnly*/ true });

    // The previous pak stays open while the entries it can donate are read out
    // of it, and is closed before the file is replaced below.
    auto prevPak = std::make_unique<HpakReader>();
    Hpak::IncrementalCache cache;
    const bool haveCache = !cachePath.empty()
        && HE::openIncrementalCache(outputFile, cachePath, settings, *prevPak, cache);
    if (!cachePath.empty() && !haveCache)
        std::cout << "No usable cache (" << cachePath << ") for " << outputFile
                  << ": packing everything\n";

    HpakWriter packer;
    const int added = packer.addDirectories(roots, settings, {}, haveCache ? &cache : nullptr);
    prevPak.reset();
    if (onlyReferenced)
        std::cout << "Left out " << packer.unreferencedCount() << " asset(s), "
                  << packer.unreferencedBytes() / 1024 << " KB, and " << packer.unreachableScenes().size()
                  << " scene(s)\n";
    std::cout << "Packed " << added << " asset(s) from: " << inputPath;
    if (!engineContent.empty()) std::cout << " (+ engine defaults from " << engineContent << ")";
    if (haveCache) std::cout << ", " << packer.reusedCount() << " reused from the cache";
    std::cout << "\n";

    if (std::filesystem::exists(outputFile))
        std::filesystem::remove(outputFile);

    if (!packer.write(outputFile))
    {
        std::cerr << "Failed to write: " << outputFile << "\n";
        return 1;
    }

    std::cout << "Written: " << outputFile << "\n";

    // Best effort: without a manifest the next --cache run just packs everything.
    if (!cachePath.empty()
        && !HE::writeIncrementalManifest(outputFile, cachePath, settings, packer.entryInfos()))
        std::cerr << "hpak_packer: could not write the cache manifest " << cachePath << "\n";
    return 0;
}
