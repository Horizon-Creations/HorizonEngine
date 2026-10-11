#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/FloatingOrigin.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

// Thema 162, Schritt 3: HE::propagateTransforms recomputes only the subtrees that
// changed. What these pin:
//   * the result is bit-identical to the full walk (propagateTransformsFull), over
//     random sequences of every kind of change the engine makes, not only the easy ones;
//   * the work is what it says (the stats: a quiet world is scanned and nothing is
//     written, one moved leaf is one matrix, one moved group is the group and its
//     children, a child that moved under a group that moved is not done twice);
//   * each of the four things that can change a world matrix without the entity's own
//     transform moving is noticed, and the comparison has teeth: with that one detector
//     switched off (PropagateTestSwitches) the same comparison goes red.
// docs/render-extractor-shadow-pass-plan.md section 7 has the design and the numbers.

namespace
{
bool sameBits(const glm::mat4& a, const glm::mat4& b)
{
	return std::memcmp(&a, &b, sizeof(glm::mat4)) == 0;
}

TransformComponent& tf(HorizonWorld& w, Entity e)
{
	return w.registry().get_or_emplace<TransformComponent>(e);
}

// Every world matrix the world holds, as the full walk computes it, against the ones it
// held a moment ago. True when the pass that came before had them all right. NOTE: this
// walks the world, so it also puts everything right: call it once per state.
bool matchesFullWalk(HorizonWorld& w)
{
	std::vector<std::pair<Entity, glm::mat4>> before;
	for (auto [e, t] : w.registry().view<TransformComponent>().each())
		before.emplace_back(e, t.worldMatrix);
	HE::propagateTransformsFull(w);
	bool same = true;
	for (const auto& [e, m] : before)
		if (!sameBits(m, w.registry().get<TransformComponent>(e).worldMatrix)) same = false;
	return same;
}

// One LCG, every draw in its own statement: the same numbers on every compiler (the order
// in which an argument list is evaluated is not specified).
struct Rng
{
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed * 6364136223846793005ull + 1442695040888963407ull) { next(); }
	uint32_t next()
	{
		s = s * 6364136223846793005ull + 1442695040888963407ull;
		return static_cast<uint32_t>(s >> 33);
	}
	int below(int n) { return static_cast<int>(next() % static_cast<uint32_t>(n)); }
};

// Values the writers use. Never -0.0 or NaN: the local cache compares by ==, which is
// the same for +0 and -0, and the comparison below is by bits.
const float kPos[]   = { -100.0f, -10.0f, -1.5f, 0.0f, 0.25f, 3.0f, 7.5f, 40.0f, 1000.0f };
const float kRot[]   = { 0.0f, 15.0f, 30.0f, 45.0f, 90.0f, 180.0f, 270.0f, 359.5f };
const float kScale[] = { 0.5f, 1.0f, 1.0f, 2.0f, 3.0f };

glm::vec3 pickVec(Rng& r, const float* set, int n)
{
	const float x = set[r.below(n)];
	const float y = set[r.below(n)];
	const float z = set[r.below(n)];
	return glm::vec3(x, y, z);
}
glm::vec3 pickPos(Rng& r)   { return pickVec(r, kPos,   static_cast<int>(sizeof(kPos)   / sizeof(float))); }
glm::vec3 pickRot(Rng& r)   { return pickVec(r, kRot,   static_cast<int>(sizeof(kRot)   / sizeof(float))); }
glm::vec3 pickScale(Rng& r) { return pickVec(r, kScale, static_cast<int>(sizeof(kScale) / sizeof(float))); }

// A switch is set for the length of a scope, whatever the scope does.
struct Switches
{
	explicit Switches(const HE::PropagateTestSwitches& s) { HE::setPropagateTestSwitches(s); }
	~Switches() { HE::setPropagateTestSwitches(HE::PropagateTestSwitches{}); }
};

struct Grouped
{
	std::vector<Entity> groups;
	std::vector<Entity> leaves;
};

// The reference world's shape (scripts/perf/gen_reference_world.py): groups under the
// root, things under the groups.
Grouped buildGrouped(HorizonWorld& w, int groups, int perGroup)
{
	Grouped g;
	g.leaves.reserve(static_cast<size_t>(groups) * static_cast<size_t>(perGroup));
	for (int i = 0; i < groups; ++i)
	{
		const Entity grp = w.createEntity("Group");
		tf(w, grp).position = glm::vec3(static_cast<float>(i) * 8.0f, 0.0f, 0.0f);
		tf(w, grp).rotation = glm::vec3(0.0f, static_cast<float>(i % 360), 0.0f);
		g.groups.push_back(grp);
		for (int j = 0; j < perGroup; ++j)
		{
			const Entity e = w.createEntity("Leaf");
			w.reparentEntity(e, grp);
			TransformComponent& t = tf(w, e);
			t.position = glm::vec3(static_cast<float>(j % 10), 0.5f, static_cast<float>(j / 10));
			t.rotation = glm::vec3(static_cast<float>(j), 15.0f, 0.0f);
			t.scale    = glm::vec3(1.0f + static_cast<float>(j % 3));
			g.leaves.push_back(e);
		}
	}
	return g;
}
} // namespace

