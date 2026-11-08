#include "HardwarePage.h"

#include "EditorHelp.h"
#include "EditorWidgets.h"

#include <Diagnostics/HardwareInfo.h>

#include <imgui.h>

#include <cstdio>

namespace
{
std::string orDash(const std::string& s) { return s.empty() ? std::string("-") : s; }

std::string megabytes(uint64_t mb)
{
    if (mb == 0) return "-";
    char buf[64];
    if (mb >= 1024) std::snprintf(buf, sizeof buf, "%llu MB (%.1f GB)", static_cast<unsigned long long>(mb), static_cast<double>(mb) / 1024.0);
    else            std::snprintf(buf, sizeof buf, "%llu MB", static_cast<unsigned long long>(mb));
    return buf;
}
}

namespace HardwarePage
{

std::vector<Row> rows()
{
    const HE::HardwareInfo h = HE::hardwareInfo();
    std::vector<Row> out;
    auto add = [&](const char* group, const char* label, const std::string& value) {
        out.push_back({ group, label, value });
    };
    char buf[96];

    add("Processor", "Name", orDash(h.cpuName));
    add("Processor", "Cores", h.cpuCores > 0 ? std::to_string(h.cpuCores) : "-");
    add("Processor", "Threads", h.cpuThreads > 0 ? std::to_string(h.cpuThreads) : "-");
    add("Processor", "Architecture", orDash(h.cpuArch));

    add("Memory", "Installed", megabytes(h.ramTotalMB));
    add("Memory", "Free now", megabytes(HE::freeMemoryMB()));

    add("Graphics", "Name", orDash(h.gpu.name));
    add("Graphics", "Vendor", orDash(h.gpu.vendor));
    add("Graphics", "Driver", orDash(h.gpu.driver));
    add("Graphics", "Video memory", megabytes(h.gpu.vramMB));
    add("Graphics", "Graphics API", orDash(h.gpu.api));

    if (h.display.valid())
    {
        if (h.display.refreshHz > 0.0f)
            std::snprintf(buf, sizeof buf, "%d x %d @ %.0f Hz", h.display.width, h.display.height, static_cast<double>(h.display.refreshHz));
        else
            std::snprintf(buf, sizeof buf, "%d x %d", h.display.width, h.display.height);
        add("Display", "Resolution", buf);
        std::snprintf(buf, sizeof buf, "%.0f %%", static_cast<double>(h.display.scale) * 100.0);
        add("Display", "Interface scale", buf);
    }
    else
    {
        add("Display", "Resolution", "-");
        add("Display", "Interface scale", "-");
    }
    add("Display", "Name", orDash(h.display.name));

    add("System", "Operating system", orDash(h.osVersion));
    add("System", "Battery", !h.hasBattery ? "none" : (HE::onBattery() ? "running on battery" : "plugged in"));
    return out;
}

void draw()
{
    HE::Ed::Help::Scope helpScope("Hardware");

    ImGui::TextWrapped("What this machine is, as the engine sees it. A game reads the same values "
                       "through the hardware rows (Get Hardware Info in HorizonCode, horizon.hardware in "
                       "Lua and Python) to show them on its own settings screen. Nothing here is sent anywhere.");
    ImGui::Spacing();

    const std::vector<Row> all = rows();
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##hardwaretable", 2, kFlags))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        std::string lastGroup;
        for (const Row& r : all)
        {
            if (r.group != lastGroup)
            {
                lastGroup = r.group;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::SeparatorText(r.group.c_str());
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r.label.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", r.value.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (EditorWidgets::button("Copy summary"))
        ImGui::SetClipboardText(HE::hardwareSummary().c_str());
}

} // namespace HardwarePage
