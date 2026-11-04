#include "doctest.h"
#include <HorizonScene/RuntimeVirtualTexture.h>
#include <algorithm>
#include <set>

// Runtime Virtual Textures, the CPU side (Thema 333, step 1): the layout of a volume, which pages a camera
// needs, that a frame in which nothing changed paints nothing, dirty rects, the shared pool and the page table.

using namespace HE::rvt;

namespace
{
	// 512 m square, 128-texel pages of 0.25 m texels = 32 m pages at mip 0; 16 x 16 pages, 5 mips.
	VolumeDesc square512()
	{
		VolumeDesc d;
		d.origin = { 0.0f, 0.0f };
		d.sizeM = 512.0f;
		return d;
	}
	ViewDesc viewAt(float x, float z, float near = 64.0f)
	{
		ViewDesc v;
		v.cameraXZ = { x, z };
		v.nearRadiusM = near;
		return v;
	}
	size_t countMip(const std::vector<PageRender>& r, int mip)
	{
		return size_t(std::count_if(r.begin(), r.end(), [&](const PageRender& p) { return p.key.mip == mip; }));
	}
}

TEST_CASE("RVT layout: a power-of-two page grid with a mip chain down to one page")
{
	const VolumeLayout L(square512());
	CHECK(L.pagesPerSide(0) == 16);
	CHECK(L.mipCount() == 5);
	CHECK(L.pagesPerSide(4) == 1);
	CHECK(L.pageMeters(0) == doctest::Approx(32.0f));
	CHECK(L.pageMeters(4) == doctest::Approx(512.0f));
	CHECK(L.extentM() == doctest::Approx(512.0f));
	CHECK(L.physicalPageSize() == 136);

	// A volume that is not a power of two rounds its grid up (the extra is simply empty).
	VolumeDesc d = square512();
	d.sizeM = 600.0f;
	const VolumeLayout R(d);
	CHECK(R.pagesPerSide(0) == 32);
	CHECK(R.mipCount() == 6);
	CHECK(R.extentM() == doctest::Approx(1024.0f));

	glm::vec2 mn, mx;
	L.pageRect(0, 2, 3, mn, mx, false);
	CHECK(mn.x == doctest::Approx(64.0f)); CHECK(mn.y == doctest::Approx(96.0f)); CHECK(mx.x == doctest::Approx(96.0f));
	L.pageRect(0, 2, 3, mn, mx, true);       // 4 texels of 0.25 m on every side
	CHECK(mn.x == doctest::Approx(63.0f)); CHECK(mx.y == doctest::Approx(129.0f));

	int x = 0, y = 0;
	L.pageAt(0, { 70.0f, 100.0f }, x, y);
	CHECK(x == 2); CHECK(y == 3);
	L.pageAt(0, { -50.0f, 9999.0f }, x, y);   // outside: clamped to the edge page
	CHECK(x == 0); CHECK(y == 15);

	CHECK(L.mipForMetersPerPixel(0.1f) == 0);
	CHECK(L.mipForMetersPerPixel(0.5f) == 1);
	CHECK(L.mipForMetersPerPixel(1.0f) == 2);
	CHECK(L.mipForMetersPerPixel(1000.0f) == 4);
}

TEST_CASE("RVT: the first frame paints the view coarsest first, and a frame with no change paints nothing")
{
	RvtManager m(1000, 100000);
	const uint16_t v = m.addVolume(square512());
	m.beginFrame();
	const auto first = m.update(v, viewAt(256, 256));
	// The camera square of mip m has half extent 64 * 2^m: 5 x 5 pages at mips 0 and 1, then the volume shrinks it.
	CHECK(countMip(first, 0) == 25);
	CHECK(countMip(first, 1) == 25);
	CHECK(countMip(first, 2) == 16);
	CHECK(countMip(first, 3) == 4);
	CHECK(countMip(first, 4) == 1);
	CHECK(first.size() == 71);
	CHECK(first.front().key.mip == 4);       // the whole volume is covered before any detail
	std::set<uint64_t> keys;
	for (const PageRender& p : first) keys.insert(p.key.pack());
	CHECK(keys.size() == first.size());      // no page twice
	CHECK(m.stats().resident == 71);

	// Nothing moved, nothing changed: the producer has nothing to do, the table is not re-uploaded.
	const uint64_t ver = m.tableVersion(v);
	for (int f = 0; f < 5; ++f)
	{
		m.beginFrame();
		CHECK(m.update(v, viewAt(256, 256)).empty());
	}
	CHECK(m.tableVersion(v) == ver);
}

