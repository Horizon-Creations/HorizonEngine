#include "doctest.h"

#include "../src/HE_Editor/EditorSelection.h"
#include "../src/HE_Editor/FrustumLines.h"
#include "../src/HE_Editor/ViewportOverlays.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>

#include <glm/glm.hpp>

#include <cmath>

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

// ─── The reach of a selected light or camera ─────────────────────────────────
// Drawn only for what is selected, at the world pose, in the light's own hue:
// a point light's range as a sphere, a spot's cone along its -Z, a camera's
// view volume.

TEST_CASE("ViewportOverlays: a selected point light draws its range, at the world position")
{
	HorizonWorld world;
	auto& reg = world.registry();
	const Entity light = childOf(world, { 10.0f, 0.0f, 0.0f }, 1.0f, { 1.0f, 2.0f, 0.0f });
	LightComponent lc;
	lc.type  = HE::LightType::Point;
	lc.range = 4.0f;
	lc.color = { 0.4f, 0.2f, 0.1f };   // dim: shown at its hue, brightest channel 1
	reg.emplace<LightComponent>(light, lc);

	DebugDrawBuffer none;
	O::appendSelectedLightAndCameraShapes(world, EditorSelection{}, glm::vec3(0.0f, 0.0f, 30.0f),
	                                      16.0f / 9.0f, none);
	CHECK(none.empty());   // not selected, nothing drawn

	EditorSelection sel;
	sel.set(light);
	DebugDrawBuffer buf;
	O::appendSelectedLightAndCameraShapes(world, sel, glm::vec3(0.0f, 0.0f, 30.0f), 16.0f / 9.0f, buf);

	REQUIRE(static_cast<int>(buf.lines().size()) == 3 * O::kRangeSphereSegments);
	const glm::vec3 centre{ 11.0f, 2.0f, 0.0f };
	for (const DebugLine& l : buf.lines())
	{
		CHECK(glm::length(l.start - centre) == doctest::Approx(4.0f));
		CHECK(glm::length(l.end   - centre) == doctest::Approx(4.0f));
	}
	checkVec(buf.lines().front().color, { 1.0f, 0.5f, 0.25f });
}

TEST_CASE("ViewportOverlays: a selected spot light draws its cone along -Z at half its angle")
{
	HorizonWorld world;
	auto& reg = world.registry();
	const Entity e = world.createEntity("Spot");
	TransformComponent tc;
	tc.position = { 0.0f, 3.0f, 0.0f };
	tc.rotation = { 0.0f, 90.0f, 0.0f };   // yaw 90°: -Z turns to -X
	reg.emplace<TransformComponent>(e, tc);
	LightComponent lc;
	lc.type      = HE::LightType::Spot;
	lc.range     = 6.0f;
	lc.spotAngle = 60.0f;                  // FULL angle, so 30° to each side
	reg.emplace<LightComponent>(e, lc);

	EditorSelection sel;
	sel.set(e);
	DebugDrawBuffer buf;
	O::appendSelectedLightAndCameraShapes(world, sel, glm::vec3(0.0f, 0.0f, 30.0f), 1.0f, buf);

	REQUIRE(static_cast<int>(buf.lines().size())
	        == O::kSpotRingSegments + O::kSpotSideLines + 2 * O::kSpotArcSegments);
	const glm::vec3 apex = tc.position;
	const glm::vec3 dir{ -1.0f, 0.0f, 0.0f };
	const float     half = glm::radians(30.0f);
	for (const DebugLine& l : buf.lines())
		for (const glm::vec3& p : { l.start, l.end })
		{
			if (glm::length(p - apex) < 1e-4f) continue;   // a side line's apex end
			// Everything else lies on the sphere of `range`, inside the cone.
			CHECK(glm::length(p - apex) == doctest::Approx(6.0f));
			const float c = glm::dot(glm::normalize(p - apex), dir);
			CHECK(c >= std::cos(half) - 1e-4f);
		}
	// The ring (the first lines) stands exactly on the cone's edge.
	const glm::vec3 ringPt = buf.lines().front().start;
	CHECK(glm::dot(glm::normalize(ringPt - apex), dir) == doctest::Approx(std::cos(half)));
	checkVec(buf.lines().front().color, glm::vec3(1.0f));   // white light, white lines
}

