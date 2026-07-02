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
    std::vector<EntryMeta> m_entries;
};
