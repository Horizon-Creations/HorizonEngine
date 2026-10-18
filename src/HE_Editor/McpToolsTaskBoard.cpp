#include "McpToolCommon.h"
#include "McpToolHooks.h"

#include "TaskBoard.h"
#include "TaskBoardTeam.h"

#include <algorithm>
#include <string>
#include <vector>

namespace HE::Ed
{
namespace
{
using json = nlohmann::json;

std::string statusWord(const HE::Task& t)
{
	return t.status == HE::TaskStatus::Other ? t.statusOther : HE::taskStatusName(t.status);
}

std::string priorityWord(const HE::Task& t)
{
	return t.priority == HE::TaskPriority::Other ? t.priorityOther : HE::taskPriorityName(t.priority);
}

json linkJson(const HE::TaskLink& l)
{
	json j{
		{ "kind",  l.kind == HE::TaskLinkKind::Other ? l.kindOther : HE::taskLinkKindName(l.kind) },
		{ "label", l.label },
		// A link whose target is gone: the panel greys its chip out.
		{ "orphaned", l.orphaned },
	};
	if (l.id != HE::UUID{}) j["id"] = HE::uuidToHex(l.id);
	if (!l.scene.empty())   j["scene"] = l.scene;
	if (!l.panel.empty())   j["panel"] = l.panel;
	return j;
}

json updateJson(const HE::TaskUpdate& u)
{
	json j{
		{ "id",     HE::uuidToHex(u.id) },
		{ "kind",   u.kind == HE::TaskUpdateKind::Other ? u.kindOther : HE::taskUpdateKindName(u.kind) },
		{ "author", u.author },
		{ "time",   HE::taskTimeToIso(u.timeMs) },
	};
	if (!u.text.empty())   j["text"] = u.text;
	if (!u.status.empty()) j["status"] = u.status;
	if (u.kind == HE::TaskUpdateKind::Assignment) j["assignees"] = u.assignees;
	return j;
}

// One shape for a task wherever a tool answers with one. `history` adds the
// updates themselves; without it only their count, so a list of fifty tasks
// does not carry every comment ever written on them.
json taskJson(const HE::Task& t, bool history)
{
	json links = json::array();
	for (const HE::TaskLink& l : t.links) links.push_back(linkJson(l));
	json j{
		{ "id",          HE::uuidToHex(t.id) },
		{ "title",       t.title },
		{ "description", t.description },
		{ "status",      statusWord(t) },
		{ "priority",    priorityWord(t) },
		{ "createdBy",   t.createdBy },
		{ "assignedBy",  t.assignedBy },
		{ "assignees",   t.assignees },
		{ "created",     HE::taskTimeToIso(t.createdMs) },
		{ "modified",    HE::taskTimeToIso(t.modifiedMs) },
		{ "links",       std::move(links) },
		{ "updateCount", t.updates.size() },
	};
	if (history)
	{
		json updates = json::array();
		for (const HE::TaskUpdate& u : t.updates) updates.push_back(updateJson(u));
		j["updates"] = std::move(updates);
	}
	return j;
}

// What a script needs to know about the team before it assigns: whether there
// is one, who we are on it, who is on it and who of them is online (a notice
// to somebody offline waits in the outbox until they sign in).
json teamJson(const McpTaskBoardHooks& h)
{
	HE::TaskBoardTeam* team = h.team ? h.team() : nullptr;
	json out{ { "connected", team && team->connected() } };
	if (!team || !team->connected()) return out;
	json members = json::array();
	for (const HE::TaskTeamMember& m : team->members())
		members.push_back(json{ { "name", m.name }, { "online", m.online } });
	out["self"]    = team->self();
	out["members"] = std::move(members);
	out["pending"] = team->pending();
	return out;
}

std::string authorOf(const McpTaskBoardHooks& h)
{
	return h.author ? h.author() : std::string{};
}

HE::TaskBoard* boardOf(const McpTaskBoardHooks& h)
{
	return h.board ? h.board() : nullptr;
}

ToolResult failNoProject()
{
	return ToolResult::fail("no_project",
		"no project is open — the task board is the open project's Config/TaskBoard.json");
}

ToolResult failNotFound(const std::string& id)
{
	return ToolResult::fail("not_found",
		"no task with id '" + id + "' on this board — task_list gives the ids");
}

// Parse `id`; false with the refusal already in `fail` otherwise.
bool taskIdFrom(const json& args, HE::UUID& out, ToolResult& fail)
{
	const std::string hex = strArg(args, "id");
	if (hex.empty() || !HE::uuidFromHex(hex, out))
	{
		fail = ToolResult::fail("invalid_payload", "'id' must be a task id (32 hex digits) from task_list");
		return false;
	}
	return true;
}

// Run `fn` through the one writer and say what happened. `found` is set by
// `fn`: false there is "no such task", anything else that comes back false is
// a save that did not land (the edit stays in the editor's memory, the panel
// shows the same banner).
ToolResult runEdit(const McpTaskBoardHooks& h, const std::string& id, bool& found,
                   const std::function<bool(HE::TaskBoard&)>& fn,
                   const std::function<json()>& answer)
{
	found = false;
	const bool saved = h.edit && h.edit(fn);
	if (!found) return failNotFound(id);
	if (!saved)
		return ToolResult::fail("save_failed",
			"the change is in the editor but Config/TaskBoard.json was not written: " +
			(h.lastError ? h.lastError() : std::string("unknown reason")));
	return ToolResult::ok(answer());
}

json taskAnswer(const McpTaskBoardHooks& h, const HE::UUID& id)
{
	json out = json::object();
	if (HE::TaskBoard* b = boardOf(h))
		if (const HE::Task* t = b->find(id)) out["task"] = taskJson(*t, true);
	out["team"] = teamJson(h);
	return out;
}

json statusEnumProp(const char* what)
{
	return json{ { "type", "string" }, { "enum", { "open", "in-progress", "blocked", "done" } },
	             { "description", what } };
}

json priorityEnumProp(const char* what)
{
	return json{ { "type", "string" }, { "enum", { "low", "normal", "high", "urgent" } },
	             { "description", what } };
}

} // namespace

void registerTaskBoardTools(McpToolRegistry& registry, McpTaskBoardHooks hooks)
{
	// ── task_list ────────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "task_list";
		t.description =
			"The open project's task board (Window > Task Board): every task with its "
			"id, title, status (open, in-progress, blocked, done), priority, who created "
			"and who assigned it, its assignees and the project content it links to. "
			"Also who edits are recorded under (`author`), the open counts the footer "
			"shows, and the team: whether one is signed in, its members and who of them "
			"is online. Filters are optional and combine.";
		t.inputSchema = objectSchema(json{
			{ "status",   statusEnumProp("Only tasks with this status.") },
			{ "assignee", stringProp("Only tasks assigned to this member (any case).") },
			{ "includeDone", json{ { "type", "boolean" },
			                       { "description", "List done tasks too (default true)." } } },
			{ "history", json{ { "type", "boolean" },
			                   { "description", "Include every task's updates (comments, status "
			                                    "changes, assignments). Default false: only "
			                                    "their count." } } },
		}, {});
		t.handler = [hooks](const json& args) -> ToolResult {
			HE::TaskBoard* b = boardOf(hooks);
			if (!b) return failNoProject();
			const std::string status   = strArg(args, "status");
			const std::string assignee = strArg(args, "assignee");
			const bool includeDone = boolArg(args, "includeDone", true);
			const bool history     = boolArg(args, "history", false);

			json tasks = json::array();
			for (const HE::Task& task : b->tasks)
			{
				if (!includeDone && task.isDone()) continue;
				if (!status.empty() && statusWord(task) != status) continue;
				if (!assignee.empty() &&
				    std::none_of(task.assignees.begin(), task.assignees.end(),
				                 [&](const std::string& a) { return HE::sameTaskMember(a, assignee); }))
					continue;
				tasks.push_back(taskJson(task, history));
			}
			const std::string me = authorOf(hooks);
			return ToolResult::ok(json{
				{ "author",    me },
				{ "openCount", b->openCount() },
				{ "openMine",  me.empty() ? 0 : b->openCountFor(me) },
				{ "tasks",     std::move(tasks) },
				{ "team",      teamJson(hooks) },
			});
		};
		registry.add(std::move(t));
	}

