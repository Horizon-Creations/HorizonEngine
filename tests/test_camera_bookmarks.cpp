#include "doctest.h"
#include "CameraBookmarks.h"
#include "EditorCamera.h"
#include <glm/glm.hpp>
#include <string>

// Camera bookmarks are pure data over EditorCamera (no ImGui), so the whole
// store / recall / persist cycle runs headless. CameraBookmarks.cpp and
// EditorCamera.cpp are compiled directly into the test target.

namespace
{
	EditorCamera cameraAt(const glm::vec3& pos, float yaw, float pitch, float pivot, bool ortho)
	{
		EditorCamera cam;
		cam.restoreView(pos, yaw, pitch, pivot);
		cam.setOrthographic(ortho);
		return cam;
	}
}

TEST_CASE("CameraBookmarks: store and recall bring back the exact pose, lens included")
{
	CameraBookmarks::Set set;
	CHECK_FALSE(set.any());

	const EditorCamera src = cameraAt(glm::vec3(12.5f, -3.25f, 40.0f), 1.25f, -0.4f, 17.5f, true);
	set.store(3, src);
	CHECK(set.any());
	CHECK(set.isSet(3));
	CHECK_FALSE(set.isSet(2));

	EditorCamera dst;
	CHECK(set.recall(3, dst));
	CHECK(dst.position()      == src.position());
	CHECK(dst.yaw()           == doctest::Approx(src.yaw()));
	CHECK(dst.pitch()         == doctest::Approx(src.pitch()));
	CHECK(dst.pivotDistance() == doctest::Approx(src.pivotDistance()));
	CHECK(dst.orthographic()  == src.orthographic());
	// A recalled camera counts as initialised: ensureInit must not re-aim it
	// at the origin on the next update.
	CHECK(dst.initialised());
}

TEST_CASE("CameraBookmarks: an empty or out-of-range slot leaves the camera alone")
{
	CameraBookmarks::Set set;
	EditorCamera cam = cameraAt(glm::vec3(1.0f, 2.0f, 3.0f), 0.5f, 0.1f, 9.0f, false);
	const glm::vec3 before = cam.position();

	CHECK_FALSE(set.recall(0, cam));
	CHECK_FALSE(set.recall(-1, cam));
	CHECK_FALSE(set.recall(CameraBookmarks::kSlots, cam));
	CHECK(cam.position() == before);

	// Storing out of range is ignored rather than writing past the array.
	set.store(-1, cam);
	set.store(CameraBookmarks::kSlots, cam);
	CHECK_FALSE(set.any());
}

TEST_CASE("CameraBookmarks: clear and clearAll")
{
	CameraBookmarks::Set set;
	const EditorCamera cam = cameraAt(glm::vec3(0.0f), 0.0f, 0.0f, 5.0f, false);
	set.store(0, cam);
	set.store(9, cam);
	set.clear(0);
	CHECK_FALSE(set.isSet(0));
	CHECK(set.isSet(9));
	set.clearAll();
	CHECK_FALSE(set.any());
}

TEST_CASE("CameraBookmarks: encode / decode round-trips every set slot exactly")
{
	CameraBookmarks::Set set;
	set.store(0, cameraAt(glm::vec3(-100.125f, 0.001f, 2500.5f), -3.0f, 1.5f, 0.25f, false));
	set.store(7, cameraAt(glm::vec3(1.0f / 3.0f, 2.0f / 3.0f, -7.0f), 0.7071f, -1.2f, 123.456f, true));

	const std::string text = set.encode();
	CHECK_FALSE(text.empty());
	const CameraBookmarks::Set back = CameraBookmarks::Set::decode(text);

	for (int i = 0; i < CameraBookmarks::kSlots; ++i)
	{
		CHECK(back.slots[i].set == set.slots[i].set);
		if (!set.slots[i].set) continue;
		// Exact, not Approx: %.9g is enough digits to round-trip a float.
		CHECK(back.slots[i].position      == set.slots[i].position);
		CHECK(back.slots[i].yaw           == set.slots[i].yaw);
		CHECK(back.slots[i].pitch         == set.slots[i].pitch);
		CHECK(back.slots[i].pivotDistance == set.slots[i].pivotDistance);
		CHECK(back.slots[i].orthographic  == set.slots[i].orthographic);
	}
	// And the text itself is stable across a second pass.
	CHECK(back.encode() == text);
}

TEST_CASE("CameraBookmarks: decode tolerates an empty or damaged string")
{
	CHECK_FALSE(CameraBookmarks::Set::decode("").any());
	CHECK_FALSE(CameraBookmarks::Set::decode("garbage").any());
	CHECK_FALSE(CameraBookmarks::Set::decode("12:1,2,3,4,5,6,0;").any());   // slot out of range
	CHECK_FALSE(CameraBookmarks::Set::decode("2:1,2,3;").any());            // too few numbers

	// A damaged record does not take the good ones down with it.
	const CameraBookmarks::Set part =
		CameraBookmarks::Set::decode("4:1,2,3,0.5,0.25,8,1;x:nope;5:9,8,7,0,0,2,0;");
	CHECK(part.isSet(4));
	CHECK(part.isSet(5));
	CHECK(part.slots[4].orthographic);
	CHECK_FALSE(part.slots[5].orthographic);
	CHECK(part.slots[5].position == glm::vec3(9.0f, 8.0f, 7.0f));
}
