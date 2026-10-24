#include "PlannerBoardView.h"

#include "AssetThumbnailCache.h"   // a Texture asset's full-size picture
#include "EditorApplication.h"   // AppContext
#include "EditorAssetTypeCache.h"
#include "EditorHelp.h"
#include "EditorInput.h"       // the trackpad pinch
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "NotificationStore.h"
#include "PlannerFontText.h"
#include "PlannerIcons.h"
#include "PlannerPanel.h"
#include "PlannerWorkspace.h"
#include "TaskBoardPanel.h"      // the links a Content Browser / Outliner drop stands for
#include "vendor/stb_image.h"
#include "../../tests/ImGuiSoftwareRaster.h"   // the CPU rasteriser the editor already carries (EditorUiShot)

#include <Application/AppIcon.h>      // hePngWrite
#include <Renderer/IRenderer.h>   // CreateImGuiTexture

#include <imgui.h>
#include <imgui_internal.h>   // a draw list of our own for the export
#include <misc/cpp/imgui_stdlib.h>   // InputText over std::string

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>

using namespace HE;
using namespace HE::Planner;
namespace Theme = HE::Ed::Theme;

namespace PlannerBoardView
{

namespace
{

// Every control below is drawn inside the Planner tab's render(), which pushes the help
// scope
//     Help::Scope helpScope("Planner")
// further up than this file. The line above is for scripts/editor_help_audit.py, which has
// to know the scope before it meets the first label.

const Services* S = nullptr;   // the panel's services for the frame being drawn

// ── State: per board where it should survive switching boards, else one for the canvas ──
std::unordered_map<UUID, ViewState>    s_views;
std::unordered_map<UUID, BoardHistory> s_history;
std::vector<UUID> s_sel;
UUID              s_selEdge;
UUID              s_boardOfSelection;   // the board s_sel and s_selEdge belong to
Stats             s_stats;

enum class Drag { None, Pan, Move, Marquee, Resize, Connect };
Drag        s_drag = Drag::None;
ImVec2      s_dragStart;                 // the mouse, in screen space, when the drag began
std::vector<UUID> s_dragNodes;           // Move: what moves
bool        s_dragMoved = false;
UUID        s_dragHit;                   // the card the press landed on
bool        s_dragShift = false;
UUID        s_resizeNode;                // Resize
int         s_resizeCorner = 0;          // 0 nw, 1 ne, 2 se, 3 sw
Rect2       s_resizeFrom;
Rect2       s_resizeTo;
UUID        s_connectFrom;               // Connect

// Pictures: a media file's texture is made the first time a card shows it. A failed try is kept
// (null handle) so a missing file is not read again every frame.
struct MediaTex { ImTextureID id = 0; float w = 0, h = 0; bool tried = false; IRenderer* renderer = nullptr; };
std::unordered_map<std::string, MediaTex> s_mediaTex;

// Files let go over the window, waiting for the next frame's canvas to turn them into cards.
struct DroppedFile { std::string path; ImVec2 at; };
std::vector<DroppedFile> s_dropped;
ImVec2 s_canvasMin(0, 0), s_canvasMax(0, 0);
int    s_canvasFrame = -100;        // the frame the canvas was last drawn in

bool        s_minimap = true;            // the overview in the canvas corner
bool        s_mapDrag = false;           // a press that began inside it
bool        s_snap = false;              // moves and resizes land on the grid
constexpr double kGrid = 20.0;
BoardClip   s_clip;                      // Copy: cards (and the arrows between them) waiting to be pasted
int         s_pasteCount = 0;            // each paste of one copy lands a little further on
constexpr const char* kClipMarker = "HorizonEngine/board-cards";   // what Copy leaves on the system clipboard

UUID        s_editNode;                  // the card whose text is being typed
std::string s_editBuf;
bool        s_editFocus = false;

UUID        s_titleFor;
std::string s_titleBuf;
UUID        s_labelFor;
std::string s_labelBuf;

constexpr double kMinZoom = 0.1, kMaxZoom = 4.0;
constexpr float  kHandle = 8.0f, kDotRadius = 5.0f;

// A card that stands for something in the editor rather than holding its own text.
inline bool isLinkNode(NodeType t) { return t == NodeType::Row || t == NodeType::Page || t == NodeType::Object; }

// ── Colours ──────────────────────────────────────────────────────────────────
struct Swatch { const char* word; ImVec4 colour; };
const Swatch kSwatches[] = {
	{ "yellow", { 0.98f, 0.88f, 0.45f, 1 } }, { "green", { 0.55f, 0.85f, 0.55f, 1 } },
	{ "blue",   { 0.55f, 0.75f, 0.98f, 1 } }, { "pink",  { 0.97f, 0.65f, 0.80f, 1 } },
	{ "purple", { 0.75f, 0.62f, 0.95f, 1 } }, { "orange", { 0.98f, 0.70f, 0.40f, 1 } },
	{ "gray",   { 0.70f, 0.70f, 0.70f, 1 } },
};

bool colourOf(const std::string& word, ImVec4& out)
{
	for (const Swatch& s : kSwatches)
		if (word == s.word) { out = s.colour; return true; }
	return false;
}

// ── The view's arithmetic ────────────────────────────────────────────────────
struct Xf
{
	ImVec2 origin;
	ViewState v;
	ImVec2 toScreen(double x, double y) const
	{
		return ImVec2(origin.x + static_cast<float>((x - v.x) * v.zoom), origin.y + static_cast<float>((y - v.y) * v.zoom));
	}
	void toWorld(ImVec2 p, double& x, double& y) const
	{
		x = v.x + (p.x - origin.x) / v.zoom;
		y = v.y + (p.y - origin.y) / v.zoom;
	}
	void rectToScreen(const Rect2& r, ImVec2& a, ImVec2& b) const
	{
		a = toScreen(r.x, r.y);
		b = toScreen(r.x + r.w, r.y + r.h);
	}
};

bool has(const std::vector<UUID>& v, const UUID& id) { return std::find(v.begin(), v.end(), id) != v.end(); }

// Where the line from a card's centre toward (tx, ty) leaves the card.
void anchorOf(const Node& n, double tx, double ty, double& ox, double& oy)
{
	const double cx = n.x + n.w * 0.5, cy = n.y + n.h * 0.5;
	const double dx = tx - cx, dy = ty - cy;
	if (std::abs(dx) < 1e-9 && std::abs(dy) < 1e-9) { ox = cx; oy = cy; return; }
	const double sx = std::abs(dx) < 1e-9 ? DBL_MAX : (n.w * 0.5) / std::abs(dx);
	const double sy = std::abs(dy) < 1e-9 ? DBL_MAX : (n.h * 0.5) / std::abs(dy);
	const double t = std::min(sx, sy);
	ox = cx + dx * t;
	oy = cy + dy * t;
}

float distanceToSegment(ImVec2 p, ImVec2 a, ImVec2 b)
{
	const float dx = b.x - a.x, dy = b.y - a.y;
	const float len2 = dx * dx + dy * dy;
	float t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0f;
	t = std::clamp(t, 0.0f, 1.0f);
	const float qx = a.x + dx * t - p.x, qy = a.y + dy * t - p.y;
	return std::sqrt(qx * qx + qy * qy);
}

// ── Edits ────────────────────────────────────────────────────────────────────
// One change to a board: applied after the frame, saved by the workspace, and put on the
// undo stack as the state before it. `fn` may run twice (a save refused by a pull reads it
// and applies the change again), so it addresses nodes by id and the snapshot is the one of
// the run that was saved.
void change(const UUID& boardId, std::function<bool(Board&)> fn, std::function<void()> afterwards = {})
{
	Workspace* ws = S->ws;
	S->post([=] {
		BoardSnapshot before;
		bool changed = false;
		ws->editBoard(boardId, [&](Board& b) {
			before  = snapshotOf(b);
			changed = fn(b);
			return changed;
		});
		if (changed)
		{
			s_history[boardId].record(std::move(before));
			if (afterwards) afterwards();
		}
	});
}

void createNode(const UUID& boardId, NodeType type, double centreX, double centreY)
{
	const bool frame = type == NodeType::Frame;
	const double w = frame ? 480 : 240, h = frame ? 320 : 120;
	// The id is only known once the board has made the node.
	auto made = std::make_shared<UUID>();
	change(boardId, [=](Board& b) {
		Node* n = b.addNode(type, centreX - w * 0.5, centreY - h * 0.5, w, h, "", taskNowMs());
		*made = n->id;   // before sendToBack: that moves the cards in the vector
		if (frame) b.sendToBack(*made, taskNowMs());
		return true;
	}, [=] {
		s_sel = { *made };
		s_selEdge = UUID{};
		s_editNode = *made;
		s_editBuf.clear();
		s_editFocus = true;
	});
}

void deleteSelection(const UUID& boardId)
{
	const std::vector<UUID> nodes = s_sel;
	const UUID edge = s_selEdge;
	if (nodes.empty() && edge == UUID{}) return;
	change(boardId, [=](Board& b) {
		bool any = false;
		for (const UUID& id : nodes) any |= b.removeNode(id, taskNowMs());
		if (!(edge == UUID{})) any |= b.removeEdge(edge, taskNowMs());
		return any;
	}, [] { s_sel.clear(); s_selEdge = UUID{}; });
}

void undoOrRedo(const UUID& boardId, bool undo)
{
	Workspace* ws = S->ws;
	S->post([=] {
		Board* b = ws->findBoard(boardId);
		if (!b) return;
		BoardSnapshot target;
		if (!(undo ? s_history[boardId].undo(*b, target) : s_history[boardId].redo(*b, target))) return;
		ws->editBoard(boardId, [&](Board& bb) { restore(bb, target, taskNowMs()); return true; });
	});
}

void setColour(const UUID& boardId, const std::string& word)
{
	const std::vector<UUID> nodes = s_sel;
	if (nodes.empty()) return;
	change(boardId, [=](Board& b) {
		bool any = false;
		for (const UUID& id : nodes)
			if (const Node* n = b.findNode(id); n && n->color != word) any |= b.setNodeColor(id, word, taskNowMs());
		return any;
	});
}

using namespace PlannerIcons;

// What the floating bars look like: a rounded plate over the canvas.
bool beginPlate(const char* id, ImVec2 at, ImVec2 size)
{
	ImGui::SetCursorScreenPos(at);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::alpha(Theme::warm(0.11f), 0.97f));
	ImGui::PushStyleColor(ImGuiCol_Border, Theme::warm(0.30f));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5, 5));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 4));
	const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
	                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	return open;
}

