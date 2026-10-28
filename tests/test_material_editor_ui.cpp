#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "MaterialEditorPanel.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorToolbar.h"       // kWarn, the amber the cell and the notes are drawn in
#include "EditorUndo.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <HorizonScene/HorizonWorld.h>
#include <MaterialGraph/MaterialGraph.h>
#include <Renderer/IRenderer.h>

#include <imgui.h>
#include <imgui_internal.h>      // the open-popup stack, a window's ID stack

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

// ── The Material Editor's Tessellation cell (Thema 207, step 5) ───────────────
// While the Output node's Displacement pin is wired, the material bar carries a
// "Tessellation" cell whose popup holds the settings. On a backend that
// tessellates it is a plain cell; where the pin has no effect (Metal, Software)
// it turns amber, the popup says why and the preview says "Displacement not
// shown on …". Step 3 could only compile that; this drives the REAL panel
// headless on the SHIPPED example material (Engine/Materials/DisplacementDemo),
// clicks the cell like a pointer would and looks at the result:
//
//     HE_UI_DUMP_DIR=/tmp/ui ./he_tests --source-file='*test_material_editor_ui.cpp'
//
// The renderer is a stand-in that answers SupportsTessellation() and hands out
// a shaded disc as the preview: what is under test is the editor, not a GPU.

#ifdef HE_EDITOR_DEPS_DIR
namespace
{
namespace fs = std::filesystem;

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
		const std::string font = std::string(HE_EDITOR_DEPS_DIR) + "/Fonts/Roboto_Condensed-Bold.ttf";
		if (std::FILE* f = std::fopen(font.c_str(), "rb"))
		{
			std::fclose(f);
			ImFontConfig cfg;
			cfg.OversampleH = 2;
			cfg.OversampleV = 2;
			io.FontDefault = io.Fonts->AddFontFromFileTTF(font.c_str(), 15.0f, &cfg);
		}
		HE::Ed::applyHorizonDarkTheme();
	}
	~Harness() { ImGui::DestroyContext(); }
};

// The renderer's half of the question, and a preview picture to draw the note on.
class PreviewRenderer final : public IRenderer
{
public:
	explicit PreviewRenderer(bool tessellates) : m_tess(tessellates)
	{
		// A lit disc, so the shot reads as the preview ball it stands in for.
		constexpr int S = 256;
		std::vector<uint8_t> px(size_t(S) * S * 4, 0);
		for (int y = 0; y < S; ++y)
			for (int x = 0; x < S; ++x)
			{
				const float nx = (x - S * 0.5f) / (S * 0.42f), ny = (y - S * 0.5f) / (S * 0.42f);
				const float r2 = nx * nx + ny * ny;
				if (r2 > 1.0f) continue;
				const float nz = std::sqrt(1.0f - r2);
				const float l  = std::max(0.0f, -0.45f * nx - 0.55f * ny + 0.70f * nz);
				uint8_t* p = &px[(size_t(y) * S + x) * 4];
				p[0] = uint8_t(40 + 150 * l); p[1] = uint8_t(30 + 120 * l); p[2] = uint8_t(22 + 90 * l);
				p[3] = 255;
			}
		m_preview = he_ui::registerTexture(px.data(), S, S);
	}
	~PreviewRenderer() override { he_ui::unregisterTexture(m_preview); }

	void Initialize(HE::Window*) override {}
	void Shutdown() override {}
	void Render() override {}
	Capabilities GetCapabilities() const override { return {}; }
	void* CreateImGuiTexture(const void* rgba, int w, int h) override
	{
		return reinterpret_cast<void*>(static_cast<uintptr_t>(he_ui::registerTexture(rgba, w, h)));
	}
	void DestroyImGuiTexture(void* t) override
	{
		he_ui::unregisterTexture(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(t)));
	}
	bool SupportsTessellation() const override { return m_tess; }
	void* RenderMaterialPreview(ContentManager&, const HE::UUID&, uint32_t, float, float, float,
	                            int, const HE::UUID&, float) override
	{
		++previews;
		return reinterpret_cast<void*>(static_cast<uintptr_t>(m_preview));
	}

	int previews = 0;

private:
	bool        m_tess;
	ImTextureID m_preview = 0;
};

struct ContextBits
{
	EditorConfig    config;
	bool            vsync = false;
	std::string     backendName;
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

	AppContext make(HorizonWorld& world, EditorUndo& undo)
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
		return ctx;
	}
};

fs::path freshDir()
{
	std::mt19937_64 rng{ std::random_device{}() };
	const fs::path d = fs::temp_directory_path() /
		("he_mated_ui_" + std::to_string(rng() & 0xFFFFFFu));
	fs::create_directories(d / "Materials");
	return d;
}

// The shipped example, copied into a throwaway project: nothing here can write
// to EditorDeps. `unwire` drops the link into the Displacement pin first and
// saves the result (the negative control: no wire, no cell).
struct Project
{
	fs::path       root = freshDir();
	ContentManager content;
	std::string    abs;

