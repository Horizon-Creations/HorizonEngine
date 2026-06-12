// hpak_packer.exe entrypoint
// Usage: hpak_packer <project_root> <output.hpak>
//
// Scans <project_root> for .hasset files, derives the AES-256 key,
// and writes them as an encrypted .hpak archive.

#include "HpakWriter.h"
#include "KeyDerivation.h"
#include <iostream>
#include <string>
#include <filesystem>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: hpak_packer <project_root> <output.hpak>" << std::endl;
        return 1;
    }
    std::string inputPath = argv[1];
    std::string outputFile = argv[2];
	if (!std::filesystem::exists(inputPath))
    {
        std::cerr << "Input path does not exist: " << inputPath << std::endl;
        return 1;
    }
    if (std::filesystem::exists(outputFile))
    {
        std::filesystem::remove(outputFile);
    }
    //Implement the actual hpack creation after this.
    return 0;
}
