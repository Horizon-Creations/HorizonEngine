#pragma once
#include <Types/Defines.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// ── One place to decide what an application looks like ────────────────────────
// docs/he-apps-plan.md D1. Without this, an author who builds ten buttons types
// the same corner radius, the same border colour and the same shadow ten times —
// and changes them ten times later. A theme turns those literals into ROLES: an
// element says "I am a Surface" or "I am Muted Text", and what that means is
// decided once, in one asset, for the whole application.
//
// It is also what makes light and dark one decision instead of a second set of
// widgets: every role carries BOTH values, and switching mode re-resolves them.
//
// Deliberately small and closed, for the same reason "Schicht 0" is: a fixed
// vocabulary can be offered in a dropdown, checked by a test, and understood
// without documentation. Nine colours is what interfaces actually use.
namespace HE
{

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  THESE NAMES ARE AN ON-DISK FORMAT.                                      ║
// ║                                                                          ║
// ║  A theme asset stores them, and every element that is bound to a role    ║
// ║  stores the role's NAME. Renaming one silently unbinds every element     ║
// ║  that used it — the lookup misses and the element keeps whatever literal ║
// ║  it last had. test_ui_theme.cpp pins the full list.                      ║
// ╚══════════════════════════════════════════════════════════════════════════╝
enum class UIThemeRole : uint8_t
{
    Background = 0, // the window behind everything
    Surface,        // a panel, a card, a field — something ON the background
    Border,         // the line around a surface
    Text,           // ordinary reading text
    MutedText,      // secondary text: hints, placeholders, disabled labels
    Accent,         // the one colour the eye is meant to go to
    Warning,
    Error,
    Success,
    COUNT
};

enum class UIThemeMode : uint8_t { Light = 0, Dark, COUNT };

// What an application was ASKED for, which is not the same as what it resolves
// to. "System" is the default and the reason this is a separate enum: an
// application that follows the desktop has no fixed mode, it has a rule — and
// storing the resolved value instead would make a project that was authored on
// a dark machine ship as dark for everyone.
enum class UIThemePreference : uint8_t { System = 0, Light, Dark, COUNT };
HE_API const char*        uiThemePreferenceName(UIThemePreference p);
HE_API UIThemePreference  uiThemePreferenceFromName(const std::string& s);

HE_API const char* uiThemeRoleName(UIThemeRole r);
// Unknown name → COUNT, which every caller reads as "not bound".
HE_API UIThemeRole uiThemeRoleFromName(const std::string& s);
HE_API const char* uiThemeModeName(UIThemeMode m);

// Sizes come in steps rather than in numbers, for the same reason colours come
// in roles: "Medium" survives a redesign, "6.5" does not.
enum class UIThemeSize : uint8_t { Small = 0, Medium, Large, COUNT };
// Text sizes. Mono is a SIZE here and not a font: shipping a second typeface is
// its own piece of work (D2), and a level that only changes size is still the
// level an author asks for.
enum class UIThemeTextLevel : uint8_t { Title = 0, Heading, Body, Small, Mono, COUNT };

HE_API const char* uiThemeSizeName(UIThemeSize s);
HE_API const char* uiThemeTextLevelName(UIThemeTextLevel t);
// Unknown → COUNT, which every caller reads as "not bound". Both vocabularies
// contain "Small", which is why an element decides which of the two a binding
// belongs to from the PROPERTY it sits on, never from the name (uiApplyTheme).
HE_API UIThemeSize      uiThemeSizeFromName(const std::string& s);
HE_API UIThemeTextLevel uiThemeTextLevelFromName(const std::string& s);

// Which of the theme's three number scales a Float property reads. This is the
// rule the sentence above describes, written once so that resolving a binding
// (uiThemeValueFor) and offering one (the designer's role button) cannot drift
// apart — they used to be two half-rules, and the half in the resolver said
// "radius" for everything that was not a font size.
//
// A gap is not a rounding. "Padding" and "Spacing" are distances between things
// and read the spacing steps; a corner radius and a border width are the shape
// of one thing and read the radius steps. Every other Float lands on Radius,
// which is where it landed before: a slider's minimum has no step that means it,
// nothing offers a binding for it, and a hand-written one keeps its old answer.
enum class UIThemeScale : uint8_t { Radius = 0, Spacing, Text };
HE_API UIThemeScale uiThemeScaleFor(const std::string& prop);
// Whether a Float property can be bound to a step BY HAND at all — true for
// exactly the ones with a vocabulary that names their case. False does not mean
// "the theme decides nothing here": a style may still name any property.
HE_API bool uiThemeScaleBindable(const std::string& prop);

// A drop shadow as the theme hands it out: one step of "how far off the page".
struct UIThemeShadow
{
    glm::vec4 color{ 0.0f, 0.0f, 0.0f, 0.35f };
    float     blur    = 8.0f;
    float     offsetX = 0.0f;
    float     offsetY = 3.0f;
};
enum class UIThemeElevation : uint8_t { Raised = 0, Overlay, COUNT };
HE_API const char* uiThemeElevationName(UIThemeElevation e);

// ── A style: the answer for a whole KIND of element, at once ─────────────────
// A role is one colour, and binding an element to roles is one decision PER
// VALUE: a button is six of them, and its hover and its pressed colour are two
// that no role vocabulary can name (there is no "Accent, but hovered").
//
// A style is the other half. It is keyed by the very property names the element
// already has — "Normal Color", "Hovered Color", "Pressed Color",
// "Corner Radius", "FontSize" — so it needs no per-type code and no second
// vocabulary: whatever a type exposes, a style can decide. An element says
// which style it follows (UIElement::themeStyled / themeStyle) and that is ONE
// decision for its whole look.
//
// Deliberately open where the roles are closed. The roles are nine because a
// dropdown of nine can be understood; a style's keys come from the element in
// front of you, and its NAME may be anything, which is what lets a project
// theme its own components ("Card", "Danger Button") and not only the built-in
// types.
struct UIThemeStyleValue
{
    // Colours carry both modes, like a role, because that is the whole point of
    // a theme. A number does not: a corner radius is not lighter in light mode.
    bool      isColor = true;
    glm::vec4 color[static_cast<int>(UIThemeMode::COUNT)]{};
    float     number = 0.0f;
};

struct UIThemeStyle
{
    // Ordered, not a map. The editor shows a style in the order the element's
    // own property table has, and a map would sort "Corner Radius" above
    // "Normal Color" for a reason no author can see.
    std::vector<std::pair<std::string, UIThemeStyleValue>> values;

