#include "doctest.h"

#include <Diagnostics/HardwareInfo.h>
#include <Diagnostics/Log.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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

// ── More than one GPU (Thema 337 step 7) ─────────────────────────────────────────────────────────

namespace
{
HE::GpuInfo listed(const char* name, uint32_t vendor, uint32_t device, int ordinal, uint64_t vramMB, const char* driver, bool software = false)
{
    HE::GpuInfo g;
    g.name = name; g.vendorId = vendor; g.deviceId = device; g.vramMB = vramMB; g.driver = driver; g.software = software;
    g.vendor = HE::gpuVendorFromPciId(vendor);
    g.id = HE::makeGpuId(vendor, device, ordinal);
    return g;
}

std::shared_ptr<FakeProvider> twoGpuMachine()
{
    auto f = makeFake();
    f->info.gpus = {
        listed("AMD Radeon(TM) Graphics", 0x1002, 0x13c0, 0, 512, "32.0.21034.41"),
        listed("NVIDIA GeForce RTX 4070", 0x10de, 0x2786, 0, 12010, "621.01"),
        listed("Microsoft Basic Render Driver", 0x1414, 0x008c, 0, 0, "10.0", true),
    };
    return f;
}
}

TEST_CASE("gpu list: ids are vendor:device:ordinal in lower-case hex")
{
    CHECK(HE::makeGpuId(0x10de, 0x2786, 0) == "10de:2786:0");
    CHECK(HE::makeGpuId(0x1002, 0x13C0, 1) == "1002:13c0:1");
    CHECK(HE::makeGpuId(0x1, 0x2, 0) == "0001:0002:0");
}

TEST_CASE("gpu list: the renderer's card is marked active by PCI ids, by id, or by name")
{
    RestoreProvider restore;
    HE::setHardwareProvider(twoGpuMachine());
    {
        const HE::HardwareInfo h = HE::hardwareInfo();
        REQUIRE(h.gpus.size() == 3);
        for (const auto& g : h.gpus) CHECK_FALSE(g.active);      // no renderer yet: none active
        CHECK_FALSE(h.gpu.valid());
    }
    // D3D / Vulkan: the PCI ids
    HE::GpuInfo gpu; gpu.name = "NVIDIA GeForce RTX 4070"; gpu.vendorId = 0x10de; gpu.deviceId = 0x2786; gpu.api = "D3D12"; gpu.vramMB = 12010;
    HE::setGpuInfo(gpu);
    {
        const HE::HardwareInfo h = HE::hardwareInfo();
        CHECK(h.gpu.id == "10de:2786:0");                       // learned from the list
        CHECK(h.gpu.active);
        REQUIRE(h.gpus.size() == 3);
        CHECK_FALSE(h.gpus[0].active);
        CHECK(h.gpus[1].active);
        CHECK_FALSE(h.gpus[2].active);
    }
    // The other card
    HE::GpuInfo igpu; igpu.name = "AMD Radeon(TM) Graphics"; igpu.vendorId = 0x1002; igpu.deviceId = 0x13c0; igpu.api = "Vulkan";
    HE::setGpuInfo(igpu);
    CHECK(HE::hardwareInfo().gpus[0].active);
    CHECK_FALSE(HE::hardwareInfo().gpus[1].active);
    CHECK(HE::hardwareInfo().gpu.vendor == "AMD");              // the list fills what the API left out

    // OpenGL cannot read ids: by name; the memory is whatever the API said, the ids come from the list
    HE::GpuInfo gl; gl.name = "NVIDIA GeForce RTX 4070"; gl.api = "OpenGL";
    HE::setGpuInfo(gl);
    {
        const HE::HardwareInfo h = HE::hardwareInfo();
        CHECK(h.gpus[1].active);
        CHECK(h.gpu.id == "10de:2786:0");
        CHECK(h.gpu.vendorId == 0x10de);
    }
}

TEST_CASE("gpu list: two identical cards are told apart by the id the renderer gives")
{
    RestoreProvider restore;
    auto f = makeFake();
    f->info.gpus = { listed("Twin GPU", 0x10de, 0x2786, 0, 12010, "1"), listed("Twin GPU", 0x10de, 0x2786, 1, 12010, "1") };
    HE::setHardwareProvider(f);
    HE::GpuInfo gpu; gpu.name = "Twin GPU"; gpu.vendorId = 0x10de; gpu.deviceId = 0x2786; gpu.id = "10de:2786:1";
    HE::setGpuInfo(gpu);
    const HE::HardwareInfo h = HE::hardwareInfo();
    REQUIRE(h.gpus.size() == 2);
    CHECK_FALSE(h.gpus[0].active);
    CHECK(h.gpus[1].active);
    CHECK(h.gpu.id == "10de:2786:1");
}

