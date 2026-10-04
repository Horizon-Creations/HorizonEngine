#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "UIEditorPanel.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorUndo.h"
#include "EditorWidgets.h"
#include "AssetThumbnailCache.h"
#include "TextureImporter.h"
#include "fixtures/catania_startup_107.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <HorizonScene/HorizonWorld.h>
#include <Renderer/IRenderer.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetAnim.h>
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/WidgetManager.h>

#include <imgui.h>
#include <imgui_internal.h>   // the window list, to find the hierarchy's rows
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ── The widget designer, driven headless ─────────────────────────────────────
// The real UIEditorPanel::render with a real AppContext and a real widget asset,
// in a headless ImGui context, rasterised by the software renderer the other
// "ui shot" scenes use. Written for Thema 92 (the designer's Details panel is
// to become easier to read): so the panel as it IS can be looked at, and so
// every intermediate state of that work can be shown as a picture before it
// lands, not described.
//
//     HE_UI_DUMP_DIR=/tmp/ui ./he_tests -tc="ui shot: widget designer*"
//     scripts/he_uishot.py OUT --filter "ui shot: widget designer*"
//
// What it cannot show by default: the texture on the canvas. The designer draws
// it through AssetThumbnailCache::image, which needs a renderer to upload into;
// headless there is none, so the Image element draws its empty placeholder. The
// DETAILS side, which is what these shots are for, is complete. The Thema 107
// cases at the bottom hand the cache a stub renderer whose uploads land in the
// software rasterizer, so there the canvas shows exactly the picture it gets.

using namespace HE::Ed;

namespace
{
	// Wide enough for the designer's three columns (230 + canvas + 300), and
	// tall enough that the Details column of a selected Image fits without
	// scrolling — the length of that column is half of what is being judged.
	constexpr int W = 1280, H = 1600;

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

	// Everything AppContext insists on being given (same shape as
	// test_inspector_ui.cpp).
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

	// A small but typical page: a background panel, a picture, a title and a
	// button. The picture is the element the Thema is about.
	HE::UIWidgetTree samplePage()
	{
		HE::UIWidgetTree t;
		t.canvasWidth = 1280.0f; t.canvasHeight = 720.0f;
		auto place = [&](HE::UIWidgetType type, const char* name,
		                 float x, float y, float w, float h) -> HE::UIElement&
		{
			HE::UIElement& e = *t.find(t.add(type));
			e.name = name;
			HE::uiSetAnchorPreset(e, 0);
			e.pivotX = e.pivotY = 0.0f;
			e.posX = x; e.posY = y; e.sizeX = w; e.sizeY = h;
			return e;
		};
		place(HE::UIWidgetType::Panel,  "Background", 0.0f,   0.0f,   1280.0f, 720.0f);
		HE::UIElement& logo = place(HE::UIWidgetType::Image, "Logo", 540.0f, 120.0f, 200.0f, 200.0f);
		logo.texture = "Textures/Logo.hasset";
		place(HE::UIWidgetType::Text,   "Title",      440.0f, 360.0f, 400.0f, 60.0f);
		place(HE::UIWidgetType::Button, "Start",      540.0f, 460.0f, 200.0f, 56.0f);
		return t;
	}

	struct Designer
	{
		AppContext& ctx;
		std::string assetPath;   // absolute, the key the panel's state is held by
		int         height = H;  // taller for the shot with every section open
		int         width  = W;  // wider where the timeline's whole bar has to fit
		// Called before every frame — what the editor's own loop does around the
		// panel (the thumbnail cache's per-frame budget, Thema 107).
		void (*beforeFrame)() = nullptr;

		// One frame of the designer filling the whole screen. Returns the id
		// ImGui says the pointer is on; rasterises into `shot` when given one.
		ImGuiID frame(bool left, he_ui::Image* shot = nullptr)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(float(width), float(height));
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, left);
			if (beforeFrame) beforeFrame();
			ImGui::NewFrame();
			UIEditorPanel::render(ctx, assetPath, ImVec2(0.0f, 0.0f),
			                      ImVec2(float(width), float(height)));
			EditorWidgets::drawQueuedHelp();
			const ImGuiID hovered = ImGui::GetHoveredID();
			ImGui::Render();
			if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), width, height);
			return hovered;
		}

		// Open the Details panel's folding sections by name, the way a click on
		// each header would: ImGui keeps a header's open state in its window's
		// storage, under the label hashed into the window's id. `inside` names
		// the section a nested node sits in (the Image's 9-Slice sits in
		// "Image", which pushes its own id scope).
		bool openSection(const char* label, const char* inside = nullptr)
		{
			for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
				if (std::strstr(w->Name, "##uiw_details"))
				{
					const ImGuiID seed = inside ? ImHashStr(inside, 0, w->ID) : w->ID;
					w->StateStorage.SetInt(ImHashStr(label, 0, seed), 1);
					return true;
				}
			return false;
		}

		// The id a row of the hierarchy tree carries: its label hashed into the
		// tree child's own id seed. Roots are drawn straight into that child,
		// without a PushID between, so this is the whole derivation.
		ImGuiID hierarchyRowId(const std::string& label) const
		{
			for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
				if (std::strstr(w->Name, "##uiw_tree"))
					return ImHashStr(label.c_str(), 0, w->ID);
			return 0;
		}

		// Click the row whose id is `wanted`, found by walking the pointer down
		// the hierarchy column. False when the walk never met it.
		bool clickRow(ImGuiID wanted)
		{
			if (wanted == 0) return false;
			ImGuiIO& io = ImGui::GetIO();
			for (float y = 40.0f; y < float(H) - 20.0f; y += 3.0f)
			{
				io.AddMousePosEvent(60.0f, y);
				frame(false);
				if (frame(false) != wanted) continue;
				frame(true);
				frame(false);
				frame(false);
				io.AddMousePosEvent(-1000.0f, -1000.0f);   // out of the way of the shot
				frame(false);
				frame(false);
				return true;
			}
			return false;
		}

		he_ui::Image shoot(const char* name)
		{
			he_ui::Image img;
			for (int i = 0; i < 3; ++i) frame(false, i == 2 ? &img : nullptr);
			if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
				he_ui::writeBmp(img, std::string(dir) + "/" + name + ".bmp");
			return img;
		}
	};

	std::filesystem::path tempRoot()
	{
		return std::filesystem::temp_directory_path() / "he_widget_designer_ui";
	}
}

TEST_CASE("ui shot: widget designer — the Details panel as it is (Thema 92)")
{
	Harness harness;

	const std::filesystem::path root = tempRoot();
	std::filesystem::create_directories(root / "UI");
	ContentManager cm;
	cm.setContentRoot(root.string());

	UIWidgetAsset asset;
	asset.name     = "MainMenu";
	asset.path     = "UI/MainMenu.hasset";
	asset.treeJson = HE::uiWidgetTreeToJson(samplePage());
	REQUIRE(cm.registerWidget(std::move(asset)) != HE::UUID{});

	HorizonWorld world;
	EditorUndo   undo;
	ContextBits  bits;
	AppContext   ctx = bits.make(world, undo);
	ctx.contentManager = &cm;

	Designer d{ ctx, (root / "UI" / "MainMenu.hasset").string() };

	SUBCASE("nothing selected: the canvas settings")
	{
		const he_ui::Image img = d.shoot("widget-designer-canvas");
		REQUIRE(img.valid());
		CHECK(img.inkedPixels(20, 18, 15) > 10000);
	}

	SUBCASE("the Image selected: its Details column in full")
	{
		d.shoot("widget-designer-warmup");   // lay the hierarchy out once
		const ImGuiID logoRow = d.hierarchyRowId("Logo##hn2");
		REQUIRE(logoRow != 0);
		REQUIRE(d.clickRow(logoRow));
		const he_ui::Image img = d.shoot("widget-designer-image-details");
		REQUIRE(img.valid());
		CHECK(img.inkedPixels(20, 18, 15) > 10000);

		// …and with every section unfolded, so the regrouping can be judged
		// as a whole and not only by what is open on arrival. Taller, because
		// that is the point: this is the length the folding saves.
		for (const char* s : { "Surface", "Material", "Interaction", "Events" })
			REQUIRE(d.openSection(s));
		REQUIRE(d.openSection("###nineslice", "Image"));
		d.height = 2300;
		const he_ui::Image all = d.shoot("widget-designer-image-details-all-open");
		REQUIRE(all.valid());
		CHECK(all.inkedPixels(20, 18, 15) > 10000);
	}

	UIEditorPanel::forget(d.assetPath);
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
}

// ── Thema 107: the reported designer bugs ────────────────────────────────────
// Written as reproductions in the diagnosis (Schritt 1, see
// docs/widget-designer-bugs-107-diagnose.md); the canvas one is flipped into
// the fix's regression test (Schritt 2).
namespace
{
	// Every texture the thumbnail cache uploads, and at what size.
	std::vector<std::pair<int, int>> g_uploads107;
	// The bytes of the last upload of at least 1024 px — the full image.
	std::vector<uint8_t> g_fullUpload107;
	int g_fullW107 = 0, g_fullH107 = 0;

	// A renderer that renders nothing and uploads into the software rasterizer:
	// enough for AssetThumbnailCache to hand the canvas a texture headless.
	class ThumbUploadRenderer final : public IRenderer
	{
	public:
		void Initialize(HE::Window*) override {}
		void Shutdown() override {}
		void Render() override {}
		Capabilities GetCapabilities() const override { return {}; }
		void* CreateImGuiTexture(const void* rgba, int w, int h) override
		{
			g_uploads107.push_back({ w, h });
			if (w >= 1024 || h >= 1024)
			{
				const auto* p = static_cast<const uint8_t*>(rgba);
				g_fullUpload107.assign(p, p + size_t(w) * h * 4);
				g_fullW107 = w; g_fullH107 = h;
			}
			return reinterpret_cast<void*>(static_cast<uintptr_t>(he_ui::registerTexture(rgba, w, h)));
		}
		void DestroyImGuiTexture(void* t) override
		{
			he_ui::unregisterTexture(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(t)));
		}
	};

	// 32-bit uncompressed TGA, rows top-down (descriptor 0x28).
	void writeTga(const std::filesystem::path& p, int w, int h, const std::vector<uint8_t>& rgba)
	{
		std::vector<uint8_t> f(18, 0);
		f[2]  = 2;
		f[12] = uint8_t(w & 255); f[13] = uint8_t(w >> 8);
		f[14] = uint8_t(h & 255); f[15] = uint8_t(h >> 8);
		f[16] = 32; f[17] = 0x28;
		for (size_t i = 0; i < rgba.size(); i += 4)
		{
			f.push_back(rgba[i + 2]); f.push_back(rgba[i + 1]);
			f.push_back(rgba[i + 0]); f.push_back(rgba[i + 3]);
		}
		std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(f.data()),
		                                         static_cast<std::streamsize>(f.size()));
	}

	// A logo-like 1024² picture: transparent background, an orange disc, and a
	// band of 1-px black/white columns that only survives at full resolution.
	std::vector<uint8_t> logoLikePicture(int S)
	{
		std::vector<uint8_t> px(size_t(S) * S * 4, 0);
		const float c = S * 0.5f, r = S * 0.39f;
		for (int y = 0; y < S; ++y)
			for (int x = 0; x < S; ++x)
			{
				const float dx = x + 0.5f - c, dy = y + 0.5f - c;
				if (dx * dx + dy * dy > r * r) continue;
				uint8_t* p = &px[(size_t(y) * S + x) * 4];
				const bool band = y > S * 0.45f && y < S * 0.55f;
				const uint8_t v = (x & 1) ? 255 : 0;
				p[0] = band ? v : 216; p[1] = band ? v : 128; p[2] = band ? v : 24; p[3] = 255;
			}
		return px;
	}

	int countGrey(const he_ui::Image& img, uint8_t v)
	{
		int n = 0;
		for (size_t i = 0; i + 3 < img.rgba.size(); i += 4)
			if (img.rgba[i] == v && img.rgba[i + 1] == v && img.rgba[i + 2] == v) ++n;
		return n;
	}

	int countColour(const he_ui::Image& img, uint8_t r, uint8_t g, uint8_t b)
	{
		int n = 0;
		for (size_t i = 0; i + 3 < img.rgba.size(); i += 4)
			if (img.rgba[i] == r && img.rgba[i + 1] == g && img.rgba[i + 2] == b) ++n;
		return n;
	}

	// The picture the Thema 107 canvas cases put on the page, staged under
	// `root`: HE_REPRO107_LOGO when it names a texture .hasset (the Catania
	// project's UI/Source/HE_Logo.hasset), else the generated 1024² logo-like
	// picture imported as sRGB. The content-relative path, or "" on failure.
	// `folder` is the content folder it lands in (Catania keeps it in UI/Source).
	std::string stageLogo107(const std::filesystem::path& root, const std::string& folder = "UI")
	{
		namespace fs = std::filesystem;
		fs::create_directories(root / folder);
		fs::create_directories(root / "Src");
		if (const char* logo = std::getenv("HE_REPRO107_LOGO"); logo && *logo && fs::exists(logo))
		{
			fs::copy_file(logo, root / folder / "HE_Logo.hasset", fs::copy_options::overwrite_existing);
			return folder + "/HE_Logo.hasset";
		}
		constexpr int S = 1024;
		writeTga(root / "Src" / "HE_Logo.tga", S, S, logoLikePicture(S));
		TextureImporter::ImportSettings s;
		s.srgb = true;   // what the import dialog guesses for a logo
		if (!TextureImporter::import(root / "Src" / "HE_Logo.tga", root, folder, s)) return {};
		std::string rel;
		for (const auto& e : fs::directory_iterator(root / folder))
			if (e.path().extension() == ".hasset") rel = folder + "/" + e.path().filename().string();
		return rel;
	}
}