	explicit Project(bool unwire)
	{
		const fs::path shipped = fs::path(HE_EDITOR_DEPS_DIR) / "EngineContent" / "Materials" / "DisplacementDemo.hasset";
		REQUIRE(fs::exists(shipped));
		const fs::path dst = root / "Materials" / "DisplacementDemo.hasset";
		REQUIRE(fs::copy_file(shipped, dst));
		content.setContentRoot(root.string());
		abs = dst.string();
		if (unwire)
		{
			const HE::UUID id = content.loadAsset("Materials/DisplacementDemo.hasset");
			MaterialAsset* m = content.getMaterialMutable(id);
			REQUIRE(m);
			HE::MaterialGraph g;
			REQUIRE(HE::materialGraphFromJson(m->nodeGraphJson, g));
			for (const HE::MatGraphNode& n : g.nodes)
				if (n.type == HE::MatNodeType::Output) g.disconnectInput(n.id, HE::kMatOutputDisplacementPin);
			m->nodeGraphJson = HE::materialGraphToJson(g);
			content.regenerateMaterialFromGraph(id);
			m = content.getMaterialMutable(id);
			REQUIRE(m);
			REQUIRE(m->customShaderDispGlsl.empty());
			REQUIRE(content.saveAsset(*m));
		}
	}
	~Project()
	{
		MaterialEditorPanel::forget(abs);
		std::error_code ec;
		fs::remove_all(root, ec);
	}
};

ImGuiID frame(AppContext& ctx, const std::string& path, bool mouseDown, he_ui::Image* shot = nullptr)
{
	ImGuiIO& io = ImGui::GetIO();
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
	ImGui::NewFrame();
	MaterialEditorPanel::render(ctx, path, ImVec2(0.0f, 0.0f), ImVec2(float(W), float(H)));
	const ImGuiID hovered = ImGui::GetHoveredID();
	ImGui::Render();
	if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
	return hovered;
}

ImGuiID hoverAt(AppContext& ctx, const std::string& path, float x, float y)
{
	ImGui::GetIO().AddMousePosEvent(x, y);
	frame(ctx, path, false);
	return frame(ctx, path, false);
}

// The id an item called `label` gets in the tab's window — the bar's cells sit
// directly in it (EditorToolbar::cellImpl, InvisibleButton).
ImGuiID tabId(const char* label)
{
	ImGuiWindow* w = ImGui::FindWindowByName("##MaterialGraphTab");
	REQUIRE(w);
	return w->GetID(label);
}

// Where the pointer has to be for `id` to be hovered: the bar is one row of
// cells, so find the row on the Save cell at the left edge, then walk right.
bool findCell(AppContext& ctx, const std::string& path, ImGuiID id, ImVec2& at)
{
	float rowY = -1.0f;
	for (float y = 2.0f; y < 80.0f && rowY < 0.0f; y += 2.0f)
		for (float x = 6.0f; x < 120.0f; x += 12.0f)
			if (hoverAt(ctx, path, x, y) != 0) { rowY = y + 4.0f; break; }
	if (rowY < 0.0f) return false;
	for (float x = 2.0f; x < float(W); x += 4.0f)
		if (hoverAt(ctx, path, x, rowY) == id) { at = ImVec2(x + 6.0f, rowY); return true; }
	return false;
}

ImGuiID openPopupId()
{
	const ImGuiContext& g = *ImGui::GetCurrentContext();
	return g.OpenPopupStack.Size > 0 ? g.OpenPopupStack[0].PopupId : 0;
}

ImRect openPopupRect()
{
	const ImGuiContext& g = *ImGui::GetCurrentContext();
	if (g.OpenPopupStack.Size == 0 || !g.OpenPopupStack[0].Window) return ImRect();
	const ImGuiWindow* w = g.OpenPopupStack[0].Window;
	return ImRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y));
}

// Pixels in `r` drawn in the toolbar's amber (kWarn = 230,176,86). Tight on
// purpose: a comment frame's corner on the canvas (222,183,97) is close enough
// to pass a looser test, and it is no warning.
int amberIn(const he_ui::Image& img, ImRect r)
{
	int n = 0;
	for (int y = std::max(0, int(r.Min.y)); y < std::min(img.height, int(r.Max.y)); ++y)
		for (int x = std::max(0, int(r.Min.x)); x < std::min(img.width, int(r.Max.x)); ++x)
		{
			uint8_t R, G, B, A;
			img.pixel(x, y, R, G, B, A);
			if (std::abs(R - 230) < 12 && std::abs(G - 176) < 9 && std::abs(B - 86) < 10) ++n;
		}
	return n;
}

void dump(const he_ui::Image& img, const char* name)
{
	if (const char* dir = std::getenv("HE_UI_DUMP_DIR"))
	{
		fs::create_directories(dir);
		CHECK(he_ui::writeBmp(img, (fs::path(dir) / name).string()));
	}
}

