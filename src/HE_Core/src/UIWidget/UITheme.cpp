#include <UIWidget/UITheme.h>
#include <GraphCommon/GraphJson.h>

#include <nlohmann/json.hpp>

#include <array>

namespace HE
{
namespace
{
    constexpr int kRoles = static_cast<int>(UIThemeRole::COUNT);
    constexpr int kModes = static_cast<int>(UIThemeMode::COUNT);
    constexpr int kSizes = static_cast<int>(UIThemeSize::COUNT);
    constexpr int kLevels = static_cast<int>(UIThemeTextLevel::COUNT);
    constexpr int kElevations = static_cast<int>(UIThemeElevation::COUNT);

    constexpr const char* kRoleNames[kRoles] = {
        "Background", "Surface", "Border", "Text", "MutedText",
        "Accent", "AccentText", "Warning", "Error", "Success" };
    constexpr const char* kModeNames[kModes] = { "Light", "Dark" };
    constexpr const char* kSizeNames[kSizes] = { "Small", "Medium", "Large" };
    constexpr const char* kLevelNames[kLevels] = {
        "Title", "Heading", "Body", "Small", "Mono" };
    constexpr const char* kElevationNames[kElevations] = { "Raised", "Overlay" };

    nlohmann::json toArray(const glm::vec4& c)
    { return nlohmann::json::array({ c.r, c.g, c.b, c.a }); }

