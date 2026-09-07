#include "ThemeAssetPanel.h"
#include "EditorApplication.h"     // AppContext
#include "EditorAssetTypeCache.h"  // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"      // shared per-tab state map
#include "EditorToolbar.h"         // shared toolbar strip
#include "EditorHelp.h"            // Help::Scope — "Theme Editor/<label>"
#include "EditorWidgets.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>

#include <Types/Enums.h>
#include <UIWidget/UIElement.h>    // makeUIElement, uiBaseProperties — a style's keys
#include <UIWidget/UITheme.h>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <filesystem>
#include <string>
#include <vector>

namespace
{

struct PanelState
{
	bool        loaded = false;
	bool        dirty  = false;
	std::string relPath;
	HE::UUID    assetId;
	HE::UITheme theme;
	std::string lastSaveError;
	// Scratch buffer for "add a style with a name of my own". Never the theme
	// itself: an InputText writes on every keystroke, so typing straight into
	// the style list would leave a style called "C" behind on the way to "Card".
	std::string newStyleName;
	std::string newTagName;
};
AssetPanelState<PanelState> s_states;

bool saveState(PanelState& st, AppContext& ctx)
{
	if (!ctx.contentManager) return false;
	st.lastSaveError.clear();
	ThemeAsset* a = ctx.contentManager->getThemeMutable(st.assetId);
	if (!a)
	{
		st.lastSaveError = "Not saved: this theme asset is no longer loaded.";
		return false;
	}
	a->json = HE::uiThemeToJson(st.theme);
	if (!ctx.contentManager->saveAsset(*a))
	{
		st.lastSaveError = "Not saved: the file could not be written.";
		return false;
	}
	st.dirty = false;
	// If this IS the project's theme, the editor's own widget runtime is now a
	// version behind. Handing it over here is what makes an edit show up in an
	// open designer at once, instead of the next time the project is opened.
	if (ctx.projectManager &&
	    ctx.projectManager->currentProject().theme == st.relPath)
		ThemeAssetPanel::applyProjectTheme(ctx);
	return true;
}

// One role, both modes, on one line. The swatches sit next to each other because
// the question an author is actually answering is "do these two work in their
// own mode", and that is unanswerable when the other one is on a different tab.
void roleRow(PanelState& st, HE::UIThemeRole role)
{
	const char* name = HE::uiThemeRoleName(role);
	const int r = static_cast<int>(role);
	ImGui::PushID(r);
	ImGui::TextUnformatted(name);
	ImGui::SameLine(140.0f);
	constexpr ImGuiColorEditFlags kFlags =
		ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf |
		ImGuiColorEditFlags_AlphaBar;
	if (ImGui::ColorEdit4("##light",
	        &st.theme.color[r][static_cast<int>(HE::UIThemeMode::Light)].x, kFlags))
		st.dirty = true;
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s — Light", name);
	ImGui::SameLine();
	if (ImGui::ColorEdit4("##dark",
	        &st.theme.color[r][static_cast<int>(HE::UIThemeMode::Dark)].x, kFlags))
		st.dirty = true;
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s — Dark", name);
	ImGui::PopID();
}

// ── Styles ───────────────────────────────────────────────────────────────────
// A role is one colour and an element binds to it one value at a time. A style
// is the answer for a whole KIND of element at once, keyed by the property names
// the element already has — which is the only way "hover" and "pressed" can be
// themed at all: there is no role called "the accent, but hovered".
//
// Everything below is generated from the element types themselves
// (makeUIElement + its property table), so a new widget type shows up here with
// its own knobs and no line of code in this file.

// The colour and number properties of one element type, base ones included. The
// base list first, because "Corner Radius" and "Border Color" are what most
// styles are made of and they are the same on every type.
std::vector<HE::UIPropDesc> styleableProps(HE::UIWidgetType type)
{
	std::vector<HE::UIPropDesc> out;
	auto take = [&out](const std::vector<HE::UIPropDesc>& src)
	{
		for (const HE::UIPropDesc& p : src)
			if (p.type == HE::UIPropType::Color || p.type == HE::UIPropType::Float)
			{
				bool seen = false;
				for (const HE::UIPropDesc& have : out) if (have.name == p.name) seen = true;
				if (!seen) out.push_back(p);
			}
	};
	take(HE::uiBaseProperties());
	if (const std::unique_ptr<HE::UIElement> e = HE::makeUIElement(type)) take(e->properties());
	return out;
}

// What a property's value is on a freshly made element of that type — what a
// newly added style entry starts at, so switching a type over to the theme
// changes nothing until somebody actually edits a value.
bool defaultValueOf(HE::UIWidgetType type, const std::string& prop, HE::UIPropValue& out)
{
	const std::unique_ptr<HE::UIElement> e = HE::makeUIElement(type);
	if (!e) return false;
	out = e->getPropAny(prop);
	return out.type == HE::UIPropType::Color || out.type == HE::UIPropType::Float;
}

// Is this style key the name of an element type? Type-keyed styles are followed
// by every element of that type without being asked; free names are pointed at
// deliberately, and the two are worth telling apart on screen.
bool isTypeKey(const std::string& key)
{
	for (HE::UIWidgetType t : HE::uiWidgetTypeRegistry())
		if (key == HE::uiWidgetTypeName(t)) return true;
	return false;
}

void styleValueRow(PanelState& st, const std::string& key, const std::string& prop,
                   HE::UIThemeStyleValue& v, std::string& removeProp)
{
	ImGui::PushID(prop.c_str());
	ImGui::TextUnformatted(prop.c_str());
	ImGui::SameLine(180.0f);
	if (v.isColor)
	{
		constexpr ImGuiColorEditFlags kFlags =
			ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf |
			ImGuiColorEditFlags_AlphaBar;
		if (ImGui::ColorEdit4("##light",
		        &v.color[static_cast<int>(HE::UIThemeMode::Light)].x, kFlags))
			st.dirty = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s — Light", prop.c_str());
		ImGui::SameLine();
		if (ImGui::ColorEdit4("##dark",
		        &v.color[static_cast<int>(HE::UIThemeMode::Dark)].x, kFlags))
			st.dirty = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s — Dark", prop.c_str());
	}
	else
	{
		// One value, not two: a corner radius is not lighter in light mode.
		ImGui::SetNextItemWidth(110.0f);
		if (ImGui::DragFloat("##num", &v.number, 0.5f)) st.dirty = true;
	}
	ImGui::SameLine();
	if (EditorWidgets::smallButton("x")) removeProp = prop;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Take this value out of the style.\nThe element keeps whatever it has.");
	ImGui::PopID();
	(void)key;
}

// One entry a style could hold, seeded from a freshly made element of `t`, so
// adding it changes nothing until somebody edits the value.
HE::UIThemeStyleValue seedValue(HE::UIWidgetType t, const std::string& prop)
{
	HE::UIThemeStyleValue v;
	HE::UIPropValue def;
	if (defaultValueOf(t, prop, def))
	{
		v.isColor = def.type == HE::UIPropType::Color;
		v.number  = def.f;
		for (int m = 0; m < static_cast<int>(HE::UIThemeMode::COUNT); ++m)
			v.color[m] = def.col;
	}
	return v;
}

// Give every element type a style holding every value it has, at that type's own
// defaults. Called when a theme is opened and after one is replaced.
//
// Eagerly, not on demand: a theme's job is to answer "what does a Button look
// like" for the whole project, and an editor that starts empty makes an author
// add a style and then thirteen values before answering anything. Since every
// value starts where the type itself starts, a freshly seeded theme changes
// nothing on screen — it is a form to fill in, not an edit.
//
// Existing values are left exactly as they are, so this is safe over a theme
// somebody has already worked on, and it is what makes a NEW element type appear
// in every theme that was authored before it existed.
void ensureTypeStyles(HE::UITheme& theme)
{
	for (HE::UIWidgetType t : HE::uiWidgetTypeRegistry())
	{
		HE::UIThemeStyle& s = theme.styleMut(HE::uiWidgetTypeName(t));
		for (const HE::UIPropDesc& pd : styleableProps(t))
			if (!s.find(pd.name)) s.set(pd.name, seedValue(t, pd.name));
	}
}

// The values of one style. `owner` is the type whose properties it decides —
// only meaningful when `ownerIsType`, since a style with a name of its own may
// dress anything and therefore gets one submenu per type.
//
// A type's own style is complete by construction (ensureTypeStyles), so it needs
// no buttons at all: the list IS the style. A VARIANT is the opposite — it says
// only what differs — so that one gets Add Value and Remove.
// Returns true when the caller should delete this style.
bool styleBody(PanelState& st, const std::string& key, HE::UIThemeStyle& style,
               HE::UIWidgetType owner, bool ownerIsType, bool isBase)
{
	// Its own scope, though the only caller already pushes the same one: a helper
	// that names its scope is a helper the coverage audit can place, and one that
	// borrows the caller's is filed under whichever function sits above it.
	HE::Ed::Help::Scope helpScope("Theme Styles");
	bool remove = false;
	ImGui::PushID(key.c_str());

	// Colours first, as a block with its two columns titled, then the numbers.
	// The property table's order is the order the ELEMENT wants (its own fields
	// after the shared ones), which puts a rounding between two colours and makes
	// the two swatch columns restart every few rows. What an author is doing here
	// is comparing colours down a column, so the columns have to be columns.
	std::string removeProp;
	bool anyColor = false;
	for (const auto& [prop, v] : style.values) if (v.isColor) anyColor = true;
	if (anyColor)
	{
		ImGui::TextDisabled("Colour");
		ImGui::SameLine(180.0f); ImGui::TextDisabled("Light");
		ImGui::SameLine();       ImGui::TextDisabled("Dark");
	}
	for (auto& [prop, v] : style.values)
		if (v.isColor) styleValueRow(st, key, prop, v, removeProp);
	bool anyNumber = false;
	for (const auto& [prop, v] : style.values) if (!v.isColor) anyNumber = true;
	if (anyNumber)
	{
		if (anyColor) ImGui::Spacing();
		ImGui::TextDisabled("Number");
	}
	for (auto& [prop, v] : style.values)
		if (!v.isColor) styleValueRow(st, key, prop, v, removeProp);
	if (!removeProp.empty()) { style.erase(removeProp); st.dirty = true; }
	if (style.values.empty())
		ImGui::TextDisabled("(decides nothing yet — Add Value)");

	// A type's own style holds everything that type has, always, so there is
	// nothing to add and nothing to remove. Only a variant needs the buttons.
	if (isBase) { ImGui::PopID(); return false; }

	auto offer = [&](HE::UIWidgetType t)
	{
		for (const HE::UIPropDesc& pd : styleableProps(t))
		{
			if (style.find(pd.name)) continue;
			if (ImGui::Selectable(pd.name.c_str()))
			{
				style.set(pd.name, seedValue(t, pd.name));
				st.dirty = true;
			}
		}
	};

	if (EditorWidgets::smallButton("Add Value")) ImGui::OpenPopup("##addval");
	if (ImGui::BeginPopup("##addval"))
	{
		if (ownerIsType) offer(owner);
		else
			for (HE::UIWidgetType t : HE::uiWidgetTypeRegistry())
				if (ImGui::BeginMenu(HE::uiWidgetTypeName(t)))
				{ offer(t); ImGui::EndMenu(); }
		ImGui::EndPopup();
	}
	else EditorWidgets::helpForLabel("Add Value");

	ImGui::SameLine();
	if (EditorWidgets::dangerSmallButton("Remove Style")) remove = true;
	EditorWidgets::helpForLabel("Remove Style");

	ImGui::PopID();
	return remove;
}

void stylesSection(PanelState& st)
{
	HE::Ed::Help::Scope helpScope("Theme Styles");
	ImGui::Spacing();
	ImGui::SeparatorText("Styles");
	ImGui::TextDisabled("What a whole kind of element looks like. Every element type is here");
	ImGui::TextDisabled("with every value it has — set them once and every element of that");
	ImGui::TextDisabled("type follows. A VARIANT is a tag an element carries, so");
	ImGui::TextDisabled("\"Button.success\" is a button that takes everything Button says");
	ImGui::TextDisabled("and then the green on top.");

	std::string removeStyle;

	// ── Every element type, complete ─────────────────────────────────────────
	// ensureTypeStyles has already given each one every value it has, so this is
	// a list to edit rather than a list to build. "Which types can I theme" is
	// answered by looking.
	for (HE::UIWidgetType t : HE::uiWidgetTypeRegistry())
	{
		const std::string type = HE::uiWidgetTypeName(t);
		const std::vector<std::string> tags = st.theme.tagsFor(type);
		HE::UIThemeStyle& base = st.theme.styleMut(type);
		const int variants = static_cast<int>(tags.size());

		// The header says at a glance what is here, so a folded list is still
		// readable — and it ends in "###type" so that the ID does NOT depend on
		// the numbers in it. ImGui keys a header's open state on its label, so a
		// count in the label means adding a value folds the section you added it
		// to, which is exactly what it did.
		std::string header = type + "  (" + std::to_string(base.values.size()) + " values";
		if (variants) header += ", " + std::to_string(variants) + " variant" +
		                        (variants == 1 ? "" : "s");
		header += ")###" + type;

		ImGui::PushID(type.c_str());
		if (ImGui::CollapsingHeader(header.c_str()))
		{
			styleBody(st, type, base, t, true, /*isBase=*/true);

			for (const std::string& tag : tags)
			{
				const std::string key = HE::uiThemeSelector(type, tag);
				ImGui::SeparatorText(key.c_str());
				HE::UIThemeStyle* v = const_cast<HE::UIThemeStyle*>(st.theme.styleFor(key));
				if (v && styleBody(st, key, *v, t, true, /*isBase=*/false)) removeStyle = key;
			}

			ImGui::Spacing();
			if (EditorWidgets::smallButton("Add Variant")) ImGui::OpenPopup("##addtag");
			if (ImGui::BeginPopup("##addtag"))
			{
				ImGui::TextDisabled("A tag an element of this type can carry");
				ImGui::SetNextItemWidth(180.0f);
				// Its own buffer, not the theme: an InputText writes on every
				// keystroke, so typing straight into the style list would leave a
				// "Button.s" behind on the way to "Button.success".
				if (ImGui::InputText("##newtag", &st.newTagName,
				                     ImGuiInputTextFlags_EnterReturnsTrue) &&
				    !st.newTagName.empty())
				{
					st.theme.styleMut(HE::uiThemeSelector(type, st.newTagName));
					st.newTagName.clear();
					st.dirty = true;
					ImGui::CloseCurrentPopup();
				}
				ImGui::TextDisabled("(type a name, then Enter)");
				ImGui::EndPopup();
			}
			else EditorWidgets::helpForLabel("Add Variant");
		}
		ImGui::PopID();
	}

	// ── Styles with a name of their own ──────────────────────────────────────
	// A look that is not tied to one type: an element points at it by name, and
	// it layers over whatever its type already said.
	ImGui::Spacing();
	ImGui::SeparatorText("By name");
	int named = 0;
	for (auto& [key, style] : st.theme.styles)
	{
		if (isTypeKey(HE::uiThemeSelectorType(key))) continue;
		++named;
		ImGui::PushID(key.c_str());
		// "###key" for the same reason the type headers have it: the count in the
		// label must not be part of the ID, or adding a value folds the section.
		if (ImGui::CollapsingHeader((key + "  (" + std::to_string(style.values.size()) +
		                             " values)###" + key).c_str()))
			if (styleBody(st, key, style, HE::UIWidgetType::Panel, false, /*isBase=*/false))
				removeStyle = key;
		ImGui::PopID();
	}
	if (named == 0) ImGui::TextDisabled("(none yet)");
	if (EditorWidgets::smallButton("Add Named Style")) ImGui::OpenPopup("##addnamed");
	if (ImGui::BeginPopup("##addnamed"))
	{
		ImGui::SetNextItemWidth(180.0f);
		if (ImGui::InputText("##newstyle", &st.newStyleName,
		                     ImGuiInputTextFlags_EnterReturnsTrue) &&
		    !st.newStyleName.empty())
		{
			st.theme.styleMut(st.newStyleName);
			st.newStyleName.clear();
			st.dirty = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::TextDisabled("(type a name, then Enter)");
		ImGui::EndPopup();
	}
	else EditorWidgets::helpForLabel("Add Named Style");

	if (!removeStyle.empty())
	{
		// Elements pointing at it keep the values they last got — the same rule a
		// renamed role follows. Nothing is repainted white.
		st.theme.eraseStyle(removeStyle);
		st.dirty = true;
	}
}

} // namespace

void ThemeAssetPanel::applyProjectTheme(AppContext& ctx)
{
	if (!ctx.world || !ctx.contentManager || !ctx.projectManager) return;
	const ProjectData& proj = ctx.projectManager->currentProject();
	// The mode first, and whatever the theme turns out to be: "System" is a rule
	// and a project that names no theme still gets to say which half of the
	// default it starts in.
	ctx.world->widgets().setThemePreference(
		HE::uiThemePreferenceFromName(proj.themeMode.empty() ? "System" : proj.themeMode));
	if (proj.theme.empty()) return;

	const HE::UUID id = ctx.contentManager->loadAsset(proj.theme);
	const ThemeAsset* a = id == HE::UUID{} ? nullptr : ctx.contentManager->getTheme(id);
	if (!a)
	{
		HE_LOG_WARN(Editor, "Project theme '%s' could not be loaded", proj.theme.c_str());
		return;
	}
	HE::UITheme t;
	// Read the JSON out and parse it here: the getter points into the content
	// manager's dense asset vector, and one more load would move it.
	if (!HE::uiThemeFromJson(a->json, t))
	{
		HE_LOG_WARN(Editor, "Project theme '%s' is not readable", proj.theme.c_str());
		return;
	}
	ctx.world->widgets().setTheme(t);
}

bool ThemeAssetPanel::isThemeAsset(const std::string& path)
{ return EditorAssetTypeCache::is(path, HE::AssetType::Theme); }

bool ThemeAssetPanel::isDirty(const std::string& path) { return s_states.dirty(path); }

bool ThemeAssetPanel::reloadFromDisk(const std::string& assetPath)
{
	auto* st = s_states.find(assetPath);
	if (!st) return false;
	st->loaded = false;
	st->dirty  = false;
	return true;
}

void ThemeAssetPanel::appendDirtyPaths(std::vector<std::string>& out)
{ s_states.appendDirtyPaths(out); }

bool ThemeAssetPanel::save(AppContext& ctx, const std::string& path)
{
	PanelState* st = s_states.find(path);
	if (!st || !st->dirty) return true;   // "not mine" reads as success
	return saveState(*st, ctx);
}

void ThemeAssetPanel::forget(const std::string& path) { s_states.forget(path); }

void ThemeAssetPanel::render(AppContext& ctx, const std::string& assetPath,
                             const ImVec2& pos, const ImVec2& size)
{
	PanelState& st = s_states[assetPath];
	if (!st.loaded && ctx.contentManager)
	{
		st.relPath = ctx.contentManager->toContentRelativePath(assetPath);
		st.assetId = ctx.contentManager->loadAsset(st.relPath);
		st.theme   = HE::uiDefaultTheme();
		if (const ThemeAsset* a = ctx.contentManager->getTheme(st.assetId))
			HE::uiThemeFromJson(a->json, st.theme);
		st.theme.name = std::filesystem::path(assetPath).stem().string();
		// Every element type gets its full set of values, here rather than in the
		// format: a theme file written before a type existed is completed on the
		// way in, and nothing on screen changes because each new value starts at
		// that type's own default. The theme is only WRITTEN on save, so opening
		// one and closing it again leaves the file alone.
		ensureTypeStyles(st.theme);
		st.loaded = true;
	}

	HE::Ed::Help::Scope helpScope("Theme Editor");
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin(("##themeasset_" + assetPath).c_str(), nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

	{
		namespace T = EditorToolbar;
		T::Bar bar;
		T::assetHeader(bar, st.theme.name.c_str(), T::iconGear, st.dirty);
		bar.group();
		bar.readout(nullptr, "Theme", T::kFgDim);
		bar.endGroup();
		if (T::saveButton(bar, true)) saveState(st, ctx);
	}
	if (!st.lastSaveError.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 80, 80, 255));
		ImGui::TextWrapped("%s", st.lastSaveError.c_str());
		ImGui::PopStyleColor();
	}

