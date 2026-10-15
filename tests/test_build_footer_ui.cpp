#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "BuildProgressDialog.h"
#include "NotificationStore.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdlib>
#include <string>
#include <vector>

// ── A build whose window is put away ─────────────────────────────────────────
// The Build window used to be a modal the user could not leave while a run went
// on. Now "Hide" puts it away: the footer shows the progress (and a click brings
// the window back), and a run that ends with the window away raises a
// notification — while a run that ends in front of the user does not. The
// progress arithmetic is tested on its own first. HE_UI_DUMP_DIR writes BMP frames.

using namespace HE::Ed;
namespace BPD = BuildProgressDialog;

namespace
{
	constexpr int W = 900, H = 600;
	constexpr float kFootY = 572.0f, kFootH = 28.0f;

	struct Harness
	{
		Harness()
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(float(W), float(H));
			io.DeltaTime   = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
			applyHorizonDarkTheme();
			setGlobalNotifications(&store);
		}
		~Harness()
		{
			setGlobalNotifications(nullptr);
			ImGui::DestroyContext();
		}
		NotificationStore store;
	};

	struct ContextBits
	{
		EditorConfig    config;
		bool            vsync = false;
		std::string     backendName = "Software";
		EditorSelection selection;
		std::string     scenePath;
		bool            exitRequested = false, projectLoaded = true;
		bool            refreshPending = false, refreshDone = false;
		int   fpsOffset = 0, fpsCount = 0;
		float fpsAccum = 0.0f, smoothFps = 0.0f;
		std::vector<AppContext::EditorTab> tabs;
		int   activeTab = 0;
		float cbTreeWidth = 200.0f;
		int   hubPreset = 0, hubLang = 0;
		bool  hubFx = false;
		std::string hubCreateError, hubOpenError;
		int   hubRemoveIndex = -1;
		bool  hubRemoveRequested = false;
		std::string dirResult, fileResult;
		bool  dirReady = false, fileReady = false;

		AppContext make()
		{
			return AppContext{
				.editorConfig          = config,
				.vsync                 = vsync,
				.backendName           = backendName,
				.selection             = selection,
				.currentScenePath      = scenePath,
				.exitRequested         = exitRequested,
				.projectLoaded         = projectLoaded,
				.contentRefreshPending = refreshPending,
				.contentRefreshDone    = refreshDone,
				.fpsHistoryOffset      = fpsOffset,
				.fpsAccum              = fpsAccum,
				.fpsAccumCount         = fpsCount,
				.smoothFps             = smoothFps,
				.tabs                  = tabs,
				.activeTab             = activeTab,
				.cbTreeWidth           = cbTreeWidth,
				.hubSelectedPreset     = hubPreset,
				.hubSelectedLang       = hubLang,
				.hubAdvancedShaderFx   = hubFx,
				.hubCreateError        = hubCreateError,
				.hubOpenError          = hubOpenError,
				.hubRemoveIndex        = hubRemoveIndex,
				.hubRemoveRequested    = hubRemoveRequested,
				.pendingDirResult      = dirResult,
				.pendingDirReady       = dirReady,
				.pendingFileResult     = fileResult,
				.pendingFileReady      = fileReady,
			};
		}
	};

	constexpr float kClusterX = 640.0f;

	// One frame: the Build window, and a footer strip with the cluster at kClusterX.
	void frame(ContextBits& bits, he_ui::Image* shot = nullptr)
	{
		AppContext ctx = bits.make();
		ImGui::NewFrame();
		BPD::render(ctx);

		ImGui::SetNextWindowPos(ImVec2(0.0f, kFootY));
		ImGui::SetNextWindowSize(ImVec2(float(W), kFootH));
		ImGui::Begin("##footer", nullptr,
		             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
		if (BPD::FooterWidth(ctx) > 0.0f)
		{
			ImGui::SetCursorScreenPos(ImVec2(kClusterX, kFootY + 6.0f));
			BPD::DrawFooter(ctx);
		}
		ImGui::End();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
	}

	void frames(ContextBits& bits, int n) { for (int i = 0; i < n; ++i) frame(bits); }

	void click(ContextBits& bits, float x, float y)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMousePosEvent(x, y);
		frame(bits);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		frame(bits);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		frame(bits);
	}

	void dump(ContextBits& bits, const char* name)
	{
		he_ui::Image img;
		frame(bits, &img);
		if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
			he_ui::writeBmp(img, std::string(d) + "/" + name + ".bmp");
	}

	// Starts an export run and brings its window up.
	void startRun(ContextBits& bits)
	{
		BPD::Build::begin({ "Setup", "Build", "Package" }, BPD::Kind::Export);
		BPD::Build::stepBegin(1);
		BPD::Build::stepProgress(0.5f);
		BPD::requestOpen();
		frames(bits, 4);
	}

	// Clicks along the window's bottom row until the run is hidden.
	bool pressHide(ContextBits& bits)
	{
		ImGuiWindow* w = ImGui::GetTopMostPopupModal();
		if (!w) return false;
		const float y = w->Pos.y + w->Size.y - 10.0f - 11.0f;
		for (float x = w->Pos.x + 16.0f; x < w->Pos.x + w->Size.x - 8.0f; x += 6.0f)
		{
			click(bits, x, y);
			if (BPD::hidden()) return true;
		}
		return false;
	}

	int countByTitle(const NotificationStore& s, const std::string& title)
	{
		int n = 0;
		for (const Notification& e : s.snapshot()) if (e.title == title) ++n;
		return n;
	}

	void finishRun()
	{
		// A finished run is left behind so the next test starts from a clean model.
		if (BPD::Build::running()) BPD::Build::finish(false, "Error: test cleanup");
	}
}

