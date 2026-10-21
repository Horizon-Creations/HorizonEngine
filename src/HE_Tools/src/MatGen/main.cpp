// mat_gen — generates the engine's built-in MATERIALS as loose .hasset assets,
// written via ContentManager::saveAsset so the byte layout is identical to
// editor-authored materials.
//
// Usage:  mat_gen <output-dir>
//   <output-dir> is the folder the .hasset files are written into (e.g.
//   EditorDeps/EngineContent/Materials). It is used verbatim as the
//   ContentManager content root, and each material is saved under "<Name>.hasset".
//
// The bargain mesh_gen, widget_gen and matfn_gen make: output is deterministic
// (every material gets a stable, well-known UUID), the results are COMMITTED,
// and this is not part of the normal build graph. A material lives in
// EngineContent and not as a mem:// default because only a FILE shows up in the
// Content Browser and the Inspector's material picker, and only a file ships
// with an exported game (docs/water-shader-plan.md §1, §4).
//
// ── The file carries the generated shader, not only the graph ────────────────
// The graph is the source of truth, and the editor regenerates the shader from
// it on every load. The PACKER does not: HpakWriter ships the baked fragment,
// parameter block and approximations that are in the file. A file holding only
// the graph would therefore ship as the plain PBR uber-shader. So every material
// is saved, loaded back through a fresh ContentManager — which runs
// regenerateMaterialFromGraph, the editor's own load path — and saved again.
//
// ── Failing, not warning ─────────────────────────────────────────────────────
// matfn_gen warns and leaves the failure to the test. Here the generator fails
// itself on the mistake the codegen hides best: a Param node past the 16-slot
// budget is baked as a literal (MaterialGraph.cpp, paramSlot), and a Param node
// nothing reads is never emitted at all. Either way a knob vanishes from the
// Inspector without a word, so the exposed set is compared to the list the
// material promises, name for name.
//
// A note on types, the same one matfn_gen makes: the maths nodes are Vec3 in and
// Vec3 out, so a scalar chain runs as a splatted vec3 and a Float pin takes .x.

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/DefaultAssets.h>
#include <MaterialGraph/MaterialGraph.h>
#include <Physics/WaterWaves.h>
#include <Types/UUID.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
using namespace HE;

// Well-known UUID base for the built-in materials. `hi` stays far below the
// version-4 bit pattern UUID::generate() enforces and clear of the DefaultAssets
// sentinels, mesh_gen's 0x100, widget_gen's 0x200 and matfn_gen's 0x300 block.
// Not 0x400: landscape_tex_gen owns 0x400..0x41F, and one UUID names ONE asset in
// the ContentManager (Water used to be {0x400, 1} next to T_Landscape_Grass_Albedo).
constexpr uint64_t kMatBaseHi = 0x0000000000000600ULL;
static_assert(kEngineWaterMaterialId.hi == kMatBaseHi + 0 && kEngineWaterMaterialId.lo == 1,
              "Water is entry 0 of the 0x600 block — DefaultAssets.h must agree");

constexpr float kColStep = 230.0f;
constexpr float kRowStep = 110.0f;

constexpr float kPi       = 3.14159265f;
constexpr float kTau      = 6.28318531f;
constexpr float kHalfPi   = 1.57079633f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kE        = 2.71828183f;

using T = MatNodeType;

// An output pin: node id + which of its outputs. Every node but Split RGBA has
// one, so a bare node id converts.
struct Pin
{
    int n = 0, p = 0;
    Pin() = default;
    Pin(int node, int pin = 0) : n(node), p(pin) {}
};

// ── The builder ──────────────────────────────────────────────────────────────
// MaterialGraph with shorter names and a canvas position per node — not a
// second graph model. A failed link is remembered rather than ignored: connect()
// refuses an out-of-range pin, and a graph that silently lost a wire still
// generates a shader, just the wrong one.
struct Mb
{
    MaterialGraph g;
    bool ok = true;

    int node(T type, float col, float row)
    {
        return g.addNode(type, col * kColStep, row * kRowStep);
    }

    void link(Pin src, int dst, int dstPin)
    {
        if (!g.connect(src.n, src.p, dst, dstPin))
        {
            std::fprintf(stderr, "  link %d.%d -> %d.%d refused\n", src.n, src.p, dst, dstPin);
            ok = false;
        }
    }

    // A baked literal: a constant of the MATERIAL (a tau, an epsilon, a shape
    // factor), never a knob — a knob is a Param node.
    int k(float v, float col, float row)
    {
        const int id = node(T::ConstFloat, col, row);
        g.findNode(id)->p[0] = v;
        return id;
    }

    int un(T type, Pin a, float col, float row)
    {
        const int id = node(type, col, row);
        link(a, id, 0);
        return id;
    }

    int op(T type, Pin a, Pin b, float col, float row)
    {
        const int id = node(type, col, row);
        link(a, id, 0);
        link(b, id, 1);
        return id;
    }

    int op3(T type, Pin a, Pin b, Pin c, float col, float row)
    {
        const int id = node(type, col, row);
        link(a, id, 0);
        link(b, id, 1);
        link(c, id, 2);
        return id;
    }

    // Fractal noise on the integer hash (heFbmI): the float hash of the plain Fbm
    // node loses its low bits at large world coordinates and rounds differently
    // per GPU, which a water surface made of nothing BUT noise slopes would show.
    // The integer hash gives the same field on all five backends.
    int fbm(Pin uv, Pin scale, float col, float row)
    {
        const int id = op(T::Fbm, uv, scale, col, row);
        g.findNode(id)->p[0] = 1.0f;
        return id;
    }

    // Balanced sum: a long left-leaning Add chain would make the codegen's
    // recursion (emitNode, one frame per level) as deep as the term count.
    Pin sum(std::vector<Pin> v, float col, float row)
    {
        while (v.size() > 1)
        {
            std::vector<Pin> next;
            for (size_t i = 0; i + 1 < v.size(); i += 2)
                next.push_back(Pin{ op(T::Add, v[i], v[i + 1], col, row + 0.5f * static_cast<float>(i)) });
            if (v.size() & 1) next.push_back(v.back());
            v = next;
            col += 1.0f;
        }
        return v.front();
    }

    // An exposed parameter. Group and tooltip travel with the slot into the
    // material (graphParamGroups / graphParamTooltips) — the Material Editor's
    // panel shows them today, so the meaning of a packed Vec4's components is
    // written down where a user can find it. A Float gets a slider range.
    int param(T type, const char* name, std::initializer_list<float> v,
              const char* group, const char* tip, float row,
              float sliderMin = 0.0f, float sliderMax = 0.0f)
    {
        const int id = node(type, 0, row);
        MatGraphNode& n = *g.findNode(id);
        n.s       = name;
        n.group   = group;
        n.tooltip = tip;
        int i = 0;
        for (float f : v) n.p[i++] = f;
        if (type == T::ParamFloat) { n.p[1] = sliderMin; n.p[2] = sliderMax; }
        return id;
    }

    void comment(const char* text, float col, float row, float cols, float rows)
    {
        MatGraphComment c;
        c.id   = g.nextId++;
        c.text = text;
        c.x = col * kColStep - 20.0f;  c.y = row * kRowStep - 40.0f;
        c.w = cols * kColStep;         c.h = rows * kRowStep;
        g.comments.push_back(c);
    }
};

// How many nodes the longest wire chain passes. The codegen recurses once per
// level (emitNode → inputExpr → emitNode), and the stack frame is big (0x9E08 in
// a Debug MSVC build, docs/water-shader-plan.md and CMakeLists.txt "MSVC
// main-thread stack"), so a graph that grows level by level is worth a number
// in the generator's output.
int longestChain(const MaterialGraph& g)
{
    std::map<int, int> memo;
    std::vector<int> stack;
    // Iterative post-order: a recursive version would hit the very limit it measures.
    int best = 0;
    for (const MatGraphNode& root : g.nodes)
    {
        stack.push_back(root.id);
        while (!stack.empty())
        {
            const int id = stack.back();
            if (memo.count(id)) { stack.pop_back(); continue; }
            int deepest = 0;
            bool ready = true;
            for (const MatGraphLink& l : g.links)
            {
                if (l.dstNode != id) continue;
                auto it = memo.find(l.srcNode);
                if (it == memo.end()) { stack.push_back(l.srcNode); ready = false; }
                else deepest = std::max(deepest, it->second);
            }
            if (!ready) continue;
            memo[id] = deepest + 1;
            stack.pop_back();
        }
        best = std::max(best, memo[root.id]);
    }
    return best;
}

// ── One wave train ───────────────────────────────────────────────────────────
// A directional sine, h = (steep / k) · sin(k · (d·p − speed · t) + warp), whose
// slope is analytic: ∂h/∂p = steep · cos(phase) · d. That is why the Wave
// parameter's w is a STEEPNESS (height over wavelength) and not a height: the
// normal only ever sees the slope, so a long swell and a short ripple of the
// same steepness tilt the surface equally.
//
// A knob drives TWO trains: itself, and a companion that the generator rotates,
// shortens and weakens by fixed ratios. Three sines of one direction each make a
// lattice of equal patches however their phases are warped; a wind sea is a
// SPREAD of directions and lengths, and the companions are the cheapest spread
// that is still driven by the three knobs a user sees. A short wave is slower
// than a long one (deep-water dispersion, c ∝ √λ), so a companion's speed
// follows its length.
//
// The primary's phase is warped by FBM over the wave's own frame, scaled to its
// wavelength, moving with the wave. It bends and breaks the crests. Its own
// slope is left out of the normal on purpose: it varies over a wavelength, the
// sine within one. A companion does not run an FBM of its own (it is the weaker
// half, and the FBM is the dear part): it takes its primary's warp field, which
// bends ITS crests too, in a different direction across the same noise. Without
// that its crests are ruler-straight and cross the primary's as a diamond grid.
//
// Everything is in WORLD units off World Position (x, z), never the mesh UV:
// the engine plane's UVs run 0..1 over the whole scaled plane, so UV waves
// would stretch with it.
//
// ── Bombed (docs/water-shader-plan.md §12) ───────────────────────────────────
// Six sines repeat. However their phases are warped, their crests keep one
// spacing and their sum is a lattice: on a plane a few hundred metres across the
// same patch of glow and the same foam dot come back row after row (§12.1). That
// is tiling, and the cure is the one textures get: texture bombing. The plane is
// cut into hexes (the Bombing Cells node, the grid of the engine's bombing nodes),
// and every train is evaluated once per hex of the pixel — three times — with that
// hex's own direction and phase, the three results blended by the hex weights.
// Inside a hex the waves are as regular as before; no two hexes agree, so the
// lattice never gets a second period to repeat on. The warp field is NOT
// bombed: it is noise, it does not tile, and it is the dear part.
//
// A sum of waves of different phase is an interference pattern: at a seam the
// amplitude is lower or higher than in a hex, depending on the two random phases.
// The gain below (1/√Σw², from the hex-tiling paper) restores the MEAN power of
// that, so a seam is not systematically calmer than a hex, only different.
struct Wave { Pin slopeX, slopeZ, height, steep, dirX, dirZ, speed, warp; };

