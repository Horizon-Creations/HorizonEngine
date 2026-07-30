#pragma once
#include <Renderer/IRenderer.h>

struct EnvironmentComponent;

namespace HE {

// Build the renderer's EnvironmentSettings from a scene's Sky (EnvironmentComponent).
// The editor viewport and the packaged game runtime are supposed to render the SAME
// world, so this field map exists exactly once — it used to be copy-pasted into both
// (EditorApplication::pushEnvironment and GameApplication::OnRender) and could drift
// silently with every new sky knob.
//
// !! MUTATES `env` !!  It looks like a builder, but with dayNightCycle + autoAdvance
// set it ADVANCES timeOfDay (and, when moonPhaseAuto, moonPhase) by `dt`. It is a
// per-frame TICK: call it exactly ONCE per frame per app, and pass dt = 0 for a pure
// read (the editor's headless dump does that).
//
// Callers handle the no-Sky case themselves: no Sky entity → push
// EnvironmentSettings{ .skyEnabled = false } so the backend skips the sky pass.
IRenderer::EnvironmentSettings makeEnvironmentSettings(EnvironmentComponent& env, float dt);

} // namespace HE
