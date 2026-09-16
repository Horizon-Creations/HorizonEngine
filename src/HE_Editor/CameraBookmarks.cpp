#include "CameraBookmarks.h"
#include "EditorCamera.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace CameraBookmarks
{

static bool validSlot(int slot) { return slot >= 0 && slot < kSlots; }

void Set::store(int slot, const EditorCamera& cam)
{
	if (!validSlot(slot)) return;
	Bookmark& b     = slots[slot];
	b.set           = true;
	b.position      = cam.position();
	b.yaw           = cam.yaw();
	b.pitch         = cam.pitch();
	b.pivotDistance = cam.pivotDistance();
	b.orthographic  = cam.orthographic();
}

bool Set::recall(int slot, EditorCamera& cam) const
{
	if (!validSlot(slot) || !slots[slot].set) return false;
	const Bookmark& b = slots[slot];
	// restoreView is the exact-pose setter the config restore uses; it does
	// not carry the projection, which is why the ortho flag follows separately.
	cam.restoreView(b.position, b.yaw, b.pitch, b.pivotDistance);
	cam.setOrthographic(b.orthographic);
	return true;
}

void Set::clear(int slot)
{
	if (validSlot(slot)) slots[slot] = Bookmark{};
}

void Set::clearAll()
{
	for (Bookmark& b : slots) b = Bookmark{};
}

bool Set::any() const
{
	for (const Bookmark& b : slots)
		if (b.set) return true;
	return false;
}

bool Set::isSet(int slot) const
{
	return validSlot(slot) && slots[slot].set;
}

std::string Set::encode() const
{
	std::string out;
	char buf[192];
	for (int i = 0; i < kSlots; ++i)
	{
		const Bookmark& b = slots[i];
		if (!b.set) continue;
		// %.9g round-trips a float exactly through strtod.
		std::snprintf(buf, sizeof(buf), "%d:%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d;",
		              i, b.position.x, b.position.y, b.position.z,
		              b.yaw, b.pitch, b.pivotDistance, b.orthographic ? 1 : 0);
		out += buf;
	}
	return out;
}

Set Set::decode(const std::string& text)
{
	Set set;
	const char* p = text.c_str();
	while (*p)
	{
		// One record up to the next ';' (or the end); a record that does not
		// parse is skipped, the rest still count.
		const char* end = std::strchr(p, ';');
		const char* next = end ? end + 1 : p + std::strlen(p);

		char* cur = nullptr;
		const long slot = std::strtol(p, &cur, 10);
		if (cur == p || *cur != ':' || !validSlot(static_cast<int>(slot))) { p = next; continue; }
		++cur;

		float v[6] = {};
		bool ok = true;
		for (int k = 0; k < 6 && ok; ++k)
		{
			char* after = nullptr;
			v[k] = static_cast<float>(std::strtod(cur, &after));
			ok   = (after != cur) && (*after == ',');
			cur  = after + 1;
		}
		if (!ok) { p = next; continue; }
		char* after = nullptr;
		const long ortho = std::strtol(cur, &after, 10);
		if (after == cur) { p = next; continue; }

		Bookmark& b     = set.slots[slot];
		b.set           = true;
		b.position      = glm::vec3(v[0], v[1], v[2]);
		b.yaw           = v[3];
		b.pitch         = v[4];
		b.pivotDistance = v[5];
		b.orthographic  = ortho != 0;
		p = next;
	}
	return set;
}

Set& editorSet()
{
	static Set s_set;
	return s_set;
}

} // namespace CameraBookmarks
