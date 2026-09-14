#include "EditorSelection.h"
#include <algorithm>

void EditorSelection::set(Entity e)
{
	m_entities.clear();
	if (e != entt::null) m_entities.push_back(e);
	m_anchor = e;
	++m_revision;
}

void EditorSelection::add(Entity e)
{
	if (e == entt::null) return;
	// Already in: move it to the back rather than duplicate it, so the entity
	// the user just (re)clicked is the primary either way.
	auto it = std::find(m_entities.begin(), m_entities.end(), e);
	if (it != m_entities.end()) m_entities.erase(it);
	m_entities.push_back(e);
	m_anchor = e;
	++m_revision;
}

void EditorSelection::remove(Entity e)
{
	auto it = std::find(m_entities.begin(), m_entities.end(), e);
	if (it == m_entities.end()) return;
	m_entities.erase(it);
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
	m_anchor = entt::null;
	++m_revision;
}

void EditorSelection::setMany(const std::vector<Entity>& range)
{
	m_entities.clear();
	addMany(range);
	++m_revision; // addMany bumped already; harmless, and set-to-empty still counts
}

void EditorSelection::addMany(const std::vector<Entity>& range)
{
	for (Entity e : range)
	{
		if (e == entt::null || contains(e)) continue;
		m_entities.push_back(e);
	}
	++m_revision;
}

void EditorSelection::prune(const entt::registry& registry)
{
	const std::size_t before = m_entities.size();
	m_entities.erase(std::remove_if(m_entities.begin(), m_entities.end(),
	                                [&](Entity e) { return !registry.valid(e); }),
	                 m_entities.end());
	if (m_anchor != entt::null && !registry.valid(m_anchor)) m_anchor = primary();
	if (m_entities.size() != before) ++m_revision;
}

bool EditorSelection::contains(Entity e) const
{
	return std::find(m_entities.begin(), m_entities.end(), e) != m_entities.end();
}
