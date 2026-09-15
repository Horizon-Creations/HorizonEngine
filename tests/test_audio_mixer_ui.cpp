#include "doctest.h"
#include "ImGuiSoftwareRaster.h"
#include "TestFsUtil.h"

#include "AudioMixerPanel.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorUndo.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"

#include <HorizonScene/AudioEngine.h>
#include "ProjectManager.h"

#include <imgui.h>
#include <imgui_internal.h>   // GetHoveredID: which item the pointer is on

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// ── The Audio Mixer, driven headless ─────────────────────────────────────────
// The window is strips of unlabelled controls — a fader, an M, an S — whose
// only text is a bus name that comes from the project. What can be checked
// without a person is what the outliner test checks: that the real panel, with
// a real ProjectData and a real (device-less) AudioEngine, puts the buses on
// screen, and that a click on M reaches the engine as a mute and a click on S
// as a solo. With HE_UI_DUMP_DIR set the frames are written out as well
// (scripts/he_uishot.py turns them into PNGs).

using namespace HE::Ed;

namespace
{
	constexpr int W = 560, H = 380;

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

	// Everything AppContext insists on being given. The panel reads
	// projectManager and audioEngine; the rest is here because the struct has
	// no defaults for its references.
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

		AppContext make(ProjectManager& pm, AudioEngine& audio)
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
			ctx.projectManager = &pm;
			ctx.audioEngine    = &audio;
			return ctx;
		}
	};

	// One frame of the window at a fixed place, with the pointer where the
	// caller put it. Returns the id ImGui says the pointer is on.
	ImGuiID frame(AppContext& ctx, bool mouseDown, he_ui::Image* shot = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W) - 20.0f, float(H) - 20.0f));
		bool open = true;
		AudioMixerPanel::DrawAudioMixerWindow(ctx, open);
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

	struct Item { ImGuiID id; float mid; };

	// The strips live in a child window, and the space between two buttons is
	// the child itself — ImGui reports its id as hovered there. A control is
	// crossed once on a straight walk; the child is crossed between every
	// pair, so anything seen more than once is background and dropped.
	std::vector<Item> withoutBackground(std::vector<Item> items)
	{
		std::vector<Item> out;
		for (const Item& it : items)
		{
			int n = 0;
			for (const Item& o : items) n += o.id == it.id;
			if (n == 1) out.push_back(it);
		}
		return out;
	}

	// Every distinct item the pointer crosses walking DOWN at x, top to bottom.
	std::vector<Item> itemsDown(AppContext& ctx, float x, float y0, float y1)
	{
		std::vector<Item> out;
		ImGuiID last = 0; float start = y0;
		for (float y = y0; y < y1; y += 2.0f)
		{
			const ImGuiID id = idAt(ctx, x, y);
			if (id == last) continue;
			if (last != 0) out.push_back({ last, (start + y - 2.0f) * 0.5f });
			last = id; start = y;
		}
		if (last != 0) out.push_back({ last, (start + y1) * 0.5f });
		return withoutBackground(std::move(out));
	}

	// …and walking RIGHT at y.
	std::vector<Item> itemsAcross(AppContext& ctx, float y, float x0, float x1)
	{
		std::vector<Item> out;
		ImGuiID last = 0; float start = x0;
		for (float x = x0; x < x1; x += 2.0f)
		{
			const ImGuiID id = idAt(ctx, x, y);
			if (id == last) continue;
			if (last != 0) out.push_back({ last, (start + x - 2.0f) * 0.5f });
			last = id; start = x;
		}
		if (last != 0) out.push_back({ last, (start + x1) * 0.5f });
		return withoutBackground(std::move(out));
	}

	constexpr std::uint8_t kBgR = 20, kBgG = 18, kBgB = 15;
}

