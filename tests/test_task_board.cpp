// The task board's data and its file (Config/TaskBoard.json). What it promises
// a team that keeps the file in git: a round trip changes nothing, a layout
// from before still loads, a layout from a newer editor survives an older one
// saving it, a merge conflict is never saved over, and a link whose target is
// gone is marked orphaned instead of crashing anything.
#include "doctest.h"
#include "TestFsUtil.h"

#include "TaskBoard.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace HE;

// Non-ASCII spelled as UTF-8 bytes: the tests compile without /utf-8, so MSVC
// would read a literal umlaut through the ANSI code page and hand the parser
// something that is not UTF-8 at all.
#define HE_TB_UNICODE "\xC3\x9C" "n" "\xC3\xAF" "c" "\xC3\xB6" "d" "\xC3\xA9"   // U-umlaut n i-diaeresis c o-umlaut d e-acute
#define HE_TB_DASH "\xE2\x80\x94"      // em dash
#define HE_TB_JAPANESE "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x82\xBF\xE3\x82\xB9\xE3\x82\xAF"   // "task" in Japanese, seven characters


namespace
{

struct TempProject
{
	fs::path root;
	TempProject()
	{
		root = fs::temp_directory_path() / ("he_test_task_board_" + uuidToHex(UUID::generate()));
		fs::create_directories(root);
	}
	~TempProject() { he_test::removeAllQuiet(root); }
};

std::string readBytes(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeBytes(const fs::path& p, const std::string& text)
{
	fs::create_directories(p.parent_path());
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	out << text;
}

UUID hex(const char* s)
{
	UUID id;
	REQUIRE(uuidFromHex(s, id));
	return id;
}

// ── The first layout, frozen ─────────────────────────────────────────────────
// Version 1 exactly as this build writes it. It is the "older layout" every
// later build must still load: when the format moves to version 2, this text
// stays as it is and this test keeps asserting it loads field for field. Do
// not regenerate it from toJson() — that would test nothing.
const char* const kLayoutV1 = R"({
    "version": 1,
    "tasks": [
        {
            "id": "0123456789abcdef0123456789abcdef",
            "title": "Fix the bridge collision",
            "description": "The player falls through the second plank.",
            "status": "in-progress",
            "priority": "high",
            "createdBy": "Ada",
            "assignedBy": "Ada",
            "assignees": [
                "Ben",
                "Cleo"
            ],
            "created": "2026-10-14T09:30:00.000Z",
            "modified": "2026-10-14T10:15:42.250Z",
            "links": [
                {
                    "kind": "asset",
                    "asset": "a0000000000000000000000000000001",
                    "label": "Bridge_Plank.hasset"
                },
                {
                    "kind": "entity",
                    "entity": "e0000000000000000000000000000002",
                    "scene": "Content/Levels/Town.hescene",
                    "label": "Plank 2"
                },
                {
                    "kind": "scene",
                    "scene": "Content/Levels/Town.hescene"
                },
                {
                    "kind": "panel",
                    "panel": "Physics Debug",
                    "label": "Physics Debug"
                }
            ],
            "updates": [
                {
                    "id": "c0000000000000000000000000000001",
                    "kind": "assignment",
                    "author": "Ada",
                    "time": "2026-10-14T09:31:00.000Z",
                    "assignees": [
                        "Ben",
                        "Cleo"
                    ]
                },
                {
                    "id": "c0000000000000000000000000000002",
                    "kind": "status",
                    "author": "Ben",
                    "time": "2026-10-14T10:15:42.250Z",
                    "text": "Looking at it now.",
                    "status": "in-progress"
                },
                {
                    "id": "c0000000000000000000000000000003",
                    "kind": "comment",
                    "author": "Cleo",
                    "time": "2026-10-14T10:16:00.000Z",
                    "text": "It is the convex hull )" HE_TB_DASH " " HE_TB_UNICODE R"( stays as it is."
                }
            ]
        },
        {
            "id": "fedcba9876543210fedcba9876543210",
            "title": "Write the credits",
            "description": "",
            "status": "done",
            "priority": "low",
            "createdBy": "Ben",
            "assignedBy": "",
            "assignees": [],
            "created": "2026-10-01T00:00:00.000Z",
            "modified": "2026-10-02T12:00:00.000Z",
            "links": [],
            "updates": []
        }
    ]
}
)";

} // namespace