	// ── task_create ──────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "task_create";
		t.description =
			"Add a task to the open project's task board, as the panel's New Task "
			"does, and save Config/TaskBoard.json. It is recorded under `author` "
			"(task_list says who that is). Answers with the new task and its id.";
		t.inputSchema = objectSchema(json{
			{ "title",       stringProp("What is to be done; one line.") },
			{ "description", stringProp("Longer text; optional.") },
			{ "priority",    priorityEnumProp("Default normal.") },
			{ "status",      statusEnumProp("Default open.") },
		}, { "title" });
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			if (!boardOf(hooks)) return failNoProject();
			const std::string title = strArg(args, "title");
			if (title.empty())
				return ToolResult::fail("invalid_payload", "'title' must not be empty");
			HE::TaskPriority priority = HE::TaskPriority::Normal;
			const std::string pw = strArg(args, "priority");
			if (!pw.empty() && (!HE::taskPriorityFromName(pw, priority) || priority == HE::TaskPriority::Other))
				return ToolResult::fail("invalid_payload", "'priority' must be low, normal, high or urgent");
			HE::TaskStatus status = HE::TaskStatus::Open;
			const std::string sw = strArg(args, "status");
			if (!sw.empty() && (!HE::taskStatusFromName(sw, status) || status == HE::TaskStatus::Other))
				return ToolResult::fail("invalid_payload", "'status' must be open, in-progress, blocked or done");
			const std::string description = strArg(args, "description");

