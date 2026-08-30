#include "doctest.h"

#include <Backends/Software/SoftwareRaster.h>
#include <Renderer/UIFont.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetTree.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

// ═══ The software UI rasterizer ══════════════════════════════════════════════
// What this file is FOR: "Schicht 0" — rounded corners, borders, gradients,
// shadows — could until now only be checked by taking a screenshot on a machine
// with a GPU and looking at it. The CPU rasterizer is a plain function, so the
// same questions become assertions.
//
// The tests below therefore ask about PIXELS, not about fields: is the corner
// empty where it was rounded away, does the shadow put ink outside the shape,
// does the gradient run the way the angle says. A field test would have passed
// just as happily with a rasterizer that drew nothing.

using HE::sw::Image;

namespace
{
    struct Px { std::uint8_t r, g, b, a; };
    Px at(const Image& img, int x, int y)
    {
        Px p{};
        img.pixel(x, y, p.r, p.g, p.b, p.a);
        return p;
    }

    UIRenderObject solid(float x, float y, float w, float h, glm::vec4 c)
    {
        UIRenderObject o;
        o.position = { x, y };
        o.size     = { w, h };
        o.color    = c;
        o.type     = 0;
        return o;
    }

    Image canvas(int w, int h)
    {
        Image img;
        img.resize(w, h);
        img.clear(0, 0, 0, 255);
        return img;
    }
}

TEST_CASE("Software raster: the rounded-box distance agrees with the shaders")
{
    // The same numbers the offline simulation produced for the MSL/GLSL copy of
    // this formula. Three implementations, one set of expectations — that is the
    // only thing keeping them from drifting apart.
    const glm::vec4 all10(10.0f);
    CHECK(HE::sw::roundedBoxSDF(0.0f, 0.0f, 50.0f, 25.0f, all10) == doctest::Approx(-25.0f));
    CHECK(HE::sw::roundedBoxSDF(50.0f, 0.0f, 50.0f, 25.0f, all10) == doctest::Approx(0.0f));
    // A rounded corner pushes the corner POINT outside the shape.
    CHECK(HE::sw::roundedBoxSDF(-50.0f, -25.0f, 50.0f, 25.0f, all10)
          == doctest::Approx(4.142f).epsilon(0.001));

    // Per corner: only the quadrant a point is in decides its radius.
    const glm::vec4 topOnly(20.0f, 20.0f, 0.0f, 0.0f);   // TL, TR round; BR, BL square
    CHECK(HE::sw::roundedBoxSDF(-50.0f, -25.0f, 50.0f, 25.0f, topOnly)
          == doctest::Approx(8.284f).epsilon(0.001));
    CHECK(HE::sw::roundedBoxSDF(50.0f, 25.0f, 50.0f, 25.0f, topOnly)
          == doctest::Approx(0.0f));
}

TEST_CASE("Software raster: a plain quad fills its rectangle and nothing else")
{
    Image img = canvas(64, 64);
    HE::sw::draw(img, { solid(10.0f, 10.0f, 20.0f, 20.0f, { 1.0f, 0.0f, 0.0f, 1.0f }) });

    CHECK(at(img, 20, 20).r == 255);          // inside
    CHECK(at(img, 20, 20).g == 0);
    CHECK(at(img, 5, 20).r == 0);             // left of it
    CHECK(at(img, 35, 20).r == 0);            // right of it
    CHECK(at(img, 20, 5).r == 0);             // above
    CHECK(at(img, 20, 35).r == 0);            // below
}

TEST_CASE("Software raster: a corner radius empties the corner")
{
    Image img = canvas(64, 64);
    UIRenderObject o = solid(8.0f, 8.0f, 48.0f, 48.0f, { 1.0f, 1.0f, 1.0f, 1.0f });
    o.cornerRadius = glm::vec4(16.0f);
    HE::sw::draw(img, { o });

    CHECK(at(img, 32, 32).r == 255);          // the middle is filled
    CHECK(at(img, 9, 9).r == 0);              // every corner is cut away
    CHECK(at(img, 54, 9).r == 0);
    CHECK(at(img, 9, 54).r == 0);
    CHECK(at(img, 54, 54).r == 0);

    // Per corner: rounded at the top, square at the bottom — a tab.
    Image tab = canvas(64, 64);
    o.cornerRadius = { 16.0f, 16.0f, 0.0f, 0.0f };
    HE::sw::draw(tab, { o });
    CHECK(at(tab, 9, 9).r == 0);              // top corners still gone…
    CHECK(at(tab, 54, 9).r == 0);
    CHECK(at(tab, 9, 54).r == 255);           // …bottom ones filled to the point
    CHECK(at(tab, 54, 54).r == 255);
}