// ─── The work it does ───────────────────────────────────────────────────────

TEST_CASE("propagateTransforms: a world nothing happened to is scanned, not walked")
{
	HorizonWorld w;
	const Grouped g = buildGrouped(w, 20, 50);   // 20 + 1000 transforms
	(void)g;

	HE::propagateTransforms(w);
	HE::PropagateStats s = HE::lastPropagateStats(w);
	CHECK(s.full);   // the first pass has nothing to build on
	CHECK(s.recomputed == 1020u);
	CHECK(s.transforms == 1020u);

	HE::propagateTransforms(w);
	s = HE::lastPropagateStats(w);
	CHECK_FALSE(s.full);
	CHECK(s.scanned);
	CHECK(s.flagged == 0u);
	CHECK(s.recomputed == 0u);

	for (auto [e, t] : w.registry().view<TransformComponent>().each())
		CHECK_FALSE(t.dirty);
}

TEST_CASE("propagateTransforms: one moved leaf is one matrix, and nobody else's")
{
	HorizonWorld w;
	const Grouped g = buildGrouped(w, 20, 50);
	HE::propagateTransforms(w);

	const glm::mat4 neighbour = tf(w, g.leaves[124]).worldMatrix;
	const glm::mat4 before    = tf(w, g.leaves[123]).worldMatrix;

	// An Inspector drag: the value changes, the flag does not.
	tf(w, g.leaves[123]).position.x += 5.0f;
	tf(w, g.leaves[123]).dirty = false;
	HE::propagateTransforms(w);

	const HE::PropagateStats s = HE::lastPropagateStats(w);
	CHECK_FALSE(s.full);
	CHECK(s.flagged == 1u);
	CHECK(s.recomputed == 1u);
	CHECK_FALSE(sameBits(tf(w, g.leaves[123]).worldMatrix, before));
	CHECK(sameBits(tf(w, g.leaves[124]).worldMatrix, neighbour));
	CHECK(matchesFullWalk(w));
}

TEST_CASE("propagateTransforms: a moved group carries its children, once")
{
	HorizonWorld w;
	const Grouped g = buildGrouped(w, 20, 50);
	HE::propagateTransforms(w);
	const glm::mat4 otherGroupsLeaf = tf(w, g.leaves[3 * 50 + 50]).worldMatrix;   // group 4

	// A group and one of its own leaves and a leaf of another group, in the same frame.
	tf(w, g.groups[3]).position.y += 2.0f;
	tf(w, g.leaves[3 * 50 + 7]).position.z += 1.0f;
	tf(w, g.leaves[9 * 50 + 1]).rotation.y += 10.0f;
	HE::propagateTransforms(w);

	const HE::PropagateStats s = HE::lastPropagateStats(w);
	CHECK_FALSE(s.full);
	CHECK(s.flagged == 3u);
	// Group 3 and its fifty (the moved leaf among them, done by its group's pass, not again
	// by its own), plus the one leaf in group 9.
	CHECK(s.recomputed == 51u + 1u);
	CHECK(sameBits(tf(w, g.leaves[3 * 50 + 50]).worldMatrix, otherGroupsLeaf));
	CHECK(matchesFullWalk(w));

	// The same with a group made AFTER its children. The scan walks the storage backwards,
	// so this group comes first in its list and the leaves after it have been done by its
	// pass when their turn comes: they must not be done again.
	std::vector<Entity> kids;
	for (int i = 0; i < 20; ++i)
	{
		kids.push_back(w.createEntity("Late"));
		tf(w, kids.back()).position = glm::vec3(static_cast<float>(i), 0.0f, 1.0f);
	}
	const Entity late = w.createEntity("LateGroup");
	tf(w, late).position = glm::vec3(0.0f, 5.0f, 0.0f);
	for (const Entity k : kids) w.reparentEntity(k, late);
	HE::propagateTransforms(w);

	tf(w, late).position.x += 3.0f;
	tf(w, kids[4]).position.y += 1.0f;
	HE::propagateTransforms(w);
	const HE::PropagateStats late2 = HE::lastPropagateStats(w);
	CHECK_FALSE(late2.full);
	CHECK(late2.flagged == 2u);
	CHECK(late2.recomputed == 21u);
	CHECK(matchesFullWalk(w));
}

TEST_CASE("propagateTransforms: the dirty flag is honoured when it is set, and cleared")
{
	HorizonWorld w;
	const Grouped g = buildGrouped(w, 4, 10);
	HE::propagateTransforms(w);

	// A writer that says "I changed this" without changing a value the cache can see.
	tf(w, g.leaves[5]).dirty = true;
	HE::propagateTransforms(w);
	const HE::PropagateStats s = HE::lastPropagateStats(w);
	CHECK(s.flagged == 1u);
	CHECK(s.recomputed == 1u);
	CHECK_FALSE(tf(w, g.leaves[5]).dirty);
	CHECK(matchesFullWalk(w));
}

