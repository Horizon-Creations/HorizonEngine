#include "doctest.h"

#include <Diagnostics/HardwareInfo.h>
#include <Diagnostics/Log.h>

#include <memory>
#include <string>

// Thema 337 step 1: the HardwareInfo model, its provider seam and the text summary. The platform
// detection itself is checked against the machine only as far as every machine answers (a CPU
// name, some RAM, an OS); exact values belong to the Windows / macOS verification in the topic doc.

namespace
{
struct FakeProvider final : HE::HardwareProvider
{
    HE::HardwareInfo info;
    uint64_t         freeMB = 0;
    bool             battery = false;
    int              detects = 0;
    HE::HardwareInfo detect() override { ++detects; return info; }
    uint64_t freeMemoryMB() override   { return freeMB; }
    bool     onBattery() override      { return battery; }
};

std::shared_ptr<FakeProvider> makeFake()
{
    auto f = std::make_shared<FakeProvider>();
    f->info.cpuName    = "Test CPU 9000";
    f->info.cpuCores   = 8;
    f->info.cpuThreads = 16;
    f->info.cpuArch    = "x64";
    f->info.ramTotalMB = 32768;
    f->info.platform   = "Windows";
    f->info.osName     = "Windows 11";
    f->info.osVersion  = "Windows 11 (10.0 build 26300)";
    f->info.hasBattery = true;
    f->freeMB          = 12000;
    return f;
}

// Every test puts the real detection back when it is done.
struct RestoreProvider { ~RestoreProvider() { HE::setHardwareProvider(nullptr); } };
}

TEST_CASE("hardware info: the platform answers what every machine can answer")
{
    RestoreProvider restore;
    HE::setHardwareProvider(nullptr);
    const HE::HardwareInfo h = HE::hardwareInfo();
    CHECK_FALSE(h.cpuName.empty());
    CHECK(h.cpuThreads > 0);
    CHECK(h.cpuCores >= 0);
    CHECK(h.cpuCores <= h.cpuThreads);
    CHECK(h.ramTotalMB > 256);
    CHECK_FALSE(h.platform.empty());
    CHECK_FALSE(h.osVersion.empty());
    CHECK(HE::freeMemoryMB() <= h.ramTotalMB);
    // The GPU and the display come from the renderer and the window system: empty here.
    CHECK_FALSE(h.gpu.valid());
    CHECK_FALSE(h.display.valid());
}

TEST_CASE("hardware info: a provider replaces the detection, once")
{
    RestoreProvider restore;
    auto fake = makeFake();
    HE::setHardwareProvider(fake);
    const HE::HardwareInfo a = HE::hardwareInfo();
    const HE::HardwareInfo b = HE::hardwareInfo();
    CHECK(a.cpuName == "Test CPU 9000");
    CHECK(a.cpuCores == 8);
    CHECK(a.cpuThreads == 16);
    CHECK(a.ramTotalMB == 32768);
    CHECK(a.osName == "Windows 11");
    CHECK(a.hasBattery);
    CHECK(b.cpuName == a.cpuName);
    CHECK(fake->detects == 1);                  // detected once, then read
    CHECK(HE::freeMemoryMB() == 12000);         // live values ask the provider every time
    fake->freeMB = 9000;
    CHECK(HE::freeMemoryMB() == 9000);
    CHECK_FALSE(HE::onBattery());
    fake->battery = true;
    CHECK(HE::onBattery());
}