TEST_CASE("audio mixer ui: the project's buses become strips, M mutes and S solos in the engine")
{
	Harness harness;

	const auto dir = std::filesystem::temp_directory_path() / "he_mixer_ui";
	he_test::removeAllQuiet(dir);
	ProjectManager pm;
	REQUIRE(pm.createNewProject(dir.string(), "Mixer", ProjectPreset::Empty));
	auto& cfg = pm.currentProject().audioBuses;
	REQUIRE(cfg.add("Music", 0.5f));
	REQUIRE(cfg.add("SFX"));
	REQUIRE(pm.saveProject(pm.currentProject().path));

	AudioEngine audio;
	REQUIRE(audio.init(true));

	ContextBits bits;
	AppContext ctx = bits.make(pm, audio);

	// Pointer away, a few frames to settle; one shot to look at.
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	he_ui::Image img;
	for (int i = 0; i < 4; ++i) frame(ctx, false, i == 3 ? &img : nullptr);
	REQUIRE(img.valid());
	CHECK(img.inkedPixels(kBgR, kBgG, kBgB) > 4000);
	if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
		he_ui::writeBmp(img, std::string(d) + "/audio-mixer.bmp");

	// Drawing the window is what brings the engine in line with the project.
	CHECK(audio.hasBus("Music"));
	CHECK(audio.hasBus("SFX"));
	CHECK(audio.getBusVolume("Music") == doctest::Approx(0.5f));

	// Down the first strip (Master): the fader, then its M. The strip starts
	// at the window's left padding; 40 px in is inside it whatever the font.
	const std::vector<Item> master = itemsDown(ctx, 50.0f, 40.0f, float(H) - 60.0f);
	REQUIRE_MESSAGE(master.size() >= 2, "found " << master.size() << " items down the Master strip");
	const float mRowY = master[1].mid;

	// Across the M row: Master-M, then per bus M and S (and nothing else on
	// that line). Master's S does not exist — there is nothing to solo it
	// against. Started clear of the window's edge: its resize border is an
	// item too, crossed once.
	const std::vector<Item> mRow = itemsAcross(ctx, mRowY, 24.0f, float(W) - 24.0f);
	REQUIRE_MESSAGE(mRow.size() >= 5, "found " << mRow.size() << " items across the M row");
	const Item masterM = mRow[0];
	const Item musicM  = mRow[1];
	const Item musicS  = mRow[2];
	const Item sfxM    = mRow[3];

	// M on Master: the engine is muted and STAYS muted across the frames that
	// follow — the window re-applies the project's bus list every frame, and
	// that must not undo a mute. The fader value survives, in the engine and
	// in the project alike.
	clickAt(ctx, masterM.mid, mRowY);
	CHECK(audio.isMasterMuted());
	for (int i = 0; i < 3; ++i) frame(ctx, false);
	CHECK(audio.isMasterMuted());
	CHECK(audio.getMasterVolume() == doctest::Approx(1.0f));
	CHECK(cfg.masterVolume == doctest::Approx(1.0f));
	clickAt(ctx, masterM.mid, mRowY);
	CHECK_FALSE(audio.isMasterMuted());

	// M on Music: the engine's Music bus is muted, the fader value is kept.
	clickAt(ctx, musicM.mid, mRowY);
	CHECK(audio.isBusMuted("Music"));
	CHECK_FALSE(audio.isBusMuted("SFX"));
	CHECK(audio.getBusVolume("Music") == doctest::Approx(0.5f));
	frame(ctx, false, &img);
	if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
		he_ui::writeBmp(img, std::string(d) + "/audio-mixer-muted.bmp");
	// …and again: unmuted.
	clickAt(ctx, musicM.mid, mRowY);
	CHECK_FALSE(audio.isBusMuted("Music"));

	// S on Music: everything that is not Music goes quiet, Music stays up.
	clickAt(ctx, musicS.mid, mRowY);
	CHECK_FALSE(audio.isBusMuted("Music"));
	CHECK(audio.isBusMuted("SFX"));
	// M on SFX while Music is soloed changes nothing audible; clearing the
	// solo then leaves SFX muted by its own M.
	clickAt(ctx, sfxM.mid, mRowY);
	CHECK(audio.isBusMuted("SFX"));
	clickAt(ctx, musicS.mid, mRowY);
	CHECK(audio.isBusMuted("SFX"));
	CHECK_FALSE(audio.isBusMuted("Music"));

	// None of that reached the project: mute and solo are the session's.
	{
		ProjectManager fresh;
		REQUIRE(fresh.loadProject(pm.currentProject().path));
		REQUIRE(fresh.currentProject().audioBuses.buses.size() == 2);
		CHECK(fresh.currentProject().audioBuses.buses[0].volume == doctest::Approx(0.5f));
	}

	audio.shutdown();
	he_test::removeAllQuiet(dir);
}