	{
		EditorWidgets::WrapText wrap;
		ImGui::TextDisabled("Nine roles, each in a light and a dark value. Widgets point at a");
		ImGui::TextDisabled("role instead of carrying a colour, so one edit here changes all of");
		ImGui::TextDisabled("them — and switching mode is a switch, not a second set of widgets.");
	}

	// ── Which theme the PROJECT uses ─────────────────────────────────────────
	// Here rather than in a settings window, for the same reason the savegame
	// template's "set as default" is on the template: the answer belongs to the
	// thing you are looking at, and a project setting three menus away is one
	// nobody finds.
	if (ctx.projectManager)
	{
		ProjectData& proj = ctx.projectManager->currentProject();
		const bool isProjectTheme = proj.theme == st.relPath;
		ImGui::Spacing();
		if (isProjectTheme)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 210, 140, 255));
			ImGui::TextUnformatted("This project's theme — the exported application boots with it.");
			ImGui::PopStyleColor();
		}
		else if (EditorWidgets::button("Use for this Project", ImVec2(200.0f, 0.0f)))
		{
			proj.theme = st.relPath;
			ctx.projectManager->saveProject(proj.path);
			// …and take effect here and now. The editor's widget runtime is what
			// the live preview and the designer both read, so without this the
			// project theme would be a promise the editor never keeps.
			ThemeAssetPanel::applyProjectTheme(ctx);
		}

		// The mode is the PROJECT's, not this asset's: both belong to the same
		// theme, and which of them an application starts in is a decision about
		// the application.
		const HE::UIThemePreference pref = HE::uiThemePreferenceFromName(proj.themeMode);
		ImGui::SetNextItemWidth(160.0f);
		if (ImGui::BeginCombo("Starts in", HE::uiThemePreferenceName(pref)))
		{
			for (int i = 0; i < static_cast<int>(HE::UIThemePreference::COUNT); ++i)
			{
				const auto p = static_cast<HE::UIThemePreference>(i);
				if (ImGui::Selectable(HE::uiThemePreferenceName(p), p == pref))
				{
					proj.themeMode = HE::uiThemePreferenceName(p);
					ctx.projectManager->saveProject(proj.path);
					ThemeAssetPanel::applyProjectTheme(ctx);
				}
			}
			ImGui::EndCombo();
		}
		else EditorWidgets::helpForLabel("Starts in");
	}
	ImGui::Spacing();

	// ── Starting from one of the shipped palettes ────────────────────────────
	// The two built-in themes are C++ (HE::uiDefaultTheme / uiAmberTheme) rather
	// than files: EngineContent's real payload is fetched over SFTP and the
	// repository keeps only its folder structure, so a checked-in theme asset
	// would be the one thing in there nobody could regenerate. Offering them
	// HERE is better than shipping a file anyway — an author gets a palette AND
	// an asset they can edit, in one click.
	{
		ImGui::SetNextItemWidth(160.0f);
		const bool open = ImGui::BeginCombo("Start from", "Shipped palette");
		if (!open) EditorWidgets::helpForLabel("Start from");
		if (open)
		{
			for (const HE::UITheme* preset : { &HE::uiDefaultTheme(), &HE::uiAmberTheme() })
				if (ImGui::Selectable(preset->name.c_str()))
				{
					// The NAME is the asset's, not the palette's: this replaces
					// what the theme looks like, not what it is called.
					const std::string keep = st.theme.name;
					st.theme = *preset;
					st.theme.name = keep;
					// A palette answers for the types it cares about; the rest are
					// filled in at their defaults, so the list below stays complete
					// whichever one is taken.
					ensureTypeStyles(st.theme);
					st.dirty = true;
				}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(replaces every value below)");
	}
	ImGui::Spacing();

	// ── Globals ──────────────────────────────────────────────────────────────
	// What a colour MEANS, rather than what any one control looks like. Since
	// every element type now carries its own style below, the ten roles are no
	// longer where an interface is coloured — three of them are still worth
	// having in one place, because "this went wrong" has to look the same on a
	// label, a banner and a border, and no per-type style can say that.
	ImGui::SeparatorText("Globals");
	ImGui::TextDisabled("Meaning");
	ImGui::SameLine(140.0f); ImGui::TextDisabled("Light");
	ImGui::SameLine();       ImGui::TextDisabled("Dark");
	roleRow(st, HE::UIThemeRole::Success);
	roleRow(st, HE::UIThemeRole::Warning);
	roleRow(st, HE::UIThemeRole::Error);
	roleRow(st, HE::UIThemeRole::Accent);
	// Directly under the accent, because it is the other half of the same
	// decision: change one and the pair has to be looked at again.
	roleRow(st, HE::UIThemeRole::AccentText);

	// The remaining five are still a stored format: an element bound to
	// "Surface" by hand resolves against this and would otherwise hold a value
	// nobody can reach. Folded away rather than deleted, because a value with no
	// editor is worse than one out of the way.
	if (ImGui::CollapsingHeader("Surfaces and text (bound by hand)"))
	{
		ImGui::TextDisabled("Only elements bound to one of these by name use them.");
		ImGui::TextDisabled("What a Panel or a Text looks like is its style, below.");
		roleRow(st, HE::UIThemeRole::Background);
		roleRow(st, HE::UIThemeRole::Surface);
		roleRow(st, HE::UIThemeRole::Border);
		roleRow(st, HE::UIThemeRole::Text);
		roleRow(st, HE::UIThemeRole::MutedText);
	}

	ImGui::Spacing();
	ImGui::SeparatorText("Sizes");
	for (int s = 0; s < static_cast<int>(HE::UIThemeSize::COUNT); ++s)
	{
		const auto size_ = static_cast<HE::UIThemeSize>(s);
		ImGui::PushID(1000 + s);
		ImGui::SetNextItemWidth(90.0f);
		if (ImGui::DragFloat((std::string("Radius ") + HE::uiThemeSizeName(size_)).c_str(),
		                     &st.theme.radius[s], 0.5f, 0.0f, 500.0f))
			st.dirty = true;
		ImGui::SameLine();
		ImGui::SetNextItemWidth(90.0f);
		if (ImGui::DragFloat((std::string("Spacing ") + HE::uiThemeSizeName(size_)).c_str(),
		                     &st.theme.spacing[s], 0.5f, 0.0f, 500.0f))
			st.dirty = true;
		ImGui::PopID();
	}

	ImGui::Spacing();
	ImGui::SeparatorText("Text");
	for (int l = 0; l < static_cast<int>(HE::UIThemeTextLevel::COUNT); ++l)
	{
		ImGui::SetNextItemWidth(120.0f);
		if (ImGui::DragFloat(HE::uiThemeTextLevelName(static_cast<HE::UIThemeTextLevel>(l)),
		                     &st.theme.textSize[l], 0.5f, 4.0f, 400.0f))
			st.dirty = true;
	}

	ImGui::Spacing();
	ImGui::SeparatorText("Shadows");
	for (int e = 0; e < static_cast<int>(HE::UIThemeElevation::COUNT); ++e)
	{
		ImGui::PushID(2000 + e);
		ImGui::TextUnformatted(HE::uiThemeElevationName(static_cast<HE::UIThemeElevation>(e)));
		ImGui::SameLine(140.0f);
		if (ImGui::ColorEdit4("##col", &st.theme.shadow[e].color.x,
		                      ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
			st.dirty = true;
		ImGui::SameLine();
		ImGui::SetNextItemWidth(90.0f);
		if (ImGui::DragFloat("Blur", &st.theme.shadow[e].blur, 0.5f, 0.0f, 500.0f))
			st.dirty = true;
		ImGui::SameLine();
		ImGui::SetNextItemWidth(120.0f);
		float off[2] = { st.theme.shadow[e].offsetX, st.theme.shadow[e].offsetY };
		if (ImGui::DragFloat2("Offset", off, 0.5f))
		{ st.theme.shadow[e].offsetX = off[0]; st.theme.shadow[e].offsetY = off[1]; st.dirty = true; }
		ImGui::PopID();
	}

	stylesSection(st);

	ImGui::End();
}
