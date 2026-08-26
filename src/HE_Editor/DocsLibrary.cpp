#include "DocsLibrary.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace HE::Ed::Docs
{
namespace
{
	using nlohmann::json;

	// The schema this build understands. A bundle from a NEWER generator is
	// refused rather than half-read: an unknown block kind would silently
	// disappear from the page, which is the one failure a reader cannot detect.
	constexpr int kSchemaVersion = 1;

	// ── ASCII case folding ───────────────────────────────────────────────────
	// Deliberately ASCII-only. The docs are English, but they are full of typo-
	// graphic punctuation (— … ·) and the occasional accented word, and a naive
	// std::tolower over a UTF-8 byte sequence would corrupt the continuation
	// bytes. Leaving every byte >= 0x80 alone keeps those sequences intact; the
	// only cost is that a search for "uber" does not match "über", which no page
	// in this manual needs.
	std::string lower(std::string_view s)
	{
		std::string out;
		out.reserve(s.size());
		for (unsigned char c : s)
			out.push_back(c < 0x80 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c));
		return out;
	}

	Style styleFromCode(std::string_view s)
	{
		if (s == "b") return Style::Bold;
		if (s == "i") return Style::Italic;
		if (s == "c") return Style::Code;
		if (s == "l") return Style::Link;
		return Style::Body;
	}

	BlockKind kindFromCode(std::string_view k)
	{
		if (k == "p")       return BlockKind::Paragraph;
		if (k == "lead")    return BlockKind::Lead;
		if (k == "h3")      return BlockKind::Heading;
		if (k == "ul")      return BlockKind::Bullets;
		if (k == "ol")      return BlockKind::Numbers;
		if (k == "table")   return BlockKind::Table;
		if (k == "code")    return BlockKind::Code;
		if (k == "callout") return BlockKind::Callout;
		if (k == "flow")    return BlockKind::Flow;
		if (k == "figure")  return BlockKind::Figure;
		if (k == "tile")    return BlockKind::Tile;
		return BlockKind::Unknown;
	}

	Tone toneFromCode(std::string_view t)
	{
		if (t == "warning") return Tone::Warning;
		if (t == "tip")     return Tone::Tip;
		return Tone::Note;
	}

	std::string str(const json& j, const char* key)
	{
		auto it = j.find(key);
		return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
	}

	Cell parseRuns(const json& j)
	{
		Cell out;
		if (!j.is_array()) return out;
		out.reserve(j.size());
		for (const json& r : j)
		{
			if (!r.is_object()) continue;
			Run run;
			run.text  = str(r, "t");
			run.style = styleFromCode(str(r, "s"));
			run.href  = str(r, "h");
			if (!run.text.empty()) out.push_back(std::move(run));
		}
		return out;
	}

	Cells parseCells(const json& j)
	{
		Cells out;
		if (!j.is_array()) return out;
		out.reserve(j.size());
		for (const json& c : j) out.push_back(parseRuns(c));
		return out;
	}

	Block parseBlock(const json& j)
	{
		Block b;
		b.kind = kindFromCode(str(j, "k"));
		switch (b.kind)
		{
		case BlockKind::Paragraph:
		case BlockKind::Lead:
		case BlockKind::Heading:
			b.runs = parseRuns(j.value("r", json::array()));
			break;
		case BlockKind::Bullets:
		case BlockKind::Numbers:
			b.items = parseCells(j.value("items", json::array()));
			break;
		case BlockKind::Table:
			b.head = parseCells(j.value("head", json::array()));
			for (const json& row : j.value("rows", json::array()))
				b.rows.push_back(parseCells(row));
			break;
		case BlockKind::Code:
			b.text  = str(j, "text");
			b.title = str(j, "title");
			break;
		case BlockKind::Callout:
			b.tone = toneFromCode(str(j, "tone"));
			for (const json& inner : j.value("blocks", json::array()))
				b.blocks.push_back(parseBlock(inner));
			break;
		case BlockKind::Flow:
			for (const json& s : j.value("steps", json::array()))
				b.steps.push_back({ str(s, "label"), str(s, "sub") });
			break;
		case BlockKind::Figure:
			b.src = str(j, "src");
			b.alt = str(j, "alt");
			break;
		case BlockKind::Tile:
			b.title = str(j, "title");
			b.sub   = str(j, "sub");
			b.href  = str(j, "href");
			break;
		case BlockKind::Unknown:
			break;
		}
		return b;
	}

	// ── Query tokens ─────────────────────────────────────────────────────────
	// Split on anything that is not a word character, so "Ctrl+P", "onUpdate()"
	// and "sky/weather" all break into the words a user actually meant. Single
	// characters are kept — "3D" and "UI" are real queries — but a token that is
	// only punctuation is not.
	std::vector<std::string> tokenize(std::string_view q, std::size_t maxTokens = 8)
	{
		std::vector<std::string> out;
		std::string cur;
		auto flush = [&] {
			if (!cur.empty() && out.size() < maxTokens) out.push_back(cur);
			cur.clear();
		};
		for (unsigned char c : q)
		{
			if (std::isalnum(c) || c >= 0x80) cur.push_back(static_cast<char>(std::tolower(c)));
			else                              flush();
		}
		flush();
		return out;
	}

	bool isWordChar(unsigned char c) { return std::isalnum(c) != 0 || c == '_'; }

	// ── Word matching ────────────────────────────────────────────────────────
	// Every match in this file is anchored to the START of a word. A plain
	// substring search looks fine until someone types a short word: "ui" then
	// hits "build", "guide" and "quick", "light" hits "highlight", and the
	// result list stops being about what was asked. The END is deliberately
	// left open, so "shader" still matches "shaders" and "profil" matches
	// "profiler" as the user is still typing.
	//
	// `cap` bounds the count because a section that says a word forty times is
	// not forty times the answer — it is usually the reference table.
	int countWordMatches(const std::string& hay, const std::string& needle, int cap)
	{
		if (needle.empty()) return 0;
		int n = 0;
		for (std::size_t p = hay.find(needle); p != std::string::npos && n < cap;
		     p = hay.find(needle, p + 1))
			if (p == 0 || !isWordChar(static_cast<unsigned char>(hay[p - 1]))) ++n;
		return n;
	}

	bool containsWord(const std::string& hay, const std::string& needle)
	{
		return countWordMatches(hay, needle, 1) > 0;
	}

	// The one piece of morphology worth having: people search in the plural for
	// something the manual titles in the singular ("shortcuts" → "Shortcut
	// Reference") and the other way round. Tried only when the typed word found
	// nothing, and scored lower, so an exact match always wins.
	std::string otherNumber(const std::string& token)
	{
		if (token.size() >= 4 && token.back() == 's') return token.substr(0, token.size() - 1);
		if (token.size() >= 3 && token.back() != 's') return token + "s";
		return {};
	}

	// ── Snippet ──────────────────────────────────────────────────────────────
	// The line under a result has one job: show the query IN CONTEXT so the user
	// can tell two similar hits apart without opening both. Centred on the first
	// match, snapped to word boundaries, elided where it was cut.
	std::string snippetAround(const std::string& text, const std::string& lowerText,
	                          const std::vector<std::string>& tokens)
	{
		constexpr std::size_t kRadius = 90;

		std::size_t at = std::string::npos;
		for (const std::string& t : tokens)
		{
			std::size_t p = lowerText.find(t);
			// The section may have matched on the other number ("shortcut" for a
			// typed "shortcuts"); centre the snippet on that word rather than
			// silently falling back to the start of the section.
			if (p == std::string::npos)
				if (const std::string alt = otherNumber(t); !alt.empty()) p = lowerText.find(alt);
			if (p != std::string::npos && (at == std::string::npos || p < at)) at = p;
		}
		if (at == std::string::npos) at = 0;

		std::size_t begin = at > kRadius ? at - kRadius : 0;
		std::size_t end   = std::min(text.size(), at + kRadius);
		// Snap outward-in to whitespace so the snippet never starts or ends
		// mid-word (or, worse, mid-UTF-8-sequence).
		while (begin > 0 && !std::isspace(static_cast<unsigned char>(text[begin]))) --begin;
		while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;

		std::string out;
		if (begin > 0) out += "... ";
		out.append(text, begin, end - begin);
		if (end < text.size()) out += " ...";

		// Collapse the whitespace the flattener left behind, so a snippet that
		// crossed a block boundary reads as one line.
		std::string tidy;
		tidy.reserve(out.size());
		bool space = false;
		for (char c : out)
		{
			const bool ws = std::isspace(static_cast<unsigned char>(c)) != 0;
			if (ws) { space = true; continue; }
			if (space && !tidy.empty()) tidy.push_back(' ');
			space = false;
			tidy.push_back(c);
		}
		return tidy;
	}
} // namespace

