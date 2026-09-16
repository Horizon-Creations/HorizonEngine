#pragma once
#include <filesystem>
#include <string>

struct AppContext;
struct ProjectData;

// The rows that say what the exported application IS to the person who
// installs it — its icon, its version, its splash — drawn in ONE place and used
// from two: Project Settings (where they live) and the Export dialog (where
// somebody about to ship wants to see them without leaving the dialog). Both
// callers bind the rows straight to the ProjectData; nothing here keeps a copy
// in a static, because a dialog static remembered across projects is how one
// project's icon ended up in another's build (see dialog-static-leaks).
//
// Every function returns true when an edit ENDED (a field was deactivated
// after a change, a file was picked, a checkbox flipped) — that is the
// caller's cue to save the .heproj resp. the settings file. The model itself
// follows every keystroke.
//
// Help entries: the labels are asked for through the CALLER's help scope, so
// each scope that draws these rows carries "<Scope>/<Label>" entries.
namespace AppMetadataRows
{
	// "Icon file" + Browse…: the project's own PNG instead of the generated
	// glyph. Stored PROJECT-relative when the file lies inside the project,
	// absolute otherwise (with a warning line — it will not travel with the
	// project). Returns true on commit (edit ended or a file was picked).
	bool drawIconFileRow(AppContext& ctx, ProjectData& p);

	// The icon as it will be exported, at `px`, as an ImGui texture — the real
	// bytes the export would write (file or generated glyph), rebuilt only when
	// name, plate colour or file change. 0 when there is no icon to show.
	// Textures are owned here and freed when the answer changes.
	unsigned long long iconPreviewTexture(AppContext& ctx, const ProjectData& p, int px);

	// Splash: the on/off switch, the image row + Browse…, the subtitle. Writes
	// p.settings.game.splash*; returns true on commit. `compact` draws the
	// short form the export dialog has room for (no explanatory lines).
	bool drawSplashRows(AppContext& ctx, ProjectData& p, bool compact);

	// Resolve a stored icon/splash path against the project root: relative →
	// <root>/<path>, absolute stays. Empty in → empty out.
	std::filesystem::path resolveProjectFile(const AppContext& ctx, const std::string& stored);
}
