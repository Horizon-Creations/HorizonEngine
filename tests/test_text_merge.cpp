// The conflict window's diff and merge (SourceControl/TextMerge.h): equal runs and
// changed blocks, picks per block, line endings, a big file, binary detection.
#include "doctest.h"

#include <SourceControl/TextMerge.h>

#include <string>

using namespace HE::Sc;

TEST_CASE("text merge: equal texts are one block; a changed line is one block between equal ones")
{
	CHECK(diffText("a\nb\n", "a\nb\n").changes() == 0);
	const TextDiff d = diffText("a\nmine\nc\n", "a\ntheirs\nc\n");
	REQUIRE(d.blocks.size() == 3);
	CHECK(d.blocks[0].same);
	CHECK_FALSE(d.blocks[1].same);
	CHECK(d.blocks[1].ours == std::vector<std::string>{ "mine" });
	CHECK(d.blocks[1].theirs == std::vector<std::string>{ "theirs" });
	CHECK(d.merge({ Pick::Ours }) == "a\nmine\nc\n");
	CHECK(d.merge({ Pick::Theirs }) == "a\ntheirs\nc\n");
	CHECK(d.merge({ Pick::Both }) == "a\nmine\ntheirs\nc\n");
	CHECK(d.merge({}) == "a\ntheirs\nc\n");   // no pick: theirs (what the project has)
}

TEST_CASE("text merge: two separate changes are picked independently; inserts and deletes")
{
	const std::string ours   = "{\n  \"a\": 1,\n  \"b\": 2,\n  \"c\": 3,\n  \"d\": 4\n}\n";
	const std::string theirs = "{\n  \"a\": 10,\n  \"b\": 2,\n  \"c\": 3,\n  \"e\": 5,\n  \"d\": 4\n}\n";
	const TextDiff d = diffText(ours, theirs);
	REQUIRE(d.changes() == 2);
	CHECK(d.merge({ Pick::Ours, Pick::Theirs }) == "{\n  \"a\": 1,\n  \"b\": 2,\n  \"c\": 3,\n  \"e\": 5,\n  \"d\": 4\n}\n");
	CHECK(d.merge({ Pick::Theirs, Pick::Ours }) == "{\n  \"a\": 10,\n  \"b\": 2,\n  \"c\": 3,\n  \"d\": 4\n}\n");
	CHECK(d.merge({ Pick::Ours, Pick::Ours }) == ours);
	CHECK(d.merge({ Pick::Theirs, Pick::Theirs }) == theirs);
}

TEST_CASE("text merge: CRLF and a missing final newline come back as they were")
{
	const TextDiff d = diffText("x\r\ny\r\n", "x\r\nz\r\n");
	CHECK(d.merge({ Pick::Ours }) == "x\r\ny\r\n");
	const TextDiff e = diffText("a\nb", "a\nc\n");
	CHECK(e.merge({ Pick::Ours }) == "a\nb");
	CHECK(e.merge({ Pick::Theirs }) == "a\nc\n");
	CHECK(diffText("", "").changes() == 0);
	CHECK(diffText("", "new\n").merge({ Pick::Theirs }) == "new\n");
	CHECK(diffText("old\n", "").merge({ Pick::Theirs }) == "");
}

TEST_CASE("text merge: a big file with one change stays small, binary files are recognised")
{
	std::string a, b;
	for (int i = 0; i < 20000; ++i)
	{
		a += "line " + std::to_string(i) + "\n";
		b += (i == 12345 ? std::string("changed") : "line " + std::to_string(i)) + "\n";
	}
	const TextDiff d = diffText(a, b);
	CHECK(d.changes() == 1);
	CHECK(d.merge({ Pick::Ours }) == a);
	CHECK(looksBinary(std::string("PNG\0\x01", 5)));
	CHECK_FALSE(looksBinary("plain text\n"));
}
