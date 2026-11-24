#include "doctest.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <MaterialGraph/MatCpuEval.h>
#include <MaterialGraph/MaterialGraph.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// ── Material graphs evaluated on the CPU (the software renderer's materials) ──
// Each case states what the GPU computes for the same graph — the rule in
// generateFragment it follows — and asks the interpreter for the same number.
// The pixel-level comparison against Metal and GL is the UITEST material sheet
// (HE_DUMP_UITEST=materials); this file pins the rules one by one.

using HE::MaterialGraph;
using HE::MatNodeType;

namespace
{
    struct G
    {
        MaterialGraph g;
        int out = 0;
        explicit G(HE::MatBlendMode blend = HE::MatBlendMode::Translucent)
        {
            out = g.addNode(MatNodeType::Output);
            g.findNode(out)->p[1] = static_cast<float>(blend);
            g.findNode(out)->p[3] = static_cast<float>(HE::MatDomain::UserInterface);
        }
        int add(MatNodeType t, float a = 0, float b = 0, float c = 0, float d = 0)
        {
            const int id = g.addNode(t);
            auto* n = g.findNode(id);
            n->p[0] = a; n->p[1] = b; n->p[2] = c; n->p[3] = d;
            return id;
        }
        void link(int s, int sp, int d, int dp) { REQUIRE(g.connect(s, sp, d, dp)); }
    };

    glm::vec4 run(const HE::MatCpuProgram& p, glm::vec2 uv = { 0.5f, 0.5f },
                  HE::MatCpuElement e = {}, glm::vec3 color = glm::vec3(1.0f), float time = 0.0f)
    {
        bool discard = false;
        return p.eval(e, uv, color, time, HE::MatCpuHooks{}, discard);
    }

    bool near(float a, float b) { return std::abs(a - b) < 1e-5f; }
}

TEST_CASE("CPU material: arithmetic and coercion follow the codegen")
{
    G m;
    // BaseColor = Lerp(red, blue, ElementUV) — a Vec2 into a Float pin is its .x.
    const int red = m.add(MatNodeType::ConstColor, 1, 0, 0);
    const int blue = m.add(MatNodeType::ConstColor, 0, 0, 1);
    const int euv = m.add(MatNodeType::ElementUV);
    const int lerp = m.add(MatNodeType::Lerp);
    m.link(red, 0, lerp, 0); m.link(blue, 0, lerp, 1); m.link(euv, 0, lerp, 2);
    m.link(lerp, 0, m.out, HE::kMatOutputBaseColorPin);
    // Opacity = Divide(1, 0): the guard divides by max(b, 1e-5), it never NaNs.
    const int one = m.add(MatNodeType::ConstFloat, 0.25f);
    const int zero = m.add(MatNodeType::ConstFloat, 0.0f);
    const int div = m.add(MatNodeType::Divide);
    m.link(one, 0, div, 0); m.link(zero, 0, div, 1);
    const int sat = m.add(MatNodeType::Saturate);
    m.link(div, 0, sat, 0);
    m.link(sat, 0, m.out, HE::kMatOutputOpacityPin);

    HE::MatCpuProgram p;
    std::string err;
    REQUIRE_MESSAGE(p.compile(m.g, {}, nullptr, &err), err);
    const glm::vec4 c = run(p, { 0.25f, 0.9f });
    CHECK(near(c.r, 0.75f));
    CHECK(near(c.b, 0.25f));
    CHECK(near(c.a, 1.0f));          // 0.25 / 1e-5, saturated
}

