#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "HcWatchPanel.h"
#include "HcExecTrace.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>

#include <imgui.h>
#include <imgui_internal.h>   // GetHoveredID: which item the pointer is on

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ── The Watch window, driven headless ────────────────────────────────────────
// The rows themselves are HcWatch's (test_hc_watch.cpp); what is checked here
// is the window on top of them: that the real panel, on a real runtime with a
// run stopped at a breakpoint, puts the sections and their rows on screen as
// items the pointer can land on, that Go to Node asks for the reveal, and that
// the two idle states say so instead of showing an empty list. With
// HE_UI_DUMP_DIR set the frames are written out as well.

using namespace HE::Ed;
using namespace HorizonCode;

namespace
{
	constexpr int W = 400, H = 440;

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
			HcExecTrace::detach();
			HcExecTrace::clearAllBreakpoints();
			HcExecTrace::clearPaused();
			HcExecTrace::cancelReveal();
		}
		~Harness()
		{
			HcExecTrace::detach();
			HcExecTrace::clearAllBreakpoints();
			HcExecTrace::clearPaused();
			HcExecTrace::cancelReveal();
			ImGui::DestroyContext();
		}
	};

	// Everything AppContext insists on being given. The panel reads
	// projectLoaded, isPlaying and appLivePreview; the rest is here because
	// the struct has no defaults for its references.
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
		HcWatchPanel::DrawWatchWindow(ctx, open);
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

	// Every distinct item the pointer crosses walking DOWN at x. The rows sit
	// in a child window whose own id is reported between them; anything
	// crossed more than once is that background and dropped.
	std::vector<Item> itemsDown(AppContext& ctx, float x, float y0, float y1)
	{
		std::vector<Item> raw;
		ImGuiID last = 0; float start = y0;
		for (float y = y0; y < y1; y += 2.0f)
		{
			const ImGuiID id = idAt(ctx, x, y);
			if (id == last) continue;
			if (last != 0) raw.push_back({ last, (start + y - 2.0f) * 0.5f });
			last = id; start = y;
		}
		if (last != 0) raw.push_back({ last, (start + y1) * 0.5f });
		std::vector<Item> out;
		for (const Item& it : raw)
		{
			int n = 0;
			for (const Item& o : raw) n += o.id == it.id;
			if (n == 1) out.push_back(it);
		}
		return out;
	}

	void shotTo(const he_ui::Image& img, const char* name)
	{
		if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
			he_ui::writeBmp(img, std::string(d) + "/" + name + ".bmp");
	}

	// Print: execIn 0 / execOut 1 / Text dataIn 2; ConstString dataOut 0.
	int printNode(Graph& g, const std::string& text)
	{
		Node cs; cs.type = NodeType::ConstString; cs.s = text;
		const int c = g.addNode(cs);
		Node pr; pr.type = NodeType::Print;
		const int p = g.addNode(pr);
		REQUIRE(g.connect(c, 0, p, 2));
		return p;
	}
	void chain(Graph& g, int from, int pin, int to) { REQUIRE(g.connect(from, pin, to, 0)); }

	// Go(Int) → Call F(21) → Print "after".  F: Entry x → Set l = x → Print "inner" → Return.
	// Three instance variables, one local. Stopped at "inner", the window has
	// three sections: the argument (1 row), the frame (x, l), the variables (3).
	struct Fixture
	{
		Graph g;
		int inner = 0;
		Fixture()
		{
			{ Variable v; v.name = "score"; v.type = PinType::Int; v.f[0] = 42; g.variables.push_back(v); }
			{ Variable v; v.name = "name";  v.type = PinType::String; v.s = "hero"; g.variables.push_back(v); }
			{ Variable v; v.name = "aim";   v.type = PinType::Vec3; v.f[0] = 1; v.f[1] = 2.5f; v.f[2] = -3;
			  g.variables.push_back(v); }
			Node fe; fe.type = NodeType::FunctionEntry; fe.s = "F";
			fe.params = { { "x", PinType::Int } }; fe.results = { { "y", PinType::Int } };
			const int feId = g.addNode(fe);
			{ Variable l; l.name = "l"; l.type = PinType::Int; l.f[0] = 5; l.scope = feId; g.variables.push_back(l); }
			Node sl; sl.type = NodeType::SetVariable; sl.s = "l"; sl.propType = PinType::Int; const int setL = g.addNode(sl);
			inner = printNode(g, "inner");
			Node fr; fr.type = NodeType::FunctionReturn; fr.s = "F"; const int frId = g.addNode(fr);
			Node ev; ev.type = NodeType::Event; ev.s = "Go"; ev.hasArg = true; ev.propType = PinType::Int;
			const int e = g.addNode(ev);
			Node ci; ci.type = NodeType::ConstInt; ci.f[0] = 21; const int c = g.addNode(ci);
			Node fc; fc.type = NodeType::FunctionCall; fc.s = "F"; const int fcId = g.addNode(fc);
			const int after = printNode(g, "after");
			syncFunctionSignatures(g);
			chain(g, feId, 0, setL); chain(g, setL, 1, inner); chain(g, inner, 1, frId);
			REQUIRE(g.connect(feId, 1, setL, 2));
			REQUIRE(g.connect(feId, 1, frId, 1));
			chain(g, e, 0, fcId);
			REQUIRE(g.connect(c, 0, fcId, 2));
			chain(g, fcId, 1, after);
		}
	};

	constexpr std::uint8_t kBgR = 20, kBgG = 18, kBgB = 15;
}

