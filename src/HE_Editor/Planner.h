#pragma once
#include "TaskBoard.h"   // TaskLink (a pointer at an engine object), the UUID/time spellings

#include <Types/UUID.h>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// ── The Planner: databases and pages for planning a game ─────────────────────
// The data and its files only — no ImGui, no network — so the Planner tab, the
// MCP tools and the tests all edit the same thing (the TaskBoard's rule, see
// TaskBoard.h; this file is the same bargain with git, for a richer shape).
//
// Two kinds of document:
//   * a Database: a schema (properties), rows, and saved views (table, board,
//     timeline, calendar: filter, sort, group). A task, a feature, a bug, a
//     milestone are all rows; the property "Links" is an engine-link property
//     whose cells are TaskLinks (asset, entity, scene, panel).
//   * a Page: a tree of blocks (text, headings, lists, to-dos, a callout, an
//     engine object, an embedded database view). A row opens as the Page with
//     the SAME id (created on its first edit, so a row alone costs no file).
//
// ── Where it lives, and why one file each ─────────────────────────────────────
//   <ProjectRoot>/Config/Planner/Databases/<32 hex>.json
//   <ProjectRoot>/Config/Planner/Pages/<32 hex>.json
// Config/ travels with source control (see TaskBoard.h). One file per document,
// not one big file: two people editing two pages never touch the same file, so
// git has nothing to merge. The file name is the id; a file whose "id" disagrees
// is read under the NAME's id (a renamed copy must not shadow the original).
//
// ── The bargain with git (same as TaskBoard.h) ────────────────────────────────
//   * every id is a random UUID, written as 32 lowercase hex digits;
//   * times are wall-clock UTC, ISO 8601, milliseconds;
//   * enums are words ("multi-select", "heading-2"), not numbers;
//   * load-then-save is byte-identical: file order, LF, one trailing newline;
//   * anything this build does not know is KEPT where it was: unknown keys on
//     the document, a property, an option, a row, a view, a block; an unknown
//     property type, view kind or block type is kept as the word it was; a cell
//     is kept as the JSON it was (so a property type from a newer editor loses
//     nothing even though this build cannot show it);
//   * a file that does not parse is never overwritten, nor one that changed
//     under the open document (a pull): that is a reload.
//
// ── Cells ─────────────────────────────────────────────────────────────────────
// A cell is the JSON value the file holds, keyed by the property's id. The typed
// accessors below read and write it by the property's type and never throw: a
// cell of the wrong shape reads as empty; a write replaces it. Shapes:
//   text / url       "string"            number      1.5
//   checkbox         true | false        date        "2026-10-14T00:00:00.000Z"
//   select           "<option id>"       multi-select ["<option id>", ...]
//   person           ["Anna", ...]       relation    ["<row id>", ...]  (rows of relationDb)
//   engine-link      [ {TaskLink}, ... ]
// An empty cell is not written: no key.

namespace HE::Planner
{

// ── Words ────────────────────────────────────────────────────────────────────
// `Other` means "a word this build does not know"; the word itself is kept next
// to the field (typeOther, …) and written back unchanged.
enum class PropertyType : std::uint8_t
{
	Text, Number, Checkbox, Date, Select, MultiSelect, Person, Relation, EngineLink, Url, Other
};
enum class ViewKind : std::uint8_t { Table, Board, Timeline, Calendar, Other };
enum class BlockType : std::uint8_t
{
	Paragraph, Heading1, Heading2, Heading3, Bullet, Numbered, Todo, Quote, Callout,
	Divider, Code, EngineLink, DatabaseView, Other
};

const char* propertyTypeName(PropertyType t);   // "text", "number", "checkbox", "date", "select",
bool        propertyTypeFromName(std::string_view n, PropertyType& out);   // "multi-select", "person", "relation", "engine-link", "url"
const char* viewKindName(ViewKind k);           // "table", "board", "timeline", "calendar"
bool        viewKindFromName(std::string_view n, ViewKind& out);
const char* blockTypeName(BlockType t);         // "paragraph", "heading-1".."heading-3", "bullet", "numbered",
bool        blockTypeFromName(std::string_view n, BlockType& out);         // "todo", "quote", "callout", "divider", "code", "engine-link", "database-view"

// ── Database ─────────────────────────────────────────────────────────────────

struct SelectOption
{
	UUID        id;       // what cells hold, so renaming an option renames it everywhere
	std::string name;
	std::string color;    // a word ("blue"), "" = none
	std::string extraJson;
};

struct Property
{
	UUID         id;
	std::string  name;
	PropertyType type = PropertyType::Text;
	std::string  typeOther;              // the file's word when type == Other
	std::vector<SelectOption> options;   // Select / MultiSelect
	UUID         relationDb;             // Relation: the database whose rows it points at
	std::string  extraJson;

