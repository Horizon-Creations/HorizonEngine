// matfn_gen — generates the engine's built-in UI effect library as loose .hasset
// Material Function assets, written via ContentManager::saveAsset so the byte
// layout is identical to editor-authored functions.
//
// Usage:  matfn_gen <output-dir>
//   <output-dir> is the folder the .hasset files are written into (e.g.
//   EditorDeps/EngineContent/MaterialFunctions). It is used verbatim as the
//   ContentManager content root, and each effect is saved under "<Name>.hasset".
//
// Same bargain mesh_gen and widget_gen make: output is deterministic (every
// effect gets a stable, well-known UUID), the results are COMMITTED, and this is
// not part of the normal build graph. Re-running produces byte-identical files,
// so a material that calls "Engine/MaterialFunctions/FrostedGlass.hasset" keeps
// working across regenerations.
//
// ── Why a generator and not nine hand-saved assets ───────────────────────────
// A .hasset is a binary container. Nine of them in the repository would be nine
// blobs nobody can review, diff or fix without opening the editor — and the
// library is exactly the thing that has to be reviewable, because every project
// inherits it. Here an effect is a function, and the wiring is the source.
//
// ── Three rules every effect here follows ────────────────────────────────────
// 1. EVERY INPUT DECLARES A DEFAULT. An effect dropped into a graph has to show
//    something before a single wire is drawn — that is what separates a library
//    from a snippet. The number lives on the Function Input node (p[1], flagged
//    by p[2]); an unconnected caller pin reads it.
// 2. FLAT. No effect calls another. A call would have to name the other one by
//    its content-relative path ("Engine/MaterialFunctions/…"), which is a second
//    resolution path to get right for the sake of saving four nodes.
// 3. THE ELEMENT UNDER THE PIXEL IS THE POINT. Each of these reads a v11 UI node
//    (element size / UV / SDF / border distance / state / backdrop) or Time.
//    A pure-maths helper is not something a UI effect library owes anyone —
//    the standard node palette already has Lerp and Power.
//
// A note on types: the maths nodes are Vec3 in and Vec3 out, so a scalar chain
// runs as a splatted vec3 and the Float output takes .x. That is how the whole
// standard library behaves; the compiler folds it, and writing it any other way
// would mean a second set of scalar nodes.

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <MaterialGraph/MaterialGraph.h>
#include <Types/UUID.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
using namespace HE;

// Well-known UUID base for the built-in effects. `hi` stays far below the
// version-4 bit pattern UUID::generate() enforces and clear of the DefaultAssets
// sentinels (1..7), mesh_gen's 0x100 block and widget_gen's 0x200 block.
constexpr uint64_t kFnBaseHi = 0x0000000000000300ULL;

// Canvas columns, so an effect opens tidy instead of stacked on the origin.
constexpr float kColIn   = 0.0f;
constexpr float kColStep = 230.0f;
constexpr float kRowStep = 110.0f;

using T = MatNodeType;
using P = MatPinType;

// ── The builder ──────────────────────────────────────────────────────────────
// Thin on purpose: it is MaterialGraph with shorter names and column bookkeeping,
// not a second graph model.
struct Fx
{
    MaterialGraph g;
    int inRow = 0;   // next free row in the input column

    // A function input: name, type, and the value a caller's unwired pin takes.
    // Inputs are added FIRST and in display order — the call node's pins are its
    // FnInput nodes sorted by id, so the order they are created in is the order
    // the user sees.
    int in(const char* name, P type, float def)
    {
        const int id = g.addNode(T::FnInput, kColIn, static_cast<float>(inRow++) * kRowStep);
        MatGraphNode& n = *g.findNode(id);
        n.s    = name;
        n.p[0] = static_cast<float>(type);
        n.p[1] = def;
        n.p[2] = 1.0f;   // "this default is authored" — see the FnInput registry entry
        return id;
    }

    int node(T type, int col, float row = 0.0f)
    {
        return g.addNode(type, kColIn + static_cast<float>(col) * kColStep, row * kRowStep);
    }