struct Train
{
    float turnDeg;     // companion: added to the knob's direction
    float lengthScale; // companion: × the knob's wavelength
    float steepScale;  // companion: × the knob's steepness
    bool  warp;
};
constexpr Train kPrimary{ 0.0f, 1.0f, 1.0f, true };

// What one Wave knob's hex grid hands its trains: per hex a weight, and a
// direction offset (degrees) and phase offset (radians) for the primary train and
// for the companion — all four in the Variation knob's ranges — plus the gain.
struct Hex
{
    Pin weight[3];
    Pin dirOff[3], dirOffC[3];
    Pin phOff[3],  phOffC[3];
    Pin gain;
};

// max(a, b) out of the library: lerp(a, b, step(a, b)).
Pin maxOf(Mb& m, Pin a, Pin b, float col, float row)
{
    return Pin{ m.op3(T::Lerp, a, b, m.op(T::Step, a, b, col, row), col + 1, row) };
}

// The hex grid of one Wave knob: cells `Variation.x` wavelengths wide (never under
// half a wavelength: a cell smaller than the wave only makes noise), the seam
// sharpness `Variation.w`, offsets in `Variation.y` (phase, 0..1 of a turn) and
// `Variation.z` (direction, ± degrees). `seed` keeps the three knobs' hexes from
// choosing the same numbers. y = z = 0 makes the three hexes identical: the
// unbombed water, the way to switch it off.
Hex hexCells(Mb& m, int waveParam, int variation, Pin px, Pin pz, float seed, float col, float row)
{
    const int vs = m.un(T::SplitRGBA, variation, col, row);
    const Pin cellWaves{ vs, 0 }, phaseSpread{ vs, 1 }, dirSpread{ vs, 2 }, blend{ vs, 3 };
    const int ws  = m.un(T::SplitRGBA, waveParam, col, row + 1);
    const Pin cell = m.op(T::Multiply, maxOf(m, cellWaves, m.k(0.5f, col, row + 2), col + 1, row + 2),
                          Pin{ ws, 2 }, col + 3, row + 1);
    const int hex = m.node(T::BombCells, col + 4, row);
    m.g.findNode(hex)->p[0] = seed;
    m.link(m.op(T::CombineRGBA, px, pz, col + 3, row), hex, 0);
    m.link(cell, hex, 1);
    m.link(blend, hex, 2);
    const int wS = m.un(T::SplitRGBA, Pin{ hex, 0 }, col + 5, row);
    const int aS = m.un(T::SplitRGBA, Pin{ hex, 1 }, col + 5, row + 1);
    const int bS = m.un(T::SplitRGBA, Pin{ hex, 2 }, col + 5, row + 2);
    const int cS = m.un(T::SplitRGBA, Pin{ hex, 3 }, col + 5, row + 3);

    const Pin dirRange   = m.op(T::Multiply, dirSpread, m.k(2.0f, col + 5, row + 4), col + 6, row + 4);
    const Pin phaseRange = m.op(T::Multiply, phaseSpread, m.k(kTau, col + 5, row + 5), col + 6, row + 5);
    const Pin half = m.k(0.5f, col + 6, row + 6);
    Hex h;
    for (int i = 0; i < 3; ++i)
    {
        const float r = row + 7.0f * static_cast<float>(i);
        h.weight[i]  = Pin{ wS, i };
        h.dirOff[i]  = m.op(T::Multiply, m.op(T::Subtract, Pin{ aS, i }, half, col + 7, r), dirRange, col + 8, r);
        h.dirOffC[i] = m.op(T::Multiply, m.op(T::Subtract, Pin{ cS, i }, half, col + 7, r + 1), dirRange, col + 8, r + 1);
        h.phOff[i]   = m.op(T::Multiply, Pin{ bS, i }, phaseRange, col + 8, r + 2);
        h.phOffC[i]  = m.op(T::Multiply, m.un(T::Fract, m.op(T::Add, Pin{ bS, i }, Pin{ cS, i }, col + 7, r + 3), col + 8, r + 3),
                            phaseRange, col + 9, r + 3);
    }
    // Gain: restores the mean power the blend loses where the weights are mixed,
    // by as much as the hexes differ (phase spread, plus direction spread as a
    // fraction of 45°); not at all when they do not.
    const Pin w2 = m.sum({ Pin{ m.op(T::Multiply, h.weight[0], h.weight[0], col + 6, row + 28) },
                           Pin{ m.op(T::Multiply, h.weight[1], h.weight[1], col + 6, row + 29) },
                           Pin{ m.op(T::Multiply, h.weight[2], h.weight[2], col + 6, row + 30) } }, col + 7, row + 28);
    const Pin full = m.op(T::Divide, m.k(1.0f, col + 8, row + 28),
                          m.op(T::Power, w2, m.k(0.5f, col + 8, row + 29), col + 9, row + 28), col + 10, row + 28);
    const Pin differ = m.un(T::Saturate, m.op(T::Add, phaseSpread,
                                               m.op(T::Divide, dirSpread, m.k(45.0f, col + 8, row + 30), col + 9, row + 30),
                                               col + 10, row + 30), col + 11, row + 30);
    h.gain = Pin{ m.op3(T::Lerp, m.k(1.0f, col + 11, row + 28), full, differ, col + 12, row + 28) };
    return h;
}

Wave waveTrain(Mb& m, int param, Pin px, Pin pz, Pin time, const Train& tr, const Pin* fade,
               const Wave* primary, const Hex& hx, float row)
{
    const int s = m.un(T::SplitRGBA, param, 1, row);
    Pin dirDeg{ s, 0 }, speed{ s, 1 }, length{ s, 2 }, steep{ s, 3 };
    const Pin knobSpeed = speed;
    if (tr.turnDeg != 0.0f)     dirDeg = m.op(T::Add, dirDeg, m.k(tr.turnDeg, 1, row + 1), 2, row + 1);
    if (tr.lengthScale != 1.0f)
    {
        length = m.op(T::Multiply, length, m.k(tr.lengthScale, 1, row + 2), 2, row + 2);
        speed  = m.op(T::Multiply, speed, m.k(std::sqrt(tr.lengthScale), 1, row + 3), 2, row + 3);
    }
    if (tr.steepScale != 1.0f)  steep = m.op(T::Multiply, steep, m.k(tr.steepScale, 1, row + 3.5f), 2, row + 3.5f);

    // Direction in degrees → (cos, sin) on the XZ plane. The library has no
    // Cosine node; Sine(x + π/2) is one.
    struct Dir { int sin, cos; };
    auto direction = [&](Pin deg, float r) {
        const int rad = m.op(T::Multiply, deg, m.k(kDegToRad, 1, r + 1), 2, r);
        return Dir{ m.un(T::Sine, rad, 3, r),
                    m.un(T::Sine, m.op(T::Add, rad, m.k(kHalfPi, 2, r + 1), 3, r + 1), 4, r + 1) };
    };
    // The wave's frame for a direction: `along` runs with the wave, `across` along
    // its crest. (Both off the raw knob direction for the warp field below; per hex
    // off that hex's own.)
    auto alongOf = [&](const Dir& d, float r) {
        return m.op(T::Add, m.op(T::Multiply, d.cos, px, 5, r),
                            m.op(T::Multiply, d.sin, pz, 5, r + 0.6f), 6, r);
    };
    const int drift = m.op(T::Multiply, speed, time, 6, row + 2.4f);

    // The primary's frame, unbombed: the warp field and the direction the chop
    // drifts along are both read off it.
    Dir d0{ -1, -1 };
    int warp = -1;
    if (tr.warp)
    {
        d0 = direction(dirDeg, row);
        const int travel0 = m.op(T::Subtract, alongOf(d0, row), drift, 7, row);
        const int across  = m.op(T::Subtract, m.op(T::Multiply, d0.cos, pz, 5, row + 1.2f),
                                              m.op(T::Multiply, d0.sin, px, 5, row + 1.8f), 6, row + 1.2f);
        const int warpUV = m.op(T::CombineRGBA, travel0, across, 8, row + 1);
        const int fbm    = m.fbm(warpUV, m.op(T::Divide, m.k(1.0f, 7, row + 2), length, 8, row + 2), 9, row + 1);
        warp = m.op(T::Multiply, fbm, m.k(2.5f, 9, row + 2), 10, row + 1);
    }
    else if (primary)
        warp = m.op(T::Multiply, primary->warp, m.k(0.9f, 9, row + 2), 10, row + 1);

    // Once per hex of the pixel: this hex's direction, its own phase, the same
    // warp. A companion draws its own pair of numbers (dirOffC / phOffC).
    const int kWave = m.op(T::Divide, m.k(kTau, 7, row + 3), length, 8, row + 3);
    const Pin* dirOff = tr.warp ? hx.dirOff : hx.dirOffC;
    const Pin* phOff  = tr.warp ? hx.phOff  : hx.phOffC;
    Pin tiltX[3], tiltZ[3], sines[3];   // cos(phase)·cos(dir), cos(phase)·sin(dir), sin(phase)
    for (int i = 0; i < 3; ++i)
    {
        const float r = row + 0.3f * static_cast<float>(i);
        const Dir di = direction(m.op(T::Add, dirDeg, dirOff[i], 2, r + 4), r + 4);
        const int travel = m.op(T::Subtract, alongOf(di, r + 4), drift, 7, r + 4);
        int phase = m.op(T::Multiply, kWave, travel, 9, r + 4);
        if (warp >= 0) phase = m.op(T::Add, phase, warp, 11, r + 4);
        phase = m.op(T::Add, phase, phOff[i], 11, r + 4.5f);
        const int sinPh = m.un(T::Sine, phase, 12, r + 4);
        const int cosPh = m.un(T::Sine, m.op(T::Add, phase, m.k(kHalfPi, 11, r + 5), 12, r + 5), 13, r + 5);
        tiltX[i] = m.op(T::Multiply, cosPh, di.cos, 14, r + 4);
        tiltZ[i] = m.op(T::Multiply, cosPh, di.sin, 14, r + 5);
        sines[i] = Pin{ sinPh };
    }
    // Blend by the hex weights, then steepness, the distance fade and the gain.
    auto blend = [&](const Pin* v, float r) {
        return Pin{ m.op(T::Add, m.op(T::Add, m.op(T::Multiply, hx.weight[0], v[0], 15, r),
                                              m.op(T::Multiply, hx.weight[1], v[1], 15, r + 0.4f), 16, r),
                         m.op(T::Multiply, hx.weight[2], v[2], 15, r + 0.8f), 17, r) };
    };
    Pin scale = m.op(T::Multiply, steep, hx.gain, 18, row);
    if (fade) scale = m.op(T::Multiply, scale, *fade, 18, row + 1);
    Wave w;
    w.slopeX = m.op(T::Multiply, blend(tiltX, row), scale, 19, row);
    w.slopeZ = m.op(T::Multiply, blend(tiltZ, row + 1), scale, 19, row + 1);
    // Height, the crest finder for the foam: steepness × the blended sine, without
    // the distance fade (a crest is a crest however far).
    w.height = m.op(T::Multiply, blend(sines, row + 2), m.op(T::Multiply, steep, hx.gain, 18, row + 2), 19, row + 2);
    w.steep  = steep;
    w.dirX   = tr.warp ? Pin{ d0.cos } : Pin{};
    w.dirZ   = tr.warp ? Pin{ d0.sin } : Pin{};
    w.speed  = knobSpeed;
    w.warp   = warp >= 0 ? Pin{ warp } : Pin{};
    return w;
}

