#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// ── The manual, in the editor ────────────────────────────────────────────────
// The published documentation, converted to blocks by scripts/build_docs_bundle.py
// and shipped next to the executable (EditorDeps/Docs/he-docs.json). This file is
// the model over it: load, look a topic up, search. DocsPanel is the skin.
//
// Like TutorialSteps, this translation unit deliberately knows nothing about
// ImGui, SDL or AppContext — the whole point is that the content and the search
// can be tested without a window (tests/test_docs_library.cpp), and that the
// panel is left with nothing but drawing to get wrong.
//
// The bundle is DATA, not code: it is regenerated from the website whenever the
// docs change, so nothing here may hard-code a page id, a section id or a
// heading. The one place that legitimately names topics is EditorHelp's table,
// and there is a test whose only job is to fail when one of those names stops
// resolving.

namespace HE::Ed::Docs
{

// ── Inline text ──────────────────────────────────────────────────────────────
// A paragraph is a sequence of styled runs rather than a string: the reference
// pages are mostly API names (code), the terms being defined (bold) and cross
// references (links), and flattening those to one grey wall is most of what
// makes generated documentation unreadable.
enum class Style : std::uint8_t { Body, Bold, Italic, Code, Link };

struct Run
{
	std::string text;
	Style       style = Style::Body;
	// Link targets only. Either "page" / "page#section" (another topic in this
	// bundle) or an absolute URL to open in the browser.
	std::string href;
};

using Cell  = std::vector<Run>;   // one table cell / one list item
using Cells = std::vector<Cell>;

enum class BlockKind : std::uint8_t
{
	Paragraph,  // runs
	Lead,       // runs — the section's opening sentence, drawn larger
	Heading,    // runs — a sub-heading inside the section
	Bullets,    // items
	Numbers,    // items
	Table,      // head + rows
	Code,       // text (+ title, e.g. a file name)
	Callout,    // tone + blocks
	Flow,       // steps — the website's arrow diagrams, as an ordered list
	Figure,     // src (a file in Docs/img) + alt
	Tile,       // title + sub + href — a "read on" link card
	// A picture of the node itself: its header in its own colour, its pins where
	// they sit on it, in the glyphs and colours the canvas draws them with.
	// Never comes from the website bundle — the node reference is built from the
	// engine's registries at run time (HcNodeReference), so the picture is drawn
	// from the same signature the real node has rather than photographed from a
	// build that has since moved on.
	NodePreview,
	Unknown,
};

// One row of a Pins block. The colour is resolved when the page is built, so
// this file needs to know nothing about HorizonCode's types.
struct PinRow
{
	std::string   name;
	std::string   type;        // spelled out: "Vec3", "Array<Int>", "Map<String, Bool>"
	std::uint32_t color = 0;   // ImU32, from HcEditorUtil::pinTypeColor
	bool          isExec      = false;
	bool          isContainer = false;
	bool          isInput     = true;
};

enum class Tone : std::uint8_t { Note, Warning, Tip };

struct Block
{
	BlockKind kind = BlockKind::Unknown;

	std::vector<Run>   runs;    // Paragraph, Lead, Heading
	Cells              items;   // Bullets, Numbers
	Cells              head;    // Table header row (empty when the table has none)
	std::vector<Cells> rows;    // Table body
	std::string        text;    // Code body
	std::string        title;   // Code file name · Tile heading
	std::string        sub;     // Tile subtitle
	std::string        href;    // Tile target
	std::string        src;     // Figure image file name (inside Docs/img)
	std::string        alt;     // Figure description
	Tone               tone = Tone::Note;   // Callout
	std::uint32_t      accent = 0;          // NodePreview: the node's header colour