    // A baked literal. Every one of these is a constant of the EFFECT (a tau, a
    // half, an epsilon that keeps a divide finite), never a knob — a knob is an
    // input with a default.
    int k(float v, int col, float row = 0.0f)
    {
        const int id = node(T::ConstFloat, col, row);
        g.findNode(id)->p[0] = v;
        return id;
    }

    int out(const char* name, P type, int col, float row = 0.0f)
    {
        const int id = node(T::FnOutput, col, row);
        MatGraphNode& n = *g.findNode(id);
        n.s    = name;
        n.p[0] = static_cast<float>(type);
        return id;
    }

    void link(int src, int srcPin, int dst, int dstPin) { g.connect(src, srcPin, dst, dstPin); }
};

// Divisors come from inputs, and an input can be zero. saturate() turns an
// infinity into 1 but 0/0 is a NaN that survives everything downstream, so every
// divisor in this file goes through here first.
int nonZero(Fx& f, int value, int col, float row = 0.0f)
{
    const int add = f.node(T::Add, col, row);
    f.link(value, 0, add, 0);
    f.link(f.k(0.001f, col, row + 0.6f), 0, add, 1);
    return add;
}

// ── Frosted Glass ────────────────────────────────────────────────────────────
// The dialog-over-a-sidebar effect, in one node. Backdrop is what the UI pass has
// drawn so far, so this blurs the SIDEBAR — and only inside a UI material; on a
// mesh the node is black by construction.
Fx frostedGlass()
{
    Fx f;
    const int radius = f.in("Blur Radius", P::Float, 14.0f);
    const int tint   = f.in("Tint",        P::Vec3,  1.0f);
    const int amount = f.in("Tint Amount", P::Float, 0.18f);

    const int back = f.node(T::Backdrop, 2, 0.0f);
    f.link(radius, 0, back, 0);

    // Tint by MIXING rather than multiplying: a white tint at any amount then
    // leaves the backdrop alone, which is what "no tint" should mean.
    const int mix = f.node(T::Lerp, 3, 0.5f);
    f.link(back,   0, mix, 0);
    f.link(tint,   0, mix, 1);
    f.link(amount, 0, mix, 2);

    f.link(mix, 0, f.out("Color", P::Vec3, 4, 0.5f), 0);
    return f;
}

// ── State Tint ───────────────────────────────────────────────────────────────
// The whole hover/press/disabled behaviour of a button as one node. Element State
// hands over the element's BLEND (B8), not a 0/1 — so this eases with the widget's
// own Transition instead of snapping.
Fx stateTint()
{
    Fx f;
    const int base     = f.in("Base Color",    P::Vec3,  1.0f);
    const int hoverAmt = f.in("Hover Lift",    P::Float, 0.08f);
    const int pressAmt = f.in("Press Drop",    P::Float, 0.10f);
    const int fade     = f.in("Disabled Fade", P::Float, 0.45f);

    const int st = f.node(T::ElementState, 2, 0.0f);

    const int lift = f.node(T::Multiply, 3, 0.0f);
    f.link(st, 0, lift, 0);          // Hovered
    f.link(hoverAmt, 0, lift, 1);
    const int drop = f.node(T::Multiply, 3, 1.0f);
    f.link(st, 1, drop, 0);          // Pressed
    f.link(pressAmt, 0, drop, 1);

    const int up   = f.node(T::Add, 4, 0.0f);
    f.link(f.k(1.0f, 3, 2.0f), 0, up, 0);
    f.link(lift, 0, up, 1);
    const int factor = f.node(T::Subtract, 4, 1.0f);
    f.link(up,   0, factor, 0);
    f.link(drop, 0, factor, 1);

    const int tinted = f.node(T::Multiply, 5, 0.0f);
    f.link(base,   0, tinted, 0);
    f.link(factor, 0, tinted, 1);

    // Disabled dims what the other two produced, so a disabled button that is
    // still hovered does not brighten.
    const int dimmed = f.node(T::Multiply, 5, 1.2f);
    f.link(tinted, 0, dimmed, 0);
    f.link(fade,   0, dimmed, 1);
    const int res = f.node(T::Lerp, 6, 0.5f);
    f.link(tinted, 0, res, 0);
    f.link(dimmed, 0, res, 1);
    f.link(st,     3, res, 2);       // Disabled

    f.link(res, 0, f.out("Color", P::Vec3, 7, 0.5f), 0);
    return f;
}

