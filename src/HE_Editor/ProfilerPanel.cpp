#include "ProfilerPanel.h"
#include "EditorApplication.h"           // AppContext
#include <Diagnostics/EngineProfiler.h>
#include <Diagnostics/Logger.h>
#include <cstdio>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include "EditorWidgets.h"               // WrapText
#include <imgui.h>
#endif

namespace ProfilerPanel
{

// ─── Performance Profiler window (View > Performance Profiler) ──────────────────
// Three tabs over the runtime EngineProfiler:
//   Overview     — always-live HUD: FPS / CPU / GPU / counters + a frame-time graph.
//   Capture      — benchmark capture (F9 → JSON dump), single-frame capture, toggles.
//   Frame Detail — the full per-pass GPU + per-scope CPU breakdown of one frame
//                  (the single-frame capture if taken, else the last captured frame).
// Reads the profiler singleton directly. Sets liveEnabled() so the app feeds the HUD.
#ifdef HE_IMGUI_ENABLED
// Full breakdown of one captured frame: counters, GPU passes, CPU scope tree.
static void DrawFrameDetail(const ProfFrameRecord& f)
{
    const double fps = f.deltaMs > 0.0 ? 1000.0 / f.deltaMs : 0.0;
    ImGui::Text("Frame %llu", static_cast<unsigned long long>(f.index));
    ImGui::Text("CPU %.3f ms", f.cpuFrameMs);
    ImGui::SameLine(160); ImGui::Text("frame %.3f ms (%.0f FPS)", f.deltaMs, fps);
    if (f.gpuFrameMs >= 0.0)
    {
        ImGui::Text("GPU %.3f ms", f.gpuFrameMs);
        if (f.gpuTimingMode && f.gpuTimingMode[0])
        { ImGui::SameLine(160); ImGui::TextDisabled("mode: %s", f.gpuTimingMode); }
    }
    else
        ImGui::TextDisabled("GPU n/a on this backend");

    const ProfRenderStats& s = f.stats;
    ImGui::Text("draws %u  ·  tris %u  ·  objects %u/%u visible",
                s.drawCalls, s.triangles, s.visibleObjects, s.totalObjects);
    if (s.vramBudgetMB > 0.0)
        ImGui::Text("VRAM %.0f / %.0f MB", s.vramUsedMB, s.vramBudgetMB);

    // ── GPU passes ──────────────────────────────────────────────────────────
    if (!f.gpuPasses.empty())
    {
        const std::string gpuMode = f.gpuTimingMode ? f.gpuTimingMode : "";
        // "detailed" (Metal, serialized cmd-buffer/pass) and "gl-timer" (GL timer
        // queries) are both exclusive + additive per-pass, so the sum is meaningful;
        // "counter" spans overlap on TBDR and must NOT be summed.
        const bool detailed = gpuMode == "detailed" || gpuMode == "gl-timer";
        const double gref = f.gpuFrameMs > 0.0 ? f.gpuFrameMs : 1.0;
        ImGui::Separator();
        ImGui::TextUnformatted(detailed ? "GPU passes (exclusive, additive)"
                                        : "GPU passes (per-encoder spans — see caveat)");
        double sumExact = 0.0; bool anyExact = false;
        for (const ProfGpuPass& gp : f.gpuPasses)
        {
            const char* nm = gp.name ? gp.name : "?";
            if (gp.approx)
            {
                ImGui::TextDisabled("    ~ %s", nm);
                ImGui::SameLine(210); ImGui::TextDisabled("%7.3f ms", gp.ms);
            }
            else
            {
                sumExact += gp.ms; anyExact = true;
                ImGui::Text("%s", nm);
                ImGui::SameLine(210); ImGui::Text("%7.3f ms", gp.ms);
            }
            ImGui::SameLine(300);
            ImGui::ProgressBar(static_cast<float>(gp.ms / gref), ImVec2(-1, 0), "");
        }
        if (anyExact && f.gpuFrameMs > 0.0)
        {
            if (!detailed && sumExact > f.gpuFrameMs * 1.05)
            {
                // ── Wrapped, not clipped ────────────────────────────────────
                // This one sentence is the entire warning: it says the column
                // above must NOT be added up. Drawn unwrapped in a docked
                // profiler the reader gets "Σ spans 4.21 ms = 3.1x GPU frame —
                // spans OVE" and no reason at all to distrust the numbers they
                // were about to sum, which is worse than not printing it.
                //
                // The wrap is pushed around the PROSE in this panel only, never
                // around the pass and scope rows: those place their millisecond
                // column and their bar with SameLine(210) / SameLine(300), so a
                // name that wrapped would leave its own number stranded beside
                // the second line. Pass and scope names are short by
                // construction — a pass is called "Shadow", not a sentence — so
                // there is nothing to win there and a broken table to lose.
                EditorWidgets::WrapText wrap;
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.2f, 1.0f),
                    "\xCE\xA3 spans %.2f ms = %.1fx GPU frame — spans OVERLAP, not exclusive; do not sum.",
                    sumExact, sumExact / f.gpuFrameMs);
            }
            else
            {
                ImGui::Text("\xCE\xA3 passes %.3f ms", sumExact);
                ImGui::SameLine(220);
                ImGui::TextDisabled("untimed %.3f ms", f.gpuFrameMs - sumExact);
            }
        }
        {
            // Same reasoning as the overlap warning above: these are sentences
            // that qualify every number in the list, and half a caveat reads as
            // no caveat.
            EditorWidgets::WrapText wrap;
            if (!detailed)
                ImGui::TextDisabled("Per-encoder spans overlap on TBDR — enable 'Detailed GPU' for exclusive per-pass.");
            else if (gpuMode == "detailed")
                ImGui::TextDisabled("Note: the FIRST pass (Shadow) absorbs GPU queue/present latency in a single\n"
                                    "serialized frame — it can read high here. Trust the Overview median, not one frame.");
            else // gl-timer: exact, exclusive per-pass GPU time — no serialization caveat.
                ImGui::TextDisabled("GL timer queries: exact per-pass GPU time; \xCE\xA3 passes + untimed = GPU frame.");
        }
    }

    // ── CPU scopes (nested) ─────────────────────────────────────────────────
    if (!f.scopes.empty())
    {
        const double ref = f.cpuFrameMs > 0.0 ? f.cpuFrameMs : 1.0;
        ImGui::Separator();
        ImGui::TextUnformatted("CPU scopes");
        for (const ProfScopeSample& sc : f.scopes)
        {
            std::string label(static_cast<size_t>(sc.depth) * 2, ' ');
            label += sc.name ? sc.name : "?";
            ImGui::Text("%s", label.c_str());
            ImGui::SameLine(210); ImGui::Text("%7.3f ms", sc.ms);
            ImGui::SameLine(300);
            ImGui::ProgressBar(static_cast<float>(sc.ms / ref), ImVec2(-1, 0), "");
        }
    }
}
#endif // HE_IMGUI_ENABLED