TEST_CASE("Build progress: the whole run's fraction counts finished steps whole")
{
	using BPD::StepView;
	bool indeterminate = true;
	CHECK(BPD::overallProgress({}, &indeterminate) == 0.0f);
	CHECK_FALSE(indeterminate);

	// done, running at 50 %, pending → (1 + 0.5 + 0) / 3
	std::vector<StepView> steps = {
		{ "a", 2, 1.0f, false, "" }, { "b", 1, 0.5f, false, "" }, { "c", 0, 0.0f, true, "" } };
	CHECK(BPD::overallProgress(steps, &indeterminate) == doctest::Approx(0.5f));
	CHECK_FALSE(indeterminate);

	// A failed step is over: it counts whole, so the bar does not run backwards.
	steps[1].state = 3;
	CHECK(BPD::overallProgress(steps) == doctest::Approx(2.0f / 3.0f));

	// Nothing known yet: the first step is running and has no length.
	std::vector<StepView> fresh = {
		{ "a", 1, 0.0f, true, "" }, { "b", 0, 0.0f, true, "" } };
	CHECK(BPD::overallProgress(fresh, &indeterminate) == 0.0f);
	CHECK(indeterminate);

	// But a run that is mostly done is not "unknown" because the last step is.
	std::vector<StepView> late = {
		{ "a", 2, 1.0f, false, "" }, { "b", 2, 1.0f, false, "" }, { "c", 1, 0.0f, true, "" } };
	CHECK(BPD::overallProgress(late, &indeterminate) == doctest::Approx(2.0f / 3.0f));
	CHECK_FALSE(indeterminate);
}

TEST_CASE("Build footer: nothing to show until a running build is hidden")
{
	Harness h;
	ContextBits bits;
	AppContext ctx = bits.make();
	ImGui::NewFrame();
	CHECK(BPD::FooterWidth(ctx) == 0.0f);   // nothing built yet
	ImGui::EndFrame();

	startRun(bits);
	REQUIRE(BPD::isOpen());                 // shown for a new run
	AppContext ctx2 = bits.make();
	ImGui::NewFrame();
	CHECK(BPD::FooterWidth(ctx2) == 0.0f);  // while the window is up there is no cluster
	ImGui::EndFrame();
	finishRun();
	frames(bits, 2);
}

TEST_CASE("Build footer: Hide puts the window away, the run goes on, a click brings it back")
{
	Harness h;
	ContextBits bits;
	startRun(bits);
	REQUIRE(BPD::isOpen());

	REQUIRE(pressHide(bits));
	frames(bits, 3);
	CHECK_FALSE(BPD::isOpen());
	CHECK(BPD::Build::running());           // the run did not stop
	CHECK(ImGui::GetTopMostPopupModal() == nullptr);   // and the window did not come back by itself

	{
		AppContext ctx = bits.make();
		ImGui::NewFrame();
		CHECK(BPD::FooterWidth(ctx) > 40.0f);
		ImGui::EndFrame();
	}
	dump(bits, "build_footer_hidden");

	// A click on the cluster reopens the window and drops the cluster.
	click(bits, kClusterX + 8.0f, kFootY + 12.0f);
	frames(bits, 3);
	CHECK_FALSE(BPD::hidden());
	CHECK(BPD::isOpen());

	finishRun();
	frames(bits, 3);
}

TEST_CASE("Build footer: a run that ends with the window away raises a notification")
{
	Harness h;
	ContextBits bits;
	startRun(bits);
	REQUIRE(pressHide(bits));
	frames(bits, 3);
	CHECK(h.store.snapshot().empty());

	BPD::Build::setLaunchTarget("/tmp/Game", /*runnableHere*/ true);
	BPD::Build::finish(true, "OK: exported to /tmp/Game");
	frames(bits, 3);

	const auto rows = h.store.snapshot();
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].title == "Export finished");
	CHECK(rows[0].level == NoteLevel::Important);
	CHECK(rows[0].text == "exported to /tmp/Game");   // the "OK: " prefix is the title's job
	REQUIRE(rows[0].actions.size() == 2);
	CHECK(rows[0].actions[0].label == "Start Game");
	CHECK(rows[0].actions[0].primary);
	CHECK(rows[0].actions[1].label == "Show Build");
	// Only once, however many frames pass.
	frames(bits, 5);
	CHECK(h.store.snapshot().size() == 1);

	// "Show Build" brings the window back.
	REQUIRE(h.store.runAction(rows[0].id, 1));
	frames(bits, 3);
	CHECK(BPD::isOpen());
}

TEST_CASE("Build footer: a failed export while hidden is announced with Show Build first")
{
	Harness h;
	ContextBits bits;
	startRun(bits);
	REQUIRE(pressHide(bits));
	frames(bits, 2);

	BPD::Build::finish(false, "Error: no game runtime found");
	frames(bits, 3);

	const auto rows = h.store.snapshot();
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].title == "Export failed");
	CHECK(rows[0].text == "no game runtime found");
	REQUIRE(rows[0].actions.size() == 1);
	CHECK(rows[0].actions[0].label == "Show Build");
	CHECK(rows[0].actions[0].primary);
}

TEST_CASE("Build footer: a run that ends in front of the user is not announced")
{
	Harness h;
	ContextBits bits;
	startRun(bits);
	REQUIRE(BPD::isOpen());

	BPD::Build::finish(true, "OK: done");
	frames(bits, 4);
	CHECK(countByTitle(h.store, "Export finished") == 0);
	CHECK(h.store.snapshot().empty());
}
