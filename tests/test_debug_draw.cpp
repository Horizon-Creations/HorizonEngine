#include "doctest.h"
#include <DebugDraw/DebugDraw.h>
#include <cmath>

TEST_CASE("DebugDrawBuffer line() adds a single segment")
{
	DebugDrawBuffer buf;
	CHECK(buf.empty());
	buf.line({ 0,0,0 }, { 1,0,0 }, { 1,1,0 });
	CHECK(buf.lines().size() == 1);
	CHECK(buf.lines()[0].start.x == doctest::Approx(0.0f));
	CHECK(buf.lines()[0].end.x   == doctest::Approx(1.0f));
	CHECK(buf.lines()[0].color.r == doctest::Approx(1.0f));
}

TEST_CASE("DebugDrawBuffer aabb() produces exactly 12 edges")
{
	DebugDrawBuffer buf;
	buf.aabb({ -1,-1,-1 }, { 1,1,1 });
	CHECK(buf.lines().size() == 12);
}

TEST_CASE("DebugDrawBuffer aabb() corners are within the AABB bounds")
{
	DebugDrawBuffer buf;
	const glm::vec3 mn(-2.0f, -3.0f, -4.0f);
	const glm::vec3 mx( 5.0f,  6.0f,  7.0f);
	buf.aabb(mn, mx);
	for (const DebugLine& l : buf.lines())
	{
		for (const glm::vec3* v : { &l.start, &l.end })
		{
			CHECK(v->x >= mn.x - 0.001f); CHECK(v->x <= mx.x + 0.001f);
			CHECK(v->y >= mn.y - 0.001f); CHECK(v->y <= mx.y + 0.001f);
			CHECK(v->z >= mn.z - 0.001f); CHECK(v->z <= mx.z + 0.001f);
		}
	}
}

TEST_CASE("DebugDrawBuffer sphere() produces 3*segments lines")
{
	DebugDrawBuffer buf;
	const int segs = 16;
	buf.sphere({ 0,0,0 }, 1.0f, { 1,0,0 }, segs);
	CHECK(buf.lines().size() == size_t(3 * segs));
}

TEST_CASE("DebugDrawBuffer sphere() segment endpoints lie on the sphere surface")
{
	DebugDrawBuffer buf;
	const float radius = 3.0f;
	const glm::vec3 center(1.0f, 2.0f, 3.0f);
	buf.sphere(center, radius, { 1,1,0 }, 16);
	for (const DebugLine& l : buf.lines())
	{
		const float ds = glm::length(l.start - center);
		const float de = glm::length(l.end   - center);
		CHECK(ds == doctest::Approx(radius).epsilon(0.001f));
		CHECK(de == doctest::Approx(radius).epsilon(0.001f));
	}
}

TEST_CASE("DebugDrawBuffer clear() resets the buffer")
{
	DebugDrawBuffer buf;
	buf.line({ 0,0,0 }, { 1,0,0 });
	buf.aabb({ 0,0,0 }, { 1,1,1 });
	REQUIRE(!buf.empty());
	buf.clear();
	CHECK(buf.empty());
	CHECK(buf.lines().empty());
}

TEST_CASE("DebugDrawBuffer default line color is yellow")
{
	DebugDrawBuffer buf;
	buf.line({ 0,0,0 }, { 1,0,0 });
	const glm::vec3& c = buf.lines()[0].color;
	CHECK(c.r == doctest::Approx(1.0f));
	CHECK(c.g == doctest::Approx(1.0f));
	CHECK(c.b == doctest::Approx(0.0f));
}

TEST_CASE("DebugDrawBuffer multiple primitives accumulate correctly")
{
	DebugDrawBuffer buf;
	buf.line({ 0,0,0 }, { 1,0,0 });         // 1
	buf.aabb({ 0,0,0 }, { 1,1,1 });          // +12 = 13
	buf.sphere({ 0,0,0 }, 1.0f, {}, 8);      // +24 = 37
	CHECK(buf.lines().size() == 37u);
}

TEST_CASE("DebugDrawBuffer append() adds kept lines after the ones already there")
{
	// The editor's ground grid is built once per camera position and appended
	// from a kept list every other frame; order and values must survive that.
	DebugDrawBuffer kept;
	kept.line({ 1,0,0 }, { 2,0,0 }, { 0,1,0 });
	kept.line({ 3,0,0 }, { 4,0,0 }, { 0,0,1 });

	DebugDrawBuffer buf;
	buf.line({ 0,0,0 }, { 0,1,0 });
	buf.append(kept.lines());
	REQUIRE(buf.lines().size() == 3u);
	CHECK(buf.lines()[0].end.y   == doctest::Approx(1.0f));
	CHECK(buf.lines()[1].start.x == doctest::Approx(1.0f));
	CHECK(buf.lines()[1].color.g == doctest::Approx(1.0f));
	CHECK(buf.lines()[2].end.x   == doctest::Approx(4.0f));
	CHECK(buf.lines()[2].color.b == doctest::Approx(1.0f));
	CHECK(kept.lines().size() == 2u);   // the source is left as it was

	buf.append({});
	CHECK(buf.lines().size() == 3u);
}

TEST_CASE("DebugDrawBuffer clear() keeps the storage for the next frame")
{
	// The editor keeps one buffer across frames and clears it at the top of
	// each; that only saves the per-frame regrowth if clear() does not give the
	// storage back.
	DebugDrawBuffer buf;
	for (int i = 0; i < 1000; ++i) buf.line({ 0,0,0 }, { 1,0,0 });
	const DebugLine* storage = buf.lines().data();
	buf.clear();
	CHECK(buf.empty());
	for (int i = 0; i < 1000; ++i) buf.line({ 0,0,0 }, { 1,0,0 });
	CHECK(buf.lines().data() == storage);
}
