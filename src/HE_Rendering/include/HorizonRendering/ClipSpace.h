#pragma once
#include <Math/Math.h>

// ─── Clip-space remaps ────────────────────────────────────────────────────────
// The shared RenderExtractor builds every camera / light projection with OpenGL
// conventions (Y up, depth -1..1). Backends whose NDC differs pre-multiply one of
// these fix-ups instead of depending on how glm happened to be compiled for them
// (GLM_FORCE_DEPTH_ZERO_TO_ONE is set on some targets and not others, so the
// matrices must be explicit).
//
// Header-only constants: the backend static libraries reuse HorizonRendering's
// include path but do not link the shared library, and exported *data* has no
// import thunk on MSVC — so these must be inline, not HE_RENDERING_API.
namespace HE
{

// Depth only: -1..1 → 0..1. NDC y stays up. Used by D3D11, D3D12 and Metal —
// their three local copies were byte-identical. D3D and Metal both flip V when
// SAMPLING instead (texture origin is top-left), which is why no y flip belongs
// in the matrix.
inline const glm::mat4 kZeroToOneDepthClipFix(
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.5f, 0.0f,
	0.0f, 0.0f, 0.5f, 1.0f);

// Backend-facing spellings of the same matrix — kept so call sites still read as
// "this is the D3D fix" / "this is the Metal fix".
inline const glm::mat4& kD3DClipFix   = kZeroToOneDepthClipFix;
inline const glm::mat4& kMetalClipFix = kZeroToOneDepthClipFix;

// Vulkan additionally flips Y in clip space (its NDC y points down).
inline const glm::mat4 kVulkanClipFix(
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f,-1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.5f, 0.0f,
	0.0f, 0.0f, 0.5f, 1.0f);

} // namespace HE
