#include <Hpak/HpakReader.h>
#include <cstdint>
#include <Hpak/Aes256Gcm.h>
#ifdef HE_HAVE_LZ4
#  include <lz4.h>
#endif
#ifdef HE_HAVE_ZSTD
#  include <zstd.h>
#endif
#include <algorithm>
#include <cstring>

bool HpakReader::open(const std::string& path)
{
    m_path = path;
    m_entries.clear();
    if (m_file.is_open()) m_file.close();

    m_file.open(path, std::ios::binary);
    if (!m_file) return false;

    Hpak::FileHeader hdr{};
    m_file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!m_file) return false;
    if (std::memcmp(hdr.magic, Hpak::k_magic, 4) != 0) return false;
    if (hdr.version != Hpak::k_version) return false;

    // Read the whole TOC region so we can validate its hash before trusting it.
    const size_t tocBytes = static_cast<size_t>(hdr.entryCount) * sizeof(Hpak::EntryDesc);
    std::vector<uint8_t> toc(tocBytes);
    if (tocBytes > 0)
    {
        m_file.read(reinterpret_cast<char*>(toc.data()),
                    static_cast<std::streamsize>(tocBytes));
        if (!m_file) return false;
    }
    if (Hpak::hash64(toc.data(), toc.size()) != hdr.tocHash)
        return false; // corrupt / truncated TOC
    m_tocHash = hdr.tocHash;

    m_entries.resize(hdr.entryCount);
    for (uint32_t i = 0; i < hdr.entryCount; ++i)
    {
        Hpak::EntryDesc desc{};
        std::memcpy(&desc, toc.data() + static_cast<size_t>(i) * sizeof(desc), sizeof(desc));
        EntryMeta& e = m_entries[i];
        e.uuid        = { desc.uuidHi, desc.uuidLo };
        e.origSize    = desc.origSize;
        e.dataSize    = desc.dataSize;
        e.dataOffset  = desc.dataOffset;
        e.contentHash = desc.contentHash;
        std::memcpy(e.nonce, desc.nonce, sizeof(e.nonce));
        e.codec       = desc.codec;
        e.flags       = desc.entryFlags;
    }
    return true;
}

const HpakReader::EntryMeta* HpakReader::find(const HE::UUID& id) const
{
    // TOC is ascending by (hi,lo) → binary search.
    auto it = std::lower_bound(m_entries.begin(), m_entries.end(), id,
        [](const EntryMeta& e, const HE::UUID& key) {
            return e.uuid.hi != key.hi ? e.uuid.hi < key.hi : e.uuid.lo < key.lo;
        });
    if (it != m_entries.end() && it->uuid == id) return &*it;
    return nullptr;
}

bool HpakReader::hasEntry(const HE::UUID& id) const
{
    return find(id) != nullptr;
}

std::vector<HE::UUID> HpakReader::enumerate() const
{
    std::vector<HE::UUID> ids;
    ids.reserve(m_entries.size());
    for (const auto& e : m_entries)
        ids.push_back(e.uuid);
    return ids;
}

bool HpakReader::readStoredEntry(const HE::UUID& id, StoredEntry& out) const
{
    const EntryMeta* e = find(id);
    if (!e || !m_file.is_open()) return false;
    if (e->dataSize > 0x7FFFFFFFu || e->origSize > 0x7FFFFFFFu) return false;

    m_file.clear(); // drop sticky fail/eof bits from a previous read
    m_file.seekg(static_cast<std::streamoff>(e->dataOffset));
    if (!m_file) return false;

    out.data.resize(e->dataSize);
    if (e->dataSize > 0)
    {
        m_file.read(reinterpret_cast<char*>(out.data.data()),
                    static_cast<std::streamsize>(e->dataSize));
        if (!m_file) return false;
    }
    // Verify before handing the bytes on — a corrupt stored entry must be
    // repacked from source, never carried verbatim into the next archive.
    if (Hpak::hash64(out.data.data(), out.data.size()) != e->contentHash) return false;

    out.origSize    = e->origSize;
    out.contentHash = e->contentHash;
    out.codec       = e->codec;
    out.flags       = e->flags;
    std::memcpy(out.nonce, e->nonce, sizeof(out.nonce));
    return true;
}

std::vector<uint8_t> HpakReader::readEntry(const HE::UUID& id, const uint8_t key[32]) const
{
    const EntryMeta* e = find(id);
    // Use is_open() (independent of stream state), NOT operator bool(): a prior
    // failed read leaves a sticky failbit on this persistent handle, and testing
    // it here — before clear() — would make one bad entry poison every later read.
    if (!e || !m_file.is_open()) return {};

    // Reject implausibly large sizes up front: origSize/dataSize are uint32 (up to
    // ~4 GB) but the LZ4 and OpenSSL update APIs take int, so a crafted entry with
    // size > INT_MAX would overflow the cast. Also avoids a huge speculative alloc.
    if (e->dataSize > 0x7FFFFFFFu || e->origSize > 0x7FFFFFFFu) return {};

    m_file.clear(); // drop any sticky fail/eof bits from a previous read
    m_file.seekg(static_cast<std::streamoff>(e->dataOffset));
    if (!m_file) return {};

    std::vector<uint8_t> raw(e->dataSize);
    if (e->dataSize > 0)
    {
        m_file.read(reinterpret_cast<char*>(raw.data()),
                    static_cast<std::streamsize>(e->dataSize));
        if (!m_file) return {};
    }

    // Content-hash check on the stored bytes (corruption detection).
    if (Hpak::hash64(raw.data(), raw.size()) != e->contentHash) return {};

    // Step 1: decrypt (AES-256-GCM; order matches write: compress → encrypt).
    // The auth tag makes a wrong key / tampered pak fail here rather than
    // silently yielding garbage.
    if (e->flags & Hpak::kFlagEncrypted)
    {
        if (!key) return {};                 // encrypted entry needs a key
        std::vector<uint8_t> pt;
        if (!Hpak::aesGcmDecrypt(key, e->nonce, raw.data(), raw.size(), pt))
            return {};                        // wrong key / tampered / no backend
        raw = std::move(pt);
    }

    // Step 2: decompress by codec
    switch (static_cast<Hpak::Codec>(e->codec))
    {
    case Hpak::Codec::Store:
        return raw;

    case Hpak::Codec::LZ4:
    {
#ifdef HE_HAVE_LZ4
        if (e->origSize == 0) return {};
        std::vector<uint8_t> out(e->origSize);
        const int result = LZ4_decompress_safe(
            reinterpret_cast<const char*>(raw.data()),
            reinterpret_cast<char*>(out.data()),
            static_cast<int>(raw.size()),
            static_cast<int>(e->origSize));
        if (result != static_cast<int>(e->origSize)) return {};
        return out;
#else
        return {};
#endif
    }

    case Hpak::Codec::Zstd:
    {
#ifdef HE_HAVE_ZSTD
        if (e->origSize == 0) return {};
        std::vector<uint8_t> out(e->origSize);
        const size_t result = ZSTD_decompress(
            out.data(), out.size(), raw.data(), raw.size());
        if (ZSTD_isError(result) || result != e->origSize) return {};
        return out;
#else
        return {};
#endif
    }
    }
    return {};
}