// ── Loading ──────────────────────────────────────────────────────────────────
bool Library::load(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
	{
		m_pages.clear();
		m_groups.clear();
		m_index.clear();
		m_error = "could not open " + path;
		return false;
	}
	std::ostringstream ss;
	ss << in.rdbuf();
	return loadFromJson(ss.str());
}

bool Library::loadFromJson(std::string_view text)
{
	m_pages.clear();
	m_groups.clear();
	m_index.clear();
	m_baseUrl.clear();
	m_generated.clear();
	m_error.clear();

	json root = json::parse(text, nullptr, false);
	if (root.is_discarded() || !root.is_object())
	{
		m_error = "bundle is not valid JSON";
		return false;
	}

	const int version = root.value("version", 0);
	if (version > kSchemaVersion)
	{
		m_error = "bundle schema v" + std::to_string(version) +
		          " is newer than this editor understands (v" +
		          std::to_string(kSchemaVersion) + ")";
		return false;
	}

	m_baseUrl   = root.value("baseUrl", std::string());
	m_generated = root.value("generated", std::string());

	for (const json& p : root.value("pages", json::array()))
	{
		if (!p.is_object()) continue;
		Page page;
		page.id      = str(p, "id");
		page.file    = str(p, "file");
		page.title   = str(p, "title");
		page.summary = str(p, "summary");
		if (page.id.empty()) continue;

		const int pageIdx = static_cast<int>(m_pages.size());
		for (const json& s : p.value("sections", json::array()))
		{
			if (!s.is_object()) continue;
			Section sec;
			sec.id      = str(s, "id");
			sec.title   = str(s, "title");
			sec.eyebrow = str(s, "eyebrow");
			sec.text    = str(s, "text");
			sec.page    = pageIdx;
			for (const json& b : s.value("blocks", json::array()))
				sec.blocks.push_back(parseBlock(b));
			page.sections.push_back(std::move(sec));
		}
		m_pages.push_back(std::move(page));
	}

	if (m_pages.empty())
	{
		m_error = "bundle contains no pages";
		return false;
	}

	for (const json& g : root.value("groups", json::array()))
	{
		if (!g.is_object()) continue;
		Group group;
		group.title = str(g, "title");
		for (const json& id : g.value("pages", json::array()))
		{
			if (!id.is_string()) continue;
			const int idx = pageIndex(id.get<std::string>());
			if (idx >= 0) group.pages.push_back(idx);
		}
		if (!group.pages.empty()) m_groups.push_back(std::move(group));
	}
	// A bundle whose groups did not survive (an older generator, a hand-edited
	// file) still has to be navigable, so fall back to one flat group rather
	// than an empty sidebar.
	if (m_groups.empty())
	{
		Group all;
		all.title = "Documentation";
		for (int i = 0; i < static_cast<int>(m_pages.size()); ++i) all.pages.push_back(i);
		m_groups.push_back(std::move(all));
	}

	buildIndex();
	return true;
}