TEST_CASE("CPU material: logic, Step, Smoothstep, Checker and If as the GPU computes them")
{
    G m;
    const int x = m.add(MatNodeType::ConstFloat, 0.3f);
    // Equal: |a-b| < 1e-4.
    const int y = m.add(MatNodeType::ConstFloat, 0.30005f);
    const int eq = m.add(MatNodeType::Equal);
    m.link(x, 0, eq, 0); m.link(y, 0, eq, 1);
    const int yes = m.add(MatNodeType::ConstColor, 0, 1, 0);
    const int no = m.add(MatNodeType::ConstColor, 1, 0, 0);
    const int iff = m.add(MatNodeType::If);
    m.link(eq, 0, iff, 0); m.link(yes, 0, iff, 1); m.link(no, 0, iff, 2);
    // Emissive.b = Checker(uv, 2): GLSL mod, so a negative cell is 0 or 1 too.
    const int chk = m.add(MatNodeType::Checker);
    const int sc = m.add(MatNodeType::ConstFloat, 2.0f);
    m.link(sc, 0, chk, 1);
    const int comb = m.add(MatNodeType::Combine3);
    m.link(chk, 0, comb, 2);
    m.link(iff, 0, m.out, HE::kMatOutputBaseColorPin);
    m.link(comb, 0, m.out, HE::kMatOutputEmissivePin);
    // Opacity = Smoothstep(0, 1, Step(0.5, 0.7)) = smoothstep(0, 1, 1) = 1.
    const int st = m.add(MatNodeType::Step);
    const int e = m.add(MatNodeType::ConstFloat, 0.5f), v = m.add(MatNodeType::ConstFloat, 0.7f);
    m.link(e, 0, st, 0); m.link(v, 0, st, 1);
    const int ss = m.add(MatNodeType::Smoothstep);
    m.link(st, 0, ss, 2);
    m.link(ss, 0, m.out, HE::kMatOutputOpacityPin);

    HE::MatCpuProgram p;
    REQUIRE(p.compile(m.g));
    const glm::vec4 a = run(p, { 0.1f, 0.1f });   // checker cell (0,0) → 0
    CHECK(near(a.g, 1.0f));
    CHECK(near(a.r, 0.0f));
    CHECK(near(a.b, 0.0f));
    CHECK(near(a.a, 1.0f));
    const glm::vec4 b = run(p, { 0.6f, 0.1f });   // cell (1,0) → 1
    CHECK(near(b.b, 1.0f));
}

TEST_CASE("CPU material: functions inline with their authored defaults; switches resolve")
{
    // f(In) = In * 2, In defaults to 0.25 when the caller leaves it unwired.
    MaterialGraph fn;
    const int in = fn.addNode(MatNodeType::FnInput);
    fn.findNode(in)->s = "In"; fn.findNode(in)->p[0] = 0; fn.findNode(in)->p[1] = 0.25f; fn.findNode(in)->p[2] = 1;
    const int two = fn.addNode(MatNodeType::ConstFloat);
    fn.findNode(two)->p[0] = 2.0f;
    const int mul = fn.addNode(MatNodeType::Multiply);
    REQUIRE(fn.connect(in, 0, mul, 0));
    REQUIRE(fn.connect(two, 0, mul, 1));
    const int fo = fn.addNode(MatNodeType::FnOutput);
    fn.findNode(fo)->s = "Out"; fn.findNode(fo)->p[0] = 0;
    REQUIRE(fn.connect(mul, 0, fo, 0));
    const HE::MatFunctionLoader loader = [&](const std::string& path) -> const MaterialGraph*
    { return path == "F.hasset" ? &fn : nullptr; };

    G m;
    const int call = m.add(MatNodeType::FunctionCall);
    m.g.findNode(call)->s = "F.hasset";
    // A switch named "Flip": on takes pin 0 (the call), off pin 1 (black).
    const int sw = m.add(MatNodeType::StaticSwitch, 1.0f);
    m.g.findNode(sw)->s = "Flip";
    m.link(call, 0, sw, 0);
    m.link(sw, 0, m.out, HE::kMatOutputBaseColorPin);

    HE::MatCpuProgram p;
    REQUIRE(p.compile(m.g, loader));
    CHECK(near(run(p).r, 0.5f));      // 0.25 * 2, splatted
    std::map<std::string, bool> off{ { "Flip", false } };
    REQUIRE(p.compile(m.g, loader, &off));
    CHECK(near(run(p).r, 0.0f));
}

TEST_CASE("CPU material: parameters come from the asset by name, Bool thresholded")
{
    G m;
    const int col = m.add(MatNodeType::ParamColor, 0.1f, 0.2f, 0.3f);
    m.g.findNode(col)->s = "Tint";
    const int b = m.add(MatNodeType::ParamBool, 0.0f);
    m.g.findNode(b)->s = "On";
    m.link(col, 0, m.out, HE::kMatOutputBaseColorPin);
    m.link(b, 0, m.out, HE::kMatOutputOpacityPin);

    HE::MatCpuProgram p;
    REQUIRE(p.compile(m.g));
    CHECK(near(run(p).g, 0.2f));    // authored default before anything is bound
    CHECK(near(run(p).a, 0.0f));
    p.bindParams({ "On", "Tint" }, { 0.7f, 0, 0, 0,   0.9f, 0.8f, 0.7f, 0 });
    CHECK(near(run(p).r, 0.9f));
    CHECK(near(run(p).a, 1.0f));     // 0.7 → step(0.5, 0.7)
}

