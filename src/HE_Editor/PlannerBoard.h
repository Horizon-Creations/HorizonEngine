#pragma once
#include "Planner.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// ── The Whiteboard: cards on a free surface ──────────────────────────────────
// The third kind of Planner document, next to the Database and the Page: an unbounded
// plane with cards (nodes) placed anywhere and arrows (edges) between them — the
// thinking-out-loud tool a design document is too tidy for. Not to be mistaken for a
// database's Board VIEW (lanes). Data and files only, no ImGui, like the rest of Planner.h
// (see docs/planner-plan-2026-10-22.md, "Whiteboard").
//
// ── What a node is ────────────────────────────────────────────────────────────
//   text     a card with text
//   image    a picture: a Texture asset of the project (`asset`, by UUID: no copy, a rename
//            keeps it) or a file the Whiteboard owns (`media`, a name under
//            Config/Planner/Media: for what is pasted or dropped in from outside the project)
//   object   an engine object (asset, scene, entity, panel): a chip that jumps there
//   row      a row of a database (`database` + `row`): a card that opens it
//   page     a page (`page`): a card that opens it
//   frame    a titled rectangle that groups what lies inside it
// A node's position and size are in board units (floating point, y down); the order in
// the file is the stacking order, the last one on top.
//
// ── The file ──────────────────────────────────────────────────────────────────
// <ProjectRoot>/Config/Planner/Boards/<32 hex>.json, one per board, under the Planner's
// bargain with git (Planner.h): UUIDs, lossless, unknown keys kept, never saved over an
// unreadable or changed-under-us file. The view (pan, zoom) is not part of it: that is the
// editor's, per person.
namespace HE::Planner
{

enum class NodeType : std::uint8_t { Text, Image, Object, Row, Page, Frame, Other };

const char* nodeTypeName(NodeType t);   // "text", "image", "object", "row", "page", "frame"
bool        nodeTypeFromName(std::string_view n, NodeType& out);

struct Rect2
{
	double x = 0, y = 0, w = 0, h = 0;
	bool contains(double px, double py) const { return px >= x && px <= x + w && py >= y && py <= y + h; }
	bool containsRect(const Rect2& o) const { return o.x >= x && o.y >= y && o.x + o.w <= x + w && o.y + o.h <= y + h; }
	bool intersects(const Rect2& o) const { return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h; }
};

struct Node
{
	UUID         id;
	NodeType     type = NodeType::Text;
	std::string  typeOther;      // the file's word when type == Other
	double       x = 0, y = 0, w = 240, h = 120;
	std::string  color;          // a word ("yellow"), "" = none
	std::string  text;           // Text: the content; Image, Frame: a caption / the title

	UUID         asset;          // Image: a Texture asset of the project
	std::string  media;          // Image: or a file under Config/Planner/Media (its name)
	TaskLink     link;           // Object
	UUID         database;       // Row
	UUID         row;            // Row
	UUID         page;           // Page
	std::string  extraJson;

	Rect2 rect() const { return { x, y, w, h }; }
};

struct Edge
{
	UUID         id;
	UUID         from, to;       // node ids
	std::string  label;
	bool         arrow = true;   // an arrowhead at `to`
	std::string  color;
	std::string  extraJson;
};

class Board
{
public:
	static constexpr int kVersion = 1;

	UUID         id;
	std::string  title;
	std::string  icon;
	std::string  createdBy;
	std::int64_t createdMs  = 0;
	std::int64_t modifiedMs = 0;
	std::vector<Node> nodes;     // file order = stacking order, the last one on top
	std::vector<Edge> edges;

