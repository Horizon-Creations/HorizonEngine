#pragma once
#include <functional>

struct AppContext;
struct ImVec2;
class  RenderWorld;
namespace HE { struct UUID; }

// ── Landscape (terrain) editing tools ────────────────────────────────────────
// Everything the Landscape editor mode owns: the sculpt/paint brush state
// (tool, radius, falloff, strength and the stroke-scoped Flatten/Ramp targets),
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
}