	const SelectOption* findOption(const UUID& optionId) const;
	const SelectOption* findOptionByName(std::string_view name) const;
};

struct Cell
{
	UUID        property;
	std::string json;     // the value as JSON text (never empty)
};

struct Row
{
	UUID         id;      // also the id of this row's Page
	std::string  title;
	std::string  icon;    // one emoji or "", free text
	std::string  createdBy;
	std::int64_t createdMs  = 0;
	std::int64_t modifiedMs = 0;
	std::vector<Cell> cells;   // file order
	std::string  extraJson;

	const Cell* cell(const UUID& property) const;
};

struct Filter
{
	UUID        property;   // {} = the row's title
	// "equals", "not-equals", "contains", "not-contains", "is-empty", "is-not-empty",
	// "gt", "gte", "lt", "lte". Kept as written; an op this build does not know
	// matches every row (a filter you cannot evaluate must not hide work).
	std::string op;
	std::string valueJson;   // the operand as JSON text, "" for is-empty / is-not-empty
};

struct Sort
{
	UUID property;          // {} = the row's title
	bool descending = false;
};

struct View
{
	UUID         id;
	std::string  name;
	ViewKind     kind = ViewKind::Table;
	std::string  kindOther;
	std::vector<Filter> filters;      // all must match
	std::vector<Sort>   sorts;        // first one wins, later ones break ties
	UUID         groupBy;             // Board: the select property that makes the columns
	UUID         dateProperty;        // Timeline start / Calendar day
	UUID         endProperty;         // Timeline end
	std::vector<UUID> hidden;         // properties this view does not show
	std::string  extraJson;
};

class Database
{
public:
	static constexpr int kVersion = 1;

	// A new database: a fresh id and one table view, so it is never viewless.
	static Database create(std::string name);

	UUID         id;
	std::string  name;
	std::string  icon;
	std::vector<Property> properties;
	std::vector<Row>      rows;      // file order; views sort a copy of the indices
	std::vector<View>     views;
	// False for a database whose rows live somewhere else (the Tasks database is
	// a view of the Task Board): toJson() then writes the schema and the views
	// but no rows. Runtime state, never read from or written to a file.
	bool                  persistRows = true;

	// ── Schema ───────────────────────────────────────────────────────────────
	Property*       addProperty(std::string name, PropertyType type);
	// Removes the property, its cells in every row, and it from every view
	// (filters, sorts, grouping, hidden). False when no property has `id`.
	bool            removeProperty(const UUID& id);
	Property*       findProperty(const UUID& id);
	const Property* findProperty(const UUID& id) const;
	const Property* findPropertyByName(std::string_view name) const;
	// nullptr when the property is not a select type; the existing option when
	// one of that name is there already (names are unique within a property).
	SelectOption*   addOption(const UUID& property, std::string name, std::string color = {});
	// Removes the option and clears it from every cell that held it.
	bool            removeOption(const UUID& property, const UUID& option);

	// ── Rows ─────────────────────────────────────────────────────────────────
	Row*       addRow(std::string title, std::string author, std::int64_t nowMs);
	bool       removeRow(const UUID& id);   // also drops it from relation cells in this database
	Row*       findRow(const UUID& id);
	const Row* findRow(const UUID& id) const;
	bool       setTitle(const UUID& row, std::string title, std::int64_t nowMs);