// ─── Words, ids, times ───────────────────────────────────────────────────────

TEST_CASE("TaskBoard: enum words, UUID hex and ISO times round-trip and reject junk")
{
	for (TaskStatus s : { TaskStatus::Open, TaskStatus::InProgress, TaskStatus::Blocked, TaskStatus::Done })
	{
		TaskStatus back = TaskStatus::Other;
		CHECK(taskStatusFromName(taskStatusName(s), back));
		CHECK(back == s);
	}
	TaskStatus st;
	CHECK_FALSE(taskStatusFromName("review", st));
	TaskLinkKind lk;
	CHECK_FALSE(taskLinkKindFromName("Asset", lk));   // words are lowercase, exactly

	const UUID id = UUID::generate();
	UUID back;
	CHECK(uuidFromHex(uuidToHex(id), back));
	CHECK(back == id);
	CHECK(uuidToHex(id).size() == 32);
	CHECK_FALSE(uuidFromHex("0123", back));
	CHECK_FALSE(uuidFromHex("0123456789abcdef0123456789abcdeg", back));

	CHECK(taskTimeToIso(0) == "1970-01-01T00:00:00.000Z");
	std::int64_t ms = 0;
	REQUIRE(taskTimeFromIso("2026-10-14T10:15:42.250Z", ms));
	CHECK(taskTimeToIso(ms) == "2026-10-14T10:15:42.250Z");
	REQUIRE(taskTimeFromIso("2024-02-29T23:59:59Z", ms));   // leap day, no millis
	CHECK(taskTimeToIso(ms) == "2024-02-29T23:59:59.000Z");
	const std::int64_t now = taskNowMs();
	REQUIRE(taskTimeFromIso(taskTimeToIso(now), ms));
	CHECK(ms == now);
	CHECK_FALSE(taskTimeFromIso("2026-10-14 10:15:42Z", ms));
	CHECK_FALSE(taskTimeFromIso("2026-10-14T10:15:42.250", ms));   // no 'Z': no zone, no guess
	CHECK_FALSE(taskTimeFromIso("2026-13-01T00:00:00Z", ms));
}

// ─── Editing ─────────────────────────────────────────────────────────────────

