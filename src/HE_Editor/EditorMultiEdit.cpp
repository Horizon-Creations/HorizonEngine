#include "EditorMultiEdit.h"
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <algorithm>
#include <cctype>

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

namespace
{
	std::vector<std::string> tokensOf(json::json_pointer p)
	{
		std::vector<std::string> out;
		while (!p.empty()) { out.push_back(p.back()); p.pop_back(); }
		std::reverse(out.begin(), out.end());
		return out;
	}

	// "3" → 3; anything that is not a small array index → -1.
	int indexOf(const std::string& token)
	{
		if (token.empty() || token.size() > 4) return -1;
		for (const char c : token)
			if (c < '0' || c > '9') return -1;
		return std::stoi(token);
	}

	// "castsShadow" → "Casts Shadow", "spotAngle" → "Spot Angle".
	std::string spelled(const std::string& key)
	{
		std::string out;
		for (std::size_t i = 0; i < key.size(); ++i)
		{
			const char c = key[i];
			if (c == '_') { out += ' '; continue; }
			if (i == 0) { out += static_cast<char>(std::toupper(static_cast<unsigned char>(c))); continue; }
			const bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
			const bool prevLower = std::islower(static_cast<unsigned char>(key[i - 1])) != 0;
			if (upper && prevLower) out += ' ';
			out += c;
		}
		return out;
	}

	std::string elementName(const std::string& field, int i)
	{
		std::string lower;
		for (const char c : field) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		const bool colour = lower.find("color") != std::string::npos ||
		                    lower.find("colour") != std::string::npos ||
		                    lower.find("tint") != std::string::npos;
		static const char* kXyzw[] = { "X", "Y", "Z", "W" };
		static const char* kRgba[] = { "R", "G", "B", "A" };
		if (i >= 0 && i < 4) return colour ? kRgba[i] : kXyzw[i];
		return "#" + std::to_string(i + 1);
	}
}

std::vector<Mixed> mixed(const std::vector<json>& states)
{
	std::vector<Mixed> out;
	if (states.size() < 2 || !states[0].is_object()) return out;
	const json& ref = states[0];
	for (const auto& [component, value] : ref.items())
	{
		std::vector<Change> leaves;
		for (std::size_t i = 1; i < states.size(); ++i)
		{
			if (!states[i].is_object()) continue;
			const auto it = states[i].find(component);
			if (it == states[i].end()) continue;
			diffInto(component, json::json_pointer(), value, *it, leaves);
		}
		for (Change& c : leaves)
		{
			const bool seen = std::any_of(out.begin(), out.end(), [&](const Mixed& m)
				{ return m.component == c.component && m.path == c.path; });
			if (!seen) out.push_back({ std::move(c.component), std::move(c.path) });
		}
	}
	return out;
}

std::unordered_map<std::string, unsigned> rowMarks(const std::vector<Mixed>& mixed,
                                                   const std::string& component)
{
	std::unordered_map<std::string, unsigned> marks;
	for (const Mixed& m : mixed)
	{
		if (m.component != component) continue;
		const std::vector<std::string> t = tokensOf(m.path);
		if (t.size() == 1)
			marks[t[0]] = ~0u;
		else if (t.size() == 2 && indexOf(t[1]) >= 0 && indexOf(t[1]) < 32)
			marks[t[0]] |= 1u << indexOf(t[1]);
		// Deeper: summary line only (see the header).
	}
	return marks;
}

std::string describe(const std::vector<Mixed>& mixed, const std::string& component)
{
	// Grouped by field, in first-seen order, so a vec3 that differs in X and
	// Z reads "Position (X, Z)" rather than two entries.
	std::vector<std::pair<std::string, std::vector<std::string>>> groups;
	for (const Mixed& m : mixed)
	{
		if (m.component != component) continue;
		const std::vector<std::string> t = tokensOf(m.path);
		std::string field;
		std::string element;
		if (t.empty())
			field = spelled(component);
		else if (t.size() == 2 && indexOf(t[1]) >= 0)
		{
			field   = spelled(t[0]);
			element = elementName(t[0], indexOf(t[1]));
		}
		else
		{
			// Nested: the path spelled out, array indices counted from 1.
			for (std::size_t i = 0; i < t.size(); ++i)
			{
				if (i) field += " / ";
				const int idx = indexOf(t[i]);
				field += idx >= 0 ? "#" + std::to_string(idx + 1) : spelled(t[i]);
			}
		}
		auto g = std::find_if(groups.begin(), groups.end(),
		                      [&](const auto& p) { return p.first == field; });
		if (g == groups.end()) { groups.push_back({ field, {} }); g = groups.end() - 1; }
		if (!element.empty()) g->second.push_back(element);
	}
	std::string out;
	for (const auto& [field, elements] : groups)
	{
		if (!out.empty()) out += ", ";
		out += field;
		if (elements.empty()) continue;
		out += " (";
		for (std::size_t i = 0; i < elements.size(); ++i)
		{
			if (i) out += ", ";
			out += elements[i];
		}
		out += ")";
	}
	return out;
}

} // namespace EditorMultiEdit
