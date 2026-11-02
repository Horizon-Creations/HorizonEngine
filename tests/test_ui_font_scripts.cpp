#include "doctest.h"
#include <Renderer/UIFont.h>

// Its own file, so its own ctest PROCESS: the cases below need a process in which
// nobody has baked the shared font yet, because uiSetFontScripts is refused after
// that. (test_ui_font_weight.cpp is the same arrangement for the weight.)

TEST_CASE("The bold face follows the scripts set after its first lookup")
{
    // Regular base, or `<b>` is a no-op and has no key at all.
    if (!HE::uiSetFontWeightBold(false))
    {
        MESSAGE("shared font already baked in this process — this file needs one of its own");
        return;
    }
    const std::uint32_t latinOnly = HE::uiEngineFaceKey(HE::UIFontFace::Bold);
    REQUIRE(latinOnly != 0);

    // Still allowed: nothing has baked the shared font yet.
    if (!HE::uiSetFontScripts(HE::UIFontScriptCyrillic))
    {
        MESSAGE("shared font already baked in this process — this file needs one of its own");
        return;
    }
    const HE::BakedUIFont& base = HE::sharedUIFont();
    REQUIRE(base.ok);
    CHECK(base.glyph(0x0416) != nullptr);          // Cyrillic 'Ж'

    // The base face has Cyrillic, so the bold one must too, or `<b>Ж</b>` is a
    // gap in an otherwise correct sentence.
    const HE::BakedUIFont* bold =
        HE::UIFontCache::find(HE::uiEngineFaceKey(HE::UIFontFace::Bold));
    REQUIRE(bold != nullptr);
    REQUIRE(bold->ok);
    CHECK(bold->scripts == base.scripts);
    CHECK(bold->glyph(0x0416) != nullptr);
}
