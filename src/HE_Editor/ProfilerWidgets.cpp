#include "ProfilerWidgets.h"

#ifdef HE_IMGUI_ENABLED

#include "EditorTheme.h"
#include <imgui_internal.h>   // ImGui::ItemAdd / ItemSize for custom-drawn items

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace L = HE::Prof::Layout;

namespace
{
// ── Palette ─────────────────────────────────────────────────────────────────
// The panel borrows the editor's warm theme rather than inventing a second one,
// with exactly one exception: the budget verdict. Green/amber/red is worth
// breaking the palette for, because it is the only colour in the panel that
// carries meaning instead of identity, and a warm-amber "everything is fine"
// against a warm-amber "you are over budget" says nothing.
constexpr ImVec4 kGood { 0.36f, 0.72f, 0.42f, 1.0f };
constexpr ImVec4 kWarn { 0.90f, 0.68f, 0.24f, 1.0f };
constexpr ImVec4 kBad  { 0.86f, 0.32f, 0.28f, 1.0f };

// Graph backgrounds and tile fills, derived through the theme's warm() so they
// shift with the rest of the editor instead of being two more hand-picked greys.
constexpr ImVec4 kTrack = HE::Ed::Theme::warm(0.10f);
constexpr ImVec4 kTile  = HE::Ed::Theme::warm(0.17f);

ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

ImVec4 mixv(const ImVec4& a, const ImVec4& b, float t)
{
	return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
	              a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

// Ratio → colour. Two stops rather than a smooth ramp all the way: a frame at 99%
// of budget is not "nearly bad", it is fine, and shading it orange trains people
// to ignore the colour.
ImVec4 budgetColor(double ratio)
{
	if (ratio <= 0.75) return kGood;
	if (ratio <= 1.0)  return mixv(kGood, kWarn, static_cast<float>((ratio - 0.75) / 0.25));
	if (ratio <= 2.0)  return mixv(kWarn, kBad,  static_cast<float>(ratio - 1.0));
	return kBad;
}

// HSV → RGB for the per-scope hues below.
ImVec4 hsv(float h, float s, float v)
{
	float r = 0, g = 0, b = 0;
	ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);
	return ImVec4(r, g, b, 1.0f);
}

void drawTextClipped(ImDrawList* dl, const L::Rect& r, const char* text, ImU32 col)
{
	// Only label a box that can actually hold a couple of characters; a one-pixel
	// sliver with a glyph bleeding out of it is noise, not information.
	if (r.width() < 22.0f) return;
	const ImVec2 clipMin(r.x0 + 3.0f, r.y0);
	const ImVec2 clipMax(r.x1 - 2.0f, r.y1);
	dl->PushClipRect(clipMin, clipMax, true);
	dl->AddText(ImVec2(r.x0 + 4.0f, r.y0 + 1.0f), col, text);
	dl->PopClipRect();
}

std::string fmtMs(double ms)
{
	char buf[32];
	if (ms >= 1.0)      std::snprintf(buf, sizeof(buf), "%.2f ms", ms);
	else if (ms >= 0.001) std::snprintf(buf, sizeof(buf), "%.0f \xC2\xB5s", ms * 1000.0);
	else                std::snprintf(buf, sizeof(buf), "%.0f ns", ms * 1e6);
	return buf;
}
} // namespace

