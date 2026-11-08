#include "Diagnostics/HardwareInfo.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <dxgi.h>      // adapter list (loaded at run time, no import library)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>  // RtlGetVersion probe, GlobalMemoryStatusEx, GetLogicalProcessorInformationEx, GetSystemPowerStatus
  #include <intrin.h>   // __cpuid — CPU brand string
#endif
#if defined(__APPLE__) || defined(__linux__)
  #include <sys/utsname.h>
  #include <unistd.h>
#endif
#ifdef __APPLE__
  #include <mach/mach.h>
  #include <sys/sysctl.h>
#endif

namespace HE
{
namespace
{

// ── Small helpers ───────────────────────────────────────────────────────────

std::string trim(const std::string& s)
{
    const size_t first = s.find_first_not_of(" \t\"\r\n");
    if (first == std::string::npos) return {};
    const size_t last = s.find_last_not_of(" \t\"\r\n");
    return s.substr(first, last - first + 1);
}

#if defined(__linux__)
// First "Key=value" / "Key : value" match in a /etc or /proc text file, with surrounding quotes and
// whitespace stripped. Empty when the file or the key is missing - every caller has a fallback, none of
// this is load-bearing.
std::string firstFieldFromFile(const char* path, const char* key, char separator)
{
    std::ifstream in(path);
    if (!in) return {};
    const size_t keyLen = std::strlen(key);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.compare(0, keyLen, key) != 0) continue;
        const size_t sep = line.find(separator, keyLen);
        if (sep == std::string::npos) continue;
        const std::string value = trim(line.substr(sep + 1));
        if (!value.empty()) return value;
    }
    return {};
}
#endif

const char* platformName()
{
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "unknown";
#endif
}

std::string cpuArchName()
{
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return {};
#endif
}

// ── OS ──────────────────────────────────────────────────────────────────────

struct OsText { std::string name; std::string version; };

OsText osText()
{
#if defined(_WIN32)
    // GetVersionEx reports 6.2 for anything past Windows 8 unless the exe carries a compatibility
    // manifest. RtlGetVersion always tells the truth; resolving it from the already-loaded ntdll keeps
    // this link-dependency free. The 22000 build cut-off is Microsoft's own Windows 11 boundary.
    using RtlGetVersionFn = long(__stdcall*)(OSVERSIONINFOEXW*);
    if (HMODULE nt = ::GetModuleHandleW(L"ntdll.dll"))
    {
        if (auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(nt, "RtlGetVersion")))
        {
            OSVERSIONINFOEXW vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (rtlGetVersion(&vi) == 0)
            {
                const char* name = vi.dwMajorVersion == 10
                    ? (vi.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10")
                    : "Windows";
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s (%lu.%lu build %lu)", name,
                              static_cast<unsigned long>(vi.dwMajorVersion),
                              static_cast<unsigned long>(vi.dwMinorVersion),
                              static_cast<unsigned long>(vi.dwBuildNumber));
                return { name, buf };
            }
        }
    }
#elif defined(__APPLE__) || defined(__linux__)
    struct utsname u{};
    if (uname(&u) == 0)
    {
        const std::string kernel = std::string(u.sysname) + " " + u.release + " (" + u.machine + ")";
    #if defined(__APPLE__)
        char ver[64]{};
        size_t size = sizeof(ver);
        if (sysctlbyname("kern.osproductversion", ver, &size, nullptr, 0) == 0 && ver[0])
            return { std::string("macOS ") + ver, kernel };
        return { "macOS", kernel };
    #else
        // The kernel version alone rarely identifies a Linux bug; the distro does.
        const std::string distro = firstFieldFromFile("/etc/os-release", "PRETTY_NAME", '=');
        if (!distro.empty()) return { distro, distro + " \xe2\x80\x94 " + kernel };
        return { u.sysname, kernel };
    #endif
    }
#endif
    return { "unknown", "unknown" };
}

// ── CPU ─────────────────────────────────────────────────────────────────────

