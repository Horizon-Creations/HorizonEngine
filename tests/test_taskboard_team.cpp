#include "doctest.h"
#include "FakeTaskTeam.h"

#include "TaskBoard.h"
#include "TaskBoardTeam.h"
#include "NotificationStore.h"

#include <algorithm>
#include <string>
#include <vector>

// ── The task board's team half (TaskBoardTeam.h) ─────────────────────────────
// Two (or more) editors over a fake team server: who hears about an
// assignment, a status change and a comment, that an assignment settles before
// it goes, that a member who is not online gets it once they are and that the
// outbox gives up after its time, that nothing goes without a signed-in link,
// and that what arrives is a notice — the bell and the recent list — never an
// edit of the receiver's board. All on one forward-only fake clock.

using HE::Task;
using HE::TaskBoard;
using HE::TaskBoardTeam;
using HE::TaskNotice;
using HE::TaskNoticeKind;
using HE::TaskStatus;
using HE::TaskUpdate;
using HE::TaskUpdateKind;
using he_test::FakeTaskTeam;

namespace
{
	constexpr std::int64_t kWall = 1'790'000'000'000;   // a wall-clock time on the board

	// One editor: its link, its bridge, its bell, and the board it edits.
	struct Editor
	{
		FakeTaskTeam::Link&   link;
		TaskBoardTeam         bridge;
		HE::Ed::NotificationStore bell;
		TaskBoard             board;

		Editor(FakeTaskTeam& team, const std::string& member) : link(team.editor(member))
		{
			bridge.setLink(&link);
			bridge.setNotifications(&bell);
		}

		// What TaskBoardPanel::edit() does around a saved edit.
		template <typename F>
		void edit(F&& fn, std::uint64_t nowMs, const std::string& project = "Harbour")
		{
			const TaskBoard before = board;
			fn(board);
			bridge.afterEdit(before, board, project, nowMs);
		}

		std::vector<std::string> bellTexts() const
		{
			std::vector<std::string> out;
			for (const auto& n : bell.snapshot()) out.push_back(n.text);
			return out;
		}
	};

	std::vector<HE::TaskTeamMember> team(std::initializer_list<const char*> names)
	{
		std::vector<HE::TaskTeamMember> out;
		for (const char* n : names) out.push_back({ n, true });
		return out;
	}
}

TEST_CASE("task notice: what is sent reads back, and what is not a task notice is refused")
{
	TaskNotice n;
	n.kind      = TaskNoticeKind::Assigned;
	n.project   = "Harbour";
	n.task      = HE::UUID::generate();
	n.update    = HE::UUID::generate();
	n.title     = "Retexture the crates \"B\" (pasted copy)";
	n.from      = "Anna";
	n.assignees = { "Bob", "Cleo" };
	n.text      = "ünïcødé ✓";
	n.timeMs    = kWall + 120;

	TaskNotice back;
	REQUIRE(HE::decodeTaskNotice(HE::encodeTaskNotice(n), back));
	CHECK(back.kind == TaskNoticeKind::Assigned);
	CHECK(back.project == "Harbour");
	CHECK(back.task == n.task);
	CHECK(back.update == n.update);
	CHECK(back.title == n.title);
	CHECK(back.from == "Anna");
	CHECK(back.assignees == std::vector<std::string>{ "Bob", "Cleo" });
	CHECK(back.text == n.text);
	CHECK(back.timeMs == n.timeMs);

	// A status, with its word.
	n.kind = TaskNoticeKind::Status;
	n.status = "in-progress";
	REQUIRE(HE::decodeTaskNotice(HE::encodeTaskNotice(n), back));
	CHECK(back.kind == TaskNoticeKind::Status);
	CHECK(back.status == "in-progress");
	CHECK(back.assignees.empty());   // only an assignment carries them

	// A newer editor's kind and version: kept as a word, read as far as known.
	REQUIRE(HE::decodeTaskNotice(R"J({"type":"task-notice","version":3,"kind":"reaction","task":"0123456789abcdef0123456789abcdef","update":"fedcba9876543210fedcba9876543210","from":"Bob","emoji":"+1"})J", back));
	CHECK(back.kind == TaskNoticeKind::Other);
	CHECK(back.kindOther == "reaction");
	CHECK(back.version == 3);
	CHECK(back.from == "Bob");

	// Not ours: garbage, another JSON message, a task notice without its ids.
	CHECK_FALSE(HE::decodeTaskNotice("", back));
	CHECK_FALSE(HE::decodeTaskNotice("not json {", back));
	CHECK_FALSE(HE::decodeTaskNotice("[1,2]", back));
	CHECK_FALSE(HE::decodeTaskNotice(R"J({"type":"chat","text":"hi"})J", back));
	CHECK_FALSE(HE::decodeTaskNotice(R"J({"type":"task-notice","kind":"comment","task":"xyz","update":"fedcba9876543210fedcba9876543210"})J", back));

	// A comment that is not valid UTF-8 still goes out instead of throwing.
	n.text = std::string("bad \xC3 byte");
	CHECK_FALSE(HE::encodeTaskNotice(n).empty());
}

