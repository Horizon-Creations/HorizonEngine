#include "doctest.h"

#include "ImGuiSoftwareRaster.h"

#include "DocsLibrary.h"
#include "DocsPanel.h"
#include "EditorHelp.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "HcEditorUtil.h"
#include "HcNodeReference.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonScene/EngineApi.h>

#include <imgui.h>

#include <cstdlib>
#include <cstring>
#include <string>

// ── Editor UI, rendered without a GPU ────────────────────────────────────────
// The headless tests could already DRIVE ImGui — a context and a display size
// are enough — but not see what came out, so anything visual ended at "somebody
// will have to look at it". This closes that: ImGui's output is a triangle list,
// ImGuiSoftwareRaster turns it into pixels, and a scene here is a small ImGui
// tree drawn into an image that can be asserted on and, with HE_UI_DUMP_DIR set,
// written to a file and looked at.
//
//     HE_UI_DUMP_DIR=/tmp/ui ./he_tests -tc="ui shot*"
//     scripts/he_uishot.py /tmp/ui          # → PNGs
//
// The assertions here are deliberately coarse — "the panel drew something",
// "the tooltip added ink where there was none" — because the point of the
// harness is not to pin every pixel. A pixel-exact expectation would fail on
// the next font change and teach everyone to regenerate it unread. What it does
// catch is the class of bug that has no other witness at all: a panel that draws
// nothing, a tooltip that never appears, a theme that leaves text invisible
// against its own background.

using namespace HE::Ed;

namespace
{
	// Where EditorDeps sits in the source tree, for the editor's real font. The
	// scenes are worth little in the default proggy font: half of what they show
	// is spacing and line height, and those are the font's.
	const char* depsDir()
	{
#ifdef HE_EDITOR_DEPS_DIR
		return HE_EDITOR_DEPS_DIR;
#else
		return "";
#endif
	}

	// A headless ImGui context carrying the editor's own look, so a scene is a
	// picture of the editor rather than of default ImGui.
	struct Harness
	{
		Harness(int w, int h)
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(float(w), float(h));
			io.DeltaTime   = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			// The renderer here IS the one that owns textures — it honours the
			// atlas requests in ImDrawData::Textures.
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			// And it reads ImDrawCmd::VtxOffset, which a scene big enough to pass
			// 64k vertices depends on: without this flag ImGui keeps writing
			// 16-bit indices into one buffer and they wrap, which rasterises as
			// long diagonal smears of text — how this was found. The editor's own
			// backends all set it, so only the harness was ever wrong.
			io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

			const std::string font = std::string(depsDir()) + "/Fonts/Roboto_Condensed-Bold.ttf";
			ImFontConfig cfg;
			cfg.OversampleH = 2;
			cfg.OversampleV = 2;
			// AddFontFromFileTTF asserts on a missing file in debug builds, so ask
			// first: a checkout without EditorDeps still runs the test, in the
			// default font.
			if (std::FILE* f = std::fopen(font.c_str(), "rb"))
			{
				std::fclose(f);
				body       = io.Fonts->AddFontFromFileTTF(font.c_str(), 15.0f, &cfg);
				subheading = io.Fonts->AddFontFromFileTTF(font.c_str(), 18.0f, &cfg);
				heading    = io.Fonts->AddFontFromFileTTF(font.c_str(), 26.0f, &cfg);
				io.FontDefault = body;
			}
			applyHorizonDarkTheme();
		}
		~Harness() { ImGui::DestroyContext(); }

		ImFont* body       = nullptr;
		ImFont* subheading = nullptr;
		ImFont* heading    = nullptr;
	};

	// Run `body` for `frames` frames and rasterise the last one. More than one
	// frame because almost nothing in ImGui is right on the first: a window has
	// no size until it has been laid out once, and anything that depends on a
	// hover delay needs the clock to run.
	template <typename F>
	he_ui::Image shoot(const char* name, int w, int h, int frames, F&& body)
	{
		he_ui::Image img;
		for (int i = 0; i < frames; ++i)
		{
			ImGui::NewFrame();
			body(i);
			ImGui::Render();
			if (i == frames - 1)
				img = he_ui::rasterize(ImGui::GetDrawData(), w, h);
		}

		// Dumping is opt-in: a test suite that writes files on every run is a
		// test suite people stop running.
		if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
			he_ui::writeBmp(img, std::string(dir) + "/" + name + ".bmp");
		return img;
	}

	// The editor's window background, which is what "no ink" means in a shot.
	constexpr std::uint8_t kBgR = 20, kBgG = 18, kBgB = 15;

	// What the documentation reader needs, filled from the harness. The renderer
	// stays null — there is no GPU here, and the panel has to cope with that
	// anyway: it draws on the Project Hub, before one exists.
	DocsPanel::Host hostOf(const Harness& h)
	{
		DocsPanel::Host host;
		host.fontBody       = h.body;
		host.fontSubheading = h.subheading;
		host.fontHeading    = h.heading;
		host.fontCode       = h.body;   // no separate mono face in the harness
		return host;
	}
} // namespace