TEST_CASE("TaskBoard: editing stamps modified and writes the task's story")
{
	TaskBoard b;
	Task* t = b.add("Paint the barn", "Ada", 1000);
	REQUIRE(t);
	const UUID id = t->id;
	CHECK(t->createdBy == "Ada");
	CHECK(t->createdMs == 1000);
	CHECK(t->status == TaskStatus::Open);
	CHECK(t->priority == TaskPriority::Normal);
	CHECK(b.openCount() == 1);

	CHECK(b.assign(id, { "Ben", "", "Cleo", "Ben" }, "Ada", 2000));
	t = b.find(id);
	CHECK(t->assignees == std::vector<std::string>{ "Ben", "Cleo" });
	CHECK(t->assignedBy == "Ada");
	CHECK(t->modifiedMs == 2000);
	REQUIRE(t->updates.size() == 1);
	CHECK(t->updates[0].kind == TaskUpdateKind::Assignment);
	CHECK(t->updates[0].assignees == t->assignees);
	CHECK(b.openCountFor("Ben") == 1);
	CHECK(b.openCountFor("Dora") == 0);

	CHECK(b.setStatus(id, TaskStatus::InProgress, "Ben", 3000, "On it"));
	CHECK(b.setStatus(id, TaskStatus::InProgress, "Ben", 3500));   // same status: no new entry
	t = b.find(id);
	REQUIRE(t->updates.size() == 2);
	CHECK(t->updates[1].kind == TaskUpdateKind::Status);
	CHECK(t->updates[1].status == "in-progress");
	CHECK(t->updates[1].text == "On it");
	CHECK(t->modifiedMs == 3000);
	CHECK_FALSE(b.setStatus(id, TaskStatus::Other, "Ben", 3600));

	REQUIRE(b.comment(id, "Cleo", "Red or white?", 4000));
	CHECK(b.find(id)->updates.back().kind == TaskUpdateKind::Comment);

	CHECK(b.addLink(id, TaskLink::asset(hex("a0000000000000000000000000000001"), "Barn.hasset"), 5000));
	CHECK_FALSE(b.addLink(id, TaskLink::asset(hex("a0000000000000000000000000000001"), "renamed"), 5100));
	CHECK(b.addLink(id, TaskLink::entity(hex("e0000000000000000000000000000002"), "Content/A.hescene"), 5200));
	// The same entity id in another scene is another target.
	CHECK(b.addLink(id, TaskLink::entity(hex("e0000000000000000000000000000002"), "Content/B.hescene"), 5300));
	CHECK(b.find(id)->links.size() == 3);
	CHECK(b.removeLink(id, 1, 5400));
	CHECK_FALSE(b.removeLink(id, 9, 5500));
	CHECK(b.find(id)->links.size() == 2);
	CHECK(b.find(id)->modifiedMs == 5400);

	CHECK(b.setStatus(id, TaskStatus::Done, "Ben", 6000));
	CHECK(b.openCount() == 0);
	CHECK(b.openCountFor("Ben") == 0);

	const UUID nobody = UUID::generate();
	CHECK_FALSE(b.setTitle(nobody, "x", 1));
	CHECK_FALSE(b.assign(nobody, { "Ben" }, "Ada", 1));
	CHECK(b.comment(nobody, "Ada", "x", 1) == nullptr);
	CHECK(b.remove(id));
	CHECK_FALSE(b.remove(id));
	CHECK(b.tasks.empty());
}

// ─── The file ────────────────────────────────────────────────────────────────

