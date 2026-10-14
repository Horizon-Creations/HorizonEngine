#include "TaskBoardTeam.h"
#include "NotificationStore.h"   // the bell

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace HE
{

const char* taskNoticeKindName(TaskNoticeKind k)
{
	switch (k)
	{
		case TaskNoticeKind::Assigned: return "assigned";
		case TaskNoticeKind::Status:   return "status";
		case TaskNoticeKind::Comment:  return "comment";
		default:                       return "";
	}
}

bool sameTaskMember(std::string_view a, std::string_view b)
{
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0; i < a.size(); ++i)
		if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
	return true;
}

// ── The wire payload ─────────────────────────────────────────────────────────
std::string encodeTaskNotice(const TaskNotice& n)
{
	nlohmann::ordered_json j;
	j["type"]    = "task-notice";
	j["version"] = n.version;
	j["kind"]    = n.kind == TaskNoticeKind::Other ? n.kindOther : std::string(taskNoticeKindName(n.kind));
	j["project"] = n.project;
	j["task"]    = uuidToHex(n.task);
	j["title"]   = n.title;
	j["update"]  = uuidToHex(n.update);
	j["from"]    = n.from;
	if (!n.status.empty()) j["status"] = n.status;
	if (!n.text.empty())   j["text"]   = n.text;
	if (n.kind == TaskNoticeKind::Assigned) j["assignees"] = n.assignees;
	j["time"] = taskTimeToIso(n.timeMs);
	// A comment can hold anything a person types; a broken UTF-8 sequence must
	// not throw on the way out.
	return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool decodeTaskNotice(std::string_view payload, TaskNotice& out)
{
	const nlohmann::json j = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
	if (j.is_discarded() || !j.is_object()) return false;
	auto str = [&](const char* key) -> std::string {
		const auto it = j.find(key);
		return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
	};
	if (str("type") != "task-notice") return false;

	TaskNotice n;
	if (const auto it = j.find("version"); it != j.end() && it->is_number_integer()) n.version = it->get<int>();
	if (!uuidFromHex(str("task"), n.task) || !uuidFromHex(str("update"), n.update)) return false;

	const std::string kind = str("kind");
	if      (kind == "assigned") n.kind = TaskNoticeKind::Assigned;
	else if (kind == "status")   n.kind = TaskNoticeKind::Status;
	else if (kind == "comment")  n.kind = TaskNoticeKind::Comment;
	else { n.kind = TaskNoticeKind::Other; n.kindOther = kind; }

	n.project = str("project");
	n.title   = str("title");
	n.from    = str("from");
	n.status  = str("status");
	n.text    = str("text");
	if (const auto it = j.find("assignees"); it != j.end() && it->is_array())
		for (const auto& a : *it)
			if (a.is_string()) n.assignees.push_back(a.get<std::string>());
	taskTimeFromIso(str("time"), n.timeMs);
	out = std::move(n);
	return true;
}

// ── Who it concerns ──────────────────────────────────────────────────────────
std::vector<std::string> taskNoticeRecipients(const Task& task, const TaskUpdate& update,
                                              const std::vector<std::string>& previousAssignees,
                                              const std::string& self,
                                              const std::vector<TaskTeamMember>& team)
{
	std::vector<std::string> out;
	auto add = [&](const std::string& name) {
		if (name.empty() || sameTaskMember(name, self)) return;
		const auto m = std::find_if(team.begin(), team.end(),
		                            [&](const TaskTeamMember& t) { return sameTaskMember(t.name, name); });
		if (m == team.end()) return;   // not on the team: there is nobody to tell
		if (std::none_of(out.begin(), out.end(), [&](const std::string& o) { return sameTaskMember(o, m->name); }))
			out.push_back(m->name);
	};
	for (const std::string& a : task.assignees) add(a);
	if (update.kind == TaskUpdateKind::Assignment)
		for (const std::string& a : previousAssignees) add(a);   // it was taken from them
	add(task.createdBy);
	add(task.assignedBy);
	for (const TaskUpdate& u : task.updates) add(u.author);
	return out;
}

std::string describeTaskNotice(const TaskNotice& n, const std::string& self)
{
	const std::string who   = n.from.empty() ? std::string("Someone") : n.from;
	const std::string title = "“" + (n.title.empty() ? std::string("a task") : n.title) + "”";
	std::string s;
	switch (n.kind)
	{
		case TaskNoticeKind::Assigned:
		{
			const bool you = std::any_of(n.assignees.begin(), n.assignees.end(),
			                             [&](const std::string& a) { return sameTaskMember(a, self); });
			if (you)                      s = who + " assigned you " + title;
			else if (n.assignees.empty()) s = who + " unassigned " + title;
			else
			{
				std::string names;
				for (std::size_t i = 0; i < n.assignees.size(); ++i) names += (i ? ", " : "") + n.assignees[i];
				s = who + " assigned " + title + " to " + names;
			}
			break;
		}
		case TaskNoticeKind::Status:
		{
			TaskStatus st;
			std::string word = n.status;
			if (taskStatusFromName(n.status, st))
			{
				static const char* const kLabels[] = { "Open", "In Progress", "Blocked", "Done" };
				word = kLabels[(int)st];
			}
			s = who + " set " + title + " to " + word;
			break;
		}
		case TaskNoticeKind::Comment: s = who + " commented on " + title; break;
		default:                      s = who + " updated " + title; break;
	}
	return s;
}

// ── The bridge ───────────────────────────────────────────────────────────────
std::string TaskBoardTeam::self() const
{
	return connected() ? m_link->self() : std::string{};
}

std::vector<TaskTeamMember> TaskBoardTeam::members() const
{
	return connected() ? m_link->members() : std::vector<TaskTeamMember>{};
}

void TaskBoardTeam::afterEdit(const TaskBoard& before, const TaskBoard& after,
                              const std::string& project, std::uint64_t nowMs)
{
	if (!connected()) return;
	const std::string me = m_link->self();
	if (me.empty()) return;
	const std::vector<TaskTeamMember> team = m_link->members();

	for (const Task& t : after.tasks)
	{
		const Task* old = before.find(t.id);
		std::unordered_set<UUID> had;
		if (old) for (const TaskUpdate& u : old->updates) had.insert(u.id);
		const std::vector<std::string> previous = old ? old->assignees : std::vector<std::string>{};

		for (const TaskUpdate& u : t.updates)
		{
			if (had.count(u.id) || !sameTaskMember(u.author, me)) continue;
			TaskNotice n;
			switch (u.kind)
			{
				case TaskUpdateKind::Assignment: n.kind = TaskNoticeKind::Assigned; n.assignees = u.assignees; break;
				case TaskUpdateKind::Status:     n.kind = TaskNoticeKind::Status;   n.status = u.status; break;
				case TaskUpdateKind::Comment:    n.kind = TaskNoticeKind::Comment;  break;
				default: continue;   // a kind this build does not know is not ours to announce
			}
			n.project = project;
			n.task    = t.id;
			n.title   = t.title;
			n.update  = u.id;
			n.from    = me;
			n.text    = u.text;
			n.timeMs  = u.timeMs;
			for (const std::string& to : taskNoticeRecipients(t, u, previous, me, team))
				queue(to, n, nowMs);
		}
	}
}

void TaskBoardTeam::queue(const std::string& to, TaskNotice n, std::uint64_t nowMs)
{
	Outgoing o;
	o.to        = to;
	o.dueMs     = nowMs + (n.kind == TaskNoticeKind::Assigned ? kAssignSettleMs : 0);
	o.expiresMs = nowMs + kOutboxTtlMs;
	if (n.kind == TaskNoticeKind::Assigned)
	{
		// A newer assignment of the same task to the same person replaces one
		// still settling: the picker ticked twice, the team hears the result.
		m_outbox.erase(std::remove_if(m_outbox.begin(), m_outbox.end(), [&](const Outgoing& w) {
			return w.notice.kind == TaskNoticeKind::Assigned && w.notice.task == n.task &&
			       sameTaskMember(w.to, to) && w.dueMs > nowMs;
		}), m_outbox.end());
	}
	o.payload = encodeTaskNotice(n);
	o.notice  = std::move(n);
	m_outbox.push_back(std::move(o));
}

void TaskBoardTeam::update(std::uint64_t nowMs)
{
	// Expired first: what is past its time goes whether or not it could be sent.
	m_outbox.erase(std::remove_if(m_outbox.begin(), m_outbox.end(),
	                              [&](const Outgoing& o) { return nowMs >= o.expiresMs; }),
	               m_outbox.end());
	if (!m_link) return;

	if (m_link->connected() && !m_outbox.empty())
	{
		std::unordered_map<std::string, bool> online;
		for (const TaskTeamMember& m : m_link->members())
		{
			std::string key = m.name;
			for (char& c : key) c = (char)std::tolower((unsigned char)c);
			online[key] = m.online;
		}
		for (auto it = m_outbox.begin(); it != m_outbox.end();)
		{
			std::string key = it->to;
			for (char& c : key) c = (char)std::tolower((unsigned char)c);
			const auto on = online.find(key);
			if (on == online.end()) { it = m_outbox.erase(it); continue; }   // left the team
			if (nowMs < it->dueMs || !on->second) { ++it; continue; }       // settling, or not here yet
			if (!m_link->send(it->to, it->payload)) break;                   // the link went down under us
			it = m_outbox.erase(it);
		}
	}

	TaskTeamLink::Incoming in;
	while (m_link->poll(in)) receive(in, nowMs);
}

bool TaskBoardTeam::seen(const UUID& update)
{
	if (!m_seenSet.insert(update).second) return true;
	m_seenOrder.push_back(update);
	if (m_seenOrder.size() > kSeenMax) { m_seenSet.erase(m_seenOrder.front()); m_seenOrder.pop_front(); }
	return false;
}

void TaskBoardTeam::receive(const TaskTeamLink::Incoming& in, std::uint64_t nowMs)
{
	TaskNotice n;
	if (!decodeTaskNotice(in.payload, n)) return;
	// The server's word for who sent it beats the payload's.
	if (!in.from.empty()) n.from = in.from;
	const std::string me = m_link ? m_link->self() : std::string{};
	if (sameTaskMember(n.from, me)) return;
	// A reconnect can deliver a notice twice; a person gets it once.
	if (seen(n.update)) return;

	Received r;
	r.text   = describeTaskNotice(n, me);
	r.atMs   = nowMs;
	r.notice = std::move(n);
	if (m_notes)
	{
		std::string detail = r.notice.text;
		if (!r.notice.project.empty()) detail = (detail.empty() ? std::string{} : detail + "  ·  ") + "Project " + r.notice.project;
		m_notes->post(Ed::NoteLevel::Info, r.text, detail);
	}
	m_recent.push_front(r);
	while (m_recent.size() > kRecentMax) m_recent.pop_back();
	m_notices.push_back(std::move(r));
	while (m_notices.size() > kRecentMax) m_notices.pop_front();
}

bool TaskBoardTeam::pollNotice(Received& out)
{
	if (m_notices.empty()) return false;
	out = std::move(m_notices.front());
	m_notices.pop_front();
	return true;
}

} // namespace HE
