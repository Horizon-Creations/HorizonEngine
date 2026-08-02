#pragma once

// ─── Cross-backend renderer constants ────────────────────────────────────────
// Values every backend must agree on, previously repeated as bare literals.
// Header-only (see ClipSpace.h for why exported data is avoided here).
namespace HE
{

// Directional shadow map / CSM cascade side length in texels. Every backend
// allocated its own `2048`; a cascade and a single whole-scene map are both this
// size, so the CSM backends allocate a kShadowMapResolution² array slice per
// cascade and the single-map backends one kShadowMapResolution² texture.
inline constexpr int kShadowMapResolution = 2048;

// GPU-timer query ring depth. A slot's results are only read back this many
// frames after they were issued, so the read never stalls the pipeline; a slot
// that is still not ready at that age is dropped rather than waited on.
// Shared by the three backends that ring their timestamp queries (OpenGL,
// Vulkan, D3D11) — the ring *payload* is per-API and stays backend-side.
inline constexpr int kGpuTimerRing = 4;

} // namespace HE