TEST_CASE("TaskBoard: save, load and save again is byte-identical, LF only")
{
	TempProject p;
	TaskBoard b;
	const UUID a = b.add("Light the cave", "Ada", taskNowMs())->id;
	const UUID c = b.add(HE_TB_JAPANESE, "Ben", taskNowMs())->id;
	b.setDescription(a, "Two torches\nand a lantern", taskNowMs());
	b.setPriority(a, TaskPriority::Urgent, taskNowMs());
	b.assign(a, { "Cleo" }, "Ada", taskNowMs());
	b.setStatus(c, TaskStatus::Blocked, "Ben", taskNowMs(), "Waiting for audio");
	b.comment(a, "Cleo", "\"quoted\" and \\ backslash", taskNowMs());
	b.addLink(a, TaskLink::asset(UUID::generate(), "Torch.hasset"), taskNowMs());
	b.addLink(a, TaskLink::entity(UUID::generate(), "Content/Cave.hescene", "Lantern"), taskNowMs());
	b.addLink(a, TaskLink::sceneLink("Content/Cave.hescene"), taskNowMs());
	b.addLink(c, TaskLink::panelLink("Audio Mixer", "Audio Mixer"), taskNowMs());

	std::string err;
	REQUIRE(saveTaskBoard(p.root, b, &err));
	const fs::path file = taskBoardPath(p.root);
	CHECK(file == p.root / "Config" / "TaskBoard.json");
	const std::string first = readBytes(file);
	CHECK(first.find('\r') == std::string::npos);
	REQUIRE(!first.empty());
	CHECK(first.back() == '\n');
	CHECK(first.find(HE_TB_JAPANESE) != std::string::npos);   // not \u-escaped
	CHECK(first.find("orphaned") == std::string::npos);        // runtime state stays out

	TaskBoard loaded;
	const TaskBoard::LoadResult r = loadTaskBoard(p.root, loaded);
	REQUIRE(r.ok);
	CHECK(r.version == TaskBoard::kVersion);
	CHECK(r.dropped == 0);
	REQUIRE(loaded.tasks.size() == 2);
	const Task* la = loaded.find(a);
	REQUIRE(la);
	const Task* oa = b.find(a);
	CHECK(la->title == oa->title);
	CHECK(la->description == oa->description);
	CHECK(la->priority == TaskPriority::Urgent);
	CHECK(la->assignees == oa->assignees);
	CHECK(la->createdMs == oa->createdMs);
	CHECK(la->modifiedMs == oa->modifiedMs);
	REQUIRE(la->links.size() == 3);
	for (std::size_t i = 0; i < 3; ++i)
	{
		CHECK(la->links[i].sameTarget(oa->links[i]));
		CHECK(la->links[i].label == oa->links[i].label);
	}
	REQUIRE(la->updates.size() == oa->updates.size());
	for (std::size_t i = 0; i < la->updates.size(); ++i)
	{
		CHECK(la->updates[i].id == oa->updates[i].id);
		CHECK(la->updates[i].kind == oa->updates[i].kind);
		CHECK(la->updates[i].text == oa->updates[i].text);
		CHECK(la->updates[i].timeMs == oa->updates[i].timeMs);
	}
	CHECK(loaded.find(c)->status == TaskStatus::Blocked);

	// A load must not re-stamp anything: what comes back out is the same bytes.
	CHECK(loaded.toJson() == first);
	const auto mtime = fs::last_write_time(file);
	REQUIRE(saveTaskBoard(p.root, loaded, &err));
	CHECK(readBytes(file) == first);
	CHECK(fs::last_write_time(file) == mtime);   // unchanged content is not rewritten
}

TEST_CASE("TaskBoard: the version 1 layout loads field for field and writes back unchanged")
{
	TaskBoard b;
	const TaskBoard::LoadResult r = b.fromJson(kLayoutV1);
	REQUIRE_MESSAGE(r.ok, r.error);
	CHECK(r.version == 1);
	CHECK(r.dropped == 0);
	REQUIRE(b.tasks.size() == 2);

	const Task& t = b.tasks[0];
	CHECK(uuidToHex(t.id) == "0123456789abcdef0123456789abcdef");
	CHECK(t.title == "Fix the bridge collision");
	CHECK(t.description == "The player falls through the second plank.");
	CHECK(t.status == TaskStatus::InProgress);
	CHECK(t.priority == TaskPriority::High);
	CHECK(t.createdBy == "Ada");
	CHECK(t.assignedBy == "Ada");
	CHECK(t.assignees == std::vector<std::string>{ "Ben", "Cleo" });
	CHECK(taskTimeToIso(t.createdMs) == "2026-10-14T09:30:00.000Z");
	CHECK(taskTimeToIso(t.modifiedMs) == "2026-10-14T10:15:42.250Z");

	REQUIRE(t.links.size() == 4);
	CHECK(t.links[0].kind == TaskLinkKind::Asset);
	CHECK(uuidToHex(t.links[0].id) == "a0000000000000000000000000000001");
	CHECK(t.links[0].label == "Bridge_Plank.hasset");
	CHECK(t.links[1].kind == TaskLinkKind::Entity);
	CHECK(uuidToHex(t.links[1].id) == "e0000000000000000000000000000002");
	CHECK(t.links[1].scene == "Content/Levels/Town.hescene");
	CHECK(t.links[2].kind == TaskLinkKind::Scene);
	CHECK(t.links[2].scene == "Content/Levels/Town.hescene");
	CHECK(t.links[2].label.empty());
	CHECK(t.links[3].kind == TaskLinkKind::Panel);
	CHECK(t.links[3].panel == "Physics Debug");
	for (const TaskLink& l : t.links) CHECK(l.extraJson.empty());

	REQUIRE(t.updates.size() == 3);
	CHECK(t.updates[0].kind == TaskUpdateKind::Assignment);
	CHECK(t.updates[0].assignees == std::vector<std::string>{ "Ben", "Cleo" });
	CHECK(t.updates[1].kind == TaskUpdateKind::Status);
	CHECK(t.updates[1].status == "in-progress");
	CHECK(t.updates[1].author == "Ben");
	CHECK(t.updates[2].kind == TaskUpdateKind::Comment);
	CHECK(t.updates[2].text == "It is the convex hull " HE_TB_DASH " " HE_TB_UNICODE " stays as it is.");

	CHECK(b.tasks[1].isDone());
	CHECK(b.tasks[1].assignees.empty());

	CHECK(b.toJson() == kLayoutV1);
}