			// A retry after a pull runs this on the reloaded board, which does
			// not have the first attempt's task: adding again is right, and
			// `id` ends up the one that was saved.
			HE::UUID id;
			bool found = false;
			const auto fn = [&](HE::TaskBoard& b) {
				const std::string me = authorOf(hooks);
				const std::int64_t now = HE::taskNowMs();
				id = b.add(title, me, now)->id;
				if (!description.empty()) b.setDescription(id, description, now);
				if (priority != HE::TaskPriority::Normal) b.setPriority(id, priority, now);
				if (status != HE::TaskStatus::Open) b.setStatus(id, status, me, now);
				found = true;
				return true;
			};
			return runEdit(hooks, {}, found, fn, [&] { return taskAnswer(hooks, id); });
		};
		registry.add(std::move(t));
	}

	// ── task_update ──────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "task_update";
		t.description =
			"Post an update on a task: a new status, a comment, or both (the comment "
			"then goes with the status change, as the panel's status note does). "
			"Recorded under `author` and saved. While a team is signed in, the people "
			"the task concerns — assignees, creator, whoever assigned it, whoever "
			"wrote in its history — are notified; `team.pending` in the answer counts "
			"the notices still waiting to go.";
		t.inputSchema = objectSchema(json{
			{ "id",      stringProp("The task's id, from task_list.") },
			{ "status",  statusEnumProp("The new status.") },
			{ "comment", stringProp("A comment to post.") },
		}, { "id" });
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			if (!boardOf(hooks)) return failNoProject();
			HE::UUID id;
			ToolResult bad;
			if (!taskIdFrom(args, id, bad)) return bad;
			const std::string comment = strArg(args, "comment");
			const std::string sw = strArg(args, "status");
			HE::TaskStatus status = HE::TaskStatus::Open;
			if (!sw.empty() && (!HE::taskStatusFromName(sw, status) || status == HE::TaskStatus::Other))
				return ToolResult::fail("invalid_payload", "'status' must be open, in-progress, blocked or done");
			if (sw.empty() && comment.empty())
				return ToolResult::fail("invalid_payload", "give a 'status', a 'comment' or both");

			bool found = false;
			const auto fn = [&](HE::TaskBoard& b) {
				HE::Task* task = b.find(id);
				if (!task) return false;
				found = true;
				const std::string me = authorOf(hooks);
				const std::int64_t now = HE::taskNowMs();
				// setStatus to the status the task already has writes nothing,
				// the comment included: post it on its own then.
				if (!sw.empty() && task->status != status)
					return b.setStatus(id, status, me, now, comment);
				if (!comment.empty()) return b.comment(id, me, comment, now) != nullptr;
				return true;
			};
			return runEdit(hooks, strArg(args, "id"), found, fn, [&] { return taskAnswer(hooks, id); });
		};
		registry.add(std::move(t));
	}

	// ── task_assign ──────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "task_assign";
		t.description =
			"Assign a task to members of the team, as the panel's Assigned To field "
			"does: the list REPLACES the current assignees (an empty list unassigns). "
			"Only while a team server is signed in (task_list's `team.connected`) and "
			"only to members on its roster; without a team the board is local and "
			"this refuses with no_team. The new assignees, and whoever it was taken "
			"from, get a notice once the assignment has settled (about 1.5 s; "
			"members who are offline get it when they sign in).";
		t.inputSchema = objectSchema(json{
			{ "id",        stringProp("The task's id, from task_list.") },
			{ "assignees", json{ { "type", "array" }, { "items", json{ { "type", "string" } } },
			                     { "description", "Member names from task_list's team.members." } } },
		}, { "id", "assignees" });
		t.mutates = true;
		t.handler = [hooks](const json& args) -> ToolResult {
			if (!boardOf(hooks)) return failNoProject();
			HE::UUID id;
			ToolResult bad;
			if (!taskIdFrom(args, id, bad)) return bad;
			HE::TaskBoardTeam* team = hooks.team ? hooks.team() : nullptr;
			if (!team || !team->connected())
				return ToolResult::fail("no_team",
					"no team server is signed in — assigning is a team feature, and the "
					"panel offers no Assigned To field on a local board either");
			if (!args.contains("assignees") || !args["assignees"].is_array())
				return ToolResult::fail("invalid_payload", "'assignees' must be an array of member names");

			// The roster's spelling, each once: the file and the notices compare
			// names in any case, but a task should read as the team spells it.
			const std::vector<HE::TaskTeamMember> roster = team->members();
			std::vector<std::string> names;
			for (const json& a : args["assignees"])
			{
				const std::string want = a.is_string() ? a.get<std::string>() : std::string{};
				const auto it = std::find_if(roster.begin(), roster.end(), [&](const HE::TaskTeamMember& m) {
					return HE::sameTaskMember(m.name, want);
				});
				if (want.empty() || it == roster.end())
				{
					std::string known;
					for (const HE::TaskTeamMember& m : roster) known += (known.empty() ? "" : ", ") + m.name;
					return ToolResult::fail("unknown_member",
						"'" + want + "' is not on the team (members: " + known + ")");
				}
				if (std::find(names.begin(), names.end(), it->name) == names.end())
					names.push_back(it->name);
			}

			bool found = false;
			const auto fn = [&](HE::TaskBoard& b) {
				if (!b.find(id)) return false;
				found = true;
				return b.assign(id, names, authorOf(hooks), HE::taskNowMs());
			};
			return runEdit(hooks, strArg(args, "id"), found, fn, [&] { return taskAnswer(hooks, id); });
		};
		registry.add(std::move(t));
	}

	// ── task_notices ─────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "task_notices";
		t.description =
			"What the team told this editor about tasks, newest first — the panel's "
			"From the Team list (the bell shows the same sentences): who assigned you "
			"a task, set one to a status, commented on one. A notice only says what "
			"happened; the task itself arrives with the next pull of "
			"Config/TaskBoard.json. Empty, with `connected` false, without a team.";
		t.inputSchema = objectSchema(json::object(), {});
		t.handler = [hooks](const json&) -> ToolResult {
			HE::TaskBoardTeam* team = hooks.team ? hooks.team() : nullptr;
			json notices = json::array();
			if (team)
				for (const HE::TaskBoardTeam::Received& r : team->recent())
				{
					const HE::TaskNotice& n = r.notice;
					json j{
						{ "text",    r.text },
						{ "kind",    n.kind == HE::TaskNoticeKind::Other ? n.kindOther
						                                                 : HE::taskNoticeKindName(n.kind) },
						{ "from",    n.from },
						{ "project", n.project },
						{ "task",    HE::uuidToHex(n.task) },
						{ "title",   n.title },
						{ "time",    HE::taskTimeToIso(n.timeMs) },
					};
					if (!n.status.empty()) j["status"] = n.status;
					if (!n.text.empty())   j["comment"] = n.text;
					if (n.kind == HE::TaskNoticeKind::Assigned) j["assignees"] = n.assignees;
					notices.push_back(std::move(j));
				}
			return ToolResult::ok(json{
				{ "connected", team && team->connected() },
				{ "notices",   std::move(notices) },
			});
		};
		registry.add(std::move(t));
	}
}

} // namespace HE::Ed
