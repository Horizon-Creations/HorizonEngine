#pragma once
#include <Types/UUID.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// ── The task board: a project's to-do list ───────────────────────────────────
// What still has to be done in THIS project, by whom, and about what. A task
// is a sentence with a status, and it points at the things it is about: an
// asset, an entity in a scene, a scene, an editor panel. The board panel draws
// those links as chips and jumps to them; the footer widget counts what is
// open; with a team server the assignment and every update reach the people it
// concerns. This file is only the data and its file — no ImGui, no network —
// so the panel, the footer, the MCP tools and the team bridge all edit the same
// thing and the tests can drive it without an editor.
//
// ── Where it lives, and why there ─────────────────────────────────────────────
// <ProjectRoot>/Config/TaskBoard.json (taskBoardPath()). Config/ is created for
// every new project and is NOT in the .gitignore the engine writes, so the
// board travels with source control the way ProjectSettings.json does: the
// whole team sees the same list, and a branch carries the tasks it was opened
// for. Not under Saved/ (ignored), not under Content/ (the content browser and
// the packer walk that; a to-do list must never ship inside a game).
//
// ── The bargain with git: lossless, stable, mergeable ─────────────────────────
//   * Every id is a random UUID (tasks AND updates), so two people adding tasks
//     on two branches never collide when the branches merge.
//   * Times are wall-clock UTC (ISO 8601, milliseconds), not a steady clock —
//     a steady clock means nothing on the next person's machine.
//   * Enums are words ("in-progress", "high"), not numbers, so a diff reads.
//   * Load-then-save is byte-identical: order is file order, nothing is
//     re-stamped on load, LF line ends on every platform, one trailing newline.
//   * Anything this build does not know is KEPT: unknown keys on the file, a
//     task, a link or an update are written back where they were; an unknown
//     status, priority, link kind or update kind is kept as the word it was. A
//     teammate on a newer editor must not lose a field because someone on an
//     older one saved the board. A non-object entry in the task list is kept
//     verbatim too. (A non-object entry INSIDE a task's links or updates is the
//     one thing dropped — counted in LoadResult::dropped.)
//   * A file that does not parse (merge-conflict markers, a truncated write) is
//     never overwritten: saveTaskBoard() refuses until a human has fixed it.
//     Nor is one that changed under the open board (a pull): that is a reload.
//
// ── Versions ──────────────────────────────────────────────────────────────────
// `version` is the layout's version, 1 today. A file without the key is read
// as version 1 — the layout before the key existed would be the same shape. A
// file from a NEWER engine loads with everything this one understands, keeps
// the rest, and is written back under ITS version, not ours: writing "1" over
// a version-2 file would tell the newer editor its own data was old.
//
// ── Orphaned links ────────────────────────────────────────────────────────────
// A link is an address, not a pointer: the asset may have been deleted, the
// entity's scene renamed, the panel removed in a later editor. Nothing here
// dereferences a link. refreshOrphans() asks a resolver the caller supplies
// and marks what is gone as orphaned — runtime state, never written to the
// file, never a reason to delete the link (the target may come back with the
// next pull). An orphaned link still carries its `label`, the name the target
// had when it was linked, so the chip can say what is missing.

namespace HE
{

// Words in the file, one per value. `Other` means "a word this build does not
// know"; the word itself is kept next to the field (statusOther, …) and
// written back unchanged.
enum class TaskStatus : std::uint8_t { Open, InProgress, Blocked, Done, Other };
enum class TaskPriority : std::uint8_t { Low, Normal, High, Urgent, Other };
enum class TaskLinkKind : std::uint8_t { Asset, Entity, Scene, Panel, Other };
enum class TaskUpdateKind : std::uint8_t { Comment, Status, Assignment, Other };

const char* taskStatusName(TaskStatus s);           // "open", "in-progress", "blocked", "done"
bool        taskStatusFromName(std::string_view name, TaskStatus& out);
const char* taskPriorityName(TaskPriority p);       // "low", "normal", "high", "urgent"
bool        taskPriorityFromName(std::string_view name, TaskPriority& out);
const char* taskLinkKindName(TaskLinkKind k);       // "asset", "entity", "scene", "panel"
bool        taskLinkKindFromName(std::string_view name, TaskLinkKind& out);
const char* taskUpdateKindName(TaskUpdateKind k);   // "comment", "status", "assignment"
bool        taskUpdateKindFromName(std::string_view name, TaskUpdateKind& out);

// 32 lowercase hex digits, hi then lo — the spelling uuidOf() in
// EditorCommands.cpp and the MCP tools use for entities. False (out untouched)
// for anything that is not exactly that.
std::string uuidToHex(const UUID& id);
bool        uuidFromHex(std::string_view text, UUID& out);

// Wall-clock UTC in milliseconds since 1970 — what every time on the board is.
std::int64_t taskNowMs();
// "2026-10-14T18:51:03.120Z" and back. The parser takes exactly that shape
// (the 'Z' required, milliseconds optional); false for anything else.
std::string  taskTimeToIso(std::int64_t ms);
bool         taskTimeFromIso(std::string_view text, std::int64_t& outMs);

struct TaskLink
{
	TaskLinkKind kind = TaskLinkKind::Asset;
	std::string  kindOther;   // the file's word when kind == Other