// (1) + (4): the canvas used to draw an Image from the Content Browser's 128 px
// tile — checkerboard baked in, alpha forced opaque, 8x downscaled from 1024.
// It now draws the texture itself (AssetThumbnailCache::image): uploaded at its
// own size, straight alpha, and no tile is made for the canvas at all.
//     HE_UI_DUMP_DIR=/tmp/ui ./he_tests -tc="repro 107*"
// HE_REPRO107_LOGO=<path to a texture .hasset> uses that picture instead of the
// generated one (e.g. the Catania project's UI/Source/HE_Logo.hasset).
TEST_CASE("repro 107: designer canvas draws the texture itself, not the thumbnail tile")
{
	Harness harness;
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "he_widget_designer_repro107";
	std::error_code ec;
	fs::remove_all(root, ec);
	const std::string texRel = stageLogo107(root);
	REQUIRE(!texRel.empty());

	ContentManager cm;
	cm.setContentRoot(root.string());

	// The Catania "Startup" page: 1920x1080, the logo 550x550 left of centre.
	HE::UIWidgetTree t;
	t.canvasWidth = 1920.0f; t.canvasHeight = 1080.0f;
	{
		HE::UIElement& e = *t.find(t.add(HE::UIWidgetType::Image));
		e.name = "Logo";
		HE::uiSetAnchorPreset(e, 0);
		e.pivotX = e.pivotY = 0.0f;
		e.posX = 235.0f; e.posY = 265.0f; e.sizeX = 550.0f; e.sizeY = 550.0f;
		e.texture = texRel;
	}
	UIWidgetAsset asset;
	asset.name     = "Startup";
	asset.path     = "UI/Startup.hasset";
	asset.treeJson = HE::uiWidgetTreeToJson(t);
	REQUIRE(cm.registerWidget(std::move(asset)) != HE::UUID{});

	HorizonWorld world;
	EditorUndo   undo;
	ContextBits  bits;
	AppContext   ctx = bits.make(world, undo);
	ctx.contentManager = &cm;

	Designer d{ ctx, (root / "UI" / "Startup.hasset").string() };
	d.beforeFrame = [] { static double now = 0.0; AssetThumbnailCache::beginFrame(now += 1.0 / 60.0); };

	// Negative control: no renderer, no texture — the checker greys are absent.
	AssetThumbnailCache::setContext(nullptr, &cm, "");
	const he_ui::Image none = d.shoot("repro107-canvas-no-texture");
	REQUIRE(none.valid());
	const int ctl90 = countGrey(none, 90), ctl130 = countGrey(none, 130);

	ThumbUploadRenderer stub;
	g_uploads107.clear();
	AssetThumbnailCache::setContext(&stub, &cm, "");
	d.shoot("repro107-warmup");
	const he_ui::Image img = d.shoot("repro107-canvas-image");
	REQUIRE(img.valid());

	// (4) What reached the canvas is the texture at its own size. No tile at all:
	// the Content Browser is not drawn here, so a 128 px upload could only be the
	// canvas borrowing one again.
	const int S = static_cast<int>(AssetThumbnailCache::thumbnailSize());
	REQUIRE(!g_uploads107.empty());
	bool tileSized = false, fullSized = false;
	for (const auto& [w, h] : g_uploads107)
	{
		MESSAGE("upload " << w << "x" << h);
		if (w == S && h == S) tileSized = true;
		if (w >= 1024 || h >= 1024) fullSized = true;
	}
	CHECK_FALSE(tileSized);
	CHECK(fullSized);

	// (1) No checkerboard: the tile's 90/130 greys are not on the canvas — the
	// transparent background lets the page show through.
	const int n90 = countGrey(img, 90), n130 = countGrey(img, 130);
	MESSAGE("checker greys: control " << ctl90 << "/" << ctl130 << ", with image " << n90 << "/" << n130);
	CHECK(n90  < ctl90  + 200);
	CHECK(n130 < ctl130 + 200);

	// The generated picture's own facts, so the checks above cannot pass by the
	// canvas drawing nothing: the upload keeps its alpha (corner transparent,
	// centre opaque orange), and the orange disc is on the canvas.
	if (texRel != "UI/HE_Logo.hasset")
	{
		REQUIRE(g_fullW107 == 1024);
		REQUIRE(g_fullH107 == 1024);
		CHECK(g_fullUpload107[3] == 0);   // (0,0): outside the disc
		const size_t c = (size_t(512) * 1024 + 512) * 4;
		CHECK(g_fullUpload107[c + 3] == 255);
		const size_t o = (size_t(300) * 1024 + 512) * 4;   // above the band
		CHECK(g_fullUpload107[o]     == 216);
		CHECK(g_fullUpload107[o + 1] == 128);
		CHECK(g_fullUpload107[o + 2] == 24);
		const int orange = countColour(img, 216, 128, 24);
		MESSAGE("orange canvas pixels: " << orange << " (control " << countColour(none, 216, 128, 24) << ")");
		CHECK(orange > 5000);
	}

	AssetThumbnailCache::setContext(nullptr, nullptr, "");
	UIEditorPanel::forget(d.assetPath);
	fs::remove_all(root, ec);
}

// (3), the fix: the end state is one click, and a key dropped near the end is
// AT the end. The real timeline, driven headless: open the clip, pick the
// track, press "Key at End"; then drag that key away and back to a few pixels
// short of the end — it snaps onto it, unless Alt is held.
TEST_CASE("repro 107: the timeline puts an end key exactly at the end")
{
	Harness harness;
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "he_widget_designer_timeline107";
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::create_directories(root / "UI");

	ContentManager cm;
	cm.setContentRoot(root.string());
	HE::UIWidgetTree t;
	t.canvasWidth = 1280.0f; t.canvasHeight = 720.0f;
	const int logo = t.add(HE::UIWidgetType::Image);
	t.find(logo)->name = "Logo";
	{
		// The user's clip before its second key: one second, one track, the
		// fade's start at 0.
		HE::UIAnimClip c;
		c.name = "Blend";
		c.duration = 1.0f;
		HE::UIAnimTrack tr;
		tr.element = logo;
		tr.prop    = "Render Opacity";
		tr.keys.push_back({ 0.0f, HE::UIPropValue::ofFloat(0.0f), HE::UIEase::Linear });
		c.tracks.push_back(tr);
		t.animations.push_back(c);
	}
	UIWidgetAsset asset;
	asset.name     = "Anim107";
	asset.path     = "UI/Anim107.hasset";
	asset.treeJson = HE::uiWidgetTreeToJson(t);
	REQUIRE(cm.registerWidget(std::move(asset)) != HE::UUID{});

	HorizonWorld world;
	EditorUndo   undo;
	ContextBits  bits;
	AppContext   ctx = bits.make(world, undo);
	ctx.contentManager = &cm;
	Designer d{ ctx, (root / "UI" / "Anim107.hasset").string() };
	for (int i = 0; i < 3; ++i) d.frame(false);

	ImGuiIO& io = ImGui::GetIO();
	auto windowNamed = [](const char* part) -> ImGuiWindow*
	{
		for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
			if (w->Active && std::strstr(w->Name, part)) return w;
		return nullptr;
	};
	// Walk the pointer over a grid until ImGui says it is on `wanted`. The
	// position it was found at, or (-1,-1).
	auto find = [&](ImGuiID wanted, float x0, float x1, float y0, float y1,
	                float sx, float sy) -> ImVec2
	{
		for (float y = y0; y <= y1; y += sy)
			for (float x = x0; x <= x1; x += sx)
			{
				io.AddMousePosEvent(x, y);
				d.frame(false);
				if (d.frame(false) == wanted) return ImVec2(x, y);
			}
		return ImVec2(-1.0f, -1.0f);
	};
	auto click = [&](ImVec2 p)
	{
		io.AddMousePosEvent(p.x, p.y);
		d.frame(true); d.frame(false); d.frame(false);
	};
	auto keys = [&]() -> std::vector<HE::UIAnimKey>&
	{
		HE::UIWidgetTree* live = UIEditorPanel::liveTree("UI/Anim107.hasset");
		REQUIRE(live);
		REQUIRE(live->animations.size() == 1);
		REQUIRE(live->animations[0].tracks.size() == 1);
		return live->animations[0].tracks[0].keys;
	};

	ImGuiWindow* tw = windowNamed("##uiw_timeline");
	REQUIRE(tw);
	const float wx0 = tw->Pos.x, wx1 = tw->Pos.x + tw->Size.x;
	const float wy0 = tw->Pos.y, wy1 = tw->Pos.y + tw->Size.y;

	// Open "Blend" through the clip combo, as a person would.
	const ImVec2 combo = find(ImHashStr("##clip", 0, tw->ID), wx0, wx0 + 200.0f,
	                          wy0 + 4.0f, wy0 + 30.0f, 8.0f, 4.0f);
	REQUIRE(combo.x >= 0.0f);
	click(combo);
	ImGuiWindow* pop = windowNamed("##Combo_");
	REQUIRE(pop);
	const ImVec2 blend = find(ImHashStr("Blend##c", 0, pop->ID), pop->Pos.x + 10.0f,
	                          pop->Pos.x + 30.0f, pop->Pos.y, pop->Pos.y + pop->Size.y,
	                          10.0f, 2.0f);
	REQUIRE(blend.x >= 0.0f);
	click(blend);

	// The track row: its name, under PushID(0).
	int zero = 0;
	const ImGuiID trackSeed = ImHashData(&zero, sizeof(zero), tw->ID);
	const ImVec2 track = find(ImHashStr("Logo  \xC2\xB7  Render Opacity##t", 0, trackSeed),
	                          wx0 + 20.0f, wx0 + 20.0f, wy0 + 20.0f, wy1, 10.0f, 3.0f);
	REQUIRE(track.x >= 0.0f);
	click(track);

	// "Key at End": the row under the tracks, found by its neighbour first.
	const ImVec2 addTrack = find(ImHashStr("Add Track", 0, tw->ID), wx0 + 10.0f, wx0 + 40.0f,
	                             track.y, wy1, 10.0f, 3.0f);
	REQUIRE(addTrack.x >= 0.0f);
	const ImVec2 keyAtEnd = find(ImHashStr("Key at End", 0, tw->ID), addTrack.x, wx1,
	                             addTrack.y, addTrack.y, 4.0f, 1.0f);
	REQUIRE(keyAtEnd.x >= 0.0f);
	click(keyAtEnd);

	REQUIRE(keys().size() == 2);
	CHECK(keys()[1].time == 1.0f);                     // exactly the length
	CHECK(keys()[1].value.f == doctest::Approx(0.0f)); // holding what was there

	// The end key's diamond: PushID(track 0), PushID(key 1), "##key".
	int one = 1;
	const ImGuiID keyId = ImHashStr("##key", 0, ImHashData(&one, sizeof(one), trackSeed));
	// Press on `from`, carry the pointer `dx` px sideways and — with `back` —
	// all the way back again before letting go.
	auto drag = [&](ImVec2 from, float dx, bool alt, bool back = false)
	{
		io.AddKeyEvent(ImGuiMod_Alt, alt);
		io.AddMousePosEvent(from.x, from.y);
		d.frame(false);
		d.frame(true);
		for (int s = 1; s <= 10; ++s)
		{
			io.AddMousePosEvent(from.x + dx * float(s) / 10.0f, from.y);
			d.frame(true);
		}
		if (back)
			for (int s = 9; s >= 0; --s)
			{
				io.AddMousePosEvent(from.x + dx * float(s) / 10.0f, from.y);
				d.frame(true);
			}
		d.frame(false); d.frame(false);
		io.AddKeyEvent(ImGuiMod_Alt, false);
		d.frame(false);
	};
	// The diamonds sit mid-row; the track's name was found near its top.
	auto findKey = [&]
	{
		return find(keyId, wx0 + 190.0f, wx1, track.y + 8.0f, track.y + 8.0f, 2.0f, 1.0f);
	};

	// Grabbed 3 px right of the diamond's centre and carried 200 px left.
	ImVec2 k = findKey();
	REQUIRE(k.x >= 0.0f);
	k.x += 3.0f;
	drag(k, -200.0f, false);
	const float away = keys()[1].time;
	CHECK(away < 0.9f);
	CHECK(away > 0.1f);

	// Carried back to 3 px short of the end with Alt held: no pull, it stays
	// short. The control for the next step.
	k = findKey();
	REQUIRE(k.x >= 0.0f);
	drag(k, 197.0f, true);
	const float shortOfEnd = keys()[1].time;
	MESSAGE("key after drag away: " << away << " s, back with Alt: " << shortOfEnd << " s");
	CHECK(shortOfEnd < 1.0f);
	CHECK(shortOfEnd > 0.98f);

	// A click on it, no travel: selecting a key must not move it — the grab
	// offset is what keeps an off-centre click from pulling it under the pointer.
	k = findKey();
	REQUIRE(k.x >= 0.0f);
	drag(k, 0.0f, false);
	CHECK(keys()[1].time == shortOfEnd);

	// The same key picked up without Alt, wiggled past the drag threshold and
	// put down exactly where it lay, 3 px short: a few pixels from the end IS
	// the end.
	k = findKey();
	REQUIRE(k.x >= 0.0f);
	drag(k, -20.0f, false, /*back*/ true);
	CHECK(keys()[1].time == 1.0f);

	io.AddMousePosEvent(-1000.0f, -1000.0f);
	d.frame(false);
	UIEditorPanel::forget(d.assetPath);
	fs::remove_all(root, ec);
}