TEST_CASE("Software raster: a gradient runs the way its angle says")
{
    // 0° is straight down: the element's own colour at the top, the second at
    // the bottom. The same convention the two shaders use.
    Image down = canvas(64, 64);
    UIRenderObject o = solid(0.0f, 0.0f, 64.0f, 64.0f, { 0.0f, 0.0f, 0.0f, 1.0f });
    o.gradient      = true;
    o.gradientColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    HE::sw::draw(down, { o });
    CHECK(at(down, 32, 2).r < 40);
    CHECK(at(down, 32, 61).r > 215);
    CHECK(at(down, 2, 32).r == at(down, 61, 32).r);   // nothing happens across x

    // 90° is left to right.
    Image right = canvas(64, 64);
    o.gradientAngleDeg = 90.0f;
    HE::sw::draw(right, { o });
    CHECK(at(right, 2, 32).r < 40);
    CHECK(at(right, 61, 32).r > 215);
    CHECK(at(right, 32, 2).r == at(right, 32, 61).r);

    // Radial: the element's colour in the middle, the second at the corners.
    Image radial = canvas(64, 64);
    o.gradientAngleDeg = 0.0f;
    o.gradientShape    = 1;
    HE::sw::draw(radial, { o });
    CHECK(at(radial, 32, 32).r < 20);
    CHECK(at(radial, 1, 1).r > 235);
    // Symmetric about the centre, which a linear fade never is.
    CHECK(at(radial, 2, 32).r == at(radial, 61, 32).r);
    CHECK(at(radial, 32, 2).r == at(radial, 32, 61).r);
}

TEST_CASE("Software raster: a border is a ring inside the shape")
{
    Image img = canvas(64, 64);
    UIRenderObject o = solid(8.0f, 8.0f, 48.0f, 48.0f, { 0.0f, 0.0f, 1.0f, 1.0f });
    o.borderWidth = 4.0f;
    o.borderColor = { 1.0f, 0.0f, 0.0f, 1.0f };
    HE::sw::draw(img, { o });

    CHECK(at(img, 32, 10).r > 200);           // on the rim: the border's red
    CHECK(at(img, 32, 10).b < 60);
    CHECK(at(img, 32, 32).b > 200);           // in the middle: the fill's blue
    CHECK(at(img, 32, 32).r < 60);
    CHECK(at(img, 32, 6).a == 255);           // …and nothing OUTSIDE the rect
    CHECK(at(img, 32, 6).r == 0);
    CHECK(at(img, 32, 6).b == 0);
}

TEST_CASE("Software raster: a drop shadow puts ink outside the shape")
{
    // A shadow is an ordinary quad with a soft edge: the producer grows the rect
    // by the blur and offsets it, and the shader measures the shape against a box
    // inset by that much. Which means ink lands where the element itself is not.
    Image img = canvas(80, 80);
    UIRenderObject sh = solid(20.0f - 8.0f, 24.0f - 8.0f, 40.0f + 16.0f, 32.0f + 16.0f,
                              { 1.0f, 1.0f, 1.0f, 1.0f });
    sh.blur = 8.0f;
    HE::sw::draw(img, { sh });

    // Just under the shape's own bottom edge (y = 56) there is ink…
    CHECK(at(img, 40, 59).r > 20);
    // …it fades with distance…
    CHECK(at(img, 40, 59).r > at(img, 40, 63).r);
    // …and it is gone well past the blur.
    CHECK(at(img, 40, 70).r == 0);
    // The middle is solid.
    CHECK(at(img, 40, 40).r > 240);
}

TEST_CASE("Software raster: an inner shadow darkens the rim, not the middle")
{
    Image img = canvas(64, 64);
    UIRenderObject o = solid(8.0f, 8.0f, 48.0f, 48.0f, { 1.0f, 1.0f, 1.0f, 1.0f });
    o.innerShadowBlur  = 10.0f;
    o.innerShadowColor = { 0.0f, 0.0f, 0.0f, 1.0f };
    HE::sw::draw(img, { o });

    CHECK(at(img, 32, 32).r > 240);           // the middle keeps its colour
    CHECK(at(img, 32, 10).r < 120);           // the rim is darkened
    CHECK(at(img, 32, 10).r < at(img, 32, 16).r);   // and it fades inwards
}

