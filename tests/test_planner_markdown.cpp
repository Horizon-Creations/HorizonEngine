// A page as Markdown and back (PlannerMarkdown.h): the dialect the Planner's MCP tools
// speak. What is checked: every block kind renders as documented, what is rendered parses
// back to the same blocks, children follow the indentation, and a line that names a page or
// a database that does not exist stays plain text instead of vanishing.
#include "doctest.h"

#include "PlannerMarkdown.h"

#include <string>

using namespace HE;
using namespace HE::Planner;

namespace
{

UUID hex(const char* s)
{
	UUID id;
	REQUIRE(uuidFromHex(s, id));
	return id;
}

// The shape of a block tree, ignoring ids: "type:text[child,child]".
std::string shape(const std::vector<Block>& blocks)
{
	std::string out;
	for (const Block& b : blocks)
	{
		out += std::string(blockTypeName(b.type)) + ":" + b.text;
		if (b.type == BlockType::Todo) out += b.checked ? "[x]" : "[ ]";
		if (b.type == BlockType::Code) out += "@" + b.language;
		if (!b.children.empty()) out += "{" + shape(b.children) + "}";
		out += ";";
	}
	return out;
}

MarkdownContext context()
{
	MarkdownContext c;
	c.page = [](const std::string& title) {
		UUID id;
		if (title == "Notes") uuidFromHex("0000000000000000000000000000aaaa", id);
		return id;
	};
	c.database = [](const std::string& name) {
		UUID id;
		if (name == "Features") uuidFromHex("0000000000000000000000000000bbbb", id);
		return id;
	};
	c.pageTitle = [](const UUID& id) { return id == hex("0000000000000000000000000000aaaa") ? std::string("Notes") : std::string{}; };
	c.databaseName = [](const UUID& id) { return id == hex("0000000000000000000000000000bbbb") ? std::string("Features") : std::string{}; };
	return c;
}

const char* const kDocument =
	"# Game design\n"
	"\n"
	"Foggy harbour, two islands.\n"
	"\n"
	"## Pillars\n"
	"- Exploration\n"
	"  - Climbing\n"
	"  - Boats\n"
	"- Puzzles\n"
	"1. First\n"
	"2. Second\n"
	"- [ ] Ship alpha\n"
	"- [x] Block out the harbour\n"
	"> Keep it small\n"
	"> [!callout] Mind the budget\n"
	"---\n"
	"```cpp\n"
	"int x = 1;\n"
	"\n"
	"return x;\n"
	"```\n"
	"[Crate](asset:00000000000000000000000000000c0a)\n"
	"[Harbour](scene:Content/Levels/Harbour.hescene)\n"
	"[Crate](entity:00000000000000000000000000000e01@Content/Levels/Harbour.hescene)\n"
	"[Console](panel:Console)\n"
	"[[Notes]]\n"
	"{{database: Features}}\n";

} // namespace

TEST_CASE("planner markdown: every block kind parses, and renders back as it was written")
{
	const std::vector<Block> blocks = markdownToBlocks(kDocument, context());
	CHECK(shape(blocks) ==
	      "heading-1:Game design;paragraph:Foggy harbour, two islands.;heading-2:Pillars;"
	      "bullet:Exploration{bullet:Climbing;bullet:Boats;};bullet:Puzzles;"
	      "numbered:First;numbered:Second;todo:Ship alpha[ ];todo:Block out the harbour[x];"
	      "quote:Keep it small;callout:Mind the budget;divider:;code:int x = 1;\n\nreturn x;@cpp;"
	      "engine-link:;engine-link:;engine-link:;engine-link:;page-link:;database-view:;");

	// Ids are fresh and distinct; the link blocks carry their targets.
	CHECK(blocks[0].id != blocks[1].id);
	const Block& crate = blocks[blocks.size() - 6];
	CHECK(crate.link.kind == TaskLinkKind::Asset);
	CHECK(crate.link.id == hex("00000000000000000000000000000c0a"));
	CHECK(crate.link.label == "Crate");
	CHECK(blocks[blocks.size() - 5].link.scene == "Content/Levels/Harbour.hescene");
	CHECK(blocks[blocks.size() - 4].link.kind == TaskLinkKind::Entity);
	CHECK(blocks[blocks.size() - 4].link.scene == "Content/Levels/Harbour.hescene");
	CHECK(blocks[blocks.size() - 3].link.panel == "Console");
	CHECK(blocks[blocks.size() - 2].page == hex("0000000000000000000000000000aaaa"));
	CHECK(blocks.back().database == hex("0000000000000000000000000000bbbb"));

	// And back: what was written is what is rendered.
	CHECK(blocksToMarkdown(blocks, context()) == kDocument);
}

TEST_CASE("planner markdown: a page renders blocks made in the editor")
{
	Page page;
	page.id = UUID::generate();
	page.addBlock(BlockType::Heading1, "Title", 1);
	Block* todo = page.addBlock(BlockType::Todo, "Do it", 2);
	todo->checked = true;
	page.addBlock(BlockType::Numbered, "a", 3);
	page.addBlock(BlockType::Numbered, "b", 4);
	page.addBlock(BlockType::Paragraph, "after the list", 5);
	page.addBlock(BlockType::Numbered, "again", 6);   // numbering restarts after another kind
	CHECK(pageToMarkdown(page) ==
	      "# Title\n"
	      "- [x] Do it\n"
	      "1. a\n"
	      "2. b\n"
	      "\n"
	      "after the list\n"
	      "\n"                      // text is set off from what follows it, too
	      "1. again\n");
}

TEST_CASE("planner markdown: indentation makes children, a jump in depth is one level, a fence can stay open")
{
	const std::vector<Block> blocks = markdownToBlocks(
		"- a\n"
		"      - deep jump\n"      // six spaces after a top-level item: one level down, not three
		"  - b\n"
		"- c\n"
		"  ```\n"
		"  indented code\n"
		"  ```\n"
		"```\n"
		"never closed\n");
	CHECK(shape(blocks) ==
	      "bullet:a{bullet:deep jump;bullet:b;};bullet:c{code:indented code@;};code:never closed@;");
}

TEST_CASE("planner markdown: a page or database that does not exist stays text")
{
	const std::vector<Block> blocks = markdownToBlocks("[[Nowhere]]\n{{database: Nothing}}\n[[Notes]]\n", context());
	CHECK(shape(blocks) == "paragraph:[[Nowhere]];paragraph:{{database: Nothing}};page-link:;");
	// With no resolver at all nothing is resolved, and nothing is lost.
	CHECK(shape(markdownToBlocks("[[Notes]]\n")) == "paragraph:[[Notes]];");
	// Windows line ends and trailing blanks are not part of the text.
	CHECK(shape(markdownToBlocks("# T\r\nline  \r\n")) == "heading-1:T;paragraph:line;");
	CHECK(markdownToBlocks("").empty());
	CHECK(markdownToBlocks("\n\n  \n").empty());
}
