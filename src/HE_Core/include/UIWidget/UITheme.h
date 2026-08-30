#pragma once
#include <Types/Defines.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>

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

    glm::vec4 colorFor(UIThemeRole r, UIThemeMode m) const
    {
        if (r >= UIThemeRole::COUNT || m >= UIThemeMode::COUNT) return glm::vec4(1.0f);
        return color[static_cast<int>(r)][static_cast<int>(m)];
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
