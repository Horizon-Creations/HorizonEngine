#pragma once
// One log line naming the GPU a D3D device runs on. The Vulkan backend always did
// ("VulkanRenderer: device 0 of N: ..."); the D3D ones left it out, so a bug report with a
// D3D log could not say whether a laptop's iGPU or the dGPU drew, nor which driver.
//
// Header-only and Windows-only: both D3D backends include it after <dxgi.h>/<dxgi1_4.h>.

#include <Diagnostics/Log.h>

#include <dxgi.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

namespace HE::d3d
{

// "<backend>: adapter <name> (vendor 0x10de, device 0x2786, <vram> MB dedicated, driver <a.b.c.d>)"; also
// returns the same facts as a HE::GpuInfo for IRenderer::GetAdapterInfo (Thema 337).
// The driver version needs IDXGIAdapter::CheckInterfaceSupport, which is only answered for
// the D3D UMD interface; when it refuses the field reads "unknown".
inline HE::GpuInfo logAdapter(const char* backend, IDXGIAdapter* adapter, const char* api = "")
{
    if (!adapter)
    {
        HE_LOG_WARN(RHI, "%s: adapter unknown (no DXGI adapter for the device)", backend);
        return {};
    }
    DXGI_ADAPTER_DESC d{};
    if (FAILED(adapter->GetDesc(&d)))
    {
        HE_LOG_WARN(RHI, "%s: adapter unknown (IDXGIAdapter::GetDesc failed)", backend);
        return {};
    }
    char name[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, static_cast<int>(sizeof name) - 1, nullptr, nullptr);

    char driver[48] = "unknown";
    LARGE_INTEGER umd{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        std::snprintf(driver, sizeof driver, "%u.%u.%u.%u",
                      unsigned(HIWORD(umd.HighPart)), unsigned(LOWORD(umd.HighPart)),
                      unsigned(HIWORD(umd.LowPart)),  unsigned(LOWORD(umd.LowPart)));

    // A software adapter (WARP, vendor 0x1414) is the one thing a "works on my machine" report
    // must not hide: it has a feature level and renders, just ~100x slower.
    HE_LOG_INFO(RHI, "%s: adapter %s (vendor 0x%04x, device 0x%04x, %llu MB dedicated, driver %s)%s",
                backend, name, d.VendorId, d.DeviceId,
                static_cast<unsigned long long>(d.DedicatedVideoMemory / (1024ull * 1024ull)), driver,
                d.VendorId == 0x1414 ? " — SOFTWARE adapter" : "");

    HE::GpuInfo info;
    info.name   = name;
    info.vendor = HE::gpuVendorFromPciId(d.VendorId);
    info.driver = driver == std::string("unknown") ? std::string{} : std::string(driver);
    // NVIDIA's own number (the one its control panel and Vulkan print): the last digit of the third part
    // of the Windows driver version followed by the fourth, 32.0.16.2101 -> 621.01.
    if (d.VendorId == 0x10de && umd.QuadPart != 0)
    {
        char digits[16];
        std::snprintf(digits, sizeof digits, "%u%04u", unsigned(HIWORD(umd.LowPart)) % 10u, unsigned(LOWORD(umd.LowPart)));
        const std::string s = digits;
        if (s.size() == 5) info.driver = s.substr(0, 3) + "." + s.substr(3);
    }
    info.api    = api;
    info.vendorId = d.VendorId;
    info.deviceId = d.DeviceId;
    info.software = d.VendorId == 0x1414;
    info.vramMB = d.DedicatedVideoMemory / (1024ull * 1024ull);
    return info;
}

} // namespace HE::d3d
