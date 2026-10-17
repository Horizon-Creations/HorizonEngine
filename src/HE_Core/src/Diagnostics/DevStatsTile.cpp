#include "Diagnostics/DevStatsTile.h"

#include "ContentManager/ContentManager.h"
#include "Diagnostics/Log.h"
#include "Diagnostics/EngineProfiler.h"
#include "UIWidget/UIWidgetTree.h"
#include "UIWidget/WidgetManager.h"

#include <algorithm>
#include <cstdio>

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
	constexpr float kFontSize  = 15.0f;
	constexpr float kLineH     = kFontSize * 1.25f;   // what the card is sized from
}

std::vector<std::string> formatDevStats(const DevStatsInput& in, bool extended, std::size_t maxPasses)
{
	std::vector<std::string> lines;
	char buf[128];

	std::snprintf(buf, sizeof(buf), "%.0f FPS   %.2f ms", in.fps, in.frameMs);
	lines.emplace_back(buf);
	if (in.gpu.gpuFrameMs >= 0.0)
	{
		std::snprintf(buf, sizeof(buf), "GPU        %.2f ms", in.gpu.gpuFrameMs);
		lines.emplace_back(buf);
	}
	lines.push_back("Draws      " + thousands(in.gpu.drawCalls));
	lines.push_back("Triangles  " + thousands(in.gpu.triangles));
	lines.push_back("Objects    " + thousands(in.gpu.visibleObjects) + " / " + thousands(in.gpu.totalObjects));
	if (in.gpu.occlusionCulled > 0)
		lines.push_back("Occluded   " + thousands(in.gpu.occlusionCulled));
	if (in.gpu.vramBudgetMB > 0.0)
	{
		std::snprintf(buf, sizeof(buf), "VRAM       %.0f / %.0f MB", in.gpu.vramUsedMB, in.gpu.vramBudgetMB);
		lines.emplace_back(buf);
	}
	if (!extended) return lines;

	// ── The extended block ────────────────────────────────────────────────────
	if (!in.backend.empty() || (in.width > 0 && in.height > 0))
	{
		std::string row = "Renderer   ";
		row += in.backend.empty() ? std::string("?") : in.backend;
		if (in.width > 0 && in.height > 0)
			row += "  " + std::to_string(in.width) + "x" + std::to_string(in.height);
		lines.push_back(std::move(row));
	}
	if (in.haveCapture && in.captureCpuMs >= 0.0)
	{
		std::snprintf(buf, sizeof(buf), "CPU        %.2f ms", in.captureCpuMs);
		lines.emplace_back(buf);
	}

	// Where the GPU's time goes. From the detailed capture when there is one, else
	// whatever passes the renderer reported for the last frame (usually none: the
	// backends only split a frame while the profiler records).
	const std::vector<IRenderer::GpuPassTime>& passes =
		in.haveCapture ? in.capturePasses : in.gpu.passes;
	const char* mode = in.haveCapture ? in.captureMode : in.gpu.gpuTimingMode;

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

	std::vector<IRenderer::GpuPassTime> sorted = passes;
	std::stable_sort(sorted.begin(), sorted.end(),
	                 [](const IRenderer::GpuPassTime& a, const IRenderer::GpuPassTime& b) { return a.ms > b.ms; });
	const std::size_t shown = std::min(sorted.size(), maxPasses);
	for (std::size_t i = 0; i < shown; ++i)
	{
		// "~" marks an approximate interval (a split inside one encoder), the same
		// rule the profiler window follows: never read as an authoritative cost.
		std::snprintf(buf, sizeof(buf), "%s%-14.14s %6.2f ms", sorted[i].approx ? "~" : " ",
		              sorted[i].name ? sorted[i].name : "", sorted[i].ms);
		lines.emplace_back(buf);
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
		m_widget = m_panel = m_text = 0;
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
		page.find(text)->parentId = card;   // the text lives on the card

		UIWidgetAsset asset;
		asset.treeJson = uiWidgetTreeToJson(page);
		asset.path     = kAssetPath;
		content.registerWidget(std::move(asset));

		m_widget = widgets.createWidget(content, kAssetPath);
		m_panel  = card;
		m_text   = text;
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
	m_primed = false;
	m_loggedFirst = false;
	m_sinceRefresh = m_sinceCapture = 0.0f;
	m_frames = 0;
	m_elapsed = 0.0f;
}

void DevStatsTile::update(WidgetManager& widgets, IRenderer* renderer, float dt,
                          const std::string& backendName, int w, int h)
{
	if (!m_visible || m_widget == 0) return;

	m_sinceRefresh += dt;
	m_sinceCapture += dt;
	m_elapsed      += dt;
	++m_frames;

	EngineProfiler& prof = EngineProfiler::instance();
	if (m_sinceCapture >= kCaptureSeconds || !m_primed)
	{
		m_sinceCapture = 0.0f;
		prof.requestSingleFrameCapture();   // applied at the next frame start
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
	if (const ProfFrameRecord* f = prof.singleFrame())
	{
		in.haveCapture  = true;
		in.captureCpuMs = f->cpuFrameMs;
		for (const ProfGpuPass& p : f->gpuPasses)
			in.capturePasses.push_back({ p.name, p.ms, p.approx });
		in.captureMode = f->gpuTimingMode;
	}
	m_sinceRefresh = 0.0f;
	m_frames  = 0;
	m_elapsed = 0.0f;

	const std::vector<std::string> lines = formatDevStats(in, /*extended*/ true);
	std::string text;
	for (std::size_t i = 0; i < lines.size(); ++i)
	{
		if (i > 0) text += '\n';
		text += lines[i];
	}
	const bool wroteText = widgets.setElementProp(m_widget, m_text, "Text", UIPropValue::ofString(std::move(text)));
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
	m_widget = m_panel = m_text = 0;
	m_visible = false;
	EngineProfiler::instance().setLiveEnabled(false);
}

} // namespace HE
