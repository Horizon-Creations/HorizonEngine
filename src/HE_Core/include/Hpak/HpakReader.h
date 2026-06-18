#pragma once
#include <Hpak/HpakFormat.h>
#include <Types/UUID.h>
#include <Types/Defines.h>
#include <string>
#include <vector>
#include <cstdint>

// Read entries from a .hpak archive. Construct once per file; thread-unsafe.
class HE_API HpakReader
{
public:
    // Open and parse the TOC. Returns false on I/O or format errors.
    bool open(const std::string& path);

    bool hasEntry(const HE::UUID& id) const;

    // All UUIDs present in this archive.
    std::vector<HE::UUID> enumerate() const;

    // Read and return the raw (decrypted, decompressed) .hasset bytes for one
    // entry. Pass key=nullptr for unencrypted entries. Returns an empty vector
    // when the entry is not found, the file is unreadable, or decryption fails.
    std::vector<uint8_t> readEntry(const HE::UUID& id,
                                   const uint8_t   key[32] = nullptr) const;

private:
    struct EntryMeta {
        HE::UUID uuid;
        uint32_t origSize;
        uint32_t dataSize;
        uint64_t dataOffset;
        uint8_t  flags;
    };

    std::string            m_path;
    std::vector<EntryMeta> m_entries;
};
