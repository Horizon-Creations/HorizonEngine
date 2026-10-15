#include "doctest.h"
#include "FakeScServer.h"

#include <Net/ScClient.h>
#include <Net/TeamClient.h>
#include <SourceControl/TeamSyncService.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Saving through the team server (topic 199, step 4;
// docs/team-server-source-control-design-2026-10-13.md sections 6 and 12).
//
// HE::Net::ScClient speaks the sync connection, HE::Sc::TeamSyncService keeps
// the per-file bases and runs upload / reconcile on its worker. Both run here
// against FakeScServer (FakeScServer.h) on the fake team server's port, with
// the tickets coming from a real TeamClient logged in to it, as in the editor.
// The case at the bottom runs two members against the real collab_server
// (HE_TEST_TEAM_SERVER=host:port), where scenes are merged by entity.

using HE::Sc::TeamSyncService;
namespace fs = std::filesystem;

namespace {

constexpr auto kSyncDeadline = std::chrono::seconds(30);

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "<none>";
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeText(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

// A project folder of its own per member and run (parallel ctest runs share /tmp).
fs::path scratchDir(const std::string& who) {
    static std::atomic<int> n{ 0 };
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path p = fs::temp_directory_path() /
                 ("he-team-sync-" + who + "-" + std::to_string(stamp) + "-" + std::to_string(n++));
    fs::create_directories(p);
    return p;
}

TeamSyncService::TicketFn ticketsFrom(TeamClient& client) {
    return [&client](const std::string& project, TeamSyncService::Deliver deliver) {
        if (!client.loggedIn()) return false;
        return client.scTicket(project, [&client, deliver](const TeamClient::Reply& r) {
            deliver(r.ticket, r.ok() ? std::string() : TeamClient::describeResult(r.result, r.text),
                    r.result == TeamClient::Result::NoSuchProject, client.isAdmin());
        }) != 0;
    };
}

// One editor: a project folder, a team connection, the sync service.
struct Member {
    std::string                 name;
    fs::path                    root;
    std::unique_ptr<TeamClient> team;
    TeamSyncService             sync;
    std::vector<std::string>    written;    // what pump() wrote, in order
    std::vector<std::string>    notices;

    explicit Member(std::string n) : name(std::move(n)), root(scratchDir(name)) {}
    ~Member() {
        sync.stop();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void start(const RelayAddress& at, const std::string& password, const std::string& teamName = "studio",
               const std::string& project = "game") {
        team = TeamClient::connect(at, teamName, name, password);
        REQUIRE(team);
        TeamSyncService::Settings s;
        s.projectRoot = root;
        s.host        = at.host;
        s.port        = at.port;
        s.team        = teamName;
        s.member      = name;
        s.project     = project;
        sync.start(s, ticketsFrom(*team));
    }
    void pump() {
        team->update();
        TeamClient::Event ev;
        while (team->pollEvent(ev))
            if (ev.kind == TeamClient::EventKind::ScHead && ev.project == sync.settings().project)
                sync.notifyHead(ev.rev);
        for (const std::string& p : sync.pump()) written.push_back(p);
        std::string text;
        bool error = false;
        while (sync.pollNotice(text, error)) notices.push_back(text);
    }
    std::string file(const std::string& rel) const { return readText(root / rel); }
    void put(const std::string& rel, const std::string& text) const { writeText(root / rel, text); }
    TeamSyncService::Status st() const { return sync.status(); }
    bool settled(std::uint64_t rev) const {
        const auto s = st();
        return s.connected && s.rev == rev && s.phase == TeamSyncService::Phase::Idle && s.activity.empty();
    }
};

template <typename Fn>
bool pumpSync(FakeTeamServer* server, std::vector<Member*> members, Fn done) {
    const auto deadline = std::chrono::steady_clock::now() + kSyncDeadline;
    for (;;) {
        if (server) server->pump();
        for (Member* m : members) m->pump();
        if (done()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Anna saves door.txt and settings.json as r1; Bob, starting empty, gets both.
void startInSync(FakeTeamServer& server, FakeScServer& sc, Member& anna, Member& bob) {
    anna.put("Content/door.txt", "closed\n");
    anna.put("Config/settings.json", "{}\n");
    anna.put("Saved/Autosave/Main.hescene", "{ \"autosave\": true }\n");   // never uploaded
    anna.put(".DS_Store", "finder noise");                                   // generated .gitignore
    anna.start(server.address(), "correct-horse");
    bob.start(server.address(), "correct-horse");
    REQUIRE(pumpSync(&server, { &anna, &bob }, [&] { return anna.settled(0) && bob.settled(0); }));
    CHECK(anna.st().waiting == 2);

    anna.sync.requestSync();
    REQUIRE(pumpSync(&server, { &anna, &bob }, [&] {
        return anna.settled(1) && anna.st().waiting == 0 && bob.settled(1) &&
               bob.file("Content/door.txt") == "closed\n";
    }));
    REQUIRE(sc.treesOf("game").back().size() == 2);
    CHECK(sc.text("game", "Config/settings.json") == "{}\n");
    CHECK(bob.file("Config/settings.json") == "{}\n");
    CHECK(bob.st().waiting == 0);
}

} // namespace

TEST_CASE("Team sync: wire bytes of the sync connection")
{
    const std::string ticket(16, '\x07');
    std::vector<std::uint8_t> hello = header(10);
    str8(hello, "studio");
    str8(hello, "Anna");
    str8(hello, "game");
    hello.insert(hello.end(), ticket.begin(), ticket.end());
    CHECK(ScClient::encodeHello("studio", "Anna", "game", ticket) == hello);

    std::uint8_t raw[32];
    CHECK(ScClient::hexToBytes(std::string(64, 'a'), raw));
    CHECK(raw[0] == 0xaa);
    CHECK_FALSE(ScClient::hexToBytes(std::string(64, 'A'), raw));   // the server writes lower case
    CHECK_FALSE(ScClient::hexToBytes("abc", raw));
    CHECK(std::string(ScClient::resultName(12)) == "hash-mismatch");
    CHECK(std::string(TeamClient::resultName(18)) == "no-such-project");

    // No ticket, a bad name: refused before any socket.
    CHECK_FALSE(ScClient::connect(RelayAddress{ "127.0.0.1", 7777 }, "studio", "Anna", "game", "short"));
    CHECK_FALSE(ScClient::connect(RelayAddress{ "127.0.0.1", 7777 }, "studio", "Anna", "a b", ticket));
}

TEST_CASE("Team sync: trailer and ignore parsing")
{
    std::string outcome, path, detail;
    REQUIRE(TeamSyncService::parseMerge(
        "merged-arrival Scenes/Main Level.hescene against r4 by Alex (1 fields) "
        "lost=horizon-conflicts:r5/Scenes/Main Level.hescene", outcome, path, detail));
    CHECK(outcome == "merged-arrival");
    CHECK(path == "Scenes/Main Level.hescene");
    CHECK(detail == "against r4 by Alex (1 fields) lost=horizon-conflicts:r5/Scenes/Main Level.hescene");
    REQUIRE(TeamSyncService::parseMerge("applied Scripts/door.lua", outcome, path, detail));
    CHECK(path == "Scripts/door.lua");
    CHECK(detail.empty());

    std::uint64_t rev = 0;
    std::string   who;
    CHECK(TeamSyncService::parseAgainst(detail = "against r41 by connor (2 fields) lost=x", rev, who));
    CHECK(rev == 41);
    CHECK(who == "connor");
    CHECK(TeamSyncService::parseAgainst("changed in r7 by Bob", rev, who));
    CHECK(rev == 7);
    CHECK_FALSE(TeamSyncService::parseAgainst("(both added it)", rev, who));

    TeamSyncService::Revision r;
    r.rev    = 9;
    r.member = "Bob";
    r.merges = { "applied Content/a.txt",
                 "replaced Content/door.hmat against r8 by anna lost=horizon-conflicts:r9/Content/door.hmat",
                 "merged-arrival Scenes/Main.hescene against r6 by Cleo (1 fields)",
                 "delete-dropped Content/old.hasset changed in r8 by Anna" };
    const auto lost = TeamSyncService::lostFor(r, "Anna");
    REQUIRE(lost.size() == 1);
    CHECK(lost[0].path == "Content/door.hmat");
    CHECK(lost[0].ourRev == 8);
    CHECK(lost[0].winner == "Bob");
    CHECK(TeamSyncService::lostFor(r, "bob").empty());   // nobody loses to themselves

    TeamSyncService::IgnoreRules rules;
    rules.addText("Saved/\n*.log\nSource/build/\nGameLogic.hot-*.*\n# comment\n!keep.log\n");
    CHECK(rules.ignored("Saved", true));
    CHECK(rules.ignored("Content/Saved", true));       // a "dir/" line matches at any level
    CHECK_FALSE(rules.ignored("Saved", false));        // ...but only directories
    CHECK(rules.ignored("Logs/run.log", false));
    CHECK(rules.ignored("Source/build", true));
    CHECK_FALSE(rules.ignored("Content/Source/build", true));   // anchored
    CHECK(rules.ignored("GameLogic.hot-3.dylib", false));
    CHECK(rules.ignored(".git", true));
    CHECK_FALSE(rules.ignored("Content/door.txt", false));
}

TEST_CASE("Team sync: a ticket and the sc-head event on the team connection")
{
    FakeTeamServer server;
    auto anna = TeamClient::connect(server.address(), "studio", "Anna", "correct-horse");
    auto bob  = TeamClient::connect(server.address(), "studio", "Bob", "correct-horse");
    std::uint64_t now = 1;   // 0 would read as "before the start" to the timeout
    auto pumpBoth = [&](auto done) {
        const auto deadline = std::chrono::steady_clock::now() + kSyncDeadline;
        for (;;) {
            server.pump();
            anna->update(now);
            bob->update(now);
            if (done()) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            now += 16;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    REQUIRE(pumpBoth([&] { return anna->loggedIn() && bob->loggedIn(); }));

    TeamClient::Reply got, refused, created;
    int replies = 0;
    anna->scTicket("game", [&](const TeamClient::Reply& r) { got = r; ++replies; });
    bob->scTicket("other", [&](const TeamClient::Reply& r) { refused = r; ++replies; });
    anna->scTicket("other", [&](const TeamClient::Reply& r) { created = r; ++replies; });
    REQUIRE(pumpBoth([&] { return replies == 3; }));
    CHECK(got.ok());
    CHECK(got.ticket.size() == TeamClient::kScTicketSize);
    CHECK(server.scTickets.count(got.ticket) == 1);
    CHECK(refused.result == TeamClient::Result::NoSuchProject);   // Bob is no admin
    CHECK(created.ok());                                          // an admin may create it

    server.sendToAll(FakeTeamServer::scHeadEvent("game", 0x100000002ull, "Bob"));
    TeamClient::Event ev;
    REQUIRE(pumpBoth([&] { return anna->pollEvent(ev); }));
    CHECK(ev.kind == TeamClient::EventKind::ScHead);
    CHECK(ev.project == "game");
    CHECK(ev.rev == 0x100000002ull);
    CHECK(ev.member == "Bob");
    CHECK(anna->loggedIn());

    // A server from before source control: refused, the connection stays.
    server.noSourceControl = true;
    replies = 0;
    anna->scTicket("game", [&](const TeamClient::Reply& r) { got = r; ++replies; });
    REQUIRE(pumpBoth([&] { return replies == 1; }));
    CHECK(got.result == TeamClient::Result::BadRequest);
    CHECK(anna->loggedIn());
}

TEST_CASE("Team sync: ScClient requests against the fake sync server")
{
    FakeTeamServer server;
    FakeScServer   sc(server);
    auto team = TeamClient::connect(server.address(), "studio", "Anna", "correct-horse");
    std::string ticket;
    team->scTicket("game", [&](const TeamClient::Reply& r) { ticket = r.ticket; });
    std::unique_ptr<ScClient> c;
    auto pump = [&](auto done) {
        const auto deadline = std::chrono::steady_clock::now() + kSyncDeadline;
        for (;;) {
            server.pump();
            team->update();
            if (c) c->update();
            if (done()) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    REQUIRE(pump([&] { return !ticket.empty(); }));

    // A used ticket is refused with bad-login and the connection closes.
    c = ScClient::connect(server.address(), "studio", "Anna", "game", std::string(16, 'x'));
    REQUIRE(c);
    REQUIRE(pump([&] { return c->state() == ScClient::State::Closed; }));
    CHECK(c->closeCode() == std::uint8_t(ScClient::StatusCode::BadLogin));

    c = ScClient::connect(server.address(), "studio", "Anna", "game", ticket);
    REQUIRE(c);
    // Asked before sc-ready: held, then sent in order.
    const std::vector<std::uint8_t> big(3 * 1024 * 1024 + 17, 0x5a);   // more than three SC_DATA frames
    const std::string bigSha = sha256Hex(big);
    std::vector<ScClient::Reply> replies;
    auto keep = [&](ScClient::Reply& r) { replies.push_back(std::move(r)); };
    c->head(keep);
    c->have({ bigSha }, keep);
    c->put(bigSha, big, keep);
    c->get(std::string(64, '0'), keep);   // unknown: refused, the connection stays
    c->commit(0, "first", "A\tContent/big.bin\t-\t" + bigSha + "\n", keep);
    c->get(bigSha, keep);
    c->log(1, 10, keep);
    REQUIRE(pump([&] { return replies.size() == 7 || c->state() == ScClient::State::Closed; }));
    REQUIRE(replies.size() == 7);
    CHECK(c->ready());
    CHECK(replies[0].ok());
    CHECK(std::string(replies[1].stream.begin(), replies[1].stream.end()) == bigSha + "\n");
    CHECK(replies[2].ok());
    CHECK(replies[3].result == ScClient::Result::MissingBlob);
    REQUIRE(replies[4].ok());
    std::size_t pos = 0;
    std::uint64_t rev = 0;
    CHECK(ScClient::readU64(replies[4].body, pos, rev));
    CHECK(rev == 1);
    CHECK(std::string(replies[4].stream.begin(), replies[4].stream.end()) ==
          "applied\tContent/big.bin\t" + bigSha + "\t\n");
    CHECK(replies[5].stream == big);
    CHECK(std::string(replies[6].stream.begin(), replies[6].stream.end()).rfind("R\t1\tAnna\t", 0) == 0);
    CHECK(sc.puts == 1);
}

TEST_CASE("Team sync: a save goes up and reaches the other member")
{
    FakeTeamServer server;
    FakeScServer   sc(server);
    Member anna("Anna"), bob("Bob");
    startInSync(server, sc, anna, bob);
    CHECK(bob.file("Saved/Autosave/Main.hescene") == "<none>");
    CHECK(bob.file(".DS_Store") == "<none>");
    CHECK(fs::exists(anna.root / "Saved" / "TeamSync" / "studio-game.state"));

    // Nothing changed: a sync sends no COMMIT.
    const int commits = sc.commits;
    anna.sync.requestSync();
    REQUIRE(pumpSync(&server, { &anna, &bob }, [&] { return anna.settled(1); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(pumpSync(&server, { &anna, &bob }, [&] { return anna.settled(1); }));
    CHECK(sc.commits == commits);

    // Bob's change and a deletion go up together as one revision.
    bob.put("Content/door.txt", "open\n");
    fs::remove(bob.root / "Config/settings.json");
    bob.sync.requestSync("door opens");
    REQUIRE(pumpSync(&server, { &anna, &bob }, [&] {
        return bob.settled(2) && anna.settled(2) && anna.file("Content/door.txt") == "open\n" &&
               !fs::exists(anna.root / "Config/settings.json");
    }));
    CHECK(sc.head("game") == 2);
    CHECK(sc.logOf("game")[2].message.find("door opens") != std::string::npos);
    const auto results = bob.sync.lastResults();
    REQUIRE(results.size() == 2);
    CHECK(results[0].outcome == "applied");
    CHECK(anna.notices.empty());

    // A restarted editor picks up from its state file: no first-sync question.
    anna.sync.stop();
    anna.start(server.address(), "correct-horse");
    REQUIRE(pumpSync(&server, { &anna }, [&] { return anna.settled(2); }));
    CHECK(anna.st().firstSyncDiffers.empty());
    CHECK(anna.st().waiting == 0);
}

TEST_CASE("Team sync: both change one file — the later save wins, the earlier one is kept")
{
    FakeTeamServer server;
    FakeScServer   sc(server);
    Member anna("Anna"), bob("Bob");
    startInSync(server, sc, anna, bob);

    anna.put("Content/door.txt", "anna's door\n");
    bob.put("Content/door.txt", "bob's door\n");
    anna.sync.requestSync();
    REQUIRE(pumpSync(&server, { &anna, &bob }, [&] { return anna.settled(2); }));
    // Bob changed it too: the pull leaves his copy alone (merged at his save).
    REQUIRE(pumpSync(&server, { &anna, &bob }, [&] { return bob.settled(2); }));
    CHECK(bob.file("Content/door.txt") == "bob's door\n");
    CHECK(bob.st().waiting == 1);

    bob.sync.requestSync();
    REQUIRE(pumpSync(&server, { &anna, &bob }, [&] {
        return bob.settled(3) && anna.settled(3) && anna.file("Content/door.txt") == "bob's door\n";
    }));
    const auto results = bob.sync.lastResults();
    REQUIRE(results.size() == 1);
    CHECK(results[0].outcome == "replaced");
    CHECK(results[0].detail.find("against r2 by Anna") == 0);

    // Anna is told, and gets her version back from the server.
    REQUIRE(pumpSync(&server, { &anna, &bob }, [&] { return !anna.sync.lost().empty() && !anna.notices.empty(); }));
    const auto lost = anna.sync.lost();
    CHECK(lost[0].path == "Content/door.txt");
    CHECK(lost[0].rev == 3);
    CHECK(lost[0].ourRev == 2);
    CHECK(lost[0].winner == "Bob");
    CHECK(anna.notices.back().find("Content/door.txt: Bob's later save replaced your version") == 0);
    CHECK(bob.sync.lost().empty());

    anna.sync.fetchLost(lost[0]);
    fs::path opened;
    REQUIRE(pumpSync(&server, { &anna, &bob }, [&] { return anna.sync.takeOpened(opened); }));
    CHECK(opened == anna.root / "Saved" / "TeamSync" / "Lost" / "r3" / "Content" / "door.txt");
    CHECK(readText(opened) == "anna's door\n");
    CHECK(anna.file("Content/door.txt") == "bob's door\n");   // the project keeps the winner
}

TEST_CASE("Team sync: a deletion against a change keeps the change")
{
    FakeTeamServer server;
    FakeScServer   sc(server);
    Member anna("Anna"), bob("Bob");
    startInSync(server, sc, anna, bob);

    bob.put("Content/door.txt", "bob's door\n");
    bob.sync.requestSync();
    REQUIRE(pumpSync(&server, { &bob }, [&] { return bob.settled(2); }));   // Anna does not pump: no pull yet
    fs::remove(anna.root / "Content/door.txt");
    anna.sync.requestSync();
    REQUIRE(pumpSync(&server, { &anna, &bob }, [&] {
        return anna.settled(2) && anna.file("Content/door.txt") == "bob's door\n";
    }));
    const auto results = anna.sync.lastResults();
    REQUIRE(results.size() == 1);
    CHECK(results[0].outcome == "delete-dropped");
    CHECK(results[0].detail == "changed in r2 by Bob");
    CHECK(sc.head("game") == 2);   // nothing new for the server
}

TEST_CASE("Team sync: a busy server and a first sync that differs")
{
    FakeTeamServer server;
    FakeScServer   sc(server);
    Member anna("Anna"), bob("Bob");
    startInSync(server, sc, anna, bob);

    // busy: retried on its own.
    anna.put("Content/door.txt", "ajar\n");
    sc.busyNextCommit = true;
    anna.sync.requestSync();
    REQUIRE(pumpSync(&server, { &anna, &bob }, [&] { return anna.settled(2) && bob.settled(2); }));
    CHECK(bob.file("Content/door.txt") == "ajar\n");

    // A second machine of Bob's with an older, different copy and no state.
    Member bob2("Bob");
    bob2.put("Content/door.txt", "closed\n");
    bob2.put("Content/only-here.txt", "new\n");
    bob2.start(server.address(), "correct-horse");
    REQUIRE(pumpSync(&server, { &anna, &bob, &bob2 }, [&] {
        return !bob2.st().firstSyncDiffers.empty() && bob2.st().phase == TeamSyncService::Phase::Waiting;
    }));
    CHECK(bob2.st().firstSyncDiffers == std::vector<std::string>{ "Content/door.txt" });
    CHECK(bob2.file("Config/settings.json") == "{}\n");   // only on the server: taken
    // A save while the question is open uploads nothing yet...
    const int commits = sc.commits;
    bob2.sync.requestSync();
    for (int i = 0; i < 50; ++i) {
        pumpSync(&server, { &anna, &bob, &bob2 }, [] { return true; });
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(bob2.st().phase == TeamSyncService::Phase::Waiting);
    CHECK(sc.commits == commits);

    // ...and goes up once the team's version has replaced the differing file.
    bob2.sync.resolveFirstSync(false);
    REQUIRE(pumpSync(&server, { &anna, &bob, &bob2 }, [&] {
        return bob2.settled(3) && bob2.file("Content/door.txt") == "ajar\n" &&
               anna.file("Content/only-here.txt") == "new\n";
    }));
    CHECK(bob2.st().firstSyncDiffers.empty());
    CHECK(bob2.st().waiting == 0);
    CHECK(sc.text("game", "Content/door.txt") == "ajar\n");   // nothing of the old copy went up
}

TEST_CASE("Team sync: a session client pauses and takes the team's state after it")
{
    FakeTeamServer server;
    FakeScServer   sc(server);
    Member anna("Anna"), bob("Bob");
    startInSync(server, sc, anna, bob);

    // Bob joins a session: his copy follows the host's, nothing of his goes up.
    bob.sync.setPaused(true);
    bob.put("Content/door.txt", "session state, stale\n");
    bob.sync.requestSync();
    // The host (Anna) uploads the session's state; Bob hears of it but waits.
    anna.put("Content/door.txt", "session state, kept by the host\n");
    anna.sync.requestSync();
    REQUIRE(pumpSync(&server, { &anna, &bob }, [&] { return anna.settled(2); }));
    REQUIRE(pumpSync(&server, { &anna, &bob }, [&] { return bob.st().phase == TeamSyncService::Phase::Waiting; }));
    CHECK(bob.file("Content/door.txt") == "session state, stale\n");
    CHECK(sc.head("game") == 2);

    // Session over: the team's version replaces what the session left here.
    bob.sync.setPaused(false, true);
    REQUIRE(pumpSync(&server, { &anna, &bob }, [&] {
        return bob.settled(2) && bob.file("Content/door.txt") == "session state, kept by the host\n";
    }));
    CHECK(bob.st().waiting == 0);
    CHECK(sc.head("game") == 2);   // Bob uploaded nothing
}

TEST_CASE("Team sync: an admin creates the project, others are told it is missing")
{
    FakeTeamServer server;
    FakeScServer   sc(server);
    server.scProjects.clear();
    Member anna("Anna"), bob("Bob");
    anna.put("Content/door.txt", "closed\n");
    bob.start(server.address(), "correct-horse");
    REQUIRE(pumpSync(&server, { &bob }, [&] {
        return bob.st().phase == TeamSyncService::Phase::Offline && bob.st().projectMissing;
    }));
    CHECK(bob.st().error.find("no such project") != std::string::npos);

    anna.start(server.address(), "correct-horse");
    REQUIRE(pumpSync(&server, { &anna }, [&] { return anna.st().projectMissing && anna.st().connected; }));
    CHECK(anna.st().isAdmin);
    anna.sync.createProject();
    REQUIRE(pumpSync(&server, { &anna }, [&] { return anna.settled(1) && anna.st().waiting == 0; }));
    CHECK(server.scProjects.count("game") == 1);
    CHECK(sc.text("game", "Content/door.txt") == "closed\n");
    CHECK_FALSE(anna.st().projectMissing);
}

// ─── Against the real server ─────────────────────────────────────────────────
// collab_server PORT --data-dir DIR (git on PATH), then
//   HE_TEST_TEAM_SERVER=127.0.0.1:PORT he_tests --source-file='*test_team_sync.cpp'
// Two members edit one scene: different entities merge, the same field goes to
// the later save, and the earlier one learns it lost and gets its version back.

namespace {

std::string sceneJson(int cubeX, int lampX) {
    return "{\n"
           "    \"entities\": [\n"
           "        { \"uuid\": \"100\", \"name\": \"Root\", \"parent\": null, \"children\": [\"101\", \"102\"], \"components\": {} },\n"
           "        { \"uuid\": \"101\", \"name\": \"Cube\", \"parent\": \"100\", \"children\": [], \"components\": { \"Transform\": { \"x\": " +
           std::to_string(cubeX) + " } } },\n"
           "        { \"uuid\": \"102\", \"name\": \"Lamp\", \"parent\": \"100\", \"children\": [], \"components\": { \"Transform\": { \"x\": " +
           std::to_string(lampX) + " } } }\n"
           "    ],\n"
           "    \"version\": 1\n"
           "}\n";
}

// The Transform x of the entity called `name` ("" when there is none). A
// scene the server merged is re-dumped with sorted keys, an applied one is
// stored byte for byte, so this parses instead of searching text.
std::string xOf(const std::string& scene, const std::string& name) {
    const nlohmann::json j = nlohmann::json::parse(scene, nullptr, false);
    if (j.is_discarded() || !j.contains("entities")) return {};
    for (const auto& e : j["entities"])
        if (e.value("name", "") == name && e.contains("components") && e["components"].contains("Transform"))
            return e["components"]["Transform"].value("x", nlohmann::json()).dump();
    return {};
}

} // namespace

TEST_CASE("Team sync: two members merge one scene through a real collab_server (HE_TEST_TEAM_SERVER)")
{
    const char* env = std::getenv("HE_TEST_TEAM_SERVER");
    if (!env || !*env) {
        MESSAGE("HE_TEST_TEAM_SERVER not set — skipped (start collab_server with --data-dir and point it there)");
        return;
    }
    RelayAddress at;
    REQUIRE(parseRelayAddress(env, at));
    const std::string teamName = "he-sync-" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count() % 1000000007);

    // Anna creates the team and takes Bob in, on a plain team connection.
    {
        auto admin = TeamClient::connect(at, teamName, "Anna", "anna-password", TeamClient::LoginOp::CreateTeam);
        REQUIRE(admin);
        bool added = false;
        const auto deadline = std::chrono::steady_clock::now() + kSyncDeadline;
        bool asked = false;
        while (!added && std::chrono::steady_clock::now() < deadline) {
            admin->update();
            if (admin->loggedIn() && !asked) {
                asked = true;
                admin->addMember("Bob", "bob-password", false, [&](const TeamClient::Reply& r) {
                    REQUIRE(r.ok());
                    added = true;
                });
            }
            REQUIRE(admin->state() != TeamClient::State::Closed);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(added);
    }

    Member anna("Anna"), bob("Bob");
    anna.put("Scenes/Main.hescene", sceneJson(0, 0));
    anna.put("Scripts/door.lua", "-- door\nlocal open = false\n");
    anna.start(at, "anna-password", teamName, "game");
    REQUIRE(pumpSync(nullptr, { &anna }, [&] { return anna.st().projectMissing && anna.st().connected; }));
    anna.sync.createProject();
    REQUIRE(pumpSync(nullptr, { &anna }, [&] { return anna.settled(1) && anna.st().waiting == 0; }));

    bob.start(at, "bob-password", teamName, "game");
    REQUIRE(pumpSync(nullptr, { &anna, &bob }, [&] {
        return bob.settled(1) && bob.file("Scripts/door.lua") == "-- door\nlocal open = false\n";
    }));

    // Different entities: both edits end up in both copies.
    anna.put("Scenes/Main.hescene", sceneJson(5, 0));
    bob.put("Scenes/Main.hescene", sceneJson(0, 7));
    anna.sync.requestSync();
    REQUIRE(pumpSync(nullptr, { &anna, &bob }, [&] { return anna.settled(2) && bob.st().rev == 2; }));
    bob.sync.requestSync();
    REQUIRE(pumpSync(nullptr, { &anna, &bob }, [&] {
        return bob.settled(3) && anna.settled(3) && xOf(anna.file("Scenes/Main.hescene"), "Lamp") == "7";
    }));
    CHECK(xOf(bob.file("Scenes/Main.hescene"), "Cube") == "5");   // fetched back: the merge differs from Bob's upload
    CHECK(xOf(anna.file("Scenes/Main.hescene"), "Cube") == "5");
    auto results = bob.sync.lastResults();
    REQUIRE_FALSE(results.empty());
    CHECK(results[0].outcome == "merged");

    // The same field: the later save wins, the earlier member is told.
    anna.put("Scenes/Main.hescene", sceneJson(11, 7));
    bob.put("Scenes/Main.hescene", sceneJson(22, 7));
    anna.sync.requestSync();
    REQUIRE(pumpSync(nullptr, { &anna, &bob }, [&] { return anna.settled(4) && bob.st().rev == 4; }));
    bob.sync.requestSync();
    REQUIRE(pumpSync(nullptr, { &anna, &bob }, [&] {
        return bob.settled(5) && anna.settled(5) && xOf(anna.file("Scenes/Main.hescene"), "Cube") == "22";
    }));
    results = bob.sync.lastResults();
    REQUIRE_FALSE(results.empty());
    CHECK(results[0].outcome == "merged-arrival");
    CHECK(results[0].detail.find("against r4 by Anna") == 0);
    REQUIRE(pumpSync(nullptr, { &anna, &bob }, [&] { return !anna.sync.lost().empty(); }));
    const auto lost = anna.sync.lost();
    CHECK(lost[0].path == "Scenes/Main.hescene");
    CHECK(lost[0].ourRev == 4);
    anna.sync.fetchLost(lost[0]);
    fs::path opened;
    REQUIRE(pumpSync(nullptr, { &anna, &bob }, [&] { return anna.sync.takeOpened(opened); }));
    CHECK(xOf(readText(opened), "Cube") == "11");
}
