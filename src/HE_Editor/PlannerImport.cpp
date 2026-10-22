#include "PlannerImport.h"

#include <algorithm>

namespace HE::Planner
{

namespace
{

// "plan" + "ner" in ASCII: a high half no random UUID is going to have.
constexpr std::uint64_t kDerivedHi = 0x706c616e6e657200ULL;

std::uint64_t fnv1a(std::string_view s)
{
	std::uint64_t h = 1469598103934665603ULL;
	for (const char c : s)
	{
		h ^= static_cast<unsigned char>(c);
		h *= 1099511628211ULL;
	}
	return h;
}

const char* const kStatusWords[]   = { "Open", "In progress", "Blocked", "Done" };
const char* const kPriorityWords[] = { "Low", "Normal", "High", "Urgent" };

// The display word of a task's status/priority: the four known ones by name,
// anything else as the word the file had.
std::string statusWord(const Task& t)
{
	return t.status == TaskStatus::Other ? t.statusOther : kStatusWords[static_cast<int>(t.status)];
}

std::string priorityWord(const Task& t)
{
	return t.priority == TaskPriority::Other ? t.priorityOther : kPriorityWords[static_cast<int>(t.priority)];
}

Property& ensureProperty(Database& db, const char* name, PropertyType type)
{
	const UUID id = derivedId("property", name);
	if (Property* p = db.findProperty(id)) return *p;
	Property p;
	p.id   = id;
	p.name = name;
	p.type = type;
	db.properties.push_back(std::move(p));
	return db.properties.back();
}

// The option of that name, with the id derived from (property, name).
UUID ensureOption(Database& db, const UUID& property, const std::string& word, const char* color = "")
{
	Property* p = db.findProperty(property);
	if (!p || word.empty()) return {};
	if (const SelectOption* o = p->findOptionByName(word)) return o->id;
	SelectOption o;
	o.id    = derivedId("option:" + p->name, word);
	o.name  = word;
	o.color = color;
	p->options.push_back(std::move(o));
	return p->options.back().id;
}

View& ensureView(Database& db, const char* name, ViewKind kind)
{
	const UUID id = derivedId("view", name);
	if (View* v = db.findView(id)) return *v;
	View v;
	v.id   = id;
	v.name = name;
	v.kind = kind;
	db.views.push_back(std::move(v));
	return db.views.back();
}

} // namespace

UUID derivedId(std::string_view kind, std::string_view name)
{
	UUID id;
	id.hi = kDerivedHi;
	id.lo = fnv1a(std::string(kind) + "\n" + std::string(name));
	return id;
}

UUID tasksDatabaseId() { return derivedId("database", "Tasks"); }

void ensureTasksSchema(Database& db)
{
	if (db.id == UUID{})  db.id = tasksDatabaseId();
	if (db.name.empty())  db.name = "Tasks";

	const UUID status = ensureProperty(db, "Status", PropertyType::Select).id;
	const char* const statusColors[] = { "gray", "blue", "red", "green" };
	for (int i = 0; i < 4; ++i) ensureOption(db, status, kStatusWords[i], statusColors[i]);
	const UUID priority = ensureProperty(db, "Priority", PropertyType::Select).id;
	const char* const priorityColors[] = { "gray", "blue", "orange", "red" };
	for (int i = 0; i < 4; ++i) ensureOption(db, priority, kPriorityWords[i], priorityColors[i]);
	ensureProperty(db, "Assignees", PropertyType::Person);
	ensureProperty(db, "Description", PropertyType::Text);
	ensureProperty(db, "Links", PropertyType::EngineLink);

	const bool fresh = db.views.empty();
	View& all = ensureView(db, "All tasks", ViewKind::Table);
	if (fresh) all.sorts = { { status, false } };
	View& lanes = ensureView(db, "By status", ViewKind::Board);
	if (lanes.groupBy == UUID{}) lanes.groupBy = status;
}

ImportResult importTaskBoard(const TaskBoard& board, Database& db)
{
	ensureTasksSchema(db);
	const UUID status   = derivedId("property", "Status");
	const UUID priority = derivedId("property", "Priority");
	const UUID people   = derivedId("property", "Assignees");
	const UUID text     = derivedId("property", "Description");
	const UUID links    = derivedId("property", "Links");

	ImportResult result;
	for (const Task& t : board.tasks)
	{
		Row* row = db.findRow(t.id);
		const bool isNew = row == nullptr;
		if (isNew)
		{
			Row r;
			r.id        = t.id;
			r.createdBy = t.createdBy;
			r.createdMs = t.createdMs;
			db.rows.push_back(std::move(r));
			row = &db.rows.back();
		}
		else if (t.modifiedMs <= row->modifiedMs)
		{
			++result.unchanged;
			continue;
		}

		// The board's time stamps go onto the row as they are, so the next import
		// compares like with like and a later Planner edit is newer than both.
		row->title = t.title;
		db.setSelect(t.id, status, ensureOption(db, status, statusWord(t)), t.modifiedMs);
		db.setSelect(t.id, priority, ensureOption(db, priority, priorityWord(t)), t.modifiedMs);
		db.setPeople(t.id, people, t.assignees, t.modifiedMs);
		db.setText(t.id, text, t.description, t.modifiedMs);
		db.setLinks(t.id, links, t.links, t.modifiedMs);
		row = db.findRow(t.id);
		row->modifiedMs = t.modifiedMs;
		++(isNew ? result.added : result.updated);
	}
	return result;
}

} // namespace HE::Planner
