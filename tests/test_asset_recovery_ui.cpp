#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "AssetRecoveryDialog.h"
#include "AssetAutosave.h"       // AssetRecoveryEntry
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "SceneAutosave.h"       // RecoveryInfo

#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName, the window list, GetHoveredID

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ── The asset recovery dialog, driven headless ───────────────────────────────
// SceneRecoveryDialog's test, one list wider: every answer has to reach the
// editor as the callback it names, with the KEY of the row it was pressed on,
// and the dialog has to wait for the scene's offer instead of fighting it for
// the one root-level modal slot. Same harness; HE_UI_DUMP_DIR writes frames.

using namespace HE::Ed;

namespace
{
	constexpr int W = 900, H = 560;
	constexpr const char* kTitle = "##AssetRecovery";

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

	AssetRecoveryEntry entry(const char* key, const char* rel)
	{
		AssetRecoveryEntry e;
		e.key          = key;
		e.relativePath = rel;
		e.targetPath   = std::string("/tmp/Proj/") + rel;
		e.snapshotPath = std::string("/tmp/Proj/Saved/Autosave/Assets/Pending/") + key + ".snap";
		e.savedAtUnix  = 1'789'000'000;
		return e;
	}

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

		// The editor's side, as EditorApplication::makeContext wires it: an
		// answered row leaves the list, Keep for Later empties it.
		std::vector<AssetRecoveryEntry> offers;
		RecoveryInfo sceneOffer;
		bool sceneOfferStands = false;
		std::vector<std::string> restored, deleted;
		int  deferred = 0;
		bool failRestore = false;

		AppContext make()
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
			ctx.assetRecoveryReplacedDir = "/tmp/Proj/Saved/Autosave/Assets/Replaced";
			ctx.restoreAssetRecovery = [this](const std::string& key, std::string* error) {
				if (failRestore)
				{
					if (error) *error = "cannot keep a copy of " + key;
					return false;
				}
				restored.push_back(key);
				std::erase_if(offers, [&](const AssetRecoveryEntry& e) { return e.key == key; });
				return true;
			};
			ctx.discardAssetRecovery = [this](const std::string& key) {
				deleted.push_back(key);
				std::erase_if(offers, [&](const AssetRecoveryEntry& e) { return e.key == key; });
			};
			ctx.deferAssetRecovery = [this] { ++deferred; offers.clear(); };
			return ctx;
		}
	};

	ImGuiID frame(ContextBits& bits, AppContext& ctx, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		ctx.assetRecoveryOffers = bits.offers.empty() ? nullptr : &bits.offers;
		ctx.recoveryOffer       = bits.sceneOfferStands ? &bits.sceneOffer : nullptr;
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		AssetRecoveryDialog::Draw(ctx);
		EditorWidgets::drawQueuedHelp();
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		return hovered;
	}

	ImGuiID idAt(ContextBits& bits, AppContext& ctx, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame(bits, ctx, false);
		return frame(bits, ctx, false);
	}

	void clickAt(ContextBits& bits, AppContext& ctx, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame(bits, ctx, false);
		frame(bits, ctx, false);
		frame(bits, ctx, true);
		frame(bits, ctx, false);
		frame(bits, ctx, false);
	}

	void settle(ContextBits& bits, AppContext& ctx, he_ui::Image* shot = nullptr)
	{
		ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
		for (int i = 0; i < 4; ++i) frame(bits, ctx, false, i == 3 ? shot : nullptr);
	}

	struct Item { ImGuiID id; float mid; };

	// The distinct items the pointer crosses walking right at y (a background
	// crossed twice is dropped), as in the scene dialog's test.
	std::vector<Item> itemsAcross(ContextBits& bits, AppContext& ctx, float y, float x0, float x1)
	{
		std::vector<Item> raw;
		ImGuiID last = 0; float start = x0;
		for (float x = x0; x < x1; x += 2.0f)
		{
			const ImGuiID id = idAt(bits, ctx, x, y);
			if (id == last) continue;
			if (last != 0) raw.push_back({ last, (start + x - 2.0f) * 0.5f });
			last = id; start = x;
		}
		if (last != 0) raw.push_back({ last, (start + x1) * 0.5f });
		std::vector<Item> out;
		for (const Item& it : raw)
		{
			int n = 0;
			for (const Item& o : raw) n += o.id == it.id;
			if (n == 1) out.push_back(it);
		}
		return out;
	}

	bool dialogShowing()
	{
		ImGuiWindow* w = ImGui::FindWindowByName(kTitle);
		return w && w->Active;
	}

	// The bottom row: Restore All, Delete All, Keep for Later.
	bool buttonRow(float& y, float& x0, float& x1)
	{
		ImGuiWindow* w = ImGui::FindWindowByName(kTitle);
		if (!w || !w->WasActive) return false;
		const ImGuiStyle& st = ImGui::GetStyle();
		y  = w->Pos.y + w->Size.y - st.WindowPadding.y - ImGui::GetFrameHeight() * 0.5f;
		x0 = w->Pos.x + 2.0f;
		x1 = w->Pos.x + w->Size.x - 2.0f;
		return true;
	}

	// The first row's buttons sit on its second line, inside the list child.
	bool firstRowButtons(float& y, float& x0, float& x1)
	{
		for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
		{
			if (!w->WasActive || std::string(w->Name).find("asset_recovery_rows") == std::string::npos)
				continue;
			const ImGuiStyle& st = ImGui::GetStyle();
			y  = w->Pos.y + st.WindowPadding.y + ImGui::GetTextLineHeightWithSpacing() +
			     ImGui::GetTextLineHeight() * 0.5f;
			x0 = w->Pos.x + 2.0f;
			x1 = w->Pos.x + w->Size.x - 2.0f;
			return true;
		}
		return false;
	}

	void dump(const he_ui::Image& img, const char* name)
	{
		if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
			he_ui::writeBmp(img, std::string(d) + "/" + name + ".bmp");
	}

	constexpr std::uint8_t kBgR = 20, kBgG = 18, kBgB = 15;
}