void endPlate()
{
	ImGui::EndChild();
	ImGui::PopStyleVar(4);
	ImGui::PopStyleColor(2);
}

// ── Pictures ─────────────────────────────────────────────────────────────────
struct Pic { ImTextureID tex = 0; float w = 0, h = 0; };

// While exporting: media pictures go to the CPU rasteriser instead of the GPU, assets are not drawn.
std::unordered_map<std::string, MediaTex>* s_exportTex = nullptr;

Pic pictureOf(const Node& n)
{
	Pic p;
	if (s_exportTex)
	{
		if (n.media.empty() || !(n.asset == UUID{}) || !S) return p;
		MediaTex& m = (*s_exportTex)[n.media];
		if (!m.tried)
		{
			m.tried = true;
			const std::filesystem::path file = mediaPath(S->root, n.media);
			int w = 0, h = 0, ch = 0;
			if (!file.empty())
				if (unsigned char* px = stbi_load(file.string().c_str(), &w, &h, &ch, 4))
				{
					m.id = he_ui::registerTexture(px, w, h);
					m.w = static_cast<float>(w);
					m.h = static_cast<float>(h);
					stbi_image_free(px);
				}
		}
		return { m.id, m.w, m.h };
	}
	if (!(n.asset == UUID{}))
	{
		if (!S || !S->ctx || !S->ctx->contentManager) return p;
		const std::string rel = S->ctx->contentManager->pathForId(n.asset);
		if (rel.empty()) return p;
		uint32_t w = 0, h = 0;
		if (void* t = AssetThumbnailCache::image(S->ctx->contentManager->resolveAbsolutePath(rel), &w, &h))
			p = { static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(t)), static_cast<float>(w), static_cast<float>(h) };
		return p;
	}
	if (n.media.empty() || !S || !S->ctx || !S->ctx->renderer) return p;
	MediaTex& m = s_mediaTex[n.media];
	if (!m.tried)
	{
		m.tried = true;
		const std::filesystem::path file = mediaPath(S->root, n.media);
		int w = 0, h = 0, ch = 0;
		if (!file.empty())
			if (unsigned char* px = stbi_load(file.string().c_str(), &w, &h, &ch, 4))
			{
				if (void* handle = S->ctx->renderer->CreateImGuiTexture(px, w, h))
				{
					m.id = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(handle));
					m.w = static_cast<float>(w);
					m.h = static_cast<float>(h);
					m.renderer = S->ctx->renderer;
				}
				stbi_image_free(px);
			}
	}
	return { m.id, m.w, m.h };
}

// A new picture card is as big as the picture, down to at most this on its longer side.
void cardSizeFor(int pw, int ph, double& w, double& h)
{
	constexpr double kLongest = 320.0, kCaption = 8.0;
	if (pw <= 0 || ph <= 0) { w = 240; h = 160; return; }
	const double k = std::min(1.0, kLongest / std::max(pw, ph));
	w = std::max(60.0, pw * k) + 2 * kCaption;
	h = std::max(40.0, ph * k) + 2 * kCaption;
}

// A card of `type` centred on (cx, cy); `fill` sets what the type needs. It is selected afterwards.
void placeCard(const UUID& boardId, NodeType type, double cx, double cy, double w, double h,
               std::function<void(Node&)> fill)
{
	auto made = std::make_shared<UUID>();
	change(boardId, [=](Board& b) {
		Node* n = b.addNode(type, cx - w * 0.5, cy - h * 0.5, w, h, "", taskNowMs());
		if (fill) fill(*n);
		*made = n->id;
		return true;
	}, [=] {
		s_sel = { *made };
		s_selEdge = UUID{};
	});
}

void placeMediaImage(const UUID& boardId, const std::string& name, double cx, double cy)
{
	int w = 0, h = 0, ch = 0;
	const std::filesystem::path file = mediaPath(S->root, name);
	if (!file.empty()) stbi_info(file.string().c_str(), &w, &h, &ch);
	double cw, chh;
	cardSizeFor(w, h, cw, chh);
	placeCard(boardId, NodeType::Image, cx, cy, cw, chh, [=](Node& n) { n.media = name; });
}

// A picture file (a drop) or the clipboard's picture becomes a card; false when it was not one.
bool importPictureFile(const UUID& boardId, const std::filesystem::path& file, double cx, double cy)
{
	if (S->root.empty()) return false;
	const std::string name = importMediaFile(S->root, file, nullptr);
	if (name.empty()) return false;
	placeMediaImage(boardId, name, cx, cy);
	return true;
}

// A link from the Content Browser or the Outliner: a Texture asset is shown as a picture, the
// rest stands as a card that names it.
void placeLink(const UUID& boardId, const TaskLink& link, double cx, double cy)
{
	if (link.kind == TaskLinkKind::Asset && S->ctx && S->ctx->contentManager)
	{
		const std::string rel = S->ctx->contentManager->pathForId(link.id);
		const std::string abs = rel.empty() ? std::string{} : S->ctx->contentManager->resolveAbsolutePath(rel);
		if (!abs.empty() && EditorAssetTypeCache::assetTypeOf(abs) == AssetType::Texture)
		{
			uint32_t pw = 0, ph = 0;
			AssetThumbnailCache::image(abs, &pw, &ph);
			double w, h;
			cardSizeFor(static_cast<int>(pw), static_cast<int>(ph), w, h);
			const UUID id = link.id;
			const std::string label = link.label;
			placeCard(boardId, NodeType::Image, cx, cy, w, h, [=](Node& n) { n.asset = id; n.text = label; });
			return;
		}
	}
	placeCard(boardId, NodeType::Object, cx, cy, 240, 80, [=](Node& n) { n.link = link; });
}

void copySelection(const UUID& boardId)
{
	const Board* b = S->ws->findBoard(boardId);
	if (!b || s_sel.empty()) return;
	s_clip = copyNodes(*b, s_sel);
	s_pasteCount = 0;
	if (S->setClipboardText) S->setClipboardText(kClipMarker);
}

void pasteClip(const UUID& boardId, const BoardClip& clip, double offset)
{
	if (clip.empty()) return;
	auto made = std::make_shared<std::vector<UUID>>();
	change(boardId, [=](Board& b) {
		*made = pasteNodes(b, clip, offset, offset, taskNowMs());
		return !made->empty();
	}, [=] {
		s_sel = *made;
		s_selEdge = UUID{};
	});
}

void duplicateSelection(const UUID& boardId)
{
	const Board* b = S->ws->findBoard(boardId);
	if (!b || s_sel.empty()) return;
	pasteClip(boardId, copyNodes(*b, s_sel), 30.0);
}

void alignSelection(const UUID& boardId, Board::Align how)
{
	const std::vector<UUID> ids = s_sel;
	change(boardId, [=](Board& b) { return b.alignNodes(ids, how, taskNowMs()) > 0; });
}

void distributeSelection(const UUID& boardId, bool horizontal)
{
	const std::vector<UUID> ids = s_sel;
	change(boardId, [=](Board& b) { return b.distributeNodes(ids, horizontal, taskNowMs()) > 0; });
}

// A card for a page or a row of a database.
void addPageCard(const UUID& boardId, const UUID& page, double cx, double cy)
{
	placeCard(boardId, NodeType::Page, cx, cy, 240, 80, [=](Node& n) { n.page = page; });
}

void addRowCard(const UUID& boardId, const UUID& database, const UUID& row, double cx, double cy)
{
	placeCard(boardId, NodeType::Row, cx, cy, 240, 80, [=](Node& n) { n.database = database; n.row = row; });
}

void paste(const UUID& boardId, double cx, double cy)
{
	// Cards copied in the editor: Copy left the marker where a picture or text would be.
	if (!s_clip.empty() && S->clipboardText && S->clipboardText() == kClipMarker)
	{
		++s_pasteCount;
		pasteClip(boardId, s_clip, 30.0 * s_pasteCount);
		return;
	}
	std::string bytes, ext;
	if (S->clipboardImage && S->clipboardImage(bytes, ext) && !bytes.empty() && !S->root.empty())
	{
		const std::string name = importMediaBytes(S->root, bytes, ext, nullptr);
		if (!name.empty()) { placeMediaImage(boardId, name, cx, cy); return; }
	}
	const std::string text = S->clipboardText ? S->clipboardText() : std::string{};
	if (text.empty()) return;
	placeCard(boardId, NodeType::Text, cx, cy, 240, 120, [=](Node& n) { n.text = text; });
}

bool isPictureFile(const std::string& path)
{
	std::string ext = std::filesystem::path(path).extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".bmp" || ext == ".webp" || ext == ".tga";
}

// ── Drawing the cards ────────────────────────────────────────────────────────
std::string labelOfNode(Workspace& ws, const Node& n)
{
	switch (n.type)
	{
	case NodeType::Row:
	{
		const Database* db = ws.find(n.database);
		const Row* r = db ? db->findRow(n.row) : nullptr;
		return r ? (r->title.empty() ? std::string("Untitled") : r->title) : std::string("(row not found)");
	}
	case NodeType::Page:
	{
		const Page* p = ws.findPage(n.page);
		return p ? (p->title.empty() ? std::string("Untitled") : p->title) : std::string("(page not found)");
	}
	case NodeType::Object: return n.link.label.empty() ? std::string("(object)") : n.link.label;
	case NodeType::Image:  return n.text.empty() ? std::string("(picture)") : n.text;
	default:               return n.text;
	}
}