	// Asset: the asset's UUID. Entity: the entity's UUID (EntityIdComponent),
	// with `scene` saying where it lives. Unused for Scene and Panel.
	UUID         id;
	// Scene: the scene itself. Entity: the scene the entity is in. A
	// PROJECT-relative path with forward slashes ("Content/Levels/Town.hescene").
	std::string  scene;
	// Panel: the panel's window name — the one identity a panel keeps across
	// docking, closing and reopening (PanelSpotlight uses the same).
	std::string  panel;
	// What the target was called when it was linked, for the chip — and the only
	// way to say WHAT is missing once the link is orphaned.
	std::string  label;

	std::string  extraJson;   // keys this build does not know, as a JSON object
	bool         orphaned = false;   // runtime only — see refreshOrphans()

	static TaskLink asset(const UUID& assetId, std::string label = {});
	static TaskLink entity(const UUID& entityId, std::string scenePath, std::string label = {});
	static TaskLink sceneLink(std::string scenePath, std::string label = {});
	static TaskLink panelLink(std::string windowName, std::string label = {});

	// Same target (kind and address; label and orphaned do not count).
	bool sameTarget(const TaskLink& o) const;
};

// One entry in a task's history: a comment, a status change, a (re)assignment.
// The team bridge posts these; the board shows them as the task's thread.
struct TaskUpdate
{
	UUID           id;
	TaskUpdateKind kind = TaskUpdateKind::Comment;
	std::string    kindOther;     // the file's word when kind == Other
	std::string    author;        // team member name, or the local user's name
	std::int64_t   timeMs = 0;
	std::string    text;          // optional for Status / Assignment
	std::string    status;        // Status: the new status, as its word
	std::vector<std::string> assignees;   // Assignment: who it is assigned to now
	std::string    extraJson;
};

struct Task
{
	UUID         id;
	std::string  title;
	std::string  description;

	TaskStatus   status = TaskStatus::Open;
	std::string  statusOther;
	TaskPriority priority = TaskPriority::Normal;
	std::string  priorityOther;

	// People are team member names (TeamMember::name on the team server); with
	// no team server, whatever name the local user goes by.
	std::string  createdBy;
	std::string  assignedBy;                 // who made the current assignment
	std::vector<std::string> assignees;

	std::int64_t createdMs  = 0;
	std::int64_t modifiedMs = 0;

	std::vector<TaskLink>   links;
	std::vector<TaskUpdate> updates;         // oldest first

	std::string  extraJson;

	bool isDone() const { return status == TaskStatus::Done; }
	bool isAssignedTo(std::string_view member) const;
	int  orphanedLinkCount() const;
};

class TaskBoard
{
public:
	static constexpr int kVersion = 1;

	// File order. Public on purpose: the panel sorts and filters a copy of the
	// indices, never this vector — its order is the file's.
	std::vector<Task> tasks;

	// ── Editing ──────────────────────────────────────────────────────────────
	// Each stamps the task's modifiedMs with `nowMs`; the ones that are part of
	// the task's story also append a TaskUpdate (by `author`). All return
	// nullptr / false when no task has `id`.
	Task* add(std::string title, std::string author, std::int64_t nowMs);
	bool  remove(const UUID& id);
	Task*       find(const UUID& id);
	const Task* find(const UUID& id) const;