// ── Focus Ring ───────────────────────────────────────────────────────────────
// The one piece of chrome a keyboard user cannot do without, and the one nobody
// draws by hand twice. Border Distance is the element's OWN outline, so the ring
// follows whatever corner radius the element was given.
Fx focusRing()
{
    Fx f;
    const int color = f.in("Ring Color", P::Vec3,  1.0f);
    const int width = f.in("Width",      P::Float, 2.0f);
    const int inset = f.in("Inset",      P::Float, 2.0f);

    const int bd = f.node(T::BorderDistance, 2, 0.0f);
    const int d  = f.node(T::Subtract, 3, 0.0f);
    f.link(bd,    0, d, 0);
    f.link(inset, 0, d, 1);

    // One pixel of coverage on each side of the band, which is exactly what
    // saturate() of a distance in pixels already is — no smoothstep needed.
    const int outer = f.node(T::Saturate, 4, 0.0f);
    f.link(d, 0, outer, 0);
    const int innerD = f.node(T::Subtract, 4, 1.0f);
    f.link(width, 0, innerD, 0);
    f.link(d,     0, innerD, 1);
    const int inner = f.node(T::Saturate, 5, 1.0f);
    f.link(innerD, 0, inner, 0);

    const int band = f.node(T::Multiply, 6, 0.5f);
    f.link(outer, 0, band, 0);
    f.link(inner, 0, band, 1);

    const int st   = f.node(T::ElementState, 5, 2.5f);
    const int mask = f.node(T::Multiply, 7, 1.0f);
    f.link(band, 0, mask, 0);
    f.link(st,   2, mask, 1);        // Focused

    // Colour is forwarded rather than mixed here: the call site wants one wire
    // into Emissive and one into Opacity, and what the ring sits ON is the
    // caller's business.
    f.link(color, 0, f.out("Color", P::Vec3,  8, 0.0f), 0);
    f.link(mask,  0, f.out("Mask",  P::Float, 8, 1.0f), 0);
    return f;
}

// ── Inner Border ─────────────────────────────────────────────────────────────
// The same band without the state — a hairline inside the element's own outline,
// for a card edge or a divider that has to follow a rounded corner.
Fx innerBorder()
{
    Fx f;
    const int width = f.in("Width",    P::Float, 1.5f);
    const int inset = f.in("Inset",    P::Float, 0.0f);
    const int soft  = f.in("Softness", P::Float, 1.0f);

    const int bd = f.node(T::BorderDistance, 2, 0.0f);
    const int d  = f.node(T::Subtract, 3, 0.0f);
    f.link(bd,    0, d, 0);
    f.link(inset, 0, d, 1);

    const int s = nonZero(f, soft, 3, 2.0f);

    const int outerT = f.node(T::Divide, 4, 0.0f);
    f.link(d, 0, outerT, 0);
    f.link(s, 0, outerT, 1);
    const int outer = f.node(T::Saturate, 5, 0.0f);
    f.link(outerT, 0, outer, 0);

    const int innerD = f.node(T::Subtract, 4, 1.2f);
    f.link(width, 0, innerD, 0);
    f.link(d,     0, innerD, 1);
    const int innerT = f.node(T::Divide, 5, 1.2f);
    f.link(innerD, 0, innerT, 0);
    f.link(s,      0, innerT, 1);
    const int inner = f.node(T::Saturate, 6, 1.2f);
    f.link(innerT, 0, inner, 0);

    const int mask = f.node(T::Multiply, 7, 0.6f);
    f.link(outer, 0, mask, 0);
    f.link(inner, 0, mask, 1);

    f.link(mask, 0, f.out("Mask", P::Float, 8, 0.6f), 0);
    return f;
}

