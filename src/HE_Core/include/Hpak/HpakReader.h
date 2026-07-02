#pragma once
#include <Hpak/HpakFormat.h>
#include <Types/UUID.h>
#include <Types/Defines.h>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>

// Read entries from a .hpak archive. Construct once per file; thread-unsafe.
// The TOC is loaded once at open() and looked up via binary search (the archive
// stores it ascending by UUID); a single file handle is held for the reader's
// lifetime rather than reopened per read.
class HE_API HpakReader
{
public:
    // Open, validate the header + TOC hash, and load the TOC. Returns false on
    // I/O errors, a bad magic/version, or a TOC-hash mismatch (corruption).
    bool open(const std::string& path);

    bool hasEntry(const HE::UUID& id) const;

    // All UUIDs present in this archive (in stored / sorted order).
    std::vector<HE::UUID> enumerate() const;

    // TOC hash of the opened archive (identity for incremental-pack manifests).
    uint64_t tocHash() const { return m_tocHash; }

    // One entry's STORED representation: the on-disk bytes (compressed +
    // encrypted, exactly as written) plus the TOC metadata needed to carry the
    // entry verbatim into another archive. Used by incremental packing to skip
    // re-compressing unchanged assets.
    struct StoredEntry {
        std::vector<uint8_t> data;        // stored bytes, content-hash verified
        uint32_t             origSize = 0;
        uint64_t             contentHash = 0;
        uint8_t              codec = 0;   // Hpak::Codec
        uint8_t              flags = 0;   // Hpak::kFlag*
        uint8_t              nonce[12] = {};
    };
    // False when the entry is absent, unreadable, or fails its content hash.
    bool readStoredEntry(const HE::UUID& id, StoredEntry& out) const;

    // Read and return the raw (decrypted, decompressed) .hasset bytes for one
    // entry. Pass key=nullptr for unencrypted entries. Returns an empty vector
    // when the entry is not found, the file is unreadable, the stored bytes fail
    // their content hash, or decode/decrypt fails.
    std::vector<uint8_t> readEntry(const HE::UUID& id,
                                   const uint8_t   key[32] = nullptr) const;

private:
    struct EntryMeta {
        HE::UUID uuid;
        uint32_t origSize;
        uint32_t dataSize;
        uint64_t dataOffset;
        uint64_t contentHash;
        uint8_t  nonce[12];
        uint8_t  codec;
        uint8_t  flags;
    };

    // Binary search over the sorted TOC. Returns nullptr when absent.
    const EntryMeta* find(const HE::UUID& id) const;

    std::string            m_path;
    mutable std::ifstream  m_file;   // held open for the reader's lifetime
    uint64_t               m_tocHash = 0;
    std::vector<EntryMeta> m_entries;
};
