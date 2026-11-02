#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// On-disk format of VulkanRenderer's VkPipelineCache (REN-808,
// docs/audit/modul_rendering.md). The renderer hands one VkPipelineCache to
// every vkCreate*Pipelines; at shutdown it writes vkGetPipelineCacheData into
// <pref path>/vkpipelinecache/<vendor>_<device>.bin and feeds it back on the
// next start. Before, every pipeline (built-ins at startup, graph materials on
// their first draw) was compiled from SPIR-V by the driver on every launch,
// unless the driver happened to keep a disk cache of its own.
//
// The file is our header + the driver's blob unchanged. The driver validates
// the blob too, but a driver that is handed a blob of another GPU or version
// may also just reject it slowly, and a truncated write would pass its header
// check. So a blob is only handed over when
//   • our header is intact and repeats the device (vendor, device, driver
//     version, pipelineCacheUUID) field by field,
//   • the payload has the stated size and hash64,
//   • the driver's own VkPipelineCacheHeaderVersionOne at the start of the
//     blob names the same vendor, device and UUID.
// Anything else is a miss: the renderer starts with an empty cache and the
// file is overwritten at shutdown.
//
// No <vulkan/vulkan.h> here, like VulkanUploadAlloc.h: he_tests builds without
// the Vulkan SDK, so the accept/reject rules are pinned in
// tests/test_vulkan_pipeline_cache.cpp.
// ─────────────────────────────────────────────────────────────────────────────
#include <Hpak/HpakFormat.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace HE::vkpc
{
    // Bump when the header layout changes.
    inline constexpr char kMagic[8] = { 'H', 'E', 'V', 'K', 'P', 'C', '0', '1' };

    // VkPipelineCacheHeaderVersionOne as the spec lays it out (16+16 bytes,
    // little-endian on every platform the engine runs on).
    inline constexpr uint32_t kDriverHeaderBytes   = 32;
    inline constexpr uint32_t kDriverHeaderVersion = 1;   // VK_PIPELINE_CACHE_HEADER_VERSION_ONE

    struct DeviceId
    {
        uint32_t vendorID      = 0;
        uint32_t deviceID      = 0;
        uint32_t driverVersion = 0;
        uint8_t  uuid[16]      = {};   // VkPhysicalDeviceProperties::pipelineCacheUUID
    };

    struct FileHeader
    {
        char     magic[8];
        uint32_t vendorID;
        uint32_t deviceID;
        uint32_t driverVersion;
        uint32_t reserved;
        uint8_t  uuid[16];
        uint64_t dataSize;
        uint64_t dataHash;     // hash64 of the driver blob
    };
    static_assert(sizeof(FileHeader) == 56, "FileHeader layout is the file format");

    enum class Verdict
    {
        Ok,
        Empty,          // no file, or a zero-length one
        BadHeader,      // shorter than our header, or the magic differs
        OtherDevice,    // written for another GPU or driver
        Truncated,      // payload shorter (or longer) than the header says
        BadHash,        // payload bytes changed
        BadBlob,        // the driver header inside does not match the device
    };

    inline const char* verdictName(Verdict v)
    {
        switch (v)
        {
        case Verdict::Ok:          return "ok";
        case Verdict::Empty:       return "empty";
        case Verdict::BadHeader:   return "bad header";
        case Verdict::OtherDevice: return "other device/driver";
        case Verdict::Truncated:   return "truncated";
        case Verdict::BadHash:     return "hash mismatch";
        case Verdict::BadBlob:     return "driver header mismatch";
        }
        return "?";
    }

    inline uint64_t blobHash(const uint8_t* blob, size_t size)
    {
        return Hpak::hash64(blob, size);
    }

    // The file bytes for a driver blob.
    inline std::vector<uint8_t> wrap(const DeviceId& dev, const uint8_t* blob, size_t size)
    {
        FileHeader h{};
        std::memcpy(h.magic, kMagic, sizeof(kMagic));
        h.vendorID      = dev.vendorID;
        h.deviceID      = dev.deviceID;
        h.driverVersion = dev.driverVersion;
        std::memcpy(h.uuid, dev.uuid, sizeof(h.uuid));
        h.dataSize = size;
        h.dataHash = blobHash(blob, size);
        std::vector<uint8_t> out(sizeof(h) + size);
        std::memcpy(out.data(), &h, sizeof(h));
        if (size) std::memcpy(out.data() + sizeof(h), blob, size);
        return out;
    }

    // Checks file bytes against the device; on Ok, blob/blobSize point into `file`.
    inline Verdict unwrap(const DeviceId& dev, const uint8_t* file, size_t size,
                          const uint8_t*& blob, size_t& blobSize)
    {
        blob = nullptr; blobSize = 0;
        if (!file || size == 0) return Verdict::Empty;
        FileHeader h{};
        if (size < sizeof(h)) return Verdict::BadHeader;
        std::memcpy(&h, file, sizeof(h));
        if (std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0) return Verdict::BadHeader;
        if (h.vendorID != dev.vendorID || h.deviceID != dev.deviceID ||
            h.driverVersion != dev.driverVersion ||
            std::memcmp(h.uuid, dev.uuid, sizeof(h.uuid)) != 0)
            return Verdict::OtherDevice;
        if (h.dataSize != size - sizeof(h)) return Verdict::Truncated;
        const uint8_t* data = file + sizeof(h);
        if (blobHash(data, size - sizeof(h)) != h.dataHash) return Verdict::BadHash;

        // The driver's VkPipelineCacheHeaderVersionOne.
        if (h.dataSize < kDriverHeaderBytes) return Verdict::BadBlob;
        uint32_t w[4];
        std::memcpy(w, data, sizeof(w));   // headerSize, headerVersion, vendorID, deviceID
        if (w[0] < kDriverHeaderBytes || w[0] > h.dataSize || w[1] != kDriverHeaderVersion ||
            w[2] != dev.vendorID || w[3] != dev.deviceID ||
            std::memcmp(data + 16, dev.uuid, sizeof(dev.uuid)) != 0)
            return Verdict::BadBlob;

        blob = data; blobSize = static_cast<size_t>(h.dataSize);
        return Verdict::Ok;
    }
}
