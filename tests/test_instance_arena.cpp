#include "doctest.h"
#include <HorizonRendering/InstanceArena.h>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

// The sizing policy behind D3D12's and Vulkan's instance arena (Thema 182 step 8).
// At 500k foliage plants all in range a frame asks for ~1.1M {mvp, model} slots
// (scene + SSAO + the cascades); the arena before step 8 counted chunk CAPACITIES
// against a 1M cap and refused the next chunk at 983 040, every frame: D3D12 drew
// no scene, Vulkan looped per object (RTX 4070, step 5). Replayed here without a GPU.
using HE::InstanceArena;

namespace
{
	// One frame's instanced runs in recording order: three shadow cascades that see
	// a few buckets each, then the SSAO pre-pass and the scene with every bucket
	// (169 buckets of ~2958 plants = 500k, the step 5 ladder's "all in range").
	std::vector<uint32_t> frame500kAll()
	{
		std::vector<uint32_t> runs;
		const uint32_t bucket = 2958;
		for (int c = 0; c < 3; ++c) runs.insert(runs.end(), 12, bucket);
		runs.insert(runs.end(), 169, bucket); // SSAO
		runs.insert(runs.end(), 169, bucket); // scene
		return runs;
	}
	uint32_t sum(const std::vector<uint32_t>& runs)
	{
		uint32_t s = 0;
		for (uint32_t r : runs) s += r;
		return s;
	}

	// The pre-step-8 arena, verbatim in its arithmetic (D3D12 allocInstances /
	// beginInstanceFrame of 6154b254): capacities summed against 1 << 20.
	struct OldArena
	{
		static constexpr uint32_t kMax = 1u << 20;
		uint32_t mainCap, curCap = 0, cursor = 0, frameCap = 0, refused = 0;
		explicit OldArena(uint32_t main) : mainCap(main) {}
		void begin()
		{
			if (frameCap > mainCap)
			{
				uint32_t want = mainCap;
				while (want < frameCap && want < kMax) want *= 2;
				mainCap = want;
			}
			curCap = frameCap = mainCap; cursor = 0; refused = 0;
		}
		bool alloc(uint32_t count)
		{
			if (uint64_t(cursor) + count > curCap)
			{
				uint32_t cap = std::max(curCap, 1024u) * 2;
				while (cap < count) cap *= 2;
				if (uint64_t(frameCap) + cap > kMax) { refused += count; return false; }
				frameCap += cap; curCap = cap; cursor = 0;
			}
			cursor += count;
			return true;
		}
	};

	// Drives the arena the way allocInstances does; returns the chunks it opened.
	uint32_t runFrame(InstanceArena& a, const std::vector<uint32_t>& runs)
	{
		uint32_t opened = 0;
		for (uint32_t n : runs)
		{
			const uint32_t c = a.chunkFor(n);
			if (c == InstanceArena::kRefused) { a.refuse(n); continue; }
			if (c) { a.openChunk(c); ++opened; }
			a.take(n);
		}
		return opened;
	}

	struct ScopedEnv
	{
		std::string name, prev; bool hadPrev = false;
		ScopedEnv(const char* n, const char* value) : name(n)
		{
			if (const char* p = std::getenv(n)) { hadPrev = true; prev = p; }
#ifdef _WIN32
			_putenv_s(n, value);
#else
			setenv(n, value, 1);
#endif
		}
		~ScopedEnv()
		{
#ifdef _WIN32
			_putenv_s(name.c_str(), hadPrev ? prev.c_str() : "");
#else
			if (hadPrev) setenv(name.c_str(), prev.c_str(), 1);
			else         unsetenv(name.c_str());
#endif
		}
	};
}

TEST_CASE("InstanceArena: the old capacity count refused 500k plants in range every frame")
{
	// Negative control: the replay must show the step 5 numbers, or the fix below
	// proves nothing.
	const std::vector<uint32_t> runs = frame500kAll();
	OldArena old(65536);
	for (int f = 0; f < 4; ++f)
	{
		old.begin();
		for (uint32_t n : runs) old.alloc(n);
		INFO("frame " << f);
		CHECK(old.refused > 0);
		if (f == 0) CHECK(old.frameCap == 983040u); // 64k + 128k + 256k + 512k, the logged figure
	}
	CHECK(old.mainCap == (1u << 20)); // grown to the cap, still short of ~1.1M
}

