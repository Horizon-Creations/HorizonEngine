#include "doctest.h"

#include "EditorUndo.h"
#include "ViewportPick.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/EntityVisibility.h>
#include <HorizonScene/Components/EditorLockComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/RopeComponent.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <string>
#include <vector>

// ── The Outliner's per-row verbs, without the Outliner ───────────────────────
// The eye, the padlock and the sibling order are three edits a row offers now.
// None of them is an ImGui matter: the eye is EntityVisibility.h, the padlock is
// EditorLockComponent plus the viewport's refusal to pick it, the order is two
// HorizonWorld calls. Each of those has a rule a test can state, and this file
// states them — above all the ones that only show up later: a lock that an
// undo forgets, a hidden prop the file does not remember.

namespace
{
Entity childOf(HorizonWorld& world, Entity parent, const char* name)
{
	const Entity e = world.createEntity(name);
	world.reparentEntity(e, parent);
	return e;
}

std::vector<std::string> childNames(HorizonWorld& world, Entity parent)
{
	std::vector<std::string> out;
	for (const Entity c : world.registry().get<HierarchyComponent>(parent).children)
		out.push_back(world.registry().get<NameComponent>(c).name);
	return out;
}

Entity findByName(HorizonWorld& w, const std::string& name)
{
	for (auto [e, n] : w.registry().view<NameComponent>().each())
		if (n.name == name) return e;
	return entt::null;
}
} // namespace

// ── Eye ──────────────────────────────────────────────────────────────────────

TEST_CASE("EntityVisibility: the eye reads the subtree, and flips all of it")
{
	HorizonWorld world;
	auto& reg = world.registry();

	// House ▸ (Door[mesh], Lamp[light], Porch ▸ Rope[rope]); Garden is empty.
	const Entity house  = childOf(world, world.rootEntity(), "House");
	const Entity door   = childOf(world, house, "Door");
	const Entity lamp   = childOf(world, house, "Lamp");
	const Entity porch  = childOf(world, house, "Porch");
	const Entity rope   = childOf(world, porch, "Rope");
	const Entity garden = childOf(world, world.rootEntity(), "Garden");
	reg.emplace<MeshComponent>(door, MeshComponent{ HE::kDefaultCubeMeshId });
	reg.emplace<LightComponent>(lamp, LightComponent{});
	reg.emplace<RopeComponent>(rope, RopeComponent{});

	using HE::Visibility;
	// A group with nothing to draw under it has no eye; one with renderables
	// anywhere below reads as visible even though it draws nothing itself.
	CHECK(HE::entityVisibility(reg, house) == Visibility::None);
	CHECK(HE::subtreeVisibility(reg, house) == Visibility::Visible);
	CHECK(HE::subtreeVisibility(reg, garden) == Visibility::None);
	CHECK(HE::subtreeVisibility(reg, door) == Visibility::Visible);

	// Hiding the house reaches the door, the lamp AND the rope two levels down.
	HE::setSubtreeVisible(reg, house, false);
	CHECK_FALSE(reg.get<MeshComponent>(door).visible);
	CHECK_FALSE(reg.get<LightComponent>(lamp).visible);
	CHECK_FALSE(reg.get<RopeComponent>(rope).visible);
	CHECK(HE::subtreeVisibility(reg, house) == Visibility::Hidden);
	CHECK(HE::subtreeVisibility(reg, porch) == Visibility::Hidden);
	CHECK(HE::entityVisibility(reg, door) == Visibility::Hidden);

	// One thing shown again under it: the house's eye opens (something is
	// drawn), the porch's stays shut (its rope still is not).
	HE::setEntityVisible(reg, door, true);
	CHECK(HE::subtreeVisibility(reg, house) == Visibility::Visible);
	CHECK(HE::subtreeVisibility(reg, porch) == Visibility::Hidden);

	// Per-entity does not reach the children — that is what the subtree
	// variant is for, and the runtime (zone hiding) relies on it not doing so.
	HE::setEntityVisible(reg, house, true);
	CHECK_FALSE(reg.get<RopeComponent>(rope).visible);
}

TEST_CASE("EntityVisibility: the hidden flag is scene data and comes back from the file")
{
	HorizonWorld world;
	auto& reg = world.registry();
	const Entity crate = childOf(world, world.rootEntity(), "Crate");
	reg.emplace<MeshComponent>(crate, MeshComponent{ HE::kDefaultCubeMeshId });
	HE::setSubtreeVisible(reg, crate, false);

	SceneSerializer ser;
	std::vector<uint8_t> blob;
	REQUIRE(ser.saveToMemory(world, blob));
	HorizonWorld loaded;
	REQUIRE(ser.loadFromMemory(loaded, blob));
	const Entity again = findByName(loaded, "Crate");
	REQUIRE((again != entt::null));
	CHECK(HE::subtreeVisibility(loaded.registry(), again) == HE::Visibility::Hidden);
}

