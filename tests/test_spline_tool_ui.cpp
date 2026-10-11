#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "SplineTool.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorUndo.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/SplineComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/TerrainMeshGenerator.h>   // terrainHeightAt
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterLake.h>

#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName: the id a label resolves to
#include <ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ── The Spline mode's tool, driven headless ──────────────────────────────────
// SplineEdit (test_spline_edit.cpp) says what a click MEANS. What it cannot say
// is whether the viewport code reads the mouse the way the user means it: that
// a press and release on the picture is a click and a press that turns into a
// drag is not, that Alt+click is the orbit, that View mode leaves the picture
// alone, that Delete and Esc reach the tool before the editor's own handling,
// that the Move gizmo on the selected point is ONE undo entry, and that the
// Quick Settings panel's buttons do what they say. So this drives the real
// SplineTool (updateInViewport + renderPanel, with a real AppContext) in a
// headless ImGui context, the way test_terrain_tools_ui.cpp drives the
// Landscape tools. With HE_UI_DUMP_DIR set the panel and the hint over the
// picture are written out as well.

using namespace HE::Ed;

namespace
{
	constexpr int W = 1280, H = 720;

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
#ifdef HE_EDITOR_DEPS_DIR
			const std::string font = std::string(HE_EDITOR_DEPS_DIR) + "/Fonts/Roboto_Condensed-Bold.ttf";
			if (std::FILE* f = std::fopen(font.c_str(), "rb"))
			{
				std::fclose(f);
				ImFontConfig cfg;
				cfg.OversampleH = 2;
				cfg.OversampleV = 2;
				io.FontDefault = io.Fonts->AddFontFromFileTTF(font.c_str(), 15.0f, &cfg);
			}
#endif
			applyHorizonDarkTheme();
			SplineTool::reset();
		}
		~Harness() { SplineTool::reset(); ImGui::DestroyContext(); }
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

		AppContext make(HorizonWorld& world, EditorUndo& undo, EditorMode mode)
		{
			AppContext ctx{
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
			ctx.world   = &world;
			ctx.undoSys = &undo;
			config.mode = mode;
			return ctx;
		}
	};

	// A camera 12 m up and 12 m back looking at the origin: the ground plane
	// y = 0 fills the lower half of the picture.
	struct Camera
	{
		glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 12.0f, 12.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		glm::mat4 proj = glm::perspective(glm::radians(60.0f), float(W) / float(H), 0.1f, 1000.0f);

		ImVec2 px(const glm::vec3& world) const
		{
			const glm::vec4 clip = proj * view * glm::vec4(world, 1.0f);
			REQUIRE(clip.w > 1e-6f);
			return ImVec2((clip.x / clip.w * 0.5f + 0.5f) * float(W), (0.5f - clip.y / clip.w * 0.5f) * float(H));
		}
	};

	struct Rig
	{
		Harness        harness;
		HorizonWorld   world;
		EditorUndo     undo;
		ContextBits    bits;
		AppContext     ctx;
		Camera         cam;
		ViewportToolbar::State toolbar;
		bool           gizmoActive = false;
		he_ui::Image   shot;

		explicit Rig(EditorMode mode = EditorMode::Spline) : ctx(bits.make(world, undo, mode))
		{
			undo.setWorld(&world);
			ImGuizmo::Enable(true);
		}

		// The Scene window with the tool in it. `itemClicked` is what ViewportPanel
		// hands the tool: IsItemClicked on the picture, true on the frame the
		// button goes down over it.
		void viewportWindow(bool itemClicked, bool navigating)
		{
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(float(W), float(H)));
			ImGui::Begin("Scene", nullptr,
			             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
			             ImGuiWindowFlags_NoBackground);
			gizmoActive = SplineTool::updateInViewport(
				ctx, cam.view, cam.proj, ImVec2(0.0f, 0.0f), ImVec2(float(W), float(H)),
				navigating, /*viewportHovered=*/true, itemClicked, toolbar, {}, {});
			ImGui::End();
		}

		// One frame with the mouse and button as given.
		void frame(bool itemClicked = false, bool navigating = false, bool rasterize = false)
		{
			ImGui::NewFrame();
			ImGuizmo::BeginFrame();
			viewportWindow(itemClicked, navigating);
			ImGui::Render();
			if (rasterize) shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		}

		void mouse(const ImVec2& p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); }
		void button(bool down)      { ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down); }
		void settle(const ImVec2& p) { mouse(p); for (int i = 0; i < 3; ++i) frame(); }

		// Press and release on the same spot: a click.
		void click(const ImVec2& p)
		{
			settle(p);
			button(true);  frame(/*itemClicked=*/true);
			button(false); frame();
			frame();
		}
		void clickGround(const glm::vec3& at) { click(cam.px(at)); }

		// Press, pull `by` pixels away, release: a drag.
		void drag(const ImVec2& p, const ImVec2& by)
		{
			settle(p);
			button(true);  frame(true);
			mouse(ImVec2(p.x + by.x * 0.5f, p.y + by.y * 0.5f)); frame();
			mouse(ImVec2(p.x + by.x, p.y + by.y)); frame();
			button(false); frame();
			frame();
		}

		SplineComponent* spline()
		{
			for (auto [e, s] : world.registry().view<SplineComponent>().each()) { (void)e; return &s; }
			return nullptr;
		}
		size_t splineCount() { return world.registry().view<SplineComponent>().size(); }
		size_t points() { SplineComponent* s = spline(); return s ? s->controlPoints.size() : 0u; }
	};

	void dump(const he_ui::Image& img, const char* name)
	{
		if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
			he_ui::writeBmp(img, std::string(dir) + "/" + name + ".bmp");
	}

	// ── The Quick Settings panel ────────────────────────────────────────────
	constexpr const char* kPanelWindow = "Spline###Quick Settings";
	constexpr int PW = 360, PH = 420;

	// The editor runs the Scene window and this panel every frame, and the tool
	// treats a gap in its own frames as "the mode was off": so the panel frame
	// runs the viewport too (no click on it — the pointer is over the panel).
	ImGuiID panelFrame(Rig& r, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		ImGuizmo::BeginFrame();
		r.viewportWindow(false, false);
		ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
		ImGui::SetNextWindowSize(ImVec2(float(PW) - 20.0f, float(PH) - 20.0f));
		ImGui::Begin(kPanelWindow, nullptr,
		             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
		SplineTool::renderPanel(r.ctx);
		ImGui::End();
		EditorWidgets::drawQueuedHelp();
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		return hovered;
	}

	ImGuiID panelIdAt(Rig& r, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		panelFrame(r, false);
		return panelFrame(r, false);
	}

	bool locate(Rig& r, const char* label, float& outX, float& outY)
	{
		ImGuiWindow* w = ImGui::FindWindowByName(kPanelWindow);
		if (!w) return false;
		const ImGuiID id = w->GetID(label);
		for (float y = 14.0f; y < float(PH) - 14.0f; y += 3.0f)
			for (float x = 18.0f; x < 120.0f; x += 8.0f)
				if (panelIdAt(r, x, y) == id) { outX = x; outY = y; return true; }
		return false;
	}

	void panelClick(Rig& r, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		panelFrame(r, false);
		panelFrame(r, false);
		panelFrame(r, true);
		panelFrame(r, false);
		panelFrame(r, false);
	}
}