// Chop strengths relative to `drive` (the summed steepness), see the chop layers in waterMaterial.
constexpr float kChopBase = 0.3f, kChopMid = 0.9f, kChopFine = 0.8f, kChopFiner = 0.55f;

// Shallow water (Thema 206 step 4), defaults of the ShallowWater parameter (step 6).
// The tint depth follows the real shore depth for e above 0 (water shallower than one metre)
// and has fully taken over at e = kShallowTintE (half a metre); the Opacity floor falls to
// kShallowOpacity of itself at no depth and is whole again from kShallowClearDepth metres.
constexpr float kShallowTintE = 0.5f, kShallowOpacity = 0.25f, kShallowClearDepth = 0.8f;

// ── Fine ripples ─────────────────────────────────────────────────────────────
// The slope of a drifting FBM height field, by finite differences — the plan's
// "Normal from Height by hand" (§6), three noise reads per layer. Sines alone,
// however many, stay a smooth interference pattern; what makes a water surface
// read as water at a few metres is the short, irregular chop riding on them, and
// it is what breaks a reflection into the broken, glittering stripes of the real
// thing instead of one clean band per crest.
//
// `size` is metres per noise cell, `amp` the slope per unit of noise gradient
// (a Pin: it follows the Waves knobs, so flat waves mean flat water), and the
// field drifts with the wave it is paired with.
struct Grad { Pin x, z; };

Grad rippleLayer(Mb& m, Pin px, Pin pz, Pin time, float size, Pin driftX, Pin driftZ,
                 Pin amp, float col, float row)
{
    constexpr float kStep = 0.04f;                            // finite-difference step, in noise cells
    const int ox = m.op(T::Add, px, m.op(T::Multiply, driftX, time, col, row), col + 1, row);
    const int oz = m.op(T::Add, pz, m.op(T::Multiply, driftZ, time, col, row + 1), col + 1, row + 1);
    const int exx = m.op(T::Add, ox, m.k(size * kStep, col + 1, row + 2), col + 2, row + 2);
    const int ezz = m.op(T::Add, oz, m.k(size * kStep, col + 1, row + 3), col + 2, row + 3);
    const int uv0 = m.op(T::CombineRGBA, ox, oz, col + 2, row);
    const int uvX = m.op(T::CombineRGBA, exx, oz, col + 3, row + 2);
    const int uvZ = m.op(T::CombineRGBA, ox, ezz, col + 3, row + 3);
    const int inv = m.k(1.0f / size, col + 2, row + 1);
    const int h0  = m.fbm(uv0, inv, col + 4, row);
    const int hX  = m.fbm(uvX, inv, col + 4, row + 2);
    const int hZ  = m.fbm(uvZ, inv, col + 4, row + 3);
    const int gain = m.op(T::Multiply, amp, m.k(1.0f / kStep, col + 4, row + 1), col + 5, row + 1);
    Grad g;
    g.x = m.op(T::Multiply, m.op(T::Subtract, hX, h0, col + 5, row + 2), gain, col + 6, row + 2);
    g.z = m.op(T::Multiply, m.op(T::Subtract, hZ, h0, col + 5, row + 3), gain, col + 6, row + 3);
    return g;
}

// ── The underside (Thema 205, Schritt 3) ─────────────────────────────────────
// Seen from below, a water surface is not the top with its normal turned over.
// Light crosses it from the dense side, so Snell's law closes everything past
// ~48.6° from the vertical: inside that cone (Snell's window) the sky shows
// through, outside it the surface is a perfect mirror of the water below (total
// internal reflection). The waves tilt the normal under every pixel, so the
// window's rim crinkles and the sun, seen through it, breaks into moving sparks.
//
// What the renderer gives this branch: the scene BEHIND the surface is already in
// the target (the sky, the bank), so "see-through" is low opacity, not a colour
// of its own; the underwater fog post pass then covers the surface by its
// distance from the camera (it cuts the view ray at the surface plane), which is
// what makes the near surface clear and the far one blue. heLitP cannot light it
// — from below the sun is behind the normal — so the underside switches heLitP
// off (black base, Specular 0, Roughness 1: its sky Fresnel term goes to zero)
// and carries everything in Emissive, with the Sun node for the light coming
// through. The material still reads no scene colour, so what lies above is not
// bent: the refraction shows as the crinkled rim, the sparks and the net.
//
// The blend is SrcAlpha / OneMinusSrcAlpha. Light added where the surface is
// clear would arrive times that small alpha, so Emissive is divided by it (alpha
// has a floor of 0.04: the half-float target holds the quotient easily). No
// knob: every HeParams slot is taken. The net reuses the top side's caustic
// fields (Caustics x = strength, w = fade distance, so 0 turns both off).
struct Under
{
    int below = 0;     // 1 where the camera is under the surface's plane side, else 0
    Pin alpha;         // opacity
    Pin emissive;      // the whole underside colour, already divided by alpha
};

