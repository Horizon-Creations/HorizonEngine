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

TEST_CASE("RVT: a page is stale per layer, and a repaint names exactly the stale layers")
{
	RvtManager m(1000, 100000);
	const uint16_t v = m.addVolume(square512());
	m.beginFrame();
	for (const PageRender& p : m.update(v, viewAt(256, 256))) CHECK(p.layers == kRvtAllLayers);   // first paint: everything
	const uint32_t resident = m.stats().resident;

	// Layer 1 (normal) only, whole volume: every page is dirty, in that layer alone.
	CHECK(m.invalidateVolume(v, 1u << 1) == resident);
	CHECK(m.stats().dirty == resident);
	CHECK(m.stats().dirtyLayer[0] == 0);
	CHECK(m.stats().dirtyLayer[1] == resident);
	CHECK(m.stats().dirtyLayer[2] == 0);
	CHECK(m.dirtyLayers({ v, 0, 7, 7 }) == (1u << 1));

	// A rect in layer 2 on top: the page under it carries both, its neighbours only layer 1.
	CHECK(m.invalidateRect(v, { 240, 240 }, { 245, 245 }, 1u << 2) == 5);
	CHECK(m.dirtyLayers({ v, 0, 7, 7 }) == ((1u << 1) | (1u << 2)));
	CHECK(m.dirtyLayers({ v, 0, 6, 7 }) == (1u << 1));
	CHECK(m.stats().dirtyLayer[2] == 5);
	CHECK(m.stats().dirty == resident);              // still one page count, not one per layer

	// The repaint of a page asks for the layers that are stale, nothing more, and clears them.
	m.beginFrame();
	const auto r = m.update(v, viewAt(256, 256));
	REQUIRE(r.size() == resident);
	int withLayer2 = 0;
	for (const PageRender& p : r)
	{
		CHECK(p.repaint);
		CHECK((p.layers & (1u << 0)) == 0);          // nobody marked the colour layer: it is never repainted
		CHECK((p.layers & (1u << 1)) != 0);          // every page was stale in layer 1
		if (p.layers & (1u << 2)) ++withLayer2;
		if (p.key.mip == 0) CHECK(p.layers == ((p.key.x == 7 && p.key.y == 7) ? ((1u << 1) | (1u << 2)) : (1u << 1)));
	}
	CHECK(withLayer2 == 5);                          // the stroke's page at each of the 5 mips
	CHECK(m.stats().dirty == 0);
	CHECK(m.stats().dirtyLayer[1] == 0);
	m.beginFrame();
	CHECK(m.update(v, viewAt(256, 256)).empty());

	// A layer mask of 0 (or bits outside the three layers) marks nothing and is no news.
	const uint64_t dv = m.dirtyVersion();
	CHECK(m.invalidateVolume(v, 0) == 0);
	CHECK(m.invalidateRect(v, { 0, 0 }, { 512, 512 }, 0x80) == 0);
	CHECK(m.dirtyVersion() == dv);
	// Marking a layer a page already has stale is no news either; adding a new one is.
	CHECK(m.invalidateVolume(v, 1u << 0) == resident);
	const uint64_t dv1 = m.dirtyVersion();
	CHECK(dv1 > dv);
	CHECK(m.invalidateVolume(v, 1u << 0) == resident);
	CHECK(m.dirtyVersion() == dv1);
	CHECK(m.invalidateVolume(v, 1u << 2) == resident);
	CHECK(m.dirtyVersion() > dv1);

	// The debug view's stale mark follows "any layer".
	const std::vector<uint32_t> marked = m.pageTable(v, 0, true);
	for (uint32_t e : marked) CHECK(pageTableStale(e));
}