// ── Text: the canvas lets ImGui draw it; the export collects it and draws it itself ─────────────
struct SunkText { ImVec2 pos; float size; ImU32 col; std::string text; float wrapW; ImVec2 cmin, cmax; };
std::vector<SunkText>* s_textSink = nullptr;
UUID s_hoverNode;   // the card under the mouse (a link underlines)

void putText(ImDrawList* dl, float size, ImVec2 pos, ImU32 col, const std::string& text, float wrapW, ImVec2 cmin, ImVec2 cmax)
{
	if (s_textSink) { s_textSink->push_back({ pos, size, col, text, wrapW, cmin, cmax }); return; }
	dl->PushClipRect(cmin, cmax, true);
	dl->AddText(ImGui::GetFont(), size, pos, col, text.c_str(), nullptr, wrapW);
	dl->PopClipRect();
}

// What a link card says under its name.
std::string linkKind(Workspace& ws, const Node& n)
{
	switch (n.type)
	{
	case NodeType::Row:
	{
		const Database* db = ws.find(n.database);
		return db ? (db->name.empty() ? std::string("Row") : db->name) : std::string("not found");
	}
	case NodeType::Page: return ws.findPage(n.page) ? "Page" : "not found";
	default:
		switch (n.link.kind)
		{
		case TaskLinkKind::Asset:  return "Asset";
		case TaskLinkKind::Scene:  return "Scene";
		case TaskLinkKind::Entity: return "Entity";
		case TaskLinkKind::Panel:  return "Editor panel";
		default:                   return "Link";
		}
	}
}

bool linkMissing(Workspace& ws, const Node& n)
{
	if (n.type == NodeType::Row) { const Database* db = ws.find(n.database); return !db || !db->findRow(n.row); }
	if (n.type == NodeType::Page) return !ws.findPage(n.page);
	return false;
}

void drawNode(ImDrawList* dl, const Xf& xf, Workspace& ws, const Node& n, const Rect2& shown, bool selected, bool editing)
{
	ImVec2 a, b;
	xf.rectToScreen(shown, a, b);
	const float z = static_cast<float>(xf.v.zoom);
	const float round = std::min(8.0f * z, 12.0f);
	ImVec4 tint{};
	const bool coloured = colourOf(n.color, tint);

	if (n.type == NodeType::Frame)
	{
		const ImVec4 base = coloured ? tint : ImVec4(0.7f, 0.7f, 0.7f, 1);
		dl->AddRectFilled(a, b, Theme::u32(Theme::alpha(base, 0.10f)), round);
		dl->AddRect(a, b, Theme::u32(Theme::alpha(base, selected ? 0.95f : 0.55f)), round, 0, selected ? 2.5f : 1.5f);
	}
	else
	{
		// A link keeps a neutral card: a colour recolours its accent (bar, glyph, name), not the background.
		const bool isLink = isLinkNode(n.type);
		const ImVec4 fill = coloured && !isLink ? tint : Theme::warm(0.20f);
		dl->AddRectFilled(ImVec2(a.x + 2 * z, a.y + 3 * z), ImVec2(b.x + 2 * z, b.y + 3 * z), IM_COL32(0, 0, 0, 60), round);
		dl->AddRectFilled(a, b, Theme::u32(fill), round);
		dl->AddRect(a, b, selected ? Theme::u32(Theme::AccentBright) : Theme::u32(Theme::warm(0.32f)), round, 0, selected ? 2.5f : 1.0f);
	}
	const float fontSize = ImGui::GetFontSize() * z;

	if (n.type == NodeType::Image)
	{
		const Pic pic = pictureOf(n);
		if (pic.tex && pic.w > 0 && pic.h > 0)
		{
			const float pad = 8.0f * z;
			const float captionH = n.text.empty() ? 0.0f : fontSize * 1.5f;
			const ImVec2 lo(a.x + pad, a.y + pad), hi(b.x - pad, b.y - pad - captionH);
			const float aw = hi.x - lo.x, ah = hi.y - lo.y;
			if (aw > 2 && ah > 2)
			{
				const float k = std::min(aw / pic.w, ah / pic.h);
				const float dw = pic.w * k, dh = pic.h * k;
				const ImVec2 p0(lo.x + (aw - dw) * 0.5f, lo.y + (ah - dh) * 0.5f);
				dl->AddImage(pic.tex, p0, ImVec2(p0.x + dw, p0.y + dh));
			}
			if (!editing && captionH > 0 && fontSize >= 5.0f)
				putText(dl, fontSize, ImVec2(a.x + pad, b.y - pad - fontSize * 1.2f), Theme::u32(coloured ? Theme::Ink : Theme::Text), n.text,
				        std::max(10.0f, (b.x - a.x) - 2 * pad), a, b);
			return;
		}
	}
	if (editing) return;   // the field over it draws the text

	// A card that stands for something in the editor is a link: a coloured bar and glyph, the name in the link
	// colour (underlined under the mouse), what it is under it, and an arrow out.
	if (isLinkNode(n.type))
	{
		const bool missing = linkMissing(ws, n);
		const ImVec4 linkCol = missing ? Theme::Bad : coloured ? tint : ImVec4(0.50f, 0.74f, 1.0f, 1.0f);
		const bool hot = s_hoverNode == n.id;
		// A wash of the link colour over the card, a bar down its left side.
		dl->AddRectFilled(a, b, Theme::u32(Theme::alpha(linkCol, hot ? 0.20f : 0.11f)), round);
		dl->AddRectFilled(a, ImVec2(a.x + 5.0f * z, b.y), Theme::u32(Theme::alpha(linkCol, 0.95f)), round, ImDrawFlags_RoundCornersLeft);
		const float midY = (a.y + b.y) * 0.5f;
		const PlannerIcons::Icon icon = n.type == NodeType::Row ? PlannerIcons::Icon::Table
		                              : n.type == NodeType::Page ? PlannerIcons::Icon::Page : PlannerIcons::Icon::Cube;
		PlannerIcons::drawIcon(dl, icon, ImVec2(a.x + 24.0f * z, midY), Theme::u32(linkCol), 0, std::max(0.4f, 1.25f * z));
		PlannerIcons::drawIcon(dl, PlannerIcons::Icon::ArrowOut, ImVec2(b.x - 16.0f * z, a.y + 16.0f * z),
		                       Theme::u32(Theme::alpha(linkCol, hot ? 1.0f : 0.7f)), 0, std::max(0.4f, z));
		if (fontSize < 5.0f) return;
		const std::string label = labelOfNode(ws, n);
		const std::string kind = linkKind(ws, n);
		const float textX = a.x + 44.0f * z, room = std::max(10.0f, b.x - textX - 30.0f * z);
		const float subSize = fontSize * 0.82f;
		const float blockH = fontSize + subSize + 3.0f * z;
		const float titleY = midY - blockH * 0.5f;
		putText(dl, fontSize, ImVec2(textX, titleY), Theme::u32(linkCol), label, room, a, b);
		if (hot || selected)
		{
			const float w = std::min(room, ImGui::CalcTextSize(label.c_str()).x * z);
			dl->AddLine(ImVec2(textX, titleY + fontSize + 1.0f * z), ImVec2(textX + w, titleY + fontSize + 1.0f * z), Theme::u32(linkCol), std::max(1.0f, z));
		}
		putText(dl, subSize, ImVec2(textX, titleY + fontSize + 3.0f * z), Theme::u32(Theme::TextDim), kind, room, a, b);
		return;
	}

	if (fontSize < 5.0f) return;
	const std::string label = labelOfNode(ws, n);
	const ImU32 ink = Theme::u32(n.type == NodeType::Frame ? Theme::TextHeading : (coloured ? Theme::Ink : Theme::Text));
	const float pad = 8.0f * z;
	const std::string prefix = n.type == NodeType::Image ? (n.asset == UUID{} && n.media.empty() ? "Image: " : "Image (not shown): ") : std::string();
	putText(dl, fontSize, ImVec2(a.x + pad, a.y + pad), ink, prefix + label, std::max(10.0f, (b.x - a.x) - 2 * pad), a, b);
}

void drawArrowHead(ImDrawList* dl, ImVec2 tip, ImVec2 from, float size, ImU32 col)
{
	const float dx = tip.x - from.x, dy = tip.y - from.y;
	const float len = std::sqrt(dx * dx + dy * dy);
	if (len < 1e-3f) return;
	const float ux = dx / len, uy = dy / len;
	const ImVec2 base(tip.x - ux * size, tip.y - uy * size);
	dl->AddTriangleFilled(tip, ImVec2(base.x - uy * size * 0.5f, base.y + ux * size * 0.5f),
	                      ImVec2(base.x + uy * size * 0.5f, base.y - ux * size * 0.5f), col);
}

} // namespace

// ── The public state ─────────────────────────────────────────────────────────

ViewState viewOf(const UUID& board)
{
	const auto it = s_views.find(board);
	return it == s_views.end() ? ViewState{} : it->second;
}

void setView(const UUID& board, ViewState v)
{
	v.zoom = std::clamp(v.zoom, kMinZoom, kMaxZoom);
	s_views[board] = v;
}

std::vector<UUID> selection() { return s_sel; }
UUID              selectedEdge() { return s_selEdge; }
Stats             lastStats() { return s_stats; }
bool              canUndo(const UUID& board) { const auto it = s_history.find(board); return it != s_history.end() && it->second.canUndo(); }
bool              canRedo(const UUID& board) { const auto it = s_history.find(board); return it != s_history.end() && it->second.canRedo(); }

void forget(const UUID& board)
{
	s_views.erase(board);
	s_history.erase(board);
	if (s_boardOfSelection == board)
	{
		s_sel.clear();
		s_selEdge = UUID{};
		s_editNode = UUID{};
		s_drag = Drag::None;
	}
}

bool wantsFileDrop(const std::string& path, float x, float y)
{
	if (!isPictureFile(path)) return false;
	if (!ImGui::GetCurrentContext() || ImGui::GetFrameCount() - s_canvasFrame > 2) return false;
	return x >= s_canvasMin.x && x <= s_canvasMax.x && y >= s_canvasMin.y && y <= s_canvasMax.y;
}

void dropFile(const std::string& path, float x, float y)
{
	s_dropped.push_back({ path, ImVec2(x, y) });
}