// Thema 114: the key at 0 ms could not be grabbed. The track's "x" (remove
// track) stood beside a 168 px name, so it reached ~9 px past the lane's left
// edge — straight over the 12 px button of a key at time zero. It is submitted
// first, so ImGui gave it the pointer everywhere the two overlapped.
//     HE_UI_DUMP_DIR=/tmp/ui ./he_tests -tc="timeline: the key at 0 ms*"
TEST_CASE("timeline: the key at 0 ms is not covered by the track's remove button")
{
	Harness harness;
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "he_widget_designer_key0_114";
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::create_directories(root / "UI");

	ContentManager cm;
	cm.setContentRoot(root.string());
	HE::UIWidgetTree t;
	t.canvasWidth = 1280.0f; t.canvasHeight = 720.0f;
	const int logo = t.add(HE::UIWidgetType::Image);
	t.find(logo)->name = "Logo";
	{
		HE::UIAnimClip c;
		c.name = "Blend";
		c.duration = 1.0f;
		HE::UIAnimTrack tr;
		tr.element = logo;
		tr.prop    = "Render Opacity";
		tr.keys.push_back({ 0.0f, HE::UIPropValue::ofFloat(0.0f), HE::UIEase::Linear });
		tr.keys.push_back({ 1.0f, HE::UIPropValue::ofFloat(1.0f), HE::UIEase::Linear });
		c.tracks.push_back(tr);
		t.animations.push_back(c);
	}
	UIWidgetAsset asset;
	asset.name     = "Key0";
	asset.path     = "UI/Key0.hasset";
	asset.treeJson = HE::uiWidgetTreeToJson(t);
	REQUIRE(cm.registerWidget(std::move(asset)) != HE::UUID{});

	HorizonWorld world;
	EditorUndo   undo;
	ContextBits  bits;
	AppContext   ctx = bits.make(world, undo);
	ctx.contentManager = &cm;
	Designer d{ ctx, (root / "UI" / "Key0.hasset").string() };
	for (int i = 0; i < 3; ++i) d.frame(false);

	ImGuiIO& io = ImGui::GetIO();
	auto windowNamed = [](const char* part) -> ImGuiWindow*
	{
		for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
			if (w->Active && std::strstr(w->Name, part)) return w;
		return nullptr;
	};
	auto find = [&](ImGuiID wanted, float x0, float x1, float y0, float y1,
	                float sx, float sy) -> ImVec2
	{
		for (float y = y0; y <= y1; y += sy)
			for (float x = x0; x <= x1; x += sx)
			{
				io.AddMousePosEvent(x, y);
				d.frame(false);
				if (d.frame(false) == wanted) return ImVec2(x, y);
			}
		return ImVec2(-1.0f, -1.0f);
	};
	auto click = [&](ImVec2 p)
	{
		io.AddMousePosEvent(p.x, p.y);
		d.frame(true); d.frame(false); d.frame(false);
	};
	auto clip = [&]() -> HE::UIAnimClip&
	{
		HE::UIWidgetTree* live = UIEditorPanel::liveTree("UI/Key0.hasset");
		REQUIRE(live);
		REQUIRE(live->animations.size() == 1);
		return live->animations[0];
	};

	ImGuiWindow* tw = windowNamed("##uiw_timeline");
	REQUIRE(tw);
	const float wx0 = tw->Pos.x, wy0 = tw->Pos.y, wy1 = tw->Pos.y + tw->Size.y;

	const ImVec2 combo = find(ImHashStr("##clip", 0, tw->ID), wx0, wx0 + 200.0f,
	                          wy0 + 4.0f, wy0 + 30.0f, 8.0f, 4.0f);
	REQUIRE(combo.x >= 0.0f);
	click(combo);
	ImGuiWindow* pop = windowNamed("##Combo_");
	REQUIRE(pop);
	const ImVec2 blend = find(ImHashStr("Blend##c", 0, pop->ID), pop->Pos.x + 10.0f,
	                          pop->Pos.x + 30.0f, pop->Pos.y, pop->Pos.y + pop->Size.y,
	                          10.0f, 2.0f);
	REQUIRE(blend.x >= 0.0f);
	click(blend);

	int zero = 0;
	const ImGuiID trackSeed = ImHashData(&zero, sizeof(zero), tw->ID);
	const ImGuiID nameId    = ImHashStr("Logo  \xC2\xB7  Render Opacity##t", 0, trackSeed);
	const ImGuiID removeId  = ImHashStr("x", 0, trackSeed);
	const ImGuiID key0Id    = ImHashStr("##key", 0, ImHashData(&zero, sizeof(zero), trackSeed));
	const ImVec2 track = find(nameId, wx0 + 20.0f, wx0 + 20.0f, wy0 + 20.0f, wy1, 10.0f, 3.0f);
	REQUIRE(track.x >= 0.0f);

	// Every pixel across the name column's end and the lane's start, along the
	// middle of the row (the diamonds sit mid-row, the name was found near its
	// top): who does ImGui say the pointer is on?
	const float rowMid = track.y + 8.0f;
	int keyPx = 0, removePx = 0;
	float keyL = 1e9f, keyR = -1e9f, removeR = -1e9f;
	for (float x = wx0 + 150.0f; x <= wx0 + 240.0f; x += 1.0f)
	{
		io.AddMousePosEvent(x, rowMid);
		d.frame(false);
		const ImGuiID h = d.frame(false);
		if (h == key0Id)    { ++keyPx; keyL = std::min(keyL, x); keyR = std::max(keyR, x); }
		if (h == removeId)  { ++removePx; removeR = std::max(removeR, x); }
	}
	MESSAGE("key at 0 ms hovered on " << keyPx << " px (" << keyL - wx0 << ".." << keyR - wx0
	        << "), remove button on " << removePx << " px (right edge " << removeR - wx0 << ")");

	// The picture: the pointer on the diamond's visible half, where a person
	// aims — the key lights up when it is the one under the pointer.
	if (keyPx > 0) io.AddMousePosEvent(keyR - 3.0f, rowMid);
	d.shoot("timeline-key0-114");

	// The whole 12 px button of the key belongs to the key, and the remove
	// button still exists beside it — clear of it.
	CHECK(keyPx >= 11);
	CHECK(removePx >= 10);
	CHECK(removeR < keyL);

	// Grabbed where it is visible and dragged right: the first key moves.
	REQUIRE(keyPx > 0);
	const ImVec2 k(keyR - 2.0f, rowMid);
	io.AddMousePosEvent(k.x, k.y);
	d.frame(false);
	d.frame(true);
	for (int s = 1; s <= 10; ++s) { io.AddMousePosEvent(k.x + 8.0f * float(s), k.y); d.frame(true); }
	d.frame(false); d.frame(false);
	REQUIRE(clip().tracks.size() == 1);
	REQUIRE(clip().tracks[0].keys.size() == 2);
	MESSAGE("first key after the drag: " << clip().tracks[0].keys[0].time << " s");
	CHECK(clip().tracks[0].keys[0].time > 0.05f);
	CHECK(clip().tracks[0].keys[1].time == 1.0f);

	// And the remove button still removes the track.
	const ImVec2 x = find(removeId, wx0 + 150.0f, wx0 + 200.0f, rowMid, rowMid, 1.0f, 1.0f);
	REQUIRE(x.x >= 0.0f);
	click(x);
	CHECK(clip().tracks.empty());

	io.AddMousePosEvent(-1000.0f, -1000.0f);
	d.frame(false);
	UIEditorPanel::forget(d.assetPath);
	fs::remove_all(root, ec);
}