TEST_CASE("asset recovery ui: no offers, no dialog; and it waits for the scene's")
{
	Harness harness;
	ContextBits bits;
	AppContext ctx = bits.make();
	he_ui::Image img;
	settle(bits, ctx, &img);
	CHECK_FALSE(dialogShowing());
	CHECK(img.inkedPixels(kBgR, kBgG, kBgB) == 0);

	bits.offers = { entry("k1", "Content/Hero.hasset") };
	bits.sceneOfferStands = true;
	settle(bits, ctx);
	CHECK_FALSE(dialogShowing());      // the scene's offer is answered first

	bits.sceneOfferStands = false;
	settle(bits, ctx);
	CHECK(dialogShowing());
}

TEST_CASE("asset recovery ui: a row's Restore and Delete Copy name that row's key")
{
	Harness harness;
	ContextBits bits;
	bits.offers = { entry("k1", "Content/Materials/Rock.hasset"),
	                entry("k2", "Source/Player.h") };
	bits.offers[1].changedSince = true;
	AppContext ctx = bits.make();
	he_ui::Image img;
	settle(bits, ctx, &img);
	REQUIRE(dialogShowing());
	REQUIRE(img.valid());
	CHECK(img.inkedPixels(kBgR, kBgG, kBgB) > 4000);
	dump(img, "asset-recovery");

	float y = 0, x0 = 0, x1 = 0;
	REQUIRE(firstRowButtons(y, x0, x1));
	std::vector<Item> row = itemsAcross(bits, ctx, y, x0, x1);
	REQUIRE_MESSAGE(row.size() == 2, "found " << row.size() << " items across the first row");

	clickAt(bits, ctx, row[0].mid, y);            // Restore on Rock
	REQUIRE(bits.restored.size() == 1);
	CHECK(bits.restored[0] == "k1");
	CHECK(bits.deleted.empty());
	REQUIRE(bits.offers.size() == 1);
	settle(bits, ctx);
	CHECK(dialogShowing());                       // one row left

	REQUIRE(firstRowButtons(y, x0, x1));
	row = itemsAcross(bits, ctx, y, x0, x1);
	REQUIRE(row.size() == 2);
	clickAt(bits, ctx, row[1].mid, y);            // Delete Copy on Player.h
	REQUIRE(bits.deleted.size() == 1);
	CHECK(bits.deleted[0] == "k2");
	settle(bits, ctx);
	CHECK_FALSE(dialogShowing());                 // nothing left: it closes itself
	CHECK(bits.deferred == 0);
}

TEST_CASE("asset recovery ui: Restore All, Delete All, Keep for Later and Escape")
{
	Harness harness;
	ContextBits bits;
	AppContext ctx = bits.make();
	const std::vector<AssetRecoveryEntry> three = {
		entry("a", "Content/A.hasset"), entry("b", "Content/B.hasset"), entry("c", "Content/C.hasset") };

	// Keep for Later: deferred, nothing restored or deleted.
	bits.offers = three;
	settle(bits, ctx);
	float y = 0, x0 = 0, x1 = 0;
	REQUIRE(buttonRow(y, x0, x1));
	std::vector<Item> row = itemsAcross(bits, ctx, y, x0, x1);
	REQUIRE_MESSAGE(row.size() == 3, "found " << row.size() << " items across the button row");
	clickAt(bits, ctx, row[2].mid, y);
	CHECK(bits.deferred == 1);
	CHECK(bits.restored.empty());
	CHECK(bits.deleted.empty());
	settle(bits, ctx);
	CHECK_FALSE(dialogShowing());

	// Delete All: every key, once.
	bits.offers = three;
	settle(bits, ctx);
	REQUIRE(buttonRow(y, x0, x1));
	row = itemsAcross(bits, ctx, y, x0, x1);
	REQUIRE(row.size() == 3);
	clickAt(bits, ctx, row[1].mid, y);
	CHECK(bits.deleted == std::vector<std::string>{ "a", "b", "c" });
	settle(bits, ctx);
	CHECK_FALSE(dialogShowing());

	// Restore All that fails: the rows stay, the dialog stays, with the reason.
	bits.offers = three;
	bits.failRestore = true;
	settle(bits, ctx);
	REQUIRE(buttonRow(y, x0, x1));
	row = itemsAcross(bits, ctx, y, x0, x1);
	REQUIRE(row.size() == 3);
	clickAt(bits, ctx, row[0].mid, y);
	CHECK(bits.offers.size() == 3);
	he_ui::Image img;
	settle(bits, ctx, &img);
	CHECK(dialogShowing());
	dump(img, "asset-recovery-failed");

	// ...and when it works, all three go.
	bits.failRestore = false;
	REQUIRE(buttonRow(y, x0, x1));
	row = itemsAcross(bits, ctx, y, x0, x1);
	REQUIRE(row.size() == 3);
	clickAt(bits, ctx, row[0].mid, y);
	CHECK(bits.restored == std::vector<std::string>{ "a", "b", "c" });
	settle(bits, ctx);
	CHECK_FALSE(dialogShowing());

	// Escape is Keep for Later.
	bits.offers = three;
	settle(bits, ctx);
	REQUIRE(dialogShowing());
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
	frame(bits, ctx, false);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
	frame(bits, ctx, false);
	CHECK(bits.deferred == 2);
	settle(bits, ctx);
	CHECK_FALSE(dialogShowing());
}
