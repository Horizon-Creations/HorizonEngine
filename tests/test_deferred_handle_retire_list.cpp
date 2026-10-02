#include "doctest.h"
#include "DeferredHandleRetireList.h"
#include <cstdint>
#include <map>
#include <vector>

// The rule behind the Vulkan editor's viewport descriptor set (Thema 124): an
// old set is destroyed only once every frame that could draw it has finished
// on the GPU. The GPU is faked by a serial that runs a few frames behind.

namespace
{
using Set = std::uintptr_t;   // stands in for a VkDescriptorSet

// The editor loop of a Dock-Splitter drag, reduced to what matters: every
// frame the viewport RT changes, the editor retires the set the frame before
// drew with and registers a new one, the frame draws ImGui::Image with the
// current set and is submitted; the GPU finishes frames `lag` behind. Counts
// the destroys that hit a set some unfinished frame still draws.
struct DragLoop
{
	int lag = 2;
	std::uint64_t submitted = 0;
	std::map<std::uint64_t, Set> drawnBy;   // frame serial → set it drew
	std::vector<Set> destroyed;
	int destroyedInUse = 0;

	std::uint64_t completed() const { return submitted > std::uint64_t(lag) ? submitted - lag : 0; }

	void destroy(Set s)
	{
		destroyed.push_back(s);
		for (const auto& [serial, set] : drawnBy)
			if (set == s && serial > completed()) ++destroyedInUse;
	}
};
} // namespace

TEST_CASE("DeferredHandleRetireList: a handle comes back only once its serial has passed")
{
	DeferredHandleRetireList<Set> list;
	std::vector<Set> out;
	auto collect = [&out](Set s) { out.push_back(s); };

	list.retire(0xA, 5);
	CHECK(list.pendingCount() == 1);
	list.reclaim(4, collect);
	CHECK(out.empty());
	CHECK(list.pendingCount() == 1);

	list.reclaim(5, collect);   // at the serial counts as passed (≤)
	REQUIRE(out.size() == 1);
	CHECK(out[0] == 0xA);
	CHECK_FALSE(list.hasPending());

	list.reclaim(100, collect);   // nothing left: destroyed exactly once
	CHECK(out.size() == 1);
}

TEST_CASE("DeferredHandleRetireList: several parked handles leave in serial order, each once")
{
	DeferredHandleRetireList<Set> list;
	std::vector<Set> out;
	auto collect = [&out](Set s) { out.push_back(s); };

	list.retire(0xA, 1);
	list.retire(0xB, 2);
	list.retire(0xC, 3);
	list.retire(Set{}, 1);   // a null handle is not parked at all
	CHECK(list.pendingCount() == 3);

	list.reclaim(1, collect);
	REQUIRE(out.size() == 1);
	CHECK(out[0] == 0xA);

	// A slot freed above is reused for the next retire; the handle in it must
	// not be confused with the one destroyed before.
	list.retire(0xD, 4);
	list.reclaim(3, collect);
	REQUIRE(out.size() == 3);
	CHECK(((out[1] == 0xB && out[2] == 0xC) || (out[1] == 0xC && out[2] == 0xB)));
	CHECK(list.pendingCount() == 1);

	list.reclaim(4, collect);
	REQUIRE(out.size() == 4);
	CHECK(out[3] == 0xD);
}

TEST_CASE("DeferredHandleRetireList: clear forgets parked handles without destroying them")
{
	DeferredHandleRetireList<Set> list;
	int destroys = 0;
	list.retire(0xA, 1);
	list.retire(0xB, 2);
	list.clear();
	CHECK_FALSE(list.hasPending());
	list.reclaim(100, [&destroys](Set) { ++destroys; });
	CHECK(destroys == 0);
}

TEST_CASE("DeferredHandleRetireList: a splitter drag never frees a set a running frame draws")
{
	DragLoop loop;
	DeferredHandleRetireList<Set> list;
	Set current = 0;
	Set next    = 1;

	for (int frame = 0; frame < 20; ++frame)
	{
		// Editor: reclaim what the GPU is done with, retire the old set, add a new one.
		list.reclaim(loop.completed(), [&loop](Set s) { loop.destroy(s); });
		if (current) list.retire(current, loop.submitted);
		current = next++;
		// Frame draws ImGui::Image with `current` and is submitted.
		loop.drawnBy[++loop.submitted] = current;
	}
	CHECK(loop.destroyedInUse == 0);
	// Every set but the live one and the ones still parked has been destroyed.
	CHECK(loop.destroyed.size() + list.pendingCount() == 19);
	CHECK(list.pendingCount() <= std::size_t(loop.lag) + 1);
}

TEST_CASE("DeferredHandleRetireList: Gegenprobe, freeing at once (the old RemoveTexture) hits running frames")
{
	DragLoop loop;
	Set current = 0;
	Set next    = 1;

	for (int frame = 0; frame < 20; ++frame)
	{
		if (current) loop.destroy(current);   // ImGui_ImplVulkan_RemoveTexture on the spot
		current = next++;
		loop.drawnBy[++loop.submitted] = current;
	}
	// The set the previous frame drew is still in flight every time.
	CHECK(loop.destroyedInUse > 0);
}