// Thema 108: Visible and Enabled in the real Add Track list. Enabled is listed
// but greyed on an Image (nothing there to switch off) and pickable on a
// Button; a switch made with the playhead past 0 also gets a key at 0.
TEST_CASE("Thema 108: Add Track offers Visible everywhere, Enabled where it acts")
{
	Harness harness;
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "he_widget_designer_timeline108";
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::create_directories(root / "UI");

	ContentManager cm;
	cm.setContentRoot(root.string());
	HE::UIWidgetTree t = samplePage();
	{
		// One clip with one track, so the timeline has something open.
		HE::UIAnimClip c;
		c.name = "Intro";
		c.duration = 1.0f;
		HE::UIAnimTrack tr;
		tr.element = 2;   // Logo
		tr.prop    = "Render Opacity";
		tr.keys.push_back({ 0.0f, HE::UIPropValue::ofFloat(0.0f), HE::UIEase::Linear });
		c.tracks.push_back(tr);
		t.animations.push_back(c);
	}
	UIWidgetAsset asset;
	asset.name     = "Anim108";
	asset.path     = "UI/Anim108.hasset";
	asset.treeJson = HE::uiWidgetTreeToJson(t);
	REQUIRE(cm.registerWidget(std::move(asset)) != HE::UUID{});

	HorizonWorld world;
	EditorUndo   undo;
	ContextBits  bits;
	AppContext   ctx = bits.make(world, undo);
	ctx.contentManager = &cm;
	Designer d{ ctx, (root / "UI" / "Anim108.hasset").string() };
	d.width = 1920;   // the whole transport bar, ">|" included
	for (int i = 0; i < 3; ++i) d.frame(false);

	ImGuiIO& io = ImGui::GetIO();
	auto windowNamed = [](const char* part) -> ImGuiWindow*
	{
		for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
			if (w->Active && std::strstr(w->Name, part)) return w;
		return nullptr;
	};
	auto find = [&](ImGuiID wanted, float x0, float x1, float y0, float y1,
	                float sx, float sy) -> ImVec2
	{
		for (float y = y0; y <= y1; y += sy)
			for (float x = x0; x <= x1; x += sx)
			{
				io.AddMousePosEvent(x, y);
				d.frame(false);
				if (d.frame(false) == wanted) return ImVec2(x, y);
			}
		return ImVec2(-1.0f, -1.0f);
	};
	auto click = [&](ImVec2 p)
	{
		io.AddMousePosEvent(p.x, p.y);
		d.frame(true); d.frame(false); d.frame(false);
	};
	auto tracks = [&]() -> std::vector<HE::UIAnimTrack>&
	{
		HE::UIWidgetTree* live = UIEditorPanel::liveTree("UI/Anim108.hasset");
		REQUIRE(live);
		REQUIRE(live->animations.size() == 1);
		return live->animations[0].tracks;
	};

	ImGuiWindow* tw = windowNamed("##uiw_timeline");
	REQUIRE(tw);
	const float wx0 = tw->Pos.x, wx1 = tw->Pos.x + tw->Size.x;
	const float wy0 = tw->Pos.y, wy1 = tw->Pos.y + tw->Size.y;

	// Open "Intro" through the clip combo.
	const ImVec2 combo = find(ImHashStr("##clip", 0, tw->ID), wx0, wx0 + 200.0f,
	                          wy0 + 4.0f, wy0 + 30.0f, 8.0f, 4.0f);
	REQUIRE(combo.x >= 0.0f);
	click(combo);
	ImGuiWindow* pop = windowNamed("##Combo_");
	REQUIRE(pop);
	const ImVec2 intro = find(ImHashStr("Intro##c", 0, pop->ID), pop->Pos.x + 10.0f,
	                          pop->Pos.x + 30.0f, pop->Pos.y, pop->Pos.y + pop->Size.y,
	                          10.0f, 2.0f);
	REQUIRE(intro.x >= 0.0f);
	click(intro);

	// Playhead to the end, so the new switch's own key is not at 0.
	const ImVec2 toEnd = find(ImHashStr(">|", 0, tw->ID), wx0, wx1,
	                          wy0 + 4.0f, wy0 + 30.0f, 6.0f, 4.0f);
	REQUIRE(toEnd.x >= 0.0f);
	click(toEnd);

	// Add Track → the popup → the entry named `prop`. Returns where it was
	// found, or (-1,-1) when the list does not have it at all.
	auto openAddTrack = [&]() -> ImGuiWindow*
	{
		const ImVec2 add = find(ImHashStr("Add Track", 0, tw->ID), wx0 + 10.0f, wx0 + 40.0f,
		                        wy0 + 30.0f, wy1, 10.0f, 3.0f);
		REQUIRE(add.x >= 0.0f);
		click(add);
		return windowNamed("##Popup_");
	};
	auto entry = [&](ImGuiWindow* p, const char* prop)
	{
		return find(ImHashStr(prop, 0, p->ID), p->Pos.x + 12.0f, p->Pos.x + 12.0f,
		            p->Pos.y, p->Pos.y + p->Size.y, 10.0f, 2.0f);
	};

	// ── The Image ─────────────────────────────────────────────────────────────
	d.shoot("anim108-warmup");
	REQUIRE(d.clickRow(d.hierarchyRowId("Logo##hn2")));
	ImGuiWindow* p = openAddTrack();
	REQUIRE(p);
	// Enabled is there — greyed, not missing — and a click on it adds nothing.
	const ImVec2 enabledOnImage = entry(p, "Enabled");
	REQUIRE(enabledOnImage.x >= 0.0f);
	click(enabledOnImage);
	CHECK(tracks().size() == 1);
	// No other Bool is listed: Hit Testable stays out in v1.
	CHECK(entry(p, "Hit Testable").x < 0.0f);
	// Visible is pickable.
	const ImVec2 visible = entry(p, "Visible");
	REQUIRE(visible.x >= 0.0f);
	click(visible);
	REQUIRE(tracks().size() == 2);
	{
		const HE::UIAnimTrack& tr = tracks()[1];
		CHECK(tr.element == 2);
		CHECK(tr.prop == "Visible");
		// Made at the end, so it also holds the current value at 0 — a switch
		// set to off at the end must not hide the Logo from the very start.
		REQUIRE(tr.keys.size() == 2);
		CHECK(tr.keys[0].time == 0.0f);
		CHECK(tr.keys[1].time == 1.0f);
		CHECK(tr.keys[0].value.type == HE::UIPropType::Bool);
		CHECK(tr.keys[0].value.b);
		CHECK(tr.keys[1].value.b);
	}

	// ── The Button ────────────────────────────────────────────────────────────
	io.AddMousePosEvent(-1000.0f, -1000.0f);
	d.frame(false);
	REQUIRE(d.clickRow(d.hierarchyRowId("Start##hn4")));
	p = openAddTrack();
	REQUIRE(p);
	const ImVec2 enabledOnButton = entry(p, "Enabled");
	REQUIRE(enabledOnButton.x >= 0.0f);
	click(enabledOnButton);
	REQUIRE(tracks().size() == 3);
	CHECK(tracks()[2].element == 4);
	CHECK(tracks()[2].prop == "Enabled");
	CHECK(tracks()[2].keys.size() == 2);

	io.AddMousePosEvent(-1000.0f, -1000.0f);
	d.frame(false);
	UIEditorPanel::forget(d.assetPath);
	fs::remove_all(root, ec);
}