void Library::appendPage(Page page)
{
	if (page.id.empty()) return;

	// The section→page back-reference is an index, so it has to be fixed up for
	// wherever the page lands — and a replacement must land in the SAME slot, or
	// every group entry pointing at the old index would now name another page.
	int slot = pageIndex(page.id);
	if (slot < 0)
	{
		slot = static_cast<int>(m_pages.size());
		m_pages.push_back(std::move(page));
	}
	else
	{
		m_pages[static_cast<std::size_t>(slot)] = std::move(page);
	}
	for (Section& s : m_pages[static_cast<std::size_t>(slot)].sections) s.page = slot;

	// A page that no group lists is unreachable from the sidebar. Put a new one
	// in the last group ("Reference", where the bundle's own reference pages
	// live) rather than inventing one for it.
	bool listed = false;
	for (const Group& g : m_groups)
		for (int i : g.pages) if (i == slot) listed = true;
	if (!listed && !m_groups.empty()) m_groups.back().pages.push_back(slot);

	buildIndex();
}

void Library::buildIndex()
{
	m_index.clear();
	m_index.reserve(m_pages.size());
	for (const Page& p : m_pages)
	{
		std::vector<Index> sections;
		sections.reserve(p.sections.size());
		const std::string pageTitle = lower(p.title);
		for (const Section& s : p.sections)
			sections.push_back({ lower(s.title), lower(s.eyebrow), lower(s.text), pageTitle });
		m_index.push_back(std::move(sections));
	}
}

// ── Lookup ───────────────────────────────────────────────────────────────────
int Library::pageIndex(std::string_view id) const
{
	for (int i = 0; i < static_cast<int>(m_pages.size()); ++i)
		if (m_pages[i].id == id) return i;
	return -1;
}

const Page* Library::page(std::string_view id) const
{
	const int i = pageIndex(id);
	return i >= 0 ? &m_pages[i] : nullptr;
}

