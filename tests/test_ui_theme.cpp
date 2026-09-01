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

TEST_CASE("Theme: sizes and text levels bind like colours do")
{
    HE::UIWidgetTree tree;
    const int text  = tree.add(HE::UIWidgetType::Text);
    const int panel = tree.add(HE::UIWidgetType::Panel);

    // One map carries all three kinds. WHAT a binding means is decided by the
    // property's type — and by the property itself for the two Float
    // vocabularies, which both contain "Small".
    tree.find(text)->setThemeRole("FontSize", "Heading");
    tree.find(panel)->setThemeRole("Corner Radius", "Large");

    HE::UITheme t = HE::uiDefaultTheme();
    t.textSize[static_cast<int>(HE::UIThemeTextLevel::Heading)] = 26.0f;
    t.radius[static_cast<int>(HE::UIThemeSize::Large)] = 19.0f;

    CHECK(HE::uiApplyTheme(tree, t, HE::UIThemeMode::Dark) == 2);
    CHECK(tree.find(text)->getPropAny("FontSize").f == doctest::Approx(26.0f));
    CHECK(tree.find(panel)->cornerRadius.x == doctest::Approx(19.0f));
    CHECK(tree.find(panel)->cornerRadius.z == doctest::Approx(19.0f));  // all four

    // The collision that makes "decide by the property" necessary: "Small" is a
    // size step AND a text level, and each side has to read its own.
    tree.find(text)->setThemeRole("FontSize", "Small");
    tree.find(panel)->setThemeRole("Corner Radius", "Small");
    t.textSize[static_cast<int>(HE::UIThemeTextLevel::Small)] = 11.0f;
    t.radius[static_cast<int>(HE::UIThemeSize::Small)] = 3.0f;
    CHECK(HE::uiApplyTheme(tree, t, HE::UIThemeMode::Dark) == 2);
    CHECK(tree.find(text)->getPropAny("FontSize").f == doctest::Approx(11.0f));
    CHECK(tree.find(panel)->cornerRadius.x == doctest::Approx(3.0f));

    // A name from the WRONG vocabulary resolves in neither, so the value is left
    // alone — the same "visible and fixable" rule a renamed colour role gets.
    const float keptSize = tree.find(text)->getPropAny("FontSize").f;
    tree.find(text)->setThemeRole("FontSize", "Medium");   // a size step, not a level
    CHECK(HE::uiApplyTheme(tree, t, HE::UIThemeMode::Dark) == 1);   // only the panel
    CHECK(tree.find(text)->getPropAny("FontSize").f == doctest::Approx(keptSize));

    // …and all three kinds survive a save together.
    HE::UIWidgetTree loaded;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(tree), loaded));
    CHECK(loaded.find(text)->themeRoleFor("FontSize") == "Medium");
    CHECK(loaded.find(panel)->themeRoleFor("Corner Radius") == "Small");
}

// ═══ Styles ══════════════════════════════════════════════════════════════════
// A role is one value and binding to it is one decision PER VALUE. A style is
// the answer for a whole KIND of element at once — and the only way a hover or a
// pressed colour can be themed at all, since no role vocabulary has a name for
// "the accent, but hovered".

namespace
{
    // A colour entry that is the same in both modes, for the tests that are not
    // about light and dark.
    HE::UIThemeStyleValue col(const glm::vec4& c)
    {
        HE::UIThemeStyleValue v;
        v.color[static_cast<int>(HE::UIThemeMode::Light)] = c;
        v.color[static_cast<int>(HE::UIThemeMode::Dark)]  = c;
        return v;
    }
    HE::UIThemeStyleValue num(float f)
    {
        HE::UIThemeStyleValue v;
        v.isColor = false;
        v.number  = f;
        return v;
    }
}