Under undersideTerms(Mb& m, Pin geoN, Pin view, Pin waveN, Pin pan1, Pin pan2, Pin invC, Pin nearFade, Pin causStrength,
                     Pin shallow, Pin deep, float col, float row)
{
    constexpr float kEta = 1.333f;     // water → air
    const float c0 = col, r0 = row;

    // Which side: the MESH normal against the view, not the wave normal (a steep
    // wave must not flip a pixel to the other look).
    const int below = m.op(T::Less, m.op(T::DotProduct, geoN, view, c0, r0), m.k(0.0f, c0, r0 + 0.5f), c0 + 1, r0);

    // The view ray going up, and its cosine against the wave normal on the water side.
    const int up   = m.op(T::Multiply, view, m.k(-1.0f, c0, r0 + 1.5f), c0 + 1, r0 + 1);
    const int cosI = m.un(T::Saturate, m.op(T::DotProduct, waveN, up, c0 + 2, r0 + 1), c0 + 3, r0 + 1);
    // cos of the angle in AIR: k = 1 - eta^2 (1 - cos_i^2), negative = total
    // internal reflection. Clamped before the root so the root never sees a
    // negative number; the epsilon keeps every backend's pow away from pow(0, y).
    const int sin2 = m.un(T::OneMinus, m.op(T::Multiply, cosI, cosI, c0 + 4, r0 + 1), c0 + 5, r0 + 1);
    const int kSn  = m.op(T::Subtract, m.k(1.0f, c0 + 5, r0 + 2),
                          m.op(T::Multiply, sin2, m.k(kEta * kEta, c0 + 5, r0 + 2.5f), c0 + 6, r0 + 2), c0 + 7, r0 + 1);
    const int cosT = m.op(T::Power, m.op(T::Add, m.un(T::Saturate, kSn, c0 + 8, r0 + 1), m.k(1e-6f, c0 + 8, r0 + 2), c0 + 9, r0 + 1),
                          m.k(0.5f, c0 + 9, r0 + 2), c0 + 10, r0 + 1);
    // Schlick on the air-side cosine: 0.02 looking straight up, 1 at the rim and
    // past it, continuous across (cosT runs to 0 at the rim), so the rim is as
    // sharp as the waves make it and no sharper.
    const int fr   = m.op(T::Add, m.k(0.02f, c0 + 11, r0 + 2),
                          m.op(T::Multiply, m.k(0.98f, c0 + 11, r0 + 3),
                               m.op(T::Power, m.un(T::OneMinus, cosT, c0 + 11, r0 + 1), m.k(5.0f, c0 + 11, r0 + 1.5f), c0 + 12, r0 + 1),
                               c0 + 13, r0 + 2), c0 + 14, r0 + 1);
    const int trans = m.un(T::OneMinus, fr, c0 + 15, r0 + 1);

    // The ray on the air side (GLSL refract with N facing the incoming ray):
    // t = (cos_t - eta cos_i) N + eta * up. Only read where trans > 0, where it is unit.
    const int tN = m.op(T::Multiply, waveN,
                        m.op(T::Subtract, cosT, m.op(T::Multiply, cosI, m.k(kEta, c0 + 2, r0 + 3.5f), c0 + 3, r0 + 3), c0 + 4, r0 + 3),
                        c0 + 5, r0 + 3);
    const int tDir = m.op(T::Add, tN, m.op(T::Multiply, up, m.k(kEta, c0 + 2, r0 + 4.5f), c0 + 3, r0 + 4), c0 + 6, r0 + 3);
    const int sun  = m.node(T::Sun, c0, r0 + 6);
    const int toSun = m.un(T::Saturate, m.op(T::DotProduct, tDir, Pin{ sun, 0 }, c0 + 7, r0 + 4), c0 + 8, r0 + 4);
    // The sun through the surface: tight sparks where a facet bends the ray onto the
    // disc, a broad glow around it (the bright patch of the window toward the sun).
    const int spark = m.op(T::Multiply, m.op(T::Power, toSun, m.k(250.0f, c0 + 8, r0 + 5), c0 + 9, r0 + 4),
                           m.k(6.0f, c0 + 9, r0 + 5), c0 + 10, r0 + 4);
    const int glow  = m.op(T::Multiply, m.op(T::Power, toSun, m.k(6.0f, c0 + 8, r0 + 6), c0 + 9, r0 + 6),
                           m.k(0.35f, c0 + 9, r0 + 7), c0 + 10, r0 + 6);
    // The net: the top side's two drifting fields, read 2.5x finer (the top's 2 m
    // cells gave a few long lines, not a net) and cut thinner. Brighter toward the
    // sun, a little everywhere the window lets light in.
    const int fine  = m.op(T::Multiply, invC, m.k(2.5f, c0 + 6, r0 + 9.5f), c0 + 7, r0 + 9);
    const int uf1   = m.fbm(pan1, fine, c0 + 8, r0 + 9.5f);
    const int uf2   = m.fbm(pan2, m.op(T::Multiply, fine, m.k(1.37f, c0 + 7, r0 + 10.5f), c0 + 8, r0 + 10.5f), c0 + 9, r0 + 10);
    const int gap   = m.un(T::Absolute, m.op(T::Subtract, uf1, uf2, c0 + 10, r0 + 9.5f), c0 + 11, r0 + 9.5f);
    const int netL  = m.op(T::Power, m.un(T::Saturate, m.un(T::OneMinus, m.op(T::Multiply, gap, m.k(14.0f, c0 + 8, r0 + 8.5f), c0 + 9, r0 + 8),
                                                            c0 + 10, r0 + 8), c0 + 11, r0 + 8),
                          m.k(3.0f, c0 + 11, r0 + 8.5f), c0 + 12, r0 + 8);
    const int net   = m.op(T::Multiply,
                           m.op(T::Multiply, m.op(T::Multiply, netL, nearFade, c0 + 13, r0 + 8),
                                m.op(T::Multiply, causStrength, m.k(3.0f, c0 + 13, r0 + 7.5f), c0 + 14, r0 + 7.5f), c0 + 14, r0 + 8),
                           m.op(T::Add, m.k(0.6f, c0 + 13, r0 + 9), m.op(T::Multiply, glow, m.k(3.0f, c0 + 12, r0 + 9.5f), c0 + 13, r0 + 9.5f), c0 + 14, r0 + 9),
                           c0 + 15, r0 + 8);
    const int light = m.op(T::Multiply, m.op(T::Multiply, Pin{ sun, 1 }, trans, c0 + 15, r0 + 5),
                           m.sum({ spark, glow, net }, c0 + 14, r0 + 5), c0 + 16, r0 + 5);

    // Past the rim the surface mirrors the water below. Just past it the mirrored
    // ray points steeply down, at the lit floor, so it is brighter; toward grazing
    // it runs level into the water body. The wave normal moves that line, which is
    // what gives the mirror its texture. The floor's net shows in it, dimmer.
    const int mirror0 = m.op3(T::Lerp, shallow, deep, m.k(0.3f, c0 + 13, r0 + 11), c0 + 14, r0 + 11);
    const int steep   = m.op3(T::Smoothstep, m.k(0.1f, c0 + 12, r0 + 12), m.k(0.66f, c0 + 12, r0 + 12.5f), cosI, c0 + 13, r0 + 12);
    const int mirrorB = m.op3(T::Lerp, m.k(0.7f, c0 + 13, r0 + 13), m.k(1.8f, c0 + 13, r0 + 13.5f), steep, c0 + 14, r0 + 12);
    const int mirNet  = m.op(T::Multiply, m.op(T::Multiply, Pin{ sun, 1 }, net, c0 + 14, r0 + 14), m.k(0.5f, c0 + 14, r0 + 14.5f), c0 + 15, r0 + 14);
    const int mirror  = m.op(T::Add, m.op(T::Multiply, mirror0, mirrorB, c0 + 15, r0 + 12), mirNet, c0 + 16, r0 + 12);
    const int alpha  = m.op(T::Add, m.op(T::Multiply, fr, m.k(0.96f, c0 + 15, r0 + 2.5f), c0 + 16, r0 + 2), m.k(0.04f, c0 + 16, r0 + 3), c0 + 17, r0 + 2);
    const int emis   = m.op(T::Divide, m.op(T::Add, m.op(T::Multiply, mirror, fr, c0 + 16, r0 + 11), light, c0 + 17, r0 + 6),
                            alpha, c0 + 18, r0 + 4);
    m.comment("Underside: Snell's window, total internal reflection, the sun through the surface", c0, r0, 19, 12);
    return { below, Pin{ alpha }, Pin{ emis } };
}

