// hpak_packer — pack a project's .hasset files into a single .hpak archive
// Usage: hpak_packer <project_root> <output.hpak> [--secret <passphrase>]
//
// If --secret is provided the entries are XOR-encrypted with a key derived
// from the passphrase + a zero salt. The same passphrase must be passed to
// ContentManager::loadPak() at runtime.

#include "HpakWriter.h"
#include <cstdint>
#include "KeyDerivation.h"
#include <iostream>
#include <string>
#include <filesystem>
#include <cstring>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: hpak_packer <project_root> <output.hpak> "
                     "[--codec store|lz4|zstd] [--secret <passphrase>] "
                     "[--exclude <glob>]...\n";
        return 1;
    }

    const std::string inputPath  = argv[1];
    const std::string outputFile = argv[2];

    std::string   secret;
    Hpak::Codec   codec = Hpak::Codec::Zstd; // sensible ship default
    std::vector<std::string> excludes;
    for (int i = 3; i < argc - 1; ++i)
    {
        if (std::strcmp(argv[i], "--secret") == 0)
        {
            secret = argv[i + 1];
            ++i;
        }
        else if (std::strcmp(argv[i], "--codec") == 0)
        {
            const std::string c = argv[i + 1];
            if      (c == "store") codec = Hpak::Codec::Store;
            else if (c == "lz4")   codec = Hpak::Codec::LZ4;
            else if (c == "zstd")  codec = Hpak::Codec::Zstd;
            ++i;
        }
        else if (std::strcmp(argv[i], "--exclude") == 0)
        {
            // Repeatable. Glob vs the project-root-relative path ('*' spans '/').
            excludes.emplace_back(argv[i + 1]);
            ++i;
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

    HpakWriter packer;
    const int added = packer.addDirectory(inputPath, settings);
    std::cout << "Packed " << added << " asset(s) from: " << inputPath << "\n";

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
