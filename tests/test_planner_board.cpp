// The Whiteboard's data, its file and its media (PlannerBoard.h). What it promises a team
// that keeps Config/Planner in git: a round trip changes nothing, a node or key this build
// does not know survives an older build saving it, a damaged file is never saved over, a
// picture put in twice is one file. And the model's own rules: nodes stack, edges need two
// different nodes, a card inside a frame wins a click, a frame takes its contents along.
#include "doctest.h"
#include "TestFsUtil.h"

#include "PlannerBoard.h"
#include "PlannerWorkspace.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace HE;
using namespace HE::Planner;

namespace
{

struct TempProject
{
	fs::path root;
	TempProject()
	{
		root = fs::temp_directory_path() / ("he_test_planner_board_" + uuidToHex(UUID::generate()));
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

constexpr std::int64_t kNow = 1760000000000;

// A board with one of every kind of node and two edges.
Board makeSample()
{
	Board b;
	b.id        = UUID::generate();
	b.title     = "Harbour ideas";
	b.createdBy = "Anna";
	b.createdMs = b.modifiedMs = kNow;
	Node* frame = b.addNode(NodeType::Frame, 0, 0, 800, 600, "Level 1", kNow + 1);
	frame->color = "blue";
	Node* note = b.addNode(NodeType::Text, 40, 60, 240, 120, "Fog rolls in at dusk", kNow + 2);
	note->color = "yellow";
	Node* picture = b.addNode(NodeType::Image, 320, 60, 200, 150, "Mood", kNow + 3);
	picture->asset = hex("00000000000000000000000000000a01");
	Node* pasted = b.addNode(NodeType::Image, 320, 260, 200, 150, "", kNow + 4);
	pasted->media = "0123456789abcdef.png";
	Node* object = b.addNode(NodeType::Object, 560, 60, 180, 40, "", kNow + 5);
	object->link = TaskLink::sceneLink("Content/Levels/Harbour.hescene", "Harbour");
	Node* row = b.addNode(NodeType::Row, 560, 140, 180, 60, "", kNow + 6);
	row->database = hex("00000000000000000000000000000d01");
	row->row = hex("00000000000000000000000000000d02");
	Node* page = b.addNode(NodeType::Page, 560, 240, 180, 60, "", kNow + 7);
	page->page = hex("00000000000000000000000000000e01");
	const UUID n0 = b.nodes[1].id, n1 = b.nodes[2].id, n2 = b.nodes[4].id;
	REQUIRE(b.addEdge(n0, n1, "inspires", kNow + 8) != nullptr);
	Edge* plain = b.addEdge(n1, n2, "", kNow + 9);
	REQUIRE(plain != nullptr);
	plain->arrow = false;
	return b;
}

} // namespace

TEST_CASE("planner board: node type words round-trip")
{
	for (int i = 0; i < static_cast<int>(NodeType::Other); ++i)
	{
		NodeType t = NodeType::Other;
		CHECK(nodeTypeFromName(nodeTypeName(static_cast<NodeType>(i)), t));
		CHECK(static_cast<int>(t) == i);
	}
	NodeType t;
	CHECK_FALSE(nodeTypeFromName("sticky", t));
}

TEST_CASE("planner board: nodes stack, move, resize and take their edges with them")
{
	Board b;
	const UUID a = b.addNode(NodeType::Text, 0, 0, 100, 50, "a", kNow)->id;
	const UUID c = b.addNode(NodeType::Text, 200, 0, 100, 50, "c", kNow)->id;
	const UUID d = b.addNode(NodeType::Text, 400, 0, 100, 50, "d", kNow)->id;

	// A size never goes below the minimum; a Text node is the default for "Other".
	b.addNode(NodeType::Other, 0, 0, 1, 1, "tiny", kNow);
	CHECK(b.nodes.back().type == NodeType::Text);
	CHECK(b.nodes.back().w == Board::kMinSize);
	b.removeNode(b.nodes.back().id, kNow);

	CHECK(b.moveNode(a, 10, 20, kNow + 1));
	CHECK(b.findNode(a)->x == 10);
	CHECK(b.findNode(a)->y == 20);
	CHECK(b.resizeNode(c, 200, 0, 5, 5, kNow + 2));
	CHECK(b.findNode(c)->w == Board::kMinSize);
	CHECK(b.moveNodesBy({ a, d, UUID::generate() }, 5, -5, kNow + 3) == 2);
	CHECK(b.findNode(a)->x == 15);
	CHECK(b.findNode(d)->y == -5);
	CHECK(b.setNodeText(a, "alpha", kNow + 4));
	CHECK(b.setNodeColor(a, "red", kNow + 4));
	CHECK(b.findNode(a)->text == "alpha");
	CHECK(b.modifiedMs == kNow + 4);
	CHECK_FALSE(b.moveNode(UUID::generate(), 0, 0, kNow));

	// Stacking: the last is on top; bring and send reorder.
	CHECK(b.bringToFront(a, kNow + 5));
	CHECK(b.nodes.back().id == a);
	CHECK(b.sendToBack(d, kNow + 6));
	CHECK(b.nodes.front().id == d);
	CHECK_FALSE(b.bringToFront(UUID::generate(), kNow));

	// Edges: two different nodes that exist, once per direction.
	CHECK(b.addEdge(a, c, "x", kNow + 7) != nullptr);
	CHECK(b.addEdge(c, a, "", kNow + 7) != nullptr);   // the other way is another edge
	CHECK(b.addEdge(a, c, "again", kNow + 7) == nullptr);
	CHECK(b.addEdge(a, a, "", kNow + 7) == nullptr);
	CHECK(b.addEdge(a, UUID::generate(), "", kNow + 7) == nullptr);
	CHECK(b.edges.size() == 2);
	const UUID e0 = b.edges[0].id;
	CHECK(b.setEdgeLabel(e0, "leads to", kNow + 8));
	CHECK(b.setEdgeArrow(e0, false, kNow + 8));
	CHECK(b.findEdge(e0)->label == "leads to");
	CHECK_FALSE(b.findEdge(e0)->arrow);
	CHECK(b.edgesOf(a).size() == 2);
	CHECK(b.edgesOf(d).empty());
	CHECK(b.removeEdge(b.edges[1].id, kNow + 9));
	CHECK(b.edges.size() == 1);
	// Removing a node removes the edges that ended at it.
	CHECK(b.removeNode(c, kNow + 10));
	CHECK(b.edges.empty());
	CHECK_FALSE(b.removeNode(c, kNow));
}

TEST_CASE("planner board: a click finds the card, not the frame; a frame takes what is inside it")
{
	Board b;
	const UUID frame = b.addNode(NodeType::Frame, 0, 0, 400, 300, "Level 1", kNow)->id;
	const UUID inside = b.addNode(NodeType::Text, 20, 20, 100, 60, "inside", kNow)->id;
	const UUID overlap = b.addNode(NodeType::Text, 350, 250, 100, 100, "half out", kNow)->id;
	const UUID outside = b.addNode(NodeType::Text, 600, 0, 100, 60, "outside", kNow)->id;
	// A frame made later (on top) still loses a click to a card inside it.
	const UUID late = b.addNode(NodeType::Frame, 0, 0, 400, 300, "Above", kNow)->id;

	CHECK(b.hitTest(30, 30) == inside);
	CHECK(b.hitTest(300, 100) == late);          // only frames here: the topmost one
	CHECK(b.hitTest(700, 20) == outside);
	CHECK(b.hitTest(1000, 1000) == UUID{});
	CHECK(b.hitTest(375, 275) == overlap);

	// Inside = wholly inside (another frame of the same size counts); the half-out card and
	// the frame itself do not.
	const std::vector<UUID> in = b.nodesInside(frame);
	CHECK(in == std::vector<UUID>{ inside, late });
	CHECK(b.nodesInside(UUID::generate()).empty());

	// A selection rectangle takes what it touches, bottom to top.
	const std::vector<UUID> hit = b.nodesIn({ 340, 240, 20, 20 });
	CHECK(hit == std::vector<UUID>{ frame, overlap, late });

	Rect2 r;
	REQUIRE(b.bounds(r));
	CHECK(r.x == 0);
	CHECK(r.y == 0);
	CHECK(r.w == 700);
	CHECK(r.h == 350);
	Board empty;
	CHECK_FALSE(empty.bounds(r));
}

TEST_CASE("planner board: the file round-trips byte for byte and loads what it wrote")
{
	TempProject tp;
	Board b = makeSample();
	std::string err;
	REQUIRE(saveBoard(tp.root, b, &err));
	const fs::path path = boardPath(tp.root, b.id);
	const std::string first = readBytes(path);
	CHECK(first.back() == '\n');
	CHECK(first.find('\r') == std::string::npos);

	Board again;
	const auto r = loadBoard(tp.root, b.id, again);
	REQUIRE(r.ok);
	CHECK(again.toJson() == first);
	CHECK(again.title == "Harbour ideas");
	REQUIRE(again.nodes.size() == 7);
	CHECK(again.nodes[0].type == NodeType::Frame);
	CHECK(again.nodes[0].color == "blue");
	CHECK(again.nodes[2].asset == hex("00000000000000000000000000000a01"));
	CHECK(again.nodes[3].media == "0123456789abcdef.png");
	CHECK(again.nodes[4].link.scene == "Content/Levels/Harbour.hescene");
	CHECK(again.nodes[5].row == hex("00000000000000000000000000000d02"));
	CHECK(again.nodes[5].database == hex("00000000000000000000000000000d01"));
	CHECK(again.nodes[6].page == hex("00000000000000000000000000000e01"));
	REQUIRE(again.edges.size() == 2);
	CHECK(again.edges[0].label == "inspires");
	CHECK(again.edges[0].arrow);
	CHECK_FALSE(again.edges[1].arrow);

	REQUIRE(saveBoard(tp.root, again, &err));
	CHECK(readBytes(path) == first);
	CHECK(listBoards(tp.root) == std::vector<UUID>{ b.id });

	// Fractions survive; so does non-ASCII text, unescaped.
	again.addNode(NodeType::Text, 12.5, -3.25, 100.5, 40, "Hafen \xC3\xA4 \xE6\x97\xA5", kNow + 20);
	REQUIRE(saveBoard(tp.root, again, &err));
	CHECK(readBytes(path).find("Hafen \xC3\xA4") != std::string::npos);
	Board third;
	REQUIRE(loadBoard(tp.root, b.id, third).ok);
	CHECK(third.nodes.back().x == 12.5);
	CHECK(third.nodes.back().y == -3.25);
}

TEST_CASE("planner board: what this build does not know is kept")
{
	TempProject tp;
	const UUID id = hex("11111111111111111111111111111111");
	const std::string text = R"JSON({
    "version": 1,
    "id": "11111111111111111111111111111111",
    "title": "Future",
    "grid": {"size": 20},
    "nodes": [
        {"id": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1", "type": "sticky", "x": 1, "y": 2, "w": 100, "h": 80, "curl": 0.5, "page": "keep me"},
        {"id": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa2", "type": "text", "x": 10, "y": 20, "w": 100, "h": 80, "text": "hi", "rotation": 12, "media": "text-nodes-have-none"},
        "not an object"
    ],
    "edges": [
        {"id": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb1", "from": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1", "to": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa2", "style": "dashed"},
        7
    ]
})JSON";
	writeBytes(boardPath(tp.root, id), text);

	Board b;
	REQUIRE(loadBoard(tp.root, id, b).ok);
	REQUIRE(b.nodes.size() == 2);
	CHECK(b.nodes[0].type == NodeType::Other);
	CHECK(b.nodes[0].typeOther == "sticky");
	const std::string out = b.toJson();
	for (const char* needle : { "\"grid\"", "\"sticky\"", "\"curl\"", "\"page\": \"keep me\"", "\"rotation\"",
	                            "\"media\": \"text-nodes-have-none\"", "\"style\"", "not an object", "7" })
		CHECK_MESSAGE(out.find(needle) != std::string::npos, needle);

	// Reading what was kept and writing it again changes nothing more.
	Board second;
	REQUIRE(second.fromJson(out).ok);
	CHECK(second.toJson() == out);

	// A newer layout is written back under its own version; no version is the first layout.
	Board newer;
	REQUIRE(newer.fromJson("{\"version\": 9, \"title\": \"N\"}").ok);
	CHECK(newer.writeVersion() == 9);
	Board old;
	CHECK(old.fromJson("{\"title\": \"O\"}").version == 1);
	CHECK(old.writeVersion() == Board::kVersion);

	// Repeated ids get fresh ones; a size below the minimum is raised.
	Board dup;
	REQUIRE(dup.fromJson(R"({"nodes": [{"id": "cccccccccccccccccccccccccccccccc", "w": 1, "h": 1}, {"id": "cccccccccccccccccccccccccccccccc"}]})").ok);
	REQUIRE(dup.nodes.size() == 2);
	CHECK(dup.nodes[0].id != dup.nodes[1].id);
	CHECK(dup.nodes[0].w == Board::kMinSize);
}

TEST_CASE("planner board: never saved over a file that is damaged or changed under it")
{
	TempProject tp;
	Board b = makeSample();
	REQUIRE(saveBoard(tp.root, b));
	const fs::path path = boardPath(tp.root, b.id);

	// Somebody's version arrived (a pull): refuse, leave their bytes.
	std::string theirs = b.toJson();
	theirs.replace(theirs.find("Harbour ideas"), 13, "Theirs!!!!!!!");
	writeBytes(path, theirs);
	b.title = "Mine";
	std::string err;
	CHECK_FALSE(saveBoard(tp.root, b, &err));
	CHECK(err.find("changed on disk") != std::string::npos);
	CHECK(readBytes(path) == theirs);

	// A merge conflict is never loaded and never written over.
	const std::string conflict = "<<<<<<< HEAD\n{}\n=======\n{}\n>>>>>>> x\n";
	writeBytes(path, conflict);
	Board broken;
	const auto r = loadBoard(tp.root, b.id, broken);
	CHECK_FALSE(r.ok);
	CHECK(r.error.find("merge-conflict") != std::string::npos);
	broken.title = "Fresh";
	CHECK_FALSE(saveBoard(tp.root, broken, &err));
	CHECK(readBytes(path) == conflict);
	writeBytes(path, "{\"nodes\": 5}");
	CHECK_FALSE(loadBoard(tp.root, b.id, broken).ok);
	writeBytes(path, "[1]");
	CHECK_FALSE(loadBoard(tp.root, b.id, broken).ok);

	// No file is an empty board under the id asked for; the name is the identity.
	const UUID other = UUID::generate();
	Board none;
	CHECK(loadBoard(tp.root, other, none).ok);
	CHECK(none.id == other);
	CHECK(none.nodes.empty());
	writeBytes(boardPath(tp.root, hex("00000000000000000000000000000002")), "{\"id\": \"99999999999999999999999999999999\", \"title\": \"Copy\"}");
	Board copy;
	REQUIRE(loadBoard(tp.root, hex("00000000000000000000000000000002"), copy).ok);
	CHECK(copy.id == hex("00000000000000000000000000000002"));
	CHECK(copy.toJson().find("99999999999999999999999999999999") == std::string::npos);

	CHECK(deleteBoard(tp.root, b.id));
	CHECK_FALSE(fs::exists(path));
	CHECK(deleteBoard(tp.root, b.id));   // already gone is fine
}

// A small PNG-shaped blob: the importer judges by extension and size, not by decoding.
static std::string fakePng(char seed, std::size_t size = 64)
{
	std::string s = "\x89PNG\r\n\x1a\n";
	s.append(size, seed);
	return s;
}

TEST_CASE("planner board media: pictures are copied in once, by content, under names that cannot escape")
{
	TempProject tp;
	const fs::path source = tp.root / "outside" / "Mood Board.PNG";
	writeBytes(source, fakePng('a'));

	std::string err;
	const std::string name = importMediaFile(tp.root, source, &err);
	REQUIRE_MESSAGE(!name.empty(), err);
	CHECK(name.size() == 16 + 4);
	CHECK(name.substr(16) == ".png");   // the extension is lower-cased
	CHECK(fs::exists(mediaPath(tp.root, name)));
	CHECK(readBytes(mediaPath(tp.root, name)) == fakePng('a'));
	CHECK(mediaPath(tp.root, name) == mediaDir(tp.root) / name);

	// The same picture again, from anywhere, is the same file; another one is another file.
	writeBytes(tp.root / "copy.png", fakePng('a'));
	CHECK(importMediaFile(tp.root, tp.root / "copy.png") == name);
	CHECK(importMediaBytes(tp.root, fakePng('a'), "png") == name);
	const std::string other = importMediaBytes(tp.root, fakePng('b'), "jpg", &err);
	REQUIRE(!other.empty());
	CHECK(other != name);
	CHECK(other.substr(16) == ".jpg");
	CHECK(listMedia(tp.root).size() == 2);
	CHECK(fs::exists(mediaDir(tp.root) / name));
	CHECK_FALSE(fs::exists(mediaDir(tp.root) / (name + ".tmp")));   // no half-written file is left

	// Refusals: not a file, not a picture, empty, a name with a path in it.
	CHECK(importMediaFile(tp.root, tp.root / "missing.png", &err).empty());
	CHECK(err.find("not a file") != std::string::npos);
	writeBytes(tp.root / "notes.txt", "hello");
	CHECK(importMediaFile(tp.root, tp.root / "notes.txt", &err).empty());
	CHECK(err.find("not a picture") != std::string::npos);
	CHECK(importMediaBytes(tp.root, "", "png", &err).empty());
	CHECK(importMediaBytes(tp.root, fakePng('c'), "exe", &err).empty());
	for (const char* bad : { "", "../x.png", "a/b.png", "a\\b.png", "..", ".hidden.png", "noext", "x.exe" })
		CHECK_MESSAGE(mediaPath(tp.root, bad).empty(), bad);

	// Only picture files count as media.
	writeBytes(mediaDir(tp.root) / "readme.txt", "x");
	CHECK(listMedia(tp.root).size() == 2);
}

TEST_CASE("planner workspace boards: create, edit, reload, remove, and a pull under the open board")
{
	TempProject tp;
	Workspace ws;
	ws.setProject(tp.root);
	CHECK(ws.boards().empty());

	Board* made = ws.createBoard("Ideas");
	REQUIRE(made);
	const UUID id = made->id;
	CHECK(fs::exists(boardPath(tp.root, id)));

	UUID node;
	REQUIRE(ws.editBoard(id, [&](Board& b) { node = b.addNode(NodeType::Text, 0, 0, 100, 50, "first", kNow)->id; return true; }));
	CHECK_FALSE(ws.editBoard(id, [](Board&) { return false; }));
	CHECK_FALSE(ws.editBoard(UUID::generate(), [](Board&) { return true; }));

	Workspace other;
	other.setProject(tp.root);
	REQUIRE(other.findBoard(id));
	REQUIRE(other.findBoard(id)->findNode(node));
	CHECK(other.findBoard(id)->findNode(node)->text == "first");

	// A teammate's node arrives with a pull; ours lands on top of it.
	REQUIRE(other.editBoard(id, [&](Board& b) { b.addNode(NodeType::Text, 200, 0, 100, 50, "theirs", kNow + 1); return true; }));
	REQUIRE(ws.editBoard(id, [&](Board& b) { b.addNode(NodeType::Text, 400, 0, 100, 50, "mine too", kNow + 2); return true; }));
	std::vector<std::string> texts;
	for (const Node& n : ws.findBoard(id)->nodes) texts.push_back(n.text);
	CHECK(texts == std::vector<std::string>{ "first", "theirs", "mine too" });
	CHECK(ws.lastError().empty());

	// Sorted by title; poll takes in new, changed and removed files and lists a damaged one.
	other.createBoard("alpha");
	CHECK(ws.poll(true));
	REQUIRE(ws.boards().size() == 2);
	CHECK(ws.boards()[0]->title == "alpha");
	other.editBoard(id, [](Board& b) { b.title = "Renamed"; return true; });
	CHECK(ws.poll(true));
	CHECK(ws.findBoard(id)->title == "Renamed");
	const UUID bad = UUID::generate();
	writeBytes(boardPath(tp.root, bad), "<<<<<<< HEAD\n{}\n=======\n{}\n>>>>>>> x\n");
	CHECK(ws.poll(true));
	REQUIRE(ws.unreadableBoards().size() == 1);
	CHECK(ws.unreadableBoards()[0].id == bad);
	writeBytes(boardPath(tp.root, bad), "{\"title\": \"Fixed\"}");
	CHECK(ws.poll(true));
	CHECK(ws.unreadableBoards().empty());
	REQUIRE(ws.findBoard(bad));
	CHECK(ws.findBoard(bad)->title == "Fixed");

	CHECK(ws.removeBoard(id));
	CHECK_FALSE(fs::exists(boardPath(tp.root, id)));
	CHECK(ws.findBoard(id) == nullptr);
	CHECK_FALSE(ws.removeBoard(id));
}

TEST_CASE("planner board history: undo and redo walk the snapshots; a new change ends the redo future")
{
	Board b;
	BoardHistory h;
	CHECK_FALSE(h.canUndo());
	CHECK_FALSE(h.canRedo());
	BoardSnapshot target;
	CHECK_FALSE(h.undo(b, target));
	CHECK_FALSE(h.redo(b, target));

	// Three changes, each recorded as the state before it.
	h.record(snapshotOf(b));
	const UUID a = b.addNode(NodeType::Text, 0, 0, 100, 50, "a", kNow)->id;
	h.record(snapshotOf(b));
	const UUID c = b.addNode(NodeType::Text, 200, 0, 100, 50, "c", kNow)->id;
	h.record(snapshotOf(b));
	b.addEdge(a, c, "x", kNow);
	CHECK(h.canUndo());

	REQUIRE(h.undo(b, target));   // the edge goes
	restore(b, target, kNow + 1);
	CHECK(b.edges.empty());
	CHECK(b.nodes.size() == 2);
	REQUIRE(h.undo(b, target));   // c goes
	restore(b, target, kNow + 2);
	CHECK(b.nodes.size() == 1);
	CHECK(b.modifiedMs == kNow + 2);
	CHECK(h.canRedo());
	REQUIRE(h.redo(b, target));   // c comes back
	restore(b, target, kNow + 3);
	CHECK(b.nodes.size() == 2);
	CHECK(b.edges.empty());

	// A new change ends the future that was left.
	h.record(snapshotOf(b));
	b.moveNode(a, 5, 5, kNow + 4);
	CHECK_FALSE(h.canRedo());
	REQUIRE(h.undo(b, target));
	restore(b, target, kNow + 5);
	CHECK(b.findNode(a)->x == 0);
	REQUIRE(h.redo(b, target));
	restore(b, target, kNow + 6);
	CHECK(b.findNode(a)->x == 5);

	// Restoring is plain assignment: doing it twice (a save retried after a pull) is the same.
	restore(b, target, kNow + 7);
	CHECK(b.findNode(a)->x == 5);
	CHECK(b.nodes.size() == 2);

	// The oldest steps go beyond the limit.
	BoardHistory many;
	for (std::size_t i = 0; i < BoardHistory::kLimit + 20; ++i)
	{
		many.record(snapshotOf(b));
		b.moveNode(a, static_cast<double>(i), 0, kNow);
	}
	std::size_t steps = 0;
	while (many.undo(b, target)) ++steps;
	CHECK(steps == BoardHistory::kLimit);
}