// ── Water ────────────────────────────────────────────────────────────────────
// Lit, Translucent, Surface. The renderer's lighting (heLitP) does the sky
// reflection, the sun glint and the screen-space reflection from the wave
// normal and the roughness; the graph supplies the water around it.
//
// What it cannot have yet: the material pass reads neither the depth nor the
// colour of the scene behind the surface (docs/water-shader-plan.md §2, §6). Until
// it does, every term that would need them falls back to something that still
// earns its knob:
//   * depth tint and turbidity — Beer–Lambert through an ASSUMED depth
//     (Turbidity.y) along the view ray: shallow colour looking straight down,
//     deep colour toward the horizon, opacity rising with it;
//   * refraction — bends that view ray by the wave normal, so the waves move the
//     shallow/deep tint and the caustics drawn "below" the surface; the scene
//     behind is not distorted;
//   * shore foam — foam on the wave CRESTS, broken up by moving noise;
//   * caustics — a moving pattern in the water's own tint (it brightens the
//     body where light would be focused), faded with camera distance (where it
//     would only alias) and with the water's own transmittance.
//
// Realism, in the order it was missing (docs/water-shader-plan.md §11): a body
// colour dark enough to let the reflection carry the picture (real water is
// mostly mirror), waves that are a SPREAD of directions and lengths rather than
// three parallel sines, short chop on top, patches of calmer and rougher water,
// roughness that opens up with distance (what the pixels can no longer resolve
// is exactly what a rougher surface averages), lighter water in the thin crests
// where light shines through, and foam that is a thin broken lace rather than a
// white blob. A caustic net drawn ON the surface and white blobs were the two
// things that read as a cartoon at first sight, so both are quiet by default.
//
// Tiling, then (docs/water-shader-plan.md §12): a sum of sines is periodic, and on
// a surface a few hundred metres across the same bright patch and the same foam dot
// came back row after row. The wave trains are therefore bombed on a hex grid
// (waveTrain, hexCells; the Variation knob). Sixteen parameters, every HeParams
// slot is taken.
//
// Assumes a roughly horizontal surface: the wave slope is subtracted from the
// mesh normal in world XZ.
// (Not called `water`: `using namespace HE` above also brings in HE::water, the
// namespace of Physics/WaterWaves.h, and the two would be ambiguous.)
Mb waterMaterial()
{
    Mb m;

    // Params, column 0, in the order the Inspector should read them.
    const int shallow = m.param(T::ParamColor, "ShallowColor", { 0.045f, 0.20f, 0.22f }, "Color",
        "Water colour where the view ray through the water is short (looking straight down).", 0);
    const int deep = m.param(T::ParamColor, "DeepColor", { 0.004f, 0.028f, 0.052f }, "Color",
        "Water colour where the view ray through the water is long (toward the horizon).", 1);
    const int turb = m.param(T::ParamVec4, "Turbidity", { 0.45f, 4.0f, 0.35f, 0.7f }, "Color",
        "x = absorption per metre (higher = murkier, the deep colour arrives sooner). "
        "y = water depth in metres the tint assumes (no scene depth is read yet; in the last metre "
        "before a landscape's shore the real depth takes over, so shallows are clear). "
        "z = shore width: the water fades in over this many metres of depth at the water's "
        "edge (0 = a hard edge, up to 0.7). w = shore foam: the lace along the water's edge "
        "(0 = none, 1 = strong). Needs the water mesh of a landscape; a plane has no shore.", 2);
    // The three knobs start from the trains in Physics/WaterWaves.h, the same ones
    // the water physics follows (WaterQuery), so retuning the water here retunes
    // the surface a crate floats on with it.
    constexpr HE::water::WaveTrain wa = HE::water::kWaveA, wb = HE::water::kWaveB, wc = HE::water::kWaveC;
    const int waveA = m.param(T::ParamVec4, "WaveA", { wa.directionDeg, wa.speed, wa.length, wa.steepness }, "Waves",
        "Swell. x = direction (degrees, 0 = +X, 90 = +Z), y = speed (m/s), "
        "z = wavelength (m), w = steepness (0 = flat, ~0.3 = choppy). Drives a second, "
        "shorter train at a turned angle.", 3);
    const int waveB = m.param(T::ParamVec4, "WaveB", { wb.directionDeg, wb.speed, wb.length, wb.steepness }, "Waves",
        "Second wave train. x = direction (degrees), y = speed (m/s), "
        "z = wavelength (m), w = steepness. Drives a second train at a turned angle.", 4);
    const int waveC = m.param(T::ParamVec4, "WaveC", { wc.directionDeg, wc.speed, wc.length, wc.steepness }, "Waves",
        "Fine ripples. x = direction (degrees), y = speed (m/s), "
        "z = wavelength (m), w = steepness. Also sets the strength of the chop on top.", 5);
    // The four plain floats share one slot (Thema 206 step 6): each used only .x of a whole
    // Vec4 slot, and all sixteen were taken. Slider ranges of the old floats, for the tooltip:
    // FresnelPower 1..10, the other three 0..1.
    const int surf = m.param(T::ParamVec4, "Surface", { 5.0f, 0.8f, 0.08f, 0.3f }, "Surface",
        "x = Fresnel exponent (1..10): how quickly the surface turns reflective toward grazing "
        "angles (5 = physical water). "
        "y = reflection (0..1): how strongly sky and scene reflection cover the water, scales the "
        "Fresnel lift of the opacity and the specular F0. "
        "z = roughness (0..1): low = sharp reflections and a tight sun glint, distant water is "
        "made rougher on top of this. "
        "w = dielectric specular (0..1): 0.5 = F0 0.04, multiplied by reflection, ~0.3 gives "
        "water's F0 0.02.", 6);
    // Shallow water (Thema 206 step 4): the tint and the Opacity floor follow the real shore
    // depth in the last metre before a landscape's shore. Defaults reproduce the step 4 constants.
    const int shallowW = m.param(T::ParamVec4, "ShallowWater", { kShallowClearDepth, kShallowTintE, kShallowOpacity, 0.0f }, "Surface",
        "Shore shallows (need the water mesh of a landscape; a plane has no shore, no effect). "
        "x = clear depth (m, 0..1): from this much water on the opacity floor is whole, "
        "shallower water lets the ground show through more. "
        "y = tint start (0..1, shore closeness: 0 = a metre of water or more, 1 = none): "
        "where the depth tint has fully taken over the real depth, 0.5 = half a metre; "
        "further out it blends to the assumed depth (Turbidity.y). "
        "z = opacity floor (0..1): the share of Opacity left at no depth at all. "
        "w = reserved, no effect yet.", 7);
    const int sfs = m.un(T::SplitRGBA, surf, 1, 20);
    const Pin fresPow{ sfs, 0 }, refl{ sfs, 1 }, rough{ sfs, 2 }, spec{ sfs, 3 };
    const int shs = m.un(T::SplitRGBA, shallowW, 1, 22);
    const Pin clearDepth{ shs, 0 }, tintStart{ shs, 1 }, opacFloor{ shs, 2 };
    const int opac = m.param(T::ParamFloat, "Opacity", { 0.5f }, "Surface",
        "Opacity looking straight down into clear water. Depth tint, Fresnel and foam raise it. "
        "In the last metre before a landscape's shore it falls toward a quarter of itself as the "
        "water gets shallower, so the ground shows through.",
        10, 0.0f, 1.0f);
    const int refr = m.param(T::ParamFloat, "Refraction", { 0.3f }, "Surface",
        "How much the waves bend the view into the water: moves the depth tint and the caustics "
        "with the waves (the scene behind is not distorted yet).", 11, 0.0f, 1.0f);
    const int foamCol = m.param(T::ParamColor, "FoamColor", { 0.82f, 0.88f, 0.90f }, "Foam",
        "Foam colour.", 12);
    const int foam = m.param(T::ParamVec4, "Foam", { 0.06f, 0.5f, 0.7f, 0.3f }, "Foam",
        "x = coverage (share of the wave crests that foam, 0 = none), y = strength (0..1), "
        "z = noise size (m), w = drift speed (m/s).", 13);
    const int caus = m.param(T::ParamVec4, "Caustics", { 0.2f, 2.0f, 0.2f, 18.0f }, "Caustics",
        "x = strength (0 = off), y = pattern size (m), z = speed, "
        "w = camera distance (m) at which the pattern has faded out.", 14);
    const int vari = m.param(T::ParamVec4, "Variation", { 3.5f, 1.0f, 22.0f, 2.5f }, "Waves",
        "Breaks the repeat of the waves (texture bombing): the water is cut into hexes and every "
        "wave train runs once per hex with its own direction and phase, the hexes blend. "
        "x = hex size in wavelengths, y = phase offset between hexes (0..1 of a wavelength), "
        "z = direction offset between hexes (degrees, +/-), w = seam sharpness (1 = broad blends, "
        "8 = sharp). y = 0 and z = 0 switch it off.", 15);

    // Inputs, column 1 below the wave rows.
    const int wp   = m.node(T::WorldPos, 0, 16);
    const int wps  = m.un(T::SplitRGBA, wp, 1, 16);
    const Pin px{ wps, 0 }, pz{ wps, 2 };
    const int time = m.node(T::Time, 0, 17);
    const int geoN = m.node(T::NormalWS, 0, 18);      // the mesh normal; n.x also carries the shore (see "Soft shore")
    const int gs   = m.un(T::SplitRGBA, geoN, 1, 18);
    const int view = m.node(T::ViewDir, 0, 19);
    const int dist = m.node(T::CameraDistance, 0, 20);

    // Fine waves lose their slope with distance: past a point a wavelength is
    // under a pixel and the sine only flickers (there are no mips to filter it).
    const int fadeB = m.un(T::OneMinus, m.op3(T::Smoothstep, m.k(60.0f, 17, 21), m.k(220.0f, 17, 21.5f), dist, 18, 21), 19, 21);
    const int fadeC = m.un(T::OneMinus, m.op3(T::Smoothstep, m.k(25.0f, 17, 22), m.k(110.0f, 17, 22.5f), dist, 18, 22), 19, 22);
    const int fadeR = m.un(T::OneMinus, m.op3(T::Smoothstep, m.k(14.0f, 17, 23), m.k(70.0f, 17, 23.5f), dist, 18, 23), 19, 23);
    // The two finest chop layers (below) are a few centimetres to a decimetre per noise
    // octave, so they go under a pixel much sooner than the coarse chop: each gets its
    // own, shorter fade. The footprint of a pixel at 25° grazing is ~2.4x its width, which
    // is why 0.17 m cells are gone by ~14 m and 0.4 m cells by ~40 m.
    const int fadeF = m.un(T::OneMinus, m.op3(T::Smoothstep, m.k(7.0f, 17, 24), m.k(40.0f, 17, 24.5f), dist, 18, 24), 19, 24);
    const int fadeG = m.un(T::OneMinus, m.op3(T::Smoothstep, m.k(3.0f, 17, 25), m.k(14.0f, 17, 25.5f), dist, 18, 25), 19, 25);
    const Pin fB{ fadeB }, fC{ fadeC }, fR{ fadeR }, fF{ fadeF }, fG{ fadeG };

    // Gusts: slow large patches where the sea is rougher and where it is calmer.
    // Without them the same texture repeats evenly to the horizon. The same field
    // also trades weight between each wave and its companion: where the primary
    // dominates the companion fades and the other way round, so the two sets only
    // cross at equal strength along the patch borders, instead of everywhere —
    // two sets of equal waves crossing at a fixed angle are a diamond lattice.
    const int gustUV = m.op(T::CombineRGBA,
                            m.op(T::Add, px, m.op(T::Multiply, time, m.k(0.35f, 34, 27), 35, 27), 36, 27),
                            m.op(T::Add, pz, m.op(T::Multiply, time, m.k(0.2f, 34, 28), 35, 28), 36, 28), 37, 27);
    const int gustN  = m.fbm(gustUV, m.k(1.0f / 22.0f, 37, 28), 38, 27);
    const int gustS  = m.op3(T::Smoothstep, m.k(0.28f, 38, 31), m.k(0.68f, 38, 32), gustN, 39, 29);
    const int gust   = m.op3(T::Lerp, m.k(0.55f, 38, 29), m.k(1.45f, 38, 30), gustS, 40, 29);
    const int wPrim  = m.op3(T::Lerp, m.k(0.55f, 38, 33), m.k(1.35f, 38, 34), gustS, 40, 31);
    const int wComp  = m.op3(T::Lerp, m.k(1.35f, 38, 35), m.k(0.55f, 38, 36), gustS, 40, 33);
    m.comment("Gusts", 34, 26, 7, 12);

    // Waves: a primary and a companion per knob, four rows each. The slope each
    // train hands on is its own steepness × the gust weight × the distance fade.
    const Pin wA1 = wPrim, wA2 = wComp;
    const Pin wB1 = m.op(T::Multiply, wPrim, fB, 41, 30), wB2 = m.op(T::Multiply, wComp, fB, 41, 31);
    const Pin wC1 = m.op(T::Multiply, wPrim, fC, 41, 32), wC2 = m.op(T::Multiply, wComp, fC, 41, 33);
    constexpr Train kCompanionA{  38.0f, 0.58f, 0.55f, false };
    constexpr Train kCompanionB{ -33.0f, 0.64f, 0.55f, false };
    constexpr Train kCompanionC{  47.0f, 0.71f, 0.60f, false };
    // One hex grid per knob (own cell size from its wavelength, own seed), shared by
    // the knob's train and its companion.
    const Hex hexA = hexCells(m, waveA, vari, px, pz, 3.0f, 50, 0);
    const Hex hexB = hexCells(m, waveB, vari, px, pz, 17.0f, 50, 34);
    const Hex hexC = hexCells(m, waveC, vari, px, pz, 41.0f, 50, 68);
    m.comment("Bombing: one hex grid per wave knob", 50, 0, 14, 100);
    const Wave a  = waveTrain(m, waveA, px, pz, time, kPrimary,    &wA1, nullptr, hexA, 26);
    const Wave a2 = waveTrain(m, waveA, px, pz, time, kCompanionA, &wA2, &a,      hexA, 30);
    const Wave b  = waveTrain(m, waveB, px, pz, time, kPrimary,    &wB1, nullptr, hexB, 34);
    const Wave b2 = waveTrain(m, waveB, px, pz, time, kCompanionB, &wB2, &b,      hexB, 38);
    const Wave c  = waveTrain(m, waveC, px, pz, time, kPrimary,    &wC1, nullptr, hexC, 42);
    const Wave c2 = waveTrain(m, waveC, px, pz, time, kCompanionC, &wC2, &c,      hexC, 46);
    m.comment("Wave A (swell) and its companion", 1, 26, 15, 8);
    m.comment("Wave B and its companion", 1, 34, 15, 8);
    m.comment("Wave C (ripples) and its companion", 1, 42, 15, 8);

    // Chop: four layers of drifting noise slope, from 2.6 m down to 0.17 m per noise
    // cell. Reference 29 (open sea from above) is not a few waves with some noise on top:
    // it is short, irregular wavelets at EVERY scale from a metre to a hand's width, each
    // lit only on its flank, with calm glassy patches between. Two layers left the near
    // field a smooth dark sheet with sparse glints; the finer layers fill the gap down to
    // what a pixel can still resolve (each fades out sooner, see fadeF / fadeG), and they
    // ride different directions and speeds so the pattern does not crawl as one. Strength
    // follows the sum of the three steepnesses, so flat waves (w = 0) mean flat water.
    const int sA = m.un(T::SplitRGBA, waveA, 17, 26);
    const int sB = m.un(T::SplitRGBA, waveB, 17, 27);
    const int sC = m.un(T::SplitRGBA, waveC, 17, 28);
    const Pin drive = m.sum({ Pin{ sA, 3 }, Pin{ sB, 3 }, Pin{ sC, 3 } }, 18, 26);
    const int ampBase = m.op(T::Multiply, drive, m.k(kChopBase, 20, 26), 21, 26);
    const int amp1    = m.op(T::Multiply, ampBase, fR, 22, 26);
    const int amp2    = m.op(T::Multiply, m.op(T::Multiply, ampBase, m.k(kChopMid, 22, 27), 22, 27), fR, 23, 27);
    const int amp3    = m.op(T::Multiply, m.op(T::Multiply, ampBase, m.k(kChopFine, 22, 28), 22, 28), fF, 23, 28);
    const int amp4    = m.op(T::Multiply, m.op(T::Multiply, ampBase, m.k(kChopFiner, 22, 29), 22, 29), fG, 23, 29);
    auto drift = [&](const Wave& w, float speed, float col, float row) {
        return std::pair<Pin, Pin>{
            m.op(T::Multiply, w.dirX, m.op(T::Multiply, w.speed, m.k(speed, col, row), col + 1, row), col + 2, row),
            m.op(T::Multiply, w.dirZ, m.op(T::Multiply, w.speed, m.k(speed, col, row + 1), col + 1, row + 1), col + 2, row + 1) };
    };
    const auto d1 = drift(b, 0.55f, 24, 29), d2 = drift(c, 0.9f, 24, 36);
    const auto d3 = drift(b, -0.8f, 24, 43), d4 = drift(c, -1.3f, 24, 50);
    const Grad r1 = rippleLayer(m, px, pz, time, 2.6f,  d1.first, d1.second, amp1, 24, 31);
    const Grad r2 = rippleLayer(m, px, pz, time, 0.9f,  d2.first, d2.second, amp2, 24, 38);
    const Grad r3 = rippleLayer(m, px, pz, time, 0.4f,  d3.first, d3.second, amp3, 24, 45);
    const Grad r4 = rippleLayer(m, px, pz, time, 0.17f, d4.first, d4.second, amp4, 24, 52);
    m.comment("Chop (noise slope, finite differences)", 24, 29, 8, 14);

    const Pin slopesX[] = { a.slopeX, a2.slopeX, b.slopeX, b2.slopeX, c.slopeX, c2.slopeX, r1.x, r2.x, r3.x, r4.x };
    const Pin slopesZ[] = { a.slopeZ, a2.slopeZ, b.slopeZ, b2.slopeZ, c.slopeZ, c2.slopeZ, r1.z, r2.z, r3.z, r4.z };
    // The rings the water physics raised where a body went into or out of the surface
    // (the Water Ripple node, which reads the lighting block the engine fills). Their
    // slope is one more term of the SAME sum, so it leans the one normalised wave normal
    // below and everything downstream (Fresnel, refraction, caustics, the sun glint) sees it;
    // it is added AFTER the gust, because a ring is not a patch of rougher water, and fades
    // with distance for the reason the chop does (a ring is a few decimetres long, under a
    // pixel at the horizon). With no ring in the scene the node returns zeros and the terms
    // add nothing. No parameter: all sixteen HeParams slots are taken.
    const int ring   = m.node(T::WaterRipple, 44, 31);        // position unwired = the fragment's world XZ
    const int fadeRg = m.un(T::OneMinus, m.op3(T::Smoothstep, m.k(30.0f, 44, 33), m.k(120.0f, 44, 33.5f), dist, 45, 33), 46, 33);
    const int rs     = m.un(T::SplitRGBA, Pin{ ring, 1 }, 45, 31);
    const int ringX  = m.op(T::Multiply, Pin{ rs, 0 }, fadeRg, 46, 31);
    const int ringZ  = m.op(T::Multiply, Pin{ rs, 1 }, fadeRg, 46, 32);
    const int sx    = m.op(T::Add, m.op(T::Multiply, m.sum(std::vector<Pin>(std::begin(slopesX), std::end(slopesX)), 41, 26), gust, 44, 26),
                           ringX, 45, 26);
    const int sz    = m.op(T::Add, m.op(T::Multiply, m.sum(std::vector<Pin>(std::begin(slopesZ), std::end(slopesZ)), 41, 28), gust, 44, 28),
                           ringZ, 45, 28);
    const int tilt  = m.node(T::Combine3, 45, 27);
    // The mesh normal's x tilt is the shore proximity, not a slope: it goes into the
    // tilt too, so subtracting the tilt leaves the wave slope alone (0 off a landscape).
    // A landscape rotated about Y moves that tilt out of x and into z (and back), so
    // both components get the same treatment — whatever the rotation did to it, it is
    // still exactly (gs.x, gs.z) and subtracting that cancels it regardless of heading.
    m.link(m.op(T::Add, sx, Pin{ gs, 0 }, 45, 25), tilt, 0);
    m.link(m.op(T::Add, sz, Pin{ gs, 2 }, 45, 29), tilt, 2);
    const int waveN = m.un(T::Normalize3, m.op(T::Subtract, geoN, tilt, 46, 27), 47, 27);

    // Fresnel off the WAVE normal with an exposed exponent. The Fresnel node
    // bakes its power and reads the mesh normal, so it would see neither.
    const int ndv  = m.un(T::Saturate, m.op(T::DotProduct, waveN, view, 21, 2), 22, 2);
    const int fres = m.op(T::Power, m.un(T::OneMinus, ndv, 23, 2), fresPow, 24, 2);
    m.comment("Fresnel", 21, 2, 4, 1.6f);

    // Crest finder for the foam: the wind waves' summed height (B, its companion
    // and C) normalised by their summed steepness, −1 (trough) .. 1 (every train
    // at its crest), then 0..1. Whitecaps sit on the steep short waves, not on the
    // long swell, and a threshold on THREE sines leaves crest-shaped streaks where
    // a threshold on all six left round spots.
    const Pin sumH = m.sum({ b.height, b2.height, c.height }, 17, 17);
    const Pin sumS = m.sum({ b.steep, b2.steep, c.steep }, 17, 19);
    const int crest = m.op(T::Add, m.op(T::Multiply, m.op(T::Divide, sumH, sumS, 21, 17), m.k(0.5f, 21, 18), 22, 17),
                           m.k(0.5f, 22, 18), 23, 17);

    // The water's depth under this pixel, as far as the mesh knows it (see "Soft shore"
    // below): e = 1 at the waterline falling to 0 at kShoreFadeDepth metres of water, read
    // from the normal's x tilt. Nothing deeper than that is visible here; on a plane (and in
    // open water) e = 0, the depth is the full fade depth and everything built on it is neutral.
    // Read as the tilt's magnitude in the XZ plane, not x alone: a landscape rotated about Y
    // moves the baked tilt between x and z (zeroing a raw x/y read at 90°) but leaves
    // sqrt(x^2+z^2) exactly where it was, since the local normal only ever had one horizontal
    // component to begin with. A non-uniform X/Z-vs-Y scale on the landscape also scales this
    // read by sx/sy, since mat3(uModel) is what every backend's vertex stage puts the normal
    // through (MaterialShaderLibrary.cpp) — the shader itself still can't see that scale to
    // undo it, so the mesh bakes the inverse ratio in instead (SurfaceOptions::shoreTiltScale,
    // WaterMesh.h/WaterSurface.cpp's shoreTiltScaleOf), and the two cancel.
    constexpr float kShoreGain  = HE::water::kShoreNormalGain;
    constexpr float kShoreDepth = HE::water::kShoreFadeDepth;
    const int shoreX2  = m.op(T::Multiply, Pin{ gs, 0 }, Pin{ gs, 0 }, 44, 11);
    const int shoreZ2  = m.op(T::Multiply, Pin{ gs, 2 }, Pin{ gs, 2 }, 44, 12);
    const int shoreMag = m.op(T::Power, m.op(T::Add, shoreX2, shoreZ2, 45, 11), m.k(0.5f, 45, 12), 46, 11);
    const int shoreE  = m.un(T::Saturate, m.op(T::Divide, shoreMag, m.op(T::Multiply, Pin{ gs, 1 }, m.k(kShoreGain, 47, 13), 48, 13), 49, 12), 50, 12);
    const int shoreD  = m.op(T::Multiply, m.un(T::OneMinus, shoreE, 51, 12), m.k(kShoreDepth, 51, 13), 52, 12);

    // Water body: transmittance exp(-absorption · depth / cos) along the view ray,
    // with the ray bent toward the wave normal by Refraction.
    const int ts    = m.un(T::SplitRGBA, turb, 16, 5);
    const int refrN = m.un(T::Normalize3, m.op3(T::Lerp, geoN, waveN, refr, 21, 5), 22, 5);
    const int ndvR  = m.un(T::Saturate, m.op(T::DotProduct, refrN, view, 23, 5), 24, 5);
    // The depth the tint assumes is Turbidity.y (a guess, no scene depth is read), until the
    // mesh knows better: in the last half metre of water before the shore the real depth
    // takes over, so shallow water is as clear as it is shallow (the ground and the shallow
    // colour come through) and thickens to the assumed depth over the metre before. The blend
    // is a smoothstep of e, which is 0 at one metre: no step where the shore range begins,
    // and in open water (e = 0) this is Turbidity.y itself.
    // tintStart is a user knob (0..1, see ShallowWater param above): a branchless
    // max(tintStart, eps) before it feeds Smoothstep as the non-zero edge — equal
    // Smoothstep edges (tintStart <= 0) are undefined behaviour. A true max rather
    // than an unconditional epsilon add (like shoreW below) so any value already
    // above eps, including the default, passes through unchanged.
    const int epsTint = m.k(0.001f, 13, 3.5f);
    const int tintOk  = m.op(T::Greater, tintStart, epsTint, 13, 4);
    const int tintStartC = m.op3(T::If, tintOk, tintStart, epsTint, 14, 4);
    const int tintMix = m.op3(T::Smoothstep, m.k(0.0f, 14, 5), tintStartC, shoreE, 15, 5);
    const int tintD   = m.op3(T::Lerp, Pin{ ts, 1 }, shoreD, tintMix, 17, 5);
    const int path  = m.op(T::Divide, tintD, m.op(T::Add, ndvR, m.k(0.05f, 24, 6), 25, 5), 26, 5);
    const int dens  = m.op(T::Multiply, m.op(T::Multiply, Pin{ ts, 0 }, path, 27, 5), m.k(-1.0f, 27, 6), 28, 5);
    const int trans = m.op(T::Power, m.k(kE, 28, 6), dens, 29, 5);
    const int body0 = m.op3(T::Lerp, shallow, deep, m.un(T::OneMinus, trans, 30, 6), 31, 5);
    // Thin crests let light through: the shallow colour comes back along the
    // SWELL's crests (the green-blue glow of a wave's top). Read off the long
    // swell alone: the sum of all six trains peaks where they happen to line up,
    // which is a lattice of separate spots, not a wave.
    const Pin swellH = m.sum({ a.height, a2.height }, 31, 8);
    const Pin swellS = m.sum({ a.steep, a2.steep }, 31, 9);
    const int swellCrest = m.op(T::Add, m.op(T::Multiply, m.op(T::Divide, swellH, swellS, 32, 8), m.k(0.5f, 32, 9), 33, 8),
                                m.k(0.5f, 33, 9), 34, 8);
    const int glow  = m.op3(T::Smoothstep, m.k(0.65f, 31, 6), m.k(1.0f, 31, 7), swellCrest, 35, 8);
    // Calm and rough patches differ a little in brightness too (the gust field).
    const int patch = m.op(T::Add, m.k(0.88f, 33, 10), m.op(T::Multiply, gust, m.k(0.12f, 33, 11), 34, 10), 35, 10);
    const int body  = m.op(T::Multiply,
                           m.op(T::Add, body0, m.op(T::Multiply, m.op(T::Multiply, shallow, glow, 36, 6), m.k(0.1f, 36, 7), 37, 6), 38, 6),
                           patch, 39, 6);
    m.comment("Water body (depth tint)", 16, 5, 20, 3);

    // Caustics: two FBM fields drifting apart; where they agree, a bright line.
    // Applied as a brightening of the body (light focused through the surface),
    // soft and wide: a thin white line on the surface is what a cartoon draws.
    const int cs    = m.un(T::SplitRGBA, caus, 16, 9);
    const int ns    = m.un(T::SplitRGBA, waveN, 21, 9);
    const int shift = m.op(T::Multiply, refr, Pin{ cs, 1 }, 22, 10);
    const int cx    = m.op(T::Add, px, m.op(T::Multiply, Pin{ ns, 0 }, shift, 23, 9), 24, 9);
    const int cz    = m.op(T::Add, pz, m.op(T::Multiply, Pin{ ns, 2 }, shift, 23, 10), 24, 10);
    const int cuv   = m.op(T::CombineRGBA, cx, cz, 25, 9);
    const int invC  = m.op(T::Divide, m.k(1.0f, 24, 11), Pin{ cs, 1 }, 25, 11);
    const int pan1  = m.op3(T::Panner, cuv, Pin{ cs, 2 },
                            m.op(T::Multiply, Pin{ cs, 2 }, m.k(0.4f, 25, 12), 26, 12), 26, 9);
    const int pan2  = m.op3(T::Panner, cuv,
                            m.op(T::Multiply, Pin{ cs, 2 }, m.k(-0.6f, 25, 13), 26, 13),
                            m.op(T::Multiply, Pin{ cs, 2 }, m.k(0.8f, 25, 14), 26, 14), 27, 10);
    const int f1    = m.fbm(pan1, invC, 28, 9);
    const int f2    = m.fbm(pan2, m.op(T::Multiply, invC, m.k(1.37f, 27, 12), 28, 11), 29, 10);
    const int gap   = m.un(T::Absolute, m.op(T::Subtract, f1, f2, 30, 9), 31, 9);
    const int web   = m.op(T::Power,
                           m.un(T::Saturate, m.un(T::OneMinus, m.op(T::Multiply, gap, m.k(3.5f, 31, 10), 32, 9), 33, 9), 34, 9),
                           m.k(3.0f, 34, 10), 35, 9);
    const int near  = m.un(T::OneMinus, m.op3(T::Smoothstep, m.k(0.0f, 33, 12), Pin{ cs, 3 }, dist, 34, 12), 35, 12);
    const int caust = m.op(T::Multiply, m.op(T::Multiply, m.op(T::Multiply, web, Pin{ cs, 0 }, 36, 9), trans, 37, 9),
                           near, 38, 9);
    const int lit   = m.op(T::Multiply, body, m.op(T::Add, m.k(1.0f, 38, 10), m.op(T::Multiply, caust, m.k(3.0f, 38, 11), 39, 10), 40, 10), 41, 8);
    m.comment("Caustics", 16, 9, 26, 6);

    // Foam: whitecaps on the highest crests, broken into lace by two scales of
    // drifting noise (a coarse one that decides WHERE, a fine one that decides
    // how it frays), and see-through at its edges.
    const int fs      = m.un(T::SplitRGBA, foam, 16, 16);
    const int thresh  = m.op(T::Subtract, m.k(0.97f, 21, 16), m.op(T::Multiply, Pin{ fs, 0 }, m.k(0.9f, 21, 17), 22, 16), 23, 16);
    const int band    = m.op3(T::Smoothstep, thresh, m.op(T::Add, thresh, m.k(0.15f, 21, 18.5f), 22, 18), crest, 25, 17);
    const int fuv     = m.op3(T::Panner, m.op(T::CombineRGBA, px, pz, 21, 19), Pin{ fs, 3 },
                              m.op(T::Multiply, Pin{ fs, 3 }, m.k(0.5f, 21, 20), 22, 20), 23, 19);
    const int fuv2    = m.op3(T::Panner, m.op(T::CombineRGBA, px, pz, 21, 21), m.op(T::Multiply, Pin{ fs, 3 }, m.k(-0.7f, 21, 22), 22, 22),
                              m.op(T::Multiply, Pin{ fs, 3 }, m.k(0.4f, 21, 23), 22, 23), 23, 21);
    const int invF    = m.op(T::Divide, m.k(1.0f, 23, 20), Pin{ fs, 2 }, 24, 20);
    const int n1      = m.fbm(fuv, invF, 24, 19);
    const int n2      = m.fbm(fuv2, m.op(T::Multiply, invF, m.k(3.1f, 24, 22), 25, 22), 25, 21);
    const int lace    = m.op(T::Add, m.op(T::Multiply, n1, m.k(0.6f, 26, 19), 27, 19),
                             m.op(T::Multiply, n2, m.k(0.4f, 26, 21), 27, 21), 28, 20);
    const int breakup = m.op3(T::Smoothstep, m.k(0.36f, 28, 21), m.k(0.74f, 28, 22), lace, 29, 20);
    const int waveFoam = m.op(T::Multiply, m.op(T::Multiply, band, breakup, 30, 18), Pin{ fs, 1 }, 31, 18);
    // The crest of a strong ring is white too: the Water Ripple node's foam, broken up by the
    // same lace (half of it stays solid so a ring does not turn into dots) and faded with
    // distance like its slope. Zero without a ring, so the sum is the wave foam alone.
    const int ringFoam = m.op(T::Multiply, m.op(T::Multiply, Pin{ ring, 2 }, fadeRg, 31, 19),
                              m.op(T::Add, m.k(0.5f, 31, 20), m.op(T::Multiply, breakup, m.k(0.5f, 31, 21), 32, 20), 33, 20), 34, 19);
    const int foamAmt = m.un(T::Saturate, m.op(T::Add, waveFoam, ringFoam, 35, 18), 36, 18);
    m.comment("Foam", 16, 16, 16, 8);

    // Soft shore (docs/water-realistic-baseline-2026-10-20.md, Thema 206 Schritt 2). The
    // material pass has no scene depth, so the water mesh brings the water's depth along:
    // each vertex of a landscape's sheet tilts its normal in x by kShoreNormalGain · e
    // (WaterMesh.cpp), e = 1 at the waterline falling to 0 at kShoreFadeDepth metres of
    // water. Read back here (shoreE above) as e = |n.xz| / n.y / gain, depth = (1 − e) ·
    // kShoreFadeDepth — the magnitude, not n.x alone, because a landscape rotated about Y
    // moves the tilt between x and z (Thema 206 Schritt 8). A plane (or any mesh without the
    // channel) has n.x = n.z = 0: e = 0, the depth is the full fade depth, and every term
    // below is exactly neutral (ramp 1, foam 0, the gate shut), so open water is bit for bit
    // what it was.
    //   * alpha ramp: smoothstep over the width, so the edge fades out instead of cutting;
    //   * the ramp's own edge is wobbled by the foam lace, so the line of the water is
    //     organic, not the contour of a lattice cell;
    //   * foam fringe: a lace band over the part of the ramp where it is half there.
    const int shoreW  = m.op(T::Add, m.un(T::Saturate, Pin{ ts, 2 }, 47, 15), m.k(0.001f, 47, 16), 48, 15);      // never 0
    const int shoreGate = m.op(T::Step, m.k(1e-4f, 51, 15), shoreE, 52, 15);                                      // 0 on open water
    const int wobble  = m.op(T::Multiply, m.op(T::Multiply, m.op(T::Subtract, lace, m.k(0.5f, 49, 17), 50, 17), shoreW, 51, 17), m.k(0.6f, 51, 18), 52, 17);
    const int shoreF  = m.op(T::Divide, m.op(T::Add, shoreD, wobble, 53, 12), shoreW, 54, 12);                    // depth in widths
    const int rampS   = m.op3(T::Smoothstep, m.k(0.0f, 53, 14), m.k(1.0f, 53, 14.5f), shoreF, 55, 12);
    const int ramp    = m.op3(T::Lerp, m.k(1.0f, 55, 14), rampS, shoreGate, 56, 12);
    const int fringe  = m.op(T::Multiply,
                             m.op3(T::Smoothstep, m.k(0.04f, 53, 16), m.k(0.14f, 53, 16.5f), shoreF, 55, 16),
                             m.un(T::OneMinus, m.op3(T::Smoothstep, m.k(0.14f, 53, 18), m.k(0.5f, 53, 18.5f), shoreF, 55, 18), 56, 18), 57, 16);
    const int shoreFoam = m.op(T::Multiply, m.op(T::Multiply, fringe, Pin{ ts, 3 }, 58, 16),
                               m.op(T::Add, m.k(0.35f, 57, 19), m.op(T::Multiply, breakup, m.k(0.65f, 57, 20), 58, 19), 59, 19), 60, 16);
    const int foamAll = m.un(T::Saturate, m.op(T::Add, foamAmt, shoreFoam, 61, 16), 62, 16);
    m.comment("Soft shore", 47, 12, 16, 9);

    // Output tail. Far water is rougher: what a pixel can no longer resolve is
    // what a rougher surface averages, and it keeps the horizon from turning into
    // a mirror strip of single-pixel glints.
    const int farRough = m.op(T::Multiply, m.op3(T::Smoothstep, m.k(10.0f, 41, 1), m.k(160.0f, 41, 1.5f), dist, 42, 1),
                              m.k(0.25f, 42, 1.5f), 43, 1);
    const int foamTint = m.op(T::Multiply, foamCol, m.op(T::Add, m.k(0.55f, 42, 8), m.op(T::Multiply, lace, m.k(0.7f, 42, 9), 43, 8), 44, 8), 45, 8);
    const int base   = m.op3(T::Lerp, lit, foamTint, foamAll, 46, 7);
    // Opacity is what clear water still covers looking straight down. Water that is only
    // centimetres deep covers far less: the floor falls toward kShallowOpacity of itself as
    // the depth goes to 0, from full at kShallowClearDepth metres (a smoothstep, 1 at e = 0
    // where shoreD is a metre, so neutral outside the shore range).
    // clearDepth is a user knob too: same branchless max(., eps) clamp as tintStart above.
    const int epsClear = m.k(0.001f, 36, 1.5f);
    const int clearOk  = m.op(T::Greater, clearDepth, epsClear, 36, 2);
    const int clearDepthC = m.op3(T::If, clearOk, clearDepth, epsClear, 37, 2);
    const int clearMix = m.op3(T::Smoothstep, m.k(0.0f, 37, 3), clearDepthC, shoreD, 38, 3);
    const int opacEff  = m.op(T::Multiply, opac, m.op3(T::Lerp, opacFloor, m.k(1.0f, 38, 2.5f), clearMix, 39, 2), 40, 2.5f);
    const int bodyA  = m.op3(T::Lerp, opacEff, m.k(1.0f, 39, 3), m.un(T::OneMinus, trans, 39, 4), 40, 3);
    const int alpha1 = m.op3(T::Lerp, bodyA, m.k(1.0f, 40, 4.5f), m.op(T::Multiply, refl, fres, 40, 2), 41, 3);
    const int alpha0 = m.op3(T::Lerp, alpha1, m.k(1.0f, 41, 4.5f), m.op(T::Multiply, foamAmt, m.k(0.85f, 41, 5), 42, 5), 43, 3);
    // The shore: the water (and the wave foam on it) fades in over the width, the
    // shore's own foam comes on top of the ramp and is opaque where it is thick.
    const int alpha  = m.op3(T::Lerp, m.op(T::Multiply, alpha0, ramp, 44, 3.5f), m.k(1.0f, 44, 4.5f),
                             m.op(T::Multiply, shoreFoam, m.k(0.85f, 44, 5), 45, 5), 46, 3);
    const int specO  = m.op(T::Multiply, spec, refl, 41, 0);
    const int roughO = m.op3(T::Lerp, m.op(T::Add, rough, farRough, 44, 2), m.k(0.6f, 43, 2), foamAll, 45, 2);

    const Under under = undersideTerms(m, geoN, view, waveN, pan1, pan2, invC, near, Pin{ cs, 0 }, shallow, deep, 66, 0);

    // Every output picks the top or the underside. If is mix(top, under, 0 or 1),
    // so from above it is the old expression exactly — as long as nothing on the
    // underside can turn into NaN or Inf, which undersideTerms keeps to.
    const int below  = under.below;
    const int baseO  = m.op3(T::If, below, m.k(0.0f, 76, 0), base, 77, 0);
    const int specO2 = m.op3(T::If, below, m.k(0.0f, 76, 1), specO, 77, 1);
    const int roughO2 = m.op3(T::If, below, m.k(1.0f, 76, 2), roughO, 77, 2);
    const int alphaO = m.op3(T::If, below, under.alpha, alpha, 77, 3);
    const int emisO  = m.op3(T::If, below, under.emissive, m.k(0.0f, 76, 4), 77, 4);
    const int normO  = m.op3(T::If, below, m.op(T::Multiply, waveN, m.k(-1.0f, 75, 5), 76, 5), waveN, 77, 5);
    m.comment("Top or underside", 76, 0, 2, 6);

    const int out = m.node(T::Output, 79, 2);
    MatGraphNode& o = *m.g.findNode(out);
    o.p[0] = 1.0f;                                                  // lit
    o.p[1] = static_cast<float>(MatBlendMode::Translucent);
    o.p[3] = static_cast<float>(MatDomain::Surface);
    m.link(baseO,   out, kMatOutputBaseColorPin);
    m.link(specO2,  out, kMatOutputSpecularPin);
    m.link(roughO2, out, kMatOutputRoughnessPin);
    m.link(emisO,   out, kMatOutputEmissivePin);
    m.link(alphaO,  out, kMatOutputOpacityPin);
    m.link(normO,   out, kMatOutputNormalPin);
    return m;
}

