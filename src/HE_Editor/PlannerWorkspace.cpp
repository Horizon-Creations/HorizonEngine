#include "PlannerWorkspace.h"

#include <Diagnostics/Log.h>

#include <algorithm>

namespace fs = std::filesystem;

namespace HE::Planner
{

void Workspace::stamp(Entry& e) const
{
	std::error_code ec;
	const fs::path file = databasePath(m_root, e.db.id);
	e.mtime = fs::exists(file, ec) ? fs::last_write_time(file, ec) : fs::file_time_type{};
	e.size  = fs::exists(file, ec) ? fs::file_size(file, ec) : 0;
}

bool Workspace::changedOnDisk(const Entry& e) const
{
	std::error_code ec;
	const fs::path file = databasePath(m_root, e.db.id);
	if (!fs::exists(file, ec)) return true;
	return fs::last_write_time(file, ec) != e.mtime || fs::file_size(file, ec) != e.size;
}

Workspace::Entry* Workspace::entryFor(const UUID& id)
{
	for (Entry& e : m_entries)
		if (e.db.id == id) return &e;
	return nullptr;
}

void Workspace::loadAll()
{
	m_entries.clear();
	m_unreadable.clear();
	if (m_root.empty()) return;
	for (const UUID& id : listDatabases(m_root))
	{
		Entry e;
		const Database::LoadResult r = loadDatabase(m_root, id, e.db);
		if (!r.ok)
		{
			m_unreadable.push_back({ id, r.error });
			continue;
		}
		afterLoad(e);
		stamp(e);
		m_entries.push_back(std::move(e));
	}
	attachExternal();
	++m_generation;
}

void Workspace::afterLoad(Entry& e)
{
	if (!isExternal(e.db.id)) return;
	e.db.persistRows = false;
	m_ext.refresh(e.db);
	e.extGen = m_ext.generation ? m_ext.generation() : 0;
}

void Workspace::attachExternal()
{
	if (!isExternal(m_ext.id) || m_root.empty() || entryFor(m_ext.id)) return;
	// No file yet (or one that cannot be read, which stays listed and is never
	// written over): the database exists in memory, with the schema and the
	// default views, and is written when a view changes.
	Entry e;
	e.db.id   = m_ext.id;
	e.db.name = m_ext.name;
	afterLoad(e);
	m_entries.push_back(std::move(e));
	++m_generation;
}

void Workspace::setExternal(External ext)
{
	m_ext = std::move(ext);
	if (!m_ext.refresh) return;
	attachExternal();
	// A file loaded before the source was installed (setProject came first): the
	// database is adopted now, its rows replaced by the source's.
	if (Entry* e = entryFor(m_ext.id); e && e->db.persistRows)
	{
		afterLoad(*e);
		++m_generation;
	}
}

void Workspace::setProject(const fs::path& root)
{
	if (root == m_root) return;
	m_root = root;
	m_error.clear();
	loadAll();
}

std::vector<Database*> Workspace::databases()
{
	std::vector<Database*> out;
	for (Entry& e : m_entries) out.push_back(&e.db);
	std::sort(out.begin(), out.end(), [](const Database* a, const Database* b) {
		if (a->name != b->name) return a->name < b->name;
		return a->id.hi != b->id.hi ? a->id.hi < b->id.hi : a->id.lo < b->id.lo;
	});
	return out;
}

Database* Workspace::find(const UUID& id)
{
	Entry* e = entryFor(id);
	return e ? &e->db : nullptr;
}

Database* Workspace::create(std::string name)
{
	if (m_root.empty()) return nullptr;
	Entry e;
	e.db = Database::create(std::move(name));
	std::string err;
	if (!saveDatabase(m_root, e.db, &err))
	{
		m_error = err;
		return nullptr;
	}
	stamp(e);
	m_entries.push_back(std::move(e));
	++m_generation;
	return &m_entries.back().db;
}

Database* Workspace::ensure(const UUID& id, const std::string& name,
                            const std::function<void(Database&)>& init)
{
	if (Entry* e = entryFor(id)) return &e->db;
	if (m_root.empty()) return nullptr;
	// A file that exists but cannot be read is not "no database": making a new
	// one over it would be the overwrite the loader refuses.
	for (const Unreadable& u : m_unreadable)
		if (u.id == id)
		{
			m_error = "the database file cannot be read (" + u.error + ")";
			return nullptr;
		}
	Entry e;
	e.db.id   = id;
	e.db.name = name;
	if (init) init(e.db);
	if (e.db.views.empty()) e.db.addView("Table", ViewKind::Table);
	std::string err;
	if (!saveDatabase(m_root, e.db, &err))
	{
		m_error = err;
		return nullptr;
	}
	stamp(e);
	m_entries.push_back(std::move(e));
	++m_generation;
	return &m_entries.back().db;
}

bool Workspace::remove(const UUID& id)
{
	if (isExternal(id))
	{
		m_error = "The Tasks database is the Task Board and cannot be deleted.";
		return false;
	}
	const auto it = std::find_if(m_entries.begin(), m_entries.end(),
	                             [&](const Entry& e) { return e.db.id == id; });
	if (it == m_entries.end()) return false;
	std::string err;
	if (!deleteDatabase(m_root, id, &err))
	{
		m_error = err;
		return false;
	}
	m_entries.erase(it);
	++m_generation;
	return true;
}

bool Workspace::edit(const UUID& id, const std::function<bool(Database&)>& fn)
{
	Entry* e = entryFor(id);
	if (!e) return false;

	if (isExternal(id))
	{
		const Database before = e->db;
		if (!fn(e->db)) return false;
		std::string err;
		if (!m_ext.apply(before, e->db, err))
		{
			e->db = before;   // the source refused: the database stays as it was
			m_error = err;
			++m_generation;
			return false;
		}
		// The source is the truth: take back what it made of the change.
		m_ext.refresh(e->db);
		e->extGen = m_ext.generation ? m_ext.generation() : 0;
		++m_generation;
		if (before.toJson() == e->db.toJson()) { m_error.clear(); return true; }   // rows only
		std::string saveErr;
		if (!saveDatabase(m_root, e->db, &saveErr))
		{
			m_error = saveErr;
			return false;
		}
		stamp(*e);
		m_error.clear();
		return true;
	}

	if (!fn(e->db)) return false;   // nothing changed, nothing to save
	++m_generation;

	std::string err;
	if (saveDatabase(m_root, e->db, &err))
	{
		stamp(*e);
		m_error.clear();
		return true;
	}

	// The file moved under us (a pull): take what arrived and put the change on
	// top of it, once. A file that does not parse is not "moved", it is damaged;
	// reloading would blank the list, so that stays an error for a human.
	Database fresh;
	const Database::LoadResult r = loadDatabase(m_root, id, fresh);
	if (r.ok)
	{
		e->db = std::move(fresh);
		if (fn(e->db) && saveDatabase(m_root, e->db, &err))
		{
			stamp(*e);
			++m_generation;
			m_error.clear();
			return true;
		}
	}
	m_error = err.empty() ? r.error : err;
	return false;
}

bool Workspace::poll(bool force)
{
	if (m_root.empty()) return false;
	const auto now = std::chrono::steady_clock::now();
	if (!force && now < m_nextPoll) return false;
	m_nextPoll = now + std::chrono::milliseconds(500);

	bool changed = false;

	// The external source changed behind our back (the footer ticked a task off,
	// an MCP tool, a pull): its rows are read again.
	if (isExternal(m_ext.id))
		if (Entry* e = entryFor(m_ext.id))
		{
			const unsigned gen = m_ext.generation ? m_ext.generation() : 0;
			if (gen != e->extGen)
			{
				m_ext.refresh(e->db);
				e->extGen = gen;
				changed = true;
			}
		}

	// Databases that changed or went away.
	for (std::size_t i = 0; i < m_entries.size();)
	{
		Entry& e = m_entries[i];
		if (!changedOnDisk(e)) { ++i; continue; }
		std::error_code ec;
		const bool hasFile = fs::exists(databasePath(m_root, e.db.id), ec);
		if (!hasFile && isExternal(e.db.id)) { ++i; continue; }   // lives in memory until a view changes
		if (!hasFile)
		{
			m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
			changed = true;
			continue;
		}
		Database fresh;
		const Database::LoadResult r = loadDatabase(m_root, e.db.id, fresh);
		if (r.ok)
		{
			e.db = std::move(fresh);
			afterLoad(e);
			stamp(e);
			m_unreadable.erase(std::remove_if(m_unreadable.begin(), m_unreadable.end(),
			                   [&](const Unreadable& u) { return u.id == e.db.id; }), m_unreadable.end());
		}
		else
		{
			// Keep the last good copy on screen, say why, and re-stamp so this is
			// reported once and not every poll.
			m_error = "Config/Planner/Databases/" + uuidToHex(e.db.id) + ".json cannot be read (" +
			          r.error + "). Fix the file; nothing is saved to it until then.";
			stamp(e);
		}
		changed = true;
		++i;
	}

	// New files, and unreadable ones that got fixed.
	for (const UUID& id : listDatabases(m_root))
	{
		if (entryFor(id)) continue;
		Entry e;
		const Database::LoadResult r = loadDatabase(m_root, id, e.db);
		auto it = std::find_if(m_unreadable.begin(), m_unreadable.end(),
		                       [&](const Unreadable& u) { return u.id == id; });
		if (!r.ok)
		{
			if (it == m_unreadable.end())
			{
				m_unreadable.push_back({ id, r.error });
				changed = true;
			}
			continue;
		}
		if (it != m_unreadable.end()) m_unreadable.erase(it);
		afterLoad(e);
		stamp(e);
		m_entries.push_back(std::move(e));
		changed = true;
	}

	if (changed) ++m_generation;
	return changed;
}

} // namespace HE::Planner