std::string cpuBrand()
{
#if defined(__APPLE__)
    char brand[128]{};
    size_t size = sizeof(brand);
    if (sysctlbyname("machdep.cpu.brand_string", brand, &size, nullptr, 0) == 0)
        return brand;
#elif defined(_WIN32) && (defined(_M_X64) || defined(_M_IX86))
    // Leaves 0x80000002..4 spell the brand string 16 bytes at a time; leaf 0x80000000 reports how far
    // the extended range goes.
    int regs[4]{};
    __cpuid(regs, static_cast<int>(0x80000000u));
    if (static_cast<unsigned>(regs[0]) >= 0x80000004u)
    {
        char brand[49]{};
        for (int leaf = 0; leaf < 3; ++leaf)
        {
            __cpuid(regs, static_cast<int>(0x80000002u + static_cast<unsigned>(leaf)));
            std::memcpy(brand + leaf * 16, regs, 16);
        }
        brand[48] = '\0';
        const std::string s = trim(brand);
        if (!s.empty()) return s;
    }
#elif defined(__linux__)
    // x86 spells it "model name"; ARM boards usually only offer "Hardware".
    std::string brand = firstFieldFromFile("/proc/cpuinfo", "model name", ':');
    if (brand.empty()) brand = firstFieldFromFile("/proc/cpuinfo", "Hardware", ':');
    if (!brand.empty()) return brand;
#endif
    return {};
}

int physicalCores()
{
#if defined(_WIN32)
    DWORD bytes = 0;
    ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (bytes == 0) return 0;
    std::vector<char> buf(bytes);
    if (!::GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &bytes))
        return 0;
    int cores = 0;
    for (DWORD off = 0; off < bytes;)
    {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (info->Size == 0) break;
        if (info->Relationship == RelationProcessorCore) ++cores;
        off += info->Size;
    }
    return cores;
#elif defined(__APPLE__)
    int n = 0;
    size_t size = sizeof(n);
    if (sysctlbyname("hw.physicalcpu", &n, &size, nullptr, 0) == 0 && n > 0) return n;
    return 0;
#elif defined(__linux__)
    // One core is one distinct (physical id, core id) pair of /proc/cpuinfo. ARM boards often list
    // neither: then the answer stays 0 (unknown) rather than a guess.
    std::ifstream in("/proc/cpuinfo");
    if (!in) return 0;
    std::set<std::pair<std::string, std::string>> cores;
    std::string line, phys = "0", core;
    bool haveCore = false;
    while (std::getline(in, line))
    {
        if (line.rfind("physical id", 0) == 0)      { const auto p = line.find(':'); if (p != std::string::npos) phys = trim(line.substr(p + 1)); }
        else if (line.rfind("core id", 0) == 0)     { const auto p = line.find(':'); if (p != std::string::npos) { core = trim(line.substr(p + 1)); haveCore = true; } }
        else if (line.empty() && haveCore)          { cores.insert({ phys, core }); haveCore = false; }
    }
    if (haveCore) cores.insert({ phys, core });
    return static_cast<int>(cores.size());
#else
    return 0;
#endif
}

// ── Memory ──────────────────────────────────────────────────────────────────

uint64_t physicalMemoryMB()
{
#if defined(__APPLE__)
    uint64_t mem = 0;
    size_t size = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &size, nullptr, 0) == 0)
        return mem / (1024 * 1024);
#elif defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (::GlobalMemoryStatusEx(&status))
        return status.ullTotalPhys / (1024 * 1024);
#elif defined(__linux__)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page  = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page > 0)
        return (uint64_t(pages) * uint64_t(page)) / (1024 * 1024);
#endif
    return 0;
}

uint64_t availableMemoryMB()
{
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (::GlobalMemoryStatusEx(&status))
        return status.ullAvailPhys / (1024 * 1024);
#elif defined(__APPLE__)
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS)
    {
        vm_size_t page = 0;
        host_page_size(mach_host_self(), &page);
        // free + inactive + speculative is what the system hands out without paging anything.
        return (uint64_t(vm.free_count) + vm.inactive_count + vm.speculative_count) * page / (1024 * 1024);
    }
#elif defined(__linux__)
    const std::string kb = firstFieldFromFile("/proc/meminfo", "MemAvailable", ':');
    if (!kb.empty()) return std::strtoull(kb.c_str(), nullptr, 10) / 1024;
