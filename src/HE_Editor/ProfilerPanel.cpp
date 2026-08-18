#include "ProfilerPanel.h"
#include "EditorApplication.h"           // AppContext
#include <Diagnostics/EngineProfiler.h>
#include <Diagnostics/Logger.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include "EditorTheme.h"
#include "EditorWidgets.h"               // WrapText
#include "ProfilerLayout.h"
#include "ProfilerWidgets.h"
#include <Diagnostics/ProfilerStats.h>
#include <imgui.h>
#endif

namespace ProfilerPanel
{

// ─── Performance Profiler window (View > Performance Profiler) ──────────────────
// Five tabs over the runtime EngineProfiler:
//   Overview     — live HUD: stat tiles, budget bars, frame-time graph with hitch
//                  colouring and percentile guides, distribution histogram, counters.
//   Timeline     — the multi-lane flame view: main thread + one lane per JobSystem
//                  worker + frame separators. The only view that shows parallelism.
//   Scopes       — the working screen: every CPU scope ranked by SELF time, with
//                  count / avg / p95 / max and a sparkline for shape.
//   Capture      — start/stop, single frame, toggles, dump, and the hitch list.
//   Frame Detail — one frame in full: GPU passes + the nested CPU scope tree.
// Reads the profiler singleton directly. Sets liveEnabled() so the app feeds the HUD.
#ifdef HE_IMGUI_ENABLED

namespace L = HE::Prof::Layout;

namespace
{
// ── Panel-local UI state ────────────────────────────────────────────────────
// Static rather than a member because the panel is a free function called from
// the editor's frame loop, matching every other panel in this folder. The timeline
// camera in particular MUST survive across frames and tab switches — a view that
// resets to "whole capture" every frame cannot be zoomed at all.
struct PanelState
{
	L::TimeView timelineView;
	// The frame's own index, NOT its position in the snapshot: while a capture is
	// recording, the ring drops the oldest frames and every position shifts under
	// the selection. Resolved to a position at draw time.
	uint64_t    selectedFrame    = 0;
	bool        haveSelectedFrame = false;
	double      targetFps       = 60.0;   // the budget everything is coloured against
	bool        showGpuGraph    = true;
};
PanelState g_ui;

// ── Analysis cache ──────────────────────────────────────────────────────────
// snapshot() and timelineSnapshot() both DEEP COPY — all captured frames with
// their scope vectors, every thread lane with up to a quarter-million spans. Three
// tabs want that data, and drawing runs at editor frame rate, so calling them per
// draw would copy megabytes 60x a second and (for the timeline) hold each lane's
// mutex against the worker threads still writing into it: the profiler would
// visibly slow down the thing it is measuring.
//
// So the copies happen on change, not on draw. A finished capture is fetched once
// (the frame count stops moving); a running one refreshes on a slow timer, which
// is plenty for a view of thousands of frames.
struct AnalysisCache
{
	size_t                                framesSeen  = 0;
	bool                                  wasRecording = false;
	bool                                  primed      = false;
	double                                lastRefresh = -1.0;
	std::vector<ProfFrameRecord>          frames;
	std::vector<HE::Prof::ScopeAggregate> aggregates;
	std::vector<HE::Prof::Hitch>          hitches;

