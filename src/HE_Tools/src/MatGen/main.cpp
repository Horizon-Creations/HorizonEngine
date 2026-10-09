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
constexpr uint64_t kMatBaseHi = 0x0000000000000400ULL;
static_assert(kEngineWaterMaterialId.hi == kMatBaseHi + 0 && kEngineWaterMaterialId.lo == 1,
              "Water is entry 0 of the 0x400 block — DefaultAssets.h must agree");

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
struct Wave { Pin slopeX, slopeZ, height, steep, dirX, dirZ, speed, warp; };

struct Train
{
    float turnDeg;     // companion: added to the knob's direction
    float lengthScale; // companion: × the knob's wavelength
    float steepScale;  // companion: × the knob's steepness
    bool  warp;
};
constexpr Train kPrimary{ 0.0f, 1.0f, 1.0f, true };

Wave waveTrain(Mb& m, int param, Pin px, Pin pz, Pin time, const Train& tr, const Pin* fade,
               const Wave* primary, float row)
{
    const int s = m.un(T::SplitRGBA, param, 1, row);
    Pin dirDeg{ s, 0 }, speed{ s, 1 }, length{ s, 2 }, steep{ s, 3 };
    const Pin knobSpeed = speed, knobSteep = steep;
    if (tr.turnDeg != 0.0f)     dirDeg = m.op(T::Add, dirDeg, m.k(tr.turnDeg, 1, row + 1), 2, row + 1);
    if (tr.lengthScale != 1.0f)
    {
        length = m.op(T::Multiply, length, m.k(tr.lengthScale, 1, row + 2), 2, row + 2);
        speed  = m.op(T::Multiply, speed, m.k(std::sqrt(tr.lengthScale), 1, row + 3), 2, row + 3);
    }
    if (tr.steepScale != 1.0f)  steep = m.op(T::Multiply, steep, m.k(tr.steepScale, 1, row + 3.5f), 2, row + 3.5f);

    // Direction in degrees → (cos, sin) on the XZ plane. The library has no
    // Cosine node; Sine(x + π/2) is one.
    const int rad  = m.op(T::Multiply, dirDeg, m.k(kDegToRad, 1, row + 1), 2, row);
    const int sinD = m.un(T::Sine, rad, 3, row);
    const int cosD = m.un(T::Sine, m.op(T::Add, rad, m.k(kHalfPi, 2, row + 1), 3, row + 1), 4, row + 1);

    // Into the wave's frame: `along` runs with the wave, `across` along its crest.
    const int along  = m.op(T::Add, m.op(T::Multiply, cosD, px, 5, row),
                                    m.op(T::Multiply, sinD, pz, 5, row + 0.6f), 6, row);
    const int travel = m.op(T::Subtract, along, m.op(T::Multiply, speed, time, 6, row + 2.4f), 7, row);

    int warp = -1;
    if (tr.warp)
    {
        const int across = m.op(T::Subtract, m.op(T::Multiply, cosD, pz, 5, row + 1.2f),
                                             m.op(T::Multiply, sinD, px, 5, row + 1.8f), 6, row + 1.2f);
        const int warpUV = m.op(T::CombineRGBA, travel, across, 8, row + 1);
        const int fbm    = m.fbm(warpUV, m.op(T::Divide, m.k(1.0f, 7, row + 2), length, 8, row + 2), 9, row + 1);
        warp = m.op(T::Multiply, fbm, m.k(2.5f, 9, row + 2), 10, row + 1);
    }
    else if (primary)
        warp = m.op(T::Multiply, primary->warp, m.k(0.9f, 9, row + 2), 10, row + 1);

    const int kWave = m.op(T::Divide, m.k(kTau, 7, row + 3), length, 8, row + 3);
    int phase = m.op(T::Multiply, kWave, travel, 9, row);
    if (warp >= 0) phase = m.op(T::Add, phase, warp, 11, row);
    const int sinP  = m.un(T::Sine, phase, 12, row);
    const int cosP  = m.un(T::Sine, m.op(T::Add, phase, m.k(kHalfPi, 11, row + 1), 12, row + 1), 13, row + 1);

    int slope = m.op(T::Multiply, steep, cosP, 14, row + 1);
    if (fade) slope = m.op(T::Multiply, slope, *fade, 14, row + 2);
    Wave w;
    w.slopeX = m.op(T::Multiply, slope, cosD, 15, row);
    w.slopeZ = m.op(T::Multiply, slope, sinD, 15, row + 1);
    w.height = m.op(T::Multiply, steep, sinP, 15, row + 2);   // crest finder for the foam
    w.steep  = steep;
    w.dirX   = cosD;
    w.dirZ   = sinD;
    w.speed  = knobSpeed;
    w.warp   = warp >= 0 ? Pin{ warp } : Pin{};
    (void)knobSteep;
    return w;
}

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
// Assumes a roughly horizontal surface: the wave slope is subtracted from the
// mesh normal in world XZ.
Mb water()
{
    Mb m;

    // Params, column 0, in the order the Inspector should read them.
    const int shallow = m.param(T::ParamColor, "ShallowColor", { 0.045f, 0.20f, 0.22f }, "Color",
        "Water colour where the view ray through the water is short (looking straight down).", 0);
    const int deep = m.param(T::ParamColor, "DeepColor", { 0.004f, 0.028f, 0.052f }, "Color",
        "Water colour where the view ray through the water is long (toward the horizon).", 1);
    const int turb = m.param(T::ParamVec2, "Turbidity", { 0.45f, 4.0f }, "Color",
        "x = absorption per metre (higher = murkier, the deep colour arrives sooner). "
        "y = water depth in metres the tint assumes (no scene depth is read yet).", 2);
    const int waveA = m.param(T::ParamVec4, "WaveA", { 20.0f, 1.0f, 14.0f, 0.07f }, "Waves",
        "Swell. x = direction (degrees, 0 = +X, 90 = +Z), y = speed (m/s), "
        "z = wavelength (m), w = steepness (0 = flat, ~0.3 = choppy). Drives a second, "
        "shorter train at a turned angle.", 3);
    const int waveB = m.param(T::ParamVec4, "WaveB", { 335.0f, 0.8f, 5.0f, 0.10f }, "Waves",
        "Second wave train. x = direction (degrees), y = speed (m/s), "
        "z = wavelength (m), w = steepness. Drives a second train at a turned angle.", 4);
    const int waveC = m.param(T::ParamVec4, "WaveC", { 70.0f, 0.5f, 1.6f, 0.09f }, "Waves",
        "Fine ripples. x = direction (degrees), y = speed (m/s), "
        "z = wavelength (m), w = steepness. Also sets the strength of the chop on top.", 5);
    const int fresPow = m.param(T::ParamFloat, "FresnelPower", { 5.0f }, "Surface",
        "Fresnel exponent: how quickly the surface turns reflective toward grazing angles "
        "(5 = physical water).", 6, 1.0f, 10.0f);
    const int refl = m.param(T::ParamFloat, "Reflection", { 0.8f }, "Surface",
        "How strongly sky and scene reflection cover the water: scales the Fresnel lift of "
        "the opacity and the specular F0.", 7, 0.0f, 1.0f);
    const int rough = m.param(T::ParamFloat, "Roughness", { 0.08f }, "Surface",
        "Surface roughness. Low = sharp reflections and a tight sun glint. Distant water "
        "is made rougher on top of this.", 8, 0.0f, 1.0f);
    const int spec = m.param(T::ParamFloat, "Specular", { 0.3f }, "Surface",
        "Dielectric specular (0.5 = F0 0.04). Multiplied by Reflection; ~0.3 gives water's F0 0.02.",
        9, 0.0f, 1.0f);
    const int opac = m.param(T::ParamFloat, "Opacity", { 0.5f }, "Surface",
        "Opacity looking straight down into clear water. Depth tint, Fresnel and foam raise it.",
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

    // Inputs, column 1 below the wave rows.
    const int wp   = m.node(T::WorldPos, 0, 16);
    const int wps  = m.un(T::SplitRGBA, wp, 1, 16);
    const Pin px{ wps, 0 }, pz{ wps, 2 };
    const int time = m.node(T::Time, 0, 17);
    const int geoN = m.node(T::NormalWS, 0, 18);
    const int view = m.node(T::ViewDir, 0, 19);
    const int dist = m.node(T::CameraDistance, 0, 20);

    // Fine waves lose their slope with distance: past a point a wavelength is
    // under a pixel and the sine only flickers (there are no mips to filter it).
    const int fadeB = m.un(T::OneMinus, m.op3(T::Smoothstep, m.k(60.0f, 17, 21), m.k(220.0f, 17, 21.5f), dist, 18, 21), 19, 21);
    const int fadeC = m.un(T::OneMinus, m.op3(T::Smoothstep, m.k(25.0f, 17, 22), m.k(110.0f, 17, 22.5f), dist, 18, 22), 19, 22);
    const int fadeR = m.un(T::OneMinus, m.op3(T::Smoothstep, m.k(14.0f, 17, 23), m.k(70.0f, 17, 23.5f), dist, 18, 23), 19, 23);
    const Pin fB{ fadeB }, fC{ fadeC }, fR{ fadeR };

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
    const Wave a  = waveTrain(m, waveA, px, pz, time, kPrimary,    &wA1, nullptr, 26);
    const Wave a2 = waveTrain(m, waveA, px, pz, time, kCompanionA, &wA2, &a,      30);
    const Wave b  = waveTrain(m, waveB, px, pz, time, kPrimary,    &wB1, nullptr, 34);
    const Wave b2 = waveTrain(m, waveB, px, pz, time, kCompanionB, &wB2, &b,      38);
    const Wave c  = waveTrain(m, waveC, px, pz, time, kPrimary,    &wC1, nullptr, 42);
    const Wave c2 = waveTrain(m, waveC, px, pz, time, kCompanionC, &wC2, &c,      46);
    m.comment("Wave A (swell) and its companion", 1, 26, 15, 8);
    m.comment("Wave B and its companion", 1, 34, 15, 8);
    m.comment("Wave C (ripples) and its companion", 1, 42, 15, 8);

    // Chop: two layers of drifting noise slope, one riding each of the two
    // shorter wave knobs' direction. Strength follows the sum of the three
    // steepnesses, so flat waves (w = 0) mean flat water.
    const int sA = m.un(T::SplitRGBA, waveA, 17, 26);
    const int sB = m.un(T::SplitRGBA, waveB, 17, 27);
    const int sC = m.un(T::SplitRGBA, waveC, 17, 28);
    const Pin drive = m.sum({ Pin{ sA, 3 }, Pin{ sB, 3 }, Pin{ sC, 3 } }, 18, 26);
    const int ampBase = m.op(T::Multiply, drive, m.k(0.2f, 20, 26), 21, 26);
    const int amp1    = m.op(T::Multiply, ampBase, fR, 22, 26);
    const int amp2    = m.op(T::Multiply, m.op(T::Multiply, ampBase, m.k(0.55f, 22, 27), 22, 27), fR, 23, 27);
    const Grad r1 = rippleLayer(m, px, pz, time, 2.6f,
                                m.op(T::Multiply, b.dirX, m.op(T::Multiply, b.speed, m.k(0.55f, 24, 29), 25, 29), 26, 29),
                                m.op(T::Multiply, b.dirZ, m.op(T::Multiply, b.speed, m.k(0.55f, 24, 30), 25, 30), 26, 30),
                                amp1, 24, 31);
    const Grad r2 = rippleLayer(m, px, pz, time, 0.9f,
                                m.op(T::Multiply, c.dirX, m.op(T::Multiply, c.speed, m.k(0.9f, 24, 36), 25, 36), 26, 36),
                                m.op(T::Multiply, c.dirZ, m.op(T::Multiply, c.speed, m.k(0.9f, 24, 37), 25, 37), 26, 37),
                                amp2, 24, 38);
    m.comment("Chop (noise slope, finite differences)", 24, 29, 8, 14);

    const Pin slopesX[] = { a.slopeX, a2.slopeX, b.slopeX, b2.slopeX, c.slopeX, c2.slopeX, r1.x, r2.x };
    const Pin slopesZ[] = { a.slopeZ, a2.slopeZ, b.slopeZ, b2.slopeZ, c.slopeZ, c2.slopeZ, r1.z, r2.z };
    const int sx    = m.op(T::Multiply, m.sum(std::vector<Pin>(std::begin(slopesX), std::end(slopesX)), 41, 26), gust, 44, 26);
    const int sz    = m.op(T::Multiply, m.sum(std::vector<Pin>(std::begin(slopesZ), std::end(slopesZ)), 41, 28), gust, 44, 28);
    const int tilt  = m.node(T::Combine3, 45, 27);
    m.link(sx, tilt, 0);
    m.link(sz, tilt, 2);                                     // Y stays 0
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

    // Water body: transmittance exp(-absorption · depth / cos) along the view ray,
    // with the ray bent toward the wave normal by Refraction.
    const int ts    = m.un(T::SplitRGBA, turb, 16, 5);
    const int refrN = m.un(T::Normalize3, m.op3(T::Lerp, geoN, waveN, refr, 21, 5), 22, 5);
    const int ndvR  = m.un(T::Saturate, m.op(T::DotProduct, refrN, view, 23, 5), 24, 5);
    const int path  = m.op(T::Divide, Pin{ ts, 1 }, m.op(T::Add, ndvR, m.k(0.05f, 24, 6), 25, 5), 26, 5);
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
    const int foamAmt = m.op(T::Multiply, m.op(T::Multiply, band, breakup, 30, 18), Pin{ fs, 1 }, 31, 18);
    m.comment("Foam", 16, 16, 16, 8);

    // Output tail. Far water is rougher: what a pixel can no longer resolve is
    // what a rougher surface averages, and it keeps the horizon from turning into
    // a mirror strip of single-pixel glints.
    const int farRough = m.op(T::Multiply, m.op3(T::Smoothstep, m.k(10.0f, 41, 1), m.k(160.0f, 41, 1.5f), dist, 42, 1),
                              m.k(0.25f, 42, 1.5f), 43, 1);
    const int foamTint = m.op(T::Multiply, foamCol, m.op(T::Add, m.k(0.55f, 42, 8), m.op(T::Multiply, lace, m.k(0.7f, 42, 9), 43, 8), 44, 8), 45, 8);
    const int base   = m.op3(T::Lerp, lit, foamTint, foamAmt, 46, 7);
    const int bodyA  = m.op3(T::Lerp, opac, m.k(1.0f, 39, 3), m.un(T::OneMinus, trans, 39, 4), 40, 3);
    const int alpha1 = m.op3(T::Lerp, bodyA, m.k(1.0f, 40, 4.5f), m.op(T::Multiply, refl, fres, 40, 2), 41, 3);
    const int alpha  = m.op3(T::Lerp, alpha1, m.k(1.0f, 41, 4.5f), m.op(T::Multiply, foamAmt, m.k(0.85f, 41, 5), 42, 5), 43, 3);
    const int specO  = m.op(T::Multiply, spec, refl, 41, 0);
    const int roughO = m.op3(T::Lerp, m.op(T::Add, rough, farRough, 44, 2), m.k(0.6f, 43, 2), foamAmt, 45, 2);

    const int out = m.node(T::Output, 48, 4);
    MatGraphNode& o = *m.g.findNode(out);
    o.p[0] = 1.0f;                                                  // lit
    o.p[1] = static_cast<float>(MatBlendMode::Translucent);
    o.p[3] = static_cast<float>(MatDomain::Surface);
    m.link(base,   out, kMatOutputBaseColorPin);
    m.link(specO,  out, kMatOutputSpecularPin);
    m.link(roughO, out, kMatOutputRoughnessPin);
    m.link(alpha,  out, kMatOutputOpacityPin);
    m.link(waveN,  out, kMatOutputNormalPin);
    return m;
}

// The parameters a material promises, in no particular order — the codegen
// numbers slots by first use, not by canvas position.
const std::set<std::string> kWaterParams = {
    "ShallowColor", "DeepColor", "Turbidity", "WaveA", "WaveB", "WaveC", "FresnelPower",
    "Reflection", "Roughness", "Specular", "Opacity", "Refraction", "FoamColor", "Foam",
    "Caustics",
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
    entries.push_back({ "Water", water(), &kWaterParams });

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
