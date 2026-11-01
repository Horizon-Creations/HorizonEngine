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
//
// Every op below adds exactly one node per call. C++ does not order a function call's own
// arguments, so passing two node-creating calls as the two (or three) siblings of one call —
// e.g. add(mul(a,b), mul(c,d)) — lets Clang/GCC/MSVC each build either side first: same shader,
// different node ids, so the committed .hasset text differs by COMPILER, not just by platform
// (seen: Clang's order shipped, MSVC's and GCC's did not match it — Thema 329 step 7). Below,
// every call with 2+ node-creating siblings first assigns each side to a named local, so at most
// one call is ever "live" when the next begins. A `{}` initializer-list argument (f.e() takes
// one) IS ordered by the standard and needs no such care.
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
    // Straight to f.e(), not mix(k(a), k(b), t): the latter makes k(a)/k(b) two siblings of
    // ONE call again (see the Ops comment above); the {} list here is ordered for free.
    R mix(float a, float b, R t) { return f.e(T::Lerp, { k(a), k(b), t }); }
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
    const R wxSq  = o.mul(wx, wx);
    const R wzSq  = o.mul(wz, wz);
    const R lenSq = o.add(wxSq, wzSq);
    const R has    = o.f.e(T::Step, { o.k(1e-6f), lenSq });
    // Zero vector: lenSq + 1 keeps the power finite, and `has` picks the +X fallback anyway.
    const R invLen = o.pw(o.add(lenSq, o.inv(has)), -0.5f);
    const R one = o.k(1.0f);
    const R wxN = o.mul(wx, invLen);
    const R dx = o.mix(one, wxN, has);
    const R zero = o.k(0.0f);
    const R wzN  = o.mul(wz, invLen);
    const R dz = o.mix(zero, wzN, has);
    const R alongA  = o.mul(px, dx);
    const R alongB  = o.mul(pz, dz);
    const R along   = o.add(alongA, alongB);     // along the wind
    const R acrossA = o.mul(pz, dx);
    const R acrossB = o.mul(px, dz);
    const R across  = o.sub(acrossA, acrossB);    // across it
    return { dx, dz, along, across };
}

// 1 up close, 0 once a feature `cell` metres wide is a handful of pixels: noise finer than the
// screen can resolve only shimmers (a 1080p pixel is ~1 mm per metre of distance).
R fadeFar(Ops& o, R cell)
{
    const R dist = o.f.e(T::CameraDistance, {});
    const R near = o.mul(cell, 150.0f);
    const R far  = o.mul(cell, 450.0f);
    return o.inv(o.ss(near, far, dist));
}

// The geometric world normal leaned by a height field's slope (gx, gz) — n - (gx, 0, gz), the
// normal of y = h(x, z) tilted by dh/dx, dh/dz on a surface that is mostly level.
R bendNormal(Ops& o, R gx, R gz)
{
    const R ns = o.split(o.f.e(T::NormalWS, {}));
    return o.f.e(T::Normalize3, { o.f.e(T::Combine3, { o.sub(o.pin(ns, 0), gx), o.pin(ns, 1),
                                                     o.sub(o.pin(ns, 2), gz) }) });
}