TEST_CASE("Software raster: a clip rectangle cuts a quad down")
{
    Image img = canvas(64, 64);
    UIRenderObject o = solid(0.0f, 0.0f, 64.0f, 64.0f, { 1.0f, 1.0f, 1.0f, 1.0f });
    o.clipRect = { 0.0f, 0.0f, 32.0f, 64.0f };   // left half only
    HE::sw::draw(img, { o });

    CHECK(at(img, 10, 32).r == 255);
    CHECK(at(img, 50, 32).r == 0);

    // The caller's own clip narrows it further — that is how a partial redraw
    // says "only this region", without touching the object list.
    Image part = canvas(64, 64);
    HE::sw::draw(part, { solid(0.0f, 0.0f, 64.0f, 64.0f, { 1.0f, 1.0f, 1.0f, 1.0f }) },
                 glm::vec4(0.0f, 0.0f, 16.0f, 16.0f));
    CHECK(at(part, 8, 8).r == 255);
    CHECK(at(part, 24, 8).r == 0);
    CHECK(at(part, 8, 24).r == 0);
}

TEST_CASE("Software raster: a rotated quad lands where it was turned to")
{
    // 90° about the quad's own centre: a wide bar becomes a tall one. The bug
    // this guards is walking the UNROTATED bounds, which cuts the turned corners
    // off — the whole reason the rasterizer walks the box around all four.
    Image img = canvas(64, 64);
    UIRenderObject o = solid(12.0f, 28.0f, 40.0f, 8.0f, { 1.0f, 1.0f, 1.0f, 1.0f });
    o.rotation      = 3.14159265f * 0.5f;
    o.rotationPivot = { 32.0f, 32.0f };
    HE::sw::draw(img, { o });

    CHECK(at(img, 32, 14).r == 255);          // now tall
    CHECK(at(img, 32, 50).r == 255);
    CHECK(at(img, 14, 32).r == 0);            // and no longer wide
    CHECK(at(img, 50, 32).r == 0);
}

TEST_CASE("Software raster: text draws through the shared font atlas")
{
    // Glyph quads carry an atlas tile rather than a colour, so "did anything
    // draw" is the honest question here — the layout itself is UIFont's business
    // and is covered there.
    const HE::BakedUIFont& f = HE::sharedUIFont();
    REQUIRE(f.ok);

    HE::UITextLayout opts;
    std::vector<UIRenderObject> glyphs;
    HE::emitUITextGlyphs(f, 0, "Hello", { 4.0f, 4.0f }, { 200.0f, 40.0f }, 28.0f,
                         glm::vec4(1.0f), 0, opts, glyphs);
    REQUIRE_FALSE(glyphs.empty());

    Image img = canvas(220, 48);
    HE::sw::draw(img, glyphs);
    int inked = 0;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
            if (at(img, x, y).r > 30) ++inked;
    CHECK(inked > 100);
}

// ── Dirty rectangles ─────────────────────────────────────────────────────────
// A full 4K frame is 8.3 million pixels. Event-driven drawing keeps the idle
// case free, but dragging a slider repaints sixty times a second, and repainting
// a whole window each time is the difference between smooth and treacle.
TEST_CASE("Dirty rects: an unchanged frame is no work at all")
{
    std::vector<UIRenderObject> a{
        solid(10.0f, 10.0f, 40.0f, 20.0f, { 1.0f, 0.0f, 0.0f, 1.0f }),
        solid(80.0f, 60.0f, 30.0f, 30.0f, { 0.0f, 1.0f, 0.0f, 1.0f }) };
    std::vector<glm::vec4> rects;
    CHECK(HE::sw::dirtyRects(a, a, 200, 200, rects));
    CHECK(rects.empty());        // nothing to repaint, not even one pixel
}

TEST_CASE("Dirty rects: only what moved, and where it moved FROM")
{
    std::vector<UIRenderObject> before{
        solid(10.0f, 10.0f, 20.0f, 20.0f, { 1.0f, 0.0f, 0.0f, 1.0f }),
        solid(150.0f, 150.0f, 20.0f, 20.0f, { 0.0f, 1.0f, 0.0f, 1.0f }) };
    std::vector<UIRenderObject> after = before;
    after[0].position = { 40.0f, 10.0f };     // the first one slid right

    std::vector<glm::vec4> rects;
    REQUIRE(HE::sw::dirtyRects(before, after, 200, 200, rects));
    REQUIRE(rects.size() == 1);
    // The union of where it WAS and where it IS — repainting only the new spot
    // would leave the old one standing on screen.
    CHECK(rects[0].x <= 10.0f);
    CHECK(rects[0].y <= 10.0f);
    CHECK(rects[0].x + rects[0].z >= 60.0f);
    // …and the untouched quad down at (150,150) is not in it.
    CHECK(rects[0].y + rects[0].w < 150.0f);
}

