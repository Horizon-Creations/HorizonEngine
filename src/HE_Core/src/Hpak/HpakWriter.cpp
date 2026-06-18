#include <Hpak/HpakWriter.h>
#include <ContentManager/HAsset.h>
#ifdef HE_HAVE_LZ4
#  include <lz4.h>
#endif
#include <fstream>
#include <cstring>

// Extract the asset UUID from a raw .hasset blob's META chunk.
static HE::UUID uuidFromHasset(const std::vector<uint8_t>& data)
{
    HAsset::Reader r;
    if (!r.openData(data)) return {};
    const auto* meta = r.findChunk(HAsset::CHUNK_META);
    if (!meta) return {};
    // META layout: uint16_t type, uint64_t hi, uint64_t lo, strings...
    constexpr size_t kTypeSize = sizeof(uint16_t);
    if (meta->data.size() < kTypeSize + 16) return {};
    HE::UUID id;
    size_t off = kTypeSize;
    HAsset::Reader::readPOD(meta->data, off, id.hi);
    HAsset::Reader::readPOD(meta->data, off, id.lo);
    return id;
}

void HpakWriter::addEntry(const HE::UUID& id, const std::vector<uint8_t>& hassetData,
                          const Hpak::PackSettings& settings)
{
    PendingEntry e;
    e.uuid     = id;
    e.origSize = static_cast<uint32_t>(hassetData.size());
    e.flags    = 0;

    // Step 1: compress (LZ4) if requested and available
    const uint8_t* src     = hassetData.data();
    size_t         srcSize = hassetData.size();
    std::vector<uint8_t> compressed; // only populated when compress=true

#ifdef HE_HAVE_LZ4
    if (settings.compress && srcSize > 0)
    {
        const int bound = LZ4_compressBound(static_cast<int>(srcSize));
        compressed.resize(static_cast<size_t>(bound));
        const int written = LZ4_compress_default(
            reinterpret_cast<const char*>(src),
            reinterpret_cast<char*>(compressed.data()),
            static_cast<int>(srcSize),
            bound);
        if (written > 0)
        {
            compressed.resize(static_cast<size_t>(written));
            src     = compressed.data();
            srcSize = compressed.size();
            e.flags |= Hpak::kFlagCompressed;
        }
        // on failure (written <= 0) fall through and store uncompressed
    }
#endif

    // Step 2: encrypt (XOR) if requested
    if (settings.encrypt)
    {
        e.flags |= Hpak::kFlagEncrypted;
        e.data.resize(srcSize);
        for (size_t i = 0; i < srcSize; ++i)
            e.data[i] = src[i] ^ settings.key[i % 32];
    }
    else
    {
        e.data.assign(src, src + srcSize);
    }

    m_entries.push_back(std::move(e));
}

int HpakWriter::addDirectory(const std::filesystem::path& rootDir,
                              const Hpak::PackSettings& settings)
{
    int count = 0;
    std::error_code ec;
    for (const auto& p : std::filesystem::recursive_directory_iterator(rootDir, ec))
    {
        if (!p.is_regular_file(ec) || p.path().extension() != ".hasset") continue;

        std::ifstream f(p.path(), std::ios::binary);
        if (!f) continue;

        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());

        const HE::UUID id = uuidFromHasset(bytes);
        if (id == HE::UUID{}) continue;

        addEntry(id, bytes, settings);
        ++count;
    }
    return count;
}

bool HpakWriter::write(const std::string& outputPath) const
{
    std::ofstream f(outputPath, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    const uint64_t dataStart =
        sizeof(Hpak::FileHeader) +
        static_cast<uint64_t>(m_entries.size()) * sizeof(Hpak::EntryDesc);

    // Header
    Hpak::FileHeader hdr{};
    std::memcpy(hdr.magic, Hpak::k_magic, 4);
    hdr.version    = Hpak::k_version;
    hdr.entryCount = static_cast<uint32_t>(m_entries.size());
    hdr.flags      = 0;
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    // TOC — compute offsets on the fly
    uint64_t offset = dataStart;
    for (const auto& e : m_entries)
    {
        Hpak::EntryDesc desc{};
        desc.uuidHi     = e.uuid.hi;
        desc.uuidLo     = e.uuid.lo;
        desc.origSize   = e.origSize;
        desc.dataSize   = static_cast<uint32_t>(e.data.size());
        desc.dataOffset = offset;
        desc.entryFlags = e.flags;
        f.write(reinterpret_cast<const char*>(&desc), sizeof(desc));
        offset += e.data.size();
    }

    // Data blocks
    for (const auto& e : m_entries)
    {
        if (!e.data.empty())
            f.write(reinterpret_cast<const char*>(e.data.data()),
                    static_cast<std::streamsize>(e.data.size()));
    }

    return f.good();
}