TEST_CASE("CPU material: the element inputs, Masked discard, and refusing what needs a world")
{
    G m(HE::MatBlendMode::Masked);
    // Mask = Rounded Rect SDF's Mask with radius 20 on a 100x100 element: the
    // corner pixel is outside the rounding, the middle inside.
    const int rr = m.add(MatNodeType::RoundedRectSDF);
    const int r20 = m.add(MatNodeType::ConstFloat, 20.0f);
    m.link(r20, 0, rr, 0);
    m.link(rr, 1, m.out, HE::kMatOutputOpacityPin);
    const int st = m.add(MatNodeType::ElementState);
    const int c3 = m.add(MatNodeType::Combine3);
    m.link(st, 0, c3, 0);           // Hovered
    m.link(st, 3, c3, 2);           // Disabled
    m.link(c3, 0, m.out, HE::kMatOutputBaseColorPin);

    HE::MatCpuProgram p;
    REQUIRE(p.compile(m.g));
    HE::MatCpuElement e;
    e.size = { 100.0f, 100.0f };
    e.state = { 1.0f, 0.0f, 0.0f, 0.5f };
    bool discard = false;
    const glm::vec4 mid = p.eval(e, { 0.5f, 0.5f }, glm::vec3(1.0f), 0.0f, {}, discard);
    CHECK_FALSE(discard);
    CHECK(near(mid.r, 1.0f));
    CHECK(near(mid.b, 0.5f));
    p.eval(e, { 0.005f, 0.005f }, glm::vec3(1.0f), 0.0f, {}, discard);
    CHECK(discard);

    // A node that needs a mesh is refused by name, not drawn wrong.
    G w;
    const int fr = w.add(MatNodeType::Fresnel, 5.0f);
    w.link(fr, 0, w.out, HE::kMatOutputBaseColorPin);
    HE::MatCpuProgram q;
    std::string err;
    CHECK_FALSE(q.compile(w.g, {}, nullptr, &err));
    CHECK(err == "Fresnel");
    CHECK_FALSE(HE::matCpuSupports(MatNodeType::NormalWS));
    CHECK(HE::matCpuSupports(MatNodeType::Backdrop));
}

#ifdef HE_EDITOR_DEPS_DIR
TEST_CASE("CPU material: every shipped UI effect compiles for the CPU")
{
    const std::filesystem::path dir =
        std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent" / "MaterialFunctions";
    REQUIRE(std::filesystem::is_directory(dir));
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        if (entry.path().extension() == ".hasset") names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    REQUIRE(names.size() >= 9);

    // Load everything, then read: the next loadAsset moves every getter pointer.
    ContentManager cm(dir.string());
    std::vector<HE::UUID> ids;
    for (const std::string& f : names) ids.push_back(cm.loadAsset(f));
    std::map<std::string, MaterialGraph> fns;
    for (size_t i = 0; i < names.size(); ++i)
    {
        const MaterialFunctionAsset* fn = cm.getMaterialFunction(ids[i]);
        REQUIRE(fn);
        MaterialGraph g;
        REQUIRE(HE::materialGraphFromJson(fn->nodeGraphJson, g));
        fns.emplace(names[i], std::move(g));
    }
    const HE::MatFunctionLoader loader = [&](const std::string& path) -> const MaterialGraph*
    {
        const auto it = fns.find(path);
        return it == fns.end() ? nullptr : &it->second;
    };
    for (const std::string& name : names)
    {
        CAPTURE(name);
        G m;
        const int call = m.add(MatNodeType::FunctionCall);
        m.g.findNode(call)->s = name;
        m.link(call, 0, m.out, HE::kMatOutputBaseColorPin);
        HE::MatCpuProgram p;
        std::string err;
        CHECK_MESSAGE(p.compile(m.g, loader, nullptr, &err), err);
        if (name == "FrostedGlass.hasset") CHECK(p.usesBackdrop());
        if (name == "Sheen.hasset" || name == "Pulse.hasset") CHECK(p.usesTime());
    }
}
#endif