TEST_CASE("hardware info: the renderer's GPU and the window's display are merged in")
{
    RestoreProvider restore;
    HE::setHardwareProvider(makeFake());
    CHECK_FALSE(HE::hardwareInfo().gpu.valid());

    HE::GpuInfo gpu;
    gpu.name = "Test GPU 4070"; gpu.vendor = "NVIDIA"; gpu.driver = "32.0.1"; gpu.api = "D3D12"; gpu.vramMB = 12010;
    HE::setGpuInfo(gpu);
    HE::DisplayInfo disp;
    disp.width = 2560; disp.height = 1440; disp.refreshHz = 144.0f; disp.scale = 1.25f; disp.name = "Test Monitor";
    HE::setDisplayInfo(disp);

    const HE::HardwareInfo h = HE::hardwareInfo();
    CHECK(h.gpu.valid());
    CHECK(h.gpu.name == "Test GPU 4070");
    CHECK(h.gpu.vramMB == 12010);
    CHECK(h.gpu.api == "D3D12");
    CHECK(h.display.valid());
    CHECK(h.display.width == 2560);
    CHECK(h.display.refreshHz == doctest::Approx(144.0f));
    CHECK(h.cpuName == "Test CPU 9000");        // the detected part is untouched

    // A new provider starts clean: nothing of the old renderer survives.
    HE::setHardwareProvider(makeFake());
    CHECK_FALSE(HE::hardwareInfo().gpu.valid());
    CHECK_FALSE(HE::hardwareInfo().display.valid());
}

TEST_CASE("hardware info: the summary text")
{
    RestoreProvider restore;
    HE::setHardwareProvider(makeFake());
    {
        const std::string s = HE::hardwareSummary();
        CHECK(s.find("CPU: Test CPU 9000 (8 cores, 16 threads)") != std::string::npos);
        CHECK(s.find("RAM: 32768 MB") != std::string::npos);
        CHECK(s.find("GPU: unknown") != std::string::npos);          // nothing handed over yet: honest
        CHECK(s.find("Display: unknown") != std::string::npos);
        CHECK(s.find("OS: Windows 11 (10.0 build 26300)") != std::string::npos);
    }
    HE::GpuInfo gpu;
    gpu.name = "Test GPU"; gpu.driver = "1.2"; gpu.api = "Vulkan"; gpu.vramMB = 8192;
    HE::setGpuInfo(gpu);
    HE::DisplayInfo disp;
    disp.width = 1920; disp.height = 1080; disp.refreshHz = 60.0f; disp.scale = 1.0f;
    HE::setDisplayInfo(disp);
    {
        const std::string s = HE::hardwareSummary();
        CHECK(s.find("GPU: Test GPU (8192 MB, driver 1.2, Vulkan)") != std::string::npos);
        CHECK(s.find("Display: 1920x1080 @ 60 Hz (scale 1.00)") != std::string::npos);
    }
    // An unknown core count leaves the cores out instead of printing 0.
    auto noCores = makeFake();
    noCores->info.cpuCores = 0;
    HE::setHardwareProvider(noCores);
    CHECK(HE::hardwareSummary().find("CPU: Test CPU 9000 (16 threads)") != std::string::npos);
}

TEST_CASE("hardware info: the startup log's system block still carries CPU, OS and memory")
{
    RestoreProvider restore;
    HE::setHardwareProvider(makeFake());
    const std::string block = HE::Log::systemInfoBlock();
    CHECK(block.find("Platform : Windows") != std::string::npos);
    CHECK(block.find("Windows 11 (10.0 build 26300)") != std::string::npos);
    CHECK(block.find("CPU      : Test CPU 9000 (16 hardware threads)") != std::string::npos);
    CHECK(block.find("Memory   : 32768 MB physical") != std::string::npos);
}

TEST_CASE("hardware info: GPU vendor names from PCI ids and from API text")
{
    CHECK(HE::gpuVendorFromPciId(0x10de) == "NVIDIA");
    CHECK(HE::gpuVendorFromPciId(0x1002) == "AMD");
    CHECK(HE::gpuVendorFromPciId(0x8086) == "Intel");
    CHECK(HE::gpuVendorFromPciId(0x106b) == "Apple");
    CHECK(HE::gpuVendorFromPciId(0x1414) == "Microsoft");   // WARP
    CHECK(HE::gpuVendorFromPciId(0xdead).empty());
    CHECK(HE::gpuVendorFromText("NVIDIA Corporation NVIDIA GeForce RTX 4070") == "NVIDIA");
    CHECK(HE::gpuVendorFromText("ATI Technologies Inc. AMD Radeon RX 7900") == "AMD");
    CHECK(HE::gpuVendorFromText("Intel Intel(R) UHD Graphics 770") == "Intel");
    CHECK(HE::gpuVendorFromText("Apple Apple M5") == "Apple");
    CHECK(HE::gpuVendorFromText("something else").empty());
}

