#include "UIWidget/UIShortcut.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace HE
{
namespace
{
	std::string lower(const std::string& s)
	{
		std::string o;
		o.reserve(s.size());
		for (char c : s) o.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		return o;
	}

	// Whitespace is dropped rather than trimmed: "Page Up" is one token with a
	// space in it, and "Ctrl + Page Up" would otherwise have to be spelled
	// without one. Nothing this parser accepts needs an inner space to be told
	// apart, so throwing them all away is safe and makes the table below flat.
	std::string squeeze(const std::string& s)
	{
		std::string o;
		for (char c : s)
			if (!std::isspace(static_cast<unsigned char>(c))) o.push_back(c);
		return o;
	}

	// The canonical name for a key token, or empty when this engine cannot
	// express it. The right-hand side is what SDL_GetKeyName returns, because
	// that is the string the host compares against — a table that invented its
	// own spellings would match nothing at all.
	//
	// The set is bounded on purpose (see the header): letters, digits, the
	// function row and the named keys AppKit can put on a menu row.
	std::string canonicalKey(const std::string& tokLower)
	{
		if (tokLower.empty()) return {};

		// A single letter or digit: "s" → "S", "1" → "1".
		if (tokLower.size() == 1)
		{
			const unsigned char c = static_cast<unsigned char>(tokLower[0]);
			if (std::isalpha(c)) return std::string(1, static_cast<char>(std::toupper(c)));
			if (std::isdigit(c)) return std::string(1, static_cast<char>(c));
			return {};
		}

		// The function row. Parsed rather than tabulated so F1 and F12 cannot
		// disagree about which of them somebody forgot to add.
		if (tokLower[0] == 'f' && tokLower.size() <= 3)
		{
			int n = 0;
			for (std::size_t i = 1; i < tokLower.size(); ++i)
			{
				if (!std::isdigit(static_cast<unsigned char>(tokLower[i]))) return {};
				n = n * 10 + (tokLower[i] - '0');
			}
			if (n >= 1 && n <= 12)
			{
				char buf[8];
				std::snprintf(buf, sizeof(buf), "F%d", n);
				return std::string(buf);
			}
			return {};
		}

		struct Named { const char* alias; const char* canonical; };
		static const Named kNamed[] = {
			{ "return",    "Return"    }, { "enter",   "Return"    },
			{ "escape",    "Escape"    }, { "esc",     "Escape"    },
			{ "backspace", "Backspace" }, { "bksp",    "Backspace" },
			{ "delete",    "Delete"    }, { "del",     "Delete"    },
			{ "tab",       "Tab"       },
			{ "space",     "Space"     }, { "spacebar", "Space"    },
			{ "left",      "Left"      }, { "right",   "Right"     },
			{ "up",        "Up"        }, { "down",    "Down"      },
			{ "home",      "Home"      }, { "end",     "End"       },
			{ "pageup",    "Page Up"   }, { "pgup",    "Page Up"   },
			{ "pagedown",  "Page Down" }, { "pgdn",    "Page Down" },
		};
		for (const Named& n : kNamed)
			if (tokLower == n.alias) return n.canonical;
		return {};
	}
}

bool uiParseShortcut(const std::string& text, UIShortcut& out)
{
	const std::string s = squeeze(text);
	if (s.empty()) return false;

	UIShortcut r;
	std::size_t at = 0;
	while (at <= s.size())
	{
		const std::size_t plus = s.find('+', at);
		// A trailing "+" is the key named "+", which is not in the set, and a
		// leading one is an empty token: both fall out as "no key" below.
		const std::string tok = lower(s.substr(at, plus == std::string::npos
		                                              ? std::string::npos : plus - at));
		at = (plus == std::string::npos) ? s.size() + 1 : plus + 1;
		if (tok.empty()) return false;

		// Modifiers first. Cmd, Command, Meta and Super all fold onto ctrl —
		// see the header for why there is only one flag.
		if (tok == "ctrl" || tok == "control" || tok == "cmd" || tok == "command" ||
		    tok == "meta" || tok == "super" || tok == "win")
		{ r.ctrl = true; continue; }
		if (tok == "shift") { r.shift = true; continue; }
		if (tok == "alt" || tok == "option" || tok == "opt")
		{ r.alt = true; continue; }

		const std::string key = canonicalKey(tok);
		if (key.empty()) return false;      // an unknown token is not a chord
		if (!r.key.empty()) return false;   // two keys is not a chord either
		r.key = key;
	}

	if (r.key.empty()) return false;        // modifiers alone are not a chord
	out = r;
	return true;
}

std::string uiFormatShortcut(const UIShortcut& s)
{
	if (!s.valid()) return {};
	std::string o;
	if (s.ctrl)  o += "Ctrl+";
	if (s.alt)   o += "Alt+";
	if (s.shift) o += "Shift+";
	o += s.key;
	return o;
}

bool uiShortcutMatches(const UIShortcut& s, const std::string& keyName,
                       bool ctrl, bool shift, bool alt)
{
	if (!s.valid()) return false;
	// Every modifier is compared, the absent ones included: Ctrl+S must not fire
	// on Ctrl+Shift+S, or an application cannot have both.
	if (s.ctrl != ctrl || s.shift != shift || s.alt != alt) return false;
	return lower(s.key) == lower(keyName);
}

bool uiShortcutMatchesText(const std::string& text, const std::string& keyName,
                           bool ctrl, bool shift, bool alt)
{
	UIShortcut s;
	if (!uiParseShortcut(text, s)) return false;
	return uiShortcutMatches(s, keyName, ctrl, shift, alt);
}

} // namespace HE
