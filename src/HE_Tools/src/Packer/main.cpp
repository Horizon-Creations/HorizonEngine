// hpak_packer — pack a project's .hasset files into a single .hpak archive
// Usage: hpak_packer <project_root> <output.hpak> [--secret <passphrase>]
//
// If --secret is provided the entries are AES-256-GCM encrypted with a key
// derived from the passphrase + a zero salt (KeyDerivation::derive). Reading the
// archive back needs that same 32-byte key: the runtime hands it to
// ContentManager::mountPak() (GameApplication does the mounting for a packaged
// game); loadPak() is the eager tools/tests variant.

#include "HpakWriter.h"
#include <cstdint>
#include "KeyDerivation.h"
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
                 "[--engine-content <dir>] [--exclude <glob>]...\n";
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
    if (!secret.empty())
    {
        settings.encrypt = true;
        uint8_t salt[16] = {};  // zero salt for the packer tool
        KeyDerivation::derive(secret, salt, settings.key);
    }

    std::vector<HpakWriter::SourceRoot> roots{ { inputPath, {} } };
    if (!engineContent.empty())
        roots.push_back({ engineContent, "Engine/" });

    HpakWriter packer;
    const int added = packer.addDirectories(roots, settings);
    std::cout << "Packed " << added << " asset(s) from: " << inputPath;
    if (!engineContent.empty()) std::cout << " (+ engine defaults from " << engineContent << ")";
    std::cout << "\n";

    if (std::filesystem::exists(outputFile))
        std::filesystem::remove(outputFile);

    if (!packer.write(outputFile))
    {
        std::cerr << "Failed to write: " << outputFile << "\n";
        return 1;
    }

    std::cout << "Written: " << outputFile << "\n";
    return 0;
}