// The slope of a trail's field (dv/dx, dv/dz per metre, the "Disturbance Slope" input; v = pit depth, so a
// heap is negative) as a normal lean. The surface HEIGHT is -v, and bendNormal takes the height gradient g
// (n - (gx, 0, gz)), so g = -0.3 x slope: the wall of a pit leans toward its floor, a heap's flank away from
// its crest. The lean stays small (0.3 per unit of slope) so a 0.25 m texel's steepest bilinear flank does
// not fold the normal over.
struct RimLean { R x, z; };
RimLean disturbanceRim(Ops& o, R slope)
{
    const R s = o.split(slope);
    const R x = o.mul(o.pin(s, 0), -0.3f);
    const R z = o.mul(o.pin(s, 1), -0.3f);
    return { x, z };
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
    const R dist     = f.input(f.in("Disturbance",       P::Float, 0.0f));
    const R distSlope = f.input(f.in("Disturbance Slope", P::Vec2,  0.0f));
    const R distHeap  = f.input(f.in("Disturbance Heap",  P::Float, 0.0f));

    const R ps = o.split(pos);
    const WindFrame w = windFrame(o, wind, o.pin(ps, 0), o.pin(ps, 1));
    const R eff  = o.sat(dist);                      // how deep a print lies here, 0..1
    const R heap = o.sat(distHeap);                  // how high the pushed-aside snow lies here, 0..1
    const RimLean rim = disturbanceRim(o, distSlope);

    // ── Drifts: ridges long along the wind (8 m) and narrow across it (1.8 m) ───────────────
    // FBM in the wind's own frame; its gradient comes with it, and turns back into a world
    // slope through the same two directions. More snow on the ground, more to blow around.
    const R q  = o.f.e(T::CombineRGBA, { o.mul(w.across, 0.55f), o.mul(w.along, 0.125f) });
    const R dn = o.f.e(T::FbmGrad, { q, o.k(1.0f) });
    const R driftBase = o.mul(drift, 1.0f);
    const R coverLift = o.add(o.mul(cover, 0.6f), 0.4f);
    const R driftAmp   = o.mul(driftBase, coverLift);
    const R slopeAcross = o.mul(o.mul(driftAmp, o.pin(dn, 1)), 0.55f);
    const R slopeAlong  = o.mul(o.mul(driftAmp, o.pin(dn, 2)), 0.125f);
    const R slopeAlongDx  = o.mul(slopeAlong, w.dx);
    const R slopeAcrossDz = o.mul(slopeAcross, w.dz);
    const R gxDrift = o.sub(slopeAlongDx, slopeAcrossDz);
    const R slopeAcrossDx = o.mul(slopeAcross, w.dx);
    const R slopeAlongDz  = o.mul(slopeAlong, w.dz);
    const R gzDrift = o.add(slopeAcrossDx, slopeAlongDz);

    // ── Grain: crystals a few centimetres wide ──────────────────────────────────────────────
    const R gn    = o.f.e(T::FbmGrad, { pos, o.k(16.0f) });
    const R g     = o.pin(gn, 0);
    const R fadeG = fadeFar(o, o.k(1.0f / 16.0f));
    // Powder buries the grain, meltwater and a deep cover smooth it.
    const R softDamp  = o.inv(o.mul(soft, 0.65f));
    const R meltDamp  = o.inv(o.mul(melt, 0.6f));
    const R softMelt  = o.mul(softDamp, meltDamp);
    const R coverDamp = o.inv(o.mul(cover, 0.4f));
    const R relax = o.mul(softMelt, coverDamp);
    const R grainBase   = o.mul(grain, 0.2f);
    const R relaxFade   = o.mul(relax, fadeG);
    const R grainRelief = o.mul(grainBase, relaxFade);
    const R gxSnow = o.add(gxDrift, o.mul(o.pin(gn, 1), grainRelief));
    const R gzSnow = o.add(gzDrift, o.mul(o.pin(gn, 2), grainRelief));
    const R gx = o.add(gxSnow, rim.x);
    const R gz = o.add(gzSnow, rim.z);
    const R normal = bendNormal(o, gx, gz);

    // ── Height: the structure alone — Position, Wind, Drift and Grain, nothing weather-driven ─
    const R d = o.pin(dn, 0);
    const R driftScaled     = o.mul(drift, 0.55f);
    const R dCentered       = o.sub(d, 0.5f);
    const R driftHeight     = o.mul(driftScaled, dCentered);
    const R grainHeightBase = o.mul(grain, 0.3f);
    const R gCentered       = o.sub(g, 0.5f);
    const R grainHeight     = o.mul(grainHeightBase, gCentered);
    const R height = o.sat(o.add(o.add(driftHeight, grainHeight), 0.55f));

    // ── Colour: speckle, melt darkening, glitter ────────────────────────────────────────────
    const R speckGrain = o.mul(o.mul(o.sub(g, 0.47f), 0.6f), grain);
    const R speckSoft  = o.mul(fadeG, o.inv(o.mul(soft, 0.5f)));
    const R speck = o.add(o.mul(speckGrain, speckSoft), 1.0f);
    const R glitterShape  = o.mul(o.mul(o.ss(0.62f, 0.8f, g), glitter), fadeG);
    const R meltSparkDamp = o.inv(o.mul(melt, 0.6f));
    const R softSparkDamp = o.inv(o.mul(soft, 0.4f));
    const R sparkDamp = o.mul(meltSparkDamp, softSparkDamp);
    const R spark = o.mul(glitterShape, sparkDamp);
    const R meltDark = o.mix(1.0f, 0.8f, melt);
    const R albedoBase  = o.mul(o.mul(color, speck), meltDark);
    const R albedoSpark = o.mul(spark, 0.3f);
    const R albedoLit = o.sat(o.add(albedoBase, albedoSpark));
    // A print is compacted snow: darker (and, below, glossier and shaded by its own pit). A heap is loose, fresh
    // snow: it lifts toward white.
    const R albedoPress = o.mul(albedoLit, o.inv(o.mul(eff, 0.18f)));
    const R toWhite = o.inv(albedoPress);
    const R heapLift = o.mul(heap, 0.55f);
    const R albedoLift = o.mul(toWhite, heapLift);
    const R albedo = o.add(albedoPress, albedoLift);

    // ── Roughness: crust is glossy, powder matte, meltwater and glittering crystals catch light ─
    const R base = o.mix(0.55f, 0.92f, soft);
    const R roughMelt  = o.mul(base, o.mix(1.0f, 0.4f, melt));
    const R roughSpark = o.mul(spark, 0.45f);
    const R roughLit = o.sub(roughMelt, roughSpark);
    const R roughPress = o.sub(roughLit, o.mul(eff, 0.25f));
    const R rough0 = o.add(roughPress, o.mul(heap, 0.15f));   // loose snow is matte
    const R roughness = o.add(o.mul(o.sat(rough0), 0.95f), 0.05f);

    // ── AO: the troughs of the drifts and the pits between the grains ───────────────────────
    const R driftRelief = o.mul(drift, 0.6f);
    const R dInv = o.inv(d);
    const R driftCavity = o.mul(driftRelief, dInv);
    const R grainCavityRelief = o.mul(grain, 0.12f);
    const R gInv = o.inv(g);
    const R grainCavity = o.mul(grainCavityRelief, gInv);
    const R cavity = o.add(driftCavity, grainCavity);
    const R aoLit = o.sat(o.inv(o.mul(cavity, o.inv(o.mul(soft, 0.6f)))));
    const R ao = o.sat(o.sub(aoLit, o.mul(eff, 0.35f)));

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
    const R dist       = f.input(f.in("Disturbance",       P::Float, 0.0f));
    const R distSlope  = f.input(f.in("Disturbance Slope", P::Vec2,  0.0f));
    const R distHeap   = f.input(f.in("Disturbance Heap",  P::Float, 0.0f));

    const R ps = o.split(pos);
    const WindFrame w = windFrame(o, wind, o.pin(ps, 0), o.pin(ps, 1));
    const R eff  = o.sat(dist);                      // how deep a print lies here, 0..1
    const R heap = o.sat(distHeap);                  // how high the pushed-aside sand lies here, 0..1
    const RimLean rim = disturbanceRim(o, distSlope);

    // ── Grain: the speckle of the sand itself ───────────────────────────────────────────────
    const R gn    = o.f.e(T::FbmGrad, { pos, o.div(o.k(1.0f), grainSize) });
    const R g     = o.pin(gn, 0);
    const R fadeG = fadeFar(o, grainSize);
    // Wet sand packs down: the grain relief halves.
    const R grainBase = o.mul(o.k(0.3f), fadeG);
    const R wetDamp    = o.inv(o.mul(wet, 0.5f));
    const R grainRelief = o.mul(grainBase, wetDamp);

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
    const R alongPhase = o.div(w.along, rippleSize);
    const R wobPhase    = o.mul(o.sub(wob, 0.47f), 5.0f);
    const R phase = o.mul(o.fract(o.add(alongPhase, wobPhase)), kTwoPi);
    const R phase2 = o.add(o.mul(phase, 2.0f), 1.1f);
    const R sinPhase        = o.sin(phase);
    const R sinPhase2Scaled = o.mul(o.sin(phase2), 0.3f);
    const R shape  = o.add(sinPhase, sinPhase2Scaled);                          // the crest profile
    const R sinPhaseHalf        = o.sin(o.add(phase, kHalfPi));
    const R sinPhase2HalfScaled = o.mul(o.sin(o.add(phase2, kHalfPi)), 0.6f);
    const R slope  = o.add(sinPhaseHalf, sinPhase2HalfScaled); // its derivative in phase
    const R fadeR = fadeFar(o, rippleSize);
    const R rippleFade = o.mul(ripple, fadeR);
    const R wobShape    = o.ss(0.28f, 0.5f, wob);
    const R rip   = o.mul(rippleFade, wobShape);
    const R slopeRipFull = o.mul(o.mul(rip, 0.2f), slope);   // d(height)/d(along): the crests' flank slope
    const R slopeRip = o.mul(slopeRipFull, o.inv(eff));       // a print stamps the ripples flat
    const R slopeDx = o.mul(slopeRip, w.dx);
    const R grainDx = o.mul(o.pin(gn, 1), grainRelief);
    const R gxSand = o.add(slopeDx, grainDx);
    const R gx = o.add(gxSand, rim.x);
    const R slopeDz = o.mul(slopeRip, w.dz);
    const R grainDz = o.mul(o.pin(gn, 2), grainRelief);
    const R gzSand = o.add(slopeDz, grainDz);
    const R gz = o.add(gzSand, rim.z);
    const R normal = bendNormal(o, gx, gz);

    // ── Height: ripples and grain ───────────────────────────────────────────────────────────
    const R rippleHeight = o.mul(o.mul(ripple, 0.25f), shape);
    const R grainHeight  = o.mul(o.sub(g, 0.47f), 0.3f);
    const R height = o.sat(o.add(o.add(rippleHeight, grainHeight), 0.5f));

    // ── Colour: speckle, lighter crests, wet darkening, glitter ─────────────────────────────
    const R speck = o.add(o.mul(o.mul(o.sub(g, 0.47f), 0.45f), fadeG), 1.0f);
    const R rippleShape = o.mul(o.mul(ripple, 0.07f), shape);
    const R fadeWob     = o.mul(fadeR, o.ss(0.28f, 0.5f, wob));
    const R crest = o.add(o.mul(rippleShape, fadeWob), 1.0f);
    const R wetDark = o.mix(1.0f, 0.52f, wet);
    const R sparkShape = o.mul(o.mul(o.ss(0.66f, 0.82f, g), glitter), fadeG);
    const R wetInv      = o.inv(wet);
    const R spark = o.mul(sparkShape, wetInv);
    const R albedoBase  = o.mul(o.mul(o.mul(color, speck), crest), wetDark);
    const R albedoSpark = o.mul(spark, 0.22f);
    const R albedoLit = o.sat(o.add(albedoBase, albedoSpark));
    // A print is compacted, slightly damp sand: darker. A heap is loose dry sand from below the surface crust: lighter.
    const R albedoPress = o.mul(albedoLit, o.inv(o.mul(eff, 0.25f)));
    const R heapGain = o.add(o.mul(heap, 0.3f), 1.0f);
    const R albedo = o.sat(o.mul(albedoPress, heapGain));

    // ── Roughness: dry sand is rough, wet sand sleek, a glint is a mirror ───────────────────
    const R dryRough = o.k(0.92f);
    const R wetMix    = o.mix(1.0f, 0.45f, wet);
    const R roughWet   = o.mul(dryRough, wetMix);
    const R roughSpark = o.mul(spark, 0.5f);
    const R roughLit = o.sub(roughWet, roughSpark);
    const R roughPress = o.sub(roughLit, o.mul(eff, 0.15f));
    const R rough0 = o.add(roughPress, o.mul(heap, 0.1f));
    const R roughness = o.add(o.mul(o.sat(rough0), 0.95f), 0.05f);

    // ── AO: the troughs between the ripples, the pits between the grains ────────────────────
    const R ripScaled  = o.mul(rip, 0.12f);
    const R shapeInv   = o.inv(shape);
    const R aoRip   = o.mul(ripScaled, shapeInv);
    const R grainAoBase = o.mul(o.k(0.12f), fadeG);
    const R halfMinusG  = o.sat(o.sub(o.k(0.5f), g));
    const R aoGrain = o.mul(grainAoBase, halfMinusG);
    const R aoLit = o.sat(o.inv(o.add(aoRip, aoGrain)));
    const R ao = o.sat(o.sub(aoLit, o.mul(eff, 0.35f)));

    surfaceOutputs(f, albedo, normal, roughness, ao, height);
    return f.g;
}
} // namespace HE
