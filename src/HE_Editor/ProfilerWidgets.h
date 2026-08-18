#pragma once
// ─── ProfilerWidgets ─────────────────────────────────────────────────────────
// Hand-drawn ImDrawList views for the profiler panel: stat tiles, sparklines,
// budget bars, the frame-time graph, the histogram and the multi-lane flame
// timeline.
//
// Hand-drawn rather than a charting dependency on purpose: ImPlot is not vendored
// (EditorDeps carries content, fonts and images — no third-party UI libraries),
// and pulling one in for five views would be the largest dependency in the editor
// for the smallest panel. Everything here is ImDrawList primitives, the same way
// the graph editor and the toolbar already draw themselves.
//
// The geometry lives next door in ProfilerLayout.h and is unit-tested; this file
// is intentionally only colours and draw calls.

#include <cstdint>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>

#include "ProfilerLayout.h"
#include <Diagnostics/EngineProfiler.h>
#include <Diagnostics/ProfilerStats.h>

namespace ProfilerWidgets
{

// A big number with a caption and an optional sparkline underneath — the readout
// a person glances at, as opposed to the tables they read. `accent` tints the
// value; pass a budget-derived colour to make the tile itself carry the verdict.
void StatTile(const char* caption, const char* value, const char* sub,
              const std::vector<float>& spark, ImVec4 accent, float width);

// value/budget as a filled bar with the budget line marked. Colour crosses from
// calm to alarming at the budget, because "is this inside the frame budget" is a
// different question from "how many milliseconds" and deserves a different shape.
void BudgetBar(const char* label, double valueMs, double budgetMs, float width);

// Per-frame bars over a value series, coloured against `budgetMs`, with optional
// p95/p99 guide lines. Returns the index the user clicked, or -1.
// `hoverOut` receives the hovered index (-1 when none) so the caller can show its
// own tooltip with data this widget does not know about.
int FrameGraph(const char* id, const std::vector<float>& values, double budgetMs,
               double p95, double p99, float height, int* hoverOut = nullptr,
               int selected = -1);

// Distribution of the same series. A bimodal 8/22 ms capture has a perfectly
// reasonable 11 ms mean and feels like 22 ms; only this view shows that.
void HistogramView(const char* id, const HE::Prof::Histogram& h, float height);

// Multi-lane flame timeline: one lane per thread, one row per nesting depth,
// frame boundaries drawn across all lanes. Zoom with the wheel, pan by dragging.
// `view` is in/out — the caller owns the camera so it survives tab switches.
void TimelineView(const char* id,
                  const std::vector<ProfThreadTimeline>& lanes,
                  const std::vector<ProfFrameMark>&      marks,
                  const std::vector<HE::Prof::LaneOccupancy>& occupancy,
                  HE::Prof::Layout::TimeView& view, float height);

// Inline mini-graph for a table row. No axes, no labels: it is there to show the
// SHAPE (steady, spiky, ramping) next to a number that cannot show shape.
void Sparkline(const char* id, const std::vector<float>& values, ImVec2 size, ImVec4 col);

// Stable colour for a scope name (see Layout::nameHash) — the same scope keeps
// its hue across frames, lanes and sessions, which is what makes a flame graph
// readable while it updates.
ImU32 ScopeColor(const char* name, float saturationScale = 1.0f);

} // namespace ProfilerWidgets

#endif // HE_IMGUI_ENABLED
