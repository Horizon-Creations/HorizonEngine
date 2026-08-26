#include "doctest.h"

#include "ImGuiSoftwareRaster.h"

#include "DocsLibrary.h"
#include "DocsPanel.h"
#include "EditorHelp.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "HcEditorUtil.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonScene/EngineApi.h>

#include <imgui.h>

#include <cstdlib>
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
