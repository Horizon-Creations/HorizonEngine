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
#ifdef HE_HAVE_ASTCENC
#  include <astcenc.h>
#endif
#include <algorithm>
#include <fstream>
#include <string>
#include <unordered_map>
#include <cstring>

#ifdef HE_HAVE_ASTCENC
// Encode one RGBA8 image to ASTC 4x4 (LDR). Returns empty on failure. Each 4x4
// block is 16 bytes; the output is ceil(w/4)*ceil(h/4)*16 bytes.
static std::vector<uint8_t> encodeAstc4x4(const uint8_t* rgba, uint32_t w, uint32_t h)
{
    astcenc_config cfg{};
    if (astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_FAST, 0, &cfg) != ASTCENC_SUCCESS)
        return {};
    astcenc_context* ctx = nullptr;
    if (astcenc_context_alloc(&cfg, 1, &ctx) != ASTCENC_SUCCESS) return {};

    astcenc_image img{};
    img.dim_x = w; img.dim_y = h; img.dim_z = 1;
    img.data_type = ASTCENC_TYPE_U8;
    void* slice = const_cast<uint8_t*>(rgba);
    void* slices[1] = { slice };
    img.data = slices;

    const astcenc_swizzle swz{ ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
    const size_t blocks = static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4);
    std::vector<uint8_t> out(blocks * 16);
    const astcenc_error e = astcenc_compress_image(ctx, &img, &swz, out.data(), out.size(), 0);
    astcenc_context_free(ctx);
    if (e != ASTCENC_SUCCESS) return {};
    return out;
}
#endif

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
    const std::unordered_map<std::string, HE::UUID>& pathToUuid,
    const Hpak::PackSettings& settings)
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

            // Walk the scalar/string tail (PBR scalars, custom shader, node graph,
            // params) WITHOUT re-serializing it — we only need the byte offset where the
            // node-graph texture paths begin, so we can copy everything before verbatim.
            // graphTexturePaths is DROPPED (baked into MTLU as UUIDs) but graphParamNames
            // (which follows it) must be KEPT so shipped games can setMaterialParam by
            // name — so we re-emit an empty texturePaths vec + the real param names.
            size_t tailStart = o;
            std::string customShaderGlsl;
            { float f; std::string s; uint32_t n = 0;
              for (int i = 0; i < 6; ++i) HAsset::Reader::readPOD(c.data, o, f); // baseColor3+met+rough+opacity
              HAsset::Reader::readString(c.data, o, customShaderGlsl); // customShaderFragGlsl
              HAsset::Reader::readString(c.data, o, s);   // nodeGraphJson
              if (HAsset::Reader::readPOD(c.data, o, n))  // param count + floats
                  for (uint32_t i = 0; i < n && o + 4 <= c.data.size(); ++i) HAsset::Reader::readPOD(c.data, o, f);
            }
            const size_t graphTexOffset = o;
            std::vector<std::string> graphTexPaths;
            HAsset::Reader::readVec(c.data, o, graphTexPaths); // node-graph textures (dropped)
            std::vector<std::string> graphParamNames;
            HAsset::Reader::readVec(c.data, o, graphParamNames); // param names (kept)
            std::vector<uint8_t> graphParamTypes;
            HAsset::Reader::readVec(c.data, o, graphParamTypes); // param widget kinds (kept)
            const size_t afterTypes = o; // everything after is copied verbatim below

            // Skim ahead (bounds-checked) to the WPO vertex body so PSHD can bake the
            // matching custom vertex. Layout after graphParamTypes: minmax, groups,
            // tooltips, parentPath, overriddenParams, switchNames, switchValues,
            // blendMode, customShaderVertGlsl.
            std::string customVertBody;
            {
                size_t o2 = afterTypes;
                std::vector<float> mm; std::vector<std::string> sv; std::vector<uint8_t> bv;
                std::string sp; uint8_t bm = 0;
                HAsset::Reader::readVec(c.data, o2, mm);   // minmax
                HAsset::Reader::readVec(c.data, o2, sv);   // groups
                HAsset::Reader::readVec(c.data, o2, sv);   // tooltips
                HAsset::Reader::readString(c.data, o2, sp);// parentMaterialPath
                HAsset::Reader::readVec(c.data, o2, sv);   // overridden params
                HAsset::Reader::readVec(c.data, o2, sv);   // switch names
                HAsset::Reader::readVec(c.data, o2, bv);   // switch values
                HAsset::Reader::readPOD(c.data, o2, bm);   // blend mode
                HAsset::Reader::readString(c.data, o2, customVertBody);
            }

            std::vector<uint8_t> mtrl;
            HAsset::Writer::appendString(mtrl, std::string{});           // shaderPath dropped
            HAsset::Writer::appendVec(mtrl, std::vector<std::string>{}); // texturePaths dropped
            mtrl.insert(mtrl.end(), c.data.begin() + tailStart, c.data.begin() + graphTexOffset); // scalar tail verbatim
            HAsset::Writer::appendVec(mtrl, std::vector<std::string>{}); // graphTexturePaths dropped (baked to MTLU)
            HAsset::Writer::appendVec(mtrl, graphParamNames);            // graphParamNames kept for runtime
            HAsset::Writer::appendVec(mtrl, graphParamTypes);            // graphParamTypes kept for runtime
            // Post-v9 tail (metadata, instance info, blend mode, WPO vertex body): copy
            // BYTE-VERBATIM so new fields ship without the packer learning each one.
            mtrl.insert(mtrl.end(), c.data.begin() + afterTypes, c.data.end());
            w.addChunk(HAsset::CHUNK_MTRL, mtrl.data(), mtrl.size());

            const HE::UUID sid = shaderPath.empty() ? HE::UUID{} : resolve(shaderPath);
            std::vector<HE::UUID> texIds; texIds.reserve(texPaths.size());
            for (const auto& tp : texPaths) texIds.push_back(resolve(tp));
            std::vector<HE::UUID> graphTexIds; graphTexIds.reserve(graphTexPaths.size());
            for (const auto& tp : graphTexPaths) graphTexIds.push_back(resolve(tp));
            std::vector<uint8_t> d;
            HAsset::Writer::appendPOD(d, sid.hi);
            HAsset::Writer::appendPOD(d, sid.lo);
            HAsset::Writer::appendVec(d, texIds);
            HAsset::Writer::appendVec(d, graphTexIds); // baked node-graph textures
            w.addChunk(HAsset::CHUNK_MTLU, d.data(), d.size());

            // Precompile this material's shader for the chosen backends (CHUNK_PSHD) so
            // the shipped game never cross-compiles. The editor supplies the compiler.
            if (!customShaderGlsl.empty() && settings.shaderBackends != 0 && settings.compileShaderVariants)
            {
                std::vector<uint8_t> pshd = settings.compileShaderVariants(
                    customShaderGlsl, customVertBody, settings.shaderBackends);
                if (!pshd.empty()) w.addChunk(HAsset::CHUNK_PSHD, pshd.data(), pshd.size());
            }
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

