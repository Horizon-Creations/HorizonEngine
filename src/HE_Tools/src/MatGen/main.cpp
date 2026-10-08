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

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
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

// ── One wave train ───────────────────────────────────────────────────────────
// A directional sine, h = (steep / k) · sin(k · (d·p − speed · t) + warp), whose
// slope is analytic: ∂h/∂p = steep · cos(phase) · d. That is why the Wave
// parameter's w is a STEEPNESS (height over wavelength) and not a height: the
// normal only ever sees the slope, so a long swell and a short ripple of the
// same steepness tilt the surface equally.
//
// The warp is FBM over the wave's own frame, scaled to its wavelength, and it
// moves with the wave. It bends and breaks the crests, which is what keeps three
// sines from reading as a woven pattern. Its own slope is left out of the
// normal on purpose: it varies over a wavelength, the sine within one.
//
// Everything is in WORLD units off World Position (x, z), never the mesh UV:
// the engine plane's UVs run 0..1 over the whole scaled plane, so UV waves
// would stretch with it.
struct Wave { Pin slopeX, slopeZ, height, steep; };

Wave waveTrain(Mb& m, int param, Pin px, Pin pz, Pin time, float row)
{
    const int s = m.un(T::SplitRGBA, param, 1, row);
    const Pin dirDeg{ s, 0 }, speed{ s, 1 }, length{ s, 2 }, steep{ s, 3 };

    // Direction in degrees → (cos, sin) on the XZ plane. The library has no
    // Cosine node; Sine(x + π/2) is one.
    const int rad  = m.op(T::Multiply, dirDeg, m.k(kDegToRad, 1, row + 1), 2, row);
    const int sinD = m.un(T::Sine, rad, 3, row);
    const int cosD = m.un(T::Sine, m.op(T::Add, rad, m.k(kHalfPi, 2, row + 1), 3, row + 1), 4, row + 1);

    // Into the wave's frame: `along` runs with the wave, `across` along its crest.
    const int along  = m.op(T::Add, m.op(T::Multiply, cosD, px, 5, row),
                                    m.op(T::Multiply, sinD, pz, 5, row + 0.6f), 6, row);
    const int across = m.op(T::Subtract, m.op(T::Multiply, cosD, pz, 5, row + 1.2f),
                                         m.op(T::Multiply, sinD, px, 5, row + 1.8f), 6, row + 1.2f);
    const int travel = m.op(T::Subtract, along, m.op(T::Multiply, speed, time, 6, row + 2.4f), 7, row);

    const int warpUV = m.op(T::CombineRGBA, travel, across, 8, row + 1);
    const int fbm    = m.op(T::Fbm, warpUV, m.op(T::Divide, m.k(1.0f, 7, row + 2), length, 8, row + 2), 9, row + 1);
    const int warp   = m.op(T::Multiply, fbm, m.k(2.5f, 9, row + 2), 10, row + 1);

    const int kWave = m.op(T::Divide, m.k(kTau, 7, row + 3), length, 8, row + 3);
    const int phase = m.op(T::Add, m.op(T::Multiply, kWave, travel, 9, row), warp, 11, row);
    const int sinP  = m.un(T::Sine, phase, 12, row);
    const int cosP  = m.un(T::Sine, m.op(T::Add, phase, m.k(kHalfPi, 11, row + 1), 12, row + 1), 13, row + 1);

    const int slope = m.op(T::Multiply, steep, cosP, 14, row + 1);
    Wave w;
    w.slopeX = m.op(T::Multiply, slope, cosD, 15, row);
    w.slopeZ = m.op(T::Multiply, slope, sinD, 15, row + 1);
    w.height = m.op(T::Multiply, steep, sinP, 15, row + 2);   // crest finder for the foam
    w.steep  = steep;
    return w;
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
//   * caustics — a moving web on the surface, faded with camera distance (where
//     it would only alias) and with the water's own transmittance.
//
// Assumes a roughly horizontal surface: the wave slope is subtracted from the
// mesh normal in world XZ.
Mb water()
{
    Mb m;

    // Params, column 0, in the order the Inspector should read them.
    const int shallow = m.param(T::ParamColor, "ShallowColor", { 0.10f, 0.42f, 0.45f }, "Color",
        "Water colour where the view ray through the water is short (looking straight down).", 0);
    const int deep = m.param(T::ParamColor, "DeepColor", { 0.01f, 0.07f, 0.12f }, "Color",
        "Water colour where the view ray through the water is long (toward the horizon).", 1);
    const int turb = m.param(T::ParamVec2, "Turbidity", { 0.35f, 3.0f }, "Color",
        "x = absorption per metre (higher = murkier, the deep colour arrives sooner). "
        "y = water depth in metres the tint assumes (no scene depth is read yet).", 2);
    const int waveA = m.param(T::ParamVec4, "WaveA", { 30.0f, 1.2f, 8.0f, 0.25f }, "Waves",
        "Swell. x = direction (degrees, 0 = +X, 90 = +Z), y = speed (m/s), "
        "z = wavelength (m), w = steepness (0 = flat, ~0.4 = choppy).", 3);
    const int waveB = m.param(T::ParamVec4, "WaveB", { 310.0f, 0.8f, 3.5f, 0.18f }, "Waves",
        "Second wave train. x = direction (degrees), y = speed (m/s), "
        "z = wavelength (m), w = steepness.", 4);
    const int waveC = m.param(T::ParamVec4, "WaveC", { 100.0f, 0.5f, 1.2f, 0.12f }, "Waves",
        "Fine ripples. x = direction (degrees), y = speed (m/s), "
        "z = wavelength (m), w = steepness.", 5);
    const int fresPow = m.param(T::ParamFloat, "FresnelPower", { 5.0f }, "Surface",
        "Fresnel exponent: how quickly the surface turns reflective toward grazing angles "
        "(5 = physical water).", 6, 1.0f, 10.0f);
    const int refl = m.param(T::ParamFloat, "Reflection", { 0.8f }, "Surface",
        "How strongly sky and scene reflection cover the water: scales the Fresnel lift of "
        "the opacity and the specular F0.", 7, 0.0f, 1.0f);
    const int rough = m.param(T::ParamFloat, "Roughness", { 0.06f }, "Surface",
        "Surface roughness. Low = sharp reflections and a tight sun glint.", 8, 0.0f, 1.0f);
    const int spec = m.param(T::ParamFloat, "Specular", { 0.3f }, "Surface",
        "Dielectric specular (0.5 = F0 0.04). Multiplied by Reflection; ~0.3 gives water's F0 0.02.",
        9, 0.0f, 1.0f);
    const int opac = m.param(T::ParamFloat, "Opacity", { 0.55f }, "Surface",
        "Opacity looking straight down into clear water. Depth tint, Fresnel and foam raise it.",
        10, 0.0f, 1.0f);
    const int refr = m.param(T::ParamFloat, "Refraction", { 0.3f }, "Surface",
        "How much the waves bend the view into the water: moves the depth tint and the caustics "
        "with the waves (the scene behind is not distorted yet).", 11, 0.0f, 1.0f);
    const int foamCol = m.param(T::ParamColor, "FoamColor", { 0.92f, 0.95f, 0.97f }, "Foam",
        "Foam colour.", 12);
    const int foam = m.param(T::ParamVec4, "Foam", { 0.18f, 0.8f, 1.5f, 0.3f }, "Foam",
        "x = coverage (share of the wave crests that foam, 0 = none), y = strength (0..1), "
        "z = noise size (m), w = drift speed (m/s).", 13);
    const int caus = m.param(T::ParamVec4, "Caustics", { 0.35f, 2.5f, 0.25f, 25.0f }, "Caustics",
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

    // Waves, three bands of rows.
    const Wave a = waveTrain(m, waveA, px, pz, time, 22);
    const Wave b = waveTrain(m, waveB, px, pz, time, 26);
    const Wave c = waveTrain(m, waveC, px, pz, time, 30);
    m.comment("Wave A", 1, 22, 15, 4);
    m.comment("Wave B", 1, 26, 15, 4);
    m.comment("Wave C", 1, 30, 15, 4);

    const int sx    = m.op(T::Add, m.op(T::Add, a.slopeX, b.slopeX, 16, 23), c.slopeX, 17, 23);
    const int sz    = m.op(T::Add, m.op(T::Add, a.slopeZ, b.slopeZ, 16, 24), c.slopeZ, 17, 24);
    const int tilt  = m.node(T::Combine3, 18, 23);
    m.link(sx, tilt, 0);
    m.link(sz, tilt, 2);                                     // Y stays 0
    const int waveN = m.un(T::Normalize3, m.op(T::Subtract, geoN, tilt, 19, 23), 20, 23);

    // Fresnel off the WAVE normal with an exposed exponent. The Fresnel node
    // bakes its power and reads the mesh normal, so it would see neither.
    const int ndv  = m.un(T::Saturate, m.op(T::DotProduct, waveN, view, 21, 2), 22, 2);
    const int fres = m.op(T::Power, m.un(T::OneMinus, ndv, 23, 2), fresPow, 24, 2);
    m.comment("Fresnel", 21, 2, 4, 1.6f);

    // Water body: transmittance exp(-absorption · depth / cos) along the view ray,
    // with the ray bent toward the wave normal by Refraction.
    const int ts    = m.un(T::SplitRGBA, turb, 16, 5);
    const int refrN = m.un(T::Normalize3, m.op3(T::Lerp, geoN, waveN, refr, 21, 5), 22, 5);
    const int ndvR  = m.un(T::Saturate, m.op(T::DotProduct, refrN, view, 23, 5), 24, 5);
    const int path  = m.op(T::Divide, Pin{ ts, 1 }, m.op(T::Add, ndvR, m.k(0.05f, 24, 6), 25, 5), 26, 5);
    const int dens  = m.op(T::Multiply, m.op(T::Multiply, Pin{ ts, 0 }, path, 27, 5), m.k(-1.0f, 27, 6), 28, 5);
    const int trans = m.op(T::Power, m.k(kE, 28, 6), dens, 29, 5);
    const int body  = m.op3(T::Lerp, shallow, deep, m.un(T::OneMinus, trans, 30, 6), 31, 5);
    m.comment("Water body (depth tint)", 16, 5, 16, 2);

    // Caustics: two FBM fields drifting apart; where they agree, a bright line.
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
    const int f1    = m.op(T::Fbm, pan1, invC, 28, 9);
    const int f2    = m.op(T::Fbm, pan2, m.op(T::Multiply, invC, m.k(1.37f, 27, 12), 28, 11), 29, 10);
    const int gap   = m.un(T::Absolute, m.op(T::Subtract, f1, f2, 30, 9), 31, 9);
    const int web   = m.op(T::Power,
                           m.un(T::Saturate, m.un(T::OneMinus, m.op(T::Multiply, gap, m.k(6.0f, 31, 10), 32, 9), 33, 9), 34, 9),
                           m.k(5.0f, 34, 10), 35, 9);
    const int near  = m.un(T::OneMinus, m.op3(T::Smoothstep, m.k(0.0f, 33, 12), Pin{ cs, 3 }, dist, 34, 12), 35, 12);
    const int caust = m.op(T::Multiply, m.op(T::Multiply, m.op(T::Multiply, web, Pin{ cs, 0 }, 36, 9), trans, 37, 9),
                           near, 38, 9);
    const int lit   = m.op(T::Add, body, caust, 39, 7);
    m.comment("Caustics", 16, 9, 23, 6);

    // Foam on the crests: the summed wave height, normalised by the summed
    // steepness to -1..1, past a coverage threshold, broken up by drifting noise.
    const int fs      = m.un(T::SplitRGBA, foam, 16, 16);
    const int sumH    = m.op(T::Add, m.op(T::Add, a.height, b.height, 17, 17), c.height, 18, 17);
    const int sumS    = m.op(T::Add, m.op(T::Add, a.steep, b.steep, 17, 18), c.steep, 18, 18);
    const int crest   = m.op(T::Add, m.op(T::Multiply, m.op(T::Divide, sumH, sumS, 19, 17), m.k(0.5f, 19, 18), 20, 17),
                             m.k(0.5f, 20, 18), 21, 17);
    const int thresh  = m.un(T::OneMinus, Pin{ fs, 0 }, 21, 16);
    const int band    = m.op3(T::Smoothstep, thresh, m.op(T::Add, thresh, m.k(0.12f, 21, 18.5f), 22, 18), crest, 23, 17);
    const int fuv     = m.op3(T::Panner, m.op(T::CombineRGBA, px, pz, 21, 19), Pin{ fs, 3 },
                              m.op(T::Multiply, Pin{ fs, 3 }, m.k(0.5f, 21, 20), 22, 20), 23, 19);
    const int fnoise  = m.op(T::Fbm, fuv, m.op(T::Divide, m.k(1.0f, 23, 20), Pin{ fs, 2 }, 24, 20), 24, 19);
    const int breakup = m.op3(T::Smoothstep, m.k(0.38f, 24, 21), m.k(0.62f, 24, 21.5f), fnoise, 25, 19);
    const int foamAmt = m.op(T::Multiply, m.op(T::Multiply, band, breakup, 26, 18), Pin{ fs, 1 }, 27, 18);
    m.comment("Foam", 16, 16, 12, 6);

    // Output tail.
    const int base   = m.op3(T::Lerp, lit, foamCol, foamAmt, 40, 7);
    const int bodyA  = m.op3(T::Lerp, opac, m.k(1.0f, 39, 3), m.un(T::OneMinus, trans, 39, 4), 40, 3);
    const int alpha1 = m.op3(T::Lerp, bodyA, m.k(1.0f, 40, 4.5f), m.op(T::Multiply, refl, fres, 40, 2), 41, 3);
    const int alpha  = m.op3(T::Lerp, alpha1, m.k(1.0f, 41, 4.5f), foamAmt, 42, 3);
    const int specO  = m.op(T::Multiply, spec, refl, 41, 1);
    const int roughO = m.op3(T::Lerp, rough, m.k(0.6f, 41, 2), foamAmt, 42, 2);

    const int out = m.node(T::Output, 44, 4);
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
            std::printf("  %-8s %3zu nodes  %2zu params  blend %u  slots:",
                        e.name, e.mb.g.nodes.size(), gen.params.size(), unsigned(gen.blendMode));
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