TEST_CASE("RVT: the per-frame budget spreads the first fill, coarse pages first")
{
	RvtManager m(1000, 10);
	const uint16_t v = m.addVolume(square512());
	size_t total = 0;
	for (int f = 0; f < 20 && total < 71; ++f)
	{
		m.beginFrame();
		const auto r = m.update(v, viewAt(256, 256));
		CHECK(r.size() <= 10);
		if (f == 0)
		{
			// 5 pages at mips 4 and 3, then the nearest-first rest: never a mip 0 page before the coverage exists.
			for (size_t i = 0; i < 5; ++i) CHECK(r[i].key.mip >= 3);
			CHECK(m.stats().wantedMissing == 61);
		}
		total += r.size();
	}
	CHECK(total == 71);
	m.beginFrame();
	CHECK(m.update(v, viewAt(256, 256)).empty());
}

TEST_CASE("RVT: moving the camera paints only the pages that came into view")
{
	RvtManager m(1000, 100000);
	const uint16_t v = m.addVolume(square512());
	m.beginFrame(); m.update(v, viewAt(256, 256));
	m.beginFrame();
	const auto r = m.update(v, viewAt(288, 256));    // one mip-0 page to the right
	CHECK(r.size() == 5);                            // a new column of 5 pages at mip 0, nothing coarser changed
	CHECK(countMip(r, 0) == 5);
	for (const PageRender& p : r) CHECK_FALSE(p.repaint);
}

TEST_CASE("RVT: a dirty rect re-paints exactly the pages it touches, once")
{
	RvtManager m(1000, 100000);
	const uint16_t v = m.addVolume(square512());
	m.beginFrame(); m.update(v, viewAt(256, 256));

	CHECK(m.invalidateRect(v, { 1000, 1000 }, { 1010, 1010 }) == 0);     // outside the volume

	// A stroke in the middle of one page: that page at every mip (5 mips), nothing else.
	CHECK(m.invalidateRect(v, { 240, 240 }, { 245, 245 }) == 5);
	CHECK(m.stats().dirty == 5);
	m.beginFrame();
	const uint64_t ver = m.tableVersion(v);
	const auto r = m.update(v, viewAt(256, 256));
	CHECK(r.size() == 5);
	for (const PageRender& p : r) { CHECK(p.repaint); CHECK(p.rectMin.x <= 240.0f); CHECK(p.rectMax.x >= 245.0f); }
	CHECK(m.tableVersion(v) == ver);                 // a repaint does not change residency
	CHECK(m.stats().dirty == 0);
	m.beginFrame();
	CHECK(m.update(v, viewAt(256, 256)).empty());

	// A stroke on a page edge reaches into the neighbour through the border texels.
	CHECK(m.invalidateRect(v, { 255.5f, 240 }, { 255.9f, 245 }) >= 2);
	CHECK(m.isDirty({ v, 0, 7, 7 }));
	CHECK(m.isDirty({ v, 0, 8, 7 }));
	CHECK_FALSE(m.isDirty({ v, 0, 9, 7 }));

	// A parameter or material change dirties the whole volume.
	CHECK(m.invalidateVolume(v) == m.stats().resident);
}

TEST_CASE("RVT: a full pool evicts the least recently used page, never one the view needs or a pinned one")
{
	RvtManager m(80, 100000);                         // 71 pages for the view, 9 spare
	const uint16_t v = m.addVolume(square512());
	m.beginFrame(); m.update(v, viewAt(256, 256));
	CHECK(m.freeSlots() == 9);

	m.beginFrame();
	m.update(v, viewAt(40, 40));                      // a far corner: many new pages, the old ones are stale
	CHECK(m.stats().evictions > 0);
	CHECK(m.stats().starved == 0);
	for (int y = 0; y <= 3; ++y)
		for (int x = 0; x <= 3; ++x)
			CHECK(pageTableMip(m.resolve(v, 0, x, y)) == 0);     // everything the new view needs is there at full detail
	// Pages both views share (here: all of mip 2, one of mip 1) were touched by the new view, so they were
	// not among the victims even though they are as old as the stale ones.
	for (int y = 0; y <= 2; ++y)
		for (int x = 0; x <= 2; ++x)
		{
			CHECK(pageTableMip(m.resolve(v, 1, x, y)) == 1);
			CHECK(pageTableMip(m.resolve(v, 2, x, y)) == 2);
		}
	CHECK(m.stats().evictions == 15);                 // 24 new pages, 9 free slots
	CHECK(m.isResident({ v, 4, 0, 0 }));              // the pinned whole-volume page survived
	CHECK(m.isResident({ v, 3, 1, 1 }));
}