TEST_CASE("RVT: the debug view's stale mark rides in bit 30 of the table entry, only on request, and follows the dirty flag")
{
	RvtManager m(1000, 100000);
	const uint16_t v = m.addVolume(square512());
	m.beginFrame(); m.update(v, viewAt(256, 256));
	const std::vector<uint32_t> clean = m.pageTable(v, 0);
	const uint64_t dv0 = m.dirtyVersion();

	// Nothing dirty: the marked table is the plain one.
	CHECK(m.pageTable(v, 0, /*markStale=*/true) == clean);

	CHECK(m.invalidateRect(v, { 240, 240 }, { 245, 245 }) == 5);
	CHECK(m.dirtyVersion() > dv0);                    // the driver's cue to upload the table again
	CHECK(m.invalidateRect(v, { 240, 240 }, { 245, 245 }) == 5);   // already dirty: counted, no news
	const uint64_t dv1 = m.dirtyVersion();
	CHECK(m.invalidateRect(v, { 240, 240 }, { 245, 245 }) == 5);
	CHECK(m.dirtyVersion() == dv1);

	// Without the request the table does not change (every backend decodes it as it always did).
	CHECK(m.pageTable(v, 0) == clean);
	const std::vector<uint32_t> marked = m.pageTable(v, 0, true);
	int stale = 0;
	for (size_t i = 0; i < marked.size(); ++i)
	{
		CHECK(marked[i] != kPageTableInvalid);
		CHECK((marked[i] & ~kPageTableStale) == clean[i]);   // only that one bit differs
		CHECK(pageTableMip(marked[i]) == pageTableMip(clean[i]));
		CHECK(pageTableSlot(marked[i]) == pageTableSlot(clean[i]));
		if (pageTableStale(marked[i])) ++stale;
	}
	CHECK(stale == 1);                                 // the one mip-0 page under the stroke
	CHECK(pageTableStale(m.pageTable(v, 0, true)[size_t(7) * 16 + 7]));

	// A repaint clears it.
	m.beginFrame(); m.update(v, viewAt(256, 256));
	CHECK(m.dirtyVersion() > dv1);
	for (uint32_t e : m.pageTable(v, 0, true)) CHECK_FALSE(pageTableStale(e));
	for (int mip = 0; mip < 5; ++mip)
		for (uint32_t e : m.pageTable(v, mip, true)) CHECK_FALSE(pageTableStale(e));
	CHECK(pageTableStale(kPageTableInvalid) == false);   // "no page" is never a stale page
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

TEST_CASE("RVT segmentation: updateAll gives every volume its coverage first and pays one budget")
{
	RvtManager m(1000, 10);                           // ten pages a frame for BOTH volumes
	VolumeDesc a = square512();
	VolumeDesc b = square512();
	b.origin = { 512.0f, 0.0f };
	const uint16_t va = m.addVolume(a), vb = m.addVolume(b);
	m.beginFrame();
	const auto r = m.updateAll({ { va, viewAt(500, 256) }, { vb, viewAt(500, 256) } });
	REQUIRE(r.size() == 10);
	// Each volume has 71 wanted pages; the first ten are the coverage of BOTH (top mip first), not ten of the first volume.
	size_t topA = 0, topB = 0;
	for (const PageRender& p : r)
	{
		CHECK(p.key.mip >= 3);                        // nothing finer before every volume is covered
		if (p.key.mip == 4 && p.key.volume == va) ++topA;
		if (p.key.mip == 4 && p.key.volume == vb) ++topB;
	}
	CHECK(topA == 1);
	CHECK(topB == 1);
	CHECK(m.missing(va) + m.missing(vb) == m.stats().wantedMissing);
	CHECK(m.missing(va) > 0);
	CHECK(m.missing(vb) > 0);

	// The control: one update() per volume. The first volume spends the whole budget, the second has no coverage at all.
	RvtManager c(1000, 10);
	const uint16_t ca = c.addVolume(a), cb = c.addVolume(b);
	c.beginFrame();
	const auto ra = c.update(ca, viewAt(500, 256));
	const auto rb = c.update(cb, viewAt(500, 256));
	CHECK(ra.size() == 10);
	CHECK(rb.empty());
}

TEST_CASE("RVT segmentation: volumes that do not fit one pool settle instead of evicting each other forever")
{
	// The camera at the border of two volumes wants W pages in all; the pool holds 20 fewer. One-by-one updates would let A
	// evict the pages B still needs (not yet touched this frame) and B take them back next frame, for ever. updateAll protects
	// every wanted page first, so after the pool has filled a static view paints nothing and evicts nothing.
	uint32_t wanted = 0;
	{
		RvtManager big(10000, 10000);
		VolumeDesc a = square512(), b = square512();
		b.origin = { 512.0f, 0.0f };
		const uint16_t va = big.addVolume(a), vb = big.addVolume(b);
		big.beginFrame();
		big.updateAll({ { va, viewAt(500, 256) }, { vb, viewAt(500, 256) } });
		wanted = big.stats().resident;
	}
	REQUIRE(wanted > 60);
	RvtManager m(wanted - 20, 1000);
	VolumeDesc a = square512();
	VolumeDesc b = square512();
	b.origin = { 512.0f, 0.0f };
	const uint16_t va = m.addVolume(a), vb = m.addVolume(b);
	for (int f = 0; f < 3; ++f) { m.beginFrame(); m.updateAll({ { va, viewAt(500, 256) }, { vb, viewAt(500, 256) } }); }
	const uint32_t evictions = m.stats().evictions;
	CHECK(m.stats().resident == wanted - 20);         // the pool is full
	CHECK(m.stats().starved > 0);                     // and says so
	CHECK(m.missing(va) + m.missing(vb) == 20);
	uint32_t painted = 0;
	for (int f = 0; f < 10; ++f)
	{
		m.beginFrame();
		painted += uint32_t(m.updateAll({ { va, viewAt(500, 256) }, { vb, viewAt(500, 256) } }).size());
	}
	CHECK(painted == 0);                              // no ping-pong
	CHECK(m.stats().evictions == evictions);
}

TEST_CASE("RVT segmentation: dropping one volume frees exactly its slots and leaves the others")
{
	RvtManager m(1000, 1000);
	VolumeDesc a = square512();
	VolumeDesc b = square512();
	b.origin = { 512.0f, 0.0f };
	const uint16_t va = m.addVolume(a), vb = m.addVolume(b);
	m.beginFrame();
	m.updateAll({ { va, viewAt(500, 256) }, { vb, viewAt(500, 256) } });
	const uint32_t both = m.stats().resident;
	std::vector<uint32_t> tableB;
	for (uint32_t e : m.pageTable(vb, 0)) tableB.push_back(e);
	m.dropVolumePages(va);
	CHECK(m.stats().resident < both);
	CHECK(m.freeSlots() == 1000 - m.stats().resident);
	CHECK(m.pageTable(vb, 0) == tableB);              // the other volume's table is untouched
	for (uint32_t e : m.pageTable(va, 0)) CHECK(e == kPageTableInvalid);
}
