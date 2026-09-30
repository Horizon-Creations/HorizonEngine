#include "doctest.h"
#include <HorizonRendering/FrameUploadRing.h>
#include <set>
#include <thread>
#include <vector>

// The bookkeeping behind Metal's per-frame upload ring (perf audit B6): slices
// out of a few long-lived chunks, a frame's chunks back only after its command
// buffer completed.
using HE::FrameUploadRing;

TEST_CASE("FrameUploadRing: outside a frame nothing is handed out")
{
	FrameUploadRing ring(4096, 256);
	CHECK_FALSE(ring.frameOpen());
	CHECK_FALSE(ring.allocate(64).valid());   // caller falls back to its own buffer
	ring.beginFrame();
	CHECK(ring.allocate(64).valid());
	(void)ring.endFrame();
	CHECK_FALSE(ring.allocate(64).valid());
	CHECK(ring.chunkCount() == 1);
}

TEST_CASE("FrameUploadRing: slices are aligned, packed and never overlap")
{
	FrameUploadRing ring(4096, 256);
	ring.beginFrame();
	const auto a = ring.allocate(100);
	const auto b = ring.allocate(300);
	const auto c = ring.allocate(1);
	CHECK(a.chunk == 0);
	CHECK(a.offset == 0);
	CHECK(b.chunk == 0);
	CHECK(b.offset == 256);                    // 100 rounded up to the alignment
	CHECK(c.offset == 768);                    // 256 + 300 = 556 → 768
	CHECK(a.offset % 256 == 0);
	CHECK(b.offset % 256 == 0);
	CHECK(c.offset % 256 == 0);
	// Filling the chunk spills into a second one rather than overrunning it.
	const auto d = ring.allocate(4096 - 1024 + 1);
	CHECK(d.chunk == 1);
	CHECK(d.offset == 0);
	const auto t = ring.endFrame();
	CHECK(t == FrameUploadRing::Ticket{ 0, 1 });
}

TEST_CASE("FrameUploadRing: a frame's chunks come back only after release")
{
	FrameUploadRing ring(1024, 256);
	ring.beginFrame();
	(void)ring.allocate(1000);
	const auto t0 = ring.endFrame();

	// Frame 1 while frame 0 is still on the GPU: it must not get chunk 0.
	ring.beginFrame();
	const auto s1 = ring.allocate(1000);
	CHECK(s1.chunk != t0[0]);
	const auto t1 = ring.endFrame();
	CHECK(ring.chunkCount() == 2);

	// Frame 0 completes; frame 2 reuses its chunk instead of growing the pool.
	ring.release(t0);
	CHECK(ring.freeChunkCount() == 1);
	ring.beginFrame();
	CHECK(ring.allocate(1000).chunk == t0[0]);
	(void)ring.endFrame();
	CHECK(ring.chunkCount() == 2);
	ring.release(t1);
}

TEST_CASE("FrameUploadRing: steady state allocates no new chunks")
{
	// Three frames in flight, each the same size: the pool settles at the
	// in-flight peak and every later frame is served from it.
	FrameUploadRing ring(2048, 256);
	std::vector<FrameUploadRing::Ticket> inFlight;
	size_t countAfterWarmup = 0;
	for (int frame = 0; frame < 50; ++frame)
	{
		ring.beginFrame();
		for (int batch = 0; batch < 20; ++batch) (void)ring.allocate(128 * 7);
		inFlight.push_back(ring.endFrame());
		if (inFlight.size() > 3)
		{
			ring.release(inFlight.front());
			inFlight.erase(inFlight.begin());
		}
		if (frame == 10) countAfterWarmup = ring.chunkCount();
	}
	CHECK(countAfterWarmup > 0);
	CHECK(ring.chunkCount() == countAfterWarmup);
}

TEST_CASE("FrameUploadRing: an oversized request gets a chunk of its own size, reused by fit")
{
	FrameUploadRing ring(1024, 256);
	ring.beginFrame();
	const auto big = ring.allocate(5000);
	CHECK(big.valid());
	CHECK(ring.chunkCapacity(big.chunk) >= 5000);
	CHECK(ring.chunkCapacity(big.chunk) % 256 == 0);
	const auto small = ring.allocate(10);      // does not fit behind 5000 → own chunk
	CHECK(small.chunk != big.chunk);
	ring.release(ring.endFrame());

	// Both are free now. A small request takes the SMALL chunk, so the big one
	// stays available for the next big request instead of being carved up.
	ring.beginFrame();
	CHECK(ring.allocate(10).chunk == small.chunk);
	CHECK(ring.allocate(4000).chunk == big.chunk);
	ring.release(ring.endFrame());
	CHECK(ring.chunkCount() == 2);
}

TEST_CASE("FrameUploadRing: a frame never committed recycles its chunks at the next begin")
{
	FrameUploadRing ring(1024, 256);
	ring.beginFrame();
	const auto s = ring.allocate(512);
	// Early return: no endFrame, no command buffer committed.
	ring.beginFrame();
	CHECK(ring.allocate(512).chunk == s.chunk);
	CHECK(ring.chunkCount() == 1);
	ring.release(ring.endFrame());
}

TEST_CASE("FrameUploadRing: release from a completion thread while the main thread allocates")
{
	FrameUploadRing ring(4096, 256);
	std::vector<FrameUploadRing::Ticket> tickets;
	for (int i = 0; i < 64; ++i)
	{
		ring.beginFrame();
		(void)ring.allocate(4000);
		tickets.push_back(ring.endFrame());
	}
	std::thread completer([&] { for (const auto& t : tickets) ring.release(t); });
	for (int i = 0; i < 200; ++i)
	{
		ring.beginFrame();
		(void)ring.allocate(4000);
		ring.release(ring.endFrame());
	}
	completer.join();
	// Every chunk ended up free exactly once: no chunk lost, none duplicated.
	CHECK(ring.freeChunkCount() == ring.chunkCount());
	ring.beginFrame();
	std::set<uint32_t> seen;
	for (size_t i = 0; i < ring.chunkCount(); ++i)
		seen.insert(ring.allocate(4000).chunk);
	CHECK(seen.size() == ring.chunkCount());
	(void)ring.endFrame();
}
