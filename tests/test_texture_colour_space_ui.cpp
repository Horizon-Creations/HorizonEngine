#include "doctest.h"
#include "ImGuiSoftwareRaster.h"
#include "TestFsUtil.h"

#include "TextureColourSpaceDialog.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "ImporterCommon.h"      // textureSrgbOf: what the clicks wrote

#include <ContentManager/ContentManager.h>

#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName, GetHoveredID

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ── The texture colour-space dialog, driven headless ─────────────────────────
// What matters is that a click reaches the disk: Apply rewrites exactly the
// textures whose tick differs from what they carry, and a tick changed in the
// dialog is the flag the import writes. Same harness as the recovery dialog
// test; with HE_UI_DUMP_DIR set the frames are written out.

using namespace HE::Ed;
namespace fs = std::filesystem;

namespace
{
	constexpr int W = 900, H = 600;
	constexpr const char* kTitle = "Texture Color Space##texture_colour_space";

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

	// Everything AppContext insists on being given; the dialog reads the
	// refresh flags, the collab pointer (null: no session) and the key hook.
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

	ImGuiID frame(AppContext& ctx, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		TextureColourSpaceDialog::Draw(ctx);
		EditorWidgets::drawQueuedHelp();
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		return hovered;
	}

	ImGuiID idAt(AppContext& ctx, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame(ctx, false);
		return frame(ctx, false);
	}

	void clickAt(AppContext& ctx, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame(ctx, false);
		frame(ctx, false);
		frame(ctx, true);
		frame(ctx, false);
		frame(ctx, false);
	}

	ImGuiWindow* dialog()
	{
		ImGuiWindow* w = ImGui::FindWindowByName(kTitle);
		return (w && w->Active) ? w : nullptr;
	}

	// The primary button: the first item on the modal's last line.
	bool clickPrimary(AppContext& ctx)
	{
		ImGuiWindow* w = dialog();
		if (!w) return false;
		const ImGuiStyle& st = ImGui::GetStyle();
		const float y = w->Pos.y + w->Size.y - st.WindowPadding.y - ImGui::GetFrameHeight() * 0.5f;
		const float x = w->Pos.x + st.WindowPadding.x + 20.0f;
		if (idAt(ctx, x, y) == 0 || idAt(ctx, x, y) == w->ID) return false;
		clickAt(ctx, x, y);
		return true;
	}

	// A labelled button in the modal's own ID scope, found by walking a
	// vertical line down its left edge until the pointer hovers that id.
	bool clickButton(AppContext& ctx, const char* label)
	{
		ImGuiWindow* w = dialog();
		if (!w) return false;
		const ImGuiID want = ImHashStr(label, 0, w->ID);
		const float x = w->Pos.x + ImGui::GetStyle().WindowPadding.x + 6.0f;
		for (float y = w->Pos.y; y < w->Pos.y + w->Size.y; y += 4.0f)
		{
			// Walk right along this line too: the small buttons share a row.
			for (float dx = 0.0f; dx < 360.0f; dx += 12.0f)
				if (idAt(ctx, x + dx, y) == want)
				{
					clickAt(ctx, x + dx, y);
					return true;
				}
		}
		return false;
	}

	void dump(const he_ui::Image& img, const char* name)
	{
		if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
			he_ui::writeBmp(img, std::string(d) + "/" + name + ".bmp");
	}

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

	void saveTexture(const fs::path& root, const std::string& rel, bool srgb)
	{
		ContentManager cm(root.string());
		TextureAsset t;
		t.type = HE::AssetType::Texture;
		t.name = fs::path(rel).stem().string();
		t.path = rel;
		t.width = 1; t.height = 1; t.channels = 4;
		t.srgb = srgb;
		t.data = { 0x80, 0x80, 0x80, 0xFF };
		REQUIRE(cm.saveAsset(t));
	}

	void writePng1x1(const fs::path& file)
	{
		static const unsigned char kPng1x1[] = {
			0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
			0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
			0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
			0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
			0x00, 0x03, 0x01, 0x01, 0x00, 0xF7, 0x03, 0x41, 0x43, 0x00, 0x00, 0x00,
			0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
		};
		std::ofstream png(file, std::ios::binary | std::ios::trunc);
		REQUIRE(png);
		png.write(reinterpret_cast<const char*>(kPng1x1),
		          static_cast<std::streamsize>(sizeof(kPng1x1)));
	}
}