	std::vector<ProfThreadTimeline>            lanes;
	std::vector<HE::Prof::LaneOccupancy>       occupancy;
	double                                lastLaneRefresh = -1.0;
};
AnalysisCache g_cache;

constexpr double kRefreshSeconds = 0.25;   // while recording; instant otherwise

double budgetMs() { return g_ui.targetFps > 0.0 ? 1000.0 / g_ui.targetFps : 16.666; }

bool cacheIsStale(EngineProfiler& prof, double now, double& lastRefreshField)
{
	const bool   recording = prof.isRecording();
	const size_t count     = prof.recordedFrames();
	if (!g_cache.primed)                     return true;
	if (count != g_cache.framesSeen)         return recording ? (now - lastRefreshField >= kRefreshSeconds) : true;
	if (recording != g_cache.wasRecording)   return true;   // capture just stopped: refresh once, exactly
	return false;
}

// Frames the analysis views work on: the captured benchmark frames. The live ring
// cannot feed these — it carries no scopes.
const std::vector<ProfFrameRecord>& analysisFrames(EngineProfiler& prof)
{
	const double now = ImGui::GetTime();
	if (cacheIsStale(prof, now, g_cache.lastRefresh))
	{
		g_cache.frames       = prof.snapshot();
		g_cache.aggregates   = HE::Prof::aggregateScopes(g_cache.frames);
		g_cache.hitches      = HE::Prof::findHitches(g_cache.frames);
		g_cache.framesSeen   = prof.recordedFrames();
		g_cache.wasRecording = prof.isRecording();
		g_cache.lastRefresh  = now;
		g_cache.primed       = true;
	}
	return g_cache.frames;
}

// Same deal for the timeline, on its own timer: the Timeline tab is usually open
// while a capture runs, and its copy is the expensive one.
void refreshLanes(EngineProfiler& prof)
{
	const double now = ImGui::GetTime();
	const bool   due = g_cache.lastLaneRefresh < 0.0 ||
	                   !prof.isRecording() ||
	                   (now - g_cache.lastLaneRefresh) >= kRefreshSeconds;
	if (!due) return;
	// Not recording: only re-copy when the capture size actually changed, so an
	// idle panel sitting on a finished capture costs nothing at all.
	if (!prof.isRecording() && g_cache.lastLaneRefresh >= 0.0 &&
	    g_cache.framesSeen == prof.recordedFrames() && !g_cache.lanes.empty())
		return;

	g_cache.lanes = prof.timelineSnapshot();
	double captureMs = 0.0;
	if (!prof.frameMarks().empty())
		captureMs = static_cast<double>(prof.frameMarks().back().endNs) * 1e-6;
	g_cache.occupancy = HE::Prof::laneOccupancy(g_cache.lanes, captureMs);
	g_cache.lastLaneRefresh = now;
}

void CountersGrid(uint32_t draws, uint32_t tris, uint32_t visible, uint32_t total,
                  const ProfSceneCounters& sc)
{
	if (!ImGui::BeginTable("##counters", 4,
	                       ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV))
		return;
	auto cell = [](const char* label, uint64_t value) {
		ImGui::TableNextColumn();
		ImGui::TextColored(HE::Ed::Theme::TextDim, "%s", label);
		ImGui::Text("%llu", static_cast<unsigned long long>(value));
	};
	ImGui::TableNextRow();
	cell("draws", draws);
	cell("triangles", tris);
	cell("visible", visible);
	cell("objects", total);
	ImGui::TableNextRow();
	cell("entities", sc.entities);
	cell("lights", sc.lights);
	cell("particles", sc.particles);
	cell("emitters", sc.emitters);
	ImGui::TableNextRow();
	cell("rigid bodies", sc.rigidBodies);
	cell("audio srcs", sc.audioSources);
	cell("scripts", sc.scripts);
	// Streaming is the one counter here that explains a HITCH rather than a cost:
	// a spike while loads are in flight is a stall on I/O, not on the renderer.
	cell("streaming", sc.streamingInFlight);
	ImGui::EndTable();
}

// ── Overview ────────────────────────────────────────────────────────────────
void DrawOverview(EngineProfiler& prof)
{
	const std::vector<ProfLiveFrame> live = prof.liveSnapshot();
	if (live.empty()) { ImGui::TextDisabled("Collecting live data…"); return; }

	std::vector<double> deltas; deltas.reserve(live.size());
	std::vector<float>  bars;   bars.reserve(live.size());
	std::vector<float>  cpuSpark, gpuSpark;
	bool anyGpu = false;
	for (const ProfLiveFrame& f : live)
	{
		deltas.push_back(f.deltaMs);
		bars.push_back(static_cast<float>(f.deltaMs));
		cpuSpark.push_back(static_cast<float>(f.cpuFrameMs));
		if (f.gpuFrameMs >= 0.0) { anyGpu = true; gpuSpark.push_back(static_cast<float>(f.gpuFrameMs)); }
	}
	const ProfLiveFrame&      cur  = live.back();
	const HE::Prof::Percentiles pct = HE::Prof::computePercentiles(deltas);
	const HE::Prof::FpsLows     low = HE::Prof::computeFpsLows(deltas);

	// ── Stat tiles ──────────────────────────────────────────────────────────
	const float avail = ImGui::GetContentRegionAvail().x;
	const float tileW = std::max(96.0f, (avail - ImGui::GetStyle().ItemSpacing.x * 3.0f) / 4.0f);
	char v0[32], v1[32], v2[32], v3[32], s0[48], s1[48], s2[48], s3[48];

	std::snprintf(v0, sizeof(v0), "%.0f", cur.deltaMs > 0.0 ? 1000.0 / cur.deltaMs : 0.0);
	std::snprintf(s0, sizeof(s0), "avg %.0f  ·  1%% low %.0f", low.avgFps, low.low1Fps);
	std::snprintf(v1, sizeof(v1), "%.2f", cur.deltaMs);
	std::snprintf(s1, sizeof(s1), "p95 %.2f  p99 %.2f", pct.p95, pct.p99);
	std::snprintf(v2, sizeof(v2), "%.2f", cur.cpuFrameMs);
	std::snprintf(s2, sizeof(s2), "frame loop");
	if (cur.gpuFrameMs >= 0.0) std::snprintf(v3, sizeof(v3), "%.2f", cur.gpuFrameMs);
	else                       std::snprintf(v3, sizeof(v3), "n/a");
	std::snprintf(s3, sizeof(s3), cur.gpuFrameMs >= 0.0 ? "whole frame" : "backend has no timer");

	// The FPS tile is coloured by the frame budget, so the headline number itself
	// carries the verdict; the rest stay neutral rather than competing with it.
	const ImVec4 fpsCol = cur.deltaMs <= budgetMs()          ? ImVec4(0.36f, 0.72f, 0.42f, 1.0f)
	                    : cur.deltaMs <= budgetMs() * 2.0    ? ImVec4(0.90f, 0.68f, 0.24f, 1.0f)
	                                                         : ImVec4(0.86f, 0.32f, 0.28f, 1.0f);
	ProfilerWidgets::StatTile("FPS", v0, s0, {}, fpsCol, tileW);
	ImGui::SameLine();
	ProfilerWidgets::StatTile("FRAME ms", v1, s1, bars, HE::Ed::Theme::TextHeading, tileW);
	ImGui::SameLine();
	ProfilerWidgets::StatTile("CPU ms", v2, s2, cpuSpark, HE::Ed::Theme::Accent, tileW);
	ImGui::SameLine();
	ProfilerWidgets::StatTile("GPU ms", v3, s3, gpuSpark, HE::Ed::Theme::AccentBright, tileW);

	// ── Budget ──────────────────────────────────────────────────────────────
	ImGui::Spacing();
	ImGui::SetNextItemWidth(120.0f);
	int fpsIdx = g_ui.targetFps >= 144.0 ? 3 : g_ui.targetFps >= 120.0 ? 2 : g_ui.targetFps >= 60.0 ? 1 : 0;
	if (ImGui::Combo("Target", &fpsIdx, "30 FPS\0" "60 FPS\0" "120 FPS\0" "144 FPS\0"))
		g_ui.targetFps = (fpsIdx == 0) ? 30.0 : (fpsIdx == 1) ? 60.0 : (fpsIdx == 2) ? 120.0 : 144.0;
	ImGui::SameLine();
	ImGui::TextDisabled("budget %.2f ms/frame", budgetMs());

	ProfilerWidgets::BudgetBar("CPU", cur.cpuFrameMs, budgetMs(), ImGui::GetContentRegionAvail().x);
	if (cur.gpuFrameMs >= 0.0)
		ProfilerWidgets::BudgetBar("GPU", cur.gpuFrameMs, budgetMs(), ImGui::GetContentRegionAvail().x);

	// ── Frame-time graph ────────────────────────────────────────────────────
	ImGui::Spacing();
	ImGui::TextColored(HE::Ed::Theme::TextHeading, "Frame time");
	int hovered = -1;
	ProfilerWidgets::FrameGraph("##ftgraph", bars, budgetMs(), pct.p95, pct.p99, 90.0f, &hovered);
	if (hovered >= 0 && hovered < static_cast<int>(live.size()))
	{
		const ProfLiveFrame& f = live[static_cast<size_t>(hovered)];
		ImGui::SetTooltip("frame %.2f ms (%.0f FPS)\nCPU %.2f ms\nGPU %s\ndraws %u · tris %u",
		                  f.deltaMs, f.deltaMs > 0.0 ? 1000.0 / f.deltaMs : 0.0, f.cpuFrameMs,
		                  f.gpuFrameMs >= 0.0 ? std::to_string(f.gpuFrameMs).c_str() : "n/a",
		                  f.draws, f.triangles);
	}

	if (anyGpu)
	{
		ImGui::Checkbox("GPU time graph", &g_ui.showGpuGraph);
		if (g_ui.showGpuGraph)
			ProfilerWidgets::FrameGraph("##gpugraph", gpuSpark, budgetMs(), 0.0, 0.0, 60.0f);
	}

	// ── Distribution ────────────────────────────────────────────────────────
	// The mean hides bimodality; this is the view that shows it.
	ImGui::Spacing();
	ImGui::TextColored(HE::Ed::Theme::TextHeading, "Frame-time distribution");
	ProfilerWidgets::HistogramView("##hist", HE::Prof::computeHistogram(deltas, 40), 70.0f);

	ImGui::Spacing();
	ImGui::TextColored(HE::Ed::Theme::TextHeading, "Counters");
	CountersGrid(cur.draws, cur.triangles, cur.visible, cur.total, prof.sceneCounters());
}

// ── Timeline ────────────────────────────────────────────────────────────────
void DrawTimeline(EngineProfiler& prof)
{
	refreshLanes(prof);
	const std::vector<ProfThreadTimeline>& lanes = g_cache.lanes;
	{
		EditorWidgets::WrapText wrap;
		ImGui::TextDisabled("Wheel = zoom, drag = pan. One lane per thread; rows are nesting depth. "
		                    "Gaps on a worker lane are idle cores.");
	}
	if (ImGui::Button("Fit")) g_ui.timelineView = {};   // invalid → refit to the capture
	ImGui::SameLine();
	const size_t dropped = prof.timelineDroppedSpans();
	if (dropped > 0)
	{
		// Truncation is stated, never silent: a capped lane looks exactly like an
		// idle one, and reading "this worker did nothing" off a full buffer is the
		// kind of wrong conclusion the whole panel exists to prevent.
		ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.2f, 1.0f),
		                   "%zu spans dropped (per-thread cap) — lanes are truncated", dropped);
	}
	else
	{
		size_t spans = 0;
		for (const ProfThreadTimeline& l : lanes) spans += l.spans.size();
		ImGui::TextDisabled("%zu lanes · %zu spans", lanes.size(), spans);
	}

