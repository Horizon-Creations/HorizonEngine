// Auto landscape material — see AutoLandscapeMaterial.h for what it does and why
// it is a plain graph. This file is only the wiring.
#include <MaterialGraph/AutoLandscapeMaterial.h>

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
} // namespace

AutoLandscapeGraph buildAutoLandscapeGraph(AutoLandscapeView view)
{
    AutoLandscapeGraph r;
    Wire w{ r.graph };

    // ── Parameters (column 0) ────────────────────────────────────────────────
    const int pGroundTile = w.param(kAutoLandscapeParamGroundTile, 2.0f, 0.25f, 16.0f, "Tiling",
        "Metres one Grass / Dirt / Wet Ground texture covers (world space).");
    const int pRockTile   = w.param(kAutoLandscapeParamRockTile, 4.0f, 0.25f, 32.0f, "Tiling",
        "Metres one Rock / Snow texture covers (world space).");
    const int pCell       = w.param(kAutoLandscapeParamBombCell, 0.5f, 0.1f, 2.0f, "Tiling",
        "Texture bombing: spacing of the random hexes, in texture repeats. Smaller = more variation, more seams.");
    const int pRockSlope  = w.param(kAutoLandscapeParamRockSlope, 0.12f, 0.0f, 1.0f, "Rock",
        "Slope (1 - normal.y) where rock starts. 0.12 ~ 28 degrees, 0.29 ~ 45 degrees.");
    const int pRockBlend  = w.param(kAutoLandscapeParamRockBlend, 0.12f, 0.01f, 0.5f, "Rock",
        "Slope range over which ground turns into rock.");
    const int pHeight     = w.param(kAutoLandscapeParamHeightBlend, 1.0f, 0.0f, 4.0f, "Rock",
        "How much the layers' height maps bend every transition (0 = plain smooth blends).");
    const int pDirtAmt    = w.param(kAutoLandscapeParamDirtAmount, 0.35f, 0.0f, 1.0f, "Ground",
        "Share of the flat ground covered by dirt patches instead of grass.");
    const int pDirtSize   = w.param(kAutoLandscapeParamDirtSize, 24.0f, 1.0f, 200.0f, "Ground",
        "Size of the dirt patches, metres.");
    const int pSnowH      = w.param(kAutoLandscapeParamSnowHeight, 60.0f, -500.0f, 3000.0f, "Snow",
        "World height (Y, metres) where snow starts.");
    const int pSnowBlend  = w.param(kAutoLandscapeParamSnowBlend, 6.0f, 0.1f, 100.0f, "Snow",
        "Height range over which the snow cover closes, metres.");
    const int pSnowSlope  = w.param(kAutoLandscapeParamSnowMaxSlope, 0.45f, 0.0f, 1.0f, "Snow",
        "Steepest slope that holds snow; steeper faces stay rock.");
    const int pPudAmt     = w.param(kAutoLandscapeParamPuddleAmount, 0.32f, 0.0f, 1.0f, "Puddles",
        "Water level in the puddle noise field: 0 = none, 0.32 = scattered puddles (~1/5 of the flat ground), ~0.5 = half.");
    const int pPudSize    = w.param(kAutoLandscapeParamPuddleSize, 10.0f, 0.5f, 100.0f, "Puddles",
        "Size of the puddle hollows, metres.");
    const int pPudSlope   = w.param(kAutoLandscapeParamPuddleMaxSlope, 0.03f, 0.002f, 0.2f, "Puddles",
        "Steepest slope that holds water (1 - normal.y). 0.03 ~ 14 degrees.");

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

    // ── Texture reads (column 2): albedo / normal / mask per layer ──────────
    // A bombed layer reads all three maps on ONE hex grid: same uv node, same
    // Cell node, same rotation/sharpness/seed → codegen emits the grid once.
    auto layer = [&](AutoLandscapeLayer L, int uv, int seed) {
        LayerReads lr;
        const int slice = w.constF(static_cast<float>(static_cast<int>(L)), 2);
        auto read = [&](T plain, T bombed, const char* path) {
            const int p = w.node(plain, 2);
            r.graph.findNode(p)->s = path;
            r.graph.connect(uv, 0, p, 0);
            r.graph.connect(slice, 0, p, 1);
            if (seed < 0) return p;
            const int b = w.node(bombed, 2);
            MatGraphNode* bn = r.graph.findNode(b);
            bn->s = path;
            bn->p[2] = static_cast<float>(seed);
            r.graph.connect(uv, 0, b, 0);
            r.graph.connect(slice, 0, b, 1);
            r.graph.connect(pCell, 0, b, 2);
            // Compile-time choice: the untaken read is never emitted.
            const int sw = w.op(T::StaticSwitch, 2, { { b }, { p } });
            r.graph.findNode(sw)->s = kAutoLandscapeSwitchBombing;
            r.graph.findNode(sw)->p[0] = 1.0f;
            return sw;
        };
        lr.albedo = read(T::TextureArraySample,   T::TextureArrayBombSample,   kAutoLandscapeAlbedoArray);
        lr.normal = read(T::NormalMapArraySample, T::NormalMapArrayBombSample, kAutoLandscapeNormalArray);
        lr.mask   = read(T::TextureArraySample,   T::TextureArrayBombSample,   kAutoLandscapeMaskArray);
        lr.height = w.op(T::SplitRGBA, 2, { { lr.mask } }); // pin 2 = B = height
        return lr;
    };
    const LayerReads grass = layer(AutoLandscapeLayer::Grass,     uvGround, 11);
    const LayerReads dirt  = layer(AutoLandscapeLayer::Dirt,      uvGround, 23);
    const LayerReads rock  = layer(AutoLandscapeLayer::Rock,      uvRock,   37);
    const LayerReads snow  = layer(AutoLandscapeLayer::Snow,      uvRock,   -1);
    const LayerReads wet   = layer(AutoLandscapeLayer::WetGround, uvGround, -1);

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

    // ── Ground: grass, dirt patches, dirt belt below the rock (column 3) ─────
    const int invDirt  = w.op(T::Divide, 3, { { one }, { pDirtSize } });
    const int dirtFbm  = w.op(T::Fbm, 3, { { xz }, { invDirt } });
    // Integer hash: the float heHash21 differs per driver at world-space lattice
    // indices (blocky patches on D3D12/Vulkan, Thema 158 Schritt 7/8).
    r.graph.findNode(dirtFbm)->p[0] = 1.0f;
    const int tenth    = w.constF(0.1f, 3);
    const int dirtT    = w.op(T::Add, 3, { { dirtFbm }, { heightBias(dirt.height, 2, grass.height, 2, tenth, 3) } });
    // fBm sits around 0.5 (range ~0.1..0.85): edge = 0.75 − 0.5 × amount.
    const int half     = w.constF(0.5f, 3);
    const int amtHalf  = w.op(T::Multiply, 3, { { pDirtAmt }, { half } });
    const int edgeBase = w.constF(0.75f, 3);
    const int dirtE0   = w.op(T::Subtract, 3, { { edgeBase }, { amtHalf } });
    const int dirtW    = w.constF(0.06f, 3);
    const int dirtNoise = w.ramp(dirtE0, dirtW, { dirtT }, 3);
    const int beltE0   = w.op(T::Subtract, 3, { { pRockSlope }, { pRockBlend } });
    const int dirtBelt = w.ramp(beltE0, pRockBlend, { r.slope }, 3);
    const int dirtSum  = w.op(T::Add, 3, { { dirtNoise }, { dirtBelt } });
    r.dirtMask = w.op(T::Saturate, 3, { { dirtSum } });
    const Surface ground = blend({ grass.albedo, grass.normal, grass.mask }, dirt, r.dirtMask, 3);
    const int groundSplit = w.op(T::SplitRGBA, 3, { { ground.mask } });

    // ── Rock on the slopes (column 4) ────────────────────────────────────────
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
    const Surface s2 = blend(s1, snow, r.snowMask, 5);

    // ── Puddles: flat ground in the minima of a noise field (column 6) ───────
    const int halfSl = w.op(T::Multiply, 6, { { pPudSlope }, { half } });
    const int steep  = w.op(T::Smoothstep, 6, { { halfSl }, { pPudSlope }, { r.slope } });
    const int noSnow = w.op(T::OneMinus, 6, { { r.snowMask } });
    const int flat   = w.op(T::OneMinus, 6, { { steep } });
    r.flatMask = w.op(T::Multiply, 6, { { flat }, { noSnow } });
    // A second field, decorrelated from the dirt patches by an offset.
    const int offs   = w.node(T::ConstVec2, 6);
    r.graph.findNode(offs)->p[0] = 173.1f;
    r.graph.findNode(offs)->p[1] = 419.7f;
    const int xzOff  = w.op(T::Add, 6, { { xz }, { offs } });
    const int invPud = w.op(T::Divide, 6, { { one }, { pPudSize } });
    const int basin  = w.op(T::Fbm, 6, { { xzOff }, { invPud } });
    r.graph.findNode(basin)->p[0] = 1.0f; // integer hash, see dirtFbm
    const int depth  = w.op(T::Subtract, 6, { { pPudAmt }, { basin } });  // > 0 inside a hollow
    // Wet rim: from 0.06 below the water line up to it.
    const int rimW   = w.constF(0.06f, 6);
    const int negRim = w.constF(-0.06f, 6);
    const int rim    = w.ramp(negRim, rimW, { depth }, 6);
    r.wetMask = w.op(T::Multiply, 6, { { rim }, { r.flatMask } });
    // Water fills the low texels of the wet ground first.
    const int midH   = w.constF(0.5f, 6);
    const int k004   = w.constF(0.04f, 6);
    const int hOff   = w.op(T::Subtract, 6, { { wet.height, 2 }, { midH } });
    const int hBias  = w.op(T::Multiply, 6, { { hOff }, { pHeight } });
    const int hBias2 = w.op(T::Multiply, 6, { { hBias }, { k004 } });
    const int waterT = w.op(T::Subtract, 6, { { depth }, { hBias2 } });
    const int water  = w.ramp(zero, k004, { waterT }, 6);
    r.waterMask = w.op(T::Multiply, 6, { { water }, { r.flatMask } });
    const Surface s3 = blend(s2, wet, r.wetMask, 6);

    // ── Standing water: darker, mirror-smooth, flat (column 7) ───────────────
    const int dark   = w.constF(0.35f, 7);
    const int darkA  = w.op(T::Multiply, 7, { { s3.albedo }, { dark } });
    const int albedo = w.op(T::Lerp, 7, { { s3.albedo }, { darkA }, { r.waterMask } });
    const int nWater = w.op(T::Lerp, 7, { { s3.normal }, { nrm }, { r.waterMask } });
    const int normal = w.op(T::Normalize3, 7, { { nWater } });
    const int mSplit = w.op(T::SplitRGBA, 7, { { s3.mask } });
    const int glossy = w.constF(0.05f, 7);
    const int rough  = w.op(T::Lerp, 7, { { mSplit, 1 }, { glossy }, { r.waterMask } });

    // ── Output (column 8) ────────────────────────────────────────────────────
    r.output = w.node(T::Output, 8);
    if (view == AutoLandscapeView::Lit)
    {
        r.graph.connect(albedo, 0, r.output, kMatOutputBaseColorPin);
        r.graph.connect(rough, 0, r.output, kMatOutputRoughnessPin);
        r.graph.connect(normal, 0, r.output, kMatOutputNormalPin);
        r.graph.connect(mSplit, 0, r.output, kMatOutputAOPin);
        const int metal = w.constF(0.0f, 7);
        r.graph.connect(metal, 0, r.output, kMatOutputMetallicPin);
    }
    else if (view == AutoLandscapeView::Normal || view == AutoLandscapeView::Surface)
    {
        r.graph.findNode(r.output)->p[0] = 0.0f; // unlit: the frame IS the lighting inputs
        int dbg = 0;
        if (view == AutoLandscapeView::Normal)
        {
            const int m = w.op(T::Multiply, 7, { { normal }, { half } });
            dbg = w.op(T::Add, 7, { { m }, { half } });
        }
        else
            dbg = w.op(T::Combine3, 7, { { mSplit, 0 }, { rough }, { zero } });
        r.graph.connect(dbg, 0, r.output, kMatOutputBaseColorPin);
    }
    else
    {
        r.graph.findNode(r.output)->p[0] = 0.0f; // unlit: the frame IS the masks
        const bool rsw = view == AutoLandscapeView::MasksRockSnowWater;
        const int dbg = w.op(T::Combine3, 7, { { rsw ? r.rockMask : r.dirtMask },
                                               { rsw ? r.snowMask : r.wetMask },
                                               { rsw ? r.waterMask : r.flatMask } });
        r.graph.connect(dbg, 0, r.output, kMatOutputBaseColorPin);
    }
    return r;
}
} // namespace HE
