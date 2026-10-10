#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ── A project handed to the editor from outside ──────────────────────────────
// Double-clicking a .heproj in the file manager reaches the editor in one of two
// ways, and the platform decides which, not the user:
//  * Windows and Linux start the program with the file as an argument (the
//    registry's "%1", the .desktop file's %f). Application::Run keeps every
//    argument that is not an option in launchArguments().
//  * macOS starts it with no arguments and sends an "open document" event
//    instead — also to an editor that is already running. SDL turns that into
//    SDL_EVENT_DROP_FILE with no window (windowID 0).
//
// Both end up as a path in the one-slot request below, and the two screens take
// it through the SAME code they use for their own Open: the Project Hub loads it
// directly, the editor asks about unsaved work and ends the current session
// first (EditorUI's requestGuarded). GitCloneDialog::takeOpenRequest is the twin
// this copies.
//
// This file is ImGui- and SDL-free on purpose: what a path string means, whether
// it may be opened and what happens to it are the parts worth testing, and none
// of them needs a window. It also does not log — it answers with a reason and
// the caller says it, so a test can check the reason instead of a log line.
namespace ProjectLaunchOpen
{
	enum class PathError
	{
		None,
		Empty,           // nothing left after trimming and unquoting
		WrongExtension,  // not a .heproj (case-insensitive)
		NotFound,        // nothing at that path
		NotAFile,        // a folder (or something else) named *.heproj
	};

	// One sentence for the log, e.g. "is not a .heproj project file".
	const char* describe(PathError e);

	struct ParsedPath
	{
		std::string path;       // absolute and normalised when ok(); else what was understood
		PathError   error = PathError::Empty;
		bool ok() const { return error == PathError::None; }
	};

	// A raw argument or event payload → an openable project path.
	//  * surrounding whitespace is dropped, then ONE matching pair of quotes
	//    ("…" or '…') — what a registry entry missing its own quoting, or a
	//    copied shell line, leaves behind. Spaces inside the path stay.
	//  * a file:// URL (a .desktop file using %u instead of %f) is turned back
	//    into a path, percent-escapes included.
	//  * a relative path is resolved against `cwd` — the directory the process
	//    was started in, which is what the person who typed it meant.
	//  * the extension is checked case-insensitively, then that it exists and
	//    is a regular file.
	ParsedPath parse(std::string_view raw, const std::filesystem::path& cwd);

	// The launch arguments of one start: the FIRST openable project wins. A
	// second project is not opened (the editor holds one project at a time) and
	// is reported in `ignored`; anything unopenable is reported in `rejected`
	// with its reason. The caller logs both.
	struct LaunchPick
	{
		std::string project;                                    // empty: none
		std::vector<std::pair<std::string, PathError>> rejected;
		std::vector<std::string> ignored;
	};
	LaunchPick pickFromArguments(const std::vector<std::string>& args,
	                             const std::filesystem::path& cwd);

	// Whether two spellings name the same project file: the same file on disk
	// when both exist, else the same normalised absolute path.
	bool samePath(const std::string& a, const std::string& b);

	// What to do with a request, given what is open now.
	enum class Action
	{
		OpenDirect,   // no project open: the Hub's own load path
		OpenGuarded,  // another project open: unsaved-work prompt, then switch
		AlreadyOpen,  // the very project that is open: nothing to do
	};
	Action decide(bool projectLoaded, const std::string& currentProjectPath,
	              const std::string& requested);

	// ── The pending request ──────────────────────────────────────────────────
	// One slot. post() refuses while a request is still waiting (macOS sends
	// one open event per file when several are double-clicked at once; only the
	// first can win), so the caller can say the rest were ignored.
	bool post(const std::string& absPath);
	bool take(std::string& absPath);
	bool pending();
}