	ProfilerWidgets::TimelineView("##timeline", lanes, prof.frameMarks(), g_cache.occupancy,
	                              g_ui.timelineView,
	                              std::max(160.0f, ImGui::GetContentRegionAvail().y - 4.0f));
}

// ── Scopes table ────────────────────────────────────────────────────────────
void DrawScopes(EngineProfiler& prof)
{
	const std::vector<ProfFrameRecord>& frames = analysisFrames(prof);
	if (frames.empty())
	{
		EditorWidgets::WrapText wrap;
		ImGui::TextDisabled("No capture yet. Run a benchmark capture, then this table ranks every "
		                    "CPU scope by the time spent in its OWN body.");
		return;
	}

	const std::vector<HE::Prof::ScopeAggregate>& agg = g_cache.aggregates;
	{
		EditorWidgets::WrapText wrap;
		ImGui::TextDisabled("%zu frames. Self = time in the scope's own body, excluding anything it "
		                    "called — the column that names the culprit rather than its call chain.",
		                    frames.size());
	}

	double maxSelf = 0.0;
	for (const HE::Prof::ScopeAggregate& a : agg) maxSelf = std::max(maxSelf, a.selfMs);

	constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
	                                   ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
	                                   ImGuiTableFlags_SizingStretchProp;
	if (!ImGui::BeginTable("##scopes", 7, kFlags)) return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch, 2.4f);
	ImGui::TableSetupColumn("Self ms", ImGuiTableColumnFlags_WidthStretch, 1.2f);
	ImGui::TableSetupColumn("Total ms", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthStretch, 0.8f);
	ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthStretch, 0.8f);
	ImGui::TableSetupColumn("p95", ImGuiTableColumnFlags_WidthStretch, 0.8f);
	ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthStretch, 0.8f);
	ImGui::TableHeadersRow();

	for (const HE::Prof::ScopeAggregate& a : agg)
	{
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		// Indent by the shallowest depth the scope was ever seen at, so the table
		// still hints at the tree without pretending to BE the tree (a scope called
		// from two places has one row here, on purpose).
		ImGui::Dummy(ImVec2(static_cast<float>(a.minDepth) * 10.0f, 0.0f));
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ProfilerWidgets::ScopeColor(a.name.c_str())),
		                   "\xE2\x96\xA0");
		ImGui::SameLine();
		ImGui::TextUnformatted(a.name.c_str());

		ImGui::TableNextColumn();
		// A bar behind the number: the ranking is readable at a glance instead of
		// by comparing decimals down a column.
		{
			const float frac = maxSelf > 0.0 ? static_cast<float>(a.selfMs / maxSelf) : 0.0f;
			char label[32];
			std::snprintf(label, sizeof(label), "%.3f", a.selfMs);
			ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), label);
		}
		ImGui::TableNextColumn(); ImGui::Text("%.3f", a.totalMs);
		ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(a.count));
		ImGui::TableNextColumn(); ImGui::Text("%.3f", a.avgMs);
		ImGui::TableNextColumn(); ImGui::Text("%.3f", a.p95Ms);
		ImGui::TableNextColumn(); ImGui::Text("%.3f", a.maxMs);
	}
	ImGui::EndTable();
}