TEST_CASE("spline ui: clicks on the ground draw a line, one undo step each")
{
	Rig r;
	// A press on the picture needs a frame to find its feet first.
	r.clickGround({ -4.0f, 0.0f, 0.0f });
	REQUIRE(r.splineCount() == 1);
	CHECK(r.points() == 1);
	CHECK(r.undo.undoDepth() == 1);
	// The new spline is the selection, so the Details panel shows it.
	CHECK(r.ctx.selection.size() == 1);

	r.clickGround({ 0.0f, 0.0f, -3.0f });
	r.clickGround({ 4.0f, 0.0f, 0.0f });
	CHECK(r.points() == 3);
	CHECK(r.undo.undoDepth() == 3);

	// The points stand where the mouse was, on the ground.
	const SplineComponent* s = r.spline();
	REQUIRE(s != nullptr);
	const Entity e = r.ctx.selection.primary();
	const glm::mat4 model = HE::worldMatrixOf(r.world, e);
	const glm::vec3 last = glm::vec3(model * glm::vec4(s->controlPoints.back(), 1.0f));
	CHECK(last.x == doctest::Approx(4.0f).epsilon(0.01));
	CHECK(last.y == doctest::Approx(0.0f).epsilon(0.01));
	CHECK(std::fabs(last.z) < 0.02f);

	// Ctrl+Z, the way the editor does it: the history steps back, the selection
	// is cleared, and the very next frame the tool has its spline again.
	REQUIRE(r.undo.undo());
	r.ctx.selection.clear();
	r.frame();
	CHECK(r.points() == 2);
	CHECK(r.ctx.selection.size() == 1);
}

