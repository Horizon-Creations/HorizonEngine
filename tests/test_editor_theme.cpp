#include "doctest.h"
#include <imgui.h>
#include <cmath>
#include <string>

#include "EditorTheme.h"
#include "EditorToolbar.h"   // the hand-drawn bars' palette
#include <utility>

// ── The theme must not leave ImGui's blue behind ─────────────────────────────
// applyHorizonDarkTheme starts from ImGui::StyleColorsDark, and StyleColorsDark
// does not merely fill in defaults — it DERIVES colours from its own palette:
//
//     CheckboxSelectedBg  = lerp(FrameBg, FrameBgActive, 0.65)
//     TabSelectedOverline = HeaderActive
//     TextLink            = HeaderActive
//
// ImGui's HeaderActive and FrameBgActive are its signature blue (0.26, 0.59,
// 0.98). So a slot the theme forgets to name does not fall back to something
// neutral — it keeps a blue computed from a palette that has already been
// thrown away. That is not hypothetical: the first pass of the amber theme
// shipped a blue tile behind every checked checkbox and a blue rule above every
// docked tab, and both were found by eye, one at a time, after the fact.
//
// Eyes do not scale to 67 slots. These tests do.

namespace {

struct ImGuiCtx
{
	ImGuiCtx()
	{
		ImGui::CreateContext();
		ImGuiIO& io    = ImGui::GetIO();
		io.DisplaySize = ImVec2(1280.0f, 720.0f);
		io.DeltaTime   = 1.0f / 60.0f;
		io.IniFilename = nullptr;
		// A font atlas the backend never builds still needs to exist for
		// NewFrame; the tests here never draw, but the style is read from a
		// live context.
		unsigned char* px = nullptr;
		int w = 0, h = 0;
		io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
	}
	~ImGuiCtx() { ImGui::DestroyContext(); }
};

bool near(float a, float b, float eps = 0.02f) { return std::fabs(a - b) <= eps; }

// ImGui's accent, the one that must not survive the theme anywhere.
bool isImGuiBlue(const ImVec4& c)
{
	return near(c.x, 0.26f) && near(c.y, 0.59f) && near(c.z, 0.98f);
}

// Anything whose blue channel clearly dominates BOTH others is off-brand for a
// palette whose every accent is amber or gold. Deliberately loose: it is there
// to catch a forgotten slot, not to police a designer.
bool isBlueDominant(const ImVec4& c)
{
	if (c.w < 0.05f) return false;              // fully transparent: no colour on screen
	return c.z > c.x + 0.12f && c.z > c.y + 0.08f;
}

const char* name(int i) { return ImGui::GetStyleColorName(i); }

} // namespace

TEST_CASE("editor theme leaves no ImGui blue in any colour slot")
{
	ImGuiCtx ctx;
	HE::Ed::applyHorizonDarkTheme();

	const ImVec4* c = ImGui::GetStyle().Colors;
	for (int i = 0; i < ImGuiCol_COUNT; ++i)
	{
		INFO("slot " << i << " = " << name(i));
		CHECK_FALSE(isImGuiBlue(c[i]));
	}
}

TEST_CASE("editor theme has no blue-dominant colour slot")
{
	ImGuiCtx ctx;
	HE::Ed::applyHorizonDarkTheme();

	const ImVec4* c = ImGui::GetStyle().Colors;
	for (int i = 0; i < ImGuiCol_COUNT; ++i)
	{
		INFO("slot " << i << " = " << name(i)
		     << "  rgba(" << c[i].x << ", " << c[i].y << ", "
		     << c[i].z << ", " << c[i].w << ")");
		CHECK_FALSE(isBlueDominant(c[i]));
	}
}