    const UIThemeStyleValue* find(const std::string& prop) const
    {
        for (const auto& [p, v] : values) if (p == prop) return &v;
        return nullptr;
    }
    // Set or replace. Removing is erase(prop) — a style holds only what it
    // actually decides, and a value it does not hold is one the element keeps.
    void set(const std::string& prop, const UIThemeStyleValue& v)
    {
        for (auto& [p, cur] : values) if (p == prop) { cur = v; return; }
        values.emplace_back(prop, v);
    }
    void erase(const std::string& prop)
    {
        for (auto it = values.begin(); it != values.end(); ++it)
            if (it->first == prop) { values.erase(it); return; }
    }
};

// ── Selectors ────────────────────────────────────────────────────────────────
// A style's key is a SELECTOR, and the vocabulary is deliberately the smallest
// one that answers what people actually ask of a theme:
//
//   "Button"          every button
//   "Button.success"  a button whose tag is "success"
//   "Card"            whatever points at it by name
//
// The dot is CSS's, and so is what happens when several match: they LAYER, most
// specific last, property by property. A "Button.success" that names only a
// colour still gets the rounding and the border from "Button" — which is the
// entire reason to write a variant instead of a whole second style.
//
// Deliberately not CSS: no descendant combinators, no inheritance down the
// tree, no bare ".tag" that matches every type. Each of those needs a
// specificity rule, and a rule an author cannot predict in their head is worse
// than a vocabulary that cannot express the case.
HE_API std::string uiThemeSelector(const std::string& type, const std::string& tag);
// The type half of a selector ("Button.success" → "Button"), and the tag half
// ("success", empty for a plain type selector).
HE_API std::string uiThemeSelectorType(const std::string& selector);
HE_API std::string uiThemeSelectorTag(const std::string& selector);

// The binding that means "the theme decides nothing here". Not a role, and not
// the same as having no binding at all: no binding lets the element's STYLE
// answer, this shuts even that out. Written wherever a value was decided
// somewhere else and must survive the next theme change — a component
// parameter, above all (uiApplyWidgetParams).
inline constexpr const char* kUIThemeLiteral = "(literal)";

struct HE_API UITheme
{
    std::string name = "Default";

