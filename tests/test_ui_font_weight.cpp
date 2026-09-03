#include "doctest.h"
#include <Renderer/UIFont.h>
#include <vector>

// The shared font atlas is baked ONCE per process, so a project's weight can be
// chosen only before the first character is drawn. That makes this file its own
// ctest entry on purpose: it is the one process in the suite that runs as a
// REGULAR-weight project, and every case here needs that to still be true.
//
// Everything else about faces (the parser, the no-op when the base is already
// bold) lives in test_ui_widgets.cpp, where the weight is the engine's default.

// ctest runs one process per test FILE, so asking here is asking first. Run the
// whole binary by hand instead and whichever file gets there first wins — so
// every case below steps aside rather than failing on somebody else's weight.
// (A static initialiser would NOT do: it runs even when doctest's filter excludes
// this file, which is exactly how the bold cases in test_ui_widgets.cpp were
// broken once already.)
#define REQUIRE_REGULAR_PROCESS()                                              \
    do {                                                                       \
        if (!HE::uiSetFontWeightBold(false))                                   \
        {                                                                      \
            MESSAGE("shared font already baked bold in this process — this "    \
                    "file needs one of its own (ctest gives it one)");         \
            return;                                                            \
        }                                                                      \
    } while (0)

TEST_CASE("A project can ask for regular text, and gets it")
{
    REQUIRE_REGULAR_PROCESS();
    CHECK_FALSE(HE::uiFontWeightBold());
    const HE::BakedUIFont& base = HE::sharedUIFont();
    REQUIRE(base.ok);

    // …and now that it IS baked, the answer is fixed for this process. That is
    // the whole reason the settings page can say "restart the editor".
    CHECK(HE::uiSetFontWeightBold(false));
    CHECK_FALSE(HE::uiSetFontWeightBold(true));
    CHECK_FALSE(HE::uiFontWeightBold());
}

TEST_CASE("Bold is a face of its own, and it is heavier than the base")
{
    REQUIRE_REGULAR_PROCESS();
    const HE::BakedUIFont& base = HE::sharedUIFont();
    REQUIRE(base.ok);
    const std::uint32_t boldKey = HE::uiEngineFaceKey(HE::UIFontFace::Bold);
    REQUIRE(boldKey != 0);                      // a regular base HAS something bolder
    const HE::BakedUIFont* bold = HE::UIFontCache::find(boldKey);
    REQUIRE(bold != nullptr);
    REQUIRE(bold->ok);

    // Heavier means MORE INK, not a wider advance — in a condensed family the
    // bold 'H' is actually a hair narrower, which is why measuring weight by
    // advance would have pinned the wrong thing. Coverage per pixel of the box,
    // read straight out of the two atlases.
    auto inkDensity = [](const HE::BakedUIFont& f, std::uint32_t cp) {
        const HE::BakedGlyph* g = f.glyph(cp);
        REQUIRE(g != nullptr);
        const int x0 = (int)g->x0, y0 = (int)g->y0, x1 = (int)g->x1, y1 = (int)g->y1;
        REQUIRE(x1 > x0);
        REQUIRE(y1 > y0);
        double sum = 0.0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                sum += f.pixels[(std::size_t)y * f.atlasW + x];
        return sum / ((double)(x1 - x0) * (y1 - y0));
    };
    CHECK(inkDensity(*bold, 'H') > inkDensity(base, 'H'));
    // The umlauts came along, so <b> works in German too.
    CHECK(bold->glyph(0x00F6) != nullptr);
}

TEST_CASE("<b> draws its run out of the bold atlas, on the line's own baseline")
{
    REQUIRE_REGULAR_PROCESS();
    const HE::BakedUIFont& base = HE::sharedUIFont();
    REQUIRE(base.ok);
    const HE::UIRichText rt = HE::uiParseRichText("a<b>a</>a");
    REQUIRE(rt.text == "aaa");
    REQUIRE(rt.runs.size() == 3);
    CHECK(rt.runs[0].face == HE::UIFontFace::Base);
    CHECK(rt.runs[1].face == HE::UIFontFace::Bold);
    CHECK(rt.runs[2].face == HE::UIFontFace::Base);

    HE::UITextLayout opts;
    const HE::UIRichLayout lay =
        HE::uiLayoutRichText(base, rt, { 0.0f, 0.0f }, { 400.0f, 40.0f }, 24.0f, opts);
    REQUIRE(lay.pieces.size() == 3);
    // One line, one baseline — a bold word must not sit a pixel above its
    // neighbours, which is what per-run ascents would do.
    CHECK(lay.pieces[0].baseline == doctest::Approx(lay.pieces[1].baseline));
    CHECK(lay.pieces[1].baseline == doctest::Approx(lay.pieces[2].baseline));
    // The middle 'a' is measured in the face it is DRAWN in — not wider, just
    // its own — so the layout and the draw cannot disagree about where the third
    // one starts.
    const HE::BakedUIFont* bold = HE::UIFontCache::find(HE::uiEngineFaceKey(HE::UIFontFace::Bold));
    REQUIRE(bold != nullptr);
    const HE::BakedGlyph* ba = bold->glyph('a');
    REQUIRE(ba != nullptr);
    CHECK(lay.pieces[1].width == doctest::Approx(ba->xadvance * (24.0f / bold->bakePx)));
    CHECK(lay.pieces[1].width != doctest::Approx(lay.pieces[0].width));
    CHECK(lay.pieces[2].x == doctest::Approx(lay.pieces[1].x + lay.pieces[1].width));

    std::vector<UIRenderObject> out;
    HE::uiEmitRichText(base, 0, rt, lay, { 1, 1, 1, 1 }, 0, out);
    REQUIRE(out.size() == 3);
    const std::uint32_t boldKey = HE::uiEngineFaceKey(HE::UIFontFace::Bold);
    CHECK(out[0].fontAtlasKey == 0u);
    CHECK(out[1].fontAtlasKey == boldKey);
    CHECK(out[2].fontAtlasKey == 0u);
}
