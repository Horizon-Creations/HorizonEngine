#include "EditorShortcuts.h"

#ifdef HE_IMGUI_ENABLED
#include "EditorHelp.h"   // shortcutLabel: Ctrl → Cmd on macOS
#include <imgui_internal.h>   // IsMouseKey / IsGamepadKey for the capture walk

#include <cstring>
#include <unordered_map>

namespace EditorShortcuts
{
namespace
{
	// The table. Order = the order on the Shortcuts page, category by
	// category. Ids are addresses: renaming one orphans a user's override.
	const std::vector<Action>& table()
	{
		static const std::vector<Action> kActions = {
			// File — the native macOS menu carries these as key equivalents.
			{ "file.save",        "Save",              "File", Scope::Global, ImGuiMod_Ctrl | ImGuiKey_S, true },
			{ "file.saveAll",     "Save All",          "File", Scope::Global, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, true },
			{ "file.saveSceneAs", "Save Scene As\xe2\x80\xa6", "File", Scope::Global, ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_S, true },
			{ "file.openProject", "Open Project\xe2\x80\xa6", "File", Scope::Global, ImGuiMod_Ctrl | ImGuiKey_O, true },

			// Edit
			{ "edit.undo",        "Undo",              "Edit", Scope::Global, ImGuiMod_Ctrl | ImGuiKey_Z },
			{ "edit.redo",        "Redo",              "Edit", Scope::Global, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z },
			{ "edit.redoAlt",     "Redo (alternate)",  "Edit", Scope::Global, ImGuiMod_Ctrl | ImGuiKey_Y },
			{ "edit.preferences", "Preferences",       "Edit", Scope::Global, ImGuiMod_Ctrl | ImGuiKey_Comma, true },

			// Entities — the scene tab's clipboard verbs, wherever the
			// selection was made (viewport, Outliner).
			{ "entity.duplicate", "Duplicate",         "Entities", Scope::Global, ImGuiMod_Ctrl | ImGuiKey_D },
			{ "entity.copy",      "Copy",              "Entities", Scope::Global, ImGuiMod_Ctrl | ImGuiKey_C },
			{ "entity.cut",       "Cut",               "Entities", Scope::Global, ImGuiMod_Ctrl | ImGuiKey_X },
			{ "entity.paste",     "Paste",             "Entities", Scope::Global, ImGuiMod_Ctrl | ImGuiKey_V },
			{ "entity.delete",    "Delete",            "Entities", Scope::Global, ImGuiKey_Delete },

			// View
			{ "view.console",     "Toggle Console",    "View", Scope::Global, ImGuiMod_Ctrl | ImGuiKey_GraveAccent },
			{ "view.fullscreen",  "Toggle Fullscreen", "View", Scope::Global, ImGuiKey_F11, false, true },

			// Viewport — only while the pointer is over the picture.
			{ "viewport.move",         "Move Tool",       "Viewport", Scope::Viewport, ImGuiKey_W },
			{ "viewport.rotate",       "Rotate Tool",     "Viewport", Scope::Viewport, ImGuiKey_E },
			{ "viewport.scale",        "Scale Tool",      "Viewport", Scope::Viewport, ImGuiKey_R },
			{ "viewport.focus",        "Focus Selected",  "Viewport", Scope::Viewport, ImGuiKey_F },
			{ "viewport.snapToGround", "Snap to Ground",  "Viewport", Scope::Viewport, ImGuiKey_End },
			{ "viewport.hide",         "Hide Selected",   "Viewport", Scope::Viewport, ImGuiKey_H },
			{ "viewport.isolate",      "Isolate Selected","Viewport", Scope::Viewport, ImGuiMod_Shift | ImGuiKey_H },
			{ "viewport.showAll",      "Show All",        "Viewport", Scope::Viewport, ImGuiMod_Alt | ImGuiKey_H },
			{ "viewport.group",        "Group",           "Viewport", Scope::Viewport, ImGuiMod_Ctrl | ImGuiKey_G },
			{ "viewport.ungroup",      "Ungroup",         "Viewport", Scope::Viewport, ImGuiMod_Shift | ImGuiKey_G },
		};
		return kActions;
	}

