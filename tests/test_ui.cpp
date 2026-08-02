#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/UISystem.h>
#include <Renderer/UIFont.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/UICanvasComponent.h>
#include <HorizonScene/Components/UIElementComponent.h>
#include <HorizonScene/Components/UITextComponent.h>
#include <HorizonScene/Components/UIImageComponent.h>
#include <HorizonScene/Components/UIButtonComponent.h>

// ── Component default values ─────────────────────────────────────────────────

TEST_CASE("UICanvasComponent defaults")
{
    UICanvasComponent c;
    CHECK(c.width  == doctest::Approx(1920.0f));
    CHECK(c.height == doctest::Approx(1080.0f));
    CHECK(c.renderMode == UIRenderMode::ScreenSpace);
    CHECK(c.active);
}

TEST_CASE("UIElementComponent defaults")
{
    UIElementComponent e;
    CHECK(e.position.x == doctest::Approx(0.0f));
    CHECK(e.position.y == doctest::Approx(0.0f));
    CHECK(e.size.x     == doctest::Approx(100.0f));
    CHECK(e.size.y     == doctest::Approx(30.0f));
    CHECK(e.pivot.x    == doctest::Approx(0.5f));
    CHECK(e.pivot.y    == doctest::Approx(0.5f));
    CHECK(e.rotation   == doctest::Approx(0.0f));
    CHECK(e.anchor     == UIAnchor::TopLeft);
    CHECK(e.layer      == 0);
    CHECK(e.active);
}

TEST_CASE("UITextComponent defaults")
{
    UITextComponent t;
    CHECK(t.text     == "Text");
    CHECK(t.fontSize == doctest::Approx(14.0f));
    CHECK(t.color.r  == doctest::Approx(1.0f));
    CHECK(t.color.a  == doctest::Approx(1.0f));
}

TEST_CASE("UIButtonComponent defaults")
{
    UIButtonComponent b;
    CHECK(b.state          == UIButtonState::Normal);
    CHECK(b.normalColor.r  == doctest::Approx(0.20f));
    CHECK(b.hoveredColor.r == doctest::Approx(0.30f));
    CHECK(b.pressedColor.r == doctest::Approx(0.10f));
    CHECK(b.onClickFunction.empty());
}

// ── UISystem::buildFontAtlas ─────────────────────────────────────────────────

TEST_CASE("UISystem::buildFontAtlas produces a non-empty bitmap")
{
    constexpr int W = 256, H = 256;
    std::vector<uint8_t> pixels;
    bool ok = UISystem::buildFontAtlas(W, H, 13.0f, pixels);
    REQUIRE(ok);
    CHECK(pixels.size() == static_cast<size_t>(W * H));
    // At least one non-zero alpha byte (font glyphs present).
    bool hasGlyph = false;
    for (uint8_t px : pixels) if (px > 0) { hasGlyph = true; break; }
    CHECK(hasGlyph);
}

TEST_CASE("UISystem::buildFontAtlas returns false for a zero-sized atlas")
{
    std::vector<uint8_t> pixels;
    bool ok = UISystem::buildFontAtlas(0, 0, 13.0f, pixels);
    CHECK(!ok);
}

// The shared runtime UI font (Roboto) must bake into its atlas and emit one
// glyph quad per visible character — guards the atlas size vs. bake resolution.
TEST_CASE("sharedUIFont bakes Roboto and emits glyph quads")
{
    const HE::BakedUIFont& f = HE::sharedUIFont();
    REQUIRE(f.ok); // the font fit the atlas at kBakePx
    CHECK(f.pixels.size() ==
          static_cast<size_t>(HE::BakedUIFont::kWidth) * HE::BakedUIFont::kHeight);

    std::vector<UIRenderObject> out;
    HE::emitUITextGlyphs("Hi", { 0.0f, 0.0f }, { 100.0f, 20.0f }, 16.0f,
                         { 1, 1, 1, 1 }, 0, /*centerH=*/false, out);
    CHECK(out.size() == 2);                 // one quad per visible glyph
    for (const auto& ro : out) CHECK(ro.type == 2); // glyph quads (atlas-sampled)
}

// ── UISystem::extract ────────────────────────────────────────────────────────

