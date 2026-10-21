#pragma once
#include <bit>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <Types/Defines.h>

// ─────────────────────────────────────────────────────────────────────────────
//  .hpak  —  Horizon Engine packed asset archive (binary format), version 3
//
//  Layout:
//    [ FileHeader (64 bytes)            ]
//    [ EntryDesc × entryCount           ]  TOC, ASCENDING by (uuidHi,uuidLo)
//    [ shared zstd dictionary  RESERVED ]  slot at FileHeader::dictOffset/dictSize —
//                                          HpakWriter always writes 0/0, HpakReader
//                                          never looks at it (see kArchiveHasDict)
//    [ raw data blocks at each offset   ]  .hasset blobs
//
//  All values are little-endian.
//
//  Per entry: an optional codec (store/lz4/zstd) and optional AES-256-GCM
//  encryption, selected via EntryDesc::codec and EntryDesc::entryFlags.
//    Operation order on write: compress → encrypt.
//    Operation order on read:  decrypt  → decompress.
//    origSize always holds the uncompressed .hasset size.
//
//  Integrity (two independent layers, both cheap):
//    • FileHeader::tocHash    — hash64 over the EntryDesc region; verified at open()
//                               → fail-fast on a truncated/corrupt archive.
//    • EntryDesc::contentHash — hash64 over the STORED (compressed+encrypted) bytes;
//                               verified per read → catches localized corruption.
//  These use a fast non-cryptographic hash: they detect ACCIDENTAL corruption,
//  they are NOT tamper-proof (an attacker just recomputes them). Tamper-evidence
//  requires a secret: encrypted entries (kFlagEncrypted) get it from the AES-GCM
//  auth tag — a modified payload fails to decrypt. Plain entries have none.
// ─────────────────────────────────────────────────────────────────────────────