	// id → chord, only for what differs from the default.
	std::unordered_map<std::string, ImGuiKeyChord>& overrides()
	{
		static std::unordered_map<std::string, ImGuiKeyChord> s;
		return s;
	}

	constexpr ImGuiKeyChord kModMask = ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt | ImGuiMod_Super;

	// A key that can be the KEY of a chord: on the keyboard, and not a
	// modifier itself (those ride in the chord's flags). Mouse buttons, the
	// gamepad and the reserved mod slots are out.
	bool bindable(ImGuiKey key)
	{
		if (ImGui::IsMouseKey(key) || ImGui::IsGamepadKey(key)) return false;
		if (key >= ImGuiKey_LeftCtrl && key <= ImGuiKey_RightSuper) return false;
		if (key >= ImGuiKey_ReservedForModCtrl) return false;
		return true;
	}

	// The Delete key reads better as "Del" in a menu, the comma as itself.
	// Only for what the menus print; the config spelling stays ImGui's.
	const char* prettyKey(ImGuiKey key)
	{
		switch (key)
		{
		case ImGuiKey_Comma:       return ",";
		case ImGuiKey_Period:      return ".";
		case ImGuiKey_GraveAccent: return "`";
		case ImGuiKey_Delete:      return "Del";
		case ImGuiKey_Escape:      return "Esc";
		case ImGuiKey_Backspace:   return "Backspace";
		default:                   return ImGui::GetKeyName(key);
		}
	}

	std::string modsPrefix(ImGuiKeyChord chord)
	{
		std::string s;
		if (chord & ImGuiMod_Ctrl)  s += "Ctrl+";
		if (chord & ImGuiMod_Shift) s += "Shift+";
		if (chord & ImGuiMod_Alt)   s += "Alt+";
		if (chord & ImGuiMod_Super) s += "Super+";
		return s;
	}

	bool sameId(const Action& a, std::string_view id) { return id == a.id; }

