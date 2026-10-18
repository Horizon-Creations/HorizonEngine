#pragma once
#include <string>

struct AppContext;

// ── The open scene changed on disk ───────────────────────────────────────────
// A git pull (or a checkout, a restore, a stash pop) rewrites the scene file under
// the editor's feet, and nothing noticed: the editor kept showing the version it had
// loaded, the next Save wrote that stale version back over what came from the
// server, and whoever pulled saw an older landscape than the one that was pushed.
//
// This watches the file of the scene that is open. When its CONTENT changes (see
// SceneFileStamp) it asks, once:
//
//   Reload Scene    load what is on disk now (unsaved edits in the open scene are
//                   dropped - the dialog says so when there are any)
//   Keep My Version leave the world alone; the next Save overwrites the disk file
//
// Polled once a second, never during play, and never while a project preflight or
// another prompt owns the screen.
namespace SceneDiskWatch
{
	// The editor has just read or written `scenePath`: take its stamp, so that the
	// editor's own save is not mistaken for somebody else's change. Empty = no scene.
	void remember(const std::string& scenePath);

	// Poll and draw the question. Call every frame while a project is open.
	void render(AppContext& ctx);

	// A question is on screen.
	bool asking();

	// Called right before the editor writes `scenePath`. True - and the question is
	// raised at once - when that file changed on disk since the editor last read or
	// wrote it and nobody has answered yet: a save then would silently put the stale
	// open scene over what came from the server (a pull a moment ago, a poll that
	// has not run yet). The save is to be refused; once the question is answered
	// ("Keep My Version" included) the next save goes through.
	bool blocksSave(const std::string& scenePath);
}