namespace ProfilerWidgets
{

ImU32 ScopeColor(const char* name, float saturationScale)
{
	// Hue from the name hash, but saturation/value pinned to a narrow band: a free
	// hash over full HSV produces neon greens next to muddy browns and the lanes
	// stop reading as one picture. Golden-ratio spacing keeps neighbouring hashes
	// from landing on near-identical hues.
	const uint32_t h  = L::nameHash(name);
	const float    hue = std::fmod(static_cast<float>(h % 3600) / 3600.0f + 0.618034f, 1.0f);
	return u32(hsv(hue, 0.42f * saturationScale, 0.72f));
}

// ─── Stat tile ───────────────────────────────────────────────────────────────

void StatTile(const char* caption, const char* value, const char* sub,
              const std::vector<float>& spark, ImVec4 accent, float width)
{
	ImDrawList*  dl     = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float  lineH  = ImGui::GetTextLineHeight();
	const float  height = lineH * 3.4f + (spark.empty() ? 0.0f : 16.0f);

	const ImVec2 p0 = origin;
	const ImVec2 p1 = ImVec2(origin.x + width, origin.y + height);
	dl->AddRectFilled(p0, p1, u32(HE::Ed::Theme::alpha(kTile, 0.55f)), 4.0f);
	dl->AddRect(p0, p1, u32(HE::Ed::Theme::alpha(accent, 0.30f)), 4.0f);

	dl->AddText(ImVec2(p0.x + 8.0f, p0.y + 4.0f),
	            u32(HE::Ed::Theme::TextDim), caption);

	// The value is the point of the tile, so it gets the accent and the space;
	// everything else on the tile is deliberately quiet.
	ImGui::PushFont(nullptr);
	dl->AddText(ImVec2(p0.x + 8.0f, p0.y + 4.0f + lineH * 1.0f), u32(accent), value);
	ImGui::PopFont();

	if (sub && sub[0])
		dl->AddText(ImVec2(p0.x + 8.0f, p0.y + 4.0f + lineH * 2.1f),
		            u32(HE::Ed::Theme::TextDim), sub);

	if (!spark.empty())
	{
		float mn = spark[0], mx = spark[0];
		for (float v : spark) { mn = std::min(mn, v); mx = std::max(mx, v); }
		const float rng = (mx - mn) > 1e-6f ? (mx - mn) : 1.0f;
		const float y0  = p1.y - 15.0f, y1 = p1.y - 3.0f;
		const float x0  = p0.x + 6.0f,  x1 = p1.x - 6.0f;
		ImVec2 prev;
		for (size_t i = 0; i < spark.size(); ++i)
		{
			const float t = spark.size() > 1
			              ? static_cast<float>(i) / static_cast<float>(spark.size() - 1) : 0.0f;
			const ImVec2 pt(x0 + (x1 - x0) * t, y1 - (spark[i] - mn) / rng * (y1 - y0));
			if (i > 0) dl->AddLine(prev, pt, u32(HE::Ed::Theme::alpha(accent, 0.65f)), 1.0f);
			prev = pt;
		}
	}

	ImGui::Dummy(ImVec2(width, height));
}

// ─── Budget bar ──────────────────────────────────────────────────────────────

void BudgetBar(const char* label, double valueMs, double budgetMs, float width)
{
	ImDrawList*  dl     = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float  h      = ImGui::GetTextLineHeight() + 4.0f;

	const double ratio = budgetMs > 0.0 ? valueMs / budgetMs : 0.0;
	const ImVec4 col   = budgetColor(ratio);

	const ImVec2 p0(origin.x, origin.y), p1(origin.x + width, origin.y + h);
	dl->AddRectFilled(p0, p1, u32(HE::Ed::Theme::alpha(kTrack, 0.8f)), 3.0f);

	// Bar is clamped at the track width, but the NUMBER is not: a 3x-over frame
	// must not look identical to a 1.01x-over one just because both bars are full.
	const float fill = static_cast<float>(std::clamp(ratio, 0.0, 1.0)) * width;
	if (fill > 1.0f)
		dl->AddRectFilled(p0, ImVec2(p0.x + fill, p1.y), u32(HE::Ed::Theme::alpha(col, 0.75f)), 3.0f);
	dl->AddRect(p0, p1, u32(HE::Ed::Theme::alpha(col, 0.55f)), 3.0f);

	char text[96];
	std::snprintf(text, sizeof(text), "%s  %.2f / %.2f ms  (%.0f%%)",
	              label, valueMs, budgetMs, ratio * 100.0);
	dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 2.0f), u32(HE::Ed::Theme::Text), text);

	ImGui::Dummy(ImVec2(width, h));
}

// ─── Frame graph ─────────────────────────────────────────────────────────────