// ── Documentation figures ────────────────────────────────────────────────────
// Scenes whose name starts with "doc-" are not just for looking at: they are the
// manual's own illustrations. scripts/he_uishot.py --docs converts them into
// EditorDeps/Docs/img, where the reader loads its figures from, and the bundle
// generator places them on the sections named in its FIGURES table.
//
// They are drawn rather than screenshotted from a running editor because a
// picture of where something IS should not also be a picture of somebody's
// project, their panel widths and whatever scene they had open. A labelled map
// of the layout answers "where do I find this" and stays true when the editor's
// contents change.
namespace
{
	// The editor's dock layout as a map, with one panel picked out. `highlight`
	// names the panel to pick out ("" = label them all evenly, which is the
	// figure for the layout section itself).
	void drawLayoutMap(const Harness& h, const char* highlight)
	{
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->Pos);
		ImGui::SetNextWindowSize(vp->Size);
		ImGui::Begin("##layoutmap", nullptr,
		             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
		             ImGuiWindowFlags_NoSavedSettings);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 o = vp->Pos, s = vp->Size;

		struct Box { const char* name; float x, y, w, h; };
		// Proportions of the editor's default layout: menu and toolbar strips
		// across the top, the outliner left, details right, content browser
		// along the bottom, the viewport in the middle, the footer under it.
		const float menuH = 0.055f, toolH = 0.075f, footH = 0.055f;
		const float leftW = 0.20f,  rightW = 0.24f, contentH = 0.26f;
		const float midY  = menuH + toolH;
		const float midH  = 1.0f - midY - footH;
		const Box boxes[] = {
			{ "Menu Bar",        0.0f,            0.0f,  1.0f,                    menuH },
			{ "Toolbar",         0.0f,            menuH, 1.0f,                    toolH },
			{ "World Outliner",  0.0f,            midY,  leftW,                   midH - contentH },
			{ "Quick Settings",  0.0f,            midY + midH - contentH, leftW,  contentH },
			{ "Scene",           leftW,           midY,  1.0f - leftW - rightW,   midH - contentH },
			{ "Content Browser", leftW,           midY + midH - contentH,
			                     1.0f - leftW - rightW, contentH },
			{ "Details",         1.0f - rightW,   midY,  rightW,                  midH },
			{ "Footer",          0.0f,            1.0f - footH, 1.0f,             footH },
		};

		for (const Box& b : boxes)
		{
			const ImVec2 p0(o.x + b.x * s.x + 3.0f, o.y + b.y * s.y + 3.0f);
			const ImVec2 p1(o.x + (b.x + b.w) * s.x - 3.0f, o.y + (b.y + b.h) * s.y - 3.0f);
			const bool on = highlight && *highlight && std::strcmp(highlight, b.name) == 0;
			const bool dim = highlight && *highlight && !on;

			dl->AddRectFilled(p0, p1, ImGui::GetColorU32(on ? Theme::warm(0.18f)
			                                                : Theme::warm(0.105f)), 5.0f);
			dl->AddRect(p0, p1, ImGui::GetColorU32(on ? Theme::Accent : Theme::warm(0.22f)),
			            5.0f, 0, on ? 2.5f : 1.0f);

			if (h.subheading && on) ImGui::PushFont(h.subheading, 0.0f);
			const ImVec2 t = ImGui::CalcTextSize(b.name);
			dl->AddText(ImVec2((p0.x + p1.x - t.x) * 0.5f, (p0.y + p1.y - t.y) * 0.5f),
			            ImGui::GetColorU32(on ? Theme::TextHeading
			                                  : dim ? Theme::TextDim : Theme::Text),
			            b.name);
			if (h.subheading && on) ImGui::PopFont();
		}
		ImGui::End();
	}
} // namespace

