#pragma once
#include <Hpak/HpakFormat.h>
#include <Hpak/HpakReader.h>
#include <Types/UUID.h>
#include <Types/Defines.h>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <cstdint>

namespace Hpak {
// Incremental-pack input: the previously written archive plus the source hash
// (hash64 of the REWRITTEN .hasset blob, pre-compression) each of its entries
// was packed from. When a directory scan produces a blob whose hash matches,
// the stored (compressed + encrypted) bytes are carried over verbatim instead
// of being re-compressed. The caller is responsible for only supplying a cache
// whose pack settings (codec/level/encrypt/key) match the current ones.
struct IncrementalCache {
    const HpakReader* previousPak = nullptr;
    std::unordered_map<HE::UUID, uint64_t> srcHashes; // uuid → hash64(rewritten blob)
};
} // namespace Hpak

// Pack raw .hasset blobs into a single .hpak archive.
class HE_API HpakWriter
{
public:
    // Add a raw .hasset memory blob keyed by UUID.
    void addEntry(const HE::UUID& id, const std::vector<uint8_t>& hassetData,
                  const Hpak::PackSettings& settings = Hpak::PackSettings{});

    // Add an entry whose STORED representation is already known (carried
    // verbatim from a previous archive) — no compression/encryption happens.
    void addPackedEntry(const HE::UUID& id, const HpakReader::StoredEntry& stored);

    // Progress callback for addDirectory: (entriesDone, entriesTotal, currentFile).
    // Called before each file is packed and once more with (total, total, "") at
    // the end. currentFile is the rootDir-relative path being packed.
    using AddProgressFn = std::function<void(int, int, const std::string&)>;

    // Scan rootDir recursively for *.hasset files. UUID is read from the
    // embedded META chunk; files where the UUID cannot be parsed are skipped,
    // as are files matching settings.excludePatterns (see PackSettings).
    // With a cache, unchanged entries (same rewritten-blob hash) are copied
    // verbatim from the previous archive (see reusedCount()).
    // Returns the number of entries successfully added.
    int addDirectory(const std::filesystem::path& rootDir,
                     const Hpak::PackSettings& settings = Hpak::PackSettings{},
                     const AddProgressFn& progress = {},
                     const Hpak::IncrementalCache* cache = nullptr);

    // Write all added entries to outputPath. Returns false on I/O error.
    bool write(const std::string& outputPath) const;

    int entryCount() const { return static_cast<int>(m_entries.size()); }

    // After addDirectory: (uuid, hash64 of the rewritten blob) for every added
    // entry — the caller persists these as the next incremental-pack manifest.
    const std::vector<std::pair<HE::UUID, uint64_t>>& sourceHashes() const { return m_srcHashes; }
    // How many addDirectory entries were carried over verbatim from the cache.
    int reusedCount() const { return m_reused; }

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
    std::vector<std::pair<HE::UUID, uint64_t>> m_srcHashes; // filled by addDirectory
    int m_reused = 0;
};
