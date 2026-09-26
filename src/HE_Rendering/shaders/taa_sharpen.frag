#version 450

// The temporal average is softer than one frame by construction — this is the
// sharpen that buys that back, run in the AA-resolve slot on the resolved TAA
// history (GL's kTaaSharpenFS). Same bindings and push constants as
// postfx_aa_blit.frag, so it shares the postFx pipeline layout and render pass.
// params: xy = 1/resolution, z = amount (0 = exact copy).

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(set = 0, binding = 1) uniform sampler2D _dummy;
layout(push_constant) uniform PC { vec4 params; } pc;

void main()
{
    vec2 rcp = pc.params.xy;
    vec3 c   = texture(uScene, vUV).rgb;
    if (pc.params.z <= 0.0) { outColor = vec4(c, 1.0); return; }
    vec3 blur = 0.25 * (texture(uScene, vUV + vec2( rcp.x, 0.0)).rgb
                      + texture(uScene, vUV + vec2(-rcp.x, 0.0)).rgb
                      + texture(uScene, vUV + vec2(0.0,  rcp.y)).rgb
                      + texture(uScene, vUV + vec2(0.0, -rcp.y)).rgb);
    outColor = vec4(clamp(c + (c - blur) * pc.params.z, 0.0, 1.0), 1.0);
}
