#include <Hpak/HpakWriter.h>
#include <Hpak/Aes256Gcm.h>
#include <ContentManager/HAsset.h>
#include <Types/Enums.h>
#ifdef HE_HAVE_LZ4
#  include <lz4.h>
#  include <lz4hc.h>
#endif
#ifdef HE_HAVE_ZSTD
#  include <zstd.h>
#endif
#include <algorithm>
#include <fstream>
#include <string>
#include <unordered_map>
#include <cstring>

// Extract the asset UUID + embedded path from a raw .hasset blob's META chunk.
// META layout: uint16_t type, uint64_t hi, uint64_t lo, string name, string path.
static bool metaFromHasset(const std::vector<uint8_t>& data, HE::UUID& id, std::string& path)
{
    HAsset::Reader r;
    if (!r.openData(data)) return false;
    const auto* meta = r.findChunk(HAsset::CHUNK_META);
    if (!meta) return false;
    size_t off = sizeof(uint16_t); // skip asset type
    std::string name;
    if (!HAsset::Reader::readPOD(meta->data, off, id.hi))   return false;
    if (!HAsset::Reader::readPOD(meta->data, off, id.lo))   return false;
    if (!HAsset::Reader::readString(meta->data, off, name)) return false;
    if (!HAsset::Reader::readString(meta->data, off, path)) return false;
    return true;
}

// Pack-time reference rewrite: resolve an asset's path-based refs to UUIDs and
// RE-serialize the asset with the path strings dropped — packed assets carry
// UUID refs only (loose editor .hasset files keep paths for debugging):
//   • StaticMesh/SkeletalMesh: MREF (material path) → replaced by MRFU (UUID).
//   • Material: MTRL keeps its PBR scalar tail byte-identical, but shaderPath and
//     texturePaths are written as empty strings; MTLU carries shaderId+textureIds.
//   • Scene: SCNE (object paths) → replaced by SCNU (UUIDs). (objectPaths has no
//     runtime consumer — verified before dropping.)
// A ref whose target isn't in the pack becomes a null UUID placeholder so the
// vectors stay index-parallel; the streaming frontier skips nulls. Assets of any
// other type are returned unchanged.
static std::vector<uint8_t> rewriteRefsForPack(
    const std::vector<uint8_t>& blob,
    const std::unordered_map<std::string, HE::UUID>& pathToUuid)
{
    HAsset::Reader r;
    if (!r.openData(blob)) return blob;
    const auto type = static_cast<HE::AssetType>(r.assetType());

    const bool isMesh  = type == HE::AssetType::StaticMesh || type == HE::AssetType::SkeletalMesh;
    const bool isMat   = type == HE::AssetType::Material;
    const bool isScene = type == HE::AssetType::Scene;
    if (!isMesh && !isMat && !isScene) return blob; // no refs to rewrite

    auto resolve = [&](const std::string& p) -> HE::UUID {
        auto it = pathToUuid.find(p);
        return it != pathToUuid.end() ? it->second : HE::UUID{};
    };

    HAsset::Writer w;
    for (const auto& c : r.chunks())
    {
        if (isMesh && c.id == HAsset::CHUNK_MREF)
        {
            // Replace the material path with its baked UUID.
            size_t o = 0; std::string matPath;
            HAsset::Reader::readString(c.data, o, matPath);
            const HE::UUID mid = matPath.empty() ? HE::UUID{} : resolve(matPath);
            std::vector<uint8_t> d;
            HAsset::Writer::appendPOD(d, mid.hi);
            HAsset::Writer::appendPOD(d, mid.lo);
            w.addChunk(HAsset::CHUNK_MRFU, d.data(), d.size());
            continue;
        }
        if (isMat && c.id == HAsset::CHUNK_MTRL)
        {
            // Read the two leading path fields, then copy the remaining scalar
            // tail (baseColor/metallic/roughness/opacity + any future additions)
            // byte-verbatim — no fragile re-serialization of the PBR values.
            size_t o = 0; std::string shaderPath; std::vector<std::string> texPaths;
            HAsset::Reader::readString(c.data, o, shaderPath);
            HAsset::Reader::readVec(c.data, o, texPaths);

            std::vector<uint8_t> mtrl;
            HAsset::Writer::appendString(mtrl, std::string{});           // shaderPath dropped
            HAsset::Writer::appendVec(mtrl, std::vector<std::string>{}); // texturePaths dropped
            mtrl.insert(mtrl.end(), c.data.begin() + o, c.data.end());   // scalar tail verbatim
            w.addChunk(HAsset::CHUNK_MTRL, mtrl.data(), mtrl.size());

            const HE::UUID sid = shaderPath.empty() ? HE::UUID{} : resolve(shaderPath);
            std::vector<HE::UUID> texIds; texIds.reserve(texPaths.size());
            for (const auto& tp : texPaths) texIds.push_back(resolve(tp));
            std::vector<uint8_t> d;
            HAsset::Writer::appendPOD(d, sid.hi);
            HAsset::Writer::appendPOD(d, sid.lo);
            HAsset::Writer::appendVec(d, texIds);
            w.addChunk(HAsset::CHUNK_MTLU, d.data(), d.size());
            continue;
        }
        if (isScene && c.id == HAsset::CHUNK_SCNE)
        {
            size_t o = 0; std::vector<std::string> objPaths;
            HAsset::Reader::readVec(c.data, o, objPaths);
            std::vector<HE::UUID> objIds; objIds.reserve(objPaths.size());
            for (const auto& op : objPaths) objIds.push_back(resolve(op));
            std::vector<uint8_t> d;
            HAsset::Writer::appendVec(d, objIds);
            w.addChunk(HAsset::CHUNK_SCNU, d.data(), d.size());
            continue;
        }
        w.addChunk(c.id, c.data.data(), c.data.size()); // everything else verbatim
    }
    return w.toBytes(r.assetType());
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

// Glob matcher for PackSettings::excludePatterns: `*` matches any sequence
// (including '/'), `?` matches exactly one character. Iterative backtracking
// (classic wildcard match) — no regex, no recursion.
bool Hpak::globMatch(const std::string& pattern, const std::string& path)
{
    size_t p = 0, s = 0, starP = std::string::npos, starS = 0;
    while (s < path.size())
    {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == path[s]))
        {
            ++p; ++s;
        }
        else if (p < pattern.size() && pattern[p] == '*')
        {
            starP = p++;   // remember the star; try matching zero chars first
            starS = s;
        }
        else if (starP != std::string::npos)
        {
            p = starP + 1; // backtrack: let the star swallow one more char
            s = ++starS;
        }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p; // trailing stars match empty
    return p == pattern.size();
}

