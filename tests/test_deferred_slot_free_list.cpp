#include "doctest.h"
#include "DeferredSlotFreeList.h"
#include <set>
#include <vector>

// The free-list behind the D3D12 editor's ImGui SRV heap (Thema 113). The
// questions are the ones the Dock-Splitter drag asks: does freeing my slot and
// allocating again hand me the slot a frame in flight still reads, and when
// does that slot come back. "fence" here stands for the value the GPU has
// completed — the editor's ID3D12Fence on the renderer's queue.

TEST_CASE("DeferredSlotFreeList: a fresh list hands out slots in the old LIFO order")
{
	DeferredSlotFreeList l;
	l.reset(4);
	int a = -1, b = -1, c = -1, d = -1, e = -1;
	CHECK(l.alloc(a));
	CHECK(l.alloc(b));
	CHECK(l.alloc(c));
	CHECK(l.alloc(d));
	CHECK(a == 0);
	CHECK(b == 1);
	CHECK(c == 2);
	CHECK(d == 3);
	CHECK_FALSE(l.alloc(e));
	CHECK_FALSE(l.hasPending());
}

TEST_CASE("DeferredSlotFreeList: a retired slot is not handed out before its fence passes")
{
	DeferredSlotFreeList l;
	l.reset(64);
	int viewport = -1;
	REQUIRE(l.alloc(viewport));

	// Re-registration: free my slot (signal 1 on the queue), allocate again.
	l.retire(viewport, 1);
	l.reclaim(0); // the GPU has not finished the frame that still reads it
	int next = -1;
	REQUIRE(l.alloc(next));
	CHECK(next != viewport);
	CHECK(l.pendingCount() == 1);

	// Once the fence passes 1 the slot is free again — and, LIFO, first in line.
	l.reclaim(1);
	CHECK_FALSE(l.hasPending());
	int again = -1;
	REQUIRE(l.alloc(again));
	CHECK(again == viewport);
}

TEST_CASE("DeferredSlotFreeList: negative control — release() is the old, immediate reuse")
{
	// What the editor did before the fix: free and allocate in one go gives the
	// same slot back. Kept as a test so the difference above stays visible.
	DeferredSlotFreeList l;
	l.reset(64);
	int viewport = -1, next = -1;
	REQUIRE(l.alloc(viewport));
	l.release(viewport);
	REQUIRE(l.alloc(next));
	CHECK(next == viewport);
}

TEST_CASE("DeferredSlotFreeList: a long splitter drag never reuses a slot a frame in flight reads")
{
	// One re-registration per frame, the GPU three frames behind (the D3D12
	// renderer's frames in flight). Frame g draws with slotOfFrame[g]; at the
	// start of frame g+1 that slot is retired with fence value g+1, and while
	// the CPU is at frame f the fence has completed f-3 — so every frame from
	// f-3 up has to be treated as still reading its slot.
	constexpr int kFrames = 500;
	constexpr int kLag    = 3;
	DeferredSlotFreeList l;
	l.reset(64);

	std::vector<int> slotOfFrame;
	int s = -1;
	REQUIRE(l.alloc(s));
	slotOfFrame.push_back(s);
	for (int f = 1; f < kFrames; ++f)
	{
		const std::uint64_t completed = f > kLag ? static_cast<std::uint64_t>(f - kLag) : 0;
		l.retire(slotOfFrame.back(), static_cast<std::uint64_t>(f));
		l.reclaim(completed);
		int n = -1;
		REQUIRE(l.alloc(n));
		// No frame the GPU has not finished (completed+1 .. f-1) may hold n.
		for (int g = static_cast<int>(completed); g < f; ++g)
			CHECK(slotOfFrame[static_cast<size_t>(g)] != n);
		slotOfFrame.push_back(n);
		// Bounded: only the frames in flight park a slot, the heap never drains.
		CHECK(l.pendingCount() <= static_cast<size_t>(kLag));
	}
}

TEST_CASE("DeferredSlotFreeList: an otherwise full heap waits for the oldest retired slot")
{
	DeferredSlotFreeList l;
	l.reset(3);
	int a = -1, b = -1, c = -1;
	REQUIRE(l.alloc(a));
	REQUIRE(l.alloc(b));
	REQUIRE(l.alloc(c));
	l.retire(b, 7);
	l.retire(a, 5);

	int n = -1;
	CHECK_FALSE(l.alloc(n));        // nothing free right now …
	REQUIRE(l.hasPending());
	CHECK(l.oldestPending() == 5);  // … the caller waits for the fence to reach 5
	l.reclaim(5);
	REQUIRE(l.alloc(n));
	CHECK(n == a);                  // only the slot whose value passed
	CHECK(l.pendingCount() == 1);
	CHECK(l.oldestPending() == 7);
	CHECK_FALSE(l.alloc(n));
}

TEST_CASE("DeferredSlotFreeList: reclaim keeps every slot exactly once")
{
	DeferredSlotFreeList l;
	l.reset(8);
	std::vector<int> held(8);
	for (int& h : held) REQUIRE(l.alloc(h));
	for (size_t i = 0; i < held.size(); ++i)
		l.retire(held[i], static_cast<std::uint64_t>(10 - i % 3)); // out of order
	l.reclaim(9);
	l.reclaim(10);
	CHECK_FALSE(l.hasPending());
	CHECK(l.oldestPending() == 0);
	std::set<int> seen;
	int n = -1;
	while (l.alloc(n)) CHECK(seen.insert(n).second);
	CHECK(seen.size() == 8);
}