TEST_CASE("TaskBoard: a file without a version key is the first layout")
{
	TaskBoard b;
	const TaskBoard::LoadResult r = b.fromJson(R"({ "tasks": [ { "id": "0123456789abcdef0123456789abcdef",
	    "title": "Hand-written", "status": "open", "created": 1760434200000 } ] })");
	REQUIRE(r.ok);
	CHECK(r.version == 1);
	REQUIRE(b.tasks.size() == 1);
	CHECK(b.tasks[0].title == "Hand-written");
	CHECK(b.tasks[0].createdMs == 1760434200000);   // epoch millis accepted too
	CHECK(b.writeVersion() == 1);
	CHECK(b.toJson().find("\"version\": 1") != std::string::npos);
}

TEST_CASE("TaskBoard: what a newer editor wrote survives an older one saving it")
{
	// Version 2 from the future: a key on the file, on a task, on a link and on
	// an update this build does not know; a status, a priority, a link kind
	// and an update kind it does not know; an entry in "tasks" that is not
	// even an object. Written in this build's canonical order so the round
	// trip can be compared byte for byte.
	const char* const kFuture = R"({
    "version": 2,
    "tasks": [
        {
            "id": "0123456789abcdef0123456789abcdef",
            "title": "Future task",
            "description": "",
            "status": "in-review",
            "priority": "someday",
            "createdBy": "Ada",
            "assignedBy": "",
            "assignees": [],
            "created": "2027-01-01T00:00:00.000Z",
            "modified": "2027-01-01T00:00:00.000Z",
            "links": [
                {
                    "kind": "asset",
                    "asset": "a0000000000000000000000000000001",
                    "thumbnailHint": "atlas:12"
                },
                {
                    "kind": "graph-node",
                    "label": "Branch",
                    "graph": "b0000000000000000000000000000001",
                    "node": 42
                }
            ],
            "updates": [
                {
                    "id": "c0000000000000000000000000000001",
                    "kind": "reaction",
                    "author": "Ben",
                    "time": "2027-01-01T00:00:01.000Z",
                    "emoji": "+1"
                },
                {
                    "id": "c0000000000000000000000000000002",
                    "kind": "comment",
                    "author": "Cleo",
                    "time": "2027-01-01T00:00:02.000Z",
                    "text": "Seen.",
                    "mentions": [
                        "Ada"
                    ]
                }
            ],
            "estimate": {
                "points": 3
            }
        },
        "a stray string the future put here"
    ],
    "boardName": "Sprint 12"
}
)";
	TaskBoard b;
	const TaskBoard::LoadResult r = b.fromJson(kFuture);
	REQUIRE_MESSAGE(r.ok, r.error);
	CHECK(r.version == 2);
	CHECK(b.writeVersion() == 2);   // not downgraded to ours
	REQUIRE(b.tasks.size() == 1);
	const Task& t = b.tasks[0];
	CHECK(t.status == TaskStatus::Other);
	CHECK(t.statusOther == "in-review");
	CHECK(t.priority == TaskPriority::Other);
	CHECK(t.priorityOther == "someday");
	CHECK_FALSE(t.isDone());
	REQUIRE(t.links.size() == 2);
	CHECK(t.links[1].kind == TaskLinkKind::Other);
	CHECK(t.links[1].kindOther == "graph-node");
	CHECK(t.links[1].label == "Branch");
	REQUIRE(t.updates.size() == 2);
	CHECK(t.updates[0].kind == TaskUpdateKind::Other);
	CHECK(t.updates[0].kindOther == "reaction");

	CHECK(b.toJson() == kFuture);

	// Editing a known field keeps the unknown ones around it.
	b.comment(t.id, "Dora", "Older editor was here", 1);
	const std::string edited = b.toJson();
	for (const char* kept : { "\"boardName\": \"Sprint 12\"", "\"thumbnailHint\"", "\"graph-node\"",
	                          "\"node\": 42", "\"emoji\": \"+1\"", "\"mentions\"", "\"points\": 3",
	                          "a stray string the future put here", "\"in-review\"", "\"someday\"",
	                          "Older editor was here" })
		CHECK_MESSAGE(edited.find(kept) != std::string::npos, std::string(kept));

	// A status set by this build replaces the unknown word — that is an edit.
	b.setStatus(t.id, TaskStatus::Done, "Dora", 2);
	CHECK(b.toJson().find("\"in-review\"") == std::string::npos);
}

