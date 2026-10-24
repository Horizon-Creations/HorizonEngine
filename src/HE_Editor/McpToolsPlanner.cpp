#include "McpToolCommon.h"
#include "McpToolHooks.h"

#include "Planner.h"
#include "PlannerBoard.h"
#include "PlannerMarkdown.h"
#include "PlannerWorkspace.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace HE::Ed
{
namespace
{
using json = nlohmann::json;
using namespace HE::Planner;

std::string lower(std::string s)
{
	for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

bool sameName(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

ToolResult failNoProject()
{
	return ToolResult::fail("no_project", "no project is open — the Planner lives in the open project's Config/Planner");
}

ToolResult invalid(const std::string& what) { return ToolResult::fail("invalid_payload", what); }

// ── Finding things ───────────────────────────────────────────────────────────

Workspace* workspaceOf(const McpPlannerHooks& h)
{
	Workspace* ws = h.workspace ? h.workspace() : nullptr;
	if (ws) ws->poll();   // a pull or an edit made elsewhere since the last call
	return ws;
}

// A database by id (32 hex digits) or by name (any case).
Database* findDatabase(Workspace& ws, const std::string& key)
{
	UUID id;
	if (uuidFromHex(key, id)) return ws.find(id);
	Database* found = nullptr;
	for (Database* db : ws.databases())
		if (sameName(db->name, key))
		{
			if (found) return nullptr;   // two databases of that name: the caller must use the id
			found = db;
		}
	return found;
}

const Property* findProperty(const Database& db, const std::string& key)
{
	UUID id;
	if (uuidFromHex(key, id)) return db.findProperty(id);
	for (const Property& p : db.properties)
		if (sameName(p.name, key)) return &p;
	return nullptr;
}

// A row by id, or by title when exactly one has it.
const Row* findRow(const Database& db, const std::string& key)
{
	UUID id;
	if (uuidFromHex(key, id)) return db.findRow(id);
	const Row* found = nullptr;
	for (const Row& r : db.rows)
		if (sameName(r.title, key))
		{
			if (found) return nullptr;
			found = &r;
		}
	return found;
}

const View* findView(const Database& db, const std::string& key)
{
	UUID id;
	if (uuidFromHex(key, id)) return db.findView(id);
	for (const View& v : db.views)
		if (sameName(v.name, key)) return &v;
	return nullptr;
}

// ── Values as JSON ───────────────────────────────────────────────────────────

json linkJson(const TaskLink& l)
{
	json j{ { "kind", l.kind == TaskLinkKind::Other ? l.kindOther : taskLinkKindName(l.kind) }, { "label", l.label } };
	if (!(l.id == UUID{})) j["id"] = uuidToHex(l.id);
	if (!l.scene.empty())  j["scene"] = l.scene;
	if (!l.panel.empty())  j["panel"] = l.panel;
	return j;
}

std::string dayOrTime(std::int64_t ms)
{
	const std::string iso = taskTimeToIso(ms);
	return iso.substr(10) == "T00:00:00.000Z" ? iso.substr(0, 10) : iso;
}

// The value of one cell as a script reads it: names where the file has ids.
json valueJson(Workspace& ws, const Database& db, const Row& row, const Property& p)
{
	if (db.isEmpty(row.id, p.id) && p.type != PropertyType::Checkbox) return nullptr;
	switch (p.type)
	{
	case PropertyType::Text:
	case PropertyType::Url:      return db.text(row.id, p.id);
	case PropertyType::Number:   return db.number(row.id, p.id);
	case PropertyType::Checkbox: return db.checked(row.id, p.id);
	case PropertyType::Date:
	{
		std::int64_t ms = 0;
		return db.date(row.id, p.id, ms) ? json(dayOrTime(ms)) : json(nullptr);
	}
	case PropertyType::Select:
	{
		const SelectOption* o = p.findOption(db.select(row.id, p.id));
		return o ? json(o->name) : json(nullptr);
	}
	case PropertyType::MultiSelect:
	{
		json names = json::array();
		for (const UUID& id : db.multiSelect(row.id, p.id))
			if (const SelectOption* o = p.findOption(id)) names.push_back(o->name);
		return names;
	}
	case PropertyType::Person:   return db.people(row.id, p.id);
	case PropertyType::Relation:
	{
		const Database* target = (p.relationDb == UUID{} || p.relationDb == db.id) ? &db : ws.find(p.relationDb);
		json rows = json::array();
		for (const UUID& id : db.relation(row.id, p.id))
		{
			const Row* r = target ? target->findRow(id) : nullptr;
			rows.push_back(json{ { "id", uuidToHex(id) }, { "title", r ? r->title : std::string{} } });
		}
		return rows;
	}
	case PropertyType::EngineLink:
	{
		json links = json::array();
		for (const TaskLink& l : db.links(row.id, p.id)) links.push_back(linkJson(l));
		return links;
	}
	default: return nullptr;
	}
}

json rowJson(Workspace& ws, const Database& db, const Row& row)
{
	json values = json::object();
	for (const Property& p : db.properties)
	{
		const json v = valueJson(ws, db, row, p);
		if (!v.is_null()) values[p.name] = v;
	}
	json j{
		{ "id",       uuidToHex(row.id) },
		{ "title",    row.title },
		{ "values",   std::move(values) },
		{ "createdBy", row.createdBy },
		{ "created",  taskTimeToIso(row.createdMs) },
		{ "modified", taskTimeToIso(row.modifiedMs) },
	};
	if (const Page* page = ws.findPage(row.id)) j["pageBlocks"] = page->blocks.size();
	return j;
}

json propertyJson(const Property& p)
{
	json j{ { "id", uuidToHex(p.id) }, { "name", p.name },
	        { "type", p.type == PropertyType::Other ? p.typeOther : propertyTypeName(p.type) } };
	if (!p.options.empty())
	{
		json options = json::array();
		for (const SelectOption& o : p.options) options.push_back(json{ { "id", uuidToHex(o.id) }, { "name", o.name } });
		j["options"] = std::move(options);
	}
	if (p.type == PropertyType::Relation) j["relationDatabase"] = uuidToHex(p.relationDb);
	return j;
}

json databaseJson(const Database& db, bool withRowCount = true)
{
	json props = json::array();
	for (const Property& p : db.properties) props.push_back(propertyJson(p));
	json views = json::array();
	for (const View& v : db.views)
		views.push_back(json{ { "id", uuidToHex(v.id) }, { "name", v.name },
		                      { "kind", v.kind == ViewKind::Other ? v.kindOther : viewKindName(v.kind) } });
	json j{ { "id", uuidToHex(db.id) }, { "name", db.name }, { "properties", std::move(props) }, { "views", std::move(views) } };
	if (withRowCount) j["rowCount"] = db.rows.size();
	return j;
}

// ── JSON into cells ──────────────────────────────────────────────────────────

bool parseDate(const std::string& text, std::int64_t& ms)
{
	if (text.size() == 10) return taskTimeFromIso(text + "T00:00:00.000Z", ms);
	return taskTimeFromIso(text, ms);
}

std::vector<std::string> stringList(const json& v)
{
	std::vector<std::string> out;
	if (v.is_string())
	{
		std::string cur;
		for (const char c : v.get<std::string>())
		{
			if (c == ',') { out.push_back(cur); cur.clear(); }
			else          cur += c;
		}
		out.push_back(cur);
		for (std::string& s : out)
		{
			const auto b = s.find_first_not_of(' '), e = s.find_last_not_of(' ');
			s = b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
		}
		out.erase(std::remove(out.begin(), out.end(), std::string{}), out.end());
	}
	else if (v.is_array())
		for (const json& e : v)
			if (e.is_string()) out.push_back(e.get<std::string>());
	return out;
}

// One link as a script gives it: {kind: asset|scene|entity|panel, id, scene, panel, label}.
bool linkFrom(const json& v, TaskLink& out, std::string& err)
{
	if (!v.is_object()) { err = "a link must be an object {kind, id | scene | panel, label}"; return false; }
	const std::string kind = v.value("kind", std::string{}), label = v.value("label", std::string{});
	UUID id;
	const bool hasId = v.contains("id") && v["id"].is_string() && uuidFromHex(v["id"].get<std::string>(), id);
	if (kind == "asset" && hasId)                                   { out = TaskLink::asset(id, label); return true; }
	if (kind == "entity" && hasId)                                  { out = TaskLink::entity(id, v.value("scene", std::string{}), label); return true; }
	if (kind == "scene" && !v.value("scene", std::string{}).empty()) { out = TaskLink::sceneLink(v["scene"].get<std::string>(), label); return true; }
	if (kind == "panel" && !v.value("panel", std::string{}).empty()) { out = TaskLink::panelLink(v["panel"].get<std::string>(), label); return true; }
	err = "a link needs kind asset/entity (with 'id', a 32-digit hex), scene (with 'scene', a project path) or panel (with 'panel')";
	return false;
}

// Sets one cell from JSON. `null` clears it. On a value that does not fit, false and `err`.
bool applyValue(Workspace& ws, Database& db, const UUID& rowId, const Property& p, const json& v,
                bool createOptions, std::string& err)
{
	const std::int64_t now = taskNowMs();
	const UUID pid = p.id;
	if (v.is_null()) return db.clearCell(rowId, pid, now);
	switch (p.type)
	{
	case PropertyType::Text:
	case PropertyType::Url:
		if (!v.is_string()) { err = "expects a string"; return false; }
		return db.setText(rowId, pid, v.get<std::string>(), now);
	case PropertyType::Number:
	{
		double n = 0;
		if (v.is_number()) n = v.get<double>();
		else if (v.is_string())
		{
			char* end = nullptr;
			const std::string s = v.get<std::string>();
			n = std::strtod(s.c_str(), &end);
			if (end == s.c_str() || *end != '\0') { err = "expects a number"; return false; }
		}
		else { err = "expects a number"; return false; }
		return db.setNumber(rowId, pid, n, now);
	}
	case PropertyType::Checkbox:
		if (!v.is_boolean()) { err = "expects true or false"; return false; }
		return db.setChecked(rowId, pid, v.get<bool>(), now);
	case PropertyType::Date:
	{
		std::int64_t ms = 0;
		if (!v.is_string() || !parseDate(v.get<std::string>(), ms)) { err = "expects a date: 'YYYY-MM-DD' or an ISO time"; return false; }
		return db.setDate(rowId, pid, ms, now);
	}
	case PropertyType::Select:
	{
		if (!v.is_string()) { err = "expects an option name"; return false; }
		const std::string name = v.get<std::string>();
		const Property* cur = db.findProperty(pid);
		const SelectOption* o = cur ? cur->findOptionByName(name) : nullptr;
		if (!o)
		{
			UUID oid;
			if (uuidFromHex(name, oid) && cur) o = cur->findOption(oid);
		}
		if (!o && createOptions) o = db.addOption(pid, name);
		if (!o) { err = "no option '" + name + "' (pass createOptions: true to add it)"; return false; }
		return db.setSelect(rowId, pid, o->id, now);
	}
	case PropertyType::MultiSelect:
	{
		std::vector<UUID> ids;
		for (const std::string& name : stringList(v))
		{
			const Property* cur = db.findProperty(pid);
			const SelectOption* o = cur ? cur->findOptionByName(name) : nullptr;
			if (!o && createOptions) o = db.addOption(pid, name);
			if (!o) { err = "no option '" + name + "' (pass createOptions: true to add it)"; return false; }
			ids.push_back(o->id);
		}
		return db.setMultiSelect(rowId, pid, ids, now);
	}
	case PropertyType::Person:
		if (!v.is_string() && !v.is_array()) { err = "expects names: an array of strings or 'A, B'"; return false; }
		return db.setPeople(rowId, pid, stringList(v), now);
	case PropertyType::Relation:
	{
		const Database* target = (p.relationDb == UUID{} || p.relationDb == db.id) ? &db : ws.find(p.relationDb);
		if (!target) { err = "the database this relation points at is gone"; return false; }
		std::vector<UUID> ids;
		for (const std::string& key : stringList(v))
		{
			const Row* r = findRow(*target, key);
			if (!r) { err = "no row '" + key + "' in " + target->name + " (an id or a unique title)"; return false; }
			ids.push_back(r->id);
		}
		return db.setRelation(rowId, pid, ids, now);
	}
	case PropertyType::EngineLink:
	{
		if (!v.is_array()) { err = "expects an array of links"; return false; }
		std::vector<TaskLink> links;
		for (const json& e : v)
		{
			TaskLink l;
			if (!linkFrom(e, l, err)) return false;
			links.push_back(std::move(l));
		}
		return db.setLinks(rowId, pid, links, now);
	}
	default:
		err = "this kind of property cannot be set from here";
		return false;
	}
}

// Every key of `values` onto the row; the errors are collected, not thrown.
// Returns false when any key did not fit.
bool applyValues(Workspace& ws, Database& db, const UUID& rowId, const json& values, bool createOptions, json& errors)
{
	bool ok = true;
	if (!values.is_object()) return true;
	for (auto it = values.begin(); it != values.end(); ++it)
	{
		const Property* p = findProperty(db, it.key());
		std::string err;
		if (!p)
		{
			errors.push_back(json{ { "property", it.key() }, { "error", "no such property in " + db.name } });
			ok = false;
			continue;
		}
		if (!applyValue(ws, db, rowId, *p, it.value(), createOptions, err))
		{
			errors.push_back(json{ { "property", p->name }, { "error", err.empty() ? std::string("could not be set") : err } });
			ok = false;
		}
	}
	return ok;
}

// ── Pages as JSON / Markdown ─────────────────────────────────────────────────

std::string blockTypeWord(const Block& b)
{
	return b.type == BlockType::Other ? b.typeOther : blockTypeName(b.type);
}

json blockJson(const Block& b)
{
	json j{ { "id", uuidToHex(b.id) }, { "type", blockTypeWord(b) } };
	if (!b.text.empty()) j["text"] = b.text;
	if (b.type == BlockType::Todo) j["checked"] = b.checked;
	if (b.type == BlockType::Code && !b.language.empty()) j["language"] = b.language;
	if (b.type == BlockType::EngineLink) j["link"] = linkJson(b.link);
	if (b.type == BlockType::PageLink) j["page"] = uuidToHex(b.page);
	if (b.type == BlockType::DatabaseView) j["database"] = uuidToHex(b.database);
	if (!b.children.empty())
	{
		json kids = json::array();
		for (const Block& c : b.children) kids.push_back(blockJson(c));
		j["children"] = std::move(kids);
	}
	return j;
}

MarkdownContext markdownContext(Workspace& ws)
{
	MarkdownContext c;
	c.page = [&ws](const std::string& title) {
		UUID found;
		for (Page* p : ws.pages())
			if (!ws.find(p->parent) && sameName(p->title, title))
			{
				if (!(found == UUID{})) return UUID{};   // ambiguous: stays text
				found = p->id;
			}
		return found;
	};
	c.database = [&ws](const std::string& name) {
		const Database* db = findDatabase(ws, name);
		return db ? db->id : UUID{};
	};
	c.pageTitle = [&ws](const UUID& id) {
		const Page* p = ws.findPage(id);
		return p ? p->title : std::string{};
	};
	c.databaseName = [&ws](const UUID& id) {
		const Database* d = ws.find(id);
		return d ? d->name : std::string{};
	};
	return c;
}

json pageJson(Workspace& ws, const Page& page, bool withBlocks)
{
	const Database* owner = ws.find(page.parent);
	json j{ { "id", uuidToHex(page.id) }, { "title", page.title }, { "blockCount", page.blocks.size() } };
	if (owner)
	{
		const Row* row = owner->findRow(page.id);
		j["rowOf"] = json{ { "database", uuidToHex(owner->id) }, { "databaseName", owner->name },
		                   { "row", uuidToHex(page.id) } };
		if (row) j["title"] = row->title;
	}
	else if (!(page.parent == UUID{}))
		j["parent"] = uuidToHex(page.parent);
	if (withBlocks)
	{
		json blocks = json::array();
		for (const Block& b : page.blocks) blocks.push_back(blockJson(b));
		j["markdown"] = pageToMarkdown(page, markdownContext(ws));
		j["blocks"] = std::move(blocks);
	}
	return j;
}

// Which page the arguments name: `page` (an id or a unique title of a standalone page),
// or `database` + `row` (the page of a row, which exists in memory until it has a block).
// Null with the refusal in `fail` when they name none.
Page* pageFrom(Workspace& ws, const json& args, ToolResult& fail)
{
	const std::string pageKey = strArg(args, "page"), dbKey = strArg(args, "database"), rowKey = strArg(args, "row");
	if (!pageKey.empty())
	{
		UUID id;
		if (uuidFromHex(pageKey, id))
		{
			if (Page* p = ws.findPage(id)) return p;
		}
		else
		{
			Page* found = nullptr;
			for (Page* p : ws.pages())
				if (!ws.find(p->parent) && sameName(p->title, pageKey))
				{
					if (found) { fail = ToolResult::fail("ambiguous", "more than one page is called '" + pageKey + "': use its id"); return nullptr; }
					found = p;
				}
			if (found) return found;
		}
		fail = ToolResult::fail("not_found", "no page '" + pageKey + "' — planner_overview lists them");
		return nullptr;
	}
	if (!dbKey.empty() && !rowKey.empty())
	{
		Database* db = findDatabase(ws, dbKey);
		if (!db) { fail = ToolResult::fail("not_found", "no database '" + dbKey + "'"); return nullptr; }
		const Row* row = findRow(*db, rowKey);
		if (!row) { fail = ToolResult::fail("not_found", "no row '" + rowKey + "' in " + db->name + " (an id or a unique title)"); return nullptr; }
		Page* p = ws.openRowPage(row->id, db->id);
		if (!p) fail = ToolResult::fail("save_failed", "the page of that row could not be opened: " + ws.lastError());
		return p;
	}
	fail = invalid("name the page: 'page' (id or title), or 'database' and 'row'");
	return nullptr;
}


// ── Whiteboards ──────────────────────────────────────────────────────────────

Board* findBoardByKey(Workspace& ws, const std::string& key)
{
	UUID id;
	if (uuidFromHex(key, id)) return ws.findBoard(id);
	Board* found = nullptr;
	for (Board* b : ws.boards())
		if (sameName(b->title, key))
		{
			if (found) return nullptr;
			found = b;
		}
	return found;
}

Board* boardFrom(Workspace& ws, const json& args, ToolResult& fail)
{
	const std::string key = strArg(args, "board");
	Board* b = findBoardByKey(ws, key);
	if (!b) fail = ToolResult::fail("not_found", "no whiteboard '" + key + "' (or two share the title: use the id) — planner_overview lists them");
	return b;
}

json nodeJson(Workspace& ws, const Node& n)
{
	json j{ { "id", uuidToHex(n.id) }, { "type", nodeTypeName(n.type) }, { "x", n.x }, { "y", n.y }, { "w", n.w }, { "h", n.h } };
	if (!n.color.empty()) j["color"] = n.color;
	if (!n.text.empty()) j["text"] = n.text;
	if (n.type == NodeType::Page)
	{
		const Page* p = ws.findPage(n.page);
		j["page"] = json{ { "id", uuidToHex(n.page) }, { "title", p ? p->title : std::string("(not found)") } };
	}
	if (n.type == NodeType::Row)
	{
		const Database* db = ws.find(n.database);
		const Row* r = db ? db->findRow(n.row) : nullptr;
		j["row"] = json{ { "database", uuidToHex(n.database) }, { "row", uuidToHex(n.row) }, { "title", r ? r->title : std::string("(not found)") } };
	}
	if (n.type == NodeType::Image)
	{
		if (!(n.asset == UUID{})) j["asset"] = uuidToHex(n.asset);
		if (!n.media.empty()) j["media"] = n.media;
	}
	if (n.type == NodeType::Object) j["label"] = n.link.label;
	return j;
}

json edgeJson(const Edge& e)
{
	json j{ { "id", uuidToHex(e.id) }, { "from", uuidToHex(e.from) }, { "to", uuidToHex(e.to) }, { "arrow", e.arrow } };
	if (!e.label.empty()) j["label"] = e.label;
	return j;
}

json boardSummary(const Board& b)
{
	return json{ { "id", uuidToHex(b.id) }, { "title", b.title }, { "cards", b.nodes.size() }, { "arrows", b.edges.size() } };
}

// A card of this board by id.
Node* nodeFrom(Board& b, const std::string& key, ToolResult& fail)
{
	UUID id;
	Node* n = uuidFromHex(key, id) ? b.findNode(id) : nullptr;
	if (!n) fail = ToolResult::fail("not_found", "no card '" + key + "' on this whiteboard — planner_read_board lists them");
	return n;
}

} // namespace

void registerPlannerTools(McpToolRegistry& registry, McpPlannerHooks hooks)
{
	// ── planner_overview ─────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_overview";
		t.description =
			"The open project's Planner (Window > Planner): every database with its id, "
			"properties (name, type, select options), views and row count, and every page "
			"with its id, title and parent. The database \"Tasks\" is the Task Board "
			"(task_list shows the same tasks with their history). Pages of database rows "
			"are listed with the database and row they belong to.";
		t.inputSchema = objectSchema(json::object(), {});
		t.handler = [hooks](const json&) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			json dbs = json::array();
			for (Database* db : ws->databases()) dbs.push_back(databaseJson(*db));
			json pages = json::array();
			for (Page* p : ws->pages()) pages.push_back(pageJson(*ws, *p, false));
			json unreadable = json::array();
			for (const Workspace::Unreadable& u : ws->unreadable())
				unreadable.push_back(json{ { "kind", "database" }, { "id", uuidToHex(u.id) }, { "error", u.error } });
			for (const Workspace::Unreadable& u : ws->unreadablePages())
				unreadable.push_back(json{ { "kind", "page" }, { "id", uuidToHex(u.id) }, { "error", u.error } });
			json boards = json::array();
			for (Board* b : ws->boards()) boards.push_back(boardSummary(*b));
			return ToolResult::ok(json{ { "databases", std::move(dbs) }, { "pages", std::move(pages) },
			                            { "whiteboards", std::move(boards) }, { "unreadable", std::move(unreadable) } });
		};
		registry.add(std::move(t));
	}

	// ── planner_read_database ────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_read_database";
		t.description =
			"Rows of a Planner database with their values (select options as names, dates as "
			"YYYY-MM-DD, relations as {id, title}, engine links as {kind, id, scene, panel, label}). "
			"Name a `view` to apply its filters and sorting; `search` keeps rows whose title "
			"contains the text. Empty cells are left out.";
		t.inputSchema = objectSchema(json{
			{ "database", stringProp("The database's id or name.") },
			{ "view",     stringProp("A view's id or name: its filters and sorts apply. Default: every row, in file order.") },
			{ "search",   stringProp("Only rows whose title contains this (any case).") },
			{ "limit",    json{ { "type", "integer" }, { "description", "At most this many rows (default 100, at most 500)." } } },
			{ "offset",   json{ { "type", "integer" }, { "description", "Skip this many rows (default 0)." } } },
		}, { "database" });
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			const Database* db = findDatabase(*ws, strArg(args, "database"));
			if (!db) return ToolResult::fail("not_found", "no database '" + strArg(args, "database") + "' (or two share the name: use the id) — planner_overview lists them");
			View view;
			if (const std::string vk = strArg(args, "view"); !vk.empty())
			{
				const View* v = findView(*db, vk);
				if (!v) return ToolResult::fail("not_found", "no view '" + vk + "' in " + db->name);
				view = *v;
			}
			const std::string search = lower(strArg(args, "search"));
			const int limit  = std::clamp(intArg(args, "limit", 100), 1, 500);
			const int offset = std::max(0, intArg(args, "offset", 0));

			std::vector<std::size_t> idx = db->query(view);
			if (!search.empty())
				idx.erase(std::remove_if(idx.begin(), idx.end(), [&](std::size_t i) {
					return lower(db->rows[i].title).find(search) == std::string::npos;
				}), idx.end());
			json rows = json::array();
			for (std::size_t k = static_cast<std::size_t>(offset); k < idx.size() && rows.size() < static_cast<std::size_t>(limit); ++k)
				rows.push_back(rowJson(*ws, *db, db->rows[idx[k]]));
			return ToolResult::ok(json{ { "database", databaseJson(*db) }, { "total", idx.size() },
			                            { "offset", offset }, { "rows", std::move(rows) } });
		};
		registry.add(std::move(t));
	}

	// ── planner_add_row ──────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_add_row";
		t.description =
			"Add a row to a Planner database. `values` maps property names (or ids) to values: "
			"text/url a string, number a number, checkbox true/false, date 'YYYY-MM-DD', select an "
			"option name, multi-select/person an array of names, relation an array of row ids or "
			"unique titles of the target database, engine-link an array of {kind: asset|entity|scene|"
			"panel, id|scene|panel, label}. `null` leaves a cell empty. Nothing is written if any value "
			"does not fit (the answer lists which). In the database \"Tasks\" this adds a task to the "
			"Task Board.";
		t.inputSchema = objectSchema(json{
			{ "database", stringProp("The database's id or name.") },
			{ "title",    stringProp("The row's title.") },
			{ "values",   json{ { "type", "object" }, { "description", "Property name (or id) to value." } } },
			{ "createOptions", json{ { "type", "boolean" }, { "description", "Add a select option that does not exist yet (default true)." } } },
		}, { "database", "title" });
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			Database* db = findDatabase(*ws, strArg(args, "database"));
			if (!db) return ToolResult::fail("not_found", "no database '" + strArg(args, "database") + "' — planner_overview lists them");
			const std::string title = strArg(args, "title");
			const bool create = boolArg(args, "createOptions", true);
			const json values = args.contains("values") ? args["values"] : json::object();
			if (!values.is_object()) return invalid("'values' must be an object of property to value");
			const UUID dbId = db->id;
			const std::string author = hooks.author ? hooks.author() : std::string{};

			// A dry run on a copy: nothing is written unless every value fits.
			json errors = json::array();
			{
				Database copy = *db;
				const UUID rid = copy.addRow(title, author, taskNowMs())->id;
				applyValues(*ws, copy, rid, values, create, errors);
			}
			if (!errors.empty()) return ToolResult::fail("invalid_payload", errors.dump());

			UUID rowId;
			const bool saved = ws->edit(dbId, [&](Database& d) {
				Row* r = d.addRow(title, author, taskNowMs());
				rowId = r->id;
				json again = json::array();
				applyValues(*ws, d, rowId, values, create, again);
				return true;
			});
			if (!saved)
				return ToolResult::fail("save_failed", "the row was not written: " + ws->lastError());
			const Database* now = ws->find(dbId);
			const Row* row = now ? now->findRow(rowId) : nullptr;
			if (!row)
			{
				// "Tasks": the board gave the task its own spelling of the row; find it by title.
				if (now)
					for (auto it = now->rows.rbegin(); it != now->rows.rend() && !row; ++it)
						if (it->title == title) row = &*it;
			}
			return ToolResult::ok(json{ { "row", row ? rowJson(*ws, *now, *row) : json(nullptr) } });
		};
		registry.add(std::move(t));
	}

	// ── planner_update_row ───────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_update_row";
		t.description =
			"Change a row of a Planner database: a new `title` and/or `values` (same shapes as "
			"planner_add_row; `null` clears a cell, a key that is left out is not touched). The "
			"row is its id or a unique title. Nothing is written if any value does not fit. In "
			"\"Tasks\" a status or assignee change goes into the task's history and the team is notified.";
		t.inputSchema = objectSchema(json{
			{ "database", stringProp("The database's id or name.") },
			{ "row",      stringProp("The row's id, or its title when that is unique.") },
			{ "title",    stringProp("A new title.") },
			{ "values",   json{ { "type", "object" }, { "description", "Property name (or id) to value." } } },
			{ "createOptions", json{ { "type", "boolean" }, { "description", "Add a select option that does not exist yet (default true)." } } },
		}, { "database", "row" });
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			Database* db = findDatabase(*ws, strArg(args, "database"));
			if (!db) return ToolResult::fail("not_found", "no database '" + strArg(args, "database") + "'");
			const Row* row = findRow(*db, strArg(args, "row"));
			if (!row) return ToolResult::fail("not_found", "no row '" + strArg(args, "row") + "' in " + db->name + " (an id, or a unique title)");
			const bool hasTitle = args.contains("title") && args["title"].is_string();
			const bool create = boolArg(args, "createOptions", true);
			const json values = args.contains("values") ? args["values"] : json::object();
			if (!values.is_object()) return invalid("'values' must be an object of property to value");
			if (!hasTitle && values.empty()) return invalid("give a 'title', 'values' or both");
			const UUID dbId = db->id, rowId = row->id;
			const std::string title = hasTitle ? args["title"].get<std::string>() : std::string{};

			json errors = json::array();
			{
				Database copy = *db;
				applyValues(*ws, copy, rowId, values, create, errors);
			}
			if (!errors.empty()) return ToolResult::fail("invalid_payload", errors.dump());

			const bool saved = ws->edit(dbId, [&](Database& d) {
				bool any = false;
				if (hasTitle) any |= d.setTitle(rowId, title, taskNowMs());
				json again = json::array();
				applyValues(*ws, d, rowId, values, create, again);
				any = true;
				return any;
			});
			if (!saved) return ToolResult::fail("save_failed", "the change was not written: " + ws->lastError());
			const Database* now = ws->find(dbId);
			const Row* r = now ? now->findRow(rowId) : nullptr;
			return ToolResult::ok(json{ { "row", r ? rowJson(*ws, *now, *r) : json(nullptr) } });
		};
		registry.add(std::move(t));
	}

	// ── planner_delete_row ───────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_delete_row";
		t.description =
			"Delete a row of a Planner database, its page, and any relation that pointed at it. "
			"There is no undo; source control has the previous version. In \"Tasks\" this deletes "
			"the task from the Task Board.";
		t.inputSchema = objectSchema(json{
			{ "database", stringProp("The database's id or name.") },
			{ "row",      stringProp("The row's id, or its title when that is unique.") },
		}, { "database", "row" });
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			Database* db = findDatabase(*ws, strArg(args, "database"));
			if (!db) return ToolResult::fail("not_found", "no database '" + strArg(args, "database") + "'");
			const Row* row = findRow(*db, strArg(args, "row"));
			if (!row) return ToolResult::fail("not_found", "no row '" + strArg(args, "row") + "' in " + db->name);
			const UUID dbId = db->id, rowId = row->id;
			const std::string title = row->title;
			if (!ws->edit(dbId, [&](Database& d) { return d.removeRow(rowId); }))
				return ToolResult::fail("save_failed", "the row was not deleted: " + ws->lastError());
			ws->removePage(rowId);
			return ToolResult::ok(json{ { "deleted", uuidToHex(rowId) }, { "title", title } });
		};
		registry.add(std::move(t));
	}

	// ── planner_create_database ──────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_create_database";
		t.description =
			"Make a new Planner database with a table view and, optionally, its properties. Each "
			"property is {name, type, options?}: type is one of text, number, checkbox, date, select, "
			"multi-select, person, relation, engine-link, url; options (select types) a list of names; "
			"a relation points at `target` (a database id or name; default the new database itself). "
			"Saved into Config/Planner/Databases.";
		t.inputSchema = objectSchema(json{
			{ "name", stringProp("The database's name.") },
			{ "properties", json{ { "type", "array" }, { "description", "Properties: {name, type, options?, target?}." },
			                      { "items", json{ { "type", "object" } } } } },
		}, { "name" });
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			const std::string name = strArg(args, "name");
			if (name.empty()) return invalid("'name' must not be empty");
			const json props = args.contains("properties") ? args["properties"] : json::array();
			if (!props.is_array()) return invalid("'properties' must be an array");
			for (const json& p : props)
			{
				PropertyType type;
				if (!p.is_object() || !p.contains("name") || !p["name"].is_string() ||
				    !propertyTypeFromName(p.value("type", std::string("text")), type))
					return invalid("every property needs a 'name' and a 'type' (text, number, checkbox, date, select, multi-select, person, relation, engine-link, url)");
			}
			Database* db = ws->create(name);
			if (!db) return ToolResult::fail("save_failed", "the database was not created: " + ws->lastError());
			const UUID dbId = db->id;
			json errors = json::array();
			ws->edit(dbId, [&](Database& d) {
				for (const json& p : props)
				{
					PropertyType type = PropertyType::Text;
					propertyTypeFromName(p.value("type", std::string("text")), type);
					Property* np = d.addProperty(p["name"].get<std::string>(), type);
					const UUID pid = np->id;
					if (type == PropertyType::Select || type == PropertyType::MultiSelect)
						for (const std::string& option : stringList(p.value("options", json::array())))
							d.addOption(pid, option);
					if (type == PropertyType::Relation)
					{
						const std::string target = p.value("target", std::string{});
						const Database* other = target.empty() ? &d : findDatabase(*ws, target);
						d.findProperty(pid)->relationDb = other ? other->id : d.id;
						if (!target.empty() && !other)
							errors.push_back(json{ { "property", p["name"] }, { "error", "no database '" + target + "': it points at this one" } });
					}
				}
				return true;
			});
			const Database* now = ws->find(dbId);
			json answer{ { "database", now ? databaseJson(*now) : json(nullptr) } };
			if (!errors.empty()) answer["warnings"] = errors;
			return ToolResult::ok(std::move(answer));
		};
		registry.add(std::move(t));
	}

	// ── planner_add_property ─────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_add_property";
		t.description =
			"Add a property (a column) to a Planner database. Not for \"Tasks\": its columns are fixed "
			"(the Task Board keeps them).";
		t.inputSchema = objectSchema(json{
			{ "database", stringProp("The database's id or name.") },
			{ "name",     stringProp("The property's name.") },
			{ "type",     json{ { "type", "string" }, { "enum", { "text", "number", "checkbox", "date", "select", "multi-select", "person", "relation", "engine-link", "url" } } } },
			{ "options",  json{ { "type", "array" }, { "items", json{ { "type", "string" } } }, { "description", "Option names (select, multi-select)." } } },
			{ "target",   stringProp("The database a relation points at (id or name); default: this one.") },
		}, { "database", "name", "type" });
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			Database* db = findDatabase(*ws, strArg(args, "database"));
			if (!db) return ToolResult::fail("not_found", "no database '" + strArg(args, "database") + "'");
			PropertyType type;
			if (!propertyTypeFromName(strArg(args, "type"), type))
				return invalid("'type' must be text, number, checkbox, date, select, multi-select, person, relation, engine-link or url");
			const std::string name = strArg(args, "name");
			if (name.empty()) return invalid("'name' must not be empty");
			if (findProperty(*db, name)) return ToolResult::fail("exists", "there is a property '" + name + "' already");
			UUID target = db->id;
			if (const std::string tk = strArg(args, "target"); !tk.empty())
			{
				const Database* other = findDatabase(*ws, tk);
				if (!other) return ToolResult::fail("not_found", "no database '" + tk + "' to point at");
				target = other->id;
			}
			const std::vector<std::string> options = stringList(args.contains("options") ? args["options"] : json::array());
			const UUID dbId = db->id;
			UUID pid;
			if (!ws->edit(dbId, [&](Database& d) {
				Property* np = d.addProperty(name, type);
				pid = np->id;
				if (type == PropertyType::Relation) np->relationDb = target;
				if (type == PropertyType::Select || type == PropertyType::MultiSelect)
					for (const std::string& o : options) d.addOption(pid, o);
				return true;
			}))
				return ToolResult::fail("refused", "the property was not added: " + ws->lastError());
			const Database* now = ws->find(dbId);
			const Property* np = now ? now->findProperty(pid) : nullptr;
			return ToolResult::ok(json{ { "property", np ? propertyJson(*np) : json(nullptr) } });
		};
		registry.add(std::move(t));
	}

	// ── planner_read_page ────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_read_page";
		t.description =
			"A Planner page as Markdown and as blocks. Name it by `page` (id, or a unique title of "
			"a standalone page) or by `database` + `row` (the page of a row; empty until written). "
			"The Markdown speaks the block model: # headings, - bullets, 1. numbers, - [ ] to-dos, "
			"> quotes, > [!callout], ``` code, ---, [label](asset:<id>) engine objects, [[Page]] and "
			"{{database: Name}}; children are indented two spaces.";
		t.inputSchema = objectSchema(json{
			{ "page",     stringProp("A page's id or unique title.") },
			{ "database", stringProp("With `row`: the database of the row whose page to read.") },
			{ "row",      stringProp("With `database`: the row's id or unique title.") },
		}, {});
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			ToolResult fail;
			Page* page = pageFrom(*ws, args, fail);
			if (!page) return fail;
			return ToolResult::ok(json{ { "page", pageJson(*ws, *page, true) } });
		};
		registry.add(std::move(t));
	}

	// ── planner_write_page ───────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_write_page";
		t.description =
			"Write a Planner page from Markdown (the dialect planner_read_page shows). With `page` or "
			"`database` + `row` it changes that page: `mode` replace (default) swaps every block, append adds "
			"to the end. With neither, it creates a new standalone page called `title`, under `parent` "
			"if given. A `title` on an existing page renames it (a row's page: the row). Saved into "
			"Config/Planner/Pages; the open Planner tab shows it at once. Lines that name a page "
			"([[Title]]) or database ({{database: Name}}) that does not exist stay plain text.";
		t.inputSchema = objectSchema(json{
			{ "page",     stringProp("An existing page's id or unique title.") },
			{ "database", stringProp("With `row`: the database of the row whose page to write.") },
			{ "row",      stringProp("With `database`: the row's id or unique title.") },
			{ "title",    stringProp("The title of a new page, or a new title for an existing one.") },
			{ "parent",   stringProp("For a new page: the id or title of the page it goes under.") },
			{ "markdown", stringProp("The page's content.") },
			{ "mode",     json{ { "type", "string" }, { "enum", { "replace", "append" } }, { "description", "Default replace." } } },
		}, {});
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			const bool hasMarkdown = args.contains("markdown") && args["markdown"].is_string();
			const bool hasTitle = args.contains("title") && args["title"].is_string();
			const std::string mode = strArg(args, "mode");
			if (!mode.empty() && mode != "replace" && mode != "append") return invalid("'mode' must be replace or append");
			const bool append = mode == "append";
			const bool targeted = !strArg(args, "page").empty() || (!strArg(args, "database").empty() && !strArg(args, "row").empty());

			Page* page = nullptr;
			if (targeted)
			{
				ToolResult fail;
				page = pageFrom(*ws, args, fail);
				if (!page) return fail;
			}
			else
			{
				if (!hasTitle) return invalid("name an existing page ('page', or 'database' + 'row'), or give a 'title' for a new one");
				UUID parent;
				if (const std::string pk = strArg(args, "parent"); !pk.empty())
				{
					ToolResult fail;
					json under{ { "page", pk } };
					Page* p = pageFrom(*ws, under, fail);
					if (!p) return fail;
					parent = p->id;
				}
				page = ws->createPage(args["title"].get<std::string>(), parent);
				if (!page) return ToolResult::fail("save_failed", "the page was not created: " + ws->lastError());
			}
			const UUID pageId = page->id;
			const UUID owner = page->parent;
			const bool rowPage = ws->find(owner) != nullptr;

			if (hasMarkdown)
			{
				std::vector<Block> blocks = markdownToBlocks(args["markdown"].get<std::string>(), markdownContext(*ws));
				if (!ws->editPage(pageId, [&](Page& p) {
					if (append) for (Block& b : blocks) p.blocks.push_back(b);
					else        p.blocks = blocks;
					p.modifiedMs = taskNowMs();
					return true;
				}))
					return ToolResult::fail("save_failed", "the page was not written: " + ws->lastError());
			}
			if (hasTitle && targeted)
			{
				const std::string title = args["title"].get<std::string>();
				bool ok = true;
				if (rowPage)
					ok = ws->edit(owner, [&](Database& d) { return d.setTitle(pageId, title, taskNowMs()); });
				else
					ok = ws->editPage(pageId, [&](Page& p) {
						if (p.title == title) return false;
						p.title = title;
						p.modifiedMs = taskNowMs();
						return true;
					}) || (ws->findPage(pageId) && ws->findPage(pageId)->title == title);
				if (!ok) return ToolResult::fail("save_failed", "the title was not changed: " + ws->lastError());
			}
			Page* now = ws->findPage(pageId);
			return ToolResult::ok(json{ { "page", now ? pageJson(*ws, *now, true) : json(nullptr) } });
		};
		registry.add(std::move(t));
	}

	// ── planner_delete_page ──────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_delete_page";
		t.description =
			"Delete a standalone Planner page and its sub-pages (and their files). There is no undo; "
			"source control has the previous version. The page of a database row goes with the row "
			"(planner_delete_row); to empty it, planner_write_page with empty markdown.";
		t.inputSchema = objectSchema(json{
			{ "page", stringProp("The page's id or unique title.") },
		}, { "page" });
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			ToolResult fail;
			Page* page = pageFrom(*ws, args, fail);
			if (!page) return fail;
			if (ws->find(page->parent))
				return ToolResult::fail("refused", "that is the page of a database row: it goes with the row (planner_delete_row)");
			const UUID id = page->id;
			const std::string title = page->title;
			if (!ws->removePage(id)) return ToolResult::fail("save_failed", "the page was not deleted: " + ws->lastError());
			return ToolResult::ok(json{ { "deleted", uuidToHex(id) }, { "title", title } });
		};
		registry.add(std::move(t));
	}

	// ── planner_create_board ─────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_create_board";
		t.description = "Make a new, empty Planner whiteboard (Window > Planner, Boards in the sidebar).";
		t.inputSchema = objectSchema(json{ { "title", stringProp("The board's title.") } }, { "title" });
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			const std::string title = strArg(args, "title");
			if (title.empty()) return invalid("title is empty");
			Board* b = ws->createBoard(title);
			if (!b) return ToolResult::fail("save_failed", "the whiteboard was not made: " + ws->lastError());
			return ToolResult::ok(boardSummary(*b));
		};
		registry.add(std::move(t));
	}

	// ── planner_read_board ───────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_read_board";
		t.description =
			"A Planner whiteboard: its cards (id, type text/image/object/row/page/frame, position and size in "
			"board units with y pointing down, colour, text; pages and rows with their titles) and its arrows "
			"(from/to card ids, label). A frame groups the cards that lie wholly inside it.";
		t.inputSchema = objectSchema(json{ { "board", stringProp("The board's id or unique title.") } }, { "board" });
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			ToolResult fail;
			Board* b = boardFrom(*ws, args, fail);
			if (!b) return fail;
			json nodes = json::array(), edges = json::array();
			for (const Node& n : b->nodes) nodes.push_back(nodeJson(*ws, n));
			for (const Edge& e : b->edges) edges.push_back(edgeJson(e));
			json out = boardSummary(*b);
			out["nodes"] = std::move(nodes);
			out["edges"] = std::move(edges);
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── planner_add_board_card ───────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_add_board_card";
		t.description =
			"Add a card to a whiteboard. type: text (default), frame (a titled rectangle; `text` is the title), "
			"page (give `page`, a page's id or title) or row (give `database` and `row`, a row's id or title). "
			"Position (x, y = the top left) and size are in board units; default size 240x120 (frame 480x320).";
		t.inputSchema = objectSchema(json{
			{ "board",    stringProp("The board's id or unique title.") },
			{ "type",     stringProp("text, frame, page or row. Default text.") },
			{ "text",     stringProp("The card's text, or the frame's title.") },
			{ "x",        json{ { "type", "number" } } },
			{ "y",        json{ { "type", "number" } } },
			{ "w",        json{ { "type", "number" } } },
			{ "h",        json{ { "type", "number" } } },
			{ "color",    stringProp("A colour word (red, orange, yellow, green, teal, blue, purple, pink) or empty.") },
			{ "page",     stringProp("For type page: the page's id or unique title.") },
			{ "database", stringProp("For type row: the database's id or name.") },
			{ "row",      stringProp("For type row: the row's id or title.") },
		}, { "board" });
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			ToolResult fail;
			Board* b0 = boardFrom(*ws, args, fail);
			if (!b0) return fail;
			const UUID boardId = b0->id;
			const std::string typeArg = strArg(args, "type");
			const std::string typeWord = lower(typeArg.empty() ? "text" : typeArg);
			NodeType type;
			if (!nodeTypeFromName(typeWord, type) || (type != NodeType::Text && type != NodeType::Frame && type != NodeType::Page && type != NodeType::Row))
				return invalid("type must be text, frame, page or row");
			UUID page, dbId, rowId;
			if (type == NodeType::Page)
			{
				Page* p = pageFrom(*ws, json{ { "page", strArg(args, "page") } }, fail);
				if (!p) return fail;
				page = p->id;
			}
			if (type == NodeType::Row)
			{
				Database* db = findDatabase(*ws, strArg(args, "database"));
				if (!db) return ToolResult::fail("not_found", "no database '" + strArg(args, "database") + "' — planner_overview lists them");
				const std::string rk = strArg(args, "row");
				UUID rid;
				const Row* row = uuidFromHex(rk, rid) ? db->findRow(rid) : nullptr;
				if (!row)
					for (const Row& r : db->rows)
						if (sameName(r.title, rk)) { row = &r; break; }
				if (!row) return ToolResult::fail("not_found", "no row '" + rk + "' in " + db->name);
				dbId = db->id;
				rowId = row->id;
			}
			const bool frame = type == NodeType::Frame;
			const double w = numArg(args, "w", frame ? 480 : 240), h = numArg(args, "h", frame ? 320 : 120);
			const double x = numArg(args, "x", 0), y = numArg(args, "y", 0);
			const std::string text = strArg(args, "text"), color = lower(strArg(args, "color"));
			UUID made;
			if (!ws->editBoard(boardId, [&](Board& b) {
				    Node* n = b.addNode(type, x, y, w, h, text, taskNowMs());
				    n->color = color;
				    n->page = page;
				    n->database = dbId;
				    n->row = rowId;
				    made = n->id;   // before sendToBack: that moves the cards in the vector
				    if (frame) b.sendToBack(made, taskNowMs());
				    return true;
			    }))
				return ToolResult::fail("save_failed", "the card was not added: " + ws->lastError());
			const Board* b = ws->findBoard(boardId);
			return ToolResult::ok(nodeJson(*ws, *b->findNode(made)));
		};
		registry.add(std::move(t));
	}

	// ── planner_update_board_card ────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_update_board_card";
		t.description = "Change a whiteboard card: its text, colour, position or size (only what is given). `color` empty clears it. A frame does not carry its cards along.";
		t.inputSchema = objectSchema(json{
			{ "board", stringProp("The board's id or unique title.") },
			{ "card",  stringProp("The card's id (planner_read_board).") },
			{ "text",  stringProp("New text / title.") },
			{ "color", stringProp("A colour word or empty.") },
			{ "x", json{ { "type", "number" } } }, { "y", json{ { "type", "number" } } },
			{ "w", json{ { "type", "number" } } }, { "h", json{ { "type", "number" } } },
		}, { "board", "card" });
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			ToolResult fail;
			Board* b0 = boardFrom(*ws, args, fail);
			if (!b0) return fail;
			const UUID boardId = b0->id;
			Node* n0 = nodeFrom(*b0, strArg(args, "card"), fail);
			if (!n0) return fail;
			const UUID cardId = n0->id;
			if (!ws->editBoard(boardId, [&](Board& b) {
				    Node* n = b.findNode(cardId);
				    if (!n) return false;
				    const std::int64_t now = taskNowMs();
				    if (args.contains("text"))  b.setNodeText(cardId, strArg(args, "text"), now);
				    if (args.contains("color")) b.setNodeColor(cardId, lower(strArg(args, "color")), now);
				    if (args.contains("x") || args.contains("y") || args.contains("w") || args.contains("h"))
					    b.resizeNode(cardId, numArg(args, "x", n->x), numArg(args, "y", n->y), numArg(args, "w", n->w), numArg(args, "h", n->h), now);
				    return true;
			    }))
				return ToolResult::fail("save_failed", "the card was not changed: " + ws->lastError());
			return ToolResult::ok(nodeJson(*ws, *ws->findBoard(boardId)->findNode(cardId)));
		};
		registry.add(std::move(t));
	}

	// ── planner_delete_board_card ────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_delete_board_card";
		t.description = "Delete a card from a whiteboard, with the arrows that end at it. Source control has the previous version.";
		t.inputSchema = objectSchema(json{
			{ "board", stringProp("The board's id or unique title.") },
			{ "card",  stringProp("The card's id.") },
		}, { "board", "card" });
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			ToolResult fail;
			Board* b0 = boardFrom(*ws, args, fail);
			if (!b0) return fail;
			const UUID boardId = b0->id;
			Node* n0 = nodeFrom(*b0, strArg(args, "card"), fail);
			if (!n0) return fail;
			const UUID cardId = n0->id;
			if (!ws->editBoard(boardId, [&](Board& b) { return b.removeNode(cardId, taskNowMs()); }))
				return ToolResult::fail("save_failed", "the card was not deleted: " + ws->lastError());
			return ToolResult::ok(json{ { "deleted", uuidToHex(cardId) } });
		};
		registry.add(std::move(t));
	}

	// ── planner_connect_board_cards ──────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_connect_board_cards";
		t.description = "Draw an arrow between two cards of a whiteboard (from -> to), optionally labelled and without a head. The same two cards in the same direction have one arrow only.";
		t.inputSchema = objectSchema(json{
			{ "board", stringProp("The board's id or unique title.") },
			{ "from",  stringProp("The card the arrow starts at (id).") },
			{ "to",    stringProp("The card it ends at (id).") },
			{ "label", stringProp("Text on the arrow.") },
			{ "arrow", json{ { "type", "boolean" }, { "description", "An arrowhead at `to` (default true)." } } },
		}, { "board", "from", "to" });
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			ToolResult fail;
			Board* b0 = boardFrom(*ws, args, fail);
			if (!b0) return fail;
			const UUID boardId = b0->id;
			Node* f = nodeFrom(*b0, strArg(args, "from"), fail);
			if (!f) return fail;
			const UUID from = f->id;
			Node* tn = nodeFrom(*b0, strArg(args, "to"), fail);
			if (!tn) return fail;
			const UUID to = tn->id;
			if (from == to) return invalid("an arrow needs two different cards");
			const std::string label = strArg(args, "label");
			const bool head = boolArg(args, "arrow", true);
			UUID made;
			if (!ws->editBoard(boardId, [&](Board& b) {
				    Edge* e = b.addEdge(from, to, label, taskNowMs());
				    if (!e) return false;
				    e->arrow = head;
				    made = e->id;
				    return true;
			    }))
				return ToolResult::fail("refused", "no arrow was added: there is one from that card to that already, or the board could not be saved (" + ws->lastError() + ")");
			return ToolResult::ok(edgeJson(*ws->findBoard(boardId)->findEdge(made)));
		};
		registry.add(std::move(t));
	}

	// ── planner_delete_board_arrow ───────────────────────────────────────────
	{
		McpTool t;
		t.name        = "planner_delete_board_arrow";
		t.description = "Delete an arrow from a whiteboard (its id from planner_read_board).";
		t.inputSchema = objectSchema(json{
			{ "board", stringProp("The board's id or unique title.") },
			{ "arrow", stringProp("The arrow's id.") },
		}, { "board", "arrow" });
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			Workspace* ws = workspaceOf(hooks);
			if (!ws) return failNoProject();
			ToolResult fail;
			Board* b0 = boardFrom(*ws, args, fail);
			if (!b0) return fail;
			const UUID boardId = b0->id;
			UUID eid;
			if (!uuidFromHex(strArg(args, "arrow"), eid) || !b0->findEdge(eid))
				return ToolResult::fail("not_found", "no arrow '" + strArg(args, "arrow") + "' on this whiteboard");
			if (!ws->editBoard(boardId, [&](Board& b) { return b.removeEdge(eid, taskNowMs()); }))
				return ToolResult::fail("save_failed", "the arrow was not deleted: " + ws->lastError());
			return ToolResult::ok(json{ { "deleted", uuidToHex(eid) } });
		};
		registry.add(std::move(t));
	}
}

} // namespace HE::Ed
