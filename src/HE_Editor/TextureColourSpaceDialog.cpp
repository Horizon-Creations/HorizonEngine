#include "TextureColourSpaceDialog.h"
#include "CollabController.h"    // AssetWriteLease / publishReimport around a retag
#include "EditorApplication.h"   // AppContext
#include "EditorHelp.h"          // the "Texture Color Space" scope for the buttons
#include "EditorRewards.h"       // the confirmed import's footer moment
#include "EditorWidgets.h"       // pinDialogToEditorWindow, the button verbs, WrapText
#include "ImporterCommon.h"      // importSource, suggestTextureSrgb, setTextureSrgb
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Logger.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace TextureColourSpaceDialog
{

namespace
{
constexpr const char* kTitle = "Texture Color Space##texture_colour_space";

enum class Mode { Import, Retag };

struct Row
{
	std::string         path;      // absolute: the source (Import) or the .hasset (Retag)
	std::string         relDir;    // Import only: where under the root it lands
	std::string         label;     // what the row shows (file name)
	std::optional<bool> current;   // Retag only: the flag on disk, unset = none recorded
	bool                suggested = true;
	bool                srgb      = true;   // the tick
};

// Plain data, set by open*() and consumed by Draw(): the call that opens the
// dialog is a menu item whose popup closes in the same frame, and the content
// tree it came from is rebuilt underneath the dialog after an import.
Mode             s_mode = Mode::Import;
std::vector<Row> s_rows;
std::string      s_root;
bool             s_pendingOpen = false;

// Retag rows guess from the SOURCE's name when one is recorded (it is the file
// the artist named; the asset may have been renamed since), else the asset's.
bool suggestionFor(const std::string& path, Mode mode)
{
	if (mode == Mode::Retag)
	{
		const std::string source = Importer::sourceFileOf(path);
		if (!source.empty()) return Importer::suggestTextureSrgb(source);
	}
	return Importer::suggestTextureSrgb(path);
}

bool changes(const Row& r)
{
	// A texture with no flag on disk samples linear, so that is what the tick
	// is compared against.
	return s_mode == Mode::Import || r.srgb != r.current.value_or(false);
}

#ifdef HE_IMGUI_ENABLED
// The session key for a content file, the way the Content Browser derives it
// (its collabKeyFor): the editor's mapping when there is one, else the content-
// relative form.
std::string collabKeyFor(AppContext& ctx, const std::string& absPath)
{
	if (ctx.collabKeyForPath) return ctx.collabKeyForPath(absPath, false);
	return ctx.contentManager ? ctx.contentManager->toContentRelativePath(absPath)
	                          : std::string();
}

void applyImport(AppContext& ctx)
{
	size_t imported = 0;
	for (const Row& r : s_rows)
	{
		Importer::ImportOptions options;
		options.textureSrgb = r.srgb;
		if (Importer::importSource(r.path, s_root, r.relDir, {}, options)) ++imported;
		else HE_LOG_ERROR(Editor, "%s", ("Editor: import failed for " + r.path).c_str());
	}
	HE_LOG_INFO(Editor, "%s",
		("Editor: imported " + std::to_string(imported) + " of "
		 + std::to_string(s_rows.size()) + " texture(s)").c_str());
	// Reward moment (EditorRewards.h): AssetsImported — the confirmed texture
	// batch, one moment with its count. A retag (applyRetag) is not an import.
	if (imported > 0)
		HE::Ed::Rewards::fire(ctx, HE::Ed::Rewards::Moment::AssetsImported,
		                      static_cast<int>(imported));
	ctx.contentRefreshPending = true;
}

void applyRetag(AppContext& ctx)
{
	size_t changed = 0, failed = 0;
	for (const Row& r : s_rows)
	{
		if (!changes(r)) continue;
		// The same arbitration a Reimport takes (ContentBrowserPanel, "Reimport"):
		// this REPLACES a file everyone in a session shares, so the lock is held
		// from before the write until the announcement is out. Outside a session
		// the lease permits the write.
		const std::string key = collabKeyFor(ctx, r.path);
		CollabController::AssetWriteLease lease =
			CollabController::beginBackgroundWrite(ctx.collab, key);
		if (!lease)
		{
			HE_LOG_WARN(Editor, "%s",
				("Editor: " + r.label + " is being written by someone else, left as it is").c_str());
			++failed;
			continue;
		}
		if (!Importer::setTextureSrgb(r.path, s_root, r.srgb)) { ++failed; continue; }
		if (ctx.collab && !key.empty()) ctx.collab->publishReimport(key, r.path);
		++changed;
	}
	HE_LOG_INFO(Editor, "%s",
		("Editor: color space changed on " + std::to_string(changed) + " texture(s)"
		 + (failed ? ", " + std::to_string(failed) + " failed" : std::string())).c_str());
	if (changed) ctx.contentRefreshPending = true;
}
#endif
} // namespace

void openImport(const std::vector<std::string>& sources,
                const std::vector<std::string>& relDirs,
                const std::string&              contentRoot)
{
	s_mode = Mode::Import;
	s_root = contentRoot;
	s_rows.clear();
	for (size_t i = 0; i < sources.size(); ++i)
	{
		Row r;
		r.path      = sources[i];
		r.relDir    = i < relDirs.size() ? relDirs[i] : std::string();
		r.label     = std::filesystem::path(r.path).filename().string();
		r.suggested = suggestionFor(r.path, Mode::Import);
		r.srgb      = r.suggested;
		s_rows.push_back(std::move(r));
	}
	s_pendingOpen = !s_rows.empty();
}

void openRetag(const std::vector<std::string>& assets, const std::string& contentRoot)
{
	s_mode = Mode::Retag;
	s_root = contentRoot;
	s_rows.clear();
	const std::filesystem::path root(contentRoot);
	for (const std::string& a : assets)
	{
		std::error_code ec;
		const std::filesystem::path rel = std::filesystem::relative(a, root, ec);
		if (ec || rel.empty() || *rel.begin() == "..") continue;
		Row r;
		r.path      = a;
		r.label     = rel.generic_string();
		r.current   = Importer::textureSrgbOf(a);
		r.suggested = suggestionFor(a, Mode::Retag);
		// Pre-set to the guess, not to what is on disk: the batch is for textures
		// that were never decided, and those all read "linear". The rows that
		// would change are marked, and Keep Current puts every tick back.
		r.srgb      = r.suggested;
		s_rows.push_back(std::move(r));
	}
	std::sort(s_rows.begin(), s_rows.end(),
	          [](const Row& x, const Row& y) { return x.label < y.label; });
	s_pendingOpen = !s_rows.empty();
}

void Draw(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	// Never over another root-level modal (OpenPopup at root REPLACES it) and
	// never over the content refresh an import has just asked for; the request
	// waits, since it is still there next frame.
	if (s_pendingOpen && !ctx.contentRefreshPending && !ctx.contentRefreshDone &&
	    !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
	{
		ImGui::OpenPopup(kTitle);
		s_pendingOpen = false;
	}

	constexpr float kDialogWidth = 560.0f;
	ImGui::SetNextWindowSize(ImVec2(kDialogWidth, 0.0f), ImGuiCond_Appearing);
	EditorWidgets::pinDialogToEditorWindow();
	if (!ImGui::BeginPopupModal(kTitle, nullptr,
	                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
	                            ImGuiWindowFlags_NoSavedSettings))
		return;

	HE::Ed::Help::Scope helpScope("Texture Color Space");
	const bool importing = s_mode == Mode::Import;

	{
		EditorWidgets::WrapText wrap(kDialogWidth - ImGui::GetStyle().WindowPadding.x);
		ImGui::TextWrapped(
			"Tick textures that hold COLOR (albedo, base color, emissive, UI art): they are "
			"stored sRGB-encoded and the GPU decodes them. Leave DATA unticked (normal, "
			"roughness, metalness, AO, ORM, height, masks): decoding those would bend "
			"their values. The ticks are guessed from the file names.");
		if (!importing)
			ImGui::TextWrapped(
				"Textures already on screen keep their old look until they are loaded "
				"again, e.g. by reopening the project.");
	}
	ImGui::Spacing();

	if (EditorWidgets::smallButton("All Color"))
		for (Row& r : s_rows) r.srgb = true;
	ImGui::SameLine();
	if (EditorWidgets::smallButton("All Data"))
		for (Row& r : s_rows) r.srgb = false;
	ImGui::SameLine();
	if (EditorWidgets::smallButton("Guess from Name"))
		for (Row& r : s_rows) r.srgb = r.suggested;
	if (!importing)
	{
		ImGui::SameLine();
		if (EditorWidgets::smallButton("Keep Current"))
			for (Row& r : s_rows) r.srgb = r.current.value_or(false);
	}

	const float rowH   = ImGui::GetFrameHeightWithSpacing();
	const float tableH = std::min(rowH * (static_cast<float>(s_rows.size()) + 1.5f), 340.0f);
	const int   cols   = importing ? 2 : 3;
	if (ImGui::BeginTable("##texture_rows", cols,
	                      ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
	                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
	                      ImVec2(0.0f, tableH)))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Texture", ImGuiTableColumnFlags_WidthStretch);
		if (!importing)
			ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthFixed, 90.0f);
		ImGui::TableSetupColumn("sRGB (Color)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
		ImGui::TableHeadersRow();

		for (size_t i = 0; i < s_rows.size(); ++i)
		{
			Row& r = s_rows[i];
			ImGui::PushID(static_cast<int>(i));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			if (changes(r) && !importing)
				ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "%s", r.label.c_str());
			else
				ImGui::TextUnformatted(r.label.c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s\nName suggests: %s", r.path.c_str(),
				                  r.suggested ? "color (sRGB)" : "data (linear)");
			if (!importing)
			{
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				if (!r.current)          ImGui::TextDisabled("linear (old)");
				else if (*r.current)     ImGui::TextUnformatted("sRGB");
				else                     ImGui::TextUnformatted("linear");
			}
			ImGui::TableNextColumn();
			ImGui::Checkbox("##srgb", &r.srgb);
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	const size_t pending = static_cast<size_t>(
		std::count_if(s_rows.begin(), s_rows.end(), [](const Row& r) { return changes(r); }));
	if (!importing)
		ImGui::TextDisabled("%zu of %zu texture(s) will change.", pending, s_rows.size());
	ImGui::Spacing();

	const std::string go = importing
		? "Import " + std::to_string(s_rows.size()) + (s_rows.size() == 1 ? " Texture" : " Textures")
		: std::string("Apply");
	ImGui::BeginDisabled(!importing && pending == 0);
	if (EditorWidgets::primaryButton(go.c_str(), ImVec2(150, 0)))
	{
		if (importing) applyImport(ctx);
		else           applyRetag(ctx);
		s_rows.clear();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (EditorWidgets::cancelButton("Cancel", ImVec2(110, 0)) ||
	    ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		s_rows.clear();
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
#else
	(void)ctx;
#endif
}

} // namespace TextureColourSpaceDialog