int FrameGraph(const char* id, const std::vector<float>& values, double budgetMs,
               double p95, double p99, float height, int* hoverOut, int selected)
{
	if (hoverOut) *hoverOut = -1;
	const float width = ImGui::GetContentRegionAvail().x;
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(id, ImVec2(std::max(width, 1.0f), height));
	const bool hovered = ImGui::IsItemHovered();
	const bool clicked = ImGui::IsItemClicked();

	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 p0 = origin;
	const ImVec2 p1 = ImVec2(origin.x + width, origin.y + height);
	dl->AddRectFilled(p0, p1, u32(HE::Ed::Theme::alpha(kTrack, 0.75f)), 3.0f);
	if (values.empty())
	{
		dl->AddText(ImVec2(p0.x + 8.0f, p0.y + height * 0.5f - 7.0f),
		            u32(HE::Ed::Theme::TextDim), "no frames yet");
		return -1;
	}

	// Y scale: the tallest bar, but never less than 1.4x budget, so a capture that
	// comfortably makes budget still SHOWS the budget line instead of pushing it
	// off the top and looking like everything is at the limit.
	float mx = 0.0f;
	for (float v : values) mx = std::max(mx, v);
	const float top = std::max(mx * 1.08f, static_cast<float>(budgetMs) * 1.4f);
	auto yOf = [&](double v) {
		return p1.y - static_cast<float>(std::clamp(v / top, 0.0, 1.0)) * height;
	};

	const int   n  = static_cast<int>(values.size());
	const float bw = width / static_cast<float>(n);
	for (int i = 0; i < n; ++i)
	{
		const float x0 = p0.x + bw * static_cast<float>(i);
		const float x1 = x0 + std::max(bw - (bw > 3.0f ? 1.0f : 0.0f), 1.0f);
		const ImVec4 c = budgetColor(budgetMs > 0.0 ? values[i] / budgetMs : 0.0);
		dl->AddRectFilled(ImVec2(x0, yOf(values[i])), ImVec2(x1, p1.y),
		                  u32(HE::Ed::Theme::alpha(c, i == selected ? 1.0f : 0.85f)));
	}

	// Guide lines last so they sit on top of the bars they qualify.
	auto guide = [&](double v, ImVec4 col, const char* label) {
		if (!(v > 0.0) || v > top) return;
		const float y = yOf(v);
		dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), u32(HE::Ed::Theme::alpha(col, 0.55f)), 1.0f);
		char buf[48];
		std::snprintf(buf, sizeof(buf), "%s %.1f", label, v);
		dl->AddText(ImVec2(p1.x - 74.0f, y - ImGui::GetTextLineHeight() - 1.0f),
		            u32(HE::Ed::Theme::alpha(col, 0.85f)), buf);
	};
	guide(budgetMs, HE::Ed::Theme::TextHeading, "budget");
	guide(p95, kWarn, "p95");
	guide(p99, kBad,  "p99");

	if (selected >= 0 && selected < n)
	{
		const float x = p0.x + bw * (static_cast<float>(selected) + 0.5f);
		dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y),
		            u32(HE::Ed::Theme::AccentBright), 1.0f);
	}

	int result = -1;
	if (hovered)
	{
		const int idx = L::barIndexAt(ImGui::GetIO().MousePos.x, p0.x, p1.x, n);
		if (idx >= 0)
		{
			if (hoverOut) *hoverOut = idx;
			const float x = p0.x + bw * (static_cast<float>(idx) + 0.5f);
			dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y),
			            u32(HE::Ed::Theme::alpha(HE::Ed::Theme::Text, 0.35f)), 1.0f);
			if (clicked) result = idx;
		}
	}
	return result;
}

// ─── Histogram ───────────────────────────────────────────────────────────────

void HistogramView(const char* id, const HE::Prof::Histogram& h, float height)
{
	const float  width  = ImGui::GetContentRegionAvail().x;
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(id, ImVec2(std::max(width, 1.0f), height));
	const bool hovered = ImGui::IsItemHovered();

	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 p0 = origin, p1 = ImVec2(origin.x + width, origin.y + height);
	dl->AddRectFilled(p0, p1, u32(HE::Ed::Theme::alpha(kTrack, 0.75f)), 3.0f);
	if (h.bins.empty() || h.maxCount == 0)
	{
		dl->AddText(ImVec2(p0.x + 8.0f, p0.y + height * 0.5f - 7.0f),
		            u32(HE::Ed::Theme::TextDim), "no distribution yet");
		return;
	}

	const int   n  = static_cast<int>(h.bins.size());
	const float bw = width / static_cast<float>(n);
	for (int i = 0; i < n; ++i)
	{
		const float frac = static_cast<float>(h.bins[i]) / static_cast<float>(h.maxCount);
		const float y0   = p1.y - frac * (height - 2.0f);
		dl->AddRectFilled(ImVec2(p0.x + bw * i, y0),
		                  ImVec2(p0.x + bw * (i + 1) - (bw > 3.0f ? 1.0f : 0.0f), p1.y),
		                  u32(HE::Ed::Theme::alpha(HE::Ed::Theme::Accent, 0.70f)));
	}

	char lo[32], hi[32];
	std::snprintf(lo, sizeof(lo), "%.2f ms", h.lo);
	std::snprintf(hi, sizeof(hi), "%.2f ms", h.hi);
	const float lineH = ImGui::GetTextLineHeight();
	dl->AddText(ImVec2(p0.x + 4.0f, p1.y - lineH - 2.0f), u32(HE::Ed::Theme::TextDim), lo);
	dl->AddText(ImVec2(p1.x - ImGui::CalcTextSize(hi).x - 4.0f, p1.y - lineH - 2.0f),
	            u32(HE::Ed::Theme::TextDim), hi);

	if (hovered)
	{
		const int idx = L::barIndexAt(ImGui::GetIO().MousePos.x, p0.x, p1.x, n);
		if (idx >= 0)
			ImGui::SetTooltip("%.2f – %.2f ms\n%u frames",
			                  h.binStart(static_cast<size_t>(idx)),
			                  h.binStart(static_cast<size_t>(idx)) + h.binWidth,
			                  h.bins[static_cast<size_t>(idx)]);
	}
}