namespace Hpak
{

inline constexpr char     k_magic[4] = {'H','P','A','K'};
// v3: tocHash and contentHash are hash64 (word-wise, four lanes) instead of the
// byte-wise FNV-1a of v2 (fnv1a64Bytes) — the byte-wise one ran at ~0.66 GB/s and
// was the slowest step of every pak read (CORE-403). The layout is unchanged.
//
// A bump by the rule in HAsset.h: the same bytes now carry a different hash, so
// an older reader would refuse every v3 package as corrupt instead of as "too
// new". HpakReader reads both: a v2 package is verified with fnv1a64Bytes
// (hash64ForVersion), and a v2 entry carried verbatim into a v3 package gets its
// hash recomputed (HpakReader::readStoredEntry).
inline constexpr uint32_t k_version       = 3;
inline constexpr uint32_t k_oldestVersion = 2; // oldest version HpakReader still opens

// ── Per-entry compression codec (EntryDesc::codec) ────────────────────────────
enum class Codec : uint8_t
{
    Store = 0, // uncompressed
    LZ4   = 1, // LZ4 block (LZ4HC at pack time; decoded by LZ4_decompress_safe)
    Zstd  = 2, // zstd frame (always dictionary-less — see kFlagUsesDict)
};

// ── Archive-wide flags (FileHeader::flags) ────────────────────────────────────
inline constexpr uint32_t kArchiveSortedTOC = 0x1; // TOC ascending by UUID (binary search valid)
inline constexpr uint32_t kArchiveHasDict   = 0x2; // RESERVED: shared zstd dictionary (never set)
inline constexpr uint32_t kArchiveEncrypted = 0x4; // at least one entry carries kFlagEncrypted

// ── Per-entry flags (EntryDesc::entryFlags) ───────────────────────────────────
// kFlagEncrypted is AES-256-GCM (AEAD): the stored blob is ciphertext || 16-byte
// tag with the nonce in EntryDesc::nonce. Threat model in Aes256Gcm.h — this
// obfuscates against casual ripping, it is NOT a security boundary (the key ships
// with the game). kFlagUsesDict and kFlagBlockFramed are RESERVED and implemented
// on neither side: HpakWriter never sets them, and HpakReader::readEntry REJECTS
// an entry carrying either — a flag that says "these bytes must be read
// differently" cannot be ignored, or the decode yields plausible garbage instead
// of an error. (readStoredEntry still returns them: that is the verbatim re-pack
// path, where the writer itself refuses to carry a dict entry over.)
inline constexpr uint8_t  kFlagEncrypted   = 0x1; // payload is AES-256-GCM encrypted
inline constexpr uint8_t  kFlagUsesDict    = 0x2; // RESERVED: zstd shared-dictionary entry
inline constexpr uint8_t  kFlagBlockFramed = 0x4; // RESERVED: per-block framing (not yet implemented)

#pragma pack(push, 1)

struct FileHeader
{
    char     magic[4];       // "HPAK"
    uint32_t version;        // k_version = 3 (2 still read, see k_version)
    uint32_t entryCount;
    uint32_t flags;          // kArchive* bits
    uint64_t buildId;        // hash/semver of the build → patch validation
    uint64_t baseArchiveId;  // 0 = base archive; else buildId of the base this patch overlays
    uint64_t tocHash;        // hash64 over the EntryDesc region
    uint64_t dictOffset;     // RESERVED: shared zstd dictionary blob offset (always 0 today)
    uint32_t dictSize;       // RESERVED: dictionary blob length (always 0 today)
    uint32_t reserved0;
    uint64_t reserved1;
};
static_assert(sizeof(FileHeader) == 64, "Hpak::FileHeader must be 64 bytes");

struct EntryDesc
{
    uint64_t uuidHi;         // sorted key (high)
    uint64_t uuidLo;         // sorted key (low)
    uint64_t dataOffset;     // byte offset from start of file
    uint32_t origSize;       // uncompressed .hasset size
    uint32_t dataSize;       // stored size (compressed+encrypted; incl. the 16-byte AEAD tag)
    uint64_t contentHash;    // hash64 of the stored bytes
    uint8_t  nonce[12];      // AES-GCM nonce (96-bit); zero when the entry is unencrypted
    uint8_t  codec;          // Hpak::Codec
    uint8_t  entryFlags;     // kFlag* bits
    uint16_t pad;
};
static_assert(sizeof(EntryDesc) == 56, "Hpak::EntryDesc must be 56 bytes");

#pragma pack(pop)

// ── Fast non-cryptographic hashes (corruption detection only, see the top) ────
// Deterministic across platforms/runs and dependency-free by design.

// Byte-wise FNV-1a, 64-bit: the tocHash/contentHash of version-2 packages. Also
// the change key of the EngineContent manifest (ContentSync), which lives on the
// server and must keep matching what earlier builds published there.
inline uint64_t fnv1a64Bytes(const uint8_t* data, size_t len) noexcept
{
    uint64_t h = 1469598103934665603ULL;        // FNV offset basis
    for (size_t i = 0; i < len; ++i)
    {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ULL;                   // FNV prime
    }
    return h;
}

// The current hash (version 3): FNV-1a over 64-bit little-endian words in four
// independent lanes, so four multiplies are in flight instead of one per byte.
// Plain word-wise FNV only carries a difference UPWARD (a flip of bit 63 stays bit
// 63, so two such flips in one lane cancel); the xor-shift after each multiply
// feeds the high half back down. Each step is a bijection of (state ^ word), so a
// single changed word always changes its lane. Lanes fold in order, then the tail
// (whole words, then bytes) and the length, then a murmur3 finalizer.
inline uint64_t hash64(const uint8_t* data, size_t len) noexcept
{
    static_assert(std::endian::native == std::endian::little,
                  "hash64 reads words in native order; the format is little-endian");
    constexpr uint64_t kBasis = 1469598103934665603ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;
    auto step = [](uint64_t h, uint64_t w) noexcept {
        h = (h ^ w) * kPrime;
        return h ^ (h >> 32);
    };
    auto word = [data](size_t at) noexcept {
        uint64_t w;
        std::memcpy(&w, data + at, sizeof(w));
        return w;
    };

    uint64_t l0 = kBasis, l1 = kBasis ^ 1, l2 = kBasis ^ 2, l3 = kBasis ^ 3;
    size_t i = 0;
    for (; i + 32 <= len; i += 32)
    {
        l0 = step(l0, word(i));
        l1 = step(l1, word(i + 8));
        l2 = step(l2, word(i + 16));
        l3 = step(l3, word(i + 24));
    }
    uint64_t h = step(step(step(step(kBasis, l0), l1), l2), l3);
    for (; i + 8 <= len; i += 8) h = step(h, word(i));
    for (; i < len; ++i) { h ^= static_cast<uint64_t>(data[i]); h *= kPrime; }
    h ^= static_cast<uint64_t>(len);
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ULL;
    h ^= h >> 33;
    return h;
}

// The hash a package of `version` was written with (k_oldestVersion..k_version).
inline uint64_t hash64ForVersion(uint32_t version, const uint8_t* data, size_t len) noexcept
{
    return version < 3 ? fnv1a64Bytes(data, len) : hash64(data, len);
}

// Per-entry packing options.
// Operation order on write: compress → encrypt.
struct PackSettings {
    Codec   codec   = Codec::Store; // compression codec (Store = none)
    int     level   = 0;            // codec level; 0 = codec default (LZ4HC 9 / zstd 19)
    // AES-256-GCM the stored bytes with `key` (see the kFlagEncrypted note). Silently
    // falls back to storing plaintext when the build has no crypto backend
    // (Hpak::cryptoAvailable) or the encryption call fails — check the written
    // entry flags, not this field, to know whether a pak really is encrypted.
    bool    encrypt = false;
    // 32-byte key: the editor export draws a random one (Hpak::randomBytes) and ships
    // it with the game; hpak_packer --secret derives one via KeyDerivation::derive.
    uint8_t key[32] = {};
    // Glob patterns matched against each file's path RELATIVE to the packed
    // directory (forward slashes, e.g. "Debug/*", "*_test.hasset"). Matching
    // files are skipped by HpakWriter::addDirectory. `*` matches any sequence
    // (including '/'), `?` matches exactly one character. Case-sensitive.
    std::vector<std::string> excludePatterns;
    // Cook assets into a runtime-optimal form at pack time (see HpakWriter
    // cookForPack): today, static meshes are pre-interleaved into the GPU vertex
    // layout with a baked AABB. Off for editor/loose saves (which stay editable).
    bool cook = false;
    // Additionally block-compress RGBA8 textures at pack time to this HE::TextureFormat
    // value (kept as a raw uint8_t so this low-level header needn't include Assets.h):
    //   0 = RGBA8 (none), 1 = ASTC_4x4, 2 = BC7, 3 = BC3, 4 = BC1, 5 = BC4 (R only), 6 = BC5
    // (R and G only). Each needs its encoder in the build (HE_HAVE_ASTCENC / HE_HAVE_BC7ENC /
    // HE_HAVE_STB_DXT, which covers BC1/BC3/BC4/BC5); on a missing encoder or
    // an encode failure the cook silently keeps RGBA8 + baked mipmaps. The chosen format
    // must match what the target's GPU can sample — the editor picks it per target/backend.
    uint8_t textureCompression = 0;
    // How hard the encoder above works: 0 Fast (what every export did until this
    // existed), 1 Balanced, 2 High. Maps onto each encoder's own knobs — ASTC's
    // preset, BC7's partition count and uber level, stb_dxt's quality flag — so
    // one number says the same thing whichever format the target takes. Part of
    // the incremental-pack fingerprint: changing it re-encodes every texture,
    // which is the whole point of changing it.
    uint8_t textureQuality = 0;
    // Also cook texture ARRAYS (layers > 1) to `textureCompression`, every slice and level
    // encoded as it stands. Off: arrays ship RGBA8 as they always did. It is off because
    // some readers still expect RGBA8 arrays in a shipped game (the GI's landscape slice
    // means, giAutoLandscapeSliceMeans, cannot read BC7 or ASTC); whoever turns it on for
    // the Auto landscape arrays has to cover that first. Part of the pack fingerprint.
    bool cookTextureArrays = false;

