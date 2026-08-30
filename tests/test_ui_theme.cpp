#include "doctest.h"

#include <UIWidget/UIElements.h>
#include <UIWidget/UITheme.h>
#include <UIWidget/UIWidgetTree.h>

#include <string>
#include <vector>

// ═══ The theme ═══════════════════════════════════════════════════════════════
// docs/he-apps-plan.md D1. Without it an author who builds ten buttons types the
// same corner radius and the same border colour ten times, and changes them ten
// times later. What this file guards is the two things that make a theme worth
// having: the role NAMES are stable (they are an on-disk format, stored by every
// element bound to one), and binding actually changes what gets drawn.

TEST_CASE("Theme: the role names are pinned")
{
    // ╔══════════════════════════════════════════════════════════════════════╗
    // ║  These strings are an ON-DISK FORMAT. A theme asset stores them and  ║
    // ║  every bound element stores the role's NAME. Renaming one silently   ║
    // ║  unbinds every element that used it — the lookup misses, the element ║
    // ║  keeps whatever literal it had, and nothing fails.                   ║
    // ╚══════════════════════════════════════════════════════════════════════╝
    const std::vector<std::string> kRoles = {
        "Background", "Surface", "Border", "Text", "MutedText",
        "Accent", "Warning", "Error", "Success" };
    REQUIRE(kRoles.size() == static_cast<std::size_t>(HE::UIThemeRole::COUNT));
    for (std::size_t i = 0; i < kRoles.size(); ++i)
    {
        const auto role = static_cast<HE::UIThemeRole>(i);
        CAPTURE(kRoles[i]);
        CHECK(kRoles[i] == HE::uiThemeRoleName(role));
        CHECK(HE::uiThemeRoleFromName(kRoles[i]) == role);
    }
    // Anything else reads as "not bound", which is what every caller checks.
    CHECK(HE::uiThemeRoleFromName("Surfaces") == HE::UIThemeRole::COUNT);
    CHECK(HE::uiThemeRoleFromName("") == HE::UIThemeRole::COUNT);

    CHECK(std::string(HE::uiThemeModeName(HE::UIThemeMode::Light)) == "Light");
    CHECK(std::string(HE::uiThemeModeName(HE::UIThemeMode::Dark))  == "Dark");
}

TEST_CASE("Theme: light and dark are two values of one decision")
{
    const HE::UITheme& t = HE::uiDefaultTheme();
    // Every role differs between the modes — a role that is the same in both is
    // a role somebody forgot, and it shows up as unreadable text on one of them.
    for (int i = 0; i < static_cast<int>(HE::UIThemeRole::COUNT); ++i)
    {
        const auto r = static_cast<HE::UIThemeRole>(i);
        CAPTURE(HE::uiThemeRoleName(r));
        CHECK(t.colorFor(r, HE::UIThemeMode::Light) != t.colorFor(r, HE::UIThemeMode::Dark));
    }
    // …and the two that carry reading text are the way round they claim to be.
    CHECK(t.colorFor(HE::UIThemeRole::Text, HE::UIThemeMode::Light).r <
          t.colorFor(HE::UIThemeRole::Background, HE::UIThemeMode::Light).r);
    CHECK(t.colorFor(HE::UIThemeRole::Text, HE::UIThemeMode::Dark).r >
          t.colorFor(HE::UIThemeRole::Background, HE::UIThemeMode::Dark).r);

    // The amber theme is a curation of the same nine roles, not a second model.
    CHECK(std::string(HE::uiAmberTheme().name) == "Amber");
    CHECK(HE::uiAmberTheme().colorFor(HE::UIThemeRole::Accent, HE::UIThemeMode::Dark) !=
          t.colorFor(HE::UIThemeRole::Accent, HE::UIThemeMode::Dark));
}

