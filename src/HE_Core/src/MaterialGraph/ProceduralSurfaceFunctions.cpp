// Procedural surface material functions — see ProceduralSurfaceFunctions.h.
#include <MaterialGraph/ProceduralSurfaceFunctions.h>

#include "MatFnBuilder.h"

#include <map>

namespace HE
{
namespace
{
using T = MatNodeType;
using P = MatPinType;
using MatFnBuild::Fn;
using R = Fn::R;

constexpr float kTwoPi  = 6.2831853f;
constexpr float kHalfPi = 1.5707963f;

// The arithmetic the two surfaces are written in. Every node the graph has is a vec3 op, so a
// scalar is a vec3 splat that the next consumer narrows again — the shader compiler drops the
// lanes nobody reads. k() shares one constant node per value.
struct Ops
{
    Fn& f;
    std::map<float, R> consts;

    R k(float v)
    {
        auto it = consts.find(v);
        if (it == consts.end()) it = consts.emplace(v, f.c(v)).first;
        return it->second;
    }
    R pin(R r, int p) const { return { r.node, p }; }

    R add(R a, R b)        { return f.e(T::Add, { a, b }); }
    R add(R a, float b)    { return add(a, k(b)); }
    R sub(R a, R b)        { return f.e(T::Subtract, { a, b }); }
    R sub(R a, float b)    { return sub(a, k(b)); }
    R mul(R a, R b)        { return f.e(T::Multiply, { a, b }); }
    R mul(R a, float b)    { return mul(a, k(b)); }
    R div(R a, R b)        { return f.e(T::Divide, { a, b }); }   // a / max(b, 1e-5): no zero divide
    R mix(R a, R b, R t)   { return f.e(T::Lerp, { a, b, t }); }
    R mix(float a, float b, R t) { return mix(k(a), k(b), t); }
    R inv(R x)             { return f.e(T::OneMinus, { x }); }
    R sat(R x)             { return f.e(T::Saturate, { x }); }
    R ss(float e0, float e1, R x) { return f.e(T::Smoothstep, { k(e0), k(e1), x }); }
    R ss(R e0, R e1, R x)  { return f.e(T::Smoothstep, { e0, e1, x }); }
    R pw(R x, float p)     { return f.e(T::Power, { x, k(p) }); }
    R sin(R x)             { return f.e(T::Sine, { x }); }
    R fract(R x)           { return f.e(T::Fract, { x }); }
    R split(R x)           { return f.e(T::SplitRGBA, { x }); }
};

// The wind seen from the ground: the unit direction (dx, dz) — +X when the input is zero, so
// a zero vector never reaches a division — and a position along and across it.
struct WindFrame { R dx, dz, along, across; };
WindFrame windFrame(Ops& o, R wind, R px, R pz)
{
    const R ws = o.split(wind);
    const R wx = o.pin(ws, 0), wz = o.pin(ws, 1);
    const R lenSq  = o.add(o.mul(wx, wx), o.mul(wz, wz));
    const R has    = o.f.e(T::Step, { o.k(1e-6f), lenSq });
    // Zero vector: lenSq + 1 keeps the power finite, and `has` picks the +X fallback anyway.
    const R invLen = o.pw(o.add(lenSq, o.inv(has)), -0.5f);
    const R dx = o.mix(o.k(1.0f), o.mul(wx, invLen), has);
    const R dz = o.mix(o.k(0.0f), o.mul(wz, invLen), has);
    return { dx, dz,
             o.add(o.mul(px, dx), o.mul(pz, dz)),     // along the wind
             o.sub(o.mul(pz, dx), o.mul(px, dz)) };   // across it
}

// 1 up close, 0 once a feature `cell` metres wide is a handful of pixels: noise finer than the
// screen can resolve only shimmers (a 1080p pixel is ~1 mm per metre of distance).
R fadeFar(Ops& o, R cell)
{
    const R dist = o.f.e(T::CameraDistance, {});
    return o.inv(o.ss(o.mul(cell, 150.0f), o.mul(cell, 450.0f), dist));
}

// The geometric world normal leaned by a height field's slope (gx, gz) — n - (gx, 0, gz), the
// normal of y = h(x, z) tilted by dh/dx, dh/dz on a surface that is mostly level.
R bendNormal(Ops& o, R gx, R gz)
{
    const R ns = o.split(o.f.e(T::NormalWS, {}));
    return o.f.e(T::Normalize3, { o.f.e(T::Combine3, { o.sub(o.pin(ns, 0), gx), o.pin(ns, 1),
                                                     o.sub(o.pin(ns, 2), gz) }) });
}

// "Albedo, Normal, Roughness, AO, Height" — the five outputs both surfaces end in.
void surfaceOutputs(Fn& f, R albedo, R normal, R roughness, R ao, R height)
{
    f.result("Albedo",    P::Vec3,  albedo);
    f.result("Normal",    P::Vec3,  normal);
    f.result("Roughness", P::Float, roughness);
    f.result("AO",        P::Float, ao);
    f.result("Height",    P::Float, height);
}
} // namespace

MaterialGraph buildProceduralSnowFunction()
{
    Fn f;
    Ops o{ f };
    // The inputs, in the order kSnowIn* names.
    const R pos      = f.input(f.in("Position",       P::Vec2,  0.0f));
    const R cover    = f.input(f.in("Cover",          P::Float, 1.0f));
    const R color    = f.input(f.in("Color",          P::Vec3,  0.85f));
    const R grain    = f.input(f.in("Grain",          P::Float, 0.5f));
    const R drift    = f.input(f.in("Drift",          P::Float, 0.5f));
    const R wind     = f.input(f.in("Wind Direction", P::Vec2,  1.0f));
    const R glitter  = f.input(f.in("Glitter",        P::Float, 0.35f));
    const R soft     = f.input(f.in("Softness",       P::Float, 0.5f));
    const R melt     = f.input(f.in("Melt",           P::Float, 0.0f));
    f.in("Disturbance", P::Float, 0.0f);   // reserved for trails: declared, not read (yet)

    const R ps = o.split(pos);
    const WindFrame w = windFrame(o, wind, o.pin(ps, 0), o.pin(ps, 1));

    // ── Drifts: ridges long along the wind (8 m) and narrow across it (1.8 m) ───────────────
    // FBM in the wind's own frame; its gradient comes with it, and turns back into a world
    // slope through the same two directions. More snow on the ground, more to blow around.
    const R q  = o.f.e(T::CombineRGBA, { o.mul(w.across, 0.55f), o.mul(w.along, 0.125f) });
    const R dn = o.f.e(T::FbmGrad, { q, o.k(1.0f) });
    const R driftAmp   = o.mul(o.mul(drift, 1.0f), o.add(o.mul(cover, 0.6f), 0.4f));
    const R slopeAcross = o.mul(o.mul(driftAmp, o.pin(dn, 1)), 0.55f);
    const R slopeAlong  = o.mul(o.mul(driftAmp, o.pin(dn, 2)), 0.125f);
    const R gxDrift = o.sub(o.mul(slopeAlong, w.dx), o.mul(slopeAcross, w.dz));
    const R gzDrift = o.add(o.mul(slopeAcross, w.dx), o.mul(slopeAlong, w.dz));

    // ── Grain: crystals a few centimetres wide ──────────────────────────────────────────────
    const R gn    = o.f.e(T::FbmGrad, { pos, o.k(16.0f) });
    const R g     = o.pin(gn, 0);
    const R fadeG = fadeFar(o, o.k(1.0f / 16.0f));
    // Powder buries the grain, meltwater and a deep cover smooth it.
    const R relax = o.mul(o.mul(o.inv(o.mul(soft, 0.65f)), o.inv(o.mul(melt, 0.6f))), o.inv(o.mul(cover, 0.4f)));
    const R grainRelief = o.mul(o.mul(grain, 0.2f), o.mul(relax, fadeG));
    const R gx = o.add(gxDrift, o.mul(o.pin(gn, 1), grainRelief));
    const R gz = o.add(gzDrift, o.mul(o.pin(gn, 2), grainRelief));
    const R normal = bendNormal(o, gx, gz);

    // ── Height: the structure alone — Position, Wind, Drift and Grain, nothing weather-driven ─
    const R d = o.pin(dn, 0);
    const R height = o.sat(o.add(o.add(o.mul(o.mul(drift, 0.55f), o.sub(d, 0.5f)),
                                       o.mul(o.mul(grain, 0.3f), o.sub(g, 0.5f))), 0.55f));

    // ── Colour: speckle, melt darkening, glitter ────────────────────────────────────────────
    const R speck = o.add(o.mul(o.mul(o.mul(o.sub(g, 0.47f), 0.6f), grain), o.mul(fadeG, o.inv(o.mul(soft, 0.5f)))), 1.0f);
    const R spark = o.mul(o.mul(o.mul(o.ss(0.62f, 0.8f, g), glitter), fadeG),
                          o.mul(o.inv(o.mul(melt, 0.6f)), o.inv(o.mul(soft, 0.4f))));
    const R meltDark = o.mix(1.0f, 0.8f, melt);
    const R albedo = o.sat(o.add(o.mul(o.mul(color, speck), meltDark), o.mul(spark, 0.3f)));

    // ── Roughness: crust is glossy, powder matte, meltwater and glittering crystals catch light ─
    const R base = o.mix(0.55f, 0.92f, soft);
    const R rough0 = o.sub(o.mul(base, o.mix(1.0f, 0.4f, melt)), o.mul(spark, 0.45f));
    const R roughness = o.add(o.mul(o.sat(rough0), 0.95f), 0.05f);

    // ── AO: the troughs of the drifts and the pits between the grains ───────────────────────
    const R cavity = o.add(o.mul(o.mul(drift, 0.6f), o.inv(d)), o.mul(o.mul(grain, 0.12f), o.inv(g)));
    const R ao = o.sat(o.inv(o.mul(cavity, o.inv(o.mul(soft, 0.6f)))));

    surfaceOutputs(f, albedo, normal, roughness, ao, height);
    return f.g;
}

MaterialGraph buildProceduralSandFunction()
{
    Fn f;
    Ops o{ f };
    // The inputs, in the order kSandIn* names.
    const R pos        = f.input(f.in("Position",       P::Vec2,  0.0f));
    const R color      = f.input(f.in("Color",          P::Vec3,  0.76f));
    const R grainSize  = f.input(f.in("Grain Size",     P::Float, 0.03f));
    const R ripple     = f.input(f.in("Ripple",         P::Float, 0.5f));
    const R rippleSize = f.input(f.in("Ripple Size",    P::Float, 0.3f));
    const R wind       = f.input(f.in("Wind Direction", P::Vec2,  1.0f));
    const R glitter    = f.input(f.in("Glitter",        P::Float, 0.3f));
    const R wet        = f.input(f.in("Wetness",        P::Float, 0.0f));
    f.in("Disturbance", P::Float, 0.0f);   // reserved for trails: declared, not read (yet)

    const R ps = o.split(pos);
    const WindFrame w = windFrame(o, wind, o.pin(ps, 0), o.pin(ps, 1));

    // ── Grain: the speckle of the sand itself ───────────────────────────────────────────────
    const R gn    = o.f.e(T::FbmGrad, { pos, o.div(o.k(1.0f), grainSize) });
    const R g     = o.pin(gn, 0);
    const R fadeG = fadeFar(o, grainSize);
    // Wet sand packs down: the grain relief halves.
    const R grainRelief = o.mul(o.mul(o.k(0.3f), fadeG), o.inv(o.mul(wet, 0.5f)));

    // ── Ripples: ridges across the wind, wavelength "Ripple Size", wandering with a slow noise ─
    // The phase is the distance along the wind in wavelengths plus a low-frequency wobble (4 m
    // cells, integer hash) worth several wavelengths, so the crests bend, fork and bunch up
    // instead of ruling the ground like lines; the same noise takes the ripples out of the flat
    // patches between the fields of them. A crest is a sine and a weaker second harmonic: the
    // windward flank long and gentle, the lee flank short and steep.
    // The phase goes through fract() before the sine: a kilometre from the origin it is tens of
    // thousands of radians, where a GPU's sin() is no longer exact.
    const R wob = o.f.e(T::Fbm, { pos, o.k(0.25f) });
    o.f.g.findNode(wob.node)->p[0] = 1.0f;
    const R phase = o.mul(o.fract(o.add(o.div(w.along, rippleSize), o.mul(o.sub(wob, 0.47f), 5.0f))), kTwoPi);
    const R phase2 = o.add(o.mul(phase, 2.0f), 1.1f);
    const R shape  = o.add(o.sin(phase), o.mul(o.sin(phase2), 0.3f));                          // the crest profile
    const R slope  = o.add(o.sin(o.add(phase, kHalfPi)), o.mul(o.sin(o.add(phase2, kHalfPi)), 0.6f)); // its derivative in phase
    const R fadeR = fadeFar(o, rippleSize);
    const R rip   = o.mul(o.mul(ripple, fadeR), o.ss(0.28f, 0.5f, wob));
    const R slopeRip = o.mul(o.mul(rip, 0.2f), slope);   // d(height)/d(along): the crests' flank slope
    const R gx = o.add(o.mul(slopeRip, w.dx), o.mul(o.pin(gn, 1), grainRelief));
    const R gz = o.add(o.mul(slopeRip, w.dz), o.mul(o.pin(gn, 2), grainRelief));
    const R normal = bendNormal(o, gx, gz);

    // ── Height: ripples and grain ───────────────────────────────────────────────────────────
    const R height = o.sat(o.add(o.add(o.mul(o.mul(ripple, 0.25f), shape), o.mul(o.sub(g, 0.47f), 0.3f)), 0.5f));

    // ── Colour: speckle, lighter crests, wet darkening, glitter ─────────────────────────────
    const R speck = o.add(o.mul(o.mul(o.sub(g, 0.47f), 0.45f), fadeG), 1.0f);
    const R crest = o.add(o.mul(o.mul(o.mul(ripple, 0.07f), shape), o.mul(fadeR, o.ss(0.28f, 0.5f, wob))), 1.0f);
    const R wetDark = o.mix(1.0f, 0.52f, wet);
    const R spark = o.mul(o.mul(o.mul(o.ss(0.66f, 0.82f, g), glitter), fadeG), o.inv(wet));
    const R albedo = o.sat(o.add(o.mul(o.mul(o.mul(color, speck), crest), wetDark), o.mul(spark, 0.22f)));

    // ── Roughness: dry sand is rough, wet sand sleek, a glint is a mirror ───────────────────
    const R rough0 = o.sub(o.mul(o.k(0.92f), o.mix(1.0f, 0.45f, wet)), o.mul(spark, 0.5f));
    const R roughness = o.add(o.mul(o.sat(rough0), 0.95f), 0.05f);

    // ── AO: the troughs between the ripples, the pits between the grains ────────────────────
    const R aoRip   = o.mul(o.mul(rip, 0.12f), o.inv(shape));
    const R aoGrain = o.mul(o.mul(o.k(0.12f), fadeG), o.sat(o.sub(o.k(0.5f), g)));
    const R ao = o.sat(o.inv(o.add(aoRip, aoGrain)));

    surfaceOutputs(f, albedo, normal, roughness, ao, height);
    return f.g;
}
} // namespace HE
