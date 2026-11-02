#include "Diagnostics/DevStatsTile.h"
#include "Platform/BuildProfile.h"

// The game's F10 card is its only user, and F10 exists only where the dev
// hotkeys do: a release build of HorizonCore carries none of this file.
#if HE_FEATURE_DEV_HOTKEYS

#include "ContentManager/ContentManager.h"
#include "Diagnostics/Log.h"
#include "Diagnostics/EngineProfiler.h"
#include "UIWidget/UIWidgetTree.h"
#include "UIWidget/WidgetManager.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace HE
{

namespace
{
	std::string thousands(std::uint32_t v)
	{
		std::string s = std::to_string(v);
		for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
		return s;
	}

	constexpr const char* kAssetPath = "mem://DevStatsTile.hasset";
	constexpr float kTileWidth = 330.0f;
	constexpr float kPad       = 10.0f;
	constexpr float kFontSize  = 13.0f;   // 13, not 15: with the CPU block the card must still fit 1080p
	constexpr float kLineH     = kFontSize * 1.25f;   // what the card is sized from
}

std::vector<std::string> formatDevStats(const DevStatsInput& in, bool extended, std::size_t maxPasses,
                                        std::size_t maxScopes)
{
	std::vector<std::string> lines;
	char buf[128];
	// A row is "label<TAB>value": the card draws the labels in one column and the values
	// right-aligned in another, which a proportional font cannot do with padding spaces.
	auto row = [&](const std::string& label, const std::string& value) {
		lines.push_back(label + '\t' + value);
	};

	std::snprintf(buf, sizeof(buf), "%.0f FPS", in.fps);
	const std::string fpsLabel = buf;
	std::snprintf(buf, sizeof(buf), "%.2f ms", in.frameMs);
	row(fpsLabel, buf);
	if (in.gpu.gpuFrameMs >= 0.0)
	{
		std::snprintf(buf, sizeof(buf), "%.2f ms", in.gpu.gpuFrameMs);
		row("GPU", buf);
	}
	row("Draws", thousands(in.gpu.drawCalls));
	row("Triangles", thousands(in.gpu.triangles));
	row("Objects", thousands(in.gpu.visibleObjects) + " / " + thousands(in.gpu.totalObjects));
	if (in.gpu.occlusionCulled > 0)
		row("Occluded", thousands(in.gpu.occlusionCulled));
	if (in.gpu.vramBudgetMB > 0.0)
	{
		std::snprintf(buf, sizeof(buf), "%.0f / %.0f MB", in.gpu.vramUsedMB, in.gpu.vramBudgetMB);
		row("VRAM", buf);
	}
	if (!extended) return lines;

	// ── The extended block ────────────────────────────────────────────────────
	if (!in.backend.empty() || (in.width > 0 && in.height > 0))
	{
		std::string value = in.backend.empty() ? std::string("?") : in.backend;
		if (in.width > 0 && in.height > 0)
			value += "  " + std::to_string(in.width) + "x" + std::to_string(in.height);
		row("Renderer", value);
	}
	if (in.haveCapture && in.captureCpuMs >= 0.0)
	{
		std::snprintf(buf, sizeof(buf), "%.2f ms", in.captureCpuMs);
		row("CPU", buf);
	}
	if (in.frameP50 >= 0.0)
	{
		std::snprintf(buf, sizeof(buf), "%.1f / %.1f / %.1f ms", in.frameP50, in.frameP99, in.frameMax);
		row("Frame p50 / p99 / max", buf);
	}

	// Where the CPU's time goes in the captured frame: the scopes as recorded, a parent
	// before what it contains, so read it as a breakdown and not a sum.
	if (!in.captureScopes.empty())
	{
		std::vector<std::pair<std::string, double>> scopes = in.captureScopes;
		std::stable_sort(scopes.begin(), scopes.end(),
		                 [](const auto& a, const auto& b) { return a.second > b.second; });
		lines.emplace_back();
		lines.emplace_back("CPU scopes");
		const std::size_t shownScopes = std::min(scopes.size(), maxScopes);
		for (std::size_t i = 0; i < shownScopes; ++i)
		{
			std::snprintf(buf, sizeof(buf), "%.2f ms", scopes[i].second);
			row(" " + scopes[i].first.substr(0, 26), buf);
		}
	}

	// Where the GPU's time goes. From the detailed capture when there is one, else
	// whatever passes the renderer reported for the last frame (usually none: the
	// backends only split a frame while the profiler records).
	// A capture without passes takes the renderer's own last list: a backend whose detailed
	// timings come back asynchronously reports them a frame after the captured one.
	const bool fromCapture = in.haveCapture && !in.capturePasses.empty();
	const std::vector<IRenderer::GpuPassTime>& passes = fromCapture ? in.capturePasses : in.gpu.passes;
	const char* mode = fromCapture ? in.captureMode : in.gpu.gpuTimingMode;

	lines.emplace_back();   // a gap before the block
	if (passes.empty())
	{
		lines.push_back(in.haveCapture ? "GPU passes: none reported by this backend"
		                               : "GPU passes: measuring\xE2\x80\xA6");
		return lines;
	}
	std::string head = "GPU passes";
	if (mode && *mode) head += std::string("  (") + mode + ")";
	lines.push_back(std::move(head));

	// A pass that took no measurable time (GI while it is off) is left out, not listed at 0.00.
	std::vector<IRenderer::GpuPassTime> sorted;
	for (const IRenderer::GpuPassTime& p : passes)
		if (p.ms >= 0.005) sorted.push_back(p);
	std::stable_sort(sorted.begin(), sorted.end(),
	                 [](const IRenderer::GpuPassTime& a, const IRenderer::GpuPassTime& b) { return a.ms > b.ms; });
	const std::size_t shown = std::min(sorted.size(), maxPasses);
	for (std::size_t i = 0; i < shown; ++i)
	{
		// "~" marks an approximate interval (a split inside one encoder), the same
		// rule the profiler window follows: never read as an authoritative cost.
		std::snprintf(buf, sizeof(buf), "%.2f ms", sorted[i].ms);
		row(std::string(sorted[i].approx ? "~" : " ") + std::string(sorted[i].name ? sorted[i].name : "").substr(0, 26), buf);
	}
	if (sorted.size() > shown)
		lines.push_back("  + " + std::to_string(sorted.size() - shown) + " smaller passes");
	return lines;
}

bool DevStatsTile::toggle(WidgetManager& widgets, ContentManager& content)
{
	// A widget that was destroyed under us (a window closed, a manager cleared) is
	// built again instead of being shown by an id that now means nothing.
	if (m_widget != 0 && !widgets.isAlive(m_widget))
	{
		HE_LOG_WARN(Core, "%s", "DevStatsTile: its widget is gone; building it again");
		m_widget = m_panel = m_text = m_values = 0;
		m_visible = false;
	}
	if (!m_visible && m_widget == 0)
	{
		// ── Built once, in code ──────────────────────────────────────────────
		UIWidgetTree page;
		page.canvasWidth  = 1920.0f;
		page.canvasHeight = 1080.0f;
		// Pixels, not a share of the window: a debug card whose letters change size
		// with the resolution would be harder to read exactly when resolution matters.
		page.scaleMode = UICanvasScaleMode::ConstantPixel;

		const int card = page.add(UIWidgetType::Panel);
		{
			UIElement& e = *page.find(card);
			e.name = "StatsCard";
			uiSetAnchorPreset(e, 2);              // the top-right point of the screen…
			e.pivotX = 1.0f; e.pivotY = 0.0f;     // …that the card's own top-right corner sits on
			e.posX = -12.0f; e.posY = 12.0f;
			e.sizeX = kTileWidth; e.sizeY = 40.0f;
			e.cornerRadius = glm::vec4(6.0f);
			e.setProp("Color", UIPropValue::ofColor({ 0.05f, 0.06f, 0.08f, 0.78f }));
			e.hitTestable = false;                // a debug card must never eat a click meant for the game
		}
		const int text = page.add(UIWidgetType::Text);
		{
			UIElement& e = *page.find(text);
			e.name = "StatsText";
			uiSetAnchorPreset(e, 0);
			e.pivotX = 0.0f; e.pivotY = 0.0f;
			e.posX = kPad; e.posY = kPad;
			e.hitTestable = false;
			e.setProp("Text", UIPropValue::ofString(""));
			e.setProp("FontSize", UIPropValue::ofFloat(kFontSize));
			e.setProp("Color", UIPropValue::ofColor({ 0.88f, 0.92f, 0.98f, 1.0f }));
		}
		page.find(text)->parentId = card;   // the labels live on the card
		// The values: the same box, right-aligned, so the numbers line up at the card's
		// right edge whatever the labels' widths.
		const int values = page.add(UIWidgetType::Text);
		{
			UIElement& e = *page.find(values);
			e.name = "StatsValues";
			uiSetAnchorPreset(e, 0);
			e.pivotX = 0.0f; e.pivotY = 0.0f;
			e.posX = kPad; e.posY = kPad;
			e.sizeX = kTileWidth - 2.0f * kPad; e.sizeY = 600.0f;
			e.hitTestable = false;
			e.setProp("Text", UIPropValue::ofString(""));
			e.setProp("FontSize", UIPropValue::ofFloat(kFontSize));
			e.setProp("Color", UIPropValue::ofColor({ 0.88f, 0.92f, 0.98f, 1.0f }));
			e.setProp("AutoSize", UIPropValue::ofBool(false));
			e.setProp("WordWrap", UIPropValue::ofBool(false));
			e.setProp("Align H", UIPropValue::ofInt(2));
			e.setProp("Align V", UIPropValue::ofInt(0));   // top: the box is taller than the text, and a default of "middle" pushed the numbers down
		}
		page.find(values)->parentId = card;

		UIWidgetAsset asset;
		asset.treeJson = uiWidgetTreeToJson(page);
		asset.path     = kAssetPath;
		content.registerWidget(std::move(asset));

		m_widget = widgets.createWidget(content, kAssetPath);
		m_panel  = card;
		m_text   = text;
		m_values = values;
		if (m_widget == 0)
		{
			// createWidget logs why (asset not found / invalid tree) just before this.
			HE_LOG_ERROR(Core, "%s", "DevStatsTile: the stats widget could not be created");
			return false;                          // stays hidden
		}
		widgets.setZOrder(m_widget, 10000);        // over the game's own UI
		HE_LOG_INFO(Core, "DevStatsTile: widget %d built", m_widget);
	}
	setVisible(widgets, !m_visible);
	return m_visible;
}

void DevStatsTile::setVisible(WidgetManager& widgets, bool on)
{
	m_visible = on;
	if (m_widget != 0)
	{
		if (on) widgets.showWidget(m_widget);
		else    widgets.hideWidget(m_widget);
	}
	// Live mode is what makes the backends time the GPU at all; off again as soon as
	// the tile is gone so a hidden tile costs nothing.
	EngineProfiler::instance().setLiveEnabled(on);
	if (!on) EngineProfiler::instance().cancelSingleFrameCapture();
	m_primed = false;
	m_loggedFirst = false;
	m_sinceRefresh = m_sinceCapture = 0.0f;
	m_frames = 0;
	m_elapsed = 0.0f;
	m_history.clear();   // a new showing measures its own frames
	m_historyNext = 0;
	m_skipHistory = 0;
	m_requested = m_lastDetailed = false;
	m_cpuMs = -1.0;
	m_scopes.clear();
	m_passes.clear();
	m_passMode = "";
}

void DevStatsTile::update(WidgetManager& widgets, IRenderer* renderer, float dt,
                          const std::string& backendName, int w, int h)
{
	if (!m_visible || m_widget == 0) return;

	// The detailed capture's own frame is slow by design (it waits for the GPU after every
	// pass): its delta, which arrives with the next one or two frames, stays out of the spread.
	if (m_skipHistory > 0) --m_skipHistory;
	else
	{
		if (m_history.size() < kHistoryFrames) m_history.push_back(dt * 1000.0f);
		else m_history[m_historyNext] = dt * 1000.0f;
		m_historyNext = (m_historyNext + 1) % kHistoryFrames;
	}

	m_sinceRefresh += dt;
	m_sinceCapture += dt;
	m_elapsed      += dt;
	++m_frames;

	EngineProfiler& prof = EngineProfiler::instance();
	if (m_sinceCapture >= kCaptureSeconds || !m_primed)
	{
		m_sinceCapture = 0.0f;
		// Two kinds of capture, taken in turn, each kept until the next of its kind: a plain
		// frame for the CPU numbers, and a detailed one for the GPU passes. A detailed frame
		// waits for the GPU after every pass, so its own CPU time and scopes are not the game's.
		if (const ProfFrameRecord* f = prof.singleFrame(); f && m_requested)
		{
			// A detailed frame's own record holds only the passes that completed before it
			// ended; its full list comes from the renderer a frame or two later (below).
			if (!m_lastDetailed)
			{
				m_cpuMs = f->cpuFrameMs;
				m_scopes.clear();
				// Top level and one below, summed by name (a scope entered per pass counts once).
				for (const ProfScopeSample& sc : f->scopes)
				{
					if (sc.depth > 1 || !sc.name) continue;
					// "Metal::EncodeScene" reads as "EncodeScene": the backend is on the card already.
					// Only a backend's prefix goes; "RenderExtractor::extract" keeps its class.
					std::string name = sc.name;
					for (const char* backend : { "Metal::", "GL::", "OpenGL::", "Vulkan::", "VK::", "D3D11::", "D3D12::" })
						if (name.rfind(backend, 0) == 0) { name.erase(0, std::strlen(backend)); break; }
					auto it = std::find_if(m_scopes.begin(), m_scopes.end(),
					                       [&](const auto& e) { return e.first == name; });
					if (it == m_scopes.end()) m_scopes.emplace_back(std::move(name), sc.ms);
					else it->second += sc.ms;
				}
			}
		}
		m_lastDetailed = m_requested ? !m_lastDetailed : false;
		m_requested    = true;
		prof.requestSingleFrameCapture(m_lastDetailed);   // applied at the next frame start
		if (m_lastDetailed) { m_skipHistory = 3; m_passesThisRequest = false; }
	}
	if (m_primed && m_sinceRefresh < kRefreshSeconds) return;
	m_primed = true;

	DevStatsInput in;
	in.fps     = m_elapsed > 0.0f ? static_cast<double>(m_frames) / m_elapsed : 0.0;
	in.frameMs = in.fps > 0.0 ? 1000.0 / in.fps : 0.0;
	in.backend = backendName;
	in.width   = w;
	in.height  = h;
	if (renderer) in.gpu = renderer->GetFrameGpuStats();
	// A backend whose detailed timings come back asynchronously reports them in the frames
	// after the captured one: keep the last detailed list the renderer itself reports.
	// The passes complete one by one, so a list read early can be partial: per detailed
	// request the fullest one wins.
	if (in.gpu.gpuTimingMode && std::string(in.gpu.gpuTimingMode) == "detailed" && !in.gpu.passes.empty()
	    && (!m_passesThisRequest || in.gpu.passes.size() >= m_passes.size()))
	{
		m_passes   = in.gpu.passes;
		m_passMode = in.gpu.gpuTimingMode;
		m_passesThisRequest = true;
	}
	in.haveCapture   = m_cpuMs >= 0.0 || !m_passes.empty();
	in.captureCpuMs  = m_cpuMs;
	in.capturePasses = m_passes;
	in.captureMode   = m_passMode;
	in.captureScopes = m_scopes;
	if (m_history.size() >= 30)
	{
		std::vector<float> sorted = m_history;
		std::sort(sorted.begin(), sorted.end());
		auto at = [&](double q) { return static_cast<double>(sorted[static_cast<std::size_t>(q * (sorted.size() - 1))]); };
		in.frameP50 = at(0.50);
		in.frameP99 = at(0.99);
		in.frameMax = static_cast<double>(sorted.back());
	}
	m_sinceRefresh = 0.0f;
	m_frames  = 0;
	m_elapsed = 0.0f;

	const std::vector<std::string> lines = formatDevStats(in, /*extended*/ true, /*maxPasses*/ 6, /*maxScopes*/ 4);
	// Labels and values go to their own columns; the log keeps one line per row.
	std::string text, valueText;
	std::vector<std::string> flat;
	for (std::size_t i = 0; i < lines.size(); ++i)
	{
		const std::size_t tab = lines[i].find('\t');
		if (i > 0) { text += '\n'; valueText += '\n'; }
		text += lines[i].substr(0, tab);
		if (tab != std::string::npos) valueText += lines[i].substr(tab + 1);
		flat.push_back(tab == std::string::npos ? lines[i]
		                                         : lines[i].substr(0, tab) + "  " + lines[i].substr(tab + 1));
	}
	// HE_GAME_STATS_LOG=<seconds>: the card's text into the log as well, so a run on another
	// machine brings its numbers back without a screenshot.
	static const float logEvery = [] {
		const char* v = std::getenv("HE_GAME_STATS_LOG");
		return v && *v ? std::max(0.0f, static_cast<float>(std::atof(v))) : 0.0f;
	}();
	if (logEvery > 0.0f && (m_sinceLog += kRefreshSeconds) >= logEvery)
	{
		m_sinceLog = 0.0f;
		// In blocks of a few lines: one log message is cut at 512 bytes (Log.cpp), and the
		// whole card is longer than that.
		std::string block;
		int part = 0;
		auto flush = [&] { if (!block.empty()) HE_LOG_INFO(Core, "DevStatsTile [%d]:\n%s", ++part, block.c_str()); block.clear(); };
		for (const std::string& l : flat)
		{
			if (block.size() + l.size() + 1 > 400) flush();
			if (!block.empty()) block += '\n';
			block += l;
		}
		flush();
	}
	const bool wroteText = widgets.setElementProp(m_widget, m_text, "Text", UIPropValue::ofString(std::move(text)))
	                    && widgets.setElementProp(m_widget, m_values, "Text", UIPropValue::ofString(std::move(valueText)));
	// The card follows the text: its height is the lines it holds.
	const bool wroteSize = widgets.setElementProp(m_widget, m_panel, "Size",
	                       UIPropValue::ofVec2({ kTileWidth, kPad * 2.0f + kLineH * static_cast<float>(lines.size()) }));
	// Said once per showing, so the log tells "shown and written" from "shown but nothing
	// reached the widget" (the two look identical on screen: no card).
	if (!m_loggedFirst)
	{
		m_loggedFirst = true;
		if (wroteText && wroteSize)
			HE_LOG_INFO(Core, "DevStatsTile: first refresh written (%zu lines, widget %d visible=%d)",
			            lines.size(), m_widget, widgets.isVisible(m_widget) ? 1 : 0);
		else
			HE_LOG_ERROR(Core, "DevStatsTile: could not write the card (text=%d, size=%d, widget %d alive=%d)",
			             wroteText ? 1 : 0, wroteSize ? 1 : 0, m_widget, widgets.isAlive(m_widget) ? 1 : 0);
	}
}

void DevStatsTile::shutdown(WidgetManager& widgets)
{
	if (m_widget != 0) widgets.destroyWidget(m_widget);
	m_widget = m_panel = m_text = m_values = 0;
	m_visible = false;
	EngineProfiler::instance().setLiveEnabled(false);
	EngineProfiler::instance().cancelSingleFrameCapture();
}

} // namespace HE

#endif // HE_FEATURE_DEV_HOTKEYS