TEST_CASE("TaskBoard: a damaged or conflicted file is refused and never saved over")
{
	TempProject p;
	const fs::path file = taskBoardPath(p.root);
	const std::string conflicted =
		"{\n    \"version\": 1,\n    \"tasks\": [\n"
		"<<<<<<< HEAD\n        { \"id\": \"0123456789abcdef0123456789abcdef\", \"title\": \"Mine\" }\n"
		"=======\n        { \"id\": \"0123456789abcdef0123456789abcdef\", \"title\": \"Theirs\" }\n"
		">>>>>>> feature\n    ]\n}\n";
	writeBytes(file, conflicted);

	TaskBoard b;
	const TaskBoard::LoadResult r = loadTaskBoard(p.root, b);
	CHECK_FALSE(r.ok);
	CHECK(r.error.find("merge-conflict") != std::string::npos);
	CHECK(b.tasks.empty());

	b.add("Something new", "Ada", 1);
	std::string err;
	CHECK_FALSE(saveTaskBoard(p.root, b, &err));
	CHECK(err.find("merge-conflict") != std::string::npos);
	CHECK(readBytes(file) == conflicted);
	CHECK_FALSE(fs::exists(file.string() + ".tmp"));

	// Same for shapes that parse but cannot be understood without loss.
	for (const char* bad : { "[]", R"({"version": "two", "tasks": []})", R"({"version": 1, "tasks": {}})" })
	{
		writeBytes(file, bad);
		TaskBoard probe;
		CHECK_FALSE(probe.fromJson(bad).ok);
		CHECK_FALSE(saveTaskBoard(p.root, b, &err));
		CHECK(readBytes(file) == bad);
	}

	// Once a human has fixed it (here: removed it) and the board reloaded,
	// saving works again. Without the reload the board still remembers the
	// conflicted file, and a file that vanished is a change on disk too.
	fs::remove(file);
	CHECK_FALSE(saveTaskBoard(p.root, b, &err));
	REQUIRE(loadTaskBoard(p.root, b).ok);
	b.add("Something new", "Ada", 1);
	CHECK(saveTaskBoard(p.root, b, &err));
	TaskBoard back;
	REQUIRE(loadTaskBoard(p.root, back).ok);
	CHECK(back.tasks.size() == 1);
}

