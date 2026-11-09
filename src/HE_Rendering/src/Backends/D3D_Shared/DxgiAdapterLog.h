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

// "<backend>: adapter <name> (vendor 0x10de, device 0x2786, <vram> MB dedicated, driver <a.b.c.d>)".
// The driver version needs IDXGIAdapter::CheckInterfaceSupport, which is only answered for
// the D3D UMD interface; when it refuses the field reads "unknown".
inline void logAdapter(const char* backend, IDXGIAdapter* adapter)
{
    if (!adapter)
    {
        HE_LOG_WARN(RHI, "%s: adapter unknown (no DXGI adapter for the device)", backend);
        return;
    }
    DXGI_ADAPTER_DESC d{};
    if (FAILED(adapter->GetDesc(&d)))
    {
        HE_LOG_WARN(RHI, "%s: adapter unknown (IDXGIAdapter::GetDesc failed)", backend);
        return;
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
}

} // namespace HE::d3d