    bool readColor(const nlohmann::json& j, glm::vec4& out)
    {
        if (!j.is_array() || j.size() != 4) return false;
        out = { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() };
        return true;
    }
}

const char* uiThemeRoleName(UIThemeRole r)
{
    const int i = static_cast<int>(r);
    return (i >= 0 && i < kRoles) ? kRoleNames[i] : "";
}

UIThemeRole uiThemeRoleFromName(const std::string& s)
{
    for (int i = 0; i < kRoles; ++i)
        if (s == kRoleNames[i]) return static_cast<UIThemeRole>(i);
    return UIThemeRole::COUNT;
}

const char* uiThemeModeName(UIThemeMode m)
{
    const int i = static_cast<int>(m);
    return (i >= 0 && i < kModes) ? kModeNames[i] : "";
}

const char* uiThemePreferenceName(UIThemePreference p)
{
    switch (p)
    {
    case UIThemePreference::Light: return "Light";
    case UIThemePreference::Dark:  return "Dark";
    default:                       return "System";
    }
}

UIThemePreference uiThemePreferenceFromName(const std::string& s)
{
    if (s == "Light") return UIThemePreference::Light;
    if (s == "Dark")  return UIThemePreference::Dark;
    // Anything else — absent, misspelt, a value from a newer build — is
    // "follow the desktop", which is the answer that is never wrong.
    return UIThemePreference::System;
}

const char* uiThemeSizeName(UIThemeSize s)
{
    const int i = static_cast<int>(s);
    return (i >= 0 && i < kSizes) ? kSizeNames[i] : "";
}

const char* uiThemeTextLevelName(UIThemeTextLevel t)
{
    const int i = static_cast<int>(t);
    return (i >= 0 && i < kLevels) ? kLevelNames[i] : "";
}

UIThemeSize uiThemeSizeFromName(const std::string& s)
{
    for (int i = 0; i < kSizes; ++i)
        if (s == kSizeNames[i]) return static_cast<UIThemeSize>(i);
    return UIThemeSize::COUNT;
}

UIThemeTextLevel uiThemeTextLevelFromName(const std::string& s)
{
    for (int i = 0; i < kLevels; ++i)
        if (s == kLevelNames[i]) return static_cast<UIThemeTextLevel>(i);
    return UIThemeTextLevel::COUNT;
}

UIThemeScale uiThemeScaleFor(const std::string& prop)
{
    if (prop == "FontSize") return UIThemeScale::Text;
    // The two names every container agrees on (UIBoxBase, UIGrid, UIWrapBox,
    // UIScrollBox and UIListView all call them this), which is what makes one
    // line here reach all of them.
    if (prop == "Padding" || prop == "Spacing") return UIThemeScale::Spacing;
    return UIThemeScale::Radius;
}

bool uiThemeScaleBindable(const std::string& prop)
{
    return prop == "FontSize" || prop == "Padding" || prop == "Spacing" ||
           prop == "Corner Radius";
}

const char* uiThemeElevationName(UIThemeElevation e)
{
    const int i = static_cast<int>(e);
    return (i >= 0 && i < kElevations) ? kElevationNames[i] : "";
}

// ── Selectors ────────────────────────────────────────────────────────────────
// One dot, and the FIRST one: a tag with a dot in it would otherwise split in
// the middle and match nothing, which is a bug an author cannot see. A type name
// never contains one (the registry's names are single words), so the first dot
// is always the boundary.
std::string uiThemeSelector(const std::string& type, const std::string& tag)
{
    return tag.empty() ? type : type + "." + tag;
}
std::string uiThemeSelectorType(const std::string& selector)
{
    const size_t dot = selector.find('.');
    return dot == std::string::npos ? selector : selector.substr(0, dot);
}
std::string uiThemeSelectorTag(const std::string& selector)
{
    const size_t dot = selector.find('.');
    return dot == std::string::npos ? std::string() : selector.substr(dot + 1);
}

// ── The two shipped themes ───────────────────────────────────────────────────
// Neutral greys with one blue accent, in both modes. Chosen to be boring on
// purpose: a default theme is the thing an author changes, and a default with a
// personality is one they have to undo first.
const UITheme& uiDefaultTheme()
{
    static const UITheme t = []
    {
        UITheme d;
        d.name = "Default";
        auto set = [&](UIThemeRole r, glm::vec4 light, glm::vec4 dark)
        {
            d.color[static_cast<int>(r)][static_cast<int>(UIThemeMode::Light)] = light;
            d.color[static_cast<int>(r)][static_cast<int>(UIThemeMode::Dark)]  = dark;
        };
        set(UIThemeRole::Background, { 0.96f, 0.96f, 0.97f, 1.0f }, { 0.09f, 0.09f, 0.11f, 1.0f });
        set(UIThemeRole::Surface,    { 1.00f, 1.00f, 1.00f, 1.0f }, { 0.15f, 0.15f, 0.18f, 1.0f });
        set(UIThemeRole::Border,     { 0.80f, 0.80f, 0.83f, 1.0f }, { 0.28f, 0.28f, 0.33f, 1.0f });
        set(UIThemeRole::Text,       { 0.10f, 0.10f, 0.12f, 1.0f }, { 0.93f, 0.93f, 0.95f, 1.0f });
        // Light muted text was 0.45 and did not reach WCAG's 4.5:1 on the
        // BACKGROUND role — 4.30, which is the ratio uiCheckContrast reported
        // for five of the shipped components at once (a hint under a form row,
        // a list's subtitle). It clears 4.5 on Surface (white) either way; the
        // page it usually sits on is the darker of the two, and that is the one
        // that decides. 0.43 is the smallest step that passes both.
        set(UIThemeRole::MutedText,  { 0.43f, 0.43f, 0.48f, 1.0f }, { 0.60f, 0.60f, 0.66f, 1.0f });
        set(UIThemeRole::Accent,     { 0.16f, 0.44f, 0.85f, 1.0f }, { 0.36f, 0.60f, 0.96f, 1.0f });
        // The colour to write ON that accent, and the one pair in this palette
        // that goes the OTHER way round from every other: white in light mode,
        // near-black in dark. That is not a slip — it follows from the accent
        // itself. The light theme's accent is a deep blue (luminance 0.17), so
        // white reaches 4.75:1 on it and the page's own near-black manages only
        // 4.01; the dark theme's is a pale blue (0.32), where white falls to
        // 2.87 and near-black climbs to 6.08. A role that flipped with the mode
        // like the others would fail on both.
        //
        // The dark value is the light theme's Text on purpose: the colour to
        // write on a pale blue IS the colour to write on a pale page.
        set(UIThemeRole::AccentText, { 1.00f, 1.00f, 1.00f, 1.0f }, { 0.10f, 0.10f, 0.12f, 1.0f });
        set(UIThemeRole::Warning,    { 0.85f, 0.60f, 0.10f, 1.0f }, { 0.95f, 0.72f, 0.24f, 1.0f });
        set(UIThemeRole::Error,      { 0.80f, 0.22f, 0.20f, 1.0f }, { 0.93f, 0.40f, 0.36f, 1.0f });
        set(UIThemeRole::Success,    { 0.16f, 0.60f, 0.32f, 1.0f }, { 0.34f, 0.78f, 0.48f, 1.0f });
        // A shadow on a light page is a soft grey; on a dark one it is nearly
        // nothing, which is why elevation there is carried by the surface being
        // lighter rather than by the shadow being darker.
        d.shadow[static_cast<int>(UIThemeElevation::Raised)]  = { { 0, 0, 0, 0.18f },  8.0f, 0.0f, 2.0f };
        d.shadow[static_cast<int>(UIThemeElevation::Overlay)] = { { 0, 0, 0, 0.32f }, 24.0f, 0.0f, 8.0f };
        return d;
    }();
    return t;
}

// The editor's own palette, so an application can look like the tool that built
// it. Curated down from EditorTheme's colour slots to the nine roles — the point
// of a closed vocabulary is that this IS a curation and not a copy.
const UITheme& uiAmberTheme()
{
    static const UITheme t = []
    {
        UITheme d = uiDefaultTheme();
        d.name = "Amber";
        auto set = [&](UIThemeRole r, glm::vec4 light, glm::vec4 dark)
        {
            d.color[static_cast<int>(r)][static_cast<int>(UIThemeMode::Light)] = light;
            d.color[static_cast<int>(r)][static_cast<int>(UIThemeMode::Dark)]  = dark;
        };
        set(UIThemeRole::Background, { 0.95f, 0.93f, 0.89f, 1.0f }, { 0.08f, 0.07f, 0.06f, 1.0f });
        set(UIThemeRole::Surface,    { 1.00f, 0.99f, 0.96f, 1.0f }, { 0.14f, 0.13f, 0.11f, 1.0f });
        set(UIThemeRole::Border,     { 0.78f, 0.73f, 0.64f, 1.0f }, { 0.30f, 0.27f, 0.22f, 1.0f });
        set(UIThemeRole::Text,       { 0.12f, 0.11f, 0.09f, 1.0f }, { 0.94f, 0.92f, 0.88f, 1.0f });
        set(UIThemeRole::MutedText,  { 0.47f, 0.44f, 0.38f, 1.0f }, { 0.62f, 0.59f, 0.52f, 1.0f });
        set(UIThemeRole::Accent,     { 0.85f, 0.55f, 0.12f, 1.0f }, { 1.00f, 0.67f, 0.16f, 1.0f });
        // Amber is a bright colour in BOTH modes, so what you write on it is
        // dark in both: white on this orange is 2.7:1 in light and 1.9:1 in
        // dark, which is not text, it is a suggestion of text. Nearly the same
        // value twice, and that is the palette being honest rather than a role
        // that was half filled in.
        set(UIThemeRole::AccentText, { 0.12f, 0.11f, 0.09f, 1.0f }, { 0.10f, 0.09f, 0.07f, 1.0f });

        // ── …and what each KIND of element looks like in it ──────────────────
        // The roles above are the palette; these are the answers. A theme that
        // stops at nine colours leaves every author to decide for the twentieth
        // time what a hovered button is, and "the surface colour, but lighter"
        // is not something a role vocabulary can say.
        //
        // Written out with property names rather than derived from the element
        // types, on purpose: those names are an on-disk format either way, and a
        // palette should read as a list of decisions, not as a program.
        auto col = [](glm::vec4 light, glm::vec4 dark)
        {
            UIThemeStyleValue v;
            v.color[static_cast<int>(UIThemeMode::Light)] = light;
            v.color[static_cast<int>(UIThemeMode::Dark)]  = dark;
            return v;
        };
        auto num = [](float f)
        {
            UIThemeStyleValue v; v.isColor = false; v.number = f; return v;
        };
        const glm::vec4 surfaceL{ 1.00f, 0.99f, 0.96f, 1.0f }, surfaceD{ 0.14f, 0.13f, 0.11f, 1.0f };
        const glm::vec4 textL   { 0.12f, 0.11f, 0.09f, 1.0f }, textD   { 0.94f, 0.92f, 0.88f, 1.0f };
        const glm::vec4 mutedL  { 0.47f, 0.44f, 0.38f, 1.0f }, mutedD  { 0.62f, 0.59f, 0.52f, 1.0f };
        const glm::vec4 accentL { 0.85f, 0.55f, 0.12f, 1.0f }, accentD { 1.00f, 0.67f, 0.16f, 1.0f };
        const glm::vec4 borderL { 0.78f, 0.73f, 0.64f, 1.0f }, borderD { 0.30f, 0.27f, 0.22f, 1.0f };
        const glm::vec4 fieldL  { 0.97f, 0.96f, 0.93f, 1.0f }, fieldD  { 0.10f, 0.09f, 0.08f, 1.0f };

        UIThemeStyle& button = d.styleMut("Button");
        button.set("Normal Color",  col({ 0.91f, 0.88f, 0.83f, 1.0f }, { 0.22f, 0.20f, 0.17f, 1.0f }));
        button.set("Hovered Color", col({ 0.96f, 0.91f, 0.83f, 1.0f }, { 0.31f, 0.27f, 0.21f, 1.0f }));
        button.set("Pressed Color", col({ 0.85f, 0.78f, 0.68f, 1.0f }, { 0.40f, 0.30f, 0.16f, 1.0f }));
        button.set("Border Color",  col(borderL, borderD));
        button.set("Corner Radius", num(6.0f));
        button.set("Border Width",  num(1.0f));

        // The variant every application ends up needing, and the reason tags
        // exist: it says what is DIFFERENT about a primary button and inherits
        // the rest — rounding, border, the pressed feel — from the base above.
        UIThemeStyle& primary = d.styleMut(uiThemeSelector("Button", "primary"));
        primary.set("Normal Color",  col(accentL, accentD));
        primary.set("Hovered Color", col({ 0.93f, 0.63f, 0.18f, 1.0f }, { 1.00f, 0.75f, 0.30f, 1.0f }));
        primary.set("Pressed Color", col({ 0.74f, 0.46f, 0.08f, 1.0f }, { 0.86f, 0.56f, 0.10f, 1.0f }));

        UIThemeStyle& danger = d.styleMut(uiThemeSelector("Button", "danger"));
        danger.set("Normal Color",  col({ 0.82f, 0.25f, 0.22f, 1.0f }, { 0.72f, 0.24f, 0.22f, 1.0f }));
        danger.set("Hovered Color", col({ 0.90f, 0.33f, 0.29f, 1.0f }, { 0.84f, 0.31f, 0.28f, 1.0f }));
        danger.set("Pressed Color", col({ 0.68f, 0.18f, 0.16f, 1.0f }, { 0.58f, 0.17f, 0.15f, 1.0f }));

        d.styleMut("Panel").set("Color", col(surfaceL, surfaceD));
        d.styleMut("Panel").set("Corner Radius", num(8.0f));
        d.styleMut("Text").set("Color", col(textL, textD));
        // The same highlight the field uses, under the same name — a selection
        // is a selection wherever the reader made it, and a label left on its
        // own default would be the one blue thing in an amber application.
        d.styleMut("Text").set("Selection Color",
                               col({ 0.95f, 0.78f, 0.45f, 1.0f }, { 0.45f, 0.32f, 0.10f, 1.0f }));
        d.styleMut(uiThemeSelector("Text", "muted")).set("Color", col(mutedL, mutedD));

        UIThemeStyle& check = d.styleMut("CheckBox");
        check.set("Box Color",   col(fieldL, fieldD));
        check.set("Check Color", col(accentL, accentD));
        check.set("Text Color",  col(textL, textD));

        UIThemeStyle& slider = d.styleMut("Slider");
        slider.set("Track Color",  col({ 0.86f, 0.83f, 0.78f, 1.0f }, { 0.22f, 0.20f, 0.17f, 1.0f }));
        slider.set("Fill Color",   col(accentL, accentD));
        slider.set("Handle Color", col({ 0.99f, 0.98f, 0.95f, 1.0f }, { 0.86f, 0.83f, 0.78f, 1.0f }));

        UIThemeStyle& bar = d.styleMut("ProgressBar");
        bar.set("Back Color", col({ 0.86f, 0.83f, 0.78f, 1.0f }, { 0.22f, 0.20f, 0.17f, 1.0f }));
        bar.set("Fill Color", col(accentL, accentD));

        UIThemeStyle& input = d.styleMut("TextInput");
        input.set("Back Color",      col(fieldL, fieldD));
        input.set("Text Color",      col(textL, textD));
        input.set("Selection Color", col({ 0.95f, 0.78f, 0.45f, 1.0f }, { 0.45f, 0.32f, 0.10f, 1.0f }));
        input.set("Border Color",    col(borderL, borderD));
        input.set("Corner Radius",   num(4.0f));
        input.set("Border Width",    num(1.0f));

        UIThemeStyle& combo = d.styleMut("ComboBox");
        combo.set("Back Color",      col(fieldL, fieldD));
        combo.set("Text Color",      col(textL, textD));
        combo.set("Highlight Color", col(accentL, accentD));

        UIThemeStyle& tabs = d.styleMut("TabBox");
        tabs.set("Strip Color",      col({ 0.90f, 0.87f, 0.82f, 1.0f }, { 0.11f, 0.10f, 0.09f, 1.0f }));
        tabs.set("Tab Color",        col({ 0.85f, 0.82f, 0.77f, 1.0f }, { 0.18f, 0.17f, 0.14f, 1.0f }));
        tabs.set("Active Tab Color", col(surfaceL, surfaceD));
        tabs.set("Text Color",       col(textL, textD));
        tabs.set("Page Color",       col(surfaceL, surfaceD));

        UIThemeStyle& list = d.styleMut("ListView");
        list.set("Back Color",          col(surfaceL, surfaceD));
        list.set("Row Hover Color",     col({ 0.93f, 0.90f, 0.85f, 1.0f }, { 0.20f, 0.19f, 0.16f, 1.0f }));
        list.set("Row Selected Color",  col({ 0.95f, 0.82f, 0.58f, 1.0f }, { 0.38f, 0.28f, 0.12f, 1.0f }));

        d.styleMut("ScrollBox").set("Bar Color", col(borderL, borderD));
        d.styleMut("Splitter").set("Divider Color", col(borderL, borderD));

        UIThemeStyle& date = d.styleMut("DatePicker");
        date.set("Back Color",     col(surfaceL, surfaceD));
        date.set("Header Color",   col({ 0.90f, 0.87f, 0.82f, 1.0f }, { 0.11f, 0.10f, 0.09f, 1.0f }));
        date.set("Text Color",     col(textL, textD));
        date.set("Muted Color",    col(mutedL, mutedD));
        date.set("Selected Color", col(accentL, accentD));
        date.set("Hover Color",    col({ 0.93f, 0.90f, 0.85f, 1.0f }, { 0.20f, 0.19f, 0.16f, 1.0f }));

        // Only its BACKGROUND is the theme's business. The field and the strip
        // are the colour space itself — a theme that could tint those would be
        // a theme that lies about what is being picked.
        d.styleMut("ColorPicker").set("Back Color", col(surfaceL, surfaceD));
        return d;
    }();
    return t;
}

std::string uiThemeToJson(const UITheme& t)
{
    nlohmann::json j;
    j["name"] = t.name;
    // Keyed by NAME, never by index: a role added in the middle would silently
    // reinterpret every stored colour if these were positional.
    nlohmann::json colors = nlohmann::json::object();
    for (int r = 0; r < kRoles; ++r)
    {
        nlohmann::json entry = nlohmann::json::object();
        for (int m = 0; m < kModes; ++m) entry[kModeNames[m]] = toArray(t.color[r][m]);
        colors[kRoleNames[r]] = std::move(entry);
    }
    j["colors"] = std::move(colors);

    nlohmann::json radius = nlohmann::json::object(), spacing = nlohmann::json::object();
    for (int s = 0; s < kSizes; ++s)
    { radius[kSizeNames[s]] = t.radius[s]; spacing[kSizeNames[s]] = t.spacing[s]; }
    j["radius"]  = std::move(radius);
    j["spacing"] = std::move(spacing);

    nlohmann::json text = nlohmann::json::object();
    for (int l = 0; l < kLevels; ++l) text[kLevelNames[l]] = t.textSize[l];
    j["textSize"] = std::move(text);

    nlohmann::json shadows = nlohmann::json::object();
    for (int e = 0; e < kElevations; ++e)
    {
        nlohmann::json s = nlohmann::json::object();
        s["color"]   = toArray(t.shadow[e].color);
        s["blur"]    = t.shadow[e].blur;
        s["offset"]  = nlohmann::json::array({ t.shadow[e].offsetX, t.shadow[e].offsetY });
        shadows[kElevationNames[e]] = std::move(s);
    }
    j["shadows"] = std::move(shadows);

    // Styles, and only when there are any: a theme that decides nothing about
    // whole element types saves exactly as it did before styles existed.
    //
    // An ARRAY rather than an object, twice over, because both levels are
    // ordered and a JSON object is not: the editor lists the styles and their
    // values in the order they were added, and a reader that sorted them would
    // shuffle an author's list every time it round-tripped.
    if (!t.styles.empty())
    {
        nlohmann::json styles = nlohmann::json::array();
        for (const auto& [key, style] : t.styles)
        {
            nlohmann::json vals = nlohmann::json::array();
            for (const auto& [prop, v] : style.values)
            {
                nlohmann::json e = nlohmann::json::object();
                e["prop"] = prop;
                if (v.isColor)
                    for (int m = 0; m < kModes; ++m) e[kModeNames[m]] = toArray(v.color[m]);
                else
                    e["number"] = v.number;
                vals.push_back(std::move(e));
            }
            nlohmann::json s = nlohmann::json::object();
            s["for"]    = key;
            s["values"] = std::move(vals);
            styles.push_back(std::move(s));
        }
        j["styles"] = std::move(styles);
    }
    return j.dump(4);
}

bool uiThemeFromJson(const std::string& json, UITheme& out)
{
    nlohmann::json j;
    if (!HE::graph::parseGraphObject(json, j)) return false;

    // Start from the default rather than from zero: a theme file that predates a
    // role, or one an author trimmed by hand, keeps working and the missing role
    // is simply the default's — black everywhere would be a broken application
    // for a missing key.
    UITheme t = uiDefaultTheme();
    t.name = j.value("name", t.name);

    if (const auto colors = j.find("colors"); colors != j.end() && colors->is_object())
        for (int r = 0; r < kRoles; ++r)
            if (const auto entry = colors->find(kRoleNames[r]);
                entry != colors->end() && entry->is_object())
                for (int m = 0; m < kModes; ++m)
                    if (const auto v = entry->find(kModeNames[m]); v != entry->end())
                        readColor(*v, t.color[r][m]);

    auto readSizes = [&](const char* key, float (&dst)[kSizes])
    {
        if (const auto o = j.find(key); o != j.end() && o->is_object())
            for (int s = 0; s < kSizes; ++s)
                if (const auto v = o->find(kSizeNames[s]); v != o->end() && v->is_number())
                    dst[s] = v->get<float>();
    };
    readSizes("radius",  t.radius);
    readSizes("spacing", t.spacing);

    if (const auto o = j.find("textSize"); o != j.end() && o->is_object())
        for (int l = 0; l < kLevels; ++l)
            if (const auto v = o->find(kLevelNames[l]); v != o->end() && v->is_number())
                t.textSize[l] = v->get<float>();

    if (const auto o = j.find("shadows"); o != j.end() && o->is_object())
        for (int e = 0; e < kElevations; ++e)
        {
            const auto s = o->find(kElevationNames[e]);
            if (s == o->end() || !s->is_object()) continue;
            if (const auto c = s->find("color"); c != s->end()) readColor(*c, t.shadow[e].color);
            t.shadow[e].blur = s->value("blur", t.shadow[e].blur);
            if (const auto off = s->find("offset");
                off != s->end() && off->is_array() && off->size() == 2)
            {
                t.shadow[e].offsetX = (*off)[0].get<float>();
                t.shadow[e].offsetY = (*off)[1].get<float>();
            }
        }

    // Styles are optional in every direction: a file that predates them keeps
    // the default's (none), and one entry that makes no sense is dropped rather
    // than taking the rest of the style with it.
    if (const auto styles = j.find("styles"); styles != j.end() && styles->is_array())
    {
        t.styles.clear();
        for (const auto& s : *styles)
        {
            if (!s.is_object()) continue;
            const std::string key = s.value("for", std::string());
            if (key.empty()) continue;
            UIThemeStyle style;
            if (const auto vals = s.find("values"); vals != s.end() && vals->is_array())
                for (const auto& e : *vals)
                {
                    if (!e.is_object()) continue;
                    const std::string prop = e.value("prop", std::string());
                    if (prop.empty()) continue;
                    UIThemeStyleValue v;
                    if (const auto n = e.find("number"); n != e.end() && n->is_number())
                    { v.isColor = false; v.number = n->get<float>(); }
                    else
                        for (int m = 0; m < kModes; ++m)
                            if (const auto c = e.find(kModeNames[m]); c != e.end())
                                readColor(*c, v.color[m]);
                    style.set(prop, v);
                }
            t.styles.emplace_back(key, std::move(style));
        }
    }

    out = std::move(t);
    return true;
}

} // namespace HE