TEST_CASE("TaskBoard: a pull that lands while the board is open is not saved over")
{
	TempProject p;
	const fs::path file = taskBoardPath(p.root);
	{
		TaskBoard seed;
		seed.add("A: already there", "Ada", 1);
		REQUIRE(saveTaskBoard(p.root, seed));
	}

	TaskBoard mine;
	REQUIRE(loadTaskBoard(p.root, mine).ok);

	// The pull: a teammate's commit rewrites the file under the open board.
	{
		TaskBoard theirs;
		REQUIRE(loadTaskBoard(p.root, theirs).ok);
		theirs.add("B: from the pull", "Ben", 2);
		REQUIRE(saveTaskBoard(p.root, theirs));
	}
	const std::string pulled = readBytes(file);

	mine.add("C: mine", "Cleo", 3);
	std::string err;
	CHECK_FALSE(saveTaskBoard(p.root, mine, &err));
	CHECK(err.find("changed on disk") != std::string::npos);
	CHECK(readBytes(file) == pulled);   // B is still there

	// Reload, redo the edit, save: all three.
	REQUIRE(loadTaskBoard(p.root, mine).ok);
	mine.add("C: mine", "Cleo", 3);
	REQUIRE(saveTaskBoard(p.root, mine, &err));
	TaskBoard back;
	REQUIRE(loadTaskBoard(p.root, back).ok);
	REQUIRE(back.tasks.size() == 3);
	CHECK(back.tasks[1].title == "B: from the pull");

	// Saving twice in a row is fine — the board remembers its own write.
	mine.add("D: mine too", "Cleo", 4);
	CHECK(saveTaskBoard(p.root, mine, &err));

	// A board that never loaded has seen "no file": it may not replace one.
	TaskBoard fresh;
	fresh.add("E: from nowhere", "Dora", 5);
	CHECK_FALSE(saveTaskBoard(p.root, fresh, &err));

	// A file deleted under the board (a checkout without it) is a change too.
	fs::remove(file);
	CHECK_FALSE(saveTaskBoard(p.root, mine, &err));
	CHECK_FALSE(fs::exists(file));
}

TEST_CASE("TaskBoard: no file is an empty board, and an empty board writes no file")
{
	TempProject p;
	TaskBoard b;
	b.add("left over", "Ada", 1);
	const TaskBoard::LoadResult r = loadTaskBoard(p.root, b);
	CHECK(r.ok);
	CHECK(b.tasks.empty());   // the load replaced what was there
	CHECK(saveTaskBoard(p.root, b));
	CHECK_FALSE(fs::exists(taskBoardPath(p.root)));

	// But deleting the last task of an existing file is a change worth saving.
	const UUID id = b.add("only", "Ada", 1)->id;
	REQUIRE(saveTaskBoard(p.root, b));
	b.remove(id);
	REQUIRE(saveTaskBoard(p.root, b));
	TaskBoard back;
	REQUIRE(loadTaskBoard(p.root, back).ok);
	CHECK(back.tasks.empty());
	CHECK(fs::exists(taskBoardPath(p.root)));
}

TEST_CASE("TaskBoard: malformed entries are survived — duplicate ids, junk links, no id")
{
	TaskBoard b;
	const TaskBoard::LoadResult r = b.fromJson(R"J({ "version": 1, "tasks": [
	    { "id": "0123456789abcdef0123456789abcdef", "title": "A",
	      "links": [ 7, { "kind": "asset", "asset": "not-a-uuid" } ], "updates": [ null ] },
	    { "id": "0123456789abcdef0123456789abcdef", "title": "B (pasted copy)" },
	    { "title": "C (no id)", "assignees": "Ben", "status": 3 } ] })J");
	REQUIRE(r.ok);
	CHECK(r.dropped == 2);   // the 7 and the null
	REQUIRE(b.tasks.size() == 3);
	CHECK_FALSE(b.tasks[1].id == b.tasks[0].id);   // the copy got its own id
	CHECK_FALSE(b.tasks[2].id == UUID{});
	CHECK(b.tasks[2].assignees.empty());           // wrong type: default, no crash
	CHECK(b.tasks[2].status == TaskStatus::Open);
	REQUIRE(b.tasks[0].links.size() == 1);
	CHECK(b.tasks[0].links[0].id == UUID{});
	// An address that names nothing is orphaned whatever a resolver says.
	CHECK(b.refreshOrphans([](const TaskLink&) { return TaskBoard::LinkCheck::Present; }) == 1);
}

