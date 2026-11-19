#include "doctest.h"
#include "ImGuiSoftwareRaster.h"
#include "TestFsUtil.h"

#include "StaticMeshEditorPanel.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"

#include <ContentManager/ContentManager.h>

#include <imgui.h>
#include <imgui_internal.h>   // GImGui->Windows, GetHoveredID

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

// ── The Mesh Viewer's "Export bake" column, driven headless ──────────────────
// The three checkboxes write MeshBakeSettings into the asset and save it at once. What matters is
// that a click reaches the disk: a fresh load of the .hasset carries the switch. With HE_UI_DUMP_DIR
// set the frames are written out. No renderer: the 3D view says so and the left pane is all this
// looks at.

using namespace HE::Ed;
namespace fs = std::filesystem;

namespace
{
	constexpr int W = 1100, H = 700;

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
		}
		~Harness() { ImGui::DestroyContext(); }
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

	struct TempDir
	{
		fs::path path;
		explicit TempDir(const char* name)
		{
			path = fs::temp_directory_path() / name;
			he_test::removeAllQuiet(path);
			fs::create_directories(path);
		}
		~TempDir() { he_test::removeAllQuiet(path); }
	};

	ImGuiID frame(AppContext& ctx, const std::string& meshPath, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		StaticMeshEditorPanel::render(ctx, meshPath, ImVec2(0, 0), ImVec2(float(W), float(H)));
		EditorWidgets::drawQueuedHelp();
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		return hovered;
	}

	ImGuiID idAt(AppContext& ctx, const std::string& p, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame(ctx, p, false);
		return frame(ctx, p, false);
	}

	void clickAt(AppContext& ctx, const std::string& p, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame(ctx, p, false);
		frame(ctx, p, false);
		frame(ctx, p, true);
		frame(ctx, p, false);
		frame(ctx, p, false);
	}

	// The left column's child window: the checkboxes' ids are hashed in its scope.
	ImGuiWindow* infoPane()
	{
		for (ImGuiWindow* w : GImGui->Windows)
			if (w->Active && std::string(w->Name).find("##smInfo") != std::string::npos) return w;
		return nullptr;
	}

	// Walks the column until the pointer hovers `label`'s checkbox, then clicks it.
	bool clickCheckbox(AppContext& ctx, const std::string& p, const char* label)
	{
		ImGuiWindow* pane = infoPane();
		if (!pane) return false;
		const ImGuiID want = ImHashStr(label, 0, pane->ID);
		for (float y = pane->Pos.y + 4.0f; y < pane->Pos.y + pane->Size.y; y += 3.0f)
			for (float dx = 8.0f; dx < 60.0f; dx += 10.0f)
				if (idAt(ctx, p, pane->Pos.x + dx, y) == want)
				{
					clickAt(ctx, p, pane->Pos.x + dx, y);
					return true;
				}
		return false;
	}

	void dump(const he_ui::Image& img, const char* name)
	{
		if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
			he_ui::writeBmp(img, std::string(d) + "/" + name + ".bmp");
	}

	// One mesh, two quads on two materials; returns its absolute path.
	std::string saveMesh(const fs::path& root)
	{
		ContentManager cm(root.string());
		StaticMeshAsset mesh;
		mesh.type = HE::AssetType::StaticMesh; mesh.name = "Two"; mesh.path = "Meshes/Two.hasset";
		mesh.vertices = { 0,0,0,  1,0,0,  1,1,0,  0,1,0,   2,0,0,  3,0,0,  3,1,0,  2,1,0 };
		mesh.normals  = { 0,0,1,  0,0,1,  0,0,1,  0,0,1,   0,0,1,  0,0,1,  0,0,1,  0,0,1 };
		mesh.uvs      = { 0,0,  1,0,  1,1,  0,1,   0,0,  1,0,  1,1,  0,1 };
		mesh.indices  = { 0,1,2, 0,2,3,   4,5,6, 4,6,7 };
		mesh.materialPath = "Mats/A.hasset";
		mesh.sections = { { 0, 6, "Mats/A.hasset", {} }, { 6, 6, "Mats/B.hasset", {} } };
		REQUIRE(cm.saveAsset(mesh));
		return (root / mesh.path).string();
	}

	MeshBakeSettings bakeOnDisk(const fs::path& root)
	{
		ContentManager cm(root.string());
		const StaticMeshAsset* m = cm.getStaticMesh(cm.loadAsset("Meshes/Two.hasset"));
		REQUIRE(m != nullptr);
		return m->bake;
	}
}

TEST_CASE("static mesh editor ui: each Export bake checkbox writes its switch into the .hasset")
{
	TempDir dir("he_test_smesh_ui_bake");
	const fs::path root = dir.path / "Content";
	const std::string meshPath = saveMesh(root);
	REQUIRE(bakeOnDisk(root).mergeTextures);    // the defaults: merge and UV texture on, vertex colours off
	REQUIRE(bakeOnDisk(root).uvTexture);
	REQUIRE_FALSE(bakeOnDisk(root).vertexColors);

	Harness harness;
	ContextBits bits;
	AppContext ctx = bits.make();
	ContentManager cm(root.string());
	ctx.contentManager = &cm;

	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	he_ui::Image img;
	for (int i = 0; i < 4; ++i) frame(ctx, meshPath, false, i == 3 ? &img : nullptr);
	REQUIRE(infoPane() != nullptr);
	dump(img, "static-mesh-export-bake-before");

	REQUIRE(clickCheckbox(ctx, meshPath, "Merge textures"));
	for (int i = 0; i < 3; ++i) frame(ctx, meshPath, false);
	CHECK_FALSE(bakeOnDisk(root).mergeTextures);
	CHECK(bakeOnDisk(root).uvTexture);          // the other two are untouched

	REQUIRE(clickCheckbox(ctx, meshPath, "Vertex colors"));
	for (int i = 0; i < 3; ++i) frame(ctx, meshPath, false);
	CHECK(bakeOnDisk(root).vertexColors);

	REQUIRE(clickCheckbox(ctx, meshPath, "One UV texture"));
	for (int i = 0; i < 3; ++i) frame(ctx, meshPath, false, i == 2 ? &img : nullptr);
	dump(img, "static-mesh-export-bake-after");
	const MeshBakeSettings b = bakeOnDisk(root);
	CHECK_FALSE(b.mergeTextures);
	CHECK(b.vertexColors);
	CHECK_FALSE(b.uvTexture);

	// And back: a second click on the same box restores it.
	REQUIRE(clickCheckbox(ctx, meshPath, "Merge textures"));
	for (int i = 0; i < 3; ++i) frame(ctx, meshPath, false);
	CHECK(bakeOnDisk(root).mergeTextures);
}