// The catch above only fires for colours that are blue. A future ImGui can add
// a slot derived from something GREY, which no colour check would notice and
// which would then quietly ignore the brand. So: assert that applying the theme
// actually moves every slot away from where StyleColorsDark left it — with an
// explicit, named list of the ones that are legitimately identical.
TEST_CASE("every ImGui colour slot is assigned by the theme")
{
	ImGuiCtx ctx;

	ImGui::StyleColorsDark();
	ImVec4 dark[ImGuiCol_COUNT];
	for (int i = 0; i < ImGuiCol_COUNT; ++i) dark[i] = ImGui::GetStyle().Colors[i];

	HE::Ed::applyHorizonDarkTheme();
	const ImVec4* c = ImGui::GetStyle().Colors;

	// Slots where the theme deliberately agrees with ImGui's dark value. Each
	// one is a decision, which is why they are spelled out rather than skipped
	// by a tolerance: a neutral black scrim and a fully transparent slot have
	// no brand to express.
	auto agreesOnPurpose = [](int i) {
		switch (i)
		{
			case ImGuiCol_BorderShadow:     // transparent
			case ImGuiCol_ResizeGrip:       // transparent until hovered
			case ImGuiCol_ScrollbarBg:      // black at 12% — a recess, not a colour
			case ImGuiCol_TableRowBg:       // transparent
			case ImGuiCol_ModalWindowDimBg: // black scrim
				return true;
			default: return false;
		}
	};

	for (int i = 0; i < ImGuiCol_COUNT; ++i)
	{
		if (agreesOnPurpose(i)) continue;
		INFO("slot " << i << " = " << name(i)
		     << " was never assigned by applyHorizonDarkTheme (still at the "
		        "StyleColorsDark value). If ImGui added it, give it a brand "
		        "value; if it is genuinely neutral, add it to agreesOnPurpose.");
		const bool moved = !(near(c[i].x, dark[i].x, 0.001f) &&
		                     near(c[i].y, dark[i].y, 0.001f) &&
		                     near(c[i].z, dark[i].z, 0.001f) &&
		                     near(c[i].w, dark[i].w, 0.001f));
		CHECK(moved);
	}
}

TEST_CASE("the accent stays distinguishable from the warning colour")
{
	// The reason the accent is the logo's AMBER and not its gold. If someone
	// later "unifies" them, this is the test that explains why they should not.
	using namespace HE::Ed::Theme;
	const ImVec4 warn(230.0f / 255.0f, 176.0f / 255.0f, 86.0f / 255.0f, 1.0f);
	auto dist = [](const ImVec4& a, const ImVec4& b) {
		return std::sqrt((a.x - b.x) * (a.x - b.x) +
		                 (a.y - b.y) * (a.y - b.y) +
		                 (a.z - b.z) * (a.z - b.z));
	};
	CHECK(dist(Accent, warn) > 0.25f);
	CHECK(dist(TextHeading, warn) > 0.25f);
}

// ── Text stays readable on the grounds the palette puts it on ────────────────
// Topic 224: an amber Warn line on an amber-flashed row, a red error on a red
// tint, white on an amber button, and a TextDim that missed 4.5:1 on every
// surface. Each pair was picked on its own and looked fine to whoever picked it.
// These hold the palette's own pairs to Theme::kMinTextContrast, so the next
// shade that someone nudges is measured instead of eyeballed.

