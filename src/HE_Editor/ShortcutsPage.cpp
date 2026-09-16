#include "ShortcutsPage.h"

#ifdef HE_IMGUI_ENABLED
#include "EditorShortcuts.h"
#include "EditorWidgets.h"       // hint, button, helpForLabel, WrapText
#include "EditorHelp.h"          // the page's scope
#include "EditorTheme.h"         // the accent for a changed chord
#include "NotificationStore.h"   // a config write that fails has to say so
#include <Diagnostics/GlobalState.h>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <string>
#include <vector>

namespace ShortcutsPage
{

// What the page is capturing for, "" when idle. Page-local rather than in
// EditorShortcuts: the capture is a UI state, the table is data.
static std::string s_captureId;   // see capturing()
static char        s_shortcutFilter[64] = "";

static void persistShortcuts()
{
	GlobalState& gs = GlobalState::getInstance();
	gs.setCustomConfigEntry(kConfigKey, EditorShortcuts::encode());
	if (!gs.writeConfig())
		HE::Ed::notify(HE::Ed::NoteLevel::Problem,
		               "Could not save the shortcut",
		               "The binding applies now, but this session's config file could "
		               "not be written — the next launch will have the old key.");
}

void draw()
{
	using namespace EditorShortcuts;
	HE::Ed::Help::Scope helpScope("Shortcuts");
	EditorWidgets::WrapText wrap;

	EditorWidgets::hint("Click a shortcut and press the new keys. Esc keeps the old one, "
	                    "Backspace removes it. The shortcuts that belong to one editor "
	                    "\xe2\x80\x94 the graph, the material editor, the sequencer \xe2\x80\x94 "
	                    "are not listed; they are those editors' own.");
	ImGui::Spacing();

	ImGui::SetNextItemWidth(220.0f);
	ImGui::InputTextWithHint("Search", "action or key", s_shortcutFilter, sizeof(s_shortcutFilter));
	EditorWidgets::helpForLabel("Search");
	ImGui::SameLine();
	if (EditorWidgets::button("Reset All"))
	{
		resetAll();
		s_captureId.clear();
		persistShortcuts();
	}
	EditorWidgets::helpForLabel("Reset All");
	ImGui::Spacing();

	// The capture, before the rows: a key that lands on this frame must not
	// also reach the row's own widgets, and the arming click was last frame.
	if (!s_captureId.empty())
	{
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
			s_captureId.clear();
		else if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
		{
			setChord(s_captureId, ImGuiKey_None);
			s_captureId.clear();
			persistShortcuts();
		}
		else if (const ImGuiKeyChord c = captureChord(); c != ImGuiKey_None)
		{
			setChord(s_captureId, c);
			s_captureId.clear();
			persistShortcuts();
		}
	}

	// Case-insensitive "does the row mention the filter" — action, category
	// or the key itself, so "ctrl" finds every chord that carries it.
	const auto matches = [](const Action& a)
	{
		if (s_shortcutFilter[0] == '\0') return true;
		std::string needle = s_shortcutFilter, hay = std::string(a.label) + ' ' + a.category + ' ' + label(a.id);
		std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch){ return (char)std::tolower(ch); });
		std::transform(hay.begin(), hay.end(), hay.begin(),         [](unsigned char ch){ return (char)std::tolower(ch); });
		return hay.find(needle) != std::string::npos;
	};

	const float keyW = std::max(130.0f, ImGui::CalcTextSize("Ctrl+Shift+Backspace").x + 16.0f);
	if (!ImGui::BeginTable("##shortcuts", 3,
	                       ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX))
		return;
	ImGui::TableSetupColumn("Action",   ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthFixed, keyW);
	ImGui::TableSetupColumn("##reset",  ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Reset").x + 16.0f);

	const char* lastCategory = nullptr;
	for (const Action& a : actions())
	{
		if (!matches(a)) continue;
		if (!lastCategory || std::strcmp(lastCategory, a.category) != 0)
		{
			lastCategory = a.category;
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
			ImGui::TextUnformatted(a.category);
			ImGui::PopStyleColor();
		}
		ImGui::PushID(a.id);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(a.label);
		// The native macOS menu keeps the default key working through the OS
		// even after a rebind here; say so where the user would wonder.
#ifdef __APPLE__
		if (a.nativeOnMac && !isDefault(a.id))
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(%s stays on the menu)", chordLabel(a.defaultChord).c_str());
		}
#endif

		ImGui::TableSetColumnIndex(1);
		const bool capturing = (s_captureId == a.id);
		const std::vector<const Action*> clash = conflicts(a.id);
		std::string text = capturing ? std::string("Press keys\xe2\x80\xa6") : label(a.id);
		if (text.empty()) text = "(none)";
		if (capturing)    ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::AccentBright);
		else if (!clash.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.30f, 1.0f));
		else if (!isDefault(a.id)) ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::AccentHi);
		else              ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::Text);
		if (ImGui::Button(text.c_str(), ImVec2(-FLT_MIN, 0.0f)))
			s_captureId = capturing ? std::string() : std::string(a.id);
		ImGui::PopStyleColor();
		if (!clash.empty() && ImGui::IsItemHovered())
		{
			std::string who;
			for (const Action* o : clash) { if (!who.empty()) who += ", "; who += o->label; }
			ImGui::SetTooltip("Also bound to: %s", who.c_str());
		}
		else EditorWidgets::helpForKey("shortcuts.binding");

		ImGui::TableSetColumnIndex(2);
		if (!isDefault(a.id))
		{
			if (EditorWidgets::smallButton("Reset"))
			{
				reset(a.id);
				if (capturing) s_captureId.clear();
				persistShortcuts();
			}
			EditorWidgets::helpForLabel("Reset");
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
}

const char* capturing() { return s_captureId.c_str(); }

} // namespace ShortcutsPage
#endif // HE_IMGUI_ENABLED