// Thema 108, looked at: both switches keyed through the timeline as a person
// would (Add Track, a scrub to the middle, Key, the Value checkbox), then the
// clip played on the canvas. The Logo (Visible) and the Start button (Enabled)
// must hold still between keys and flip at them; the Title's Render Opacity,
// which glides, is the control that the canvas does change between frames.
//
//     HE_UI_DUMP_DIR=/tmp/ui ./he_tests -tc="ui shot: Thema 108*"
TEST_CASE("ui shot: Thema 108 — Visible and Enabled step on the canvas while playing")
{
	Harness harness;
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "he_widget_designer_shot108";
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::create_directories(root / "UI");

	ContentManager cm;
	cm.setContentRoot(root.string());
	HE::UIWidgetTree t = samplePage();
	{
		// The control: the Title fades in over the whole second.
		HE::UIAnimClip c;
		c.name = "Intro";
		c.duration = 1.0f;
		HE::UIAnimTrack tr;
		tr.element = 3;   // Title
		tr.prop    = "Render Opacity";
		tr.keys.push_back({ 0.0f, HE::UIPropValue::ofFloat(0.0f), HE::UIEase::Linear });
		tr.keys.push_back({ 1.0f, HE::UIPropValue::ofFloat(1.0f), HE::UIEase::Linear });
		c.tracks.push_back(tr);
		t.animations.push_back(c);
	}
	UIWidgetAsset asset;
	asset.name     = "Shot108";
	asset.path     = "UI/Shot108.hasset";
	asset.treeJson = HE::uiWidgetTreeToJson(t);
	REQUIRE(cm.registerWidget(std::move(asset)) != HE::UUID{});

	HorizonWorld world;
	EditorUndo   undo;
	ContextBits  bits;
	AppContext   ctx = bits.make(world, undo);
	ctx.contentManager = &cm;
	Designer d{ ctx, (root / "UI" / "Shot108.hasset").string() };
	d.width  = 1920;   // the whole transport bar
	d.height = 1100;
	for (int i = 0; i < 3; ++i) d.frame(false);

	ImGuiIO& io = ImGui::GetIO();
	auto windowNamed = [](const char* part) -> ImGuiWindow*
	{
		for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
			if (w->Active && std::strstr(w->Name, part)) return w;
		return nullptr;
	};
	auto find = [&](ImGuiID wanted, float x0, float x1, float y0, float y1,
	                float sx, float sy) -> ImVec2
	{
		for (float y = y0; y <= y1; y += sy)
			for (float x = x0; x <= x1; x += sx)
			{
				io.AddMousePosEvent(x, y);
				d.frame(false);
				if (d.frame(false) == wanted) return ImVec2(x, y);
			}
		return ImVec2(-1.0f, -1.0f);
	};
	auto click = [&](ImVec2 p)
	{
		io.AddMousePosEvent(p.x, p.y);
		d.frame(true); d.frame(false); d.frame(false);
	};
	auto away = [&]
	{
		io.AddMousePosEvent(-1000.0f, -1000.0f);
		d.frame(false);
	};
	auto shootNamed = [&](const std::string& name)
	{
		he_ui::Image img;
		d.frame(false, &img);
		if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
			he_ui::writeBmp(img, std::string(dir) + "/" + name + ".bmp");
		return img;
	};
	auto track = [&](const char* prop) -> HE::UIAnimTrack*
	{
		HE::UIWidgetTree* live = UIEditorPanel::liveTree("UI/Shot108.hasset");
		REQUIRE(live);
		REQUIRE(live->animations.size() == 1);
		for (HE::UIAnimTrack& tr : live->animations[0].tracks)
			if (tr.prop == prop) return &tr;
		return nullptr;
	};

	ImGuiWindow* tw = windowNamed("##uiw_timeline");
	REQUIRE(tw);
	const float wx0 = tw->Pos.x, wx1 = tw->Pos.x + tw->Size.x;
	const float wy0 = tw->Pos.y, wy1 = tw->Pos.y + tw->Size.y;
	auto bar = [&](const char* label)
	{
		return find(ImHashStr(label, 0, tw->ID), wx0, wx1, wy0 + 4.0f, wy0 + 30.0f, 6.0f, 4.0f);
	};

	// Open "Intro".
	const ImVec2 combo = find(ImHashStr("##clip", 0, tw->ID), wx0, wx0 + 200.0f,
	                          wy0 + 4.0f, wy0 + 30.0f, 8.0f, 4.0f);
	REQUIRE(combo.x >= 0.0f);
	click(combo);
	ImGuiWindow* pop = windowNamed("##Combo_");
	REQUIRE(pop);
	const ImVec2 intro = find(ImHashStr("Intro##c", 0, pop->ID), pop->Pos.x + 10.0f,
	                          pop->Pos.x + 30.0f, pop->Pos.y, pop->Pos.y + pop->Size.y,
	                          10.0f, 2.0f);
	REQUIRE(intro.x >= 0.0f);
	click(intro);

	ImVec2 p = bar(">|");
	REQUIRE(p.x >= 0.0f);
	click(p);

	auto addTrackButton = [&]
	{
		return find(ImHashStr("Add Track", 0, tw->ID), wx0 + 10.0f, wx0 + 40.0f,
		            wy0 + 30.0f, wy1, 10.0f, 3.0f);
	};
	auto entry = [&](ImGuiWindow* pw, const char* prop)
	{
		return find(ImHashStr(prop, 0, pw->ID), pw->Pos.x + 12.0f, pw->Pos.x + 12.0f,
		            pw->Pos.y, pw->Pos.y + pw->Size.y, 10.0f, 2.0f);
	};
	// "Key" sits on the Add Track row; the key editor's Value checkbox on the
	// row under it, after Time.
	auto clickKey = [&]
	{
		const ImVec2 add = addTrackButton();
		REQUIRE(add.x >= 0.0f);
		const ImVec2 k = find(ImHashStr("Key", 0, tw->ID), add.x, wx0 + 500.0f,
		                      add.y, add.y, 4.0f, 1.0f);
		REQUIRE(k.x >= 0.0f);
		click(k);
	};
	auto toggleValue = [&]
	{
		const ImVec2 add = addTrackButton();
		REQUIRE(add.x >= 0.0f);
		const ImVec2 time = find(ImHashStr("Time", 0, tw->ID), wx0 + 40.0f, wx0 + 40.0f,
		                         add.y + 8.0f, add.y + 60.0f, 4.0f, 2.0f);
		REQUIRE(time.x >= 0.0f);
		const ImVec2 v = find(ImHashStr("Value", 0, tw->ID), time.x, wx0 + 500.0f,
		                      time.y, time.y, 4.0f, 1.0f);
		REQUIRE(v.x >= 0.0f);
		click(v);
	};

	// ── Logo: Visible, off from 0.5 s to the end ──────────────────────────────
	away();
	REQUIRE(d.clickRow(d.hierarchyRowId("Logo##hn2")));
	p = addTrackButton();
	REQUIRE(p.x >= 0.0f);
	click(p);
	ImGuiWindow* addPop = windowNamed("##Popup_");
	REQUIRE(addPop);
	// On an Image, Enabled is listed greyed; the pointer rests on it so its
	// tooltip says why.
	const ImVec2 enabledOnImage = entry(addPop, "Enabled");
	REQUIRE(enabledOnImage.x >= 0.0f);
	for (int i = 0; i < 60; ++i) d.frame(false);
	shootNamed("shot108-1-add-track-image");
	const ImVec2 visible = entry(addPop, "Visible");
	REQUIRE(visible.x >= 0.0f);
	click(visible);
	REQUIRE(track("Visible"));

	// Scrub to the middle of the lane: the ruler, half way along.
	const float laneL = wx0 + 8.0f + 208.0f;   // kNameW in drawTimeline (Thema 114)
	const float laneR = wx1 - 8.0f - 8.0f;
	const ImVec2 ruler = find(ImHashStr("##ruler", 0, tw->ID), (laneL + laneR) * 0.5f,
	                          (laneL + laneR) * 0.5f, wy0 + 20.0f, wy0 + 80.0f, 4.0f, 2.0f);
	REQUIRE(ruler.x >= 0.0f);
	click(ruler);
	clickKey();
	toggleValue();
	{
		const HE::UIAnimTrack* tr = track("Visible");
		REQUIRE(tr);
		REQUIRE(tr->keys.size() == 3);
		MESSAGE("Visible keys: " << tr->keys[0].time << "=" << tr->keys[0].value.b << "  "
		        << tr->keys[1].time << "=" << tr->keys[1].value.b << "  "
		        << tr->keys[2].time << "=" << tr->keys[2].value.b);
		CHECK(tr->keys[0].value.b);
		CHECK_FALSE(tr->keys[1].value.b);
		CHECK(tr->keys[2].value.b);
		CHECK(tr->keys[1].time > 0.3f);
		CHECK(tr->keys[1].time < 0.7f);
	}

	// ── Start: Enabled, off from the same moment, on again at the end ─────────
	away();
	REQUIRE(d.clickRow(d.hierarchyRowId("Start##hn4")));
	p = addTrackButton();
	REQUIRE(p.x >= 0.0f);
	click(p);
	addPop = windowNamed("##Popup_");
	REQUIRE(addPop);
	const ImVec2 enabledOnButton = entry(addPop, "Enabled");
	REQUIRE(enabledOnButton.x >= 0.0f);
	shootNamed("shot108-2-add-track-button");
	click(enabledOnButton);
	REQUIRE(track("Enabled"));
	toggleValue();                    // the key at the playhead (mid) → off
	p = bar(">|");
	REQUIRE(p.x >= 0.0f);
	click(p);
	clickKey();                       // a key at the end, holding "off"
	toggleValue();                    // → on
	float switchAt = 0.0f;
	{
		const HE::UIAnimTrack* tr = track("Enabled");
		REQUIRE(tr);
		REQUIRE(tr->keys.size() == 3);
		MESSAGE("Enabled keys: " << tr->keys[0].time << "=" << tr->keys[0].value.b << "  "
		        << tr->keys[1].time << "=" << tr->keys[1].value.b << "  "
		        << tr->keys[2].time << "=" << tr->keys[2].value.b);
		CHECK(tr->keys[0].value.b);
		CHECK_FALSE(tr->keys[1].value.b);
		CHECK(tr->keys[2].value.b);
		CHECK(tr->keys[1].time == track("Visible")->keys[1].time);
		switchAt = tr->keys[1].time;
	}

	// ── Play it, and look ─────────────────────────────────────────────────────
	// Where an element sits on the screen: the canvas is fitted into its child
	// window the way drawCanvas does it (92 % of the smaller ratio, centred).
	ImGuiWindow* cw = windowNamed("##uiw_canvas");
	REQUIRE(cw);
	const ImVec2 o = cw->DC.CursorStartPos;
	const ImVec2 avail(cw->ContentRegionRect.Max.x - o.x, cw->ContentRegionRect.Max.y - o.y);
	const float  s = std::min(avail.x / 1280.0f, avail.y / 720.0f) * 0.92f;
	const ImVec2 cTL(o.x + (avail.x - 1280.0f * s) * 0.5f, o.y + (avail.y - 720.0f * s) * 0.5f);
	struct Mean { float r = 0, g = 0, b = 0; };
	// The mean colour of the middle of an element's box (`in` of it off each
	// edge, so a pixel of misplacement cannot matter).
	auto mean = [&](const he_ui::Image& img, float x, float y, float w, float h, float in = 0.2f)
	{
		const int x0 = int(cTL.x + (x + w * in) * s), x1 = int(cTL.x + (x + w * (1.0f - in)) * s);
		const int y0 = int(cTL.y + (y + h * in) * s), y1 = int(cTL.y + (y + h * (1.0f - in)) * s);
		Mean m; int n = 0;
		for (int py = y0; py < y1; ++py)
			for (int px = x0; px < x1; ++px)
			{
				std::uint8_t r, g, b, a;
				img.pixel(px, py, r, g, b, a);
				m.r += r; m.g += g; m.b += b; ++n;
			}
		if (n) { m.r /= n; m.g /= n; m.b /= n; }
		return m;
	};
	auto same = [](Mean a, Mean b)
	{
		return std::fabs(a.r - b.r) < 0.01f && std::fabs(a.g - b.g) < 0.01f && std::fabs(a.b - b.b) < 0.01f;
	};
	auto luma = [](Mean m) { return 0.299f * m.r + 0.587f * m.g + 0.114f * m.b; };
	auto logo  = [&](const he_ui::Image& i) { return mean(i, 540.0f, 120.0f, 200.0f, 200.0f); };
	// The whole box: its word sits at the left edge, where an inset misses it.
	auto title = [&](const he_ui::Image& i) { return mean(i, 440.0f, 360.0f, 400.0f,  60.0f, 0.0f); };
	auto start = [&](const he_ui::Image& i) { return mean(i, 540.0f, 460.0f, 200.0f,  56.0f); };

	p = bar("|<");
	REQUIRE(p.x >= 0.0f);
	click(p);
	p = bar("Play");
	REQUIRE(p.x >= 0.0f);
	io.AddMousePosEvent(p.x, p.y);
	d.frame(true);
	d.frame(false);          // released: playing, and this frame already advanced
	int played = 1;
	io.AddMousePosEvent(-1000.0f, -1000.0f);
	// The canvas draws before the timeline advances, so a frame shows the
	// playhead the previous one left: `played - 1` frames of 1/60 s.
	auto playTo = [&](float at, const char* name)
	{
		const int frames = int(std::lround(at * 60.0f)) + 1;
		while (played < frames - 1) { d.frame(false); ++played; }
		he_ui::Image img = shootNamed(name);
		++played;
		return img;
	};
	const he_ui::Image a = playTo(0.25f,             "shot108-3-play-250ms");
	const he_ui::Image b = playTo(switchAt - 0.1f,   "shot108-4-play-before-switch");
	const he_ui::Image c = playTo(switchAt + 0.1f,   "shot108-5-play-after-switch");
	const he_ui::Image e = playTo(0.9f,              "shot108-6-play-900ms");
	for (int i = 0; i < 20; ++i) d.frame(false);    // past the end: stopped on 1.0 s
	const he_ui::Image f = shootNamed("shot108-7-end");

	MESSAGE("logo  luma: " << luma(logo(a))  << " " << luma(logo(b))  << " | " << luma(logo(c))
	        << " " << luma(logo(e))  << " | end " << luma(logo(f)));
	MESSAGE("start luma: " << luma(start(a)) << " " << luma(start(b)) << " | " << luma(start(c))
	        << " " << luma(start(e)) << " | end " << luma(start(f)));
	MESSAGE("title luma: " << luma(title(a)) << " " << luma(title(b)) << " | " << luma(title(c))
	        << " " << luma(title(e)) << " | end " << luma(title(f)));

	// The control first: the fade glides, so the Title brightens shot by shot
	// (one short word in a wide box: the steps are small but they are steps).
	// Without it, "the switches did not change" could mean "nothing played".
	CHECK(luma(title(b)) > luma(title(a)));
	CHECK(luma(title(c)) > luma(title(b)));
	CHECK(luma(title(e)) > luma(title(c)));
	// Before the switch key: both switches hold, not a hair of change.
	CHECK(same(logo(a),  logo(b)));
	CHECK(same(start(a), start(b)));
	// After it: flipped, and again holding.
	CHECK_FALSE(same(logo(b),  logo(c)));
	CHECK(same(logo(c),  logo(e)));
	CHECK_FALSE(same(start(b), start(c)));
	CHECK(same(start(c), start(e)));
	CHECK(luma(start(c)) < luma(start(b)));   // disabled = dimmed
	// At the end both are back on, exactly as before the switch.
	CHECK(same(logo(f),  logo(a)));
	CHECK(same(start(f), start(a)));

	away();
	UIEditorPanel::forget(d.assetPath);
	fs::remove_all(root, ec);
}

// (3): the Catania "Blend" clip as the user authored it. The Render Opacity
// keys are 0 -> 1, but the second key sits at 0.0503 s in a 1 s clip, and a
// clip ends at its last key — so the fade is over in three frames: a pop. The
// same keys at 1 s fade over a second. Runtime and designer share the one
// evaluator (uiAnimEvaluate / uiAnimPlayEnd), so this is what both show.
TEST_CASE("repro 107: the user's Render Opacity clip is over in three frames")
{
	auto clipWithSecondKeyAt = [](int elem, float t1)
	{
		HE::UIAnimClip c;
		c.name = "Blend";
		c.duration = 1.0f;
		HE::UIAnimTrack tr;
		tr.element = elem;
		tr.prop    = "Render Opacity";
		tr.keys.push_back({ 0.0f, HE::UIPropValue::ofFloat(0.0f), HE::UIEase::Linear });
		tr.keys.push_back({ t1,   HE::UIPropValue::ofFloat(1.0f), HE::UIEase::Linear });
		c.tracks.push_back(tr);
		return c;
	};

	for (const float t1 : { 0.050314463675022125f, 1.0f })
	{
		CAPTURE(t1);
		ContentManager cm;
		HE::UIWidgetTree t;
		t.canvasWidth = 1920.0f; t.canvasHeight = 1080.0f;
		const int img = t.add(HE::UIWidgetType::Image);
		t.animations.push_back(clipWithSecondKeyAt(img, t1));
		CHECK(HE::uiAnimPlayEnd(t.animations.back()) == doctest::Approx(t1));

		UIWidgetAsset a;
		a.path     = "mem://startup107.hasset";
		a.treeJson = HE::uiWidgetTreeToJson(t);
		REQUIRE(cm.registerWidget(std::move(a)) != HE::UUID{});
		WidgetManager wm;
		const int id = wm.createWidget(cm, "mem://startup107.hasset");
		REQUIRE(id != 0);
		wm.showWidget(id);
		REQUIRE(wm.playAnimation(id, "Blend"));

		std::string curve;
		float at4 = -1.0f, at30 = -1.0f;
		for (int f = 1; f <= 60; ++f)
		{
			wm.tick(1.0f / 60.0f);
			const float o = wm.tree(id)->find(img)->renderOpacity;
			if (f == 4)  at4  = o;
			if (f == 30) at30 = o;
			if (f <= 6 || f % 10 == 0)
			{
				char buf[32];
				std::snprintf(buf, sizeof(buf), " f%d=%.2f", f, o);
				curve += buf;
			}
		}
		MESSAGE("second key at " << t1 << " s, opacity per 60 Hz frame:" << curve);
		if (t1 < 0.1f)
		{
			CHECK(at4  == doctest::Approx(1.0f));   // fully in after 4 frames — the pop
			CHECK(at30 == doctest::Approx(1.0f));
		}
		else
		{
			CHECK(at4  < 0.1f);                     // a real fade
			CHECK(at30 == doctest::Approx(0.5f).epsilon(0.05));
		}
	}
}

