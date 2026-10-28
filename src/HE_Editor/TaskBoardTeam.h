#pragma once
#include "TaskBoard.h"

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// ── The task board's team half ───────────────────────────────────────────────
// With a team server signed in, a task can be assigned to members of the team,
// and what happens to it — the assignment, a status change, a comment — reaches
// the people it concerns as a notice: the bell, and the panel's "From the Team"
// list. Without one the board is the same board, only local, and the panel does
// not offer an assignee field at all.
//
// ── A notice is not a sync ────────────────────────────────────────────────────
// The task itself travels with source control (Config/TaskBoard.json, see
// TaskBoard.h). A notice only SAYS what happened; it never changes the board of
// the editor that receives it. If both ends wrote the same assignment into their
// files, each would give it its own update id and the next merge would conflict
// on exactly the lines git is supposed to merge cleanly. The receiver sees the
// change itself with its next pull.
//
// ── Where the messages go ─────────────────────────────────────────────────────
// Through a TaskTeamLink: the few things this needs from a team connection,
// in its own terms, so that this file knows nothing about sockets and the tests
// drive it with a fake. The editor's link sits on TeamController (the team
// server client); the server carries a notice to every editor a member is
// signed in with (docs/taskboard-team-interop-2026-10-15.md, "Wire").
//
// ── What gets posted ──────────────────────────────────────────────────────────
// TaskBoardPanel::edit() is the one writer of the board (panel, footer list,
// MCP tools), and after every saved edit it hands this class the board before
// and after. Every TaskUpdate that is new and was written under our own member
// name becomes a notice — nothing else does: an update that arrived with a pull
// is somebody else's to announce, and opening a board full of history announces
// nothing. Who gets it: taskNoticeRecipients().
//
// ── Time ──────────────────────────────────────────────────────────────────────
// A notice waits in the outbox until it can go:
//   * an assignment settles for kAssignSettleMs first — ticking three names in
//     the picker is three edits, and the team should hear one;
//   * a recipient who is not online is held until they are (a notice reaches
//     the editors that are signed in, it is not mail), and so is everyone while
//     the link is down;
//   * after kOutboxTtlMs it is dropped: by then the change is in the file, and
//     a notice about yesterday's comment is noise.
// update() takes the clock as an argument (steady milliseconds); the editor
// passes NotificationStore::nowMs(), the tests a fake that only moves forward.

namespace HE
{
namespace Ed { class NotificationStore; }

struct TaskTeamMember
{
	std::string name;           // the server's spelling
	bool        online = false; // signed in with at least one editor
};

class TaskTeamLink
{
public:
	virtual ~TaskTeamLink() = default;

	// Signed in to a team server that carries task notices. While false the
	// board is local: no assignee field, nothing sent (the outbox waits).
	virtual bool connected() const = 0;
	// Our member name, in the server's spelling.
	virtual std::string self() const = 0;
	// The team, ourselves included.
	virtual std::vector<TaskTeamMember> members() const = 0;
	// Hand `payload` to the server for every editor `member` is signed in
	// with. False when it cannot go now (link down): kept and tried again.
	virtual bool send(const std::string& member, const std::string& payload) = 0;

