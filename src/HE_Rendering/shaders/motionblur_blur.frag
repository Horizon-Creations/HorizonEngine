// Camera motion blur, pass 2: GL's kMbBlurFS / HlslSources.h's kMbBlurHLSL. 12 taps + centre along the centre pixel's
// velocity, a tap weighted by how fast IT moves (a still foreground edge is not dragged into a streaking background);
// under half a pixel of motion the input passes through untouched.
#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(set=0, binding=0) uniform sampler2D uImage;      // the HDR image (the fog applied)
layout(set=0, binding=1) uniform sampler2D uVelocity;   // velocity in pixels
layout(push_constant) uniform PC
{
    mat4 reproject;
    vec4 params;
    vec4 texel;       // xy = 1 / target size
} pc;
const int kTaps = 6;  // per side
void main()
{
    vec2  vC = texture(uVelocity, vUV).rg;
    float lC = length(vC);
    vec4  c0 = texture(uImage, vUV);
    if (lC < 0.5) { outColor = vec4(c0.rgb, 1.0); return; }   // still: pass through, bit-exact
    vec3  sum  = c0.rgb;
    float wsum = 1.0;
    for (int i = 1; i <= kTaps; ++i)
    {
        float t = float(i) / float(kTaps + 1);
        for (int s = -1; s <= 1; s += 2)
        {
            vec2  uv = vUV + vC * (0.5 * t * float(s)) * pc.texel.xy;
            vec2  vT = texture(uVelocity, uv).rg;
            float w  = clamp(length(vT) * 2.0 / lC, 0.0, 1.0);
            sum  += texture(uImage, uv).rgb * w;
            wsum += w;
        }
    }
    outColor = vec4(sum / wsum, 1.0);
}
