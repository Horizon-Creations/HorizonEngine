#pragma once

// ── Viewport view mode: names and the headless override ─────────────────────
// The mode itself is HE::ViewMode (IRenderer.h); this is the editor's small
// glue around it — how a mode is spelled on the command line, and the rule
// that lets a headless capture win over whatever the toolbar holds. Header-
// only and ImGui-free so the dump path, the panel and a test share the exact
// same reading of the environment.
//
//   HE_DUMP_VIEWMODE=lit|unlit|wireframe|basecolor|normals|roughspecmetal|
//                    emissive   (or the enum's number)
//   HE_DUMP_GBUFFER=1..4        the older knob — one G-buffer attachment;
//                               still honoured, VIEWMODE wins when both are set
//
// Both are read once (statics — the environment does not change under a
// running process) exactly like the other HE_DUMP_* per-frame overrides in
// EditorApplication, so an interactive session started with one of them set
// keeps that view no matter what the toolbar is clicked to.

#include "Renderer/IRenderer.h"

#include <cstdlib>
#include <cstring>
#include <string_view>

namespace HE::Ed
{

// "unlit" → Unlit, "3" → GBufferBaseColor, anything unknown → `fallback`.
inline HE::ViewMode viewModeFromName(const char* name, HE::ViewMode fallback)
{
	if (!name || !*name) return fallback;
	const std::string_view s(name);
	if (s == "lit")            return HE::ViewMode::Lit;
	if (s == "unlit")          return HE::ViewMode::Unlit;
	if (s == "wireframe")      return HE::ViewMode::Wireframe;
	if (s == "basecolor")      return HE::ViewMode::GBufferBaseColor;
	if (s == "normals")        return HE::ViewMode::GBufferNormal;
	if (s == "roughspecmetal") return HE::ViewMode::GBufferRoughSpecMetal;
	if (s == "emissive")       return HE::ViewMode::GBufferEmissive;
	char* end = nullptr;
	const long v = std::strtol(name, &end, 10);
	if (end && *end == '\0' && v >= 0 && v < HE::kViewModeCount)
		return static_cast<HE::ViewMode>(v);
	return fallback;
}

// The environment's say, given what the UI wants. `viewmode` and `gbuffer`
// are the raw variable values (nullptr / "" = unset) so a test can feed them
// without touching the process environment.
inline HE::ViewMode viewModeOverride(HE::ViewMode fromUi, const char* viewmode, const char* gbuffer)
{
	if (viewmode && *viewmode) return viewModeFromName(viewmode, fromUi);
	if (gbuffer && *gbuffer)
	{
		const int n = std::atoi(gbuffer);
		if (n >= 1 && n <= 4)
			return static_cast<HE::ViewMode>(
				static_cast<int>(HE::ViewMode::GBufferBaseColor) + n - 1);
	}
	return fromUi;
}

inline HE::ViewMode viewModeOverrideFromEnv(HE::ViewMode fromUi)
{
	static const char* s_viewmode = std::getenv("HE_DUMP_VIEWMODE");
	static const char* s_gbuffer  = std::getenv("HE_DUMP_GBUFFER");
	return viewModeOverride(fromUi, s_viewmode, s_gbuffer);
}

} // namespace HE::Ed