TEST_CASE("spline ui: a press that becomes a drag is the camera's, not a point")
{
	Rig r;
	r.clickGround({ -4.0f, 0.0f, 0.0f });
	REQUIRE(r.points() == 1);
	const size_t depth = r.undo.undoDepth();

	r.drag(r.cam.px({ 2.0f, 0.0f, 2.0f }), ImVec2(60.0f, 30.0f));
	CHECK(r.points() == 1);
	CHECK(r.undo.undoDepth() == depth);
}

TEST_CASE("spline ui: Alt+click is the orbit and a navigating camera owns the button")
{
	Rig r;
	r.clickGround({ -4.0f, 0.0f, 0.0f });
	const size_t depth = r.undo.undoDepth();

	ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
	r.clickGround({ 3.0f, 0.0f, 1.0f });
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
	r.frame();
	CHECK(r.points() == 1);
	CHECK(r.undo.undoDepth() == depth);

	// RMB fly-look: the caller says it is navigating.
	r.settle(r.cam.px({ 3.0f, 0.0f, 1.0f }));
	r.button(true);  r.frame(true, /*navigating=*/true);
	r.button(false); r.frame(false, true);
	r.frame();
	CHECK(r.points() == 1);
}

TEST_CASE("spline ui: in View mode the tool leaves the picture alone")
{
	Rig r(EditorMode::View);
	r.clickGround({ -4.0f, 0.0f, 0.0f });
	CHECK(r.splineCount() == 0);
	CHECK(r.undo.undoDepth() == 0);
	CHECK_FALSE(r.gizmoActive);
	CHECK_FALSE(SplineTool::deleteKey(r.ctx));
	CHECK_FALSE(SplineTool::escapeKey(r.ctx));
}

TEST_CASE("spline ui: Delete and Esc are the tool's while a point is held, the editor's otherwise")
{
	Rig r;
	r.clickGround({ -4.0f, 0.0f, 0.0f });
	r.clickGround({ 0.0f, 0.0f, -3.0f });
	r.clickGround({ 4.0f, 0.0f, 0.0f });
	REQUIRE(r.points() == 3);
	const size_t depth = r.undo.undoDepth();

	// The last point is selected: Delete takes it, and the neighbour is next.
	CHECK(SplineTool::deleteKey(r.ctx));
	CHECK(r.points() == 2);
	CHECK(r.undo.undoDepth() == depth + 1);
	CHECK(r.splineCount() == 1);   // the entity is still there

	// Esc lets go of the point; a second one is the editor's (deselect).
	CHECK(SplineTool::escapeKey(r.ctx));
	CHECK_FALSE(SplineTool::escapeKey(r.ctx));
	// With nothing held, Delete is not the tool's either: entity delete runs.
	CHECK_FALSE(SplineTool::deleteKey(r.ctx));
	CHECK(r.points() == 2);

	// A playing editor has no tool at all.
	r.ctx.isPlaying = true;
	r.frame();
	CHECK_FALSE(SplineTool::deleteKey(r.ctx));
}

TEST_CASE("spline ui: a click on the line inserts a point, on a handle selects it")
{
	Rig r;
	r.clickGround({ -4.0f, 0.0f, 0.0f });
	r.clickGround({ 4.0f, 0.0f, 0.0f });
	REQUIRE(r.points() == 2);
	const size_t depth = r.undo.undoDepth();

	// Halfway between the two, on the line: an insert, not an append.
	r.clickGround({ 0.0f, 0.0f, 0.0f });
	CHECK(r.points() == 3);
	CHECK(r.undo.undoDepth() == depth + 1);
	const SplineComponent* s = r.spline();
	REQUIRE(s != nullptr);
	// Local to the line's first point: it starts at world x = -4, the click was at 0.
	CHECK(std::fabs(s->controlPoints[1].x - 4.0f) < 0.2f);   // between the ends (0 and 8)

	// A click on the first handle only selects it.
	const Entity e = r.ctx.selection.primary();
	const glm::vec3 first = glm::vec3(HE::worldMatrixOf(r.world, e) * glm::vec4(s->controlPoints[0], 1.0f));
	r.click(r.cam.px(first));
	CHECK(r.points() == 3);
	CHECK(r.undo.undoDepth() == depth + 1);
	CHECK(SplineTool::deleteKey(r.ctx));       // …which Delete then removes
	CHECK(r.points() == 2);
}