// Schritt 10 (reopened: "still pops in the GAME"). A fresh clip, not the
// user's: 3 s, Render Opacity keyed 0 → 1 (Out Quad) → 0.25 → 1 at 0/1/2/3 s,
// and Position, Size, Rotation and Tint on the same image over the same 3 s.
// Played the way the game plays it — WidgetManager::playAnimation is where
// widget.playAnimation lands (ScriptApi::playClipAsAuthored), tick() is what
// GameApplication calls every frame with the raw dt — and read twice: from the
// element, and from the quad extract() hands the renderer (what the Metal/GL UI
// pass multiplies into the pixel). Forward and Backward, like the Catania graph.
TEST_CASE("repro 107: a multi-key Render Opacity clip interpolates in the game path")
{
	auto key = [](float t, HE::UIPropValue v, HE::UIEase e = HE::UIEase::Linear)
	{ return HE::UIAnimKey{ t, v, e }; };

	ContentManager cm;
	// Textured, so the quad goes down the image path of the UI pass (mode 2)
	// rather than the solid one — the logo in Catania is a textured image. By
	// path, like an authored widget: WidgetManager resolves it with loadAsset.
	TextureAsset tex;
	tex.path = "mem://tex107.hasset";
	tex.width = tex.height = 2; tex.channels = 4;
	tex.data.assign(2 * 2 * 4, 255);
	const HE::UUID texId = cm.registerTexture(std::move(tex));
	REQUIRE(texId != HE::UUID{});

	HE::UIWidgetTree t;
	t.canvasWidth = 1920.0f; t.canvasHeight = 1080.0f;
	const int img = t.add(HE::UIWidgetType::Image);
	t.find(img)->setPropAny("Size", HE::UIPropValue::ofVec2({ 100.0f, 100.0f }));
	t.find(img)->texture = "mem://tex107.hasset";

	HE::UIAnimClip c;
	c.name = "Fade3s";
	c.duration = 3.0f;
	auto track = [&](const char* prop, std::vector<HE::UIAnimKey> keys)
	{
		HE::UIAnimTrack tr;
		tr.element = img; tr.prop = prop; tr.keys = std::move(keys);
		c.tracks.push_back(std::move(tr));
	};
	using V = HE::UIPropValue;
	track("Render Opacity", { key(0.0f, V::ofFloat(0.0f)),
	                          key(1.0f, V::ofFloat(1.0f), HE::UIEase::OutQuad),
	                          key(2.0f, V::ofFloat(0.25f)),
	                          key(3.0f, V::ofFloat(1.0f)) });
	track("Position", { key(0.0f, V::ofVec2({ 0.0f, 0.0f })),   key(3.0f, V::ofVec2({ 300.0f, 0.0f })) });
	track("Size",     { key(0.0f, V::ofVec2({ 100.0f, 100.0f })), key(3.0f, V::ofVec2({ 400.0f, 100.0f })) });
	track("Rotation", { key(0.0f, V::ofFloat(0.0f)),            key(3.0f, V::ofFloat(90.0f)) });
	track("Tint",     { key(0.0f, V::ofColor({ 1, 1, 1, 1 })),  key(3.0f, V::ofColor({ 1, 0, 0, 1 })) });
	t.animations.push_back(c);
	REQUIRE(HE::uiAnimPlayEnd(c) == doctest::Approx(3.0f));

	// What the evaluator alone says for a pass time: the reference both reads
	// are held against.
	auto expectedOpacity = [&](float clipT)
	{
		std::vector<HE::UIAnimSample> s;
		HE::uiAnimEvaluate(c, clipT, s);
		for (const auto& x : s) if (x.prop == "Render Opacity") return x.value.f;
		return -1.0f;
	};

	UIWidgetAsset a;
	a.path     = "mem://fade3s107.hasset";
	a.treeJson = HE::uiWidgetTreeToJson(t);
	REQUIRE(cm.registerWidget(std::move(a)) != HE::UUID{});

	for (const HE::UIAnimDirection dir : { HE::UIAnimDirection::Forward, HE::UIAnimDirection::Backward })
	{
		CAPTURE(HE::uiAnimDirectionName(dir));
		WidgetManager wm;
		const int id = wm.createWidget(cm, "mem://fade3s107.hasset");
		REQUIRE(id != 0);
		wm.showWidget(id);
		REQUIRE(wm.tree(id)->find(img)->textureAssetId == texId);
		REQUIRE(wm.playAnimation(id, "Fade3s", nullptr, dir));

		std::string curve;
		int   frames = 0, distinct = 0;
		float prevA = -1.0f, maxStep = 0.0f, maxPropVsQuad = 0.0f, maxVsEval = 0.0f;
		float x30 = -1, w30 = -1, r30 = -1, g30 = -1;
		std::vector<UIRenderObject> out;
		for (int f = 1; f <= 190; ++f)
		{
			wm.tick(1.0f / 60.0f);
			const HE::UIElement* e = wm.tree(id)->find(img);
			out.clear();
			wm.extract(1920.0f, 1080.0f, out);
			const UIRenderObject* q = nullptr;
			for (const auto& o : out) if (o.textureAssetId == texId) { q = &o; break; }
			REQUIRE(q);
			++frames;
			const float qa = q->color.a;
			// The quad's alpha IS the element's opacity (tint alpha is 1).
			maxPropVsQuad = std::max(maxPropVsQuad, std::fabs(qa - e->renderOpacity));
			const float passT = std::min(f / 60.0f, 3.0f);
			const float clipT = dir == HE::UIAnimDirection::Backward ? 3.0f - passT : passT;
			maxVsEval = std::max(maxVsEval, std::fabs(qa - expectedOpacity(clipT)));
			if (prevA >= 0.0f)
			{
				maxStep = std::max(maxStep, std::fabs(qa - prevA));
				if (std::fabs(qa - prevA) > 1e-4f) ++distinct;
			}
			prevA = qa;
			if (f == 90) { x30 = q->position.x; w30 = q->size.x; r30 = q->rotation; g30 = q->color.g; }
			if (f <= 3 || f % 15 == 0)
			{
				char buf[40];
				std::snprintf(buf, sizeof(buf), " f%d=%.3f", f, qa);
				curve += buf;
			}
		}
		MESSAGE("quad alpha per 60 Hz frame:" << curve);
		MESSAGE("distinct alpha steps " << distinct << " of " << frames - 1
		        << ", largest step " << maxStep
		        << ", |quad - element| max " << maxPropVsQuad
		        << ", |quad - evaluator| max " << maxVsEval);

		// No pop: ~180 frames of motion, every one of them a small step. The
		// steepest stretch is Out Quad leaving 0 (2/60 per frame ≈ 0.033).
		CHECK(distinct >= 170);
		CHECK(maxStep < 0.04f);
		CHECK(maxPropVsQuad < 1e-5f);
		CHECK(maxVsEval < 0.02f);
		// Half way (1.5 s, frame 90) the other tracks are half way too — the
		// same evaluator and the same write, whatever the property.
		// The quad's corner, so Position minus the pivot's share of the size.
		CHECK(x30 == doctest::Approx(150.0f - 0.5f * 250.0f).epsilon(0.02));
		CHECK(w30 == doctest::Approx(250.0f).epsilon(0.02));
		CHECK(r30 == doctest::Approx(45.0f * 3.14159265f / 180.0f).epsilon(0.02));
		CHECK(g30 == doctest::Approx(0.5f).epsilon(0.02));
		CHECK_FALSE(wm.isPlayingAnimation(id, "Fade3s"));
	}
}

namespace
{
	// The designer's timeline driven like a person would, for the Thema 107
	// canvas cases: find a control on the timeline's bar by walking the pointer
	// over it, click it, and shoot the canvas at three moments of a clip. Made
	// once the designer has drawn its first frames (the windows exist).
	struct Timeline107
	{
		Designer&    d;
		ImGuiIO&     io = ImGui::GetIO();
		ImGuiWindow* tw = windowNamed("##uiw_timeline");
		ImGuiWindow* cw = windowNamed("##uiw_canvas");
		ImVec2 toStart{ -1.0f, -1.0f }, play{ -1.0f, -1.0f }, toEnd{ -1.0f, -1.0f };

		static ImGuiWindow* windowNamed(const char* part)
		{
			for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
				if (w->Active && std::strstr(w->Name, part)) return w;
			return nullptr;
		}
		ImVec2 find(ImGuiID wanted, float x0, float x1, float y0, float y1, float sx, float sy)
		{
			for (float y = y0; y <= y1; y += sy)
				for (float x = x0; x <= x1; x += sx)
				{
					io.AddMousePosEvent(x, y);
					d.frame(false);
					if (d.frame(false) == wanted) return ImVec2(x, y);
				}
			return ImVec2(-1.0f, -1.0f);
		}
		void click(ImVec2 p)
		{
			io.AddMousePosEvent(p.x, p.y);
			d.frame(true); d.frame(false); d.frame(false);
		}
		// The pointer off the canvas before a shot, so no hover lands in it.
		he_ui::Image shoot(const char* name)
		{
			io.AddMousePosEvent(-1000.0f, -1000.0f);
			return d.shoot(name);
		}
		ImVec2 onBar(const char* label)
		{
			const float x0 = tw->Pos.x, x1 = tw->Pos.x + tw->Size.x, y0 = tw->Pos.y;
			return find(ImHashStr(label, 0, tw->ID), x0, x1, y0 + 4.0f, y0 + 30.0f, 4.0f, 4.0f);
		}

		// Open a clip through the clip combo, then find the transport — while
		// nothing plays: a walk over a playing clip would move the playhead with
		// every frame it takes.
		bool openClip(const char* name)
		{
			const ImVec2 combo = onBar("##clip");
			if (combo.x < 0.0f) return false;
			click(combo);
			ImGuiWindow* pop = windowNamed("##Combo_");
			if (!pop) return false;
			const std::string label = std::string(name) + "##c";
			const ImVec2 row = find(ImHashStr(label.c_str(), 0, pop->ID), pop->Pos.x + 10.0f,
			                        pop->Pos.x + 30.0f, pop->Pos.y, pop->Pos.y + pop->Size.y,
			                        10.0f, 2.0f);
			if (row.x < 0.0f) return false;
			click(row);
			toStart = onBar("|<");
			play    = onBar("Play");
			toEnd   = onBar(">|");
			return toStart.x >= 0.0f && play.x >= 0.0f && toEnd.x >= 0.0f;
		}

		// Three moments of the clip: its start, half a second of playing, its end.
		struct Moments { he_ui::Image start, mid, end; };
		Moments threeMoments(const char* prefix, const char* tag)
		{
			Moments m;
			char name[128];
			click(toStart);
			std::snprintf(name, sizeof(name), "%s-%s-0-start", prefix, tag);
			m.start = shoot(name);
			click(play);   // the press and release frames; it plays from the release
			for (int f = 0; f < 26; ++f) d.frame(false);
			// The same button says Stop while it plays. A clip that already ran
			// out (the authored one, after three frames) says Play again, and a
			// click there would start it over.
			io.AddMousePosEvent(play.x, play.y);
			d.frame(false);
			if (d.frame(false) == ImHashStr("Stop", 0, tw->ID)) click(play);
			std::snprintf(name, sizeof(name), "%s-%s-1-half-second", prefix, tag);
			m.mid = shoot(name);
			click(toEnd);
			std::snprintf(name, sizeof(name), "%s-%s-2-end", prefix, tag);
			m.end = shoot(name);
			return m;
		}

		// How far the canvas is through the fade half way, over the pixels the
		// fade changes (start and end differ by more than a rounding): `inside`
		// of them lie strictly between start and end, `outside` do not lie
		// between at all, and `mean` is how far along they are on average (0 =
		// start, 1 = end).
		struct Fade { int changed = 0, inside = 0, outside = 0; double mean = 0.0; };
		Fade fadeOf(const Moments& m) const
		{
			Fade r;
			const int x0 = std::max(0, int(cw->InnerRect.Min.x)), x1 = std::min(d.width, int(cw->InnerRect.Max.x));
			const int y0 = std::max(0, int(cw->InnerRect.Min.y)), y1 = std::min(d.height, int(cw->InnerRect.Max.y));
			for (int y = y0; y < y1; ++y)
				for (int x = x0; x < x1; ++x)
				{
					const size_t i = (size_t(y) * d.width + x) * 4;
					int c = 0, span = 0;
					for (int k = 0; k < 3; ++k)
					{
						const int s = std::abs(int(m.end.rgba[i + k]) - int(m.start.rgba[i + k]));
						if (s > span) { span = s; c = k; }
					}
					if (span < 24) continue;
					++r.changed;
					bool between = true;
					for (int k = 0; k < 3; ++k)
					{
						const int a = m.start.rgba[i + k], b = m.end.rgba[i + k], v = m.mid.rgba[i + k];
						if (v < std::min(a, b) - 2 || v > std::max(a, b) + 2) between = false;
					}
					if (!between) { ++r.outside; continue; }
					const int a = m.start.rgba[i + c], b = m.end.rgba[i + c], v = m.mid.rgba[i + c];
					const double f = double(v - a) / double(b - a);
					r.mean += f;
					if (std::abs(v - a) >= 8 && std::abs(v - b) >= 8) ++r.inside;
				}
			if (r.changed > r.outside) r.mean /= double(r.changed - r.outside);
			return r;
		}
	};
}