TEST_CASE("Dirty rects: a shadow's falloff counts as touched")
{
    // The bounds a shadow quad occupies are wider than the shape it belongs to,
    // because the producer grows the rect by the blur. Missing that leaves a
    // ghost rim behind when the shadow moves.
    UIRenderObject sh = solid(50.0f, 50.0f, 40.0f, 40.0f, { 0.0f, 0.0f, 0.0f, 0.5f });
    sh.blur = 12.0f;
    const glm::vec4 b = HE::sw::quadBounds(sh);
    CHECK(b.z >= 40.0f);
    CHECK(b.w >= 40.0f);

    // Rotation, likewise: a turned quad's corners swing out past its rectangle.
    UIRenderObject rotated = solid(40.0f, 48.0f, 40.0f, 4.0f, { 1.0f, 1.0f, 1.0f, 1.0f });
    rotated.rotation = 3.14159265f * 0.25f;
    rotated.rotationPivot = { 60.0f, 50.0f };
    const glm::vec4 rb = HE::sw::quadBounds(rotated);
    CHECK(rb.w > 20.0f);          // it is much taller than its own 4 px now
}

TEST_CASE("Dirty rects: a big enough change gives up and says so")
{
    // Past half the window the bookkeeping costs more than the repaint, and the
    // caller is told to paint everything rather than handed forty rectangles.
    std::vector<UIRenderObject> before{ solid(0.0f, 0.0f, 10.0f, 10.0f, glm::vec4(1.0f)) };
    std::vector<UIRenderObject> after{ solid(0.0f, 0.0f, 200.0f, 200.0f, glm::vec4(1.0f)) };
    std::vector<glm::vec4> rects;
    CHECK_FALSE(HE::sw::dirtyRects(before, after, 200, 200, rects));
    CHECK(rects.empty());
}

TEST_CASE("Dirty rects: repainting only them gives the same picture")
{
    // The one that matters: a partial redraw has to be indistinguishable from a
    // full one, or the optimisation is a bug generator.
    std::vector<UIRenderObject> before{
        solid(10.0f, 10.0f, 30.0f, 30.0f, { 1.0f, 0.0f, 0.0f, 1.0f }),
        solid(70.0f, 70.0f, 30.0f, 30.0f, { 0.0f, 0.0f, 1.0f, 1.0f }) };
    std::vector<UIRenderObject> after = before;
    after[0].color = { 0.0f, 1.0f, 0.0f, 1.0f };
    after[0].cornerRadius = glm::vec4(8.0f);

    Image full = canvas(128, 128);
    HE::sw::draw(full, after);

    // Start from the OLD picture, repaint only the dirty region, and the two
    // must agree pixel for pixel.
    Image incremental = canvas(128, 128);
    HE::sw::draw(incremental, before);
    std::vector<glm::vec4> rects;
    REQUIRE(HE::sw::dirtyRects(before, after, 128, 128, rects));
    REQUIRE_FALSE(rects.empty());
    for (const glm::vec4& r : rects)
    {
        incremental.clearRect(r, 0, 0, 0, 255);
        HE::sw::draw(incremental, after, r);
    }
    CHECK(incremental.rgba == full.rgba);
}