#endif
    return 0;
}

// ── Battery ─────────────────────────────────────────────────────────────────

bool machineHasBattery()
{
#if defined(_WIN32)
    SYSTEM_POWER_STATUS s{};
    if (!::GetSystemPowerStatus(&s)) return false;
    // BatteryFlag: 128 = no system battery, 255 = status unknown.
    return s.BatteryFlag != 255 && (s.BatteryFlag & 128) == 0;
#elif defined(__linux__)
    for (const char* p : { "/sys/class/power_supply/BAT0", "/sys/class/power_supply/BAT1" })
        if (std::ifstream(std::string(p) + "/status")) return true;
    return false;
#elif defined(__APPLE__)
    // No IOKit link here: a MacBook says so in its model identifier.
    char model[64]{};
    size_t size = sizeof(model);
    return sysctlbyname("hw.model", model, &size, nullptr, 0) == 0 && std::strstr(model, "Book") != nullptr;
#else
    return false;
#endif
}

bool runningOnBattery()
{
#if defined(_WIN32)
    SYSTEM_POWER_STATUS s{};
    if (!::GetSystemPowerStatus(&s)) return false;
    return s.BatteryFlag != 255 && (s.BatteryFlag & 128) == 0 && s.ACLineStatus == 0;
#elif defined(__linux__)
    for (const char* p : { "/sys/class/power_supply/BAT0", "/sys/class/power_supply/BAT1" })
    {
        std::ifstream in(std::string(p) + "/status");
        std::string st;
        if (in && std::getline(in, st)) return st == "Discharging";
    }
    return false;
#else
    return false;   // macOS: not asked (needs IOKit); documented as unknown -> false
#endif
}

// ── Adapters ────────────────────────────────────────────────────────────────

#ifdef _WIN32
// Every DXGI adapter, in enumeration order, as GpuInfo. dxgi.dll is loaded here so HE_Core needs no import
// library; a machine without it (never on a supported Windows) just lists nothing.
std::vector<GpuInfo> enumerateDxgiAdapters()
{
    std::vector<GpuInfo> out;
    HMODULE dll = ::LoadLibraryW(L"dxgi.dll");
    if (!dll) return out;
    using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
    auto create = reinterpret_cast<CreateFactory1Fn>(::GetProcAddress(dll, "CreateDXGIFactory1"));
    IDXGIFactory1* factory = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory)
    {
        std::map<std::pair<uint32_t, uint32_t>, int> seen;   // ordinal among identical (vendor, device)
        for (UINT i = 0;; ++i)
        {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(adapter->GetDesc1(&d)))
            {
                GpuInfo g;
                char name[256] = {};
                ::WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, static_cast<int>(sizeof name) - 1, nullptr, nullptr);
                g.name     = name;
                g.vendorId = d.VendorId;
                g.deviceId = d.DeviceId;
                g.vendor   = gpuVendorFromPciId(d.VendorId);
                g.vramMB   = d.DedicatedVideoMemory / (1024ull * 1024ull);
                g.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || d.VendorId == 0x1414;
                const int ordinal = seen[{ d.VendorId, d.DeviceId }]++;
                g.id = makeGpuId(d.VendorId, d.DeviceId, ordinal);
                // The Windows driver version of the D3D user-mode driver; NVIDIA's own number is built from it
                // (32.0.16.2101 -> 621.01), the others keep the four parts.
                LARGE_INTEGER umd{};
                if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                {
                    char buf[48];
                    if (d.VendorId == 0x10de)
                    {
                        char digits[16];
                        std::snprintf(digits, sizeof digits, "%u%04u", unsigned(HIWORD(umd.LowPart)) % 10u, unsigned(LOWORD(umd.LowPart)));
                        const std::string s = digits;
                        if (s.size() == 5) g.driver = s.substr(0, 3) + "." + s.substr(3);
                    }
                    if (g.driver.empty())
                    {
                        std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", unsigned(HIWORD(umd.HighPart)), unsigned(LOWORD(umd.HighPart)),
                                      unsigned(HIWORD(umd.LowPart)), unsigned(LOWORD(umd.LowPart)));
                        g.driver = buf;
                    }
                }
                out.push_back(std::move(g));
            }
            adapter->Release();
        }
        factory->Release();
    }
    ::FreeLibrary(dll);
    return out;
}
#endif

