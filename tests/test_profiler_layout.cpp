#include "doctest.h"
#include "ProfilerLayout.h"

#include <cmath>
#include <set>
#include <string>

// ── ProfilerLayout ────────────────────────────────────────────────────────────
// The arithmetic behind the timeline and the graphs. It cannot be checked by
// looking at the editor from here (no display in this environment), which is
// exactly why it was split out of the drawing code — everything that can be
// silently wrong about "where does this 40 µs span land at 12x zoom" is here.

using namespace HE::Prof::Layout;

TEST_CASE("spanRect maps time to pixels and clips to the view")
{
    TimeView v{ 0.0, 1'000'000.0 };   // 1 ms across 100 px → 1 px per 10 µs
    Rect r;

    // Middle half of the view, depth 0.
    REQUIRE(spanRect(250'000, 750'000, 0, v, 100.0f, 200.0f, 10.0f, 12.0f, 0.0f, r));
    CHECK(r.x0 == doctest::Approx(125.0f));
    CHECK(r.x1 == doctest::Approx(175.0f));
    CHECK(r.y0 == doctest::Approx(10.0f));
    CHECK(r.y1 == doctest::Approx(22.0f));

    // Depth places the row: depth 2 sits two rows down, same time range.
    REQUIRE(spanRect(250'000, 750'000, 2, v, 100.0f, 200.0f, 10.0f, 12.0f, 0.0f, r));
    CHECK(r.y0 == doctest::Approx(34.0f));

    // Partly outside → clipped to the viewport, not drawn off-panel.
    REQUIRE(spanRect(0, 2'000'000, 0, v, 100.0f, 200.0f, 0.0f, 12.0f, 0.0f, r));
    CHECK(r.x0 == doctest::Approx(100.0f));
    CHECK(r.x1 == doctest::Approx(200.0f));

    // Fully outside on either side → rejected.
    CHECK_FALSE(spanRect(2'000'000, 3'000'000, 0, v, 100.0f, 200.0f, 0.0f, 12.0f, 0.0f, r));
    TimeView late{ 5'000'000.0, 6'000'000.0 };
    CHECK_FALSE(spanRect(0, 1'000, 0, late, 100.0f, 200.0f, 0.0f, 12.0f, 0.0f, r));

    // Degenerate inputs must not divide by zero or produce inverted rects.
    CHECK_FALSE(spanRect(0, 100, 0, TimeView{ 5.0, 5.0 }, 100.0f, 200.0f, 0.0f, 12.0f, 0.0f, r));
    CHECK_FALSE(spanRect(0, 100, 0, v, 200.0f, 100.0f, 0.0f, 12.0f, 0.0f, r));
}

TEST_CASE("spanRect enforces a minimum width so busy lanes are not blank")
{
    // 100 ns inside a 1 ms view is 0.01 px — without a floor a thread running
    // thousands of short jobs renders as an empty lane, which reads as an idle
    // thread rather than as "too zoomed out to see".
    TimeView v{ 0.0, 1'000'000.0 };
    Rect r;
    REQUIRE(spanRect(500'000, 500'100, 0, v, 0.0f, 100.0f, 0.0f, 12.0f, 2.0f, r));
    CHECK(r.width() == doctest::Approx(2.0f));

    // A minimum-width span at the extreme right edge stays inside the viewport
    // instead of spilling past it.
    REQUIRE(spanRect(999'900, 1'000'000, 0, v, 0.0f, 100.0f, 0.0f, 12.0f, 6.0f, r));
    CHECK(r.x1 <= 100.0f);
    CHECK(r.width() == doctest::Approx(6.0f));
}

TEST_CASE("zoomAt keeps the instant under the cursor fixed")
{
    // 1 ms wide — comfortably above kMinViewSpanNs, so this case measures the
    // anchoring maths rather than the collapse guard (which the loop below covers).
    TimeView v{ 0.0, 1'000'000.0 };
    const float px0 = 0.0f, px1 = 100.0f;

    // Cursor at 25% of the width sits on t=250 µs. After any zoom it must still be
    // at 25% of the width — that is what makes wheel-zoom feel attached to the
    // mouse instead of to the window.
    auto instantAt = [&](const TimeView& tv, float px) {
        return tv.startNs + (px - px0) / (px1 - px0) * tv.span();
    };
    const TimeView in = zoomAt(v, 25.0f, px0, px1, 0.5);
    CHECK(in.span() == doctest::Approx(500'000.0));
    CHECK(instantAt(in, 25.0f) == doctest::Approx(250'000.0));

    const TimeView out = zoomAt(v, 25.0f, px0, px1, 4.0);
    CHECK(out.span() == doctest::Approx(4'000'000.0));
    CHECK(instantAt(out, 25.0f) == doctest::Approx(250'000.0));

    // Zooming in forever must not collapse the span to zero — at zero every pixel
    // is the same instant and the next zoom-out has no anchor.
    TimeView deep = v;
    for (int i = 0; i < 200; ++i) deep = zoomAt(deep, 50.0f, px0, px1, 0.5);
    CHECK(deep.span() >= kMinViewSpanNs);

    // Nonsense inputs are returned unchanged rather than producing an inverted view.
    CHECK(zoomAt(v, 25.0f, px0, px1, 0.0).span() == doctest::Approx(v.span()));
    CHECK(zoomAt(v, 25.0f, 50.0f, 50.0f, 0.5).span() == doctest::Approx(v.span()));
}

TEST_CASE("panByPixels moves the window without resizing it")
{
    TimeView v{ 1000.0, 2000.0 };            // 1000 ns over 100 px → 10 ns/px
    const TimeView p = panByPixels(v, 10.0f, 0.0f, 100.0f);
    // Dragging right moves the content right, so the window moves LEFT in time.
    CHECK(p.startNs == doctest::Approx(900.0));
    CHECK(p.span() == doctest::Approx(1000.0));

    const TimeView q = panByPixels(v, -10.0f, 0.0f, 100.0f);
    CHECK(q.startNs == doctest::Approx(1100.0));
    CHECK(q.span() == doctest::Approx(1000.0));
}

TEST_CASE("clampView keeps the window inside the capture, preserving its width")
{
    // Panned off the left edge → snaps back, same width.
    const TimeView a = clampView(TimeView{ -500.0, 500.0 }, 0, 10'000);
    CHECK(a.startNs == doctest::Approx(0.0));
    CHECK(a.span() == doctest::Approx(1000.0));

    // Off the right edge → snaps back, same width.
    const TimeView b = clampView(TimeView{ 9'500.0, 10'500.0 }, 0, 10'000);
    CHECK(b.endNs == doctest::Approx(10'000.0));
    CHECK(b.span() == doctest::Approx(1000.0));

    // Wider than the capture → shows the whole capture rather than floating in
    // empty space beyond it.
    const TimeView c = clampView(TimeView{ -5'000.0, 50'000.0 }, 0, 10'000);
    CHECK(c.startNs == doctest::Approx(0.0));
    CHECK(c.endNs == doctest::Approx(10'000.0));

    // An invalid/uninitialised view refits to the whole capture — this is what
    // the panel's "Fit" button relies on.
    const TimeView d = clampView(TimeView{}, 100, 900);
    CHECK(d.startNs == doctest::Approx(100.0));
    CHECK(d.endNs == doctest::Approx(900.0));

    // Degenerate capture bounds must still yield a usable, non-empty view.
    CHECK(clampView(TimeView{ 0.0, 10.0 }, 5, 5).span() > 0.0);
}

TEST_CASE("barIndexAt hit-tests the frame graph")
{
    // 10 bars across 100 px.
    CHECK(barIndexAt(0.0f,  0.0f, 100.0f, 10) == 0);
    CHECK(barIndexAt(9.9f,  0.0f, 100.0f, 10) == 0);
    CHECK(barIndexAt(10.0f, 0.0f, 100.0f, 10) == 1);
    CHECK(barIndexAt(99.9f, 0.0f, 100.0f, 10) == 9);
    // Outside the strip → no selection, rather than clamping to an end bar the
    // user never pointed at.
    CHECK(barIndexAt(-1.0f,  0.0f, 100.0f, 10) == -1);
    CHECK(barIndexAt(100.0f, 0.0f, 100.0f, 10) == -1);
    CHECK(barIndexAt(50.0f,  0.0f, 100.0f, 0)  == -1);
    CHECK(barIndexAt(50.0f, 100.0f, 100.0f, 5) == -1);
}

TEST_CASE("niceTickStep returns round 1/2/5 steps")
{
    CHECK(niceTickStep(100.0, 10) == doctest::Approx(10.0));
    CHECK(niceTickStep(50.0, 10)  == doctest::Approx(5.0));
    CHECK(niceTickStep(1.0, 5)    == doctest::Approx(0.2));
    CHECK(niceTickStep(0.0, 10)   == doctest::Approx(0.0));
    CHECK(niceTickStep(100.0, 0)  == doctest::Approx(0.0));

    // Whatever the range, the step is always 1, 2 or 5 times a power of ten —
    // that is the whole point (axis labels a person reads as round numbers).
    for (double range = 0.001; range < 100000.0; range *= 3.0)
    {
        const double step = niceTickStep(range, 8);
        const double mant = step / std::pow(10.0, std::floor(std::log10(step)));
        CHECK((std::fabs(mant - 1.0) < 1e-9 || std::fabs(mant - 2.0) < 1e-9 ||
               std::fabs(mant - 5.0) < 1e-9 || std::fabs(mant - 10.0) < 1e-9));
    }
}

TEST_CASE("nameHash is stable and spreads distinct scope names")
{
    // Stability is the requirement: a scope's colour is its identity on the flame
    // graph, so the same name must hash the same every frame and every session.
    CHECK(nameHash("Render") == nameHash("Render"));
    CHECK(nameHash("Render") != nameHash("render"));
    CHECK(nameHash("") == nameHash(nullptr));   // null is treated as empty, not a crash

    const char* names[] = { "Render", "OnRender", "PollEvents", "SwapBuffers", "GameLogicTick",
                            "Terrain", "Movement", "Navigation", "Weather", "ParticleSystem",
                            "Foliage", "LOD", "Animation", "PhysicsStep", "Job::Execute" };
    std::set<uint32_t> hashes;
    for (const char* n : names) hashes.insert(nameHash(n));
    CHECK(hashes.size() == sizeof(names) / sizeof(names[0]));   // no collisions in the real set
}