TEST_CASE("ui shot: the documentation's own figures")
{
	constexpr int W = 900, H = 560;

	struct Fig { const char* name; const char* highlight; };
	const Fig figures[] = {
		{ "doc-layout",          ""                },
		{ "doc-layout-outliner", "World Outliner"  },
		{ "doc-layout-details",  "Details"         },
		{ "doc-layout-content",  "Content Browser" },
		{ "doc-layout-scene",    "Scene"           },
	};
	for (const Fig& f : figures)
	{
		Harness harness(W, H);
		const he_ui::Image img =
			shoot(f.name, W, H, 3, [&](int) { drawLayoutMap(harness, f.highlight); });
		REQUIRE(img.valid());
		CHECK(img.inkedPixels(kBgR, kBgG, kBgB) > 20000);
	}

	// The node tooltip, as the manual's illustration of what hovering gives you.
	{
		constexpr int TW = 620, TH = 300;
		Harness harness(TW, TH);
		HorizonCode::Node node;
		node.type   = HorizonCode::NodeType::EngineCall;
		node.s      = "physics.addImpulse";
		node.hasArg = true;
		if (const HE::api::ApiFn* fn = HE::api::find(node.s))
		{
			for (const auto& p : fn->params)  node.params.push_back({ p.name, p.type, p.isArray });
			for (const auto& r : fn->results) node.results.push_back({ r.name, r.type, r.isArray });
		}
		const he_ui::Image img = shoot("doc-node-tooltip", TW, TH, 3, [&](int) {
			ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f));
			ImGui::SetNextWindowSize(ImVec2(TW - 24.0f, TH - 24.0f));
			ImGui::Begin("##nodedoc", nullptr,
			             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
			{
				EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 32.0f);
				HcEditorUtil::drawNodeDoc(node);
			}
			ImGui::End();
		});
		REQUIRE(img.valid());
		CHECK(img.inkedPixels(kBgR, kBgG, kBgB) > 6000);
	}
}

TEST_CASE("ui shot: a Details-style panel draws its rows")
{
	constexpr int W = 420, H = 340;
	Harness harness(W, H);

	float metallic = 0.2f, roughness = 0.6f;
	float color[3] = { 0.8f, 0.5f, 0.2f };
	bool  shadows = true;

	auto scene = [&](int) {
		ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
		ImGui::SetNextWindowSize(ImVec2(W - 20.0f, H - 20.0f));
		ImGui::Begin("Details", nullptr,
		             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
		{
			Help::Scope scope("Material");
			ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen);
			EditorWidgets::Row::sliderFloat("Metallic", &metallic, 0.0f, 1.0f);
			EditorWidgets::Row::sliderFloat("Roughness", &roughness, 0.0f, 1.0f);
			EditorWidgets::Row::colorEdit3("Base Color", color);
			EditorWidgets::checkbox("Casts Shadow", &shadows);
			EditorWidgets::hint("Whether this mesh appears in the shadow maps.");
		}
		ImGui::End();
		EditorWidgets::drawQueuedHelp();
	};

	const he_ui::Image img = shoot("details-panel", W, H, 4, scene);
	REQUIRE(img.valid());

	// A panel that draws nothing still "passes" every layout assertion in the
	// suite — this is the check that does not.
	const int ink = img.inkedPixels(kBgR, kBgG, kBgB);
	INFO("inked pixels: " << ink);
	CHECK(ink > 4000);

	// Inside the panel, and nowhere else: the margin around it must stay the
	// clear colour, which catches a window that escaped its own rectangle.
	std::uint8_t r, g, b, a;
	img.pixel(2, 2, r, g, b, a);
	CHECK(int(r) == int(kBgR));
	CHECK(int(g) == int(kBgG));
	CHECK(int(b) == int(kBgB));
}

