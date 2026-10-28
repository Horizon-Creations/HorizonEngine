#include "doctest.h"
#include <initializer_list>
#include <string>

#include <MaterialGraph/MaterialGraph.h>
#include <material/MaterialShaderLibrary.h>

// ─────────────────────────────────────────────────────────────────────────────
//  Real branching for the If node (Thema 235, Schritt 2).
//
//  If used to be mix(no, yes, step(0.5, cond)): both sides ran for every pixel.
//  Now a node needed by one side only, and made of plain arithmetic, runs inside
//  `if (cond >= 0.5) {...} else {...}`. These cases pin what may move into a
//  branch (arithmetic and noise, not textures: implicit-LOD sampling is undefined
//  in non-uniform control flow), what must stay outside (a node both sides or the
//  rest of the graph read), that Ifs on one condition share one block, and that
//  the text still cross-compiles.
// ─────────────────────────────────────────────────────────────────────────────

using HE::MaterialGraph;
using HE::MatNodeType;
using HE::MatGraphNode;

namespace
{

size_t countOf(const std::string& s, const std::string& what)
{
    size_t n = 0;
    for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + what.size())) ++n;
    return n;
}

// cond = CameraDistance > 30; yes = fbm noise as a colour; no = a constant.
struct Fixture
{
    MaterialGraph g;
    int out = 0, cond = 0, iff = 0, fbm = 0;
};

Fixture makeBranchGraph(MatNodeType yesType = MatNodeType::Fbm)
{
    Fixture f;
    f.out = f.g.addNode(MatNodeType::Output);
    const int dist  = f.g.addNode(MatNodeType::CameraDistance);
    const int limit = f.g.addNode(MatNodeType::ConstFloat);
    f.g.findNode(limit)->p[0] = 30.0f;
    f.cond = f.g.addNode(MatNodeType::Greater);
    CHECK(f.g.connect(dist, 0, f.cond, 0));
    CHECK(f.g.connect(limit, 0, f.cond, 1));
    f.fbm = f.g.addNode(yesType);
    const int no = f.g.addNode(MatNodeType::ConstColor);
    f.iff = f.g.addNode(MatNodeType::If);
    CHECK(f.g.connect(f.cond, 0, f.iff, 0));
    CHECK(f.g.connect(f.fbm, 0, f.iff, 1));
    CHECK(f.g.connect(no, 0, f.iff, 2));
    CHECK(f.g.connect(f.iff, 0, f.out, 0));   // BaseColor
    return f;
}

std::string mainBody(const std::string& glsl)
{
    const size_t m = glsl.find("void main()");
    return m == std::string::npos ? std::string() : glsl.substr(m);
}

} // namespace

TEST_CASE("material If: an arithmetic side runs inside a branch, not under mix()")
{
    const Fixture f = makeBranchGraph();
    const std::string body = mainBody(HE::generateFragment(f.g).glsl);
    REQUIRE_FALSE(body.empty());
    CHECK(body.find("step(0.5,") == std::string::npos);
    const size_t ifPos  = body.find("    if (");
    const size_t fbmPos = body.find("heFbm(");
    REQUIRE(ifPos != std::string::npos);
    REQUIRE(fbmPos != std::string::npos);
    CHECK(fbmPos > ifPos);   // the noise is computed only where the condition holds
    CHECK(countOf(body, "heFbm(") == 1);
}

TEST_CASE("material If: a texture side stays a mix() (no implicit derivatives in a branch)")
{
    const Fixture f = makeBranchGraph(MatNodeType::TextureSample);
    const std::string body = mainBody(HE::generateFragment(f.g).glsl);
    CHECK(body.find("step(0.5,") != std::string::npos);
    CHECK(body.find("    if (") == std::string::npos);
}

TEST_CASE("material If: a node that reads an implicit-LOD texture stays outside the branch")
{
    // yes = Multiply(TextureSample, 2): the multiply alone would be arithmetic, but its
    // only input is a texture read the driver could sink into the branch with it.
    Fixture f = makeBranchGraph(MatNodeType::Multiply);
    const int tex = f.g.addNode(MatNodeType::TextureSample);
    const int two = f.g.addNode(MatNodeType::ConstFloat);
    f.g.findNode(two)->p[0] = 2.0f;
    CHECK(f.g.connect(tex, 0, f.fbm, 0));
    CHECK(f.g.connect(two, 0, f.fbm, 1));
    const std::string body = mainBody(HE::generateFragment(f.g).glsl);
    CHECK(body.find("step(0.5,") != std::string::npos);
    CHECK(body.find("    if (") == std::string::npos);
}

TEST_CASE("material If: a node the rest of the graph also reads stays outside the branch")
{
    Fixture f = makeBranchGraph();
    // The fbm also drives Roughness, unconditionally.
    CHECK(f.g.connect(f.fbm, 0, f.out, 3));
    const std::string body = mainBody(HE::generateFragment(f.g).glsl);
    // Nothing is left to move: the If keeps its old mix().
    CHECK(body.find("step(0.5,") != std::string::npos);
    CHECK(body.find("    if (") == std::string::npos);
}

