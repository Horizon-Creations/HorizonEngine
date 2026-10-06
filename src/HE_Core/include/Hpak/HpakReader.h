#pragma once
#include <Hpak/HpakFormat.h>
#include <Types/UUID.h>
#include <Types/Defines.h>
#include <fstream>
#include <memory>
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

    // The validated table of contents, shared read-only between readers of the
    // same archive. Opaque outside this class.
    struct Toc;
    // This reader's TOC for openShared(); null before a successful open().
    std::shared_ptr<const Toc> sharedToc() const { return m_toc; }
    // Open `path` with a TOC another reader already read and validated. Only the
    // header is read, to check the file still carries that TOC (same tocHash):
    // a streaming job reads one entry, and re-reading and re-hashing the whole
    // table for it made pak streaming quadratic in the entry count. Logs nothing
    // on success. False when the file is gone or now holds a different archive.
    bool openShared(const std::string& path, std::shared_ptr<const Toc> toc);

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
    // their content hash, the entry carries a reserved flag this build cannot
    // honour (Hpak::kFlagUsesDict / kFlagBlockFramed), or decode/decrypt fails.
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

    // Locate an entry and read its STORED bytes (compressed+encrypted, exactly as
    // written) into `out`, content-hash verified. Returns the entry's metadata, or
    // nullptr when absent/unreadable/corrupt. Shared prologue of readStoredEntry
    // (verbatim re-pack) and readEntry (decode).
    const EntryMeta* readStoredBytes(const HE::UUID& id, std::vector<uint8_t>& out) const;

    std::string                m_path;
    mutable std::ifstream      m_file;   // held open for the reader's lifetime
    uint64_t                   m_tocHash = 0;
    std::shared_ptr<const Toc> m_toc;    // entries sorted by UUID; never null after open()
};
