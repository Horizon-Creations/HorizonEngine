#pragma once
#include <Hpak/HpakFormat.h>
#include <Types/UUID.h>
#include <Types/Defines.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>

// Pack raw .hasset blobs into a single .hpak archive.
class HE_API HpakWriter
{
public:
    // Add a raw .hasset memory blob keyed by UUID.
    void addEntry(const HE::UUID& id, const std::vector<uint8_t>& hassetData,
                  const Hpak::PackSettings& settings = Hpak::PackSettings{});

    // Scan rootDir recursively for *.hasset files. UUID is read from the
    // embedded META chunk; files where the UUID cannot be parsed are skipped.
    // Returns the number of entries successfully added.
    int addDirectory(const std::filesystem::path& rootDir,
                     const Hpak::PackSettings& settings = Hpak::PackSettings{});

    // Write all added entries to outputPath. Returns false on I/O error.
    bool write(const std::string& outputPath) const;

    int entryCount() const { return static_cast<int>(m_entries.size()); }

private:
    struct PendingEntry {
        HE::UUID             uuid;
        std::vector<uint8_t> data;        // stored bytes (compressed then encrypted)
        uint32_t             origSize;
        uint64_t             contentHash;  // hash64 of `data`
        uint8_t              codec;        // Hpak::Codec
        uint8_t              flags;        // Hpak::kFlag*
        uint8_t              nonce[12];    // reserved (zero for XOR)
    };
    std::vector<PendingEntry> m_entries;
};
