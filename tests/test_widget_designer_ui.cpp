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
// it from the thumbnail cache, which needs a renderer to upload into; headless
// there is none, so the Image element draws its empty placeholder. The DETAILS
// side, which is what these shots are for, is complete. The Thema 107 cases at
// the bottom hand the cache a stub renderer whose uploads land in the software
// rasterizer, so there the canvas shows exactly the picture the cache made.

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
		// Called before every frame — what the editor's own loop does around the
		// panel (the thumbnail cache's per-frame budget, Thema 107).
		void (*beforeFrame)() = nullptr;

		// One frame of the designer filling the whole screen. Returns the id
		// ImGui says the pointer is on; rasterises into `shot` when given one.
		ImGuiID frame(bool left, he_ui::Image* shot = nullptr)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(float(W), float(height));
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, left);
			if (beforeFrame) beforeFrame();
			ImGui::NewFrame();
			UIEditorPanel::render(ctx, assetPath, ImVec2(0.0f, 0.0f),
			                      ImVec2(float(W), float(height)));
			EditorWidgets::drawQueuedHelp();
			const ImGuiID hovered = ImGui::GetHoveredID();
			ImGui::Render();
			if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, height);
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

// ── Thema 107: reproductions of the reported designer bugs ───────────────────
// Diagnosis only (Schritt 1). These pin what the designer does TODAY, so the fix
// can flip them; see docs/widget-designer-bugs-107-diagnose.md.
namespace
{
	// Every texture the thumbnail cache uploads, and at what size.
	std::vector<std::pair<int, int>> g_uploads107;

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
}

// (1) + (4): the canvas draws an Image from the Content Browser's 128 px tile —
// checkerboard baked in, alpha forced opaque, 8x downscaled from 1024.
// Skipped by default because it pins the BUG; run with --no-skip:
//     HE_UI_DUMP_DIR=/tmp/ui ./he_tests -tc="repro 107*" --no-skip
// HE_REPRO107_LOGO=<path to a texture .hasset> uses that picture instead of the
// generated one (e.g. the Catania project's UI/Source/HE_Logo.hasset).
TEST_CASE("repro 107: designer canvas draws the thumbnail tile, checker and all"
          * doctest::skip())
{
	Harness harness;
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "he_widget_designer_repro107";
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::create_directories(root / "UI");
	fs::create_directories(root / "Src");

	std::string texRel;
	if (const char* logo = std::getenv("HE_REPRO107_LOGO"); logo && *logo && fs::exists(logo))
	{
		fs::copy_file(logo, root / "UI" / "HE_Logo.hasset", fs::copy_options::overwrite_existing);
		texRel = "UI/HE_Logo.hasset";
	}
	else
	{
		constexpr int S = 1024;
		writeTga(root / "Src" / "Logo.tga", S, S, logoLikePicture(S));
		TextureImporter::ImportSettings s;
		s.srgb = true;   // what the import dialog guesses for a logo
		REQUIRE(TextureImporter::import(root / "Src" / "Logo.tga", root, "UI", s));
		for (const auto& e : fs::directory_iterator(root / "UI"))
			if (e.path().extension() == ".hasset") texRel = "UI/" + e.path().filename().string();
	}
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

	// (4) What reached the canvas is the tile, not the texture.
	const int S = static_cast<int>(AssetThumbnailCache::thumbnailSize());
	REQUIRE(!g_uploads107.empty());
	bool tileSized = false, fullSized = false;
	for (const auto& [w, h] : g_uploads107)
	{
		MESSAGE("upload " << w << "x" << h);
		if (w == S && h == S) tileSized = true;
		if (w >= 1024 || h >= 1024) fullSized = true;
	}
	CHECK(tileSized);
	CHECK_FALSE(fullSized);

	// (1) The transparent background shows the tile's checkerboard (90/130
	// grey) where the canvas should show through.
	const int n90 = countGrey(img, 90), n130 = countGrey(img, 130);
	MESSAGE("checker greys: control " << ctl90 << "/" << ctl130 << ", with tile " << n90 << "/" << n130);
	CHECK(n90  > ctl90  + 2000);
	CHECK(n130 > ctl130 + 2000);

	AssetThumbnailCache::setContext(nullptr, nullptr, "");
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
