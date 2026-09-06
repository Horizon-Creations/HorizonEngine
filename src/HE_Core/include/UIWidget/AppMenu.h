#pragma once
#include <Types/Defines.h>
#include <string>
#include <vector>

namespace HE {

// ── The application's menu bar (plan A6) ─────────────────────────────────────
// The DATA, and only the data: who draws it is a separate question with two
// answers. On Windows and Linux the widget layer draws a strip itself; on macOS
// the same vector becomes an NSMenu in the system bar. Both read this, so the
// application says what its menus are exactly once.
//
// The plan asked for an ASSET rather than code. This is the same intention by a
// shorter road: what mattered was that HorizonCode can fill the menu instead of
// it being hard-coded, and a runtime API does that directly, where a new asset
// type would first need an editor surface nobody has asked for. If a menu ever
// wants to be authored rather than built, this struct is what that asset would
// load into.
struct AppMenuItem
{
    // The id is what comes back in OnMenuItem, the label is what somebody reads.
    // Two of them, for the reason a tray entry has two: a translated menu must
    // keep doing what it did.
    std::string id;
    std::string label;
    // A line, not a row. It carries no id and cannot be chosen.
    bool separator = false;
    // The chord that chooses this entry without opening the menu, written the
    // way people write one: "Ctrl+Shift+S" (UIWidget/UIShortcut.h). Empty means
    // no shortcut, which is what most entries have.
    //
    // Stored as TEXT and not as a parsed pair, because that is what a project
    // file holds and what a menu row shows. It is also why it sits here and not
    // beside the key handler: the entry owns its chord, so a bar rebuilt by a
    // graph brings its shortcuts with it and nothing has to be kept in step.
    std::string shortcut;
};

struct AppMenu
{
    std::string id;      // "file" — what addMenuItem names to reach it
    std::string label;   // "File" — what is drawn in the strip
    std::vector<AppMenuItem> items;
};

} // namespace HE