	// ── Nodes ────────────────────────────────────────────────────────────────
	// The new node goes on top. The pointer is good until the board changes again.
	Node*       addNode(NodeType type, double x, double y, double w, double h, std::string text, std::int64_t nowMs);
	Node*       findNode(const UUID& id);
	const Node* findNode(const UUID& id) const;
	// Also drops every edge that ended at it.
	bool        removeNode(const UUID& id, std::int64_t nowMs);
	bool        moveNode(const UUID& id, double x, double y, std::int64_t nowMs);
	// Moves each of `ids` by the same amount (what dragging a selection does); the number moved.
	int         moveNodesBy(const std::vector<UUID>& ids, double dx, double dy, std::int64_t nowMs);
	// A size never goes below `kMinSize`.
	bool        resizeNode(const UUID& id, double x, double y, double w, double h, std::int64_t nowMs);
	bool        setNodeText(const UUID& id, std::string text, std::int64_t nowMs);
	bool        setNodeColor(const UUID& id, std::string color, std::int64_t nowMs);
	bool        bringToFront(const UUID& id, std::int64_t nowMs);
	bool        sendToBack(const UUID& id, std::int64_t nowMs);
	static constexpr double kMinSize = 24.0;

	// The topmost node under the point ({} = none). A frame only counts where no card
	// lies above it.
	UUID        hitTest(double x, double y) const;
	// Nodes whose rectangle meets `area` (a selection rectangle), bottom to top.
	std::vector<UUID> nodesIn(const Rect2& area) const;
	// Nodes that lie wholly inside the frame (what moves with it); not the frame itself.
	std::vector<UUID> nodesInside(const UUID& frame) const;
	// The rectangle round every node; false for an empty board.
	bool        bounds(Rect2& out) const;

	// ── Arranging ────────────────────────────────────────────────────────────
	// Line the cards up on the selection's outer edge / middle (a frame is moved by its own
	// rectangle; what lies inside it is the caller's to carry). The number moved.
	enum class Align { Left, CentreH, Right, Top, CentreV, Bottom };
	int         alignNodes(const std::vector<UUID>& ids, Align how, std::int64_t nowMs);
	// Equal gaps between three or more cards, the outermost two stay where they are.
	int         distributeNodes(const std::vector<UUID>& ids, bool horizontal, std::int64_t nowMs);
	// Moves each of `ids` so its top left lies on the grid; the number moved.
	int         snapNodes(const std::vector<UUID>& ids, double grid, std::int64_t nowMs);

	// ── Edges ────────────────────────────────────────────────────────────────
	// nullptr for a node that is not there, an arrow from a node to itself, or one that
	// exists already (same two nodes, same direction).
	Edge*       addEdge(const UUID& from, const UUID& to, std::string label, std::int64_t nowMs);
	Edge*       findEdge(const UUID& id);
	bool        removeEdge(const UUID& id, std::int64_t nowMs);
	bool        setEdgeLabel(const UUID& id, std::string label, std::int64_t nowMs);
	bool        setEdgeArrow(const UUID& id, bool arrow, std::int64_t nowMs);
	std::vector<const Edge*> edgesOf(const UUID& node) const;

	// ── The document ─────────────────────────────────────────────────────────
	Database::LoadResult fromJson(std::string_view text);
	std::string          toJson() const;
	int                  writeVersion() const { return m_fileVersion > kVersion ? m_fileVersion : kVersion; }

private:
	int                      m_fileVersion = kVersion;
	std::string              m_extraJson;
	std::vector<std::string> m_unreadableNodes;   // non-object entries of "nodes", verbatim
	std::vector<std::string> m_unreadableEdges;

	bool        m_diskHadFile = false;
	std::string m_diskBytes;