void reset()
{
	for (auto& kv : s_mediaTex)
		if (kv.second.id && kv.second.renderer) kv.second.renderer->DestroyImGuiTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(kv.second.id)));
	s_mediaTex.clear();
	s_dropped.clear();
	s_views.clear();
	s_history.clear();
	s_sel.clear();
	s_selEdge = s_editNode = s_titleFor = s_labelFor = s_boardOfSelection = s_connectFrom = s_resizeNode = s_dragHit = UUID{};
	s_drag = Drag::None;
	s_dragNodes.clear();
	s_stats = {};
	s_editBuf.clear();
	s_editFocus = false;
}


// Draws the collected texts into the picture from the UI font's own file data (PlannerFontText.h): the glyphs
// of ImGui's atlas live on the GPU in the editor, and a picture of the board must not depend on them.
void blitTexts(he_ui::Image& img, const std::vector<SunkText>& texts)
{
	ImFont* font = ImGui::GetFont();
	const ImFontConfig* src = font && font->Sources.Size > 0 ? font->Sources[0] : nullptr;
	if (!src || !src->FontData || !img.valid()) return;
	std::vector<PlannerFontText::Run> runs;
	for (const SunkText& t : texts)
		runs.push_back({ t.pos.x, t.pos.y, t.size, t.col, t.text, t.wrapW, t.cmin.x, t.cmin.y, t.cmax.x, t.cmax.y });
	PlannerFontText::draw(static_cast<const unsigned char*>(src->FontData), src->FontNo, img.rgba.data(), img.width, img.height, runs);
}