TEST_CASE("texture colour space ui: Apply writes the name guess, and only where it changes something")
{
	TempDir dir("he_test_texcs_ui_retag");
	const fs::path root = dir.path / "Content";
	// The legacy state: all linear, whatever they hold. One is already sRGB.
	saveTexture(root, "Textures/rock_albedo.hasset", false);
	saveTexture(root, "Textures/rock_normal.hasset", false);
	saveTexture(root, "Textures/Logo.hasset",        false);
	saveTexture(root, "UI/ui_icon.hasset",           true);
	const fs::path icon = root / "UI" / "ui_icon.hasset";
	const auto iconTime = fs::last_write_time(icon);

	Harness harness;
	ContextBits bits;
	AppContext ctx = bits.make();

	TextureColourSpaceDialog::openRetag({
		(root / "Textures" / "rock_albedo.hasset").string(),
		(root / "Textures" / "rock_normal.hasset").string(),
		(root / "Textures" / "Logo.hasset").string(),
		icon.string(),
		(dir.path / "Elsewhere" / "stray.hasset").string(),   // outside the root: dropped
	}, root.string());

	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	he_ui::Image img;
	for (int i = 0; i < 4; ++i) frame(ctx, false, i == 3 ? &img : nullptr);
	REQUIRE(dialog() != nullptr);
	dump(img, "texture-colour-space-retag");

	REQUIRE(clickPrimary(ctx));   // Apply
	for (int i = 0; i < 3; ++i) frame(ctx, false);
	CHECK(dialog() == nullptr);

	CHECK(Importer::textureSrgbOf(root / "Textures" / "rock_albedo.hasset") == true);
	CHECK(Importer::textureSrgbOf(root / "Textures" / "rock_normal.hasset") == false);
	CHECK(Importer::textureSrgbOf(root / "Textures" / "Logo.hasset")        == true);
	CHECK(Importer::textureSrgbOf(icon) == true);
	CHECK(fs::last_write_time(icon) == iconTime);   // nothing to change, not rewritten
	CHECK(bits.refreshPending);                     // the browser rescans afterwards
}

TEST_CASE("texture colour space ui: the tick is the flag the import writes")
{
	TempDir dir("he_test_texcs_ui_import");
	const fs::path src  = dir.path / "src";
	const fs::path root = dir.path / "Content";
	fs::create_directories(src);
	fs::create_directories(root);
	writePng1x1(src / "rock_albedo.png");
	writePng1x1(src / "rock_normal.png");

	Harness harness;
	ContextBits bits;
	AppContext ctx = bits.make();

	TextureColourSpaceDialog::openImport(
		{ (src / "rock_albedo.png").string(), (src / "rock_normal.png").string() },
		{ "Textures", "Textures" }, root.string());
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	he_ui::Image img;
	for (int i = 0; i < 4; ++i) frame(ctx, false, i == 3 ? &img : nullptr);
	REQUIRE(dialog() != nullptr);
	dump(img, "texture-colour-space-import");

	// Against the albedo's name: everything is data. Nothing on disk yet.
	REQUIRE(clickButton(ctx, "All Data"));
	CHECK_FALSE(fs::exists(root / "Textures" / "rock_albedo.hasset"));
	REQUIRE(clickPrimary(ctx));   // Import 2 Textures
	for (int i = 0; i < 3; ++i) frame(ctx, false);
	CHECK(dialog() == nullptr);

	CHECK(Importer::textureSrgbOf(root / "Textures" / "rock_albedo.hasset") == false);
	CHECK(Importer::textureSrgbOf(root / "Textures" / "rock_normal.hasset") == false);
	CHECK(bits.refreshPending);

	// Cancel imports nothing.
	TextureColourSpaceDialog::openImport({ (src / "rock_albedo.png").string() },
	                                     { "Other" }, root.string());
	bits.refreshPending = false;
	for (int i = 0; i < 4; ++i) frame(ctx, false);
	REQUIRE(dialog() != nullptr);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
	frame(ctx, false);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
	for (int i = 0; i < 3; ++i) frame(ctx, false);
	CHECK(dialog() == nullptr);
	CHECK_FALSE(fs::exists(root / "Other"));
	CHECK_FALSE(bits.refreshPending);
}