TEST_CASE("task notice recipients: everyone the task concerns, never yourself, only the team, once each")
{
	TaskBoard b;
	Task* t = b.add("Crates", "anna", kWall);   // created under another spelling
	REQUIRE(t);
	const HE::UUID id = t->id;
	REQUIRE(b.assign(id, { "Bob" }, "Anna", kWall + 1));
	REQUIRE(b.comment(id, "Dave", "I can help", kWall + 2));
	REQUIRE(b.comment(id, "connor", "from before the team server", kWall + 3));   // not a member
	t = b.find(id);

	const auto members = team({ "Anna", "Bob", "Cleo", "Dave", "Eve" });

	// Seen from Bob, about the last comment: Anna (creator, in the team's
	// spelling, once though she is creator and assigner) and Dave (wrote in
	// the history). Not Bob, not Cleo
	// or Eve (nothing to do with it), not "connor" (not on the team).
	const TaskUpdate& last = t->updates.back();
	auto r = HE::taskNoticeRecipients(*t, last, {}, "bob", members);
	CHECK(r == std::vector<std::string>{ "Anna", "Dave" });

	// An assignment that takes it from Bob and gives it to Cleo: Bob hears it too.
	REQUIRE(b.assign(id, { "Cleo" }, "Anna", kWall + 4));
	t = b.find(id);
	r = HE::taskNoticeRecipients(*t, t->updates.back(), { "Bob" }, "Anna", members);
	CHECK(r == std::vector<std::string>{ "Cleo", "Bob", "Dave" });
	// The previous assignees count only for an assignment.
	TaskUpdate comment;
	comment.kind = TaskUpdateKind::Comment;
	r = HE::taskNoticeRecipients(*t, comment, { "Eve" }, "Anna", members);
	CHECK(std::find(r.begin(), r.end(), "Eve") == r.end());

	// Alone on a task: nobody to tell.
	TaskBoard solo;
	Task* s = solo.add("Mine", "Anna", kWall);
	REQUIRE(solo.setStatus(s->id, TaskStatus::Done, "Anna", kWall + 1));
	s = solo.find(s->id);
	CHECK(HE::taskNoticeRecipients(*s, s->updates.back(), {}, "Anna", members).empty());

	// The sentence, from the receiver's side.
	TaskNotice n;
	n.kind = TaskNoticeKind::Assigned; n.from = "Anna"; n.title = "Crates"; n.assignees = { "Bob" };
	CHECK(HE::describeTaskNotice(n, "bob") == "Anna assigned you “Crates”");
	CHECK(HE::describeTaskNotice(n, "Cleo") == "Anna assigned “Crates” to Bob");
	n.assignees.clear();
	CHECK(HE::describeTaskNotice(n, "Bob") == "Anna unassigned “Crates”");
	n.kind = TaskNoticeKind::Status; n.status = "in-progress";
	CHECK(HE::describeTaskNotice(n, "Bob") == "Anna set “Crates” to In Progress");
	n.kind = TaskNoticeKind::Comment;
	CHECK(HE::describeTaskNotice(n, "Bob") == "Anna commented on “Crates”");
}

