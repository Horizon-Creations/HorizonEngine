#include "doctest.h"
#include <Renderer/UIFont.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Characterization of the text layout: wrap decisions, rich pieces and emitted
// quads for a long mixed paragraph, folded into one digest each. The digests
// were taken from the code BEFORE the layout loops were reworked, so a refactor
// that moves a single float bit or a single break fails here. When a layout
// change is MEANT to move text, re-take the numbers and say so in the commit.

// The digests are float BITS: they hold for the compiler and CPU they were taken
// on (arm64 clang, which contracts a*b+c into an fma). Another toolchain rounds
// some of the same expressions differently without anything being wrong, so the
// exact comparison runs only there; the case still checks that the text wraps.
#if defined(__APPLE__) && defined(__aarch64__)
#define CHECK_DIGEST(actual, expected) CHECK((actual) == (expected))
#else
#define CHECK_DIGEST(actual, expected) ((void)(actual), (void)(expected))
#endif

namespace
{
    struct Digest
    {
        std::uint64_t h = 1469598103934665603ull;
        void bytes(const void* p, std::size_t n)
        {
            const auto* b = static_cast<const unsigned char*>(p);
            for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        }
        void f(float v) { bytes(&v, sizeof v); }
        void u(std::uint64_t v) { bytes(&v, sizeof v); }
        void s(const std::string& v) { u(v.size()); bytes(v.data(), v.size()); }
    };

    // Words of every length, runs of spaces, an unbreakable word, umlauts (two
    // bytes), hard breaks and a CRLF: every branch of the wrappers.
    std::string paragraph()
    {
        std::string p;
        const char* words[] = { "The", "quick", "brown", "fox", "jumps", "over", "the", "lazy", "dog",
                                "Donaudampfschifffahrtsgesellschaftskapitaen", "\xC3\xBC" "ber",
                                "gr\xC3\xB6\xC3\x9F" "ere", "a", "I", "W", "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmm" };
        for (int i = 0; i < 90; ++i)
        {
            p += words[(i * 7 + i / 3) % (sizeof words / sizeof *words)];
            p += (i % 11 == 10) ? "   " : " ";
            if (i % 23 == 22) p += "\n";
            if (i % 37 == 36) p += "\r\n";
        }
        p += "tail  ";
        return p;
    }

    const float kWidths[] = { 37.0f, 90.0f, 161.5f, 333.0f, 1000.0f };
    const float kSizes[]  = { 14.0f, 24.0f };
}

TEST_CASE("text layout digest: layoutUITextLines and uiTextWrapRanges")
{
    const HE::BakedUIFont& font = HE::sharedUIFont();
    REQUIRE(font.ok);
    const std::string text = paragraph();
    Digest d1, d2;
    std::size_t rows = 0;
    for (float size : kSizes)
        for (float w : kWidths)
        {
            const auto lines = HE::layoutUITextLines(font, text, size, w, true);
            rows += lines.size();
            d1.u(lines.size());
            for (const auto& l : lines) d1.s(l);
            for (const auto& r : HE::uiTextWrapRanges(font, text, size, w))
            { d2.u(r.begin); d2.u(r.end); d2.u(r.next); d2.u(r.hard); }
            const glm::vec2 m = HE::measureUIText(font, text, size, w, HE::UITextLayout{});
            d1.f(m.x); d1.f(m.y);
        }
    CHECK(rows > 60);                       // the paragraph really wraps
    CHECK_DIGEST(d1.h, 7426628980194271662ull);
    CHECK_DIGEST(d2.h, 15585308492689547544ull);
}

TEST_CASE("text layout digest: rich layout and emitted quads")
{
    const HE::BakedUIFont& font = HE::sharedUIFont();
    REQUIRE(font.ok);
    std::string markup = "Hello <b>bold gr\xC3\xB6\xC3\x9F" "e</> and <size=1.6>big words that wrap</> "
                         "<color=#ff0000>red <size=0.7>small red</></> <link=x>a link</link> ";
    markup += paragraph();
    const HE::UIRichText rt = HE::uiParseRichText(markup);
    REQUIRE(!rt.runs.empty());

    Digest d;
    std::size_t pieces = 0;
    for (float size : kSizes)
        for (float w : kWidths)
            for (int align = 0; align < 3; ++align)
            {
                HE::UITextLayout opts;
                opts.wrap = true;
                opts.alignH = align;
                const HE::UIRichLayout lay =
                    HE::uiLayoutRichText(font, rt, { 3.0f, 5.0f }, { w, 400.0f }, size, opts);
                pieces += lay.pieces.size();
                d.f(lay.size.x); d.f(lay.size.y);
                for (const HE::UIRichPiece& p : lay.pieces)
                {
                    d.u(p.run); d.u(p.begin); d.u(p.end); d.u((std::uint64_t)p.line);
                    d.f(p.x); d.f(p.width); d.f(p.sizePx); d.f(p.baseline); d.f(p.top); d.f(p.height);
                }
                std::vector<UIRenderObject> quads;
                HE::uiEmitRichText(font, 0, rt, lay, { 1, 0.5f, 0.25f, 0.8f }, 2, quads);
                for (const UIRenderObject& q : quads)
                {
                    d.f(q.position.x); d.f(q.position.y); d.f(q.size.x); d.f(q.size.y);
                    d.f(q.uvMin.x); d.f(q.uvMin.y); d.f(q.uvMax.x); d.f(q.uvMax.y);
                    d.f(q.color.r); d.f(q.color.g); d.f(q.color.b); d.f(q.color.a);
                    d.u(q.type); d.u((std::uint64_t)q.layer); d.u(q.fontAtlasKey);
                }
            }
    CHECK(pieces > 100);
    CHECK_DIGEST(d.h, 15268918858594207563ull);
}

TEST_CASE("text layout digest: plain emitted quads")
{
    const HE::BakedUIFont& font = HE::sharedUIFont();
    REQUIRE(font.ok);
    const std::string text = paragraph();
    Digest d;
    std::size_t n = 0;
    for (float size : kSizes)
        for (float w : kWidths)
            for (int align = 0; align < 3; ++align)
                for (int alignV = 0; alignV < 3; ++alignV)
                {
                    HE::UITextLayout opts;
                    opts.wrap = true;
                    opts.alignH = align;
                    opts.alignV = alignV;
                    std::vector<UIRenderObject> quads;
                    HE::emitUITextGlyphs(font, 7, text, { 11.0f, 13.0f }, { w, 500.0f }, size,
                                         { 0.2f, 0.4f, 0.6f, 0.9f }, 3, opts, quads);
                    n += quads.size();
                    for (const UIRenderObject& q : quads)
                    {
                        d.f(q.position.x); d.f(q.position.y); d.f(q.size.x); d.f(q.size.y);
                        d.f(q.uvMin.x); d.f(q.uvMin.y); d.f(q.uvMax.x); d.f(q.uvMax.y);
                        d.f(q.color.r); d.f(q.color.g); d.f(q.color.b); d.f(q.color.a);
                        d.u(q.type); d.u((std::uint64_t)q.layer); d.u(q.fontAtlasKey);
                    }
                }
    CHECK(n > 1000);
    CHECK_DIGEST(d.h, 3141856323047941843ull);
}
