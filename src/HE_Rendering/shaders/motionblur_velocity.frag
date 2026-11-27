// Camera motion blur, pass 1 (IRenderer::MotionBlurSettings): GL's kMbVelocityFS / HlslSources.h's kMbVelocityHLSL in the
// Vulkan convention. The depth holds Vulkan clip z (kVulkanClipFix: 0..1, ndc y down, the same as uv), so the pixel's ndc
// point is (uv * 2 - 1, d) and the C++ side builds reproject from the clip-fixed matrices (HE::motionBlurReproject).
// Velocity in PIXELS (RG16F), capped at the max blur.
#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec2 outVelocity;
layout(set=0, binding=0) uniform sampler2D uDepth;
layout(push_constant) uniform PC
{
    mat4 reproject;   // (fix * prevViewProj) * inverse(fix * viewProj)
    vec4 params;      // x shutter fraction, y max blur (px), zw target size (px)
    vec4 texel;       // xy = 1 / target size
} pc;
void main()
{
    float d    = texture(uDepth, vUV).r;
    vec4  prev = pc.reproject * vec4(vUV * 2.0 - 1.0, d, 1.0);
    if (prev.w <= 1e-5) { outVelocity = vec2(0.0); return; }   // behind the previous camera
    vec2  prevUV = (prev.xy / prev.w) * 0.5 + 0.5;
    vec2  vPx    = (vUV - prevUV) * pc.params.zw * pc.params.x;
    float len    = length(vPx);
    if (len > pc.params.y) vPx *= pc.params.y / len;
    outVelocity = vPx;
}