TEST_CASE("ViewportOverlays: a selected camera draws its frustum the way it looks")
{
	HorizonWorld world;
	auto& reg = world.registry();
	const Entity e = world.createEntity("Cam");
	TransformComponent tc;
	tc.position = { 2.0f, 1.0f, 0.0f };
	reg.emplace<TransformComponent>(e, tc);
	CameraComponent cc;
	cc.fovDegrees = 60.0f;
	reg.emplace<CameraComponent>(e, cc);

	EditorSelection sel;
	sel.set(e);
	const glm::vec3 viewer{ 2.0f, 1.0f, 20.0f };   // 20 m behind it
	DebugDrawBuffer buf;
	O::appendSelectedLightAndCameraShapes(world, sel, viewer, 2.0f, buf);

	REQUIRE(static_cast<int>(buf.lines().size()) == HE::Ed::FrustumLines::kLinesPerFrustum);
	// The first four lines run from the eye to the far corners: -Z ahead, the
	// vertical half-opening tan(30°), the horizontal one twice that (aspect 2).
	const float length = 20.0f * 0.12f;
	for (int i = 0; i < 4; ++i)
	{
		const DebugLine& l = buf.lines()[i];
		checkVec(l.start, tc.position);
		const glm::vec3 d = l.end - tc.position;
		CHECK(d.z == doctest::Approx(-length));
		CHECK(std::abs(d.y) == doctest::Approx(length * std::tan(glm::radians(30.0f))));
		CHECK(std::abs(d.x) == doctest::Approx(2.0f * std::abs(d.y)));
	}
	CHECK(buf.lines().front().color == O::kCameraFrustumColor);

	SUBCASE("orthographic: a box, same line count")
	{
		reg.get<CameraComponent>(e).orthographic = true;
		DebugDrawBuffer box;
		O::appendSelectedLightAndCameraShapes(world, sel, viewer, 2.0f, box);
		REQUIRE(static_cast<int>(box.lines().size()) == HE::Ed::FrustumLines::kLinesPerFrustum);
		const DebugLine& side = box.lines().front();
		CHECK((side.end - side.start).x == doctest::Approx(0.0f));   // parallel sides
		CHECK(std::abs(side.start.x - tc.position.x) == doctest::Approx(10.0f));   // 5 × aspect
	}
	SUBCASE("the viewer standing in the camera sees no frustum")
	{
		DebugDrawBuffer inside;
		O::appendSelectedLightAndCameraShapes(world, sel, tc.position, 2.0f, inside);
		CHECK(inside.empty());
	}
}

TEST_CASE("ViewportOverlays: directional lights and plain entities draw no reach")
{
	HorizonWorld world;
	auto& reg = world.registry();
	const Entity sun = world.createEntity("Dir");
	reg.emplace<TransformComponent>(sun);
	LightComponent lc;
	lc.type = HE::LightType::Directional;
	reg.emplace<LightComponent>(sun, lc);
	const Entity plain = world.createEntity("Plain");
	reg.emplace<TransformComponent>(plain);

	EditorSelection sel;
	sel.set(sun);
	sel.add(plain);
	DebugDrawBuffer buf;
	O::appendSelectedLightAndCameraShapes(world, sel, glm::vec3(0.0f, 0.0f, 10.0f), 1.0f, buf);
	CHECK(buf.empty());
}

TEST_CASE("lightDisplayColor keeps the hue and lifts the brightest channel to one")
{
	checkVec(HE::lightDisplayColor({ 0.2f, 0.1f, 0.0f }), { 1.0f, 0.5f, 0.0f });
	checkVec(HE::lightDisplayColor({ 4.0f, 2.0f, 2.0f }), { 1.0f, 0.5f, 0.5f });
	checkVec(HE::lightDisplayColor(glm::vec3(0.0f)), glm::vec3(1.0f));   // no hue: white
}