TEST_CASE("gpu list: the renderer's list is used where the platform lists none, the active card is never missing")
{
    RestoreProvider restore;
    HE::setHardwareProvider(makeFake());                       // the fake lists no adapters (a Linux without DXGI)
    CHECK(HE::hardwareInfo().gpus.empty());
    HE::setGpuList({ listed("Vulkan GPU A", 0x10de, 0x1111, 0, 8192, "1"), listed("Vulkan GPU B", 0x1002, 0x2222, 0, 4096, "2") });
    CHECK(HE::hardwareInfo().gpus.size() == 2);
    HE::GpuInfo gpu; gpu.name = "Vulkan GPU B"; gpu.vendorId = 0x1002; gpu.deviceId = 0x2222;
    HE::setGpuInfo(gpu);
    CHECK(HE::hardwareInfo().gpus[1].active);

    // A card the list does not know (a listing that failed): it is added, so the active one is always there.
    HE::setHardwareProvider(makeFake());
    HE::GpuInfo lone; lone.name = "Lone GPU"; lone.vendorId = 0x8086; lone.deviceId = 0x9a49;
    HE::setGpuInfo(lone);
    const HE::HardwareInfo h = HE::hardwareInfo();
    REQUIRE(h.gpus.size() == 1);
    CHECK(h.gpus[0].active);
    CHECK(h.gpus[0].name == "Lone GPU");
    CHECK(h.gpus[0].id == "8086:9a49:0");
}

TEST_CASE("gpu list: the summary names the other adapters, the rows describe all of them")
{
    RestoreProvider restore;
    HE::setHardwareProvider(twoGpuMachine());
    HE::GpuInfo gpu; gpu.name = "NVIDIA GeForce RTX 4070"; gpu.vendorId = 0x10de; gpu.deviceId = 0x2786; gpu.api = "D3D12"; gpu.vramMB = 12010; gpu.driver = "621.01";
    HE::setGpuInfo(gpu);
    const std::string s = HE::hardwareSummary();
    CHECK(s.find("GPU: NVIDIA GeForce RTX 4070 (12010 MB, driver 621.01, D3D12)") != std::string::npos);
    CHECK(s.find("Other GPU: AMD Radeon(TM) Graphics (512 MB, driver 32.0.21034.41)") != std::string::npos);
    CHECK(s.find("Other GPU: Microsoft Basic Render Driver (driver 10.0, software)") != std::string::npos);
    CHECK(s.find("Other GPU: NVIDIA") == std::string::npos);    // the active card is not listed twice

    using HorizonCode::Value;
    CHECK(callRow("hardware.gpuCount")[0].i == 3);
    CHECK(callRow("hardware.activeGpuId")[0].s == "10de:2786:0");
    const auto names = callRow("hardware.gpuNames")[0];
    REQUIRE(names.items.size() == 3);
    CHECK(names.items[0].s == "AMD Radeon(TM) Graphics");
    CHECK(names.items[1].s == "NVIDIA GeForce RTX 4070");
    const auto ids = callRow("hardware.gpuIds")[0];
    REQUIRE(ids.items.size() == 3);
    CHECK(ids.items[0].s == "1002:13c0:0");
    CHECK(ids.items[1].s == "10de:2786:0");
    CHECK(callRow("hardware.gpuAt", { Value::ofInt(1) })[0].s == "NVIDIA GeForce RTX 4070 (12010 MB, driver 621.01)");
    CHECK(callRow("hardware.gpuAt", { Value::ofInt(2) })[0].s == "Microsoft Basic Render Driver (driver 10.0, software)");
    CHECK(callRow("hardware.gpuAt", { Value::ofInt(3) })[0].s.empty());
    CHECK(callRow("hardware.gpuAt", { Value::ofInt(-1) })[0].s.empty());
}

// ── Choosing the GPU (Thema 337 step 8) ──────────────────────────────────────────────────────────