TEST_CASE("propagateTransforms: most of the world moving falls back to the walk, and scans again once calm")
{
	HorizonWorld w;
	const Grouped g = buildGrouped(w, 10, 50);   // 510 transforms
	HE::propagateTransforms(w);

	// Three quarters of the leaves: past what the scan pays for.
	for (size_t i = 0; i < g.leaves.size(); ++i)
		if (i % 4 != 0) tf(w, g.leaves[i]).position.x += 1.0f;
	HE::propagateTransforms(w);
	HE::PropagateStats s = HE::lastPropagateStats(w);
	CHECK(s.scanned);
	CHECK(s.full);
	CHECK(matchesFullWalk(w));

	// Nothing changed since, but the last pass was a walk because of the scan: the next
	// one walks without scanning, finds the world calm, and the one after scans.
	HE::propagateTransforms(w);
	s = HE::lastPropagateStats(w);
	CHECK(s.full);
	CHECK_FALSE(s.scanned);
	HE::propagateTransforms(w);
	s = HE::lastPropagateStats(w);
	CHECK(s.scanned);
	CHECK_FALSE(s.full);
	CHECK(s.recomputed == 0u);

	// And a world that stays busy stays on the walk.
	for (int round = 0; round < 3; ++round)
	{
		for (size_t i = 0; i < g.leaves.size(); ++i)
			if (i % 4 != 0) tf(w, g.leaves[i]).position.x += 1.0f;
		HE::propagateTransforms(w);
		CHECK(HE::lastPropagateStats(w).full);
	}
	CHECK(matchesFullWalk(w));
}

// ─── What it has to notice ──────────────────────────────────────────────────

TEST_CASE("propagateTransforms: a reparent moves the subtree it did not touch")
{
	HorizonWorld w;
	const Grouped g = buildGrouped(w, 6, 10);
	HE::propagateTransforms(w);

	// reparentEntity writes no transform: the entity and its children keep every value
	// they had, and what they hang off is somewhere else.
	REQUIRE(w.reparentEntity(g.groups[1], g.groups[4]));
	HE::propagateTransforms(w);
	CHECK(HE::lastPropagateStats(w).full);
	CHECK(matchesFullWalk(w));

	// Negative control: with the structure epoch ignored the pass sees nothing change.
	{
		HE::PropagateTestSwitches sw;
		sw.ignoreStructureEpoch = true;
		Switches guard(sw);
		REQUIRE(w.reparentEntity(g.groups[2], g.groups[5]));
		HE::propagateTransforms(w);
		CHECK(HE::lastPropagateStats(w).recomputed == 0u);
		CHECK_FALSE(matchesFullWalk(w));
	}
}

TEST_CASE("propagateTransforms: a hierarchy edited by hand is noticed through noteStructureChanged")
{
	HorizonWorld w;
	const Grouped g = buildGrouped(w, 4, 10);
	HE::propagateTransforms(w);

	// What rebuildHierarchy and an additive load do: move a link without the world's methods.
	const auto rewire = [&](Entity e, Entity from, Entity to)
	{
		auto& fh = w.registry().get<HierarchyComponent>(from);
		fh.children.erase(std::remove(fh.children.begin(), fh.children.end(), e), fh.children.end());
		w.registry().get<HierarchyComponent>(to).children.push_back(e);
		w.registry().get<HierarchyComponent>(e).parent = to;
	};

	// Without the note the pass cannot know (this is the contract, not a defect).
	{
		HE::PropagateTestSwitches sw;
		sw.suspendVerify = true;   // a run under HE_PROPAGATE_VERIFY would rightly abort here
		Switches guard(sw);
		rewire(g.leaves[3], g.groups[0], g.groups[2]);
		HE::propagateTransforms(w);
		CHECK_FALSE(matchesFullWalk(w));
	}

	rewire(g.leaves[13], g.groups[1], g.groups[3]);
	w.noteStructureChanged();
	HE::propagateTransforms(w);
	CHECK(matchesFullWalk(w));
}

TEST_CASE("propagateTransforms: a floating-origin shift writes matrices by hand, and the next pass rebuilds them")
{
	HorizonWorld w;
	const Grouped g = buildGrouped(w, 6, 10);
	// The three things a shift touches that a root child's new position does not reach:
	// an entity outside the hierarchy, a top-level node with no transform of its own and
	// the transform under it.
	const Entity loose = w.registry().create();
	w.registry().emplace<TransformComponent>(loose).position = glm::vec3(5.0f, 1.0f, 2.0f);
	const Entity folder = w.createEntity("Folder");
	const Entity inFolder = w.createEntity("InFolder");
	w.reparentEntity(inFolder, folder);
	tf(w, inFolder).position = glm::vec3(2.0f, 0.0f, 3.0f);
	HE::propagateTransforms(w);

	HE::shiftWorldOrigin(w, nullptr, glm::vec3(100.0f, 0.0f, 0.0f));
	HE::propagateTransforms(w);
	CHECK(HE::lastPropagateStats(w).full);
	CHECK(matchesFullWalk(w));
	// The root children moved by the shift, and everything under them with them.
	CHECK(tf(w, g.groups[0]).position.x == doctest::Approx(-100.0f));

	// Negative control: with the invalidation switched off the pass keeps the matrices
	// the shift wrote, for the entities it did not find changed.
	{
		HE::PropagateTestSwitches sw;
		sw.ignoreInvalidation = true;
		Switches guard(sw);
		HE::shiftWorldOrigin(w, nullptr, glm::vec3(0.0f, 0.0f, 100.0f));
		HE::propagateTransforms(w);
		CHECK_FALSE(matchesFullWalk(w));
	}
}

