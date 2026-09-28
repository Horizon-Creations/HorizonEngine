#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "TextureViewerPanel.h"
#include "TextureImporter.h"
#include "EditorApplication.h"   // AppContext
#include "EditorAssetTypeCache.h"
#include "EditorHelp.h"
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorUndo.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <HorizonScene/HorizonWorld.h>

#include <imgui.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

// ── The texture viewer (Thema 92, Schritt 4) ─────────────────────────────────
// What has to hold: the viewer shows a picture THE RIGHT WAY UP — top row on
// top — both for an image not imported yet (the preview before the Import
// button) and for the asset the import writes, although the importer stores
// rows bottom-up. And the import opens the asset the importer actually wrote.
//
// The pixel half is checked on toDisplayRgba directly and on what the real
// panel hands to its texture upload; the picture half is a headless shot of the
// real TextureViewerPanel::render:
//
//     HE_UI_DUMP_DIR=/tmp/ui ./he_tests -tc="ui shot: texture viewer*"

namespace fs = std::filesystem;

namespace
{
	// An uncompressed 32-bit TGA, rows given top-down and written with the
	// top-left-origin bit set. A real importable extension (.tga), and no
	// encoder needed — stb_image reads it through the importer's own path.
	std::vector<uint8_t> tga(int w, int h, const std::vector<uint8_t>& rgbaTopDown)
	{
		std::vector<uint8_t> b(18, 0);
		b[2]  = 2;                                   // uncompressed true-colour
		b[12] = uint8_t(w & 0xFF); b[13] = uint8_t(w >> 8);
		b[14] = uint8_t(h & 0xFF); b[15] = uint8_t(h >> 8);
		b[16] = 32;
		b[17] = 0x28;                                // 8 alpha bits, top-left origin
		for (size_t i = 0; i + 3 < rgbaTopDown.size(); i += 4)
		{
			b.push_back(rgbaTopDown[i + 2]);         // TGA is BGRA
			b.push_back(rgbaTopDown[i + 1]);
			b.push_back(rgbaTopDown[i + 0]);
			b.push_back(rgbaTopDown[i + 3]);
		}
		return b;
	}

	// The 2x2 picture test_texture_orientation.cpp uses, which tells all four
	// mirrorings apart: top-left red, top-right green, bottom-left blue,
	// bottom-right white.
	std::vector<uint8_t> quadrantTga()
	{
		return tga(2, 2, {
			255,   0,   0, 255,     0, 255,   0, 255,
			  0,   0, 255, 255,   255, 255, 255, 255,
		});
	}

