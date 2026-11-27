// The camera flight's path (EditorFlythrough.h): parsing, the curve through the keys, lookAt, yaw the short way round.
#include "doctest.h"

#include "EditorFlythrough.h"

#include <cmath>

using HE::Ed::FlyPath;

TEST_CASE("flythrough: the path passes through every key and holds the ends")
{
	FlyPath p;
	REQUIRE(HE::Ed::parseFlyPath(R"({ "fps": 24, "warmupSeconds": 2,
		"keys": [ { "t": 0, "pos": [0, 10, 0], "yaw": 0, "pitch": 0 },
		          { "t": 4, "pos": [100, 20, 0], "yaw": 90, "pitch": -10 },
		          { "t": 8, "pos": [100, 30, 100], "yaw": 180, "pitch": 0 } ] })", p));
	CHECK(p.fps == doctest::Approx(24.0f));
	CHECK(p.duration() == doctest::Approx(8.0));
	glm::dvec3 pos;
	float yaw = 0.0f, pitch = 0.0f;
	HE::Ed::sampleFlyPath(p, 4.0, pos, yaw, pitch);
	CHECK(pos.x == doctest::Approx(100.0));
	CHECK(pos.y == doctest::Approx(20.0));
	CHECK(yaw == doctest::Approx(90.0f));
	CHECK(pitch == doctest::Approx(-10.0f));
	HE::Ed::sampleFlyPath(p, -1.0, pos, yaw, pitch);   // before the first key: the first key
	CHECK(pos.y == doctest::Approx(10.0));
	HE::Ed::sampleFlyPath(p, 99.0, pos, yaw, pitch);   // after the last: the last
	CHECK(pos.z == doctest::Approx(100.0));
	HE::Ed::sampleFlyPath(p, 2.0, pos, yaw, pitch);    // between: on the way, not at either end
	CHECK(pos.x > 0.0);
	CHECK(pos.x < 100.0);
}

TEST_CASE("flythrough: lookAt gives the angles of EditorCamera::forward, and yaw turns the short way")
{
	FlyPath p;
	REQUIRE(HE::Ed::parseFlyPath(R"({ "keys": [ { "t": 0, "pos": [0, 0, 0], "lookAt": [10, 0, 0] },
		                                            { "t": 1, "pos": [0, 0, 0], "lookAt": [0, 10, -10] },
		                                            { "t": 2, "pos": [0, 0, 0], "yaw": 170 },
		                                            { "t": 3, "pos": [0, 0, 0], "yaw": -170 } ] })", p));
	CHECK(p.keys[0].yawDeg == doctest::Approx(90.0f));     // +X: a quarter turn right of -Z
	CHECK(p.keys[0].pitchDeg == doctest::Approx(0.0f));
	CHECK(p.keys[1].yawDeg == doctest::Approx(0.0f));      // -Z
	CHECK(p.keys[1].pitchDeg == doctest::Approx(45.0f));
	CHECK(p.keys[3].yawDeg == doctest::Approx(190.0f));    // -170 unwrapped against 170: 20 degrees on, not 340 back
}

TEST_CASE("flythrough: a broken path file is refused with a reason")
{
	FlyPath p;
	std::string why;
	CHECK_FALSE(HE::Ed::parseFlyPath("{}", p, &why));
	CHECK(!why.empty());
	CHECK_FALSE(HE::Ed::parseFlyPath(R"({ "keys": [ { "t": 1, "pos": [0,0,0] }, { "t": 1, "pos": [1,0,0] } ] })", p, &why));
	CHECK_FALSE(HE::Ed::parseFlyPath(R"({ "fps": 0, "keys": [ { "t": 0, "pos": [0,0,0] } ] })", p, &why));
	CHECK_FALSE(HE::Ed::parseFlyPath("not json", p, &why));
}
