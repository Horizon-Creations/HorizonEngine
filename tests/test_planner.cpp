// The Planner's data and its files (Config/Planner/Databases, Config/Planner/Pages).
// It makes the TaskBoard's promises to a team that keeps the files in git: a round
// trip changes nothing, a layout from before still loads, a layout from a newer
// editor survives an older one saving it, a merge conflict is never saved over,
// and a cell, view or block this build cannot show is kept as it was. On top of
// that the model's own rules: typed cells, filtered and sorted views, the cascade
// when a property, option or row goes, and the block tree of a page.
#include "doctest.h"
#include "TestFsUtil.h"

#include "Planner.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace HE;
using namespace HE::Planner;

#define HE_PL_JAPANESE "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"   // "Japanese" in Japanese, UTF-8 bytes

namespace
{

struct TempProject
{
	fs::path root;
	TempProject()
	{
		root = fs::temp_directory_path() / ("he_test_planner_" + uuidToHex(UUID::generate()));
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

constexpr std::int64_t kNow = 1760000000000;   // 2025-10-09

// The ids a sample database is built from, copied out at once: the vectors they
// live in reallocate as the database grows.
struct Sample
{
	Database db;
	UUID status, points, done, assignees, links, due, tags, blockedBy;
	UUID oOpen, oProg, oDone, tagA, tagB;
	UUID r1, r2, r3, r4;
	UUID viewTable;
};

Sample makeSample()
{
	Sample s;
	s.db = Database::create("Features");
	Database& db = s.db;
	s.viewTable = db.views[0].id;
	s.status    = db.addProperty("Status", PropertyType::Select)->id;
	s.oOpen     = db.addOption(s.status, "Open", "gray")->id;
	s.oProg     = db.addOption(s.status, "In progress", "blue")->id;
	s.oDone     = db.addOption(s.status, "Done", "green")->id;
	s.points    = db.addProperty("Points", PropertyType::Number)->id;
	s.done      = db.addProperty("Shipped", PropertyType::Checkbox)->id;
	s.assignees = db.addProperty("Owner", PropertyType::Person)->id;
	s.links     = db.addProperty("Links", PropertyType::EngineLink)->id;
	s.due       = db.addProperty("Due", PropertyType::Date)->id;
	s.tags      = db.addProperty("Tags", PropertyType::MultiSelect)->id;
	s.tagA      = db.addOption(s.tags, "Audio")->id;
	s.tagB      = db.addOption(s.tags, "Level")->id;
	s.blockedBy = db.addProperty("Blocked by", PropertyType::Relation)->id;
	db.findProperty(s.blockedBy)->relationDb = db.id;

	s.r1 = db.addRow("Harbour level", "Anna", kNow)->id;
	s.r2 = db.addRow("Climbing", "Bob", kNow + 1)->id;
	s.r3 = db.addRow("Inventory UI", "Anna", kNow + 2)->id;
	s.r4 = db.addRow("Rain audio", "Bob", kNow + 3)->id;

	REQUIRE(db.setSelect(s.r1, s.status, s.oProg, kNow + 10));
	REQUIRE(db.setSelect(s.r2, s.status, s.oOpen, kNow + 10));
	REQUIRE(db.setSelect(s.r4, s.status, s.oDone, kNow + 10));
	REQUIRE(db.setNumber(s.r1, s.points, 5, kNow + 10));
	REQUIRE(db.setNumber(s.r2, s.points, 2, kNow + 10));
	REQUIRE(db.setNumber(s.r3, s.points, 8, kNow + 10));
	REQUIRE(db.setChecked(s.r4, s.done, true, kNow + 10));
	REQUIRE(db.setPeople(s.r1, s.assignees, { "Anna", "Bob" }, kNow + 10));
	REQUIRE(db.setDate(s.r1, s.due, kNow + 86400000, kNow + 10));
	REQUIRE(db.setMultiSelect(s.r4, s.tags, { s.tagA }, kNow + 10));
	REQUIRE(db.setRelation(s.r1, s.blockedBy, { s.r2 }, kNow + 10));
	UUID crate = hex("00000000000000000000000000000c0a");
	REQUIRE(db.setLinks(s.r1, s.links,
	                    { TaskLink::asset(crate, "Crate_Wood"), TaskLink::sceneLink("Content/Levels/Harbour.hescene", "Harbour") },
	                    kNow + 10));
	return s;
}

} // namespace

TEST_CASE("planner words round-trip and unknown ones are refused")
{
	for (int i = 0; i < static_cast<int>(PropertyType::Other); ++i)
	{
		PropertyType t = PropertyType::Other;
		CHECK(propertyTypeFromName(propertyTypeName(static_cast<PropertyType>(i)), t));
		CHECK(static_cast<int>(t) == i);
	}
	for (int i = 0; i < static_cast<int>(ViewKind::Other); ++i)
	{
		ViewKind k = ViewKind::Other;
		CHECK(viewKindFromName(viewKindName(static_cast<ViewKind>(i)), k));
		CHECK(static_cast<int>(k) == i);
	}
	for (int i = 0; i < static_cast<int>(BlockType::Other); ++i)
	{
		BlockType b = BlockType::Other;
		CHECK(blockTypeFromName(blockTypeName(static_cast<BlockType>(i)), b));
		CHECK(static_cast<int>(b) == i);
	}
	PropertyType t;
	CHECK_FALSE(propertyTypeFromName("rollup", t));
	CHECK(std::string(propertyTypeName(PropertyType::MultiSelect)) == "multi-select");
	CHECK(std::string(blockTypeName(BlockType::Heading2)) == "heading-2");
}

TEST_CASE("planner typed cells read back what was written and refuse the wrong type")
{
	Sample s = makeSample();
	Database& db = s.db;

	CHECK(db.select(s.r1, s.status) == s.oProg);
	CHECK(db.number(s.r3, s.points) == 8);
	CHECK(db.checked(s.r4, s.done));
	CHECK_FALSE(db.checked(s.r1, s.done));
	CHECK(db.people(s.r1, s.assignees) == std::vector<std::string>{ "Anna", "Bob" });
	std::int64_t ms = 0;
	REQUIRE(db.date(s.r1, s.due, ms));
	CHECK(ms == kNow + 86400000);
	CHECK(db.multiSelect(s.r4, s.tags) == std::vector<UUID>{ s.tagA });
	CHECK(db.relation(s.r1, s.blockedBy) == std::vector<UUID>{ s.r2 });
	const std::vector<TaskLink> links = db.links(s.r1, s.links);
	REQUIRE(links.size() == 2);
	CHECK(links[0].kind == TaskLinkKind::Asset);
	CHECK(links[0].label == "Crate_Wood");
	CHECK(links[1].scene == "Content/Levels/Harbour.hescene");

	// Not the property's type, an option the property does not have, an unknown row.
	CHECK_FALSE(db.setText(s.r1, s.points, "five", kNow));
	CHECK_FALSE(db.setNumber(s.r1, s.status, 1, kNow));
	CHECK_FALSE(db.setSelect(s.r1, s.status, s.tagA, kNow));
	CHECK_FALSE(db.setSelect(UUID::generate(), s.status, s.oOpen, kNow));
	CHECK_FALSE(db.setMultiSelect(s.r1, s.tags, { s.oOpen }, kNow));
	CHECK(db.select(s.r1, s.status) == s.oProg);   // the refused write changed nothing

	// An empty value clears the cell; the row's modified time moves with every write.
	CHECK(db.isEmpty(s.r3, s.status));
	CHECK(db.setSelect(s.r1, s.status, UUID{}, kNow + 99));
	CHECK(db.isEmpty(s.r1, s.status));
	CHECK(db.findRow(s.r1)->modifiedMs == kNow + 99);
	CHECK(db.setChecked(s.r4, s.done, false, kNow + 100));
	CHECK(db.findRow(s.r4)->cell(s.done) == nullptr);   // unchecked is no cell at all

	// Duplicate people and empty names are dropped, order kept.
	CHECK(db.setPeople(s.r2, s.assignees, { "Bob", "", "Anna", "Bob" }, kNow));
	CHECK(db.people(s.r2, s.assignees) == std::vector<std::string>{ "Bob", "Anna" });

	// An option name is unique within its property.
	CHECK(db.addOption(s.status, "Open")->id == s.oOpen);
	CHECK(db.addOption(s.points, "x") == nullptr);   // a number takes no options
}

TEST_CASE("planner database round trip is byte-identical and survives a reload")
{
	TempProject tp;
	Sample s = makeSample();
	std::string err;
	REQUIRE(saveDatabase(tp.root, s.db, &err));
	const fs::path path = databasePath(tp.root, s.db.id);
	REQUIRE(fs::exists(path));
	const std::string first = readBytes(path);
	CHECK(first.back() == '\n');
	CHECK(first.find('\r') == std::string::npos);

	Database again;
	const auto r = loadDatabase(tp.root, s.db.id, again);
	REQUIRE(r.ok);
	CHECK(r.dropped == 0);
	CHECK(again.toJson() == first);
	CHECK(again.properties.size() == s.db.properties.size());
	CHECK(again.rows.size() == 4);
	CHECK(again.select(s.r1, s.status) == s.oProg);
	CHECK(again.links(s.r1, s.links).size() == 2);

	REQUIRE(saveDatabase(tp.root, again, &err));
	CHECK(readBytes(path) == first);
}

TEST_CASE("planner keeps what this build does not know")
{
	TempProject tp;
	const UUID id = hex("11111111111111111111111111111111");
	const std::string text = R"JSON({
    "version": 1,
    "id": "11111111111111111111111111111111",
    "name": "Future",
    "futureTopLevel": {"a": [1, 2, 3]},
    "properties": [
        {"id": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1", "name": "Score", "type": "rollup", "formula": "sum(x)"},
        {"id": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa2", "name": "Mood", "type": "select",
         "options": [{"id": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb1", "name": "Good", "sparkle": true}], "pin": 7}
    ],
    "views": [
        {"id": "cccccccccccccccccccccccccccccc01", "name": "Gallery", "kind": "gallery", "cover": "big"}
    ],
    "rows": [
        {"id": "dddddddddddddddddddddddddddddd01", "title": "One", "created": "2025-10-09T00:00:00.000Z",
         "cells": {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1": {"sum": 12}, "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeee1": "orphan"},
         "rowExtra": "keep"},
        "not an object"
    ]
})JSON";
	writeBytes(databasePath(tp.root, id), text);

	Database db;
	const auto r = loadDatabase(tp.root, id, db);
	REQUIRE(r.ok);
	REQUIRE(db.properties.size() == 2);
	CHECK(db.properties[0].type == PropertyType::Other);
	CHECK(db.views[0].kind == ViewKind::Other);

	const std::string out = db.toJson();
	for (const char* needle : { "futureTopLevel", "\"rollup\"", "\"formula\"", "\"sparkle\"", "\"pin\"",
	                            "\"gallery\"", "\"cover\"", "\"rowExtra\"", "\"sum\": 12",
	                            "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeee1", "not an object" })
		CHECK_MESSAGE(out.find(needle) != std::string::npos, needle);

	// Reading what was kept and writing it again changes nothing more.
	Database second;
	REQUIRE(second.fromJson(out).ok);
	CHECK(second.toJson() == out);
}

TEST_CASE("planner file from a newer editor is written back under its own version")
{
	TempProject tp;
	const UUID id = hex("22222222222222222222222222222222");
	writeBytes(databasePath(tp.root, id), "{\n    \"version\": 7,\n    \"name\": \"New\"\n}\n");
	Database db;
	const auto r = loadDatabase(tp.root, id, db);
	REQUIRE(r.ok);
	CHECK(r.version == 7);
	CHECK(db.writeVersion() == 7);
	CHECK(db.toJson().find("\"version\": 7") != std::string::npos);

	// A file with no version is the first layout.
	Database old;
	CHECK(old.fromJson("{\"name\":\"Old\"}").version == 1);
	CHECK(old.writeVersion() == Database::kVersion);
}

TEST_CASE("planner layout version 1 is frozen")
{
	// Version 1 as it is written today. When the layout moves, this text stays and
	// keeps asserting that it loads, field for field. Do not regenerate it.
	const char* const kLayoutV1 = R"({
    "version": 1,
    "id": "0123456789abcdef0123456789abcdef",
    "name": "Bugs",
    "properties": [
        {
            "id": "00000000000000000000000000000a01",
            "name": "Severity",
            "type": "select",
            "options": [
                {"id": "00000000000000000000000000000b01", "name": "Crash", "color": "red"}
            ]
        }
    ],
    "views": [
        {
            "id": "00000000000000000000000000000c01",
            "name": "By severity",
            "kind": "board",
            "filters": [{"property": "00000000000000000000000000000a01", "op": "is-not-empty"}],
            "sorts": [{"property": "00000000000000000000000000000a01", "descending": true}],
            "groupBy": "00000000000000000000000000000a01",
            "hidden": []
        }
    ],
    "rows": [
        {
            "id": "00000000000000000000000000000d01",
            "title": "Crash on load",
            "createdBy": "Anna",
            "created": "2025-10-09T08:00:00.000Z",
            "modified": "2025-10-09T09:30:15.250Z",
            "cells": {"00000000000000000000000000000a01": "00000000000000000000000000000b01"}
        }
    ]
})";
	Database db;
	const auto r = db.fromJson(kLayoutV1);
	REQUIRE(r.ok);
	CHECK(db.id == hex("0123456789abcdef0123456789abcdef"));
	CHECK(db.name == "Bugs");
	REQUIRE(db.properties.size() == 1);
	CHECK(db.properties[0].type == PropertyType::Select);
	CHECK(db.properties[0].options[0].color == "red");
	REQUIRE(db.views.size() == 1);
	CHECK(db.views[0].kind == ViewKind::Board);
	CHECK(db.views[0].filters[0].op == "is-not-empty");
	CHECK(db.views[0].sorts[0].descending);
	CHECK(db.views[0].groupBy == hex("00000000000000000000000000000a01"));
	REQUIRE(db.rows.size() == 1);
	CHECK(db.rows[0].title == "Crash on load");
	CHECK(db.rows[0].createdBy == "Anna");
	CHECK(db.rows[0].modifiedMs - db.rows[0].createdMs == 5415250);   // 1 h 30 min 15.25 s
	CHECK(db.select(db.rows[0].id, db.properties[0].id) == hex("00000000000000000000000000000b01"));
}

TEST_CASE("planner never saves over a file it cannot read or one that changed")
{
	TempProject tp;
	Sample s = makeSample();
	REQUIRE(saveDatabase(tp.root, s.db));
	const fs::path path = databasePath(tp.root, s.db.id);

	// Somebody else wrote it in between (a pull): refuse, leave their bytes.
	std::string theirs = s.db.toJson();
	theirs.replace(theirs.find("Features"), 8, "Theirs!!");
	writeBytes(path, theirs);
	s.db.name = "Renamed";
	std::string err;
	CHECK_FALSE(saveDatabase(tp.root, s.db, &err));
	CHECK(err.find("changed on disk") != std::string::npos);
	CHECK(readBytes(path) == theirs);

	// A merge conflict: not loaded, not saved over, and said so.
	const std::string conflict = "<<<<<<< HEAD\n{}\n=======\n{}\n>>>>>>> branch\n";
	writeBytes(path, conflict);
	Database broken;
	const auto r = loadDatabase(tp.root, s.db.id, broken);
	CHECK_FALSE(r.ok);
	CHECK(r.error.find("merge-conflict") != std::string::npos);
	CHECK(broken.rows.empty());
	broken.name = "Fresh";
	CHECK_FALSE(saveDatabase(tp.root, broken, &err));
	CHECK(readBytes(path) == conflict);

	// Truncated JSON and non-objects are damage too.
	writeBytes(path, "{\"rows\": [");
	CHECK_FALSE(loadDatabase(tp.root, s.db.id, broken).ok);
	writeBytes(path, "[1, 2]");
	CHECK_FALSE(loadDatabase(tp.root, s.db.id, broken).ok);
	writeBytes(path, "{\"rows\": 5}");
	CHECK_FALSE(loadDatabase(tp.root, s.db.id, broken).ok);
}

TEST_CASE("planner no file is an empty document, and an unwritten page writes nothing")
{
	TempProject tp;
	const UUID id = UUID::generate();
	Database db;
	const auto r = loadDatabase(tp.root, id, db);
	CHECK(r.ok);
	CHECK(db.id == id);
	CHECK(db.rows.empty());
	CHECK(listDatabases(tp.root).empty());

	Page page;
	REQUIRE(loadPage(tp.root, id, page).ok);
	CHECK(savePage(tp.root, page));
	CHECK_FALSE(fs::exists(pagePath(tp.root, id)));   // a row opened but not yet written in
	page.addBlock(BlockType::Paragraph, "Hello", kNow);
	CHECK(savePage(tp.root, page));
	CHECK(fs::exists(pagePath(tp.root, id)));

	CHECK(deletePage(tp.root, id));
	CHECK_FALSE(fs::exists(pagePath(tp.root, id)));
	CHECK(deletePage(tp.root, id));   // already gone is fine
}

TEST_CASE("planner file name is the identity and the listing is ordered")
{
	TempProject tp;
	const UUID a = hex("00000000000000000000000000000002");
	const UUID b = hex("00000000000000000000000000000001");
	// A copied file keeps the original's "id" inside; the name wins.
	writeBytes(databasePath(tp.root, a), "{\"id\": \"99999999999999999999999999999999\", \"name\": \"Copy\"}");
	writeBytes(databasePath(tp.root, b), "{\"name\": \"Other\"}");
	writeBytes(plannerDir(tp.root) / "Databases" / "notes.txt", "x");
	writeBytes(plannerDir(tp.root) / "Databases" / "readme.json", "{}");

	CHECK(listDatabases(tp.root) == std::vector<UUID>{ b, a });
	Database db;
	REQUIRE(loadDatabase(tp.root, a, db).ok);
	CHECK(db.id == a);
	CHECK(db.name == "Copy");
	CHECK(db.toJson().find("99999999999999999999999999999999") == std::string::npos);
}

TEST_CASE("planner repeated ids in a file get fresh ones")
{
	Database db;
	const char* const text = R"({
    "properties": [{"id": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "name": "A", "type": "text"},
                   {"id": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "name": "B", "type": "text"},
                   {"name": "C", "type": "text"}],
    "rows": [{"id": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "title": "1"}, {"id": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "title": "2"}]
})";
	REQUIRE(db.fromJson(text).ok);
	REQUIRE(db.properties.size() == 3);
	CHECK(db.properties[0].id != db.properties[1].id);
	CHECK(db.properties[2].id != UUID{});
	CHECK(db.rows[0].id != db.rows[1].id);
}

TEST_CASE("planner writes non-ASCII text as it is")
{
	Database db = Database::create(HE_PL_JAPANESE);
	db.addRow(HE_PL_JAPANESE, "Anna", kNow);
	const std::string out = db.toJson();
	CHECK(out.find(HE_PL_JAPANESE) != std::string::npos);
	CHECK(out.find("\\u") == std::string::npos);
	Database back;
	REQUIRE(back.fromJson(out).ok);
	CHECK(back.name == HE_PL_JAPANESE);
}

TEST_CASE("planner view: filter, sort, group")
{
	Sample s = makeSample();
	Database& db = s.db;
	View view;
	view.id = UUID::generate();

	auto titles = [&](const std::vector<std::size_t>& idx) {
		std::vector<std::string> t;
		for (const std::size_t i : idx) t.push_back(db.rows[i].title);
		return t;
	};
	using V = std::vector<std::string>;

	CHECK(titles(db.query(view)) == V{ "Harbour level", "Climbing", "Inventory UI", "Rain audio" });   // file order

	// Select sorts by the option's place in the list, empty last in both directions.
	view.sorts = { { s.status, false } };
	CHECK(titles(db.query(view)) == V{ "Climbing", "Harbour level", "Rain audio", "Inventory UI" });
	view.sorts = { { s.status, true } };
	CHECK(titles(db.query(view)) == V{ "Rain audio", "Harbour level", "Climbing", "Inventory UI" });

	// Title (the null property), then a number with a tie-break by the next sort.
	view.sorts = { { UUID{}, false } };
	CHECK(titles(db.query(view)) == V{ "Climbing", "Harbour level", "Inventory UI", "Rain audio" });
	view.sorts = { { s.points, true }, { UUID{}, false } };
	CHECK(titles(db.query(view)) == V{ "Inventory UI", "Harbour level", "Climbing", "Rain audio" });

	view.sorts.clear();
	auto filtered = [&](const char* op, const std::string& valueJson, const UUID& prop) {
		view.filters = { { prop, op, valueJson } };
		return titles(db.query(view));
	};
	CHECK(filtered("equals", "\"Open\"", s.status) == V{ "Climbing" });                       // by option name
	CHECK(filtered("equals", "\"" + uuidToHex(s.oDone) + "\"", s.status) == V{ "Rain audio" });   // by option id
	CHECK(filtered("not-equals", "\"Open\"", s.status) == V{ "Harbour level", "Inventory UI", "Rain audio" });
	CHECK(filtered("is-empty", "", s.status) == V{ "Inventory UI" });
	CHECK(filtered("is-not-empty", "", s.status).size() == 3);
	CHECK(filtered("gt", "3", s.points) == V{ "Harbour level", "Inventory UI" });
	CHECK(filtered("lte", "5", s.points) == V{ "Harbour level", "Climbing" });
	CHECK(filtered("contains", "\"LEVEL\"", UUID{}) == V{ "Harbour level" });                 // title, case-insensitive
	CHECK(filtered("contains", "\"Anna\"", s.assignees) == V{ "Harbour level" });
	CHECK(filtered("contains", "\"Audio\"", s.tags) == V{ "Rain audio" });
	CHECK(filtered("equals", "true", s.done) == V{ "Rain audio" });
	CHECK(filtered("gt", "\"" + taskTimeToIso(kNow) + "\"", s.due) == V{ "Harbour level" });
	// What it cannot evaluate must not hide work.
	CHECK(filtered("matches-regex", "\"x\"", s.status).size() == 4);
	CHECK(filtered("gt", "1", s.status).size() == 4);
	CHECK(filtered("equals", "1", s.links).size() == 4);
	CHECK(filtered("equals", "1", hex("ffffffffffffffffffffffffffffffff")).size() == 4);   // property is gone

	// All filters must match.
	view.filters = { { s.points, "gt", "1" }, { s.status, "is-not-empty", "" } };
	CHECK(titles(db.query(view)) == V{ "Harbour level", "Climbing" });

	// Board columns: by option, and the "No value" column.
	view.filters.clear();
	view.groupBy = s.status;
	CHECK(titles(db.column(view, s.oOpen)) == V{ "Climbing" });
	CHECK(titles(db.column(view, s.oProg)) == V{ "Harbour level" });
	CHECK(titles(db.column(view, UUID{})) == V{ "Inventory UI" });
}

TEST_CASE("planner removing a property, option or row cleans up after itself")
{
	Sample s = makeSample();
	Database& db = s.db;

	View* v = db.addView("Board", ViewKind::Board);
	v->groupBy = s.status;
	v->filters = { { s.status, "is-not-empty", "" }, { s.points, "gt", "1" } };
	v->sorts   = { { s.status, false }, { s.points, false } };
	v->hidden  = { s.status, s.points };

	CHECK(db.removeOption(s.status, s.oProg));
	CHECK(db.isEmpty(s.r1, s.status));                  // the cell that held it is gone
	CHECK(db.select(s.r2, s.status) == s.oOpen);        // the others are not
	CHECK(db.setSelect(s.r1, s.status, s.oProg, kNow) == false);

	CHECK(db.removeOption(s.tags, s.tagA));
	CHECK(db.findRow(s.r4)->cell(s.tags) == nullptr);   // the only tag: the cell goes

	CHECK(db.removeProperty(s.status));
	CHECK(db.findRow(s.r2)->cell(s.status) == nullptr);
	const View* board = db.findView(db.views.back().id);
	REQUIRE(board);
	CHECK(board->groupBy == UUID{});
	CHECK(board->filters.size() == 1);
	CHECK(board->sorts.size() == 1);
	CHECK(board->hidden == std::vector<UUID>{ s.points });
	CHECK_FALSE(db.removeProperty(s.status));

	// A row that other rows point at (a self-relation): the pointer goes with it.
	REQUIRE(db.relation(s.r1, s.blockedBy) == std::vector<UUID>{ s.r2 });
	CHECK(db.removeRow(s.r2));
	CHECK(db.relation(s.r1, s.blockedBy).empty());
	CHECK(db.findRow(s.r1)->cell(s.blockedBy) == nullptr);
	CHECK_FALSE(db.removeRow(s.r2));

	// A database always has a view.
	while (db.views.size() > 1) CHECK(db.removeView(db.views.back().id));
	CHECK_FALSE(db.removeView(db.views[0].id));
}

TEST_CASE("planner page: block tree, moves and the file")
{
	TempProject tp;
	Page page;
	page.id        = UUID::generate();
	page.title     = "Harbour";
	page.createdBy = "Anna";
	page.createdMs = page.modifiedMs = kNow;

	const UUID h   = page.addBlock(BlockType::Heading1, "Idea", kNow + 1)->id;
	const UUID p   = page.addBlock(BlockType::Paragraph, "Foggy harbour", kNow + 2)->id;
	const UUID td  = page.addBlock(BlockType::Todo, "Blockout", kNow + 3)->id;
	const UUID sub = page.addBlock(BlockType::Bullet, "Crates", kNow + 4, td)->id;
	CHECK(page.addBlock(BlockType::Bullet, "orphan", kNow, UUID::generate()) == nullptr);
	page.findBlock(td)->checked = true;

	Block* link = page.addBlock(BlockType::EngineLink, "", kNow + 5);
	link->link = TaskLink::sceneLink("Content/Levels/Harbour.hescene", "Harbour");
	const UUID linkId = link->id;
	Block* dbv = page.addBlock(BlockType::DatabaseView, "", kNow + 6);
	dbv->database = hex("0123456789abcdef0123456789abcdef");
	const UUID dbvId = dbv->id;
	page.addBlock(BlockType::Code, "int x;", kNow + 7)->language = "cpp";
	page.parent = hex("0123456789abcdef0123456789abcdef");

	std::string err;
	REQUIRE(savePage(tp.root, page, &err));
	const std::string first = readBytes(pagePath(tp.root, page.id));

	Page back;
	REQUIRE(loadPage(tp.root, page.id, back).ok);
	CHECK(back.toJson() == first);
	CHECK(back.title == "Harbour");
	CHECK(back.parent == page.parent);
	REQUIRE(back.findBlock(td));
	CHECK(back.findBlock(td)->checked);
	REQUIRE(back.findBlock(sub));
	CHECK(back.findBlock(sub)->text == "Crates");
	CHECK(back.findBlock(linkId)->link.scene == "Content/Levels/Harbour.hescene");
	CHECK(back.findBlock(dbvId)->database == hex("0123456789abcdef0123456789abcdef"));
	CHECK(back.blocks.back().language == "cpp");

	// Moves: out of a parent, into another, never into itself or its own subtree.
	CHECK(back.moveBlock(sub, UUID{}, 0, kNow + 20));
	CHECK(back.blocks.front().id == sub);
	CHECK(back.findBlock(td)->children.empty());
	CHECK(back.moveBlock(sub, td, 99, kNow + 21));   // an index past the end appends
	CHECK(back.findBlock(td)->children.size() == 1);
	CHECK_FALSE(back.moveBlock(td, td, 0, kNow));
	CHECK_FALSE(back.moveBlock(td, sub, 0, kNow));
	CHECK_FALSE(back.moveBlock(UUID::generate(), UUID{}, 0, kNow));
	CHECK(back.moveBlock(p, UUID{}, 0, kNow + 22));
	CHECK(back.blocks.front().id == p);
	CHECK(back.modifiedMs == kNow + 22);

	CHECK(back.removeBlock(td, kNow + 30));          // takes its children along
	CHECK(back.findBlock(sub) == nullptr);
	CHECK_FALSE(back.removeBlock(td, kNow + 31));
	CHECK(back.findBlock(h) != nullptr);
}

TEST_CASE("planner page keeps unknown blocks and keys")
{
	Page page;
	const char* const text = R"({
    "version": 1,
    "id": "33333333333333333333333333333333",
    "title": "Future page",
    "cover": "sunset",
    "blocks": [
        {"id": "44444444444444444444444444444441", "type": "synced", "text": "kept", "source": "elsewhere",
         "children": [{"id": "44444444444444444444444444444442", "type": "paragraph", "text": "child", "mood": 1}]},
        42
    ]
})";
	REQUIRE(page.fromJson(text).ok);
	REQUIRE(page.blocks.size() == 1);
	CHECK(page.blocks[0].type == BlockType::Other);
	CHECK(page.blocks[0].children.size() == 1);
	const std::string out = page.toJson();
	for (const char* needle : { "\"cover\"", "\"synced\"", "\"source\"", "\"mood\"", "42" })
		CHECK_MESSAGE(out.find(needle) != std::string::npos, needle);
	Page second;
	REQUIRE(second.fromJson(out).ok);
	CHECK(second.toJson() == out);
}