int HpakWriter::addDirectory(const std::filesystem::path& rootDir,
                              const Hpak::PackSettings& settings,
                              const AddProgressFn& progress)
{
    std::error_code ec;

    // Pass 1: read every .hasset, extract (UUID, embedded path), and build a
    // path→UUID map. Each asset's own META (path,uuid) pair IS the manifest — no
    // separate manifest file is needed to resolve intra-asset path references.
    struct Pending { std::vector<uint8_t> bytes; HE::UUID id; std::string relPath; };
    std::vector<Pending> pending;
    std::unordered_map<std::string, HE::UUID> pathToUuid;
    // Manual iteration with increment(ec): the range-for's operator++ THROWS on
    // unreadable subdirectories (this runs on the editor's export worker thread,
    // where an escaped exception is std::terminate). skip_permission_denied
    // covers the common case; increment(ec) the rest.
    std::filesystem::recursive_directory_iterator it(
        rootDir, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end)
    {
        const auto& p = *it;
        const bool regular = p.is_regular_file(ec);
        if (ec) { ec.clear(); it.increment(ec); continue; }
        if (!regular || p.path().extension() != ".hasset") { it.increment(ec); continue; }

        // Exclude filter: match against the rootDir-relative path with forward
        // slashes (e.g. "Debug/Test.hasset") — the shape shown in the editor.
        // lexically_relative is pure string math: no symlink resolution (which
        // would match against the TARGET path) and no filesystem access.
        std::string rel = p.path().lexically_relative(rootDir).generic_string();
        if (rel.empty()) rel = p.path().filename().generic_string();
        bool excluded = false;
        for (const auto& pat : settings.excludePatterns)
            if (!pat.empty() && Hpak::globMatch(pat, rel)) { excluded = true; break; }
        if (excluded) { it.increment(ec); continue; }

        do {
            std::ifstream f(p.path(), std::ios::binary);
            if (!f) break;
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());

            HE::UUID id; std::string path;
            if (!metaFromHasset(bytes, id, path) || id == HE::UUID{}) break;
            if (!path.empty()) pathToUuid[path] = id;
            pending.push_back({std::move(bytes), id, std::move(rel)});
        } while (false);
        it.increment(ec);
    }
    ec.clear();

    // Pass 2: rewrite path refs to baked UUIDs (dropping the path strings), then pack.
    // This is the expensive pass (compression + encryption) → progress reports here.
    int count = 0;
    const int total = static_cast<int>(pending.size());
    for (auto& pe : pending)
    {
        if (progress) progress(count, total, pe.relPath);
        addEntry(pe.id, rewriteRefsForPack(pe.bytes, pathToUuid), settings);
        ++count;
    }
    if (progress) progress(count, total, {});
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