TEST_CASE("propagateTransforms: a transform taken off a parent hands its children to the grandparent")
{
	HorizonWorld w;
	const Grouped g = buildGrouped(w, 4, 10);
	HE::propagateTransforms(w);

	// A leaf losing its transform leaves nothing under it: nothing to walk.
	w.registry().remove<TransformComponent>(g.leaves[0]);
	HE::propagateTransforms(w);
	CHECK_FALSE(HE::lastPropagateStats(w).full);
	CHECK(matchesFullWalk(w));

	// A group losing its transform changes what its leaves hang off.
	w.registry().remove<TransformComponent>(g.groups[1]);
	HE::propagateTransforms(w);
	CHECK(HE::lastPropagateStats(w).full);
	CHECK(matchesFullWalk(w));

	// Negative control: not noticed.
	{
		HE::PropagateTestSwitches sw;
		sw.ignoreTransformRemoval = true;
		Switches guard(sw);
		w.registry().remove<TransformComponent>(g.groups[2]);
		HE::propagateTransforms(w);
		CHECK_FALSE(matchesFullWalk(w));
	}
}

TEST_CASE("propagateTransforms: a whole component written back is picked up, a flag that survived the copy would hide it")
{
	HorizonWorld w;
	const Grouped g = buildGrouped(w, 4, 10);
	HE::propagateTransforms(w);

	// CinematicPreview's pattern: a saved copy of the component written back later, while
	// what it hangs off has moved in between.
	TransformComponent saved = tf(w, g.leaves[2]);
	CHECK_FALSE(saved.localCacheValid);   // a copy starts out "not valid"
	saved.dirty = false;
	tf(w, g.groups[0]).position.x += 10.0f;
	HE::propagateTransforms(w);

	tf(w, g.leaves[2]) = saved;           // its worldMatrix is the one from before the move
	HE::propagateTransforms(w);
	CHECK(HE::lastPropagateStats(w).flagged >= 1u);
	CHECK(matchesFullWalk(w));

	// What a plain bool would have done: the cache still "valid" and equal to the values.
	{
		HE::PropagateTestSwitches sw;
		sw.suspendVerify = true;
		Switches guard(sw);
		tf(w, g.groups[0]).position.x += 10.0f;
		HE::propagateTransforms(w);
		tf(w, g.leaves[2]) = saved;
		tf(w, g.leaves[2]).localCacheValid = true;
		HE::propagateTransforms(w);
		CHECK_FALSE(matchesFullWalk(w));
	}
}

TEST_CASE("TransformCacheFlag: a copy is not valid, a move keeps the value")
{
	TransformComponent a;
	a.localCacheValid = true;
	CHECK(a.localCacheValid);
	TransformComponent b = a;
	CHECK_FALSE(b.localCacheValid);
	TransformComponent c;
	c.localCacheValid = true;
	c = a;
	CHECK_FALSE(c.localCacheValid);
	TransformComponent d = std::move(a);
	CHECK(d.localCacheValid);
}

TEST_CASE("propagateTransforms: a cell loaded and unloaded between two passes")
{
	HorizonWorld world;
	const Grouped g = buildGrouped(world, 6, 10);
	HE::propagateTransforms(world);

	// A cell: a root with a few things under it, saved and grafted back as the streamer does.
	std::vector<uint8_t> blob;
	{
		HorizonWorld src;
		const Entity root = src.createEntity("Cell");
		tf(src, root).position = glm::vec3(300.0f, 0.0f, 0.0f);
		for (int i = 0; i < 5; ++i)
		{
			const Entity e = src.createEntity("House");
			src.reparentEntity(e, root);
			tf(src, e).position = glm::vec3(static_cast<float>(i) * 4.0f, 0.0f, 1.0f);
		}
		SceneSerializer ser;
		REQUIRE(ser.saveToMemory(src, blob));
	}
	SceneSerializer ser;
	std::vector<Entity> created;
	REQUIRE(ser.loadAdditiveFromMemory(world, blob, &created));
	REQUIRE(!created.empty());
	for (const Entity e : created)
		if (world.registry().get<HierarchyComponent>(e).parent == world.rootEntity())
			tf(world, e).position = -glm::vec3(world.origin());

	HE::propagateTransforms(world);
	CHECK(HE::lastPropagateStats(world).full);
	CHECK(matchesFullWalk(world));

	// Quiet again after the load.
	HE::propagateTransforms(world);
	CHECK(HE::lastPropagateStats(world).recomputed == 0u);

	// The floating origin moves the cell with everything else, and the cell goes.
	HE::shiftWorldOrigin(world, nullptr, glm::vec3(200.0f, 0.0f, 0.0f));
	HE::propagateTransforms(world);
	CHECK(matchesFullWalk(world));
	for (const Entity e : created)
		if (world.registry().valid(e) && world.registry().get<HierarchyComponent>(e).parent == world.rootEntity())
		{
			world.destroyEntity(e);
			break;
		}
	tf(world, g.groups[2]).position.x += 1.0f;
	HE::propagateTransforms(world);
	CHECK(matchesFullWalk(world));
}