TEST_CASE("watch ui: a stopped run shows its sections as rows, and Go to Node asks for the reveal")
{
	Harness harness;
	Fixture f;
	Runtime rt;
	HcExecTrace::attach(rt);
	ClassIdentity cls; cls.key = "Content/Enemies/Goblin.hasset";
	const InstanceId id = rt.add(f.g, {}, cls);
	HcExecTrace::setBreakpoint("Content/Enemies/Goblin.hasset", f.inner, true);
	rt.fireEvent(id, "Go", 0, Value::ofInt(7));
	REQUIRE(rt.isSuspended());
	HcExecTrace::cancelReveal();   // the stop's own reveal; the button's is what is checked
	HcExecTrace::takeBreakHit();

	ContextBits bits;
	AppContext ctx = bits.make();
	ctx.isPlaying = true;

	// Pointer away, a few frames to settle; one shot to look at.
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	he_ui::Image img;
	for (int i = 0; i < 4; ++i) frame(ctx, false, i == 3 ? &img : nullptr);
	REQUIRE(img.valid());
	CHECK(img.inkedPixels(kBgR, kBgG, kBgB) > 3000);
	shotTo(img, "watch-stopped");

	// Down the list, below the header lines and the filter: three section
	// headers and their rows — 1 + 2 + 3 — every one an item of its own.
	std::vector<Item> items = itemsDown(ctx, 60.0f, 96.0f, float(H) - 30.0f);
	CHECK_MESSAGE(items.size() >= 9, "found " << items.size() << " items; expected 3 headers + 6 rows");

	// Go to Node sits at the right of the second header line.
	std::vector<Item> top = itemsDown(ctx, float(W) - 40.0f, 30.0f, 96.0f);
	REQUIRE_MESSAGE(!top.empty(), "no Go to Node button found in the header");
	clickAt(ctx, float(W) - 40.0f, top[0].mid);
	std::string tab;
	REQUIRE(HcExecTrace::takeRevealTab(tab));
	CHECK(tab == "Content/Enemies/Goblin.hasset");
	int node = 0;
	REQUIRE(HcExecTrace::takeRevealNode("Content/Enemies/Goblin.hasset", node));
	CHECK(node == f.inner);

	// Continue: the run ends; the window falls back to the live view.
	rt.debugContinue();
	CHECK_FALSE(rt.isSuspended());
	HcExecTrace::refreshPaused();
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) frame(ctx, false, i == 2 ? &img : nullptr);
	shotTo(img, "watch-live");
	// Below the hint and the filter box: no Game Instance, so no rows.
	CHECK(itemsDown(ctx, 60.0f, 135.0f, float(H) - 30.0f).empty());
}

TEST_CASE("watch ui: nothing running says so instead of listing")
{
	Harness harness;
	Runtime rt;
	HcExecTrace::attach(rt);
	ContextBits bits;
	AppContext ctx = bits.make();
	ctx.isPlaying = false;

	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	he_ui::Image img;
	for (int i = 0; i < 3; ++i) frame(ctx, false, i == 2 ? &img : nullptr);
	REQUIRE(img.valid());
	CHECK(img.inkedPixels(kBgR, kBgG, kBgB) > 500);   // the sentence, not an empty window
	shotTo(img, "watch-idle");
	CHECK(itemsDown(ctx, 60.0f, 40.0f, float(H) - 30.0f).empty());
}