// ── Edge Fade ────────────────────────────────────────────────────────────────
// 1 in the middle, 0 at the element's outline: the mask a scrolling list wants at
// its ends and a glow wants everywhere.
Fx edgeFade()
{
    Fx f;
    const int dist = f.in("Distance", P::Float, 12.0f);
    const int pow  = f.in("Power",    P::Float, 1.0f);

    const int bd = f.node(T::BorderDistance, 2, 0.0f);
    const int dd = nonZero(f, dist, 2, 1.5f);

    const int t = f.node(T::Divide, 3, 0.5f);
    f.link(bd, 0, t, 0);
    f.link(dd, 0, t, 1);
    const int sat = f.node(T::Saturate, 4, 0.5f);
    f.link(t, 0, sat, 0);

    // Power last, on a value already in 0..1 — the shape of the falloff, not a
    // second scale.
    const int shaped = f.node(T::Power, 5, 0.5f);
    f.link(sat, 0, shaped, 0);
    f.link(pow, 0, shaped, 1);

    f.link(shaped, 0, f.out("Mask", P::Float, 6, 0.5f), 0);
    return f;
}

// ── Sheen ────────────────────────────────────────────────────────────────────
// The highlight that sweeps across a button. Element UV and not the mesh UV: this
// has to run from one side of THIS element to the other, whatever it is tiled to.
Fx sheen()
{
    Fx f;
    const int speed    = f.in("Speed",    P::Float, 0.35f);
    const int width    = f.in("Width",    P::Float, 0.18f);
    const int skew     = f.in("Skew",     P::Float, 0.40f);
    const int strength = f.in("Strength", P::Float, 0.35f);

    const int uv  = f.node(T::ElementUV, 2, 0.0f);
    const int spl = f.node(T::SplitRGBA, 3, 0.0f);
    f.link(uv, 0, spl, 0);

    // x runs along the element, tilted by the row — a vertical bar sweeping
    // sideways reads as a reflection, a straight one reads as a wipe.
    const int tilt = f.node(T::Multiply, 4, 1.0f);
    f.link(spl,  1, tilt, 0);        // V
    f.link(skew, 0, tilt, 1);
    const int x = f.node(T::Add, 5, 0.5f);
    f.link(spl,  0, x, 0);           // U
    f.link(tilt, 0, x, 1);

    const int time = f.node(T::Time, 2, 3.0f);
    const int adv  = f.node(T::Multiply, 3, 3.0f);
    f.link(time,  0, adv, 0);
    f.link(speed, 0, adv, 1);
    const int pos = f.node(T::Fract, 4, 3.0f);
    f.link(adv, 0, pos, 0);

    const int diff = f.node(T::Subtract, 6, 1.5f);
    f.link(x,   0, diff, 0);
    f.link(pos, 0, diff, 1);
    const int dist = f.node(T::Absolute, 7, 1.5f);
    f.link(diff, 0, dist, 0);

    const int w = nonZero(f, width, 6, 3.5f);
    const int t = f.node(T::Divide, 8, 1.5f);
    f.link(dist, 0, t, 0);
    f.link(w,    0, t, 1);
    const int inv = f.node(T::OneMinus, 9, 1.5f);
    f.link(t, 0, inv, 0);
    const int band = f.node(T::Saturate, 10, 1.5f);
    f.link(inv, 0, band, 0);

    const int amount = f.node(T::Multiply, 11, 1.5f);
    f.link(band,     0, amount, 0);
    f.link(strength, 0, amount, 1);

    f.link(amount, 0, f.out("Amount", P::Float, 12, 1.5f), 0);
    return f;
}