// ─── Random change sequences against the full walk ──────────────────────────
//
// Two worlds, built and changed the same way. One is brought up to date with
// propagateTransforms after every batch of changes, the other with the full walk, and
// then every world matrix of one is compared with the other, by bits. The changes are
// the ones the engine makes: position, rotation or scale written with and without the
// dirty flag, only the flag, reparent, spawn, destroy, a transform added or taken off,
// the floating origin, a whole component written back, sibling order, an entity outside
// the hierarchy, one that hangs off nothing, a cell grafted in, and a large share of the
// world at once (past the threshold of the scan).

namespace
{
struct Pair
{
	HorizonWorld a;   // propagateTransforms: the one under test
	HorizonWorld b;   // propagateTransformsFull: the reference
	std::vector<Entity> ea, eb;

	bool alive(size_t i) { return a.registry().valid(ea[i]); }
	bool hasTransform(size_t i) { return alive(i) && a.registry().all_of<TransformComponent>(ea[i]); }
	template <class F> void both(F&& f) { f(a, ea); f(b, eb); }
};

struct RunResult
{
	bool   diverged = false;
	int    step     = -1;
	size_t quiet    = 0;   // passes that wrote nothing
	size_t subtrees = 0;   // passes that recomputed some subtrees
	size_t full     = 0;   // passes that walked everything
	size_t ops[16]  = {};
};

// -1: none found. Entities that exist, and (for the transform kinds) have a transform.
int pickAlive(Pair& p, Rng& r, bool needTransform)
{
	const int n = static_cast<int>(p.ea.size());
	if (n == 0) return -1;
	for (int tries = 0; tries < 12; ++tries)
	{
		const int i = r.below(n);
		if (needTransform ? p.hasTransform(static_cast<size_t>(i)) : p.alive(static_cast<size_t>(i))) return i;
	}
	return -1;
}

void buildStart(Pair& p, Rng& r)
{
	// Six groups, one of them with no transform of its own, each with children and
	// grandchildren; a node with no transform in the middle of a chain.
	struct Spec { int parent; bool transform; };
	std::vector<Spec> spec;
	for (int gi = 0; gi < 6; ++gi)
	{
		const int g = static_cast<int>(spec.size());
		spec.push_back({ -1, gi != 3 });
		for (int ci = 0; ci < 8; ++ci)
		{
			const int c = static_cast<int>(spec.size());
			spec.push_back({ g, (ci % 5) != 4 });   // every fifth child is a bare node
			for (int di = 0; di < 3; ++di) spec.push_back({ c, true });
		}
	}
	// Random values drawn once, applied to both worlds.
	std::vector<glm::vec3> pos, rot, scl;
	for (size_t i = 0; i < spec.size(); ++i)
	{
		pos.push_back(pickPos(r));
		rot.push_back(pickRot(r));
		scl.push_back(pickScale(r));
	}
	p.both([&](HorizonWorld& w, std::vector<Entity>& ents)
	{
		for (size_t i = 0; i < spec.size(); ++i)
		{
			const Entity e = w.createEntity("N");
			ents.push_back(e);
			if (spec[i].parent >= 0) w.reparentEntity(e, ents[static_cast<size_t>(spec[i].parent)]);
			if (spec[i].transform)
			{
				TransformComponent t;
				t.position = pos[i]; t.rotation = rot[i]; t.scale = scl[i];
				w.registry().emplace<TransformComponent>(e, t);
			}
		}
		// One outside the hierarchy.
		const Entity loose = w.registry().create();
		TransformComponent t;
		t.position = glm::vec3(3.0f, 1.0f, 2.0f);
		w.registry().emplace<TransformComponent>(loose, t);
		ents.push_back(loose);
	});
}

enum Op : int
{
	SetTrs, SetTrsDirty, DirtyHint, Reparent, Spawn, Destroy, RemoveTransform, AddTransform,
	Shift, Overwrite, Sort, Loose, Orphan, CellLoad, Bulk, Idle, OpCount
};
// weights: values the engine writes most, structure changes less, the odd ones rarely
const int kWeight[OpCount] = { 30, 8, 3, 8, 6, 3, 3, 3, 2, 4, 2, 1, 1, 2, 2, 10 };

void applyOp(Pair& p, Rng& r, Op op, const std::vector<uint8_t>& cellBlob)
{
	switch (op)
	{
	case SetTrs:
	case SetTrsDirty:
	{
		const int i = pickAlive(p, r, true);
		if (i < 0) return;
		const int which = r.below(3);
		const glm::vec3 v = which == 0 ? pickPos(r) : which == 1 ? pickRot(r) : pickScale(r);
		p.both([&](HorizonWorld& w, std::vector<Entity>& ents)
		{
			TransformComponent& t = w.registry().get<TransformComponent>(ents[static_cast<size_t>(i)]);
			(which == 0 ? t.position : which == 1 ? t.rotation : t.scale) = v;
			if (op == SetTrsDirty) t.dirty = true;
		});
		return;
	}
	case DirtyHint:
	{
		const int i = pickAlive(p, r, true);
		if (i < 0) return;
		p.both([&](HorizonWorld& w, std::vector<Entity>& ents)
		{ w.registry().get<TransformComponent>(ents[static_cast<size_t>(i)]).dirty = true; });
		return;
	}
	case Reparent:
	{
		const int i = pickAlive(p, r, false);
		const int j = r.below(static_cast<int>(p.ea.size()) + 1) - 1;   // -1: the world root
		if (i < 0) return;
		if (j >= 0 && !p.alive(static_cast<size_t>(j))) return;
		p.both([&](HorizonWorld& w, std::vector<Entity>& ents)
		{ w.reparentEntity(ents[static_cast<size_t>(i)], j < 0 ? w.rootEntity() : ents[static_cast<size_t>(j)]); });
		return;
	}
	case Spawn:
	{
		const int parent = r.below(5) == 0 ? -1 : pickAlive(p, r, false);
		const bool transform = r.below(5) != 0;
		const glm::vec3 pos = pickPos(r);
		const glm::vec3 rot = pickRot(r);
		const glm::vec3 scl = pickScale(r);
		p.both([&](HorizonWorld& w, std::vector<Entity>& ents)
		{
			const Entity e = w.createEntity("Spawn");
			ents.push_back(e);
			if (parent >= 0) w.reparentEntity(e, ents[static_cast<size_t>(parent)]);
			if (transform)
			{
				TransformComponent t;
				t.position = pos; t.rotation = rot; t.scale = scl;
				w.registry().emplace<TransformComponent>(e, t);
			}
		});
		return;
	}
	case Destroy:
	{
		const int i = pickAlive(p, r, false);
		if (i < 0) return;
		p.both([&](HorizonWorld& w, std::vector<Entity>& ents)
		{
			// Only entities of the hierarchy: the loose ones and the orphan are not destroyed by
			// the world's own path, a plain registry destroy does it for them.
			if (w.registry().all_of<HierarchyComponent>(ents[static_cast<size_t>(i)]))
				w.destroyEntity(ents[static_cast<size_t>(i)]);
			else
				w.registry().destroy(ents[static_cast<size_t>(i)]);
		});
		return;
	}
	case RemoveTransform:
	{
		const int i = pickAlive(p, r, true);
		if (i < 0) return;
		p.both([&](HorizonWorld& w, std::vector<Entity>& ents)
		{ w.registry().remove<TransformComponent>(ents[static_cast<size_t>(i)]); });
		return;
	}
	case AddTransform:
	{
		const int i = pickAlive(p, r, false);
		if (i < 0 || p.hasTransform(static_cast<size_t>(i))) return;
		const glm::vec3 pos = pickPos(r);
		const glm::vec3 rot = pickRot(r);
		const glm::vec3 scl = pickScale(r);
		p.both([&](HorizonWorld& w, std::vector<Entity>& ents)
		{
			TransformComponent t;
			t.position = pos; t.rotation = rot; t.scale = scl;
			w.registry().emplace<TransformComponent>(ents[static_cast<size_t>(i)], t);
		});
		return;
	}
	case Shift:
	{
		const float sx = static_cast<float>((r.below(5) - 2) * 100);
		const float sz = static_cast<float>((r.below(5) - 2) * 100);
		const glm::vec3 shift(sx, 0.0f, sz);
		p.both([&](HorizonWorld& w, std::vector<Entity>&) { HE::shiftWorldOrigin(w, nullptr, shift); });
		return;
	}
	case Overwrite:
	{
		const int i = pickAlive(p, r, true);
		const int j = pickAlive(p, r, true);
		if (i < 0 || j < 0) return;
		p.both([&](HorizonWorld& w, std::vector<Entity>& ents)
		{
			TransformComponent copy = w.registry().get<TransformComponent>(ents[static_cast<size_t>(j)]);
			copy.dirty = false;   // the writer that does not say so
			w.registry().get<TransformComponent>(ents[static_cast<size_t>(i)]) = copy;
		});
		return;
	}
	case Sort:
	{
		const int i = pickAlive(p, r, false);
		const int delta = r.below(2) == 0 ? -1 : 1;
		const bool sort = r.below(3) == 0;
		if (i < 0) return;
		p.both([&](HorizonWorld& w, std::vector<Entity>& ents)
		{
			const Entity e = ents[static_cast<size_t>(i)];
			if (sort) w.sortChildrenByName(e);
			else      w.moveChild(e, delta);
		});
		return;
	}
	case Loose:
	{
		const glm::vec3 pos = pickPos(r);
		p.both([&](HorizonWorld& w, std::vector<Entity>& ents)
		{
			const Entity e = w.registry().create();
			TransformComponent t;
			t.position = pos;
			w.registry().emplace<TransformComponent>(e, t);
			ents.push_back(e);
		});
		return;
	}
	case Orphan:
	{
		// A hierarchy that is not under the world root: the full walk never reaches it.
		const glm::vec3 pos = pickPos(r);
		p.both([&](HorizonWorld& w, std::vector<Entity>& ents)
		{
			const Entity e = w.registry().create();
			w.registry().emplace<HierarchyComponent>(e);
			TransformComponent t;
			t.position = pos;
			w.registry().emplace<TransformComponent>(e, t);
			ents.push_back(e);
		});
		return;
	}
	case CellLoad:
	{
		p.both([&](HorizonWorld& w, std::vector<Entity>& ents)
		{
			SceneSerializer ser;
			std::vector<Entity> created;
			if (!ser.loadAdditiveFromMemory(w, cellBlob, &created)) return;
			for (const Entity e : created)
			{
				ents.push_back(e);
				// What the streamer does with the cell's root.
				if (w.registry().get<HierarchyComponent>(e).parent == w.rootEntity())
					w.registry().get_or_emplace<TransformComponent>(e).position = -glm::vec3(w.origin());
			}
		});
		return;
	}
	case Bulk:
	{
		// A share of the world at once: sometimes under the scan's threshold, sometimes past it.
		const int percent = r.below(4) == 0 ? 100 : 20 + r.below(70);
		std::vector<char> pick(p.ea.size(), 0);
		std::vector<glm::vec3> v(p.ea.size());
		for (size_t i = 0; i < p.ea.size(); ++i)
		{
			pick[i] = r.below(100) < percent;
			v[i]    = pickPos(r);
		}
		p.both([&](HorizonWorld& w, std::vector<Entity>& ents)
		{
			for (size_t i = 0; i < ents.size(); ++i)
				if (pick[i] && w.registry().valid(ents[i]))
					if (auto* t = w.registry().try_get<TransformComponent>(ents[i])) t->position = v[i];
		});
		return;
	}
	case Idle:
	case OpCount:
		return;
	}
}

bool worldsAgree(Pair& p)
{
	for (size_t i = 0; i < p.ea.size(); ++i)
	{
		const bool va = p.a.registry().valid(p.ea[i]);
		const bool vb = p.b.registry().valid(p.eb[i]);
		if (va != vb) return false;
		if (!va) continue;
		const auto* ta = p.a.registry().try_get<TransformComponent>(p.ea[i]);
		const auto* tb = p.b.registry().try_get<TransformComponent>(p.eb[i]);
		if ((ta == nullptr) != (tb == nullptr)) return false;
		if (ta && !sameBits(ta->worldMatrix, tb->worldMatrix)) return false;
	}
	return true;
}

std::vector<uint8_t> makeCellBlob()
{
	HorizonWorld src;
	const Entity root = src.createEntity("Cell");
	tf(src, root).position = glm::vec3(300.0f, 0.0f, 0.0f);
	for (int i = 0; i < 4; ++i)
	{
		const Entity e = src.createEntity("House");
		src.reparentEntity(e, root);
		tf(src, e).position = glm::vec3(static_cast<float>(i) * 4.0f, 0.0f, 1.0f);
		const Entity part = src.createEntity("Part");
		src.reparentEntity(part, e);
		tf(src, part).rotation = glm::vec3(0.0f, 30.0f * static_cast<float>(i), 0.0f);
	}
	std::vector<uint8_t> blob;
	SceneSerializer ser;
	REQUIRE(ser.saveToMemory(src, blob));
	return blob;
}

RunResult runDifferential(uint64_t seed, int steps)
{
	static const std::vector<uint8_t> cellBlob = makeCellBlob();
	RunResult res;
	Rng r(seed);
	Pair p;
	buildStart(p, r);
	HE::propagateTransforms(p.a);
	HE::propagateTransformsFull(p.b);

	int total = 0;
	for (int w : kWeight) total += w;

	for (int step = 0; step < steps; ++step)
	{
		const int batch = 1 + r.below(3);
		for (int k = 0; k < batch; ++k)
		{
			int roll = r.below(total);
			int op = 0;
			while (roll >= kWeight[op]) { roll -= kWeight[op]; ++op; }
			++res.ops[op];
			applyOp(p, r, static_cast<Op>(op), cellBlob);
		}
		HE::propagateTransforms(p.a);
		HE::propagateTransformsFull(p.b);

		const HE::PropagateStats s = HE::lastPropagateStats(p.a);
		if (s.full)               ++res.full;
		else if (s.recomputed == 0) ++res.quiet;
		else                      ++res.subtrees;

		if (!worldsAgree(p))
		{
			res.diverged = true;
			res.step     = step;
			return res;
		}
	}
	return res;
}
} // namespace

