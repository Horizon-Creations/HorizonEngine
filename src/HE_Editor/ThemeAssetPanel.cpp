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

void stylesSection(PanelState& st)
{
	HE::Ed::Help::Scope helpScope("Theme Styles");
	ImGui::Spacing();
	ImGui::SeparatorText("Styles");
	ImGui::TextDisabled("What a whole kind of element looks like. An element that follows a");
	ImGui::TextDisabled("style takes every value here in one decision — hover and pressed");
	ImGui::TextDisabled("included, which no single role can name.");

	if (EditorWidgets::button("Add Style", ImVec2(120.0f, 0.0f)))
		ImGui::OpenPopup("##addstyle");
	if (ImGui::BeginPopup("##addstyle"))
	{
		ImGui::TextDisabled("For an element type");
		for (HE::UIWidgetType t : HE::uiWidgetTypeRegistry())
		{
			const char* name = HE::uiWidgetTypeName(t);
			if (st.theme.styleFor(name)) continue;      // it already has one
			if (ImGui::Selectable(name))
			{
				st.theme.styleMut(name);
				st.dirty = true;
			}
		}
		ImGui::Separator();
		ImGui::TextDisabled("Or a name of your own");
		ImGui::SetNextItemWidth(180.0f);
		// Its own buffer, not the theme: an InputText writes on every keystroke,
		// so typing into the style list directly would create a style called "C"
		// on the way to "Card".
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
	else EditorWidgets::helpForLabel("Add Style");

	std::string removeStyle;
	for (auto& [key, style] : st.theme.styles)
	{
		ImGui::PushID(key.c_str());
		const std::string header = isTypeKey(key)
			? key + "  (every " + key + ")" : key + "  (by name)";
		if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
		{
			std::string removeProp;
			for (auto& [prop, v] : style.values) styleValueRow(st, key, prop, v, removeProp);
			if (!removeProp.empty()) { style.erase(removeProp); st.dirty = true; }

			if (EditorWidgets::smallButton("Add Value")) ImGui::OpenPopup("##addval");
			if (ImGui::BeginPopup("##addval"))
			{
				// A type-keyed style is offered its own type's properties. A
				// named one may dress anything, so it gets one submenu per type
				// — a flat list of every property in the engine is a list nobody
				// reads.
				auto offer = [&](HE::UIWidgetType t)
				{
					for (const HE::UIPropDesc& pd : styleableProps(t))
					{
						if (style.find(pd.name)) continue;
						if (!ImGui::Selectable(pd.name.c_str())) continue;
						HE::UIPropValue def;
						HE::UIThemeStyleValue v;
						if (defaultValueOf(t, pd.name, def))
						{
							v.isColor = def.type == HE::UIPropType::Color;
							v.number  = def.f;
							for (int m = 0; m < static_cast<int>(HE::UIThemeMode::COUNT); ++m)
								v.color[m] = def.col;
						}
						style.set(pd.name, v);
						st.dirty = true;
					}
				};
				if (isTypeKey(key)) offer(HE::uiWidgetTypeFromName(key));
				else
					for (HE::UIWidgetType t : HE::uiWidgetTypeRegistry())
						if (ImGui::BeginMenu(HE::uiWidgetTypeName(t)))
						{ offer(t); ImGui::EndMenu(); }
				ImGui::EndPopup();
			}
			ImGui::SameLine();
			if (EditorWidgets::dangerSmallButton("Remove Style")) removeStyle = key;
		}
		ImGui::PopID();
	}
	if (!removeStyle.empty())
	{
		// Elements pointing at it keep the values they last got — the same rule a
		// renamed role follows. Nothing is repainted white.
		st.theme.eraseStyle(removeStyle);
		st.dirty = true;
	}
	if (st.theme.styles.empty())
		ImGui::TextDisabled("(no styles yet — every element decides for itself)");
}

} // namespace

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
					st.dirty = true;
				}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(replaces every value below)");
	}
	ImGui::Spacing();

	ImGui::SeparatorText("Colours");
	ImGui::TextDisabled("Role");
	ImGui::SameLine(140.0f); ImGui::TextDisabled("Light");
	ImGui::SameLine();       ImGui::TextDisabled("Dark");
	for (int r = 0; r < static_cast<int>(HE::UIThemeRole::COUNT); ++r)
		roleRow(st, static_cast<HE::UIThemeRole>(r));

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