// ── Pulse ────────────────────────────────────────────────────────────────────
// A number that breathes between two values. The only effect here with no UI node
// in it, and it earns its place: every "this needs attention" in an application is
// this multiplied into something.
Fx pulse()
{
    Fx f;
    const int speed = f.in("Speed", P::Float, 1.5f);
    const int lo    = f.in("Min",   P::Float, 0.6f);
    const int hi    = f.in("Max",   P::Float, 1.0f);

    const int time = f.node(T::Time, 2, 0.0f);
    const int adv  = f.node(T::Multiply, 3, 0.0f);
    f.link(time,  0, adv, 0);
    f.link(speed, 0, adv, 1);

    // Speed in CYCLES per second, not radians: 1.5 has to mean one and a half
    // breaths, or the knob is a physics unit rather than a design one.
    const int phase = f.node(T::Multiply, 4, 0.0f);
    f.link(adv, 0, phase, 0);
    f.link(f.k(6.2831853f, 3, 1.2f), 0, phase, 1);

    const int s = f.node(T::Sine, 5, 0.0f);
    f.link(phase, 0, s, 0);
    const int half = f.node(T::Multiply, 6, 0.0f);
    f.link(s, 0, half, 0);
    f.link(f.k(0.5f, 5, 1.2f), 0, half, 1);
    const int t = f.node(T::Add, 7, 0.0f);
    f.link(half, 0, t, 0);
    f.link(f.k(0.5f, 6, 1.4f), 0, t, 1);

    const int v = f.node(T::Lerp, 8, 0.5f);
    f.link(lo, 0, v, 0);
    f.link(hi, 0, v, 1);
    f.link(t,  0, v, 2);

    f.link(v, 0, f.out("Value", P::Float, 9, 0.5f), 0);
    return f;
}

// ── Scanlines ────────────────────────────────────────────────────────────────
// A multiplier, not a colour: the CRT/terminal look belongs on top of whatever the
// element already is, and Speed 0 (the default) makes it a static texture.
Fx scanlines()
{
    Fx f;
    const int count    = f.in("Line Count", P::Float, 48.0f);
    const int strength = f.in("Strength",   P::Float, 0.12f);
    const int speed    = f.in("Speed",      P::Float, 0.0f);

    const int uv  = f.node(T::ElementUV, 2, 0.0f);
    const int spl = f.node(T::SplitRGBA, 3, 0.0f);
    f.link(uv, 0, spl, 0);

    const int time  = f.node(T::Time, 2, 2.0f);
    const int drift = f.node(T::Multiply, 3, 2.0f);
    f.link(time,  0, drift, 0);
    f.link(speed, 0, drift, 1);

    const int y = f.node(T::Add, 4, 1.0f);
    f.link(spl,   1, y, 0);          // V
    f.link(drift, 0, y, 1);

    const int freq = f.node(T::Multiply, 4, 3.0f);
    f.link(count, 0, freq, 0);
    f.link(f.k(6.2831853f, 3, 4.0f), 0, freq, 1);
    const int phase = f.node(T::Multiply, 5, 2.0f);
    f.link(y,    0, phase, 0);
    f.link(freq, 0, phase, 1);

    const int s = f.node(T::Sine, 6, 2.0f);
    f.link(phase, 0, s, 0);
    const int half = f.node(T::Multiply, 7, 2.0f);
    f.link(s, 0, half, 0);
    f.link(f.k(0.5f, 6, 3.2f), 0, half, 1);
    const int band = f.node(T::Add, 8, 2.0f);
    f.link(half, 0, band, 0);
    f.link(f.k(0.5f, 7, 3.4f), 0, band, 1);

    const int cut = f.node(T::Multiply, 9, 2.0f);
    f.link(band,     0, cut, 0);
    f.link(strength, 0, cut, 1);
    const int mul = f.node(T::OneMinus, 10, 2.0f);
    f.link(cut, 0, mul, 0);

    f.link(mul, 0, f.out("Multiplier", P::Float, 11, 2.0f), 0);
    return f;
}

