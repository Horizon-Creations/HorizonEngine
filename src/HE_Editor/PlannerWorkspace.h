#pragma once
#include "Planner.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// ── The open project's Planner databases, loaded, edited and saved ───────────
// What TaskBoardPanel's Store is for the board: ONE copy of every database in
// the editor and one writer of its file. ImGui-free, so the tab, the MCP tools
// later on and the tests all go through the same door.
//
//   * load: every file under Config/Planner/Databases, once per project; a file
//     that does not parse is listed as unreadable (its name and why) and left
//     alone — nothing is written to it until a human has fixed it;
//   * edit(): apply a change and save that database. When the save is refused
//     because the file changed under it (a pull), the database is reloaded from
//     disk and the change applied again — once — so it lands on what arrived;
//   * poll(): notice files that changed, appeared or went away behind our back
//     (a pull, a checkout) and take them in.
namespace HE::Planner
{

class Workspace
{
public:
	struct Unreadable
	{
		UUID        id;
		std::string error;
	};

	// The project the databases belong to. Changing it drops everything and
	// loads the new one; an empty root is "no project". Cheap when unchanged.
	void setProject(const std::filesystem::path& root);
	const std::filesystem::path& project() const { return m_root; }

	// Databases in a stable order (by name, then id). The pointers (and find()'s)
	// stay valid until the next create(), ensure(), remove() or poll() that
	// changes something — resolve by id each frame, do not keep them.
	std::vector<Database*> databases();
	Database*       find(const UUID& id);
	const std::vector<Unreadable>& unreadable() const { return m_unreadable; }

	// A new, empty database (one table view) saved at once. nullptr when the
	// file could not be written; lastError() says why.
	Database* create(std::string name);
	// The database with this id, created (named `name`, with `init` applied to
	// it) when there is none yet. Used for the Tasks database, whose id is fixed.
	Database* ensure(const UUID& id, const std::string& name,
	                 const std::function<void(Database&)>& init = {});
	// Deletes the database and its file.
	bool remove(const UUID& id);

	// Apply `fn` to the database and save it (see the header). `fn` must address
	// rows, properties and views by id, never by pointer or index, because it can
	// run twice. Returns false when the database is unknown or the save failed
	// (lastError() says why); the change is kept in memory either way.
	bool edit(const UUID& id, const std::function<bool(Database&)>& fn);

	// Re-reads files that changed on disk since they were read or written, picks
	// up new ones and drops databases whose file is gone. `force` ignores the rate
	// limit (a test, or the user pressing Reload). True when anything changed.
	bool poll(bool force = false);
	// Bumped on every change to what databases() returns, so a UI can keep its
	// own caches (selected row, scroll) tied to it.
	unsigned generation() const { return m_generation; }

	const std::string& lastError() const { return m_error; }
	void clearError() { m_error.clear(); }

	// The name edits are recorded under (a new row's createdBy).
	void setAuthor(std::string name) { m_author = std::move(name); }
	const std::string& author() const { return m_author; }

private:
	struct Entry
	{
		Database                         db;
		std::filesystem::file_time_type  mtime{};
		std::uintmax_t                   size = 0;
	};

	void loadAll();
	void stamp(Entry& e) const;
	bool changedOnDisk(const Entry& e) const;
	Entry* entryFor(const UUID& id);

	std::filesystem::path   m_root;
	std::vector<Entry>      m_entries;
	std::vector<Unreadable> m_unreadable;
	std::string             m_error;
	std::string             m_author;
	unsigned                m_generation = 0;
	std::chrono::steady_clock::time_point m_nextPoll{};
};

} // namespace HE::Planner
