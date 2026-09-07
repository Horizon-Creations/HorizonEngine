#pragma once
#include "Types/Defines.h"
#include <string>

// ── Keyboard shortcuts, as text (docs/he-apps-plan.md B10) ──────────────────
// A chord is written the way people write one — "Ctrl+Shift+S" — because that
// is what goes into a project file, a translation table and a menu row, and a
// pair of numbers would go into none of them. This is the parser for that text
// and the printer back out of it.
//
// It lives in HorizonCore and knows nothing about SDL: the key is a NAME, and
// the names are exactly the ones SDL_GetKeyName gives back ("S", "F5",
// "Return", "Page Up"), so the host's side of the match is one call and no
// table. The set of keys it accepts is deliberately the set macOS can also put
// on a menu row — a chord that parses here but cannot be shown there would fire
// nowhere on the platform where the system draws the bar.

namespace HE
{

// One chord. `key` empty means nothing was parsed.
//
// There is no separate "Cmd": on a Mac the command key IS this ctrl, mapped on
// the way in. One flag, because a menu that means Ctrl+S on Windows means Cmd+S
// on a Mac and an application should not have to say so twice — the same
// reasoning the text-editing keys in GameApplication already follow.
struct UIShortcut
{
    std::string key;                 // canonical, e.g. "S", "F5", "Return"
    bool ctrl  = false;              // Ctrl, and Cmd on macOS
    bool shift = false;
    bool alt   = false;              // Alt, and Option on macOS

    bool valid() const { return !key.empty(); }
    bool operator==(const UIShortcut& o) const
    {
        return key == o.key && ctrl == o.ctrl && shift == o.shift && alt == o.alt;
    }
};

// "ctrl + shift+s" → { "S", ctrl, shift }. Case-insensitive, whitespace is
// ignored, and the parts may come in any order: the modifiers are a SET, and
// insisting on an order would reject a file somebody typed correctly.
//
// False (and an untouched `out`) when there is no key, when there is more than
// one, or when the key is not one this engine can express. Empty text is false
// too — "no shortcut" is the normal state of a menu entry and not an error.
HE_API bool uiParseShortcut(const std::string& text, UIShortcut& out);

// Back to text, in the canonical spelling: "Ctrl+Alt+Shift+S", modifiers in
// that fixed order. Written out in words rather than as ⌘⇧ symbols because this
// string is drawn by the engine's own font, which has no key glyphs — and the
// only platform whose bar wants the symbols draws its rows itself.
HE_API std::string uiFormatShortcut(const UIShortcut& s);

// Does this key press mean that chord? The name is compared case-insensitively,
// so a host that hands over "s" is answered the same as one that hands over "S".
HE_API bool uiShortcutMatches(const UIShortcut& s, const std::string& keyName,
                              bool ctrl, bool shift, bool alt);

// Convenience for the common shape: parse and match in one go. False whenever
// the text does not parse, so an unreadable chord is a chord that never fires
// rather than one that fires on everything.
HE_API bool uiShortcutMatchesText(const std::string& text, const std::string& keyName,
                                  bool ctrl, bool shift, bool alt);

} // namespace HE