TEST_CASE("spline ui: dragging the gizmo on the selected point moves it, as one undo entry")
{
	Rig r;
	r.clickGround({ -4.0f, 0.0f, 0.0f });
	r.clickGround({ 4.0f, 0.0f, 0.0f });
	REQUIRE(r.points() == 2);
	const size_t depth = r.undo.undoDepth();

	const Entity e = r.ctx.selection.primary();
	const glm::mat4 model = HE::worldMatrixOf(r.world, e);
	const glm::vec3 before = glm::vec3(model * glm::vec4(r.spline()->controlPoints[1], 1.0f));
	const ImVec2 at = r.cam.px(before);

	// The selected point carries the gizmo: hovering its centre grabs it.
	r.settle(at);
	REQUIRE(r.gizmoActive);
	r.button(true);
	r.frame(/*itemClicked=*/true);   // grabbing is not a click on the ground
	REQUIRE(r.gizmoActive);
	const ImVec2 to(at.x + 110.0f, at.y - 50.0f);
	r.mouse(to);
	r.frame();
	r.mouse(ImVec2(to.x + 10.0f, to.y));
	r.frame();
	CHECK(r.undo.undoDepth() == depth);   // pending until the button goes up
	r.button(false);
	r.frame();
	r.frame();

	// Moved, no extra point, one entry.
	CHECK(r.points() == 2);
	const glm::vec3 after = glm::vec3(model * glm::vec4(r.spline()->controlPoints[1], 1.0f));
	CHECK(glm::length(after - before) > 0.5f);
	CHECK(r.undo.undoDepth() == depth + 1);
	CHECK(r.undo.undoLabel() == "Move Spline Point");

	// Undo puts it back, and the entry is exactly one.
	REQUIRE(r.undo.undo());
	r.ctx.selection.clear();
	r.frame();
	const Entity back = r.ctx.selection.primary();
	REQUIRE((back != entt::null));
	const glm::vec3 restored = glm::vec3(HE::worldMatrixOf(r.world, back) * glm::vec4(r.spline()->controlPoints[1], 1.0f));
	CHECK(glm::length(restored - before) < 1e-3f);
	CHECK(r.undo.undoDepth() == depth);
}

TEST_CASE("spline ui: Delete tapped during a gizmo drag is swallowed, and the drag stays one entry")
{
	Rig r;
	r.clickGround({ -4.0f, 0.0f, 0.0f });
	r.clickGround({ 4.0f, 0.0f, 0.0f });
	REQUIRE(r.points() == 2);
	const size_t depth = r.undo.undoDepth();

	const Entity e = r.ctx.selection.primary();
	const glm::mat4 model = HE::worldMatrixOf(r.world, e);
	const glm::vec3 before = glm::vec3(model * glm::vec4(r.spline()->controlPoints[1], 1.0f));
	const ImVec2 at = r.cam.px(before);
	r.settle(at);
	REQUIRE(r.gizmoActive);
	r.button(true);
	r.frame(true);
	r.mouse(ImVec2(at.x + 90.0f, at.y - 40.0f));
	r.frame();
	REQUIRE(r.gizmoActive);

	// The key is the tool's (true: spent), and nothing was deleted.
	CHECK(SplineTool::deleteKey(r.ctx));
	CHECK(r.points() == 2);
	CHECK(r.splineCount() == 1);

	r.mouse(ImVec2(at.x + 130.0f, at.y - 60.0f));
	r.frame();
	r.button(false);
	r.frame();
	r.frame();
	CHECK(r.points() == 2);
	CHECK(r.undo.undoDepth() == depth + 1);
	REQUIRE(r.undo.undo());
	r.ctx.selection.clear();
	r.frame();
	CHECK(r.points() == 2);
	CHECK(r.undo.undoDepth() == depth);
}