    // Every role in both modes. Indexed [role][mode] — one array rather than two
    // structs, because "light and dark are two values of one decision" is the
    // whole idea and splitting them invites one half being edited alone.
    glm::vec4 color[static_cast<int>(UIThemeRole::COUNT)]
                   [static_cast<int>(UIThemeMode::COUNT)]{};

    float radius[static_cast<int>(UIThemeSize::COUNT)]{ 4.0f, 8.0f, 16.0f };
    float spacing[static_cast<int>(UIThemeSize::COUNT)]{ 4.0f, 8.0f, 16.0f };
    float textSize[static_cast<int>(UIThemeTextLevel::COUNT)]{ 32.0f, 24.0f, 16.0f, 13.0f, 15.0f };
    UIThemeShadow shadow[static_cast<int>(UIThemeElevation::COUNT)];

    // Styles, keyed by the element TYPE NAME ("Button") for the one every
    // element of that type follows by default, or by a free name ("Card") for
    // one an element points at deliberately. Empty is the normal state of a
    // fresh theme and means "styles decide nothing" — every element keeps the
    // values it was authored with until somebody adds a style here.
    std::vector<std::pair<std::string, UIThemeStyle>> styles;

    glm::vec4 colorFor(UIThemeRole r, UIThemeMode m) const
    {
        if (r >= UIThemeRole::COUNT || m >= UIThemeMode::COUNT) return glm::vec4(1.0f);
        return color[static_cast<int>(r)][static_cast<int>(m)];
    }

    const UIThemeStyle* styleFor(const std::string& key) const
    {
        if (key.empty()) return nullptr;
        for (const auto& [k, s] : styles) if (k == key) return &s;
        return nullptr;
    }
    // The style under `key`, created empty if there is none. The editor writes
    // through this; nothing else should, because an empty style saved into a
    // theme is a key an author then has to find and delete.
    UIThemeStyle& styleMut(const std::string& key)
    {
        for (auto& [k, s] : styles) if (k == key) return s;
        styles.emplace_back(key, UIThemeStyle{});
        return styles.back().second;
    }
    void eraseStyle(const std::string& key)
    {
        for (auto it = styles.begin(); it != styles.end(); ++it)
            if (it->first == key) { styles.erase(it); return; }
    }

    // The variants this theme defines for one element type — the tags of every
    // "Type.tag" style it holds. What an element's tag dropdown offers, so a tag
    // is picked from what exists instead of typed from memory.
    std::vector<std::string> tagsFor(const std::string& type) const
    {
        std::vector<std::string> out;
        for (const auto& [key, s] : styles)
        {
            const std::string t = uiThemeSelectorTag(key);
            if (!t.empty() && uiThemeSelectorType(key) == type) out.push_back(t);
        }
        return out;
    }
};

// The theme an application has before it has one. Like the shared UI font: there
// is never a state where the engine has no theme, so nothing has to answer
// "what if none is set" — it just resolves against this.
HE_API const UITheme& uiDefaultTheme();
// The editor's own amber palette as a theme, shipped as the third one.
HE_API const UITheme& uiAmberTheme();

HE_API std::string uiThemeToJson(const UITheme& t);
HE_API bool        uiThemeFromJson(const std::string& json, UITheme& out);

} // namespace HE