void HpakWriter::addPackedEntry(const HE::UUID& id, const HpakReader::StoredEntry& stored)
{
    PendingEntry e;
    e.uuid        = id;
    e.data        = stored.data;
    e.origSize    = stored.origSize;
    e.contentHash = stored.contentHash;
    e.codec       = stored.codec;
    e.flags       = stored.flags;
    std::memcpy(e.nonce, stored.nonce, sizeof(e.nonce));
    m_entries.push_back(std::move(e));
}

// Box-downsample an RGBA8 image to half size (min 1px), averaging each 2x2 block
// (clamped on odd dimensions). Used to bake the mip chain at pack time.
static std::vector<uint8_t> halveRGBA8(const uint8_t* src, uint32_t w, uint32_t h,
                                       uint32_t& outW, uint32_t& outH)
{
    outW = std::max<uint32_t>(1, w / 2);
    outH = std::max<uint32_t>(1, h / 2);
    std::vector<uint8_t> out(static_cast<size_t>(outW) * outH * 4);
    for (uint32_t y = 0; y < outH; ++y)
    {
        const uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
        for (uint32_t x = 0; x < outW; ++x)
        {
            const uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
            for (int c = 0; c < 4; ++c)
            {
                const uint32_t s = src[(y0 * w + x0) * 4 + c] + src[(y0 * w + x1) * 4 + c]
                                 + src[(y1 * w + x0) * 4 + c] + src[(y1 * w + x1) * 4 + c];
                out[(y * outW + x) * 4 + c] = static_cast<uint8_t>((s + 2) / 4);
            }
        }
    }
    return out;
}