// ── Padlock ──────────────────────────────────────────────────────────────────

TEST_CASE("EditorLock: written beside the name, read back by the scene loaders, never by a prefab")
{
	HorizonWorld world;
	auto& reg = world.registry();
	const Entity floor = childOf(world, world.rootEntity(), "Floor");
	const Entity crate = childOf(world, world.rootEntity(), "Crate");
	reg.emplace<EditorLockComponent>(floor);

	SceneSerializer ser;
	std::vector<uint8_t> blob;
	REQUIRE(ser.saveToMemory(world, blob));

	// The full load — what the editor, play-stop and undo all go through.
	{
		HorizonWorld loaded;
		REQUIRE(ser.loadFromMemory(loaded, blob));
		CHECK(loaded.registry().all_of<EditorLockComponent>(findByName(loaded, "Floor")));
		CHECK_FALSE(loaded.registry().all_of<EditorLockComponent>(findByName(loaded, "Crate")));
	}
	// The additive merge: the entity arrives as it was in its own scene.
	{
		HorizonWorld target;
		REQUIRE(ser.loadAdditiveFromMemory(target, blob));
		CHECK(target.registry().all_of<EditorLockComponent>(findByName(target, "Floor")));
	}
	// A prefab made from a locked entity does not carry the lock: the lock is
	// where the user parked the mouse in THIS scene, not part of the asset.
	{
		const std::vector<uint8_t> prefab = ser.serializeSubtree(world, floor);
		HorizonWorld target;
		const Entity placed = ser.instantiatePrefab(target, prefab);
		REQUIRE((placed != entt::null));
		CHECK_FALSE(target.registry().all_of<EditorLockComponent>(placed));
	}
	(void)crate;
}

TEST_CASE("EditorLock: survives an undo and is itself undone")
{
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	const Entity floor = childOf(world, world.rootEntity(), "Floor");
	world.registry().emplace<EditorLockComponent>(floor);

	// An unrelated edit after the lock: undoing it rebuilds the world from a
	// snapshot taken WITH the lock, so the lock must be in that snapshot.
	undo.snapshotNow();
	world.createEntity("Later");
	REQUIRE(undo.undo());
	CHECK(world.registry().all_of<EditorLockComponent>(findByName(world, "Floor")));

	// And the lock is an edit of its own: snapshot, lock, undo → unlocked.
	HorizonWorld world2;
	EditorUndo   undo2;
	undo2.setWorld(&world2);
	const Entity wall = childOf(world2, world2.rootEntity(), "Wall");
	undo2.snapshotNow();
	world2.registry().emplace<EditorLockComponent>(wall);
	REQUIRE(undo2.undo());
	CHECK_FALSE(world2.registry().all_of<EditorLockComponent>(findByName(world2, "Wall")));
}

namespace
{
constexpr glm::vec2 kRectMin(40.0f, 24.0f);
constexpr glm::vec2 kRectSize(900.0f, 600.0f);

EditorCameraOverride editorCamAt(const glm::vec3& eye)
{
	EditorCameraOverride cam;
	cam.active     = true;
	cam.position   = eye;
	cam.view       = glm::lookAt(eye, eye + glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
	cam.fovDegrees = 60.0f;
	return cam;
}

Entity placeCube(HorizonWorld& world, const char* name, const glm::vec3& pos)
{
	const Entity e = world.createEntity(name);
	TransformComponent t;
	t.position = pos;
	world.registry().emplace_or_replace<TransformComponent>(e, t);
	world.registry().emplace<MeshComponent>(e, MeshComponent{ HE::kDefaultCubeMeshId });
	return e;
}

glm::vec2 screenOf(const RenderWorld& rw, const glm::vec3& world)
{
	const glm::vec4 clip = rw.camera.projection * rw.camera.view * glm::vec4(world, 1.0f);
	REQUIRE(clip.w > 1e-6f);
	const glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);
	return kRectMin + glm::vec2((ndc.x + 1.0f) * 0.5f, (1.0f - ndc.y) * 0.5f) * kRectSize;
}
} // namespace