// ── Soft Rounded Mask ────────────────────────────────────────────────────────
// An AUTHORED rounded rectangle, unlike Inner Border above: this is the shape you
// want, feathered, not the one the element happens to have. What a drop shadow, a
// highlight blob or a rounded clip is made of.
Fx softRoundedMask()
{
    Fx f;
    const int radius  = f.in("Radius",  P::Float, 10.0f);
    const int inset   = f.in("Inset",   P::Float, 0.0f);
    const int feather = f.in("Feather", P::Float, 1.0f);

    const int sdf = f.node(T::RoundedRectSDF, 2, 0.0f);
    f.link(radius, 0, sdf, 0);
    f.link(inset,  0, sdf, 1);

    // Half a pixel out from the zero crossing is where coverage is 50%, which is
    // what makes Feather 1 an antialiased edge rather than a hard one.
    const int num = f.node(T::Subtract, 3, 0.0f);
    f.link(f.k(0.5f, 2, 1.5f), 0, num, 0);
    f.link(sdf, 0, num, 1);          // Distance, negative inside

    const int fe = nonZero(f, feather, 3, 2.5f);
    const int t  = f.node(T::Divide, 4, 0.5f);
    f.link(num, 0, t, 0);
    f.link(fe,  0, t, 1);
    const int mask = f.node(T::Saturate, 5, 0.5f);
    f.link(t, 0, mask, 0);

    f.link(mask, 0, f.out("Mask", P::Float, 6, 0.5f), 0);
    return f;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: matfn_gen <output-dir>\n");
        return 2;
    }
    const std::string outDir = argv[1];
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    ContentManager cm(outDir);

    // ORDER IS IDENTITY: the index picks the UUID, and a material that already
    // calls one of these stores that UUID. Append, never reorder.
    struct Entry { const char* name; Fx fx; };
    std::vector<Entry> entries;
    entries.push_back({ "FrostedGlass",    frostedGlass()    });
    entries.push_back({ "StateTint",       stateTint()       });
    entries.push_back({ "FocusRing",       focusRing()       });
    entries.push_back({ "InnerBorder",     innerBorder()     });
    entries.push_back({ "EdgeFade",        edgeFade()        });
    entries.push_back({ "Sheen",           sheen()           });
    entries.push_back({ "Pulse",           pulse()           });
    entries.push_back({ "Scanlines",       scanlines()       });
    entries.push_back({ "SoftRoundedMask", softRoundedMask() });

    int ok = 0, index = 0;
    for (const Entry& e : entries)
    {
        MaterialFunctionAsset a;
        a.type         = HE::AssetType::MaterialFunction;
        a.name         = e.name;
        a.path         = std::string(e.name) + ".hasset";
        a.id           = HE::UUID{ kFnBaseHi + static_cast<uint64_t>(index),
                                   0x0000000000000001ULL };
        a.nodeGraphJson = materialGraphToJson(e.fx.g);

        // An input without an authored default is the one mistake that is
        // invisible until someone drops the effect in and sees 0.5 px of blur.
        // Warned here, failed in the test — the same split widget_gen uses.
        std::vector<MatPinDesc> ins, outs;
        matFunctionPins(e.fx.g, ins, outs);
        for (const MatGraphNode& n : e.fx.g.nodes)
            if (n.type == MatNodeType::FnInput && n.p[2] < 0.5f)
                std::fprintf(stderr, "  %-16s WARNING: input '%s' declares no default\n",
                             e.name, n.s.c_str());
        if (outs.empty())
            std::fprintf(stderr, "  %-16s WARNING: no outputs\n", e.name);

        if (cm.saveAsset(a))
        {
            std::printf("  %-16s %2zu nodes  %zu in / %zu out\n",
                        e.name, e.fx.g.nodes.size(), ins.size(), outs.size());
            ++ok;
        }
        else
            std::fprintf(stderr, "  FAILED to write %s.hasset\n", e.name);
        ++index;
    }

    std::printf("matfn_gen: wrote %d/%zu effects to %s\n",
                ok, entries.size(), outDir.c_str());
    return ok == static_cast<int>(entries.size()) ? 0 : 1;
}
