#pragma once
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
//  .hpak  —  Horizon Engine packed asset archive (binary format)
//
//  Layout:
//    [ FileHeader (16 bytes)            ]
//    [ EntryDesc × entryCount           ]  TOC
//    [ raw data blocks at each offset   ]  .hasset blobs
//
//  All values are little-endian. Compression (LZ4) and encryption (XOR) are
//  opt-in per entry via entryFlags. Operation order: compress → encrypt (write);
//  decrypt → decompress (read). origSize always holds the uncompressed size.
// ─────────────────────────────────────────────────────────────────────────────

namespace Hpak
{

inline constexpr char     k_magic[4]      = {'H','P','A','K'};
inline constexpr uint32_t k_version       = 1;

// EntryDesc::entryFlags bits
inline constexpr uint8_t  kFlagCompressed = 0x01; // LZ4 compressed
inline constexpr uint8_t  kFlagEncrypted  = 0x02; // XOR with 32-byte derived key

#pragma pack(push, 1)

struct FileHeader
{
    char     magic[4];      // "HPAK"
    uint32_t version;       // k_version = 1
    uint32_t entryCount;
    uint32_t flags;         // reserved
};
static_assert(sizeof(FileHeader) == 16, "Hpak::FileHeader must be 16 bytes");

struct EntryDesc
{
    uint64_t uuidHi;
    uint64_t uuidLo;
    uint32_t origSize;      // original .hasset size before compression
    uint32_t dataSize;      // stored size (may differ with compression)
    uint64_t dataOffset;    // byte offset from start of file
    uint8_t  entryFlags;    // kFlagCompressed | kFlagEncrypted
    uint8_t  pad[3];
};
static_assert(sizeof(EntryDesc) == 36, "Hpak::EntryDesc must be 36 bytes");

#pragma pack(pop)

// Per-entry packing options.
// Operation order on write: compress → encrypt.
// Operation order on read:  decrypt  → decompress.
struct PackSettings {
    bool    compress = false; // LZ4-compress before optional encryption
    bool    encrypt  = false;
    uint8_t key[32]  = {};    // 32-byte XOR key (from KeyDerivation::derive)
};

} // namespace Hpak
