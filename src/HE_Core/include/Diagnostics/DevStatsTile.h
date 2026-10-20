#pragma once

// ── The development stats tile ───────────────────────────────────────────────
// The editor shows frame counters in the viewport's corner (Show ▸ Stats,
// ViewportPanel.cpp) — painted with ImGui, which a game does not have. A
// development build of the game gets the same card through the engine's own UI:
// a widget built in code, registered as an in-memory asset, shown and hidden by a
// hotkey (GameApplication, HE_GAME_DEV_HOTKEYS), and rewritten a few times a second.
//
// "Extended" is what the editor's overlay does not carry: where the GPU's time
// goes. Backends only measure per-pass GPU time while the profiler records, so
// while the tile is up it keeps the profiler's live mode on (whole-frame GPU time
// and counters) and asks for ONE detailed single-frame capture a second
// (EngineProfiler::requestSingleFrameCapture); the pass list is that frame's.
// That is a stall of one frame per second on tile-deferred GPUs, which is the price
// of the numbers and why this is a hotkey and not always on.
//
// The text itself is a pure function (formatDevStats) so what the tile says can be
// tested without a window, a GPU or a frame.

#include "Renderer/IRenderer.h"
#include "Types/Defines.h"

#include <string>
#include <vector>

class ContentManager;
class WidgetManager;

namespace HE
{

// Everything a tile line is made from, gathered by the caller.
struct DevStatsInput
{
	double fps     = 0.0;    // averaged over the refresh interval, not one frame
	double frameMs = 0.0;
	std::string backend;     // "Vulkan", "Metal", … ("" = not shown)
	int    width = 0, height = 0;   // the drawable, 0 = not shown
	IRenderer::FrameGpuStats gpu;   // the renderer's last frame (counters, whole-frame GPU ms, VRAM)
	// The last detailed single-frame capture, when there is one yet.
	bool   haveCapture = false;
	double captureCpuMs = -1.0;
	std::vector<IRenderer::GpuPassTime> capturePasses;
	const char* captureMode = "";
};

// The lines of the card, top to bottom. A line the backend cannot fill is left out
// rather than shown as 0 or -1, which would read as a measurement. `extended` adds
// the detailed block (CPU time of the captured frame, backend and resolution, and
// the GPU passes sorted by cost, at most `maxPasses`).
HE_API std::vector<std::string> formatDevStats(const DevStatsInput& in, bool extended,
                                               std::size_t maxPasses = 8);

class HE_API DevStatsTile
{
public:
	// Shows the tile (building and registering its widget on first use) or hides it.
	// Returns whether it is visible afterwards.
	bool toggle(WidgetManager& widgets, ContentManager& content);
	bool visible() const { return m_visible; }

	// Once per frame, whether visible or not (a hidden tile costs one bool test).
	// `renderer` may be null (no graphics): the tile then shows what it can.
	void update(WidgetManager& widgets, IRenderer* renderer, float deltaSeconds,
	            const std::string& backendName, int drawableWidth, int drawableHeight);

	// Removes the widget and turns the profiler's live mode back off.
	void shutdown(WidgetManager& widgets);

	// For tests: the widget and the text element's ids (0 = not built).
	int widgetId() const { return m_widget; }
	int textElementId() const { return m_text; }
	int panelElementId() const { return m_panel; }

	// How often the text is rewritten and the detailed capture is taken. Public so
	// a test does not have to wait a second of wall time.
	static constexpr float kRefreshSeconds = 0.25f;
	static constexpr float kCaptureSeconds = 1.0f;

private:
	void setVisible(WidgetManager& widgets, bool on);

	bool  m_visible = false;
	int   m_widget  = 0;
	int   m_panel   = 0;
	int   m_text    = 0;

	float m_sinceRefresh = 0.0f;
	float m_sinceCapture = 0.0f;
	// Frames and time since the last refresh: the rate shown is their quotient, so a
	// single slow frame does not make the number flicker.
	int   m_frames = 0;
	float m_elapsed = 0.0f;
	bool  m_primed  = false;   // the first frame after showing refreshes at once
	bool  m_loggedFirst = false;   // the first refresh of this showing has been logged
};

} // namespace HE