TEST_CASE("propagateTransforms: random change sequences are bit-identical to the full walk")
{
	size_t quiet = 0, subtrees = 0, full = 0;
	size_t ops[16] = {};
	for (uint64_t seed = 1; seed <= 16; ++seed)
	{
		const RunResult res = runDifferential(seed, 300);
		INFO("seed " << seed << ", diverged at step " << res.step);
		CHECK_FALSE(res.diverged);
		quiet += res.quiet; subtrees += res.subtrees; full += res.full;
		for (int i = 0; i < 16; ++i) ops[i] += res.ops[i];
	}
	// The run used every path: it is not a comparison of the full walk with itself.
	CHECK(quiet > 20u);
	CHECK(subtrees > 100u);
	CHECK(full > 100u);
	for (int i = 0; i < OpCount; ++i)
	{
		INFO("op " << i);
		CHECK(ops[i] > 0u);
	}
}

// The comparison above is only worth something if it can fail. Each detector switched off
// in turn, the same sequences: some seed has to diverge.
TEST_CASE("propagateTransforms: with one detector off, the random sequences diverge")
{
	const auto diverges = [](const HE::PropagateTestSwitches& sw)
	{
		Switches guard(sw);
		for (uint64_t seed = 1; seed <= 16; ++seed)
			if (runDifferential(seed, 300).diverged) return true;
		return false;
	};

	HE::PropagateTestSwitches off;
	CHECK_FALSE(diverges(off));   // control of the control: all on, none diverges

	HE::PropagateTestSwitches sw;
	sw.ignoreStructureEpoch = true;
	CHECK(diverges(sw));
	sw = HE::PropagateTestSwitches{};
	sw.ignoreInvalidation = true;
	CHECK(diverges(sw));
	sw = HE::PropagateTestSwitches{};
	sw.ignoreValueCompare = true;
	CHECK(diverges(sw));
	sw = HE::PropagateTestSwitches{};
	sw.ignoreTransformRemoval = true;
	CHECK(diverges(sw));
}