	struct Step { std::string label, sub; };
	std::vector<Step>   steps;   // Flow
	std::vector<Block>  blocks;  // Callout body
	std::vector<PinRow> pins;    // Pins
};

struct Section
{
	std::string        id;        // "play-mode" — stable, part of a topic ref
	std::string        title;
	std::string        eyebrow;   // the small label above the heading
	std::vector<Block> blocks;
	std::string        text;      // every word of the blocks, flattened, for search
	int                page = -1; // index back into pages()
};

struct Page
{
	std::string          id;      // "editor" — the file stem, part of a topic ref
	std::string          file;    // "editor.html", for the online link
	std::string          title;
	std::string          summary;
	std::vector<Section> sections;
};

// The docs' own table of contents ("Manual", "Reference"), in the order the
// website's sidebar lists them.
struct Group
{
	std::string      title;
	std::vector<int> pages;   // indices into pages()
};

// ── Search ───────────────────────────────────────────────────────────────────
// One hit is one SECTION: that is the unit the reader scrolls to, the unit a
// deep link addresses, and small enough that a hit is an answer rather than a
// page to search again by hand.
struct Hit
{
	int         page    = -1;
	int         section = -1;
	float       score   = 0.0f;
	std::string snippet;   // the matching sentence, elided at both ends
};

class Library
{
public:
	// Read and parse the bundle. Replaces whatever was loaded before; on failure
	// the library is left EMPTY (not half-loaded) and error() says why.
	bool load(const std::string& path);
	// Same, from memory — what the tests use, and the only entry point that has
	// to work without a filesystem.
	bool loadFromJson(std::string_view json);

	bool loaded() const { return !m_pages.empty(); }
	const std::string& error() const { return m_error; }
	// The date the bundle was generated ("2026-08-25"), shown in the reader so a
	// stale offline copy is visible rather than merely suspected.
	const std::string& generated() const { return m_generated; }

	const std::vector<Page>&  pages()  const { return m_pages; }
	const std::vector<Group>& groups() const { return m_groups; }

	int         pageIndex(std::string_view id) const;
	const Page* page(std::string_view id) const;

	// Resolve a topic reference — "editor" or "editor#play-mode" — to indices.
	// A page that exists with a section that does not still resolves, to the page
	// and section -1: a renamed anchor should land the reader on the right page,
	// not nowhere. Returns false only when the PAGE is unknown.
	bool resolve(std::string_view topic, int& pageOut, int& sectionOut) const;

	// The topic reference for a position — the inverse of resolve().
	std::string topicOf(int page, int section) const;
	// The published URL for a position, for "open this in the browser".
	std::string url(int page, int section) const;

	// Ranked sections for a query. Tokens are ANDed: every word must appear
	// somewhere in the section (its title, its eyebrow, its page's title or its
	// text), which is what keeps a two-word query from returning half the manual.
	// Empty query → no hits.
	std::vector<Hit> search(std::string_view query, int maxHits = 40) const;

	// Add a page that was not in the bundle, or REPLACE the one with its id.
	// The node reference is generated from the engine's own registries rather
	// than written on the website, and this is how it joins the manual: once it
	// is a page, search, navigation, topics and F1 all reach it with no special
	// case anywhere. Replace-by-id keeps it idempotent — building it twice (a
	// second reader, a test) must not produce two.
	void appendPage(Page page);

private:
	std::vector<Page>  m_pages;
	std::vector<Group> m_groups;
	std::string        m_baseUrl;
	std::string        m_generated;
	std::string        m_error;

	// Lower-cased haystacks, built once at load: search runs on every keystroke
	// and must not re-lower 500 KB of text to answer.
	struct Index
	{
		std::string title, eyebrow, text, pageTitle;
	};
	std::vector<std::vector<Index>> m_index;   // [page][section]

	void buildIndex();
};

// The one library the editor shares — the panel, the help tooltips and the menu
// all address the same loaded bundle.
Library& library();

// Where the bundle sits relative to the executable. Kept here (rather than in
// the panel) so the test can assert the layout the CMake deploy actually
// produces. `basePath` is SDL_GetBasePath(); an empty one yields a relative path.
std::string bundlePath(std::string_view basePath);
// The directory figures are loaded from, same rule.
std::string imageDir(std::string_view basePath);

} // namespace HE::Ed::Docs