TEST_CASE("Theme: one style dresses a whole element type, hover and pressed included")
{
    HE::UIWidgetTree tree;
    const int button = tree.add(HE::UIWidgetType::Button);
    const int panel  = tree.add(HE::UIWidgetType::Panel);

    const glm::vec4 normal { 0.10f, 0.20f, 0.30f, 1.0f };
    const glm::vec4 hover  { 0.20f, 0.40f, 0.60f, 1.0f };
    const glm::vec4 press  { 0.05f, 0.10f, 0.15f, 1.0f };

    HE::UITheme t;
    HE::UIThemeStyle& s = t.styleMut("Button");
    s.set("Normal Color",  col(normal));
    s.set("Hovered Color", col(hover));
    s.set("Pressed Color", col(press));
    s.set("Corner Radius", num(11.0f));

    const glm::vec4 panelBefore = tree.find(panel)->getPropAny("Color").col;

    // Four values, one decision, and NOT ONE of them was bound by hand: the
    // element only says "I am a Button" (themeStyled, on by default for anything
    // constructed rather than read from a file).
    REQUIRE(tree.find(button)->themeStyled);
    REQUIRE(tree.find(button)->themeStyle.empty());
    CHECK(HE::uiApplyTheme(tree, t, HE::UIThemeMode::Light) == 4);
    CHECK(tree.find(button)->getPropAny("Normal Color").col  == normal);
    CHECK(tree.find(button)->getPropAny("Hovered Color").col == hover);
    CHECK(tree.find(button)->getPropAny("Pressed Color").col == press);
    CHECK(tree.find(button)->cornerRadius.x == doctest::Approx(11.0f));

    // The Panel is not a Button, so nothing about it moved.
    CHECK(tree.find(panel)->getPropAny("Color").col == panelBefore);

    // Switching it off leaves the element exactly where the last apply left it —
    // a style is a subscription, not a bond.
    tree.find(button)->themeStyled = false;
    s.set("Normal Color", col(glm::vec4(1.0f, 0.0f, 1.0f, 1.0f)));
    CHECK(HE::uiApplyTheme(tree, t, HE::UIThemeMode::Light) == 0);
    CHECK(tree.find(button)->getPropAny("Normal Color").col == normal);
}

TEST_CASE("Theme: a style carries light and dark, like a role does")
{
    HE::UIWidgetTree tree;
    const int button = tree.add(HE::UIWidgetType::Button);

    HE::UIThemeStyleValue v;
    v.color[static_cast<int>(HE::UIThemeMode::Light)] = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f);
    v.color[static_cast<int>(HE::UIThemeMode::Dark)]  = glm::vec4(0.1f, 0.1f, 0.1f, 1.0f);

    HE::UITheme t;
    t.styleMut("Button").set("Normal Color", v);
    // A number is one value: a corner radius is not lighter in light mode.
    t.styleMut("Button").set("Corner Radius", num(7.0f));

    HE::uiApplyTheme(tree, t, HE::UIThemeMode::Light);
    CHECK(tree.find(button)->getPropAny("Normal Color").col.r == doctest::Approx(0.9f));
    CHECK(tree.find(button)->cornerRadius.x == doctest::Approx(7.0f));
    HE::uiApplyTheme(tree, t, HE::UIThemeMode::Dark);
    CHECK(tree.find(button)->getPropAny("Normal Color").col.r == doctest::Approx(0.1f));
    CHECK(tree.find(button)->cornerRadius.x == doctest::Approx(7.0f));
}

TEST_CASE("Theme: a named style covers whatever points at it, including a component's parts")
{
    HE::UIWidgetTree tree;
    const int panel  = tree.add(HE::UIWidgetType::Panel);
    const int button = tree.add(HE::UIWidgetType::Button);

    const glm::vec4 card{ 0.15f, 0.15f, 0.18f, 1.0f };
    HE::UITheme t;
    // One style, two types, and a property only one of them has: a style is a
    // bag of property names, so what does not apply is simply skipped. That is
    // what lets a project name a look ("Card", "Danger") instead of being
    // limited to the built-in type names.
    HE::UIThemeStyle& s = t.styleMut("Card");
    s.set("Color", col(card));            // the Panel has this one
    s.set("Normal Color", col(card));     // the Button has this one
    s.set("Corner Radius", num(14.0f));   // both

    tree.find(panel)->themeStyle  = "Card";
    tree.find(button)->themeStyle = "Card";
    CHECK(HE::uiApplyTheme(tree, t, HE::UIThemeMode::Dark) == 4);
    CHECK(tree.find(panel)->getPropAny("Color").col == card);
    CHECK(tree.find(button)->getPropAny("Normal Color").col == card);
    CHECK(tree.find(panel)->cornerRadius.x == doctest::Approx(14.0f));
    CHECK(tree.find(button)->cornerRadius.x == doctest::Approx(14.0f));

    // A name that no longer resolves — the style was renamed or deleted — leaves
    // the element alone rather than falling back to its type's style. Same rule
    // a renamed role gets: visible and fixable beats quietly something else.
    tree.find(panel)->themeStyle = "Crd";
    t.styleMut("Panel").set("Color", col(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)));
    HE::uiApplyTheme(tree, t, HE::UIThemeMode::Dark);
    CHECK(tree.find(panel)->getPropAny("Color").col == card);
}