// All four together (Schritt 6): the fixes touch the same canvas, so they are
// looked at on one. The Catania page as the user has it — the logo drawn
// through the full-resolution image path (1+4), and the user's "Blend" clip
// with its second key at 0.0503 s (3). The designer is driven like a person
// would: open the clip, |<, Play for half a second, Stop, >| — once as authored
// and once after "Stretch to Length". On the canvas, every pixel the fade
// changes between the first and the last moment has to sit BETWEEN them half
// way through: as authored it is already at the end (the pop), stretched it is
// half way. The tint alpha of the fade rides the new image path, which is the
// one place the fixes could get in each other's way. (2) lives in the runtime's
// Metal/GL UI pass, which does not exist headless; see the diagnosis.
//     HE_UI_DUMP_DIR=/tmp/ui HE_REPRO107_LOGO=<tex.hasset> ./he_tests -tc="repro 107: all four*"
TEST_CASE("repro 107: all four fixes together on one canvas")
{
	Harness harness;
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "he_widget_designer_all107";
	std::error_code ec;
	fs::remove_all(root, ec);
	const std::string texRel = stageLogo107(root);
	REQUIRE(!texRel.empty());

	ContentManager cm;
	cm.setContentRoot(root.string());
	HE::UIWidgetTree t;
	t.canvasWidth = 1920.0f; t.canvasHeight = 1080.0f;
	const int logo = t.add(HE::UIWidgetType::Image);
	{
		HE::UIElement& e = *t.find(logo);
		e.name = "Logo";
		HE::uiSetAnchorPreset(e, 0);
		e.pivotX = e.pivotY = 0.0f;
		e.posX = 235.0f; e.posY = 265.0f; e.sizeX = 550.0f; e.sizeY = 550.0f;
		e.texture = texRel;
		HE::UIAnimClip c;
		c.name = "Blend";
		c.duration = 1.0f;
		HE::UIAnimTrack tr;
		tr.element = logo;
		tr.prop    = "Render Opacity";
		tr.keys.push_back({ 0.0f,                  HE::UIPropValue::ofFloat(0.0f), HE::UIEase::Linear });
		tr.keys.push_back({ 0.050314463675022125f, HE::UIPropValue::ofFloat(1.0f), HE::UIEase::Linear });
		c.tracks.push_back(tr);
		t.animations.push_back(c);
	}
	UIWidgetAsset asset;
	asset.name     = "Startup";
	asset.path     = "UI/Startup.hasset";
	asset.treeJson = HE::uiWidgetTreeToJson(t);
	REQUIRE(cm.registerWidget(std::move(asset)) != HE::UUID{});

	HorizonWorld world;
	EditorUndo   undo;
	ContextBits  bits;
	AppContext   ctx = bits.make(world, undo);
	ctx.contentManager = &cm;
	Designer d{ ctx, (root / "UI" / "Startup.hasset").string() };
	d.beforeFrame = [] { static double now = 0.0; AssetThumbnailCache::beginFrame(now += 1.0 / 60.0); };
	// A 1920 px editor: at 1280 the canvas column is ~700 px and the
	// timeline's bar is cut off before "Stretch to Length".
	d.width = 1920;
	d.height = 1200;

	ThumbUploadRenderer stub;
	g_uploads107.clear();
	AssetThumbnailCache::setContext(&stub, &cm, "");
	for (int i = 0; i < 3; ++i) d.frame(false);

	ImGuiIO& io = ImGui::GetIO();
	Timeline107 tl{ d };
	REQUIRE(tl.tw);
	REQUIRE(tl.cw);
	// Open "Blend" through the clip combo.
	REQUIRE(tl.openClip("Blend"));

	// ── As authored: the fade is over in three frames, half a second in the
	// logo is long fully there. The runtime rule is unchanged, so this is the
	// same pop the user saw — the canvas shows it honestly.
	const Timeline107::Moments before = tl.threeMoments("repro107-all", "as-authored");
	const Timeline107::Fade fb = tl.fadeOf(before);
	MESSAGE("as authored: changed " << fb.changed << ", strictly between " << fb.inside
	        << ", outside " << fb.outside << ", mean " << fb.mean);
	CHECK(fb.changed > 2000);                   // the logo appears at all
	CHECK(fb.inside  < fb.changed / 100);        // …and is already all there
	CHECK(fb.mean    > 0.97);

	// ── "Stretch to Length": the last key lands on 1 s, and half a second in
	// the logo is half there. Found only now: it sits right of the time
	// readout, which is as wide as the moment it shows.
	const ImVec2 stretch = tl.onBar("Stretch to Length");
	REQUIRE(stretch.x >= 0.0f);
	tl.click(stretch);
	{
		HE::UIWidgetTree* live = UIEditorPanel::liveTree("UI/Startup.hasset");
		REQUIRE(live);
		REQUIRE(live->animations.size() == 1);
		REQUIRE(live->animations[0].tracks.size() == 1);
		REQUIRE(live->animations[0].tracks[0].keys.size() == 2);
		CHECK(live->animations[0].tracks[0].keys[1].time == 1.0f);
	}
	const Timeline107::Moments after = tl.threeMoments("repro107-all", "stretched");
	const Timeline107::Fade fa = tl.fadeOf(after);
	MESSAGE("stretched: changed " << fa.changed << ", strictly between " << fa.inside
	        << ", outside " << fa.outside << ", mean " << fa.mean);
	CHECK(fa.changed > 2000);
	CHECK(fa.outside < fa.changed / 100);
	CHECK(fa.inside  > fa.changed * 8 / 10);    // a fade across the picture…
	CHECK(fa.mean    > 0.35);                   // …about half way at half a second
	CHECK(fa.mean    < 0.65);

	// (1) + (4) on the same canvas, at every moment: the texture itself went up,
	// no 128 px tile, and none of the tile's checker greys where the page shows
	// through the picture's transparent corners.
	const int S = static_cast<int>(AssetThumbnailCache::thumbnailSize());
	bool tileSized = false, fullSized = false;
	for (const auto& [w, h] : g_uploads107)
	{
		if (w == S && h == S) tileSized = true;
		if (w >= 1024 || h >= 1024) fullSized = true;
	}
	CHECK_FALSE(tileSized);
	CHECK(fullSized);
	for (const he_ui::Image* img : { &after.start, &after.mid, &after.end })
	{
		CHECK(countGrey(*img, 90)  < 200);
		CHECK(countGrey(*img, 130) < 200);
	}

	AssetThumbnailCache::setContext(nullptr, nullptr, "");
	io.AddMousePosEvent(-1000.0f, -1000.0f);
	d.frame(false);
	UIEditorPanel::forget(d.assetPath);
	fs::remove_all(root, ec);
}

// Schritt 11: the user's own file, repaired with the Schritt 5 tool rather than
// a clip rebuilt from its numbers. Catania's Content/UI/Startup.hasset as saved
// on 28.09. (fixtures/catania_startup_107.h) is opened in the designer, "Blend"
// is opened, "Stretch to Length" clicked and the widget saved the way the
// editor saves it (UIEditorPanel::save → saveState → ContentManager::saveAsset).
// A fresh ContentManager then reads the file back: same UUID, the same widget
// and graph, only the key moved to 1 s. And the repaired file is played the way
// the Catania graph plays it — Forward on Construct, Backward after the Delay —
// through the game's path (playAnimation, tick, extract), where the logo's quad
// alpha now fades over a second instead of popping in three frames.
//     HE_UI_DUMP_DIR=/tmp/ui ./he_tests -tc="repro 107: the Catania*"
// HE_REPAIR107_CONTENT=<project>/Content does all of it to that project's own
// file, in place. Schritt 11 ran it once on ~/HorizonEngineProjects/Catania;
// run again on a repaired file it only checks.
TEST_CASE("repro 107: the Catania Startup widget repaired in the designer")
{
	Harness harness;
	namespace fs = std::filesystem;
	const char* project = std::getenv("HE_REPAIR107_CONTENT");
	const bool inPlace = project && *project;
	const fs::path root = inPlace ? fs::path(project)
	                              : fs::temp_directory_path() / "he_widget_designer_catania107";
	const fs::path file = root / "UI" / "Startup.hasset";
	const std::string rel = "UI/Startup.hasset", logoRel = "UI/Source/HE_Logo.hasset";
	std::error_code ec;
	if (!inPlace)
	{
		fs::remove_all(root, ec);
		REQUIRE(stageLogo107(root, "UI/Source") == logoRel);
		std::ofstream(file, std::ios::binary).write(
			reinterpret_cast<const char*>(he_test::kCataniaStartup107), he_test::kCataniaStartup107Size);
	}
	REQUIRE(fs::exists(file));
	REQUIRE(fs::exists(root / logoRel));

	// The file as it is before anything is touched.
	auto blendOf = [](const HE::UIWidgetTree& t) -> const HE::UIAnimClip*
	{
		for (const HE::UIAnimClip& c : t.animations) if (c.name == "Blend") return &c;
		return nullptr;
	};
	HE::UUID idBefore;
	HE::UIWidgetTree treeBefore;
	std::string graphBefore;
	{
		ContentManager cm0;
		cm0.setContentRoot(root.string());
		idBefore = cm0.loadAsset(rel);
		REQUIRE(idBefore != HE::UUID{});
		const UIWidgetAsset* a = cm0.getWidget(idBefore);
		REQUIRE(a);
		HE::uiWidgetTreeFromJson(a->treeJson, treeBefore);
		graphBefore = a->graphJson;
	}
	const HE::UIAnimClip* blend = blendOf(treeBefore);
	REQUIRE(blend);
	REQUIRE(blend->tracks.size() == 1);
	REQUIRE(blend->tracks[0].prop == "Render Opacity");
	REQUIRE(blend->tracks[0].keys.size() == 2);
	const int logo = blend->tracks[0].element;
	const bool authored = HE::uiAnimPlayEnd(*blend) < blend->duration - 0.0005f;
	MESSAGE("Blend as found: " << blend->duration << " s clip, second key at "
	        << blend->tracks[0].keys[1].time << " s, "
	        << std::string(authored ? "repairing" : "already repaired"));
	if (!inPlace)
	{
		REQUIRE(authored);
		CHECK(blend->tracks[0].keys[1].time == doctest::Approx(0.050314463675022125f));
	}

	// ── In the designer: open, look, stretch, look again, save.
	{
		ContentManager cm;
		cm.setContentRoot(root.string());
		HorizonWorld world;
		EditorUndo   undo;
		ContextBits  bits;
		AppContext   ctx = bits.make(world, undo);
		ctx.contentManager = &cm;
		Designer d{ ctx, file.string() };
		d.beforeFrame = [] { static double now = 0.0; AssetThumbnailCache::beginFrame(now += 1.0 / 60.0); };
		d.width = 1920;   // the whole timeline bar, "Stretch to Length" included
		d.height = 1200;
		ThumbUploadRenderer stub;
		AssetThumbnailCache::setContext(&stub, &cm, "");
		for (int i = 0; i < 3; ++i) d.frame(false);

		Timeline107 tl{ d };
		REQUIRE(tl.tw);
		REQUIRE(tl.cw);
		REQUIRE(tl.openClip("Blend"));

		if (authored)
		{
			const Timeline107::Moments m = tl.threeMoments("repro107-catania", "as-saved");
			const Timeline107::Fade f = tl.fadeOf(m);
			MESSAGE("designer, as saved: changed " << f.changed << ", strictly between " << f.inside
			        << ", mean " << f.mean);
			CHECK(f.changed > 2000);
			CHECK(f.mean > 0.97);       // half a second in, long fully there: the pop

			const ImVec2 stretch = tl.onBar("Stretch to Length");
			REQUIRE(stretch.x >= 0.0f);
			tl.click(stretch);
			CHECK(UIEditorPanel::isDirtyByContentPath(rel));
			REQUIRE(UIEditorPanel::save(ctx, d.assetPath));
			CHECK_FALSE(UIEditorPanel::isDirtyByContentPath(rel));
		}

		const Timeline107::Moments m = tl.threeMoments("repro107-catania", "repaired");
		const Timeline107::Fade f = tl.fadeOf(m);
		MESSAGE("designer, repaired: changed " << f.changed << ", strictly between " << f.inside
		        << ", outside " << f.outside << ", mean " << f.mean);
		CHECK(f.changed > 2000);
		CHECK(f.outside < f.changed / 100);
		CHECK(f.inside  > f.changed * 8 / 10);   // a fade across the logo…
		CHECK(f.mean    > 0.35);                  // …about half way at half a second
		CHECK(f.mean    < 0.65);

		AssetThumbnailCache::setContext(nullptr, nullptr, "");
		tl.io.AddMousePosEvent(-1000.0f, -1000.0f);
		d.frame(false);
		UIEditorPanel::forget(d.assetPath);
	}

	// ── Read back from disk, as the game (or the editor's hot reload) will.
	ContentManager cm;
	cm.setContentRoot(root.string());
	const HE::UUID idAfter = cm.loadAsset(rel);
	CHECK(idAfter == idBefore);   // nothing that points at the widget breaks
	const UIWidgetAsset* a = cm.getWidget(idAfter);
	REQUIRE(a);
	HE::UIWidgetTree treeAfter;
	HE::uiWidgetTreeFromJson(a->treeJson, treeAfter);
	const HE::UIAnimClip* fixed = blendOf(treeAfter);
	REQUIRE(fixed);
	REQUIRE(fixed->tracks.size() == 1);
	REQUIRE(fixed->tracks[0].keys.size() == 2);
	CHECK(fixed->duration == 1.0f);
	CHECK(fixed->tracks[0].keys[0].time == 0.0f);
	CHECK(fixed->tracks[0].keys[0].value.f == 0.0f);
	CHECK(fixed->tracks[0].keys[1].time == 1.0f);
	CHECK(fixed->tracks[0].keys[1].value.f == 1.0f);
	// Everything else as it was: the tree the Stretch button makes of the old
	// one, and the same graph. The graph compared with each node's pin defaults
	// in pin order: Node::pinDefaults is an unordered_map, so the order they
	// are written in flips with every load and save, and that is all that
	// differs.
	HE::UIWidgetTree expected = treeBefore;
	for (HE::UIAnimClip& c : expected.animations) HE::uiAnimStretchToLength(c);
	CHECK(HE::uiWidgetTreeToJson(treeAfter) == HE::uiWidgetTreeToJson(expected));
	auto pinOrdered = [](const std::string& graphJson)
	{
		nlohmann::json j = nlohmann::json::parse(graphJson);
		for (nlohmann::json& n : j["nodes"])
			if (n.contains("pinDefaults"))
				std::sort(n["pinDefaults"].begin(), n["pinDefaults"].end(),
				          [](const nlohmann::json& x, const nlohmann::json& y) { return x["i"] < y["i"]; });
		return j.dump(1);
	};
	CHECK(pinOrdered(a->graphJson) == pinOrdered(graphBefore));

	// ── Played the way Catania's graph plays it, through the game's path.
	WidgetManager wm;
	const int id = wm.createWidget(cm, rel);
	REQUIRE(id != 0);
	wm.showWidget(id);
	const HE::UUID texId = wm.tree(id)->find(logo)->textureAssetId;
	REQUIRE(texId != HE::UUID{});
	std::vector<UIRenderObject> out;
	for (const HE::UIAnimDirection dir : { HE::UIAnimDirection::Forward, HE::UIAnimDirection::Backward })
	{
		const std::string direction = HE::uiAnimDirectionName(dir);
		CAPTURE(direction);
		REQUIRE(wm.playAnimation(id, "Blend", nullptr, dir));
		std::string curve;
		int   distinct = 0;
		float prevA = -1.0f, maxStep = 0.0f, at30 = -1.0f;
		for (int f = 1; f <= 70; ++f)
		{
			wm.tick(1.0f / 60.0f);
			out.clear();
			wm.extract(1920.0f, 1080.0f, out);
			const UIRenderObject* q = nullptr;
			for (const auto& o : out) if (o.textureAssetId == texId) { q = &o; break; }
			REQUIRE(q);
			const float qa = q->color.a;
			if (prevA >= 0.0f)
			{
				maxStep = std::max(maxStep, std::fabs(qa - prevA));
				if (std::fabs(qa - prevA) > 1e-4f) ++distinct;
			}
			prevA = qa;
			if (f == 30) at30 = qa;
			if (f <= 3 || f % 10 == 0)
			{
				char buf[40];
				std::snprintf(buf, sizeof(buf), " f%d=%.3f", f, qa);
				curve += buf;
			}
		}
		MESSAGE("logo quad alpha per 60 Hz frame:" << curve);
		MESSAGE("distinct alpha steps " << distinct << ", largest step " << maxStep);
		// A second of fade: ~60 small steps, half way at half a second, then
		// held (restoreAfterCompleted is off in the graph) until the Delay's
		// Backward takes it down the same way.
		CHECK(distinct >= 55);
		CHECK(maxStep < 0.02f);
		CHECK(at30 == doctest::Approx(0.5f).epsilon(0.05));
		// Over after a second, standing at its end: fully in, or fully out.
		CHECK(prevA == doctest::Approx(dir == HE::UIAnimDirection::Forward ? 1.0f : 0.0f));
		CHECK_FALSE(wm.isPlayingAnimation(id, "Blend"));
	}

	if (!inPlace) fs::remove_all(root, ec);
}