TEST_CASE("UISystem::extract produces no objects from empty world")
{
    HorizonWorld world;
    std::vector<UIRenderObject> out;
    UISystem::extract(world, 1920.0f, 1080.0f, out);
    CHECK(out.empty());
}

TEST_CASE("UISystem::extract skips inactive canvas")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto ce = world.createEntity("canvas");
    UICanvasComponent cv;
    cv.active = false;
    reg.emplace<UICanvasComponent>(ce, cv);

    auto ee = world.createEntity("elem");
    UIElementComponent el;
    reg.emplace<UIElementComponent>(ee, el);
    reg.emplace<UITextComponent>(ee);

    std::vector<UIRenderObject> out;
    UISystem::extract(world, 1920.0f, 1080.0f, out);
    CHECK(out.empty());
}

TEST_CASE("UISystem::extract skips inactive element")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto ce = world.createEntity("canvas");
    reg.emplace<UICanvasComponent>(ce);

    auto ee = world.createEntity("elem");
    UIElementComponent el;
    el.active = false;
    reg.emplace<UIElementComponent>(ee, el);
    reg.emplace<UITextComponent>(ee);

    std::vector<UIRenderObject> out;
    UISystem::extract(world, 1920.0f, 1080.0f, out);
    CHECK(out.empty());
}

TEST_CASE("UISystem::extract text element at TopLeft anchor, zero offset")
{
    HorizonWorld world;
    auto& reg = world.registry();

    // Canvas: 1920×1080
    auto ce = world.createEntity("canvas");
    reg.emplace<UICanvasComponent>(ce);

    // Element at TopLeft (0,0), size 200×50, pivot (0,0)
    auto ee = world.createEntity("label");
    UIElementComponent el;
    el.position = {0.0f, 0.0f};
    el.size     = {200.0f, 50.0f};
    el.pivot    = {0.0f, 0.0f};
    el.anchor   = UIAnchor::TopLeft;
    reg.emplace<UIElementComponent>(ee, el);

    UITextComponent txt;
    txt.text = "Hello";
    reg.emplace<UITextComponent>(ee, txt);

    std::vector<UIRenderObject> out;
    UISystem::extract(world, 1920.0f, 1080.0f, out);

    // Text is emitted as one font-atlas glyph quad per character (type 2).
    REQUIRE(out.size() == 5); // "Hello"
    for (const auto& ro : out)
    {
        CHECK(ro.type == 2);
        CHECK(ro.uvMax.x > ro.uvMin.x);
        // Every glyph lies inside the element's rect (200×50 at origin),
        // with a little slack for glyph overhang.
        CHECK(ro.position.x >= -1.0f);
        CHECK(ro.position.x < 200.0f);
    }
}

TEST_CASE("UISystem::extract MiddleCenter anchor centers element")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto ce = world.createEntity("canvas");
    reg.emplace<UICanvasComponent>(ce);  // 1920×1080

    // Element at MiddleCenter, size 100×30, pivot (0.5, 0.5), zero offset
    auto ee = world.createEntity("btn");
    UIElementComponent el;
    el.position = {0.0f, 0.0f};
    el.size     = {100.0f, 30.0f};
    el.pivot    = {0.5f, 0.5f};
    el.anchor   = UIAnchor::MiddleCenter;
    reg.emplace<UIElementComponent>(ee, el);
    reg.emplace<UIImageComponent>(ee);

    std::vector<UIRenderObject> out;
    UISystem::extract(world, 1920.0f, 1080.0f, out);

    REQUIRE(!out.empty());
    // anchorScreen = (960, 540). pivotOffset = (0.5*100, 0.5*30) = (50, 15).
    // screenPos = (960 - 50, 540 - 15) = (910, 525).
    CHECK(out[0].position.x == doctest::Approx(910.0f));
    CHECK(out[0].position.y == doctest::Approx(525.0f));
}

TEST_CASE("UISystem::extract layer sorting")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto ce = world.createEntity("canvas");
    reg.emplace<UICanvasComponent>(ce);

    for (int layer : {2, 0, 1})
    {
        auto ee = world.createEntity("e");
        UIElementComponent el;
        el.pivot  = {0.0f, 0.0f};
        el.layer  = layer;
        el.active = true;
        reg.emplace<UIElementComponent>(ee, el);
        reg.emplace<UIImageComponent>(ee);
    }

    std::vector<UIRenderObject> out;
    UISystem::extract(world, 1920.0f, 1080.0f, out);

    REQUIRE(out.size() == 3);
    CHECK(out[0].layer <= out[1].layer);
    CHECK(out[1].layer <= out[2].layer);
}