// The parameters a material promises, in no particular order — the codegen
// numbers slots by first use, not by canvas position.
const std::set<std::string> kWaterParams = {
    "ShallowColor", "DeepColor", "Turbidity", "WaveA", "WaveB", "WaveC", "Surface",
    "ShallowWater", "Opacity", "Refraction", "FoamColor", "Foam",
    "Caustics", "Variation",
};

// A cap on the longest wire chain, well inside what the editor's own stack
// holds on every platform (CMakeLists.txt "MSVC main-thread stack"). A graph
// that creeps past it fails here, not in a Debug test on another OS.
constexpr int kMaxChain = 90;

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: mat_gen <output-dir>\n");
        return 2;
    }
    const std::string outDir = argv[1];
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    // ORDER IS IDENTITY: the index picks the UUID, and a scene that already uses
    // one of these stores that UUID. Append, never reorder.
    struct Entry { const char* name; Mb mb; const std::set<std::string>* params; };
    std::vector<Entry> entries;
    entries.push_back({ "Water", waterMaterial(), &kWaterParams });

    int ok = 0, index = 0;
    for (Entry& e : entries)
    {
        const std::string file = std::string(e.name) + ".hasset";
        bool good = e.mb.ok;

        // Check the graph BEFORE writing anything: the exposed set must be the
        // promised one exactly, inside the slot budget, and texture-free (an
        // engine material must not depend on a file the project may not have).
        const MatShaderGen gen = generateFragment(e.mb.g);
        std::set<std::string> got;
        for (const MatParamSlot& s : gen.params) got.insert(s.name);
        for (const std::string& n : *e.params)
            if (!got.count(n)) { std::fprintf(stderr, "  %s: parameter '%s' is not exposed\n", e.name, n.c_str()); good = false; }
        for (const std::string& n : got)
            if (!e.params->count(n)) { std::fprintf(stderr, "  %s: unexpected parameter '%s'\n", e.name, n.c_str()); good = false; }
        if (static_cast<int>(gen.params.size()) > kMatMaxParams)
            { std::fprintf(stderr, "  %s: %zu parameters, budget is %d\n", e.name, gen.params.size(), kMatMaxParams); good = false; }
        if (!gen.textures.empty())
            { std::fprintf(stderr, "  %s: samples %zu texture(s); engine materials must not\n", e.name, gen.textures.size()); good = false; }
        if (gen.glsl.empty()) { std::fprintf(stderr, "  %s: codegen produced no shader\n", e.name); good = false; }
        const int chain = longestChain(e.mb.g);
        if (chain > kMaxChain)
            { std::fprintf(stderr, "  %s: longest wire chain is %d nodes, cap is %d\n", e.name, chain, kMaxChain); good = false; }
        if (!good) { std::fprintf(stderr, "  FAILED %s — nothing written\n", e.name); ++index; continue; }

        MaterialAsset a;
        a.type          = HE::AssetType::Material;
        a.name          = e.name;
        a.path          = file;
        a.id            = HE::UUID{ kMatBaseHi + static_cast<uint64_t>(index), 0x0000000000000001ULL };
        a.nodeGraphJson = materialGraphToJson(e.mb.g);
        // Not read by any backend today (every draw is two-sided), but it is
        // the truth about a water plane seen from below.
        a.doubleSided   = true;

        // Pass 1 writes the graph; pass 2 writes what the editor's load path
        // makes of it (see the header: the packer ships the baked fields).
        bool written = false;
        {
            ContentManager cm(outDir);
            written = cm.saveAsset(a);
        }
        if (written)
        {
            ContentManager cm(outDir);
            const HE::UUID id = cm.loadAsset(file);
            MaterialAsset* m = cm.getMaterialMutable(id);
            written = m && id == a.id && m->graphParamNames.size() == gen.params.size()
                   && !m->customShaderFragGlsl.empty() && cm.saveAsset(*m);
        }

        if (written)
        {
            std::printf("  %-8s %3zu nodes  chain %2d  %2zu params  blend %u  slots:",
                        e.name, e.mb.g.nodes.size(), chain, gen.params.size(), unsigned(gen.blendMode));
            for (const MatParamSlot& s : gen.params) std::printf(" %s", s.name.c_str());
            std::printf("\n");
            ++ok;
        }
        else
            std::fprintf(stderr, "  FAILED to write %s\n", file.c_str());
        ++index;
    }

    std::printf("mat_gen: wrote %d/%zu materials to %s\n", ok, entries.size(), outDir.c_str());
    return ok == static_cast<int>(entries.size()) ? 0 : 1;
}
