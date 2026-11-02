#version 450
// Roughness lerp between the SHARP trace and the blurred copy — port of
// kGiReflMixHLSL / GL's kGiReflMixFS. Without it a mirror would take the same
// blur as a rough receiver (the blur is sized to the glossy lobe, i.e. wide).
// The sharp copy may only win where the trace is DETERMINISTIC: the kernel
// opens its cone above roughness 0.08, and from there every ray is a random
// sample, so the sharp copy is raw noise, not detail. uParams.x is the
// resolution floor — below full resolution even a mirror needs some blur, or
// its upsampled stair-steps show. Shares the layout with gi_refl_blur.frag
// (unused bindings there hold valid views, same convention as gi_atrous.frag
// reusing gi_temporal.frag's DSL).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D uSharp;
layout(set = 0, binding = 1) uniform sampler2D uBlurred;
layout(set = 0, binding = 2) uniform sampler2D uGNorm; // a = receiver roughness (this target's size)
layout(push_constant) uniform GiReflMixPC { vec4 uParams; };

void main()
{
    float rough = clamp(texelFetch(uGNorm, ivec2(gl_FragCoord.xy), 0).a, 0.0, 1.0);
    float t = max(clamp(uParams.x, 0.0, 1.0), smoothstep(0.08, 0.25, rough));
    FragColor = mix(texture(uSharp, vUV), texture(uBlurred, vUV), t);
}