// ─── The numbers ────────────────────────────────────────────────────────────

// Not part of the normal run:
//   out/build/release/tests/he_tests --no-skip --test-case='Transform propagation bench*'
TEST_CASE("Transform propagation bench: the full walk against the scan and the changed subtrees" * doctest::skip())
{
	using Clock = std::chrono::steady_clock;
	const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
	const auto median = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };

	for (const bool flat : { false, true })
	for (const int n : { 1000, 10000, 50000, 100000, 200000 })
	{
		HorizonWorld world;
		const Grouped g = flat ? buildGrouped(world, 1, n) : buildGrouped(world, n / 100, 100);
		HE::propagateTransforms(world);
		HE::propagateTransforms(world);

		struct Pattern { const char* name; std::vector<Entity> who; };
		std::vector<Pattern> patterns;
		patterns.push_back({ "nothing moved", {} });
		patterns.push_back({ "one leaf", { g.leaves[g.leaves.size() / 2] } });
		{
			std::vector<Entity> v;
			for (size_t i = 0; i < g.leaves.size(); i += 100) v.push_back(g.leaves[i]);
			patterns.push_back({ "1% of the leaves", v });
		}
		if (!flat)
		{
			patterns.push_back({ "one group (100 leaves)", { g.groups[g.groups.size() / 2] } });
			std::vector<Entity> v;
			for (size_t i = 0; i < g.groups.size(); i += 10) v.push_back(g.groups[i]);
			patterns.push_back({ "10% of the groups", v });
		}
		patterns.push_back({ "every leaf", g.leaves });

		for (const Pattern& pat : patterns)
		{
			std::vector<double> inc, full;
			size_t recomputed = 0;
			for (int i = 0; i < 7; ++i)
			{
				for (const Entity e : pat.who) tf(world, e).position.x += 0.5f;
				Clock::time_point t0 = Clock::now();
				HE::propagateTransforms(world);
				inc.push_back(ms(Clock::now() - t0));
				recomputed = HE::lastPropagateStats(world).recomputed;

				for (const Entity e : pat.who) tf(world, e).position.x += 0.5f;
				t0 = Clock::now();
				HE::propagateTransformsFull(world);
				full.push_back(ms(Clock::now() - t0));
			}
			std::ostringstream line;
			line << (flat ? "flat " : "grouped ") << n << " entities, " << pat.name << ": full walk "
			     << median(full) << " ms, propagateTransforms " << median(inc) << " ms ("
			     << recomputed << " matrices written)";
			MESSAGE(line.str());
		}
	}
}
