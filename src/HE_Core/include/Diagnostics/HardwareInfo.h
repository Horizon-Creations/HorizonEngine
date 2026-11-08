#pragma once
#include "Types/Defines.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Hardware information (Thema 337)
//
// What the machine is, as the engine and a script can ask for it: CPU, RAM, GPU,
// display, OS. Detected once, read afterwards; the script rows (hardware.*), the
// editor's Hardware page and the startup log all read the same struct.
//
// Only type and model names and sizes. No serial numbers, device ids, MAC
// addresses or user names — nothing here identifies a machine or a person, and
// nothing in the engine sends it anywhere: a game may read it and decide.
//
// Who fills what:
//   * CPU / RAM / OS / battery: probed by the engine itself (HardwareInfo.cpp),
//     per platform, on the first call to hardwareInfo().
//   * GPU: only the renderer knows which adapter it runs on. Once a renderer is
//     up, the application hands over IRenderer::GetAdapterInfo() through
//     setGpuInfo(); until then the GPU fields are empty.
//   * Display: the window system, through setDisplayInfo() once a window exists.
// A field a platform cannot answer stays empty / 0 and says so in the docs; the
// editor page shows a dash for it, never a made-up value.
//
// Tests replace the detection with their own provider (setHardwareProvider), so
// a unit test never depends on the machine it runs on.
// ─────────────────────────────────────────────────────────────────────────────
namespace HE
{

struct GpuInfo
{
    // A stable name for one adapter, the same on every API and across restarts: "vvvv:dddd:n" = PCI vendor
    // id : device id (lower-case hex) : the adapter's number among adapters with the same two ids (0 for the
    // usual single card). What the GPU choice is saved as. "" when the ids are unknown (some OpenGL drivers).
    std::string id;
    uint32_t    vendorId = 0;    // PCI ids as DXGI / Vulkan report them; 0 = unknown
    uint32_t    deviceId = 0;
    bool        software = false;// a software adapter (WARP, llvmpipe), not a card
    bool        active   = false;// the one the renderer runs on (set in HardwareInfo::gpus)
    std::string name;            // "NVIDIA GeForce RTX 4070"
    std::string vendor;          // "NVIDIA", "AMD", "Intel", "Apple", ... ("" when unknown)
    std::string driver;          // driver version as the API reports it ("32.0.16.2101")
    std::string api;             // the graphics API in use: "D3D11", "D3D12", "OpenGL", "Vulkan", "Metal", "Software"
    uint64_t    vramMB = 0;      // dedicated video memory; 0 = unknown (unified-memory GPUs report the shared size)
    bool        valid() const { return !name.empty(); }
};

struct DisplayInfo
{
    int         width   = 0;     // pixels of the display the window is on
    int         height  = 0;
    float       refreshHz = 0.0f;
    float       scale   = 1.0f;  // OS content scale (1.25 = 125 %)
    std::string name;            // the monitor's name as the OS gives it
    bool        valid() const { return width > 0 && height > 0; }
};

struct HardwareInfo
{
    // CPU
    std::string cpuName;         // "AMD Ryzen 9 9950X 16-Core Processor"
    int         cpuCores   = 0;  // physical cores; 0 = unknown
    int         cpuThreads = 0;  // logical processors
    std::string cpuArch;         // "x64", "arm64", "x86"
    // Memory
    uint64_t    ramTotalMB = 0;
    // OS
    std::string platform;        // "Windows", "macOS", "Linux"
    std::string osName;          // "Windows 11", "macOS 15.1", "Ubuntu 24.04 LTS"
    std::string osVersion;       // the full line the startup log prints, e.g. "Windows 11 (10.0 build 26300)"
    // Power
    bool        hasBattery = false;
    // Filled in by the renderer / window system after start-up
    GpuInfo     gpu;             // the adapter the renderer runs on
    DisplayInfo display;         // the display the window is on (follows the window)
    // Every adapter the machine has, the active one marked (GpuInfo::active). Windows lists them through DXGI
    // before any renderer exists; elsewhere the running renderer reports its own list (Vulkan, Metal).
    std::vector<GpuInfo> gpus;
};

// The machine's detection, run once on first use (thread-safe), as a copy: a reader never sees a
// half-written struct while the renderer hands its GPU over. GPU and display are empty until
// setGpuInfo / setDisplayInfo ran.
HE_API HardwareInfo hardwareInfo();

// Live values, asked every time.
HE_API uint64_t freeMemoryMB();   // RAM currently available to a process; 0 = unknown
HE_API bool     onBattery();      // true only when a battery exists AND the machine runs on it

// A GPU vendor's name from its PCI vendor id (what DXGI and Vulkan report) or from the text an API
// returns (GL_VENDOR / GL_RENDERER). "" when it is none of the known ones.
HE_API std::string gpuVendorFromPciId(uint32_t vendorId);
HE_API std::string gpuVendorFromText(const std::string& text);

// Hand-over from the renderer / window system (see the header comment).
HE_API void setGpuInfo(const GpuInfo& gpu);
// The renderer's own list of adapters. Used only where the platform detection found none (macOS, Linux);
// on Windows the DXGI list wins. The active one is marked from setGpuInfo.
HE_API void setGpuList(const std::vector<GpuInfo>& gpus);
// "10de:2786:0": the stable id of an adapter (see GpuInfo::id).
HE_API std::string makeGpuId(uint32_t vendorId, uint32_t deviceId, int ordinal);

// ── Which GPU to use (Thema 337 step 8) ──────────────────────────────────────────────────────────
// The choice is an id (GpuInfo::id) or "" for the system's default adapter. It is read once at start,
// BEFORE the renderer is created, and every backend that can choose creates its device on that adapter;
// an id that is not there (a card removed, a project moved to another PC) or a device that cannot be
// created falls back to the default adapter with a warning. Changing it takes effect at the next start.

// config.json key the editor keeps the choice under (an export may carry one as the game's default).
constexpr const char* kGpuAdapterConfigKey = "GpuAdapter";

// Index of the adapter with this id in the list, -1 when there is none ("" never matches). Case-insensitive.
HE_API int findGpuIndex(const std::vector<GpuInfo>& gpus, const std::string& id);

// A shipped game has no writable config.json: its player's choice lives in a file of its own in this folder
// (the game's per-user data folder). Empty = the editor's way, config.json.
HE_API void setGpuChoiceDirectory(const std::string& directory);

// The saved choice: HE_GPU_ADAPTER from the environment ("default" = none) wins, then the choice file of
// the game, then the config.json key. "" = the default adapter.
HE_API std::string loadPreferredGpu();
// Saves the choice for the next start (the file or config.json, see above). "" goes back to the default.
// False when it could not be written.
HE_API bool savePreferredGpu(const std::string& id);

// What this run starts with: set by the application from loadPreferredGpu() before the renderer exists,
// read by the renderers that can choose an adapter.
HE_API std::string preferredGpuId();
HE_API void        setPreferredGpuId(const std::string& id);

// What came of it, written by the renderer: the choice that was asked for, whether the device really runs
// on it, and a sentence for the log and the settings page ("OpenGL cannot choose the graphics card").
struct GpuSelection
{
    std::string requested;        // "" = default was asked for
    bool        honored = true;   // the device runs on the requested adapter (true for "default")
    bool        canChoose = true; // false for a backend that cannot pick an adapter (OpenGL)
    std::string note;
};
HE_API void         setGpuSelection(const GpuSelection& selection);
HE_API GpuSelection gpuSelection();
HE_API void setDisplayInfo(const DisplayInfo& display);

// The main facts as text, one line per group, for a settings page or a bug report:
//   CPU: AMD Ryzen 9 9950X 16-Core Processor (16 cores, 32 threads)
//   RAM: 31859 MB
//   GPU: NVIDIA GeForce RTX 4070 (12010 MB, driver 32.0.16.2101, D3D12)
//   Display: 2560x1440 @ 144 Hz (scale 1.25)
//   OS: Windows 11 (10.0 build 26300)
// A group with no data says "unknown".
HE_API std::string hardwareSummary();

// ── Properties by name (scripts, the editor page) ───────────────────────────
// Every value as one entry of an enum: the built-in HorizonCode enum "HardwareProperty" has these entries,
// in this order, with these names; Lua and Python ask by name (hardware.fields / hardware.value).
enum class HardwareProperty : int
{
    CpuName = 0, CpuCores, CpuThreads, CpuArchitecture,
    RamTotalMB, RamFreeMB,
    GpuName, GpuVendor, GpuDriver, GpuVramMB, GraphicsApi,
    OsName, OsVersion,
    DisplayWidth, DisplayHeight, DisplayRefreshHz, DisplayScale, DisplayName,
    HasBattery, OnBattery,
    Count
};
HE_API const char* hardwarePropertyName(int property);                  // "GpuName"; "" past the end
HE_API int          hardwarePropertyFromName(const std::string& name);  // case-insensitive; -1 = none
// The value as text. "" when the machine could not tell (0 cores, no GPU handed over, ...), so a UI shows a
// dash instead of a made-up number; booleans are "true"/"false"; floats print without trailing zeros.
HE_API std::string  hardwarePropertyText(int property);

// ── Test seam ───────────────────────────────────────────────────────────────
// The probes the engine runs. A test installs one that answers with fixed values.
class HardwareProvider
{
public:
    virtual ~HardwareProvider() = default;
    virtual HardwareInfo detect() = 0;          // CPU / RAM / OS / battery part
    virtual uint64_t     freeMemoryMB() = 0;
    virtual bool         onBattery() = 0;
};
// nullptr restores the platform detection. Drops what was detected and what the
// renderer / window system handed over, so the next hardwareInfo() starts clean.
HE_API void setHardwareProvider(std::shared_ptr<HardwareProvider> provider);

} // namespace HE
