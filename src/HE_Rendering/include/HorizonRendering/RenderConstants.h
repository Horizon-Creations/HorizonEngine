#pragma once
#include <cstdint>

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

// Most instances one instanced draw may carry. Every backend with an instance ring
// (Metal, D3D11, D3D12, Vulkan) has its own `k_maxInstances = 65536` and, ABOVE it,
// quietly drops back to one draw per instance — a 140k-plant batch cost 140k draws.
// GeometryPass and RenderSorter::batchDepthRuns cut a longer run into several draws
// of at most this many, so each one still fits. Keep it equal to the smallest of the
// backends' limits (a backend that lowers its own must lower this too). D3D12 and
// Vulkan additionally spend one ring slot per instance per frame across ALL passes;
// splitting does not change that budget, it only stops one huge batch from missing it.
inline constexpr uint32_t kMaxInstancesPerDraw = 65536;

} // namespace HE
