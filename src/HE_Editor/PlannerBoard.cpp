#include "PlannerBoard.h"
#include "PlannerFile.h"
#include "TaskBoardJson.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <unordered_set>

namespace HE::Planner
{

namespace TJ = HE::TaskJson;
using ojson = TJ::ojson;

namespace
{

const char* const kNodeNames[] = { "text", "image", "object", "row", "page", "frame" };

bool isNull(const UUID& id) { return id == UUID{}; }

void writeUuidIf(ojson& j, const char* key, const UUID& id)
{
	if (!isNull(id)) j[key] = uuidToHex(id);
}

void settleId(UUID& id, std::unordered_set<UUID>& seen)
{
	if (isNull(id) || !seen.insert(id).second)
	{
		id = UUID::generate();
		seen.insert(id);
	}
}

double readNumber(const ojson& j, const char* key, double fallback)
{
	const auto it = j.find(key);
	return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

Node readNode(const ojson& j, std::unordered_set<UUID>& seen)
{
	Node n;
	TJ::readUuid(j, "id", n.id);
	settleId(n.id, seen);
	std::string word;
	TJ::readString(j, "type", word);
	if (!nodeTypeFromName(word, n.type))
	{
		n.type      = NodeType::Other;
		n.typeOther = word;
	}
	n.x = readNumber(j, "x", 0);
	n.y = readNumber(j, "y", 0);
	n.w = std::max(Board::kMinSize, readNumber(j, "w", n.w));
	n.h = std::max(Board::kMinSize, readNumber(j, "h", n.h));
	TJ::readString(j, "color", n.color);
	TJ::readString(j, "text", n.text);
	// The keys that belong to a kind are known only for that kind; on another node, or on a
	// node of a kind this build does not know, they are somebody's own and stay as they were.
	switch (n.type)
	{
	case NodeType::Image:
		TJ::readUuid(j, "asset", n.asset);
		TJ::readString(j, "media", n.media);
		n.extraJson = TJ::extrasOf(j, { "id", "type", "x", "y", "w", "h", "color", "text", "asset", "media" });
		break;
	case NodeType::Object:
		if (const auto it = j.find("link"); it != j.end() && it->is_object()) n.link = TJ::readLink(*it);
		n.extraJson = TJ::extrasOf(j, { "id", "type", "x", "y", "w", "h", "color", "text", "link" });
		break;
	case NodeType::Row:
		TJ::readUuid(j, "database", n.database);
		TJ::readUuid(j, "row", n.row);
		n.extraJson = TJ::extrasOf(j, { "id", "type", "x", "y", "w", "h", "color", "text", "database", "row" });
		break;
	case NodeType::Page:
		TJ::readUuid(j, "page", n.page);
		n.extraJson = TJ::extrasOf(j, { "id", "type", "x", "y", "w", "h", "color", "text", "page" });
		break;
	default:
		n.extraJson = TJ::extrasOf(j, { "id", "type", "x", "y", "w", "h", "color", "text" });
		break;
	}
	return n;
}

ojson writeNode(const Node& n)
{
	ojson j = ojson::object();
	j["id"]   = uuidToHex(n.id);
	j["type"] = n.type == NodeType::Other ? n.typeOther : nodeTypeName(n.type);
	j["x"] = n.x;
	j["y"] = n.y;
	j["w"] = n.w;
	j["h"] = n.h;
	if (!n.color.empty()) j["color"] = n.color;
	if (!n.text.empty())  j["text"] = n.text;
	switch (n.type)
	{
	case NodeType::Image:
		writeUuidIf(j, "asset", n.asset);
		if (!n.media.empty()) j["media"] = n.media;
		break;
	case NodeType::Object: j["link"] = TJ::writeLink(n.link); break;
	case NodeType::Row:
		writeUuidIf(j, "database", n.database);
		writeUuidIf(j, "row", n.row);
		break;
	case NodeType::Page: writeUuidIf(j, "page", n.page); break;
	default: break;
	}
	TJ::appendExtras(j, n.extraJson);
	return j;
}

Edge readEdge(const ojson& j, std::unordered_set<UUID>& seen)
{
	Edge e;
	TJ::readUuid(j, "id", e.id);
	settleId(e.id, seen);
	TJ::readUuid(j, "from", e.from);
	TJ::readUuid(j, "to", e.to);
	TJ::readString(j, "label", e.label);
	TJ::readString(j, "color", e.color);
	if (const auto it = j.find("arrow"); it != j.end() && it->is_boolean()) e.arrow = it->get<bool>();
	e.extraJson = TJ::extrasOf(j, { "id", "from", "to", "label", "color", "arrow" });
	return e;
}

ojson writeEdge(const Edge& e)
{
	ojson j = ojson::object();
	j["id"]   = uuidToHex(e.id);
	j["from"] = uuidToHex(e.from);
	j["to"]   = uuidToHex(e.to);
	if (!e.label.empty()) j["label"] = e.label;
	if (!e.arrow) j["arrow"] = false;
	if (!e.color.empty()) j["color"] = e.color;
	TJ::appendExtras(j, e.extraJson);
	return j;
}

bool mediaExtensionOk(std::string ext)
{
	for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	static const char* const ok[] = { "png", "jpg", "jpeg", "gif", "bmp", "webp", "tga" };
	return std::any_of(std::begin(ok), std::end(ok), [&](const char* e) { return ext == e; });
}

std::string lowered(std::string s)
{
	for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

std::uint64_t fnv1a(const std::string& bytes)
{
	std::uint64_t h = 1469598103934665603ULL;
	for (const char c : bytes)
	{
		h ^= static_cast<unsigned char>(c);
		h *= 1099511628211ULL;
	}
	return h;
}

} // namespace

const char* nodeTypeName(NodeType t)
{
	const auto i = static_cast<std::size_t>(t);
	return i < std::size(kNodeNames) ? kNodeNames[i] : "";
}

bool nodeTypeFromName(std::string_view n, NodeType& out)
{
	for (std::size_t i = 0; i < std::size(kNodeNames); ++i)
		if (n == kNodeNames[i]) { out = static_cast<NodeType>(i); return true; }
	return false;
}

// ── Nodes ────────────────────────────────────────────────────────────────────

Node* Board::addNode(NodeType type, double x, double y, double w, double h, std::string text, std::int64_t nowMs)
{
	Node n;
	n.id   = UUID::generate();
	n.type = type == NodeType::Other ? NodeType::Text : type;
	n.x = x;
	n.y = y;
	n.w = std::max(kMinSize, w);
	n.h = std::max(kMinSize, h);
	n.text = std::move(text);
	nodes.push_back(std::move(n));
	modifiedMs = nowMs;
	return &nodes.back();
}

Node* Board::findNode(const UUID& nid)
{
	for (Node& n : nodes)
		if (n.id == nid) return &n;
	return nullptr;
}

const Node* Board::findNode(const UUID& nid) const { return const_cast<Board*>(this)->findNode(nid); }

bool Board::removeNode(const UUID& nid, std::int64_t nowMs)
{
	const auto it = std::find_if(nodes.begin(), nodes.end(), [&](const Node& n) { return n.id == nid; });
	if (it == nodes.end()) return false;
	nodes.erase(it);
	edges.erase(std::remove_if(edges.begin(), edges.end(),
	                           [&](const Edge& e) { return e.from == nid || e.to == nid; }), edges.end());
	modifiedMs = nowMs;
	return true;
}

bool Board::moveNode(const UUID& nid, double x, double y, std::int64_t nowMs)
{
	Node* n = findNode(nid);
	if (!n) return false;
	n->x = x;
	n->y = y;
	modifiedMs = nowMs;
	return true;
}

int Board::moveNodesBy(const std::vector<UUID>& ids, double dx, double dy, std::int64_t nowMs)
{
	int moved = 0;
	for (Node& n : nodes)
		if (std::find(ids.begin(), ids.end(), n.id) != ids.end())
		{
			n.x += dx;
			n.y += dy;
			++moved;
		}
	if (moved > 0) modifiedMs = nowMs;
	return moved;
}

bool Board::resizeNode(const UUID& nid, double x, double y, double w, double h, std::int64_t nowMs)
{
	Node* n = findNode(nid);
	if (!n) return false;
	n->x = x;
	n->y = y;
	n->w = std::max(kMinSize, w);
	n->h = std::max(kMinSize, h);
	modifiedMs = nowMs;
	return true;
}

bool Board::setNodeText(const UUID& nid, std::string text, std::int64_t nowMs)
{
	Node* n = findNode(nid);
	if (!n) return false;
	n->text = std::move(text);
	modifiedMs = nowMs;
	return true;
}

bool Board::setNodeColor(const UUID& nid, std::string color, std::int64_t nowMs)
{
	Node* n = findNode(nid);
	if (!n) return false;
	n->color = std::move(color);
	modifiedMs = nowMs;
	return true;
}

bool Board::bringToFront(const UUID& nid, std::int64_t nowMs)
{
	const auto it = std::find_if(nodes.begin(), nodes.end(), [&](const Node& n) { return n.id == nid; });
	if (it == nodes.end()) return false;
	std::rotate(it, it + 1, nodes.end());
	modifiedMs = nowMs;
	return true;
}

bool Board::sendToBack(const UUID& nid, std::int64_t nowMs)
{
	const auto it = std::find_if(nodes.begin(), nodes.end(), [&](const Node& n) { return n.id == nid; });
	if (it == nodes.end()) return false;
	std::rotate(nodes.begin(), it, it + 1);
	modifiedMs = nowMs;
	return true;
}

UUID Board::hitTest(double x, double y) const
{
	// Cards first, topmost first: a card inside a frame is what you meant, not the frame.
	for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
		if (it->type != NodeType::Frame && it->rect().contains(x, y)) return it->id;
	for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
		if (it->type == NodeType::Frame && it->rect().contains(x, y)) return it->id;
	return UUID{};
}

std::vector<UUID> Board::nodesIn(const Rect2& area) const
{
	std::vector<UUID> out;
	for (const Node& n : nodes)
		if (n.rect().intersects(area)) out.push_back(n.id);
	return out;
}

std::vector<UUID> Board::nodesInside(const UUID& frame) const
{
	std::vector<UUID> out;
	const Node* f = findNode(frame);
	if (!f) return out;
	for (const Node& n : nodes)
		if (!(n.id == frame) && f->rect().containsRect(n.rect())) out.push_back(n.id);
	return out;
}

bool Board::bounds(Rect2& out) const
{
	if (nodes.empty()) return false;
	double x0 = nodes[0].x, y0 = nodes[0].y, x1 = nodes[0].x + nodes[0].w, y1 = nodes[0].y + nodes[0].h;
	for (const Node& n : nodes)
	{
		x0 = std::min(x0, n.x);
		y0 = std::min(y0, n.y);
		x1 = std::max(x1, n.x + n.w);
		y1 = std::max(y1, n.y + n.h);
	}
	out = { x0, y0, x1 - x0, y1 - y0 };
	return true;
}

// ── Edges ────────────────────────────────────────────────────────────────────

int Board::alignNodes(const std::vector<UUID>& ids, Align how, std::int64_t nowMs)
{
	double lo_x = 1e300, hi_x = -1e300, lo_y = 1e300, hi_y = -1e300;
	std::vector<Node*> picked;
	for (const UUID& id : ids)
		if (Node* n = findNode(id)) picked.push_back(n);
	if (picked.size() < 2) return 0;
	for (const Node* n : picked)
	{
		lo_x = std::min(lo_x, n->x); hi_x = std::max(hi_x, n->x + n->w);
		lo_y = std::min(lo_y, n->y); hi_y = std::max(hi_y, n->y + n->h);
	}
	int moved = 0;
	for (Node* n : picked)
	{
		double nx = n->x, ny = n->y;
		switch (how)
		{
		case Align::Left:    nx = lo_x; break;
		case Align::Right:   nx = hi_x - n->w; break;
		case Align::CentreH: nx = (lo_x + hi_x) * 0.5 - n->w * 0.5; break;
		case Align::Top:     ny = lo_y; break;
		case Align::Bottom:  ny = hi_y - n->h; break;
		case Align::CentreV: ny = (lo_y + hi_y) * 0.5 - n->h * 0.5; break;
		}
		if (nx != n->x || ny != n->y) { n->x = nx; n->y = ny; ++moved; }
	}
	if (moved) modifiedMs = nowMs;
	return moved;
}

int Board::distributeNodes(const std::vector<UUID>& ids, bool horizontal, std::int64_t nowMs)
{
	std::vector<Node*> picked;
	for (const UUID& id : ids)
		if (Node* n = findNode(id)) picked.push_back(n);
	if (picked.size() < 3) return 0;
	auto lo  = [&](const Node* n) { return horizontal ? n->x : n->y; };
	auto len = [&](const Node* n) { return horizontal ? n->w : n->h; };
	std::stable_sort(picked.begin(), picked.end(), [&](const Node* a, const Node* b) { return lo(a) < lo(b); });
	double span = lo(picked.back()) + len(picked.back()) - lo(picked.front()), used = 0;
	for (const Node* n : picked) used += len(n);
	const double gap = (span - used) / static_cast<double>(picked.size() - 1);
	double at = lo(picked.front());
	int moved = 0;
	for (Node* n : picked)
	{
		if (lo(n) != at) { (horizontal ? n->x : n->y) = at; ++moved; }
		at += len(n) + gap;
	}
	if (moved) modifiedMs = nowMs;
	return moved;
}

int Board::snapNodes(const std::vector<UUID>& ids, double grid, std::int64_t nowMs)
{
	if (grid <= 0) return 0;
	int moved = 0;
	for (const UUID& id : ids)
		if (Node* n = findNode(id))
		{
			const double nx = std::round(n->x / grid) * grid, ny = std::round(n->y / grid) * grid;
			if (nx != n->x || ny != n->y) { n->x = nx; n->y = ny; ++moved; }
		}
	if (moved) modifiedMs = nowMs;
	return moved;
}

BoardClip copyNodes(const Board& board, const std::vector<UUID>& ids)
{
	BoardClip clip;
	for (const Node& n : board.nodes)   // board order keeps the stacking
		if (std::find(ids.begin(), ids.end(), n.id) != ids.end()) clip.nodes.push_back(n);
	for (const Edge& e : board.edges)
	{
		const auto in = [&](const UUID& id) {
			return std::any_of(clip.nodes.begin(), clip.nodes.end(), [&](const Node& n) { return n.id == id; });
		};
		if (in(e.from) && in(e.to)) clip.edges.push_back(e);
	}
	return clip;
}

std::vector<UUID> pasteNodes(Board& board, const BoardClip& clip, double dx, double dy, std::int64_t nowMs)
{
	std::vector<UUID> made;
	std::vector<std::pair<UUID, UUID>> remap;
	for (const Node& src : clip.nodes)
	{
		Node n = src;
		n.id = UUID::generate();
		n.x += dx;
		n.y += dy;
		remap.emplace_back(src.id, n.id);
		made.push_back(n.id);
		board.nodes.push_back(std::move(n));
	}
	const auto mapped = [&](const UUID& id) {
		for (const auto& r : remap) if (r.first == id) return r.second;
		return UUID{};
	};
	for (const Edge& src : clip.edges)
	{
		Edge e = src;
		e.id = UUID::generate();
		e.from = mapped(src.from);
		e.to = mapped(src.to);
		board.edges.push_back(std::move(e));
	}
	if (!made.empty()) board.modifiedMs = nowMs;
	return made;
}

Edge* Board::addEdge(const UUID& from, const UUID& to, std::string label, std::int64_t nowMs)
{
	if (from == to || !findNode(from) || !findNode(to)) return nullptr;
	for (const Edge& e : edges)
		if (e.from == from && e.to == to) return nullptr;
	Edge e;
	e.id    = UUID::generate();
	e.from  = from;
	e.to    = to;
	e.label = std::move(label);
	edges.push_back(std::move(e));
	modifiedMs = nowMs;
	return &edges.back();
}

Edge* Board::findEdge(const UUID& eid)
{
	for (Edge& e : edges)
		if (e.id == eid) return &e;
	return nullptr;
}

bool Board::removeEdge(const UUID& eid, std::int64_t nowMs)
{
	const auto it = std::find_if(edges.begin(), edges.end(), [&](const Edge& e) { return e.id == eid; });
	if (it == edges.end()) return false;
	edges.erase(it);
	modifiedMs = nowMs;
	return true;
}

bool Board::setEdgeLabel(const UUID& eid, std::string label, std::int64_t nowMs)
{
	Edge* e = findEdge(eid);
	if (!e) return false;
	e->label = std::move(label);
	modifiedMs = nowMs;
	return true;
}

bool Board::setEdgeArrow(const UUID& eid, bool arrow, std::int64_t nowMs)
{
	Edge* e = findEdge(eid);
	if (!e) return false;
	e->arrow = arrow;
	modifiedMs = nowMs;
	return true;
}

std::vector<const Edge*> Board::edgesOf(const UUID& node) const
{
	std::vector<const Edge*> out;
	for (const Edge& e : edges)
		if (e.from == node || e.to == node) out.push_back(&e);
	return out;
}

// ── The document ─────────────────────────────────────────────────────────────

Database::LoadResult Board::fromJson(std::string_view text)
{
	*this = Board{};
	Database::LoadResult r;
	ojson j;
	int version = 1;
	if (!detail::openDocument(text, nullptr, j, version, r)) return r;
	for (const char* key : { "nodes", "edges" })
		if (const auto it = j.find(key); it != j.end() && !it->is_array())
		{
			r.error = std::string("\"") + key + "\" is not a list";
			return r;
		}

	TJ::readUuid(j, "id", id);
	TJ::readString(j, "title", title);
	TJ::readString(j, "icon", icon);
	TJ::readString(j, "createdBy", createdBy);
	TJ::readTime(j, "created", createdMs);
	TJ::readTime(j, "modified", modifiedMs);
	std::unordered_set<UUID> seen;
	if (const auto it = j.find("nodes"); it != j.end())
		for (const ojson& e : *it)
		{
			if (!e.is_object()) { m_unreadableNodes.push_back(e.dump()); continue; }
			nodes.push_back(readNode(e, seen));
		}
	seen.clear();
	if (const auto it = j.find("edges"); it != j.end())
		for (const ojson& e : *it)
		{
			if (!e.is_object()) { m_unreadableEdges.push_back(e.dump()); continue; }
			edges.push_back(readEdge(e, seen));
		}
	m_fileVersion = version;
	m_extraJson   = TJ::extrasOf(j, { "version", "id", "title", "icon", "createdBy", "created", "modified", "nodes", "edges" });
	r.ok      = true;
	r.version = version;
	return r;
}

std::string Board::toJson() const
{
	ojson j = ojson::object();
	j["version"] = writeVersion();
	j["id"]      = uuidToHex(id);
	j["title"]   = title;
	if (!icon.empty()) j["icon"] = icon;
	j["createdBy"] = createdBy;
	j["created"]   = taskTimeToIso(createdMs);
	j["modified"]  = taskTimeToIso(modifiedMs);
	ojson ns = ojson::array();
	for (const Node& n : nodes) ns.push_back(writeNode(n));
	for (const std::string& raw : m_unreadableNodes)
	{
		ojson e = ojson::parse(raw, nullptr, /*allow_exceptions=*/false);
		if (!e.is_discarded()) ns.push_back(std::move(e));
	}
	j["nodes"] = std::move(ns);
	ojson es = ojson::array();
	for (const Edge& e : edges) es.push_back(writeEdge(e));
	for (const std::string& raw : m_unreadableEdges)
	{
		ojson e = ojson::parse(raw, nullptr, /*allow_exceptions=*/false);
		if (!e.is_discarded()) es.push_back(std::move(e));
	}
	j["edges"] = std::move(es);
	TJ::appendExtras(j, m_extraJson);
	return TJ::dumpDocument(j);
}

// ── Undo and redo ────────────────────────────────────────────────────────────

BoardSnapshot snapshotOf(const Board& board) { return { board.nodes, board.edges }; }

void restore(Board& board, const BoardSnapshot& snapshot, std::int64_t nowMs)
{
	board.nodes = snapshot.nodes;
	board.edges = snapshot.edges;
	board.modifiedMs = nowMs;
}

void BoardHistory::record(BoardSnapshot before)
{
	m_undo.push_back(std::move(before));
	if (m_undo.size() > kLimit) m_undo.erase(m_undo.begin());
	m_redo.clear();
}

bool BoardHistory::undo(const Board& current, BoardSnapshot& target)
{
	if (m_undo.empty()) return false;
	target = std::move(m_undo.back());
	m_undo.pop_back();
	m_redo.push_back(snapshotOf(current));
	return true;
}

bool BoardHistory::redo(const Board& current, BoardSnapshot& target)
{
	if (m_redo.empty()) return false;
	target = std::move(m_redo.back());
	m_redo.pop_back();
	m_undo.push_back(snapshotOf(current));
	return true;
}

// ── The files ────────────────────────────────────────────────────────────────

std::filesystem::path boardPath(const std::filesystem::path& projectRoot, const UUID& id)
{
	return plannerDir(projectRoot) / "Boards" / (uuidToHex(id) + ".json");
}

std::vector<UUID> listBoards(const std::filesystem::path& projectRoot)
{
	return detail::listDir(plannerDir(projectRoot) / "Boards");
}

Database::LoadResult loadBoard(const std::filesystem::path& projectRoot, const UUID& id, Board& out)
{
	const auto reset = [&] { out = Board{}; out.id = id; };
	Database::LoadResult r = detail::loadFile(
		boardPath(projectRoot, id), "board",
		[&](std::string_view t) { return out.fromJson(t); }, reset,
		out.m_diskHadFile, out.m_diskBytes);
	out.id = id;   // the file name is the identity
	return r;
}

bool saveBoard(const std::filesystem::path& projectRoot, Board& board, std::string* error)
{
	if (isNull(board.id))
	{
		if (error) *error = "the board has no id";
		return false;
	}
	return detail::saveFile(boardPath(projectRoot, board.id), "board", board.toJson(), /*nothingToSay=*/false,
	                        [](std::string_view t) { Board probe; return probe.fromJson(t); },
	                        board.m_diskHadFile, board.m_diskBytes, error);
}

bool deleteBoard(const std::filesystem::path& projectRoot, const UUID& id, std::string* error)
{
	return detail::deleteFile(boardPath(projectRoot, id), error);
}

// ── Media ────────────────────────────────────────────────────────────────────

std::filesystem::path mediaDir(const std::filesystem::path& projectRoot) { return plannerDir(projectRoot) / "Media"; }

std::filesystem::path mediaPath(const std::filesystem::path& projectRoot, const std::string& name)
{
	if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
	    name == "." || name == ".." || name.front() == '.')
		return {};
	const std::filesystem::path p(name);
	if (!p.has_extension() || !mediaExtensionOk(p.extension().string().substr(1))) return {};
	return mediaDir(projectRoot) / name;
}

namespace
{

std::string storeMedia(const std::filesystem::path& projectRoot, const std::string& bytes, const std::string& ext,
                       std::string* error)
{
	auto fail = [&](std::string why) -> std::string {
		if (error) *error = std::move(why);
		return {};
	};
	if (!mediaExtensionOk(ext)) return fail("'" + ext + "' is not a picture format (png, jpg, jpeg, gif, bmp, webp, tga)");
	if (bytes.empty()) return fail("the picture is empty");
	if (bytes.size() > kMaxMediaBytes) return fail("the picture is larger than 64 MB");

	char hash[20];
	std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(fnv1a(bytes)));
	const std::string name = std::string(hash) + "." + lowered(ext);
	const std::filesystem::path path = mediaPath(projectRoot, name);
	std::error_code ec;
	if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) == bytes.size()) return name;   // the same picture again