TEST_CASE("Theme: a binding beats the style, a lock beats both")
{
    HE::UIWidgetTree tree;
    const int button = tree.add(HE::UIWidgetType::Button);

    HE::UITheme t;
    const glm::vec4 styled{ 0.2f, 0.2f, 0.2f, 1.0f };
    t.styleMut("Button").set("Normal Color",  col(styled));
    t.styleMut("Button").set("Hovered Color", col(styled));
    t.styleMut("Button").set("Pressed Color", col(styled));

    // A role binding is a deliberate exception to the style — "these buttons,
    // but this one is the accent".
    tree.find(button)->setThemeRole("Normal Color", "Accent");
    // A lock is "somebody already decided this", which is what a component
    // parameter writes.
    const glm::vec4 mine{ 1.0f, 0.5f, 0.0f, 1.0f };
    tree.find(button)->setPropAny("Pressed Color", HE::UIPropValue::ofColor(mine));
    tree.find(button)->setThemeRole("Pressed Color", HE::kUIThemeLiteral);

    HE::uiApplyTheme(tree, t, HE::UIThemeMode::Dark);
    CHECK(tree.find(button)->getPropAny("Normal Color").col ==
          t.colorFor(HE::UIThemeRole::Accent, HE::UIThemeMode::Dark));
    CHECK(tree.find(button)->getPropAny("Hovered Color").col == styled);
    CHECK(tree.find(button)->getPropAny("Pressed Color").col == mine);

    // The same three answers, asked instead of written. This is what the
    // designer draws with, and it has to agree with what the runtime assigns —
    // one function, so it cannot do otherwise.
    HE::UIPropValue v;
    const HE::UIElement& e = *tree.find(button);
    CHECK(HE::uiThemeValueFor(e, t, HE::UIThemeMode::Dark, "Normal Color", v));
    CHECK(v.col == t.colorFor(HE::UIThemeRole::Accent, HE::UIThemeMode::Dark));
    CHECK(HE::uiThemeValueFor(e, t, HE::UIThemeMode::Dark, "Hovered Color", v));
    CHECK(v.col == styled);
    CHECK_FALSE(HE::uiThemeValueFor(e, t, HE::UIThemeMode::Dark, "Pressed Color", v));
}

TEST_CASE("Theme: styles round-trip, and a theme without any saves as before")
{
    HE::UITheme plain;
    CHECK(HE::uiThemeToJson(plain).find("styles") == std::string::npos);

    HE::UITheme t;
    t.name = "Night";
    HE::UIThemeStyleValue v;
    v.color[static_cast<int>(HE::UIThemeMode::Light)] = glm::vec4(0.7f, 0.6f, 0.5f, 1.0f);
    v.color[static_cast<int>(HE::UIThemeMode::Dark)]  = glm::vec4(0.2f, 0.3f, 0.4f, 0.5f);
    t.styleMut("Button").set("Normal Color", v);
    t.styleMut("Button").set("Corner Radius", num(9.5f));
    t.styleMut("Card").set("Color", col(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)));

    HE::UITheme back;
    REQUIRE(HE::uiThemeFromJson(HE::uiThemeToJson(t), back));
    REQUIRE(back.styles.size() == 2);
    // Order is kept, both levels: an author's list must not be reshuffled by a
    // save, which a JSON object keyed by name would do.
    CHECK(back.styles[0].first == "Button");
    CHECK(back.styles[1].first == "Card");
    REQUIRE(back.styleFor("Button"));
    REQUIRE(back.styleFor("Button")->values.size() == 2);
    CHECK(back.styleFor("Button")->values[0].first == "Normal Color");
    const HE::UIThemeStyleValue* n = back.styleFor("Button")->find("Normal Color");
    REQUIRE(n);
    CHECK(n->isColor);
    CHECK(n->color[static_cast<int>(HE::UIThemeMode::Dark)].a == doctest::Approx(0.5f));
    const HE::UIThemeStyleValue* r = back.styleFor("Button")->find("Corner Radius");
    REQUIRE(r);
    CHECK_FALSE(r->isColor);
    CHECK(r->number == doctest::Approx(9.5f));
    CHECK(back.styleFor("Nothing") == nullptr);
}