namespace
{
void setEnvVar(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

// A scratch folder for the game's choice file, with the choice state put back afterwards.
struct ChoiceScratch
{
    std::filesystem::path dir;
    ChoiceScratch()
    {
        dir = std::filesystem::temp_directory_path() / "he_gpu_choice_test";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        setEnvVar("HE_GPU_ADAPTER", "");
        HE::setGpuChoiceDirectory(dir.string());
    }
    ~ChoiceScratch()
    {
        HE::setGpuChoiceDirectory("");
        HE::setPreferredGpuId("");
        setEnvVar("HE_GPU_ADAPTER", "");
        std::filesystem::remove_all(dir);
    }
};
}

TEST_CASE("gpu choice: an id is found in the list, case-insensitively, and never when empty")
{
    const std::vector<HE::GpuInfo> list = twoGpuMachine()->info.gpus;
    CHECK(HE::findGpuIndex(list, "1002:13c0:0") == 0);
    CHECK(HE::findGpuIndex(list, "10DE:2786:0") == 1);
    CHECK(HE::findGpuIndex(list, "10de:2786:1") == -1);        // there is no second RTX 4070
    CHECK(HE::findGpuIndex(list, "") == -1);
    CHECK(HE::findGpuIndex({}, "10de:2786:0") == -1);
    CHECK(HE::findGpuIndex(list, "nonsense") == -1);
}

TEST_CASE("gpu choice: the game keeps its choice in a file of its own, the environment overrides it")
{
    ChoiceScratch scratch;
    CHECK(HE::loadPreferredGpu().empty());                      // nothing chosen: the default adapter
    CHECK(HE::savePreferredGpu("10de:2786:0"));
    CHECK(HE::loadPreferredGpu() == "10de:2786:0");
    CHECK(std::filesystem::exists(scratch.dir / "gpu_adapter.txt"));

    CHECK(HE::savePreferredGpu(""));                            // back to the default
    CHECK(HE::loadPreferredGpu().empty());

    CHECK(HE::savePreferredGpu("1002:13c0:0"));
    setEnvVar("HE_GPU_ADAPTER", "10de:2786:0");                 // a developer's override wins ...
    CHECK(HE::loadPreferredGpu() == "10de:2786:0");
    setEnvVar("HE_GPU_ADAPTER", "default");                     // ... and "default" means none
    CHECK(HE::loadPreferredGpu().empty());
    setEnvVar("HE_GPU_ADAPTER", "");
    CHECK(HE::loadPreferredGpu() == "1002:13c0:0");             // the file again

    // Whitespace in a hand-edited file does not break the id.
    { std::ofstream out(scratch.dir / "gpu_adapter.txt", std::ios::trunc); out << "  10de:2786:0 \r\n"; }
    CHECK(HE::loadPreferredGpu() == "10de:2786:0");
}

TEST_CASE("gpu choice: the run's choice and what came of it")
{
    ChoiceScratch scratch;
    HE::setPreferredGpuId("10de:2786:0");
    CHECK(HE::preferredGpuId() == "10de:2786:0");
    CHECK(HE::gpuSelection().requested == "10de:2786:0");
    CHECK(HE::gpuSelection().honored);                          // until a renderer says otherwise

    HE::GpuSelection s; s.requested = "10de:2786:0"; s.honored = false; s.canChoose = false; s.note = "OpenGL cannot choose";
    HE::setGpuSelection(s);
    CHECK_FALSE(HE::gpuSelection().honored);
    CHECK_FALSE(HE::gpuSelection().canChoose);
    CHECK(callRow("hardware.gpuChoiceNote")[0].s == "OpenGL cannot choose");

    HE::setPreferredGpuId("");                                  // a new start resets the outcome
    CHECK(HE::gpuSelection().note.empty());
    CHECK(HE::gpuSelection().canChoose);
}

TEST_CASE("gpu choice: the rows save a known id for the next start and refuse an unknown one")
{
    RestoreProvider restore;
    ChoiceScratch scratch;
    HE::setHardwareProvider(twoGpuMachine());
    using HorizonCode::Value;

    CHECK(callRow("hardware.preferredGpu")[0].s.empty());
    CHECK(callRow("hardware.setPreferredGpu", { Value::ofString("10de:2786:0") })[0].b);
    CHECK(callRow("hardware.preferredGpu")[0].s == "10de:2786:0");
    CHECK(HE::loadPreferredGpu() == "10de:2786:0");
    CHECK(HE::preferredGpuId().empty());                        // the running game keeps its card

    CHECK_FALSE(callRow("hardware.setPreferredGpu", { Value::ofString("dead:beef:0") })[0].b);
    CHECK(callRow("hardware.preferredGpu")[0].s == "10de:2786:0");   // the refused id changed nothing

    CHECK(callRow("hardware.setPreferredGpu", { Value::ofString("") })[0].b);   // back to the default
    CHECK(callRow("hardware.preferredGpu")[0].s.empty());
}
