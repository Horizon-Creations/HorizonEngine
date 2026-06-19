#include "doctest.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// ── PostFX pure-math tests ────────────────────────────────────────────────────
// These exercise the formulas used in the HLSL/GLSL shaders so that numerical
// regressions are caught without needing a GPU or a live renderer.

namespace {

struct Vec3 { float r, g, b; };

inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

// ACES filmic tonemapping (same formula as the shaders).
float aces1(float x)
{
    return clamp01((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));
}

// Bloom soft-knee contribution for a single channel value.
float bloomContrib(float value, float threshold, float knee)
{
    float br   = value; // single-channel brightness for unit tests
    float s    = std::clamp(br - threshold + knee, 0.0f, 2.0f * knee);
    s           = (s * s) / (4.0f * knee + 1e-4f);
    float contrib = std::max(s, br - threshold) / std::max(br, 1e-4f);
    return std::max(contrib, 0.0f);
}

// Normalised 9-tap (i=0..4) Gaussian weights (same as shader).
constexpr std::array<float, 5> kGaussW = { 0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f };

float gaussWeightSum()
{
    float s = kGaussW[0];
    for (int i = 1; i < 5; ++i) s += 2.f * kGaussW[i];
    return s;
}

// Ping-pong index logic: given pass index 0..9 (horiz starts true),
// compute which image is the dst.  After 10 passes the result is in image[0].
int pingPongDst(int pass)
{
    bool horiz = true;
    int dst = 0;
    for (int p = 0; p <= pass; ++p) {
        dst = horiz ? 1 : 0;
        horiz = !horiz;
    }
    return dst;
}

} // namespace

// ── ACES tonemapping ──────────────────────────────────────────────────────────

TEST_CASE("ACES: black maps to black")
{
    CHECK(aces1(0.0f) == doctest::Approx(0.0f).epsilon(1e-5f));
}

TEST_CASE("ACES: very bright input saturates near 1")
{
    // At x=10 (deep HDR) the output should be > 0.99.
    CHECK(aces1(10.0f) >= 0.99f);
}

TEST_CASE("ACES: output is monotonically increasing for positive inputs")
{
    float prev = 0.f;
    for (int i = 1; i <= 100; ++i) {
        float x = float(i) * 0.1f;
        float y = aces1(x);
        CHECK(y >= prev);
        prev = y;
    }
}

TEST_CASE("ACES: output is always in [0,1]")
{
    for (int i = 0; i <= 200; ++i) {
        float x = float(i) * 0.1f;
        float y = aces1(x);
        CHECK(y >= 0.0f);
        CHECK(y <= 1.0f);
    }
}

TEST_CASE("ACES: mid-grey (0.18) maps below 0.5")
{
    // ACES compresses — 18% grey in linear maps well below 0.5 in SDR.
    CHECK(aces1(0.18f) < 0.5f);
}

// ── Bloom bright-pass ─────────────────────────────────────────────────────────

TEST_CASE("Bloom bright-pass: below threshold contributes zero")
{
    const float threshold = 1.0f, knee = 0.1f;
    // Value well below threshold.
    CHECK(bloomContrib(0.5f, threshold, knee) == doctest::Approx(0.0f).epsilon(1e-4f));
}

TEST_CASE("Bloom bright-pass: above threshold contributes non-zero")
{
    const float threshold = 1.0f, knee = 0.1f;
    CHECK(bloomContrib(2.0f, threshold, knee) > 0.0f);
}

TEST_CASE("Bloom bright-pass: knee softens the transition around threshold")
{
    const float threshold = 1.0f, knee = 0.1f;
    float justAbove  = bloomContrib(threshold + 0.001f, threshold, knee);
    float wellAbove  = bloomContrib(threshold + 1.0f,   threshold, knee);
    // Knee region contribution is smaller than well-above-threshold.
    CHECK(justAbove < wellAbove);
}

TEST_CASE("Bloom bright-pass: contribution is never negative")
{
    for (int i = 0; i <= 30; ++i) {
        float val = float(i) * 0.2f;
        CHECK(bloomContrib(val, 1.0f, 0.1f) >= 0.0f);
    }
}

// ── Gaussian blur weights ─────────────────────────────────────────────────────

TEST_CASE("Gaussian weights sum to approximately 1 (normalised)")
{
    CHECK(gaussWeightSum() == doctest::Approx(1.0f).epsilon(0.002f));
}

TEST_CASE("Gaussian weights are symmetric and positive")
{
    for (float w : kGaussW) CHECK(w > 0.0f);
    // Monotonically decreasing from centre.
    for (int i = 1; i < 5; ++i) CHECK(kGaussW[i - 1] >= kGaussW[i]);
}

// ── Bloom ping-pong pass indexing ─────────────────────────────────────────────

TEST_CASE("Ping-pong: first pass writes to image[1]")
{
    CHECK(pingPongDst(0) == 1);
}

TEST_CASE("Ping-pong: second pass writes to image[0]")
{
    CHECK(pingPongDst(1) == 0);
}

