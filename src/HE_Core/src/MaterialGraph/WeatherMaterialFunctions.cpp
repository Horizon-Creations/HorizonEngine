// Weather material functions — see WeatherMaterialFunctions.h.
#include <MaterialGraph/WeatherMaterialFunctions.h>

#include <string>

namespace HE
{
namespace
{
using T = MatNodeType;
using P = MatPinType;

constexpr float kColStep = 230.0f;
constexpr float kRowStep = 110.0f;

// The same thin builder matfn_gen uses for the UI effects: MaterialGraph with column
// bookkeeping so the function opens tidy in the editor.
struct Fn
{
    MaterialGraph g;
    int inRow = 0;

    // Inputs first and in display order: a call node's pins are its FnInput nodes by id.
    int in(const char* name, P type, float def)
    {
        const int id = g.addNode(T::FnInput, 0.0f, static_cast<float>(inRow++) * kRowStep);
        MatGraphNode& n = *g.findNode(id);
        n.s    = name;
        n.p[0] = static_cast<float>(type);
        n.p[1] = def;
        n.p[2] = 1.0f;   // the default is authored (see the FnInput registry entry)
        return id;
    }
    int node(T type, int col, float row = 0.0f)
    {
        return g.addNode(type, static_cast<float>(col) * kColStep, row * kRowStep);
    }
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

// Weather node pins.
constexpr int kPinWetness = 0, kPinPuddles = 2, kPinSnowCover = 3, kPinPuddleSize = 4;
} // namespace

MaterialGraph buildWeatherPuddlesFunction()
{
    Fn f;
    const int maxLevel = f.in("Max Water Level", P::Float, 0.5f);

    const int wx = f.node(T::Weather, 1, 0.0f);
    // The slider decides how much of the field's maximum level is under water.
    const int level = f.node(T::Multiply, 2, 0.0f);
    f.link(wx, kPinPuddles, level, 0);
    f.link(maxLevel, 0, level, 1);

    f.link(level, 0, f.out("Water Level", P::Float, 3, 0.0f), 0);
    f.link(wx, kPinPuddles, f.out("Puddles", P::Float, 3, 1.0f), 0);
    f.link(wx, kPinWetness, f.out("Wetness", P::Float, 3, 2.0f), 0);
    // The hollow size in metres, from the panel's Puddle Size slider — appended last so the
    // pins a graph already wired keep their index.
    f.link(wx, kPinPuddleSize, f.out("Size", P::Float, 3, 3.0f), 0);
    return f.g;
}

MaterialGraph buildWeatherSnowFunction()
{
    Fn f;
    const int slope    = f.in("Slope",       P::Float, 0.0f);
    const int maxSlope = f.in("Max Slope",   P::Float, 0.45f);
    const int bias     = f.in("Height Bias", P::Float, 0.0f);

    const int wx = f.node(T::Weather, 1, 0.0f);

    // How much has settled here: the slider, scaled by (1 + bias) — a positive bias
    // lets this spot fill first, a negative one last — so a slider at 0 stays at 0
    // whatever the bias. Closed over a short ramp, so the slider reads as "more snow"
    // across its whole range instead of switching at one value.
    const int onePlus = f.node(T::Add, 2, 2.0f);
    f.link(f.k(1.0f, 1, 3.0f), 0, onePlus, 0);
    f.link(bias, 0, onePlus, 1);
    const int amount = f.node(T::Multiply, 3, 0.0f);
    f.link(wx, kPinSnowCover, amount, 0);
    f.link(onePlus, 0, amount, 1);
    const int settle = f.node(T::Smoothstep, 4, 0.0f);
    f.link(f.k(0.0f, 3, 1.0f), 0, settle, 0);
    f.link(f.k(0.3f, 3, 1.8f), 0, settle, 1);
    f.link(amount, 0, settle, 2);

    // Cliffs shed it: full below Max Slope, gone 0.1 above.
    const int edgeEnd = f.node(T::Add, 2, 3.5f);
    f.link(maxSlope, 0, edgeEnd, 0);
    f.link(f.k(0.1f, 1, 4.5f), 0, edgeEnd, 1);
    const int cliff = f.node(T::Smoothstep, 3, 3.5f);
    f.link(maxSlope, 0, cliff, 0);
    f.link(edgeEnd,  0, cliff, 1);
    f.link(slope,    0, cliff, 2);
    const int holds = f.node(T::OneMinus, 4, 3.5f);
    f.link(cliff, 0, holds, 0);

    const int snow = f.node(T::Multiply, 5, 1.5f);
    f.link(settle, 0, snow, 0);
    f.link(holds,  0, snow, 1);

    f.link(snow, 0, f.out("Snow", P::Float, 6, 1.5f), 0);
    f.link(wx, kPinSnowCover, f.out("Cover", P::Float, 6, 2.5f), 0);
    return f.g;
}

MatFunctionLoader weatherFunctionLoader()
{
    static const MaterialGraph puddles = buildWeatherPuddlesFunction();
    static const MaterialGraph snow    = buildWeatherSnowFunction();
    return [](const std::string& path) -> const MaterialGraph*
    {
        if (path == kWeatherPuddlesFunctionPath) return &puddles;
        if (path == kWeatherSnowFunctionPath)    return &snow;
        return nullptr;
    };
}
} // namespace HE