// Full breakdown of one captured frame: counters, GPU passes, CPU scope tree.
void DrawFrameDetail(const ProfFrameRecord& f)
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
	CountersGrid(s.drawCalls, s.triangles, s.visibleObjects, s.totalObjects, s.scene);
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

	// ── CPU scopes (nested, with self time) ─────────────────────────────────
	if (!f.scopes.empty())
	{
		const double ref = f.cpuFrameMs > 0.0 ? f.cpuFrameMs : 1.0;
		ImGui::Separator();
		ImGui::TextUnformatted("CPU scopes");
		// Reverse of the stored order: scopes are recorded as they CLOSE, so the
		// raw list reads inside-out (a leaf before the function that called it).
		// Walking it backwards puts parents above their children, which is the
		// order a call tree is read in.
		for (size_t i = f.scopes.size(); i-- > 0;)
		{
			const ProfScopeSample& sc = f.scopes[i];
			std::string label(static_cast<size_t>(sc.depth) * 2, ' ');
			label += sc.name ? sc.name : "?";
			ImGui::Text("%s", label.c_str());
			ImGui::SameLine(210); ImGui::Text("%7.3f ms", sc.ms);
			ImGui::SameLine(300);
			ImGui::ProgressBar(static_cast<float>(sc.ms / ref), ImVec2(-1, 0), "");
		}
	}
}

