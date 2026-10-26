#include "doctest.h"

#include <ContentManager/ContentManager.h>
#include <Diagnostics/DevStatsTile.h>
#include <Diagnostics/EngineProfiler.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/WidgetManager.h>

#include "TestFsUtil.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

// ── The development stats tile (F10 in a development build of the game) ─────
// What the tile SAYS is a pure function; how it sits on screen is the widget
// system's layout, asked headless through WidgetManager::extract. The hotkey and
// the frame hook live in GameApplication and are not reachable from here.

namespace
{
	struct TempDir
	{
		std::filesystem::path path;
		explicit TempDir(const char* name)
		{
			path = std::filesystem::temp_directory_path() /
			       (std::string(name) + "_" +
			        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			he_test::removeAllQuiet(path);
			std::filesystem::create_directories(path);
		}
		~TempDir() { he_test::removeAllQuiet(path); }
	};

	bool has(const std::vector<std::string>& lines, const std::string& part)
	{
		return std::any_of(lines.begin(), lines.end(),
			[&](const std::string& l) { return l.find(part) != std::string::npos; });
	}

	// The profiler's live mode is process-wide state; every case leaves it off.
	struct LiveOff { ~LiveOff() { EngineProfiler::instance().setLiveEnabled(false); } };

	HE::DevStatsInput typicalFrame()
	{
		HE::DevStatsInput in;
		in.fps = 143.0; in.frameMs = 6.99;
		in.gpu.gpuFrameMs = 4.21;
		in.gpu.drawCalls = 1234; in.gpu.triangles = 2345678;
		in.gpu.visibleObjects = 800; in.gpu.totalObjects = 2000;
		in.gpu.vramUsedMB = 3200.0; in.gpu.vramBudgetMB = 8192.0;
		in.backend = "Vulkan"; in.width = 1920; in.height = 1080;
		return in;
	}
}

TEST_CASE("Dev stats: the compact card carries the editor's numbers and nothing the backend cannot fill")
{
	HE::DevStatsInput in = typicalFrame();
	std::vector<std::string> lines = HE::formatDevStats(in, /*extended*/ false);
	CHECK(lines[0] == "143 FPS   6.99 ms");
	CHECK(has(lines, "GPU        4.21 ms"));
	CHECK(has(lines, "Draws      1,234"));
	CHECK(has(lines, "Triangles  2,345,678"));
	CHECK(has(lines, "Objects    800 / 2,000"));
	CHECK(has(lines, "VRAM       3200 / 8192 MB"));
	CHECK_FALSE(has(lines, "Occluded"));
	CHECK_FALSE(has(lines, "Renderer"));      // that is the extended block

	// A backend that does not measure the GPU or the VRAM leaves the lines OUT: a 0
	// or a -1 there would read as a measurement.
	in.gpu.gpuFrameMs = -1.0;
	in.gpu.vramBudgetMB = 0.0;
	in.gpu.occlusionCulled = 12;
	lines = HE::formatDevStats(in, false);
	CHECK_FALSE(has(lines, "GPU"));
	CHECK_FALSE(has(lines, "VRAM"));
	CHECK(has(lines, "Occluded   12"));
}

TEST_CASE("Dev stats: the extended card says where the GPU's time goes, most expensive first")
{
	HE::DevStatsInput in = typicalFrame();
	in.haveCapture = true;
	in.captureCpuMs = 3.10;
	in.captureMode = "detailed";
	in.capturePasses = { { "Shadows", 0.80, false }, { "GBuffer", 1.90, false },
	                     { "Bloom", 0.35, true }, { "Tonemap", 0.10, false } };

	const std::vector<std::string> lines = HE::formatDevStats(in, /*extended*/ true);
	CHECK(has(lines, "Renderer   Vulkan  1920x1080"));
	CHECK(has(lines, "CPU        3.10 ms"));
	CHECK(has(lines, "GPU passes  (detailed)"));

	// Sorted by cost, an approximate interval marked.
	auto at = [&](const char* name) {
		for (std::size_t i = 0; i < lines.size(); ++i)
			if (lines[i].find(name) != std::string::npos) return static_cast<int>(i);
		return -1;
	};
	REQUIRE(at("GBuffer") > 0);
	CHECK(at("GBuffer") < at("Shadows"));
	CHECK(at("Shadows") < at("Bloom"));
	CHECK(at("Bloom") < at("Tonemap"));
	CHECK(lines[static_cast<std::size_t>(at("Bloom"))][0] == '~');
	CHECK(lines[static_cast<std::size_t>(at("GBuffer"))][0] == ' ');
	CHECK(has(lines, "1.90 ms"));
}

TEST_CASE("Dev stats: the frame-time spread and the captured frame's costliest CPU scopes come before the GPU passes")
{
	HE::DevStatsInput in = typicalFrame();
	in.haveCapture = true;
	in.captureCpuMs = 9.0;
	in.frameP50 = 16.6; in.frameP99 = 41.2; in.frameMax = 88.0;
	in.captureScopes = { { "OnRender", 1.2 }, { "Render", 7.5 }, { "Metal::EncodeScene", 3.25 },
	                     { "SceneSystemsTick", 0.4 } };
	in.capturePasses = { { "Scene", 2.0, false } };
	const std::vector<std::string> lines = HE::formatDevStats(in, true, 8, /*maxScopes*/ 3);
	CHECK(has(lines, "Frame p50 16.6  p99 41.2  max 88.0"));
	auto at = [&](const char* name) {
		for (std::size_t i = 0; i < lines.size(); ++i)
			if (lines[i].find(name) != std::string::npos) return static_cast<int>(i);
		return -1;
	};
	REQUIRE(at("CPU scopes") > 0);
	CHECK(at("Render ") < at("Metal::EncodeScene"));         // by cost
	CHECK(at("Metal::EncodeScene") < at("OnRender"));
	CHECK(at("SceneSystemsTick") == -1);                       // capped at three
	CHECK(at("OnRender") < at("GPU passes"));
	CHECK(has(lines, "3.25 ms"));
	// Without the numbers the lines are left out, not shown as -1 or 0.
	HE::DevStatsInput bare = typicalFrame();
	CHECK_FALSE(has(HE::formatDevStats(bare, true), "Frame p50"));
	CHECK_FALSE(has(HE::formatDevStats(bare, true), "CPU scopes"));
}

TEST_CASE("Dev stats: the pass list is capped and says how many it left out")
{
	HE::DevStatsInput in = typicalFrame();
	in.haveCapture = true;
	for (int i = 0; i < 12; ++i)
		in.capturePasses.push_back({ "Pass", 1.0 + i, false });
	const std::vector<std::string> lines = HE::formatDevStats(in, true, /*maxPasses*/ 5);
	CHECK(has(lines, "+ 7 smaller passes"));
	int passRows = 0;
	for (const std::string& l : lines)
		if (l.find(" ms") != std::string::npos && l.find("Pass") != std::string::npos) ++passRows;
	CHECK(passRows == 5);
}

TEST_CASE("Dev stats: before the first capture it says it is measuring, and a backend without passes says so")
{
	HE::DevStatsInput in = typicalFrame();
	CHECK(has(HE::formatDevStats(in, true), "measuring"));

	in.haveCapture = true;      // a capture came back, with no passes in it
	CHECK(has(HE::formatDevStats(in, true), "none reported by this backend"));
}

TEST_CASE("Dev stats tile: toggling shows and hides one widget and keeps the profiler's live mode in step")
{
	LiveOff off;
	TempDir dir("he_dev_stats");
	ContentManager cm(dir.path.string());
	WidgetManager wm;
	HE::DevStatsTile tile;

	CHECK_FALSE(tile.visible());
	CHECK_FALSE(EngineProfiler::instance().liveEnabled());

	CHECK(tile.toggle(wm, cm));
	REQUIRE(tile.widgetId() != 0);
	CHECK(tile.visible());
	CHECK(wm.isAlive(tile.widgetId()));
	CHECK(wm.isVisible(tile.widgetId()));
	// Without live mode the backends do not time the GPU at all.
	CHECK(EngineProfiler::instance().liveEnabled());

	const int id = tile.widgetId();
	CHECK_FALSE(tile.toggle(wm, cm));
	CHECK_FALSE(tile.visible());
	CHECK_FALSE(wm.isVisible(id));
	CHECK(wm.isAlive(id));                              // hidden, not destroyed: no rebuild per press
	CHECK_FALSE(EngineProfiler::instance().liveEnabled());

	CHECK(tile.toggle(wm, cm));
	CHECK(tile.widgetId() == id);                       // the same widget comes back

	tile.shutdown(wm);
	CHECK_FALSE(wm.isAlive(id));
	CHECK_FALSE(tile.visible());
	CHECK_FALSE(EngineProfiler::instance().liveEnabled());
}

TEST_CASE("Dev stats tile: update writes the text at the refresh rate and sizes the card to it")
{
	LiveOff off;
	TempDir dir("he_dev_stats");
	ContentManager cm(dir.path.string());
	WidgetManager wm;
	HE::DevStatsTile tile;
	REQUIRE(tile.toggle(wm, cm));

	auto textOf = [&]() -> std::string {
		const HE::UIWidgetTree* t = wm.tree(tile.widgetId());
		const auto* e = t ? dynamic_cast<const HE::UIText*>(t->find(tile.textElementId())) : nullptr;
		return e ? e->text : std::string("<missing>");
	};
	auto cardHeight = [&]() {
		const HE::UIWidgetTree* t = wm.tree(tile.widgetId());
		return t->find(tile.panelElementId())->sizeY;
	};

	CHECK(textOf().empty());                            // built empty

	// No renderer (running without graphics): the first frame after showing refreshes
	// at once and shows what it can.
	tile.update(wm, nullptr, 0.016f, "Vulkan", 1280, 720);
	const std::string first = textOf();
	CHECK(first.find("FPS") != std::string::npos);
	CHECK(first.find("Renderer   Vulkan  1280x720") != std::string::npos);
	CHECK(first.find("measuring") != std::string::npos);
	const float h = cardHeight();
	CHECK(h > 100.0f);                                  // lines, not the placeholder height

	// Inside the refresh interval nothing is rewritten.
	wm.setElementProp(tile.widgetId(), tile.textElementId(), "Text", HE::UIPropValue::ofString("sentinel"));
	tile.update(wm, nullptr, 0.016f, "Vulkan", 1280, 720);
	CHECK(textOf() == "sentinel");

	// Past it, it is.
	tile.update(wm, nullptr, HE::DevStatsTile::kRefreshSeconds, "Vulkan", 1280, 720);
	CHECK(textOf() != "sentinel");
	CHECK(textOf().find("FPS") != std::string::npos);

	// Hidden: a bool test, no writes.
	tile.toggle(wm, cm);
	wm.setElementProp(tile.widgetId(), tile.textElementId(), "Text", HE::UIPropValue::ofString("kept"));
	tile.update(wm, nullptr, 10.0f, "Vulkan", 1280, 720);
	CHECK(textOf() == "kept");
	tile.shutdown(wm);
}

TEST_CASE("Dev stats tile: the card sits in the top-right corner of the window and draws text")
{
	LiveOff off;
	TempDir dir("he_dev_stats");
	ContentManager cm(dir.path.string());
	WidgetManager wm;
	HE::DevStatsTile tile;
	REQUIRE(tile.toggle(wm, cm));
	tile.update(wm, nullptr, 0.016f, "Vulkan", 1280, 720);

	std::vector<UIRenderObject> out;
	wm.extract(1280.0f, 720.0f, out);

	// The card: the solid quad (type 0) that is 330 wide.
	const UIRenderObject* card = nullptr;
	int glyphs = 0;
	for (const UIRenderObject& ro : out)
	{
		if (ro.type == 2) ++glyphs;
		else if (ro.type == 0 && std::abs(ro.size.x - 330.0f) < 1.0f) card = &ro;
	}
	REQUIRE(card != nullptr);
	CHECK(card->position.x + card->size.x == doctest::Approx(1280.0f - 12.0f).epsilon(0.01));
	CHECK(card->position.y == doctest::Approx(12.0f).epsilon(0.05));
	CHECK(glyphs > 20);
	// Translucent: the scene stays readable behind it.
	CHECK(card->color.a < 1.0f);

	tile.shutdown(wm);
}

TEST_CASE("WidgetManager::setElementProp refuses what does not fit")
{
	TempDir dir("he_dev_stats");
	ContentManager cm(dir.path.string());
	WidgetManager wm;
	HE::DevStatsTile tile;
	REQUIRE(tile.toggle(wm, cm));
	LiveOff off;

	const int w = tile.widgetId(), t = tile.textElementId();
	CHECK(wm.setElementProp(w, t, "Text", HE::UIPropValue::ofString("ok")));
	CHECK_FALSE(wm.setElementProp(w, t, "Text", HE::UIPropValue::ofFloat(1.0f)));       // wrong type
	CHECK_FALSE(wm.setElementProp(w, 9999, "Text", HE::UIPropValue::ofString("x")));    // no such element
	CHECK_FALSE(wm.setElementProp(9999, t, "Text", HE::UIPropValue::ofString("x")));    // no such widget
	tile.shutdown(wm);
}