// ── Thema 119, Schritt 4: Pre Construct at design time ──────────────────────
// The canvas shows what the widget's PreConstruct sets (a sandboxed run, see
// docs/widget-pre-construct-design.md §5) — and the document never does: not
// after the frame, not in the live asset a drag commits from INSIDE the frame,
// not in the saved file. The panel is grey as designed and green from the
// graph; the title says "designed" and the graph writes "Placeholder".
TEST_CASE("ui shot: widget designer — Pre Construct shows on the canvas, never in the document")
{
	Harness harness;
	namespace fs = std::filesystem;
	const fs::path root = tempRoot() / "precons";
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::create_directories(root / "UI");
	ContentManager cm;
	cm.setContentRoot(root.string());

	HE::UIWidgetTree t;
	t.canvasWidth = 1280.0f; t.canvasHeight = 720.0f;
	const int panel = t.add(HE::UIWidgetType::Panel);
	{
		HE::UIElement& e = *t.find(panel);
		e.name = "Card";
		HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
		e.posX = 300.0f; e.posY = 200.0f; e.sizeX = 500.0f; e.sizeY = 300.0f;
		e.setProp("Color", HE::UIPropValue::ofColor({ 0.4f, 0.4f, 0.4f, 1.0f }));
	}
	const int title = t.add(HE::UIWidgetType::Text);
	{
		HE::UIElement& e = *t.find(title);
		e.name = "Title";
		HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
		e.posX = 20.0f; e.posY = 20.0f; e.sizeX = 300.0f; e.sizeY = 40.0f;
		e.setProp("Text", HE::UIPropValue::ofString("designed"));
	}
	HorizonCode::Graph g;
	{
		HorizonCode::Node ev; ev.type = HorizonCode::NodeType::Event; ev.s = "PreConstruct";
		const int evId = g.addNode(ev);
		HorizonCode::Node col; col.type = HorizonCode::NodeType::ConstColor;
		col.f[0] = 0.1f; col.f[1] = 0.85f; col.f[2] = 0.2f; col.f[3] = 1.0f;
		const int colId = g.addNode(col);
		HorizonCode::Node setC; setC.type = HorizonCode::NodeType::SetProperty;
		setC.elem = panel; setC.s = "Color"; setC.propType = HorizonCode::PinType::Color;
		const int setCId = g.addNode(setC);
		REQUIRE(g.connect(evId, 0, setCId, 0));
		REQUIRE(g.connect(colId, 0, setCId, 2));
		HorizonCode::Node txt; txt.type = HorizonCode::NodeType::ConstString; txt.s = "Placeholder";
		const int txtId = g.addNode(txt);
		HorizonCode::Node setT; setT.type = HorizonCode::NodeType::SetProperty;
		setT.elem = title; setT.s = "Text"; setT.propType = HorizonCode::PinType::String;
		const int setTId = g.addNode(setT);
		REQUIRE(g.connect(setCId, 1, setTId, 0));
		REQUIRE(g.connect(txtId, 0, setTId, 2));
	}
	const std::string rel = "UI/PreCons.hasset";
	UIWidgetAsset asset;
	asset.name      = "PreCons";
	asset.path      = rel;
	asset.treeJson  = HE::uiWidgetTreeToJson(t);
	asset.graphJson = HorizonCode::toJson(g);
	const HE::UUID assetId = cm.registerWidget(std::move(asset));
	REQUIRE(assetId != HE::UUID{});

	HorizonWorld world;
	EditorUndo   undo;
	ContextBits  bits;
	AppContext   ctx = bits.make(world, undo);
	ctx.contentManager = &cm;
	Designer d{ ctx, (root / rel).string() };

	// What the document says about the two elements, read from any tree.
	const auto colorG = [&](const HE::UIWidgetTree& tr)
	{ return tr.find(panel)->getProp("Color").col.g; };
	const auto text = [&](const HE::UIWidgetTree& tr)
	{ return tr.find(title)->getProp("Text").s; };
	const auto assetTree = [&]()
	{
		HE::UIWidgetTree tr;
		const UIWidgetAsset* a = cm.getWidget(assetId);
		REQUIRE(a != nullptr);
		REQUIRE(HE::uiWidgetTreeFromJson(a->treeJson, tr));
		return tr;
	};
	const auto isGreen = [](std::uint8_t r, std::uint8_t g8, std::uint8_t b)
	{ return g8 > 170 && r < 70 && b < 90; };

	// ── The canvas shows the green PreConstruct sets… ───────────────────────
	const he_ui::Image img = d.shoot("widget-designer-pre-construct");
	REQUIRE(img.valid());
	long sx = 0, sy = 0, green = 0;
	for (int y = 0; y < img.height; ++y)
		for (int x = 0; x < img.width; ++x)
		{
			std::uint8_t r, g8, b, a;
			img.pixel(x, y, r, g8, b, a);
			if (isGreen(r, g8, b)) { sx += x; sy += y; ++green; }
		}
	MESSAGE("green canvas pixels: " << green);
	CHECK(green > 5000);

	// …while the document, between frames, still holds what was designed.
	HE::UIWidgetTree* live = UIEditorPanel::liveTree(rel);
	REQUIRE(live != nullptr);
	CHECK(colorG(*live) == doctest::Approx(0.4f));
	CHECK(text(*live) == "designed");

	// ── A drag commits from inside the canvas frame ─────────────────────────
	// Grab the green card where it is drawn and move it: on release the
	// designer pushes an undo snapshot and writes the live asset, with the
	// shown values still in the tree around it.
	REQUIRE(green > 0);
	const float px = float(sx) / float(green), py = float(sy) / float(green);
	const float posBefore = live->find(panel)->posX;
	ImGuiIO& io = ImGui::GetIO();
	io.AddMousePosEvent(px, py);
	d.frame(false);
	d.frame(true);
	for (int i = 1; i <= 6; ++i)
	{
		io.AddMousePosEvent(px + 12.0f * float(i), py + 3.0f * float(i));
		d.frame(true);
	}
	d.frame(false);
	d.frame(false);
	io.AddMousePosEvent(-1000.0f, -1000.0f);
	d.frame(false);

	live = UIEditorPanel::liveTree(rel);
	REQUIRE(live != nullptr);
	MESSAGE("card x " << posBefore << " -> " << live->find(panel)->posX);
	CHECK(live->find(panel)->posX != doctest::Approx(posBefore));   // the drag did commit
	CHECK(colorG(*live) == doctest::Approx(0.4f));
	CHECK(text(*live) == "designed");
	CHECK(UIEditorPanel::isDirtyByContentPath(rel));
	{
		const HE::UIWidgetTree committed = assetTree();   // the live asset PIE reads
		CHECK(committed.find(panel)->posX == doctest::Approx(live->find(panel)->posX));
		CHECK(colorG(committed) == doctest::Approx(0.4f));
		CHECK(text(committed) == "designed");
	}

	// ── …and the saved file ──────────────────────────────────────────────────
	REQUIRE(UIEditorPanel::saveByContentPath(ctx, rel));
	{
		const HE::UIWidgetTree saved = assetTree();
		CHECK(colorG(saved) == doctest::Approx(0.4f));
		CHECK(text(saved) == "designed");
	}

	UIEditorPanel::forget(d.assetPath);
	fs::remove_all(root, ec);
}
