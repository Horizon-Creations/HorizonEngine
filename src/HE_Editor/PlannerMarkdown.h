#pragma once
#include "Planner.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// ── A page as Markdown, and back ─────────────────────────────────────────────
// For the MCP tools (and anything else that wants a page as text): a page's blocks
// become Markdown a person or a model reads and writes without knowing the block
// model, and Markdown becomes blocks again. Not a general Markdown parser: it
// speaks exactly the dialect the block model has.
//
//   paragraph        text                          heading-1/2/3   # / ## / ###
//   bullet           - text                        numbered        1. text
//   to-do            - [ ] text / - [x] text       quote           > text
//   callout          > [!callout] text             divider         ---
//   code             ```lang … ```                 engine object   [label](asset:<id>), (scene:<path>),
//                                                                   (entity:<id>@<scene>), (panel:<name>)
//   page link        [[Page title]]                database view   {{database: Name}}
//
// Children are indented two spaces under their parent. A line that is none of the
// above is a paragraph, one block per line; blank lines only separate.
namespace HE::Planner
{

struct MarkdownContext
{
	// A page / a database by the name written in the Markdown; {} when there is none
	// (the line then stays a plain paragraph, so no text is lost).
	std::function<UUID(const std::string&)> page;
	std::function<UUID(const std::string&)> database;
	// The other way, for rendering: the title of a page / name of a database, "" if unknown.
	std::function<std::string(const UUID&)> pageTitle;
	std::function<std::string(const UUID&)> databaseName;
};

std::string pageToMarkdown(const Page& page, const MarkdownContext& ctx = {});
std::string blocksToMarkdown(const std::vector<Block>& blocks, const MarkdownContext& ctx = {});

// New blocks with fresh ids, in a tree by indentation.
std::vector<Block> markdownToBlocks(std::string_view markdown, const MarkdownContext& ctx = {});

} // namespace HE::Planner
