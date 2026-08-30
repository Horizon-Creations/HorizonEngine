#include "doctest.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/WidgetManager.h>
#include <Hpak/ProjectConfig.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/UITheme.h>
#include <UIWidget/UIWidgetTree.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ═══ The theme ═══════════════════════════════════════════════════════════════
// docs/he-apps-plan.md D1. Without it an author who builds ten buttons types the
// same corner radius and the same border colour ten times, and changes them ten
// times later. What this file guards is the two things that make a theme worth
// having: the role NAMES are stable (they are an on-disk format, stored by every
// element bound to one), and binding actually changes what gets drawn.


namespace
{
    // Same shape as the other widget tests: a scratch content root that goes
    // away with the test.
    struct TempWidgetDir
    {
        std::filesystem::path path;
        TempWidgetDir()
        {
            path = std::filesystem::temp_directory_path() / "he_test_uitheme";
            std::filesystem::remove_all(path);
            std::filesystem::create_directories(path);
        }
        ~TempWidgetDir() { std::filesystem::remove_all(path); }
    };
}

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

// ── Through the runtime, end to end ──────────────────────────────────────────
// A theme that only changes fields is a theme that could be drawing nothing.
// This one asks what comes out of the extractor, which is what the backends get.
TEST_CASE("Theme: switching the mode changes what gets drawn")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree tree;
    tree.canvasWidth = 200.0f; tree.canvasHeight = 100.0f;
    tree.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int panel = tree.add(HE::UIWidgetType::Panel);
    {
        HE::UIElement& e = *tree.find(panel);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 100.0f;
        e.setThemeRole("Color", "Surface");
        // A deliberately wrong literal: if the theme is not applied, THIS is what
        // the test would see, so the assertion cannot pass by accident.
        e.setProp("Color", HE::UIPropValue::ofColor({ 1.0f, 0.0f, 1.0f, 1.0f }));
    }
    UIWidgetAsset a;
    a.treeJson = HE::uiWidgetTreeToJson(tree);
    a.path     = "mem://themed.hasset";
    cm.registerWidget(std::move(a));

    WidgetManager wm;
    const int id = wm.createWidget(cm, "mem://themed.hasset");
    REQUIRE(id != 0);
    wm.showWidget(id);

    auto surfaceColor = [&]
    {
        std::vector<UIRenderObject> out;
        wm.extract(200.0f, 100.0f, out);
        REQUIRE_FALSE(out.empty());
        return out[0].color;
    };

    const HE::UITheme& t = HE::uiDefaultTheme();
    // Creating it already resolved the role — the magenta literal never reached
    // the screen.
    wm.setThemePreference(HE::UIThemePreference::Light);
    CHECK(surfaceColor() == t.colorFor(HE::UIThemeRole::Surface, HE::UIThemeMode::Light));

    wm.setThemePreference(HE::UIThemePreference::Dark);
    CHECK(surfaceColor() == t.colorFor(HE::UIThemeRole::Surface, HE::UIThemeMode::Dark));

    // Another theme, same roles: the application looks different without a
    // single widget being edited.
    wm.setTheme(HE::uiAmberTheme());
    CHECK(surfaceColor() == HE::uiAmberTheme().colorFor(HE::UIThemeRole::Surface,
                                                        HE::UIThemeMode::Dark));

    // Switching is a change to the picture, so it has to ask for a redraw — an
    // event-driven application would otherwise keep showing the old colours
    // until the mouse happened to move.
    wm.consumeVisualDirty();
    wm.setThemePreference(HE::UIThemePreference::Light);
    CHECK(wm.consumeVisualDirty());
}

TEST_CASE("Theme: \"follow the system\" is a rule, not a colour")
{
    WidgetManager wm;
    // Default: follow the desktop. Dark until the host says otherwise — a tool
    // that flashes white on a dark desktop for one frame is the thing this
    // setting exists to avoid.
    CHECK(wm.themePreference() == HE::UIThemePreference::System);
    CHECK(wm.themeMode() == HE::UIThemeMode::Dark);

    wm.setSystemThemeMode(HE::UIThemeMode::Light);
    CHECK(wm.themeMode() == HE::UIThemeMode::Light);   // followed

    // A fixed preference overrides the desktop, and keeps overriding it.
    wm.setThemePreference(HE::UIThemePreference::Dark);
    CHECK(wm.themeMode() == HE::UIThemeMode::Dark);
    wm.setSystemThemeMode(HE::UIThemeMode::Light);
    CHECK(wm.themeMode() == HE::UIThemeMode::Dark);
    // …and what was ASKED for is still what is remembered, so a Preferences
    // screen shows the choice rather than today's weather.
    CHECK(wm.themePreference() == HE::UIThemePreference::Dark);

    // Back to following, and it picks the desktop up again straight away.
    wm.setThemePreference(HE::UIThemePreference::System);
    CHECK(wm.themeMode() == HE::UIThemeMode::Light);

    // A desktop change that the preference hides is not a reason to redraw: an
    // event-driven application must not wake up for a colour nobody sees.
    wm.setThemePreference(HE::UIThemePreference::Dark);
    wm.consumeVisualDirty();
    wm.setSystemThemeMode(HE::UIThemeMode::Light);
    CHECK_FALSE(wm.consumeVisualDirty());

    // Names, both ways, including the ones that are not words we know.
    CHECK(std::string(HE::uiThemePreferenceName(HE::UIThemePreference::System)) == "System");
    CHECK(HE::uiThemePreferenceFromName("Light") == HE::UIThemePreference::Light);
    CHECK(HE::uiThemePreferenceFromName("")      == HE::UIThemePreference::System);
    CHECK(HE::uiThemePreferenceFromName("Sepia") == HE::UIThemePreference::System);
}

TEST_CASE("Theme: the project's choice survives project.hcfg")
{
    const auto dir = std::filesystem::temp_directory_path() / "he_theme_hcfg";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    ProjectConfig cfg;
    cfg.projectName = "Themed";
    cfg.theme       = "UI/Night.hasset";
    cfg.themeMode   = "Light";
    REQUIRE(ProjectConfigLoader::save(dir, cfg));
    ProjectConfig back;
    REQUIRE(ProjectConfigLoader::load(dir, back));
    CHECK(back.theme     == "UI/Night.hasset");
    CHECK(back.themeMode == "Light");

    // A project that chose nothing keeps emitting the OLD format, so a
    // user-dropped prebuilt runtime that predates themes still reads it.
    ProjectConfig plain;
    plain.projectName = "Plain";
    REQUIRE(ProjectConfigLoader::save(dir, plain));
    ProjectConfig plainBack;
    REQUIRE(ProjectConfigLoader::load(dir, plainBack));
    CHECK(plainBack.theme.empty());
    CHECK(plainBack.themeMode.empty());
    {
        std::ifstream f(dir / "project.hcfg", std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
        REQUIRE(bytes.size() > 6);
        uint16_t version = 0;
        std::memcpy(&version, bytes.data() + 4, sizeof(version));
        CHECK(version == 2);        // still the version every runtime knows
    }
    std::filesystem::remove_all(dir);
}