// ── Hitch list (Capture tab) ────────────────────────────────────────────────
void DrawHitches(EngineProfiler& prof)
{
	if (analysisFrames(prof).empty()) return;
	const std::vector<HE::Prof::Hitch>& hitches = g_cache.hitches;

	ImGui::Separator();
	ImGui::TextColored(HE::Ed::Theme::TextHeading, "Hitches");
	if (hitches.empty())
	{
		ImGui::TextDisabled("None — no frame exceeded 2x the median frame time.");
		return;
	}
	{
		EditorWidgets::WrapText wrap;
		ImGui::TextDisabled("%zu frames over 2x the median, worst first. Click one to open it in "
		                    "Frame Detail.", hitches.size());
	}
	if (!ImGui::BeginTable("##hitches", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
	                                        ImGuiTableFlags_SizingStretchProp))
		return;
	ImGui::TableSetupColumn("Frame");
	ImGui::TableSetupColumn("ms");
	ImGui::TableSetupColumn("x median");
	ImGui::TableSetupColumn("Worst scope");
	ImGui::TableHeadersRow();
	for (const HE::Prof::Hitch& h : hitches)
	{
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		char id[32];
		std::snprintf(id, sizeof(id), "%llu##hitch", static_cast<unsigned long long>(h.frameIndex));
		if (ImGui::Selectable(id, false, ImGuiSelectableFlags_SpanAllColumns))
		{
			// Store the frame's own index, not its position: the ring drops the
			// oldest frames while a capture runs, so a position saved now points at
			// a different frame a second later. Frame Detail resolves it on draw.
			g_ui.selectedFrame     = h.frameIndex;
			g_ui.haveSelectedFrame = true;
		}
		ImGui::TableNextColumn(); ImGui::Text("%.2f", h.deltaMs);
		ImGui::TableNextColumn(); ImGui::Text("%.1fx", h.ratio);
		ImGui::TableNextColumn();
		if (h.worstScope && h.worstScope[0]) ImGui::Text("%s (%.2f ms)", h.worstScope, h.worstScopeMs);
		else                                 ImGui::TextDisabled("—");
	}
	ImGui::EndTable();
}
} // namespace
#endif // HE_IMGUI_ENABLED