// The end-to-end one. Fields changing is not proof: this asks what comes out of
// the extractor, which is what the backends draw — and it goes through the
// runtime's own path (createWidget applies the theme, setTheme re-applies it).
TEST_CASE("Theme: a style reaches the screen, and a second theme redresses it")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree tree;
    tree.canvasWidth = 200.0f; tree.canvasHeight = 100.0f;
    tree.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int button = tree.add(HE::UIWidgetType::Button);
    {
        HE::UIElement& e = *tree.find(button);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 200.0f; e.sizeY = 100.0f;
        // Nothing is bound by hand. The element only says "I am a Button".
        e.themeStyled = true;
        // A deliberately wrong literal: if the style is not applied, THIS is what
        // the test would see, so the assertion cannot pass by accident.
        e.setProp("Normal Color", HE::UIPropValue::ofColor({ 1.0f, 0.0f, 1.0f, 1.0f }));
    }
    UIWidgetAsset a;
    a.treeJson = HE::uiWidgetTreeToJson(tree);
    a.path     = "mem://styled.hasset";
    cm.registerWidget(std::move(a));

    const glm::vec4 light{ 0.80f, 0.80f, 0.85f, 1.0f };
    const glm::vec4 dark { 0.15f, 0.16f, 0.20f, 1.0f };
    HE::UITheme t;
    {
        HE::UIThemeStyleValue v;
        v.color[static_cast<int>(HE::UIThemeMode::Light)] = light;
        v.color[static_cast<int>(HE::UIThemeMode::Dark)]  = dark;
        t.styleMut("Button").set("Normal Color", v);
    }

    WidgetManager wm;
    wm.setTheme(t);
    wm.setThemePreference(HE::UIThemePreference::Light);
    const int id = wm.createWidget(cm, "mem://styled.hasset");
    REQUIRE(id != 0);
    wm.showWidget(id);

    auto surfaceColor = [&]
    {
        std::vector<UIRenderObject> out;
        wm.extract(200.0f, 100.0f, out);
        REQUIRE_FALSE(out.empty());
        return out[0].color;
    };
    CHECK(surfaceColor() == light);
    wm.setThemePreference(HE::UIThemePreference::Dark);
    CHECK(surfaceColor() == dark);

    // A second theme with the same style key: the application looks different
    // without a single widget being edited, and without anything being bound.
    const glm::vec4 other{ 0.60f, 0.30f, 0.10f, 1.0f };
    HE::UITheme t2;
    {
        HE::UIThemeStyleValue v;
        v.color[static_cast<int>(HE::UIThemeMode::Light)] = other;
        v.color[static_cast<int>(HE::UIThemeMode::Dark)]  = other;
        t2.styleMut("Button").set("Normal Color", v);
    }
    wm.setTheme(t2);
    CHECK(surfaceColor() == other);
}

TEST_CASE("Theme: a widget authored before styles keeps the colours somebody typed")
{
    // The whole compatibility question in one test. An element READ from a file
    // that does not mention styles must not be repainted by opening it; one that
    // is CONSTRUCTED follows the theme, because that is what "place a button and
    // it looks like the project's buttons" means.
    HE::UIWidgetTree fresh;
    const int b = fresh.add(HE::UIWidgetType::Button);
    CHECK(fresh.find(b)->themeStyled);

    const std::string old =
        R"({"canvasWidth":1920,"canvasHeight":1080,"elements":[)"
        R"({"id":1,"parent":0,"type":"Button","name":"Old"}]})";
    HE::UIWidgetTree loaded;
    REQUIRE(HE::uiWidgetTreeFromJson(old, loaded));
    REQUIRE(loaded.find(1));
    CHECK_FALSE(loaded.find(1)->themeStyled);

    HE::UITheme t;
    t.styleMut("Button").set("Normal Color", col(glm::vec4(1.0f, 0.0f, 1.0f, 1.0f)));
    const glm::vec4 authored = loaded.find(1)->getPropAny("Normal Color").col;
    CHECK(HE::uiApplyTheme(loaded, t, HE::UIThemeMode::Dark) == 0);
    CHECK(loaded.find(1)->getPropAny("Normal Color").col == authored);

    // And it survives a save either way round: off writes nothing at all (so an
    // old widget stays byte-identical), on writes the key.
    CHECK(HE::uiWidgetTreeToJson(loaded).find("themeStyled") == std::string::npos);
    loaded.find(1)->themeStyled = true;
    loaded.find(1)->themeStyle  = "Card";
    HE::UIWidgetTree again;
    REQUIRE(HE::uiWidgetTreeFromJson(HE::uiWidgetTreeToJson(loaded), again));
    CHECK(again.find(1)->themeStyled);
    CHECK(again.find(1)->themeStyle == "Card");
}