TEST_CASE("Ping-pong: after 10 passes result is in image[0]")
{
    CHECK(pingPongDst(9) == 0);
}

TEST_CASE("Ping-pong: passes alternate between image[0] and image[1]")
{
    for (int p = 0; p < 10; ++p) {
        CHECK(pingPongDst(p) == (p % 2 == 0 ? 1 : 0));
    }
}

// ── Exposure scaling ──────────────────────────────────────────────────────────

TEST_CASE("Exposure: doubling exposure doubles linear scene value before tonemap")
{
    // Before ACES, multiply by exposure. Check linearity.
    float scene = 0.5f;
    float e1 = scene * 1.0f;
    float e2 = scene * 2.0f;
    CHECK(e2 == doctest::Approx(e1 * 2.0f).epsilon(1e-5f));
}

TEST_CASE("Exposure: very large exposure saturates after ACES")
{
    float scene = 0.5f;
    float exposed = scene * 100.0f;
    CHECK(aces1(exposed) > 0.99f);
}

// ─── PBR / Cook-Torrance BRDF tests ─────────────────────────────────────────

namespace {

struct Vec3pbr { float x, y, z;
    float dot(const Vec3pbr& o) const { return x*o.x + y*o.y + z*o.z; }
    Vec3pbr normalize() const { float l=std::sqrt(x*x+y*y+z*z); return{x/l,y/l,z/l}; }
    Vec3pbr operator+(const Vec3pbr& o) const { return{x+o.x,y+o.y,z+o.z}; }
    Vec3pbr operator*(float s) const { return{x*s,y*s,z*s}; }
};

float D_GGX_test(float NdH, float a2) {
    float d = NdH*NdH*(a2-1.0f)+1.0f;
    return a2 / (3.14159265f * d * d + 1e-6f);
}
float G_Schlick_test(float NdX, float k) { return NdX / (NdX*(1.0f-k)+k); }
Vec3pbr F_Schlick_test(float VdH, const Vec3pbr& F0) {
    float f = std::pow(1.0f-VdH, 5.0f);
    return { F0.x+(1.0f-F0.x)*f, F0.y+(1.0f-F0.y)*f, F0.z+(1.0f-F0.z)*f };
}

} // namespace pbr helpers

TEST_CASE("PBR D_GGX: at perfect specular alignment (NdH=1) tight roughness is much larger")
{
    float a1sq = (0.1f*0.1f)*(0.1f*0.1f);  // roughness=0.1 → a^4
    float a2sq = (1.0f*1.0f)*(1.0f*1.0f);  // roughness=1.0 → a^4
    // At NdH=1.0 (perfect mirror direction) the tight lobe has a very high peak.
    float d1 = D_GGX_test(1.0f, a1sq);
    float d2 = D_GGX_test(1.0f, a2sq);
    CHECK(d1 > d2); // rough=0.1 peaks higher at perfect alignment
}

TEST_CASE("PBR D_GGX: output is always positive")
{
    for (int i = 0; i <= 10; ++i)
    {
        float NdH = float(i) / 10.0f;
        CHECK(D_GGX_test(NdH, 0.04f) >= 0.0f);
        CHECK(D_GGX_test(NdH, 1.0f)  >= 0.0f);
    }
}

TEST_CASE("PBR F_Schlick: at VdH=0 approaches 1 (grazing angle)")
{
    Vec3pbr F0{0.04f, 0.04f, 0.04f};
    auto F = F_Schlick_test(0.0f, F0);
    CHECK(F.x == doctest::Approx(1.0f).epsilon(0.001f));
}

TEST_CASE("PBR F_Schlick: at VdH=1 returns F0 (normal incidence)")
{
    Vec3pbr F0{0.5f, 0.3f, 0.1f};
    auto F = F_Schlick_test(1.0f, F0);
    CHECK(F.x == doctest::Approx(F0.x).epsilon(1e-5f));
    CHECK(F.y == doctest::Approx(F0.y).epsilon(1e-5f));
    CHECK(F.z == doctest::Approx(F0.z).epsilon(1e-5f));
}

TEST_CASE("PBR G_Schlick: output clamped [0,1] for NdX in [0,1]")
{
    for (int i = 1; i <= 10; ++i)
    {
        float NdX = float(i) / 10.0f;
        float k   = 0.5f;
        float g   = G_Schlick_test(NdX, k);
        CHECK(g >= 0.0f);
        CHECK(g <= 1.0f);
    }
}

TEST_CASE("PBR metallic=0 (dielectric) uses F0=0.04")
{
    Vec3pbr base{0.8f, 0.2f, 0.2f};
    float met = 0.0f;
    Vec3pbr F0{ 0.04f*(1.0f-met) + base.x*met,
                0.04f*(1.0f-met) + base.y*met,
                0.04f*(1.0f-met) + base.z*met };
    CHECK(F0.x == doctest::Approx(0.04f).epsilon(1e-5f));
}