// Cook a full RGBA8 texture: bake the whole mip chain into PIXL (level 0 first,
// then each halved level) and record mipLevels in TXMI. The runtime uploads the
// levels directly (Metal gains mips it never had; GL drops glGenerateMipmap).
// Level 0 stays the leading bytes, so backends that don't read mips are unaffected.
static std::vector<uint8_t> cookTexture(HAsset::Reader& r, bool astc)
{
    const HAsset::Reader::Chunk* tm = nullptr;
    const HAsset::Reader::Chunk* px = nullptr;
    for (const auto& c : r.chunks())
    {
        if      (c.id == HAsset::CHUNK_TXMI) tm = &c;
        else if (c.id == HAsset::CHUNK_PIXL) px = &c;
    }
    if (!tm || !px) return {};

    size_t o = 0, width = 0, height = 0, channels = 0;
    HAsset::Reader::readPOD(tm->data, o, width);
    HAsset::Reader::readPOD(tm->data, o, height);
    HAsset::Reader::readPOD(tm->data, o, channels);
    uint32_t existingMips = 1;
    if (o + sizeof(uint32_t) <= tm->data.size()) HAsset::Reader::readPOD(tm->data, o, existingMips);

    // Only cook plain single-level RGBA8 base textures (skip already-cooked,
    // sub-2px, or non-RGBA8/odd-sized payloads — nothing to gain / can't halve).
    if (channels != 4 || width < 2 || height < 2 || existingMips > 1) return {};
    if (px->data.size() != static_cast<size_t>(width) * height * 4) return {};

    // Build the RGBA8 mip chain first (also the fallback if ASTC is unavailable).
    std::vector<uint8_t> rgbaLevels = px->data;  // level 0
    std::vector<uint8_t> prev       = px->data;
    uint32_t cw = static_cast<uint32_t>(width), ch = static_cast<uint32_t>(height), count = 1;
    while (cw > 1 || ch > 1)
    {
        uint32_t nw = 0, nh = 0;
        std::vector<uint8_t> lvl = halveRGBA8(prev.data(), cw, ch, nw, nh);
        rgbaLevels.insert(rgbaLevels.end(), lvl.begin(), lvl.end());
        prev = std::move(lvl); cw = nw; ch = nh; ++count;
    }

    std::vector<uint8_t> levels = std::move(rgbaLevels);
    uint8_t format = 0; // 0 = RGBA8, 1 = ASTC_4x4 (TextureFormat)

#ifdef HE_HAVE_ASTCENC
    if (astc)
    {
        // Transcode each RGBA8 level to ASTC 4x4; keep RGBA8 on any failure.
        std::vector<uint8_t> astcLevels;
        size_t off = 0; uint32_t lw = static_cast<uint32_t>(width), lh = static_cast<uint32_t>(height);
        bool ok = true;
        for (uint32_t l = 0; l < count; ++l)
        {
            std::vector<uint8_t> enc = encodeAstc4x4(levels.data() + off, lw, lh);
            if (enc.empty()) { ok = false; break; }
            astcLevels.insert(astcLevels.end(), enc.begin(), enc.end());
            off += static_cast<size_t>(lw) * lh * 4;
            lw = std::max<uint32_t>(1, lw >> 1);
            lh = std::max<uint32_t>(1, lh >> 1);
        }
        if (ok) { levels = std::move(astcLevels); format = 1; }
    }
#else
    (void)astc;
#endif

    HAsset::Writer w;
    for (const auto& c : r.chunks())
    {
        if (c.id == HAsset::CHUNK_TXMI)
        {
            std::vector<uint8_t> b;
            HAsset::Writer::appendPOD(b, width);
            HAsset::Writer::appendPOD(b, height);
            HAsset::Writer::appendPOD(b, channels);
            HAsset::Writer::appendPOD(b, count);
            HAsset::Writer::appendPOD(b, format);
            HAsset::Writer::appendPOD(b, static_cast<uint8_t>(0)); // srgb false
            w.addChunk(HAsset::CHUNK_TXMI, b.data(), b.size());
        }
        else if (c.id == HAsset::CHUNK_PIXL)
            w.addChunk(HAsset::CHUNK_PIXL, levels.data(), levels.size());
        else
            w.addChunk(c.id, c.data.data(), c.data.size());
    }
    return w.toBytes(r.assetType());
}