	// Something a person can tell the orientation of at a glance: an "F"
	// (asymmetric both ways) on a sky-to-sunset gradient, with the right quarter
	// fading out so the checkerboard behind it shows.
	std::vector<uint8_t> showcaseTga(int& wOut, int& hOut)
	{
		const int w = 96, h = 64;
		std::vector<uint8_t> px(size_t(w) * h * 4);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				uint8_t* p = &px[(size_t(y) * w + x) * 4];
				const float t = float(y) / float(h - 1);   // 0 at the top
				p[0] = uint8_t( 90 + t * 150);
				p[1] = uint8_t(160 - t *  60);
				p[2] = uint8_t(230 - t * 170);
				p[3] = x < 72 ? 255 : uint8_t(255 - (x - 72) * 255 / 24);
				const bool stem = x >= 14 && x < 24 && y >= 10 && y < 54;
				const bool top  = x >= 14 && x < 56 && y >= 10 && y < 20;
				const bool mid  = x >= 14 && x < 46 && y >= 28 && y < 37;
				if (stem || top || mid) { p[0] = p[1] = p[2] = 250; p[3] = 255; }
			}
		wOut = w; hOut = h;
		return tga(w, h, px);
	}

	struct Rgba { int r, g, b, a; };
	Rgba at(const std::vector<uint8_t>& topDown, int w, int x, int y)
	{
		const size_t o = (size_t(y) * w + x) * 4;
		return { topDown[o], topDown[o + 1], topDown[o + 2], topDown[o + 3] };
	}
	bool is(const Rgba& c, int r, int g, int b)
	{
		auto near = [](int v, int e) { return v >= e - 2 && v <= e + 2; };
		return near(c.r, r) && near(c.g, g) && near(c.b, b);
	}

	void writeFile(const fs::path& p, const std::vector<uint8_t>& bytes)
	{
		fs::create_directories(p.parent_path());
		std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
		                                         std::streamsize(bytes.size()));
	}

	fs::path freshDir(const char* tag)
	{
		std::mt19937_64 rng{ std::random_device{}() };
		const fs::path d = fs::temp_directory_path() /
			(std::string("he_texview_") + tag + "_" + std::to_string(rng() & 0xFFFFFFu));
		fs::create_directories(d);
		return d;
	}

	// ── Headless ImGui + a real AppContext, as test_widget_designer_ui.cpp ──
	constexpr int W = 1100, H = 620;

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
			HE::Ed::applyHorizonDarkTheme();
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

	// What the panel handed to its texture upload last — the picture exactly as
	// it goes to the GPU — and the software rasterizer's copy of it for the shots.
	struct Uploads
	{
		std::vector<uint8_t> last;
		int w = 0, h = 0, count = 0, released = 0;
	};
	Uploads g_uploads;

	struct HookScope
	{
		HookScope()
		{
			g_uploads = {};
			TextureViewerPanel::setTextureHooks(
				[](const void* rgba, int w, int h) -> void* {
					const auto* p = static_cast<const uint8_t*>(rgba);
					g_uploads.last.assign(p, p + size_t(w) * h * 4);
					g_uploads.w = w; g_uploads.h = h; ++g_uploads.count;
					return reinterpret_cast<void*>(static_cast<uintptr_t>(he_ui::registerTexture(rgba, w, h)));
				},
				[](void* t) {
					he_ui::unregisterTexture(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(t)));
					++g_uploads.released;
				});
		}
		~HookScope() { TextureViewerPanel::setTextureHooks({}, {}); }
	};

	// One frame of the viewer filling the screen; rasterised into `shot` when given.
	void frame(AppContext& ctx, const std::string& path, he_ui::Image* shot = nullptr,
	           ImVec2 mouse = ImVec2(-1.0f, -1.0f))
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		ImGui::NewFrame();
		TextureViewerPanel::beginFrame();
		TextureViewerPanel::render(ctx, path, ImVec2(0.0f, 0.0f), ImVec2(float(W), float(H)));
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
	}

	void dump(const he_ui::Image& img, const char* name)
	{
		if (const char* dir = std::getenv("HE_UI_DUMP_DIR"))
		{
			fs::create_directories(dir);
			CHECK(he_ui::writeBmp(img, (fs::path(dir) / name).string()));
		}
	}
}

TEST_CASE("texture viewer: the picture's top row is shown on top")
{
	// The importer stores rows bottom-up; the viewer has to turn that back.
	const std::vector<uint8_t> file = quadrantTga();
	auto tex = TextureImporter::decodeFromMemory(file.data(), file.size());
	REQUIRE(tex);
	// Stored bottom-up, as test_texture_orientation.cpp pins: data row 0 is blue.
	REQUIRE(tex->data[0] == 0);
	REQUIRE(tex->data[2] == 255);

	std::vector<uint8_t> shown;
	REQUIRE(TextureViewerPanel::toDisplayRgba(*tex, shown));
	REQUIRE(shown.size() == 2 * 2 * 4);
	CHECK(is(at(shown, 2, 0, 0), 255,   0,   0));   // top-left red
	CHECK(is(at(shown, 2, 1, 0),   0, 255,   0));   // top-right green
	CHECK(is(at(shown, 2, 0, 1),   0,   0, 255));   // bottom-left blue
	CHECK(is(at(shown, 2, 1, 1), 255, 255, 255));   // bottom-right white

	SUBCASE("one and three channels are widened, opaque")
	{
		TextureAsset g;
		g.width = 1; g.height = 2; g.channels = 1;
		g.data = { 10, 200 };                         // bottom 10, top 200
		REQUIRE(TextureViewerPanel::toDisplayRgba(g, shown));
		CHECK(is(at(shown, 1, 0, 0), 200, 200, 200));
		CHECK(at(shown, 1, 0, 0).a == 255);
		CHECK(is(at(shown, 1, 0, 1), 10, 10, 10));

		TextureAsset c;
		c.width = 1; c.height = 1; c.channels = 3;
		c.data = { 1, 2, 3 };
		REQUIRE(TextureViewerPanel::toDisplayRgba(c, shown));
		CHECK(is(at(shown, 1, 0, 0), 1, 2, 3));
		CHECK(at(shown, 1, 0, 0).a == 255);
	}
	SUBCASE("block-compressed and short data are refused, not read past")
	{
		TextureAsset bc = *tex;
		bc.format = TextureFormat::BC7;
		CHECK_FALSE(TextureViewerPanel::toDisplayRgba(bc, shown));
		TextureAsset shortData = *tex;
		shortData.data.resize(5);
		CHECK_FALSE(TextureViewerPanel::toDisplayRgba(shortData, shown));
	}
}

