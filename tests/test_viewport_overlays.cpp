#include "doctest.h"

#include "../src/HE_Editor/EditorSelection.h"
#include "../src/HE_Editor/ViewportOverlays.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>

#include <glm/glm.hpp>

// ─── Selection boxes and collider outlines under a moved parent ──────────────
// TransformComponent::position is LOCAL. The viewport's editor icons and the
// physics bodies both stand at the WORLD position, so the amber box that says
// "this one is selected" and the cyan outline that says "this is where it
// collides" have to stand there too — for an entity at the top level the two
// are the same number, and only a child of a moved or scaled parent tells
// them apart. That is the case these ask about.

namespace O = HE::Ed::ViewportOverlays;

namespace {

struct Box { glm::vec3 lo, hi; };

// The extent of every endpoint in the buffer. The cases below put exactly one
// box into each buffer, so this is that box.
Box extentOf(const DebugDrawBuffer& buf)
{
	Box b{ glm::vec3(1e30f), glm::vec3(-1e30f) };
	for (const DebugLine& l : buf.lines())
		for (const glm::vec3& p : { l.start, l.end })
		{
			b.lo = glm::min(b.lo, p);
			b.hi = glm::max(b.hi, p);
		}
	return b;
}

void checkVec(const glm::vec3& got, const glm::vec3& want)
{
	CHECK(got.x == doctest::Approx(want.x));
	CHECK(got.y == doctest::Approx(want.y));
	CHECK(got.z == doctest::Approx(want.z));
}

// A parent at `parentPos` (scaled uniformly by `parentScale`) with one child at
// `childLocal`, both with a TransformComponent.
Entity childOf(HorizonWorld& world, const glm::vec3& parentPos, float parentScale,
               const glm::vec3& childLocal)
{
	auto& reg = world.registry();
	const Entity p = world.createEntity("Parent");
	TransformComponent ptc;
	ptc.position = parentPos;
	ptc.scale    = glm::vec3(parentScale);
	reg.emplace<TransformComponent>(p, ptc);

	const Entity c = world.createEntity("Child");
	TransformComponent ctc;
	ctc.position = childLocal;
	reg.emplace<TransformComponent>(c, ctc);
	REQUIRE(world.reparentEntity(c, p));
	return c;
}

} // namespace

TEST_CASE("ViewportOverlays: the selection box stands at the child's world position")
{
	HorizonWorld world;
	const Entity child = childOf(world, { 10.0f, 0.0f, 0.0f }, 1.0f, { 1.0f, 2.0f, 0.0f });

	EditorSelection sel;
	sel.set(child);
	DebugDrawBuffer buf;
	O::appendSelectionMarkers(world, sel, buf);

	REQUIRE(buf.lines().size() == 12);
	const Box b = extentOf(buf);
	checkVec((b.lo + b.hi) * 0.5f, { 11.0f, 2.0f, 0.0f });
	checkVec(b.hi - b.lo, glm::vec3(1.0f));   // still the unit marker
	CHECK(buf.lines().front().color == O::kPrimarySelectionColor);
}

TEST_CASE("ViewportOverlays: a primitive collider outline follows the parent, not its scale")
{
	// Box / Sphere / Capsule collide at their AUTHORED size (PhysicsWorld's
	// buildColliderShape, "KNOWN LIMITATION"), so the outline moves with the
	// parent but does not grow with it — drawing it scaled would draw a size
	// the physics does not have.
	HorizonWorld world;
	auto& reg = world.registry();
	const Entity child = childOf(world, { 0.0f, 5.0f, 0.0f }, 2.0f, { 1.0f, 0.0f, 0.0f });
	ColliderComponent col;
	col.shape       = ColliderShape::Box;
	col.halfExtents = { 0.5f, 1.0f, 1.5f };
	reg.emplace<ColliderComponent>(child, col);

	ContentManager cm;
	DebugDrawBuffer buf;
	O::appendColliderWireframes(world, cm, buf);

	REQUIRE(buf.lines().size() == 12);
	const Box b = extentOf(buf);
	checkVec((b.lo + b.hi) * 0.5f, { 2.0f, 5.0f, 0.0f });   // parent + 2 × local
	checkVec(b.hi - b.lo, { 1.0f, 2.0f, 3.0f });
	CHECK(buf.lines().front().color == O::kColliderColor);
}

TEST_CASE("ViewportOverlays: a mesh-shaped collider outline takes the composed scale")
{
	// Mesh and Convex Hull are built from the triangles at the WORLD scale
	// (PhysicsWorld passes decomposeWorld(worldMatrixOf(...)).scale), so a
	// unit cube under a ×2 parent collides as a 2 m cube, and that is the box
	// to draw.
	ContentManager cm;
	StaticMeshAsset mesh;
	mesh.vertices = { -0.5f, -0.5f, -0.5f,   0.5f, 0.5f, 0.5f,   0.5f, -0.5f, 0.5f };
	const HE::UUID meshId = cm.registerStaticMesh(std::move(mesh));
	REQUIRE(meshId != HE::UUID{});

	HorizonWorld world;
	auto& reg = world.registry();
	const Entity child = childOf(world, { -4.0f, 0.0f, 3.0f }, 2.0f, { 0.0f, 1.0f, 0.0f });
	reg.emplace<MeshComponent>(child, MeshComponent{ meshId });
	ColliderComponent col;
	col.shape     = ColliderShape::ConvexHull;
	col.isTrigger = true;
	reg.emplace<ColliderComponent>(child, col);

	DebugDrawBuffer buf;
	O::appendColliderWireframes(world, cm, buf);

	REQUIRE(buf.lines().size() == 12);
	const Box b = extentOf(buf);
	checkVec((b.lo + b.hi) * 0.5f, { -4.0f, 2.0f, 3.0f });
	checkVec(b.hi - b.lo, glm::vec3(2.0f));
	CHECK(buf.lines().front().color == O::kTriggerColor);
}

TEST_CASE("ViewportOverlays: at the top level nothing moves")
{
	// The case every scene without hierarchy is: local IS world, and the
	// markers must land exactly where they always have.
	HorizonWorld world;
	auto& reg = world.registry();
	const Entity e = world.createEntity("Loose");
	TransformComponent tc;
	tc.position = { 3.0f, -1.0f, 7.0f };
	reg.emplace<TransformComponent>(e, tc);
	ColliderComponent col;
	col.shape = ColliderShape::Box;
	reg.emplace<ColliderComponent>(e, col);

	EditorSelection sel;
	sel.set(e);
	ContentManager cm;
	DebugDrawBuffer selBuf, colBuf;
	O::appendSelectionMarkers(world, sel, selBuf);
	O::appendColliderWireframes(world, cm, colBuf);

	checkVec((extentOf(selBuf).lo + extentOf(selBuf).hi) * 0.5f, tc.position);
	checkVec((extentOf(colBuf).lo + extentOf(colBuf).hi) * 0.5f, tc.position);
}
