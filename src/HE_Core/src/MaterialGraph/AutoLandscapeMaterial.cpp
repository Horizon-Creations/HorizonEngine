// Auto landscape material — see AutoLandscapeMaterial.h for what it does and why
// it is a plain graph. This file is only the wiring.
#include <MaterialGraph/AutoLandscapeMaterial.h>
#include <Trails/TrailClipmap.h>
#include <MaterialGraph/ProceduralSurfaceFunctions.h>
#include <MaterialGraph/WeatherMaterialFunctions.h>

namespace HE
{
namespace
{
using T = MatNodeType;

// Thin builder: MaterialGraph with column bookkeeping, so the generated asset
// opens readable in the material editor instead of stacked on the origin.
struct Wire
{
    MaterialGraph& g;
    int row[12] = {};

    int node(T type, int col)
    {
        return g.addNode(type, 260.0f * static_cast<float>(col), 110.0f * static_cast<float>(row[col]++));
    }
    int constF(float v, int col)
    {
        const int n = node(T::ConstFloat, col);
        g.findNode(n)->p[0] = v;
        return n;
    }
    int param(const char* name, float def, float lo, float hi, const char* group, const char* tip)
    {
        const int n = node(T::ParamFloat, 0);
        MatGraphNode* p = g.findNode(n);
        p->s = name;
        p->p[0] = def; p->p[1] = lo; p->p[2] = hi;
        p->group = group;
        p->tooltip = tip;
        return n;
    }
    // op(type, col, {src...}) — input k ← pin 0 of src[k] (or {node, pin} pairs).
    struct Src { int node; int pin = 0; };
    int op(T type, int col, std::initializer_list<Src> in)
    {
        const int n = node(type, col);
        int k = 0;
        for (const Src& s : in) g.connect(s.node, s.pin, n, k++);
        return n;
    }
    // smoothstep(e0, e0 + width, x)
    int ramp(int e0, int width, Src x, int col)
    {
        const int e1 = op(T::Add, col, { { e0 }, { width } });
        return op(T::Smoothstep, col, { { e0 }, { e1 }, x });
    }
};

struct LayerReads { int albedo = 0, normal = 0, mask = 0, height = 0; };

// The three fat arrays of the mix material, in the heTexP0..2 order of the graph.
const char* const kMixArrays[3] = { kAutoLandscapeMixAlbedoArray, kAutoLandscapeMixNormalArray, kAutoLandscapeMixMaskArray };
} // namespace

AutoLandscapeGraph buildAutoLandscapeGraph(AutoLandscapeView view, AutoLandscapeGroupId groupId, bool mixed)
{
    const AutoLandscapeGroup& group = autoLandscapeGroup(groupId);
    AutoLandscapeGraph r;
    Wire w{ r.graph };

    // ── Parameters (column 0) ────────────────────────────────────────────────
    const int pGroundTile = w.param(kAutoLandscapeParamGroundTile, 2.0f, 0.25f, 16.0f, "Tiling",
        "Metres one Grass / Dirt texture covers (world space).");
    const int pRockTile   = w.param(kAutoLandscapeParamRockTile, 4.0f, 0.25f, 32.0f, "Tiling",
        "Metres one Rock / Snow texture covers (world space).");
    const int pCell       = w.param(kAutoLandscapeParamBombCell, 0.5f, 0.1f, 2.0f, "Tiling",
        "Texture bombing: spacing of the random hexes, in texture repeats. Smaller = more variation, more seams.");
    const int pRockSlope  = w.param(kAutoLandscapeParamRockSlope, group.rockSlope, 0.0f, 1.0f, "Rock",
        "Slope (1 - normal.y) where rock starts. 0.12 ~ 28 degrees, 0.29 ~ 45 degrees.");
    const int pRockBlend  = w.param(kAutoLandscapeParamRockBlend, 0.12f, 0.01f, 0.5f, "Rock",
        "Slope range over which ground turns into rock.");
    const int pHeight     = w.param(kAutoLandscapeParamHeightBlend, 1.0f, 0.0f, 4.0f, "Rock",
        "How much the layers' height maps bend every transition (0 = plain smooth blends).");
    const int pDirtAmt    = w.param(kAutoLandscapeParamDirtAmount, group.dirtAmount, 0.0f, 1.0f, "Ground",
        "Share of the flat ground covered by dirt patches instead of grass.");
    const int pDirtSize   = w.param(kAutoLandscapeParamDirtSize, 24.0f, 1.0f, 200.0f, "Ground",
        "Size of the dirt patches, metres.");
    const int pSnowH      = w.param(kAutoLandscapeParamSnowHeight, group.snowHeight, -500.0f, 3000.0f, "Snow",
        "World height (Y, metres) where snow starts.");
    const int pSnowBlend  = w.param(kAutoLandscapeParamSnowBlend, 6.0f, 0.1f, 100.0f, "Snow",
        "Height range over which the snow cover closes, metres.");
    const int pSnowSlope  = w.param(kAutoLandscapeParamSnowMaxSlope, 0.45f, 0.0f, 1.0f, "Snow",
        "Steepest slope that holds snow; steeper faces stay rock.");
    const int pPudAmt     = w.param(kAutoLandscapeParamPuddleAmount, 0.5f, 0.0f, 1.0f, "Puddles",
        "Water level in the puddle noise field when the weather's Puddles slider is at 1 (the slider scales it): "
        "0.32 = scattered puddles (~1/5 of the flat ground), ~0.5 = half.");
    const int pPudSlope   = w.param(kAutoLandscapeParamPuddleMaxSlope, 0.08f, 0.002f, 0.4f, "Puddles",
        "Slope (1 - normal.y) where automatic standing water has faded out completely; it starts to thin "
        "out from a fifth of that, and the damp ground beside a puddle fades out at twice it. "
        "0.08 ~ 23 degrees. Painted puddles ignore it.");
    // The procedural snow / sand (MF_ProceduralSnow / MF_ProceduralSand). The defaults are the
    // functions' own, the colours the references': see where they are wired below.
    const int pSnowColor = w.node(T::ParamColor, 0);
    {
        MatGraphNode* c = r.graph.findNode(pSnowColor);
        c->s = kAutoLandscapeParamSnowColor;
        c->p[0] = 0.82f; c->p[1] = 0.85f; c->p[2] = 0.90f;
        c->group = "Snow";
        c->tooltip = "Colour of the procedural snow (linear).";
    }
    const int pSnowLook = w.node(T::ParamVec4, 0);
    {
        MatGraphNode* c = r.graph.findNode(pSnowLook);
        c->s = kAutoLandscapeParamSnowLook;
        c->p[0] = 0.5f; c->p[1] = 0.5f; c->p[2] = 0.35f; c->p[3] = 0.5f;
        c->group = "Snow";
        c->tooltip = "Procedural snow, x..w: Grain (0..1, how coarse the crystals read), Drift (0..1, wind-blown "
                     "ridges), Glitter (0..1, sparkle in the sun), Softness (0..1, how deep and rounded the surface is).";
    }
    int pSandLook = -1;
    if (group.proceduralSand || mixed)
    {
        pSandLook = w.node(T::ParamVec4, 0);
        MatGraphNode* c = r.graph.findNode(pSandLook);
        c->s = kAutoLandscapeParamSandLook;
        c->p[0] = 0.03f; c->p[1] = 0.5f; c->p[2] = 0.3f; c->p[3] = 0.3f;
        c->group = "Sand";
        c->tooltip = "Procedural sand, x..w: Grain Size (metres), Ripple (0..1, how pronounced the wind ripples "
                     "are), Ripple Size (metres from crest to crest), Glitter (0..1).";
    }

    // ── Coordinates and slope (column 1) ─────────────────────────────────────
    const int wpos   = w.node(T::WorldPos, 1);
    const int wsplit = w.op(T::SplitRGBA, 1, { { wpos } });
    const int zero   = w.constF(0.0f, 1);
    const int one    = w.constF(1.0f, 1);
    const int xz     = w.op(T::CombineRGBA, 1, { { wsplit, 0 }, { wsplit, 2 }, { zero }, { zero } });
    const int uvGround = w.op(T::Divide, 1, { { xz }, { pGroundTile } });
    const int uvRock   = w.op(T::Divide, 1, { { xz }, { pRockTile } });
    const int nrm    = w.node(T::NormalWS, 1);
    const int nsplit = w.op(T::SplitRGBA, 1, { { nrm } });
    r.slope = w.op(T::Subtract, 1, { { one }, { nsplit, 1 } });
    // The weather, read once: Wetness (pin 0) and Snow Cover (pin 3) drive the procedural surfaces.
    const int weather = w.node(T::Weather, 1);

    // ── Texture reads (column 2): albedo / normal / mask per layer ──────────
    // A bombed layer reads all three maps on ONE hex grid: same uv node, same
    // Cell node, same rotation/sharpness/seed → codegen emits the grid once.
    //
    // Groups = the four painted layers: the arrays hold 4 * V + 1 slices (V variants per layer,
    // then WetGround), V = depth / 4 in the shader. Each hex of a bombed read draws its own
    // variant; a 5-slice array (V = 1) reads exactly the slices it always did.
    // The mix material reads 16 slices (group * 4 + role): the Groups pin is 16, so V = depth / 16.
    const int groups = w.constF(static_cast<float>(mixed ? kAutoLandscapeMixDepth : kAutoLandscapeRoleCount), 2);
    // Which two groups a fragment mixes, and A's weight in the pair (mix material only).
    int pick = -1, pickMix = -1, four = -1;   // pick: pins 0 (A), 1 (B), 2 (Mix)
    if (mixed)
    {
        pick    = w.node(T::LandscapeGroupPick, 2);
        pickMix = pick;
        four    = w.constF(static_cast<float>(kAutoLandscapeRoleCount), 2);
    }
    const char* const* arrays = mixed ? kMixArrays : group.arrayPaths;
    // One bombed read of role `L` at array slice `slice` (a node): the role's constant in a group's
    // own material, role + 4 * group in the mix material.
    auto layerAt = [&](int slice, int uv, int seed) {
        LayerReads lr;
        auto read = [&](T plain, T bombed, const char* path) {
            const int p = w.node(plain, 2);
            r.graph.findNode(p)->s = path;
            r.graph.connect(uv, 0, p, 0);
            r.graph.connect(slice, 0, p, 1);
            r.graph.connect(groups, 0, p, 2); // the unbombed debug read shows variant 0 of the layer
            if (seed < 0) return p;
            const int b = w.node(bombed, 2);
            MatGraphNode* bn = r.graph.findNode(b);
            bn->s = path;
            bn->p[2] = static_cast<float>(seed);
            r.graph.connect(uv, 0, b, 0);
            r.graph.connect(slice, 0, b, 1);
            r.graph.connect(pCell, 0, b, 2);
            r.graph.connect(groups, 0, b, 3);
            // Compile-time choice: the untaken read is never emitted.
            const int sw = w.op(T::StaticSwitch, 2, { { b }, { p } });
            r.graph.findNode(sw)->s = kAutoLandscapeSwitchBombing;
            r.graph.findNode(sw)->p[0] = 1.0f;
            return sw;
        };
        lr.albedo = read(T::TextureArraySample,   T::TextureArrayBombSample,   arrays[0]);
        lr.normal = read(T::NormalMapArraySample, T::NormalMapArrayBombSample, arrays[1]);
        lr.mask   = read(T::TextureArraySample,   T::TextureArrayBombSample,   arrays[2]);
        lr.height = w.op(T::SplitRGBA, 2, { { lr.mask } }); // pin 2 = B = height
        return lr;
    };
    auto layer = [&](AutoLandscapeLayer L, int uv, int seed) {
        const int role = w.constF(static_cast<float>(static_cast<int>(L)), 2);
        if (!mixed) return layerAt(role, uv, seed);
        // Two groups' slices on the same hex grid, blended by A's weight: only the slice differs, so
        // the grid is emitted once and the extra cost is the second set of taps and a lerp.
        auto sliceOf = [&](int groupPin) {
            return w.op(T::Add, 2, { { role }, { w.op(T::Multiply, 2, { { pick, groupPin }, { four } }) } });
        };
        const LayerReads a = layerAt(sliceOf(0), uv, seed), b = layerAt(sliceOf(1), uv, seed);
        LayerReads lr;
        lr.albedo = w.op(T::Lerp, 2, { { b.albedo }, { a.albedo }, { pickMix, 2 } });
        lr.normal = w.op(T::Lerp, 2, { { b.normal }, { a.normal }, { pickMix, 2 } });
        lr.mask   = w.op(T::Lerp, 2, { { b.mask },   { a.mask },   { pickMix, 2 } });
        lr.height = w.op(T::SplitRGBA, 2, { { lr.mask } });
        return lr;
    };
    // A surface the procedural functions make, as the three channel sets the blends take: albedo and
    // normal through a Reroute (a node with ONE output pin, which `blend` and the layer mix wire by
    // node id), the mask as (AO, roughness, height) — the arrays' mask layout, so the height blends
    // read it exactly like a texture's B channel.
    auto surfaceOf = [&](int call, int col) {
        LayerReads lr;
        lr.albedo = w.op(T::Reroute, col, { { call, kSurfaceOutAlbedo } });
        lr.normal = w.op(T::Reroute, col, { { call, kSurfaceOutNormal } });
        lr.mask   = w.op(T::Combine3, col, { { call, kSurfaceOutAO }, { call, kSurfaceOutRoughness },
                                             { call, kSurfaceOutHeight } });
        lr.height = w.op(T::SplitRGBA, col, { { lr.mask } }); // pin 2 = B = height
        return lr;
    };
    auto callFunction = [&](const char* path, int col) {
        const int call = w.node(T::FunctionCall, col);
        r.graph.findNode(call)->s = path;
        return call;
    };
    // ── Trails (Thema 328 step 5b): the depth and slope of whatever pressed the snow or sand ──────
    // TrailClipmap's texture (kTrailTexturePath, the fourth graph texture): a 128 m window around
    // the camera, world-aligned and wrapping, so the uv is simply xz / 128 and the sampler repeats.
    // Decoding mirrors TrailClipmap.h: depth = max((128/255 - R) * 255/127, 0), slope = (G|B - 128/255) * (255 / 127 * 8), and
    // only a texel whose alpha is about one half is trusted: a missing texture (a white or black
    // stand-in) must never read as a deep print. Beyond kTrailFadeStart metres the window no longer
    // belongs to this place (it wraps), so the trail fades out before it ends.
    int trailDepth, trailSlope, trailHeap;
    {
        const int invWindow = w.constF(1.0f / 128.0f, 2);
        const int uvTrail   = w.op(T::Multiply, 2, { { xz }, { invWindow } });
        const int tex       = w.op(T::TextureSample, 2, { { uvTrail } });
        r.graph.findNode(tex)->s = kTrailTexturePath;
        const int rgb  = w.op(T::SplitRGBA, 2, { { tex } });
        const int lo   = w.constF(0.45f, 2);
        const int hi   = w.constF(0.55f, 2);
        const int stepLo = w.op(T::Step, 2, { { lo }, { tex, 1 } });
        const int stepHi = w.op(T::Step, 2, { { hi }, { tex, 1 } });
        const int below  = w.op(T::OneMinus, 2, { { stepHi } });
        const int valid  = w.op(T::Multiply, 2, { { stepLo }, { below } });
        const int dist   = w.node(T::CameraDistance, 2);
        const int fadeS  = w.constF(kTrailFadeStart, 2);
        const int fadeE  = w.constF(kTrailFadeEnd, 2);
        const int far    = w.op(T::Smoothstep, 2, { { fadeS }, { fadeE }, { dist } });
        const int near   = w.op(T::OneMinus, 2, { { far } });
        const int gate   = w.op(T::Multiply, 2, { { valid }, { near } });
        // R is the field's signed value: 128 untouched, below a pit, above a heap. A pit is the Disturbance
        // (compacted, darker); a heap only shows in the slope below (it lights as a ridge).
        const int mid    = w.constF(128.0f / 255.0f, 2);
        const int unitK  = w.constF(255.0f / 127.0f, 2);
        const int pit0   = w.op(T::Subtract, 2, { { mid }, { rgb, 0 } });
        const int pit1   = w.op(T::Multiply, 2, { { pit0 }, { unitK } });
        const int depth  = w.op(T::Saturate, 2, { { pit1 } });
        trailDepth       = w.op(T::Multiply, 2, { { depth }, { gate } });
        const int heap0  = w.op(T::Subtract, 2, { { rgb, 0 }, { mid } });
        const int heap1  = w.op(T::Multiply, 2, { { heap0 }, { unitK } });
        const int heapS  = w.op(T::Saturate, 2, { { heap1 } });
        trailHeap        = w.op(T::Multiply, 2, { { heapS }, { gate } });
        const int slopeK = w.constF(255.0f / 127.0f * TrailClipmap::kSlopeRange, 2);
        const int gx0    = w.op(T::Subtract, 2, { { rgb, 1 }, { mid } });
        const int gx1    = w.op(T::Multiply, 2, { { gx0 }, { slopeK } });
        const int gx     = w.op(T::Multiply, 2, { { gx1 }, { gate } });
        const int gz0    = w.op(T::Subtract, 2, { { rgb, 2 }, { mid } });
        const int gz1    = w.op(T::Multiply, 2, { { gz0 }, { slopeK } });
        const int gz     = w.op(T::Multiply, 2, { { gz1 }, { gate } });
        const int zeroC  = w.constF(0.0f, 2);
        trailSlope       = w.op(T::Combine3, 2, { { gx }, { gz }, { zeroC } });
    }

    // Role 0, the ground: the group's own texture, or in the Desert the procedural sand. Its colour
    // is the mean of the human's reference (coast_sand_01, 4k diffuse: sRGB 129/114/91); the grain,
    // the ripples and the glitter keep the function's defaults. The weather's wetness darkens it
    // (the generic wet response of the lit shader comes on top, sand just darkens more).
    LayerReads grass;
    // The sand as a surface (Desert's role 0, and the Desert's share of the mix material).
    auto makeSand = [&]() {
        const int sand = callFunction(kProceduralSandFunctionPath, 2);
        const int sandColor = w.node(T::ConstColor, 2);
        {
            MatGraphNode* c = r.graph.findNode(sandColor);
            c->p[0] = 0.221f; c->p[1] = 0.168f; c->p[2] = 0.104f;
        }
        const int look = w.op(T::SplitRGBA, 2, { { pSandLook } });
        r.graph.connect(xz, 0, sand, kSandInPosition);
        r.graph.connect(sandColor, 0, sand, kSandInColor);
        r.graph.connect(look, 0, sand, kSandInGrainSize);
        r.graph.connect(look, 1, sand, kSandInRipple);
        r.graph.connect(look, 2, sand, kSandInRippleSize);
        r.graph.connect(look, 3, sand, kSandInGlitter);
        r.graph.connect(weather, 0, sand, kSandInWetness);
        r.graph.connect(trailDepth, 0, sand, kSandInDisturbance);
        r.graph.connect(trailSlope, 0, sand, kSandInDisturbanceSlope);
        r.graph.connect(trailHeap, 0, sand, kSandInDisturbanceHeap);
        return surfaceOf(sand, 2);
    };
    if (mixed)
    {
        // Role 0 of a mix: each of the two groups' ground is its array slice, or the procedural sand
        // where that group is the Desert, then the pair blends by A's weight. Both are always
        // evaluated (no branch): the Desert's sand costs every fragment of a mixed terrain.
        const LayerReads sand  = makeSand();
        // Per side (A, B): the group's slice, or the sand where that group is the Desert.
        const int role0 = w.constF(0.0f, 2);
        auto sideOf = [&](int groupPin) {
            const int slice = w.op(T::Add, 2, { { role0 }, { w.op(T::Multiply, 2, { { pick, groupPin }, { four } }) } });
            const LayerReads rd = layerAt(slice, uvGround, 11);
            const int isDesert = w.op(T::Greater, 2, { { pick, groupPin }, { w.constF(2.5f, 2) } });
            LayerReads o;
            o.albedo = w.op(T::Lerp, 2, { { rd.albedo }, { sand.albedo }, { isDesert } });
            o.normal = w.op(T::Lerp, 2, { { rd.normal }, { sand.normal }, { isDesert } });
            o.mask   = w.op(T::Lerp, 2, { { rd.mask },   { sand.mask },   { isDesert } });
            return o;
        };
        const LayerReads ga = sideOf(0), gb = sideOf(1);
        grass.albedo = w.op(T::Lerp, 2, { { gb.albedo }, { ga.albedo }, { pickMix, 2 } });
        grass.normal = w.op(T::Lerp, 2, { { gb.normal }, { ga.normal }, { pickMix, 2 } });
        grass.mask   = w.op(T::Lerp, 2, { { gb.mask },   { ga.mask },   { pickMix, 2 } });
        grass.height = w.op(T::SplitRGBA, 2, { { grass.mask } });
    }
    else if (group.proceduralSand)
        grass = makeSand();
    else
        grass = layer(AutoLandscapeLayer::Grass, uvGround, 11);
    LayerReads       dirt  = layer(AutoLandscapeLayer::Dirt,      uvGround, 23);
    LayerReads       rock  = layer(AutoLandscapeLayer::Rock,      uvRock,   37);
    // Snow is MF_ProceduralSnow, in every group: noise in world space has no tile and no hex seam to
    // hide, and nothing to read. Cover is the weather's Snow Cover slider, Melt its wetness (rain
    // melts it); it takes no gate (see below). The Weather node, not MF_WeatherSnow's Cover output,
    // because that call takes the snow's own height bias as an input further down.
    LayerReads snow;
    {
        const int call = callFunction(kProceduralSnowFunctionPath, 2);
        const int look = w.op(T::SplitRGBA, 2, { { pSnowLook } });
        r.graph.connect(xz, 0, call, kSnowInPosition);
        r.graph.connect(weather, 3, call, kSnowInCover);
        r.graph.connect(pSnowColor, 0, call, kSnowInColor);
        r.graph.connect(look, 0, call, kSnowInGrain);
        r.graph.connect(look, 1, call, kSnowInDrift);
        r.graph.connect(look, 2, call, kSnowInGlitter);
        r.graph.connect(look, 3, call, kSnowInSoftness);
        r.graph.connect(weather, 0, call, kSnowInMelt);
        r.graph.connect(trailDepth, 0, call, kSnowInDisturbance);
        r.graph.connect(trailSlope, 0, call, kSnowInDisturbanceSlope);
        r.graph.connect(trailHeap, 0, call, kSnowInDisturbanceHeap);
        snow = surfaceOf(call, 2);
    }
    // Slice 4 of the arrays (WetGround) is not read: puddles are an overlay on
    // the ground below, not a layer of their own.

    // (hA − hB) × Height Blend × scale — the height bias of a transition.
    auto heightBias = [&](int hA, int pinA, int hB, int pinB, int scale, int col) {
        const int d = w.op(T::Subtract, col, { { hA, pinA }, { hB, pinB } });
        const int m = w.op(T::Multiply, col, { { d }, { pHeight } });
        return w.op(T::Multiply, col, { { m }, { scale } });
    };
    // One blend stage over the three channels.
    struct Surface { int albedo, normal, mask; };
    auto blend = [&](const Surface& a, const LayerReads& b, int t, int col) {
        return Surface{ w.op(T::Lerp, col, { { a.albedo }, { b.albedo }, { t } }),
                        w.op(T::Lerp, col, { { a.normal }, { b.normal }, { t } }),
                        w.op(T::Lerp, col, { { a.mask },   { b.mask },   { t } }) };
    };

    // ── Painted weights (column 6, read here: the layer gates below need them) ─
    // The landscape's painted weightmap, read through the engine's own Landscape Layer
    // Blend node fed unit vectors: input k = (1,0,0) / (0,1,0) / (0,0,1) makes channel
    // x / y / z of the blended result BE layer k's weight. Two nodes carry the five
    // painted layers; a texel that sums to nothing falls back to input 0 = (0,0,0) =
    // "no paint", i.e. fully automatic. (What the weights do: column 6 below.)
    const int none = w.node(T::ConstColor, 6);
    { MatGraphNode* c = r.graph.findNode(none); c->p[0] = c->p[1] = c->p[2] = 0.0f; }
    auto unit = [&](int axis) {
        const int n = w.node(T::ConstColor, 6);
        MatGraphNode* c = r.graph.findNode(n);
        c->p[0] = c->p[1] = c->p[2] = 0.0f;
        c->p[axis] = 1.0f;
        return n;
    };
    const int ux = unit(0), uy = unit(1), uz = unit(2);
    // One source per layer, in kAutoLandscapePaintLayerNames order.
    auto layerBlend = [&](std::initializer_list<int> perLayer, int col) {
        const int n = w.node(T::LandscapeLayerBlend, col);
        r.graph.findNode(n)->s = group.paintLayerNames;
        int pin = 0;
        for (int src : perLayer) r.graph.connect(src, 0, n, pin++);
        return n;
    };
    //                                     Auto  Grass Dirt  Rock  Snow  Puddles
    const int wA = layerBlend({ none, ux, uy, uz, none, none }, 6);  // (grass, dirt, rock)
    const int wB = layerBlend({ none, none, none, none, ux, uy }, 6); // (snow, puddles, -)
    const int wASplit = w.op(T::SplitRGBA, 6, { { wA } });
    const int wBSplit = w.op(T::SplitRGBA, 6, { { wB } });
    const int mG = wASplit, mD = wASplit, mR = wASplit, mS = wBSplit, mP = wBSplit;

    // ── Layer gates: read a layer only where it can have weight ──────────────
    // Each of Dirt, Rock and Snow sits behind an If whose condition is an UPPER
    // bound of its automatic weight: the ramp's input with the height bias at its
    // largest (|hA - hB| <= 1, so the bias is at most Height Blend x scale, the
    // same product in the same order as heightBias), or a painted weight. Where the
    // condition fails the weight is exactly 0 whatever the layer's texels hold, so
    // reading zeros instead changes no pixel; the If is a real branch, so the 9
    // bombed taps of the layer (albedo, normal, mask x 3 hexes) are not run there.
    // Grass stays ungated: it is the ground everything else lies on. The mask views
    // are not gated: they are the automatic masks alone and read no weightmap. Snow has no gate
    // either: it is a function call, which the code generator never moves into a branch (a
    // function body is not branch-pure), so an If around it would only be a mix().
    const bool gated = view == AutoLandscapeView::Lit || view == AutoLandscapeView::Normal
                    || view == AutoLandscapeView::Surface;
    auto gate = [&](LayerReads& lr, int cond, int col) {
        if (!gated) return;
        const int zeroC = w.node(T::ConstColor, col);
        { MatGraphNode* c = r.graph.findNode(zeroC); c->p[0] = c->p[1] = c->p[2] = 0.0f; }
        lr.albedo = w.op(T::If, col, { { cond }, { lr.albedo }, { zeroC } });
        lr.normal = w.op(T::If, col, { { cond }, { lr.normal }, { zeroC } });
        lr.mask   = w.op(T::If, col, { { cond }, { lr.mask },   { zeroC } });
        lr.height = w.op(T::SplitRGBA, col, { { lr.mask } });
    };
    auto possible = [&](Wire::Src x, int biasMax, int e0, int paint, int paintPin, int col) {
        const int upper = w.op(T::Add, col, { x, { biasMax } });
        const int can   = w.op(T::Greater, col, { { upper }, { e0 } });
        const int drawn = w.op(T::Greater, col, { { paint, paintPin }, { zero } });
        return w.op(T::Or, col, { { can }, { drawn } });
    };

    // ── Ground: grass, dirt patches, dirt belt below the rock (column 3) ─────
    const int invDirt  = w.op(T::Divide, 3, { { one }, { pDirtSize } });
    const int dirtFbm  = w.op(T::Fbm, 3, { { xz }, { invDirt } });
    // Integer hash: the float heHash21 differs per driver at world-space lattice
    // indices (blocky patches on D3D12/Vulkan, Thema 158 Schritt 7/8).
    r.graph.findNode(dirtFbm)->p[0] = 1.0f;
    const int tenth    = w.constF(0.1f, 3);
    // fBm sits around 0.5 (range ~0.1..0.85): edge = 0.75 − 0.5 × amount.
    const int half     = w.constF(0.5f, 3);
    const int amtHalf  = w.op(T::Multiply, 3, { { pDirtAmt }, { half } });
    const int edgeBase = w.constF(0.75f, 3);
    const int dirtE0   = w.op(T::Subtract, 3, { { edgeBase }, { amtHalf } });
    const int beltE0   = w.op(T::Subtract, 3, { { pRockSlope }, { pRockBlend } });
    {
        // Dirt: the noise patches (bias scale 0.1), the belt below the rock (no bias), or paint.
        const int noiseCan = possible({ dirtFbm }, w.op(T::Multiply, 3, { { pHeight }, { tenth } }), dirtE0, mD, 1, 3);
        const int beltCan  = w.op(T::Greater, 3, { { r.slope }, { beltE0 } });
        gate(dirt, w.op(T::Or, 3, { { noiseCan }, { beltCan } }), 3);
    }
    const int dirtT    = w.op(T::Add, 3, { { dirtFbm }, { heightBias(dirt.height, 2, grass.height, 2, tenth, 3) } });
    const int dirtW    = w.constF(0.06f, 3);
    const int dirtNoise = w.ramp(dirtE0, dirtW, { dirtT }, 3);
    const int dirtBelt = w.ramp(beltE0, pRockBlend, { r.slope }, 3);
    const int dirtSum  = w.op(T::Add, 3, { { dirtNoise }, { dirtBelt } });
    r.dirtMask = w.op(T::Saturate, 3, { { dirtSum } });
    const Surface ground = blend({ grass.albedo, grass.normal, grass.mask }, dirt, r.dirtMask, 3);
    const int groundSplit = w.op(T::SplitRGBA, 3, { { ground.mask } });

    // ── Rock on the slopes (column 4) ────────────────────────────────────────
    gate(rock, possible({ r.slope }, w.op(T::Multiply, 4, { { pHeight }, { pRockBlend } }), pRockSlope, mR, 2, 4), 4);
    const int rockT = w.op(T::Add, 4, { { r.slope },
                                        { heightBias(rock.height, 2, groundSplit, 2, pRockBlend, 4) } });
    r.rockMask = w.ramp(pRockSlope, pRockBlend, { rockT }, 4);
    const Surface s1 = blend(ground, rock, r.rockMask, 4);
    const int s1Split = w.op(T::SplitRGBA, 4, { { s1.mask } });

    // ── Snow above a height, not on cliffs (column 5) ────────────────────────
    const int snowT  = w.op(T::Add, 5, { { wsplit, 1 },
                                         { heightBias(snow.height, 2, s1Split, 2, pSnowBlend, 5) } });
    const int snowHi = w.ramp(pSnowH, pSnowBlend, { snowT }, 5);
    const int cliffW = w.constF(0.1f, 5);
    const int cliff  = w.ramp(pSnowSlope, cliffW, { r.slope }, 5);
    const int holds  = w.op(T::OneMinus, 5, { { cliff } });
    r.snowMask = w.op(T::Multiply, 5, { { snowHi }, { holds } });
    // The weather's snow on top of the height snow: MF_WeatherSnow turns the Weather
    // details panel's Snow Cover slider into a coverage by slope (cliffs stay bare) and by
    // the layers' height maps (snow settles in the snow texture's high texels first). The
    // two add as probabilities, a + b - a*b, so neither can push the other past 1.
    // r.snowMask stays the HEIGHT snow alone — the mask views and the witness read it, and
    // they must not depend on the scene's weather. The snow surface's Height output feeds its
    // bias (snowT) and is the one output of MF_ProceduralSnow that reads no weather input.
    const int snowCall = w.node(T::FunctionCall, 5);
    r.graph.findNode(snowCall)->s = kWeatherSnowFunctionPath;
    r.graph.connect(r.slope, 0, snowCall, 0);                       // Slope
    r.graph.connect(pSnowSlope, 0, snowCall, 1);                    // Max Slope
    r.graph.connect(heightBias(snow.height, 2, s1Split, 2, one, 5), 0, snowCall, 2);  // Height Bias
    const int weatherSnow = snowCall;                               // pin 0 = Snow
    const int snowBoth = w.op(T::Multiply, 5, { { r.snowMask }, { weatherSnow, 0 } });
    const int snowSum  = w.op(T::Add, 5, { { r.snowMask }, { weatherSnow, 0 } });
    const int snowAny  = w.op(T::Subtract, 5, { { snowSum }, { snowBoth } });
    const Surface s2 = blend(s1, snow, snowAny, 5);

    // ── Painted layers (column 6) ────────────────────────────────────────────
    // Everything above is the AUTOMATIC distribution (s2). The landscape's painted
    // weightmap sits on top of it: layer 0 "Auto" is what an unpainted terrain reads
    // (the engine's 1x1 (1,0,0,0) default), so a fresh landscape looks exactly as
    // before, and painting Grass / Dirt / Rock / Snow / Puddles moves weight from
    // Auto to that layer — painting Auto again hands the spot back to the automatic
    // rules. The weights of a texel sum to 1, so the final surface is
    //     Auto x s2 + Grass x grass + Dirt x dirt + Rock x rock + Snow x snow
    // and Puddles is not a surface but the strength of the water overlay below.
    //
    // The weights are read through the engine's own Landscape Layer Blend node, fed
    // unit vectors: input k = (1,0,0) / (0,1,0) / (0,0,1) makes channel x / y / z of
    // the blended result BE layer k's weight. Two nodes carry the five painted
    // layers; a texel that sums to nothing falls back to input 0 = (0,0,0) = "no
    // paint", i.e. fully automatic.
    const int sumGD  = w.op(T::Add, 6, { { mG, 0 }, { mD, 1 } });
    const int sumRS  = w.op(T::Add, 6, { { mR, 2 }, { mS, 0 } });
    const int sumAll = w.op(T::Add, 6, { { sumGD }, { sumRS } });
    // How much of the AUTOMATIC PUDDLES survives painted ground. A puddle is a thin
    // wet/glossy film, and a film left at half strength over ground the user has painted
    // over reads as a leftover sheen, not as "half painted". So it dies three times as fast
    // as the surface under it is replaced: a third of the way painted over, the automatic
    // puddle is gone. (The surface itself keeps the plain weights, see the blends below.)
    const int three     = w.constF(3.0f, 6);
    const int groundHit = w.op(T::Saturate, 6, { { w.op(T::Multiply, 6, { { sumAll }, { three } }) } });
    const int puddleKeep = w.op(T::OneMinus, 6, { { groundHit } });

    // The surface the painted weights make of it: one blend per channel set. The
    // Puddles input is s2 again — puddles change what lies on the ground, not the
    // ground, so their share keeps the automatic surface underneath.
    const int mixAlbedo = layerBlend({ s2.albedo, grass.albedo, dirt.albedo, rock.albedo,
                                       snow.albedo, s2.albedo }, 6);
    const int mixNormal = layerBlend({ s2.normal, grass.normal, dirt.normal, rock.normal,
                                       snow.normal, s2.normal }, 6);
    const int mixMask   = layerBlend({ s2.mask, grass.mask, dirt.mask, rock.mask,
                                       snow.mask, s2.mask }, 6);
    const Surface s3{ mixAlbedo, mixNormal, mixMask };

    // ── Puddles: flat ground in the minima of a noise field (column 7) ───────
    // Water thins out over a WIDE slope band (a fifth of the limit up to the limit),
    // not over a hair of it: with the band at 0.5..1 x the limit a puddle ended on a
    // hard line wherever the ground tilted past it and read as cut off at every bank.
    // That is only the AUTOMATIC water; a painted puddle ignores the slope.
    const int fifth    = w.constF(0.2f, 7);
    const int fadeFrom = w.op(T::Multiply, 7, { { pPudSlope }, { fifth } });
    const int steep    = w.op(T::Smoothstep, 7, { { fadeFrom }, { pPudSlope }, { r.slope } });
    const int noSnow   = w.op(T::OneMinus, 7, { { r.snowMask } });
    const int flat     = w.op(T::OneMinus, 7, { { steep } });
    r.flatMask = w.op(T::Multiply, 7, { { flat }, { noSnow } });
    // The water level AND the hollow size come from the WEATHER: MF_WeatherPuddles turns the Weather details
    // panel's Puddles slider (0..1) into a fraction of "Puddle Amount", the level at
    // slider 1; "Size" is the panel's Puddle Size in metres. Slider 0 = no hollow reaches the surface = no automatic puddles at all.
    const int puddleCall = w.node(T::FunctionCall, 7);
    r.graph.findNode(puddleCall)->s = kWeatherPuddlesFunctionPath;
    r.graph.connect(pPudAmt, 0, puddleCall, 0);                      // Max Water Level
    // A second field, decorrelated from the dirt patches by an offset.
    const int offs   = w.node(T::ConstVec2, 7);
    r.graph.findNode(offs)->p[0] = 173.1f;
    r.graph.findNode(offs)->p[1] = 419.7f;
    const int xzOff  = w.op(T::Add, 7, { { xz }, { offs } });
    // Hollow size in metres from the weather (MF_WeatherPuddles "Size", the panel's Puddle Size).
    const int invPud = w.op(T::Divide, 7, { { one }, { puddleCall, 3 } });
    const int basin  = w.op(T::Fbm, 7, { { xzOff }, { invPud } });
    r.graph.findNode(basin)->p[0] = 1.0f; // integer hash, see dirtFbm
    const int depth  = w.op(T::Subtract, 7, { { puddleCall, 0 }, { basin } });  // > 0 inside a hollow
    // Wet rim: from 0.06 below the water line up to it. The damp film is NOT cut at the
    // water's own slope limit: the ground beside a puddle stays damp up the bank, and it
    // fades over a band wider than the water's ("Puddle Max Slope" x 0.2 up to
    // x 2). It is the same hollow-shaped patch, so its edge follows the noise, and the
    // step from dark water to dry ground is a dark -> damp -> dry gradient, not one line.
    const int rimW   = w.constF(0.06f, 7);
    const int negRim = w.constF(-0.06f, 7);
    const int rim    = w.ramp(negRim, rimW, { depth }, 7);
    const int twoL      = w.op(T::Multiply, 7, { { pPudSlope }, { w.constF(2.0f, 7) } });
    const int dampSteep = w.op(T::Smoothstep, 7, { { fadeFrom }, { twoL }, { r.slope } });
    const int dampHolds = w.op(T::OneMinus, 7, { { dampSteep } });
    r.wetMask = w.op(T::Multiply, 7, { { w.op(T::Multiply, 7, { { rim }, { dampHolds } }) }, { noSnow } });
    // Water fills the low texels of the ground below first (its height map, the
    // blended B channel of everything under the puddle). The AUTOMATIC surface s2
    // is used here, not the painted mix: the masks stay a pure function of the
    // terrain, so the mask views (and the witness) need no weightmap.
    const int s2Split = w.op(T::SplitRGBA, 7, { { s2.mask } });
    const int midH   = w.constF(0.5f, 7);
    const int k004   = w.constF(0.04f, 7);
    const int hOff   = w.op(T::Subtract, 7, { { s2Split, 2 }, { midH } });
    const int hBias  = w.op(T::Multiply, 7, { { hOff }, { pHeight } });
    const int hBias2 = w.op(T::Multiply, 7, { { hBias }, { k004 } });
    // The slope does not multiply the water away (that cut every puddle along a slope
    // contour, a clean arc wherever a hollow ran into a bank). It LOWERS THE WATER LEVEL
    // instead: the tilted ground holds less, so a puddle shrinks towards the deepest part
    // of its hollow and ends on the noise's own contour, and at "Puddle Max Slope" the
    // level has dropped by (level + 0.2), below every hollow, which is none left. Water
    // against a wall of a bank can still end on a straight line, but then it is the bank.
    const int shiftK = w.op(T::Add, 7, { { puddleCall, 0 }, { w.constF(0.2f, 7) } });
    const int shift  = w.op(T::Multiply, 7, { { steep }, { shiftK } });
    const int depthW = w.op(T::Subtract, 7, { { depth }, { shift } });
    const int waterT = w.op(T::Subtract, 7, { { depthW }, { hBias2 } });
    const int water  = w.ramp(zero, k004, { waterT }, 7);
    r.waterMask = w.op(T::Multiply, 7, { { water }, { noSnow } });

    // The strength the overlay is applied with: the automatic puddle where the
    // terrain is left to the automatic rules, plus whatever was painted (a painted
    // puddle is water whatever the slope). Unpainted: puddleKeep = 1, mP = 0 → the
    // automatic masks unchanged.
    // Weather snow lies over a puddle like it lies over everything else: it takes the
    // automatic water and its wet rim with it.
    const int notSnowed     = w.op(T::OneMinus, 7, { { weatherSnow, 0 } });
    const int keepAuto      = w.op(T::Multiply, 7, { { puddleKeep }, { notSnowed } });
    const int wetAutoPart   = w.op(T::Multiply, 7, { { r.wetMask },   { keepAuto } });
    const int waterAutoPart = w.op(T::Multiply, 7, { { r.waterMask }, { keepAuto } });
    const int wetF   = w.op(T::Saturate, 7, { { w.op(T::Add, 7, { { wetAutoPart },   { mP, 1 } }) } });
    const int waterF = w.op(T::Saturate, 7, { { w.op(T::Add, 7, { { waterAutoPart }, { mP, 1 } }) } });

    // ── Puddles as an overlay on the ground below (column 8) ─────────────────
    // No layer of its own: wet soil is the same soil, darker and smoother.
    //   wet rim  albedo x 0.6, roughness x 0.5; normal, AO and the texture untouched
    //   water    albedo x 0.35 of the DRY ground, roughness 0.05, a LEVEL normal (straight up):
    //            standing water is horizontal whatever the bank under it does, and the
    //            tilted terrain normal made a sliver of water at a slope's foot mirror the
    //            sky like a tilted pane
    const int wetDark = w.constF(0.6f, 8);
    const int wetA    = w.op(T::Multiply, 8, { { s3.albedo }, { wetDark } });
    const int albedoW = w.op(T::Lerp, 8, { { s3.albedo }, { wetA }, { wetF } });
    const int dark    = w.constF(0.35f, 8);
    const int darkA   = w.op(T::Multiply, 8, { { s3.albedo }, { dark } });
    const int albedo  = w.op(T::Lerp, 8, { { albedoW }, { darkA }, { waterF } });
    const int upN = w.node(T::ConstColor, 8);
    {
        MatGraphNode* c = r.graph.findNode(upN);
        c->p[0] = 0.0f; c->p[1] = 1.0f; c->p[2] = 0.0f;
    }
    const int nWater  = w.op(T::Lerp, 8, { { s3.normal }, { upN }, { waterF } });
    const int normal  = w.op(T::Normalize3, 8, { { nWater } });
    const int mSplit  = w.op(T::SplitRGBA, 8, { { s3.mask } });
    const int wetRgh  = w.constF(0.5f, 8);
    const int roughW  = w.op(T::Multiply, 8, { { mSplit, 1 }, { wetRgh } });
    const int roughD  = w.op(T::Lerp, 8, { { mSplit, 1 }, { roughW }, { wetF } });
    const int glossy  = w.constF(0.05f, 8);
    const int rough   = w.op(T::Lerp, 8, { { roughD }, { glossy }, { waterF } });

    // ── Output (column 9) ────────────────────────────────────────────────────
    r.output = w.node(T::Output, 9);
    if (view == AutoLandscapeView::Lit)
    {
        r.graph.connect(albedo, 0, r.output, kMatOutputBaseColorPin);
        r.graph.connect(rough, 0, r.output, kMatOutputRoughnessPin);
        r.graph.connect(normal, 0, r.output, kMatOutputNormalPin);
        r.graph.connect(mSplit, 0, r.output, kMatOutputAOPin);
        const int metal = w.constF(0.0f, 8);
        r.graph.connect(metal, 0, r.output, kMatOutputMetallicPin);
    }
    else if (view == AutoLandscapeView::Normal || view == AutoLandscapeView::Surface)
    {
        r.graph.findNode(r.output)->p[0] = 0.0f; // unlit: the frame IS the lighting inputs
        int dbg = 0;
        if (view == AutoLandscapeView::Normal)
        {
            const int m = w.op(T::Multiply, 8, { { normal }, { half } });
            dbg = w.op(T::Add, 8, { { m }, { half } });
        }
        else
            dbg = w.op(T::Combine3, 8, { { mSplit, 0 }, { rough }, { zero } });
        r.graph.connect(dbg, 0, r.output, kMatOutputBaseColorPin);
    }
    else
    {
        r.graph.findNode(r.output)->p[0] = 0.0f; // unlit: the frame IS the masks
        const bool rsw = view == AutoLandscapeView::MasksRockSnowWater;
        const int dbg = w.op(T::Combine3, 8, { { rsw ? r.rockMask : r.dirtMask },
                                               { rsw ? r.snowMask : r.wetMask },
                                               { rsw ? r.waterMask : r.flatMask } });
        r.graph.connect(dbg, 0, r.output, kMatOutputBaseColorPin);
    }
    return r;
}
} // namespace HE
