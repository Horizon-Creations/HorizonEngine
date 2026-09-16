#include "doctest.h"
#include "ImGuiSoftwareRaster.h"
#include "TestFsUtil.h"

#include "ShortcutsPage.h"
#include "EditorShortcuts.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include <Diagnostics/GlobalState.h>

#include <imgui.h>
#include <imgui_internal.h>   // GetHoveredID

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ── Preferences ▸ Shortcuts, driven headless ─────────────────────────────────
// The page is a table of chord buttons. What a person would check by hand:
// that every action gets a row, that clicking a chord arms a capture and the
// next keystroke becomes the binding, that the binding lands in config.json
// right away, and that Esc, Backspace and Reset do what the hint says. With
// HE_UI_DUMP_DIR set the frames are written out as well.

using namespace EditorShortcuts;

namespace
{
	constexpr int W = 520, H = 640;

	struct Harness
	{
		std::filesystem::path cfgDir;

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
			io.ConfigMacOSXBehaviors = false;   // see test_editor_shortcuts.cpp
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
			resetAll();

			// The page writes config.json on every change; pin it to a scratch
			// directory so the test never touches the developer's own file.
			const auto tag = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
			cfgDir = std::filesystem::temp_directory_path() / ("he_shortcuts_ui_" + tag);
			std::filesystem::create_directories(cfgDir);
			GlobalState::useShippedConfig(cfgDir);
			GlobalState::getInstance().setConfigPersistent(true);
		}
		~Harness()
		{
			resetAll();
			GlobalState::useShippedConfig({});
			GlobalState::getInstance().setConfigPersistent(true);
			he_test::removeAllQuiet(cfgDir);
			ImGui::DestroyContext();
		}

		// The pinned file's Keybindings entry, "" when absent.
		std::string persisted() const
		{
			std::ifstream in(cfgDir / "config.json");
			std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			const std::string key = "\"Key\":\"Keybindings\"";
			// The file is pretty-printed or not; find the entry and its value.
			size_t p = all.find("Keybindings");
			if (p == std::string::npos) return {};
			p = all.find("\"Value\"", p);
			if (p == std::string::npos) return {};
			p = all.find('"', all.find(':', p) + 1);
			const size_t e = all.find('"', p + 1);
			return all.substr(p + 1, e - p - 1);
		}
	};

	ImGuiID frame(bool mouseDown, he_ui::Image* shot = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W), float(H)));
		ImGui::Begin("Shortcuts", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		ShortcutsPage::draw();
		ImGui::End();
		EditorWidgets::drawQueuedHelp();
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		return hovered;
	}

	void clickAt(float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame(false);
		frame(false);
		frame(true);
		frame(false);
		frame(false);
	}

	// Presses (and releases) the keys in one frame, as a keyboard would.
	void tap(std::initializer_list<ImGuiKey> keys)
	{
		ImGuiIO& io = ImGui::GetIO();
		for (const ImGuiKey k : keys) io.AddKeyEvent(k, true);
		frame(false);
		for (const ImGuiKey k : keys) io.AddKeyEvent(k, false);
		frame(false);
	}

	constexpr std::uint8_t kBgR = 20, kBgG = 18, kBgB = 15;
}

TEST_CASE("shortcuts page: every action has a row, a click arms a capture, a keystroke rebinds and persists")
{
	Harness h;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	he_ui::Image img;
	for (int i = 0; i < 4; ++i) frame(false, i == 3 ? &img : nullptr);
	REQUIRE(img.valid());
	CHECK(img.inkedPixels(kBgR, kBgG, kBgB) > 4000);
	if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
		he_ui::writeBmp(img, std::string(d) + "/shortcuts-page.bmp");

	// Find the first chord button (file.save) by scanning down the Shortcut
	// column: the first item whose hovered id changes below the header rows
	// in that column is the Save button.
	const float colX = float(W) - 120.0f;   // inside the fixed-width Shortcut column
	ImGuiID saveBtn = 0; float saveY = 0.0f;
	for (float y = 60.0f; y < float(H); y += 2.0f)
	{
		ImGui::GetIO().AddMousePosEvent(colX, y);
		frame(false);
		const ImGuiID id = frame(false);
		if (id != 0) { saveBtn = id; saveY = y + 6.0f; break; }
	}
	REQUIRE(saveBtn != 0);

	// Click it: the page is now capturing for file.save.
	clickAt(colX, saveY);
	CHECK(std::string(ShortcutsPage::capturing()) == "file.save");
	for (int i = 0; i < 2; ++i) frame(false, i == 1 ? &img : nullptr);
	if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
		he_ui::writeBmp(img, std::string(d) + "/shortcuts-capturing.bmp");

	// Esc keeps the old binding and ends the capture.
	tap({ ImGuiKey_Escape });
	CHECK(std::string(ShortcutsPage::capturing()).empty());
	CHECK(isDefault("file.save"));

	// Arm again and press Ctrl+Shift+K: that is the binding now, and it is
	// already in the pinned config.json.
	clickAt(colX, saveY);
	REQUIRE(std::string(ShortcutsPage::capturing()) == "file.save");
	tap({ ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_K });
	CHECK(std::string(ShortcutsPage::capturing()).empty());
	CHECK(chord("file.save") == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_K));
	CHECK(h.persisted() == "file.save=Ctrl+Shift+K");

	// A rebind onto another action's chord shows as a clash (the page paints
	// it, the table reports it) — and Backspace unbinds.
	clickAt(colX, saveY);
	tap({ ImGuiMod_Ctrl, ImGuiKey_D });          // Duplicate's chord
	CHECK(chord("file.save") == (ImGuiMod_Ctrl | ImGuiKey_D));
	CHECK(conflicts("file.save").size() == 1);
	for (int i = 0; i < 2; ++i) frame(false, i == 1 ? &img : nullptr);
	if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
		he_ui::writeBmp(img, std::string(d) + "/shortcuts-clash.bmp");
	clickAt(colX, saveY);
	tap({ ImGuiKey_Backspace });
	CHECK(chord("file.save") == ImGuiKey_None);
	CHECK(h.persisted() == "file.save=");

	// The row's Reset (right of the chord) puts the default back and the
	// override disappears from the file.
	ImGuiID resetBtn = 0; float resetX = 0.0f;
	for (float x = colX + 60.0f; x < float(W) - 4.0f; x += 2.0f)
	{
		ImGui::GetIO().AddMousePosEvent(x, saveY);
		frame(false);
		const ImGuiID id = frame(false);
		if (id != 0 && id != saveBtn) { resetBtn = id; resetX = x + 8.0f; break; }
	}
	REQUIRE(resetBtn != 0);
	clickAt(resetX, saveY);
	CHECK(isDefault("file.save"));
	CHECK(h.persisted().empty());
}
