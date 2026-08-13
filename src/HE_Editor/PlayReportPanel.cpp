#include "PlayReportPanel.h"
#include "EditorApplication.h"           // AppContext (playLog / playLogMutex / playReportOpen)
#include <Diagnostics/Logger.h>
#include <cstdio>
#include <mutex>
#include <string>

#ifdef HE_IMGUI_ENABLED
#include "EditorWidgets.h"               // WrapText
#include <imgui.h>
#endif

namespace PlayReportPanel
{

// ── Post-PIE report ───────────────────────────────────────────────────────────
// Opens automatically when a play session ends with captured warnings/errors
// (EditorApplication installs a Logger sink for the duration of play).
void drawPlayReport(AppContext& ctx)
{
    if (!ctx.playReportOpen || !*ctx.playReportOpen || !ctx.playLog || !ctx.playLogMutex)
        return;

    static bool s_showWarnings = true;
    ImGui::SetNextWindowSize(ImVec2(580.0f, 380.0f), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::Begin("Play Session Report", &open, ImGuiWindowFlags_NoCollapse))
    {
        // Wrapped, not scrolled sideways. This window exists to be read once,
        // quickly, after play stops; the user may well have docked it narrow.
        // The header counts fit anywhere, but the moment they do not — a long
        // "n error(s), m warning(s) during the last play session" in a narrow
        // dock — a clipped line is indistinguishable from a shorter message,
        // and the reader has no cue that anything was withheld. The wrap has to
        // be pushed again inside the log child below: the wrap position lives on
        // the window, and a child is a window of its own.
        EditorWidgets::WrapText wrap;
        std::lock_guard<std::mutex> lk(*ctx.playLogMutex);
        int errors = 0, warnings = 0;
        for (const auto& e : *ctx.playLog)
            (e.level == HE::LogLevel::Warning ? warnings : errors)++;

        if (errors > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
                "%d error(s), %d warning(s) during the last play session", errors, warnings);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f),
                "%d warning(s) during the last play session", warnings);

        ImGui::Checkbox("Show warnings", &s_showWarnings);
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy All"))
        {
            std::string all;
            for (const auto& e : *ctx.playLog)
            {
                char head[48];
                std::snprintf(head, sizeof head, "[%7.2fs] [%s] ", e.time,
                    e.level == HE::LogLevel::Warning ? "WARN" : "ERROR");
                all += head; all += e.message;
                if (e.count > 1) { char rep[24]; std::snprintf(rep, sizeof rep, " (x%d)", e.count); all += rep; }
                all += '\n';
            }
            ImGui::SetClipboardText(all.c_str());
        }
        ImGui::Separator();

        ImGui::BeginChild("##playlog", ImVec2(0.0f, -34.0f), ImGuiChildFlags_Borders);
        {
            // Closes before EndChild on purpose: the wrap position is pushed onto
            // the child window, so it has to be popped while that window is still
            // the current one.
            EditorWidgets::WrapText wrapLog;
            for (const auto& e : *ctx.playLog)
            {
                const bool isWarn = e.level == HE::LogLevel::Warning;
                if (isWarn && !s_showWarnings) continue;
                ImGui::PushStyleColor(ImGuiCol_Text, isWarn
                    ? ImVec4(1.0f, 0.85f, 0.45f, 1.0f)      // warning → yellow
                    : ImVec4(1.0f, 0.45f, 0.45f, 1.0f));    // error/critical → red
                if (e.count > 1)
                    ImGui::TextWrapped("[%7.2fs] %s  (x%d)", e.time, e.message.c_str(), e.count);
                else
                    ImGui::TextWrapped("[%7.2fs] %s", e.time, e.message.c_str());
                ImGui::PopStyleColor();
            }
            if (ctx.playLog->size() >= 2000)
                ImGui::TextDisabled("(capture capped at 2000 entries)");
        }
        ImGui::EndChild();

        if (ImGui::Button("Close", ImVec2(90.0f, 0.0f))) open = false;
    }
    ImGui::End();
    if (!open) *ctx.playReportOpen = false;
}

} // namespace PlayReportPanel