TEST_CASE("task board team: an assignment reaches every editor of the assignee once, after it settles")
{
	FakeTaskTeam server;
	for (const char* m : { "Anna", "Bob", "Cleo" }) server.addMember(m);
	Editor anna(server, "Anna");
	Editor bob(server, "Bob");
	Editor bobLaptop(server, "Bob");   // Bob is signed in twice
	Editor cleo(server, "Cleo");
	auto frame = [&](std::uint64_t ms) { server.pump(ms, { &anna.bridge, &bob.bridge, &bobLaptop.bridge, &cleo.bridge }); };

	HE::UUID id;
	anna.edit([&](TaskBoard& b) { id = b.add("Retexture the crates", "Anna", kWall)->id; }, server.nowMs);
	CHECK(anna.bridge.pending() == 0);   // made, assigned to nobody: nobody to tell
	bob.board = anna.board;              // the pull: everyone has the task
	cleo.board = anna.board;

	// The picker: Bob, then Bob and Cleo, then only Bob again — three edits in a
	// second. Bob hears the result once; Cleo, who was in it for a moment, hears
	// that it went to Bob (it was taken from her).
	anna.edit([&](TaskBoard& b) { b.assign(id, { "Bob" }, "Anna", kWall + 1); }, server.nowMs);
	frame(300);
	anna.edit([&](TaskBoard& b) { b.assign(id, { "Bob", "Cleo" }, "Anna", kWall + 2); }, server.nowMs);
	frame(300);
	anna.edit([&](TaskBoard& b) { b.assign(id, { "Bob" }, "Anna", kWall + 3); }, server.nowMs);
	CHECK(anna.bridge.pending() == 2);   // one for Bob, one for Cleo

	// Nothing goes before the last pick has settled.
	frame(TaskBoardTeam::kAssignSettleMs - 1);
	CHECK(anna.link.sent.empty());
	frame(1);
	CHECK(anna.link.sent.size() == 2);
	CHECK(anna.bridge.pending() == 0);
	frame(server.latencyMs);

	CHECK(bob.bellTexts() == std::vector<std::string>{ "Anna assigned you “Retexture the crates”" });
	CHECK(bobLaptop.bellTexts() == bob.bellTexts());
	CHECK(cleo.bellTexts() == std::vector<std::string>{ "Anna assigned “Retexture the crates” to Bob" });
	CHECK(anna.bellTexts().empty());   // never your own

	// The detail line says which project.
	REQUIRE(bob.bell.snapshot().size() == 1);
	CHECK(bob.bell.snapshot()[0].detail.find("Harbour") != std::string::npos);

	// The recent list has the task's address, for the panel to open it.
	REQUIRE(bob.bridge.recent().size() == 1);
	CHECK(bob.bridge.recent().front().notice.task == id);
	CHECK(bob.bridge.recent().front().notice.kind == TaskNoticeKind::Assigned);
	TaskBoardTeam::Received got;
	CHECK(bob.bridge.pollNotice(got));
	CHECK_FALSE(bob.bridge.pollNotice(got));

	// A notice is not a sync: Bob's board still has the task unassigned until he pulls.
	CHECK(bob.board.find(id)->assignees.empty());
	CHECK(bob.board.find(id)->updates.empty());

	frame(5'000);
	CHECK(server.carried == 3);   // Bob ×2, Cleo — and nothing more later
}

TEST_CASE("task board team: a status change and a comment go at once, to the people involved only")
{
	FakeTaskTeam server;
	for (const char* m : { "Anna", "Bob", "Dave" }) server.addMember(m);
	Editor anna(server, "Anna");
	Editor bob(server, "Bob");
	Editor dave(server, "Dave");
	auto frame = [&](std::uint64_t ms) { server.pump(ms, { &anna.bridge, &bob.bridge, &dave.bridge }); };

	HE::UUID id;
	anna.edit([&](TaskBoard& b) {
		id = b.add("Fix the harbour lights", "Anna", kWall)->id;
		b.assign(id, { "Bob" }, "Anna", kWall + 1);
	}, server.nowMs);
	frame(TaskBoardTeam::kAssignSettleMs);
	frame(server.latencyMs);
	REQUIRE(bob.bellTexts().size() == 1);
	bob.board = anna.board;   // pulled
	bob.bell.clear();

	// Bob starts it and says so: two notices for Anna, sent in the next frame.
	bob.edit([&](TaskBoard& b) { b.setStatus(id, TaskStatus::InProgress, "Bob", kWall + 10, "on it"); }, server.nowMs);
	bob.edit([&](TaskBoard& b) { b.comment(id, "Bob", "The bulbs are in the crate by the door.", kWall + 11); }, server.nowMs);
	frame(1);
	CHECK(bob.link.sent.size() == 2);
	frame(server.latencyMs);
	CHECK(anna.bellTexts() == std::vector<std::string>{
		"Bob set “Fix the harbour lights” to In Progress",
		"Bob commented on “Fix the harbour lights”" });
	// The comment's text is the detail line.
	CHECK(anna.bell.snapshot()[1].detail.find("The bulbs are in the crate") == 0);
	// Dave has nothing to do with it.
	CHECK(dave.bellTexts().empty());
	CHECK(dave.link.received == 0);

	// Edits that are not a story (title, priority, a link) announce nothing.
	bob.edit([&](TaskBoard& b) {
		b.setTitle(id, "Fix the harbour lights (east pier)", kWall + 12);
		b.setPriority(id, HE::TaskPriority::High, kWall + 13);
	}, server.nowMs);
	CHECK(bob.bridge.pending() == 0);

	// An update that came in with a pull is somebody else's to announce: Bob
	// pulls Anna's comment, then makes an edit of his own — only his goes out.
	TaskBoard pulled = bob.board;
	pulled.comment(id, "Anna", "thanks", kWall + 20);
	bob.board = pulled;
	const std::size_t before = bob.link.sent.size();
	bob.edit([&](TaskBoard& b) { b.setStatus(id, TaskStatus::Done, "Bob", kWall + 21); }, server.nowMs);
	frame(1);
	REQUIRE(bob.link.sent.size() == before + 1);
	TaskNotice n;
	REQUIRE(HE::decodeTaskNotice(bob.link.sent.back().second, n));
	CHECK(n.kind == TaskNoticeKind::Status);
	CHECK(n.status == "done");
	CHECK(n.from == "Bob");
}

TEST_CASE("task board team: a member who is not online gets it when they are; the outbox gives up after its time")
{
	FakeTaskTeam server;
	for (const char* m : { "Anna", "Bob", "Cleo" }) server.addMember(m);
	Editor anna(server, "Anna");
	Editor bob(server, "Bob");
	Editor cleo(server, "Cleo");
	bob.link.signedIn = false;    // Bob's editor is closed
	cleo.link.signedIn = false;   // and so is Cleo's
	auto frame = [&](std::uint64_t ms) { server.pump(ms, { &anna.bridge, &bob.bridge, &cleo.bridge }); };

	HE::UUID a, c;
	anna.edit([&](TaskBoard& b) {
		a = b.add("Crates", "Anna", kWall)->id;
		c = b.add("Lights", "Anna", kWall)->id;
		b.assign(a, { "Bob" }, "Anna", kWall + 1);
		b.assign(c, { "Cleo" }, "Anna", kWall + 2);
	}, server.nowMs);
	frame(TaskBoardTeam::kAssignSettleMs + 10);
	CHECK(anna.link.sent.empty());       // nobody to give it to yet
	CHECK(anna.bridge.pending() == 2);

	// Anna changes Bob's task again while he is away: the held notice is
	// replaced, and he hears how it stands, once.
	anna.edit([&](TaskBoard& b) { b.assign(a, { "Bob", "Anna" }, "Anna", kWall + 3); }, server.nowMs);
	CHECK(anna.bridge.pending() == 2);

	// Bob opens his editor five minutes later: his notice goes, Cleo's waits.
	frame(5 * 60'000);
	bob.link.signedIn = true;
	frame(16);
	frame(server.latencyMs);
	CHECK(bob.bellTexts() == std::vector<std::string>{ "Anna assigned you “Crates”" });
	CHECK(bob.link.received == 1);
	REQUIRE(bob.bridge.recent().size() == 1);
	CHECK(bob.bridge.recent().front().notice.assignees == std::vector<std::string>{ "Bob", "Anna" });
	CHECK(anna.bridge.pending() == 1);

	// Cleo stays away past the outbox's time: dropped, and never delivered
	// later — by then the assignment is in the file she pulls.
	frame(TaskBoardTeam::kOutboxTtlMs);
	CHECK(anna.bridge.pending() == 0);
	cleo.link.signedIn = true;
	frame(16);
	frame(server.latencyMs);
	CHECK(cleo.bellTexts().empty());
	CHECK(anna.link.sent.size() == 1);
}

TEST_CASE("task board team: without a signed-in link the board is local and nothing waits")
{
	FakeTaskTeam server;
	for (const char* m : { "Anna", "Bob" }) server.addMember(m);

	// No link at all: the default, without a team server.
	{
		TaskBoardTeam local;
		CHECK_FALSE(local.connected());
		CHECK(local.self().empty());
		CHECK(local.members().empty());
		TaskBoard before, after;
		const HE::UUID id = after.add("Crates", "connor", kWall)->id;
		after.assign(id, { "Bob" }, "connor", kWall + 1);
		local.afterEdit(before, after, "Harbour", server.nowMs);
		local.update(server.nowMs + 60'000);
		CHECK(local.pending() == 0);
		CHECK(local.recent().empty());
	}

	Editor anna(server, "Anna");
	Editor bob(server, "Bob");
	auto frame = [&](std::uint64_t ms) { server.pump(ms, { &anna.bridge, &bob.bridge }); };

	// Signed out: an edit made now is not announced later as if it were news.
	anna.link.signedIn = false;
	CHECK_FALSE(anna.bridge.connected());
	CHECK(anna.bridge.members().empty());   // the panel shows no assignee field
	HE::UUID id;
	anna.edit([&](TaskBoard& b) {
		id = b.add("Crates", "Anna", kWall)->id;
		b.assign(id, { "Bob" }, "Anna", kWall + 1);
	}, server.nowMs);
	CHECK(anna.bridge.pending() == 0);
	anna.link.signedIn = true;
	frame(TaskBoardTeam::kAssignSettleMs + server.latencyMs + 10);
	CHECK(bob.bellTexts().empty());

	// Signed in when the edit was made, then the link drops before it goes:
	// it waits, and goes once the link is back.
	anna.edit([&](TaskBoard& b) { b.comment(id, "Anna", "first the big ones", kWall + 5); }, server.nowMs);
	CHECK(anna.bridge.pending() == 1);
	anna.link.signedIn = false;
	frame(20'000);
	CHECK(anna.bridge.pending() == 1);
	// Back, but the roster is not in yet: that is not "Bob left the team".
	anna.link.signedIn = true;
	anna.link.rosterIn = false;
	frame(16);
	CHECK(anna.bridge.pending() == 1);
	CHECK(anna.link.sent.empty());
	anna.link.rosterIn = true;
	frame(16);
	frame(server.latencyMs);
	CHECK(bob.bellTexts() == std::vector<std::string>{ "Anna commented on “Crates”" });
}

TEST_CASE("task board team: a notice delivered twice is shown once, and garbage on the wire is ignored")
{
	FakeTaskTeam server;
	for (const char* m : { "Anna", "Bob" }) server.addMember(m);
	Editor bob(server, "Bob");

	TaskNotice n;
	n.kind = TaskNoticeKind::Comment;
	n.task = HE::UUID::generate();
	n.update = HE::UUID::generate();
	n.title = "Crates";
	n.from = "Mallory";   // the server's word for the sender wins
	n.text = "hello";
	const std::string payload = HE::encodeTaskNotice(n);

	bob.link.inbox.push_back({ "Anna", payload });
	bob.link.inbox.push_back({ "Anna", payload });   // a reconnect replayed it
	bob.link.inbox.push_back({ "Anna", "{\"type\":\"chat\"}" });
	bob.link.inbox.push_back({ "Anna", "\x01\x02 not json" });
	TaskNotice mine = n;
	mine.update = HE::UUID::generate();
	bob.link.inbox.push_back({ "bob", HE::encodeTaskNotice(mine) });   // our own echo
	server.pump(16, { &bob.bridge });

	CHECK(bob.bellTexts() == std::vector<std::string>{ "Anna commented on “Crates”" });
	REQUIRE(bob.bridge.recent().size() == 1);
	CHECK(bob.bridge.recent().front().notice.from == "Anna");

	// The recent list is bounded, newest first.
	for (std::size_t i = 0; i < TaskBoardTeam::kRecentMax + 5; ++i)
	{
		TaskNotice k = n;
		k.update = HE::UUID::generate();
		k.title = "Task " + std::to_string(i);
		bob.link.inbox.push_back({ "Anna", HE::encodeTaskNotice(k) });
	}
	server.pump(16, { &bob.bridge });
	CHECK(bob.bridge.recent().size() == TaskBoardTeam::kRecentMax);
	CHECK(bob.bridge.recent().front().notice.title == "Task " + std::to_string(TaskBoardTeam::kRecentMax + 4));
}
