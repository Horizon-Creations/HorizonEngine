#pragma once

struct AppContext;

// ── Stamp Heightmap (Window ▸ Stamp Heightmap) ─────────────────────────────────
// Paints the heightmap a Landscape Stamp lays on the tile landscape (LandscapeStampComponent::heightmapFile): raise, lower
// and smooth with a brush, drag lines whose height runs from one value to another, drag transitions over the whole picture.
// The picture is saved as a 16-bit PGM under <project>/Heightmaps; with Live Preview on, every change is written and the
// stamps that use the file are read again, so the landscape follows the strokes. The strokes themselves are StampHeightmap.h.
namespace StampHeightmapPanel
{
	// `open` is the Window-menu toggle; the window clears it when closed.
	void DrawWindow(AppContext& ctx, bool& open);
}
