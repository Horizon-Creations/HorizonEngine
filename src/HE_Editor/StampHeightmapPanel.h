#pragma once
#include <string>

struct AppContext;
struct ImVec2;

// ── Stamp Heightmap (an editor tab) ────────────────────────────────────────────
// Makes the heightmap a Landscape Stamp lays on the tile landscape (LandscapeStampComponent::heightmapFile). Three ways in one
// picture, all in StampHeightmap.h: generate a landform (mountain, range, plateau, crater, volcano, dunes, island, hills) from a
// seed; sketch roughly with brushes, lines and transitions and let Naturalize turn the sketch into ground that looks grown
// (outlines bent, ridged relief, water and gravity erosion); filter (erode, blur, terrace, normalize, fade the edges). A painted
// selection limits generators and filters to a region, so one part can be made again with another seed while the rest stays.
//
// The tab shows the picture and, beside it, a test landscape: a terrain of the engine built from the picture and drawn by the
// renderer's world preview (the path the Mesh Viewer uses), so it is lit and shaded as ground is. It follows every change.
//
// The picture is a file, not an asset: a 16-bit PGM under <project>/Heightmaps, the form a stamp reads at full precision. With
// Live Preview on, every change is written and the scene's stamps are read again, so the real landscape follows too.
// Ways in: Window > Stamp Heightmap, "Edit Heightmap..." on a Landscape Stamp in the Inspector and on the Landscape mode's Stamp tool.
namespace StampHeightmapPanel
{
	// Sentinel "asset path" identifying the tab.
	constexpr const char* kTabPath = "::StampHeightmap::";

	// Fill the given tab rect with the editor (borderless).
	void render(AppContext& ctx, const ImVec2& pos, const ImVec2& size);
	// Load a stamp's heightmap file into the editor (absolute, or relative to the project's folder); "" keeps the picture.
	void open(AppContext& ctx, const std::string& file);
	// Write the picture to its file (the tab's Ctrl+S). False when it could not be written.
	bool save(AppContext& ctx);
}