	struct Incoming
	{
		std::string from;
		std::string payload;
	};
	// The next notice somebody sent us, or false.
	virtual bool poll(Incoming& out) = 0;
};

enum class TaskNoticeKind : std::uint8_t { Assigned, Status, Comment, Other };
const char* taskNoticeKindName(TaskNoticeKind k);   // "assigned", "status", "comment"

// What travels: enough to say what happened in a sentence and to find the task
// again on the receiver's board, never the task itself.
struct TaskNotice
{
	int            version = 1;
	TaskNoticeKind kind = TaskNoticeKind::Comment;
	std::string    kindOther;       // the word, when kind == Other
	std::string    project;         // the project's name: a team works on more than one
	UUID           task;
	std::string    title;           // the task's title when it was sent
	UUID           update;          // the TaskUpdate it announces (duplicates drop on it)
	std::string    from;            // who did it
	std::string    status;          // Status: the new status word
	std::string    text;            // Comment: the comment; Status: its note
	std::vector<std::string> assignees;   // Assigned: who it is assigned to now
	std::int64_t   timeMs = 0;      // wall clock UTC, as on the board
};

// JSON, one object. decode is false for anything that is not a task notice (a
// newer kind decodes as Other with its word; a newer version decodes with what
// this build understands).
std::string encodeTaskNotice(const TaskNotice& n);
bool        decodeTaskNotice(std::string_view payload, TaskNotice& out);

// Member names compare in any case, as on the team server.
bool sameTaskMember(std::string_view a, std::string_view b);

// Who an update on `task` concerns: its assignees, whoever created it, whoever
// assigned it, everyone who wrote in its history, and for an assignment also
// whoever it was taken from (`previousAssignees`). Not `self`. Only names on
// the team (`team`), in the team's spelling, each once — a name from before the
// team server (the OS user name a task was created under) reaches nobody.
std::vector<std::string> taskNoticeRecipients(const Task& task, const TaskUpdate& update,
                                              const std::vector<std::string>& previousAssignees,
                                              const std::string& self,
                                              const std::vector<TaskTeamMember>& team);

// One sentence for the bell, from `self`'s side: "Anna assigned you “Crates”",
// "Bob set “Crates” to Done", "Bob commented on “Crates”".
std::string describeTaskNotice(const TaskNotice& n, const std::string& self);

class TaskBoardTeam
{
public:
	static constexpr std::uint64_t kAssignSettleMs = 1'500;
	static constexpr std::uint64_t kOutboxTtlMs    = 15 * 60'000;
	static constexpr std::size_t   kRecentMax      = 20;   // the panel's list
	static constexpr std::size_t   kSeenMax        = 512;  // update ids remembered against duplicates

	// Null (the default): the board is local.
	void          setLink(TaskTeamLink* link) { m_link = link; }
	TaskTeamLink* link() const { return m_link; }
	// Received notices also go here (the bell). Null: only recent()/pollNotice().
	void setNotifications(Ed::NotificationStore* store) { m_notes = store; }

	// A link that is signed in: the panel offers the assignee field.
	bool connected() const { return m_link && m_link->connected(); }
	// Our member name while connected, else empty.
	std::string self() const;
	std::vector<TaskTeamMember> members() const;

	// After a saved edit of the board of project `project`: queue a notice for
	// every new update written by self(). Does nothing while not connected — a
	// change made locally is not announced later as if it were news.
	void afterEdit(const TaskBoard& before, const TaskBoard& after,
	               const std::string& project, std::uint64_t nowMs);

	// Send what is due, drop what expired, read what came in.
	void update(std::uint64_t nowMs);

	struct Received
	{
		TaskNotice    notice;
		std::string   text;          // describeTaskNotice
		std::uint64_t atMs = 0;      // steady clock when it arrived
	};
	// Newest first, at most kRecentMax.
	const std::deque<Received>& recent() const { return m_recent; }
	bool pollNotice(Received& out);
	void clearRecent() { m_recent.clear(); }

	// Waiting in the outbox (one per recipient and notice), for tests and the panel.
	std::size_t pending() const { return m_outbox.size(); }

private:
	struct Outgoing
	{
		std::string   to;
		TaskNotice    notice;
		std::string   payload;
		std::uint64_t dueMs = 0;     // not before (the assignment settle)
		std::uint64_t expiresMs = 0;
	};
	void queue(const std::string& to, TaskNotice n, std::uint64_t nowMs);
	void receive(const TaskTeamLink::Incoming& in, std::uint64_t nowMs);
	bool seen(const UUID& update);

	TaskTeamLink*           m_link  = nullptr;
	Ed::NotificationStore*  m_notes = nullptr;
	std::vector<Outgoing>   m_outbox;
	std::deque<Received>    m_recent;
	std::deque<Received>    m_notices;   // for pollNotice
	std::unordered_set<UUID> m_seenSet;
	std::deque<UUID>        m_seenOrder;
};

} // namespace HE
