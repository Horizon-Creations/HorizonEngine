#pragma once
#include <string>

struct AppContext;

// ── Project preflight ────────────────────────────────────────────────────────
// Opening a project used to be one call — load it — and everything it pulled from the
// EngineContent library (a material's texture arrays, a scene's default meshes, the
// functions a material graph calls) was fetched afterwards, one asset at a time, as the
// first frames asked for it: grey surfaces and a trail of warnings until the downloads
// caught up, or for good when one could not be had.
//
// This puts a step in front of the load, on the start screen (and over the editor when
// a project is switched): read the project's files for what it references in "Engine/",
// follow that through the engine's own files, sign in to the server and download what
// is not on this machine, and only then open the project. What could not be had — not
// on the server, no connection and nothing cached, a failed download — is listed and the
// user decides:
//
//   Open Anyway   the project opens with those assets missing (it falls back to the
//                 on-demand downloads, which will succeed once the server is back)
//   Close Project do not open it; back to the hub, nothing was loaded
//   Close Editor  quit
//
// The walk itself is HE::EngineDeps::resolve (HE_Core, no network of its own); this file
// is the sign-in, the progress and the question.
//
// HE_SKIP_PROJECT_PREFLIGHT=1 opens projects the old way (CI, image tests).
namespace ProjectPreflight
{
	// Open `projectFile` — after the check. Returns at once: the check runs on a worker and
	// the project is opened (or not) from render(). Does nothing while one is under way.
	void request(AppContext& ctx, const std::string& projectFile);

	// The project the editor was launched with / used last, before there is an AppContext
	// to open it with: remembered, and handed to request() by the first render().
	void requestAtStartup(const std::string& projectFile);

	// Draws the progress window and the question. Call every frame, on the hub and over
	// the editor alike (EditorUI::render does).
	void render(AppContext& ctx);

	// A check is running or waiting for the user's answer.
	bool busy();

	// Stops a running check and joins its worker (editor shutdown).
	void shutdown();
}