TEST_CASE("texture viewer: channel switches")
{
	const std::vector<uint8_t> px = { 200, 100, 50, 128 };
	std::vector<uint8_t> out;
	using namespace TextureViewerPanel;

	applyChannelMask(px, kChannelAll, out);
	CHECK(out == px);

	applyChannelMask(px, kChannelR | kChannelG | kChannelB, out);   // alpha off → opaque
	CHECK(out == std::vector<uint8_t>{ 200, 100, 50, 255 });

	applyChannelMask(px, kChannelR | kChannelA, out);
	CHECK(out == std::vector<uint8_t>{ 200, 0, 0, 128 });

	applyChannelMask(px, kChannelG, out);                           // one alone → grey
	CHECK(out == std::vector<uint8_t>{ 100, 100, 100, 255 });

	applyChannelMask(px, kChannelA, out);
	CHECK(out == std::vector<uint8_t>{ 128, 128, 128, 255 });
}

TEST_CASE("texture viewer: every control has a tooltip")
{
	for (const char* label : { "Fit", "1:1", "Red", "Green", "Blue", "Alpha", "Checkerboard",
	                           "Import as Texture Asset" })
		CHECK_MESSAGE(HE::Ed::Help::findKey(std::string("Texture Viewer/") + label) != nullptr,
		              "no help entry for Texture Viewer/", label);
}

TEST_CASE("texture viewer: which files it opens")
{
	using TextureViewerPanel::isImageSource;
	CHECK(isImageSource("/p/Content/a.png"));
	CHECK(isImageSource("/p/Content/a.PNG"));
	CHECK(isImageSource("/p/Content/a.jpeg"));
	CHECK(isImageSource("/p/Content/a.tga"));
	CHECK(isImageSource("/p/Content/a.bmp"));
	CHECK_FALSE(isImageSource("/p/Content/a.hasset"));
	CHECK_FALSE(isImageSource("/p/Content/a.ppm"));   // stb reads it, the importer does not route it
	CHECK_FALSE(isImageSource("/p/Content/png"));
	CHECK_FALSE(isImageSource("/p/Content/a.pn"));
}