TEST_CASE("ui shot: hovering a row raises its help tooltip")
{
	constexpr int W = 520, H = 320;
	Harness harness(W, H);
	ImGuiIO& io = ImGui::GetIO();

	float mass = 70.0f;
	ImVec2 rowCenter{ -1.0f, -1.0f };

	auto scene = [&](int) {
		ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
		ImGui::SetNextWindowSize(ImVec2(240.0f, 160.0f));
		ImGui::Begin("Details", nullptr,
		             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		             ImGuiWindowFlags_NoSavedSettings);
		{
			Help::Scope scope("Rigid Body");
			EditorWidgets::Row::dragFloat("Mass", &mass, 0.5f);
			const ImVec2 mn = ImGui::GetItemRectMin();
			const ImVec2 mx = ImGui::GetItemRectMax();
			rowCenter = ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
		}
		ImGui::End();
		EditorWidgets::drawQueuedHelp();
	};

	// Pointer away: the baseline.
	io.AddMousePosEvent(float(W) - 4.0f, float(H) - 4.0f);
	const he_ui::Image plain = shoot("row-plain", W, H, 4, scene);
	const int plainInk = plain.inkedPixels(kBgR, kBgG, kBgB);

	// Pointer on the row, held there long enough for ImGui to consider a tooltip
	// owed (it wants the mouse stationary first — see EditorWidgets.h).
	REQUIRE(rowCenter.x > 0.0f);
	io.AddMousePosEvent(rowCenter.x, rowCenter.y);
	const he_ui::Image hovered = shoot("row-tooltip", W, H, 40, scene);
	const int hoveredInk = hovered.inkedPixels(kBgR, kBgG, kBgB);

	// The tooltip is a whole extra window of text: it cannot fail to add ink.
	INFO("plain=" << plainInk << " hovered=" << hoveredInk);
	CHECK(hoveredInk > plainInk + 1500);

	// And it is drawn OUTSIDE the little panel — below and right of the cursor,
	// in the area the panel does not cover. That is the part a "did anything
	// change" count alone would not distinguish from a hover highlight.
	int inkRight = 0;
	for (int y = 0; y < H; ++y)
		for (int x = 270; x < W; ++x)
		{
			std::uint8_t r, g, b, a;
			hovered.pixel(x, y, r, g, b, a);
			if (std::abs(int(r) - int(kBgR)) > 6 || std::abs(int(g) - int(kBgG)) > 6 ||
			    std::abs(int(b) - int(kBgB)) > 6)
				++inkRight;
		}
	INFO("ink right of the panel: " << inkRight);
	CHECK(inkRight > 800);
}

TEST_CASE("ui shot: the theme keeps text legible against its own background")
{
	// The one property a screenshot can check that no layout assertion can: that
	// the editor's own palette does not paint text onto a background of the same
	// value. A theme change that made body text invisible would pass every other
	// test in this suite.
	constexpr int W = 300, H = 120;
	Harness harness(W, H);

	auto scene = [&](int) {
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W), float(H)));
		ImGui::Begin("legibility", nullptr,
		             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
		ImGui::TextUnformatted("Horizon Engine");
		ImGui::TextDisabled("dimmed second line");
		ImGui::End();
	};

	const he_ui::Image img = shoot("legibility", W, H, 4, scene);
	REQUIRE(img.valid());

	// Walk the two text lines and find the brightest pixel; against the window
	// background it has to be a real contrast, not a shade of it.
	int brightest = 0, darkest = 255;
	for (int y = 0; y < 60; ++y)
		for (int x = 0; x < W; ++x)
		{
			std::uint8_t r, g, b, a;
			img.pixel(x, y, r, g, b, a);
			const int lum = (int(r) * 30 + int(g) * 59 + int(b) * 11) / 100;
			brightest = std::max(brightest, lum);
			darkest   = std::min(darkest, lum);
		}
	INFO("luminance range in the text area: " << darkest << ".." << brightest);
	CHECK(brightest - darkest > 90);
}

