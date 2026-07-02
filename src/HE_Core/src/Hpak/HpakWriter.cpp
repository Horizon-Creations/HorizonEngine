#include <Hpak/HpakWriter.h>
#include <Hpak/Aes256Gcm.h>
#include <ContentManager/HAsset.h>
#ifdef HE_HAVE_LZ4
#  include <lz4.h>
#  include <lz4hc.h>
#endif
#ifdef HE_HAVE_ZSTD
#  include <zstd.h>
#endif
#include <algorithm>
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
    e.codec    = static_cast<uint8_t>(Hpak::Codec::Store);
    e.flags    = 0;
    std::memset(e.nonce, 0, sizeof(e.nonce));

    // Step 1: compress via the requested codec (falls back to Store on failure
    // or when the codec's library is unavailable at build time).
    const uint8_t* src     = hassetData.data();
    size_t         srcSize = hassetData.size();
    std::vector<uint8_t> compressed; // populated when a codec succeeds

    if (settings.codec == Hpak::Codec::LZ4 && srcSize > 0)
    {
#ifdef HE_HAVE_LZ4
        const int bound = LZ4_compressBound(static_cast<int>(srcSize));
        compressed.resize(static_cast<size_t>(bound));
        const int level  = settings.level > 0 ? settings.level : 9; // LZ4HC default
        const int written = LZ4_compress_HC(
            reinterpret_cast<const char*>(src),
            reinterpret_cast<char*>(compressed.data()),
            static_cast<int>(srcSize), bound, level);
        if (written > 0)
        {
            compressed.resize(static_cast<size_t>(written));
            src     = compressed.data();
            srcSize = compressed.size();
            e.codec = static_cast<uint8_t>(Hpak::Codec::LZ4);
        }
#endif
    }
    else if (settings.codec == Hpak::Codec::Zstd && srcSize > 0)
    {
#ifdef HE_HAVE_ZSTD
        const size_t bound = ZSTD_compressBound(srcSize);
        compressed.resize(bound);
        const int level = settings.level > 0 ? settings.level : 19; // ship default
        const size_t written = ZSTD_compress(
            compressed.data(), bound, src, srcSize, level);
        if (!ZSTD_isError(written) && written > 0)
        {
            compressed.resize(written);
            src     = compressed.data();
            srcSize = compressed.size();
            e.codec = static_cast<uint8_t>(Hpak::Codec::Zstd);
        }
#endif
    }

    // Step 2: encrypt (AES-256-GCM) if requested and a crypto backend is present.
    // The stored blob becomes ciphertext || 16-byte auth tag, with a fresh random
    // 96-bit nonce recorded in the entry. Obfuscation, not a security guarantee
    // (see Aes256Gcm.h). Falls back to storing plaintext if encryption fails.
    if (settings.encrypt && Hpak::cryptoAvailable())
    {
        uint8_t nonce[12];
        std::vector<uint8_t> ct;
        if (Hpak::randomBytes(nonce, sizeof(nonce)) &&
            Hpak::aesGcmEncrypt(settings.key, nonce, src, srcSize, ct))
        {
            e.flags |= Hpak::kFlagEncrypted;
            std::memcpy(e.nonce, nonce, sizeof(nonce));
            e.data = std::move(ct);
        }
        else
        {
            e.data.assign(src, src + srcSize);
        }
    }
    else
    {
        e.data.assign(src, src + srcSize);
    }

    // Step 3: integrity hash over the stored bytes
    e.contentHash = Hpak::hash64(e.data.data(), e.data.size());

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

    // TOC is written ASCENDING by UUID so the reader can binary-search. Sort an
    // index rather than the entries themselves (keeps addEntry order stable).
    std::vector<size_t> order(m_entries.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const auto& ua = m_entries[a].uuid;
        const auto& ub = m_entries[b].uuid;
        return ua.hi != ub.hi ? ua.hi < ub.hi : ua.lo < ub.lo;
    });

    const uint64_t dataStart =
        sizeof(Hpak::FileHeader) +
        static_cast<uint64_t>(m_entries.size()) * sizeof(Hpak::EntryDesc);
    // (dictOffset/dictSize stay 0 in Phase A — no shared dictionary yet.)

    // Build the TOC in memory first so we can hash it before writing.
    std::vector<uint8_t> toc(m_entries.size() * sizeof(Hpak::EntryDesc));
    uint64_t offset       = dataStart;
    uint32_t archiveFlags = Hpak::kArchiveSortedTOC;
    for (size_t k = 0; k < order.size(); ++k)
    {
        const auto& e = m_entries[order[k]];
        Hpak::EntryDesc desc{};
        desc.uuidHi      = e.uuid.hi;
        desc.uuidLo      = e.uuid.lo;
        desc.dataOffset  = offset;
        desc.origSize    = e.origSize;
        desc.dataSize    = static_cast<uint32_t>(e.data.size());
        desc.contentHash = e.contentHash;
        std::memcpy(desc.nonce, e.nonce, sizeof(desc.nonce));
        desc.codec       = e.codec;
        desc.entryFlags  = e.flags;
        std::memcpy(toc.data() + k * sizeof(Hpak::EntryDesc), &desc, sizeof(desc));
        if (e.flags & Hpak::kFlagEncrypted) archiveFlags |= Hpak::kArchiveEncrypted;
        offset += e.data.size();
    }

    // Header
    Hpak::FileHeader hdr{};
    std::memcpy(hdr.magic, Hpak::k_magic, 4);
    hdr.version       = Hpak::k_version;
    hdr.entryCount    = static_cast<uint32_t>(m_entries.size());
    hdr.flags         = archiveFlags;
    hdr.buildId       = 0;
    hdr.baseArchiveId = 0;
    hdr.tocHash       = Hpak::hash64(toc.data(), toc.size());
    hdr.dictOffset    = 0;
    hdr.dictSize      = 0;
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    // TOC
    if (!toc.empty())
        f.write(reinterpret_cast<const char*>(toc.data()),
                static_cast<std::streamsize>(toc.size()));

    // Data blocks — same (sorted) order as the TOC offsets
    for (size_t k = 0; k < order.size(); ++k)
    {
        const auto& e = m_entries[order[k]];
        if (!e.data.empty())
            f.write(reinterpret_cast<const char*>(e.data.data()),
                    static_cast<std::streamsize>(e.data.size()));
    }

    return f.good();
}