// ── The witness sheet ────────────────────────────────────────────────────────
// The same twelve tiles HE_DUMP_UITEST puts on a real GPU, drawn on the CPU and
// written next to the test binary. It is a test in the sense that it must not
// crash and must put ink on the page; it is a TOOL in the sense that the file it
// leaves behind can be looked at without a machine that has a display.
TEST_CASE("Software raster: the Schicht 0 witness sheet draws")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 1280.0f; t.canvasHeight = 720.0f;
    int col = 0, row = 0;
    auto tile = [&](void (*style)(HE::UIElement&))
    {
        const int id = t.add(HE::UIWidgetType::Panel);
        HE::UIElement& e = *t.find(id);
        HE::uiSetAnchorPreset(e, 0);
        e.pivotX = e.pivotY = 0.0f;
        e.posX = 60.0f + static_cast<float>(col) * 300.0f;
        e.posY = 80.0f + static_cast<float>(row) * 170.0f;
        e.sizeX = 240.0f; e.sizeY = 110.0f;
        e.setProp("Color", HE::UIPropValue::ofColor({ 0.26f, 0.30f, 0.38f, 1.0f }));
        style(e);
        if (++col == 4) { col = 0; ++row; }
    };
    tile([](HE::UIElement&){});
    tile([](HE::UIElement& e){ e.cornerRadius = glm::vec4(24.0f); });
    tile([](HE::UIElement& e){ e.cornerRadius = { 28.0f, 28.0f, 0.0f, 0.0f }; });
    tile([](HE::UIElement& e){ e.cornerRadius = { 30.0f, 0.0f, 30.0f, 0.0f }; });
    tile([](HE::UIElement& e){ e.cornerRadius = glm::vec4(16.0f); e.borderWidth = 4.0f;
                               e.borderColor = glm::vec4(1.0f, 0.72f, 0.20f, 1.0f); });
    tile([](HE::UIElement& e){ e.cornerRadius = glm::vec4(16.0f); e.gradient = true;
                               e.gradientColor = glm::vec4(0.90f, 0.35f, 0.25f, 1.0f); });
    tile([](HE::UIElement& e){ e.cornerRadius = glm::vec4(16.0f); e.gradient = true;
                               e.gradientShape = 1;
                               e.gradientColor = glm::vec4(0.10f, 0.10f, 0.14f, 1.0f); });
    tile([](HE::UIElement& e){ e.cornerRadius = glm::vec4(16.0f); e.gradient = true;
                               e.gradientAngle = 90.0f;
                               e.gradientColor = glm::vec4(0.25f, 0.70f, 0.45f, 1.0f); });
    tile([](HE::UIElement& e){ e.cornerRadius = glm::vec4(16.0f); e.shadow = true;
                               e.shadowBlur = 18.0f; e.shadowOffsetY = 8.0f; });
    tile([](HE::UIElement& e){ e.cornerRadius = glm::vec4(16.0f); e.innerShadow = true;
                               e.innerShadowBlur = 14.0f; });
    tile([](HE::UIElement& e){ e.cornerRadius = { 30.0f, 4.0f, 30.0f, 4.0f };
                               e.shadow = true; e.shadowBlur = 14.0f;
                               e.shadowOffsetX = 6.0f; e.shadowOffsetY = 6.0f;
                               e.borderWidth = 2.0f;
                               e.borderColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.55f); });
    tile([](HE::UIElement& e){ e.cornerRadius = glm::vec4(55.0f); e.gradient = true;
                               e.gradientShape = 1;
                               e.gradientColor = glm::vec4(0.55f, 0.20f, 0.65f, 1.0f);
                               e.innerShadow = true; e.innerShadowBlur = 20.0f; });

    // The tree, turned into quads by the same code the runtime uses — including
    // the surface stamp, which is where the radii, the border, the gradient and
    // the inner shadow are actually attached, and the shadow quad in front.
    HE::uiApplyAutoSize(t, nullptr);
    std::vector<UIRenderObject> quads;
    for (const auto& ep : t.elements)
    {
        const HE::UIElement& e = *ep;
        const HE::UIWidgetRect r = HE::uiElementRect(t, e, nullptr);
        if (e.shadow)
        {
            UIRenderObject sh;
            sh.position = { r.x + e.shadowOffsetX - e.shadowBlur,
                            r.y + e.shadowOffsetY - e.shadowBlur };
            sh.size = { r.w + 2.0f * e.shadowBlur, r.h + 2.0f * e.shadowBlur };
            sh.color = e.shadowColor;
            sh.cornerRadius = e.cornerRadius;
            sh.blur = e.shadowBlur;
            quads.push_back(sh);
        }
        const std::size_t first = quads.size();
        e.render(r, HE::UIElementRenderState{}, HE::UUID{}, 1.0f, quads);
        if (quads.size() > first)
        {
            UIRenderObject& q = quads[first];
            q.cornerRadius = e.cornerRadius;
            q.borderWidth  = e.borderWidth;
            q.borderColor  = e.borderColor;
            q.gradient     = e.gradient;
            q.gradientColor    = e.gradientColor;
            q.gradientAngleDeg = e.gradientAngle;
            q.gradientShape    = e.gradientShape;
            if (e.innerShadow)
            {
                q.innerShadowBlur  = e.innerShadowBlur;
                q.innerShadowColor = e.innerShadowColor;
            }
        }
    }
    REQUIRE(quads.size() >= 12);

    Image img = canvas(1280, 720);
    img.clear(30, 34, 42, 255);
    HE::sw::draw(img, quads);

    // Ink on the page, and specifically in the places the styles put it.
    // Tiles are 240x110 at x = 60 + 300*col, y = 80 + 170*row.
    CHECK(at(img, 62, 82).r > 50);                    // tile 1 is square: its corner is FILLED
    CHECK(at(img, 362, 82).r == 30);                  // tile 2 is round: the same corner is empty
    CHECK(at(img, 962, 82).r == 30);                  // tile 4 (leaf): top-left rounded away…
    CHECK(at(img, 1197, 82).r > 50);                  // …top-right square, so filled

    // Left behind for a human (or for me) to look at.
    const std::filesystem::path out =
        std::filesystem::temp_directory_path() / "he_schicht0_software.ppm";
    if (FILE* f = std::fopen(out.string().c_str(), "wb"))
    {
        std::fprintf(f, "P6\n%d %d\n255\n", img.width, img.height);
        for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4)
            std::fwrite(&img.rgba[i], 1, 3, f);
        std::fclose(f);
        MESSAGE("witness sheet written to " << out.string());
    }
}