void DrawProfilerWindow(AppContext& ctx, bool& open)
{
#ifdef HE_IMGUI_ENABLED
    EngineProfiler& prof = EngineProfiler::instance();
    if (!open) { prof.setLiveEnabled(false); return; }

    ImGui::SetNextWindowSize(ImVec2(620, 680), ImGuiCond_FirstUseEver);
    const bool visible = ImGui::Begin("Performance Profiler", &open);
    // Feed the live HUD only while the window is actually visible (not collapsed).
    prof.setLiveEnabled(visible);
    if (!visible) { ImGui::End(); return; }

    if (ImGui::BeginTabBar("##profTabs"))
    {
        if (ImGui::BeginTabItem("Overview"))  { DrawOverview(prof);  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Timeline"))  { DrawTimeline(prof);  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Scopes"))    { DrawScopes(prof);    ImGui::EndTabItem(); }

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

            bool timeline = prof.threadTimelineEnabled();
            if (ImGui::Checkbox("Per-thread timeline (worker lanes)", &timeline))
                prof.setThreadTimelineEnabled(timeline);
            ImGui::TextDisabled("Records every thread's scopes, not just the main one — this is what fills the "
                                "Timeline tab and shows whether the job pool is actually being fed. Costs memory "
                                "during a capture; the frame tree is unaffected either way.");

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

            DrawHitches(prof);
            ImGui::EndTabItem();
        }

        // ── Frame Detail: selected frame, else single-frame capture, else last ──
        if (ImGui::BeginTabItem("Frame Detail"))
        {
            const std::vector<ProfFrameRecord>& frames = analysisFrames(prof);
            const ProfFrameRecord* single = prof.singleFrame();
            const ProfFrameRecord* f      = nullptr;
            if (g_ui.haveSelectedFrame)
            {
                // Resolve by frame index, not position — see the hitch list.
                for (const ProfFrameRecord& r : frames)
                    if (r.index == g_ui.selectedFrame) { f = &r; break; }
                if (f)
                {
                    ImGui::TextDisabled("Source: frame %llu, picked from the hitch list",
                                        static_cast<unsigned long long>(g_ui.selectedFrame));
                    ImGui::SameLine();
                    if (ImGui::SmallButton("clear")) g_ui.haveSelectedFrame = false;
                }
                else
                {
                    // The ring dropped it out from under the selection while recording.
                    ImGui::TextDisabled("Frame %llu has scrolled out of the capture ring.",
                                        static_cast<unsigned long long>(g_ui.selectedFrame));
                    g_ui.haveSelectedFrame = false;
                }
            }
            if (f) { /* selection resolved above */ }
            else if (single) { f = single;              ImGui::TextDisabled("Source: single-frame capture"); }
            else if (!frames.empty()) { f = &frames.back(); ImGui::TextDisabled("Source: last benchmark frame"); }

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