TEST_CASE("UISystem::extract button uses hovered color when hovered")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto ce = world.createEntity("canvas");
    reg.emplace<UICanvasComponent>(ce);

    auto ee = world.createEntity("btn");
    UIElementComponent el;
    el.pivot  = {0.0f, 0.0f};
    el.active = true;
    reg.emplace<UIElementComponent>(ee, el);

    UIButtonComponent btn;
    btn.state        = UIButtonState::Hovered;
    btn.hoveredColor = {0.9f, 0.8f, 0.7f, 1.0f};
    reg.emplace<UIButtonComponent>(ee, btn);

    std::vector<UIRenderObject> out;
    UISystem::extract(world, 1920.0f, 1080.0f, out);

    REQUIRE(!out.empty());
    CHECK(out[0].color.r == doctest::Approx(0.9f));
    CHECK(out[0].color.g == doctest::Approx(0.8f));
}

// extract() used to iterate EVERY UIElementComponent for EVERY active canvas,
// so a second canvas doubled the whole UI — each element once more, laid out
// with the other canvas' scale. Elements now belong to the canvas they hang
// under in the hierarchy.
TEST_CASE("UISystem::extract emits an element only for its own canvas")
{
    HorizonWorld world;
    auto& reg = world.registry();

    // Canvas A is 1920×1080 (scale 1 at a 1920×1080 viewport), canvas B is
    // 960×540 (scale 2) — so the two canvases produce distinguishable rects.
    auto ca = world.createEntity("canvasA");
    reg.emplace<UICanvasComponent>(ca);
    auto cb = world.createEntity("canvasB");
    UICanvasComponent cvB;
    cvB.width  = 960.0f;
    cvB.height = 540.0f;
    reg.emplace<UICanvasComponent>(cb, cvB);

    auto makeElement = [&](const char* name, Entity canvas, glm::vec4 tint)
    {
        auto e = world.createEntity(name);
        UIElementComponent el;
        el.position = {100.0f, 0.0f};
        el.size     = {100.0f, 30.0f};
        el.pivot    = {0.0f, 0.0f};
        el.anchor   = UIAnchor::TopLeft;
        reg.emplace<UIElementComponent>(e, el);
        UIImageComponent img;
        img.tint = tint;
        reg.emplace<UIImageComponent>(e, img);
        CHECK(world.reparentEntity(e, canvas));
        return e;
    };
    makeElement("elemA", ca, {1.0f, 0.0f, 0.0f, 1.0f});
    makeElement("elemB", cb, {0.0f, 1.0f, 0.0f, 1.0f});

    std::vector<UIRenderObject> out;
    UISystem::extract(world, 1920.0f, 1080.0f, out);

    REQUIRE(out.size() == 2); // was 4 before the membership check
    int nA = 0, nB = 0;
    for (const auto& ro : out)
    {
        if (ro.color.r > 0.5f)
        {
            ++nA;
            CHECK(ro.position.x == doctest::Approx(100.0f)); // canvas A: scale 1
            CHECK(ro.size.x     == doctest::Approx(100.0f));
        }
        else
        {
            ++nB;
            CHECK(ro.position.x == doctest::Approx(200.0f)); // canvas B: scale 2
            CHECK(ro.size.x     == doctest::Approx(200.0f));
        }
    }
    CHECK(nA == 1);
    CHECK(nB == 1);
}

// Unparented elements (legacy scenes / the editor's add-component flow) have no
// canvas of their own; they must still render, but only once — under the first
// active canvas, not under every one.
TEST_CASE("UISystem::extract emits a canvas-less element exactly once")
{
    HorizonWorld world;
    auto& reg = world.registry();

    reg.emplace<UICanvasComponent>(world.createEntity("canvasA"));
    reg.emplace<UICanvasComponent>(world.createEntity("canvasB"));

    auto ee = world.createEntity("orphan"); // stays a child of the world root
    UIElementComponent el;
    el.pivot = {0.0f, 0.0f};
    reg.emplace<UIElementComponent>(ee, el);
    reg.emplace<UIImageComponent>(ee);

    std::vector<UIRenderObject> out;
    UISystem::extract(world, 1920.0f, 1080.0f, out);
    CHECK(out.size() == 1); // was 2 (once per canvas)
}