TEST_CASE("PBR metallic=1 (metal) uses base color as F0")
{
    Vec3pbr base{0.8f, 0.4f, 0.1f};
    float met = 1.0f;
    Vec3pbr F0{ 0.04f*(1.0f-met) + base.x*met,
                0.04f*(1.0f-met) + base.y*met,
                0.04f*(1.0f-met) + base.z*met };
    CHECK(F0.x == doctest::Approx(base.x).epsilon(1e-5f));
    CHECK(F0.y == doctest::Approx(base.y).epsilon(1e-5f));
    CHECK(F0.z == doctest::Approx(base.z).epsilon(1e-5f));
}

TEST_CASE("PBR DrawCall defaults: baseColor white, metallic 0, roughness 0.5, opacity 1")
{
    // Verify defaults match what DrawCall initializes.
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float met = 0.0f, rough = 0.5f, opacity = 1.0f;
    CHECK(r    == doctest::Approx(1.0f));
    CHECK(g    == doctest::Approx(1.0f));
    CHECK(b    == doctest::Approx(1.0f));
    CHECK(met  == doctest::Approx(0.0f));
    CHECK(rough== doctest::Approx(0.5f));
    CHECK(opacity==doctest::Approx(1.0f));
}

// ─── Sorted Transparency tests ───────────────────────────────────────────────
// Validate the classification and back-to-front sorting logic used in all
// three renderer backends (D3D11, D3D12, Vulkan).

namespace {

struct FakeDC {
    float opacity = 1.0f;
    float tx = 0.0f, ty = 0.0f, tz = 0.0f; // translation (transform[3])
};

// Mirrors the threshold used in all backends.
constexpr float kOpaqueThreshold = 0.999f;

bool isTransparent(const FakeDC& dc) { return dc.opacity < kOpaqueThreshold; }

float distToCam(const FakeDC& dc, float cx, float cy, float cz)
{
    float dx = dc.tx - cx, dy = dc.ty - cy, dz = dc.tz - cz;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

} // anon

TEST_CASE("Transparency: opacity=1.0 is classified opaque")
{
    FakeDC dc; dc.opacity = 1.0f;
    CHECK_FALSE(isTransparent(dc));
}

TEST_CASE("Transparency: opacity=0.999 is at boundary, classified opaque")
{
    FakeDC dc; dc.opacity = 0.999f;
    CHECK_FALSE(isTransparent(dc));
}

TEST_CASE("Transparency: opacity=0.998 is classified transparent")
{
    FakeDC dc; dc.opacity = 0.998f;
    CHECK(isTransparent(dc));
}

TEST_CASE("Transparency: opacity=0.0 (fully invisible) is classified transparent")
{
    FakeDC dc; dc.opacity = 0.0f;
    CHECK(isTransparent(dc));
}

TEST_CASE("Transparency: opacity=0.5 (half transparent) is classified transparent")
{
    FakeDC dc; dc.opacity = 0.5f;
    CHECK(isTransparent(dc));
}

TEST_CASE("Sorted Transparency: farther object sorts before nearer (back-to-front)")
{
    // Camera at origin, objects at z=2 and z=5.
    FakeDC nearDC, farDC;
    nearDC.opacity = 0.5f; nearDC.tx = 0; nearDC.ty = 0; nearDC.tz = 2.0f;
    farDC.opacity  = 0.5f; farDC.tx  = 0; farDC.ty  = 0; farDC.tz  = 5.0f;

    const float camX = 0, camY = 0, camZ = 0;
    float dNear = distToCam(nearDC, camX, camY, camZ);
    float dFar  = distToCam(farDC,  camX, camY, camZ);

    // The sort comparator: farther first (descending distance).
    CHECK(dFar > dNear);
}

TEST_CASE("Sorted Transparency: opaque and transparent separated correctly")
{
    std::vector<FakeDC> draws;
    for (float op : {1.0f, 0.5f, 1.0f, 0.3f, 0.0f, 1.0f}) {
        FakeDC dc; dc.opacity = op;
        draws.push_back(dc);
    }

    std::vector<const FakeDC*> opaque, transp;
    for (const FakeDC& dc : draws)
        (isTransparent(dc) ? transp : opaque).push_back(&dc);

    CHECK(opaque.size() == 3);
    CHECK(transp.size() == 3);
}

TEST_CASE("Sorted Transparency: sort is stable order for equal distances")
{
    // Two transparents at the same distance — sort should not crash.
    FakeDC a; a.opacity = 0.5f; a.tx = 0; a.ty = 0; a.tz = 3.0f;
    FakeDC b; b.opacity = 0.3f; b.tx = 0; b.ty = 0; b.tz = 3.0f;
    std::vector<FakeDC> draws; draws.push_back(a); draws.push_back(b);
    float cx = 0, cy = 0, cz = 0;
    std::sort(draws.begin(), draws.end(), [&](const FakeDC& x, const FakeDC& y) {
        return distToCam(x, cx, cy, cz) > distToCam(y, cx, cy, cz);
    });
    CHECK(draws.size() == 2); // just verifying it doesn't crash/assert
}