TEST_CASE("planner page: what the block editor does to the tree")
{
	Page page;
	page.id = UUID::generate();
	const UUID a = page.addBlock(BlockType::Paragraph, "a", kNow)->id;
	const UUID c = page.addBlock(BlockType::Paragraph, "c", kNow)->id;

	// A block right after another, at its level; {} appends.
	const UUID b = page.addBlockAfter(a, BlockType::Todo, "b", kNow + 1)->id;
	CHECK(page.blocks.size() == 3);
	CHECK(page.blocks[1].id == b);
	CHECK(page.blocks[2].id == c);
	CHECK(page.addBlockAfter(UUID{}, BlockType::Heading1, "end", kNow + 2) != nullptr);
	CHECK(page.blocks.back().text == "end");
	CHECK(page.addBlockAfter(UUID::generate(), BlockType::Paragraph, "x", kNow) == nullptr);

	// Type, text, checked.
	CHECK(page.setBlockType(b, BlockType::Quote, kNow + 3));
	CHECK(page.findBlock(b)->type == BlockType::Quote);
	CHECK_FALSE(page.setBlockType(b, BlockType::Other, kNow));
	CHECK(page.setBlockText(b, "bb", kNow + 4));
	CHECK(page.setBlockChecked(b, true, kNow + 5));
	CHECK(page.findBlock(b)->text == "bb");
	CHECK(page.findBlock(b)->checked);
	CHECK(page.modifiedMs == kNow + 5);

	// Indent: under the block above; the first at its level cannot.
	CHECK_FALSE(page.indentBlock(a, kNow));
	CHECK(page.indentBlock(b, kNow + 6));
	CHECK(page.blocks.size() == 3);
	REQUIRE(page.findBlock(a)->children.size() == 1);
	CHECK(page.findBlock(a)->children[0].id == b);
	// A child after a child indents again under its older sibling.
	const UUID b2 = page.addBlockAfter(b, BlockType::Paragraph, "b2", kNow + 7)->id;
	CHECK(page.indentBlock(b2, kNow + 8));
	CHECK(page.findBlock(b)->children.size() == 1);

	// Outdent: back out, and what was below it follows as its children.
	CHECK(page.outdentBlock(b2, kNow + 9));
	CHECK(page.findBlock(b)->children.empty());
	CHECK(page.findBlock(a)->children.size() == 2);
	CHECK(page.outdentBlock(b, kNow + 10));
	CHECK(page.blocks[1].id == b);
	CHECK(page.findBlock(b)->children.size() == 1);   // b2 stayed below b
	CHECK_FALSE(page.outdentBlock(a, kNow));          // top level: nothing to leave

	// Up and down among siblings, not past either end.
	CHECK(page.moveBlockBy(c, -1, kNow + 11));
	CHECK(page.blocks[1].id == c);
	CHECK_FALSE(page.moveBlockBy(a, -1, kNow));
	CHECK(page.moveBlockBy(a, 1, kNow + 12));
	CHECK(page.blocks[1].id == a);
	CHECK_FALSE(page.moveBlockBy(page.blocks.back().id, 1, kNow));
}
