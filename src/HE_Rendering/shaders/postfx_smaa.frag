#version 450
// SMAA-style spatial AA (docs/anti-aliasing-plan.md, A1) — the SPIR-V twin of
// the GL kSmaaFS / Metal smaaFragment / D3D kSmaaHLSL. Where FXAA guesses an
// edge tangent and blurs along it, this finds the span the pixel's boundary
// belongs to, classifies both of its ends, and derives the coverage analytically
// from the position inside the span. Orthogonal (L/Z/U) patterns only — no
// AreaTex, so no diagonals and no corner rounding: MLAA-tier, above FXAA.
// Same bindings/push constants as postfx_fxaa.frag so it shares the pipeline
// layout, the descriptor sets and the render pass.
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(set=0, binding=0) uniform sampler2D uScene;
layout(set=0, binding=1) uniform sampler2D _dummy;
layout(push_constant) uniform PC { vec4 params; } pc;

const float kEdgeMin     = 1.0 / 24.0;
const float kEdgeRel     = 1.0 / 8.0;
// 8 single-texel steps, then double steps: 32 texels of reach. Reach decides
// whether shallow edges get antialiased at all — see the GL twin.
const int   kFineSteps   = 8;
const int   kSearchIters = 20;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float lumaAt(vec2 uv) { return luma(texture(uScene, uv).rgb); }

float smaaSearch(vec2 uv, vec2 along, vec2 across, float thr,
                 out bool ended, out float outerLuma)
{
    ended = false; outerLuma = 0.0;
    float dist = 0.0;
    for (int i = 0; i < kSearchIters; ++i)
    {
        float st = (i < kFineSteps) ? 1.0 : 2.0;
        dist += st;
        vec2  p = uv + along * dist;
        float a = lumaAt(p);
        float b = lumaAt(p + across);
        if (abs(a - b) < thr) { ended = true; outerLuma = a; return dist - st; }
    }
    return dist;
}

float smaaCover(float t, float y1, float y2, bool split)
{
    float f = split ? ((t < 0.5) ? mix(y1, 0.0, t * 2.0) : mix(0.0, y2, (t - 0.5) * 2.0))
                    : mix(y1, y2, t);
    return max(0.0, -f);
}

float smaaWeight(vec2 uv, vec2 along, vec2 across, float lumaP, float lumaO, float thr)
{
    bool  e1, e2;
    float o1, o2;
    float d1 = smaaSearch(uv, -along, across, thr, e1, o1);
    float d2 = smaaSearch(uv,  along, across, thr, e2, o2);
    float len = d1 + d2 + 1.0;
    float t   = (d1 + 0.5) / len;
    float y1  = e1 ? (abs(o1 - lumaO) < abs(o1 - lumaP) ? -0.5 : 0.5) : 0.0;
    float y2  = e2 ? (abs(o2 - lumaO) < abs(o2 - lumaP) ? -0.5 : 0.5) : 0.0;
    bool  split = (e1 && e2 && y1 == y2);
    // Quadrature across the pixel, not one sample at its centre — see the GL twin.
    float dt = 0.25 / len;
    return 0.5 * (smaaCover(clamp(t - dt, 0.0, 1.0), y1, y2, split)
                + smaaCover(clamp(t + dt, 0.0, 1.0), y1, y2, split));
}

void main()
{
    vec2  rcp = pc.params.xy;
    vec3  C   = texture(uScene, vUV).rgb;
    float lC  = luma(C);
    float lW  = lumaAt(vUV + vec2(-rcp.x, 0.0));
    float lE  = lumaAt(vUV + vec2( rcp.x, 0.0));
    float lN  = lumaAt(vUV + vec2(0.0, -rcp.y));
    float lS  = lumaAt(vUV + vec2(0.0,  rcp.y));
    float lMax = max(lC, max(max(lW, lE), max(lN, lS)));
    float lMin = min(lC, min(min(lW, lE), min(lN, lS)));
    float thr  = max(kEdgeMin, lMax * kEdgeRel);
    if (lMax - lMin < thr) { outColor = vec4(C, 1.0); return; }

    float edgeH = abs(lN - 2.0 * lC + lS);
    float edgeV = abs(lW - 2.0 * lC + lE);
    float wA = 0.0, wB = 0.0;
    vec2  offA, offB;
    if (edgeH >= edgeV)
    {
        offA = vec2(0.0, -rcp.y); offB = vec2(0.0, rcp.y);
        if (abs(lC - lN) >= thr) wA = smaaWeight(vUV, vec2(rcp.x, 0.0), offA, lC, lN, thr);
        if (abs(lC - lS) >= thr) wB = smaaWeight(vUV, vec2(rcp.x, 0.0), offB, lC, lS, thr);
    }
    else
    {
        offA = vec2(-rcp.x, 0.0); offB = vec2(rcp.x, 0.0);
        if (abs(lC - lW) >= thr) wA = smaaWeight(vUV, vec2(0.0, rcp.y), offA, lC, lW, thr);
        if (abs(lC - lE) >= thr) wB = smaaWeight(vUV, vec2(0.0, rcp.y), offB, lC, lE, thr);
    }

    float sum = wA + wB;
    if (sum > 1.0) { wA /= sum; wB /= sum; sum = 1.0; }
    vec3 outC = C * (1.0 - sum)
              + wA * texture(uScene, vUV + offA).rgb
              + wB * texture(uScene, vUV + offB).rgb;
    outColor = vec4(outC, 1.0);
}
