#include "doctest.h"

#include "ImGuiSoftwareRaster.h"

#include "DocsLibrary.h"
#include "DocsPanel.h"
#include "EditorHelp.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "HcEditorUtil.h"
#include "HcNodeReference.h"
#include "GraphEditor.h"   // the canvas under test: node draw order vs on-node widgets
#include "EditorReference.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonScene/EngineApi.h>

#include <imgui.h>

#include <cmath>
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

	// A generated sky row: the biggest family on the page, and the one a reader
	// is most likely to land on first. One input or one output and nothing else,
	// which is the narrowest a node preview ever gets.
	DocsPanel::openTopic("horizoncode-nodes#env.getCycleSeconds");
	const he_ui::Image env = shoot("node-reference-env", W, H, 4,
	                               [&](int) { DocsPanel::draw(host); });
	REQUIRE(env.valid());
	CHECK(env.inkedPixels(kBgR, kBgG, kBgB) > 60000);

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

TEST_CASE("docs reader: the sidebar's groups belong to the user once it has landed")
{
	// The node reference's sidebar folds its three hundred sections into their
	// categories, and the group holding the section just opened unfolds itself.
	// Doing that EVERY frame is the bug: the user's click flips the group and
	// the next frame sets it straight back, so no group can be opened by hand.
	//
	// This is the window in which the sidebar may still override — it has to
	// close, and it has to be open long enough to survive the frames a
	// navigation takes to settle.
	Harness harness(400, 300);
	const DocsPanel::Host host = hostOf(harness);
	HE::Ed::Docs::Library& lib = HE::Ed::Docs::library();
#ifdef HE_DOCS_BUNDLE_PATH
	REQUIRE(lib.load(HE_DOCS_BUNDLE_PATH));
#endif
	HE::Ed::NodeReference::install(lib);

	DocsPanel::openTopic("horizoncode-nodes#physics.addImpulse");
	CHECK(DocsPanel::navigatingGroups());

	int frames = 0;
	for (; frames < 20 && DocsPanel::navigatingGroups(); ++frames)
	{
		ImGui::NewFrame();
		DocsPanel::draw(host);
		ImGui::Render();
		ImGui::EndFrame();
	}
	CHECK_FALSE(DocsPanel::navigatingGroups());
	// Long enough to survive a navigation settling, short enough that the very
	// next thing the user does is theirs.
	INFO("frames the sidebar kept control: " << frames);
	CHECK(frames >= 2);
	CHECK(frames <= 6);
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

TEST_CASE("ui shot: the generated editor reference")
{
	// The manual's new middle: one entry per control, generated from the same
	// table the hover tooltips come from, so F1 on a slider opens the slider.
	constexpr int W = 1000, H = 640;
	Harness harness(W, H);
	const DocsPanel::Host host = hostOf(harness);

	HE::Ed::Docs::Library& lib = HE::Ed::Docs::library();
#ifdef HE_DOCS_BUNDLE_PATH
	REQUIRE(lib.load(HE_DOCS_BUNDLE_PATH));
#endif
	HE::Ed::NodeReference::install(lib);
	HE::Ed::EditorReference::install(lib);

	// Exactly where F1 on the Bloom Threshold slider now lands — the entry for
	// that slider, not a chapter about post-processing. The settings page is the
	// one worth shooting: its groups are the Preferences window's own categories,
	// read out of the catalog's row() calls, so a category renamed there and not
	// here shows up in this picture.
	//
	// Asserted rather than assumed: openTopic on an anchor that does not exist
	// leaves the reader wherever it was, which in a shot looks like a page that
	// simply scrolled somewhere else. That is how a broken anchor hides.
	{
		int pg = -1, sec = -1;
		REQUIRE(lib.resolve("editor-settings#Preferences.Post-Processing.Bloom Threshold", pg, sec));
		REQUIRE(sec >= 0);
	}
	DocsPanel::openTopic("editor-settings#Preferences.Post-Processing.Bloom Threshold");
	const he_ui::Image img = shoot("editor-reference", W, H, 4,
	                               [&](int) { DocsPanel::draw(host); });
	REQUIRE(img.valid());
	CHECK(img.inkedPixels(kBgR, kBgG, kBgB) > 60000);
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

// ── On-node controls must not float above the node in front of them ─────────
// Reported as "Steuerelemente auf HC-Nodes rendern immer ganz oben, auch über
// anderen Node-Bodies", and it was structural rather than a z-order slip: the
// inline editors and node bodies were drawn inside ImGui::BeginChild, and
// AddWindowToDrawData (imgui.cpp) appends a window's OWN draw list first and
// every child window after it. Every widget in a child therefore sat above
// every node background on the canvas, whatever the node order was.
//
// This is the measurement, not an argument: two overlapping nodes, the BACK one
// carrying a body widget in a colour nothing else on the canvas uses, and a
// pixel read inside the overlap. Before the fix that pixel was the widget's
// colour. It has to be the front node's body.
TEST_CASE("Graph canvas: a node in front covers the widgets of the node behind it")
{
	constexpr int W = 320, H = 240;
	Harness harness(W, H);

	// A colour no node background, header, border or grid line uses, so a single
	// pixel read is unambiguous about which of the two it came from.
	constexpr ImU32 kLoud = IM_COL32(255, 0, 255, 255);

	GraphEditor::Model m;
	GraphEditor::State st;
	st.pan  = ImVec2(0.0f, 0.0f);
	st.zoom = 1.0f;

	// Node 1 sits back-left and owns the loud body; node 2 is drawn after it and
	// therefore in front, overlapping node 1's body area.
	m.nodeIds = []{ return std::vector<int>{ 1, 2 }; };
	m.getPos  = [](int id, float& x, float& y)
	{ x = (id == 1) ? 40.0f : 90.0f; y = (id == 1) ? 40.0f : 60.0f; };
	m.setPos  = [](int, float, float){};
	m.title   = [](int id){ return id == 1 ? std::string("Back") : std::string("Front"); };
	m.headerColor = [](int){ return IM_COL32(60, 60, 70, 255); };
	m.pins    = [](int){ return std::vector<GraphEditor::Pin>{}; };
	m.links   = []{ return std::vector<std::array<int, 4>>{}; };
	m.connect = [](int, int, int, int){ return false; };
	// BOTH nodes reserve a body, so the front one's box is tall enough to
	// actually lie over the back one's. Only the back one PAINTS into its body —
	// the front one stays its plain background, which is what the sample below
	// expects to find there.
	m.nodeBodyHeight = [](int){ return 60.0f; };
	m.drawNodeBody   = [&](int id, ImVec2 bmin, ImVec2 bmax, float)
	{ if (id == 1) ImGui::GetWindowDrawList()->AddRectFilled(bmin, bmax, kLoud); };

	const he_ui::Image img = shoot("graph-node-overlap", W, H, 3, [&](int)
	{
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2((float)W, (float)H));
		ImGui::Begin("##canvas", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
		GraphEditor::draw("##ge", m, st, ImVec2((float)W, (float)H));
		ImGui::End();
	});
	REQUIRE(img.valid());

	auto loudPixels = [&](int x0, int y0, int x1, int y1)
	{
		int n = 0;
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x)
			{
				std::uint8_t r, g, b, a;
				img.pixel(x, y, r, g, b, a);
				if (r > 200 && g < 60 && b > 200) ++n;
			}
		return n;
	};

	// The body must be drawn at all — otherwise the assertion below would pass
	// for the wrong reason (nothing drawn is also "not on top").
	CHECK(loudPixels(0, 0, W, H) > 0);

	// …and it must be drawn where we think it is, or the sample below could sit
	// beside the overlap and pass while the bug is still there.
	CHECK(loudPixels(60, 80, 90, 120) > 0);

	// The sample: well inside the FRONT node's box, in the region where the two
	// boxes overlap. Graph (90,60) with pan 0 and zoom 1, node width ~177 and a
	// 60-unit body, so this rectangle is inside both nodes. Nothing loud may
	// reach it — that is what "the node in front covers what is behind it" means.
	CHECK(loudPixels(110, 85, 200, 120) == 0);
}

// ── A click picks the node in FRONT ─────────────────────────────────────────
// Reported as "es wird nicht immer die oberste Node ausgewählt beim Klick bzw.
// beim Drag", and the cause was placement rather than maths: selection ran
// INSIDE the node loop and stopped at the first node it hit. The loop draws
// back-to-front, so the first hit is the node at the BOTTOM — in an overlap the
// node behind was selected and dragged, every time.
//
// No pixels needed here, just ImGui's own input path: the harness feeds a mouse
// position and a press, and the canvas is asked who it picked.
TEST_CASE("Graph canvas: clicking an overlap selects the node in front")
{
	constexpr int W = 320, H = 240;
	Harness harness(W, H);

	GraphEditor::Model m;
	GraphEditor::State st;
	st.pan  = ImVec2(0.0f, 0.0f);
	st.zoom = 1.0f;

	// Node 2 is listed second, so it is drawn second — in front of node 1 — and
	// their boxes overlap around screen (98,68)…(225,132).
	m.nodeIds = []{ return std::vector<int>{ 1, 2 }; };
	m.getPos  = [](int id, float& x, float& y)
	{ x = (id == 1) ? 40.0f : 90.0f; y = (id == 1) ? 40.0f : 60.0f; };
	m.setPos  = [](int, float, float){};
	m.title   = [](int id){ return id == 1 ? std::string("Back") : std::string("Front"); };
	m.headerColor = [](int){ return IM_COL32(60, 60, 70, 255); };
	m.pins    = [](int){ return std::vector<GraphEditor::Pin>{}; };
	m.links   = []{ return std::vector<std::array<int, 4>>{}; };
	m.connect = [](int, int, int, int){ return false; };
	m.nodeBodyHeight = [](int){ return 60.0f; };

	ImGuiIO& io = ImGui::GetIO();
	const ImVec2 inOverlap(150.0f, 100.0f);

	auto frame = [&](bool mouseDown)
	{
		io.MousePos = inOverlap;
		io.MouseDown[0] = mouseDown;
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2((float)W, (float)H));
		ImGui::Begin("##canvas", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
		GraphEditor::draw("##ge", m, st, ImVec2((float)W, (float)H));
		ImGui::End();
		ImGui::Render();
	};

	// Two settling frames: ImGui resolves hover from the PREVIOUS frame, so a
	// press on frame one would land on a canvas that does not know it is hovered.
	frame(false);
	frame(false);
	frame(true);          // the press

	CHECK(st.selected == 2);
	// …and the drag that press started must move the same node, or a drag would
	// pick up the one behind while the click highlighted the one in front.
	CHECK(st.dragNode == 2);
}

// ── Grabbing a node brings it to the front ──────────────────────────────────
// Reported as a feel problem, which is the right way to read it: dragging a node
// along BEHIND its neighbours looks like the canvas is fighting you, and the
// grabbed node is the one moment where the reader's attention is on exactly this
// node.
//
// Two halves are checked, because only having both is worth anything. The
// PICTURE has to change in the overlap — a raise nobody can see is not a raise —
// and the HIT TEST has to follow it, or the node would look raised while a click
// in the overlap still picks the other one.
TEST_CASE("Graph canvas: grabbing a node raises it above the one in front")
{
	constexpr int W = 320, H = 240;
	Harness harness(W, H);

	GraphEditor::Model m;
	GraphEditor::State st;
	st.pan  = ImVec2(0.0f, 0.0f);
	st.zoom = 1.0f;

	// Same geometry as the test above: node 2 is listed second, so the graph's
	// own order draws it in front of node 1, and they overlap.
	m.nodeIds = []{ return std::vector<int>{ 1, 2 }; };
	m.getPos  = [](int id, float& x, float& y)
	{ x = (id == 1) ? 40.0f : 90.0f; y = (id == 1) ? 40.0f : 60.0f; };
	m.setPos  = [](int, float, float){};
	m.title   = [](int id){ return id == 1 ? std::string("Back") : std::string("Front"); };
	// Distinct headers, so the two nodes cannot rasterise to the same pixels and
	// make the comparison below pass for the wrong reason.
	m.headerColor = [](int id){ return id == 1 ? IM_COL32(200, 60, 60, 255)
	                                           : IM_COL32(60, 60, 200, 255); };
	m.pins    = [](int){ return std::vector<GraphEditor::Pin>{}; };
	m.links   = []{ return std::vector<std::array<int, 4>>{}; };
	m.connect = [](int, int, int, int){ return false; };
	m.nodeBodyHeight = [](int){ return 60.0f; };

	ImGuiIO& io = ImGui::GetIO();
	// Inside node 1 only: left of node 2's left edge, which sits near x=98.
	const ImVec2 inNodeOneOnly(60.0f, 55.0f);
	const ImVec2 inOverlap(150.0f, 100.0f);

	he_ui::Image shot;
	auto frame = [&](const ImVec2& at, bool mouseDown)
	{
		io.MousePos = at;
		io.MouseDown[0] = mouseDown;
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2((float)W, (float)H));
		ImGui::Begin("##canvas", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
		GraphEditor::draw("##ge", m, st, ImVec2((float)W, (float)H));
		ImGui::End();
		ImGui::Render();
		shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
	};

	// Two settling frames: ImGui resolves hover from the PREVIOUS frame.
	frame(inNodeOneOnly, false);
	frame(inNodeOneOnly, false);
	const he_ui::Image before = shot;
	REQUIRE(before.valid());

	frame(inNodeOneOnly, true);   // grab node 1, where nothing else covers it
	CHECK(st.dragNode == 1);
	// The raise is recorded on the PRESS, not on the first movement: there must be
	// no frame in which the grabbed node is still behind.
	REQUIRE(st.raised.size() == 1);
	CHECK(st.raised[0] == 1);

	frame(inNodeOneOnly, false);  // release
	frame(inNodeOneOnly, false);  // and draw with the new order
	const he_ui::Image after = shot;
	REQUIRE(after.valid());

	// The overlap has to look different now. Counted over the region where the
	// two boxes cross, so a stray pixel elsewhere cannot carry the assertion.
	REQUIRE(before.width == after.width);
	int differing = 0;
	for (int y = 70; y < 130; ++y)
		for (int x = 100; x < 220; ++x)
		{
			std::uint8_t r1, g1, b1, a1, r2, g2, b2, a2;
			before.pixel(x, y, r1, g1, b1, a1);
			after.pixel(x, y, r2, g2, b2, a2);
			if (r1 != r2 || g1 != g2 || b1 != b2) ++differing;
		}
	CHECK(differing > 0);

	// And the hit test agrees with what is drawn: a press in the overlap now
	// takes node 1, which before the raise went to node 2 (the test above pins
	// that direction). Without this, a raised node would look in front while
	// clicks kept falling through to the other one.
	frame(inOverlap, false);
	frame(inOverlap, false);
	frame(inOverlap, true);
	CHECK(st.dragNode == 1);
	CHECK(st.selected == 1);
}

// ── The hover tooltip must describe a Map as a Map ──────────────────────────
// Reported from a screenshot: hovering a Get Variable node for a Map showed the
// output as an array of the VALUE type. The node itself drew the right pin all
// along — it was the tooltip that lied, and the tooltip is the half a reader
// trusts, because it is the one that spells the type out.
//
// The cause: drawPinGlyph took a BOOL for "is a container" and drew the array
// grid for all three kinds. The proof is the KEY colour: a Map glyph paints its
// left column in the key type's colour, so a String key has to be findable in
// the picture.
//
// The words next to the glyph had the same problem for longer: the type read
// "Bool{String:}", which nobody parses as "a map of String to Bool". Both halves
// are checked here — the picture through its pixels, the wording through
// typeLabel(), which is the string the tooltip rasterises. There is no OCR: the
// image says a map glyph was drawn, typeLabel says what was written beside it.
TEST_CASE("Node tooltip: a Map output shows the map glyph, key colour and all")
{
	constexpr int W = 460, H = 260;
	Harness harness(W, H);

	// Map<String, Bool>: two pin types whose colours differ, or the assertion
	// below could not tell the columns apart.
	HorizonCode::Node node;
	node.type      = HorizonCode::NodeType::GetVariable;
	node.s         = "NewVar";
	node.propType  = HorizonCode::PinType::Bool;     // the VALUE type
	node.isArray   = true;
	node.container = HorizonCode::ContainerKind::Map;
	node.keyType   = HorizonCode::PinType::String;

	// The wording, before anything is drawn. Key FIRST, the way the type is read
	// aloud and the way the manual already writes it.
	using PT = HorizonCode::PinType;
	using CK = HorizonCode::ContainerKind;
	CHECK(HcEditorUtil::typeLabel(PT::Bool, true, CK::Map, PT::String) == "Map<String, Bool>");
	CHECK(HcEditorUtil::typeLabel(PT::Bool, true, CK::Array, PT::String) == "Array<Bool>");
	CHECK(HcEditorUtil::typeLabel(PT::String, true, CK::Set, PT::String) == "Set<String>");
	CHECK(HcEditorUtil::typeLabel(PT::Bool, false, CK::None, PT::String) == "Bool");
	// A graph saved before Set/Map existed carries isArray with no kind, and has
	// to keep reading as the array it is.
	CHECK(HcEditorUtil::typeLabel(PT::Bool, true, CK::None, PT::String) == "Array<Bool>");
	// Enum and Struct name their DEFINITION — "Enum" is not a type anybody has.
	CHECK(HcEditorUtil::typeLabel(PT::Struct, false, CK::None, PT::String,
	                              "Content/Types/Loadout.hasset") == "Loadout");
	CHECK(HcEditorUtil::typeLabel(PT::Int, true, CK::Map, PT::Enum, "",
	                              "Content/Types/Team.hasset") == "Map<Team, Int>");

	const std::uint32_t keyCol = HcEditorUtil::pinTypeColor(HorizonCode::PinType::String);
	const std::uint32_t valCol = HcEditorUtil::pinTypeColor(HorizonCode::PinType::Bool);
	REQUIRE(keyCol != valCol);

	// Captured out of the render so the assertions can talk about WHERE the ink
	// is instead of just how much: the glyph sits in the first `line` pixels of
	// the content column, the words start after it.
	float contentX = 0.0f, lineH = 0.0f;

	const he_ui::Image img = shoot("doc-map-pin-tooltip", W, H, 3, [&](int) {
		ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
		ImGui::SetNextWindowSize(ImVec2(W - 20.0f, H - 20.0f));
		ImGui::Begin("##maptip", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
		{
			EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 32.0f);
			contentX = ImGui::GetCursorScreenPos().x;
			lineH    = ImGui::GetTextLineHeight();
			HcEditorUtil::drawNodeDoc(node);
		}
		ImGui::End();
	});
	REQUIRE(img.valid());
	REQUIRE(lineH > 0.0f);

	// Exact-colour pixels inside an x band. Exact, because the pin colours are
	// only ever painted flat: anti-aliased edges land on neighbouring values and
	// are not counted, which is what keeps this from matching the whole panel.
	auto countBand = [&](std::uint32_t col, int x0, int x1)
	{
		const std::uint8_t wr = (std::uint8_t)((col >> IM_COL32_R_SHIFT) & 0xFF);
		const std::uint8_t wg = (std::uint8_t)((col >> IM_COL32_G_SHIFT) & 0xFF);
		const std::uint8_t wb = (std::uint8_t)((col >> IM_COL32_B_SHIFT) & 0xFF);
		int n = 0;
		for (int y = 0; y < img.height; ++y)
			for (int x = (x0 < 0 ? 0 : x0); x < (x1 > img.width ? img.width : x1); ++x)
			{
				std::uint8_t r, g, b, a;
				img.pixel(x, y, r, g, b, a);
				if (r == wr && g == wg && b == wb) ++n;
			}
		return n;
	};
	const int glyphEnd = (int)(contentX + lineH) + 1;

	// The value colour is everywhere anyway (the glyph's right column and the
	// "Bool" in the type), so it only guards against an empty render.
	CHECK(countBand(valCol, 0, img.width) > 0);

	// Two separate claims, deliberately not one count over the whole picture.
	// They used to be the same assertion, and then the type label started
	// spelling "String" in the key colour as well — after which a regression to
	// the flat array grid would have gone through unnoticed, on the strength of
	// the WORD alone.
	//
	// The glyph: nothing but a Map paints its left column in the key type's
	// colour, and only the glyph is drawn this far left.
	CHECK(countBand(keyCol, 0, glyphEnd) > 0);
	// The words: the "String" in "Map<String, Bool>", out where the label is.
	CHECK(countBand(keyCol, glyphEnd, img.width) > 0);

}

// ── A type label must not fall apart at the wrap column ─────────────────────
// The label is five separate ImGui items (one per colour), and that is at the
// mercy of whatever wrap column the caller pushed. A piece that STARTS past that
// column gets a wrap width of one pixel and breaks after every character;
// ItemSize then reports the top of that stack as the previous line, so
// SameLine(0,0) puts the rest of the label back ABOVE it. The result is a pile
// of single letters, and no count of pixels notices — the ink is all still
// there, just spread down the panel. So the span is the assertion.
//
// The wrap here is TIGHTER than any real caller's: at the 35 em the tooltips
// actually push, the label starts around 75 px in and never reaches the column,
// so the fault is latent rather than live. It is guarded anyway, because what
// keeps it latent is a pin happening to be called "Value" — nothing structural —
// and this scene is what holds the guard in place.
TEST_CASE("Node tooltip: a type label stays on one line past the wrap column")
{
	// Tall enough that the Outputs row is still in frame once a narrow wrap has
	// stretched the description down the panel — otherwise the label under test
	// is simply off the bottom and the assertions have nothing to look at.
	constexpr int W = 420, H = 520;
	Harness harness(W, H);

	HorizonCode::Node node;
	node.type      = HorizonCode::NodeType::GetVariable;
	node.s         = "SpawnedActorsByTeamIdentifier";
	node.propType  = HorizonCode::PinType::Bool;
	node.isArray   = true;
	node.container = HorizonCode::ContainerKind::Map;
	node.keyType   = HorizonCode::PinType::String;

	const std::uint32_t keyCol = HcEditorUtil::pinTypeColor(HorizonCode::PinType::String);

	float lineH = 0.0f;
	const he_ui::Image img = shoot("doc-long-type-label", W, H, 3, [&](int) {
		ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
		ImGui::SetNextWindowSize(ImVec2(W - 20.0f, H - 20.0f));
		ImGui::Begin("##longtip", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
		{
			// Chosen so the column falls INSIDE the label: "Map<" starts before it
			// and "String" after, which is exactly the condition the fault needs.
			EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 6.0f);
			lineH = ImGui::GetTextLineHeight();
			HcEditorUtil::drawNodeDoc(node);
		}
		ImGui::End();
	});
	REQUIRE(img.valid());
	REQUIRE(lineH > 0.0f);

	// Where the key colour is, top to bottom. It belongs to two things only: the
	// map glyph's left column and the word "String" beside it, which sit on the
	// same row — so one line height, plus slack for the two being a pixel or two
	// apart. Character-stacking spreads it over half the panel.
	int minY = img.height, maxY = -1;
	const std::uint8_t wr = (std::uint8_t)((keyCol >> IM_COL32_R_SHIFT) & 0xFF);
	const std::uint8_t wg = (std::uint8_t)((keyCol >> IM_COL32_G_SHIFT) & 0xFF);
	const std::uint8_t wb = (std::uint8_t)((keyCol >> IM_COL32_B_SHIFT) & 0xFF);
	for (int y = 0; y < img.height; ++y)
		for (int x = 0; x < img.width; ++x)
		{
			std::uint8_t r, g, b, a;
			img.pixel(x, y, r, g, b, a);
			if (r == wr && g == wg && b == wb) { if (y < minY) minY = y; if (y > maxY) maxY = y; }
		}
	REQUIRE(maxY >= 0);
	CHECK((float)(maxY - minY) < lineH * 2.0f);
}

// ── The variable list has two looks, and both of them exist ──────────────────
// Preferences ▸ Editor ▸ HorizonCode offers Detailed and Compact. A setting that
// writes a config key nothing reads is worse than no setting, so this renders
// the same variable under both and asserts they differ in the way the page
// promises: Detailed puts the type on a second line, Compact keeps it on one.
//
// This is also where the spelling is checked in the place the complaint came
// from. The list used to append the type in brackets — "NewVar (Bool{String:})"
// — which is two problems in one row: a shape nobody reads, and a name whose
// type is glued to it.
TEST_CASE("Variable rows: Detailed and Compact are two real, different looks")
{
	constexpr int W = 320, H = 160;
	using PT = HorizonCode::PinType;
	using CK = HorizonCode::ContainerKind;

	HcEditorUtil::VariableRowDesc var;
	var.name      = "NewVar";
	var.type      = PT::Bool;        // the VALUE type
	var.isArray   = true;
	var.container = CK::Map;
	var.keyType   = PT::String;

	const std::uint32_t keyCol = HcEditorUtil::pinTypeColor(PT::String);
	const std::uint32_t valCol = HcEditorUtil::pinTypeColor(PT::Bool);
	REQUIRE(keyCol != valCol);

	auto shootStyle = [&](const char* name, HcEditorUtil::VariableRowStyle style,
	                      float& heightOut)
	{
		Harness harness(W, H);
		float h = 0.0f;
		he_ui::Image img = shoot(name, W, H, 3, [&](int) {
			ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f));
			ImGui::SetNextWindowSize(ImVec2(W - 16.0f, H - 16.0f));
			ImGui::Begin("##vars", nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
			const float y0 = ImGui::GetCursorPosY();
			HcEditorUtil::variableRow(var, false, style);
			h = ImGui::GetCursorPosY() - y0;
			ImGui::End();
		});
		heightOut = h;
		return img;
	};

	float detailedH = 0.0f, compactH = 0.0f;
	const he_ui::Image detailed = shootStyle("var-row-detailed",
		HcEditorUtil::VariableRowStyle::Detailed, detailedH);
	const he_ui::Image compact = shootStyle("var-row-compact",
		HcEditorUtil::VariableRowStyle::Compact, compactH);
	REQUIRE(detailed.valid());
	REQUIRE(compact.valid());

	// The promise on the settings page: two lines against one.
	CHECK(detailedH > compactH);

	auto count = [](const he_ui::Image& img, std::uint32_t col)
	{
		const std::uint8_t wr = (std::uint8_t)((col >> IM_COL32_R_SHIFT) & 0xFF);
		const std::uint8_t wg = (std::uint8_t)((col >> IM_COL32_G_SHIFT) & 0xFF);
		const std::uint8_t wb = (std::uint8_t)((col >> IM_COL32_B_SHIFT) & 0xFF);
		int n = 0;
		for (int y = 0; y < img.height; ++y)
			for (int x = 0; x < img.width; ++x)
			{
				std::uint8_t r, g, b, a;
				img.pixel(x, y, r, g, b, a);
				if (r == wr && g == wg && b == wb) ++n;
			}
		return n;
	};

	// Neither look may drop the type: the glyph's key column and the "String" of
	// "Map<String, Bool>" are both painted in the key type's colour, so a row
	// that shows a name and nothing else scores zero here.
	CHECK(count(detailed, keyCol) > 0);
	CHECK(count(compact, keyCol) > 0);
	CHECK(count(detailed, valCol) > 0);
	CHECK(count(compact, valCol) > 0);

	// And the two are actually different pictures — not one look shipped twice.
	REQUIRE(detailed.width == compact.width);
	REQUIRE(detailed.height == compact.height);
	int differing = 0;
	for (int y = 0; y < detailed.height; ++y)
		for (int x = 0; x < detailed.width; ++x)
		{
			std::uint8_t r1, g1, b1, a1, r2, g2, b2, a2;
			detailed.pixel(x, y, r1, g1, b1, a1);
			compact.pixel(x, y, r2, g2, b2, a2);
			if (r1 != r2 || g1 != g2 || b1 != b2) ++differing;
		}
	CHECK(differing > 0);
}

