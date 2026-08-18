#include "doctest.h"
#include <imgui.h>
#include <cmath>
#include <string>

#include "EditorTheme.h"

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
