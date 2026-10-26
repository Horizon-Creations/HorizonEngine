#include "PlannerTasks.h"

#include <algorithm>

namespace HE::Planner
{

namespace
{

const char* const kStatusWords[]   = { "Open", "In progress", "Blocked", "Done" };
const TaskStatus  kStatusValues[]  = { TaskStatus::Open, TaskStatus::InProgress, TaskStatus::Blocked, TaskStatus::Done };
const char* const kPriorityWords[] = { "Low", "Normal", "High", "Urgent" };
const TaskPriority kPriorityValues[] = { TaskPriority::Low, TaskPriority::Normal, TaskPriority::High, TaskPriority::Urgent };

struct Ids
{
	UUID status   = derivedId("property", "Status");
	UUID priority = derivedId("property", "Priority");
	UUID people   = derivedId("property", "Assignees");
	UUID text     = derivedId("property", "Description");
	UUID links    = derivedId("property", "Links");

	bool isSchema(const UUID& id) const
	{
		return id == status || id == priority || id == people || id == text || id == links;
	}
};

// The word an option stands for in a select cell, "" for none.
std::string optionWord(const Database& db, const UUID& row, const UUID& prop)
{
	const Property* p = db.findProperty(prop);
	const SelectOption* o = p ? p->findOption(db.select(row, prop)) : nullptr;
	return o ? o->name : std::string{};
}

struct RowValues
{
	std::string title, status = "Open", priority = "Normal", description;
	std::vector<std::string> people;
	std::vector<TaskLink> links;
};

RowValues read(const Database& db, const Row& r, const Ids& ids)
{
	RowValues v;
	v.title = r.title;
	if (std::string s = optionWord(db, r.id, ids.status); !s.empty())   v.status = s;
	if (std::string s = optionWord(db, r.id, ids.priority); !s.empty()) v.priority = s;
	v.description = db.text(r.id, ids.text);
	v.people      = db.people(r.id, ids.people);
	v.links       = db.links(r.id, ids.links);
	return v;
}

} // namespace

void refreshTasks(Database& db, const TaskBoard& board)
{
	const Ids ids;
	ensureTasksSchema(db);
	// Columns a P1-era file or a hand edit added: the board has nowhere to keep them.
	std::vector<UUID> extra;
	for (const Property& p : db.properties)
		if (!ids.isSchema(p.id)) extra.push_back(p.id);
	for (const UUID& id : extra) db.removeProperty(id);
	db.rows.clear();
	importTaskBoard(board, db);
	db.persistRows = false;
}

bool applyTasksEdit(const Database& before, const Database& after, TaskBoard& board,
                    const std::string& author, std::int64_t nowMs, bool& changed,
                    std::string& error)
{
	changed = false;
	const Ids ids;

	if (before.properties.size() != after.properties.size())
	{
		error = "The Tasks database has a fixed set of columns: they cannot be added or deleted.";
		return false;
	}
	for (std::size_t i = 0; i < before.properties.size(); ++i)
		if (!(before.properties[i].id == after.properties[i].id) || !ids.isSchema(after.properties[i].id))
		{
			error = "The Tasks database has a fixed set of columns: they cannot be added or deleted.";
			return false;
		}

	// Rows that are gone.
	for (const Row& r : before.rows)
		if (!after.findRow(r.id)) changed |= board.remove(r.id);

	for (const Row& row : after.rows)
	{
		const Row* old = before.findRow(row.id);
		const RowValues now = read(after, row, ids);
		RowValues was;   // a new row starts from the defaults a new task has
		if (old) was = read(before, *old, ids);
		else
		{
			Task* t = board.add(now.title, author, nowMs);
			t->id = row.id;
			was.title = now.title;
			changed = true;
		}

		if (now.title != was.title)
			changed |= board.setTitle(row.id, now.title, nowMs);
		if (now.description != was.description)
			changed |= board.setDescription(row.id, now.description, nowMs);

		if (now.priority != was.priority)
		{
			TaskPriority p = TaskPriority::Other;
			bool known = false;
			for (int i = 0; i < 4; ++i)
				if (now.priority == kPriorityWords[i]) { p = kPriorityValues[i]; known = true; }
			if (known)
				changed |= board.setPriority(row.id, p, nowMs);
			else if (Task* t = board.find(row.id))
			{
				t->priority      = TaskPriority::Other;
				t->priorityOther = now.priority;
				t->modifiedMs    = nowMs;
				changed = true;
			}
		}

		if (now.status != was.status)
		{
			TaskStatus s = TaskStatus::Other;
			bool known = false;
			for (int i = 0; i < 4; ++i)
				if (now.status == kStatusWords[i]) { s = kStatusValues[i]; known = true; }
			if (known)
				changed |= board.setStatus(row.id, s, author, nowMs);
			else if (Task* t = board.find(row.id))
			{
				// A word this build does not know is kept as it is; it has no
				// place in the history (there is no status to announce).
				t->status      = TaskStatus::Other;
				t->statusOther = now.status;
				t->modifiedMs  = nowMs;
				changed = true;
			}
		}

		if (now.people != was.people)
			changed |= board.assign(row.id, now.people, author, nowMs);

		// Links: remove what went, then add what is new.
		const Task* t = board.find(row.id);
		if (!t) continue;
		for (const TaskLink& l : was.links)
		{
			const bool kept = std::any_of(now.links.begin(), now.links.end(),
			                              [&](const TaskLink& n) { return n.sameTarget(l); });
			if (kept) continue;
			for (std::size_t i = 0; i < t->links.size(); ++i)
				if (t->links[i].sameTarget(l)) { changed |= board.removeLink(row.id, i, nowMs); break; }
		}
		for (const TaskLink& l : now.links)
		{
			const bool had = std::any_of(was.links.begin(), was.links.end(),
			                             [&](const TaskLink& o) { return o.sameTarget(l); });
			if (!had) changed |= board.addLink(row.id, l, nowMs);
		}
	}
	return true;
}

} // namespace HE::Planner
