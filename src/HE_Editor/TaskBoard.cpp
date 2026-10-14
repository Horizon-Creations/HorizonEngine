#include "TaskBoard.h"

#include <Diagnostics/Log.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <unordered_set>

namespace HE
{

namespace
{

// ORDERED: the file's key order is part of what a load-then-save keeps. A
// std::map-backed json would sort every task's keys alphabetically and put
// the unknown ones in among them.
using ojson = nlohmann::ordered_json;

// ── Words ────────────────────────────────────────────────────────────────────

template <class E, std::size_t N>
bool fromName(std::string_view name, const char* const (&names)[N], E& out)
{
	for (std::size_t i = 0; i < N; ++i)
		if (name == names[i]) { out = static_cast<E>(i); return true; }
	return false;
}

template <class E, std::size_t N>
const char* toName(E e, const char* const (&names)[N])
{
	const auto i = static_cast<std::size_t>(e);
	return i < N ? names[i] : "";
}

const char* const kStatusNames[]     = { "open", "in-progress", "blocked", "done" };
const char* const kPriorityNames[]   = { "low", "normal", "high", "urgent" };
const char* const kLinkKindNames[]   = { "asset", "entity", "scene", "panel" };
const char* const kUpdateKindNames[] = { "comment", "status", "assignment" };

// ── Time ─────────────────────────────────────────────────────────────────────
// Civil date <-> days since 1970-01-01 (proleptic Gregorian), H. Hinnant's
// algorithms. Hand-rolled because the C library's inverse of gmtime is
// timegm() on one platform and _mkgmtime() on another, and both stop at the
// year 2038 or 3000 depending on who built them.

std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d)
{
	y -= m <= 2;
	const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(y - era * 400);
	const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civilFromDays(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d)
{
	z += 719468;
	const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const unsigned doe = static_cast<unsigned>(z - era * 146097);
	const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = static_cast<std::int64_t>(yoe) + era * 400;
	const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const unsigned mp = (5 * doy + 2) / 153;
	d = doy - (153 * mp + 2) / 5 + 1;
	m = mp < 10 ? mp + 3 : mp - 9;
	y += m <= 2;
}

bool readDigits(std::string_view s, std::size_t at, std::size_t n, int& out)
{
	if (at + n > s.size()) return false;
	int v = 0;
	for (std::size_t i = 0; i < n; ++i)
	{
		const char c = s[at + i];
		if (c < '0' || c > '9') return false;
		v = v * 10 + (c - '0');
	}
	out = v;
	return true;
}

// ── Reading ──────────────────────────────────────────────────────────────────
// Tolerant, like ProjectSettings: a known key that is missing or of the wrong
// type leaves the field at its default. Unknown keys are not dropped — they
// are collected by extrasOf() and written back by appendExtras().

void readString(const ojson& j, const char* key, std::string& out)
{
	const auto it = j.find(key);
	if (it != j.end() && it->is_string()) out = it->get<std::string>();
}

void readStrings(const ojson& j, const char* key, std::vector<std::string>& out)
{
	const auto it = j.find(key);
	if (it == j.end() || !it->is_array()) return;
	out.clear();
	for (const ojson& e : *it)
		if (e.is_string()) out.push_back(e.get<std::string>());
}

// A UUID as this file writes it (32 hex digits), or as the engine's graph
// chunks do ({"hi":…,"lo":…}) — someone pasting an id out of an asset dump
// should not have to convert it.
bool readUuid(const ojson& j, const char* key, UUID& out)
{
	const auto it = j.find(key);
	if (it == j.end()) return false;
	if (it->is_string()) return uuidFromHex(it->get<std::string>(), out);
	if (it->is_object())
	{
		const auto hi = it->find("hi"), lo = it->find("lo");
		if (hi != it->end() && lo != it->end() &&
		    hi->is_number_unsigned() && lo->is_number_unsigned())
		{
			out.hi = hi->get<std::uint64_t>();
			out.lo = lo->get<std::uint64_t>();
			return true;
		}
	}
	return false;
}

// ISO 8601 as written, or a bare number of milliseconds (a hand-made file).
void readTime(const ojson& j, const char* key, std::int64_t& out)
{
	const auto it = j.find(key);
	if (it == j.end()) return;
	if (it->is_string())
	{
		std::int64_t ms = 0;
		if (taskTimeFromIso(it->get<std::string>(), ms)) out = ms;
	}
	else if (it->is_number_integer())
		out = it->get<std::int64_t>();
}

std::string extrasOf(const ojson& obj, std::initializer_list<std::string_view> known)
{
	ojson extra = ojson::object();
	for (auto it = obj.begin(); it != obj.end(); ++it)
	{
		const std::string& k = it.key();
		if (std::find(known.begin(), known.end(), k) == known.end())
			extra[k] = it.value();
	}
	return extra.empty() ? std::string{} : extra.dump();
}

// After the known keys, in the order the file had them. A key this build
// writes itself wins over a same-named one somebody put into extraJson by hand.
void appendExtras(ojson& out, const std::string& extraJson)
{
	if (extraJson.empty()) return;
	const ojson extra = ojson::parse(extraJson, nullptr, /*allow_exceptions=*/false);
	if (!extra.is_object()) return;
	for (auto it = extra.begin(); it != extra.end(); ++it)
		if (!out.contains(it.key())) out[it.key()] = it.value();
}

std::string dumpDocument(const ojson& j)
{
	// nlohmann writes '\n' on every platform; the stream that receives this is
	// opened binary, so it stays LF on Windows too. No ASCII escaping: a task
	// title in Japanese reads as Japanese in the diff.
	return j.dump(4, ' ', /*ensure_ascii=*/false, ojson::error_handler_t::replace) + "\n";
}

// ── Links ────────────────────────────────────────────────────────────────────

TaskLink readLink(const ojson& j)
{
	TaskLink l;
	std::string kind;
	readString(j, "kind", kind);
	readString(j, "label", l.label);
	if (!taskLinkKindFromName(kind, l.kind))
	{
		// A kind from a newer editor: keep the word, keep every other key as it
		// was. Nothing in it means anything to this build.
		l.kind = TaskLinkKind::Other;
		l.kindOther = kind;
		l.extraJson = extrasOf(j, { "kind", "label" });
		return l;
	}
	switch (l.kind)
	{
	case TaskLinkKind::Asset:
		readUuid(j, "asset", l.id);
		l.extraJson = extrasOf(j, { "kind", "asset", "label" });
		break;
	case TaskLinkKind::Entity:
		readUuid(j, "entity", l.id);
		readString(j, "scene", l.scene);
		l.extraJson = extrasOf(j, { "kind", "entity", "scene", "label" });
		break;
	case TaskLinkKind::Scene:
		readString(j, "scene", l.scene);
		l.extraJson = extrasOf(j, { "kind", "scene", "label" });
		break;
	case TaskLinkKind::Panel:
		readString(j, "panel", l.panel);
		l.extraJson = extrasOf(j, { "kind", "panel", "label" });
		break;
	case TaskLinkKind::Other:
		break;
	}
	return l;
}

ojson writeLink(const TaskLink& l)
{
	ojson j = ojson::object();
	j["kind"] = l.kind == TaskLinkKind::Other ? l.kindOther : taskLinkKindName(l.kind);
	switch (l.kind)
	{
	case TaskLinkKind::Asset:  j["asset"] = uuidToHex(l.id); break;
	case TaskLinkKind::Entity: j["entity"] = uuidToHex(l.id); j["scene"] = l.scene; break;
	case TaskLinkKind::Scene:  j["scene"] = l.scene; break;
	case TaskLinkKind::Panel:  j["panel"] = l.panel; break;
	case TaskLinkKind::Other:  break;
	}
	if (!l.label.empty()) j["label"] = l.label;
	appendExtras(j, l.extraJson);
	return j;
}

// ── Updates ──────────────────────────────────────────────────────────────────

TaskUpdate readUpdate(const ojson& j)
{
	TaskUpdate u;
	if (!readUuid(j, "id", u.id)) u.id = UUID::generate();
	std::string kind;
	readString(j, "kind", kind);
	if (!taskUpdateKindFromName(kind, u.kind))
	{
		u.kind = TaskUpdateKind::Other;
		u.kindOther = kind;
	}
	readString(j, "author", u.author);
	readTime(j, "time", u.timeMs);
	readString(j, "text", u.text);
	switch (u.kind)
	{
	case TaskUpdateKind::Status:
		readString(j, "status", u.status);
		u.extraJson = extrasOf(j, { "id", "kind", "author", "time", "text", "status" });
		break;
	case TaskUpdateKind::Assignment:
		readStrings(j, "assignees", u.assignees);
		u.extraJson = extrasOf(j, { "id", "kind", "author", "time", "text", "assignees" });
		break;
	default:
		u.extraJson = extrasOf(j, { "id", "kind", "author", "time", "text" });
		break;
	}
	return u;
}

ojson writeUpdate(const TaskUpdate& u)
{
	ojson j = ojson::object();
	j["id"]     = uuidToHex(u.id);
	j["kind"]   = u.kind == TaskUpdateKind::Other ? u.kindOther : taskUpdateKindName(u.kind);
	j["author"] = u.author;
	j["time"]   = taskTimeToIso(u.timeMs);
	if (!u.text.empty()) j["text"] = u.text;
	if (u.kind == TaskUpdateKind::Status) j["status"] = u.status;
	if (u.kind == TaskUpdateKind::Assignment) j["assignees"] = u.assignees;
	appendExtras(j, u.extraJson);
	return j;
}

// ── Tasks ────────────────────────────────────────────────────────────────────

Task readTask(const ojson& j, int& dropped)
{
	Task t;
	readUuid(j, "id", t.id);
	readString(j, "title", t.title);
	readString(j, "description", t.description);

	std::string word;
	readString(j, "status", word);
	if (!word.empty() && !taskStatusFromName(word, t.status))
	{
		t.status = TaskStatus::Other;
		t.statusOther = word;
	}
	word.clear();
	readString(j, "priority", word);
	if (!word.empty() && !taskPriorityFromName(word, t.priority))
	{
		t.priority = TaskPriority::Other;
		t.priorityOther = word;
	}

	readString(j, "createdBy", t.createdBy);
	readString(j, "assignedBy", t.assignedBy);
	readStrings(j, "assignees", t.assignees);
	readTime(j, "created", t.createdMs);
	readTime(j, "modified", t.modifiedMs);

	if (const auto it = j.find("links"); it != j.end() && it->is_array())
		for (const ojson& e : *it)
		{
			if (e.is_object()) t.links.push_back(readLink(e));
			else               ++dropped;
		}
	if (const auto it = j.find("updates"); it != j.end() && it->is_array())
		for (const ojson& e : *it)
		{
			if (e.is_object()) t.updates.push_back(readUpdate(e));
			else               ++dropped;
		}

	t.extraJson = extrasOf(j, { "id", "title", "description", "status", "priority",
	                            "createdBy", "assignedBy", "assignees", "created",
	                            "modified", "links", "updates" });
	return t;
}

ojson writeTask(const Task& t)
{
	ojson j = ojson::object();
	j["id"]          = uuidToHex(t.id);
	j["title"]       = t.title;
	j["description"] = t.description;
	j["status"]      = t.status == TaskStatus::Other ? t.statusOther : taskStatusName(t.status);
	j["priority"]    = t.priority == TaskPriority::Other ? t.priorityOther : taskPriorityName(t.priority);
	j["createdBy"]   = t.createdBy;
	j["assignedBy"]  = t.assignedBy;
	j["assignees"]   = t.assignees;
	j["created"]     = taskTimeToIso(t.createdMs);
	j["modified"]    = taskTimeToIso(t.modifiedMs);
	ojson links = ojson::array();
	for (const TaskLink& l : t.links) links.push_back(writeLink(l));
	j["links"] = std::move(links);
	ojson updates = ojson::array();
	for (const TaskUpdate& u : t.updates) updates.push_back(writeUpdate(u));
	j["updates"] = std::move(updates);
	appendExtras(j, t.extraJson);
	return j;
}

bool readFile(const std::filesystem::path& path, std::string& out)
{
	std::ifstream in(path, std::ios::binary);
	if (!in.is_open()) return false;
	out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return !in.bad();
}

} // namespace

// ── Words ────────────────────────────────────────────────────────────────────

const char* taskStatusName(TaskStatus s)         { return toName(s, kStatusNames); }
const char* taskPriorityName(TaskPriority p)     { return toName(p, kPriorityNames); }
const char* taskLinkKindName(TaskLinkKind k)     { return toName(k, kLinkKindNames); }
const char* taskUpdateKindName(TaskUpdateKind k) { return toName(k, kUpdateKindNames); }

bool taskStatusFromName(std::string_view n, TaskStatus& out)         { return fromName(n, kStatusNames, out); }
bool taskPriorityFromName(std::string_view n, TaskPriority& out)     { return fromName(n, kPriorityNames, out); }
bool taskLinkKindFromName(std::string_view n, TaskLinkKind& out)     { return fromName(n, kLinkKindNames, out); }
bool taskUpdateKindFromName(std::string_view n, TaskUpdateKind& out) { return fromName(n, kUpdateKindNames, out); }

// ── UUID ─────────────────────────────────────────────────────────────────────

std::string uuidToHex(const UUID& id)
{
	char buf[33];
	std::snprintf(buf, sizeof(buf), "%016llx%016llx",
	              static_cast<unsigned long long>(id.hi),
	              static_cast<unsigned long long>(id.lo));
	return std::string(buf, 32);
}

bool uuidFromHex(std::string_view text, UUID& out)
{
	if (text.size() != 32) return false;
	std::uint64_t parts[2] = { 0, 0 };
	for (std::size_t i = 0; i < 32; ++i)
	{
		const char c = text[i];
		unsigned v;
		if (c >= '0' && c <= '9')      v = static_cast<unsigned>(c - '0');
		else if (c >= 'a' && c <= 'f') v = static_cast<unsigned>(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') v = static_cast<unsigned>(c - 'A' + 10);
		else return false;
		parts[i / 16] = (parts[i / 16] << 4) | v;
	}
	out.hi = parts[0];
	out.lo = parts[1];
	return true;
}

// ── Time ─────────────────────────────────────────────────────────────────────

std::int64_t taskNowMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string taskTimeToIso(std::int64_t ms)
{
	std::int64_t days = ms / 86400000;
	std::int64_t rem  = ms % 86400000;
	if (rem < 0) { rem += 86400000; --days; }
	std::int64_t y; unsigned m, d;
	civilFromDays(days, y, m, d);
	const int msec = static_cast<int>(rem % 1000);
	std::int64_t sec = rem / 1000;
	char buf[48];
	std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02d:%02d:%02d.%03dZ",
	              static_cast<long long>(y), m, d,
	              static_cast<int>(sec / 3600), static_cast<int>(sec / 60 % 60),
	              static_cast<int>(sec % 60), msec);
	return buf;
}

bool taskTimeFromIso(std::string_view s, std::int64_t& outMs)
{
	// YYYY-MM-DDTHH:MM:SS[.mmm]Z
	int y, mo, d, h, mi, se, ms = 0;
	if (s.size() < 20) return false;
	if (!readDigits(s, 0, 4, y) || s[4] != '-' || !readDigits(s, 5, 2, mo) || s[7] != '-' ||
	    !readDigits(s, 8, 2, d) || s[10] != 'T' || !readDigits(s, 11, 2, h) || s[13] != ':' ||
	    !readDigits(s, 14, 2, mi) || s[16] != ':' || !readDigits(s, 17, 2, se))
		return false;
	std::size_t at = 19;
	if (s[at] == '.')
	{
		if (!readDigits(s, at + 1, 3, ms)) return false;
		at += 4;
	}
	if (at + 1 != s.size() || s[at] != 'Z') return false;
	if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return false;
	const std::int64_t days = daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
	outMs = ((days * 24 + h) * 60 + mi) * 60000 + static_cast<std::int64_t>(se) * 1000 + ms;
	return true;
}

// ── TaskLink / Task ──────────────────────────────────────────────────────────

TaskLink TaskLink::asset(const UUID& assetId, std::string label)
{
	TaskLink l;
	l.kind  = TaskLinkKind::Asset;
	l.id    = assetId;
	l.label = std::move(label);
	return l;
}

TaskLink TaskLink::entity(const UUID& entityId, std::string scenePath, std::string label)
{
	TaskLink l;
	l.kind  = TaskLinkKind::Entity;
	l.id    = entityId;
	l.scene = std::move(scenePath);
	l.label = std::move(label);
	return l;
}

TaskLink TaskLink::sceneLink(std::string scenePath, std::string label)
{
	TaskLink l;
	l.kind  = TaskLinkKind::Scene;
	l.scene = std::move(scenePath);
	l.label = std::move(label);
	return l;
}

TaskLink TaskLink::panelLink(std::string windowName, std::string label)
{
	TaskLink l;
	l.kind  = TaskLinkKind::Panel;
	l.panel = std::move(windowName);
	l.label = std::move(label);
	return l;
}

bool TaskLink::sameTarget(const TaskLink& o) const
{
	if (kind != o.kind) return false;
	switch (kind)
	{
	case TaskLinkKind::Asset:  return id == o.id;
	case TaskLinkKind::Entity: return id == o.id && scene == o.scene;
	case TaskLinkKind::Scene:  return scene == o.scene;
	case TaskLinkKind::Panel:  return panel == o.panel;
	case TaskLinkKind::Other:  return kindOther == o.kindOther && extraJson == o.extraJson;
	}
	return false;
}

bool Task::isAssignedTo(std::string_view member) const
{
	return std::find(assignees.begin(), assignees.end(), member) != assignees.end();
}

int Task::orphanedLinkCount() const
{
	return static_cast<int>(std::count_if(links.begin(), links.end(),
	                                      [](const TaskLink& l) { return l.orphaned; }));
}

// ── TaskBoard: editing ───────────────────────────────────────────────────────

Task* TaskBoard::add(std::string title, std::string author, std::int64_t nowMs)
{
	Task t;
	t.id         = UUID::generate();
	t.title      = std::move(title);
	t.createdBy  = std::move(author);
	t.createdMs  = nowMs;
	t.modifiedMs = nowMs;
	tasks.push_back(std::move(t));
	return &tasks.back();
}

bool TaskBoard::remove(const UUID& id)
{
	const auto it = std::find_if(tasks.begin(), tasks.end(),
	                             [&](const Task& t) { return t.id == id; });
	if (it == tasks.end()) return false;
	tasks.erase(it);
	return true;
}

Task* TaskBoard::find(const UUID& id)
{
	for (Task& t : tasks)
		if (t.id == id) return &t;
	return nullptr;
}

const Task* TaskBoard::find(const UUID& id) const
{
	return const_cast<TaskBoard*>(this)->find(id);
}

bool TaskBoard::setTitle(const UUID& id, std::string title, std::int64_t nowMs)
{
	Task* t = find(id);
	if (!t) return false;
	t->title      = std::move(title);
	t->modifiedMs = nowMs;
	return true;
}

bool TaskBoard::setDescription(const UUID& id, std::string text, std::int64_t nowMs)
{
	Task* t = find(id);
	if (!t) return false;
	t->description = std::move(text);
	t->modifiedMs  = nowMs;
	return true;
}

bool TaskBoard::setPriority(const UUID& id, TaskPriority p, std::int64_t nowMs)
{
	Task* t = find(id);
	if (!t || p == TaskPriority::Other) return false;
	t->priority = p;
	t->priorityOther.clear();
	t->modifiedMs = nowMs;
	return true;
}

bool TaskBoard::setStatus(const UUID& id, TaskStatus s, const std::string& author,
                          std::int64_t nowMs, std::string comment)
{
	Task* t = find(id);
	if (!t || s == TaskStatus::Other) return false;
	if (t->status == s) return true;
	t->status = s;
	t->statusOther.clear();
	t->modifiedMs = nowMs;

	TaskUpdate u;
	u.id     = UUID::generate();
	u.kind   = TaskUpdateKind::Status;
	u.author = author;
	u.timeMs = nowMs;
	u.text   = std::move(comment);
	u.status = taskStatusName(s);
	t->updates.push_back(std::move(u));
	return true;
}

bool TaskBoard::assign(const UUID& id, std::vector<std::string> assignees,
                       const std::string& author, std::int64_t nowMs)
{
	Task* t = find(id);
	if (!t) return false;

	std::vector<std::string> clean;
	for (std::string& a : assignees)
		if (!a.empty() && std::find(clean.begin(), clean.end(), a) == clean.end())
			clean.push_back(std::move(a));

	t->assignees  = clean;
	t->assignedBy = author;
	t->modifiedMs = nowMs;

	TaskUpdate u;
	u.id        = UUID::generate();
	u.kind      = TaskUpdateKind::Assignment;
	u.author    = author;
	u.timeMs    = nowMs;
	u.assignees = std::move(clean);
	t->updates.push_back(std::move(u));
	return true;
}

TaskUpdate* TaskBoard::comment(const UUID& id, const std::string& author, std::string text,
                               std::int64_t nowMs)
{
	Task* t = find(id);
	if (!t) return nullptr;
	t->modifiedMs = nowMs;

	TaskUpdate u;
	u.id     = UUID::generate();
	u.kind   = TaskUpdateKind::Comment;
	u.author = author;
	u.timeMs = nowMs;
	u.text   = std::move(text);
	t->updates.push_back(std::move(u));
	return &t->updates.back();
}

bool TaskBoard::addLink(const UUID& id, TaskLink link, std::int64_t nowMs)
{
	Task* t = find(id);
	if (!t) return false;
	for (const TaskLink& l : t->links)
		if (l.sameTarget(link)) return false;
	link.orphaned = false;
	t->links.push_back(std::move(link));
	t->modifiedMs = nowMs;
	return true;
}

bool TaskBoard::removeLink(const UUID& id, std::size_t index, std::int64_t nowMs)
{
	Task* t = find(id);
	if (!t || index >= t->links.size()) return false;
	t->links.erase(t->links.begin() + static_cast<std::ptrdiff_t>(index));
	t->modifiedMs = nowMs;
	return true;
}

int TaskBoard::openCount() const
{
	return static_cast<int>(std::count_if(tasks.begin(), tasks.end(),
	                                      [](const Task& t) { return !t.isDone(); }));
}

int TaskBoard::openCountFor(std::string_view member) const
{
	return static_cast<int>(std::count_if(tasks.begin(), tasks.end(), [&](const Task& t) {
		return !t.isDone() && t.isAssignedTo(member);
	}));
}

// ── TaskBoard: orphans ───────────────────────────────────────────────────────

int TaskBoard::refreshOrphans(const LinkResolver& resolve)
{
	int orphaned = 0;
	for (Task& t : tasks)
		for (TaskLink& l : t.links)
		{
			// An address that names nothing cannot be followed whatever the
			// resolver would say — a hand-edited file, or a kind from a newer
			// editor.
			bool blank = false;
			switch (l.kind)
			{
			case TaskLinkKind::Asset:  blank = l.id == UUID{}; break;
			case TaskLinkKind::Entity: blank = l.id == UUID{}; break;
			case TaskLinkKind::Scene:  blank = l.scene.empty(); break;
			case TaskLinkKind::Panel:  blank = l.panel.empty(); break;
			case TaskLinkKind::Other:  blank = true; break;
			}
			if (blank)                l.orphaned = true;
			else if (!resolve)        l.orphaned = false;
			else                      l.orphaned = resolve(l) == LinkCheck::Missing;
			if (l.orphaned) ++orphaned;
		}
	return orphaned;
}

// ── TaskBoard: the document ──────────────────────────────────────────────────

TaskBoard::LoadResult TaskBoard::fromJson(std::string_view text)
{
	*this = TaskBoard{};
	LoadResult r;

	const ojson j = ojson::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
	if (j.is_discarded())
	{
		r.error = text.find("<<<<<<<") != std::string_view::npos
		        ? "the file has unresolved merge-conflict markers"
		        : "the file is not valid JSON";
		return r;
	}
	if (!j.is_object())
	{
		r.error = "the file is not a JSON object";
		return r;
	}

	int version = 1;   // the key's absence is the first layout
	if (const auto it = j.find("version"); it != j.end())
	{
		if (!it->is_number_integer() || it->get<std::int64_t>() < 1 ||
		    it->get<std::int64_t>() > 1000000)
		{
			r.error = "\"version\" is not a positive whole number";
			return r;
		}
		version = static_cast<int>(it->get<std::int64_t>());
	}

	const auto list = j.find("tasks");
	if (list != j.end() && !list->is_array())
	{
		// Not a list we could add to without losing what is there.
		r.error = "\"tasks\" is not a list";
		return r;
	}

	if (list != j.end())
	{
		std::unordered_set<UUID> seen;
		for (const ojson& e : *list)
		{
			if (!e.is_object())
			{
				m_unreadableTasks.push_back(e.dump());
				continue;
			}
			Task t = readTask(e, r.dropped);
			// A task needs an id the board can find it by. One without, or a
			// copy-pasted duplicate, gets a fresh one rather than shadowing
			// (or being shadowed by) the other.
			if (t.id == UUID{} || !seen.insert(t.id).second)
			{
				t.id = UUID::generate();
				seen.insert(t.id);
			}
			tasks.push_back(std::move(t));
		}
	}

	m_fileVersion = version;
	m_extraJson   = extrasOf(j, { "version", "tasks" });
	r.ok      = true;
	r.version = version;
	return r;
}

std::string TaskBoard::toJson() const
{
	ojson j = ojson::object();
	j["version"] = writeVersion();
	ojson list = ojson::array();
	for (const Task& t : tasks) list.push_back(writeTask(t));
	for (const std::string& raw : m_unreadableTasks)
	{
		ojson e = ojson::parse(raw, nullptr, /*allow_exceptions=*/false);
		if (!e.is_discarded()) list.push_back(std::move(e));
	}
	j["tasks"] = std::move(list);
	appendExtras(j, m_extraJson);
	return dumpDocument(j);
}

// ── The file ─────────────────────────────────────────────────────────────────

std::filesystem::path taskBoardPath(const std::filesystem::path& projectRoot)
{
	return projectRoot / "Config" / "TaskBoard.json";
}

TaskBoard::LoadResult loadTaskBoard(const std::filesystem::path& projectRoot, TaskBoard& out)
{
	const std::filesystem::path path = taskBoardPath(projectRoot);
	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
	{
		out = TaskBoard{};
		TaskBoard::LoadResult r;
		r.ok      = true;
		r.version = TaskBoard::kVersion;
		return r;
	}

	std::string text;
	if (!readFile(path, text))
	{
		out = TaskBoard{};
		TaskBoard::LoadResult r;
		r.error = "cannot read " + path.string();
		HE_LOG_ERROR(Editor, "Task board: %s", r.error.c_str());
		return r;
	}
	TaskBoard::LoadResult r = out.fromJson(text);
	if (!r.ok)
		HE_LOG_WARN(Editor, "Task board: '%s' not loaded — %s. It is left untouched.",
		            path.string().c_str(), r.error.c_str());
	else if (r.dropped > 0)
		HE_LOG_WARN(Editor, "Task board: '%s' — %d malformed link/update entr%s skipped",
		            path.string().c_str(), r.dropped, r.dropped == 1 ? "y" : "ies");
	return r;
}

bool saveTaskBoard(const std::filesystem::path& projectRoot, const TaskBoard& board,
                   std::string* error)
{
	const std::filesystem::path path = taskBoardPath(projectRoot);
	auto fail = [&](std::string why) {
		if (error) *error = why;
		return false;
	};

	std::error_code ec;
	const bool exists = std::filesystem::exists(path, ec);
	if (!exists && board.empty())
		return true;   // no tasks, and no file to say so in

	const std::string text = board.toJson();
	if (exists)
	{
		std::string onDisk;
		if (!readFile(path, onDisk))
			return fail("cannot read " + path.string());
		if (onDisk == text)
			return true;   // nothing changed — leave the file and its mtime alone
		TaskBoard probe;
		const TaskBoard::LoadResult r = probe.fromJson(onDisk);
		if (!r.ok)
		{
			HE_LOG_WARN(Editor, "Task board: not saving over '%s' — %s",
			            path.string().c_str(), r.error.c_str());
			return fail(r.error + " — fix or remove " + path.string() + " first");
		}
	}

	std::filesystem::create_directories(path.parent_path(), ec);

	// Temp + rename, as ProjectSettings and the .heproj do. Binary, so the LF
	// line ends stay LF on Windows.
	const std::filesystem::path tmp = path.string() + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out.is_open())
			return fail("cannot create " + tmp.string());
		out.write(text.data(), static_cast<std::streamsize>(text.size()));
		out.flush();
		if (!out.good())
		{
			out.close();
			std::filesystem::remove(tmp, ec);
			return fail("write to " + tmp.string() + " failed — the previous file is intact");
		}
	}
	std::filesystem::rename(tmp, path, ec);
	if (ec)
	{
		std::error_code ec2;
		std::filesystem::remove(tmp, ec2);
		HE_LOG_ERROR(Editor, "Task board: could not replace '%s' (%s)",
		             path.string().c_str(), ec.message().c_str());
		return fail("could not replace " + path.string() + " (" + ec.message() + ")");
	}
	return true;
}

} // namespace HE
