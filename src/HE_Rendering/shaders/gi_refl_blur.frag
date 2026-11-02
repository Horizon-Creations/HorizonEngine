#version 450
// Separable 5-tap Gaussian over the half-res reflection result, CONFIDENCE-
// WEIGHTED (a miss, a = 0, contributes no colour, so the edges of a hit
// region do not bleed toward black — the blurred confidence feathers them
// instead). Port of kGiReflBlurHLSL / GL's kGiReflBlurFS. uDir.xy = one-stride
// UV step along the blur axis; the host runs it twice per a-trous level (H
// then V), scaling the stride per level (GIReflectionTier.h).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D uSrc;
layout(push_constant) uniform GiReflBlurPC { vec4 uDir; };

void main()
{
    const float w[5] = float[5](0.0625, 0.25, 0.375, 0.25, 0.0625);
    vec3  sumC = vec3(0.0);
    float sumA = 0.0;
    for (int i = -2; i <= 2; ++i)
    {
        vec4  s  = texture(uSrc, vUV + uDir.xy * float(i));
        float wi = w[i + 2];
        sumC += s.rgb * s.a * wi;
        sumA += s.a * wi;
    }
    FragColor = vec4(sumC / max(sumA, 1e-4), sumA);
}
