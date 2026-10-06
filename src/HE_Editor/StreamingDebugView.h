#pragma once
#include <glm/vec3.hpp>

class HorizonWorld;
class DebugDrawBuffer;
struct AppContext;
namespace HE { struct CellManifest; }

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

	// The Streaming tab's body.
	void draw(AppContext& ctx);
}