// An element under a disabled canvas is hidden — it must not fall through to
// the active canvas that happens to be the fallback for canvas-less elements.
TEST_CASE("UISystem::extract hides elements of an inactive canvas")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto active = world.createEntity("canvasActive");
    reg.emplace<UICanvasComponent>(active);

    auto off = world.createEntity("canvasOff");
    UICanvasComponent cvOff;
    cvOff.active = false;
    reg.emplace<UICanvasComponent>(off, cvOff);

    auto ee = world.createEntity("hidden");
    UIElementComponent el;
    el.pivot = {0.0f, 0.0f};
    reg.emplace<UIElementComponent>(ee, el);
    reg.emplace<UIImageComponent>(ee);
    REQUIRE(world.reparentEntity(ee, off));

    std::vector<UIRenderObject> out;
    UISystem::extract(world, 1920.0f, 1080.0f, out);
    CHECK(out.empty());
}

// The single-canvas layout/order path is unchanged: nested children still sort
// after their parent, and canvas-less siblings still render.
TEST_CASE("UISystem::extract single canvas keeps parent-before-child order")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto ce = world.createEntity("canvas");
    reg.emplace<UICanvasComponent>(ce);

    auto parent = world.createEntity("panel");
    UIElementComponent pel;
    pel.position = {0.0f, 0.0f};
    pel.size     = {400.0f, 200.0f};
    pel.pivot    = {0.0f, 0.0f};
    reg.emplace<UIElementComponent>(parent, pel);
    UIImageComponent pimg;
    pimg.tint = {1.0f, 0.0f, 0.0f, 1.0f};
    reg.emplace<UIImageComponent>(parent, pimg);
    REQUIRE(world.reparentEntity(parent, ce));

    auto child = world.createEntity("child");
    UIElementComponent cel;
    cel.position = {10.0f, 10.0f};
    cel.size     = {50.0f, 20.0f};
    cel.pivot    = {0.0f, 0.0f};
    reg.emplace<UIElementComponent>(child, cel);
    UIImageComponent cimg;
    cimg.tint = {0.0f, 1.0f, 0.0f, 1.0f};
    reg.emplace<UIImageComponent>(child, cimg);
    REQUIRE(world.reparentEntity(child, parent));

    std::vector<UIRenderObject> out;
    UISystem::extract(world, 1920.0f, 1080.0f, out);

    REQUIRE(out.size() == 2);
    CHECK(out[0].color.r == doctest::Approx(1.0f)); // panel first (depth 0)
    CHECK(out[1].color.g == doctest::Approx(1.0f)); // child over it (depth 1)
    CHECK(out[1].position.x == doctest::Approx(10.0f)); // anchored inside parent
    CHECK(out[0].layer < out[1].layer);
}

// ── Round-trip serialization ─────────────────────────────────────────────────

TEST_CASE("UICanvasComponent round-trips through SceneSerializer")
{
    HorizonWorld w1;
    auto& r1 = w1.registry();
    auto e1  = w1.createEntity("myCanvas");
    UICanvasComponent cv;
    cv.width  = 800.0f;
    cv.height = 600.0f;
    cv.active = false;
    r1.emplace<UICanvasComponent>(e1, cv);

    SceneSerializer ser;
    std::vector<uint8_t> blob;
    REQUIRE(ser.saveToMemory(w1, blob));
    CHECK(!blob.empty());

    HorizonWorld w2;
    REQUIRE(ser.loadFromMemory(w2, blob));
    auto& r2 = w2.registry();

    auto view = r2.view<UICanvasComponent>();
    bool found = false;
    for (auto [e, c] : view.each())
    {
        found = true;
        CHECK(c.width  == doctest::Approx(800.0f));
        CHECK(c.height == doctest::Approx(600.0f));
        CHECK(!c.active);
    }
    CHECK(found);
}