// ── The platform provider ───────────────────────────────────────────────────

class PlatformProvider final : public HardwareProvider
{
public:
    HardwareInfo detect() override
    {
        HardwareInfo h;
        h.cpuName    = cpuBrand();
        h.cpuCores   = physicalCores();
        h.cpuThreads = static_cast<int>(std::thread::hardware_concurrency());
        h.cpuArch    = cpuArchName();
        h.ramTotalMB = physicalMemoryMB();
        h.platform   = platformName();
        const OsText os = osText();
        h.osName     = os.name;
        h.osVersion  = os.version;
        h.hasBattery = machineHasBattery();
#ifdef _WIN32
        h.gpus = enumerateDxgiAdapters();
#endif
        return h;
    }
    uint64_t freeMemoryMB() override { return availableMemoryMB(); }
    bool     onBattery() override    { return runningOnBattery(); }
};

// ── State ───────────────────────────────────────────────────────────────────

struct State
{
    std::mutex                        mutex;
    std::shared_ptr<HardwareProvider> provider;            // never null after first use
    bool                              detected = false;
    HardwareInfo                      info;
    std::vector<GpuInfo>              detectedGpus;        // from the provider's detect()
    GpuInfo                           gpu;                 // handed over by the renderer
    std::vector<GpuInfo>              rendererGpus;        // the renderer's own list, used where the platform found none
    DisplayInfo                       display;             // handed over by the window system

    HardwareProvider& prov()
    {
        if (!provider) provider = std::make_shared<PlatformProvider>();
        return *provider;
    }
    void merge()
    {
        info.display = display;
        info.gpu     = gpu;
        // The list: the platform's own when it found any, else what the renderer reported.
        const std::vector<GpuInfo>& listed = detectedGpus.empty() ? rendererGpus : detectedGpus;
        info.gpus = listed;
        for (GpuInfo& g : info.gpus) g.active = false;
        if (!gpu.valid()) return;

        // Which listed adapter is the one the renderer runs on: by its id when it gave one (it knows its
        // ordinal), else by the PCI ids, else by name (OpenGL cannot read the ids).
        int idx = -1;
        if (!gpu.id.empty())
            for (size_t i = 0; i < info.gpus.size() && idx < 0; ++i)
                if (info.gpus[i].id == gpu.id) idx = static_cast<int>(i);
        if (idx < 0 && (gpu.vendorId || gpu.deviceId))
            for (size_t i = 0; i < info.gpus.size() && idx < 0; ++i)
                if (info.gpus[i].vendorId == gpu.vendorId && info.gpus[i].deviceId == gpu.deviceId) idx = static_cast<int>(i);
        if (idx < 0)
            for (size_t i = 0; i < info.gpus.size() && idx < 0; ++i)
                if (info.gpus[i].name == gpu.name) idx = static_cast<int>(i);
        if (idx >= 0)
        {
            info.gpus[idx].active = true;
            // The list knows the ids and the memory the API could not read (OpenGL).
            if (info.gpu.id.empty())     info.gpu.id = info.gpus[idx].id;
            if (!info.gpu.vendorId)      info.gpu.vendorId = info.gpus[idx].vendorId;
            if (!info.gpu.deviceId)      info.gpu.deviceId = info.gpus[idx].deviceId;
            if (info.gpu.vendor.empty()) info.gpu.vendor = info.gpus[idx].vendor;
            info.gpu.software = info.gpus[idx].software;
        }
        else
        {
            // Not in the list (listing failed, or a backend the platform cannot enumerate): the active card at least.
            GpuInfo g = gpu;
            g.active = true;
            if (g.id.empty() && (g.vendorId || g.deviceId)) g.id = makeGpuId(g.vendorId, g.deviceId, 0);
            info.gpus.push_back(g);
        }
        info.gpu.active = true;
    }
};

State& state()
{
    static State s;
    return s;
}

} // namespace

