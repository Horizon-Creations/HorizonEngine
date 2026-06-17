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