// ── The "+" of a section header ──────────────────────────────────────────────
// The complaint this answers, verbatim: "der + button zum hinzufuegen sollte
// auf einer hoehe mit dem title variables ... sein, das sieht doof aus wenn der
// allein in einer zeile drunter steht". Every call site drew
// SeparatorText("Variables") and then addButton() with nothing between them, so
// the button landed on the next row.
//
// That is a layout claim, and layout is exactly what this harness can answer:
// where the ink for the heading is, and where the ink for the button is.
TEST_CASE("Section header: the + sits on the heading, not on a row below it")
{
	constexpr int W = 360, H = 200;

	// What one scene reports back. Screen coordinates, which are image
	// coordinates here: the harness viewport starts at (0, 0).
	struct Probe
	{
		float  rowTopY   = 0.0f;   // where the heading row starts
		float  rowBotY   = 0.0f;   // and where it ends
		float  sectionH  = 0.0f;   // what the header cost in layout height
		float  labelX0   = 0.0f;   // the heading glyphs' column range
		float  labelX1   = 0.0f;
		float  spacingX  = 0.0f;
		float  frameH    = 0.0f;   // how tall a "+" is
		ImVec2 btnMin{}, btnMax{};
		float  windowLeft = 0.0f;
	};

	// `withButton == false` draws a bare SeparatorText in an otherwise identical
	// scene — the baseline the header must not cost more than.
	auto run = [&](const char* name, bool withButton, Probe& p)
	{
		Harness harness(W, H);
		return shoot(name, W, H, 3, [&](int) {
			ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f));
			ImGui::SetNextWindowSize(ImVec2(W - 16.0f, H - 16.0f));
			ImGui::Begin("##section", nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
				ImGuiWindowFlags_NoSavedSettings);

			const ImGuiStyle& style = ImGui::GetStyle();
			const ImVec2 before = ImGui::GetCursorScreenPos();
			if (withButton)
				EditorWidgets::sectionHeaderAdd("Variables", "##var", "Add a variable");
			else
				ImGui::SeparatorText("Variables");
			const ImVec2 after = ImGui::GetCursorScreenPos();

			p.windowLeft = ImGui::GetWindowPos().x;
			p.rowTopY    = before.y;
			p.rowBotY    = after.y - style.ItemSpacing.y;
			p.sectionH   = after.y - before.y;
			p.spacingX   = style.ItemSpacing.x;
			p.frameH     = ImGui::GetFrameHeight();
			// SeparatorTextAlign.x is 0 in this theme, so the label starts one
			// SeparatorTextPadding.x in from the row's left edge. Reading the
			// range from the style rather than from the picture keeps the two
			// column bands below independent of what was actually drawn.
			p.labelX0 = before.x + style.SeparatorTextPadding.x;
			p.labelX1 = p.labelX0 + ImGui::CalcTextSize("Variables").x;
			// The helper leaves the "+" as the last submitted item on purpose.
			if (withButton)
			{
				p.btnMin = ImGui::GetItemRectMin();
				p.btnMax = ImGui::GetItemRectMax();
			}

			// Content underneath, so a header that pushed the cursor down has
			// somewhere visible to push it to.
			ImGui::TextUnformatted("Health");
			ImGui::TextUnformatted("Score");
			ImGui::End();
		});
	};

	Probe hdr, plain;
	const he_ui::Image img = run("section-header-add", true, hdr);
	run("section-header-plain", false, plain);
	REQUIRE(img.valid());

	// ── 1. The section costs no more height than the heading alone ───────────
	// Red for the layout that prompted this: SeparatorText() followed by a bare
	// addButton() spends one ItemSpacing.y plus a whole GetFrameHeight() extra,
	// and everything below the header slides down by that much. Also red for a
	// helper that positions the button but forgets to put the cursor back.
	CHECK(hdr.sectionH == doctest::Approx(plain.sectionH));

	// ── 2. Heading and button share the same rows ────────────────────────────
	// "Ink" is anything that is not the panel's own background. Sampled from the
	// window's top padding band rather than computed: the rasteriser blends in
	// 8-bit sRGB and nothing at all is drawn up there.
	std::uint8_t bgR, bgG, bgB, bgA;
	img.pixel(int(hdr.windowLeft) + 40, int(hdr.rowTopY) - 4, bgR, bgG, bgB, bgA);
	auto isInk = [&](int x, int y)
	{
		std::uint8_t r, g, b, a;
		img.pixel(x, y, r, g, b, a);
		return std::abs(int(r) - int(bgR)) + std::abs(int(g) - int(bgG))
		     + std::abs(int(b) - int(bgB)) > 12;
	};

	// The rows a column band has ink on. Returns false for an empty band, so an
	// implementation that draws no button cannot pass the overlap test below by
	// having nothing to disagree with.
	auto inkRows = [&](int x0, int x1, int y0, int y1, int& top, int& bot)
	{
		bool any = false;
		for (int y = y0; y <= y1; ++y)
			for (int x = x0; x <= x1; ++x)
				if (isInk(x, y))
				{
					if (!any) { top = y; any = true; }
					bot = y;
					break;
				}
		return any;
	};

	// The heading's own columns: past the short rule stub that SeparatorText
	// draws to the LEFT of the label, and stopping before the rule picks up
	// again on the right, so this band holds the glyphs and nothing else.
	int labelTop = 0, labelBot = 0;
	const bool labelInked = inkRows(int(hdr.labelX0) + 1, int(hdr.labelX1) - 1,
	                                int(hdr.rowTopY), int(hdr.rowBotY),
	                                labelTop, labelBot);
	// The button's own columns, inset so the rounded corners' antialiasing is
	// not what the span is measured from — and scanned over the HEADING ROW
	// only, never past it. That bound is the point: a button that fell onto the
	// row below leaves these rows to the separator's rule, which also inks them.
	int btnTop = 0, btnBot = 0;
	const bool btnInked = inkRows(int(hdr.btnMin.x) + 3, int(hdr.btnMax.x) - 3,
	                              int(hdr.rowTopY), int(hdr.rowBotY),
	                              btnTop, btnBot);
	REQUIRE(labelInked);
	REQUIRE(btnInked);

	// So what is found here has to be a BLOCK, not a line: the "+" fills a
	// GetFrameHeight() square, a rule crossing the same columns is two pixels
	// tall. This is the assertion that stays red for the layout as it shipped —
	// SeparatorText() then addButton() with nothing between them leaves the rule
	// running through this band and the button a whole row further down, which
	// scores 2 here instead of the twenty-odd a button scores.
	const float rowH  = hdr.rowBotY - hdr.rowTopY;
	const int   solid = int(hdr.frameH < rowH ? hdr.frameH : rowH) - 4;
	INFO("button ink in the heading row: " << (btnBot - btnTop + 1)
	     << " rows, expected at least " << solid);
	CHECK(btnBot - btnTop + 1 >= solid);

	// And this is the complaint itself, as an assertion: heading and button
	// share rows. Red the moment the "+" drops onto a line of its own.
	INFO("heading rows " << labelTop << ".." << labelBot
	     << ", button rows " << btnTop << ".." << btnBot);
	CHECK(labelTop <= btnBot);
	CHECK(btnTop <= labelBot);

	// And it is CENTRED on the row rather than merely somewhere on it. The two
	// are not the same height — a "+" is GetFrameHeight(), the row is the text
	// plus twice SeparatorTextPadding.y — so "equal margins above and below" is
	// the only spelling of centred that does not depend on which of them happens
	// to be taller in a given theme. Red for a button left at the row's top
	// edge, which is where a plain SameLine() puts it.
	//
	// One pixel of slack, and one only: the offset is truncated to whole pixels,
	// so a row whose spare height is odd splits it 4/5 rather than 4.5/4.5.
	const float above = hdr.btnMin.y - hdr.rowTopY;
	const float below = hdr.rowBotY  - hdr.btnMax.y;
	INFO("margin above the + " << above << ", below " << below);
	CHECK(std::fabs(above - below) <= 1.0f);

	// ── 3. The rule stops short of the button ────────────────────────────────
	// SeparatorText draws its line out to the window's work rect, so the naive
	// repair — SeparatorText() + SameLine() + addButton() — puts the button ON
	// the line instead of after it. The helper narrows the work rect for the
	// duration of the call, which leaves exactly one ItemSpacing.x of clear
	// space before the button. Insetting by two columns on each side keeps an
	// antialiased line END from reading as a line that never stopped.
	const int gapX0 = int(hdr.btnMin.x - hdr.spacingX) + 2;
	const int gapX1 = int(hdr.btnMin.x) - 2;
	REQUIRE(gapX1 >= gapX0);
	int gapInk = 0;
	for (int y = int(hdr.rowTopY); y <= int(hdr.rowBotY); ++y)
		for (int x = gapX0; x <= gapX1; ++x)
			if (isInk(x, y)) ++gapInk;
	INFO("ink between the rule's end and the button: " << gapInk);
	CHECK(gapInk == 0);

	// And the button really is at the right-hand end of the row, not tucked in
	// behind the label — red for a helper that only fixed the height.
	CHECK(hdr.btnMin.x > hdr.labelX1);
}

