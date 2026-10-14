#include "doctest.h"

#include "EditorGraphics.h"

#include <cstring>

// ── The graphics half of the config, and the project's copy ──────────────────
// The list of fields is one X-macro (ProjectSettings.h); the editor's config
// members are named in it. What can go wrong is a field that is in the project
// block and not carried across, or carried the wrong way — and the first sign
// would be a bloom setting that quietly does nothing. So the whole list is driven
// through apply/capture, not a handful picked by hand.

TEST_CASE("EditorGraphics: a project block and the config defaults agree, field by field")
{
	// Same defaults in both places, or a project that turns graphics on without
	// touching a value would change the picture.
	EditorConfig cfg;
	HE::ProjectGraphics g;
	EditorGraphics::capture(cfg, g);
	CHECK(HE::ProjectGraphics{}.bloomIntensity == doctest::Approx(g.bloomIntensity));
	EditorConfig back;
	EditorGraphics::apply(HE::ProjectGraphics{}, back);
	CHECK(EditorGraphics::same(cfg, back));
}

TEST_CASE("EditorGraphics: apply and capture carry every field, in both directions")
{
	HE::ProjectGraphics g;
	// Every field moved off its default, found through the macro itself.
	int moved = 0;
#define HE_X(T, name, def, editorName, lo, hi) \
	g.name = (std::is_same_v<T, bool>) ? static_cast<T>(!def) \
	       : (std::is_same_v<T, int>)  ? static_cast<T>(hi) \
	                                   : static_cast<T>(static_cast<double>(def) == hi ? lo : hi); ++moved;
	HE_PROJECT_GRAPHICS_FIELDS(HE_X)
#undef HE_X
	CHECK(moved > 30);

	EditorConfig cfg;
	EditorGraphics::apply(g, cfg);
	CHECK_FALSE(EditorGraphics::same(cfg, EditorConfig{}));

	HE::ProjectGraphics round;
	EditorGraphics::capture(cfg, round);
#define HE_X(T, name, def, editorName, lo, hi) CHECK(round.name == g.name);
	HE_PROJECT_GRAPHICS_FIELDS(HE_X)
#undef HE_X

	// And the one-field cases that would be easiest to get wrong: names that
	// differ between the two sides.
	HE::ProjectGraphics one;
	one.giEnabled = true;
	one.giReflBounces = 4;
	EditorConfig c2;
	EditorGraphics::apply(one, c2);
	CHECK(c2.GlobalIlluminationEnabled);
	CHECK(c2.GIReflBounces == 4);
}

TEST_CASE("EditorGraphics: apply and capture leave everything that is not graphics alone")
{
	EditorConfig cfg;
	cfg.UiFontScale        = 1.7f;
	cfg.MaxFps             = 90.0f;
	cfg.PreviewQualityMode = 1;
	cfg.QuickSettingsFavorites = "gi,bloom";

	HE::ProjectGraphics g;
	g.bloomEnabled = false;
	g.enabled      = true;
	EditorGraphics::apply(g, cfg);
	CHECK_FALSE(cfg.BloomEnabled);
	CHECK(cfg.UiFontScale == doctest::Approx(1.7f));
	CHECK(cfg.MaxFps == doctest::Approx(90.0f));
	CHECK(cfg.PreviewQualityMode == 1);
	CHECK(cfg.QuickSettingsFavorites == "gi,bloom");

	// capture keeps the project's own switch.
	HE::ProjectGraphics target;
	target.enabled = true;
	EditorConfig other;
	EditorGraphics::capture(other, target);
	CHECK(target.enabled);
	target.enabled = false;
	EditorGraphics::capture(other, target);
	CHECK_FALSE(target.enabled);
}

TEST_CASE("EditorGraphics: copyFields keeps the user's values apart from the project's")
{
	EditorConfig user;
	user.BloomEnabled   = false;
	user.RenderScale    = 0.6f;
	user.AntiAliasing   = 0;
	EditorConfig live = user;

	HE::ProjectGraphics project;
	project.bloomEnabled = true;
	project.renderScale  = 1.5f;
	project.antiAliasing = 3;

	EditorConfig saved;
	EditorGraphics::copyFields(live, saved);       // the editor sets the user's values aside
	EditorGraphics::apply(project, live);          // the project's go on screen
	CHECK(live.BloomEnabled);
	CHECK(live.RenderScale == doctest::Approx(1.5f));
	EditorGraphics::copyFields(saved, live);       // and back
	CHECK(EditorGraphics::same(live, user));
}

TEST_CASE("EditorGraphics: the row keys name each graphics row once, and only graphics rows")
{
	const auto contains = [](EditorGraphics::RowSet s, const char* key)
	{
		for (int i = 0; i < s.count; ++i) if (std::strcmp(s.keys[i], key) == 0) return true;
		return false;
	};
	CHECK(contains(EditorGraphics::qualityRows(), "aa"));
	CHECK(contains(EditorGraphics::qualityRows(), "renderpath"));
	CHECK(contains(EditorGraphics::postProcessingRows(), "bloom"));
	CHECK(contains(EditorGraphics::reflectionsAndGiRows(), "girefl"));
	// A row is on one page only.
	for (const char* k : { "aa", "renderpath", "occlusion", "gpuparticles" })
		CHECK_FALSE(contains(EditorGraphics::postProcessingRows(), k));
	CHECK(EditorGraphics::isGraphicsRow("ssr"));
	CHECK_FALSE(EditorGraphics::isGraphicsRow("vsync"));
	CHECK_FALSE(EditorGraphics::isGraphicsRow("backend"));
	CHECK_FALSE(EditorGraphics::isGraphicsRow("weathersound"));
	CHECK_FALSE(EditorGraphics::isGraphicsRow(nullptr));
}