// Pack-time asset cook: transform an already-ref-rewritten .hasset blob into a
// runtime-optimal form. Today: static meshes are pre-interleaved into the exact
// 8-float (pos3+norm3+uv2) GPU layout both backends build at runtime, with a
// baked AABB (CHUNK_MVBO replacing VERT/NORM/TEXC; INDX kept); RGBA8 textures get
// their full mip chain baked into PIXL. Unknown/uncookable assets pass through
// unchanged. Must be lossless w.r.t. what the runtime uploads.
static std::vector<uint8_t> cookForPack(const std::vector<uint8_t>& blob, bool astcTextures)
{
    HAsset::Reader r;
    if (!r.openData(blob)) return blob;
    const HE::AssetType type = static_cast<HE::AssetType>(r.assetType());
    if (type == HE::AssetType::Texture)
    {
        std::vector<uint8_t> cooked = cookTexture(r, astcTextures);
        return cooked.empty() ? blob : cooked;
    }
    if (type != HE::AssetType::StaticMesh) return blob;

    // Pull the SoA geometry out of the raw chunks.
    const HAsset::Reader::Chunk* vc = nullptr;
    const HAsset::Reader::Chunk* nc = nullptr;
    const HAsset::Reader::Chunk* tc = nullptr;
    for (const auto& c : r.chunks())
    {
        if      (c.id == HAsset::CHUNK_VERT) vc = &c;
        else if (c.id == HAsset::CHUNK_NORM) nc = &c;
        else if (c.id == HAsset::CHUNK_TEXC) tc = &c;
    }
    if (!vc) return blob; // nothing to cook (already cooked, or empty)

    std::vector<float> positions, normals, uvs;
    { size_t o = 0; HAsset::Reader::readVec(vc->data, o, positions); }
    if (nc) { size_t o = 0; HAsset::Reader::readVec(nc->data, o, normals); }
    if (tc) { size_t o = 0; HAsset::Reader::readVec(tc->data, o, uvs); }
    if (positions.empty() || positions.size() % 3 != 0) return blob;

    const uint32_t vertexCount = static_cast<uint32_t>(positions.size() / 3);

    // Interleave + AABB — byte-identical to the backends' upload loops
    // (OpenGLRenderer.cpp / MetalRenderer.mm), so the cooked buffer uploads 1:1.
    std::vector<float> interleaved;
    interleaved.reserve(static_cast<size_t>(vertexCount) * 8);
    float mn[3] = {  1e30f,  1e30f,  1e30f };
    float mx[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t v = 0; v < vertexCount; ++v)
    {
        const float px = positions[v*3+0], py = positions[v*3+1], pz = positions[v*3+2];
        interleaved.push_back(px); interleaved.push_back(py); interleaved.push_back(pz);
        mn[0] = std::min(mn[0], px); mn[1] = std::min(mn[1], py); mn[2] = std::min(mn[2], pz);
        mx[0] = std::max(mx[0], px); mx[1] = std::max(mx[1], py); mx[2] = std::max(mx[2], pz);
        if (static_cast<size_t>(v)*3 + 2 < normals.size())
            { interleaved.push_back(normals[v*3+0]); interleaved.push_back(normals[v*3+1]); interleaved.push_back(normals[v*3+2]); }
        else
            { interleaved.push_back(0.0f); interleaved.push_back(0.0f); interleaved.push_back(0.0f); }
        if (static_cast<size_t>(v)*2 + 1 < uvs.size())
            { interleaved.push_back(uvs[v*2+0]); interleaved.push_back(uvs[v*2+1]); }
        else
            { interleaved.push_back(0.0f); interleaved.push_back(0.0f); }
    }

    // Rewrite: keep everything except the SoA geometry; emit MVBO instead.
    HAsset::Writer w;
    for (const auto& c : r.chunks())
    {
        if (c.id == HAsset::CHUNK_VERT || c.id == HAsset::CHUNK_NORM || c.id == HAsset::CHUNK_TEXC)
            continue; // replaced by MVBO
        w.addChunk(c.id, c.data.data(), c.data.size());
    }
    std::vector<uint8_t> mvbo;
    HAsset::Writer::appendPOD(mvbo, vertexCount);
    for (int i = 0; i < 3; ++i) HAsset::Writer::appendPOD(mvbo, mn[i]);
    for (int i = 0; i < 3; ++i) HAsset::Writer::appendPOD(mvbo, mx[i]);
    HAsset::Writer::appendVec(mvbo, interleaved);
    w.addChunk(HAsset::CHUNK_MVBO, mvbo.data(), mvbo.size());
    return w.toBytes(r.assetType());
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
                              const AddProgressFn& progress,
                              const Hpak::IncrementalCache* cache)
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
    // The incremental cache key is hash64 of the REWRITTEN blob (not the source
    // file): a rename/delete of a *referenced* asset changes the rewrite result
    // without touching this file, and must invalidate the cached entry.
    int count = 0;
    m_reused = 0;
    m_srcHashes.clear();
    const int  total         = static_cast<int>(pending.size());
    const bool wantEncrypted = settings.encrypt && Hpak::cryptoAvailable();
    for (auto& pe : pending)
    {
        if (progress) progress(count, total, pe.relPath);
        std::vector<uint8_t> blob = rewriteRefsForPack(pe.bytes, pathToUuid, settings);
        if (settings.cook) blob = cookForPack(blob, settings.astcTextures);
        // Hash the final (cooked) blob: incremental reuse keys on exactly the
        // bytes that get stored, so a cook change re-packs the entry.
        const uint64_t srcHash = Hpak::hash64(blob.data(), blob.size());
        m_srcHashes.emplace_back(pe.id, srcHash);

        bool reused = false;
        if (cache && cache->previousPak)
        {
            auto it = cache->srcHashes.find(pe.id);
            if (it != cache->srcHashes.end() && it->second == srcHash)
            {
                HpakReader::StoredEntry se;
                // readStoredEntry verifies the content hash, so a corrupt old
                // entry falls through to a fresh pack. The flag/size sanity
                // checks guard against a cache paired with the wrong archive.
                // Dict-compressed entries can never be carried verbatim: this
                // writer emits no shared dictionary, so the entry would be
                // undecodable in the new archive.
                if (cache->previousPak->readStoredEntry(pe.id, se)
                    && ((se.flags & Hpak::kFlagEncrypted) != 0) == wantEncrypted
                    && (se.flags & Hpak::kFlagUsesDict) == 0
                    && se.origSize == static_cast<uint32_t>(blob.size()))
                {
                    addPackedEntry(pe.id, se);
                    reused = true;
                    ++m_reused;
                }
            }
        }
        if (!reused) addEntry(pe.id, blob, settings);
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

    // Close explicitly: the final buffered tail is flushed here, and a
    // disk-full at that point must fail the write, not ship a truncated pak.
    f.close();
    return !f.fail();
}