// ── A dropdown on a pin has to FIT on the pin ────────────────────────────────
// The inline editor slot was invented for a number box and sized for one. A
// string pin that offers a list needs more: the name has to be readable, and the
// arrow that says "this is a list" sits at the right-hand end, which is the
// first thing a slot too narrow throws away. Both halves are visible only in
// pixels, so they are checked in pixels.
TEST_CASE("ui shot: a pin's dropdown fits inside its node")
{
	constexpr int W = 420, H = 200;
	Harness harness(W, H);

	// One engine call, shaped like Play Animation: a reference, a name that is
	// a list, a flag, and a second name that is a list.
	HorizonCode::Node node;
	node.type = HorizonCode::NodeType::EngineCall;
	node.s = "widget.playAnimation";
	node.hasArg = true;   // exec node
	node.params = { { "widget", HorizonCode::PinType::Ref },
	                { "animation", HorizonCode::PinType::String },
	                { "restoreAfterCompleted", HorizonCode::PinType::Bool },
	                { "direction", HorizonCode::PinType::String } };
	node.results = { { "ok", HorizonCode::PinType::Bool } };
	{
		HorizonCode::Value v; v.type = HorizonCode::PinType::String; v.s = "Ping Pong";
		node.pinDefaults[3] = v;   // data-in 3 = direction
	}

	GraphEditor::Model m;
	GraphEditor::State st;
	st.pan = ImVec2(0.0f, 0.0f);
	st.zoom = 1.0f;
	m.nodeIds = []{ return std::vector<int>{ 1 }; };
	m.getPos  = [](int, float& x, float& y){ x = 30.0f; y = 30.0f; };
	m.setPos  = [](int, float, float){};
	m.title   = [](int){ return std::string("Play Animation"); };
	m.headerColor = [](int){ return IM_COL32(60, 60, 70, 255); };
	m.links   = []{ return std::vector<std::array<int, 4>>{}; };
	m.connect = [](int, int, int, int){ return false; };
	// The pins as the graph sees them: [exec in][exec out][data ins][data outs].
	m.pins = [](int)
	{
		std::vector<GraphEditor::Pin> pins;
		pins.push_back({ 0, "",          IM_COL32(230, 230, 230, 255), true,  true  });
		pins.push_back({ 1, "",          IM_COL32(230, 230, 230, 255), false, true  });
		pins.push_back({ 2, "Widget",    IM_COL32(120, 180, 240, 255), true,  false });
		pins.push_back({ 3, "Animation", IM_COL32(240, 180, 120, 255), true,  false });
		pins.push_back({ 4, "Restore",   IM_COL32(200, 120, 200, 255), true,  false });
		pins.push_back({ 5, "Direction", IM_COL32(240, 180, 120, 255), true,  false });
		pins.push_back({ 6, "Ok",        IM_COL32(200, 120, 200, 255), false, false });
		return pins;
	};
	m.pinHasInlineEditor = [&node](int, int pin)
	{ return HcEditorUtil::pinSupportsInlineDefault(node, pin); };
	m.pinInlineEditorWidth = [&node](int, int pin)
	{ return HcEditorUtil::pinInlineEditorWidth(node, pin); };
	m.drawPinInlineEditor = [&node](int, int pin)
	{
		bool committed = false;
		HcEditorUtil::drawPinDefaultEditor(node, pin, committed,
			[](const HorizonCode::Node& n, const std::string& param)
			{ return HcEditorUtil::engineParamChoices(n, param, nullptr); });
	};

	// Where the canvas puts the editor column, measured with the same rule it
	// uses: past the longest input label, in the size the labels are drawn at.
	// Captured during the frame, because it needs the font.
	float editorX = 0.0f, originX = 0.0f, originY = 0.0f;
	const he_ui::Image img = shoot("graph-pin-dropdown", W, H, 3, [&](int)
	{
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2((float)W, (float)H));
		ImGui::Begin("##canvas", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
		const float labelScale = 13.0f / ImGui::GetFontSize();
		float labelW = 0.0f;
		for (const GraphEditor::Pin& p : m.pins(1))
			if (p.input && !p.isExec && !p.label.empty())
				labelW = std::max(labelW, ImGui::CalcTextSize(p.label.c_str()).x * labelScale);
		editorX = 8.0f + labelW + 6.0f;
		// Where graph (0,0) lands on screen: the canvas draws from the cursor,
		// and with no pan and zoom 1 that is the whole transform.
		originX = ImGui::GetCursorScreenPos().x;
		originY = ImGui::GetCursorScreenPos().y;
		GraphEditor::draw("##ge", m, st, ImVec2((float)W, (float)H));
		ImGui::End();
	});
	REQUIRE(img.valid());
	REQUIRE(editorX > 0.0f);

	// What the canvas laid out: the node grew by what the widest editor asked
	// for beyond the default slot, and the slot itself starts in a column past
	// the longest label. Measured from the picture rather than from an absolute
	// number, because where the canvas puts its origin is its own business.
	const float slotW = HcEditorUtil::pinInlineEditorWidth(node, 3);
	const float extra = slotW - GraphEditor::kPinEditorW;
	CHECK(extra > 0.0f);   // a string pin really does ask for more room

	auto isInk = [&](int x, int y)
	{
		std::uint8_t r, g, b, a;
		img.pixel(x, y, r, g, b, a);
		return std::abs(int(r) - int(kBgR)) + std::abs(int(g) - int(kBgG)) +
		       std::abs(int(b) - int(kBgB)) > 24;
	};

	// The Direction row. The left column starts with the EXEC pin, so it is the
	// fifth row under the title bar, not the fourth.
	const float rowY = originY + 30.0f + GraphEditor::kTitleH + 4.5f * GraphEditor::kRowH;
	const int y0 = int(rowY) - 8, y1 = int(rowY) + 8;

	// 1. The ARROW is drawn at the right-hand end of the slot. Bright pixels,
	//    not any ink: the frame's background fills the slot whatever happens to
	//    the widget inside it, so counting ink here would pass for a control
	//    laid out to the whole canvas and clipped back to a stub — which is the
	//    bug this is about, and which leaves the arrow outside.
	auto isGlyph = [&](int x, int y)
	{
		std::uint8_t r, g, b, a;
		img.pixel(x, y, r, g, b, a);
		return r > 150 && g > 150 && b > 150;
	};
	// Node and slot in screen pixels, from the transform the canvas used.
	const int nodeLeft  = int(originX + 30.0f);
	const int slotRight = int(originX + 30.0f + editorX + slotW);
	int arrowInk = 0;
	for (int y = y0; y < y1; ++y)
		for (int x = slotRight - 18; x < slotRight - 2; ++x)
			if (isGlyph(x, y)) ++arrowInk;
	INFO("the dropdown's arrow, in bright pixels: " << arrowInk
	     << " (band x " << (slotRight - 18) << ".." << (slotRight - 2)
	     << ", y " << y0 << ".." << y1 << ", editorX " << editorX << ")");
	CHECK(arrowInk > 10);

	// 2. …because the NODE made room for it. Its right edge is the last ink on
	//    the row, and it has to stand beyond both the default width and the end
	//    of the control. Without the growth the arrow would sit on the edge or
	//    outside it.
	int nodeRight = 0;
	for (int y = y0; y < y1; ++y)
		for (int x = W - 5; x > nodeLeft; --x)
			if (isInk(x, y)) { nodeRight = std::max(nodeRight, x); break; }
	INFO("node " << nodeLeft << ".." << nodeRight << " (" << (nodeRight - nodeLeft)
	     << " wide, default is " << int(GraphEditor::kNodeW) << "), slot ends at " << slotRight);
	CHECK(nodeRight - nodeLeft > int(GraphEditor::kNodeW));
	CHECK(slotRight < nodeRight - 2);
}
