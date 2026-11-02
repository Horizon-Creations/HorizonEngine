#include "doctest.h"
#include <Backends/Vulkan/VulkanUploadAlloc.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

// The allocators of the Vulkan upload path (REN-802/803). CI has no Vulkan
// device, so the renderer's staging ring and mesh-memory pool are only ever
// exercised for real on lavapipe and NN-WS03; what can go wrong in the offset
// arithmetic (an upload overwriting staging bytes a pending copy still reads,
// two meshes bound to overlapping memory, a misaligned bind) is pinned here.

using HE::vkup::kFail;
using HE::vkup::RangeAllocator;
using HE::vkup::StagingRing;

namespace
{
struct Live { uint64_t off, size, ticket; };

bool overlaps(uint64_t a, uint64_t as, uint64_t b, uint64_t bs)
{
	return a < b + bs && b < a + as;
}
}

TEST_CASE("StagingRing: allocations are aligned and never straddle the end")
{
	StagingRing ring(1024);
	CHECK(ring.allocate(100, 16, 1) == 0);
	CHECK(ring.allocate(10, 16, 1) == 112);           // 100 aligned up to 112
	CHECK(ring.allocate(900, 16, 1) == kFail);        // 122 + 900 > 1024, tail still at 0
	ring.retire(1);
	CHECK(ring.used() == 0);
	// Empty again → back to offset 0 (otherwise 128 + 900 > 1024 would have
	// to skip to the next lap past the free tail and fail).
	CHECK(ring.allocate(900, 16, 2) == 0);
	CHECK(ring.allocate(124, 4, 2) == 900);
	CHECK(ring.allocate(1, 1, 2) == kFail);           // full
}

TEST_CASE("StagingRing: bytes of an unretired ticket are never handed out again")
{
	StagingRing ring(256);
	const uint64_t a = ring.allocate(128, 16, 1);
	const uint64_t b = ring.allocate(64, 16, 2);
	REQUIRE(a == 0);
	REQUIRE(b == 128);
	CHECK(ring.allocate(128, 16, 3) == kFail);        // only 64 left before ticket 1's bytes
	ring.retire(1);                                   // ticket 1 done, ticket 2 still pending
	const uint64_t c = ring.allocate(128, 16, 3);     // 192 + 128 > 256 → lap, offset 0
	CHECK(c == 0);
	CHECK(!overlaps(c, 128, b, 64));
	CHECK(ring.allocate(16, 16, 3) == kFail);         // would run into ticket 2 at 128
	ring.retire(2);
	CHECK(ring.allocate(16, 16, 3) == 128);
}

TEST_CASE("StagingRing: oversize and degenerate requests fail instead of wrapping")
{
	StagingRing ring(4096);
	CHECK(ring.allocate(4097, 16, 1) == kFail);
	CHECK(ring.allocate(0, 16, 1) == kFail);
	CHECK(ring.allocate(16, 3000, 1) == kFail);       // alignment must divide the capacity
	CHECK(ring.allocate(4096, 16, 1) == 0);           // exactly the capacity fits an empty ring
	CHECK(ring.used() == 4096);
	ring.retire(0);                                   // nothing that old
	CHECK(ring.used() == 4096);
	ring.retire(1);
	CHECK(ring.used() == 0);
}

TEST_CASE("StagingRing: randomized batches never overlap a live allocation")
{
	std::mt19937 rng(802);
	StagingRing ring(1 << 16);
	std::vector<Live> live;
	uint64_t ticket = 1, retired = 0;
	int handed = 0;
	for (int step = 0; step < 20000; ++step)
	{
		const int what = int(rng() % 10);
		if (what < 7)
		{
			const uint64_t size  = 1 + rng() % 9000;
			const uint64_t align = uint64_t(1) << (rng() % 9);   // 1..256
			const uint64_t off   = ring.allocate(size, align, ticket);
			if (off == kFail) continue;
			++handed;
			CHECK(off % align == 0);
			CHECK(off + size <= ring.capacity());
			for (const Live& l : live)
				if (overlaps(off, size, l.off, l.size)) { FAIL("overlap at step " << step); }
			live.push_back({ off, size, ticket });
		}
		else if (what < 9) ++ticket;                     // the batch is submitted
		else if (retired + 1 < ticket)                   // some fence signalled
		{
			retired += 1 + rng() % (ticket - retired - 1);
			ring.retire(retired);
			live.erase(std::remove_if(live.begin(), live.end(),
			                          [&](const Live& l) { return l.ticket <= retired; }),
			           live.end());
		}
	}
	CHECK(handed > 1000);
}

TEST_CASE("RangeAllocator: first fit, alignment gap stays free, merge on free")
{
	RangeAllocator ra(1024);
	CHECK(ra.allocate(10, 1) == 0);
	CHECK(ra.allocate(100, 256) == 256);              // [10, 256) stays free
	CHECK(ra.used() == 110);
	CHECK(ra.allocate(200, 1) == 10);                 // the gap is used first-fit
	CHECK(ra.allocate(700, 1) == kFail);              // 46 + 668 free, neither fits
	ra.free(10, 200);
	ra.free(0, 10);
	ra.free(256, 100);
	CHECK(ra.empty());
	CHECK(ra.freeRanges() == 1);                      // all merged back into one range
	CHECK(ra.allocate(1024, 4096) == 0);
}

TEST_CASE("RangeAllocator: randomized alloc/free keeps ranges disjoint and accounting exact")
{
	std::mt19937 rng(803);
	const uint64_t cap = 64ull << 20;                 // the renderer's block size
	RangeAllocator ra(cap);
	std::vector<std::pair<uint64_t, uint64_t>> live;
	uint64_t used = 0;
	for (int step = 0; step < 20000; ++step)
	{
		if (live.empty() || rng() % 3 != 0)
		{
			const uint64_t size  = 1 + rng() % (2u << 20);
			const uint64_t align = uint64_t(4) << (rng() % 7);  // 4..256
			const uint64_t off   = ra.allocate(size, align);
			if (off == kFail) continue;
			CHECK(off % align == 0);
			CHECK(off + size <= cap);
			for (const auto& [o, s] : live)
				if (overlaps(off, size, o, s)) { FAIL("overlap at step " << step); }
			live.emplace_back(off, size);
			used += size;
		}
		else
		{
			const size_t i = rng() % live.size();
			ra.free(live[i].first, live[i].second);
			used -= live[i].second;
			live[i] = live.back();
			live.pop_back();
		}
		CHECK(ra.used() == used);
	}
	for (const auto& [o, s] : live) ra.free(o, s);
	CHECK(ra.empty());
	CHECK(ra.freeRanges() == 1);
}
