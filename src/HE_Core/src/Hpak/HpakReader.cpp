#include <Hpak/HpakReader.h>
#ifdef HE_HAVE_LZ4
#  include <lz4.h>
#endif
#include <fstream>
#include <cstring>

bool HpakReader::open(const std::string& path)
{
    m_path = path;
    m_entries.clear();

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    Hpak::FileHeader hdr{};
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!f) return false;
    if (std::memcmp(hdr.magic, Hpak::k_magic, 4) != 0) return false;
    if (hdr.version != Hpak::k_version) return false;

    m_entries.resize(hdr.entryCount);
    for (auto& e : m_entries)
    {
        Hpak::EntryDesc desc{};
        f.read(reinterpret_cast<char*>(&desc), sizeof(desc));
        if (!f) return false;
        e.uuid       = { desc.uuidHi, desc.uuidLo };
        e.origSize   = desc.origSize;
        e.dataSize   = desc.dataSize;
        e.dataOffset = desc.dataOffset;
        e.flags      = desc.entryFlags;
    }
    return true;
}

bool HpakReader::hasEntry(const HE::UUID& id) const
{
    for (const auto& e : m_entries)
        if (e.uuid == id) return true;
    return false;
}

std::vector<HE::UUID> HpakReader::enumerate() const
{
    std::vector<HE::UUID> ids;
    ids.reserve(m_entries.size());
    for (const auto& e : m_entries)
        ids.push_back(e.uuid);
    return ids;
}

std::vector<uint8_t> HpakReader::readEntry(const HE::UUID& id, const uint8_t key[32]) const
{
    for (const auto& e : m_entries)
    {
        if (e.uuid != id) continue;

        std::ifstream f(m_path, std::ios::binary);
        if (!f) return {};

        f.seekg(static_cast<std::streamoff>(e.dataOffset));
        if (!f) return {};

        std::vector<uint8_t> raw(e.dataSize);
        f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(e.dataSize));
        if (!f && e.dataSize > 0) return {};

        // Step 1: decrypt (order matches write: compress → encrypt)
        if ((e.flags & Hpak::kFlagEncrypted) && key)
        {
            for (size_t i = 0; i < raw.size(); ++i)
                raw[i] ^= key[i % 32];
        }

        // Step 2: decompress (LZ4)
        if (e.flags & Hpak::kFlagCompressed)
        {
#ifdef HE_HAVE_LZ4
            if (e.origSize == 0) return {};
            std::vector<uint8_t> decompressed(e.origSize);
            const int result = LZ4_decompress_safe(
                reinterpret_cast<const char*>(raw.data()),
                reinterpret_cast<char*>(decompressed.data()),
                static_cast<int>(raw.size()),
                static_cast<int>(e.origSize));
            if (result != static_cast<int>(e.origSize)) return {};
            return decompressed;
#else
            // LZ4 not available at build time — return empty to signal failure
            return {};
#endif
        }

        return raw;
    }
    return {};
}
