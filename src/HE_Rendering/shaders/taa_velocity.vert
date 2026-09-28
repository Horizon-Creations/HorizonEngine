#version 450

// TAA velocity pass (docs/anti-aliasing-plan.md A2), the Vulkan twin of GL's
// kTaaVelocityVS and D3D's kTaaVelocityHLSL. Positions only; the pipeline tests
// LESS_OR_EQUAL against the scene depth without writing it.
//
// Two matrices instead of GL's three, to stay inside the 128 bytes of push
// constants every Vulkan device guarantees (the scene's own PushConstants
// layout): mvpJitter is the scene draw's own mvp (so the raster reproduces its
// depth), mvpPrevJitter is LAST frame's clean mvp with THIS frame's jitter
// applied on the left. The jitter is a pure clip-space x/y shift of j * w, so
// ndc(mvpJitter) - ndc(mvpPrevJitter) = ndcNow - ndcPrev exactly — the jitter
// cancels and only real motion is left.

layout(location = 0) in vec3 aPos;

layout(push_constant) uniform PushConstants {
    mat4 mvpJitter;
    mat4 mvpPrevJitter;
} pc;

layout(location = 0) out vec4 vClipNow;
layout(location = 1) out vec4 vClipPrev;

void main()
{
    vec4 p = vec4(aPos, 1.0);
    gl_Position = pc.mvpJitter * p;
    vClipNow    = gl_Position;
    vClipPrev   = pc.mvpPrevJitter * p;
}