HardwareInfo hardwareInfo()
{
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.detected)
    {
        s.info = s.prov().detect();
        s.detectedGpus = s.info.gpus;
        s.detected = true;
    }
    s.merge();
    return s.info;
}

uint64_t freeMemoryMB()
{
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.prov().freeMemoryMB();
}

bool onBattery()
{
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.prov().onBattery();
}

std::string gpuVendorFromPciId(uint32_t id)
{
    switch (id)
    {
        case 0x10de: return "NVIDIA";
        case 0x1002: case 0x1022: return "AMD";
        case 0x8086: case 0x8087: return "Intel";
        case 0x106b: return "Apple";
        case 0x13b5: return "ARM";
        case 0x5143: return "Qualcomm";
        case 0x1414: return "Microsoft";   // WARP, the D3D software adapter
        case 0x1010: return "Imagination";
        default:     return {};
    }
}

std::string gpuVendorFromText(const std::string& text)
{
    std::string t = text;
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto has = [&](const char* k) { return t.find(k) != std::string::npos; };
    if (has("nvidia"))                            return "NVIDIA";
    if (has("amd") || has("ati ") || has("radeon")) return "AMD";
    if (has("intel"))                             return "Intel";
    if (has("apple"))                             return "Apple";
    if (has("qualcomm") || has("adreno"))         return "Qualcomm";
    if (has("arm") || has("mali"))                return "ARM";
    if (has("microsoft") || has("llvmpipe") || has("mesa")) return has("microsoft") ? "Microsoft" : std::string{};
    return {};
}

void setGpuInfo(const GpuInfo& gpu)
{
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.gpu = gpu;
    s.merge();
}

void setDisplayInfo(const DisplayInfo& display)
{
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.display = display;
    s.merge();
}

void setHardwareProvider(std::shared_ptr<HardwareProvider> provider)
{
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.provider = std::move(provider);
    s.detected = false;
    s.info     = HardwareInfo{};
    s.gpu      = GpuInfo{};
    s.detectedGpus.clear();
    s.rendererGpus.clear();
    s.display  = DisplayInfo{};
}

void setGpuList(const std::vector<GpuInfo>& gpus)
{
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.rendererGpus = gpus;
    s.merge();
}

std::string makeGpuId(uint32_t vendorId, uint32_t deviceId, int ordinal)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04x:%04x:%d", vendorId & 0xffffu, deviceId & 0xffffu, ordinal);
    return buf;
}

const char* hardwarePropertyName(int p)
{
    static const char* const kNames[] = {
        "CpuName", "CpuCores", "CpuThreads", "CpuArchitecture",
        "RamTotalMB", "RamFreeMB",
        "GpuName", "GpuVendor", "GpuDriver", "GpuVramMB", "GraphicsApi",
        "OsName", "OsVersion",
        "DisplayWidth", "DisplayHeight", "DisplayRefreshHz", "DisplayScale", "DisplayName",
        "HasBattery", "OnBattery" };
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<size_t>(HardwareProperty::Count),
                  "one name per HardwareProperty");
    return p >= 0 && p < static_cast<int>(HardwareProperty::Count) ? kNames[p] : "";
}

