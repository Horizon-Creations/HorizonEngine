// Auto landscape material — see AutoLandscapeMaterial.h for what it does and why
// it is a plain graph. This file is only the wiring.
#include <MaterialGraph/AutoLandscapeMaterial.h>
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
} // namespace

AutoLandscapeGraph buildAutoLandscapeGraph(AutoLandscapeView view)
{
    AutoLandscapeGraph r;
    Wire w{ r.graph };

    // ── Parameters (column 0) ────────────────────────────────────────────────
    const int pGroundTile = w.param(kAutoLandscapeParamGroundTile, 2.0f, 0.25f, 16.0f, "Tiling",
        "Metres one Grass / Dirt texture covers (world space).");
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
    const int pPudAmt     = w.param(kAutoLandscapeParamPuddleAmount, 0.5f, 0.0f, 1.0f, "Puddles",
        "Water level in the puddle noise field when the weather's Puddles slider is at 1 (the slider scales it): "
        "0.32 = scattered puddles (~1/5 of the flat ground), ~0.5 = half.");
    const int pPudSlope   = w.param(kAutoLandscapeParamPuddleMaxSlope, 0.08f, 0.002f, 0.4f, "Puddles",
        "Slope (1 - normal.y) where automatic standing water has faded out completely; it starts to thin "
        "out from a fifth of that. 0.08 ~ 23 degrees. Painted puddles ignore it.");

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
    // Snow is bombed too: read plainly, its one tile showed as a visible grid of the
    // same dark marks on any larger painted or high-altitude field of it.
    const LayerReads snow  = layer(AutoLandscapeLayer::Snow,      uvRock,   53);
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
    // The weather's snow on top of the height snow: MF_WeatherSnow turns the Weather
    // details panel's Snow Cover slider into a coverage by slope (cliffs stay bare) and by
    // the layers' height maps (snow settles in the snow texture's high texels first). The
    // two add as probabilities, a + b - a*b, so neither can push the other past 1.
    // r.snowMask stays the HEIGHT snow alone — the mask views and the witness read it, and
    // they must not depend on a function or on the scene's weather.
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
    const int none = w.node(T::ConstColor, 6);
    r.graph.findNode(none)->p[0] = r.graph.findNode(none)->p[1] = r.graph.findNode(none)->p[2] = 0.0f;
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
        r.graph.findNode(n)->s = kAutoLandscapePaintLayerNames;
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
    // Wet rim: from 0.06 below the water line up to it.
    const int rimW   = w.constF(0.06f, 7);
    const int negRim = w.constF(-0.06f, 7);
    const int rim    = w.ramp(negRim, rimW, { depth }, 7);
    r.wetMask = w.op(T::Multiply, 7, { { rim }, { r.flatMask } });
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
    const int waterT = w.op(T::Subtract, 7, { { depth }, { hBias2 } });
    const int water  = w.ramp(zero, k004, { waterT }, 7);
    r.waterMask = w.op(T::Multiply, 7, { { water }, { r.flatMask } });

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
    //   water    albedo x 0.35 of the DRY ground, roughness 0.05, flat geometric normal
    const int wetDark = w.constF(0.6f, 8);
    const int wetA    = w.op(T::Multiply, 8, { { s3.albedo }, { wetDark } });
    const int albedoW = w.op(T::Lerp, 8, { { s3.albedo }, { wetA }, { wetF } });
    const int dark    = w.constF(0.35f, 8);
    const int darkA   = w.op(T::Multiply, 8, { { s3.albedo }, { dark } });
    const int albedo  = w.op(T::Lerp, 8, { { albedoW }, { darkA }, { waterF } });
    const int nWater  = w.op(T::Lerp, 8, { { s3.normal }, { nrm }, { waterF } });
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
