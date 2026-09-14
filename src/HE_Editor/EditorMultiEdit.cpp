#include "EditorMultiEdit.h"
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <algorithm>

namespace EditorMultiEdit
{

json state(HorizonWorld& world, Entity e)
{
	if (e == entt::null || !world.registry().valid(e)) return json::object();
	SceneSerializer ser;
	const std::vector<std::uint8_t> cbor = ser.serializeEntityComponents(world, e);
	json j = json::from_cbor(cbor, /*strict=*/true, /*allow_exceptions=*/false);
	if (!j.is_object()) return json::object();
	j.erase("__name");
	return j;
}

namespace
{
	// Walks both values in step and reports the deepest differing leaves. The
	// pointer is built on the way down so a leaf knows where it lives.
	void diffInto(const std::string& component, const json::json_pointer& path,
	              const json& before, const json& after, std::vector<Change>& out)
	{
		if (before == after) return;

		if (before.is_object() && after.is_object())
		{
			for (const auto& [key, value] : after.items())
			{
				const auto it = before.find(key);
				// A field the old state did not have (a component that gained
				// one mid-session, in theory) is a leaf of its own.
				if (it == before.end()) out.push_back({ component, path / key, value });
				else                    diffInto(component, path / key, *it, value, out);
			}
			return;
		}
		if (before.is_array() && after.is_array() && before.size() == after.size())
		{
			for (std::size_t i = 0; i < after.size(); ++i)
				diffInto(component, path / i, before[i], after[i], out);
			return;
		}
		out.push_back({ component, path, after });
	}
}

std::vector<Change> diff(const json& before, const json& after)
{
	std::vector<Change> out;
	if (!before.is_object() || !after.is_object()) return out;
	for (const auto& [component, value] : after.items())
	{
		const auto it = before.find(component);
		// A component that was not there before has no counterpart on the
		// members either; whole-component copies are not what an edit means.
		if (it == before.end()) continue;
		diffInto(component, json::json_pointer(), *it, value, out);
	}
	return out;
}

int propagate(HorizonWorld& world, const std::vector<Change>& changes,
              const std::vector<Entity>& members, Entity primary,
              const std::vector<Entity>& skip)
{
	if (changes.empty()) return 0;
	auto& registry = world.registry();
	int written = 0;

	for (Entity member : members)
	{
		if (member == primary || member == entt::null || !registry.valid(member)) continue;
		if (std::find(skip.begin(), skip.end(), member) != skip.end()) continue;

		json mine = state(world, member);
		// Only the components a change actually landed in go back through the
		// serializer: applyEntityComponents re-emplaces every component it is
		// handed, and a light edit must not re-load a member's mesh or script.
		json patch = json::object();
		for (const Change& c : changes)
		{
			const auto comp = mine.find(c.component);
			if (comp == mine.end()) continue;
			if (!comp->contains(c.path)) continue;
			(*comp)[c.path] = c.value;
			patch[c.component] = *comp;
		}
		if (patch.empty()) continue;

		SceneSerializer ser;
		if (!ser.applyEntityComponents(world, member, json::to_cbor(patch))) continue;
		// The renderer rebuilds a matrix only when told to; every other
		// component is read fresh each frame. Same rule as a peer's edit
		// arriving over collab (EditorApplication's onRemoteComponents).
		if (patch.contains("transform"))
			if (auto* tc = registry.try_get<TransformComponent>(member))
				tc->dirty = true;
		++written;
	}
	return written;
}

} // namespace EditorMultiEdit
