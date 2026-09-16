#include "doctest.h"
#include "InspectorPanel.h"
#include <HorizonScene/SceneSerializer.h>
#include <cstring>
#include <set>
#include <string>

// The Details panel marks a component header "(changed here)" when a placed
// prefab's override list names that component — by the scene-format key the
// serializer writes, looked up from the header's label through a hand-kept
// table. A key in that table the serializer does not know would leave a marker
// that can never light up, silently; this is what says every key is real.

TEST_CASE("inspector prefab keys: every label maps to a key the scene format knows")
{
	const size_t n = InspectorPanel::componentKeyCount();
	CHECK(n >= 30);
	std::set<std::string> labels, keys;
	for (size_t i = 0; i < n; ++i)
	{
		const char* label = InspectorPanel::componentLabelAt(i);
		REQUIRE(label != nullptr);
		const char* key = InspectorPanel::componentKeyForLabel(label);
		REQUIRE_MESSAGE(key != nullptr, label);
		CHECK_MESSAGE(SceneSerializer::isKnownComponentKey(key),
		              "label '", label, "' maps to '", key, "', which the loader does not restore");
		// One label, one key, and no two labels sharing a key — a shared key
		// would mark the wrong header.
		CHECK_MESSAGE(labels.insert(label).second, "label listed twice: ", label);
		CHECK_MESSAGE(keys.insert(key).second, "key listed twice: ", key);
		// And back: the Add Component menu names what the clipboard holds by
		// this reverse lookup, so it has to land on the same row.
		const char* back = InspectorPanel::componentLabelForKey(key);
		REQUIRE_MESSAGE(back != nullptr, key);
		CHECK(std::strcmp(back, label) == 0);
	}
	CHECK(InspectorPanel::componentLabelAt(n) == nullptr);
}

TEST_CASE("inspector prefab keys: every key can be copied, and the reverse lookup is exact")
{
	// The header's Copy/Paste/Reset menu is only as complete as this table:
	// a section whose label is missing here gets the pair greyed out. So every
	// key the clipboard would carry must be one the serializer exports — the
	// two it refuses (the name, the prefab link) are not in the table.
	for (size_t i = 0; i < InspectorPanel::componentKeyCount(); ++i)
	{
		const std::string key = InspectorPanel::componentKeyForLabel(InspectorPanel::componentLabelAt(i));
		CHECK(key != "prefab");
		CHECK(key != "__name");
	}
	CHECK(std::strcmp(InspectorPanel::componentLabelForKey("light"), "Light") == 0);
	CHECK(InspectorPanel::componentLabelForKey("Light") == nullptr);
	CHECK(InspectorPanel::componentLabelForKey("inactive") == nullptr);   // a tag, no section
	CHECK(InspectorPanel::componentLabelForKey(nullptr) == nullptr);
}

TEST_CASE("inspector prefab keys: the lookup is exact and answers null for a stranger")
{
	CHECK(std::strcmp(InspectorPanel::componentKeyForLabel("Rigid Body"), "rigidbody") == 0);
	CHECK(std::strcmp(InspectorPanel::componentKeyForLabel("Camera Rig"), "cameraRig") == 0);
	CHECK(InspectorPanel::componentKeyForLabel("Rigidbody") == nullptr);
	CHECK(InspectorPanel::componentKeyForLabel("Prefab Instance") == nullptr);
	CHECK(InspectorPanel::componentKeyForLabel(nullptr) == nullptr);
}
