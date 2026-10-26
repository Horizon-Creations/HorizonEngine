#include "PlannerMarkdown.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace HE::Planner
{

namespace
{

// ── Rendering ────────────────────────────────────────────────────────────────

std::string linkLine(const TaskLink& l)
{
	std::string label = l.label.empty() ? std::string("link") : l.label;
	switch (l.kind)
	{
	case TaskLinkKind::Asset:  return "[" + label + "](asset:" + uuidToHex(l.id) + ")";
	case TaskLinkKind::Scene:  return "[" + label + "](scene:" + l.scene + ")";
	case TaskLinkKind::Entity: return "[" + label + "](entity:" + uuidToHex(l.id) + "@" + l.scene + ")";
	case TaskLinkKind::Panel:  return "[" + label + "](panel:" + l.panel + ")";
	default:                   return "[" + label + "](" + l.kindOther + ":)";
	}
}

void render(const std::vector<Block>& blocks, int depth, const MarkdownContext& ctx, std::string& out)
{
	const std::string pad(static_cast<std::size_t>(depth) * 2, ' ');
	int number = 0;
	bool previousWasParagraph = false;
	for (const Block& b : blocks)
	{
		number = b.type == BlockType::Numbered ? number + 1 : 0;
		// A blank line before a paragraph or heading, and after a paragraph, keeps the Markdown
		// readable; list items and the like stay together.
		const bool paragraphLike = b.type == BlockType::Paragraph || b.type == BlockType::Heading1 ||
		                           b.type == BlockType::Heading2 || b.type == BlockType::Heading3;
		if (!out.empty() && (paragraphLike || previousWasParagraph) && out.back() == '\n' &&
		    (out.size() < 2 || out[out.size() - 2] != '\n'))
			out += "\n";
		previousWasParagraph = b.type == BlockType::Paragraph;

		switch (b.type)
		{
		case BlockType::Heading1: out += pad + "# " + b.text + "\n"; break;
		case BlockType::Heading2: out += pad + "## " + b.text + "\n"; break;
		case BlockType::Heading3: out += pad + "### " + b.text + "\n"; break;
		case BlockType::Bullet:   out += pad + "- " + b.text + "\n"; break;
		case BlockType::Numbered: out += pad + std::to_string(number) + ". " + b.text + "\n"; break;
		case BlockType::Todo:     out += pad + (b.checked ? "- [x] " : "- [ ] ") + b.text + "\n"; break;
		case BlockType::Quote:    out += pad + "> " + b.text + "\n"; break;
		case BlockType::Callout:  out += pad + "> [!callout] " + b.text + "\n"; break;
		case BlockType::Divider:  out += pad + "---\n"; break;
		case BlockType::Code:
			out += pad + "```" + b.language + "\n";
			{
				std::istringstream in(b.text);
				std::string line;
				while (std::getline(in, line)) out += pad + line + "\n";
			}
			out += pad + "```\n";
			break;
		case BlockType::EngineLink:   out += pad + linkLine(b.link) + "\n"; break;
		case BlockType::PageLink:
		{
			const std::string title = ctx.pageTitle ? ctx.pageTitle(b.page) : std::string{};
			out += pad + "[[" + title + "]]\n";
			break;
		}
		case BlockType::DatabaseView:
		{
			const std::string name = ctx.databaseName ? ctx.databaseName(b.database) : std::string{};
			out += pad + "{{database: " + name + "}}\n";
			break;
		}
		case BlockType::Paragraph:
		default:                  out += pad + b.text + "\n"; break;
		}
		render(b.children, depth + 1, ctx, out);
	}
}

// ── Parsing ──────────────────────────────────────────────────────────────────

std::string trimRight(std::string s)
{
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
	return s;
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// "[label](kind:rest)" as the whole line.
bool parseLinkLine(const std::string& line, TaskLink& out)
{
	if (line.size() < 6 || line.front() != '[') return false;
	const std::size_t close = line.find("](");
	if (close == std::string::npos || line.back() != ')') return false;
	const std::string label = line.substr(1, close - 1);
	const std::string target = line.substr(close + 2, line.size() - close - 3);
	const std::size_t colon = target.find(':');
	if (colon == std::string::npos) return false;
	const std::string kind = target.substr(0, colon), rest = target.substr(colon + 1);
	UUID id;
	if (kind == "asset" && uuidFromHex(rest, id)) { out = TaskLink::asset(id, label); return true; }
	if (kind == "scene" && !rest.empty())         { out = TaskLink::sceneLink(rest, label); return true; }
	if (kind == "panel" && !rest.empty())         { out = TaskLink::panelLink(rest, label); return true; }
	if (kind == "entity")
	{
		const std::size_t at = rest.find('@');
		if (at != std::string::npos && uuidFromHex(rest.substr(0, at), id))
		{
			out = TaskLink::entity(id, rest.substr(at + 1), label);
			return true;
		}
	}
	return false;
}

struct Line
{
	int         depth = 0;
	Block       block;
	bool        valid = false;
};

Block make(BlockType type, std::string text)
{
	Block b;
	b.id   = UUID::generate();
	b.type = type;
	b.text = std::move(text);
	return b;
}

// One line (indent already removed) as a block.
Block parseLine(const std::string& s, const MarkdownContext& ctx)
{
	if (s == "---" || s == "***" || s == "___") return make(BlockType::Divider, {});
	if (startsWith(s, "### ")) return make(BlockType::Heading3, s.substr(4));
	if (startsWith(s, "## "))  return make(BlockType::Heading2, s.substr(3));
	if (startsWith(s, "# "))   return make(BlockType::Heading1, s.substr(2));
	if (startsWith(s, "- [ ] ") || startsWith(s, "* [ ] ")) return make(BlockType::Todo, s.substr(6));
	if (startsWith(s, "- [x] ") || startsWith(s, "- [X] ") || startsWith(s, "* [x] ") || startsWith(s, "* [X] "))
	{
		Block b = make(BlockType::Todo, s.substr(6));
		b.checked = true;
		return b;
	}
	if (startsWith(s, "- ") || startsWith(s, "* ")) return make(BlockType::Bullet, s.substr(2));
	{
		std::size_t i = 0;
		while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
		if (i > 0 && i + 1 < s.size() && s[i] == '.' && s[i + 1] == ' ') return make(BlockType::Numbered, s.substr(i + 2));
	}
	if (startsWith(s, "> [!callout] ")) return make(BlockType::Callout, s.substr(13));
	if (startsWith(s, "> "))            return make(BlockType::Quote, s.substr(2));
	TaskLink link;
	if (parseLinkLine(s, link))
	{
		Block b = make(BlockType::EngineLink, {});
		b.link = link;
		return b;
	}
	if (s.size() >= 4 && startsWith(s, "[[") && s.compare(s.size() - 2, 2, "]]") == 0 && ctx.page)
	{
		const UUID target = ctx.page(s.substr(2, s.size() - 4));
		if (target != UUID{})
		{
			Block b = make(BlockType::PageLink, {});
			b.page = target;
			return b;
		}
	}
	if (startsWith(s, "{{database:") && s.size() > 4 && s.compare(s.size() - 2, 2, "}}") == 0 && ctx.database)
	{
		std::string name = s.substr(11, s.size() - 13);
		const auto b0 = name.find_first_not_of(' ');
		name = b0 == std::string::npos ? std::string{} : name.substr(b0);
		const UUID target = ctx.database(name);
		if (target != UUID{})
		{
			Block b = make(BlockType::DatabaseView, {});
			b.database = target;
			return b;
		}
	}
	return make(BlockType::Paragraph, s);
}

} // namespace

std::string blocksToMarkdown(const std::vector<Block>& blocks, const MarkdownContext& ctx)
{
	std::string out;
	render(blocks, 0, ctx, out);
	return out;
}

std::string pageToMarkdown(const Page& page, const MarkdownContext& ctx)
{
	return blocksToMarkdown(page.blocks, ctx);
}

std::vector<Block> markdownToBlocks(std::string_view markdown, const MarkdownContext& ctx)
{
	std::vector<Block> root;
	// The chain of open parents: stack[d] is the vector blocks of depth d go into.
	std::vector<std::vector<Block>*> stack{ &root };

	std::istringstream in{ std::string(markdown) };
	std::string raw;
	bool inCode = false;
	int  codeDepth = 0;
	std::string codeLanguage, codeText;
	auto place = [&](int depth, Block b) {
		depth = std::clamp(depth, 0, static_cast<int>(stack.size()) - 1);
		stack.resize(static_cast<std::size_t>(depth) + 1);
		stack[static_cast<std::size_t>(depth)]->push_back(std::move(b));
		stack.push_back(&stack[static_cast<std::size_t>(depth)]->back().children);
	};

	while (std::getline(in, raw))
	{
		std::string line = trimRight(raw);
		std::size_t spaces = 0;
		while (spaces < line.size() && line[spaces] == ' ') ++spaces;
		std::string body = line.substr(spaces);

		if (inCode)
		{
			if (body == "```")
			{
				Block b = make(BlockType::Code, codeText);
				b.language = codeLanguage;
				place(codeDepth, std::move(b));
				inCode = false;
				codeText.clear();
			}
			else
			{
				// The fence's own indent is not part of the code.
				const std::size_t strip = std::min<std::size_t>(spaces, static_cast<std::size_t>(codeDepth) * 2);
				codeText += (codeText.empty() ? "" : "\n") + line.substr(strip);
			}
			continue;
		}
		if (body.empty()) continue;
		const int depth = static_cast<int>(spaces / 2);
		if (startsWith(body, "```"))
		{
			inCode       = true;
			codeDepth    = depth;
			codeLanguage = body.substr(3);
			continue;
		}
		place(depth, parseLine(body, ctx));
	}
	if (inCode)   // a fence that was never closed: what it held is still a code block
	{
		Block b = make(BlockType::Code, codeText);
		b.language = codeLanguage;
		place(codeDepth, std::move(b));
	}
	return root;
}

} // namespace HE::Planner