TEST_CASE("Theme: it round-trips, and a trimmed file still works")
{
    HE::UITheme t = HE::uiDefaultTheme();
    t.name = "Round Trip";
    t.color[static_cast<int>(HE::UIThemeRole::Accent)]
           [static_cast<int>(HE::UIThemeMode::Dark)] = { 0.1f, 0.2f, 0.3f, 0.9f };
    t.radius[static_cast<int>(HE::UIThemeSize::Large)] = 22.0f;
    t.textSize[static_cast<int>(HE::UIThemeTextLevel::Title)] = 40.0f;
    t.shadow[static_cast<int>(HE::UIThemeElevation::Overlay)].blur = 33.0f;

    HE::UITheme back;
    REQUIRE(HE::uiThemeFromJson(HE::uiThemeToJson(t), back));
    CHECK(back.name == "Round Trip");
    CHECK(back.colorFor(HE::UIThemeRole::Accent, HE::UIThemeMode::Dark).a == doctest::Approx(0.9f));
    CHECK(back.radius[static_cast<int>(HE::UIThemeSize::Large)] == doctest::Approx(22.0f));
    CHECK(back.textSize[static_cast<int>(HE::UIThemeTextLevel::Title)] == doctest::Approx(40.0f));
    CHECK(back.shadow[static_cast<int>(HE::UIThemeElevation::Overlay)].blur == doctest::Approx(33.0f));

    // A file that is missing a role — trimmed by hand, or written before that
    // role existed — falls back to the default's colour for it. Black everywhere
    // would turn a missing key into a broken application.
    HE::UITheme sparse;
    REQUIRE(HE::uiThemeFromJson(R"({"name":"Sparse","colors":{
        "Accent":{"Light":[1,0,0,1],"Dark":[0,1,0,1]}}})", sparse));
    CHECK(sparse.name == "Sparse");
    CHECK(sparse.colorFor(HE::UIThemeRole::Accent, HE::UIThemeMode::Light).r == doctest::Approx(1.0f));
    CHECK(sparse.colorFor(HE::UIThemeRole::Surface, HE::UIThemeMode::Light) ==
          HE::uiDefaultTheme().colorFor(HE::UIThemeRole::Surface, HE::UIThemeMode::Light));

    CHECK_FALSE(HE::uiThemeFromJson("not json at all", sparse));
}

TEST_CASE("Theme: a bound property takes the theme's colour, and unbound ones do not")
{
    HE::UIWidgetTree tree;
    const int panel = tree.add(HE::UIWidgetType::Panel);
    const int text  = tree.add(HE::UIWidgetType::Text);

    // The panel is a Surface; the label is Muted Text. Nothing else is bound.
    tree.find(panel)->setThemeRole("Color", "Surface");
    tree.find(text)->setThemeRole("Color", "MutedText");
    const glm::vec4 authoredBorder = tree.find(panel)->borderColor;

    const HE::UITheme& t = HE::uiDefaultTheme();
    CHECK(HE::uiApplyTheme(tree, t, HE::UIThemeMode::Light) == 2);
    CHECK(tree.find(panel)->getPropAny("Color").col ==
          t.colorFor(HE::UIThemeRole::Surface, HE::UIThemeMode::Light));
    CHECK(tree.find(text)->getPropAny("Color").col ==
          t.colorFor(HE::UIThemeRole::MutedText, HE::UIThemeMode::Light));
    // …and what was NOT bound is exactly what the author left there.
    CHECK(tree.find(panel)->borderColor == authoredBorder);

    // Switching mode re-resolves. That is the whole point: light and dark are
    // one decision, not two sets of widgets.
    CHECK(HE::uiApplyTheme(tree, t, HE::UIThemeMode::Dark) == 2);
    CHECK(tree.find(panel)->getPropAny("Color").col ==
          t.colorFor(HE::UIThemeRole::Surface, HE::UIThemeMode::Dark));

    // A role that no longer exists leaves the property alone rather than
    // painting it white: visible and fixable beats vanished.
    const glm::vec4 kept = tree.find(panel)->getPropAny("Color").col;
    tree.find(panel)->setThemeRole("Color", "Surfaces");   // typo / renamed away
    CHECK(HE::uiApplyTheme(tree, t, HE::UIThemeMode::Light) == 1);   // only the label
    CHECK(tree.find(panel)->getPropAny("Color").col == kept);

    // A binding on something that is not a colour is ignored, not written
    // through the generic setter as a size nobody typed.
    tree.find(panel)->setThemeRole("Corner Radius", "Accent");
    const glm::vec4 radiiBefore = tree.find(panel)->cornerRadius;
    HE::uiApplyTheme(tree, t, HE::UIThemeMode::Light);
    CHECK(tree.find(panel)->cornerRadius == radiiBefore);
}

TEST_CASE("Theme: bindings survive a save, and an unbound widget saves as before")
{
    HE::UIWidgetTree tree;
    const int panel = tree.add(HE::UIWidgetType::Panel);
    // Nothing bound → nothing written. An element that decided its own colours
    // must save byte-identically to before themes existed.
    CHECK(HE::uiWidgetTreeToJson(tree).find("themeRoles") == std::string::npos);

    tree.find(panel)->setThemeRole("Color", "Surface");
    tree.find(panel)->setThemeRole("Border Color", "Border");
    HE::UIWidgetTree loaded;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(tree), loaded));
    CHECK(loaded.find(panel)->themeRoleFor("Color") == "Surface");
    CHECK(loaded.find(panel)->themeRoleFor("Border Color") == "Border");
    CHECK(loaded.find(panel)->themeRoleFor("Tint").empty());

    // Unbinding is passing an empty role, and it really removes the entry.
    loaded.find(panel)->setThemeRole("Color", "");
    CHECK(loaded.find(panel)->themeRoleFor("Color").empty());
    CHECK(loaded.find(panel)->themeRoles.size() == 1);
}
