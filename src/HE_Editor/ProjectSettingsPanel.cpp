#include "ProjectSettingsPanel.h"
#include "EditorApplication.h"           // AppContext, ProjectManager
#include "EditorTheme.h"                 // rail accent bar
#include "EditorWidgets.h"               // Row:: label-above widgets + wrapped hint()
#include "EditorHelp.h"                  // "<page>/<label>" scopes for the tooltips
#include "NotificationStore.h"           // a settings write that fails has to say so
#include "AppMetadataRows.h"             // icon file, icon preview, splash — shared with Export
#include <Application/AppIcon.h>         // the generated app icon + its preview
#include <Application/GameBackendRules.h> // the backend names a shipped build can name
#include <Renderer/UIFont.h>             // icon names, the plate colour parser, font scripts
#include <Physics/CollisionLayers.h>     // the project collision matrix
#include <Project/ProjectSettings.h>     // Config/ProjectSettings.json
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>   // InputTextWithHint over std::string (layer names)
#endif

namespace ProjectSettingsPanel
{

static Page s_page          = Page::General;
static bool s_openRequested = false;
static bool s_audioMixerRequested = false;

void requestOpen()          { s_openRequested = true; }
void requestOpen(Page page) { s_openRequested = true; s_page = page; }
bool takeOpenRequest()
{
	const bool r = s_openRequested;
	s_openRequested = false;
	return r;
}
bool takeAudioMixerRequest()
{
	const bool r = s_audioMixerRequested;
	s_audioMixerRequested = false;
	return r;
}

#ifdef HE_IMGUI_ENABLED

namespace {

using EditorWidgets::hint;
namespace Row = EditorWidgets::Row;

// Every page starts the same way, and a page that draws its controls over a
// closed project would be writing into a ProjectData nobody saves.
ProjectData* openProject(AppContext& ctx)
{
	if (!ctx.projectManager || ctx.projectManager->currentProject().path.empty())
	{
		ImGui::TextDisabled("No project is open.");
		return nullptr;
	}
	return &ctx.projectManager->currentProject();
}

// The settings-file pages write through the moment an edit ENDS — the same
// split the .heproj pages make below: the model follows every keystroke, the
// file is written once per edit. Numbers are clamped first, so a "0" typed into
// the resolution field never reaches the file (fromJson would clamp it on the
// next open, but the panel would show a value the file does not hold).
void commitSettings(AppContext& ctx, ProjectData& p, const char* what)
{
	p.settings.clamp();
	if (!ctx.projectManager->saveProjectSettings())
		HE::Ed::notify(HE::Ed::NoteLevel::Problem,
		               std::string("Could not save the project's ") + what,
		               HE::projectSettingsPath(ctx.projectManager->projectRoot()).string());
}

// Said once per settings-file page, in the same words: where the value goes.
// `reads` is an optional amber line for a page whose values are read somewhere
// less obvious than "the export" — each page says where its numbers arrive.
void settingsFileHint(const char* reads)
{
	hint("Belongs to the PROJECT: saved in Config/ProjectSettings.json beside the "
	     ".heproj, and carried into the build you export.");
	if (reads && *reads)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
		ImGui::TextWrapped("%s", reads);
		ImGui::PopStyleColor();
	}
	ImGui::Spacing();
}


// ─── Project ▸ Permissions ───────────────────────────────────────────────────
// What this project's scripts may reach outside the project (plan, Block C).
// Three checkboxes, all off until somebody says otherwise, saved into the
// .heproj and carried into the exported build.
//
// They bind the EDITOR too, not just the export. A preview that may delete a
// stranger's directory while the shipped app may not is the worse of the two
// failures: it happens on the author's machine, before anything shipped.
void drawPermissionsPage(AppContext& ctx)
{
	HE::Ed::Help::Scope helpScope("Permissions");

	if (!ctx.projectManager || ctx.projectManager->currentProject().path.empty())
	{
		ImGui::TextDisabled("No project is open.");
		return;
	}
	ProjectData& p = ctx.projectManager->currentProject();

	EditorWidgets::hint("These belong to the PROJECT, not to the editor: they are saved in "
	                    "its .heproj and travel into the application you export. They also "
	                    "bind scripts you run here, so the preview can never do more than "
	                    "the shipped app.");
	ImGui::Spacing();

	bool changed = false;
	changed |= ImGui::Checkbox("Files outside the project", &p.allowFiles);
	EditorWidgets::helpForLabel("Files outside the project");
	ImGui::TextDisabled("Off, a script reads and writes only inside the project's Saved folder.\n"
	                    "A file the person using the app picks in a dialog is always allowed,\n"
	                    "whatever this says — choosing it is the permission.");
	ImGui::Spacing();

	changed |= ImGui::Checkbox("Run other programs", &p.allowProcesses);
	EditorWidgets::helpForLabel("Run other programs");
	ImGui::TextDisabled("Covers Run Program and Open URL. Finding out whether a program is\n"
	                    "installed needs no permission — that is how a script can tell\n"
	                    "somebody what it would need.");
	ImGui::Spacing();

	changed |= ImGui::Checkbox("Network access", &p.allowNetwork);
	EditorWidgets::helpForLabel("Network access");
	ImGui::TextDisabled("Reserved: nothing reads this yet. It is here so a project that\n"
	                    "already answered the question does not have to answer it again.");

	if (changed)
	{
		// Written through immediately rather than on some later Save: a
		// permission that is on in the panel and off on disk is the state that
		// makes somebody spend an hour on why their script still cannot read a
		// file. The runtime picks it up on the next call by itself (the api
		// dispatch refreshes perm::set from here).
		if (!ctx.projectManager->saveProject(p.path))
			// Problem, not Warning: the panel now says one thing and the file
			// another, and it stays that way until somebody acts.
			HE::Ed::notify(HE::Ed::NoteLevel::Problem,
			               "Could not save the project's permissions", p.path);
	}
}

// ─── Project ▸ Collision Layers ──────────────────────────────────────────────
// The sixteen named collision channels and the matrix that says which pairs may
// touch. A PROJECT setting, saved in the .heproj and carried into the exported
// build, exactly like Permissions above.
//
// The matrix is drawn as a TRIANGLE, not a square. The two halves of a symmetric
// matrix are the same answer written twice, and a square grid invites somebody
// to tick one and not the other and then wonder why nothing changed —
// CollisionLayerConfig writes both cells from either one, so the second half
// would be a mirror that cannot be edited independently anyway.
void drawCollisionLayersPage(AppContext& ctx)
{
	HE::Ed::Help::Scope helpScope("Collision Layers");

	if (!ctx.projectManager || ctx.projectManager->currentProject().path.empty())
	{
		ImGui::TextDisabled("No project is open.");
		return;
	}
	ProjectData&              p   = ctx.projectManager->currentProject();
	HE::CollisionLayerConfig& cfg = p.collisionLayers;
	constexpr int kCount = HE::CollisionLayerConfig::kCount;

	EditorWidgets::hint("A collision layer is a named channel. Every rigid body and every "
	                    "character sits in one (Details ▸ Collision Layer), and the matrix "
	                    "below decides which pairs of channels the simulation lets touch. "
	                    "Belongs to the PROJECT: saved in its .heproj and carried into the "
	                    "build you export.");
	ImGui::Spacing();

	// Written per keystroke into the model so the matrix labels follow the
	// typing, and to the FILE when an edit ends — the same split the Application
	// page makes, for the same reason: a .heproj rewritten per character is a lot
	// of temp-file churn on a versioned file that has a watcher on it.
	bool commit = false;

	ImGui::SeparatorText("Names");
	EditorWidgets::hint("A layer keeps its NUMBER for good — that is what a scene stores — so "
	                    "renaming one relabels it everywhere and remaps nothing. Leave a name "
	                    "empty and it reads as \"Layer <n>\".");

	if (ImGui::BeginTable("##collisionlayernames", 2,
	                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("##idx", ImGuiTableColumnFlags_WidthFixed,
		                        ImGui::CalcTextSize("00").x + ImGui::GetStyle().CellPadding.x * 2.0f);
		ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
		for (int i = 0; i < kCount; ++i)
		{
			ImGui::PushID(i);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%d", i);
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-FLT_MIN);
			// The placeholder shows what an empty name READS BACK AS, which is
			// not "Layer <n>" for all sixteen: the five presets answer with
			// their built-in names. Asking a default-constructed config is the
			// only spelling of that which cannot drift from layerName() itself.
			static const HE::CollisionLayerConfig kDefaults;
			const std::string placeholder = kDefaults.layerName(i);
			std::string       name        = cfg.names[i];
			if (ImGui::InputTextWithHint("##layername", placeholder.c_str(), &name))
				cfg.setLayerName(i, name);
			// By key, not by label: the control has no literal label of its own —
			// sixteen rows share one row shape and the visible text is data.
			EditorWidgets::helpForKey("Collision Layers/Name");
			commit |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	ImGui::Spacing();
	ImGui::SeparatorText("Matrix");
	EditorWidgets::hint("Ticked means the pair collides, which is how every project starts. "
	                    "Clearing a box is what makes a channel pass through another one. "
	                    "The diagonal is a layer against ITSELF.");

	// 17 columns: the row's name plus one per channel. Numbers in the header
	// rather than names — a sixteen-column grid has no room for words, and the
	// list above is the key from number to name.
	// The height is given EXPLICITLY because of ScrollX: a scrolling table with
	// an outer size of zero becomes a child that eats all the height left in the
	// page, which would put the button below it out of reach. Seventeen rows —
	// the header and the sixteen channels.
	const ImVec2 matrixSize(0.0f,
		ImGui::GetFrameHeightWithSpacing() * (kCount + 1) + ImGui::GetStyle().CellPadding.y * 2.0f);
	if (ImGui::BeginTable("##collisionmatrix", kCount + 1,
	                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
	                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
	                      matrixSize))
	{
		ImGui::TableSetupColumn("##rowname", ImGuiTableColumnFlags_WidthFixed);
		for (int b = 0; b < kCount; ++b)
		{
			char head[8];
			std::snprintf(head, sizeof(head), "%d", b);
			ImGui::TableSetupColumn(head, ImGuiTableColumnFlags_WidthFixed);
		}
		// Scrolled sideways, the names column and the numbers have to stay: a
		// grid of unlabelled boxes is not something anyone can aim at.
		ImGui::TableSetupScrollFreeze(1, 1);
		ImGui::TableHeadersRow();

		for (int a = 0; a < kCount; ++a)
		{
			ImGui::PushID(a);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(cfg.layerName(a).c_str());
			// Only the lower triangle carries a box. The upper half is the same
			// answer read the other way round; drawing it would be two controls
			// for one value.
			for (int b = 0; b <= a; ++b)
			{
				ImGui::TableSetColumnIndex(b + 1);
				ImGui::PushID(b);
				bool on = cfg.collides(a, b);
				if (ImGui::Checkbox("##cell", &on))
				{
					// setCollides, never matrix[][] by hand: it writes BOTH
					// cells, and Jolt does not promise which way round it asks.
					cfg.setCollides(a, b, on);
					commit = true;
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s \xc3\x97 %s", cfg.layerName(a).c_str(),
					                  cfg.layerName(b).c_str());
				ImGui::PopID();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	ImGui::Spacing();
	// The way back. A matrix somebody has switched most of off is otherwise
	// 136 boxes to undo by hand, and "nothing collides any more" is exactly the
	// state somebody reaches while finding out what these do.
	if (EditorWidgets::button("Everything Collides"))
	{
		for (int a = 0; a < kCount; ++a)
			for (int b = 0; b <= a; ++b)
				cfg.setCollides(a, b, true);
		commit = true;
	}
	EditorWidgets::helpForLabel("Everything Collides");
	ImGui::TextDisabled("Ticks every box again. The names stay as they are.");

	if (commit)
	{
		if (!ctx.projectManager->saveProject(p.path))
			HE::Ed::notify(HE::Ed::NoteLevel::Problem,
			               "Could not save the project's collision layers", p.path);
		// And into the running simulation, so a matrix edited during play takes
		// effect where it can be seen. PhysicsWorld copies the matrix into the
		// filter Jolt holds and wakes every body, so a box already lying on a
		// floor that just stopped colliding does fall.
		else if (ctx.applyCollisionLayers)
			ctx.applyCollisionLayers();
	}
}

// ─── Project ▸ Application ───────────────────────────────────────────────────
// What the application IS to the system it lands on (plan A7): its icon, its
// identifier, its version. The icon is GENERATED from one of the built-in icons
// on a coloured plate — the export writes the .icns, the .ico and the .png from
// it — so a project has an icon on the day it is made and nobody produces the
// same picture three times.
//
// The preview is a real texture of the real bytes, rebuilt only when the answer
// changes: a picture of the icon rendered by some other code would be the one
// thing on this page that can lie.
void drawApplicationPage(AppContext& ctx)
{
	HE::Ed::Help::Scope helpScope("Application");

	if (!ctx.projectManager || ctx.projectManager->currentProject().path.empty())
	{
		ImGui::TextDisabled("No project is open.");
		return;
	}
	ProjectData& p = ctx.projectManager->currentProject();

	EditorWidgets::hint("These belong to the PROJECT: saved in its .heproj and written into "
	                    "the application you export.");
	ImGui::Spacing();

	// ── The icon ─────────────────────────────────────────────────────────────
	ImGui::SeparatorText("Icon");

	static const int     kPreviewPx = 128;

	// The model is written per keystroke (so the preview follows the typing), but
	// the FILE is written when an edit ends. A .heproj rewritten per character is
	// a lot of temp-file churn for one word, and it is a versioned file with a
	// watcher on it.
	bool commit = false;

	EditorWidgets::Row::inputText("Icon##appiconname", &p.appIconName);
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	EditorWidgets::helpForLabel("Icon");
	const bool nameOk = !p.appIconName.empty() && HE::uiIconCodepoint(p.appIconName) != 0;
	if (!p.appIconName.empty() && !nameOk)
		ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
		                   "No built-in icon is called that — the export writes none.");
	else
		ImGui::TextDisabled("One of the %zu built-in icons, by name. The same names "
		                    "<icon=…> uses in a label.", HE::uiIconCount());

	// A few names that contain what was typed, so somebody who half-remembers one
	// can find it without leaving the page. Ten is enough to recognise the
	// pattern; a full list of two thousand is a different panel.
	if (!p.appIconName.empty() && !nameOk)
	{
		std::string matches;
		int found = 0;
		for (std::size_t i = 0; i < HE::uiIconCount() && found < 10; ++i)
		{
			const char* n = HE::uiIconNameAt(i);
			if (std::strstr(n, p.appIconName.c_str()))
			{
				matches += (found++ ? ", " : "");
				matches += n;
			}
		}
		if (found) ImGui::TextDisabled("Did you mean: %s", matches.c_str());
	}
	ImGui::Spacing();

	{
		glm::vec4 col(0.12f, 0.44f, 0.78f, 1.0f);
		HE::uiParseRichColor(p.appIconColor, col);
		float rgb[3] = { col.r, col.g, col.b };
		if (EditorWidgets::Row::colorEdit3("Plate colour##appiconcolor", rgb))
		{
			char hex[8];
			std::snprintf(hex, sizeof(hex), "#%02x%02x%02x",
			              (int)std::lround(std::clamp(rgb[0], 0.0f, 1.0f) * 255.0f),
			              (int)std::lround(std::clamp(rgb[1], 0.0f, 1.0f) * 255.0f),
			              (int)std::lround(std::clamp(rgb[2], 0.0f, 1.0f) * 255.0f));
			p.appIconColor = hex;
		}
		commit |= ImGui::IsItemDeactivatedAfterEdit();
	}
	EditorWidgets::helpForLabel("Plate colour");
	ImGui::TextDisabled("The icon itself is white on a dark plate and near-black on a light "
	                    "one, so there is one colour to choose and not two.");
	ImGui::Spacing();

	// ── …or a picture of the project's own ───────────────────────────────────
	// Drawn by the shared rows so the Export dialog shows the same thing.
	commit |= AppMetadataRows::drawIconFileRow(ctx, p);
	ImGui::TextDisabled("A PNG of yours instead of the generated icon: every size and every\n"
	                    "container is made from it. Leave it empty to keep the generated one.");
	ImGui::Spacing();

	// ── The preview ──────────────────────────────────────────────────────────
	// The real bytes the export would write — file or glyph — so the one thing
	// on this page that could lie does not.
	if (const ImTextureID tex =
	        static_cast<ImTextureID>(AppMetadataRows::iconPreviewTexture(ctx, p, kPreviewPx)))
		ImGui::Image(tex, ImVec2((float)kPreviewPx, (float)kPreviewPx));
	else
		ImGui::TextDisabled("(no icon to show)");
	ImGui::Spacing();

	// ── Identity ─────────────────────────────────────────────────────────────
	ImGui::SeparatorText("Identity");
	EditorWidgets::Row::inputText("Bundle identifier##bundleid", &p.bundleId);
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	EditorWidgets::helpForLabel("Bundle identifier");
	ImGui::TextDisabled("Empty derives com.horizonengine.<project>, which is what every\n"
	                    "export did before this field existed. Set it once you own a domain.");
	ImGui::Spacing();
	EditorWidgets::Row::inputText("Version##appversion", &p.appVersion);
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	EditorWidgets::helpForLabel("Version");
	ImGui::Spacing();

	// ── The file types this application owns ─────────────────────────────────
	ImGui::SeparatorText("File types");
	ImGui::TextWrapped("Which files belong to this application. The export declares them the "
	                   "way each system wants it: inside the bundle on macOS, as a .desktop "
	                   "and a MIME file on Linux, as a .reg on Windows. Installing the last "
	                   "two is an installer's job — the export writes what would be installed.");
	ImGui::Spacing();

	int removeAt = -1;
	for (std::size_t i = 0; i < p.documentTypes.size(); ++i)
	{
		HE::AppDocumentType& t = p.documentTypes[i];
		ImGui::PushID(static_cast<int>(i));
		EditorWidgets::Row::inputText("Extension##docext", &t.extension);
		commit |= ImGui::IsItemDeactivatedAfterEdit();
		if (i == 0) EditorWidgets::helpForLabel("Extension");
		if (!t.extension.empty() && !HE::heValidDocumentExtension(t.extension))
			ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
			                   "Letters and digits only, no dot, not starting with a digit — "
			                   "this one is skipped.");
		EditorWidgets::Row::inputText("Name##docname", &t.displayName);
		commit |= ImGui::IsItemDeactivatedAfterEdit();
		if (i == 0) EditorWidgets::helpForLabel("Name");
		EditorWidgets::Row::inputText("Icon##docicon", &t.iconName);
		commit |= ImGui::IsItemDeactivatedAfterEdit();
		if (i == 0) EditorWidgets::helpForLabel("Icon##doc");
		if (!t.iconName.empty() && HE::uiIconCodepoint(t.iconName) == 0)
			ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
			                   "No built-in icon is called that — the files wear the "
			                   "application's icon.");
		if (EditorWidgets::button("Remove")) removeAt = static_cast<int>(i);
		EditorWidgets::helpForLabel("Remove");
		ImGui::Separator();
		ImGui::PopID();
	}
	if (removeAt >= 0)
	{
		p.documentTypes.erase(p.documentTypes.begin() + removeAt);
		commit = true;
	}
	if (EditorWidgets::button("Add file type"))
	{
		p.documentTypes.push_back({ "", "Document", "" });
		commit = true;
	}
	EditorWidgets::helpForLabel("Add file type");

	if (commit)
	{
		// Straight to disk, as on the other project pages: a value that is in the
		// panel and not in the file is the state somebody loses an evening to.
		if (!ctx.projectManager->saveProject(p.path))
			HE::Ed::notify(HE::Ed::NoteLevel::Problem,
			               "Could not save the project's application settings", p.path);
	}
}

// ─── Project ▸ Fonts ─────────────────────────────────────────────────────────
// Which scripts this project's text is written in. The atlas always carries
// Latin as it is actually written — umlauts, accents, the punctuation a text
// field produces on its own — so most projects never open this page. The two
// boxes here cost atlas area, which is the whole reason they are a question.
//
// The awkward part is honest and stays visible: the atlas is baked once per
// process and every renderer backend uploads it once, so a change reaches THIS
// editor session only after a restart. The page says which answer is live.
void drawFontsPage(AppContext& ctx)
{
	HE::Ed::Help::Scope helpScope("Fonts");

	if (!ctx.projectManager || ctx.projectManager->currentProject().path.empty())
	{
		ImGui::TextDisabled("No project is open.");
		return;
	}
	ProjectData& p = ctx.projectManager->currentProject();

	EditorWidgets::hint("These belong to the PROJECT: they are saved in its .heproj and "
	                    "travel into the application you export, which has to bake its own "
	                    "atlas on a machine that never saw this editor.");
	ImGui::Spacing();

	// ── Weight ───────────────────────────────────────────────────────────────
	ImGui::SeparatorText("Text weight");
	ImGui::TextWrapped("Which weight ordinary text is drawn in. Regular is what body text "
	                   "usually is, and it is what gives <b> in rich text something to be "
	                   "bolder than.");
	ImGui::Spacing();

	const bool wasBold = p.fontWeightBold;
	if (ImGui::RadioButton("Regular", !p.fontWeightBold)) p.fontWeightBold = false;
	EditorWidgets::helpForLabel("Regular");
	ImGui::Spacing();
	if (ImGui::RadioButton("Bold", p.fontWeightBold)) p.fontWeightBold = true;
	EditorWidgets::helpForLabel("Bold");
	ImGui::Indent();
	EditorWidgets::hint("What the engine has always drawn, so an older project keeps it "
	                    "until somebody says otherwise. With Bold as the base, <b> has "
	                    "nothing bolder to reach for and draws the same letters.");
	ImGui::Unindent();
	ImGui::Spacing();

	// No control, because there is nothing to decide: the icon face is baked the
	// first time a label asks for one, and a project that shows no icon never
	// pays for it. Said out loud anyway, because "how do I get an icon" is a
	// question this page is where somebody looks for.
	ImGui::SeparatorText("Icons");
	ImGui::TextWrapped("%zu icons are built in, written as <icon=name> in a rich text "
	                   "label (<icon=home>, <icon=settings>, <icon=save>). They are baked "
	                   "only once a label actually asks for one, so a project without "
	                   "icons carries none of it.",
	                   HE::uiIconCount());
	ImGui::Spacing();

	ImGui::SeparatorText("Scripts");
	ImGui::TextWrapped("Every project gets Latin: A to Z, the umlauts and accents of "
	                   "Latin-1, the Central European letters of Latin Extended-A, the "
	                   "typographic quotes and dashes, and the Euro sign. The two below "
	                   "are added on top and cost room in the atlas.");
	ImGui::Spacing();

	const std::uint32_t before = p.fontScripts;
	bool greek    = (p.fontScripts & HE::UIFontScriptGreek)    != 0;
	bool cyrillic = (p.fontScripts & HE::UIFontScriptCyrillic) != 0;

	if (ImGui::Checkbox("Greek", &greek))
		p.fontScripts = greek ? (p.fontScripts | HE::UIFontScriptGreek)
		                      : (p.fontScripts & ~HE::UIFontScriptGreek);
	EditorWidgets::helpForLabel("Greek");
	ImGui::Spacing();

	if (ImGui::Checkbox("Cyrillic", &cyrillic))
		p.fontScripts = cyrillic ? (p.fontScripts | HE::UIFontScriptCyrillic)
		                         : (p.fontScripts & ~HE::UIFontScriptCyrillic);
	EditorWidgets::helpForLabel("Cyrillic");
	ImGui::Spacing();

	if (p.fontScripts != before || p.fontWeightBold != wasBold)
	{
		// Straight to disk, like the permissions page: a setting that is on in
		// the panel and off in the file is the state somebody spends an evening on.
		if (!ctx.projectManager->saveProject(p.path))
			HE::Ed::notify(HE::Ed::NoteLevel::Problem,
			               "Could not save the project's font settings", p.path);
	}

	// What this session actually baked, said plainly. Asking for what is already
	// live answers yes, and then there is nothing to report.
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	const bool weightLive = HE::uiSetFontWeightBold(p.fontWeightBold);
	if (!HE::uiSetFontScripts(p.fontScripts) || !weightLive)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
		ImGui::TextWrapped("Saved, but not drawn yet: this editor session already baked its "
		                   "font atlas, and every renderer holds that one texture. Restart "
		                   "the editor to see the change. An exported application bakes on "
		                   "its own start and needs no restart.");
		ImGui::PopStyleColor();
	}
	else
	{
		const HE::BakedUIFont& f = HE::sharedUIFont();
		ImGui::TextDisabled("Live: %zu characters in a %d x %d atlas, %s.",
		                    f.glyphs.size(), f.atlasW, f.atlasH,
		                    HE::uiFontWeightBold() ? "bold" : "regular");
	}
}

// ─── Game ▸ General ──────────────────────────────────────────────────────────
// What the project is called and where it starts. The NAME is the .heproj's
// file name and is shown, not edited — renaming a project is a file operation
// with a hub of its own. The TITLE is what the game calls itself to a player
// (settings file); the startup scene is the manifest's.

// The project's scenes, Content-relative, for the startup-scene picker.
// Scanned when the page is entered and on Refresh, never per frame: a walk over
// a large project's Content folder is not something to do sixty times a second.
std::vector<std::string> s_sceneList;
std::string              s_sceneListRoot;   // the project the list was made for

void rescanScenes(const std::string& projectRoot)
{
	s_sceneList.clear();
	s_sceneListRoot = projectRoot;
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path content = fs::path(projectRoot) / "Content";
	if (!fs::is_directory(content, ec)) return;
	for (fs::recursive_directory_iterator it(content, fs::directory_options::skip_permission_denied, ec), end;
	     !ec && it != end; it.increment(ec))
	{
		if (!it->is_regular_file(ec) || it->path().extension() != ".hescene") continue;
		s_sceneList.push_back(fs::relative(it->path(), content, ec).generic_string());
	}
	std::sort(s_sceneList.begin(), s_sceneList.end());
}

void drawGeneralPage(AppContext& ctx)
{
	HE::Ed::Help::Scope helpScope("Project General");
	ProjectData* pp = openProject(ctx);
	if (!pp) return;
	ProjectData& p = *pp;
	const std::string root = ctx.projectManager->projectRoot();
	if (s_sceneListRoot != root) rescanScenes(root);

	settingsFileHint(nullptr);

	ImGui::SeparatorText("Name");
	Row::labelText("Project", "%s", p.name.c_str());
	ImGui::TextDisabled("The .heproj's file name. Shown here, changed in the Project Hub.");
	ImGui::Spacing();

	bool commit = false;
	Row::inputText("Title##gametitle", &p.settings.game.title);
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	hint("What the game calls itself to a player: the packaged game's window title "
	     "and the name a launcher shows (the .app's display name, the .desktop "
	     "entry). Empty means the project name. The folder, the .hpak and the "
	     "save directory keep the project name whatever this says.");
	ImGui::Spacing();

	// ── Splash ───────────────────────────────────────────────────────────────
	// The rows are shared with the Export dialog (AppMetadataRows), which is
	// also why they write into p.settings and commit through the same path.
	ImGui::SeparatorText("Splash");
	commit |= AppMetadataRows::drawSplashRows(ctx, p, /*compact=*/false);
	ImGui::Spacing();

	// ── Startup scene ────────────────────────────────────────────────────────
	// The manifest's, not the settings file's: the .heproj has carried it since
	// the first project, and an export profile may still override it per build.
	ImGui::SeparatorText("Startup scene");
	if (p.appProject)
	{
		ImGui::TextDisabled("An application has no world and loads no scene.");
	}
	else
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		std::string current;
		if (!p.startupScene.empty())
			current = fs::relative(p.startupScene, fs::path(root) / "Content", ec).generic_string();
		const char* preview = current.empty() ? "(none)" : current.c_str();
		// Label above the control like the Row helpers draw theirs; the combo
		// itself is unlabelled, so the help is asked for by name below.
		ImGui::TextUnformatted("Scene");
		ImGui::SameLine();
		if (EditorWidgets::smallButton("Rescan")) rescanScenes(root);
		EditorWidgets::helpForLabel("Rescan");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::BeginCombo("##startupscene", preview))
		{
			for (const std::string& s : s_sceneList)
			{
				const bool selected = s == current;
				if (ImGui::Selectable(s.c_str(), selected))
				{
					p.startupScene = (fs::path(root) / "Content" / s).string();
					if (!ctx.projectManager->saveProject(p.path))
						HE::Ed::notify(HE::Ed::NoteLevel::Problem,
						               "Could not save the project's startup scene", p.path);
				}
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		EditorWidgets::helpForKey("Project General/Scene");
		hint("The scene the game opens with, and the one the editor opens on "
		                    "load. An export profile may name a different one for its build.");
	}

	if (commit) commitSettings(ctx, p, "general settings");
}

// ─── Rendering ▸ Defaults ────────────────────────────────────────────────────
// What the packaged build boots with. Until a project says otherwise the export
// takes the editor's Preferences (the way it always has); the switch at the top
// is what makes the rows below the project's word instead.
void drawRenderDefaultsPage(AppContext& ctx)
{
	HE::Ed::Help::Scope helpScope("Render Defaults");
	ProjectData* pp = openProject(ctx);
	if (!pp) return;
	ProjectData& p = *pp;
	HE::ProjectRenderDefaults& r = p.settings.renderDefaults;

	settingsFileHint(nullptr);

	bool commit = false;
	if (EditorWidgets::checkbox("Use the editor's settings", &r.useEditorSettings))
		commit = true;
	hint("On, the packaged build boots with whatever this editor's Preferences "
	                    "and Export dialog say — the way every export has worked so far.\n"
	                    "Off, the rows below are the project's answer on every machine: the "
	                    "Export dialog shows them and writes them into the build's config.json. "
	                    "Bloom, AO and the other graphics settings stay the editor's either way.");
	ImGui::Spacing();

	ImGui::BeginDisabled(r.useEditorSettings);
	ImGui::SeparatorText("Window");
	Row::inputInt("Width##winw", &r.windowWidth);
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	Row::inputInt("Height##winh", &r.windowHeight);
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	{
		static const char* const kModes[] = { "Windowed", "Fullscreen", "Borderless" };
		int mode = 1;
		for (int i = 0; i < 3; ++i) if (r.windowMode == kModes[i]) mode = i;
		if (Row::combo("Window mode##winmode", &mode, kModes, 3))
		{
			r.windowMode = kModes[mode];
			commit = true;
		}
	}
	if (EditorWidgets::checkbox("VSync", &r.vsync)) commit = true;
	ImGui::Spacing();

	ImGui::SeparatorText("Backend");
	{
		// Every backend SOME target can ship — the project does not know yet
		// which platform it will be exported for, and a name the runtime lacks
		// falls back to its platform default at start, with a line in the log.
		std::vector<const char*> names = { "(platform default)" };
		for (const char* platform : { "macOS", "Windows", "Linux" })
			for (const char* b : HE::BackendRules::choicesFor(platform))
				if (std::find_if(names.begin(), names.end(),
				                 [b](const char* n) { return std::strcmp(n, b) == 0; }) == names.end())
					names.push_back(b);
		int current = 0;
		for (std::size_t i = 1; i < names.size(); ++i)
			if (r.backend == names[i]) current = static_cast<int>(i);
		if (Row::combo("Graphics backend##backend", &current, names.data(),
		               static_cast<int>(names.size())))
		{
			r.backend = current == 0 ? std::string() : names[current];
			commit = true;
		}
	}
	hint("A backend the target platform cannot create falls back to its "
	                    "default at start. Leave it on the default unless a build needs one.");
	ImGui::EndDisabled();

	if (commit) commitSettings(ctx, p, "render defaults");
}

// ─── Rendering ▸ Shadows ─────────────────────────────────────────────────────
// The directional light's cascaded shadow maps. The defaults are the constants
// the extractor has always used, so a project that never opens this page draws
// exactly what it drew.
void drawShadowsPage(AppContext& ctx)
{
	HE::Ed::Help::Scope helpScope("Shadows");
	ProjectData* pp = openProject(ctx);
	if (!pp) return;
	ProjectData& p = *pp;
	HE::ProjectShadowSettings& s = p.settings.shadows;

	settingsFileHint(nullptr);
	hint("Read by the renderer as you edit: the viewport follows every change here, "
	     "and the exported build reads the same file. Metal and OpenGL draw cascades; "
	     "the other backends still use one whole-scene map and ignore this page.");

	bool commit = false;
	ImGui::SeparatorText("Cascades");
	Row::dragFloat("Shadow distance##shadowdist", &s.distance, 1.0f, 1.0f,
	               HE::ProjectShadowSettings::kMaxDistance, "%.0f m");
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	Row::sliderInt("Cascades##cascades", &s.cascadeCount, 1, HE::ProjectShadowSettings::kMaxCascades);
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	{
		static const char* const kRes[] = { "512", "1024", "2048", "4096", "8192" };
		static const int         kResV[] = { 512, 1024, 2048, 4096, 8192 };
		int cur = 2;
		for (int i = 0; i < 5; ++i) if (s.resolution == kResV[i]) cur = i;
		if (Row::combo("Resolution##shadowres", &cur, kRes, 5))
		{
			s.resolution = kResV[cur];
			commit = true;
		}
	}
	Row::sliderFloat("Split blend##lambda", &s.splitLambda, 0.0f, 1.0f, "%.2f");
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	hint("0 spaces the cascades evenly, 1 logarithmically: more of the map "
	                    "near the camera, where a texel covers the least ground.");
	ImGui::Spacing();

	ImGui::SeparatorText("Bias");
	Row::dragFloat("Slope bias##slopebias", &s.slopeBias, 0.0001f, 0.0f, 0.1f, "%.4f");
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	Row::dragFloat("Minimum bias##minbias", &s.minBias, 0.0001f, 0.0f, 0.1f, "%.4f");
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	hint("Too little and surfaces stripe with their own shadow (acne); too "
	                    "much and shadows float away from what casts them (peter-panning).");

	if (commit) commitSettings(ctx, p, "shadow settings");
}

// ─── Physics ▸ Simulation ────────────────────────────────────────────────────
void drawSimulationPage(AppContext& ctx)
{
	HE::Ed::Help::Scope helpScope("Physics");
	ProjectData* pp = openProject(ctx);
	if (!pp) return;
	ProjectData& p = *pp;
	HE::ProjectPhysicsSettings& ph = p.settings.physics;

	settingsFileHint(nullptr);
	hint("Read when a simulation starts, in the editor's Play as in the exported "
	     "build. Gravity also reaches a simulation that is already running.");

	bool commit = false;
	ImGui::SeparatorText("Step");
	Row::sliderInt("Fixed rate##fixedhz", &ph.fixedHz,
	               HE::ProjectPhysicsSettings::kMinHz, HE::ProjectPhysicsSettings::kMaxHz, "%d Hz");
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	hint("How many simulation steps a second of game time is cut into, "
	                    "whatever the frame rate. 60 is what every project has run at.");
	ImGui::Spacing();

	ImGui::SeparatorText("Gravity");
	float g[3] = { ph.gravity.x, ph.gravity.y, ph.gravity.z };
	if (Row::dragFloat3("Gravity##gravity", g, 0.01f, -1000.0f, 1000.0f, "%.2f"))
		ph.gravity = glm::vec3(g[0], g[1], g[2]);
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	hint("Metres per second squared, for rigid bodies. A character controller "
	                    "falls by its own component's gravity, on purpose.");
	if (EditorWidgets::button("Earth"))
	{
		ph.gravity = glm::vec3(0.0f, -9.81f, 0.0f);
		commit = true;
	}
	EditorWidgets::helpForLabel("Earth");

	if (commit)
	{
		commitSettings(ctx, p, "physics settings");
		// And into the running preview, the way the collision matrix goes: a
		// gravity edited during Play should show where it can be seen. The rate
		// needs no push — the step reads it from the project every frame.
		if (ctx.applyPhysicsSettings) ctx.applyPhysicsSettings();
	}
}

// ─── Game ▸ Anti-Cheat ───────────────────────────────────────────────────────
// What the host refuses to believe from a client, and what it does about it
// (docs/anti-cheat-plan.md §4.4). Default-constructed is OFF, which is the
// host exactly as it was before the service existed; the other numbers are the
// plan's starting values and matter only once the switch is on.
//
// Four blocks: the switch and the integrity check, the limits the detectors
// run with, the policy per level, and the game's own value rules. The policy
// is a small grid — one row per level, one column per response — rather than
// six loose checkboxes, because "what happens on Confirmed" is a row you read
// across. Log has no box: it is not optional (§5.2).
void drawAntiCheatPage(AppContext& ctx)
{
	HE::Ed::Help::Scope helpScope("Anti-Cheat");
	ProjectData* pp = openProject(ctx);
	if (!pp) return;
	ProjectData& p = *pp;
	using AC = HE::ProjectAntiCheatSettings;
	AC& a = p.settings.antiCheat;

	settingsFileHint("Read by the HOST of a session when it starts — the editor's Play as "
	                 "host and the exported build alike. Switched off, the host behaves "
	                 "exactly as it always has.");

	bool commit = false;

	// ── Switch ───────────────────────────────────────────────────────────────
	ImGui::SeparatorText("Detection");
	commit |= EditorWidgets::checkbox("Enable anti-cheat##acenabled", &a.enabled);
	hint("The host scores what each client sends — input rate, a stretched clock, "
	     "moves faster than the entity can go — and acts on the policy below. "
	     "Clients only ever send input either way; this decides whether refusals "
	     "are counted or merely dropped.");
	commit |= EditorWidgets::checkbox("Check client integrity at join##acintegrity", &a.integrityCheck);
	hint("A joining client sends the hashes of its program and paks; the host "
	     "compares them with its own. Catches an edited script or asset, not a "
	     "patched executable.");
	ImGui::Spacing();

	// ── Limits ───────────────────────────────────────────────────────────────
	ImGui::SeparatorText("Limits");
	Row::dragFloat("Clock tolerance##actolerance", &a.tolerance, 0.005f, 0.0f, AC::kMaxTolerance, "%.2f");
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	Row::dragFloat("Clock window##acwindow", &a.windowSec, 0.1f, AC::kMinWindowSec, AC::kMaxWindowSec, "%.1f s");
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	hint("How much faster than real time a client's simulated time may run before "
	     "it counts, and the span it is measured over. The window has to be "
	     "seconds: after a network stall a burst of commands arrives at once "
	     "whose time adds up to exactly what the host waited.");
	Row::dragInt("Max inputs per second##acinputs", &a.maxInputsPerSecond, 1.0f,
	             AC::kMinInputsPerSecond, AC::kMaxInputsPerSecond);
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	Row::dragFloat("Score half-life##achalflife", &a.scoreHalfLifeSec, 0.5f,
	               AC::kMinHalfLifeSec, AC::kMaxHalfLifeSec, "%.1f s");
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	Row::dragFloat("Suspect at##acsuspect", &a.scoreSuspect, 0.1f, 0.0f, AC::kMaxScoreThreshold, "%.1f");
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	Row::dragFloat("Confirmed at##acconfirmed", &a.scoreConfirmed, 0.1f, 0.0f, AC::kMaxScoreThreshold, "%.1f");
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	hint("Every observation adds its weight to a score that halves over the "
	     "half-life; the two thresholds are the levels. Confirmed is kept at or "
	     "above Suspect.");
	ImGui::Spacing();

	// ── Policy ───────────────────────────────────────────────────────────────
	ImGui::SeparatorText("Policy");
	hint("What the host does by itself when a connection reaches a level. A "
	     "script handler for OnCheatDetected may replace it per report. Log is "
	     "always on; a row with nothing else ticked is observation mode, the "
	     "recommended way to start.");
	{
		struct LevelRow { const char* label; std::uint32_t* mask; };
		LevelRow rows[] = { { "Suspect",   &a.policySuspect },
		                    { "Confirmed", &a.policyConfirmed },
		                    { "Hard",      &a.policyHard } };
		// Columns after the level name, in bit order after Log.
		struct Column { const char* head; AC::Response bit; const char* key; };
		const Column cols[] = {
			{ "Event",     AC::Event,     "Anti-Cheat/Policy Event" },
			{ "Telemetry", AC::Telemetry, "Anti-Cheat/Policy Telemetry" },
			{ "Flag",      AC::Flag,      "Anti-Cheat/Policy Flag" },
			{ "Kick",      AC::Kick,      "Anti-Cheat/Policy Kick" },
			{ "Ban",       AC::Ban,       "Anti-Cheat/Policy Ban" },
		};
		constexpr int kCols = IM_ARRAYSIZE(cols);
		if (ImGui::BeginTable("##acpolicy", kCols + 1,
		                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("##level", ImGuiTableColumnFlags_WidthFixed);
			for (const Column& c : cols)
				ImGui::TableSetupColumn(c.head, ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableHeadersRow();
			for (int r = 0; r < IM_ARRAYSIZE(rows); ++r)
			{
				ImGui::PushID(r);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(rows[r].label);
				for (int c = 0; c < kCols; ++c)
				{
					ImGui::TableSetColumnIndex(c + 1);
					ImGui::PushID(c);
					bool on = (*rows[r].mask & cols[c].bit) != 0;
					if (ImGui::Checkbox("##cell", &on))
					{
						if (on) *rows[r].mask |= cols[c].bit;
						else    *rows[r].mask &= ~static_cast<std::uint32_t>(cols[c].bit);
						commit = true;
					}
					// By key: the box has no label of its own, the column head
					// and the row name are what the user reads.
					EditorWidgets::helpForKey(cols[c].key);
					ImGui::PopID();
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}
	Row::inputText("Telemetry URL##actelemetry", &a.telemetryUrl);
	commit |= ImGui::IsItemDeactivatedAfterEdit();
	hint("Where reports are sent when a policy says Telemetry. Empty means no "
	     "telemetry at all. This is the engine's own setting, not the scripts' "
	     "Network permission.");
	ImGui::Spacing();

	// ── Rules ────────────────────────────────────────────────────────────────
	ImGui::SeparatorText("Rules");
	hint("Values the engine cannot know — damage, loot, currency — declared once "
	     "here and checked with one call: anticheat.check(\"Damage\", value, "
	     "player). Range per value, sum per second per player, and what a "
	     "violation counts as.");
	if (a.rules.empty())
		ImGui::TextDisabled("No rules yet. The engine's own checks run without any.");
	int removeAt = -1;
	if (!a.rules.empty()
	    && ImGui::BeginTable("##acrules", 6,
	                         ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
	{
		const float num = ImGui::CalcTextSize("00000000").x + ImGui::GetStyle().FramePadding.x * 2.0f;
		ImGui::TableSetupColumn("Name",       ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Min",        ImGuiTableColumnFlags_WidthFixed, num);
		ImGui::TableSetupColumn("Max",        ImGuiTableColumnFlags_WidthFixed, num);
		ImGui::TableSetupColumn("Per second", ImGuiTableColumnFlags_WidthFixed, num);
		ImGui::TableSetupColumn("Level",      ImGuiTableColumnFlags_WidthFixed,
		                        ImGui::CalcTextSize("Confirmed").x + ImGui::GetFrameHeight() + 8.0f);
		ImGui::TableSetupColumn("##remove",   ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableHeadersRow();
		for (int i = 0; i < static_cast<int>(a.rules.size()); ++i)
		{
			HE::ProjectAntiCheatRule& rule = a.rules[static_cast<std::size_t>(i)];
			ImGui::PushID(i);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint("##name", "Damage", &rule.name);
			EditorWidgets::helpForKey("Anti-Cheat/Rule Name");
			commit |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::DragFloat("##min", &rule.min, 1.0f, -AC::kMaxRuleValue, AC::kMaxRuleValue, "%g");
			EditorWidgets::helpForKey("Anti-Cheat/Rule Min");
			commit |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::TableSetColumnIndex(2);
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::DragFloat("##max", &rule.max, 1.0f, -AC::kMaxRuleValue, AC::kMaxRuleValue, "%g");
			EditorWidgets::helpForKey("Anti-Cheat/Rule Max");
			commit |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::TableSetColumnIndex(3);
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::DragFloat("##persec", &rule.maxPerSecond, 1.0f, 0.0f, AC::kMaxRuleValue, "%g");
			EditorWidgets::helpForKey("Anti-Cheat/Rule Per second");
			commit |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::TableSetColumnIndex(4);
			ImGui::SetNextItemWidth(-FLT_MIN);
			{
				static const char* const kLevelLabels[] = { "Suspect", "Confirmed", "Hard" };
				static_assert(IM_ARRAYSIZE(kLevelLabels) == HE::ProjectAntiCheatRule::kLevelCount);
				int lvl = rule.levelIndex();
				if (ImGui::Combo("##level", &lvl, kLevelLabels, IM_ARRAYSIZE(kLevelLabels)))
				{
					rule.level = HE::ProjectAntiCheatRule::kLevels[lvl];
					commit = true;
				}
				EditorWidgets::helpForKey("Anti-Cheat/Rule Level");
			}
			ImGui::TableSetColumnIndex(5);
			if (EditorWidgets::smallButton("\xc3\x97##removerule")) removeAt = i;
			EditorWidgets::helpForKey("Anti-Cheat/Remove rule");
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (removeAt >= 0)
	{
		a.rules.erase(a.rules.begin() + removeAt);
		commit = true;
	}
	if (static_cast<int>(a.rules.size()) < AC::kMaxRules)
	{
		if (EditorWidgets::button("Add Rule"))
		{
			a.rules.push_back(HE::ProjectAntiCheatRule{});
			commit = true;
		}
	}

	if (commit) commitSettings(ctx, p, "anti-cheat settings");
}

// ─── Audio ▸ Buses ───────────────────────────────────────────────────────────
// The mixer is a window of its own (View ▸ Audio Mixer): faders are something
// you operate while a scene plays, not a page you fill in. This page says where
// the buses are and what they are for, so the "Audio" heading on the rail is
// not a dead end.
void drawAudioBusesPage(AppContext& ctx)
{
	HE::Ed::Help::Scope helpScope("Audio Buses");
	ProjectData* pp = openProject(ctx);
	if (!pp) return;
	const ProjectData& p = *pp;

	hint("The mixer's buses belong to the PROJECT too: saved in its .heproj and carried "
	     "into the build you export, so a source whose Bus field says \"Music\" finds "
	     "that bus in the shipped game exactly as here.");
	ImGui::Spacing();
	const std::size_t count = p.audioBuses.buses.size();
	if (count == 0)
		ImGui::TextDisabled("No buses yet: every source plays straight into the master.");
	else
		ImGui::TextDisabled("%zu bus%s defined.", count, count == 1 ? "" : "es");
	ImGui::Spacing();
	// The mixer's open flag lives in EditorUI with the other tool windows;
	// the request is consumed there, next to this tab's own.
	if (EditorWidgets::button("Open Audio Mixer")) s_audioMixerRequested = true;
	EditorWidgets::helpForLabel("Open Audio Mixer");
}

// ─── Navigation + page plumbing ──────────────────────────────────────────────

struct NavItem  { Page page; const char* label; };
struct NavGroup { const char* label; const NavItem* items; int count; };

constexpr NavItem kGameItems[] = {
	{ Page::General,     "General" },
	{ Page::Application, "Application" },
	{ Page::Permissions, "Permissions" },
	{ Page::Fonts,       "Fonts" },
	{ Page::AntiCheat,   "Anti-Cheat" },
};
constexpr NavItem kRenderingItems[] = {
	{ Page::RenderDefaults, "Defaults" },
	{ Page::Shadows,        "Shadows" },
};
constexpr NavItem kPhysicsItems[] = {
	{ Page::Simulation,      "Simulation" },
	{ Page::CollisionLayers, "Collision Layers" },
};
constexpr NavItem kAudioItems[] = {
	{ Page::AudioBuses, "Buses" },
};
constexpr NavGroup kNavGroups[] = {
	{ "Game",      kGameItems,      IM_ARRAYSIZE(kGameItems) },
	{ "Rendering", kRenderingItems, IM_ARRAYSIZE(kRenderingItems) },
	{ "Physics",   kPhysicsItems,   IM_ARRAYSIZE(kPhysicsItems) },
	{ "Audio",     kAudioItems,     IM_ARRAYSIZE(kAudioItems) },
};

const char* pageTitle(Page p)
{
	for (const NavGroup& g : kNavGroups)
		for (int i = 0; i < g.count; ++i)
			if (g.items[i].page == p) return g.items[i].label;
	return "";
}

} // namespace

// ─── The Project Settings tab ────────────────────────────────────────────────

void render(AppContext& ctx, const ImVec2& pos, const ImVec2& size)
{
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin("##ProjectSettingsTab", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

	// ── Left: category rail — the Preferences rail, so the two tabs read as
	// the same kind of thing ───────────────────────────────────────────────
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.09f, 0.095f, 1.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 12.0f));
	ImGui::BeginChild("##projnav", ImVec2(210.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
	                  ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleVar();
	for (const NavGroup& group : kNavGroups)
	{
		if (&group != &kNavGroups[0]) { ImGui::Spacing(); ImGui::Spacing(); }
		if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
		ImGui::TextDisabled("%s", group.label);
		if (ctx.fontSubheading) ImGui::PopFont();
		ImGui::Spacing();
		for (int i = 0; i < group.count; ++i)
		{
			const NavItem& item = group.items[i];
			const bool active = s_page == item.page;
			ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
			ImGui::Indent(6.0f);
			if (ImGui::Selectable(item.label, active, 0,
			                      ImVec2(0.0f, ImGui::GetFrameHeight())))
				s_page = item.page;
			if (active)
			{
				const ImVec2 mn = ImGui::GetItemRectMin();
				const ImVec2 mx = ImGui::GetItemRectMax();
				ImGui::GetWindowDrawList()->AddRectFilled(
					ImVec2(mn.x - 6.0f, mn.y + 3.0f), ImVec2(mn.x - 3.0f, mx.y - 3.0f),
					HE::Ed::Theme::u32(HE::Ed::Theme::AccentHi), 2.0f);
			}
			ImGui::Unindent(6.0f);
			ImGui::PopStyleVar();
		}
	}
	ImGui::EndChild();
	ImGui::PopStyleColor();

	ImGui::SameLine();

	// ── Right: the selected page ─────────────────────────────────────────────
	ImGui::BeginChild("##projcontent", ImVec2(0.0f, 0.0f));

	if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
	ImGui::TextUnformatted(pageTitle(s_page));
	if (ctx.fontHeading) ImGui::PopFont();
	ImGui::Separator();
	ImGui::Spacing();

	const float footerH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
	ImGui::BeginChild("##projbody", ImVec2(0.0f, -footerH));
	switch (s_page)
	{
	case Page::General:         drawGeneralPage(ctx);         break;
	case Page::Application:     drawApplicationPage(ctx);     break;
	case Page::Permissions:     drawPermissionsPage(ctx);     break;
	case Page::Fonts:           drawFontsPage(ctx);           break;
	case Page::AntiCheat:       drawAntiCheatPage(ctx);       break;
	case Page::RenderDefaults:  drawRenderDefaultsPage(ctx);  break;
	case Page::Shadows:         drawShadowsPage(ctx);         break;
	case Page::Simulation:      drawSimulationPage(ctx);      break;
	case Page::CollisionLayers: drawCollisionLayersPage(ctx); break;
	case Page::AudioBuses:      drawAudioBusesPage(ctx);      break;
	}
	ImGui::EndChild();

	// ── Footer ───────────────────────────────────────────────────────────────
	ImGui::Separator();
	ImGui::TextDisabled("Project settings are saved the moment an edit ends.");

	ImGui::EndChild();
	ImGui::End();
}
#endif // HE_IMGUI_ENABLED

} // namespace ProjectSettingsPanel