// ── The documentation reader, drawn for real ─────────────────────────────────
// Not a stand-in: this is DocsPanel::draw over the bundle that ships, in a
// headless context. It is the only way to see the reader at all — the editor's
// chrome cannot be screenshotted through the engine's own frame dump, which
// renders the SCENE — and it is the check that the page actually lays out:
// paragraphs, a reference table, a code listing and a callout each go through a
// different path in the block renderer, and every one of them can come out empty
// without anything else noticing.
TEST_CASE("ui shot: the documentation reader lays a page out")
{
	constexpr int W = 1000, H = 640;
	Harness harness(W, H);
	const DocsPanel::Host host = hostOf(harness);

	// Point the library at the bundle in the source tree. The panel would look
	// beside the running executable, and a test binary does not live where the
	// editor does.
	HE::Ed::Docs::Library& lib = HE::Ed::Docs::library();
#ifdef HE_DOCS_BUNDLE_PATH
	REQUIRE(lib.load(HE_DOCS_BUNDLE_PATH));
#endif
	REQUIRE(lib.loaded());

	// The Editor Manual's layout section: prose, then the panel table, then more
	// prose. If any of the three is missing from the shot, the block renderer
	// dropped it.
	DocsPanel::openTopic("editor#layout");
	REQUIRE(DocsPanel::isOpen());

	const he_ui::Image img = shoot("docs-reader", W, H, 4,
	                               [&](int) { DocsPanel::draw(host); });
	REQUIRE(img.valid());

	const int ink = img.inkedPixels(kBgR, kBgG, kBgB);
	INFO("inked pixels: " << ink);
	CHECK(ink > 100000);

	// The body column, right of the sidebar, must carry text — that is where the
	// page goes, and a reader that renders its navigation and nothing else is
	// exactly the failure this catches.
	int bodyInk = 0;
	for (int y = 90; y < H - 40; ++y)
		for (int x = 300; x < W - 20; ++x)
		{
			std::uint8_t r, g, b, a;
			img.pixel(x, y, r, g, b, a);
			const int lum = (int(r) * 30 + int(g) * 59 + int(b) * 11) / 100;
			if (lum > 90) ++bodyInk;   // text, not panel background
		}
	INFO("bright pixels in the body column: " << bodyInk);
	CHECK(bodyInk > 3000);

	DocsPanel::close();
}

TEST_CASE("ui shot: a node explains itself on hover")
{
	// What a graph author sees when the cursor rests on a node: the call's name,
	// where it comes from, what it does, and its pins — in the same colours and
	// glyphs the canvas draws those pins with. Rendered here because it is the
	// one place the pin vocabulary can actually be checked: "is the exec pin a
	// white triangle" is not a question a string comparison can answer.
	constexpr int W = 560, H = 320;
	Harness harness(W, H);

	HorizonCode::Node node;
	node.type   = HorizonCode::NodeType::EngineCall;
	node.s      = "physics.addImpulse";
	node.hasArg = true;   // an exec call
	if (const HE::api::ApiFn* fn = HE::api::find(node.s))
	{
		for (const auto& p : fn->params)  node.params.push_back({ p.name, p.type, p.isArray });
		for (const auto& r : fn->results) node.results.push_back({ r.name, r.type, r.isArray });
	}

	auto scene = [&](int) {
		ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
		ImGui::SetNextWindowSize(ImVec2(W - 20.0f, H - 20.0f));
		ImGui::Begin("node", nullptr,
		             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
		{
			EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 32.0f);
			HcEditorUtil::drawNodeDoc(node);
		}
		ImGui::End();
	};

	const he_ui::Image img = shoot("node-doc", W, H, 4, scene);
	REQUIRE(img.valid());
	CHECK(img.inkedPixels(kBgR, kBgG, kBgB) > 8000);

	// The pin glyphs are the point: somewhere in the lower half there has to be
	// ink in a pin colour that is neither the background nor the text — the
	// teal of an Int, the mint of a Vec3. A tooltip that lost its dots would
	// still be full of text and pass every other check here.
	int coloured = 0;
	for (int y = H / 2; y < H - 10; ++y)
		for (int x = 10; x < 80; ++x)
		{
			std::uint8_t r, g, b, a;
			img.pixel(x, y, r, g, b, a);
			const int mx = std::max({ int(r), int(g), int(b) });
			const int mn = std::min({ int(r), int(g), int(b) });
			if (mx - mn > 40 && mx > 90) ++coloured;   // saturated = a pin dot
		}
	INFO("saturated pixels in the pin column: " << coloured);
	CHECK(coloured > 20);
}

TEST_CASE("ui shot: the generated node reference")
{
	// The page that replaced the website's hand-written node reference: one
	// section per callable thing, with the pins the node really has. Shot
	// because the two things worth checking about it are visual — that the
	// three hundred sections do not read as a wall, and that the pin colours
	// made it onto the page.
	constexpr int W = 1000, H = 640;
	Harness harness(W, H);
	const DocsPanel::Host host = hostOf(harness);

	HE::Ed::Docs::Library& lib = HE::Ed::Docs::library();
#ifdef HE_DOCS_BUNDLE_PATH
	REQUIRE(lib.load(HE_DOCS_BUNDLE_PATH));
#endif
	HE::Ed::NodeReference::install(lib);

	DocsPanel::openTopic("horizoncode-nodes#physics.addImpulse");
	const he_ui::Image img = shoot("node-reference", W, H, 4,
	                               [&](int) { DocsPanel::draw(host); });
	REQUIRE(img.valid());
	CHECK(img.inkedPixels(kBgR, kBgG, kBgB) > 80000);

	// A built-in as well: those have exec pins on both sides and headers in a
	// different colour family, so they are where a preview drawn from one
	// example would come apart.
	DocsPanel::openTopic("horizoncode-nodes#node.Branch");
	const he_ui::Image builtin = shoot("node-reference-builtin", W, H, 4,
	                                   [&](int) { DocsPanel::draw(host); });
	REQUIRE(builtin.valid());
	CHECK(builtin.inkedPixels(kBgR, kBgG, kBgB) > 60000);
	DocsPanel::close();
}