TEST_CASE("material If: two Ifs on one condition share one block, and a shared side node runs once")
{
    Fixture f = makeBranchGraph();
    // A second If on the same condition feeding Emissive, its yes side reading the
    // same fbm (so the fbm is still needed only where the condition holds).
    const int scale = f.g.addNode(MatNodeType::ConstFloat);
    f.g.findNode(scale)->p[0] = 0.5f;
    const int mul = f.g.addNode(MatNodeType::Multiply);
    CHECK(f.g.connect(f.fbm, 0, mul, 0));
    CHECK(f.g.connect(scale, 0, mul, 1));
    const int zero = f.g.addNode(MatNodeType::ConstColor);
    const int if2 = f.g.addNode(MatNodeType::If);
    CHECK(f.g.connect(f.cond, 0, if2, 0));
    CHECK(f.g.connect(mul, 0, if2, 1));
    CHECK(f.g.connect(zero, 0, if2, 2));
    CHECK(f.g.connect(if2, 0, f.out, 4));   // Emissive
    const std::string body = mainBody(HE::generateFragment(f.g).glsl);
    CHECK(countOf(body, "    if (") == 1);
    CHECK(countOf(body, "heFbm(") == 1);
    CHECK(body.find("step(0.5,") == std::string::npos);
}

TEST_CASE("material If: a graph without a movable side keeps its exact old text")
{
    // Both sides constant: no guarded node, so nothing changes for the shader cache.
    MaterialGraph g;
    const int out = g.addNode(MatNodeType::Output);
    const int c = g.addNode(MatNodeType::ConstFloat);
    const int a = g.addNode(MatNodeType::ConstColor), b = g.addNode(MatNodeType::ConstColor);
    const int iff = g.addNode(MatNodeType::If);
    CHECK(g.connect(c, 0, iff, 0));
    CHECK(g.connect(a, 0, iff, 1));
    CHECK(g.connect(b, 0, iff, 2));
    CHECK(g.connect(iff, 0, out, 0));
    // Constants may move into a branch, but alone they are no reason to open one.
    const std::string body = mainBody(HE::generateFragment(g).glsl);
    CHECK(body.find("step(0.5,") != std::string::npos);
    CHECK(body.find("    if (") == std::string::npos);
}

namespace
{
// cond = CameraDistance > 30; yes = a bombed array read (albedo or normal), no = a constant.
Fixture makeBombBranchGraph(MatNodeType bombType)
{
    Fixture f = makeBranchGraph(bombType);
    MatGraphNode* b = f.g.findNode(f.fbm);
    b->s = "Textures/T_Array.hasset";
    const int uv = f.g.addNode(MatNodeType::UV);
    CHECK(f.g.connect(uv, 0, f.fbm, 0));
    return f;
}
} // namespace

TEST_CASE("material If: a bombed read's taps move into the branch, its grid and gradients stay ahead")
{
    for (MatNodeType t : { MatNodeType::TextureArrayBombSample, MatNodeType::NormalMapArrayBombSample })
    {
        CAPTURE(static_cast<int>(t));
        const Fixture f = makeBombBranchGraph(t);
        const std::string body = mainBody(HE::generateFragment(f.g).glsl);
        const size_t ifPos   = body.find("    if (");
        const size_t gridPos = body.find("heBombGrid");
        const size_t dxPos   = body.find("dFdx(");
        const size_t tapPos  = body.find("textureGrad(");
        REQUIRE(ifPos != std::string::npos);
        REQUIRE(tapPos != std::string::npos);
        CHECK(gridPos < ifPos);
        CHECK(dxPos < ifPos);
        CHECK(tapPos > ifPos);
        // No screen derivative anywhere inside the branch.
        CHECK(body.find("dFdx(", ifPos) == std::string::npos);
        CHECK(body.find("dFdy(", ifPos) == std::string::npos);
        CHECK(body.find("\x01") == std::string::npos);
        if (t == MatNodeType::NormalMapArrayBombSample)
            CHECK(body.find("hePerturbNormalD(") > ifPos);
    }
}

TEST_CASE("material If: a bombed read outside any branch keeps its exact old text")
{
    Fixture f = makeBombBranchGraph(MatNodeType::NormalMapArrayBombSample);
    CHECK(f.g.connect(f.fbm, 0, f.out, 5));   // also the Normal, unconditionally
    const std::string glsl = HE::generateFragment(f.g).glsl;
    CHECK(glsl.find("hePerturbNormalD") == std::string::npos);
    CHECK(glsl.find("    if (") == std::string::npos);
}

#if defined(HE_TESTS_HAVE_SHADERC)
TEST_CASE("material If: branched fragments cross-compile for Metal, HLSL and GLSL 4.10")
{
    using B = HE::MaterialShaderLibrary::Backend;
    HE::MaterialShaderLibrary lib;
    Fixture a = makeBranchGraph();
    Fixture b = makeBranchGraph();
    CHECK(b.g.connect(b.fbm, 0, b.out, 3));
    const Fixture c = makeBombBranchGraph(MatNodeType::TextureArrayBombSample);
    const Fixture d = makeBombBranchGraph(MatNodeType::NormalMapArrayBombSample);
    for (const MaterialGraph* g : std::initializer_list<const MaterialGraph*>{ &a.g, &b.g, &c.g, &d.g })
    {
        const std::string glsl = HE::generateFragment(*g).glsl;
        const uint64_t hash = std::hash<std::string>{}(glsl);
        for (B be : { B::Metal, B::HLSL, B::GLSL410 })
        {
            const auto& r = lib.fragment(hash ^ static_cast<uint64_t>(be), glsl, be);
            CHECK_MESSAGE(r.ok, "backend ", static_cast<int>(be), ": ", r.log);
        }
    }
}
#endif