// ── Light against dark, in pixels ────────────────────────────────────────────
// A theme verified only by field asserts is a theme that could be resolving into
// a widget nobody draws. This renders the SAME tree twice, in the two modes, and
// asks the images whether they differ — and in which direction.
TEST_CASE("Theme: the same widgets come out light or dark")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 640.0f; t.canvasHeight = 200.0f;
    // A page, a card on it, and a line of text — the three roles every screen
    // uses, and nothing else.
    auto tile = [&](HE::UIWidgetType type, const char* role, float x, float y,
                    float w, float h)
    {
        const int id = t.add(type);
        HE::UIElement& e = *t.find(id);
        HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
        e.posX = x; e.posY = y; e.sizeX = w; e.sizeY = h;
        e.setThemeRole("Color", role);
        return id;
    };
    tile(HE::UIWidgetType::Panel, "Background", 0.0f, 0.0f, 640.0f, 200.0f);
    tile(HE::UIWidgetType::Panel, "Surface",   40.0f, 40.0f, 260.0f, 120.0f);
    tile(HE::UIWidgetType::Panel, "Accent",   340.0f, 40.0f, 260.0f, 120.0f);

    auto shoot = [&](HE::UIThemeMode mode)
    {
        HE::uiApplyTheme(t, HE::uiDefaultTheme(), mode);
        std::vector<UIRenderObject> quads;
        for (const auto& ep : t.elements)
        {
            const HE::UIWidgetRect r = HE::uiElementRect(t, *ep, nullptr);
            const std::size_t first = quads.size();
            ep->render(r, HE::UIElementRenderState{}, HE::UUID{}, 1.0f, quads);
            if (quads.size() > first) quads[first].cornerRadius = ep->cornerRadius;
        }
        Image img = canvas(640, 200);
        HE::sw::draw(img, quads);
        return img;
    };

    const Image light = shoot(HE::UIThemeMode::Light);
    const Image dark  = shoot(HE::UIThemeMode::Dark);

    // The two are not the same picture…
    CHECK(light.rgba != dark.rgba);
    // …and they are the way round they say they are: the page is bright in one
    // and near-black in the other, which is the one thing a swapped light/dark
    // table would get wrong while every field assert still passed.
    CHECK(at(light, 320, 10).r > 200);
    CHECK(at(dark,  320, 10).r < 60);
    // The card sits on the page in both, and is distinguishable from it.
    CHECK(at(light, 100, 100).r != at(light, 320, 10).r);
    CHECK(at(dark,  100, 100).r != at(dark,  320, 10).r);

    const std::filesystem::path out =
        std::filesystem::temp_directory_path() / "he_theme_light_dark.ppm";
    if (FILE* f = std::fopen(out.string().c_str(), "wb"))
    {
        // The two side by side, so one look answers "does this read".
        std::fprintf(f, "P6\n%d %d\n255\n", light.width, light.height * 2);
        for (const Image* img : { &light, &dark })
            for (std::size_t i = 0; i + 3 < img->rgba.size(); i += 4)
                std::fwrite(&img->rgba[i], 1, 3, f);
        std::fclose(f);
        MESSAGE("light/dark sheet written to " << out.string());
    }
}