TEST_CASE("EditorLock: a locked entity is not under the cursor — the click lands behind it")
{
	HorizonWorld world;
	auto& reg = world.registry();
	ContentManager cm;
	// Two cubes on one line of sight: the near one in front of the far one.
	const Entity nearCube = placeCube(world, "Near", { 0.0f, 0.0f, -5.0f });
	const Entity farCube  = placeCube(world, "Far",  { 0.0f, 0.0f, -12.0f });

	RenderExtractor ex;
	RenderWorld     rw;
	const EditorCameraOverride cam = editorCamAt({ 0, 0, 0 });
	ex.extract(world, rw, kRectSize.x / kRectSize.y, &cam);
	REQUIRE(rw.objects.size() == 2);

	const ViewportPick::BoxLookup boxes = [](const HE::UUID&) -> const HE::AABB* { return nullptr; };
	const glm::mat4 viewProj = rw.camera.projection * rw.camera.view;
	const glm::vec2 px = screenOf(rw, { 0.0f, 0.0f, -5.0f });

	// Unlocked: the near cube, as always.
	CHECK(ViewportPick::pickAtScreen(rw, reg, boxes, viewProj, kRectMin, kRectSize, px) == nearCube);
	// Locked: the click passes through it to the far cube.
	reg.emplace<EditorLockComponent>(nearCube);
	CHECK(ViewportPick::pickAtScreen(rw, reg, boxes, viewProj, kRectMin, kRectSize, px) == farCube);
	// Both locked: nothing.
	reg.emplace<EditorLockComponent>(farCube);
	CHECK((ViewportPick::pickAtScreen(rw, reg, boxes, viewProj, kRectMin, kRectSize, px) == entt::null));
	// Unlocked again: back to the near one — the lock is the only difference.
	reg.remove<EditorLockComponent>(nearCube);
	CHECK(ViewportPick::pickAtScreen(rw, reg, boxes, viewProj, kRectMin, kRectSize, px) == nearCube);
	(void)cm;
}

// ── Sibling order ────────────────────────────────────────────────────────────

TEST_CASE("HorizonWorld::moveChild shifts one row among its siblings, clamped at the ends")
{
	HorizonWorld world;
	const Entity group = childOf(world, world.rootEntity(), "Group");
	const Entity a = childOf(world, group, "A");
	const Entity b = childOf(world, group, "B");
	const Entity c = childOf(world, group, "C");
	REQUIRE(childNames(world, group) == std::vector<std::string>{ "A", "B", "C" });
	world.clearHierarchyDirty();

	CHECK(world.moveChild(b, -1));
	CHECK(childNames(world, group) == std::vector<std::string>{ "B", "A", "C" });
	CHECK(world.isHierarchyDirty());   // the Outliner rebuilds from this

	// Already first: nothing moves, nothing is dirtied.
	world.clearHierarchyDirty();
	CHECK_FALSE(world.moveChild(b, -1));
	CHECK_FALSE(world.isHierarchyDirty());

	// Two down from the top passes over both others and lands last; further
	// than the end is clamped, not refused.
	CHECK(world.moveChild(b, +2));
	CHECK(childNames(world, group) == std::vector<std::string>{ "A", "C", "B" });
	CHECK(world.moveChild(a, +10));
	CHECK(childNames(world, group) == std::vector<std::string>{ "C", "B", "A" });

	// The root has no siblings; a delta of zero is not a move.
	CHECK_FALSE(world.moveChild(world.rootEntity(), +1));
	CHECK_FALSE(world.moveChild(c, 0));

	// The order is what the file remembers.
	SceneSerializer ser;
	std::vector<uint8_t> blob;
	REQUIRE(ser.saveToMemory(world, blob));
	HorizonWorld loaded;
	REQUIRE(ser.loadFromMemory(loaded, blob));
	CHECK(childNames(loaded, findByName(loaded, "Group")) == std::vector<std::string>{ "C", "B", "A" });
}

TEST_CASE("HorizonWorld::sortChildrenByName sorts one level, case-insensitively, keeping ties")
{
	HorizonWorld world;
	const Entity group = childOf(world, world.rootEntity(), "Group");
	childOf(world, group, "torch");
	const Entity deep = childOf(world, group, "Bench");
	childOf(world, group, "apple");
	const Entity tie1 = childOf(world, group, "Same");
	const Entity tie2 = childOf(world, group, "Same");
	// A deeper level in reverse order, to prove the sort stops at one level.
	childOf(world, deep, "z");
	childOf(world, deep, "a");
	world.clearHierarchyDirty();

	CHECK(world.sortChildrenByName(group));
	CHECK(world.isHierarchyDirty());
	CHECK(childNames(world, group) == std::vector<std::string>{ "apple", "Bench", "Same", "Same", "torch" });
	// The two "Same" keep the order they had (stable).
	const auto& ch = world.registry().get<HierarchyComponent>(group).children;
	CHECK(std::find(ch.begin(), ch.end(), tie1) < std::find(ch.begin(), ch.end(), tie2));
	// Bench's own children were not touched.
	CHECK(childNames(world, deep) == std::vector<std::string>{ "z", "a" });

	// Already sorted: no change, no dirty flag.
	world.clearHierarchyDirty();
	CHECK_FALSE(world.sortChildrenByName(group));
	CHECK_FALSE(world.isHierarchyDirty());
	// A leaf has nothing to sort.
	CHECK_FALSE(world.sortChildrenByName(tie1));
}