// ── The hardware.* rows of the engine API (Thema 337 step 3) ────────────────────────────────────

#include <HorizonScene/EngineApi.h>
#include <Types/TypeRegistry.h>

namespace
{
std::vector<HorizonCode::Value> callRow(const char* id, const std::vector<HorizonCode::Value>& args = {})
{
    const HE::api::ApiFn* fn = HE::api::find(id);
    REQUIRE_MESSAGE(fn != nullptr, id);
    HE::api::Ctx ctx;
    return fn->invoke(ctx, args);
}

void installFullFake()
{
    auto fake = makeFake();
    HE::setHardwareProvider(fake);
    HE::GpuInfo gpu;
    gpu.name = "Test GPU 4070"; gpu.vendor = "NVIDIA"; gpu.driver = "621.01"; gpu.api = "D3D12"; gpu.vramMB = 12010;
    HE::setGpuInfo(gpu);
    HE::DisplayInfo disp;
    disp.width = 2560; disp.height = 1440; disp.refreshHz = 59.94f; disp.scale = 1.25f; disp.name = "Test Monitor";
    HE::setDisplayInfo(disp);
}
}

TEST_CASE("hardware rows: every value reads what the module holds")
{
    RestoreProvider restore;
    installFullFake();
    using HorizonCode::Value;
    CHECK(callRow("hardware.cpuName")[0].s == "Test CPU 9000");
    CHECK(callRow("hardware.cpuCores")[0].i == 8);
    CHECK(callRow("hardware.cpuThreads")[0].i == 16);
    CHECK(callRow("hardware.cpuArch")[0].s == "x64");
    CHECK(callRow("hardware.ramTotalMB")[0].i == 32768);
    CHECK(callRow("hardware.ramFreeMB")[0].i == 12000);
    CHECK(callRow("hardware.gpuName")[0].s == "Test GPU 4070");
    CHECK(callRow("hardware.gpuVendor")[0].s == "NVIDIA");
    CHECK(callRow("hardware.gpuDriver")[0].s == "621.01");
    CHECK(callRow("hardware.gpuVramMB")[0].i == 12010);
    CHECK(callRow("hardware.graphicsApi")[0].s == "D3D12");
    CHECK(callRow("hardware.osName")[0].s == "Windows 11");
    CHECK(callRow("hardware.osVersion")[0].s == "Windows 11 (10.0 build 26300)");
    CHECK(callRow("hardware.displayWidth")[0].i == 2560);
    CHECK(callRow("hardware.displayHeight")[0].i == 1440);
    CHECK(callRow("hardware.displayRefreshHz")[0].f == doctest::Approx(59.94f));
    CHECK(callRow("hardware.displayScale")[0].f == doctest::Approx(1.25f));
    CHECK(callRow("hardware.displayName")[0].s == "Test Monitor");
    CHECK(callRow("hardware.hasBattery")[0].b);
    CHECK_FALSE(callRow("hardware.onBattery")[0].b);
    CHECK(callRow("hardware.summary")[0].s.find("GPU: Test GPU 4070") != std::string::npos);
}