// ── Export ───────────────────────────────────────────────────────────────────
std::filesystem::path exportPathFor(const std::filesystem::path& projectRoot, const std::string& boardTitle)
{
	std::string name;
	for (char c : boardTitle)
		name += (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') ? c : '_';
	while (!name.empty() && name.back() == ' ') name.pop_back();
	if (name.empty()) name = "Whiteboard";
	return projectRoot / "Export" / "Planner" / (name + ".png");
}

bool exportPng(const Services& services, const UUID& boardId, const std::filesystem::path& file, std::string* error)
{
	auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
	const Board* board = services.ws ? services.ws->findBoard(boardId) : nullptr;
	if (!board) return fail("no such whiteboard");
	Rect2 r;
	if (!board->bounds(r)) return fail("the whiteboard is empty");
	if (!ImGui::GetCurrentContext()) return fail("no UI context");

	constexpr double kMargin = 40.0, kLongest = 4096.0;
	const double worldW = r.w + 2 * kMargin, worldH = r.h + 2 * kMargin;
	const double zoom = std::clamp(std::min(2.0, kLongest / std::max(worldW, worldH)), 0.05, 2.0);
	const int W = std::max(1, static_cast<int>(std::ceil(worldW * zoom))), H = std::max(1, static_cast<int>(std::ceil(worldH * zoom)));
	const Xf xf{ ImVec2(0, 0), ViewState{ r.x - kMargin, r.y - kMargin, zoom } };

	const Services* const savedServices = S;
	S = &services;
	std::unordered_map<std::string, MediaTex> pictures;
	s_exportTex = &pictures;
	std::vector<SunkText> texts;   // ImGui's font atlas is the GPU's in the editor: the text is drawn below, from the font file
	s_textSink = &texts;
	s_hoverNode = UUID{};

	ImDrawList dl(ImGui::GetDrawListSharedData());
	dl._ResetForNewFrame();
	dl.PushTexture(ImGui::GetIO().Fonts->TexRef);
	dl.PushClipRect(ImVec2(0, 0), ImVec2(static_cast<float>(W), static_cast<float>(H)), false);

	// Arrows under the cards' text but over frames: the same order the canvas draws in.
	for (const Node& n : board->nodes)
		if (n.type == NodeType::Frame) drawNode(&dl, xf, *services.ws, n, n.rect(), false, false);
	for (const Edge& e : board->edges)
	{
		const Node* from = board->findNode(e.from);
		const Node* to = board->findNode(e.to);
		if (!from || !to) continue;
		double ax, ay, bx, by;
		anchorOf(*from, to->x + to->w * 0.5, to->y + to->h * 0.5, ax, ay);
		anchorOf(*to, from->x + from->w * 0.5, from->y + from->h * 0.5, bx, by);
		const ImVec2 pa = xf.toScreen(ax, ay), pb = xf.toScreen(bx, by);
		ImVec4 col = Theme::TextDim;
		colourOf(e.color, col);
		const ImU32 c = Theme::u32(col);
		dl.AddLine(pa, pb, c, 2.0f);
		if (e.arrow) drawArrowHead(&dl, pb, pa, 11.0f, c);
		if (!e.label.empty())
		{
			const ImVec2 mid((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f);
			const float z = static_cast<float>(zoom);
			const ImVec2 ts(ImGui::CalcTextSize(e.label.c_str()).x * z, ImGui::GetFontSize() * z);
			dl.AddRectFilled(ImVec2(mid.x - ts.x * 0.5f - 4, mid.y - ts.y * 0.5f - 2), ImVec2(mid.x + ts.x * 0.5f + 4, mid.y + ts.y * 0.5f + 2),
			                 Theme::u32(Theme::warm(0.12f)), 4.0f);
			putText(&dl, ImGui::GetFontSize() * z, ImVec2(mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f), Theme::u32(Theme::Text), e.label, 0.0f,
			        ImVec2(0, 0), ImVec2(static_cast<float>(W), static_cast<float>(H)));
		}
	}
	for (const Node& n : board->nodes)
		if (n.type != NodeType::Frame) drawNode(&dl, xf, *services.ws, n, n.rect(), false, false);

	// The rasteriser reads CPU textures: the font atlas is copied, the pictures are already there.
	std::vector<std::pair<ImTextureID, ImTextureID>> atlases;
	auto cpuAtlas = [&](ImTextureID id) {
		for (const auto& [from, to] : atlases)
			if (from == id) return to;
		ImTextureID to = id;
		for (ImTextureData* t : ImGui::GetPlatformIO().Textures)
		{
			if (!t || t->GetTexID() != id || !t->Pixels || t->Width <= 0 || t->Height <= 0) continue;
			const size_t count = static_cast<size_t>(t->Width) * static_cast<size_t>(t->Height);
			std::vector<std::uint8_t> rgba(count * 4);
			for (size_t i = 0; i < count; ++i)
			{
				if (t->BytesPerPixel == 1) { rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255; rgba[i * 4 + 3] = t->Pixels[i]; }
				else std::memcpy(&rgba[i * 4], &t->Pixels[i * 4], 4);
			}
			to = he_ui::registerTexture(rgba.data(), t->Width, t->Height);
			break;
		}
		atlases.emplace_back(id, to);
		return to;
	};
	auto isPicture = [&](ImTextureID id) {
		for (const auto& kv : pictures) if (kv.second.id == id && id) return true;
		return false;
	};
	for (ImDrawCmd& cmd : dl.CmdBuffer)
		if (!cmd.UserCallback && !isPicture(cmd.GetTexID())) cmd.TexRef = ImTextureRef(cpuAtlas(cmd.GetTexID()));

	ImDrawData data;
	data.Valid = true;
	data.DisplayPos = ImVec2(0, 0);
	data.DisplaySize = ImVec2(static_cast<float>(W), static_cast<float>(H));
	data.FramebufferScale = ImVec2(1, 1);
	data.Textures = nullptr;
	data.CmdLists.push_back(&dl);
	data.CmdListsCount = 1;
	data.TotalVtxCount = dl.VtxBuffer.Size;
	data.TotalIdxCount = dl.IdxBuffer.Size;
	he_ui::Image img = he_ui::rasterize(&data, W, H, IM_COL32(26, 24, 21, 255));
	s_textSink = nullptr;
	blitTexts(img, texts);
	data.CmdLists.clear();   // dl is ours; the data does not own it

	for (const auto& [from, to] : atlases) if (to != from) he_ui::unregisterTexture(to);
	for (auto& kv : pictures) if (kv.second.id) he_ui::unregisterTexture(kv.second.id);
	s_exportTex = nullptr;
	S = savedServices;

	if (!img.valid()) return fail("the picture could not be drawn");
	std::error_code ec;
	std::filesystem::create_directories(file.parent_path(), ec);
	if (!hePngWrite(file, img.rgba.data(), img.width, img.height)) return fail("could not write " + file.string());
	return true;
}

// ── The toolbar and the canvas ───────────────────────────────────────────────

namespace
{
ImVec2 s_canvasSize(800, 500);   // the canvas of the last frame: where "the middle" is for a new card
}

void draw(const Services& services, const UUID& boardId)
{
	S = &services;
	Workspace& ws = *services.ws;
	Board* board = ws.findBoard(boardId);
	if (!board)
	{
		ImGui::TextDisabled("This board is gone.");
		return;
	}
	if (!(s_boardOfSelection == boardId))
	{
		// Another board: what was selected or half-done there means nothing here.
		s_boardOfSelection = boardId;
		s_sel.clear();
		s_selEdge = s_editNode = s_connectFrom = UUID{};
		s_drag = Drag::None;
	}
	// A selection that a pull or an undo made stale.
	s_sel.erase(std::remove_if(s_sel.begin(), s_sel.end(), [&](const UUID& id) { return !board->findNode(id); }), s_sel.end());
	if (!(s_selEdge == UUID{}) && !board->findEdge(s_selEdge)) s_selEdge = UUID{};
	if (!(s_editNode == UUID{}) && !board->findNode(s_editNode)) s_editNode = UUID{};

	ViewState view = viewOf(boardId);
	const ImGuiIO& io = ImGui::GetIO();
	Workspace* wsp = services.ws;

	// ── Toolbar ──
	if (!(s_titleFor == boardId) || !ImGui::IsAnyItemActive()) { s_titleFor = boardId; s_titleBuf = board->title; }
	ImGui::SetNextItemWidth(240);
	ImGui::InputTextWithHint("##boardtitle", "Untitled board", &s_titleBuf);
	if (ImGui::IsItemDeactivatedAfterEdit() && s_titleBuf != board->title)
	{
		const std::string title = s_titleBuf;
		services.post([=] {
			wsp->editBoard(boardId, [&](Board& b) {
				if (b.title == title) return false;
				b.title = title;
				b.modifiedMs = taskNowMs();
				return true;
			});
		});
	}
	services.note("board:title");

	const double centreX = view.x + s_canvasSize.x * 0.5 / view.zoom, centreY = view.y + s_canvasSize.y * 0.5 / view.zoom;

	// The view's controls at the right of the title: history, zoom, overview, picture.
	bool fit = false, actual = false, zoomIn = false, zoomOut = false;
	{
		const float fh = ImGui::GetFrameHeight();   // every control in the row is as tall as the title field
		const ImVec2 bs(fh, fh);
		const float groupW = ImGui::GetFrameHeight() * 7 + 56 + 2 * 13 + 9 * 8;
		ImGui::SameLine();
		ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - groupW));
		if (iconButton("Undo", Icon::Undo, false, canUndo(boardId), "Planner/Undo", false, 0, bs)) undoOrRedo(boardId, true);
		services.note("Undo");
		ImGui::SameLine();
		if (iconButton("Redo", Icon::Redo, false, canRedo(boardId), "Planner/Redo", false, 0, bs)) undoOrRedo(boardId, false);
		services.note("Redo");
		iconSeparator();
		zoomOut = iconButton("Zoom Out", Icon::ZoomOut, false, true, "Planner/Zoom Out", false, 0, bs);
		services.note("Zoom Out");
		ImGui::SameLine();
		{
			// The zoom: click it to go back to 100%.
			ImGui::PushID("zoomvalue");
			const ImVec2 p = ImGui::GetCursorScreenPos();
			actual = ImGui::InvisibleButton("##z", ImVec2(56, fh));
			if (ImGui::IsItemHovered())
				ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 56, p.y + fh), Theme::u32(Theme::warm(0.28f)), 7.0f);
			char buf[16];
			std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(view.zoom * 100.0)));
			const ImVec2 ts = ImGui::CalcTextSize(buf);
			ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (56 - ts.x) * 0.5f, p.y + (fh - ts.y) * 0.5f), Theme::u32(Theme::Text), buf);
			EditorWidgets::helpForKey("Planner/100%");
			ImGui::PopID();
			services.note("100%");
		}
		ImGui::SameLine();
		zoomIn = iconButton("Zoom In", Icon::ZoomIn, false, true, "Planner/Zoom In", false, 0, bs);
		services.note("Zoom In");
		ImGui::SameLine();
		fit = iconButton("Fit", Icon::Fit, false, true, "Planner/Fit", false, 0, bs);
		services.note("Fit");
		iconSeparator();
		if (iconButton("Map", Icon::Map, s_minimap, true, "Planner/Map", false, 0, bs)) s_minimap = !s_minimap;
		services.note("Map");
		ImGui::SameLine();
		if (iconButton("Export PNG", Icon::Export, false, true, "Planner/Export PNG", false, 0, bs))
		{
			const Services copy = services;   // the post runs after this frame's services are gone
			services.post([=] {
				const std::filesystem::path file = exportPathFor(copy.root, copy.ws->findBoard(boardId) ? copy.ws->findBoard(boardId)->title : std::string());
				std::string err;
				if (copy.root.empty()) err = "no project is open";
				else if (exportPng(copy, boardId, file, &err))
					HE::Ed::notify(HE::Ed::NoteLevel::Info, "Whiteboard exported", file.string());
				if (!err.empty()) HE::Ed::notify(HE::Ed::NoteLevel::Important, "Whiteboard not exported: " + err);
			});
		}
		services.note("Export PNG");
	}

	// An arrow that is selected has a label and a head.
	if (const Edge* e = board->findEdge(s_selEdge))
	{
		if (!(s_labelFor == e->id) || !ImGui::IsAnyItemActive()) { s_labelFor = e->id; s_labelBuf = e->label; }
		ImGui::SetNextItemWidth(160);
		ImGui::InputTextWithHint("##edgelabel", "Arrow label", &s_labelBuf);
		if (ImGui::IsItemDeactivatedAfterEdit() && s_labelBuf != e->label)
		{
			const UUID eid = e->id;
			const std::string label = s_labelBuf;
			change(boardId, [=](Board& b) { return b.setEdgeLabel(eid, label, taskNowMs()); });
		}
		services.note("edge:label");
		bool head = e->arrow;
		ImGui::SameLine();
		if (EditorWidgets::checkbox("Arrow", &head))
		{
			const UUID eid = e->id;
			change(boardId, [=](Board& b) { return b.setEdgeArrow(eid, head, taskNowMs()); });
		}
	}

	// ── The canvas ──
	ImGui::BeginChild("##boardcanvas", ImVec2(0, 0), ImGuiChildFlags_Borders,
	                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 size = ImGui::GetContentRegionAvail();
	s_canvasSize = size;
	ImGui::SetNextItemAllowOverlap();   // a card being typed in sits on top of it
	ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
	// The minimap sits in the bottom right corner and takes the mouse there.
	const ImVec2 mapSize(180, 120);
	const ImVec2 mapMin(origin.x + size.x - mapSize.x - 10, origin.y + size.y - mapSize.y - 10);
	const ImVec2 mapMax(mapMin.x + mapSize.x, mapMin.y + mapSize.y);
	const bool mapOn = s_minimap && !board->nodes.empty() && size.x > mapSize.x * 2 && size.y > mapSize.y * 2;
	const bool overMap = mapOn && io.MousePos.x >= mapMin.x && io.MousePos.x <= mapMax.x && io.MousePos.y >= mapMin.y && io.MousePos.y <= mapMax.y;
	const bool hovered = ImGui::IsItemHovered() && !overMap && !s_mapDrag;
	services.noteRect("canvas", origin, ImVec2(origin.x + size.x, origin.y + size.y));
	s_canvasMin = origin;
	s_canvasMax = ImVec2(origin.x + size.x, origin.y + size.y);
	s_canvasFrame = ImGui::GetFrameCount();

	// What is dropped on the canvas: assets, scenes and entities from the editor's panels (right
	// after the item they are dropped on), and files from the system that arrived since last frame.
	{
		const ImGuiIO& dio = ImGui::GetIO();
		const Xf dropXf{ origin, view };
		if (services.ctx)
		{
			const std::vector<TaskLink> links = TaskBoardPanel::acceptDroppedLinks(*services.ctx);
			double dx, dy;
			dropXf.toWorld(dio.MousePos, dx, dy);
			double step = 0;
			for (const TaskLink& l : links) { placeLink(boardId, l, dx + step, dy + step); step += 28.0 / view.zoom; }
		}
		std::vector<DroppedFile> files;
		files.swap(s_dropped);
		double step = 0;
		for (const DroppedFile& f : files)
		{
			if (!isPictureFile(f.path)) continue;
			double fx, fy;
			dropXf.toWorld(f.at, fx, fy);
			if (importPictureFile(boardId, f.path, fx + step, fy + step)) step += 28.0 / view.zoom;
		}
	}
	const bool editing = !(s_editNode == UUID{});
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

	if (zoomIn || zoomOut)
	{
		const double cx = view.x + size.x * 0.5 / view.zoom, cy = view.y + size.y * 0.5 / view.zoom;
		view.zoom = std::clamp(view.zoom * (zoomIn ? 1.25 : 0.8), kMinZoom, kMaxZoom);
		view.x = cx - size.x * 0.5 / view.zoom;
		view.y = cy - size.y * 0.5 / view.zoom;
	}
	if (fit)
	{
		Rect2 r;
		if (board->bounds(r))
		{
			const double zoom = std::clamp(std::min(size.x / (r.w + 80.0), size.y / (r.h + 80.0)), kMinZoom, kMaxZoom);
			view.zoom = zoom;
			view.x = r.x + r.w * 0.5 - size.x * 0.5 / zoom;
			view.y = r.y + r.h * 0.5 - size.y * 0.5 / zoom;
		}
		else view = ViewState{};
	}
	if (actual)
	{
		const double cx = view.x + size.x * 0.5 / view.zoom, cy = view.y + size.y * 0.5 / view.zoom;
		view.zoom = 1.0;
		view.x = cx - size.x * 0.5;
		view.y = cy - size.y * 0.5;
	}

	// The trackpad's pinch zooms at the cursor (EditorInput.h: on macOS the native magnify event).
	if (hovered && !editing)
		if (const float pinch = EditorInput::pinchDelta(); pinch != 0.0f)
		{
			double mx, my;
			Xf{ origin, view }.toWorld(io.MousePos, mx, my);
			view.zoom = std::clamp(view.zoom * std::max(0.1, 1.0 + pinch * 2.0), kMinZoom, kMaxZoom);
			view.x = mx - (io.MousePos.x - origin.x) / view.zoom;
			view.y = my - (io.MousePos.y - origin.y) / view.zoom;
		}
	// The mouse wheel zooms at the cursor. A trackpad's two-finger scroll pans instead (Ctrl or Cmd with it zooms), as on
	// the editor's other canvases.
	if (hovered && !editing && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f))
	{
		if (!EditorInput::trackpadActive() || io.KeyCtrl || io.KeySuper)
		{
			double mx, my;
			Xf{ origin, view }.toWorld(io.MousePos, mx, my);
			view.zoom = std::clamp(view.zoom * std::pow(1.1, static_cast<double>(io.MouseWheel)), kMinZoom, kMaxZoom);
			view.x = mx - (io.MousePos.x - origin.x) / view.zoom;
			view.y = my - (io.MousePos.y - origin.y) / view.zoom;
		}
		else
		{
			view.x -= io.MouseWheelH * 30.0 / view.zoom;
			view.y -= io.MouseWheel * 30.0 / view.zoom;
		}
	}
	// Panning with the middle button, or Space and the left one.
	static ViewState panFrom;
	const bool space = ImGui::IsKeyDown(ImGuiKey_Space) && !io.WantTextInput;
	if (hovered && s_drag == Drag::None &&
	    (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
	     (space && ImGui::IsMouseClicked(ImGuiMouseButton_Left))))
	{
		s_drag = Drag::Pan;
		s_dragStart = io.MousePos;
		panFrom = view;
	}
	if (s_drag == Drag::Pan)
	{
		view.x = panFrom.x - (io.MousePos.x - s_dragStart.x) / view.zoom;
		view.y = panFrom.y - (io.MousePos.y - s_dragStart.y) / view.zoom;
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) s_drag = Drag::None;
	}

	const Xf xf{ origin, view };
	double mx, my;
	xf.toWorld(io.MousePos, mx, my);

	// What is shown where: a card being moved or resized is drawn at its new place.
	ImVec2 mouseDelta(io.MousePos.x - s_dragStart.x, io.MousePos.y - s_dragStart.y);
	const bool movingNow = s_drag == Drag::Move && s_dragMoved;
	// With Snap the card in hand lands on the grid and the others keep their distance to it.
	auto draggedBy = [&]() {
		double dx = mouseDelta.x / view.zoom, dy = mouseDelta.y / view.zoom;
		if (s_snap && !s_dragNodes.empty())
			if (const Node* lead = board->findNode(has(s_dragNodes, s_dragHit) ? s_dragHit : s_dragNodes.front()))
			{
				dx = std::round((lead->x + dx) / kGrid) * kGrid - lead->x;
				dy = std::round((lead->y + dy) / kGrid) * kGrid - lead->y;
			}
		return ImVec2(static_cast<float>(dx), static_cast<float>(dy));
	};
	auto shownRect = [&](const Node& n) {
		Rect2 r = n.rect();
		if (movingNow && has(s_dragNodes, n.id)) { const ImVec2 d = draggedBy(); r.x += d.x; r.y += d.y; }
		if (s_drag == Drag::Resize && n.id == s_resizeNode) r = s_resizeTo;
		return r;
	};

	// ── The grid ──
	{
		const float step = static_cast<float>(40.0 * view.zoom);
		if (step >= 10.0f)
		{
			const float x0 = origin.x + std::fmod(std::fmod(static_cast<float>(-view.x * view.zoom), step) + step, step);
			const float y0 = origin.y + std::fmod(std::fmod(static_cast<float>(-view.y * view.zoom), step) + step, step);
			for (float x = x0; x < origin.x + size.x; x += step)
				for (float y = y0; y < origin.y + size.y; y += step)
					dl->AddCircleFilled(ImVec2(x, y), 1.2f, Theme::u32(Theme::warm(0.26f)));
		}
	}

	// ── Arrows ──
	for (const Edge& e : board->edges)
	{
		const Node* from = board->findNode(e.from);
		const Node* to = board->findNode(e.to);
		if (!from || !to) continue;
		Node fromShown = *from, toShown = *to;
		{
			const Rect2 a = shownRect(*from), b = shownRect(*to);
			fromShown.x = a.x; fromShown.y = a.y; fromShown.w = a.w; fromShown.h = a.h;
			toShown.x = b.x; toShown.y = b.y; toShown.w = b.w; toShown.h = b.h;
		}
		double ax, ay, bx, by;
		anchorOf(fromShown, toShown.x + toShown.w * 0.5, toShown.y + toShown.h * 0.5, ax, ay);
		anchorOf(toShown, fromShown.x + fromShown.w * 0.5, fromShown.y + fromShown.h * 0.5, bx, by);
		const ImVec2 pa = xf.toScreen(ax, ay), pb = xf.toScreen(bx, by);
		const bool selected = e.id == s_selEdge;
		ImVec4 col = Theme::TextDim;
		colourOf(e.color, col);
		const ImU32 c = Theme::u32(selected ? Theme::AccentBright : col);
		dl->AddLine(pa, pb, c, selected ? 3.0f : 2.0f);
		if (e.arrow) drawArrowHead(dl, pb, pa, 11.0f, c);
		const ImVec2 mid((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f);
		if (!e.label.empty() && view.zoom > 0.35)
		{
			const ImVec2 ts = ImGui::CalcTextSize(e.label.c_str());
			dl->AddRectFilled(ImVec2(mid.x - ts.x * 0.5f - 4, mid.y - ts.y * 0.5f - 2), ImVec2(mid.x + ts.x * 0.5f + 4, mid.y + ts.y * 0.5f + 2),
			                  Theme::u32(Theme::warm(0.12f)), 4.0f);
			dl->AddText(ImVec2(mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f), Theme::u32(Theme::Text), e.label.c_str());
		}
		services.noteRect("edge:" + e.label, ImVec2(mid.x - 3, mid.y - 3), ImVec2(mid.x + 3, mid.y + 3));
	}

	// The card under the mouse: a link underlines and the cursor says it opens.
	s_hoverNode = hovered && s_drag == Drag::None && !editing ? board->hitTest(mx, my) : UUID{};
	if (const Node* hot = s_hoverNode == UUID{} ? nullptr : board->findNode(s_hoverNode))
		if (isLinkNode(hot->type)) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

	// ── Cards (only those on screen) ──
	const Rect2 visible{ view.x, view.y, size.x / view.zoom, size.y / view.zoom };
	s_stats = { static_cast<int>(board->nodes.size()), 0 };
	for (const Node& n : board->nodes)
	{
		const Rect2 shown = shownRect(n);
		if (!shown.intersects(visible)) continue;
		++s_stats.drawn;
		drawNode(dl, xf, ws, n, shown, has(s_sel, n.id), n.id == s_editNode);
		ImVec2 a, b;
		xf.rectToScreen(shown, a, b);
		services.noteRect("node:" + labelOfNode(ws, n), a, b);
	}

	// ── Handles and dots for the card in hand ──
	UUID dotNode;
	{
		const bool single = s_sel.size() == 1 && s_selEdge == UUID{};
		if (single) dotNode = s_sel[0];
		else if (hovered && s_drag == Drag::None && !editing) dotNode = board->hitTest(mx, my);
		if (s_drag == Drag::Connect) dotNode = s_connectFrom;
	}
	const Node* inHand = dotNode == UUID{} ? nullptr : board->findNode(dotNode);
	bool handlesShown = false;
	ImVec2 dotAt[4];   // N, E, S, W
	ImVec2 cornerAt[4];   // nw, ne, se, sw
	if (inHand && !editing)
	{
		const Rect2 r = shownRect(*inHand);
		ImVec2 a, b;
		xf.rectToScreen(r, a, b);
		dotAt[0] = ImVec2((a.x + b.x) * 0.5f, a.y);
		dotAt[1] = ImVec2(b.x, (a.y + b.y) * 0.5f);
		dotAt[2] = ImVec2((a.x + b.x) * 0.5f, b.y);
		dotAt[3] = ImVec2(a.x, (a.y + b.y) * 0.5f);
		cornerAt[0] = a; cornerAt[1] = ImVec2(b.x, a.y); cornerAt[2] = b; cornerAt[3] = ImVec2(a.x, b.y);
		static const char* const dotNames[] = { "dot:N", "dot:E", "dot:S", "dot:W" };
		static const char* const cornerNames[] = { "handle:nw", "handle:ne", "handle:se", "handle:sw" };
		for (int i = 0; i < 4; ++i)
		{
			const bool near = hovered && std::abs(io.MousePos.x - dotAt[i].x) < 9 && std::abs(io.MousePos.y - dotAt[i].y) < 9;
			dl->AddCircleFilled(dotAt[i], near ? kDotRadius + 2 : kDotRadius, Theme::u32(Theme::AccentHi));
			dl->AddCircle(dotAt[i], near ? kDotRadius + 2 : kDotRadius, IM_COL32(0, 0, 0, 120));
			services.noteRect(dotNames[i], ImVec2(dotAt[i].x - 5, dotAt[i].y - 5), ImVec2(dotAt[i].x + 5, dotAt[i].y + 5));
		}
		if (s_sel.size() == 1 && s_selEdge == UUID{})
		{
			handlesShown = true;
			for (int i = 0; i < 4; ++i)
			{
				dl->AddRectFilled(ImVec2(cornerAt[i].x - kHandle * 0.5f, cornerAt[i].y - kHandle * 0.5f),
				                  ImVec2(cornerAt[i].x + kHandle * 0.5f, cornerAt[i].y + kHandle * 0.5f), Theme::u32(Theme::AccentBright), 2.0f);
				services.noteRect(cornerNames[i], ImVec2(cornerAt[i].x - 4, cornerAt[i].y - 4), ImVec2(cornerAt[i].x + 4, cornerAt[i].y + 4));
			}
		}
	}

	// ── A card being typed in ──
	if (editing)
	{
		if (const Node* n = board->findNode(s_editNode))
		{
			ImVec2 a, b;
			xf.rectToScreen(shownRect(*n), a, b);
			ImGui::SetCursorScreenPos(a);
			ImGui::PushStyleColor(ImGuiCol_FrameBg, Theme::warm(0.13f));
			if (s_editFocus) { ImGui::SetKeyboardFocusHere(); s_editFocus = false; }
			ImGui::InputTextMultiline("##nodeedit", &s_editBuf, ImVec2(b.x - a.x, b.y - a.y), ImGuiInputTextFlags_WordWrap);
			ImGui::PopStyleColor();
			services.noteRect("edit", a, b);
			if (ImGui::IsItemDeactivated())
			{
				const UUID id = s_editNode;
				const std::string text = s_editBuf;
				s_editNode = UUID{};
				change(boardId, [=](Board& bb) {
					const Node* x = bb.findNode(id);
					return x && x->text != text && bb.setNodeText(id, text, taskNowMs());
				});
			}
		}
	}

	// ── Mouse: press ──
	const bool shift = io.KeyShift;
	const bool insideEdit = editing && ImGui::IsItemHovered();
	if (hovered && !insideEdit && !space && s_drag == Drag::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		int corner = -1, dot = -1;
		if (handlesShown)
			for (int i = 0; i < 4 && corner < 0; ++i)
				if (std::abs(io.MousePos.x - cornerAt[i].x) <= kHandle && std::abs(io.MousePos.y - cornerAt[i].y) <= kHandle) corner = i;
		if (corner < 0 && inHand)
			for (int i = 0; i < 4 && dot < 0; ++i)
				if (std::abs(io.MousePos.x - dotAt[i].x) <= 9 && std::abs(io.MousePos.y - dotAt[i].y) <= 9) dot = i;
		const UUID hit = board->hitTest(mx, my);
		if (corner >= 0)
		{
			s_drag = Drag::Resize;
			s_resizeNode = inHand->id;
			s_resizeCorner = corner;
			s_resizeFrom = s_resizeTo = inHand->rect();
		}
		else if (dot >= 0)
		{
			s_drag = Drag::Connect;
			s_connectFrom = inHand->id;
		}
		else if (!(hit == UUID{}))
		{
			s_dragHit = hit;
			s_dragShift = shift;
			s_selEdge = UUID{};
			if (shift)
			{
				if (has(s_sel, hit)) s_sel.erase(std::find(s_sel.begin(), s_sel.end(), hit));
				else                 s_sel.push_back(hit);
			}
			else if (!has(s_sel, hit)) s_sel = { hit };
			if (has(s_sel, hit))
			{
				s_dragNodes = s_sel;
				// A frame takes what lies inside it along.
				for (const UUID& id : s_sel)
					if (const Node* n = board->findNode(id); n && n->type == NodeType::Frame)
						for (const UUID& inner : board->nodesInside(id))
							if (!has(s_dragNodes, inner)) s_dragNodes.push_back(inner);
				s_drag = Drag::Move;
				s_dragMoved = false;
			}
			s_dragStart = io.MousePos;
		}
		else
		{
			// An arrow?
			UUID edgeHit;
			for (const Edge& e : board->edges)
			{
				const Node* from = board->findNode(e.from);
				const Node* to = board->findNode(e.to);
				if (!from || !to) continue;
				double ax, ay, bx, by;
				anchorOf(*from, to->x + to->w * 0.5, to->y + to->h * 0.5, ax, ay);
				anchorOf(*to, from->x + from->w * 0.5, from->y + from->h * 0.5, bx, by);
				if (distanceToSegment(io.MousePos, xf.toScreen(ax, ay), xf.toScreen(bx, by)) <= 6.0f) { edgeHit = e.id; break; }
			}
			if (!(edgeHit == UUID{}))
			{
				s_selEdge = edgeHit;
				s_sel.clear();
			}
			else
			{
				if (!shift) { s_sel.clear(); s_selEdge = UUID{}; }
				s_drag = Drag::Marquee;
				s_dragStart = io.MousePos;
			}
		}
	}

	// A drag that began this frame measures from where it began, not from the last one.
	mouseDelta = ImVec2(io.MousePos.x - s_dragStart.x, io.MousePos.y - s_dragStart.y);

	// ── Mouse: double-click ──
	if (hovered && !insideEdit && s_drag != Drag::Pan && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
	{
		const UUID hit = board->hitTest(mx, my);
		const Node* n = hit == UUID{} ? nullptr : board->findNode(hit);
		if (!n)
			createNode(boardId, NodeType::Text, mx, my);
		else if (n->type == NodeType::Text || n->type == NodeType::Frame || n->type == NodeType::Image)
		{
			s_editNode = hit;
			s_editBuf = n->text;
			s_editFocus = true;
			s_sel = { hit };
		}
		else if (n->type == NodeType::Page || n->type == NodeType::Row)
		{
			// These cards stand for something in the Planner: the double-click goes there.
			const NodeType type = n->type;
			const UUID page = n->page, db = n->database, row = n->row;
			services.post([=] {
				if (type == NodeType::Page) PlannerPanel::selectPage(page);
				else PlannerPanel::openRowInView(db, row);
			});
		}
		else if (n->type == NodeType::Object && services.ctx)
		{
			const TaskLink link = n->link;
			AppContext* ctx = services.ctx;
			services.post([=] { TaskBoardPanel::jumpToLink(*ctx, link); });
		}
		s_drag = Drag::None;
	}

	// ── Mouse: drag and release ──
	if (s_drag == Drag::Move && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		if (std::abs(mouseDelta.x) > 3.0f || std::abs(mouseDelta.y) > 3.0f) s_dragMoved = true;
	}
	if (s_drag == Drag::Resize && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		// The corner opposite the one in hand stays; the card is never smaller than the minimum.
		const Rect2& f = s_resizeFrom;
		const double fixedX = (s_resizeCorner == 0 || s_resizeCorner == 3) ? f.x + f.w : f.x;
		const double fixedY = (s_resizeCorner == 0 || s_resizeCorner == 1) ? f.y + f.h : f.y;
		const double gx = s_snap ? std::round(mx / kGrid) * kGrid : mx, gy = s_snap ? std::round(my / kGrid) * kGrid : my;
		double nx = std::min(gx, fixedX - Board::kMinSize), ny = std::min(gy, fixedY - Board::kMinSize);
		if (s_resizeCorner == 1 || s_resizeCorner == 2) nx = std::max(gx, fixedX + Board::kMinSize);
		if (s_resizeCorner == 2 || s_resizeCorner == 3) ny = std::max(gy, fixedY + Board::kMinSize);
		s_resizeTo = { std::min(nx, fixedX), std::min(ny, fixedY), std::abs(nx - fixedX), std::abs(ny - fixedY) };
	}
	if (s_drag == Drag::Marquee && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		const ImVec2 a(std::min(s_dragStart.x, io.MousePos.x), std::min(s_dragStart.y, io.MousePos.y));
		const ImVec2 b(std::max(s_dragStart.x, io.MousePos.x), std::max(s_dragStart.y, io.MousePos.y));
		dl->AddRectFilled(a, b, Theme::u32(Theme::alpha(Theme::AccentHi, 0.12f)));
		dl->AddRect(a, b, Theme::u32(Theme::AccentHi));
	}
	if (s_drag == Drag::Connect && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		if (const Node* from = board->findNode(s_connectFrom))
		{
			double ax, ay;
			anchorOf(*from, mx, my, ax, ay);
			const ImVec2 pa = xf.toScreen(ax, ay);
			const UUID over = board->hitTest(mx, my);
			dl->AddLine(pa, io.MousePos, Theme::u32(Theme::AccentBright), 2.0f);
			drawArrowHead(dl, io.MousePos, pa, 10.0f, Theme::u32(Theme::AccentBright));
			if (!(over == UUID{}) && !(over == s_connectFrom))
				if (const Node* target = board->findNode(over))
				{
					ImVec2 a, b;
					xf.rectToScreen(target->rect(), a, b);
					dl->AddRect(a, b, Theme::u32(Theme::AccentBright), 8.0f * static_cast<float>(view.zoom), 0, 3.0f);
				}
		}
	}
	if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		switch (s_drag)
		{
		case Drag::Move:
			if (s_dragMoved)
			{
				const std::vector<UUID> ids = s_dragNodes;
				const ImVec2 d = draggedBy();
				const double dx = d.x, dy = d.y;
				change(boardId, [=](Board& b) { return b.moveNodesBy(ids, dx, dy, taskNowMs()) > 0; });
			}
			else if (!s_dragShift && s_sel.size() > 1 && !(s_dragHit == UUID{}))
				s_sel = { s_dragHit };   // a click on one of several selected cards keeps that one
			break;
		case Drag::Resize:
		{
			const UUID id = s_resizeNode;
			const Rect2 to = s_resizeTo;
			if (std::abs(to.w - s_resizeFrom.w) > 0.01 || std::abs(to.h - s_resizeFrom.h) > 0.01 ||
			    std::abs(to.x - s_resizeFrom.x) > 0.01 || std::abs(to.y - s_resizeFrom.y) > 0.01)
				change(boardId, [=](Board& b) { return b.resizeNode(id, to.x, to.y, to.w, to.h, taskNowMs()); });
			break;
		}
		case Drag::Marquee:
		{
			if (std::abs(mouseDelta.x) > 3.0f || std::abs(mouseDelta.y) > 3.0f)
			{
				double x0, y0, x1, y1;
				xf.toWorld(s_dragStart, x0, y0);
				xf.toWorld(io.MousePos, x1, y1);
				const std::vector<UUID> inside = board->nodesIn({ std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0), std::abs(y1 - y0) });
				for (const UUID& id : inside)
					if (!has(s_sel, id)) s_sel.push_back(id);
			}
			break;
		}
		case Drag::Connect:
		{
			const UUID from = s_connectFrom;
			const UUID to = board->hitTest(mx, my);
			if (!(to == UUID{}) && !(to == from))
				change(boardId, [=](Board& b) { return b.addEdge(from, to, "", taskNowMs()) != nullptr; });
			break;
		}
		default: break;
		}
		if (s_drag != Drag::Pan) s_drag = Drag::None;
		s_dragNodes.clear();
	}

	// ── The minimap ──
	if (mapOn)
	{
		Rect2 world{ view.x, view.y, size.x / view.zoom, size.y / view.zoom };   // everything and what is on screen
		Rect2 bb;
		if (board->bounds(bb))
		{
			const double x0 = std::min(world.x, bb.x), y0 = std::min(world.y, bb.y);
			const double x1 = std::max(world.x + world.w, bb.x + bb.w), y1 = std::max(world.y + world.h, bb.y + bb.h);
			world = { x0, y0, x1 - x0, y1 - y0 };
		}
		const double pad = std::max(world.w, world.h) * 0.04;
		world = { world.x - pad, world.y - pad, world.w + 2 * pad, world.h + 2 * pad };
		const double k = std::min(mapSize.x / world.w, mapSize.y / world.h);
		const ImVec2 inner(static_cast<float>(world.w * k), static_cast<float>(world.h * k));
		const ImVec2 o(mapMin.x + (mapSize.x - inner.x) * 0.5f, mapMin.y + (mapSize.y - inner.y) * 0.5f);
		auto toMap = [&](double wx, double wy) { return ImVec2(o.x + static_cast<float>((wx - world.x) * k), o.y + static_cast<float>((wy - world.y) * k)); };

		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && overMap) s_mapDrag = true;
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) s_mapDrag = false;
		if (s_mapDrag)
		{
			// The press puts the middle of the screen where it is.
			const double wx = world.x + (io.MousePos.x - o.x) / k, wy = world.y + (io.MousePos.y - o.y) / k;
			view.x = wx - size.x * 0.5 / view.zoom;
			view.y = wy - size.y * 0.5 / view.zoom;
		}

		dl->AddRectFilled(mapMin, mapMax, Theme::u32(Theme::alpha(Theme::warm(0.08f), 0.92f)), 6.0f);
		dl->AddRect(mapMin, mapMax, Theme::u32(Theme::warm(0.32f)), 6.0f);
		dl->PushClipRect(mapMin, mapMax, true);
		for (const Node& n : board->nodes)
		{
			ImVec4 tint{};
			const bool coloured = colourOf(n.color, tint);
			const ImVec2 a = toMap(n.x, n.y), b = toMap(n.x + n.w, n.y + n.h);
			if (n.type == NodeType::Frame) dl->AddRect(a, b, Theme::u32(coloured ? tint : Theme::warm(0.4f)));
			else dl->AddRectFilled(a, b, Theme::u32(coloured ? tint : Theme::warm(0.45f)));
		}
		dl->AddRect(toMap(view.x, view.y), toMap(view.x + size.x / view.zoom, view.y + size.y / view.zoom), Theme::u32(Theme::AccentBright), 0, 0, 1.5f);
		dl->PopClipRect();
		services.noteRect("map", mapMin, mapMax);
	}

	// ── The floating tools ──
	{
		// Left: what makes cards.
		if (beginPlate("##palette", ImVec2(origin.x + 12, origin.y + 12), ImVec2(40, 4 * 28 + 3 * 4 + 10 + 12)))
		{
			if (iconButton("New Card", Icon::Note, false, true, "Planner/New Card")) createNode(boardId, NodeType::Text, centreX, centreY);
			services.note("New Card");
			if (iconButton("New Frame", Icon::Frame, false, true, "Planner/New Frame")) createNode(boardId, NodeType::Frame, centreX, centreY);
			services.note("New Frame");
			if (iconButton("Add", Icon::AddCard, false, true, "Planner/Add")) ImGui::OpenPopup("##addcard");
			services.note("Add");
			if (ImGui::BeginPopup("##addcard"))
			{
				ImGui::TextDisabled("Page");
				int shown = 0;
				for (Page* p : wsp->pages())
				{
					if (++shown > 40) { ImGui::TextDisabled("..."); break; }
					ImGui::PushID(p);
					if (ImGui::Selectable(p->title.empty() ? "Untitled" : p->title.c_str())) addPageCard(boardId, p->id, centreX, centreY);
					services.note("add:" + p->title);
					ImGui::PopID();
				}
				for (Database* db : wsp->databases())
				{
					if (!ImGui::BeginMenu(db->name.empty() ? "Untitled" : db->name.c_str())) continue;
					int rows = 0;
					for (const Row& r : db->rows)
					{
						if (++rows > 60) { ImGui::TextDisabled("..."); break; }
						ImGui::PushID(&r);
						if (ImGui::Selectable(r.title.empty() ? "Untitled" : r.title.c_str())) addRowCard(boardId, db->id, r.id, centreX, centreY);
						services.note("add:" + db->name + ":" + r.title);
						ImGui::PopID();
					}
					ImGui::EndMenu();
				}
				ImGui::EndPopup();
			}
			if (iconButton("Snap", Icon::Snap, s_snap, true, "Planner/Snap")) s_snap = !s_snap;
			services.note("Snap");
		}
		endPlate();

		// Over the selection: what can be done to it.
		const bool anyCards = !s_sel.empty(), anyEdge = !(s_selEdge == UUID{});
		if ((anyCards || anyEdge) && s_drag == Drag::None && !editing)
		{
			const int buttons = anyCards ? (s_sel.size() >= 2 ? 4 : 3) : 1;
			const ImVec2 barSize(buttons * 28.0f + (buttons - 1) * 2.0f + 10.0f + (anyCards ? 16.0f : 0.0f), 38.0f);
			ImVec2 at(origin.x + size.x * 0.5f - barSize.x * 0.5f, origin.y + 12);
			if (anyCards)
			{
				float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
				for (const Node& n : board->nodes)
					if (has(s_sel, n.id))
					{
						ImVec2 a, b;
						xf.rectToScreen(shownRect(n), a, b);
						x0 = std::min(x0, a.x); y0 = std::min(y0, a.y); x1 = std::max(x1, b.x); y1 = std::max(y1, b.y);
					}
				at = ImVec2((x0 + x1) * 0.5f - barSize.x * 0.5f, y0 - barSize.y - 8.0f);
				if (at.y < origin.y + 8) at.y = std::min(y1 + 8.0f, origin.y + size.y - barSize.y - 8.0f);
			}
			at.x = std::clamp(at.x, origin.x + 60.0f, std::max(origin.x + 60.0f, origin.x + size.x - barSize.x - 8.0f));
			at.y = std::clamp(at.y, origin.y + 8.0f, std::max(origin.y + 8.0f, origin.y + size.y - barSize.y - 8.0f));
			if (beginPlate("##selbar", at, barSize))
			{
				if (anyCards)
				{
					ImU32 fill = 0;
					if (const Node* first = board->findNode(s_sel[0]))
					{
						ImVec4 tint{};
						if (colourOf(first->color, tint)) fill = Theme::u32(tint);
					}
					if (iconButton("Color", Icon::Colour, false, true, "Planner/Card color", false, fill)) ImGui::OpenPopup("##colours");
					services.note("Color");
					if (ImGui::BeginPopup("##colours"))
					{
						for (int i = -1; i < static_cast<int>(std::size(kSwatches)); ++i)
						{
							const std::string word = i < 0 ? std::string{} : kSwatches[i].word;
							if (i >= 0) ImGui::SameLine(0, 4.0f);
							ImGui::PushID(i);
							if (ImGui::ColorButton("##swatch", i < 0 ? Theme::warm(0.20f) : kSwatches[i].colour,
							                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoDragDrop,
							                       ImVec2(22, 22)))
							{
								setColour(boardId, word);
								ImGui::CloseCurrentPopup();
							}
							EditorWidgets::helpForKey("Planner/Card color");
							services.note("swatch:" + (word.empty() ? std::string("none") : word));
							ImGui::PopID();
						}
						ImGui::EndPopup();
					}
					ImGui::SameLine();
					if (iconButton("Duplicate", Icon::Duplicate, false, true, "Planner/Duplicate")) duplicateSelection(boardId);
					services.note("Duplicate");
					if (s_sel.size() >= 2)
					{
						ImGui::SameLine();
						if (iconButton("Align", Icon::Align, false, true, "Planner/Align")) ImGui::OpenPopup("##align");
						services.note("Align");
						if (ImGui::BeginPopup("##align"))
						{
							if (EditorWidgets::selectable("Align Left"))       alignSelection(boardId, Board::Align::Left);
							services.note("Align Left");
							if (EditorWidgets::selectable("Align Center"))     alignSelection(boardId, Board::Align::CentreH);
							services.note("Align Center");
							if (EditorWidgets::selectable("Align Right"))      alignSelection(boardId, Board::Align::Right);
							services.note("Align Right");
							if (EditorWidgets::selectable("Align Top"))        alignSelection(boardId, Board::Align::Top);
							services.note("Align Top");
							if (EditorWidgets::selectable("Align Middle"))     alignSelection(boardId, Board::Align::CentreV);
							services.note("Align Middle");
							if (EditorWidgets::selectable("Align Bottom"))     alignSelection(boardId, Board::Align::Bottom);
							services.note("Align Bottom");
							ImGui::BeginDisabled(s_sel.size() < 3);
							if (EditorWidgets::selectable("Distribute Horizontally")) distributeSelection(boardId, true);
							services.note("Distribute Horizontally");
							if (EditorWidgets::selectable("Distribute Vertically"))   distributeSelection(boardId, false);
							services.note("Distribute Vertically");
							ImGui::EndDisabled();
							ImGui::EndPopup();
						}
					}
					iconSeparator();
				}
				if (iconButton("Delete", Icon::Trash, false, true, "Planner/Delete", true)) deleteSelection(boardId);
				services.note("Delete");
			}
			endPlate();
		}
	}

	// ── Keys ──
	if ((hovered || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) && !io.WantTextInput && !editing)
	{
		const bool mod = io.KeyCtrl || io.KeySuper;
		if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) deleteSelection(boardId);
		if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { s_sel.clear(); s_selEdge = UUID{}; }
		if (mod && ImGui::IsKeyPressed(ImGuiKey_A))
		{
			s_sel.clear();
			for (const Node& n : board->nodes) s_sel.push_back(n.id);
			s_selEdge = UUID{};
		}
		if (mod && ImGui::IsKeyPressed(ImGuiKey_Z)) undoOrRedo(boardId, !io.KeyShift);
		if (mod && ImGui::IsKeyPressed(ImGuiKey_Y)) undoOrRedo(boardId, false);
		if (mod && ImGui::IsKeyPressed(ImGuiKey_C)) copySelection(boardId);
		if (mod && ImGui::IsKeyPressed(ImGuiKey_X)) { copySelection(boardId); deleteSelection(boardId); }
		if (mod && ImGui::IsKeyPressed(ImGuiKey_D)) duplicateSelection(boardId);
		if (mod && ImGui::IsKeyPressed(ImGuiKey_V))
		{
			// Where the cursor is when it is over the canvas, else the middle of what is shown.
			double px = view.x + size.x * 0.5 / view.zoom, py = view.y + size.y * 0.5 / view.zoom;
			if (hovered) Xf{ origin, view }.toWorld(io.MousePos, px, py);
			paste(boardId, px, py);
		}
	}

	dl->PopClipRect();
	ImGui::EndChild();
	setView(boardId, view);
}

} // namespace PlannerBoardView