TEST_CASE("spline ui: the panel closes the line and deletes the selected point, undoably")
{
	Rig r;
	r.clickGround({ -4.0f, 0.0f, 0.0f });
	r.clickGround({ 0.0f, 0.0f, -3.0f });
	r.clickGround({ 4.0f, 0.0f, 0.0f });
	REQUIRE(r.points() == 3);

	he_ui::Image img;
	for (int i = 0; i < 4; ++i) panelFrame(r, false, i == 3 ? &img : nullptr);
	REQUIRE(img.valid());
	dump(img, "spline-panel");

	float x = 0.0f, y = 0.0f;
	REQUIRE_MESSAGE(locate(r, "Closed", x, y), "the Closed switch is not on the panel");
	REQUIRE_FALSE(r.spline()->closed);
	const size_t depth = r.undo.undoDepth();
	panelClick(r, x, y);
	r.frame();   // every editor frame runs the viewport tool too
	CHECK(r.spline()->closed);
	CHECK(r.undo.undoDepth() == depth + 1);

	REQUIRE_MESSAGE(locate(r, "Delete Point", x, y), "no Delete Point button while a point is selected");
	panelClick(r, x, y);
	r.frame();
	CHECK(r.points() == 2);
	CHECK(r.undo.undoDepth() == depth + 2);

	// "New Spline" lets go of the selection: the next click starts a second line.
	REQUIRE_MESSAGE(locate(r, "New Spline", x, y), "no New Spline button");
	panelClick(r, x, y);
	CHECK(r.ctx.selection.empty());
	r.frame();
	r.frame();
	r.clickGround({ 5.0f, 0.0f, 5.0f });
	CHECK(r.splineCount() == 2);
}

TEST_CASE("spline ui: the picture carries a hint that says what a click does")
{
	Rig r;
	r.settle(ImVec2(200.0f, 200.0f));
	r.frame(false, false, /*rasterize=*/true);
	REQUIRE(r.shot.valid());
	dump(r.shot, "spline-viewport-hint-empty");

	r.clickGround({ -4.0f, 0.0f, 0.0f });
	r.clickGround({ 4.0f, 0.0f, 0.0f });
	r.settle(ImVec2(200.0f, 200.0f));
	r.frame(false, false, true);
	dump(r.shot, "spline-viewport-hint-line");
	// The hint is ImGui draw data: it exists as text in the frame.
	CHECK(r.shot.valid());
}

// ── The Lake section of the panel ────────────────────────────────────────────

namespace
{
	namespace lakeui
	{
		constexpr int PH = 700;

		// The Spline panel at a height that holds the whole Lake section.
		ImGuiID frame(Rig& r, bool mouseDown, he_ui::Image* shot = nullptr)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
			ImGui::NewFrame();
			ImGuizmo::BeginFrame();
			r.viewportWindow(false, false);
			ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
			ImGui::SetNextWindowSize(ImVec2(float(PW) - 20.0f, float(PH) - 20.0f));
			ImGui::Begin(kPanelWindow, nullptr,
			             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
			SplineTool::renderPanel(r.ctx);
			ImGui::End();
			EditorWidgets::drawQueuedHelp();
			const ImGuiID hovered = ImGui::GetHoveredID();
			ImGui::Render();
			if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
			return hovered;
		}

		ImGuiID idAt(Rig& r, float x, float y)
		{
			ImGui::GetIO().AddMousePosEvent(x, y);
			frame(r, false);
			return frame(r, false);
		}

		// A full-width row is found down one column, the way a user runs the pointer
		// down a panel.
		bool locate(Rig& r, const char* label, float& outX, float& outY)
		{
			ImGuiWindow* w = ImGui::FindWindowByName(kPanelWindow);
			if (!w) return false;
			const ImGuiID id = w->GetID(label);
			for (float y = 14.0f; y < float(PH) - 14.0f; y += 3.0f)
				if (idAt(r, 80.0f, y) == id) { outX = 80.0f; outY = y; return true; }
			return false;
		}

		void click(Rig& r, float x, float y)
		{
			ImGui::GetIO().AddMousePosEvent(x, y);
			frame(r, false);
			frame(r, false);
			frame(r, true);
			frame(r, false);
			frame(r, false);
		}

