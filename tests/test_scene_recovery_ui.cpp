#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "SceneRecoveryDialog.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "SceneAutosave.h"       // RecoveryInfo

#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName: where the modal landed; GetHoveredID

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ── The recovery dialog, driven headless ─────────────────────────────────────
// The dialog is three buttons and a paragraph, and the three answers are what
// matters: each must reach the editor as the callback it names and nothing
// else, and the dialog must be gone afterwards. Also that it does not show up
// at all without an offer — the pointer being null is what keeps it off the
// screen during startup. Same harness as the mixer test; with HE_UI_DUMP_DIR
// set the frames are written out (scripts/he_uishot.py turns them into PNGs).

using namespace HE::Ed;

namespace
{
	constexpr int W = 800, H = 480;
	constexpr const char* kTitle = "##SceneRecovery";

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

	// Everything AppContext insists on being given; the dialog reads only the
	// four recovery members.
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

		// What the editor would do on each answer, counted.
		RecoveryInfo offer;
		bool answered  = false;
		int  restored  = 0, deleted = 0, deferred = 0;

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
			ctx.restoreRecovery = [this]{ ++restored; answered = true; return true; };
			ctx.discardRecovery = [this]{ ++deleted;  answered = true; };
			ctx.deferRecovery   = [this]{ ++deferred; answered = true; };
			return ctx;
		}
	};

	// One frame with the pointer where the caller put it. The offer pointer is
	// refreshed here the way EditorApplication::makeContext refreshes it every
	// frame: null once an answer has been given.
	ImGuiID frame(ContextBits& bits, AppContext& ctx, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		ctx.recoveryOffer = bits.answered ? nullptr : &bits.offer;
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		SceneRecoveryDialog::Draw(ctx);
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

	struct Item { ImGuiID id; float mid; };

	// Every distinct item the pointer crosses walking RIGHT at y; the modal's
	// own id (the background between buttons) is crossed more than once and
	// dropped, so what is left is the buttons in order.
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

	// The modal's window, once it has been drawn: the button row is its last
	// line, one frame height above the bottom padding.
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

	// Whether the modal was drawn in the frame that just ended. Asked of the
	// window rather than of ImGui::IsPopupOpen, which wants a current window
	// to hash the id against and there is none between frames.
	bool dialogShowing()
	{
		ImGuiWindow* w = ImGui::FindWindowByName(kTitle);
		return w && w->Active;
	}

	void dump(const he_ui::Image& img, const char* name)
	{
		if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
			he_ui::writeBmp(img, std::string(d) + "/" + name + ".bmp");
	}

	constexpr std::uint8_t kBgR = 20, kBgG = 18, kBgB = 15;
}

TEST_CASE("scene recovery ui: no offer, no dialog")
{
	Harness harness;
	ContextBits bits;
	bits.answered = true;   // = nothing pending
	AppContext ctx = bits.make();
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	he_ui::Image img;
	for (int i = 0; i < 3; ++i) frame(bits, ctx, false, i == 2 ? &img : nullptr);
	CHECK_FALSE(dialogShowing());
	CHECK(img.inkedPixels(kBgR, kBgG, kBgB) == 0);
}

TEST_CASE("scene recovery ui: the three answers reach the editor as the callbacks they name")
{
	Harness harness;
	ContextBits bits;
	bits.offer.scenePath    = "/tmp/Proj/Content/Scenes/Level1.hescene";
	bits.offer.projectPath  = "/tmp/Proj/Proj.hproject";
	bits.offer.snapshotPath = "/tmp/Proj/Saved/Autosave/recovery.hescene";
	bits.offer.revision     = 7;
	bits.offer.savedAtUnix  = 1'789'000'000;
	AppContext ctx = bits.make();

	// Pointer away, a few frames to settle; one shot to look at.
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	he_ui::Image img;
	for (int i = 0; i < 4; ++i) frame(bits, ctx, false, i == 3 ? &img : nullptr);
	REQUIRE(img.valid());
	CHECK(img.inkedPixels(kBgR, kBgG, kBgB) > 4000);
	dump(img, "scene-recovery");

	float y = 0.0f, x0 = 0.0f, x1 = 0.0f;
	REQUIRE(buttonRow(y, x0, x1));
	std::vector<Item> row = itemsAcross(bits, ctx, y, x0, x1);
	REQUIRE_MESSAGE(row.size() == 3, "found " << row.size() << " items across the button row");

	// Keep for Later: the offer is deferred, nothing else happens, the dialog
	// closes — and with the pointer null it does not come back.
	clickAt(bits, ctx, row[2].mid, y);
	CHECK(bits.deferred == 1);
	CHECK(bits.restored == 0);
	CHECK(bits.deleted  == 0);
	for (int i = 0; i < 3; ++i) frame(bits, ctx, false);
	CHECK_FALSE(dialogShowing());

	// The offer stands again (as it would at the next start): Restore.
	bits.answered = false;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) frame(bits, ctx, false);
	REQUIRE(buttonRow(y, x0, x1));
	row = itemsAcross(bits, ctx, y, x0, x1);
	REQUIRE(row.size() == 3);
	clickAt(bits, ctx, row[0].mid, y);
	CHECK(bits.restored == 1);
	CHECK(bits.deleted  == 0);
	CHECK(bits.deferred == 1);
	for (int i = 0; i < 3; ++i) frame(bits, ctx, false);
	CHECK_FALSE(dialogShowing());

	// And once more: Delete Snapshot.
	bits.answered = false;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) frame(bits, ctx, false);
	REQUIRE(buttonRow(y, x0, x1));
	row = itemsAcross(bits, ctx, y, x0, x1);
	REQUIRE(row.size() == 3);
	clickAt(bits, ctx, row[1].mid, y);
	CHECK(bits.deleted  == 1);
	CHECK(bits.restored == 1);
	CHECK(bits.deferred == 1);
	for (int i = 0; i < 3; ++i) frame(bits, ctx, false);
	CHECK_FALSE(dialogShowing());
}

TEST_CASE("scene recovery ui: Escape is Keep for Later, and an unsaved scene is named as such")
{
	Harness harness;
	ContextBits bits;
	bits.offer.snapshotPath = "/tmp/Proj/Saved/Autosave/recovery.hescene";   // scenePath empty
	AppContext ctx = bits.make();

	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	he_ui::Image img;
	for (int i = 0; i < 4; ++i) frame(bits, ctx, false, i == 3 ? &img : nullptr);
	REQUIRE(dialogShowing());
	dump(img, "scene-recovery-unsaved");

	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
	frame(bits, ctx, false);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
	frame(bits, ctx, false);
	CHECK(bits.deferred == 1);
	CHECK(bits.restored == 0);
	CHECK(bits.deleted  == 0);
	for (int i = 0; i < 3; ++i) frame(bits, ctx, false);
	CHECK_FALSE(dialogShowing());
}