namespace {

using HE::Ed::Theme::contrastRatio;
using HE::Ed::Theme::kMinTextContrast;
using HE::Ed::Theme::over;

ImVec4 fromU32(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

} // namespace

TEST_CASE("theme contrast: the WCAG ratio and the helpers built on it")
{
	using namespace HE::Ed::Theme;
	const ImVec4 black(0, 0, 0, 1), white(1, 1, 1, 1);
	CHECK(contrastRatio(black, white) == doctest::Approx(21.0f).epsilon(0.001));
	CHECK(contrastRatio(white, black) == doctest::Approx(21.0f).epsilon(0.001));
	CHECK(contrastRatio(Accent, Accent) == doctest::Approx(1.0f));

	// Half-transparent white over black is the grey ImGui would blend.
	const ImVec4 grey = over(ImVec4(1, 1, 1, 0.5f), black);
	CHECK(grey.x == doctest::Approx(0.5f));
	CHECK(grey.w == doctest::Approx(1.0f));

	// Already readable: unchanged. Not readable: moved just far enough.
	const ImVec4 popup = warm(0.13f);
	const ImVec4 same  = readableOn(Text, popup);
	CHECK(same.x == Text.x);
	CHECK(same.y == Text.y);
	const ImVec4 amberRow = over(alpha(Accent, 0.35f), popup);
	const ImVec4 dimOld(0.46f, 0.44f, 0.41f, 1.0f);
	REQUIRE(contrastRatio(dimOld, amberRow) < 2.0f);   // the reported frame
	const ImVec4 fixed = readableOn(dimOld, amberRow);
	CHECK(contrastRatio(fixed, amberRow) >= kMinTextContrast);
	CHECK(contrastRatio(fixed, amberRow) < kMinTextContrast + 0.5f);   // no further than needed

	// A light ground gets dark text, a dark one light text.
	CHECK(contrastRatio(textOn(Accent), Accent) >= kMinTextContrast);
	CHECK(textOn(Accent).x == Ink.x);
	CHECK(textOn(warm(0.13f)).x == Text.x);
}

TEST_CASE("theme contrast: every text colour reads on every surface")
{
	ImGuiCtx ctx;
	HE::Ed::applyHorizonDarkTheme();
	using namespace HE::Ed::Theme;
	const ImVec4* c = ImGui::GetStyle().Colors;

	const std::pair<const char*, ImVec4> texts[] = {
		{ "Text", Text }, { "TextDim", TextDim }, { "TextHeading", TextHeading },
		{ "Ok", Ok }, { "Warn", Warn }, { "Bad", Bad },
		{ "ImGuiCol_Text", c[ImGuiCol_Text] }, { "ImGuiCol_TextDisabled", c[ImGuiCol_TextDisabled] },
	};
	// FrameBg because TextDisabled is also an input field's placeholder. Not
	// Button: a disabled label on a button is a disabled control, and Text on
	// Button is checked below.
	const int surfaces[] = { ImGuiCol_WindowBg, ImGuiCol_ChildBg, ImGuiCol_PopupBg,
	                         ImGuiCol_FrameBg, ImGuiCol_MenuBarBg, ImGuiCol_TitleBgActive };
	for (int s : surfaces)
		for (const auto& [name, fg] : texts)
		{
			const float r = contrastRatio(fg, over(c[s], c[ImGuiCol_WindowBg]));
			INFO(name << " on " << ImGui::GetStyleColorName(s) << " = " << r);
			CHECK(r >= kMinTextContrast);
		}
}

TEST_CASE("theme contrast: labels read on the theme's own highlights")
{
	ImGuiCtx ctx;
	HE::Ed::applyHorizonDarkTheme();
	using namespace HE::Ed::Theme;
	const ImVec4* c = ImGui::GetStyle().Colors;

	// Selected and pressed rows, buttons and tabs, each over the surfaces they
	// are drawn on. ButtonActive and HeaderActive used to be the accent at
	// 0.80/0.85, 3.4:1 and 3.3:1 under Text; several panels use ButtonActive as
	// the fill of a toggle that is on.
	const int fills[] = { ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive,
	                      ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive,
	                      ImGuiCol_Tab, ImGuiCol_TabHovered, ImGuiCol_TabSelected,
	                      ImGuiCol_TabDimmedSelected, ImGuiCol_TextSelectedBg,
	                      ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive };
	const int unders[] = { ImGuiCol_WindowBg, ImGuiCol_PopupBg, ImGuiCol_ChildBg };
	for (int f : fills)
		for (int u : unders)
		{
			const float r = contrastRatio(c[ImGuiCol_Text], over(c[f], c[u]));
			INFO("Text on " << ImGui::GetStyleColorName(f) << " over "
			     << ImGui::GetStyleColorName(u) << " = " << r);
			CHECK(r >= kMinTextContrast);
		}
}

TEST_CASE("theme contrast: filled buttons and lit toggles pick a label that reads")
{
	using namespace HE::Ed::Theme;
	const ImVec4 white(1, 1, 1, 1);

	// EditorWidgets::primaryButton (EditorRowWidgets.cpp): one label for rest,
	// hover and pressed, chosen from the rest fill. White was 3.2:1 here.
	const ImVec4 primary[] = { Accent, AccentHi, mix(warm(0.10f), Accent, 0.90f) };
	const ImVec4 primaryLabel = textOn(primary[0], white);
	for (const ImVec4& f : primary)
	{
		INFO("primary fill " << f.x << "," << f.y << "," << f.z);
		CHECK(contrastRatio(primaryLabel, f) >= kMinTextContrast);
	}
	// dangerButton keeps white.
	const ImVec4 danger[] = { ImVec4(0.60f, 0.20f, 0.20f, 1.0f), ImVec4(0.72f, 0.26f, 0.26f, 1.0f),
	                          ImVec4(0.52f, 0.16f, 0.16f, 1.0f) };
	const ImVec4 dangerLabel = textOn(danger[0], white);
	CHECK(dangerLabel.x == white.x);
	for (const ImVec4& f : danger) CHECK(contrastRatio(dangerLabel, f) >= kMinTextContrast);
	// dangerSmallButton: a red label on a red wash, over the lightest surface.
	const ImVec4 dangerSmallLabel(0.95f, 0.48f, 0.45f, 1.0f);
	const ImVec4 dangerSmall[] = { ImVec4(0.60f, 0.20f, 0.20f, 0.20f), ImVec4(0.66f, 0.23f, 0.23f, 0.32f),
	                               ImVec4(0.52f, 0.16f, 0.16f, 0.50f) };
	for (const ImVec4& f : dangerSmall)
		CHECK(contrastRatio(dangerSmallLabel, over(f, warm(0.13f))) >= kMinTextContrast);

	// The lit M/S/EQ of the Audio Mixer and the armed Bind of the Input asset:
	// the full accent, hovered AccentHi, under textOn(Accent).
	CHECK(contrastRatio(textOn(Accent), Accent)   >= kMinTextContrast);
	CHECK(contrastRatio(textOn(Accent), AccentHi) >= kMinTextContrast);
}

TEST_CASE("theme contrast: the toolbar palette's text reads on its own cells")
{
	namespace T = EditorToolbar;
	const ImVec4 bar = fromU32(T::kBarBg), well = fromU32(T::kWellBg);
	CHECK(contrastRatio(fromU32(T::kFg),    well) >= kMinTextContrast);
	CHECK(contrastRatio(fromU32(T::kFgDim), well) >= kMinTextContrast);   // readouts
	CHECK(contrastRatio(fromU32(T::kFgDim), bar)  >= kMinTextContrast);
	CHECK(contrastRatio(fromU32(T::kFgOn),  fromU32(T::kOnBg))      >= kMinTextContrast);
	CHECK(contrastRatio(fromU32(T::kFgOn),  fromU32(T::kOnBgHover)) >= kMinTextContrast);
	// The mirror really mirrors.
	CHECK(contrastRatio(fromU32(T::kFgDim), HE::Ed::Theme::TextDim) == doctest::Approx(1.0f).epsilon(0.02));
}

TEST_CASE("theme contrast: a tinted status row keeps its text readable")
{
	// The Console's warning / error / critical rows and the repeat-count pill on
	// them (ConsoleView.cpp), and the Tool Setup row a banner points at
	// (ToolSetupDialog.cpp). Those draw their text through readableOn() against
	// the ground composited here; this is the same arithmetic, so the floor
	// cannot be met by the test and missed by the panel.
	using namespace HE::Ed::Theme;
	namespace T = EditorToolbar;
	const ImVec4 window = warm(0.105f);
	const ImVec4 grounds[] = {
		over(fromU32(T::kWarnWell), window),
		over(fromU32(T::kBadWell), window),
		over(fromU32((T::kBad & ~IM_COL32_A_MASK) | (76u << IM_COL32_A_SHIFT)), window),
	};
	for (const ImVec4& g : grounds)
		for (ImU32 level : { T::kWarn, T::kBad, u32(TextDim), u32(Text) })
		{
			const ImVec4 fg = readableOn(fromU32(level), g);
			CHECK(contrastRatio(fg, g) >= kMinTextContrast);
			const ImVec4 pill = over(fromU32((level & ~IM_COL32_A_MASK) | (56u << IM_COL32_A_SHIFT)), g);
			CHECK(contrastRatio(readableOn(fromU32(level), pill), pill) >= kMinTextContrast);
		}

	// The flash itself must not be the accent behind the text any more: the
	// reported line was amber at 4.7:1 on amber, which passes a ratio and still
	// is not readable. Its lift is neutral, and Warn keeps a wide margin on it.
	const ImVec4 popup = warm(0.13f);
	const ImVec4 flash = over(alpha(Ivory, 0.08f), popup);
	CHECK(contrastRatio(Warn, flash) >= 6.0f);
	CHECK(contrastRatio(readableOn(TextDim, flash), flash) >= kMinTextContrast);
}
