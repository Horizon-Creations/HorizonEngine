#include "EditorSelection.h"
#include <algorithm>

void EditorSelection::set(Entity e)
{
	m_entities.clear();
	m_members.clear();
	if (e != entt::null)
	{
		m_entities.push_back(e);
		m_members.insert(e);
	}
	m_anchor = e;
	++m_revision;
}

void EditorSelection::add(Entity e)
{
	if (e == entt::null) return;
	// Already in: move it to the back rather than duplicate it, so the entity
	// the user just (re)clicked is the primary either way.
	if (!m_members.insert(e).second)
		m_entities.erase(std::find(m_entities.begin(), m_entities.end(), e));
	m_entities.push_back(e);
	m_anchor = e;
	++m_revision;
}

void EditorSelection::remove(Entity e)
{
	if (m_members.erase(e) == 0) return;
	m_entities.erase(std::find(m_entities.begin(), m_entities.end(), e));
	if (m_anchor == e) m_anchor = primary();
	++m_revision;
}

void EditorSelection::toggle(Entity e)
{
	if (e == entt::null) return;
	if (contains(e)) remove(e);
	else             add(e);
}

void EditorSelection::clear()
{
	if (m_entities.empty() && m_anchor == entt::null) return;
	m_entities.clear();
	m_members.clear();
	m_anchor = entt::null;
	++m_revision;
}

void EditorSelection::setMany(const std::vector<Entity>& range)
{
	m_entities.clear();
	m_members.clear();
	addMany(range);
	++m_revision; // addMany bumped already; harmless, and set-to-empty still counts
}

void EditorSelection::addMany(const std::vector<Entity>& range)
{
	m_members.reserve(m_members.size() + range.size());
	for (Entity e : range)
	{
		if (e == entt::null || !m_members.insert(e).second) continue;
		m_entities.push_back(e);
	}
	++m_revision;
}

void EditorSelection::prune(const entt::registry& registry)
{
	const std::size_t before = m_entities.size();
	m_entities.erase(std::remove_if(m_entities.begin(), m_entities.end(),
	                                [&](Entity e) {
		                                if (registry.valid(e)) return false;
		                                m_members.erase(e);
		                                return true;
	                                }),
	                 m_entities.end());
	if (m_anchor != entt::null && !registry.valid(m_anchor)) m_anchor = primary();
	if (m_entities.size() != before) ++m_revision;
}

std::vector<Entity> EditorSelection::roots(const entt::registry& registry) const
{
	std::vector<Entity> out;
	out.reserve(m_entities.size());
	for (Entity e : m_entities)
	{
		if (!registry.valid(e)) continue;
		// Walk up the parent chain; the first selected ancestor disqualifies
		// this member. The step cap is the guard against a cycle in a
		// hand-edited scene — reparentEntity never builds one, but a file can.
		constexpr int kMaxDepth = 1024;
		bool covered = false;
		Entity p = e;
		for (int steps = 0; steps < kMaxDepth; ++steps)
		{
			const auto* h = registry.try_get<HierarchyComponent>(p);
			if (!h || h->parent == entt::null || !registry.valid(h->parent)) break;
			p = h->parent;
			if (contains(p)) { covered = true; break; }
		}
		if (!covered) out.push_back(e);
	}
	return out;
}

bool EditorSelection::contains(Entity e) const
{
	return m_members.count(e) != 0;
}
