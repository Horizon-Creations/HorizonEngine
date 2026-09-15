#pragma once
#include <HorizonScene/HorizonWorld.h>   // Entity
#include <functional>
#include <string>

struct AppContext;
struct ImVec2;
class  RenderWorld;
namespace HE { struct UUID; }

// ── Landscape (terrain) editing tools ────────────────────────────────────────
// Everything the Landscape editor mode owns: the sculpt/paint/foliage brush
// state (tool, radius, falloff, strength, the stroke-scoped Flatten/Ramp
// targets, the foliage Grow/Erase choice and its target density),
// the brush cursor + stroke handling drawn over the Scene viewport, and the
// Landscape tool panel that replaces Quick Settings while the mode is active.
// Both halves share the brush state, which is why they live in one file.
// Split out of EditorUI.cpp; all of that state is file-static in the .cpp.
namespace TerrainTools
{
	// Drawn inside the Scene viewport, after the scene extract (it needs this
	// frame's camera) and after picking. `rectMin`/`rectMax` are the viewport
	// image's screen rect, `navigating` suppresses the cursor during a fly-look,
	// `viewportHovered` gates the stroke start, `dt` paces the per-second brush.
	// Sculpting rebuilds the terrain mesh, so the viewport's picking AABB cache
	// entry for it goes stale — the viewport owns that cache and hands in the
	// invalidation.
	void sculptInViewport(AppContext& ctx, const RenderWorld& sceneSnapshot,
	                      const ImVec2& rectMin, const ImVec2& rectMax,
	                      bool navigating, bool viewportHovered, float dt,
	                      const std::function<void(const HE::UUID&)>& invalidateMeshAabb);

	// Body of the "Landscape###Quick Settings" window while Landscape mode is
	// active: terrain creation form, landscape material, sculpt/paint mode and
	// the brush settings. The window's Begin/End stays with the editor shell,
	// which also draws Quick Settings into the same window id in View mode.
	void renderPanel(AppContext& ctx);

	// ── Heightmap import ──────────────────────────────────────────────────
	// The "Heightmap" block: the texture slot behind
	// TerrainComponent::heightmapTexture, Apply, and "Import Heightmap File…"
	// (a native file dialog, 8/16-bit PNG, PGM or .r16). Drawn by the
	// Landscape panel's Sculpt page and by the Details panel's Terrain
	// section — one function, so the two cannot disagree about what an import
	// does (replaces the sculpt, keeps paint and foliage, takes an undo step).
	// `terrain` is the entity holding the TerrainComponent.
	void drawHeightmapBlock(AppContext& ctx, Entity terrain);

	// The two imports behind the block, without the widgets: pull the slotted
	// texture asset into the heights, or read a file. Both take the undo
	// snapshot, mark the foliage layer dirty and log. The string is the status
	// line the block shows afterwards; `error` says which colour to draw it.
	std::string applyHeightmapAsset(AppContext& ctx, Entity terrain, bool& error);
	std::string importHeightmapFile(AppContext& ctx, Entity terrain,
	                                const std::string& path, bool& error);
}