int hardwarePropertyFromName(const std::string& name)
{
    auto lower = [](std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
    const std::string want = lower(name);
    for (int i = 0; i < static_cast<int>(HardwareProperty::Count); ++i)
        if (lower(hardwarePropertyName(i)) == want) return i;
    return -1;
}

std::string hardwarePropertyText(int p)
{
    const HardwareInfo h = hardwareInfo();
    auto num = [](uint64_t v) { return v ? std::to_string(v) : std::string{}; };
    auto flt = [](float v) {
        if (v <= 0.0f) return std::string{};
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(v));
        std::string s = buf;
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    };
    switch (static_cast<HardwareProperty>(p))
    {
        case HardwareProperty::CpuName:          return h.cpuName;
        case HardwareProperty::CpuCores:         return num(static_cast<uint64_t>(std::max(h.cpuCores, 0)));
        case HardwareProperty::CpuThreads:       return num(static_cast<uint64_t>(std::max(h.cpuThreads, 0)));
        case HardwareProperty::CpuArchitecture:  return h.cpuArch;
        case HardwareProperty::RamTotalMB:       return num(h.ramTotalMB);
        case HardwareProperty::RamFreeMB:        return num(freeMemoryMB());
        case HardwareProperty::GpuName:          return h.gpu.name;
        case HardwareProperty::GpuVendor:        return h.gpu.vendor;
        case HardwareProperty::GpuDriver:        return h.gpu.driver;
        case HardwareProperty::GpuVramMB:        return num(h.gpu.vramMB);
        case HardwareProperty::GraphicsApi:      return h.gpu.api;
        case HardwareProperty::OsName:           return h.osName;
        case HardwareProperty::OsVersion:        return h.osVersion;
        case HardwareProperty::DisplayWidth:     return num(static_cast<uint64_t>(std::max(h.display.width, 0)));
        case HardwareProperty::DisplayHeight:    return num(static_cast<uint64_t>(std::max(h.display.height, 0)));
        case HardwareProperty::DisplayRefreshHz: return flt(h.display.refreshHz);
        case HardwareProperty::DisplayScale:     return h.display.valid() ? flt(h.display.scale) : std::string{};
        case HardwareProperty::DisplayName:      return h.display.name;
        case HardwareProperty::HasBattery:       return h.hasBattery ? "true" : "false";
        case HardwareProperty::OnBattery:        return onBattery() ? "true" : "false";
        default:                                 return {};
    }
}

std::string hardwareSummary()
{
    const HardwareInfo h = hardwareInfo();
    char buf[384];
    std::string out;

    if (h.cpuName.empty()) out += "CPU: unknown";
    else
    {
        std::snprintf(buf, sizeof(buf), "CPU: %s (", h.cpuName.c_str());
        out += buf;
        if (h.cpuCores > 0) { std::snprintf(buf, sizeof(buf), "%d cores, ", h.cpuCores); out += buf; }
        std::snprintf(buf, sizeof(buf), "%d threads)", h.cpuThreads);
        out += buf;
    }

    if (h.ramTotalMB == 0) out += "\nRAM: unknown";
    else { std::snprintf(buf, sizeof(buf), "\nRAM: %llu MB", static_cast<unsigned long long>(h.ramTotalMB)); out += buf; }

    if (!h.gpu.valid()) out += "\nGPU: unknown";
    else
    {
        out += "\nGPU: " + h.gpu.name + " (";
        bool first = true;
        auto part = [&](const std::string& p) { if (p.empty()) return; if (!first) out += ", "; out += p; first = false; };
        if (h.gpu.vramMB) { std::snprintf(buf, sizeof(buf), "%llu MB", static_cast<unsigned long long>(h.gpu.vramMB)); part(buf); }
        if (!h.gpu.driver.empty()) part("driver " + h.gpu.driver);
        part(h.gpu.api);
        out += ")";
    }

    for (const GpuInfo& g : h.gpus)
    {
        if (g.active) continue;
        out += "\nOther GPU: " + g.name + " (";
        bool first = true;
        auto part = [&](const std::string& p) { if (p.empty()) return; if (!first) out += ", "; out += p; first = false; };
        if (g.vramMB) { std::snprintf(buf, sizeof(buf), "%llu MB", static_cast<unsigned long long>(g.vramMB)); part(buf); }
        if (!g.driver.empty()) part("driver " + g.driver);
        if (g.software) part("software");
        out += ")";
    }

    if (!h.display.valid()) out += "\nDisplay: unknown";
    else
    {
        std::snprintf(buf, sizeof(buf), "\nDisplay: %dx%d", h.display.width, h.display.height);
        out += buf;
        if (h.display.refreshHz > 0.0f) { std::snprintf(buf, sizeof(buf), " @ %.0f Hz", h.display.refreshHz); out += buf; }
        std::snprintf(buf, sizeof(buf), " (scale %.2f)", h.display.scale);
        out += buf;
    }

    out += "\nOS: " + (h.osVersion.empty() ? std::string("unknown") : h.osVersion);
    return out;
}

} // namespace HE
