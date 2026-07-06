#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>
#include <Types/Defines.h>

// ─────────────────────────────────────────────────────────────────────────────
//  .hpak  —  Horizon Engine packed asset archive (binary format), version 2
//
//  Layout:
//    [ FileHeader (64 bytes)            ]
//    [ EntryDesc × entryCount           ]  TOC, ASCENDING by (uuidHi,uuidLo)
//    [ optional shared zstd dictionary  ]  at FileHeader::dictOffset, dictSize bytes
//    [ raw data blocks at each offset   ]  .hasset blobs
//
//  All values are little-endian.
//
//  Per entry: an optional codec (store/lz4/zstd) and optional XOR obfuscation,
//  selected via EntryDesc::codec and EntryDesc::entryFlags.
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
//  requires a secret — see the AEAD auth tag once encryption lands (kFlagEncrypted).
// ─────────────────────────────────────────────────────────────────────────────

namespace Hpak
{

inline constexpr char     k_magic[4] = {'H','P','A','K'};
inline constexpr uint32_t k_version  = 2;

// ── Per-entry compression codec (EntryDesc::codec) ────────────────────────────
enum class Codec : uint8_t
{
    Store = 0, // uncompressed
    LZ4   = 1, // LZ4 block (LZ4HC at pack time; decoded by LZ4_decompress_safe)
    Zstd  = 2, // zstd frame (optionally with the archive's shared dictionary)
};

// ── Archive-wide flags (FileHeader::flags) ────────────────────────────────────
inline constexpr uint32_t kArchiveSortedTOC = 0x1; // TOC ascending by UUID (binary search valid)
inline constexpr uint32_t kArchiveHasDict   = 0x2; // dictOffset/dictSize point at a shared zstd dictionary
inline constexpr uint32_t kArchiveEncrypted = 0x4; // at least one entry carries kFlagEncrypted

// ── Per-entry flags (EntryDesc::entryFlags) ───────────────────────────────────
// NOTE: kFlagEncrypted is currently XOR (obfuscation, NOT a security guarantee —
// the key ships with the game). It is scheduled to become AES-256-GCM (AEAD).
inline constexpr uint8_t  kFlagEncrypted   = 0x1; // payload is obfuscated/encrypted
inline constexpr uint8_t  kFlagUsesDict    = 0x2; // zstd entry compressed with the archive dictionary
inline constexpr uint8_t  kFlagBlockFramed = 0x4; // RESERVED: per-block framing (not yet implemented)

#pragma pack(push, 1)

struct FileHeader
{
    char     magic[4];       // "HPAK"
    uint32_t version;        // k_version = 2
    uint32_t entryCount;
    uint32_t flags;          // kArchive* bits
    uint64_t buildId;        // hash/semver of the build → patch validation
    uint64_t baseArchiveId;  // 0 = base archive; else buildId of the base this patch overlays
    uint64_t tocHash;        // hash64 over the EntryDesc region
    uint64_t dictOffset;     // byte offset of the shared zstd dictionary blob (0 = none)
    uint32_t dictSize;       // dictionary blob length
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
    uint32_t dataSize;       // stored size (compressed+encrypted; incl. AEAD tag once encrypted)
    uint64_t contentHash;    // hash64 of the stored bytes
    uint8_t  nonce[12];      // AEAD nonce (96-bit); zero when unencrypted / XOR
    uint8_t  codec;          // Hpak::Codec
    uint8_t  entryFlags;     // kFlag* bits
    uint16_t pad;
};
static_assert(sizeof(EntryDesc) == 56, "Hpak::EntryDesc must be 56 bytes");

#pragma pack(pop)

// ── Fast non-cryptographic hash (FNV-1a, 64-bit) ──────────────────────────────
// Deterministic across platforms/runs. Used for tocHash + contentHash (corruption
// detection only — see the header comment). Dependency-free by design.
inline uint64_t hash64(const uint8_t* data, size_t len) noexcept
{
    uint64_t h = 1469598103934665603ULL;        // FNV offset basis
    for (size_t i = 0; i < len; ++i)
    {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ULL;                   // FNV prime
    }
    return h;
}

// Per-entry packing options.
// Operation order on write: compress → encrypt.
struct PackSettings {
    Codec   codec   = Codec::Store; // compression codec (Store = none)
    int     level   = 0;            // codec level; 0 = codec default (LZ4HC 9 / zstd 19)
    bool    encrypt = false;        // XOR obfuscation with `key` (see kFlagEncrypted note)
    uint8_t key[32] = {};           // 32-byte key (from KeyDerivation::derive)
    // Glob patterns matched against each file's path RELATIVE to the packed
    // directory (forward slashes, e.g. "Debug/*", "*_test.hasset"). Matching
    // files are skipped by HpakWriter::addDirectory. `*` matches any sequence
    // (including '/'), `?` matches exactly one character. Case-sensitive.
    std::vector<std::string> excludePatterns;
    // Cook assets into a runtime-optimal form at pack time (see HpakWriter
    // cookForPack): today, static meshes are pre-interleaved into the GPU vertex
    // layout with a baked AABB. Off for editor/loose saves (which stay editable).
    bool cook = false;
    // Additionally transcode RGBA8 textures to ASTC 4x4 (needs HE_HAVE_ASTCENC).
    // Only set for targets whose GPU samples ASTC (Apple-Silicon Metal); other
    // targets keep RGBA8 + baked mipmaps.
    bool astcTextures = false;

    // Precompile node-graph material shaders into the pak (CHUNK_PSHD). `shaderBackends`
    // is a bitmask of (1u << HE::RendererBackend). The callback — supplied by the editor,
    // which links the shader cross-compiler — turns a material's fragment GLSL into the
    // per-backend variants (already PSHD-encoded bytes). Null / 0 backends → no precompile
    // (the shipped game cross-compiles at runtime as before).
    uint32_t shaderBackends = 0;
    std::function<std::vector<uint8_t>(const std::string& fragGlsl,
                                       const std::string& vertBody, uint32_t backends)>
        compileShaderVariants;
};

// Glob match used by PackSettings::excludePatterns (see semantics there).
HE_API bool globMatch(const std::string& pattern, const std::string& path);

} // namespace Hpak