TEST_CASE("hardware rows: by name and by enum, unknown values read empty")
{
    RestoreProvider restore;
    installFullFake();
    using HorizonCode::Value;

    const auto fields = callRow("hardware.fields");
    REQUIRE(fields.size() == 1);
    CHECK(fields[0].isArray);
    REQUIRE(fields[0].items.size() == static_cast<size_t>(HE::HardwareProperty::Count));
    CHECK(fields[0].items.front().s == "CpuName");
    CHECK(fields[0].items.back().s == "OnBattery");

    CHECK(callRow("hardware.value", { Value::ofString("GpuName") })[0].s == "Test GPU 4070");
    CHECK(callRow("hardware.value", { Value::ofString("gpuvramMB") })[0].s == "12010");   // not case sensitive
    CHECK(callRow("hardware.value", { Value::ofString("DisplayRefreshHz") })[0].s == "59.94");
    CHECK(callRow("hardware.value", { Value::ofString("DisplayScale") })[0].s == "1.25");
    CHECK(callRow("hardware.value", { Value::ofString("HasBattery") })[0].s == "true");
    CHECK(callRow("hardware.value", { Value::ofString("Nonsense") })[0].s.empty());

    // The enum pin: ordinal -> text, the same strings.
    CHECK(callRow("hardware.info", { Value::ofInt(static_cast<int>(HE::HardwareProperty::CpuName)) })[0].s == "Test CPU 9000");
    CHECK(callRow("hardware.info", { Value::ofInt(static_cast<int>(HE::HardwareProperty::RamTotalMB)) })[0].s == "32768");
    CHECK(callRow("hardware.info", { Value::ofInt(999) })[0].s.empty());

    // Nothing handed over yet = empty, not a made-up zero.
    HE::setHardwareProvider(makeFake());
    CHECK(callRow("hardware.gpuName")[0].s.empty());
    CHECK(callRow("hardware.gpuVramMB")[0].i == 0);
    CHECK(callRow("hardware.displayWidth")[0].i == 0);
    CHECK(callRow("hardware.displayScale")[0].f == doctest::Approx(0.0f));
    CHECK(callRow("hardware.value", { Value::ofString("DisplayRefreshHz") })[0].s.empty());
}

TEST_CASE("hardware rows: the built-in HorizonCode enum carries the property names in order")
{
    HE::EnumDef def;
    REQUIRE(HE::TypeRegistry::instance().getEnum(HE::kHardwarePropertyEnum, def));
    REQUIRE(def.entries.size() == static_cast<size_t>(HE::HardwareProperty::Count));
    for (size_t i = 0; i < def.entries.size(); ++i)
    {
        CHECK(def.entries[i].name == HE::hardwarePropertyName(static_cast<int>(i)));
        CHECK(def.entries[i].value == static_cast<int>(i));
    }
    CHECK(HE::hardwarePropertyFromName("cpuname") == 0);
    CHECK(HE::hardwarePropertyFromName("OnBattery") == static_cast<int>(HE::HardwareProperty::OnBattery));
    CHECK(HE::hardwarePropertyFromName("nope") == -1);
}

// ── The suggested preset (Thema 337 step 5) ─────────────────────────────────────────────────────

#include <Project/QualitySettings.h>

namespace
{
HE::HardwareInfo machine(uint64_t vramMB, uint64_t ramMB, int threads, const char* vendor = "NVIDIA", const char* api = "D3D12")
{
    HE::HardwareInfo h;
    h.cpuThreads = threads;
    h.ramTotalMB = ramMB;
    h.gpu.name   = "Test GPU";
    h.gpu.vendor = vendor;
    h.gpu.api    = api;
    h.gpu.vramMB = vramMB;
    return h;
}
using HE::QualityPreset;
}