	// ── Cells (all stamp the row's modifiedMs; all false when row/property is unknown
	//    or the property's type does not take that value) ──────────────────────
	bool setText(const UUID& row, const UUID& property, std::string v, std::int64_t nowMs);
	bool setNumber(const UUID& row, const UUID& property, double v, std::int64_t nowMs);
	bool setChecked(const UUID& row, const UUID& property, bool v, std::int64_t nowMs);
	bool setDate(const UUID& row, const UUID& property, std::int64_t ms, std::int64_t nowMs);
	bool setSelect(const UUID& row, const UUID& property, const UUID& option, std::int64_t nowMs);
	bool setMultiSelect(const UUID& row, const UUID& property, std::vector<UUID> options, std::int64_t nowMs);
	bool setPeople(const UUID& row, const UUID& property, std::vector<std::string> people, std::int64_t nowMs);
	bool setRelation(const UUID& row, const UUID& property, std::vector<UUID> targets, std::int64_t nowMs);
	bool setLinks(const UUID& row, const UUID& property, const std::vector<TaskLink>& links, std::int64_t nowMs);
	// Removes the cell (the property keeps its place, the row loses the value).
	bool clearCell(const UUID& row, const UUID& property, std::int64_t nowMs);

	std::string            text(const UUID& row, const UUID& property) const;      // text, url
	double                 number(const UUID& row, const UUID& property) const;    // 0 when empty
	bool                   checked(const UUID& row, const UUID& property) const;
	bool                   date(const UUID& row, const UUID& property, std::int64_t& outMs) const;
	UUID                   select(const UUID& row, const UUID& property) const;    // {} when empty
	std::vector<UUID>      multiSelect(const UUID& row, const UUID& property) const;
	std::vector<std::string> people(const UUID& row, const UUID& property) const;
	std::vector<UUID>      relation(const UUID& row, const UUID& property) const;
	std::vector<TaskLink>  links(const UUID& row, const UUID& property) const;
	bool                   isEmpty(const UUID& row, const UUID& property) const;

	// ── Views ────────────────────────────────────────────────────────────────
	View*       addView(std::string name, ViewKind kind);
	bool        removeView(const UUID& id);   // never the last one: a database always has a view
	View*       findView(const UUID& id);
	const View* findView(const UUID& id) const;

	// Indices into `rows`, filtered and sorted by `view`. Never reorders `rows`.
	// Sorting is stable and puts empty cells last in either direction; two rows
	// the sorts cannot tell apart stay in file order.
	std::vector<std::size_t> query(const View& view) const;
	// Board: the rows of one column — `option` is a select option id, {} is the
	// "No value" column — in `query()` order.
	std::vector<std::size_t> column(const View& view, const UUID& option) const;

	// ── The document ─────────────────────────────────────────────────────────
	struct LoadResult
	{
		bool        ok = false;
		int         version = 0;
		int         dropped = 0;   // non-object entries inside lists whose entries must be objects
		std::string error;
	};
	LoadResult  fromJson(std::string_view text);
	std::string toJson() const;
	int         writeVersion() const { return m_fileVersion > kVersion ? m_fileVersion : kVersion; }

private:
	int                      m_fileVersion = kVersion;
	std::string              m_extraJson;
	std::vector<std::string> m_unreadableRows;   // non-object entries of "rows", verbatim

	bool        m_diskHadFile = false;
	std::string m_diskBytes;

	const Cell* cellFor(const UUID& row, const UUID& property) const;
	bool        put(const UUID& row, const UUID& property, std::string json, std::int64_t nowMs,
	                std::initializer_list<PropertyType> allowed);