TEST_CASE("InstanceArena: 500k plants in range fit, and the next frame fits the main buffer whole")
{
	const std::vector<uint32_t> runs = frame500kAll();
	const uint32_t need = sum(runs);
	REQUIRE(need > (1u << 20));

	InstanceArena a(InstanceArena::budgetFor(InstanceArena::kDefaultBudgetBytes, 128));
	a.beginFrame(65536); // the backends' initial k_maxInstances main buffer
	CHECK(runFrame(a, runs) > 0);
	CHECK(a.refused() == 0);
	CHECK(a.taken() == need);
	CHECK(a.asked() == need);
	CHECK(a.reserved() <= a.budget());
	CHECK(a.overflowed());

	const uint32_t main = a.nextMainCapacity();
	CHECK(main >= need + need / 8);
	CHECK(main % InstanceArena::kMainGranule == 0);
	CHECK(main < need + need / 8 + InstanceArena::kMainGranule); // sized from use, not doubled

	for (int f = 0; f < 3; ++f)
	{
		a.beginFrame(main);
		CHECK(runFrame(a, runs) == 0); // no chunk opens any more
		CHECK(a.refused() == 0);
		CHECK_FALSE(a.overflowed());
		CHECK(a.nextMainCapacity() == main);
	}
}

TEST_CASE("InstanceArena: the last chunk is clamped to what the budget has left")
{
	InstanceArena a(1u << 20);
	a.beginFrame(65536);
	// Fill 64k, then chunks of 128k, 256k, 512k: 983 040 reserved, as in step 5.
	for (uint32_t n : { 65536u, 131072u, 262144u, 524288u })
	{
		const uint32_t c = a.chunkFor(n);
		if (c) a.openChunk(c);
		a.take(n);
	}
	REQUIRE(a.reserved() == 983040u);
	// The doubled chunk (1M) no longer fits; the 65 536 the budget has left do.
	CHECK(a.chunkFor(60000) == 65536u);
	CHECK(a.chunkFor(65536) == 65536u);
	CHECK(a.chunkFor(65537) == InstanceArena::kRefused);
	a.openChunk(a.chunkFor(60000));
	a.take(60000);
	CHECK(a.reserved() == a.budget());
	CHECK(a.chunkFor(5536) == 0);
	CHECK(a.chunkFor(5537) == InstanceArena::kRefused);
}

TEST_CASE("InstanceArena: past the budget it refuses, counts it and asks for the whole budget")
{
	InstanceArena a(1u << 20);
	const std::vector<uint32_t> runs = frame500kAll();
	a.beginFrame(65536);
	runFrame(a, runs);
	CHECK(a.refused() > 0);
	CHECK(a.taken() + a.refused() == sum(runs));
	CHECK(a.reserved() <= a.budget());
	CHECK(a.nextMainCapacity() == a.budget());

	// A slot with no main buffer (creation failed) takes nothing.
	a.beginFrame(0);
	CHECK(a.chunkFor(1) == InstanceArena::kRefused);
	CHECK(a.chunkFor(0) == InstanceArena::kRefused);
}

TEST_CASE("InstanceArena: the budget is 512 MiB of 128-byte pairs, HE_INST_ARENA_MIB overrides it")
{
	{
		ScopedEnv e("HE_INST_ARENA_MIB", "");
		CHECK(InstanceArena::budgetFromEnv(128) == (4u << 20));
	}
	{
		ScopedEnv e("HE_INST_ARENA_MIB", "256");
		CHECK(InstanceArena::budgetFromEnv(128) == (2u << 20));
	}
	{
		ScopedEnv e("HE_INST_ARENA_MIB", "nonsense");
		CHECK(InstanceArena::budgetFromEnv(128) == (4u << 20));
	}
	{
		ScopedEnv e("HE_INST_ARENA_MIB", "1"); // below one main granule: the granule
		CHECK(InstanceArena::budgetFromEnv(128) == InstanceArena::kMainGranule);
	}
	CHECK(InstanceArena(10).budget() == InstanceArena::kMainGranule);
}
