#include "doctest.h"
#include "EditorWindowPlacement.h"

// Remembering the editor window (EditorWindowPlacement.h): the rectangle kept
// while maximized, the stable-frames rule, and the fall-back when the saved
// position is on a monitor that is gone.

using namespace HE::Ed;

namespace
{
	const std::vector<PlacementRect> kOneScreen = { { 0, 25, 1728, 1050 } };
	const std::vector<PlacementRect> kTwoScreens = { { 0, 25, 1728, 1050 }, { 1728, 0, 2560, 1415 } };

	SavedWindowPlacement saved(PlacementRect r, bool maximized = false)
	{
		SavedWindowPlacement s;
		s.valid = true; s.maximized = maximized; s.normal = r;
		return s;
	}

	void feed(WindowPlacementTracker& t, bool max, bool full, bool min, PlacementRect r, int frames)
	{
		for (int i = 0; i < frames; ++i) t.observe(max, full, min, r);
	}
}

TEST_CASE("resolveWindowPlacement: nothing saved keeps the default window")
{
	CHECK_FALSE(resolveWindowPlacement({}, kOneScreen).applied);
	CHECK_FALSE(resolveWindowPlacement(saved({ 10, 40, 1200, 800 }), {}).applied);
	CHECK_FALSE(resolveWindowPlacement(saved({ 10, 40, 0, 0 }), kOneScreen).applied);
	CHECK_FALSE(resolveWindowPlacement(saved({ 10, 40, 100, 50 }), kOneScreen).applied);
}

TEST_CASE("resolveWindowPlacement: a reachable rectangle is restored as it was")
{
	const PlacementRect r{ 200, 100, 1300, 800 };
	const auto out = resolveWindowPlacement(saved(r, true), kOneScreen);
	CHECK(out.applied);
	CHECK(out.setPosition);
	CHECK(out.maximized);
	CHECK(out.rect == r);

	// On the second monitor, which exists.
	const PlacementRect r2{ 1800, 100, 1400, 900 };
	const auto out2 = resolveWindowPlacement(saved(r2), kTwoScreens);
	CHECK(out2.setPosition);
	CHECK(out2.rect == r2);
}

TEST_CASE("resolveWindowPlacement: a vanished monitor falls back to the centre of the primary")
{
	const PlacementRect r2{ 1800, 100, 1400, 900 };
	const auto out = resolveWindowPlacement(saved(r2, true), kOneScreen);
	CHECK(out.applied);
	CHECK(out.maximized);
	CHECK(out.rect.w == 1400);
	CHECK(out.rect.h == 900);
	CHECK(placementReachable(out.rect, kOneScreen));
	CHECK(out.rect.x == (1728 - 1400) / 2);
	CHECK(out.rect.y == 25 + (1050 - 900) / 2);
}

TEST_CASE("resolveWindowPlacement: an oversized rectangle is shrunk to the primary display")
{
	const auto out = resolveWindowPlacement(saved({ 5000, 5000, 3000, 2000 }), kOneScreen);
	CHECK(out.rect.w == 1728);
	CHECK(out.rect.h == 1050);
	CHECK(placementReachable(out.rect, kOneScreen));
}

TEST_CASE("placementReachable: only the title strip has to be on a screen")
{
	CHECK(placementReachable({ -1000, 100, 1300, 800 }, kOneScreen));        // 300 px of bar still visible
	CHECK_FALSE(placementReachable({ -1250, 100, 1300, 800 }, kOneScreen));  // 50 px: not grabbable
	CHECK_FALSE(placementReachable({ 100, 2000, 1300, 800 }, kOneScreen));   // below the screen
	CHECK_FALSE(placementReachable({ 100, -500, 1300, 800 }, kOneScreen));   // bar above it
}

TEST_CASE("WindowPlacementTracker: remembers the normal rectangle, not the maximized one")
{
	WindowPlacementTracker t;
	const PlacementRect normal{ 100, 80, 1400, 850 };
	feed(t, false, false, false, normal, 10);
	feed(t, true, false, false, { 0, 25, 1728, 1050 }, 10);
	const auto r = t.result();
	CHECK(r.valid);
	CHECK(r.maximized);
	CHECK(r.normal == normal);
}

TEST_CASE("WindowPlacementTracker: maximize-animation frames never become the normal rectangle")
{
	WindowPlacementTracker t;
	const PlacementRect normal{ 100, 80, 1400, 850 };
	feed(t, false, false, false, normal, 10);
	// The window grows frame by frame before the maximized flag shows up.
	for (int i = 1; i <= 3; ++i)
		t.observe(false, false, false, { 100 - i * 10, 80 - i * 5, 1400 + i * 100, 850 + i * 60 });
	feed(t, true, false, false, { 0, 25, 1728, 1050 }, 5);
	CHECK(t.result().normal == normal);
}

TEST_CASE("WindowPlacementTracker: fullscreen and minimized are not recorded")
{
	WindowPlacementTracker t;
	const PlacementRect normal{ 100, 80, 1400, 850 };
	feed(t, false, false, false, normal, 10);
	feed(t, false, true, false, { 0, 0, 1728, 1117 }, 10);
	feed(t, false, false, true, { -32000, -32000, 160, 28 }, 10);
	CHECK(t.result().normal == normal);
	CHECK_FALSE(t.result().maximized);
}

TEST_CASE("WindowPlacementTracker: minimizing a maximized window keeps it maximized")
{
	WindowPlacementTracker t;
	feed(t, false, false, false, { 100, 80, 1400, 850 }, 10);
	feed(t, true, false, false, { 0, 25, 1728, 1050 }, 5);
	feed(t, false, false, true, { 0, 25, 1728, 1050 }, 5);
	CHECK(t.result().maximized);
}

TEST_CASE("WindowPlacementTracker: a session that never saw a stable window keeps the seed")
{
	WindowPlacementTracker t;
	const PlacementRect old{ 50, 60, 1500, 900 };
	t.seed(saved(old, true));
	feed(t, true, false, false, { 0, 25, 1728, 1050 }, 5);   // started maximized, never un-maximized
	const auto r = t.result();
	CHECK(r.valid);
	CHECK(r.maximized);
	CHECK(r.normal == old);

	WindowPlacementTracker empty;
	empty.observe(false, false, false, { 1, 1, 1000, 700 });  // one frame only
	CHECK_FALSE(empty.result().valid);
}
