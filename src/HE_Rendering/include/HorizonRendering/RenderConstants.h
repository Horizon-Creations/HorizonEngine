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

// Obergrenze der pro Frame einzeln gemessenen Pässe. Ein Backend legt seine
// Query-Kapazität danach an, statt sie mitwachsen zu lassen: auf Vulkan muss der
// Query-Pool zurückgesetzt werden, bevor der erste Render-Pass beginnt, und zu
// diesem Zeitpunkt ist noch unbekannt, wie viele Pässe der Frame haben wird.
// Pässe jenseits der Grenze werden verworfen, nie außerhalb des Bereichs
// geschrieben. OpenGL misst heute 13 — 16 lässt Luft, ohne den Pool aufzublähen.
inline constexpr int kMaxTimedPasses = 16;

} // namespace HE
