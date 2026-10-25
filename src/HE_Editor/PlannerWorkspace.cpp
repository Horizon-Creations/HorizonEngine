#include "PlannerWorkspace.h"

#include <Diagnostics/Log.h>

#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace HE::Planner
{

namespace
{

// Byte-wise, like std::string::operator< on the lowered titles, but without building them.
int compareTitlesNoCase(const std::string& a, const std::string& b)
{
	const size_t n = std::min(a.size(), b.size());
	for (size_t i = 0; i < n; ++i)
	{
		const int x = std::tolower(static_cast<unsigned char>(a[i]));
		const int y = std::tolower(static_cast<unsigned char>(b[i]));
		if (x != y) return x < y ? -1 : 1;
	}
	return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

} // namespace

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

void Workspace::stampPage(PageEntry& e) const
{
	std::error_code ec;
	const fs::path file = pagePath(m_root, e.page.id);
	e.onDisk = fs::exists(file, ec);
	e.mtime  = e.onDisk ? fs::last_write_time(file, ec) : fs::file_time_type{};
	e.size   = e.onDisk ? fs::file_size(file, ec) : 0;
}

void Workspace::stampBoard(BoardEntry& e) const
{
	std::error_code ec;
	const fs::path file = boardPath(m_root, e.board.id);
	const bool has = fs::exists(file, ec);
	e.mtime = has ? fs::last_write_time(file, ec) : fs::file_time_type{};
	e.size  = has ? fs::file_size(file, ec) : 0;
}

Workspace::BoardEntry* Workspace::boardEntryFor(const UUID& id)
{
	for (BoardEntry& e : m_boards)
		if (e.board.id == id) return &e;
	return nullptr;
}

Workspace::PageEntry* Workspace::pageEntryFor(const UUID& id)
{
	for (PageEntry& e : m_pages)
		if (e.page.id == id) return &e;
	return nullptr;
}

void Workspace::loadAll()
{
	m_entries.clear();
	m_unreadable.clear();
	m_pages.clear();
	m_unreadablePages.clear();
	m_boards.clear();
	m_unreadableBoards.clear();
	if (m_root.empty()) return;
	for (const UUID& id : listBoards(m_root))
	{
		BoardEntry e;
		const Database::LoadResult r = loadBoard(m_root, id, e.board);
		if (!r.ok)
		{
			m_unreadableBoards.push_back({ id, r.error });
			continue;
		}
		stampBoard(e);
		m_boards.push_back(std::move(e));
	}
	for (const UUID& id : listPages(m_root))
	{
		PageEntry e;
		const Database::LoadResult r = loadPage(m_root, id, e.page);
		if (!r.ok)
		{
			m_unreadablePages.push_back({ id, r.error });
			continue;
		}
		stampPage(e);
		m_pages.push_back(std::move(e));
	}
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
	// The pages of its rows go with it.
	std::vector<UUID> rowPages;
	for (const PageEntry& e : m_pages)
		if (e.page.parent == id) rowPages.push_back(e.page.id);
	for (const UUID& pid : rowPages) removePage(pid);
	++m_generation;
	return true;
}

// ── Pages ────────────────────────────────────────────────────────────────────

std::vector<Page*> Workspace::pages()
{
	std::vector<Page*> out;
	for (PageEntry& e : m_pages) out.push_back(&e.page);
	std::sort(out.begin(), out.end(), [](const Page* a, const Page* b) {
		if (const int c = compareTitlesNoCase(a->title, b->title)) return c < 0;
		return a->id.hi != b->id.hi ? a->id.hi < b->id.hi : a->id.lo < b->id.lo;
	});
	return out;
}

Page* Workspace::findPage(const UUID& id)
{
	PageEntry* e = pageEntryFor(id);
	return e ? &e->page : nullptr;
}

Page* Workspace::createPage(std::string title, const UUID& parent)
{
	if (m_root.empty()) return nullptr;
	PageEntry e;
	e.page.id        = UUID::generate();
	e.page.title     = std::move(title);
	e.page.parent    = parent;
	e.page.createdBy = m_author;
	e.page.createdMs = e.page.modifiedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	std::string err;
	if (!savePage(m_root, e.page, &err))
	{
		m_error = err;
		return nullptr;
	}
	stampPage(e);
	m_pages.push_back(std::move(e));
	++m_generation;
	return &m_pages.back().page;
}

Page* Workspace::openRowPage(const UUID& rowId, const UUID& databaseId)
{
	if (PageEntry* e = pageEntryFor(rowId)) return &e->page;
	if (m_root.empty()) return nullptr;
	// A file that exists but cannot be read is not "no page": a fresh one over it
	// would be the overwrite the loader refuses.
	for (const Unreadable& u : m_unreadablePages)
		if (u.id == rowId)
		{
			m_error = "the page file cannot be read (" + u.error + ")";
			return nullptr;
		}
	PageEntry e;
	e.page.id        = rowId;
	e.page.parent    = databaseId;
	e.page.createdBy = m_author;
	e.page.createdMs = e.page.modifiedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	stampPage(e);   // not on disk: savePage writes nothing for it until it has a block
	m_pages.push_back(std::move(e));
	++m_generation;
	return &m_pages.back().page;
}

bool Workspace::removePage(const UUID& id)
{
	// The pages under it first (a sub-page's parent is the page).
	std::vector<UUID> below;
	for (const PageEntry& e : m_pages)
		if (e.page.parent == id) below.push_back(e.page.id);
	for (const UUID& child : below) removePage(child);

	const auto it = std::find_if(m_pages.begin(), m_pages.end(),
	                             [&](const PageEntry& e) { return e.page.id == id; });
	if (it == m_pages.end()) return false;
	std::string err;
	if (!deletePage(m_root, id, &err))
	{
		m_error = err;
		return false;
	}
	m_pages.erase(it);
	++m_generation;
	return true;
}

bool Workspace::editPage(const UUID& id, const std::function<bool(Page&)>& fn)
{
	PageEntry* e = pageEntryFor(id);
	if (!e) return false;
	if (!fn(e->page)) return false;
	++m_generation;

	std::string err;
	if (savePage(m_root, e->page, &err))
	{
		stampPage(*e);
		m_error.clear();
		return true;
	}
	// The file moved under us (a pull): take what arrived, put the change on top, once.
	Page fresh;
	const Database::LoadResult r = loadPage(m_root, id, fresh);
	if (r.ok)
	{
		e->page = std::move(fresh);
		if (fn(e->page) && savePage(m_root, e->page, &err))
		{
			stampPage(*e);
			++m_generation;
			m_error.clear();
			return true;
		}
	}
	m_error = err.empty() ? r.error : err;
	return false;
}

// ── Whiteboards ──────────────────────────────────────────────────────────────

std::vector<Board*> Workspace::boards()
{
	std::vector<Board*> out;
	for (BoardEntry& e : m_boards) out.push_back(&e.board);
	std::sort(out.begin(), out.end(), [](const Board* a, const Board* b) {
		if (const int c = compareTitlesNoCase(a->title, b->title)) return c < 0;
		return a->id.hi != b->id.hi ? a->id.hi < b->id.hi : a->id.lo < b->id.lo;
	});
	return out;
}

Board* Workspace::findBoard(const UUID& id)
{
	BoardEntry* e = boardEntryFor(id);
	return e ? &e->board : nullptr;
}

Board* Workspace::createBoard(std::string title)
{
	if (m_root.empty()) return nullptr;
	BoardEntry e;
	e.board.id        = UUID::generate();
	e.board.title     = std::move(title);
	e.board.createdBy = m_author;
	e.board.createdMs = e.board.modifiedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	std::string err;
	if (!saveBoard(m_root, e.board, &err))
	{
		m_error = err;
		return nullptr;
	}
	stampBoard(e);
	m_boards.push_back(std::move(e));
	++m_generation;
	return &m_boards.back().board;
}

bool Workspace::removeBoard(const UUID& id)
{
	const auto it = std::find_if(m_boards.begin(), m_boards.end(), [&](const BoardEntry& e) { return e.board.id == id; });
	if (it == m_boards.end()) return false;
	std::string err;
	if (!deleteBoard(m_root, id, &err))
	{
		m_error = err;
		return false;
	}
	m_boards.erase(it);
	++m_generation;
	return true;
}

bool Workspace::editBoard(const UUID& id, const std::function<bool(Board&)>& fn)
{
	BoardEntry* e = boardEntryFor(id);
	if (!e) return false;
	if (!fn(e->board)) return false;
	++m_generation;

	std::string err;
	if (saveBoard(m_root, e->board, &err))
	{
		stampBoard(*e);
		m_error.clear();
		return true;
	}
	// The file moved under us (a pull): take what arrived, put the change on top, once.
	Board fresh;
	const Database::LoadResult r = loadBoard(m_root, id, fresh);
	if (r.ok)
	{
		e->board = std::move(fresh);
		if (fn(e->board) && saveBoard(m_root, e->board, &err))
		{
			stampBoard(*e);
			++m_generation;
			m_error.clear();
			return true;
		}
	}
	m_error = err.empty() ? r.error : err;
	return false;
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

	// Pages that changed, appeared or went away (a pull).
	for (std::size_t i = 0; i < m_pages.size();)
	{
		PageEntry& e = m_pages[i];
		std::error_code ec;
		const fs::path file = pagePath(m_root, e.page.id);
		const bool has = fs::exists(file, ec);
		if (!has && !e.onDisk) { ++i; continue; }   // an empty row page that lives in memory
		if (!has)
		{
			m_pages.erase(m_pages.begin() + static_cast<std::ptrdiff_t>(i));
			changed = true;
			continue;
		}
		if (e.onDisk && fs::last_write_time(file, ec) == e.mtime && fs::file_size(file, ec) == e.size) { ++i; continue; }
		Page fresh;
		const Database::LoadResult r = loadPage(m_root, e.page.id, fresh);
		if (r.ok) e.page = std::move(fresh);
		else m_error = "Config/Planner/Pages/" + uuidToHex(e.page.id) + ".json cannot be read (" + r.error +
		               "). Fix the file; nothing is saved to it until then.";
		stampPage(e);
		changed = true;
		++i;
	}
	for (const UUID& id : listPages(m_root))
	{
		if (pageEntryFor(id)) continue;
		PageEntry e;
		const Database::LoadResult r = loadPage(m_root, id, e.page);
		auto it = std::find_if(m_unreadablePages.begin(), m_unreadablePages.end(),
		                       [&](const Unreadable& u) { return u.id == id; });
		if (!r.ok)
		{
			if (it == m_unreadablePages.end())
			{
				m_unreadablePages.push_back({ id, r.error });
				changed = true;
			}
			continue;
		}
		if (it != m_unreadablePages.end()) m_unreadablePages.erase(it);
		stampPage(e);
		m_pages.push_back(std::move(e));
		changed = true;
	}

	// Boards that changed, appeared or went away (a pull).
	for (std::size_t i = 0; i < m_boards.size();)
	{
		BoardEntry& e = m_boards[i];
		std::error_code ec;
		const fs::path file = boardPath(m_root, e.board.id);
		if (!fs::exists(file, ec))
		{
			m_boards.erase(m_boards.begin() + static_cast<std::ptrdiff_t>(i));
			changed = true;
			continue;
		}
		if (fs::last_write_time(file, ec) == e.mtime && fs::file_size(file, ec) == e.size) { ++i; continue; }
		Board fresh;
		const Database::LoadResult r = loadBoard(m_root, e.board.id, fresh);
		if (r.ok) e.board = std::move(fresh);
		else m_error = "Config/Planner/Boards/" + uuidToHex(e.board.id) + ".json cannot be read (" + r.error +
		               "). Fix the file; nothing is saved to it until then.";
		stampBoard(e);
		changed = true;
		++i;
	}
	for (const UUID& id : listBoards(m_root))
	{
		if (boardEntryFor(id)) continue;
		BoardEntry e;
		const Database::LoadResult r = loadBoard(m_root, id, e.board);
		auto it = std::find_if(m_unreadableBoards.begin(), m_unreadableBoards.end(),
		                       [&](const Unreadable& u) { return u.id == id; });
		if (!r.ok)
		{
			if (it == m_unreadableBoards.end())
			{
				m_unreadableBoards.push_back({ id, r.error });
				changed = true;
			}
			continue;
		}
		if (it != m_unreadableBoards.end()) m_unreadableBoards.erase(it);
		stampBoard(e);
		m_boards.push_back(std::move(e));
		changed = true;
	}

	if (changed) ++m_generation;
	return changed;
}

} // namespace HE::Planner