bool Library::resolve(std::string_view topic, int& pageOut, int& sectionOut) const
{
	pageOut = sectionOut = -1;
	if (topic.empty()) return false;

	std::string_view pageId = topic, sectionId;
	if (const std::size_t hash = topic.find('#'); hash != std::string_view::npos)
	{
		pageId    = topic.substr(0, hash);
		sectionId = topic.substr(hash + 1);
	}
	// Tolerate a topic written the way the website spells it ("editor.html#x"):
	// the help table and the docs' own cross-links use both forms.
	if (pageId.size() > 5 && pageId.substr(pageId.size() - 5) == ".html")
		pageId = pageId.substr(0, pageId.size() - 5);

	pageOut = pageIndex(pageId);
	if (pageOut < 0) return false;
	if (sectionId.empty()) return true;

	const std::vector<Section>& sections = m_pages[pageOut].sections;
	for (int i = 0; i < static_cast<int>(sections.size()); ++i)
		if (sections[i].id == sectionId) { sectionOut = i; break; }
	return true;
}

std::string Library::topicOf(int page, int section) const
{
	if (page < 0 || page >= static_cast<int>(m_pages.size())) return {};
	const Page& p = m_pages[page];
	if (section < 0 || section >= static_cast<int>(p.sections.size())) return p.id;
	return p.id + "#" + p.sections[section].id;
}

std::string Library::url(int page, int section) const
{
	if (page < 0 || page >= static_cast<int>(m_pages.size())) return m_baseUrl;
	const Page& p = m_pages[page];
	std::string out = m_baseUrl + p.file;
	if (section >= 0 && section < static_cast<int>(p.sections.size()) &&
	    !p.sections[section].id.empty())
		out += "#" + p.sections[section].id;
	return out;
}

// ── Search ───────────────────────────────────────────────────────────────────
// The weights encode one rule: a section whose HEADING is what you typed is
// what you meant, and a section that merely mentions the word forty times is
// not. Hence the wide gap between a title hit and a body hit, and the cap on
// how often a body occurrence can keep paying.
std::vector<Hit> Library::search(std::string_view query, int maxHits) const
{
	std::vector<Hit> hits;
	const std::vector<std::string> tokens = tokenize(query);
	if (tokens.empty() || m_index.empty()) return hits;

	const std::string phrase = lower(query);

	for (int pi = 0; pi < static_cast<int>(m_pages.size()); ++pi)
	{
		for (int si = 0; si < static_cast<int>(m_pages[pi].sections.size()); ++si)
		{
			const Index& idx = m_index[pi][si];
			float score = 0.0f;
			bool  all   = true;

			// Scoring one word against one section. Returned separately from the
			// loop so the plural fallback can re-run it on the other form.
			auto scoreToken = [&](const std::string& t) {
				float best = 0.0f;
				if (containsWord(idx.title, t))        best = 120.0f;
				else if (containsWord(idx.eyebrow, t)) best = 45.0f;
				else if (containsWord(idx.pageTitle, t)) best = 35.0f;
				if (const int n = countWordMatches(idx.text, t, 6); n > 0)
					best = std::max(best, 8.0f) + static_cast<float>(n) * 4.0f;
				return best;
			};

			for (const std::string& t : tokens)
			{
				float best = scoreToken(t);
				if (best <= 0.0f)
				{
					if (const std::string alt = otherNumber(t); !alt.empty())
						best = scoreToken(alt) * 0.7f;
				}
				if (best <= 0.0f) { all = false; break; }
				score += best;
			}
			if (!all) continue;

			// The whole query as one string beats the same words scattered: it is
			// the difference between "shadow map" the term and a section that says
			// "shadow" once and "map" once about different things.
			if (tokens.size() > 1)
			{
				if (idx.title.find(phrase) != std::string::npos) score += 200.0f;
				else if (idx.text.find(phrase) != std::string::npos) score += 60.0f;
			}
			// Nudge the earlier pages up between otherwise equal hits: the sidebar
			// order runs from Getting Started to the reference, which is also the
			// order of how likely a section is to be what a newcomer wanted.
			score -= static_cast<float>(pi) * 0.25f;

			Hit h;
			h.page    = pi;
			h.section = si;
			h.score   = score;
			h.snippet = snippetAround(m_pages[pi].sections[si].text, idx.text, tokens);
			hits.push_back(std::move(h));
		}
	}

	std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
		if (a.score != b.score) return a.score > b.score;
		if (a.page != b.page)   return a.page < b.page;
		return a.section < b.section;
	});
	if (maxHits > 0 && static_cast<int>(hits.size()) > maxHits)
		hits.resize(static_cast<std::size_t>(maxHits));
	return hits;
}

// ── The shared instance + where the bundle lives ─────────────────────────────
Library& library()
{
	static Library s_library;
	return s_library;
}

std::string bundlePath(std::string_view basePath)
{
	return std::string(basePath) + "Docs/he-docs.json";
}

std::string imageDir(std::string_view basePath)
{
	return std::string(basePath) + "Docs/img/";
}

} // namespace HE::Ed::Docs