	friend Database::LoadResult loadBoard(const std::filesystem::path&, const UUID&, Board&);
	friend bool saveBoard(const std::filesystem::path&, Board&, std::string*);
};

// ── Copy, paste, duplicate ───────────────────────────────────────────────────
// Cards and the arrows that run between two of them, detached from any board (the editor's
// clipboard for cards). Pasting makes new ids, so the same copy can be pasted again and
// into another board.
struct BoardClip
{
	std::vector<Node> nodes;
	std::vector<Edge> edges;
	bool empty() const { return nodes.empty(); }
};

BoardClip         copyNodes(const Board& board, const std::vector<UUID>& ids);
// New cards offset by (dx, dy) on top; returns their ids in the clip's order.
std::vector<UUID> pasteNodes(Board& board, const BoardClip& clip, double dx, double dy, std::int64_t nowMs);

// ── Undo and redo ────────────────────────────────────────────────────────────
// The editor's, per open board, never written to a file. Each step is a snapshot of
// what the board held before a change (cards and arrows; the title and the file's
// bookkeeping are not undone). `undo` and `redo` hand back the state to restore and keep
// the one they replace for the way back; applying it is the caller's: assigning a snapshot
// is idempotent, so a save that is retried after a pull restores the same thing twice.
struct BoardSnapshot
{
	std::vector<Node> nodes;
	std::vector<Edge> edges;
};

BoardSnapshot snapshotOf(const Board& board);
void          restore(Board& board, const BoardSnapshot& snapshot, std::int64_t nowMs);

class BoardHistory
{
public:
	static constexpr std::size_t kLimit = 100;

	// `before` is the board as it was before a change that did happen. Forgets the redo
	// stack (a new change ends that future) and the oldest step beyond kLimit.
	void record(BoardSnapshot before);
	bool canUndo() const { return !m_undo.empty(); }
	bool canRedo() const { return !m_redo.empty(); }
	// The state to go back to, with `current` kept for redo; false when there is none.
	bool undo(const Board& current, BoardSnapshot& target);
	bool redo(const Board& current, BoardSnapshot& target);
	void clear() { m_undo.clear(); m_redo.clear(); }

private:
	std::vector<BoardSnapshot> m_undo, m_redo;
};

// ── The files ────────────────────────────────────────────────────────────────
std::filesystem::path boardPath(const std::filesystem::path& projectRoot, const UUID& id);
std::vector<UUID>     listBoards(const std::filesystem::path& projectRoot);
Database::LoadResult  loadBoard(const std::filesystem::path& projectRoot, const UUID& id, Board& out);
// Always written: a board is made on purpose. Refuses under the Planner's two conditions.
bool                  saveBoard(const std::filesystem::path& projectRoot, Board& board, std::string* error = nullptr);
bool                  deleteBoard(const std::filesystem::path& projectRoot, const UUID& id, std::string* error = nullptr);

// ── Media: pictures the Whiteboard owns ──────────────────────────────────────
// <ProjectRoot>/Config/Planner/Media/<name>. What is pasted or dropped in from outside
// the project is copied there (it has to travel with the project through source control,
// and a path on somebody's disk does not); a Texture asset of the project is referenced by
// UUID instead and never copied. A name is the content's hash plus the extension, so the
// same picture put in twice is one file.
inline constexpr std::uintmax_t kMaxMediaBytes = 64ull * 1024 * 1024;

std::filesystem::path mediaDir(const std::filesystem::path& projectRoot);
// The file a media name stands for; empty for a name that is not one this code makes
// (anything with a path in it, an unknown extension).
std::filesystem::path mediaPath(const std::filesystem::path& projectRoot, const std::string& name);
// Copies `source` in. Returns the name, or "" with `error` set: not a file, an extension
// that is not an image (png, jpg, jpeg, gif, bmp, webp, tga), too big, or not writable.
std::string           importMediaFile(const std::filesystem::path& projectRoot, const std::filesystem::path& source,
                                      std::string* error = nullptr);
// The same for bytes that never were a file (a picture from the clipboard); `extension`
// without the dot ("png").
std::string           importMediaBytes(const std::filesystem::path& projectRoot, const std::string& bytes,
                                       const std::string& extension, std::string* error = nullptr);
// The media names under the folder, sorted.
std::vector<std::string> listMedia(const std::filesystem::path& projectRoot);

} // namespace HE::Planner