	friend LoadResult loadDatabase(const std::filesystem::path&, const UUID&, Database&);
	friend bool saveDatabase(const std::filesystem::path&, Database&, std::string*);
};

// ── Page ─────────────────────────────────────────────────────────────────────

struct Block
{
	UUID        id;
	BlockType   type = BlockType::Paragraph;
	std::string typeOther;     // the file's word when type == Other
	std::string text;
	bool        checked = false;   // Todo
	std::string language;          // Code
	TaskLink    link;              // EngineLink: the object (kind Asset/Entity/Scene/Panel)
	UUID        database;          // DatabaseView: which database…
	UUID        view;              // …and which of its views ({} = its first)
	std::vector<Block> children;   // indented under this one
	std::string extraJson;
};

class Page
{
public:
	static constexpr int kVersion = 1;

	UUID         id;
	std::string  title;
	std::string  icon;
	UUID         parent;           // a Page or a Database id, {} = top level (the sidebar tree)
	std::string  createdBy;
	std::int64_t createdMs  = 0;
	std::int64_t modifiedMs = 0;
	std::vector<Block> blocks;

	// Appends to `blocks`, or under `parent` (nullptr = top level). nullptr when
	// `parent` is not a block of this page.
	Block*       addBlock(BlockType type, std::string text, std::int64_t nowMs, const UUID& parent = {});
	bool         removeBlock(const UUID& id, std::int64_t nowMs);
	Block*       findBlock(const UUID& id);
	const Block* findBlock(const UUID& id) const;
	// Moves a block to `index` among the children of `newParent` ({} = top level).
	// False for an unknown block/parent, or when the parent is the block itself
	// or one of its descendants.
	bool         moveBlock(const UUID& id, const UUID& newParent, std::size_t index, std::int64_t nowMs);

	using LoadResult = Database::LoadResult;
	LoadResult  fromJson(std::string_view text);
	std::string toJson() const;
	int         writeVersion() const { return m_fileVersion > kVersion ? m_fileVersion : kVersion; }

private:
	int         m_fileVersion = kVersion;
	std::string m_extraJson;
	std::vector<std::string> m_unreadableBlocks;   // non-object entries of "blocks", verbatim

	bool        m_diskHadFile = false;
	std::string m_diskBytes;

	friend LoadResult loadPage(const std::filesystem::path&, const UUID&, Page&);
	friend bool savePage(const std::filesystem::path&, Page&, std::string*);
};

// ── The files ────────────────────────────────────────────────────────────────
std::filesystem::path plannerDir(const std::filesystem::path& projectRoot);   // Config/Planner
std::filesystem::path databasePath(const std::filesystem::path& projectRoot, const UUID& id);
std::filesystem::path pagePath(const std::filesystem::path& projectRoot, const UUID& id);

// The ids of the documents on disk (file names that are 32 hex digits + ".json"),
// ordered by id so the answer does not depend on the directory's order.
std::vector<UUID> listDatabases(const std::filesystem::path& projectRoot);
std::vector<UUID> listPages(const std::filesystem::path& projectRoot);

// No file is an empty document under `id` and ok. A file that cannot be read or
// parsed gives !ok, an empty document and the reason. The document's id is `id`
// (the file name), whatever the file says.
Database::LoadResult loadDatabase(const std::filesystem::path& projectRoot, const UUID& id, Database& out);
Database::LoadResult loadPage(const std::filesystem::path& projectRoot, const UUID& id, Page& out);

// Temp + rename; refuses (false, `error` says why, file untouched) under the
// same two conditions as saveTaskBoard(): the file on disk does not parse, or it
// changed since this document loaded or saved it. A Page with no title, no block
// and no file writes nothing (a row opened but not yet written in); a Database
// is always written, it was created on purpose.
bool saveDatabase(const std::filesystem::path& projectRoot, Database& db, std::string* error = nullptr);
bool savePage(const std::filesystem::path& projectRoot, Page& page, std::string* error = nullptr);
// Deletes the file (a no-op for none). The caller drops its copy.
bool deleteDatabase(const std::filesystem::path& projectRoot, const UUID& id, std::string* error = nullptr);
bool deletePage(const std::filesystem::path& projectRoot, const UUID& id, std::string* error = nullptr);

} // namespace HE::Planner