TEST_CASE("recommended preset: the video memory sets the ceiling, the host can lower it")
{
    // 12 GB card on a big PC: Ultra. Smaller cards step down.
    CHECK(HE::recommendQualityPreset(machine(12010, 31859, 32)) == QualityPreset::Ultra);
    CHECK(HE::recommendQualityPreset(machine(10000, 16384, 8))  == QualityPreset::Ultra);
    CHECK(HE::recommendQualityPreset(machine(8192, 31859, 32))  == QualityPreset::High);
    CHECK(HE::recommendQualityPreset(machine(6000, 31859, 32))  == QualityPreset::High);
    CHECK(HE::recommendQualityPreset(machine(4096, 31859, 32))  == QualityPreset::Medium);
    CHECK(HE::recommendQualityPreset(machine(3000, 31859, 32))  == QualityPreset::Medium);
    CHECK(HE::recommendQualityPreset(machine(2048, 31859, 32))  == QualityPreset::Low);
    CHECK(HE::recommendQualityPreset(machine(128, 16384, 8, "Intel")) == QualityPreset::Low);   // an iGPU's dedicated slice

    // The host's memory and threads cap it: a strong card in a small machine.
    CHECK(HE::recommendQualityPreset(machine(12010, 16384, 4))  == QualityPreset::Medium);   // too few threads for Ultra/High
    CHECK(HE::recommendQualityPreset(machine(12010, 12288, 6))  == QualityPreset::High);
    CHECK(HE::recommendQualityPreset(machine(12010, 8192, 4))   == QualityPreset::Medium);
    CHECK(HE::recommendQualityPreset(machine(12010, 4096, 8))   == QualityPreset::Low);
}

TEST_CASE("recommended preset: no GPU, the software rasterizer, unknown memory, Apple's shared memory")
{
    HE::HardwareInfo none;                                   // nothing handed over yet
    none.ramTotalMB = 65536; none.cpuThreads = 32;
    CHECK(HE::recommendQualityPreset(none) == QualityPreset::Low);
    CHECK(HE::recommendQualityPreset(machine(0, 65536, 32, "", "Software")) == QualityPreset::Low);
    CHECK(HE::recommendQualityPreset(machine(0, 65536, 32, "AMD", "OpenGL")) == QualityPreset::Medium);   // vram unknown: never more than Medium
    // Apple: unified memory, so RAM decides.
    CHECK(HE::recommendQualityPreset(machine(12000, 16384, 10, "Apple", "Metal")) == QualityPreset::High);
    CHECK(HE::recommendQualityPreset(machine(24000, 32768, 12, "Apple", "Metal")) == QualityPreset::Ultra);
    CHECK(HE::recommendQualityPreset(machine(5000, 8192, 8, "Apple", "Metal"))    == QualityPreset::Medium);
    // Never Custom.
    CHECK(HE::recommendQualityPreset(machine(12010, 31859, 32)) != QualityPreset::Custom);
}

TEST_CASE("recommended preset: the reason line names the numbers, and the script rows answer")
{
    RestoreProvider restore;
    CHECK(HE::recommendQualityReason(machine(12010, 31859, 32)) == "12010 MB video memory, 31859 MB RAM, 32 threads");
    CHECK(HE::recommendQualityReason(machine(0, 8192, 4)) == "video memory unknown, 8192 MB RAM, 4 threads");
    HE::HardwareInfo none; none.ramTotalMB = 4096; none.cpuThreads = 2;
    CHECK(HE::recommendQualityReason(none) == "no graphics card reported, 4096 MB RAM, 2 threads");

    installFullFake();                                       // 12010 MB GPU, 32768 MB RAM, 16 threads
    CHECK(callRow("hardware.recommendedPreset")[0].s == "Ultra");
    CHECK(callRow("hardware.recommendedPresetReason")[0].s == "12010 MB video memory, 32768 MB RAM, 16 threads");
}

TEST_CASE("hardware rows: with the real detection the rows and the module agree")
{
    RestoreProvider restore;
    HE::setHardwareProvider(nullptr);
    const HE::HardwareInfo h = HE::hardwareInfo();
    CHECK(callRow("hardware.cpuName")[0].s == h.cpuName);
    CHECK(callRow("hardware.ramTotalMB")[0].i == static_cast<int>(h.ramTotalMB));
    CHECK(callRow("hardware.osVersion")[0].s == h.osVersion);
    CHECK(callRow("hardware.value", { HorizonCode::Value::ofString("CpuThreads") })[0].s == std::to_string(h.cpuThreads));
    // The summary is what the startup log prints, line for line.
    CHECK(callRow("hardware.summary")[0].s == HE::hardwareSummary());
}