	bool setTitle(const UUID& id, std::string title, std::int64_t nowMs);
	bool setDescription(const UUID& id, std::string text, std::int64_t nowMs);
	bool setPriority(const UUID& id, TaskPriority p, std::int64_t nowMs);
	// Appends a Status update (with `comment` as its text). No-op returning true
	// when the status is already `s`.
	bool setStatus(const UUID& id, TaskStatus s, const std::string& author,
	               std::int64_t nowMs, std::string comment = {});
	// Replaces the assignees (duplicates and empty names removed, order kept),
	// records `author` as assignedBy and appends an Assignment update.
	bool assign(const UUID& id, std::vector<std::string> assignees,
	            const std::string& author, std::int64_t nowMs);
	TaskUpdate* comment(const UUID& id, const std::string& author, std::string text,
	                    std::int64_t nowMs);
	// False when the task already has a link to the same target.
	bool addLink(const UUID& id, TaskLink link, std::int64_t nowMs);
	bool removeLink(const UUID& id, std::size_t index, std::int64_t nowMs);

	// ── Questions the footer and the team bridge ask ────────────────────────
	int openCount() const;
	int openCountFor(std::string_view member) const;
	// No tasks and nothing kept from the file — what saveTaskBoard() need not write.
	bool empty() const { return tasks.empty() && m_unreadableTasks.empty(); }

	// ── Orphans ──────────────────────────────────────────────────────────────
	// Present: the target is there. Missing: it is not (orphaned). Unknown: the
	// caller cannot tell right now (an entity in a scene that is not open) —
	// the link is NOT marked orphaned on a guess.
	enum class LinkCheck : std::uint8_t { Present, Missing, Unknown };
	using LinkResolver = std::function<LinkCheck(const TaskLink&)>;
	// Sets every link's `orphaned` from `resolve` and returns how many are
	// orphaned. A link of a kind this build does not know is always orphaned:
	// there is nowhere this editor could jump with it. Never throws on a missing
	// target, never removes a link. An empty resolver checks nothing (all
	// links not orphaned, except unknown kinds).
	int refreshOrphans(const LinkResolver& resolve);

	// ── The document ─────────────────────────────────────────────────────────
	struct LoadResult
	{
		bool        ok = false;
		int         version = 0;     // the file's version (1 when it had none)
		int         dropped = 0;     // non-object entries inside links/updates
		std::string error;           // why !ok, for a human
	};
	// Parses `text` into this board. On failure the board is left EMPTY and the
	// result says why — the caller must not save it over the file it came from
	// (saveTaskBoard() checks that itself).
	LoadResult  fromJson(std::string_view text);
	// The file's bytes: pretty-printed, LF only, one trailing newline.
	std::string toJson() const;

	// The version this board is written under: kVersion, or the file's own if
	// that was newer (see the header comment).
	int  writeVersion() const { return m_fileVersion > kVersion ? m_fileVersion : kVersion; }

private:
	int                      m_fileVersion = kVersion;
	std::string              m_extraJson;      // unknown top-level keys
	std::vector<std::string> m_unreadableTasks;   // non-object entries of "tasks", verbatim

	// What the file held when this board last read or wrote it: its bytes, or
	// "no file" (also for a board that never touched the disk). saveTaskBoard()
	// compares the disk against this to notice a pull that landed in between.
	bool        m_diskHadFile = false;
	std::string m_diskBytes;

	friend LoadResult loadTaskBoard(const std::filesystem::path& projectRoot, TaskBoard& out);
	friend bool saveTaskBoard(const std::filesystem::path& projectRoot, TaskBoard& board,
	                          std::string* error);
};

// ── The file ─────────────────────────────────────────────────────────────────
std::filesystem::path taskBoardPath(const std::filesystem::path& projectRoot);

// No file is an empty board and ok. A file that cannot be read or parsed gives
// !ok, an empty board and the reason.
TaskBoard::LoadResult loadTaskBoard(const std::filesystem::path& projectRoot, TaskBoard& out);

// Temp + rename (never a half-written file). Writes nothing for an empty board
// when there is no file yet — opening the panel must not add a file to every
// project. Refuses (false, `error` says why, the file untouched):
//   * when the file on disk does not parse — a merge conflict or a damaged
//     file somebody has to look at; saving over it would destroy the only copy
//     of their tasks;
//   * when the file changed since this board loaded or saved it — a git pull,
//     a checkout, a teammate's edit synced in. Writing then would silently drop
//     whatever arrived; the caller reloads (and re-applies its edit) instead.
//     A board that never loaded counts as having seen "no file".
// On success the board remembers what it wrote.
bool saveTaskBoard(const std::filesystem::path& projectRoot, TaskBoard& board,
                   std::string* error = nullptr);

} // namespace HE
