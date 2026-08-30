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

	ImGui::End();
}