    // Precompile node-graph material shaders into the pak (CHUNK_PSHD). `shaderBackends`
    // is a bitmask of (1u << HE::RendererBackend). The callback — supplied by the editor,
    // which links the shader cross-compiler — turns a material's fragment GLSL into the
    // per-backend variants (already PSHD-encoded bytes). Null / 0 backends → no precompile
    // (the shipped game cross-compiles at runtime as before).
    uint32_t shaderBackends = 0;
    std::function<std::vector<uint8_t>(const std::string& fragGlsl,
                                       const std::string& vertBody, uint32_t backends)>
        compileShaderVariants;

    // Precompile particle color/alpha-over-life shaders into the pak (CHUNK_PPSD),
    // reusing `shaderBackends` above. The callback — also editor-supplied — takes a
    // ParticleGraphAsset's raw nodeGraphJson and returns already-PPSD-encoded bytes
    // (empty → skip, e.g. a backend this particle system has no baked variant for).
    // Unlike materials there is no cross-compile step (see ParticleShaderVariant):
    // the callback hand-templates GL/Metal source directly, so it does not need the
    // shader-compiler library — only linked into the editor for consistency with
    // compileShaderVariants above.
    std::function<std::vector<uint8_t>(const std::string& nodeGraphJson, uint32_t backends)>
        compileParticleShaderVariants;

    // Called by HpakWriter::addDirectories once per asset that ENDS UP in the archive,
    // after it has been stored: (addressed path, e.g. "Textures/Rock.hasset" or
    // "Engine/Materials/Water.hasset"; true when the entry was carried over verbatim
    // from the previous archive instead of being packed afresh). Excluded files and
    // assets shadowed by an override never reach it — the list a caller builds from
    // this IS the pak's asset list. Same thread as the pack call.
    std::function<void(const std::string& relPath, bool reused)> onAsset;
    // Called by HpakWriter::addDirectories for each HpakWriter::warnings() line, as
    // it is found: a reference no root can satisfy, or a UUID collision that kept
    // a file out. Both used to be silent, and a pak missing the landscape texture
    // arrays looked exactly like a good one. Same thread as the pack call.
    std::function<void(const std::string& message)> onWarning;
};

// Glob match used by PackSettings::excludePatterns (see semantics there).
HE_API bool globMatch(const std::string& pattern, const std::string& path);

} // namespace Hpak