// ─── Orphans ─────────────────────────────────────────────────────────────────

TEST_CASE("TaskBoard: links with a missing target are marked orphaned, kept, and not saved as such")
{
	TaskBoard b;
	const UUID id = b.add("Links of every kind", "Ada", 1)->id;
	const UUID liveAsset = UUID::generate(), deadAsset = UUID::generate();
	const UUID liveEntity = UUID::generate(), elsewhereEntity = UUID::generate(),
	           deadEntity = UUID::generate();
	b.addLink(id, TaskLink::asset(liveAsset, "Live.hasset"), 1);
	b.addLink(id, TaskLink::asset(deadAsset, "Deleted.hasset"), 1);
	b.addLink(id, TaskLink::entity(liveEntity, "Content/Open.hescene"), 1);
	b.addLink(id, TaskLink::entity(elsewhereEntity, "Content/Closed.hescene"), 1);
	b.addLink(id, TaskLink::entity(deadEntity, "Content/Open.hescene", "Removed crate"), 1);
	b.addLink(id, TaskLink::sceneLink("Content/Open.hescene"), 1);
	b.addLink(id, TaskLink::sceneLink("Content/Renamed.hescene"), 1);
	b.addLink(id, TaskLink::panelLink("Outliner"), 1);
	b.addLink(id, TaskLink::panelLink("Panel From A Plugin"), 1);

	// What an editor with only Open.hescene loaded can say.
	auto resolve = [&](const TaskLink& l) {
		using C = TaskBoard::LinkCheck;
		switch (l.kind)
		{
		case TaskLinkKind::Asset: return l.id == liveAsset ? C::Present : C::Missing;
		case TaskLinkKind::Entity:
			if (l.scene != "Content/Open.hescene") return C::Unknown;   // not loaded: cannot tell
			return l.id == liveEntity ? C::Present : C::Missing;
		case TaskLinkKind::Scene: return l.scene == "Content/Open.hescene" ? C::Present : C::Missing;
		case TaskLinkKind::Panel: return l.panel == "Outliner" ? C::Present : C::Missing;
		default: return C::Missing;
		}
	};
	CHECK(b.refreshOrphans(resolve) == 4);
	const Task& t = *b.find(id);
	REQUIRE(t.links.size() == 9);   // nothing removed
	const bool expected[] = { false, true, false, false, true, false, true, false, true };
	for (std::size_t i = 0; i < 9; ++i)
		CHECK_MESSAGE(t.links[i].orphaned == expected[i], "link " << i);
	CHECK(t.orphanedLinkCount() == 4);
	CHECK(t.links[4].label == "Removed crate");   // the chip can still say what is gone
	CHECK(b.toJson().find("orphaned") == std::string::npos);

	// Targets come back (a pull restored the asset): the flag follows.
	auto allPresent = [](const TaskLink&) { return TaskBoard::LinkCheck::Present; };
	CHECK(b.refreshOrphans(allPresent) == 0);
	// No resolver: nothing checked, nothing marked.
	CHECK(b.refreshOrphans({}) == 0);

	// A kind this build does not know cannot be followed: orphaned, and kept.
	TaskBoard f;
	REQUIRE(f.fromJson(R"({"version": 3, "tasks": [ { "id": "0123456789abcdef0123456789abcdef",
	    "links": [ { "kind": "timeline-marker", "at": 12.5 } ] } ]})").ok);
	CHECK(f.refreshOrphans(allPresent) == 1);
	CHECK(f.toJson().find("\"at\": 12.5") != std::string::npos);
}