struct Shots { he_ui::Image bar, popup; ImRect cell, popupRect; ImGuiID popupId = 0; int previews = 0; };

// Open the tab, find the Tessellation cell, shoot the bar, click the cell, shoot
// the popup. Nothing here forces the popup open: the click has to do it.
Shots drive(Project& p, bool tessellates, HE::RendererBackend backend)
{
	Harness h;
	HorizonWorld world;
	EditorUndo   undo;
	ContextBits  bits;
	bits.backendName = backend == HE::RendererBackend::D3D12 ? "D3D12" : "Metal";
	AppContext ctx = bits.make(world, undo);
	PreviewRenderer renderer(tessellates);
	ctx.renderer       = &renderer;
	ctx.backend        = backend;
	ctx.contentManager = &p.content;

	Shots s;
	for (int i = 0; i < 3; ++i) frame(ctx, p.abs, false);
	ImVec2 at;
	REQUIRE(findCell(ctx, p.abs, tabId("##tess"), at));
	s.cell = ImRect(ImVec2(at.x - 40.0f, at.y - 14.0f), ImVec2(at.x + 90.0f, at.y + 14.0f));

	// The bar at rest, pointer parked off the panel.
	ImGui::GetIO().AddMousePosEvent(float(W) - 4.0f, float(H) - 4.0f);
	for (int i = 0; i < 3; ++i) frame(ctx, p.abs, false, i == 2 ? &s.bar : nullptr);

	ImGui::GetIO().AddMousePosEvent(at.x, at.y);
	frame(ctx, p.abs, false);
	frame(ctx, p.abs, true);
	frame(ctx, p.abs, false);
	frame(ctx, p.abs, false, &s.popup);
	s.popupId   = openPopupId();
	s.popupRect = openPopupRect();
	CHECK(s.popupId == tabId("##tessPopup"));
	s.previews = renderer.previews;
	return s;
}
} // namespace

TEST_CASE("material editor ui: Tessellation cell on a tessellating backend (D3D12)")
{
	Project p(false);
	const Shots s = drive(p, true, HE::RendererBackend::D3D12);
	dump(s.bar,   "material_editor_tess_d3d12_bar.bmp");
	dump(s.popup, "material_editor_tess_d3d12_popup.bmp");
	CHECK(s.previews > 0);
	// A plain cell, a grey "Tessellated on D3D12" line: no amber anywhere near.
	CHECK(amberIn(s.bar, s.cell) == 0);
	REQUIRE(s.popupRect.GetWidth() > 0.0f);
	CHECK(amberIn(s.popup, s.popupRect) == 0);
	// No "Displacement not shown" over the preview either: the whole frame is
	// free of amber (the bar holds no other warning for this material).
	CHECK(amberIn(s.bar, ImRect(0.0f, 0.0f, float(W), float(H))) == 0);
}

TEST_CASE("material editor ui: Tessellation cell where the pin has no effect (Metal)")
{
	Project p(false);
	const Shots s = drive(p, false, HE::RendererBackend::Metal);
	dump(s.bar,   "material_editor_tess_metal_bar.bmp");
	dump(s.popup, "material_editor_tess_metal_popup.bmp");
	// The cell is tinted, the popup's first line is the amber "Not tessellated on
	// Metal" warning, and the preview carries its note.
	CHECK(amberIn(s.bar, s.cell) > 20);
	REQUIRE(s.popupRect.GetWidth() > 0.0f);
	CHECK(amberIn(s.popup, s.popupRect) > 50);
	const int all  = amberIn(s.bar, ImRect(0.0f, 0.0f, float(W), float(H)));
	const int cell = amberIn(s.bar, s.cell);
	CHECK(all - cell > 50);   // the preview note, away from the bar
}

TEST_CASE("material editor ui: no Displacement wire, no Tessellation cell")
{
	Project p(true);
	Harness h;
	HorizonWorld world;
	EditorUndo   undo;
	ContextBits  bits;
	bits.backendName = "Metal";
	AppContext ctx = bits.make(world, undo);
	PreviewRenderer renderer(false);
	ctx.renderer       = &renderer;
	ctx.backend        = HE::RendererBackend::Metal;
	ctx.contentManager = &p.content;
	for (int i = 0; i < 3; ++i) frame(ctx, p.abs, false);
	ImVec2 at;
	CHECK_FALSE(findCell(ctx, p.abs, tabId("##tess"), at));
	// …while the walk itself works: the folder cell at the left edge is found.
	CHECK(findCell(ctx, p.abs, tabId("##revealAsset"), at));
	he_ui::Image img;
	ImGui::GetIO().AddMousePosEvent(float(W) - 4.0f, float(H) - 4.0f);
	for (int i = 0; i < 3; ++i) frame(ctx, p.abs, false, i == 2 ? &img : nullptr);
	CHECK(amberIn(img, ImRect(0.0f, 0.0f, float(W), float(H))) == 0);
}
#endif // HE_EDITOR_DEPS_DIR