TEST_CASE("texture viewer: preview before import, then the imported asset, both upright")
{
	Harness      harness;
	HookScope    hooks;
	HorizonWorld world;
	EditorUndo   undo;
	ContextBits  bits;
	AppContext   ctx = bits.make(world, undo);

	const fs::path root = freshDir("import");
	ContentManager cm;
	cm.setContentRoot(root.string());
	ctx.contentManager = &cm;

	const fs::path src = root / "Textures" / "quadrants.tga";
	writeFile(src, quadrantTga());

	// ── The raw file: what the Import button would store, shown upright ──────
	REQUIRE(TextureViewerPanel::isTextureAsset(src.string()));
	frame(ctx, src.string());
	REQUIRE(g_uploads.count == 1);
	REQUIRE(g_uploads.w == 2);
	REQUIRE(g_uploads.h == 2);
	CHECK(is(at(g_uploads.last, 2, 0, 0), 255, 0, 0));
	CHECK(is(at(g_uploads.last, 2, 0, 1), 0, 0, 255));
	// Once uploaded, a frame does not upload again.
	frame(ctx, src.string());
	CHECK(g_uploads.count == 1);

	// ── The import: the path the viewer opens is the file the importer wrote ─
	const std::string written = TextureViewerPanel::importImage(src, root, "Textures");
	REQUIRE_FALSE(written.empty());
	CHECK(fs::path(written) == (root / "Textures" / "quadrants.hasset").make_preferred());
	CHECK(fs::exists(written));
	CHECK(TextureViewerPanel::isTextureAsset(written));
	CHECK_FALSE(TextureViewerPanel::isImageSource(written));

	TextureViewerPanel::requestOpen(written, src.string());
	const auto req = TextureViewerPanel::takeOpenRequest();
	CHECK(req.path == written);
	CHECK(req.replacing == src.string());
	CHECK(TextureViewerPanel::takeOpenRequest().path.empty());

	// ── The asset, loaded through the ContentManager: same picture, same way up
	frame(ctx, written);
	REQUIRE(g_uploads.count == 2);
	CHECK(is(at(g_uploads.last, 2, 0, 0), 255, 0, 0));
	CHECK(is(at(g_uploads.last, 2, 1, 0), 0, 255, 0));
	CHECK(is(at(g_uploads.last, 2, 0, 1), 0, 0, 255));
	CHECK(is(at(g_uploads.last, 2, 1, 1), 255, 255, 255));
	// Copied out and let go: the viewer does not keep the asset resident.
	CHECK_FALSE(cm.isLoaded(std::string("Textures/quadrants.hasset")));

	// Only images go through importImage; anything else is refused before the importer.
	CHECK(TextureViewerPanel::importImage(root / "Textures" / "notes.txt", root, "Textures").empty());

	// ── Closing the tabs frees their textures — at the next frame, not at once
	TextureViewerPanel::forget(src.string());
	TextureViewerPanel::forget(written);
	CHECK(g_uploads.released == 0);
	frame(ctx, written);   // reopens `written` (a fresh upload), and frees the two
	CHECK(g_uploads.released == 2);
	TextureViewerPanel::forget(written);
	TextureViewerPanel::beginFrame();

	std::error_code ec;
	fs::remove_all(root, ec);
}

TEST_CASE("ui shot: texture viewer, image file before import and asset after")
{
	Harness      harness;
	HookScope    hooks;
	HorizonWorld world;
	EditorUndo   undo;
	ContextBits  bits;
	AppContext   ctx = bits.make(world, undo);

	const fs::path root = freshDir("shot");
	ContentManager cm;
	cm.setContentRoot(root.string());
	ctx.contentManager = &cm;

	int iw = 0, ih = 0;
	const fs::path src = root / "Textures" / "Signpost.tga";
	writeFile(src, showcaseTga(iw, ih));

	// Pointer over the upper stroke of the "F", so the readout row has a value.
	const ImVec2 mouse{ 260.0f + (W - 260.0f) * 0.5f - 10.0f, H * 0.5f - 60.0f };

	he_ui::Image before;
	frame(ctx, src.string(), nullptr, mouse);
	frame(ctx, src.string(), &before, mouse);
	REQUIRE(before.valid());
	dump(before, "texture_viewer_before_import.bmp");

	const std::string written = TextureViewerPanel::importImage(src, root, "Textures");
	REQUIRE_FALSE(written.empty());
	he_ui::Image after;
	frame(ctx, written, nullptr, mouse);
	frame(ctx, written, &after, mouse);
	REQUIRE(after.valid());
	dump(after, "texture_viewer_asset.bmp");

	// The picture is really on the canvas, the right way up: fit leaves an equal
	// margin, so the canvas centre column near the top edge of the picture is
	// in the gradient's TOP (sky blue, blue > red), near the bottom in its
	// sunset (red > blue).
	const float paneX0 = 268.0f, paneX1 = float(W) - 8.0f;
	const int   cx     = int((paneX0 + paneX1) * 0.5f) + 20;   // right of the F's stem, left of the fade
	int topY = -1, botY = -1;
	for (int y = 0; y < H; ++y)
	{
		uint8_t r, g, b, a;
		after.pixel(cx, y, r, g, b, a);
		const bool picture = std::abs(int(r) - int(b)) > 60;
		if (picture) { if (topY < 0) topY = y; botY = y; }
	}
	REQUIRE(topY >= 0);
	REQUIRE(botY > topY + 40);
	uint8_t r, g, b, a;
	after.pixel(cx, topY + 3, r, g, b, a);
	CHECK(int(b) > int(r));     // sky at the top
	after.pixel(cx, botY - 3, r, g, b, a);
	CHECK(int(r) > int(b));     // sunset at the bottom

	TextureViewerPanel::forget(src.string());
	TextureViewerPanel::forget(written);
	TextureViewerPanel::beginFrame();
	std::error_code ec;
	fs::remove_all(root, ec);
}