	// The frame the Shortcuts page last said it was capturing on; -2 = never.
	int g_captureFrame = -2;
}

void noteCapturing() { g_captureFrame = ImGui::GetFrameCount(); }
bool capturingNow()  { return ImGui::GetFrameCount() - g_captureFrame <= 1; }

const std::vector<Action>& actions() { return table(); }

const Action* find(std::string_view id)
{
	for (const Action& a : table())
		if (sameId(a, id)) return &a;
	return nullptr;
}

ImGuiKeyChord chord(std::string_view id)
{
	const auto& ov = overrides();
	if (const auto it = ov.find(std::string(id)); it != ov.end()) return it->second;
	const Action* a = find(id);
	return a ? a->defaultChord : ImGuiKey_None;
}

void setChord(std::string_view id, ImGuiKeyChord c)
{
	const Action* a = find(id);
	if (!a) return;
	if (c == a->defaultChord) overrides().erase(std::string(id));
	else                      overrides()[std::string(id)] = c;
}

void reset(std::string_view id) { overrides().erase(std::string(id)); }
void resetAll()                 { overrides().clear(); }
bool isDefault(std::string_view id) { return overrides().count(std::string(id)) == 0; }

bool pressed(std::string_view id)
{
	const Action* a = find(id);
	if (!a) return false;
	const ImGuiKeyChord c = chord(id);
	if (c == ImGuiKey_None) return false;
	const ImGuiIO& io = ImGui::GetIO();
	if (io.WantTextInput && !a->whileTyping) return false;
	// The keystroke is being taken as a new binding — Ctrl+O chosen for Save
	// must not open the project dialog on the way.
	if (capturingNow()) return false;

	// Modifiers, exactly — with Ctrl standing for "Ctrl or Cmd", the editor's
	// long-standing rule (io.KeyCtrl || io.KeySuper). Super on its own in a
	// chord is the OTHER key then: whichever of Ctrl/Cmd ImGui did not map to
	// KeyCtrl on this platform.
	const bool wantCtrl  = (c & ImGuiMod_Ctrl)  != 0;
	const bool wantShift = (c & ImGuiMod_Shift) != 0;
	const bool wantAlt   = (c & ImGuiMod_Alt)   != 0;
	const bool wantSuper = (c & ImGuiMod_Super) != 0;
	const bool ctrlish   = io.KeyCtrl || io.KeySuper;
	if (wantCtrl)
	{
		if (!ctrlish) return false;
	}
	else if (wantSuper)
	{
		if (!io.KeySuper) return false;
	}
	else if (ctrlish) return false;
	if (wantShift != io.KeyShift) return false;
	if (wantAlt   != io.KeyAlt)   return false;

	return ImGui::IsKeyPressed(static_cast<ImGuiKey>(c & ~kModMask), false);
}

std::string chordLabel(ImGuiKeyChord c)
{
	if (c == ImGuiKey_None) return {};
	const ImGuiKey key = static_cast<ImGuiKey>(c & ~kModMask);
	return HE::Ed::Help::shortcutLabel(modsPrefix(c) + prettyKey(key));
}

std::string label(std::string_view id) { return chordLabel(chord(id)); }

std::vector<const Action*> conflicts(std::string_view id)
{
	std::vector<const Action*> out;
	const Action* self = find(id);
	if (!self) return out;
	const ImGuiKeyChord c = chord(id);
	if (c == ImGuiKey_None) return out;
	for (const Action& other : table())
	{
		if (&other == self || chord(other.id) != c) continue;
		if (other.scope == self->scope || other.scope == Scope::Global || self->scope == Scope::Global)
			out.push_back(&other);
	}
	return out;
}

std::string chordToText(ImGuiKeyChord c)
{
	if (c == ImGuiKey_None) return {};
	return modsPrefix(c) + ImGui::GetKeyName(static_cast<ImGuiKey>(c & ~kModMask));
}

ImGuiKeyChord chordFromText(std::string_view text)
{
	ImGuiKeyChord mods = 0;
	for (;;)
	{
		const size_t plus = text.find('+');
		if (plus == std::string_view::npos || plus + 1 >= text.size()) break;
		const std::string_view mod = text.substr(0, plus);
		if      (mod == "Ctrl")  mods |= ImGuiMod_Ctrl;
		else if (mod == "Shift") mods |= ImGuiMod_Shift;
		else if (mod == "Alt")   mods |= ImGuiMod_Alt;
		else if (mod == "Super") mods |= ImGuiMod_Super;
		else return ImGuiKey_None;
		text.remove_prefix(plus + 1);
	}
	if (text.empty()) return ImGuiKey_None;
	for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
	{
		const ImGuiKey key = static_cast<ImGuiKey>(k);
		if (!bindable(key)) continue;
		if (text == ImGui::GetKeyName(key)) return mods | key;
	}
	return ImGuiKey_None;
}

std::string encode()
{
	std::string out;
	// Table order, so the string is stable across runs (a map would shuffle it).
	for (const Action& a : table())
	{
		const auto it = overrides().find(a.id);
		if (it == overrides().end()) continue;
		if (!out.empty()) out += ';';
		out += a.id;
		out += '=';
		out += chordToText(it->second);
	}
	return out;
}

void decode(std::string_view text)
{
	overrides().clear();
	while (!text.empty())
	{
		const size_t semi = text.find(';');
		const std::string_view item = text.substr(0, semi);
		text = (semi == std::string_view::npos) ? std::string_view{} : text.substr(semi + 1);
		const size_t eq = item.find('=');
		if (eq == std::string_view::npos) continue;
		const std::string_view id = item.substr(0, eq);
		if (!find(id)) continue;   // an action a later build removed
		setChord(id, chordFromText(item.substr(eq + 1)));
	}
}

ImGuiKeyChord captureChord()
{
	const ImGuiIO& io = ImGui::GetIO();
	for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
	{
		const ImGuiKey key = static_cast<ImGuiKey>(k);
		if (!bindable(key)) continue;
		if (!ImGui::IsKeyPressed(key, false)) continue;
		ImGuiKeyChord c = key;
		if (io.KeyCtrl)  c |= ImGuiMod_Ctrl;
		if (io.KeyShift) c |= ImGuiMod_Shift;
		if (io.KeyAlt)   c |= ImGuiMod_Alt;
		if (io.KeySuper) c |= ImGuiMod_Super;
		return c;
	}
	return ImGuiKey_None;
}

} // namespace EditorShortcuts
#endif // HE_IMGUI_ENABLED
