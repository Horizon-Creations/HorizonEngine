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
#include <map>
#include <string>
#include <vector>

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

// The adapter the GPU choice (HE::preferredGpuId) names, or null for the system's default adapter. The ids are
// built exactly like HE_Core's list builds them (vendor:device:number among identical cards), so the one the
// settings page showed is the one found here. Not found: null, and noteSelection() says so.
inline Microsoft::WRL::ComPtr<IDXGIAdapter1> chooseAdapter()
{
    Microsoft::WRL::ComPtr<IDXGIAdapter1> chosen;
    const std::string want = HE::preferredGpuId();
    if (want.empty()) return chosen;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return chosen;
    std::vector<HE::GpuInfo> infos;
    std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter1>> adapters;
    std::map<std::pair<uint32_t, uint32_t>, int> seen;
    for (UINT i = 0;; ++i)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND || !a) break;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) continue;
        HE::GpuInfo g;
        g.id = HE::makeGpuId(d.VendorId, d.DeviceId, seen[{ d.VendorId, d.DeviceId }]++);
        infos.push_back(std::move(g));
        adapters.push_back(a);
    }
    const int idx = HE::findGpuIndex(infos, want);
    if (idx >= 0) chosen = adapters[static_cast<size_t>(idx)];
    return chosen;
}

// After the device exists: record whether the choice was honored (null `used` = the default adapter runs) and
// say it in the log when it was not.
inline void noteSelection(const char* backend, bool used)
{
    HE::GpuSelection sel;
    sel.requested = HE::preferredGpuId();
    sel.canChoose = true;
    if (sel.requested.empty())
    {
        sel.honored = true;
        sel.note    = "the system's default graphics card";
    }
    else if (used)
    {
        sel.honored = true;
        sel.note    = "the chosen graphics card (" + sel.requested + ")";
        HE_LOG_INFO(RHI, "%s: running on the chosen adapter %s", backend, sel.requested.c_str());
    }
    else
    {
        sel.honored = false;
        sel.note    = "the chosen graphics card (" + sel.requested + ") was not found or could not be used; "
                      "the system's default graphics card runs instead";
        HE_LOG_WARN(RHI, "%s: %s", backend, sel.note.c_str());
    }
    HE::setGpuSelection(sel);
}

} // namespace HE::d3d
