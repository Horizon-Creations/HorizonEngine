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