void DrawProfilerWindow(AppContext& ctx, bool& open)
{
#ifdef HE_IMGUI_ENABLED
    EngineProfiler& prof = EngineProfiler::instance();
    if (!open) { prof.setLiveEnabled(false); return; }

    ImGui::SetNextWindowSize(ImVec2(480, 600), ImGuiCond_FirstUseEver);
    const bool visible = ImGui::Begin("Performance Profiler", &open);
    // Feed the live HUD only while the window is actually visible (not collapsed).
    prof.setLiveEnabled(visible);
    if (!visible) { ImGui::End(); return; }

    if (ImGui::BeginTabBar("##profTabs"))
    {
        // ── Overview: live HUD + frame-time graph ───────────────────────────
        if (ImGui::BeginTabItem("Overview"))
        {
            // The counter line ("draws … tris … objects …") is longer than this
            // panel is wide as soon as it is docked into a side column, and the
            // number that got cut off is the one somebody opened the profiler
            // for. Pushed per tab rather than once for the window: the Frame
            // Detail tab lays its rows out with SameLine at fixed offsets and
            // must NOT wrap (see DrawFrameDetail).
            EditorWidgets::WrapText wrap;
            const std::vector<ProfLiveFrame> live = prof.liveSnapshot();
            if (live.empty())
                ImGui::TextDisabled("Collecting live data…");
            else
            {
                const ProfLiveFrame& cur = live.back();
                const double fps = cur.deltaMs > 0.0 ? 1000.0 / cur.deltaMs : 0.0;
                ImGui::Text("%.1f FPS", fps);
                ImGui::SameLine(120); ImGui::Text("frame %.2f ms", cur.deltaMs);
                ImGui::Text("CPU %.2f ms", cur.cpuFrameMs);
                ImGui::SameLine(120);
                if (cur.gpuFrameMs >= 0.0) ImGui::Text("GPU %.2f ms", cur.gpuFrameMs);
                else                       ImGui::TextDisabled("GPU n/a");
                ImGui::Text("draws %u  ·  tris %u  ·  objects %u/%u",
                            cur.draws, cur.triangles, cur.visible, cur.total);

                // Frame-time graph over the live window (+ avg/max overlay).
                std::vector<float> ftimes; ftimes.reserve(live.size());
                float mx = 0.0f; double sum = 0.0;
                for (const ProfLiveFrame& lf : live)
                {
                    const float v = static_cast<float>(lf.deltaMs);
                    ftimes.push_back(v); sum += v; if (v > mx) mx = v;
                }
                const float avg = ftimes.empty() ? 0.0f : static_cast<float>(sum / ftimes.size());
                char overlay[64];
                std::snprintf(overlay, sizeof(overlay), "avg %.2f  max %.2f ms", avg, mx);
                ImGui::Separator();
                ImGui::TextUnformatted("Frame time (ms)");
                ImGui::PlotLines("##ft", ftimes.data(), static_cast<int>(ftimes.size()),
                                 0, overlay, 0.0f, mx > 0.0f ? mx * 1.1f : 1.0f, ImVec2(-1, 80));

                // GPU-time graph (only if available).
                bool anyGpu = false;
                std::vector<float> gtimes; gtimes.reserve(live.size());
                float gmx = 0.0f;
                for (const ProfLiveFrame& lf : live)
                {
                    const float v = lf.gpuFrameMs >= 0.0 ? static_cast<float>(lf.gpuFrameMs) : 0.0f;
                    if (lf.gpuFrameMs >= 0.0) anyGpu = true;
                    gtimes.push_back(v); if (v > gmx) gmx = v;
                }
                if (anyGpu)
                {
                    ImGui::TextUnformatted("GPU time (ms, whole frame)");
                    ImGui::PlotLines("##gt", gtimes.data(), static_cast<int>(gtimes.size()),
                                     0, nullptr, 0.0f, gmx > 0.0f ? gmx * 1.1f : 1.0f, ImVec2(-1, 60));
                }
            }
            ImGui::EndTabItem();
        }

        // ── Capture controls ────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Capture"))
        {
            // Every dimmed line in this tab is a full sentence, and the last one
            // is an absolute path to the dumps folder — the thing the user came
            // here to read before hunting for the JSON on disk. Unwrapped it is
            // cut off somewhere in the middle of the home directory.
            EditorWidgets::WrapText wrap;
            const bool recording = prof.isRecordingOrPending();
            if (recording)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
                if (ImGui::Button("Stop & Dump  (F9)", ImVec2(-1, 0)) && ctx.toggleProfilerCapture)
                    ctx.toggleProfilerCapture();
                ImGui::PopStyleColor();
                ImGui::TextDisabled("Recording — %zu frames (vsync off)", prof.recordedFrames());
            }
            else
            {
                if (ImGui::Button("Start Benchmark Capture  (F9)", ImVec2(-1, 0)) && ctx.toggleProfilerCapture)
                    ctx.toggleProfilerCapture();
                ImGui::TextDisabled("Benchmark = vsync-off multi-frame capture → JSON dump.");

                // Single-frame capture: one frame in full detail (forces detailed GPU),
                // shown in the Frame Detail tab. No dump.
                if (ImGui::Button("Capture Single Frame", ImVec2(-1, 0)))
                    prof.requestSingleFrameCapture();
                ImGui::TextDisabled("One frame (CPU scopes + counters + GPU) → 'Frame Detail'. Fast unless");
                ImGui::TextDisabled("'Detailed GPU' is ticked (then exclusive per-pass, but that frame is slow).");
            }

            ImGui::Separator();
            // Detailed GPU capture (serialized per-pass). Also auto-forced for single frames.
            bool detailed = prof.detailedGpuCapture();
            if (ImGui::Checkbox("Detailed GPU pass timing (serializes GPU — capture only)", &detailed))
                prof.setDetailedGpuCapture(detailed);
            ImGui::TextDisabled("On = exclusive per-pass GPU (ranking/upper bound). FPS during capture is meaningless.");

            // Debug: tint lit fragments by shadow-cascade index (red/green/blue) to
            // verify the CSM split placement (cascade 0 should hug the camera).
            static bool s_dbgCascades = false;
            if (ImGui::Checkbox("Debug: shadow cascades (cascade-index tint)", &s_dbgCascades))
                if (ctx.renderer) ctx.renderer->SetShadowDebug(s_dbgCascades);

            ImGui::Separator();
            if (ImGui::Button("Dump Now"))
            {
                std::string p = prof.dumpNow();
                if (!p.empty()) HE_LOG_INFO(Editor, "%s", ("Profiler dump: " + p).c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Dumps Folder"))
            {
                std::string dir = !prof.dumpsDir().empty()
                                ? prof.dumpsDir()
                                : (ctx.globalState ? ctx.globalState->getDumpsDir() : std::string());
                if (!dir.empty()) SDL_OpenURL(("file://" + dir).c_str());
            }
            {
                std::string dir = !prof.dumpsDir().empty()
                                ? prof.dumpsDir()
                                : (ctx.globalState ? ctx.globalState->getDumpsDir() : std::string("(starts on first capture)"));
                ImGui::TextDisabled("%s", dir.c_str());
            }
            ImGui::EndTabItem();
        }

        // ── Frame Detail: single-frame capture, else last captured frame ────
        if (ImGui::BeginTabItem("Frame Detail"))
        {
            const ProfFrameRecord* single = prof.singleFrame();
            const ProfFrameRecord* last   = prof.lastFrame();
            const ProfFrameRecord* f      = single ? single : last;
            if (single) ImGui::TextDisabled("Source: single-frame capture");
            else if (last) ImGui::TextDisabled("Source: last benchmark frame");
            ImGui::Separator();
            if (f) DrawFrameDetail(*f);
            else
            {
                // The empty state names the two buttons that fill it; clipped, it
                // names one and a half.
                EditorWidgets::WrapText wrap;
                ImGui::TextDisabled("No frame yet — use 'Capture Single Frame' or run a benchmark.");
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
#else
    (void)ctx; (void)open;
#endif
}

} // namespace ProfilerPanel
