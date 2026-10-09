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
//
// Two levels of reuse, cheapest first:
//   1. `entries` — the whole pack pipeline (ref rewrite, cook, import-source strip,
//      hash) is skipped. Valid while the asset's SOURCE bytes hash the same AND
//      everything the pipeline looked at to produce the last result still reads the
//      same (see PackDeps). This is the one that saves the minutes: the cook (mip
//      chains, ASTC/BC7 block compression) and the shader precompile live in there.
//   2. `srcHashes` — the pipeline ran, but its output hashes to what the previous
//      archive holds, so only compression + encryption are saved.
struct PackDeps {
    // Every path the pipeline resolved to a UUID for this asset (material/mesh/scene
    // refs), with the UUID it got — {} when the target is not in the pack. Reuse is
    // only valid while each of these still resolves to the same UUID: renaming,
    // deleting or adding the asset it points to changes what gets baked into THIS one
    // without touching its file.
    std::vector<std::pair<std::string, HE::UUID>> refs;
    // Material INSTANCES fold their approximation from the parent chain's source
    // bytes: (parent path, hash64 of its source blob; 0 = not found).
    std::vector<std::pair<std::string, uint64_t>>  parents;
};
struct CachedEntry {
    uint64_t srcHash = 0;   // hash64(final cooked blob) — level 2
    uint64_t inHash  = 0;   // hash64(source .hasset bytes); 0 = unknown, never reused
    PackDeps deps;
};
struct IncrementalCache {
    const HpakReader* previousPak = nullptr;
    std::unordered_map<HE::UUID, uint64_t>     srcHashes; // uuid → hash64(rewritten blob)
    std::unordered_map<HE::UUID, CachedEntry>  entries;   // uuid → what produced it
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

    // One directory to scan, plus the path prefix its assets are ADDRESSED by
    // (not where they live on disk): "" for the project's own Content root,
    // "Engine/" for the engine-wide default content root — exactly the prefixes
    // ContentManager::resolveAbsolutePath understands, so a reference stored as
    // "Engine/Meshes/Cube.hasset" resolves against a pack built from both roots.
    struct SourceRoot {
        std::filesystem::path dir;
        std::string           pathPrefix;
    };

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

    // Same, over SEVERAL roots packed into one archive — the shipping case,
    // where the engine's default content (primitive meshes, default materials)
    // has to travel with the project's own Content or every scene reference to
    // a built-in asset dangles at runtime. Roots are scanned in order and the
    // FIRST one to claim a UUID wins, so a project-local override of an engine
    // default (Content/Engine/…) shadows the shared default exactly like it does
    // in the editor. The path→UUID map used to bake references spans all roots.
    int addDirectories(const std::vector<SourceRoot>& roots,
                       const Hpak::PackSettings& settings = Hpak::PackSettings{},
                       const AddProgressFn& progress = {},
                       const Hpak::IncrementalCache* cache = nullptr);

    // Write all added entries to outputPath. Returns false on I/O error.
    bool write(const std::string& outputPath) const;

    int entryCount() const { return static_cast<int>(m_entries.size()); }

    // After addDirectory: (uuid, hash64 of the rewritten blob) for every added
    // entry — the caller persists these as the next incremental-pack manifest.
    const std::vector<std::pair<HE::UUID, uint64_t>>& sourceHashes() const { return m_srcHashes; }
    // After addDirectory: everything the NEXT incremental pack needs to skip an
    // unchanged asset outright (see Hpak::CachedEntry) — what the caller persists.
    const std::vector<std::pair<HE::UUID, Hpak::CachedEntry>>& entryInfos() const { return m_entryInfos; }
    // How many addDirectory entries were carried over verbatim from the cache.
    int reusedCount() const { return m_reused; }
    // After addDirectory: content-relative path → baked asset UUID for every
    // scanned .hasset. The exporter serializes this as the pak's __asset_index__
    // so the game can resolve loadAsset("<path>") to a mounted-pak UUID (pak
    // entries are UUID-keyed — the path is otherwise unrecoverable at runtime).
    const std::unordered_map<std::string, HE::UUID>& packedPaths() const { return m_packedPaths; }
    // After addDirectory: baked asset UUID → HAsset header asset type for every
    // scanned .hasset, read from the SAME open Reader that extracts the UUID —
    // the type is free here and costs a second open of every asset in the project
    // anywhere else. The exporter serializes this as the pak's __asset_types__ so
    // a shipped game knows an asset's type BEFORE loading it (ContentManager only
    // learns the type of an asset it has already loaded, which is too late for
    // discoverAssets — the call that finds a game's startup classes).
    const std::unordered_map<HE::UUID, uint16_t>& packedTypes() const { return m_packedTypes; }

private:
    struct PendingEntry {
        HE::UUID             uuid;
        std::vector<uint8_t> data;        // stored bytes (compressed then encrypted)
        uint32_t             origSize;
        uint64_t             contentHash;  // hash64 of `data`
        uint8_t              codec;        // Hpak::Codec
        uint8_t              flags;        // Hpak::kFlag*
        uint8_t              nonce[12];    // AES-GCM nonce; zero when unencrypted
    };
    std::vector<PendingEntry> m_entries;
    std::vector<std::pair<HE::UUID, uint64_t>> m_srcHashes; // filled by addDirectory
    std::vector<std::pair<HE::UUID, Hpak::CachedEntry>> m_entryInfos; // filled by addDirectory
    std::unordered_map<std::string, HE::UUID>  m_packedPaths; // filled by addDirectory
    std::unordered_map<HE::UUID, uint16_t>     m_packedTypes; // filled by addDirectory
    int m_reused = 0;
};