TEST_CASE("docs reader: an F1 that was already answered is not toggled back shut")
{
	// The interaction this whole feature exists for: F1 over a node opens the
	// manual at that node's entry. Two handlers meet in one frame — the graph
	// canvas consumes F1 inline while drawing, and the editor has a global F1
	// that toggles the reader at the end of the frame. Without the guard, the
	// second undoes the first and the key appears to do nothing.
	Harness harness(400, 300);
	HE::Ed::Docs::Library& lib = HE::Ed::Docs::library();
#ifdef HE_DOCS_BUNDLE_PATH
	REQUIRE(lib.load(HE_DOCS_BUNDLE_PATH));
#endif
	HE::Ed::NodeReference::install(lib);
	DocsPanel::close();

	ImGui::NewFrame();
	CHECK_FALSE(DocsPanel::openedThisFrame());
	DocsPanel::openTopic("horizoncode-nodes#physics.addImpulse");
	CHECK(DocsPanel::isOpen());
	// Within the same frame the editor's F1 block must stand down.
	CHECK(DocsPanel::openedThisFrame());
	ImGui::Render();
	ImGui::EndFrame();

	// And on the NEXT frame the toggle is free again, or F1 could never close
	// the reader at all.
	ImGui::NewFrame();
	CHECK_FALSE(DocsPanel::openedThisFrame());
	ImGui::Render();
	ImGui::EndFrame();
	DocsPanel::close();
}

TEST_CASE("ui shot: a page of tables and a diagram")
{
	// The rendering page is the reader's hardest case: a wide reference table
	// (Backends), a second one under it, and the frame pipeline as a diagram.
	// All three are the kinds of block that look fine in prose and wrong in a
	// panel, which is why they get a shot of their own.
	constexpr int W = 1000, H = 640;
	Harness harness(W, H);
	const DocsPanel::Host host = hostOf(harness);

	HE::Ed::Docs::Library& lib = HE::Ed::Docs::library();
#ifdef HE_DOCS_BUNDLE_PATH
	REQUIRE(lib.load(HE_DOCS_BUNDLE_PATH));
#endif
	HE::Ed::NodeReference::install(lib);

	DocsPanel::openTopic("rendering#pipeline");
	const he_ui::Image flow = shoot("docs-flow", W, H, 4,
	                                [&](int) { DocsPanel::draw(host); });
	REQUIRE(flow.valid());
	CHECK(flow.inkedPixels(kBgR, kBgG, kBgB) > 60000);

	DocsPanel::openTopic("rendering#backends");
	const he_ui::Image table = shoot("docs-tables", W, H, 4,
	                                 [&](int) { DocsPanel::draw(host); });
	REQUIRE(table.valid());
	CHECK(table.inkedPixels(kBgR, kBgG, kBgB) > 60000);
	DocsPanel::close();
}

TEST_CASE("ui shot: searching the manual from the reader")
{
	constexpr int W = 1000, H = 640;
	Harness harness(W, H);
	const DocsPanel::Host host = hostOf(harness);

	HE::Ed::Docs::Library& lib = HE::Ed::Docs::library();
#ifdef HE_DOCS_BUNDLE_PATH
	REQUIRE(lib.load(HE_DOCS_BUNDLE_PATH));
#endif
	REQUIRE(lib.loaded());

	DocsPanel::openSearch("shadow");
	const he_ui::Image img = shoot("docs-search", W, H, 4,
	                               [&](int) { DocsPanel::draw(host); });
	REQUIRE(img.valid());
	CHECK(img.inkedPixels(kBgR, kBgG, kBgB) > 60000);
	DocsPanel::close();
}