	std::filesystem::create_directories(path.parent_path(), ec);
	const std::filesystem::path tmp = path.string() + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out.is_open()) return fail("cannot create " + tmp.string());
		out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		out.flush();
		if (!out.good())
		{
			out.close();
			std::filesystem::remove(tmp, ec);
			return fail("writing " + tmp.string() + " failed");
		}
	}
	std::filesystem::rename(tmp, path, ec);
	if (ec)
	{
		std::error_code ec2;
		std::filesystem::remove(tmp, ec2);
		return fail("could not place " + path.string() + " (" + ec.message() + ")");
	}
	return name;
}

} // namespace

std::string importMediaFile(const std::filesystem::path& projectRoot, const std::filesystem::path& source, std::string* error)
{
	std::error_code ec;
	if (!std::filesystem::is_regular_file(source, ec))
	{
		if (error) *error = "'" + source.string() + "' is not a file";
		return {};
	}
	const std::uintmax_t size = std::filesystem::file_size(source, ec);
	if (!ec && size > kMaxMediaBytes)
	{
		if (error) *error = "the picture is larger than 64 MB";
		return {};
	}
	const std::string ext = source.extension().string();
	std::ifstream in(source, std::ios::binary);
	if (!in.is_open())
	{
		if (error) *error = "cannot read " + source.string();
		return {};
	}
	const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return storeMedia(projectRoot, bytes, ext.empty() ? std::string{} : ext.substr(1), error);
}

std::string importMediaBytes(const std::filesystem::path& projectRoot, const std::string& bytes, const std::string& extension,
                             std::string* error)
{
	return storeMedia(projectRoot, bytes, extension, error);
}

std::vector<std::string> listMedia(const std::filesystem::path& projectRoot)
{
	std::vector<std::string> out;
	std::error_code ec;
	for (std::filesystem::directory_iterator it(mediaDir(projectRoot), ec), end; !ec && it != end; it.increment(ec))
	{
		if (!it->is_regular_file(ec)) continue;
		const std::string name = it->path().filename().string();
		if (!mediaPath(projectRoot, name).empty()) out.push_back(name);
	}
	std::sort(out.begin(), out.end());
	return out;
}

} // namespace HE::Planner