// ─── Timeline ────────────────────────────────────────────────────────────────

void TimelineView(const char* id,
                  const std::vector<ProfThreadTimeline>&      lanes,
                  const std::vector<ProfFrameMark>&           marks,
                  const std::vector<HE::Prof::LaneOccupancy>& occupancy,
                  L::TimeView& view, float height)
{
	const float  width  = ImGui::GetContentRegionAvail().x;
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(id, ImVec2(std::max(width, 1.0f), height));
	const bool hovered = ImGui::IsItemHovered();

	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 p0 = origin, p1 = ImVec2(origin.x + width, origin.y + height);
	dl->AddRectFilled(p0, p1, u32(HE::Ed::Theme::alpha(kTrack, 0.85f)), 3.0f);
	dl->PushClipRect(p0, p1, true);

	if (lanes.empty())
	{
		dl->AddText(ImVec2(p0.x + 8.0f, p0.y + height * 0.5f - 7.0f),
		            u32(HE::Ed::Theme::TextDim),
		            "No timeline. Run a capture with 'Per-thread timeline' enabled.");
		dl->PopClipRect();
		return;
	}

	// Full extent of the capture, so the view can be clamped to it and a reset
	// button has something to reset to.
	uint64_t lo = UINT64_MAX, hi = 0;
	for (const ProfThreadTimeline& lane : lanes)
		for (const ProfThreadSpan& s : lane.spans) { lo = std::min(lo, s.startNs); hi = std::max(hi, s.endNs); }
	if (lo == UINT64_MAX) { lo = 0; hi = 1; }
	if (!view.valid()) view = { static_cast<double>(lo), static_cast<double>(hi) };

	// ── Camera ──────────────────────────────────────────────────────────────
	constexpr float kLabelW = 108.0f;   // gutter holding the lane name + utilisation
	const float     tx0     = p0.x + kLabelW;
	const float     tx1     = p1.x - 4.0f;
	if (hovered)
	{
		const float wheel = ImGui::GetIO().MouseWheel;
		if (wheel != 0.0f)
			view = L::zoomAt(view, ImGui::GetIO().MousePos.x, tx0, tx1,
			                 wheel > 0.0f ? 0.80 : 1.25);
	}
	if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
	{
		view = L::panByPixels(view, ImGui::GetIO().MouseDelta.x, tx0, tx1);
		ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
	}
	view = L::clampView(view, lo, hi);

	const float rowH  = ImGui::GetTextLineHeight() + 2.0f;
	const float lineH = ImGui::GetTextLineHeight();

	// ── Frame separators, behind the spans ──────────────────────────────────
	// Drawn first and dim: they are the ruler, not the content. Skipped entirely
	// when zoomed far out, where 4000 frame lines would paint the whole lane grey
	// and hide the very spans they are meant to locate.
	if (!marks.empty())
	{
		const double nsPerPx = view.span() / static_cast<double>(tx1 - tx0);
		size_t drawn = 0;
		for (const ProfFrameMark& m : marks)
		{
			if (static_cast<double>(m.startNs) < view.startNs ||
			    static_cast<double>(m.startNs) > view.endNs) continue;
			if (++drawn > 200) break;
			const float x = tx0 + static_cast<float>((static_cast<double>(m.startNs) - view.startNs) / nsPerPx);
			dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y),
			            u32(HE::Ed::Theme::alpha(HE::Ed::Theme::TextDim, 0.30f)), 1.0f);
		}
	}

	// ── Lanes ───────────────────────────────────────────────────────────────
	const ProfThreadSpan* hoverSpan  = nullptr;
	const ProfThreadTimeline* hoverLane = nullptr;
	const ImVec2 mouse = ImGui::GetIO().MousePos;

	float y = p0.y + 4.0f;
	for (size_t li = 0; li < lanes.size(); ++li)
	{
		const ProfThreadTimeline& lane = lanes[li];
		uint32_t maxDepth = 0;
		for (const ProfThreadSpan& s : lane.spans) maxDepth = std::max(maxDepth, s.depth);
		const float laneH = rowH * static_cast<float>(maxDepth + 1);
		if (y > p1.y) break;

		dl->AddText(ImVec2(p0.x + 6.0f, y), u32(lane.isMain ? HE::Ed::Theme::TextHeading
		                                                    : HE::Ed::Theme::Text),
		            lane.label.c_str());
		// Utilisation in the gutter turns eight lanes of rectangles into a verdict:
		// a worker at 3% is an unfed thread pool, not a busy one.
		for (const HE::Prof::LaneOccupancy& o : occupancy)
			if (o.label == lane.label)
			{
				char buf[32];
				std::snprintf(buf, sizeof(buf), "%.0f%% busy", o.utilisation * 100.0);
				dl->AddText(ImVec2(p0.x + 6.0f, y + lineH), u32(HE::Ed::Theme::TextDim), buf);
				break;
			}

		for (const ProfThreadSpan& s : lane.spans)
		{
			L::Rect r;
			if (!L::spanRect(s.startNs, s.endNs, s.depth, view, tx0, tx1, y, rowH - 1.0f, 2.0f, r))
				continue;
			if (r.y1 > p1.y) break;
			const ImU32 col = ScopeColor(s.name);
			dl->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), col, 2.0f);
			drawTextClipped(dl, r, s.name, u32(HE::Ed::Theme::alpha(HE::Ed::Theme::Text, 0.95f)));
			if (hovered && mouse.x >= r.x0 && mouse.x <= r.x1 && mouse.y >= r.y0 && mouse.y <= r.y1)
			{ hoverSpan = &s; hoverLane = &lane; }
		}

		y += laneH + 6.0f;
		dl->AddLine(ImVec2(p0.x + 4.0f, y - 3.0f), ImVec2(p1.x - 4.0f, y - 3.0f),
		            u32(HE::Ed::Theme::alpha(HE::Ed::Theme::TextDim, 0.20f)), 1.0f);
	}

	// Gutter separator drawn last so span rectangles never bleed into the labels.
	dl->AddLine(ImVec2(tx0 - 4.0f, p0.y), ImVec2(tx0 - 4.0f, p1.y),
	            u32(HE::Ed::Theme::alpha(HE::Ed::Theme::TextDim, 0.35f)), 1.0f);
	dl->PopClipRect();

	if (hoverSpan && hoverLane)
		ImGui::SetTooltip("%s\n%s\ndepth %u  ·  %s\nstart +%.3f ms",
		                  hoverSpan->name, hoverLane->label.c_str(), hoverSpan->depth,
		                  fmtMs(static_cast<double>(hoverSpan->endNs - hoverSpan->startNs) * 1e-6).c_str(),
		                  static_cast<double>(hoverSpan->startNs) * 1e-6);
}

// ─── Sparkline ───────────────────────────────────────────────────────────────

void Sparkline(const char* id, const std::vector<float>& values, ImVec2 size, ImVec4 col)
{
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(id, size);
	if (values.size() < 2) return;

	float mn = values[0], mx = values[0];
	for (float v : values) { mn = std::min(mn, v); mx = std::max(mx, v); }
	const float rng = (mx - mn) > 1e-9f ? (mx - mn) : 1.0f;

	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 prev;
	for (size_t i = 0; i < values.size(); ++i)
	{
		const float t = static_cast<float>(i) / static_cast<float>(values.size() - 1);
		const ImVec2 pt(origin.x + size.x * t,
		                origin.y + size.y - (values[i] - mn) / rng * size.y);
		if (i > 0) dl->AddLine(prev, pt, u32(col), 1.0f);
		prev = pt;
	}
}

} // namespace ProfilerWidgets

#endif // HE_IMGUI_ENABLED
