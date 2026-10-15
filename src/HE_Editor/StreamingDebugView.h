#pragma once
#include <glm/vec3.hpp>
#include <string>
#include <vector>

class HorizonWorld;
class DebugDrawBuffer;
struct AppContext;
namespace HE { struct CellManifest; struct CellSplitOptions; struct CellAnchor; }

// ── World streaming, seen from the editor (Thema 153, Schritt 6) ─────────────
// What the streaming machinery is doing right now, in one place: the job
// pool's queues per priority, the asset loads in flight, the last scene load,
// the open scene's streaming cells and how large the world has become. Drawn
// as the Streaming tab of the Performance Profiler, so it needs no window,
// dock slot or menu entry of its own.
//
// The cells also show in the Scene window (Show ▸ Streaming Cells): their
// squares on the ground, coloured by what the game would do with each from the
// editor camera, and the load and unload radius around it.
namespace StreamingDebugView
{
	// The world's cell manifest (HorizonWorld::cellManifestJson), parsed once per
	// change of its text. Null for a scene without cells or with a malformed one.
	const HE::CellManifest* manifestOf(const HorizonWorld& world);

	// The cell squares within reach of `eye` (world space, the editor camera) and
	// the two radii around it, as debug lines. Nothing for a scene without cells.
	void appendCellLines(const HorizonWorld& world, const glm::vec3& eye, DebugDrawBuffer& out);
	// The same with preview anchors besides the camera (absolute positions): the game
	// keeps a cell around while ANY anchor is near it, so each square is coloured by
	// the nearest of them and each gets its own pair of radii, stretched by its
	// radiusScale.
	void appendCellLines(const HorizonWorld& world, const glm::vec3& eye,
	                     const std::vector<HE::CellAnchor>& pins, DebugDrawBuffer& out);

	// The preview anchors the Streaming tab keeps for the Scene window: what the game
	// would hold if a script pinned a cell there (Thema 164), for the editor to look at
	// before the script API exists. Not saved, not part of the scene.
	std::vector<HE::CellAnchor>& previewPins();

	// The open scene split into streaming cells (HE::splitWorldIntoCells): the
	// cell files go to "<scene name>.cells" beside the scene file, the world
	// becomes the base, one undo entry, unsaved. `options.dir` is ignored.
	// The scene must have been saved once. `message` says what happened.
	bool splitOpenScene(AppContext& ctx, const HE::CellSplitOptions& options, std::string& message);
	// The open scene's cells loaded back into it (HE::mergeCellsIntoWorld), one
	// undo entry, unsaved. The cell files stay where they are.
	bool mergeOpenScene(AppContext& ctx, std::string& message);

	// The Streaming tab's body.
	void draw(AppContext& ctx);
}