TEST_CASE("RVT: a pool too small for the view starves instead of evicting what the view needs")
{
	RvtManager m(10, 100000);
	const uint16_t v = m.addVolume(square512());
	m.beginFrame();
	m.update(v, viewAt(256, 256));
	const Stats s = m.stats();
	CHECK(s.resident == 10);
	CHECK(s.starved > 0);
	CHECK(s.wantedMissing > 0);
	CHECK(m.isResident({ v, 4, 0, 0 }));              // the coverage pages went in first
}

TEST_CASE("RVT page table: every entry falls back to the nearest resident coarser page")
{
	RvtManager m(1000, 5);                            // only the 5 pinned coverage pages in frame one
	const uint16_t v = m.addVolume(square512());
	for (int mip = 0; mip < 5; ++mip)
		for (uint32_t e : m.pageTable(v, mip)) CHECK(e == kPageTableInvalid);   // nothing painted yet

	m.beginFrame(); m.update(v, viewAt(256, 256));
	CHECK(m.stats().resident == 5);
	const auto t0 = m.pageTable(v, 0);
	REQUIRE(t0.size() == 256);
	for (uint32_t e : t0) { CHECK(e != kPageTableInvalid); CHECK(pageTableMip(e) == 3); }   // the 2x2 mip is the finest there is
	// Mip 3's own 2x2 pages point to themselves; slots are distinct.
	std::set<uint32_t> slots;
	for (uint32_t e : m.pageTable(v, 3)) { CHECK(pageTableMip(e) == 3); slots.insert(pageTableSlot(e)); }
	CHECK(slots.size() == 4);

	// Once the fine pages arrive the table points at them.
	for (int f = 0; f < 30; ++f) { m.beginFrame(); m.update(v, viewAt(256, 256)); }
	CHECK(pageTableMip(m.resolve(v, 0, 8, 8)) == 0);
	CHECK(pageTableMip(m.resolve(v, 0, 0, 0)) >= 2);  // far from the camera only a coarse page exists
}

TEST_CASE("RVT segmentation: volumes share one pool, a dropped tile gives its slots back")
{
	RvtManager m(200, 100000);
	VolumeDesc a = square512();
	VolumeDesc b = square512();
	b.origin = { 512.0f, 0.0f };                      // the neighbouring tile
	const uint16_t va = m.addVolume(a), vb = m.addVolume(b);
	CHECK(va != vb);
	m.beginFrame();
	m.update(va, viewAt(500, 256));                   // the camera is at the shared edge: both tiles are in view
	m.update(vb, viewAt(500, 256));
	CHECK(m.stats().resident > 71);
	const uint32_t usedBoth = m.stats().resident;

	std::set<uint32_t> slots;                         // one pool: no slot is used by two pages
	for (int mip = 0; mip < 5; ++mip)
	{
		for (uint32_t e : m.pageTable(va, mip)) if (e != kPageTableInvalid && pageTableMip(e) == mip) slots.insert(pageTableSlot(e));
		for (uint32_t e : m.pageTable(vb, mip)) if (e != kPageTableInvalid && pageTableMip(e) == mip) slots.insert(pageTableSlot(e));
	}
	CHECK(slots.size() == usedBoth);

	m.removeVolume(vb);                               // the tile unloads
	CHECK(m.stats().resident < usedBoth);
	CHECK(m.freeSlots() == 200 - m.stats().resident);
	CHECK_FALSE(m.hasVolume(vb));
	CHECK(m.addVolume(b) == vb);                      // the id is reused, the new tile starts empty
	CHECK(m.pageTable(vb, 4)[0] == kPageTableInvalid);
}
