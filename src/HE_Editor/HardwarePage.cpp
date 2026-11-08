#include "HardwarePage.h"

#include "EditorHelp.h"
#include "EditorWidgets.h"

#include <Diagnostics/HardwareInfo.h>
#include <Project/QualitySettings.h>   // the suggested preset

#include <imgui.h>

#include <cstdio>
#include <string>
#include <vector>

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

    // More than one adapter / display: each one on its own line, the one in use marked.
    if (h.gpus.size() > 1)
        for (size_t i = 0; i < h.gpus.size(); ++i)
            add("Graphics cards", ("Card " + std::to_string(i + 1) + (h.gpus[i].active ? " (in use)" : "")).c_str(),
                HE::describeGpu(h.gpus[i]));
    if (h.displays.size() > 1)
        for (size_t i = 0; i < h.displays.size(); ++i)
            add("Displays", ("Display " + std::to_string(i + 1)).c_str(), HE::describeDisplay(h.displays[i]));

    add("System", "Operating system", orDash(h.osVersion));
    add("System", "Battery", !h.hasBattery ? "none" : (HE::onBattery() ? "running on battery" : "plugged in"));

    add("Suggestion", "Graphics preset", std::string(HE::qualityPresetName(HE::recommendQualityPreset(h))));
    add("Suggestion", "Based on", HE::recommendQualityReason(h));
    return out;
}

GpuChoice gpuChoice()
{
    const HE::HardwareInfo h = HE::hardwareInfo();
    GpuChoice c;
    c.options.push_back({ "", "Default (system)" });
    for (const HE::GpuInfo& g : h.gpus)
    {
        if (g.id.empty()) continue;      // an adapter that cannot be named cannot be chosen
        std::string label = g.name;
        if (g.vramMB) label += " (" + std::to_string(g.vramMB) + " MB)";
        if (g.software) label += " (software)";
        if (g.active) label += " - in use";
        c.options.push_back({ g.id, label });
    }

    const std::string saved = HE::loadPreferredGpu();
    bool gone = false;
    if (!saved.empty())
    {
        std::vector<HE::GpuInfo> named;      // the options past the default, as the lookup wants them
        for (size_t i = 1; i < c.options.size(); ++i) { HE::GpuInfo g; g.id = c.options[i].id; named.push_back(g); }
        const int found = HE::findGpuIndex(named, saved);
        if (found >= 0) c.selected = found + 1;
        else
        {
            c.options.push_back({ saved, saved + " (not present)" });
            c.selected = static_cast<int>(c.options.size()) - 1;
            gone = true;
        }
    }

    const HE::GpuSelection sel = HE::gpuSelection();
    c.canChoose = sel.canChoose;
    std::vector<std::string> notes;
    if (!sel.canChoose)
        notes.push_back(sel.note + ".");
    else if (h.gpus.size() <= 1)
        notes.push_back("This machine has one graphics card, there is nothing to choose.");
    if (sel.canChoose)
    {
        if (gone)
            notes.push_back("The chosen card is not in this machine: the default card is used until it is back or another one is chosen.");
        else if (saved != HE::preferredGpuId())
            notes.push_back("Takes effect the next time the program starts.");
        else if (!sel.honored && !sel.requested.empty())
            notes.push_back("At this start the choice could not be used: the default card runs instead.");
    }
    for (size_t i = 0; i < notes.size(); ++i) { if (i) c.note += " "; c.note += notes[i]; }
    return c;
}

void draw()
{
    HE::Ed::Help::Scope helpScope("Hardware");

    ImGui::TextWrapped("What this machine is, as the engine sees it. A game reads the same values "
                       "through the hardware rows (Get Hardware Info in HorizonCode, horizon.hardware in "
                       "Lua and Python) to show them on its own settings screen. Nothing here is sent anywhere.");
    ImGui::Spacing();

    // Which graphics card the program starts on: read before the renderer exists, so a change applies at the
    // next start. A game reads and sets the same choice through the hardware rows.
    {
        ImGui::Spacing();
        ImGui::SeparatorText("Graphics card");
        GpuChoice choice = gpuChoice();
        std::vector<const char*> labels;
        for (const GpuChoiceOption& o : choice.options) labels.push_back(o.label.c_str());
        int sel = choice.selected;
        ImGui::BeginDisabled(!choice.canChoose || choice.options.size() <= 1);
        if (EditorWidgets::Row::combo("Graphics card to use", &sel, labels.data(), static_cast<int>(labels.size())) &&
            sel >= 0 && sel < static_cast<int>(choice.options.size()) && sel != choice.selected)
            HE::savePreferredGpu(choice.options[static_cast<size_t>(sel)].id);
        ImGui::EndDisabled();
        if (!choice.note.empty())
            ImGui::TextDisabled("%s", choice.note.c_str());
    }

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
