#pragma once
#include <string>
#include <HorizonScene/HorizonWorld.h>   // Entity
#include <Types/UUID.h>
#include "AutoLod.h"

struct AppContext;

// ── Generate LODs ────────────────────────────────────────────────────────────
// One dialog and one runner behind every "Generate LODs" button (Thema 331,
// step 3): the Content Browser's mesh menu, a Foliage layer's Detail Levels and
// a mesh entity's LOD component. The simplifier lives in the tool library
// (AutoLod.h); this is the editor half: ask how many levels and how coarse,
// write the `<name>_LOD<n>` sidecar assets next to the mesh, and — for the
// layer and the entity — put the chain into FoliageLayer::lods / a
// LODComponent with distances worked out from the pixel error each level shows.
//
// Only the SCENE edit is undoable (the layer's levels, the component). The
// sidecar files are written to disk like an import: running it again with other
// settings overwrites them in place, keeping their UUIDs.
namespace AutoLodDialog
{
	enum class Kind
	{
		MeshAsset,      // only write the sidecar assets
		FoliageLayer,   // …and fill the layer's detail levels
		Entity,         // …and give the entity a LODComponent
	};

	struct Request
	{
		Kind     kind   = Kind::MeshAsset;
		HE::UUID mesh;                      // LOD0: the asset, the layer's or the entity's mesh
		Entity   entity = entt::null;       // FoliageLayer / Entity: the carrier
		int      layer  = 0;                // FoliageLayer: index into its FoliageComponent
	};

	// What the dialog asks. Kept for the session, so a second mesh starts with the
	// settings of the first.
	struct Options
	{
		int   levels      = 3;       // generated levels after LOD0
		float step        = 0.5f;    // share of the previous level each one keeps
		float lowest      = 10.0f;   // % of LOD0 the last level keeps
		float tolerancePx = 2.0f;    // pixel error a level may show where it takes over
		bool  foliage     = false;   // thin cards: whole pieces may vanish; the alpha keeps leaf area
		bool  crossCard   = false;   // FoliageLayer: end the chain on the engine cross card
	};
	Options& options();

	// The simplifier's input for these options.
	AutoLod::Settings toSettings(const Options& o);

	// The widgets only (no window), so a test can drive them. True when something
	// changed. `kind` decides which rows apply.
	bool drawOptions(Options& o, Kind kind);

	struct Outcome
	{
		bool        ok      = false;
		size_t      levels  = 0;       // generated, not counting LOD0
		uint32_t    lod0Tris = 0;
		uint32_t    lastTris = 0;
		std::string message;           // one line for the user: what happened, or why not
	};
	// Writes the chain and applies it; no window involved.
	Outcome run(AppContext& ctx, const Request& req, const Options& o);

	// Ask for the dialog: it opens on the next Draw().
	void open(const Request& req);
	// Once per frame from the editor's dialog block.
	void draw(AppContext& ctx);
}