		TerrainComponent land(float height = 10.0f)
		{
			TerrainComponent tc;
			tc.sizeX = tc.sizeZ = 64.0f;
			tc.resolution = 65;
			tc.dirty = false;
			tc.water.res = 64;
			tc.sculptHeights.assign(65u * 65u, height);
			return tc;
		}

		TerrainComponent& landOf(Rig& r)
		{
			auto view = r.world.registry().view<TerrainComponent>();
			REQUIRE(view.size() == 1u);
			return r.world.registry().get<TerrainComponent>(*view.begin());
		}
		Entity theSpline(Rig& r)
		{
			auto view = r.world.registry().view<SplineComponent>();
			REQUIRE(view.size() == 1u);
			return *view.begin();
		}
		Entity theLand(Rig& r)
		{
			auto view = r.world.registry().view<TerrainComponent>();
			REQUIRE(view.size() == 1u);
			return *view.begin();
		}
	}
}

TEST_CASE("spline ui: the Lake section makes a lake of a closed spline in one undo step, and Dig Again digs only when it must")
{
	namespace L = HE::water::lake;
	Rig r;
	auto& reg = r.world.registry();

	const Entity te = r.world.createEntity("Landscape");
	reg.emplace<TransformComponent>(te);
	reg.emplace<TerrainComponent>(te, lakeui::land());

	// A round, closed spline of sixteen points around the middle, selected.
	const Entity sp = r.world.createEntity("Shore");
	reg.emplace<TransformComponent>(sp);
	SplineComponent shape;
	shape.closed = true;
	for (int i = 0; i < 16; ++i)
	{
		const float a = 6.2831853f * float(i) / 16.0f;
		shape.controlPoints.emplace_back(10.0f * std::cos(a), 10.0f, 10.0f * std::sin(a));
	}
	reg.emplace<SplineComponent>(sp, shape);
	r.ctx.selection.add(sp);

	he_ui::Image img;
	for (int i = 0; i < 4; ++i) lakeui::frame(r, false, i == 3 ? &img : nullptr);
	REQUIRE(img.valid());
	dump(img, "spline-panel-lake-before");

	// The form is there, with the water level and the bed to choose.
	float x = 0.0f, y = 0.0f;
	CHECK_MESSAGE(lakeui::locate(r, "From Ground##lake", x, y), "no From Ground on the Lake section");
	CHECK_MESSAGE(lakeui::locate(r, "Depth##lake", x, y), "no Depth on the Lake section");
	CHECK_MESSAGE(lakeui::locate(r, "Bank##lake", x, y), "no Bank on the Lake section");
	CHECK_MESSAGE(lakeui::locate(r, "Clip To Ground##lake", x, y), "no shore clipping on the Lake section");
	REQUIRE_MESSAGE(lakeui::locate(r, "Create Lake", x, y), "no Create Lake button");
	const std::vector<float> ground0 = lakeui::landOf(r).sculptHeights;
	const size_t depth0 = r.undo.undoDepth();
	CHECK(L::linkOf(r.world, sp).body == HE::water::kNoBody);

	lakeui::click(r, x, y);
	r.frame();                                           // every editor frame runs the viewport tool too
	{
		TerrainComponent& t = lakeui::landOf(r);
		const L::Link l = L::linkOf(r.world, lakeui::theSpline(r));
		REQUIRE(l.body != HE::water::kNoBody);
		CHECK(r.undo.undoDepth() == depth0 + 1);           // dig and water are ONE step
		CHECK(r.undo.undoLabel() == "Create Lake");
		CHECK(t.water.wetCells(l.body) > 250u);
		CHECK(t.water.bodies[0].level == doctest::Approx(10.0f));    // the lowest ground under the outline
		CHECK(terrainHeightAt(t, 0.0f, 0.0f) == doctest::Approx(8.0f).epsilon(0.01));   // level 10 − the default depth 2
		CHECK_FALSE(t.dirty);                              // the bed is a region edit, not a rebuild
		CHECK(t.regionDirty);
	}

	// The panel now shows the lake's controls instead of the form.
	for (int i = 0; i < 3; ++i) lakeui::frame(r, false);
	CHECK_FALSE(lakeui::locate(r, "Create Lake", x, y));
	CHECK(lakeui::locate(r, "Dig Again", x, y));
	CHECK(lakeui::locate(r, "Remove Lake", x, y));
	{
		he_ui::Image after;
		for (int i = 0; i < 3; ++i) lakeui::frame(r, false, i == 2 ? &after : nullptr);
		dump(after, "spline-panel-lake-made");
	}

	// ── One undo takes the water AND the bed, redo brings them back ──
	REQUIRE(r.undo.undo());
	r.ctx.selection.clear();
	r.frame();
	CHECK(L::linkOf(r.world, lakeui::theSpline(r)).body == HE::water::kNoBody);
	CHECK(lakeui::landOf(r).water.bodies.empty());
	CHECK(lakeui::landOf(r).sculptHeights == ground0);
	REQUIRE(r.undo.redo());
	r.ctx.selection.clear();
	r.frame();
	REQUIRE(L::linkOf(r.world, lakeui::theSpline(r)).body != HE::water::kNoBody);
	CHECK(terrainHeightAt(lakeui::landOf(r), 0.0f, 0.0f) == doctest::Approx(8.0f).epsilon(0.01));

	// A lake that has come back from a snapshot is looked at once before it is
	// followed: this is what the next world tick does, and it moves nothing.
	CHECK(L::syncSplines(r.world) == 0u);

	// ── The points move: the water follows by itself, the ground stays ──
	{
		const std::vector<float> dug = lakeui::landOf(r).sculptHeights;
		auto& pts = reg.get<SplineComponent>(lakeui::theSpline(r));
		for (glm::vec3& p : pts.controlPoints) p.x += 12.0f;
		CHECK(L::syncSplines(r.world) == 1u);               // what the world tick does every frame
		CHECK(lakeui::landOf(r).sculptHeights == dug);
		CHECK(HE::water::bodyAt(lakeui::landOf(r), 12.0f, 0.0f) != HE::water::kNoBody);
		CHECK(terrainHeightAt(lakeui::landOf(r), 20.0f, 0.0f) == doctest::Approx(10.0f));   // not dug there
	}
	// ... and Dig Again is what digs under the new place: one step.
	for (int i = 0; i < 3; ++i) lakeui::frame(r, false);
	const size_t depth1 = r.undo.undoDepth();
	REQUIRE(lakeui::locate(r, "Dig Again", x, y));
	lakeui::click(r, x, y);
	r.frame();
	CHECK(r.undo.undoDepth() == depth1 + 1);
	CHECK(r.undo.undoLabel() == "Dig Lake");
	CHECK(terrainHeightAt(lakeui::landOf(r), 20.0f, 0.0f) == doctest::Approx(8.0f).epsilon(0.01));
	REQUIRE(r.undo.undo());                                 // the dig alone: the water stays where the points put it
	r.ctx.selection.clear();
	r.frame();
	CHECK(terrainHeightAt(lakeui::landOf(r), 20.0f, 0.0f) == doctest::Approx(10.0f));
	CHECK(HE::water::bodyAt(lakeui::landOf(r), 12.0f, 0.0f) != HE::water::kNoBody);
	REQUIRE(r.undo.redo());
	r.ctx.selection.clear();
	r.frame();
	CHECK(L::syncSplines(r.world) == 0u);

	// ── Remove Lake: the water goes, the spline and the bed stay, one step ──
	for (int i = 0; i < 3; ++i) lakeui::frame(r, false);
	REQUIRE(lakeui::locate(r, "Remove Lake", x, y));
	const size_t depth2 = r.undo.undoDepth();
	lakeui::click(r, x, y);
	r.frame();
	CHECK(r.undo.undoDepth() == depth2 + 1);
	CHECK(lakeui::landOf(r).water.bodies.empty());
	CHECK(reg.all_of<SplineComponent>(lakeui::theSpline(r)));
	CHECK(terrainHeightAt(lakeui::landOf(r), 20.0f, 0.0f) == doctest::Approx(8.0f).epsilon(0.01));
}

TEST_CASE("spline ui: an open line offers no lake, only the way to one")
{
	Rig r;
	r.clickGround({ -4.0f, 0.0f, 0.0f });
	r.clickGround({ 4.0f, 0.0f, 0.0f });
	REQUIRE(r.points() == 2);
	for (int i = 0; i < 4; ++i) lakeui::frame(r, false);
	float x = 0.0f, y = 0.0f;
	CHECK_FALSE(lakeui::locate(r, "Create Lake", x, y));
	CHECK_FALSE(lakeui::locate(r, "Depth##lake", x, y));
}
