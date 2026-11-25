#include "PlannerPanel.h"

#include "EditorApplication.h"   // AppContext
#include "EditorHelp.h"
#include "EditorInput.h"       // the trackpad pinch on the timeline
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "NotificationStore.h"
#include "Planner.h"
#include "PlannerBoard.h"
#include "PlannerBoardView.h"
#include "PlannerExport.h"
#include "PlannerIcons.h"
#include "PlannerImport.h"
#include "PlannerTasks.h"
#include "PlannerWorkspace.h"
#include "ProjectManager.h"
#include "TaskBoard.h"
#include "TaskBoardPanel.h"
#include "TaskBoardTeam.h"

#include "TextEditor.h"   // ImGuiColorTextEdit: code blocks with syntax highlighting

#include <SDL3/SDL.h>   // the clipboard
#include <imgui.h>
#include <imgui_internal.h>          // the text field's cursor, for the arrow keys between blocks
#include <misc/cpp/imgui_stdlib.h>   // InputText over std::string

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace HE;
using namespace HE::Planner;
namespace Theme = HE::Ed::Theme;

namespace PlannerPanel
{

namespace
{

// Every control below is drawn inside render(), which pushes the help scope
//     Help::Scope helpScope("Planner")
// further down the file than the helpers that draw them. The line above is for
// scripts/editor_help_audit.py, which reads the file top to bottom and has to
// know the scope before it meets the first label.

Workspace s_ws;
UUID      s_dbId, s_viewId;
ViewKind  s_newKind = ViewKind::Table;   // what the "new ... of" popup is making
ImVec2    s_flyAt(0, 0);         // where the details flyover opens: beside the last click
UUID      s_flyRow;              // the row the flyover was last placed for
UUID      s_deleteData;          // the data a "Delete Data" confirmation is about
bool      s_deleteDataOpen = false;
std::string s_search;

// The text field being typed in: which cell, and what is in it so far. A cell
// shows the stored value until it is activated, then this, and writes when it
// is left.
struct EditKey
{
	UUID row, prop;
	bool operator==(const EditKey& o) const { return row == o.row && prop == o.prop; }
};
EditKey     s_editKey;
bool        s_editing = false;
bool        s_mineOnly = false;   // "Assigned to me": only rows whose first Person property names us
std::string s_editBuf;

std::string s_newOption, s_newProperty, s_renameView;
int         s_newPropertyType = 0;
UUID        s_armedDelete;        // the property whose "Delete Property" was pressed once
UUID        s_renamingView;
UUID        s_newRelationTarget;   // the database a new Relation property points at; {} = this one
bool        s_roomyCells = false;  // cells drawn with room below them (details, a row's page): relations list their rows as links

// The context of the frame being drawn: the chips, drops and jumps of the links
// need the world, the content manager and the selection.
AppContext* s_ctx = nullptr;
// The row whose details are open on the right; none when {}.
UUID        s_selRow;
std::string s_comment;
UUID        s_commentFor;

// The page being edited instead of a database: a row's page (its id is the row's,
// s_pageDb the database) or a standalone one. {} = a database is shown.
UUID        s_pageId, s_pageDb;
UUID        s_focusBlock;    // the block whose text field gets the keyboard next frame
UUID        s_deletePage;    // the page a "Delete Page" confirmation is about
bool        s_deletePageOpen = false;
UUID        s_boardId;           // the whiteboard shown instead of a database or a page; {} = none
UUID        s_deleteBoard;       // the board a "Delete Board" confirmation is about
bool        s_deleteBoardOpen = false;
int         s_cursorAt = -1;         // with s_cursorBlock: the byte the cursor goes to (a merge joins there), -1 = start/end
UUID        s_cursorBlock;           // the block whose cursor is placed as soon as its field is active
bool        s_cursorAtEnd = false;
bool        s_focusAtEnd = false;   // the cursor goes to the end of the block that gets the keyboard (moving up), else to its start
// What has been typed into blocks and not yet committed, by block: the field a
// click or an arrow key moves away from still has to be committed after the field
// it moves to has started, so one shared buffer would lose the first one's text.
std::unordered_map<UUID, std::string> s_blockEdits;

void openPage(const UUID& rowId, const UUID& dbId)
{
	s_ws.openRowPage(rowId, dbId);
	s_pageId = rowId;
	s_pageDb = dbId;
	s_boardId = UUID{};   // a whiteboard shows before a page: leave it, or the page never appears
}

std::vector<std::function<void()>> s_posted;
void post(std::function<void()> fn) { s_posted.push_back(std::move(fn)); }

DrawStats                             s_stats;
std::unordered_map<std::string, Rect> s_rects;

// Remember where the item just submitted is, under a name a test can ask for.
void note(const std::string& name)
{
	const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
	s_rects[name] = { a.x, a.y, b.x, b.y };
}

// The left button was just let go over this item and had not been dragged: a click, not
// the end of a drag. (A drag ends on the same frame, over whatever is under the cursor.)
bool releasedWithoutDrag()
{
	const ImGuiIO& io = ImGui::GetIO();
	return ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
	       io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] <= io.MouseDragThreshold * io.MouseDragThreshold;
}

// A small square button with a glyph drawn on it (the editor has no icon font),
// explained by the help entry `helpKey`. True when clicked.
bool iconButton(const char* id, const char* helpKey, const std::function<void(ImDrawList*, ImVec2, float, ImU32)>& glyph)
{
	const float side = ImGui::GetFrameHeight();
	const bool clicked = ImGui::InvisibleButton(id, ImVec2(side, side));
	const bool hovered = ImGui::IsItemHovered();
	EditorWidgets::helpForKey(helpKey);
	const ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(a, z, ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ButtonActive
	                                           : hovered              ? ImGuiCol_ButtonHovered
	                                                                  : ImGuiCol_Button), 4.0f);
	glyph(dl, ImVec2((a.x + z.x) * 0.5f, (a.y + z.y) * 0.5f), side * 0.27f, Theme::u32(Theme::Text));
	return clicked;
}

// Keeps the current window inside [lo, hi]. With multi-viewport on, a window that
// does not fit the editor window becomes an OS window of its own and ends up
// behind the dock or the editor; one that fits stays part of the editor.
void keepInside(ImVec2 lo, ImVec2 hi)
{
	const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
	const ImVec2 want(std::clamp(pos.x, lo.x, std::max(lo.x, hi.x - size.x)),
	                  std::clamp(pos.y, lo.y, std::max(lo.y, hi.y - size.y)));
	if (want.x != pos.x || want.y != pos.y) ImGui::SetWindowPos(want);
}

// The panel's popups: never taller than the editor window (the rest scrolls), never past its edges.
void capPopupHeight()
{
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, std::max(120.0f, vp->WorkSize.y - 16.0f)));
}
bool beginPopup(const char* id, ImGuiWindowFlags flags = 0)
{
	capPopupHeight();
	if (!ImGui::BeginPopup(id, flags)) return false;
	EditorWidgets::clampCurrentWindowToEditorWindow();
	return true;
}
bool beginPopupContextItem(const char* id)
{
	capPopupHeight();
	if (!ImGui::BeginPopupContextItem(id)) return false;
	EditorWidgets::clampCurrentWindowToEditorWindow();
	return true;
}

// ── The panel's own controls: rows with a glyph, tabs, tool buttons ──────────
// A row of the sidebar: glyph, name, a bar at the left while it is open.
bool sidebarRow(const char* id, PlannerIcons::Icon icon, const std::string& label, bool selected)
{
	constexpr float h = 26.0f;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	const bool clicked = ImGui::Selectable(id, selected, 0, ImVec2(0, h));
	ImDrawList* dl = ImGui::GetWindowDrawList();
	if (selected) dl->AddRectFilled(ImVec2(p.x, p.y + 5), ImVec2(p.x + 3, p.y + h - 5), Theme::u32(Theme::Accent), 2.0f);
	PlannerIcons::drawIcon(dl, icon, ImVec2(p.x + 16, p.y + h * 0.5f), Theme::u32(selected ? Theme::AccentBright : Theme::TextDim));
	dl->PushClipRect(p, ImVec2(p.x + w - 4, p.y + h), true);
	dl->AddText(ImVec2(p.x + 32, p.y + (h - ImGui::GetFontSize()) * 0.5f), Theme::u32(selected ? Theme::TextHeading : Theme::Text), label.c_str());
	dl->PopClipRect();
	return clicked;
}

// A section title with a "+" at its right edge. Sections are set apart by air and a hairline above the
// title, so where one kind of thing ends and the next begins reads at a glance.
bool sidebarSection(const char* title, const char* newName, const char* helpKey)
{
	ImGui::Dummy(ImVec2(0, 8));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), Theme::u32(Theme::warm(0.22f)), 1.0f);
	ImGui::Dummy(ImVec2(0, 6));
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", title);
	ImGui::SameLine(w - 20);
	const bool clicked = PlannerIcons::iconButton(newName, PlannerIcons::Icon::Plus, false, true, helpKey, false, 0, ImVec2(24, 24));
	note(newName);
	return clicked;
}

// What an empty section says under its title.
void sidebarEmpty(const char* text)
{
	ImGui::Indent(12.0f);
	ImGui::TextDisabled("%s", text);
	ImGui::Unindent(12.0f);
}

// A button with a glyph and a name, for the toolbars. `active` while a filter or the like is on.
bool toolButton(const char* helpKey, const char* label, PlannerIcons::Icon icon, bool active)
{
	const float textW = ImGui::CalcTextSize(label).x;
	const ImVec2 size(textW + 40.0f, ImGui::GetFrameHeight());
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::PushID(label);
	const bool clicked = ImGui::InvisibleButton("##tool", size);
	const bool hov = ImGui::IsItemHovered();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y),
	                  Theme::u32(active ? Theme::alpha(Theme::Accent, hov ? 0.34f : 0.24f) : Theme::warm(hov ? 0.28f : 0.18f)), 7.0f);
	const ImU32 ink = Theme::u32(active ? Theme::AccentBright : Theme::Text);
	PlannerIcons::drawIcon(dl, icon, ImVec2(p.x + 15, p.y + size.y * 0.5f), ink);
	dl->AddText(ImVec2(p.x + 28, p.y + (size.y - ImGui::GetFontSize()) * 0.5f), ink, label);
	EditorWidgets::helpForKey(helpKey);
	ImGui::PopID();
	return clicked;
}

float toolWidth(const char* label) { return ImGui::CalcTextSize(label).x + 40.0f; }

// After SameLine(): moves the cursor so that the next `w` pixels end at the right edge. Stays put when there is no room.
void alignRight(float w)
{
	const float avail = ImGui::GetContentRegionAvail().x;
	if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
}

PlannerIcons::Icon viewIcon(ViewKind k)
{
	switch (k)
	{
	case ViewKind::Board:    return PlannerIcons::Icon::Kanban;
	case ViewKind::Timeline: return PlannerIcons::Icon::Timeline;
	case ViewKind::Calendar: return PlannerIcons::Icon::Calendar;
	default:                 return PlannerIcons::Icon::Table;
	}
}

// The view a data set opens on: the Task Board's tasks are shown by status (the board), everything else on its first.
const View* preferredView(const Database& db)
{
	if (db.id == tasksDatabaseId())
		for (const View& v : db.views)
			if (v.kind == ViewKind::Board) return &v;
	return db.views.empty() ? nullptr : &db.views.front();
}

// Tasks is one thing: one sidebar entry, named Tasks, opening its preferred view; its other views (the table, the
// board) are tabs away (the + of the tab bar). It cannot be deleted.
bool hiddenInSidebar(const Database& db, const View& v)
{
	if (db.id != tasksDatabaseId()) return false;
	const View* pref = preferredView(db);
	return !pref || !(pref->id == v.id);
}

// The circular arrow of "read it again".
void refreshGlyph(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
	const float a0 = 0.7f, a1 = 5.3f;
	dl->PathArcTo(c, r, a0, a1);
	dl->PathStroke(col, 0, 1.8f);
	const ImVec2 tip(c.x + r * std::cos(a1), c.y + r * std::sin(a1));
	// An arrowhead at the open end, pointing along the arc.
	const float tx = -std::sin(a1), ty = std::cos(a1);
	const float nx = std::cos(a1), ny = std::sin(a1);
	const float h = r * 0.9f;
	dl->AddTriangleFilled(ImVec2(tip.x + tx * h, tip.y + ty * h),
	                      ImVec2(tip.x - nx * h * 0.7f, tip.y - ny * h * 0.7f),
	                      ImVec2(tip.x + nx * h * 0.7f, tip.y + ny * h * 0.7f), col);
}

const char* const kTypeNames[] = { "Text", "Number", "Checkbox", "Date", "Select", "Multi-select",
                                   "Person", "Relation", "Engine link", "URL" };
constexpr int kTypeCount = 10;   // one per PropertyType before Other

const char* const kFilterOps[] = { "equals", "not-equals", "contains", "not-contains", "is-empty",
                                   "is-not-empty", "gt", "gte", "lt", "lte" };
const char* const kFilterOpLabels[] = { "is", "is not", "contains", "does not contain", "is empty",
                                        "is not empty", "is above", "is at least", "is below", "is at most" };

void pushUuid(const UUID& id)
{
	ImGui::PushID(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(id.hi * 31u ^ id.lo)));
}

ImVec4 colorOf(const std::string& word)
{
	if (word == "blue")   return ImVec4(0.40f, 0.60f, 0.95f, 1);
	if (word == "green")  return Theme::Ok;
	if (word == "red")    return Theme::Bad;
	if (word == "orange") return Theme::Warn;
	if (word == "yellow") return Theme::AccentBright;
	if (word == "purple") return ImVec4(0.70f, 0.52f, 0.90f, 1);
	if (word == "pink")   return ImVec4(0.92f, 0.52f, 0.72f, 1);
	return ImVec4(0.62f, 0.60f, 0.57f, 1);   // gray, and anything unknown
}

// A coloured dot, then the name — one option as it reads in a cell or a menu.
void optionLabel(const SelectOption& o)
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float  h = ImGui::GetTextLineHeight();
	ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + h * 0.4f, p.y + h * 0.5f), h * 0.28f,
	                                            Theme::u32(colorOf(o.color)));
	ImGui::Dummy(ImVec2(h * 0.9f, h));
	ImGui::SameLine(0, 0);
	ImGui::TextUnformatted(o.name.c_str());
}

// A menu row for one option: the whole row is the click target, the dot and the
// name are drawn over it.
bool optionRow(const SelectOption& o, bool selected, ImGuiSelectableFlags flags = 0)
{
	const ImVec2 at = ImGui::GetCursorScreenPos();
	const bool clicked = ImGui::Selectable("##opt", selected, flags);
	ImGui::SetCursorScreenPos(at);
	optionLabel(o);
	return clicked;
}

std::string numberText(double v)
{
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.10g", v);
	return buf;
}

std::string dateText(const Database& db, const UUID& row, const UUID& prop)
{
	std::int64_t ms = 0;
	if (!db.date(row, prop, ms)) return {};
	return taskTimeToIso(ms).substr(0, 10);   // "2026-10-14"
}

// "2026-10-14" (or a full ISO time) to milliseconds.
bool parseDay(const std::string& text, std::int64_t& ms)
{
	constexpr std::string_view kMidnight = "T00:00:00.000Z";
	if (text.size() == 10)
	{
		char full[10 + kMidnight.size()];
		std::memcpy(full, text.data(), 10);
		std::memcpy(full + 10, kMidnight.data(), kMidnight.size());
		return taskTimeFromIso(std::string_view(full, sizeof full), ms);
	}
	return taskTimeFromIso(text, ms);
}

std::string joinNames(const std::vector<std::string>& names)
{
	std::string out;
	for (const std::string& n : names)
	{
		if (!out.empty()) out += ", ";
		out += n;
	}
	return out;
}

std::vector<std::string> splitNames(const std::string& text)
{
	std::vector<std::string> out;
	std::string cur;
	cur.reserve(text.size());
	auto flush = [&] {
		const auto b = cur.find_first_not_of(' ');
		const auto e = cur.find_last_not_of(' ');
		if (b != std::string::npos) out.push_back(cur.substr(b, e - b + 1));
		cur.clear();
	};
	for (const char c : text)
	{
		if (c == ',') flush();
		else          cur += c;
	}
	flush();
	return out;
}

// A text field that writes only when it is left. `shown` is what the stored
// value looks like; `commit` gets the new text. Returns true when it committed.
bool textCell(const char* id, const EditKey& key, const std::string& shown,
              const std::function<void(const std::string&)>& commit, const char* hint = nullptr,
              float multilineHeight = 0.0f, float width = -FLT_MIN)
{
	const bool mine = s_editing && s_editKey == key;
	std::string text = mine ? s_editBuf : shown;
	ImGui::SetNextItemWidth(width);
	const bool typed = multilineHeight > 0.0f ? ImGui::InputTextMultiline(id, &text, ImVec2(-FLT_MIN, multilineHeight))
	                 : hint                   ? ImGui::InputTextWithHint(id, hint, &text)
	                                          : ImGui::InputText(id, &text);
	if (ImGui::IsItemActivated()) { s_editing = true; s_editKey = key; s_editBuf = shown; }
	if (typed) { s_editing = true; s_editKey = key; s_editBuf = text; }
	if (ImGui::IsItemDeactivatedAfterEdit())
	{
		const std::string value = s_editBuf;
		s_editing = false;
		commit(value);
		return true;
	}
	if (ImGui::IsItemDeactivated()) s_editing = false;
	return false;
}

// ── Cells ────────────────────────────────────────────────────────────────────

// ── Engine links ─────────────────────────────────────────────────────────────

// The links of a row, with the mark that the target is gone. That mark is
// runtime state of the Task Board's own links and not part of the database.
std::vector<TaskLink> linksOf(const Database& db, const UUID& rowId, const UUID& propId)
{
	std::vector<TaskLink> links = db.links(rowId, propId);
	if (s_ws.isExternal(db.id) && s_ctx)
		if (TaskBoard* b = TaskBoardPanel::board(*s_ctx))
			if (const Task* t = b->find(rowId))
				for (TaskLink& l : links)
					for (const TaskLink& o : t->links)
						if (o.sameTarget(l)) { l.orphaned = o.orphaned; break; }
	return links;
}

// Adds the links the row does not have yet.
void addLinks(const UUID& dbId, const UUID& rowId, const UUID& propId, std::vector<TaskLink> added)
{
	if (added.empty()) return;
	post([=] {
		s_ws.edit(dbId, [&](Database& d) {
			std::vector<TaskLink> all = d.links(rowId, propId);
			bool any = false;
			for (const TaskLink& l : added)
				if (std::none_of(all.begin(), all.end(), [&](const TaskLink& o) { return o.sameTarget(l); }))
				{
					all.push_back(l);
					any = true;
				}
			return any && d.setLinks(rowId, propId, all, taskNowMs());
		});
	});
}

void removeLinkFrom(const UUID& dbId, const UUID& rowId, const UUID& propId, const TaskLink& link)
{
	post([=] {
		s_ws.edit(dbId, [&](Database& d) {
			std::vector<TaskLink> all = d.links(rowId, propId);
			const auto it = std::find_if(all.begin(), all.end(), [&](const TaskLink& o) { return o.sameTarget(link); });
			if (it == all.end()) return false;
			all.erase(it);
			return d.setLinks(rowId, propId, all, taskNowMs());
		});
	});
}

// The chips in a table cell, on one line, and the cell as the drop target for an
// asset, a scene or an entity. What does not fit is cut off.
void linksCell(const Database& db, const Row& row, const Property& p)
{
	AppContext& ctx = *s_ctx;
	const UUID dbId = db.id, rowId = row.id, propId = p.id;
	const std::vector<TaskLink> links = linksOf(db, rowId, propId);

	const ImVec2 at = ImGui::GetCursorScreenPos();
	const float  w  = ImGui::GetContentRegionAvail().x;
	const float  fh = ImGui::GetFrameHeight();
	const float  h  = std::floor(fh * 0.9f);
	ImGui::SetNextItemAllowOverlap();   // the chips sit on top of the drop zone and take the clicks
	ImGui::InvisibleButton("##linkdrop", ImVec2(w, fh));
	note("links:" + row.title);
	addLinks(dbId, rowId, propId, TaskBoardPanel::acceptDroppedLinks(ctx));
	if (ImGui::GetDragDropPayload())
		ImGui::GetWindowDrawList()->AddRect(at, ImVec2(at.x + w, at.y + fh), Theme::u32(Theme::AccentBright), 4.0f);

	if (links.empty())
	{
		ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + 4.0f, at.y + (fh - ImGui::GetTextLineHeight()) * 0.5f),
		                                    Theme::u32(Theme::TextDim), "Drop an asset, scene or entity");
		return;
	}
	ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + (fh - h) * 0.5f));
	float used = 0.0f;
	for (std::size_t i = 0; i < links.size(); ++i)
	{
		const float want = TaskBoardPanel::linkChipWidth(links[i], 170.0f, h);
		if (used + want > w) break;
		if (i > 0) ImGui::SameLine(0, 4.0f);
		if (TaskBoardPanel::linkChip(ctx, links[i], static_cast<int>(i), 170.0f, h))
			TaskBoardPanel::jumpToLink(ctx, links[i]);
		note("chip:" + row.title + ":" + std::to_string(i));
		used += want + 4.0f;
	}
}

std::string lowerCase(std::string s)
{
	for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

// ── Assignees: from the team's roster, with a team ───────────────────────────
// Without a team server there is nobody to pick from: the Tasks cell is then a
// text field like any Person property (a name typed by hand reaches nobody, it is
// only kept on the board).
void assigneesCell(const Database& db, const Row& row, const Property& p)
{
	const UUID dbId = db.id, rowId = row.id, propId = p.id;
	const std::vector<std::string> have = db.people(rowId, propId);
	HE::TaskBoardTeam* team = TaskBoardPanel::team();
	if (!team || !team->connected()) return;   // the caller types names instead
	std::string preview = joinNames(have);
	if (preview.empty()) preview = "Nobody";
	ImGui::SetNextItemWidth(-FLT_MIN);
	capPopupHeight();
	const bool open = ImGui::BeginCombo("##assignedTo", preview.c_str());
	note("assignees:" + row.title);
	if (!open) return;
	// Typing narrows the team down; Enter takes the first one left.
	static std::string s_who;
	if (ImGui::IsWindowAppearing())
	{
		s_who.clear();
		ImGui::SetKeyboardFocusHere();
	}
	ImGui::SetNextItemWidth(-FLT_MIN);
	const bool enter = ImGui::InputTextWithHint("##who", "Search the team", &s_who, ImGuiInputTextFlags_EnterReturnsTrue);
	note("assignees:search");
	const std::string self = team->self();
	std::string label;   // one buffer for every member row
	bool first = true;
	for (const HE::TaskTeamMember& m : team->members())
	{
		if (!s_who.empty() && lowerCase(m.name).find(lowerCase(s_who)) == std::string::npos) continue;
		const bool on = std::any_of(have.begin(), have.end(),
		                            [&](const std::string& a) { return HE::sameTaskMember(a, m.name); });
		label.assign(m.name);
		if (HE::sameTaskMember(m.name, self)) label += "  (you)";
		if (!m.online) label += "  (offline)";
		label += "##member";
		ImGui::PushID(m.name.c_str());
		// Stays open: assigning two people is two ticks, not two trips.
		const bool picked = ImGui::Selectable(label.c_str(), on, ImGuiSelectableFlags_NoAutoClosePopups) || (enter && first);
		first = false;
		note("member:" + m.name);
		if (picked)
		{
			std::vector<std::string> next;
			for (const std::string& a : have)
				if (!HE::sameTaskMember(a, m.name)) next.push_back(a);
			if (!on) next.push_back(m.name);
			post([=] {
				s_ws.edit(dbId, [&](Database& d) { return d.setPeople(rowId, propId, next, taskNowMs()); });
			});
		}
		ImGui::PopID();
	}
	ImGui::EndCombo();
}

// ── Relations: rows of another database (or this one) ───────────────────────

// A combo that lists the target database's rows; a click ticks or unticks one.
void relationCell(const Database& db, const Row& row, const Property& p)
{
	const UUID dbId = db.id, rowId = row.id, propId = p.id;
	const Database* target = (p.relationDb == UUID{} || p.relationDb == db.id) ? &db : s_ws.find(p.relationDb);
	const std::vector<UUID> have = db.relation(rowId, propId);
	auto titleOf = [](const Row& r) { return r.title.empty() ? std::string("Untitled") : r.title; };

	std::string preview;
	for (const UUID& id : have)
	{
		const Row* r = target ? target->findRow(id) : nullptr;
		preview += (preview.empty() ? "" : ", ") + (r ? titleOf(*r) : std::string("?"));
	}
	ImGui::SetNextItemWidth(-FLT_MIN);
	const bool open = ImGui::BeginCombo("##relation", preview.c_str());
	note("relation:" + row.title);
	if (open)
	{
	if (!target)
		ImGui::TextDisabled("The database this points at is gone.");
	else if (target->rows.size() <= (target == &db ? 1u : 0u))
		ImGui::TextDisabled("There are no rows to point at yet.");
	else
		for (const Row& t : target->rows)
		{
			if (target == &db && t.id == rowId) continue;   // a row does not point at itself
			pushUuid(t.id);
			const bool on = std::find(have.begin(), have.end(), t.id) != have.end();
			if (ImGui::Selectable(titleOf(t).c_str(), on, ImGuiSelectableFlags_NoAutoClosePopups))
			{
				std::vector<UUID> next = have;
				const UUID tid = t.id;
				if (on) next.erase(std::remove(next.begin(), next.end(), tid), next.end());
				else    next.push_back(tid);
				post([=] {
					s_ws.edit(dbId, [&](Database& d) { return d.setRelation(rowId, propId, next, taskNowMs()); });
				});
			}
			note("relrow:" + t.title);
			ImGui::PopID();
		}
	ImGui::EndCombo();
	}

	// Where there is room (the details, a row's page), the linked rows are links that go there.
	if (s_roomyCells && target)
		for (const UUID& id : have)
		{
			const Row* r = target->findRow(id);
			if (!r) continue;
			pushUuid(id);
			ImGui::PushStyleColor(ImGuiCol_Text, Theme::AccentHi);
			const bool clicked = ImGui::Selectable((titleOf(*r) + "##goto").c_str(), false);
			ImGui::PopStyleColor();
			note("relation-open:" + r->title);
			if (clicked)
			{
				const UUID targetDb = target->id, targetRow = id;
				post([=] {
					s_pageId = s_pageDb = UUID{};
					s_dbId = targetDb;
					s_viewId = UUID{};
					s_selRow = targetRow;
				});
			}
			ImGui::PopID();
		}
}

void editCell(const Database& db, const Row& row, const Property& p)
{
	const UUID dbId = db.id, rowId = row.id, propId = p.id;
	auto edit = [dbId](std::function<bool(Database&)> fn) {
		post([dbId, fn = std::move(fn)] { s_ws.edit(dbId, fn); });
	};
	const EditKey key{ rowId, propId };

	switch (p.type)
	{
	case PropertyType::Text:
	case PropertyType::Url:
		textCell("##cell", key, db.text(rowId, propId), [=](const std::string& v) {
			edit([=](Database& d) { return d.setText(rowId, propId, v, taskNowMs()); });
		});
		break;
	case PropertyType::Number:
		textCell("##cell", key, db.isEmpty(rowId, propId) ? std::string{} : numberText(db.number(rowId, propId)),
		         [=](const std::string& v) {
			edit([=](Database& d) {
				if (v.empty()) return d.clearCell(rowId, propId, taskNowMs());
				char* end = nullptr;
				const double n = std::strtod(v.c_str(), &end);
				if (end == v.c_str() || *end != '\0') return false;   // not a number: keep the old one
				return d.setNumber(rowId, propId, n, taskNowMs());
			});
		});
		break;
	case PropertyType::Checkbox:
	{
		bool on = db.checked(rowId, propId);
		if (EditorWidgets::checkbox("##cell", &on))
			edit([=](Database& d) { return d.setChecked(rowId, propId, on, taskNowMs()); });
		break;
	}
	case PropertyType::Date:
		textCell("##cell", key, dateText(db, rowId, propId), [=](const std::string& v) {
			edit([=](Database& d) {
				if (v.empty()) return d.clearCell(rowId, propId, taskNowMs());
				std::int64_t ms = 0;
				if (!parseDay(v, ms)) return false;   // not a date: keep the old one
				return d.setDate(rowId, propId, ms, taskNowMs());
			});
		}, "YYYY-MM-DD");
		break;
	case PropertyType::Person:
		// With a team signed in: pick members from its roster. Without one, names are typed
		// (kept on the board, announced to nobody).
		if (TaskBoardPanel::team() && TaskBoardPanel::team()->connected())
		{
			assigneesCell(db, row, p);
			break;
		}
		textCell("##cell", key, joinNames(db.people(rowId, propId)), [=](const std::string& v) {
			edit([=](Database& d) { return d.setPeople(rowId, propId, splitNames(v), taskNowMs()); });
		}, "Names, separated by commas");
		break;
	case PropertyType::Select:
	{
		const SelectOption* cur = p.findOption(db.select(rowId, propId));
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::BeginCombo("##cell", cur ? cur->name.c_str() : "", ImGuiComboFlags_None))
		{
			if (EditorWidgets::selectable("(none)", cur == nullptr))
				edit([=](Database& d) { return d.setSelect(rowId, propId, UUID{}, taskNowMs()); });
			for (const SelectOption& o : p.options)
			{
				ImGui::PushID(static_cast<int>(&o - p.options.data()));
				if (optionRow(o, cur == &o))
				{
					const UUID oid = o.id;
					edit([=](Database& d) { return d.setSelect(rowId, propId, oid, taskNowMs()); });
				}
				ImGui::PopID();
			}
			ImGui::Separator();
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::InputTextWithHint("##newoption", "New option", &s_newOption, ImGuiInputTextFlags_EnterReturnsTrue)
			    && !s_newOption.empty())
			{
				const std::string name = s_newOption;
				s_newOption.clear();
				edit([=](Database& d) {
					const SelectOption* o = d.addOption(propId, name);
					return o && d.setSelect(rowId, propId, o->id, taskNowMs());
				});
			}
			ImGui::EndCombo();
		}
		break;
	}
	case PropertyType::MultiSelect:
	{
		const std::vector<UUID> have = db.multiSelect(rowId, propId);
		std::string preview;
		for (const UUID& id : have)
			if (const SelectOption* o = p.findOption(id)) preview += (preview.empty() ? "" : ", ") + o->name;
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::BeginCombo("##cell", preview.c_str(), ImGuiComboFlags_None))
		{
			for (const SelectOption& o : p.options)
			{
				ImGui::PushID(static_cast<int>(&o - p.options.data()));
				const bool on = std::find(have.begin(), have.end(), o.id) != have.end();
				if (optionRow(o, on, ImGuiSelectableFlags_NoAutoClosePopups))
				{
					std::vector<UUID> next = have;
					const UUID oid = o.id;
					if (on) next.erase(std::remove(next.begin(), next.end(), oid), next.end());
					else    next.push_back(oid);
					edit([=](Database& d) { return d.setMultiSelect(rowId, propId, next, taskNowMs()); });
				}
				ImGui::PopID();
			}
			ImGui::Separator();
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::InputTextWithHint("##newoption", "New option", &s_newOption, ImGuiInputTextFlags_EnterReturnsTrue)
			    && !s_newOption.empty())
			{
				const std::string name = s_newOption;
				s_newOption.clear();
				std::vector<UUID> next = have;
				edit([=](Database& d) {
					const SelectOption* o = d.addOption(propId, name);
					if (!o) return false;
					std::vector<UUID> all = next;
					all.push_back(o->id);
					return d.setMultiSelect(rowId, propId, all, taskNowMs());
				});
			}
			ImGui::EndCombo();
		}
		break;
	}
	case PropertyType::Relation:
		relationCell(db, row, p);
		break;
	case PropertyType::EngineLink:
		linksCell(db, row, p);
		break;
	case PropertyType::Other:
		ImGui::TextDisabled("(%s: not supported by this editor)", p.typeOther.c_str());
		break;
	}
}

// ── Views ────────────────────────────────────────────────────────────────────

// Change one view of the open database; the closure gets the view looked up by id.
void editView(const UUID& dbId, const UUID& viewId, std::function<bool(View&)> fn)
{
	post([=] {
		s_ws.edit(dbId, [&](Database& d) {
			View* v = d.findView(viewId);
			return v && fn(*v);
		});
	});
}

// A labelled combo in a view's settings popup that picks the Date property a timeline or calendar
// runs from (isEnd: until, with a "(none)" entry).
void dateCombo(const Database& db, const UUID& dbId, const UUID& viewId, const char* label, const char* id,
               const char* none, const UUID& current, bool isEnd)
{
	ImGui::TextUnformatted(label);
	ImGui::SameLine(90);
	ImGui::SetNextItemWidth(160);
	const Property* cur = current == UUID{} ? nullptr : db.findProperty(current);
	if (!ImGui::BeginCombo(id, cur ? cur->name.c_str() : none)) return;
	if (isEnd && ImGui::Selectable("(none)", cur == nullptr))
		editView(dbId, viewId, [](View& v) { v.endProperty = UUID{}; return true; });
	for (const Property& p : db.properties)
		if (p.type == PropertyType::Date && ImGui::Selectable(p.name.c_str(), cur == &p))
		{
			const UUID pid = p.id;
			editView(dbId, viewId, [pid, isEnd](View& v) { (isEnd ? v.endProperty : v.dateProperty) = pid; return true; });
		}
	ImGui::EndCombo();
}

// ── Tables, timelines, boards and calendars are the things you make; the data behind them is shared ──
const char* kindWord(ViewKind k)
{
	switch (k)
	{
	case ViewKind::Board:    return "board";
	case ViewKind::Timeline: return "timeline";
	case ViewKind::Calendar: return "calendar";
	default:                 return "table";
	}
}

// A view of `kind` with what it needs picked from the data: a Select to group a board by, the Date
// properties a timeline runs between, the one a calendar is laid out by.
View* addConfiguredView(Database& d, ViewKind kind, const std::string& name)
{
	View* v = d.addView(name, kind);
	if (!v) return nullptr;
	if (kind == ViewKind::Board)
		for (const Property& p : d.properties)
			if (p.type == PropertyType::Select) { v->groupBy = p.id; break; }
	if (kind == ViewKind::Timeline)
	{
		int found = 0;
		for (const Property& p : d.properties)
			if (p.type == PropertyType::Date)
			{
				(found == 0 ? v->dateProperty : v->endProperty) = p.id;
				if (++found == 2) break;
			}
	}
	if (kind == ViewKind::Calendar)
		for (const Property& p : d.properties)
			if (p.type == PropertyType::Date) { v->dateProperty = p.id; break; }
	return v;
}

// Columns a fresh timeline, board or calendar needs to be of any use at once.
void prepareData(Database& d, ViewKind kind)
{
	if (kind == ViewKind::Board)
	{
		const UUID status = d.addProperty("Status", PropertyType::Select)->id;
		d.addOption(status, "To do", "gray");
		d.addOption(status, "Doing", "blue");
		d.addOption(status, "Done", "green");
	}
	if (kind == ViewKind::Timeline)
	{
		d.addProperty("Start", PropertyType::Date);
		d.addProperty("End", PropertyType::Date);
	}
	if (kind == ViewKind::Calendar) d.addProperty("Date", PropertyType::Date);
}

// Make a table, timeline, board or calendar and open it: on new data of its own, or on data that exists
// (then it is one more way of looking at the same rows).
void newView(ViewKind kind, const UUID& existing)
{
	post([=] {
		UUID db = existing, view;
		if (db == UUID{})
		{
			Database* made = s_ws.create(std::string("Untitled ") + kindWord(kind));
			if (!made) return;
			db = made->id;
			s_ws.edit(db, [&](Database& d) {
				prepareData(d, kind);
				View* nv = addConfiguredView(d, kind, d.name);
				if (!nv) return false;
				view = nv->id;
				std::vector<UUID> starting;   // the table view a new database begins with
				for (const View& v : d.views)
					if (!(v.id == view)) starting.push_back(v.id);
				for (const UUID& id : starting) d.removeView(id);
				return true;
			});
		}
		else
			s_ws.edit(db, [&](Database& d) {
				View* nv = addConfiguredView(d, kind, d.name + " " + kindWord(kind));
				if (!nv) return false;
				view = nv->id;
				return true;
			});
		s_dbId = db;
		s_viewId = view;
		s_pageId = s_pageDb = UUID{};
		s_boardId = UUID{};
	});
}

bool isHidden(const View& v, const UUID& prop)
{
	return std::find(v.hidden.begin(), v.hidden.end(), prop) != v.hidden.end();
}

// ── Filters ──────────────────────────────────────────────────────────────────

// What a filter's operand looks like in the file, from what was typed. Empty:
// not a value this property takes (the filter then says nothing and shows all).
std::string operandJson(const Property* p, const std::string& text)
{
	using nlohmann::json;
	if (p)
		switch (p->type)
		{
		case PropertyType::Number:
		{
			char* end = nullptr;
			const double n = std::strtod(text.c_str(), &end);
			return end != text.c_str() && *end == '\0' ? json(n).dump() : std::string{};
		}
		case PropertyType::Checkbox:
			return json(text == "true").dump();
		case PropertyType::Date:
		{
			std::int64_t ms = 0;
			return parseDay(text, ms) ? json(taskTimeToIso(ms)).dump() : std::string{};
		}
		default:
			break;
		}
	return json(text).dump();
}

std::string operandText(const Property* p, const Filter& f)
{
	if (f.valueJson.empty()) return {};
	const nlohmann::json v = nlohmann::json::parse(f.valueJson, nullptr, false);
	if (v.is_string())
	{
		const std::string s = v.get<std::string>();
		return p && p->type == PropertyType::Date && s.size() >= 10 ? s.substr(0, 10) : s;
	}
	if (v.is_number()) return numberText(v.get<double>());
	if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
	return {};
}

bool takesOperand(const std::string& op) { return op != "is-empty" && op != "is-not-empty"; }

void drawFilterPopup(const Database& db, const View& view)
{
	const UUID dbId = db.id, viewId = view.id;
	ImGui::TextDisabled("Show rows where all of these hold");
	for (std::size_t i = 0; i < view.filters.size(); ++i)
	{
		const Filter& f = view.filters[i];
		const Property* prop = f.property == UUID{} ? nullptr : db.findProperty(f.property);
		ImGui::PushID(static_cast<int>(i));

		ImGui::SetNextItemWidth(130);
		if (ImGui::BeginCombo("##fprop", prop ? prop->name.c_str() : "Title"))
		{
			if (ImGui::Selectable("Title", prop == nullptr))
				editView(dbId, viewId, [i](View& v) {
					if (i >= v.filters.size()) return false;
					v.filters[i] = { UUID{}, "contains", "" };
					return true;
				});
			for (const Property& p : db.properties)
				if (ImGui::Selectable(p.name.c_str(), prop == &p))
				{
					const UUID pid = p.id;
					editView(dbId, viewId, [i, pid](View& v) {
						if (i >= v.filters.size()) return false;
						v.filters[i] = { pid, "equals", "" };
						return true;
					});
				}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		int opIndex = 0;
		for (int k = 0; k < 10; ++k)
			if (f.op == kFilterOps[k]) opIndex = k;
		ImGui::SetNextItemWidth(130);
		if (ImGui::BeginCombo("##fop", kFilterOpLabels[opIndex]))
		{
			for (int k = 0; k < 10; ++k)
				if (ImGui::Selectable(kFilterOpLabels[k], k == opIndex))
					editView(dbId, viewId, [i, k](View& v) {
						if (i >= v.filters.size()) return false;
						v.filters[i].op = kFilterOps[k];
						return true;
					});
			ImGui::EndCombo();
		}
		if (takesOperand(f.op))
		{
			ImGui::SameLine();
			if (prop && (prop->type == PropertyType::Select || prop->type == PropertyType::MultiSelect))
			{
				ImGui::SetNextItemWidth(150);
				if (ImGui::BeginCombo("##fvalue", operandText(prop, f).c_str()))
				{
					for (const SelectOption& o : prop->options)
						if (ImGui::Selectable(o.name.c_str(), operandText(prop, f) == o.name))
						{
							const std::string name = o.name;
							editView(dbId, viewId, [i, name](View& v) {
								if (i >= v.filters.size()) return false;
								v.filters[i].valueJson = nlohmann::json(name).dump();
								return true;
							});
						}
					ImGui::EndCombo();
				}
			}
			else if (prop && prop->type == PropertyType::Checkbox)
			{
				bool on = operandText(prop, f) == "true";
				if (EditorWidgets::checkbox("##fvalue", &on))
					editView(dbId, viewId, [i, on](View& v) {
						if (i >= v.filters.size()) return false;
						v.filters[i].valueJson = nlohmann::json(on).dump();
						return true;
					});
			}
			else
			{
				textCell("##fvalue", { viewId, UUID{ 0, i + 1 } }, operandText(prop, f), [=](const std::string& text) {
					const std::string json = operandJson(prop, text);
					editView(dbId, viewId, [i, json](View& v) {
						if (i >= v.filters.size()) return false;
						v.filters[i].valueJson = json;
						return true;
					});
				}, nullptr, 0.0f, 150.0f);
			}
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("x"))
			editView(dbId, viewId, [i](View& v) {
				if (i >= v.filters.size()) return false;
				v.filters.erase(v.filters.begin() + static_cast<std::ptrdiff_t>(i));
				return true;
			});
		ImGui::PopID();
	}
	if (EditorWidgets::button("Add Filter"))
		editView(dbId, viewId, [](View& v) { v.filters.push_back({ UUID{}, "contains", "" }); return true; });
}

// ── Properties ───────────────────────────────────────────────────────────────

void drawPropertiesPopup(const Database& db, const View& view)
{
	const UUID dbId = db.id, viewId = view.id;
	ImGui::TextDisabled("Columns of this database. The checkbox shows one in this view.");
	for (const Property& p : db.properties)
	{
		const UUID pid = p.id;
		pushUuid(pid);
		bool shown = !isHidden(view, pid);
		if (EditorWidgets::checkbox("##visible", &shown))
			editView(dbId, viewId, [pid, shown](View& v) {
				auto it = std::find(v.hidden.begin(), v.hidden.end(), pid);
				if (shown && it != v.hidden.end()) v.hidden.erase(it);
				else if (!shown && it == v.hidden.end()) v.hidden.push_back(pid);
				return true;
			});
		ImGui::SameLine();
		textCell("##pname", { dbId, pid }, p.name, [=](const std::string& name) {
			post([=] {
				s_ws.edit(dbId, [&](Database& d) {
					Property* prop = d.findProperty(pid);
					if (!prop || name.empty()) return false;
					prop->name = name;
					return true;
				});
			});
		}, nullptr, 0.0f, 160.0f);
		ImGui::SameLine();
		ImGui::TextDisabled("%s", p.type == PropertyType::Other ? p.typeOther.c_str()
		                                                        : kTypeNames[static_cast<int>(p.type)]);
		ImGui::SameLine();
		const bool armed = s_armedDelete == pid;
		ImGui::BeginDisabled(s_ws.isExternal(dbId));   // the Task Board's columns are fixed
		if (EditorWidgets::dangerSmallButton(armed ? "Really delete?" : "Delete Property"))
		{
			if (armed)
			{
				s_armedDelete = UUID{};
				post([=] { s_ws.edit(dbId, [&](Database& d) { return d.removeProperty(pid); }); });
			}
			else
				s_armedDelete = pid;
		}
		ImGui::EndDisabled();
		ImGui::PopID();
	}
	ImGui::Separator();
	ImGui::BeginDisabled(s_ws.isExternal(dbId));
	ImGui::SetNextItemWidth(150);
	ImGui::InputTextWithHint("##newprop", "New property", &s_newProperty);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120);
	if (ImGui::BeginCombo("##newtype", kTypeNames[s_newPropertyType]))
	{
		for (int i = 0; i < kTypeCount; ++i)
			if (ImGui::Selectable(kTypeNames[i], i == s_newPropertyType)) s_newPropertyType = i;
		ImGui::EndCombo();
	}
	if (s_newPropertyType == static_cast<int>(PropertyType::Relation))
	{
		// A relation points at the rows of a database: this one, or another.
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140);
		const Database* chosen = s_newRelationTarget == UUID{} ? nullptr : s_ws.find(s_newRelationTarget);
		if (ImGui::BeginCombo("##reltarget", chosen ? chosen->name.c_str() : "This database"))
		{
			if (ImGui::Selectable("This database", chosen == nullptr)) s_newRelationTarget = UUID{};
			for (const Database* other : s_ws.databases())
				if (other->id != dbId && ImGui::Selectable((other->name + "##target").c_str(), other == chosen))
					s_newRelationTarget = other->id;
			ImGui::EndCombo();
		}
	}
	ImGui::SameLine();
	if (EditorWidgets::button("Add Property") && !s_newProperty.empty())
	{
		const std::string name = s_newProperty;
		const PropertyType type = static_cast<PropertyType>(s_newPropertyType);
		const UUID target = s_newRelationTarget;
		s_newProperty.clear();
		post([=] {
			s_ws.edit(dbId, [&](Database& d) {
				Property* np = d.addProperty(name, type);
				if (np && type == PropertyType::Relation) np->relationDb = target == UUID{} ? d.id : target;
				return np != nullptr;
			});
		});
	}
	ImGui::EndDisabled();
	if (s_ws.isExternal(dbId)) ImGui::TextDisabled("The Tasks columns are fixed: the Task Board keeps them.");
}

// ── The table ────────────────────────────────────────────────────────────────

void cycleSort(const UUID& dbId, const UUID& viewId, const UUID& prop)
{
	editView(dbId, viewId, [prop](View& v) {
		if (v.sorts.empty() || !(v.sorts[0].property == prop)) v.sorts = { { prop, false } };
		else if (!v.sorts[0].descending)                       v.sorts[0].descending = true;
		else                                                   v.sorts.clear();
		return true;
	});
}

void deleteRowMenu(const Database& db, const Row& row)
{
	if (beginPopupContextItem("##rowmenu"))
	{
		const UUID dbId = db.id, rowId = row.id;
		if (EditorWidgets::dangerMenuItem("Delete Row"))
			post([=] {
				s_ws.edit(dbId, [&](Database& d) { return d.removeRow(rowId); });
				s_ws.removePage(rowId);
			});
		ImGui::EndPopup();
	}
}

void drawTable(const Database& db, const View& view, const std::vector<std::size_t>& idx)
{
	std::vector<const Property*> cols;
	for (const Property& p : db.properties)
		if (!isHidden(view, p.id)) cols.push_back(&p);

	const float rowH = ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
	const ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
	                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
	                              ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingFixedFit;
	if (!ImGui::BeginTable("##grid", 1 + static_cast<int>(cols.size()), flags)) return;
	ImGui::TableSetupScrollFreeze(1, 1);
	ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthFixed, 280.0f);
	for (const Property* p : cols) ImGui::TableSetupColumn(p->name.c_str(), ImGuiTableColumnFlags_WidthFixed, 170.0f);

	// The header is drawn by hand: a click on a name sorts by it.
	ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
	for (int c = 0; c <= static_cast<int>(cols.size()); ++c)
	{
		ImGui::TableSetColumnIndex(c);
		ImGui::PushID(c);
		const bool isTitle = c == 0;
		const Property* col = isTitle ? nullptr : cols[static_cast<std::size_t>(c - 1)];
		const UUID prop = col ? col->id : UUID{};
		const char* name = col ? col->name.c_str() : "Title";
		const bool sorted = !view.sorts.empty() && view.sorts[0].property == prop;
		if (ImGui::Selectable(name, false, 0, ImVec2(0, 0)))
			cycleSort(db.id, view.id, prop);
		if (isTitle) note("header:Title"); else note("header:" + col->name);
		if (sorted)
		{
			const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
			const float h = (b.y - a.y) * 0.22f, cx = b.x - h * 2.0f, cy = (a.y + b.y) * 0.5f;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImU32 col = Theme::u32(Theme::AccentHi);
			if (view.sorts[0].descending) dl->AddTriangleFilled({ cx - h, cy - h * 0.6f }, { cx + h, cy - h * 0.6f }, { cx, cy + h * 0.8f }, col);
			else                          dl->AddTriangleFilled({ cx - h, cy + h * 0.6f }, { cx + h, cy + h * 0.6f }, { cx, cy - h * 0.8f }, col);
		}
		ImGui::PopID();
	}

	// Only the rows on screen are laid out; every row is one frame high.
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(idx.size()), rowH);
	while (clipper.Step())
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
		{
			const Row& row = db.rows[idx[static_cast<std::size_t>(i)]];
			ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
			++s_stats.rowsDrawn;
			pushUuid(row.id);

			ImGui::TableSetColumnIndex(0);
			const UUID dbId = db.id, rowId = row.id;
			if (ImGui::SmallButton("Open")) s_selRow = rowId;
			note("open:" + row.title);
			ImGui::SameLine();
			textCell("##title", { rowId, UUID{} }, row.title, [=](const std::string& title) {
				post([=] { s_ws.edit(dbId, [&](Database& d) { return d.setTitle(rowId, title, taskNowMs()); }); });
			}, "Untitled");
			note("title:" + row.title);
			deleteRowMenu(db, row);

			for (std::size_t c = 0; c < cols.size(); ++c)
			{
				ImGui::TableSetColumnIndex(static_cast<int>(c) + 1);
				pushUuid(cols[c]->id);
				editCell(db, row, *cols[c]);
				if (i == clipper.DisplayStart) note("cell:" + cols[c]->name);
				ImGui::PopID();
			}
			ImGui::PopID();
		}
	ImGui::EndTable();
}

// ── The board ────────────────────────────────────────────────────────────────

std::string summary(const Database& db, const Row& row, const Property& p)
{
	switch (p.type)
	{
	case PropertyType::Text:
	case PropertyType::Url:         return db.text(row.id, p.id);
	case PropertyType::Number:      return db.isEmpty(row.id, p.id) ? std::string{} : numberText(db.number(row.id, p.id));
	case PropertyType::Checkbox:    return db.checked(row.id, p.id) ? "yes" : "";
	case PropertyType::Date:        return dateText(db, row.id, p.id);
	case PropertyType::Person:      return joinNames(db.people(row.id, p.id));
	case PropertyType::Select:
	{
		const SelectOption* o = p.findOption(db.select(row.id, p.id));
		return o ? o->name : std::string{};
	}
	case PropertyType::MultiSelect:
	{
		std::string out;
		for (const UUID& id : db.multiSelect(row.id, p.id))
		{
			const SelectOption* o = p.findOption(id);
			if (!o) continue;
			if (!out.empty()) out += ", ";
			out += o->name;
		}
		return out;
	}
	default:
		return {};
	}
}

void dropOnLane(const UUID& dbId, const UUID& groupProp, const UUID& option)
{
	if (!ImGui::BeginDragDropTarget()) return;
	if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("HE_PLANNER_ROW"))
		if (pl->DataSize == static_cast<int>(sizeof(UUID)))
		{
			const UUID rowId = *static_cast<const UUID*>(pl->Data);
			post([=] {
				s_ws.edit(dbId, [&](Database& d) {
					const UUID cur = d.select(rowId, groupProp);
					if (cur == option) return false;
					return d.setSelect(rowId, groupProp, option, taskNowMs());
				});
			});
		}
	ImGui::EndDragDropTarget();
}

void drawBoard(const Database& db, const View& view)
{
	const Property* group = db.findProperty(view.groupBy);
	if (!group || group->type != PropertyType::Select)
	{
		ImGui::TextDisabled("Pick a Select property to make the lanes from.");
		ImGui::SetNextItemWidth(200);
		if (ImGui::BeginCombo("##groupby", "Group by"))
		{
			for (const Property& p : db.properties)
				if (p.type == PropertyType::Select && ImGui::Selectable(p.name.c_str()))
				{
					const UUID pid = p.id;
					editView(db.id, view.id, [pid](View& v) { v.groupBy = pid; return true; });
				}
			ImGui::EndCombo();
		}
		return;
	}

	const UUID dbId = db.id, groupId = group->id;
	// Lanes: the options in the order the property lists them, "No value" first only while a row has none
	// (the Task Board's tasks always have a status, so it never shows there).
	std::vector<UUID> lanes;
	if (dbId != tasksDatabaseId() && !db.column(view, UUID{}).empty()) lanes.push_back(UUID{});
	for (const SelectOption& o : group->options) lanes.push_back(o.id);

	const float cardH = ImGui::GetTextLineHeight() * 2.0f + ImGui::GetStyle().FramePadding.y * 4.0f;
	// The lanes share the whole width; only when that would make them narrower than a card the board scrolls sideways.
	constexpr float kMinLaneW = 200.0f;
	const float innerW = std::max(ImGui::GetContentRegionAvail().x, kMinLaneW * static_cast<float>(lanes.size()));
	if (!ImGui::BeginTable("##lanes", static_cast<int>(lanes.size()),
	                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable |
	                       ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchSame,
	                       ImVec2(0, 0), innerW))
		return;
	ImGui::TableSetupScrollFreeze(0, 1);
	for (const UUID& lane : lanes)
	{
		const SelectOption* o = group->findOption(lane);
		ImGui::TableSetupColumn(o ? o->name.c_str() : "No value", ImGuiTableColumnFlags_WidthStretch);
	}
	ImGui::TableHeadersRow();
	ImGui::TableNextRow();
	for (std::size_t c = 0; c < lanes.size(); ++c)
	{
		ImGui::TableSetColumnIndex(static_cast<int>(c));
		ImGui::PushID(static_cast<int>(c));
		const UUID lane = lanes[c];
		const std::vector<std::size_t> inLane = db.column(view, lane);

		if (ImGui::SmallButton("+"))
			post([=] {
				s_ws.edit(dbId, [&](Database& d) {
					const Row* r = d.addRow("", s_ws.author(), taskNowMs());
					return r && (lane == UUID{} || d.setSelect(r->id, groupId, lane, taskNowMs()));
				});
			});
		ImGui::SameLine();
		ImGui::TextDisabled("%d", static_cast<int>(inLane.size()));
		dropOnLane(dbId, groupId, lane);

		for (const std::size_t ri : inLane)
		{
			const Row& row = db.rows[ri];
			pushUuid(row.id);
			const ImVec2 p0 = ImGui::GetCursorScreenPos();
			const float  w  = ImGui::GetContentRegionAvail().x;
			ImDrawList*  dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + cardH), Theme::u32(Theme::warm(0.17f)), 4.0f);
			if (ImGui::Selectable("##card", row.id == s_selRow, 0, ImVec2(w, cardH))) s_selRow = row.id;
			note("card:" + row.title);
			if (ImGui::BeginDragDropSource())
			{
				ImGui::SetDragDropPayload("HE_PLANNER_ROW", &row.id, sizeof(UUID));
				ImGui::TextUnformatted(row.title.empty() ? "Untitled" : row.title.c_str());
				ImGui::EndDragDropSource();
			}
			dropOnLane(dbId, groupId, lane);
			deleteRowMenu(db, row);

			// Title, then the first two things worth saying about the row.
			const float pad = ImGui::GetStyle().FramePadding.x + 2.0f;
			dl->PushClipRect(p0, ImVec2(p0.x + w, p0.y + cardH), true);
			dl->AddText(ImVec2(p0.x + pad, p0.y + ImGui::GetStyle().FramePadding.y + 1.0f),
			            Theme::u32(row.title.empty() ? Theme::TextDim : Theme::Text),
			            row.title.empty() ? "Untitled" : row.title.c_str());
			std::string line;
			int shown = 0;
			for (const Property& p : db.properties)
			{
				if (p.id == groupId || isHidden(view, p.id) || shown >= 2) continue;
				const std::string s = summary(db, row, p);
				if (s.empty()) continue;
				line += (line.empty() ? "" : "  ") + s;
				++shown;
			}
			dl->AddText(ImVec2(p0.x + pad, p0.y + cardH * 0.5f + 2.0f), Theme::u32(Theme::TextDim), line.c_str());
			dl->PopClipRect();
			ImGui::PopID();
			ImGui::Dummy(ImVec2(0, 4));
		}
		// Room to drop a card into an empty lane.
		ImGui::InvisibleButton("##drop", ImVec2(-FLT_MIN, std::max(60.0f, ImGui::GetContentRegionAvail().y)));
		{
			const SelectOption* laneOption = group->findOption(lane);
			note("lane:" + (laneOption ? laneOption->name : std::string("No value")));
		}
		dropOnLane(dbId, groupId, lane);
		ImGui::PopID();
	}
	ImGui::EndTable();
}

// ── The details of one row ───────────────────────────────────────────────────

// The links of a row with room to work: each chip with its x, a drop zone that
// says what it takes, and the three ways to link what is selected.
void linksEditor(const Database& db, const Row& row, const Property& p)
{
	AppContext& ctx = *s_ctx;
	const UUID dbId = db.id, rowId = row.id, propId = p.id;
	const std::vector<TaskLink> links = linksOf(db, rowId, propId);

	// Chips flow left to right and wrap; each with its x to unlink.
	const float chipH = std::floor(ImGui::GetFrameHeight() * 0.9f);
	const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
	const float x0 = ImGui::GetCursorScreenPos().x;
	const float rmW = ImGui::GetFrameHeight();
	for (std::size_t i = 0; i < links.size(); ++i)
	{
		const float maxW = std::max(chipH * 2.0f, std::min(220.0f, right - x0 - rmW - 2.0f));
		const float want = TaskBoardPanel::linkChipWidth(links[i], maxW, chipH);
		if (i > 0 && ImGui::GetItemRectMax().x + 8.0f + want + 2.0f + rmW <= right) ImGui::SameLine(0, 8.0f);
		if (TaskBoardPanel::linkChip(ctx, links[i], static_cast<int>(i), maxW, chipH))
			TaskBoardPanel::jumpToLink(ctx, links[i]);
		note("drawer-chip:" + std::to_string(i));
		ImGui::SameLine(0, 2.0f);
		ImGui::PushID(static_cast<int>(i));
		if (ImGui::SmallButton("x")) removeLinkFrom(dbId, rowId, propId, links[i]);
		ImGui::PopID();
	}
	// The drop zone, spelled out: a target that says what it takes is what makes
	// the gesture findable.
	{
		const ImVec2 at = ImGui::GetCursorScreenPos();
		const float  w  = ImGui::GetContentRegionAvail().x;
		const float  h  = ImGui::GetFrameHeight() * 1.4f;
		ImGui::Dummy(ImVec2(w, h));
		addLinks(dbId, rowId, propId, TaskBoardPanel::acceptDroppedLinks(ctx));
		note("drawer-drop");
		const bool dragging = ImGui::GetDragDropPayload() != nullptr;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRect(at, ImVec2(at.x + w, at.y + h),
		            Theme::u32(dragging ? Theme::AccentBright : Theme::TextDim), 4.0f, 0, dragging ? 2.0f : 1.0f);
		const char* hint = "Drop an asset, a scene or an entity here";
		const ImVec2 hs = ImGui::CalcTextSize(hint);
		dl->PushClipRect(at, ImVec2(at.x + w, at.y + h), true);
		dl->AddText(ImVec2(at.x + std::max(6.0f, (w - hs.x) * 0.5f), at.y + (h - hs.y) * 0.5f),
		            ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
		dl->PopClipRect();
	}
	ImGui::BeginDisabled(!(ctx.world && !ctx.selection.empty()));
	if (EditorWidgets::button("Link Selection"))
		addLinks(dbId, rowId, propId, TaskBoardPanel::selectionLinks(ctx));
	ImGui::EndDisabled();
	TaskLink sceneLink;
	const bool haveScene = TaskBoardPanel::currentSceneLink(ctx, sceneLink);
	ImGui::SameLine();
	ImGui::BeginDisabled(!haveScene);
	if (EditorWidgets::button("Link Scene")) addLinks(dbId, rowId, propId, { sceneLink });
	ImGui::EndDisabled();
	// Link Panel on its own line: three buttons do not fit side by side in the details.
	if (EditorWidgets::button("Link Panel")) ImGui::OpenPopup("##linkPanel");
	if (beginPopup("##linkPanel"))
	{
		for (const char* name : TaskBoardPanel::linkablePanels())
			if (ImGui::Selectable(name, false)) addLinks(dbId, rowId, propId, { TaskLink::panelLink(name, name) });
		ImGui::EndPopup();
	}
}

// What happened to a task, and a box to say something about it. The history is
// the board's: it is not part of the database.
void taskHistory(const UUID& rowId)
{
	AppContext& ctx = *s_ctx;
	TaskBoard* b = TaskBoardPanel::board(ctx);
	const Task* t = b ? b->find(rowId) : nullptr;
	if (!t) return;
	if (s_commentFor != rowId) { s_commentFor = rowId; s_comment.clear(); }

	EditorWidgets::subHeading("History");
	ImGui::TextDisabled("Created by %s, %s", t->createdBy.empty() ? "someone" : t->createdBy.c_str(),
	                    taskTimeToIso(t->createdMs).substr(0, 16).c_str());
	const std::size_t shown = std::min<std::size_t>(t->updates.size(), 6);
	{
		EditorWidgets::WrapText wrap;
		for (std::size_t i = t->updates.size() - shown; i < t->updates.size(); ++i)
		{
			const TaskUpdate& u = t->updates[i];
			std::string what;
			switch (u.kind)
			{
			case TaskUpdateKind::Status: what = "set it to " + u.status; break;
			case TaskUpdateKind::Assignment:
			{
				what = "assigned it to ";
				for (std::size_t k = 0; k < u.assignees.size(); ++k) what += (k ? ", " : "") + u.assignees[k];
				if (u.assignees.empty()) what = "unassigned it";
				break;
			}
			case TaskUpdateKind::Comment: what = "commented"; break;
			default:                      what = u.kindOther; break;
			}
			ImGui::TextDisabled("%s  %s %s", taskTimeToIso(u.timeMs).substr(0, 16).c_str(), u.author.c_str(), what.c_str());
			if (!u.text.empty()) { ImGui::Indent(); ImGui::TextUnformatted(u.text.c_str()); ImGui::Unindent(); }
		}
	}
	ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - 120.0f));
	ImGui::InputTextWithHint("##taskComment", "Write a comment", &s_comment);
	note("comment-field");
	ImGui::SameLine();
	ImGui::BeginDisabled(s_comment.empty());
	if (EditorWidgets::button("Add Comment"))
	{
		const std::string text = s_comment;
		s_comment.clear();
		post([=] {
			TaskBoardPanel::edit(*s_ctx, [&](TaskBoard& bb) {
				return bb.comment(rowId, TaskBoardPanel::author(), text, taskNowMs()) != nullptr;
			});
		});
	}
	note("Add Comment");
	ImGui::EndDisabled();
}

// The properties of a row as a two-column list: the name, then its value. Shared by
// the details and by the row's page.
void drawPropertyTable(const Database& db, const Row& row)
{
	const UUID dbId = db.id, rowId = row.id;
	const bool tasks = s_ws.isExternal(dbId);
	const UUID descriptionId = derivedId("property", "Description");
	if (db.properties.empty())
		ImGui::TextDisabled("No properties yet. Add some with Properties.");
	else if (ImGui::BeginTable("##props", 2, ImGuiTableFlags_NoSavedSettings))
	{
		ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed, 110.0f);
		ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
		for (const Property& p : db.properties)
		{
			pushUuid(p.id);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s", p.name.c_str());
			ImGui::TableSetColumnIndex(1);
			if (p.type == PropertyType::EngineLink)
				linksEditor(db, row, p);
			else if (tasks && p.id == descriptionId)
			{
				const UUID pid = p.id;
				textCell("##description", { rowId, pid }, db.text(rowId, pid), [=](const std::string& v) {
					post([=] { s_ws.edit(dbId, [&](Database& d) { return d.setText(rowId, pid, v, taskNowMs()); }); });
				}, nullptr, ImGui::GetTextLineHeight() * 4.0f);
				note("drawer:description");
			}
			else
			{
				s_roomyCells = true;
				editCell(db, row, p);
				s_roomyCells = false;
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

}

// `closeButton`: the docked column has no header of its own, so it carries the x; the flyover's header has one.
void drawDrawer(const Database& db, const Row& row, bool closeButton)
{
	const UUID dbId = db.id, rowId = row.id;
	const bool tasks = s_ws.isExternal(dbId);
	const UUID descriptionId = derivedId("property", "Description");
	pushUuid(rowId);

	// The title in the heading font, the way to close at its right edge.
	const float closeW = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
	if (s_ctx && s_ctx->fontHeading) ImGui::PushFont(s_ctx->fontHeading);
	textCell("##drawertitle", { rowId, UUID{ 0, 3 } }, row.title, [=](const std::string& title) {
		post([=] { s_ws.edit(dbId, [&](Database& d) { return d.setTitle(rowId, title, taskNowMs()); }); });
	}, "Untitled", 0.0f, -closeW);
	if (s_ctx && s_ctx->fontHeading) ImGui::PopFont();
	note("drawer:title");
	if (closeButton)
	{
		ImGui::SameLine();
		if (ImGui::SmallButton("x")) s_selRow = UUID{};
		note("drawer:close");
	}
	if (EditorWidgets::button("Open Page")) post([=] { openPage(rowId, dbId); });
	note("Open Page");
	ImGui::Spacing();

	drawPropertyTable(db, row);

	if (tasks) taskHistory(rowId);

	ImGui::Spacing();
	// The button says what the thing is in the view it was opened from: a step, an event, a card or a row.
	const View* openedIn = db.findView(s_viewId);
	const ViewKind openedKind = openedIn ? openedIn->kind : ViewKind::Table;
	bool askDelete = false;
	if (openedKind == ViewKind::Timeline)      askDelete = EditorWidgets::dangerButton("Delete Step");
	else if (openedKind == ViewKind::Calendar) askDelete = EditorWidgets::dangerButton("Delete Event");
	else if (openedKind == ViewKind::Board)    askDelete = EditorWidgets::dangerButton("Delete Card");
	else                                       askDelete = EditorWidgets::dangerButton("Delete Row");
	note("drawer:delete");
	if (askDelete) ImGui::OpenPopup("##confirmDeleteRow");
	if (beginPopup("##confirmDeleteRow"))
	{
		ImGui::Text("Delete \"%s\"?", row.title.empty() ? "Untitled" : row.title.c_str());
		ImGui::TextDisabled(tasks ? "It goes from the board and from Config/TaskBoard.json."
		                          : "It goes from the database and from any relation that pointed at it.");
		if (EditorWidgets::dangerButton("Delete"))
		{
			post([=] {
				s_ws.edit(dbId, [&](Database& d) { return d.removeRow(rowId); });
				s_ws.removePage(rowId);
			});
			s_selRow = UUID{};
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (EditorWidgets::cancelButton("Cancel")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	ImGui::PopID();
}

// ── The timeline ─────────────────────────────────────────────────────────────

constexpr std::int64_t kDayMs = 86400000;

std::int64_t dayOf(std::int64_t ms)
{
	return ms >= 0 ? ms / kDayMs : -((-ms + kDayMs - 1) / kDayMs);
}

// A new row dated from day `d0` to `d1` for the timeline or calendar view `viewId`. A view whose data has no Date
// column yet gets one (Start and End for a timeline, Date for a calendar), so the button always shows something.
// The new row is opened for a title (typed in place on a timeline, in the details otherwise).
void newDated(const UUID& dbId, const UUID& viewId, std::int64_t d0, std::int64_t d1)
{
	post([=] {
		UUID made;
		s_ws.edit(dbId, [&](Database& d) {
			View* v = d.findView(viewId);
			if (!v) return false;
			const bool timeline = v->kind == ViewKind::Timeline;
			auto isDate = [&](const UUID& id) { const Property* p = d.findProperty(id); return p && p->type == PropertyType::Date; };
			auto otherDate = [&](const UUID& not1) {
				for (const Property& p : d.properties)
					if (p.type == PropertyType::Date && !(p.id == not1)) return p.id;
				return UUID{};
			};
			if (!isDate(v->dateProperty))
			{
				const UUID found = otherDate(UUID{});
				v->dateProperty = found == UUID{} ? d.addProperty(timeline ? "Start" : "Date", PropertyType::Date)->id : found;
			}
			// A timeline always has an End; a calendar gets one when the event covers more than a day.
			if ((timeline || d1 > d0) && !isDate(v->endProperty))
			{
				const UUID found = otherDate(v->dateProperty);
				v->endProperty = found == UUID{} ? d.addProperty("End", PropertyType::Date)->id : found;
			}
			Row* r = d.addRow(timeline ? "New step" : "New event", s_ws.author(), taskNowMs());
			if (!r) return false;
			made = r->id;
			d.setDate(made, v->dateProperty, d0 * kDayMs, taskNowMs());
			if (d1 > d0 && isDate(v->endProperty)) d.setDate(made, v->endProperty, d1 * kDayMs, taskNowMs());
			return true;
		});
		if (!(made == UUID{})) s_selRow = made;   // its details open, to name it
	});
}

// ── The timeline: a time axis ────────────────────────────────────────────────
// Time runs left to right on one axis. An event on one day is a node (a diamond and its name), one with an End
// is a bar; events that would overlap sit in the next lane. The wheel (a trackpad's pinch) zooms at the cursor, a
// drag on the background pans, a drag on an event moves it and a drag on a bar's ends resizes it, a double-click
// on the axis makes an event there. Rows without a date wait in the tray under the axis: drag one onto the axis.

struct TlState { double left = 0; float scrollY = 0; bool placed = false; };   // per view: the day at the left edge
std::unordered_map<UUID, TlState> s_tlViews;
float  s_tlPpd = 24.0f;       // pixels per day: the zoom
double s_tlPending = 0;       // a zoom the toolbar asked for (pixels per day), applied around the middle
bool   s_tlGoToday = false, s_tlFit = false;
constexpr float kTlMinPpd = 1.5f, kTlMaxPpd = 240.0f, kTlLaneH = 34.0f, kTlRulerH = 46.0f;
const float kZoomDayW[]        = { 24.0f, 12.0f, 5.0f };
const char* const kZoomNames[] = { "Days", "Weeks", "Months" };

// What a drag on an event does: 1 moves it, 2 pulls its start, 3 pulls its end; by `days`.
struct DateDrag { UUID row; int mode = 0; int days = 0; };
DateDrag s_tlDrag;
DateDrag s_calDrag;

int weekdayOf(std::int64_t day) { return static_cast<int>(((day % 7) + 7 + 3) % 7); }   // 0 Monday ... 6 Sunday

// Move a row's dates (mode 1), or pull its Start (2) or End (3), by whole days. A row without an End gets one.
void shiftDates(const UUID& dbId, const UUID& rowId, const UUID& startId, const UUID& endId, int mode, int days)
{
	if (days == 0 || mode == 0) return;
	post([=] {
		s_ws.edit(dbId, [&](Database& d) {
			std::int64_t s = 0, e = 0;
			if (!d.date(rowId, startId, s)) return false;
			const bool hasEnd = !(endId == UUID{}) && d.date(rowId, endId, e);
			const std::int64_t shift = static_cast<std::int64_t>(days) * kDayMs, now = taskNowMs();
			bool any = false;
			if (mode == 1)
			{
				any |= d.setDate(rowId, startId, s + shift, now);
				if (hasEnd) any |= d.setDate(rowId, endId, e + shift, now);
			}
			else if (mode == 2)
			{
				const std::int64_t end = hasEnd ? e : s;
				any |= d.setDate(rowId, startId, std::min(s + shift, end), now);
				if (!hasEnd && !(endId == UUID{})) any |= d.setDate(rowId, endId, end, now);
			}
			else if (!(endId == UUID{}))
				any |= d.setDate(rowId, endId, std::max(s, (hasEnd ? e : s) + shift), now);
			return any;
		});
	});
}

const char* const kMonthsShort[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

void drawTimeline(const Database& db, const View& view, const std::vector<std::size_t>& idx)
{
	const Property* startP = db.findProperty(view.dateProperty);
	if (!startP || startP->type != PropertyType::Date)
	{
		ImGui::TextDisabled("This timeline has no dates yet. New Step adds Start and End columns and the first step.");
		return;
	}
	const Property* endP = view.endProperty == UUID{} ? nullptr : db.findProperty(view.endProperty);
	if (endP && endP->type != PropertyType::Date) endP = nullptr;
	const Property* colourP = db.findProperty(view.groupBy);
	if (colourP && colourP->type != PropertyType::Select) colourP = nullptr;
	const bool tasksData = db.id == tasksDatabaseId();

	struct Ev { std::size_t row; std::int64_t a, b; bool span; float labelW; int lane; };
	std::vector<Ev> evs;
	std::vector<std::size_t> undated;
	for (const std::size_t i : idx)
	{
		const Row& r = db.rows[i];
		std::int64_t s = 0, e = 0;
		if (!db.date(r.id, startP->id, s)) { undated.push_back(i); continue; }
		const std::int64_t a = dayOf(s);
		std::int64_t b = a;
		if (endP && db.date(r.id, endP->id, e)) b = std::max(a, dayOf(e));
		evs.push_back({ i, a, b, b > a, ImGui::CalcTextSize(r.title.empty() ? "Untitled" : r.title.c_str()).x, 0 });
	}
	const std::int64_t today = dayOf(taskNowMs());
	TlState& st = s_tlViews[view.id];
	if (!st.placed)
	{
		std::int64_t first = today;
		for (const Ev& e : evs) first = std::min(first, e.a);
		st.left = static_cast<double>(first) - 2.0;
		st.placed = true;
	}

	const float trayH = undated.empty() ? 0.0f : 46.0f;
	ImGui::BeginChild("##timeline", ImVec2(0, std::max(100.0f, ImGui::GetContentRegionAvail().y - trayH)), ImGuiChildFlags_Borders,
	                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImGuiIO& io = ImGui::GetIO();
	const ImVec2 origin = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
	const ImVec2 corner(origin.x + size.x, origin.y + size.y);

	// Zooms and jumps the toolbar asked for, around the middle of what is shown.
	if (s_tlPending > 0)
	{
		const double mid = st.left + size.x * 0.5 / s_tlPpd;
		s_tlPpd = static_cast<float>(std::clamp(s_tlPending, double(kTlMinPpd), double(kTlMaxPpd)));
		st.left = mid - size.x * 0.5 / s_tlPpd;
		s_tlPending = 0;
	}
	if (s_tlGoToday)
	{
		st.left = static_cast<double>(today) + 0.5 - size.x * 0.5 / s_tlPpd;
		s_tlGoToday = false;
	}
	if (s_tlFit)
	{
		s_tlFit = false;
		if (!evs.empty())
		{
			std::int64_t lo = evs.front().a, hi = evs.front().b;
			for (const Ev& e : evs) { lo = std::min(lo, e.a); hi = std::max(hi, e.b); }
			s_tlPpd = static_cast<float>(std::clamp((size.x - 120.0) / static_cast<double>(hi - lo + 2), double(kTlMinPpd), double(kTlMaxPpd)));
			st.left = static_cast<double>(lo) - 60.0 / s_tlPpd;
		}
	}

	// The background: pans when dragged, makes an event when double-clicked, takes a row dropped from the tray.
	ImGui::SetNextItemAllowOverlap();
	ImGui::InvisibleButton("##tlbg", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
	note("timeline:canvas");
	const bool bgHover = ImGui::IsItemHovered(), bgActive = ImGui::IsItemActive();
	auto dayAtX = [&](float x) { return static_cast<std::int64_t>(std::floor(st.left + (x - origin.x) / s_tlPpd)); };
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("HE_PLANNER_ROW"))
			if (pl->DataSize == static_cast<int>(sizeof(UUID)))
			{
				const UUID rowId = *static_cast<const UUID*>(pl->Data);
				const UUID dbId = db.id, sid = startP->id;
				const std::int64_t day = dayAtX(io.MousePos.x);
				post([=] {
					s_ws.edit(dbId, [&](Database& d) { return d.setDate(rowId, sid, day * kDayMs, taskNowMs()); });
				});
			}
		ImGui::EndDragDropTarget();
	}
	if (bgHover && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !tasksData)
		newDated(db.id, view.id, dayAtX(io.MousePos.x), dayAtX(io.MousePos.x));

	auto zoomAt = [&](float mx, double factor) {
		const double day = st.left + (mx - origin.x) / s_tlPpd;
		s_tlPpd = static_cast<float>(std::clamp(s_tlPpd * factor, double(kTlMinPpd), double(kTlMaxPpd)));
		st.left = day - (mx - origin.x) / s_tlPpd;
	};
	if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && s_tlDrag.mode == 0)
	{
		if (const float pinch = EditorInput::pinchDelta(); pinch != 0.0f) zoomAt(io.MousePos.x, std::max(0.1, 1.0 + pinch * 2.0));
		if (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)
		{
			if (!EditorInput::trackpadActive() || io.KeyCtrl || io.KeySuper) zoomAt(io.MousePos.x, std::pow(1.1, static_cast<double>(io.MouseWheel)));
			else { st.left -= io.MouseWheelH * 30.0 / s_tlPpd; st.scrollY -= io.MouseWheel * 30.0f; }
		}
	}
	if (bgActive)
	{
		st.left -= io.MouseDelta.x / s_tlPpd;
		st.scrollY -= io.MouseDelta.y;
	}
	const float ppd = s_tlPpd;
	auto X = [&](double day) { return origin.x + static_cast<float>((day - st.left) * ppd); };

	// Lanes: events that would overlap (a node counts with its name) go to the next one.
	std::sort(evs.begin(), evs.end(), [](const Ev& x, const Ev& y) { return x.a != y.a ? x.a < y.a : x.b > y.b; });
	std::vector<double> laneEnd;
	for (Ev& e : evs)
	{
		const double x0 = static_cast<double>(e.a) * ppd;
		const double x1 = e.span ? static_cast<double>(e.b + 1) * ppd : (static_cast<double>(e.a) + 0.5) * ppd + 21.0 + e.labelW;
		std::size_t l = 0;
		while (l < laneEnd.size() && laneEnd[l] + 6.0 > x0) ++l;
		if (l == laneEnd.size()) laneEnd.push_back(x1);
		laneEnd[l] = std::max(laneEnd[l], x1);
		e.lane = static_cast<int>(l);
	}
	const float lanesTop = origin.y + kTlRulerH;
	const float maxScroll = std::max(0.0f, static_cast<float>(laneEnd.size()) * kTlLaneH + 24.0f + kTlRulerH - size.y);
	st.scrollY = std::clamp(st.scrollY, 0.0f, maxScroll);

	dl->PushClipRect(origin, corner, true);
	const std::int64_t d0 = static_cast<std::int64_t>(std::floor(st.left)) - 1;
	const std::int64_t d1 = static_cast<std::int64_t>(std::ceil(st.left + size.x / ppd)) + 1;

	// Weekends and today in the lanes.
	if (ppd >= 12.0f)
		for (std::int64_t d = d0; d <= d1; ++d)
			if (weekdayOf(d) >= 5) dl->AddRectFilled(ImVec2(X(d), lanesTop), ImVec2(X(d + 1), corner.y), Theme::u32(Theme::warm(0.075f)));
	dl->AddRectFilled(ImVec2(X(today), origin.y), ImVec2(X(today + 1), corner.y), Theme::u32(Theme::alpha(Theme::Warn, 0.09f)));

	// The ruler: months (years when zoomed far out) above, the days (weeks, months) below.
	dl->AddRectFilled(origin, ImVec2(corner.x, lanesTop), Theme::u32(Theme::warm(0.10f)));
	const ImU32 gridCol = Theme::u32(Theme::warm(0.13f)), tickCol = Theme::u32(Theme::warm(0.40f));
	if (ppd >= 3.5f)
	{
		int step = 14;
		for (const int s : { 1, 2, 7, 14 }) if (static_cast<float>(s) * ppd >= 28.0f) { step = s; break; }
		for (std::int64_t d = d0; d <= d1; ++d)
		{
			const bool tick = step <= 2 ? (d % step == 0) : (weekdayOf(d) == 0 && (step == 7 || ((d + 3) / 7) % 2 == 0));
			const std::string iso = taskTimeToIso(d * kDayMs);
			const float x = X(d);
			if (tick)
			{
				dl->AddLine(ImVec2(x, lanesTop - 12.0f), ImVec2(x, lanesTop), tickCol);
				dl->AddLine(ImVec2(x, lanesTop), ImVec2(x, corner.y), gridCol);
				static const char* const kWd[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
				char label[16];
				if (ppd >= 56.0f) std::snprintf(label, sizeof label, "%s %.2s", kWd[weekdayOf(d)], iso.c_str() + 8);
				else              std::snprintf(label, sizeof label, "%.2s", iso.c_str() + 8);
				dl->AddText(ImVec2(x + 3.0f, lanesTop - 28.0f), Theme::u32(d == today ? Theme::Warn : Theme::TextDim), label);
			}
			if (iso.compare(8, 2, "01") == 0 || d == d0)
			{
				dl->AddLine(ImVec2(x, origin.y), ImVec2(x, lanesTop), tickCol);
				if (iso.compare(8, 2, "01") == 0) dl->AddLine(ImVec2(x, lanesTop), ImVec2(x, corner.y), Theme::u32(Theme::warm(0.24f)));
				const int mon = std::clamp(std::atoi(iso.substr(5, 2).c_str()), 1, 12);
				char monthLabel[16];
				std::snprintf(monthLabel, sizeof monthLabel, "%s %.4s", kMonthsShort[mon - 1], iso.c_str());
				dl->AddText(ImVec2(std::max(x, origin.x) + 4.0f, origin.y + 3.0f), Theme::u32(Theme::TextHeading), monthLabel);
			}
		}
	}
	else
	{
		for (std::int64_t d = d0; d <= d1; ++d)
		{
			const std::string iso = taskTimeToIso(d * kDayMs);
			if (iso.compare(8, 2, "01") != 0) continue;
			const float x = X(d);
			const int mon = std::clamp(std::atoi(iso.substr(5, 2).c_str()), 1, 12);
			dl->AddLine(ImVec2(x, lanesTop - 14.0f), ImVec2(x, corner.y), mon == 1 ? Theme::u32(Theme::warm(0.28f)) : gridCol);
			dl->AddText(ImVec2(x + 3.0f, lanesTop - 28.0f), Theme::u32(Theme::TextDim), kMonthsShort[mon - 1]);
			if (mon == 1)
			{
				dl->AddLine(ImVec2(x, origin.y), ImVec2(x, lanesTop), tickCol);
				dl->AddText(ImVec2(x + 4.0f, origin.y + 3.0f), Theme::u32(Theme::TextHeading), iso.substr(0, 4).c_str());
			}
		}
	}
	dl->AddLine(ImVec2(X(today), origin.y), ImVec2(X(today), corner.y), Theme::u32(Theme::alpha(Theme::Warn, 0.7f)), 1.5f);
	dl->AddLine(ImVec2(origin.x, lanesTop), ImVec2(corner.x, lanesTop), Theme::u32(Theme::warm(0.30f)));
	dl->PopClipRect();

	// The events.
	const float laneTop = lanesTop + 12.0f - st.scrollY;
	dl->PushClipRect(ImVec2(origin.x, lanesTop), corner, true);
	for (const Ev& e : evs)
	{
		const Row& row = db.rows[e.row];
		const std::string title = row.title.empty() ? std::string("Untitled") : row.title;
		const bool dragging = s_tlDrag.row == row.id && s_tlDrag.mode != 0;
		std::int64_t a = e.a, b = e.b;
		if (dragging)
		{
			if (s_tlDrag.mode == 1) { a += s_tlDrag.days; b += s_tlDrag.days; }
			else if (s_tlDrag.mode == 2) a = std::min(a + s_tlDrag.days, b);
			else b = std::max(b + s_tlDrag.days, a);
		}
		const bool asSpan = b > a;
		const float y = laneTop + static_cast<float>(e.lane) * kTlLaneH;
		if (y + kTlLaneH < lanesTop || y > corner.y) continue;
		ImVec4 colour = Theme::Accent;
		if (colourP)
			if (const SelectOption* o = colourP->findOption(db.select(row.id, colourP->id))) colour = colorOf(o->color);
		const bool selected = row.id == s_selRow;

		ImVec2 hitA, hitB;
		float bx0 = 0, bx1 = 0;
		if (asSpan)
		{
			bx0 = X(a) + 1.0f;
			bx1 = std::max(bx0 + 12.0f, X(b + 1) - 1.0f);
			hitA = ImVec2(bx0, y + 4.0f);
			hitB = ImVec2(bx1, y + kTlLaneH - 6.0f);
		}
		else
		{
			const float cx = X(static_cast<double>(a) + 0.5);
			hitA = ImVec2(cx - 10.0f, y + 4.0f);
			hitB = ImVec2(cx + 20.0f + e.labelW, y + kTlLaneH - 6.0f);
		}
		if (hitB.x < origin.x || hitA.x > corner.x) continue;
		++s_stats.rowsDrawn;
		pushUuid(row.id);
		ImGui::SetCursorScreenPos(hitA);
		ImGui::SetNextItemAllowOverlap();   // a bar's ends are handles of their own, on top
		ImGui::InvisibleButton("##ev", ImVec2(hitB.x - hitA.x, hitB.y - hitA.y));
		note("bar:" + row.title);
		const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
		if (hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		if (releasedWithoutDrag()) s_selRow = row.id;
		if (act && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f))
			s_tlDrag = { row.id, 1, static_cast<int>(std::lround(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x / ppd)) };
		if (ImGui::IsItemDeactivated() && s_tlDrag.row == row.id && s_tlDrag.mode == 1)
		{
			shiftDates(db.id, row.id, startP->id, endP ? endP->id : UUID{}, 1, s_tlDrag.days);
			s_tlDrag = DateDrag{};
		}

		if (asSpan)
		{
			dl->AddRectFilled(hitA, hitB, Theme::u32(Theme::alpha(colour, hov || act ? 0.97f : 0.82f)), 6.0f);
			if (selected) dl->AddRect(hitA, hitB, Theme::u32(Theme::AccentBright), 6.0f, 0, 2.0f);
			dl->PushClipRect(ImVec2(hitA.x + 4.0f, hitA.y), ImVec2(hitB.x - 4.0f, hitB.y), true);
			dl->AddText(ImVec2(std::max(hitA.x, origin.x) + 8.0f, hitA.y + (hitB.y - hitA.y - ImGui::GetTextLineHeight()) * 0.5f),
			            Theme::u32(Theme::Ink), title.c_str());
			dl->PopClipRect();
			// The ends, to pull: only a bar with an End column can have them.
			if (endP && !tasksData)
			{
				const float hw = std::min(9.0f, (bx1 - bx0) * 0.3f);
				for (int side = 0; side < 2; ++side)
				{
					ImGui::SetCursorScreenPos(ImVec2(side == 0 ? bx0 : bx1 - hw, hitA.y));
					ImGui::InvisibleButton(side == 0 ? "##rs" : "##re", ImVec2(hw, hitB.y - hitA.y));
					note(std::string(side == 0 ? "bar-start:" : "bar-end:") + row.title);
					if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
					if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f))
						s_tlDrag = { row.id, side == 0 ? 2 : 3, static_cast<int>(std::lround(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x / ppd)) };
					if (ImGui::IsItemDeactivated() && s_tlDrag.row == row.id && s_tlDrag.mode == (side == 0 ? 2 : 3))
					{
						shiftDates(db.id, row.id, startP->id, endP->id, s_tlDrag.mode, s_tlDrag.days);
						s_tlDrag = DateDrag{};
					}
					if (ImGui::IsItemHovered() || ImGui::IsItemActive())
					{
						const float gx = side == 0 ? bx0 + 3.0f : bx1 - 3.0f;
						dl->AddLine(ImVec2(gx, hitA.y + 6.0f), ImVec2(gx, hitB.y - 6.0f), Theme::u32(Theme::Ink), 2.0f);
					}
				}
			}
		}
		else
		{
			const float cx = X(static_cast<double>(a) + 0.5), cy = y + kTlLaneH * 0.5f - 1.0f;
			const ImVec2 dia[4] = { ImVec2(cx, cy - 8.0f), ImVec2(cx + 8.0f, cy), ImVec2(cx, cy + 8.0f), ImVec2(cx - 8.0f, cy) };
			dl->AddConvexPolyFilled(dia, 4, Theme::u32(Theme::alpha(colour, hov || act ? 1.0f : 0.9f)));
			dl->AddPolyline(dia, 4, selected ? Theme::u32(Theme::AccentBright) : Theme::u32(Theme::warm(0.08f)), ImDrawFlags_Closed, selected ? 2.0f : 1.0f);
			dl->AddText(ImVec2(cx + 14.0f, cy - ImGui::GetTextLineHeight() * 0.5f), Theme::u32(selected ? Theme::AccentBright : Theme::Text), title.c_str());
		}
		ImGui::PopID();
	}
	dl->PopClipRect();
	if (evs.empty() && undated.empty())
		dl->AddText(ImVec2(origin.x + 20.0f, lanesTop + 20.0f), Theme::u32(Theme::TextDim), "Double-click the axis to make an event, or press New Step.");
	ImGui::EndChild();

	// Rows with no date: drag one onto the axis to place it, or click it to open it.
	if (!undated.empty())
	{
		ImGui::BeginChild("##tltray", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImDrawList* tdl = ImGui::GetWindowDrawList();
		const float ty = ImGui::GetCursorScreenPos().y;
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Not on the timeline yet:");
		for (const std::size_t ri : undated)
		{
			const Row& row = db.rows[ri];
			const std::string title = row.title.empty() ? std::string("Untitled") : row.title;
			++s_stats.rowsDrawn;
			ImGui::SameLine();
			pushUuid(row.id);
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const ImVec2 sz(ImGui::CalcTextSize(title.c_str()).x + 20.0f, ImGui::GetFrameHeight());
			ImGui::InvisibleButton("##chip", sz);
			note("undated:" + row.title);
			if (releasedWithoutDrag()) s_selRow = row.id;   // on release: opening on the press would put the details in the way of a drag
			if (ImGui::BeginDragDropSource())
			{
				ImGui::SetDragDropPayload("HE_PLANNER_ROW", &row.id, sizeof(UUID));
				ImGui::TextUnformatted(title.c_str());
				ImGui::EndDragDropSource();
			}
			tdl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), Theme::u32(Theme::warm(ImGui::IsItemHovered() ? 0.30f : 0.20f)), 7.0f);
			tdl->AddText(ImVec2(p.x + 10.0f, p.y + (sz.y - ImGui::GetFontSize()) * 0.5f), Theme::u32(Theme::Text), title.c_str());
			ImGui::PopID();
		}
		(void)ty;
		ImGui::EndChild();
	}
}

// ── Pages ────────────────────────────────────────────────────────────────────

struct BlockKind { BlockType type; const char* label; };
const BlockKind kBlockKinds[] = {
	{ BlockType::Paragraph, "Text" },          { BlockType::Heading1, "Heading 1" },
	{ BlockType::Heading2, "Heading 2" },      { BlockType::Heading3, "Heading 3" },
	{ BlockType::Bullet, "Bulleted list" },    { BlockType::Numbered, "Numbered list" },
	{ BlockType::Todo, "To-do" },              { BlockType::Quote, "Quote" },
	{ BlockType::Callout, "Callout" },         { BlockType::Code, "Code" },
	{ BlockType::Divider, "Divider" },         { BlockType::EngineLink, "Engine object" },
	{ BlockType::DatabaseView, "Database view" }, { BlockType::PageLink, "Page link" },
};

bool isListType(BlockType t)
{
	return t == BlockType::Bullet || t == BlockType::Numbered || t == BlockType::Todo;
}

// What a paragraph turns into when it starts like Markdown: "# ", "## ", "### ",
// "- ", "1. ", "[] ", "> ", "---" and three backticks. Applied the moment the text
// matches, while typing, not only when the block is left.
bool shortcutType(const std::string& text, BlockType& type, std::string& rest)
{
	struct Rule { const char* prefix; BlockType type; };
	static const Rule rules[] = {
		{ "### ", BlockType::Heading3 }, { "## ", BlockType::Heading2 }, { "# ", BlockType::Heading1 },
		{ "- ", BlockType::Bullet },     { "* ", BlockType::Bullet },    { "1. ", BlockType::Numbered },
		{ "[] ", BlockType::Todo },      { "[ ] ", BlockType::Todo },    { "> ", BlockType::Quote },
	};
	if (text == "---") { type = BlockType::Divider; rest.clear(); return true; }
	if (text == "```") { type = BlockType::Code;    rest.clear(); return true; }
	for (const Rule& r : rules)
		if (text.rfind(r.prefix, 0) == 0)
		{
			type = r.type;
			rest = text.substr(std::strlen(r.prefix));
			return true;
		}
	return false;
}

struct FlatBlock { const Block* block; int depth; int number; };

void flattenBlocks(const std::vector<Block>& list, int depth, std::vector<FlatBlock>& out)
{
	int number = 0;
	for (const Block& b : list)
	{
		number = b.type == BlockType::Numbered ? number + 1 : 0;
		out.push_back({ &b, depth, number });
		flattenBlocks(b.children, depth + 1, out);
	}
}

// Everything a block edit changes, in one saved step: the text (with a Markdown
// shortcut applied to a paragraph), and, on Enter, the block that follows.
std::string s_lastCodeLang;   // a new code block starts in the language of the last one picked

// Everything a block edit changes, in one saved step: the text (with a Markdown shortcut applied to a
// paragraph), and, on Enter, the block that follows, which takes `tail` (the text after the cursor).
void commitBlockText(const UUID& pageId, const UUID& blockId, std::string text, bool enter, std::string tail = {})
{
	post([=] {
		s_ws.editPage(pageId, [&](Page& page) {
			Block* b = page.findBlock(blockId);
			if (!b) return false;
			bool changed = false;
			std::string value = text;
			BlockType shortcut = BlockType::Paragraph;
			std::string rest;
			if (b->type == BlockType::Paragraph && shortcutType(value, shortcut, rest))
			{
				changed |= page.setBlockType(blockId, shortcut, taskNowMs());
				value = rest;
				if (shortcut == BlockType::Code) b->language = s_lastCodeLang;
				if (shortcut == BlockType::Divider && !enter)
				{
					// A divider takes no text: the writing goes on in a paragraph under it.
					if (Block* made = page.addBlockAfter(blockId, BlockType::Paragraph, "", taskNowMs())) s_focusBlock = made->id;
					return true;
				}
			}
			if (b->text != value) changed |= page.setBlockText(blockId, value, taskNowMs());
			if (enter)
			{
				const BlockType type = b->type;
				if (isListType(type) && value.empty() && tail.empty())
				{
					changed |= page.setBlockType(blockId, BlockType::Paragraph, taskNowMs());   // Enter on an empty item ends the list
					s_focusBlock = blockId;                                                      // and the cursor stays
				}
				else
				{
					Block* made = page.addBlockAfter(blockId, isListType(type) ? type : BlockType::Paragraph, tail, taskNowMs());
					if (made) { s_focusBlock = made->id; s_focusAtEnd = false; changed = true; }
				}
			}
			return changed;
		});
	});
}

// Backspace at the very start of a block: a list item, heading or quote becomes a paragraph first; a
// paragraph joins the text block above it, the cursor where the two meet.
void backspaceAtStart(const UUID& pageId, const UUID& blockId, const UUID& prevId, const std::string& text)
{
	post([=] {
		s_ws.editPage(pageId, [&](Page& page) {
			Block* b = page.findBlock(blockId);
			if (!b) return false;
			if (b->type != BlockType::Paragraph) return page.setBlockType(blockId, BlockType::Paragraph, taskNowMs());   // the field stays active
			Block* prev = prevId == UUID{} ? nullptr : page.findBlock(prevId);
			if (!prev || prev->type == BlockType::Code || !b->children.empty()) return false;
			const int join = static_cast<int>(prev->text.size());
			page.setBlockText(prevId, prev->text + text, taskNowMs());
			page.removeBlock(blockId, taskNowMs());
			s_focusBlock = prevId;
			s_cursorAt = join;
			return true;
		});
	});
}

// The block's text as a word-wrapped field that grows with what is typed. Writes when
// it is left, and on Enter (Ctrl/Shift+Enter is a new line). `prevId` is the block
// above, for Backspace on an empty one.
void blockText(const Page& page, const Block& b, int index, float width, const UUID& prevId, const UUID& nextId)
{
	const UUID pageId = page.id, blockId = b.id;
	const auto pending = s_blockEdits.find(blockId);
	const bool hadPending = pending != s_blockEdits.end();
	std::string text = hadPending ? pending->second : b.text;
	const std::string before = text;

	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 3.0f));
	ImFont* font = nullptr;
	if (s_ctx)
		font = b.type == BlockType::Heading1 ? s_ctx->fontHeading
		     : b.type == BlockType::Heading2 ? s_ctx->fontSubheading
		     : b.type == BlockType::Code     ? s_ctx->codeFont : nullptr;
	if (font) ImGui::PushFont(font);
	const float wrapW = std::max(40.0f, width - 8.0f);
	const float textH = std::max(ImGui::GetTextLineHeight(),
	                             ImGui::CalcTextSize((text + " ").c_str(), nullptr, false, wrapW).y);
	const float h = textH + ImGui::GetStyle().FramePadding.y * 2.0f + 2.0f;

	if (b.type == BlockType::Callout || b.type == BlockType::Code)
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + width, p.y + h),
		                                          Theme::u32(b.type == BlockType::Code ? Theme::warm(0.06f) : Theme::warm(0.16f)), 4.0f);
	}
	if (b.type == BlockType::Quote)
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 3.0f, p.y + h), Theme::u32(Theme::Accent));
	}
	bool focusNow = false;
	if (s_focusBlock == blockId)
	{
		ImGui::SetKeyboardFocusHere();
		s_focusBlock = UUID{};
		focusNow = true;
	}
	// Where the cursor was before this frame's keys moved it: the first or the last
	// line of the block decides whether an arrow key leaves it.
	bool onFirstLine = false, onLastLine = false;
	int cursorBefore = -1;
	bool selectionBefore = false;
	if (ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetID("##text")))
	{
		const int cursor = std::clamp(state->GetCursorPos(), 0, static_cast<int>(text.size()));
		cursorBefore = cursor;
		selectionBefore = state->HasSelection();
		const float lineH = ImGui::GetTextLineHeight();
		const float above = ImGui::CalcTextSize((text.substr(0, static_cast<std::size_t>(cursor)) + " ").c_str(), nullptr, false, wrapW).y;
		const float all   = ImGui::CalcTextSize((text + " ").c_str(), nullptr, false, wrapW).y;
		onFirstLine = above < lineH * 1.5f;
		onLastLine  = all - above < lineH * 0.5f;
	}
	ImGui::SetNextItemWidth(width);
	const ImGuiInputTextFlags flags = ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_EnterReturnsTrue |
	                                  ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_NoHorizontalScroll;
	const bool entered = ImGui::InputTextMultiline("##text", &text, ImVec2(width, h), flags);
	note("block:" + std::to_string(index));
	if (font) ImGui::PopFont();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();

	// The keyboard focus takes a frame to arrive: the cursor is placed once the field is active.
	if (focusNow) { s_cursorBlock = blockId; s_cursorAtEnd = s_focusAtEnd; }
	if (s_cursorBlock == blockId && ImGui::IsItemActive())
		if (ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetItemID()))
		{
			const int size = static_cast<int>(text.size());
			const int at = s_cursorAt >= 0 ? std::min(s_cursorAt, size) : s_cursorAtEnd ? size : 0;
			state->SetSelection(at, at);
			s_cursorBlock = UUID{};
			s_cursorAt = -1;
		}
	// Markdown as it is typed: "# " makes a heading, "- " a list, three backticks a code block, at once.
	{
		BlockType asType = BlockType::Paragraph;
		std::string rest;
		if (b.type == BlockType::Paragraph && text != before && shortcutType(text, asType, rest))
		{
			s_blockEdits.erase(blockId);
			commitBlockText(pageId, blockId, text, false);
			if (asType == BlockType::Code || asType == BlockType::Divider)
				ImGui::ClearActiveID();   // another widget (the code editor, the paragraph under a divider) takes over
			else if (ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetItemID()))
				state->ReloadUserBufAndMoveToEnd();   // the field stays active and takes the text without the marker
			// Not ClearActiveID + focus again: a field re-activated that way does not get Enter any more.
			if (asType == BlockType::Code) s_focusBlock = blockId;
			return;
		}
	}
	if (text != before) s_blockEdits[blockId] = text;
	// Left with something typed in an earlier frame. ImGui's own "deactivated after edit"
	// is not reliable when another field takes the focus earlier in the same frame, so
	// the pending text is what says it.
	const bool left = hadPending && !ImGui::IsItemActive() && !entered;
	if (entered || left)
	{
		s_blockEdits.erase(blockId);
		// Enter splits the block at the cursor: what follows it starts the next block.
		std::string tail;
		if (entered)
			if (ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetItemID()))
			{
				const std::size_t at = static_cast<std::size_t>(std::clamp(state->GetCursorPos(), 0, static_cast<int>(text.size())));
				tail = text.substr(at);
				text.resize(at);
			}
		if (entered) ImGui::ClearActiveID();
		commitBlockText(pageId, blockId, text, entered, tail);
	}
	else if (ImGui::IsItemDeactivated()) s_blockEdits.erase(blockId);

	// An arrow key past the first or last line goes to the block above or below.
	if (ImGui::IsItemActive() && !entered)
	{
		if (onFirstLine && !(prevId == UUID{}) && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
		{
			s_focusBlock = prevId;
			s_focusAtEnd = true;
		}
		else if (onLastLine && !(nextId == UUID{}) && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
		{
			s_focusBlock = nextId;
			s_focusAtEnd = false;
		}
	}

	// Backspace at the start of a block with text in it: back to a paragraph, or joined with the block above.
	if (ImGui::IsItemActive() && !before.empty() && text == before && cursorBefore == 0 && !selectionBefore &&
	    ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
	{
		if (b.type == BlockType::Paragraph && !(prevId == UUID{})) ImGui::ClearActiveID();   // the block above takes the keyboard
		s_blockEdits.erase(blockId);
		backspaceAtStart(pageId, blockId, prevId, text);
	}
	// Backspace in an empty block takes the block away and moves up one.
	if (ImGui::IsItemActive() && before.empty() && text.empty() && b.children.empty() &&
	    ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
	{
		post([=] {
			s_ws.editPage(pageId, [&](Page& page) { return page.removeBlock(blockId, taskNowMs()); });
			s_focusBlock = prevId;
			s_focusAtEnd = true;
		});
		s_blockEdits.erase(blockId);
	}
}

// The "..." button of a block: turn it into another kind, move it, delete it.
void blockMenu(const Page& page, const Block& b, int index)
{
	const UUID pageId = page.id, blockId = b.id;
	const float side = ImGui::GetFrameHeight() * 0.8f;
	const bool clicked = ImGui::InvisibleButton("##handle", ImVec2(side, ImGui::GetFrameHeight()));
	const ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
	const ImVec2 c((a.x + z.x) * 0.5f, (a.y + z.y) * 0.5f);
	const ImU32 col = Theme::u32(ImGui::IsItemHovered() ? Theme::Text : Theme::TextDim);
	for (int i = -1; i <= 1; ++i) ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(c.x, c.y + i * 5.0f), 1.6f, col);
	note("block-menu:" + std::to_string(index));
	// The handle is also what you take hold of to move the block.
	if (ImGui::BeginDragDropSource())
	{
		ImGui::SetDragDropPayload("HE_PLANNER_BLOCK", &blockId, sizeof(UUID));
		ImGui::TextUnformatted(b.text.empty() ? "Block" : b.text.c_str());
		ImGui::EndDragDropSource();
	}
	if (clicked && releasedWithoutDrag()) ImGui::OpenPopup("##blockmenu");
	if (!beginPopup("##blockmenu")) return;

	auto change = [&](std::function<bool(Page&)> fn) {
		post([=] { s_ws.editPage(pageId, fn); });
	};
	if (ImGui::BeginMenu("Turn into"))
	{
		for (const BlockKind& k : kBlockKinds)
			if (ImGui::MenuItem(k.label, nullptr, b.type == k.type))
			{
				const BlockType t = k.type;
				change([=](Page& p) { return p.setBlockType(blockId, t, taskNowMs()); });
			}
		ImGui::EndMenu();
	}
	if (EditorWidgets::menuItem("Add Block Below"))
		post([=] {
			s_ws.editPage(pageId, [&](Page& p) {
				Block* made = p.addBlockAfter(blockId, BlockType::Paragraph, "", taskNowMs());
				if (made) s_focusBlock = made->id;
				return made != nullptr;
			});
		});
	ImGui::Separator();
	if (EditorWidgets::menuItem("Move Up"))   change([=](Page& p) { return p.moveBlockBy(blockId, -1, taskNowMs()); });
	note("menu:Move Up");
	if (EditorWidgets::menuItem("Move Down")) change([=](Page& p) { return p.moveBlockBy(blockId, 1, taskNowMs()); });
	note("menu:Move Down");
	if (EditorWidgets::menuItem("Indent"))    change([=](Page& p) { return p.indentBlock(blockId, taskNowMs()); });
	note("menu:Indent");
	if (EditorWidgets::menuItem("Outdent"))   change([=](Page& p) { return p.outdentBlock(blockId, taskNowMs()); });
	note("menu:Outdent");
	ImGui::Separator();
	if (EditorWidgets::dangerMenuItem("Delete Block"))
		change([=](Page& p) { return p.removeBlock(blockId, taskNowMs()); });
	note("menu:Delete Block");
	ImGui::EndPopup();
}

// An engine object on the page: a chip that jumps there, or a drop zone to fill it.
void linkBlock(const Page& page, const Block& b, int index, float width)
{
	AppContext& ctx = *s_ctx;
	const UUID pageId = page.id, blockId = b.id;
	const bool empty = b.link.id == UUID{} && b.link.scene.empty() && b.link.panel.empty();
	const float h = std::floor(ImGui::GetFrameHeight() * 1.1f);
	if (!empty)
	{
		if (TaskBoardPanel::linkChip(ctx, b.link, index, std::min(width - 30.0f, 360.0f), h))
			TaskBoardPanel::jumpToLink(ctx, b.link);
		note("link-chip:" + std::to_string(index));
		ImGui::SameLine(0, 4.0f);
		if (ImGui::SmallButton("x"))
			post([=] {
				s_ws.editPage(pageId, [&](Page& p) {
					Block* x = p.findBlock(blockId);
					if (!x) return false;
					x->link = TaskLink{};
					p.modifiedMs = taskNowMs();
					return true;
				});
			});
		return;
	}
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(width, h * 1.3f));
	note("link-drop:" + std::to_string(index));
	const std::vector<TaskLink> dropped = TaskBoardPanel::acceptDroppedLinks(ctx);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const bool dragging = ImGui::GetDragDropPayload() != nullptr;
	dl->AddRect(at, ImVec2(at.x + width, at.y + h * 1.3f), Theme::u32(dragging ? Theme::AccentBright : Theme::TextDim), 4.0f);
	const char* hint = "Drop an asset, a scene or an entity here";
	dl->AddText(ImVec2(at.x + 8.0f, at.y + (h * 1.3f - ImGui::GetTextLineHeight()) * 0.5f),
	            ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
	if (!dropped.empty())
	{
		const TaskLink link = dropped.front();
		post([=] {
			s_ws.editPage(pageId, [&](Page& p) {
				Block* x = p.findBlock(blockId);
				if (!x) return false;
				x->link = link;
				p.modifiedMs = taskNowMs();
				return true;
			});
		});
	}
}

// A database view on the page: pick one, then see its first rows.
void databaseBlock(const Page& page, const Block& b, float width)
{
	const UUID pageId = page.id, blockId = b.id;
	const Database* db = s_ws.find(b.database);
	if (!db)
	{
		ImGui::SetNextItemWidth(std::min(width, 260.0f));
		if (ImGui::BeginCombo("##blockdb", "Choose a database"))
		{
			for (const Database* d : s_ws.databases())
				if (ImGui::Selectable((d->name + "##pick").c_str()))
				{
					const UUID dbId = d->id;
					post([=] {
						s_ws.editPage(pageId, [&](Page& p) {
							Block* x = p.findBlock(blockId);
							if (!x) return false;
							x->database = dbId;
							x->view = UUID{};
							p.modifiedMs = taskNowMs();
							return true;
						});
					});
				}
			ImGui::EndCombo();
		}
		return;
	}
	const View* view = db->findView(b.view);
	if (!view && !db->views.empty()) view = &db->views.front();
	ImGui::TextDisabled("%s", db->name.c_str());
	ImGui::SameLine();
	if (ImGui::SmallButton("Open"))
	{
		const UUID dbId = db->id;
		post([=] { s_pageId = UUID{}; s_dbId = dbId; s_viewId = UUID{}; });
	}
	if (!view) return;
	const std::vector<std::size_t> idx = db->query(*view);
	std::vector<const Property*> cols;
	for (const Property& p : db->properties)
		if (!isHidden(*view, p.id) && cols.size() < 3) cols.push_back(&p);
	if (ImGui::BeginTable("##embedded", 1 + static_cast<int>(cols.size()),
	                      ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Title");
		for (const Property* p : cols) ImGui::TableSetupColumn(p->name.c_str());
		ImGui::TableHeadersRow();
		const std::size_t shown = std::min<std::size_t>(idx.size(), 8);
		for (std::size_t i = 0; i < shown; ++i)
		{
			const Row& r = db->rows[idx[i]];
			pushUuid(r.id);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			if (ImGui::Selectable(r.title.empty() ? "Untitled" : r.title.c_str(), false))
			{
				const UUID dbId = db->id, rowId = r.id;
				post([=] { openPage(rowId, dbId); });
			}
			note("embedded-row:" + r.title);
			for (std::size_t c = 0; c < cols.size(); ++c)
			{
				ImGui::TableSetColumnIndex(static_cast<int>(c) + 1);
				ImGui::TextUnformatted(summary(*db, r, *cols[c]).c_str());
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (idx.size() > 8) ImGui::TextDisabled("and %d more", static_cast<int>(idx.size() - 8));
}

// Go to a page: a row's page opens with its database, any other as itself.
void goToPage(const UUID& id)
{
	const Page* p = s_ws.findPage(id);
	if (!p) return;
	if (s_ws.find(p->parent)) openPage(id, p->parent);
	else
	{
		s_pageId = id;
		s_pageDb = UUID{};
	}
}

// A link to another page: a choice of pages while it points nowhere, then the page's
// title as a link.
void pageLinkBlock(const Page& page, const Block& b, int index, float width)
{
	const UUID pageId = page.id, blockId = b.id;
	auto setTarget = [=](const UUID& target) {
		post([=] {
			s_ws.editPage(pageId, [&](Page& p) {
				Block* x = p.findBlock(blockId);
				if (!x) return false;
				x->page = target;
				p.modifiedMs = taskNowMs();
				return true;
			});
		});
	};
	if (b.page == UUID{})
	{
		ImGui::SetNextItemWidth(std::min(width, 260.0f));
		if (ImGui::BeginCombo("##blockpage", "Choose a page"))
		{
			for (const Page* p : s_ws.pages())
			{
				if (p->id == pageId || s_ws.find(p->parent)) continue;   // not itself, not a row's page
				pushUuid(p->id);
				if (ImGui::Selectable((p->title.empty() ? std::string("Untitled") : p->title).c_str())) setTarget(p->id);
				note("pick-page:" + p->title);
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		note("page-link:" + std::to_string(index));
		return;
	}
	const Page* target = s_ws.findPage(b.page);
	if (!target)
		ImGui::TextDisabled("Page not found");
	else
	{
		const std::string title = target->title.empty() ? std::string("Untitled") : target->title;
		ImGui::PushStyleColor(ImGuiCol_Text, Theme::AccentHi);
		const float w = std::min(width - ImGui::GetFrameHeight() * 1.5f, ImGui::CalcTextSize(title.c_str()).x + 24.0f);
		const bool clicked = ImGui::Selectable((title + "##open").c_str(), false, 0, ImVec2(std::max(w, 40.0f), 0));
		ImGui::PopStyleColor();
		note("page-link:" + std::to_string(index));
		if (clicked)
		{
			const UUID id = target->id;
			post([=] { goToPage(id); });
		}
	}
	ImGui::SameLine(0, 4.0f);
	if (ImGui::SmallButton("x")) setTarget(UUID{});
}

// The page's blocks, one per line, children indented.
// ── Code blocks ──────────────────────────────────────────────────────────────
// A code block is an ImGuiColorTextEdit editor (the script editor's): syntax colours for the block's
// language, Enter is a new line, Tab indents. One editor per block, made when the block is first drawn;
// the block's text is written back a moment after typing stops and when the editor is left.

struct CodeLang { const char* word; const char* label; TextEditor::LanguageDefinitionId id; };
const CodeLang kCodeLangs[] = {
	{ "", "Plain text", TextEditor::LanguageDefinitionId::None },   { "cpp", "C++", TextEditor::LanguageDefinitionId::Cpp },
	{ "c", "C", TextEditor::LanguageDefinitionId::C },                { "cs", "C#", TextEditor::LanguageDefinitionId::Cs },
	{ "python", "Python", TextEditor::LanguageDefinitionId::Python }, { "lua", "Lua", TextEditor::LanguageDefinitionId::Lua },
	{ "json", "JSON", TextEditor::LanguageDefinitionId::Json },       { "sql", "SQL", TextEditor::LanguageDefinitionId::Sql },
	{ "glsl", "GLSL", TextEditor::LanguageDefinitionId::Glsl },       { "hlsl", "HLSL", TextEditor::LanguageDefinitionId::Hlsl },
	{ "angelscript", "AngelScript", TextEditor::LanguageDefinitionId::AngelScript },
};

// The language a block's word names; Markdown fences also say "c++", "py", "csharp", "javascript"...
const CodeLang& codeLang(std::string word)
{
	for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	if (word == "c++" || word == "cxx" || word == "hpp" || word == "h") word = "cpp";
	if (word == "py")                       word = "python";
	if (word == "csharp" || word == "c#")   word = "cs";
	for (const CodeLang& l : kCodeLangs)
		if (word == l.word) return l;
	return kCodeLangs[0];
}

struct CodeEdit
{
	TextEditor  editor;
	std::string language = "\x01";   // what the editor is set up for (nothing at first)
	std::string synced;               // the block text the editor last took or wrote
	bool        focused = false;
	double      typedAt = -1.0;       // when the text last changed, while not yet written
	int         undoAt  = 0;          // the editor's undo index then: it moves with every edit
};
std::unordered_map<UUID, std::unique_ptr<CodeEdit>> s_codeEdits;

void writeCode(const UUID& pageId, const UUID& blockId, CodeEdit& ce)
{
	const std::string text = ce.editor.GetText();
	ce.typedAt = -1.0;
	if (text == ce.synced) return;
	ce.synced = text;
	post([=] { s_ws.editPage(pageId, [&](Page& p) { return p.setBlockText(blockId, text, taskNowMs()); }); });
}

void codeBlock(const Page& page, const Block& b, int index, float width, const UUID& prevId, const UUID& nextId)
{
	const UUID pageId = page.id, blockId = b.id;
	std::unique_ptr<CodeEdit>& slot = s_codeEdits[blockId];
	if (!slot)
	{
		slot = std::make_unique<CodeEdit>();
		slot->editor.SetShowLineNumbersEnabled(false);
		slot->editor.SetShowWhitespacesEnabled(false);
		slot->editor.SetScrollPastEndEnabled(false);
		slot->editor.SetPalette(TextEditor::PaletteId::Dark);
		slot->editor.SetTabSize(4);
		slot->editor.SetText(b.text);
		slot->synced = b.text;
		slot->undoAt = slot->editor.GetUndoIndex();
	}
	CodeEdit& ce = *slot;
	if (ce.language != b.language)
	{
		ce.editor.SetLanguageDefinition(codeLang(b.language).id);
		ce.language = b.language;
	}
	// The text changed somewhere else (a pull, the MCP tools, undo of the page): take it, unless typing here.
	if (!ce.focused && b.text != ce.synced)
	{
		ce.editor.SetText(b.text);
		ce.synced = b.text;
	}

	if (s_ctx && s_ctx->codeFont) ImGui::PushFont(s_ctx->codeFont);
	const float lineH = ImGui::GetTextLineHeight();   // the editor's line advance (it draws with no item spacing)
	// As tall as its lines; a sideways scrollbar only takes room when a line is wider than the block.
	float widest = 0.0f;
	for (const std::string& l : ce.editor.GetTextLines()) widest = std::max(widest, ImGui::CalcTextSize(l.c_str()).x);
	const float h = static_cast<float>(std::max(1, ce.editor.GetLineCount())) * lineH + 10.0f +
	                (widest > width - 24.0f ? ImGui::GetStyle().ScrollbarSize : 0.0f);
	if (s_focusBlock == blockId)
	{
		ImGui::SetNextWindowFocus();
		s_focusBlock = UUID{};
	}
	const ImVec2 top = ImGui::GetCursorScreenPos();
	const bool focused = ce.editor.Render("##code", false, ImVec2(width, h));
	note("block:" + std::to_string(index));
	const ImVec2 after = ImGui::GetCursorScreenPos();
	if (s_ctx && s_ctx->codeFont) ImGui::PopFont();

	if (focused)
	{
		// ImGui 1.92 starts the platform's text input from PlatformImeData, which a custom widget never sets
		// (see ScriptEditorPanel): without this, typed characters never arrive on macOS.
		ImGuiContext& g = *ImGui::GetCurrentContext();
		g.PlatformImeData.WantTextInput = true;
		g.PlatformImeData.InputPos      = top;
		// Written once typing has paused, not every so often while it goes on: each write is a save the
		// team sync hears about.
		if (ce.editor.GetUndoIndex() != ce.undoAt)
		{
			ce.undoAt  = ce.editor.GetUndoIndex();
			ce.typedAt = ImGui::GetTime();
		}
		if (ce.typedAt >= 0.0 && ImGui::GetTime() - ce.typedAt > 0.8) writeCode(pageId, blockId, ce);
		// Up on the first line and Down on the last leave the block, as in a text block.
		int line = 0, column = 0;
		ce.editor.GetCursorPosition(line, column);
		if (line == 0 && !(prevId == UUID{}) && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
		{
			writeCode(pageId, blockId, ce);
			s_focusBlock = prevId;
			s_focusAtEnd = true;
		}
		else if (line >= ce.editor.GetLineCount() - 1 && !(nextId == UUID{}) && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
		{
			writeCode(pageId, blockId, ce);
			s_focusBlock = nextId;
			s_focusAtEnd = false;
		}
	}
	else if (ce.focused) writeCode(pageId, blockId, ce);   // left
	ce.focused = focused;

	// The language, small at the top right of the block.
	{
		const CodeLang& lang = codeLang(b.language);
		const float comboW = ImGui::CalcTextSize(lang.label).x + ImGui::GetFrameHeight() + 14.0f;
		ImGui::SetCursorScreenPos(ImVec2(top.x + width - comboW - 6.0f, top.y + 4.0f));
		ImGui::SetNextItemWidth(comboW);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 1.0f));
		if (ImGui::BeginCombo("##codelang", lang.label))
		{
			for (const CodeLang& l : kCodeLangs)
				if (ImGui::Selectable(l.label, &l == &lang))
				{
					const std::string word = l.word;
					s_lastCodeLang = word;
					post([=] {
						s_ws.editPage(pageId, [&](Page& p) {
							Block* blk = p.findBlock(blockId);
							if (!blk || blk->language == word) return false;
							blk->language = word;
							p.modifiedMs = taskNowMs();
							return true;
						});
					});
				}
			ImGui::EndCombo();
		}
		EditorWidgets::helpForKey("Planner/Code Language");
		note("code-lang:" + std::to_string(index));
		ImGui::PopStyleVar();
		ImGui::SetCursorScreenPos(after);
		ImGui::Dummy(ImVec2(0, 0));
	}
}

void drawBlocks(const Page& page)
{
	const UUID pageId = page.id;
	std::vector<FlatBlock> flat;
	flattenBlocks(page.blocks, 0, flat);

	const float fh = ImGui::GetFrameHeight();
	const float handleW = fh * 0.8f + 4.0f;
	const float prefixW = fh * 1.0f;
	// The blocks the cursor can be put in: the arrow keys and Backspace skip the rest.
	auto hasText = [](BlockType t) {
		return t != BlockType::Divider && t != BlockType::EngineLink && t != BlockType::DatabaseView && t != BlockType::PageLink;
	};
	std::vector<UUID> nextText(flat.size());
	{
		UUID later;
		for (std::size_t i = flat.size(); i-- > 0;)
		{
			nextText[i] = later;
			if (hasText(flat[i].block->type)) later = flat[i].block->id;
		}
	}
	UUID prevId;
	for (std::size_t i = 0; i < flat.size(); ++i)
	{
		const Block& b = *flat[i].block;
		const UUID blockId = b.id;
		pushUuid(blockId);
		const float indent = static_cast<float>(flat[i].depth) * 26.0f;
		const float avail = ImGui::GetContentRegionAvail().x;
		if (indent > 0) ImGui::Indent(indent);
		ImGui::BeginGroup();

		blockMenu(page, b, static_cast<int>(i));
		ImGui::SameLine(0, 4.0f);

		if (b.type == BlockType::Divider)
		{
			ImGui::AlignTextToFramePadding();
			const ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(avail - indent - handleW, fh));
			ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + fh * 0.5f), ImVec2(p.x + avail - indent - handleW, p.y + fh * 0.5f),
			                                    Theme::u32(Theme::TextDim));
		}
		else if (b.type == BlockType::EngineLink)
			linkBlock(page, b, static_cast<int>(i), avail - indent - handleW);
		else if (b.type == BlockType::DatabaseView)
		{
			ImGui::BeginGroup();
			databaseBlock(page, b, avail - indent - handleW);
			ImGui::EndGroup();
		}
		else if (b.type == BlockType::PageLink)
			pageLinkBlock(page, b, static_cast<int>(i), avail - indent - handleW);
		else if (b.type == BlockType::Code)
			codeBlock(page, b, static_cast<int>(i), avail - indent - handleW - 8.0f, prevId, nextText[i]);
		else
		{
			// A marker before the text for lists and to-dos.
			float used = 0.0f;
			if (b.type == BlockType::Bullet || b.type == BlockType::Numbered)
			{
				ImGui::AlignTextToFramePadding();
				if (b.type == BlockType::Bullet) ImGui::TextUnformatted("\xE2\x80\xA2");
				else                             ImGui::Text("%d.", flat[i].number);
				ImGui::SameLine(0, 6.0f);
				used = prefixW;
			}
			else if (b.type == BlockType::Todo)
			{
				bool on = b.checked;
				if (EditorWidgets::checkbox("##todo", &on))
					post([=] { s_ws.editPage(pageId, [&](Page& p) { return p.setBlockChecked(blockId, on, taskNowMs()); }); });
				note("check:" + std::to_string(i));
				ImGui::SameLine(0, 6.0f);
				used = prefixW;
			}
			if (b.type == BlockType::Todo && b.checked) ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextDim);
			blockText(page, b, static_cast<int>(i), avail - indent - handleW - used - 8.0f, prevId, nextText[i]);
			if (b.type == BlockType::Todo && b.checked) ImGui::PopStyleColor();
		}
		ImGui::EndGroup();
		// A block dropped on this row goes before it (upper half) or after it (lower half).
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(
			        "HE_PLANNER_BLOCK", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
				if (pl->DataSize == static_cast<int>(sizeof(UUID)))
				{
					const ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
					const bool after = ImGui::GetMousePos().y > (a.y + z.y) * 0.5f;
					const float y = after ? z.y : a.y;
					ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, y), ImVec2(z.x, y), Theme::u32(Theme::AccentBright), 2.0f);
					if (pl->IsDelivery())
					{
						const UUID moved = *static_cast<const UUID*>(pl->Data);
						post([=] {
							s_ws.editPage(pageId, [&](Page& p) { return p.moveBlockNear(moved, blockId, after, taskNowMs()); });
						});
					}
				}
			ImGui::EndDragDropTarget();
		}
		if (indent > 0) ImGui::Unindent(indent);
		ImGui::PopID();
		if (hasText(b.type)) prevId = blockId;
	}

	// Below the last block: a way to add one, and a place to drop an object.
	ImGui::Spacing();
	if (toolButton("Planner/Add Block", "Add Block", PlannerIcons::Icon::Plus, false))
		post([=] {
			s_ws.editPage(pageId, [&](Page& p) {
				Block* made = p.addBlockAfter(UUID{}, BlockType::Paragraph, "", taskNowMs());
				if (made) s_focusBlock = made->id;
				return made != nullptr;
			});
		});
	note("add-block");
	ImGui::SameLine();
	if (toolButton("Planner/New Sub-page", "New Sub-page", PlannerIcons::Icon::Page, false))
		post([=] {
			Page* child = s_ws.createPage("Untitled", pageId);
			if (!child) return;
			const UUID childId = child->id;
			// The new page is linked from here, so it can be found again.
			s_ws.editPage(pageId, [&](Page& p) {
				Block* made = p.addBlockAfter(UUID{}, BlockType::PageLink, "", taskNowMs());
				if (!made) return false;
				made->page = childId;
				return true;
			});
		});
	note("new-subpage");
	if (page.blocks.empty())
	{
		ImGui::SameLine();
		ImGui::TextDisabled("Type # for a heading, - for a list, [] for a to-do, ``` for code, --- for a divider.");
	}
	{
		AppContext& ctx = *s_ctx;
		const ImVec2 at = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		const float h = std::max(fh * 1.5f, ImGui::GetContentRegionAvail().y);
		// A click below the last block writes on: in the last block when it is an empty paragraph, else in a new one.
		if (ImGui::InvisibleButton("##writeon", ImVec2(w, h)))
		{
			const Block* last = page.blocks.empty() ? nullptr : &page.blocks.back();
			if (last && last->type == BlockType::Paragraph && last->text.empty() && last->children.empty())
				s_focusBlock = last->id;
			else
				post([=] {
					s_ws.editPage(pageId, [&](Page& p) {
						Block* made = p.addBlockAfter(UUID{}, BlockType::Paragraph, "", taskNowMs());
						if (made) s_focusBlock = made->id;
						return made != nullptr;
					});
				});
		}
		if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
		note("page-drop");
		const std::vector<TaskLink> dropped = TaskBoardPanel::acceptDroppedLinks(ctx);
		const bool dragging = ImGui::GetDragDropPayload() != nullptr;
		if (dragging)
		{
			ImGui::GetWindowDrawList()->AddRect(at, ImVec2(at.x + w, at.y + h), Theme::u32(Theme::AccentBright), 4.0f, 0, 2.0f);
			ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + 10.0f, at.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
			                                    ImGui::GetColorU32(ImGuiCol_TextDisabled), "Drop to add it to the page");
		}
		if (!dropped.empty())
			post([=] {
				s_ws.editPage(pageId, [&](Page& p) {
					bool any = false;
					for (const TaskLink& l : dropped)
						if (Block* made = p.addBlockAfter(UUID{}, BlockType::EngineLink, "", taskNowMs())) { made->link = l; any = true; }
					return any;
				});
			});
	}
}

// What a page's Markdown and Word export call the pages and tables it links to.
MarkdownContext exportContext()
{
	MarkdownContext c;
	c.pageTitle = [](const UUID& id) {
		const Page* p = s_ws.findPage(id);
		return p ? p->title : std::string{};
	};
	c.databaseName = [](const UUID& id) {
		const Database* d = s_ws.find(id);
		return d ? d->name : std::string{};
	};
	return c;
}

// The page as Markdown or Word under <project>/Export/Planner, and a notice saying where.
void exportPage(const UUID& pageId, const std::string& title, bool word)
{
	post([=] {
		const Page* page = s_ws.findPage(pageId);
		if (!page) return;
		Page named = *page;
		named.title = title;   // a row's page carries the row's title
		const std::filesystem::path file = exportFileFor(s_ws.project(), title, word ? "docx" : "md", "Page");
		const std::string bytes = word ? pageToDocx(named, exportContext())
		                               : "# " + (title.empty() ? std::string("Untitled") : title) + "\n\n" + pageToMarkdown(named, exportContext());
		std::string err;
		if (s_ws.project().empty()) err = "no project is open";
		else if (writeExport(file, bytes, &err)) HE::Ed::notify(HE::Ed::NoteLevel::Info, "Page exported", file.string());
		if (!err.empty()) HE::Ed::notify(HE::Ed::NoteLevel::Important, "Page not exported: " + err);
	});
}

// The page of a row (with its properties) or a standalone one.
void drawPage(AppContext& ctx)
{
	Page* page = s_ws.findPage(s_pageId);
	if (!page) { s_pageId = s_pageDb = UUID{}; return; }
	const UUID pageId = page->id;
	const bool rowPage = !(s_pageDb == UUID{});
	const Database* db = rowPage ? s_ws.find(s_pageDb) : nullptr;
	const Row* row = db ? db->findRow(pageId) : nullptr;
	if (rowPage && !row)
	{
		// The row is gone (deleted, or a pull took it): the page has nothing to hang on.
		s_pageId = s_pageDb = UUID{};
		return;
	}

	if (rowPage)
	{
		if (EditorWidgets::smallButton("Back"))
			post([=] { s_dbId = db ? db->id : UUID{}; s_pageId = s_pageDb = UUID{}; });
		note("page:back");
		ImGui::SameLine();
		ImGui::TextDisabled("%s", db->name.c_str());
	}
	else if (const Page* parent = s_ws.findPage(page->parent))
	{
		// A sub-page: the way back is the page it hangs under.
		if (EditorWidgets::smallButton("Back"))
		{
			const UUID parentId = parent->id;
			post([=] { goToPage(parentId); });
		}
		note("page:parent");
		ImGui::SameLine();
		ImGui::TextDisabled("%s", parent->title.empty() ? "Untitled" : parent->title.c_str());
	}

	// The title: the row's, or the page's own.
	const float titleW = std::min(640.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() * 2.0f);
	if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
	if (rowPage)
	{
		const UUID dbId = db->id;
		textCell("##pagetitle", { pageId, UUID{ 0, 4 } }, row->title, [=](const std::string& t) {
			post([=] { s_ws.edit(dbId, [&](Database& d) { return d.setTitle(pageId, t, taskNowMs()); }); });
		}, "Untitled", 0.0f, titleW);
	}
	else
		textCell("##pagetitle", { pageId, UUID{ 0, 4 } }, page->title, [=](const std::string& t) {
			post([=] { s_ws.editPage(pageId, [&](Page& p) {
				if (p.title == t) return false;
				p.title = t;
				p.modifiedMs = taskNowMs();
				return true;
			}); });
		}, "Untitled", 0.0f, titleW);
	if (ctx.fontHeading) ImGui::PopFont();
	note("page:title");
	{
		// Export, at the right end of the title line.
		const std::string title = rowPage ? row->title : page->title;
		ImGui::SameLine();
		alignRight(toolWidth("Export"));
		if (toolButton("Planner/Export Page", "Export", PlannerIcons::Icon::Export, false)) ImGui::OpenPopup("##pageexport");
		note("page:export");
		if (beginPopup("##pageexport"))
		{
			if (ImGui::Selectable("Markdown (.md)")) exportPage(pageId, title, false);
			note("export:md");
			if (ImGui::Selectable("Word (.docx)")) exportPage(pageId, title, true);
			note("export:docx");
			ImGui::TextDisabled("Into the project's Export/Planner folder.");
			ImGui::EndPopup();
		}
	}
	ImGui::Spacing();

	ImGui::BeginChild("##pagebody", ImVec2(0, 0), ImGuiChildFlags_None);
	const float column = std::min(820.0f, ImGui::GetContentRegionAvail().x);
	if (rowPage)
	{
		ImGui::PushItemWidth(column);
		if (ImGui::BeginChild("##pageprops", ImVec2(column, 0), ImGuiChildFlags_AutoResizeY))
		{
			drawPropertyTable(*db, *row);
			ImGui::EndChild();
		}
		else ImGui::EndChild();
		ImGui::PopItemWidth();
		ImGui::Separator();
	}
	ImGui::BeginChild("##pageblocks", ImVec2(column, 0), ImGuiChildFlags_None);
	drawBlocks(*page);
	ImGui::EndChild();
	ImGui::EndChild();
}

// ── The calendar ─────────────────────────────────────────────────────────────

int  s_calYear = 0, s_calMonth = 0;   // the month shown; 0 = not chosen yet (today's)

void splitDay(std::int64_t day, int& y, int& m, int& d)
{
	const std::string iso = taskTimeToIso(day * kDayMs);
	y = std::atoi(iso.substr(0, 4).c_str());
	m = std::atoi(iso.substr(5, 2).c_str());
	d = std::atoi(iso.substr(8, 2).c_str());
}

std::int64_t firstOfMonth(int y, int m)
{
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%04d-%02d-01T00:00:00.000Z", y, m);
	std::int64_t ms = 0;
	taskTimeFromIso(buf, ms);
	return dayOf(ms);
}

void stepMonth(int delta)
{
	int m = s_calMonth + delta, y = s_calYear;
	while (m < 1)  { m += 12; --y; }
	while (m > 12) { m -= 12; ++y; }
	s_calYear = y;
	s_calMonth = m;
}

// A month in a grid, Monday first, six weeks so it does not jump in height. A row
// shows on every day from its Date to its End (on its Date alone without one).
// Drag a row onto another day to move it; double-click a day to add a row there;
// click a row to open it.
void drawCalendar(const Database& db, const View& view, const std::vector<std::size_t>& idx)
{
	const Property* dateP = db.findProperty(view.dateProperty);
	if (!dateP || dateP->type != PropertyType::Date)
	{
		ImGui::TextDisabled("Pick a Date property above. Add one with Properties if there is none.");
		return;
	}
	const Property* endP = view.endProperty == UUID{} ? nullptr : db.findProperty(view.endProperty);
	if (endP && endP->type != PropertyType::Date) endP = nullptr;
	const Property* colourP = db.findProperty(view.groupBy);
	if (colourP && colourP->type != PropertyType::Select) colourP = nullptr;

	const std::int64_t today = dayOf(taskNowMs());
	if (s_calMonth == 0)
	{
		int d = 0;
		splitDay(today, s_calYear, s_calMonth, d);
	}
	const std::int64_t first = firstOfMonth(s_calYear, s_calMonth);
	const std::int64_t gridStart = first - ((first % 7 + 7 + 3) % 7);   // 1970-01-01 was a Thursday; weeks start on Monday

	struct Span { std::size_t row; std::int64_t a, b; };
	std::vector<Span> spans;
	for (const std::size_t i : idx)
	{
		const Row& r = db.rows[i];
		std::int64_t s = 0, e = 0;
		if (!db.date(r.id, dateP->id, s)) continue;
		const std::int64_t a = dayOf(s);
		std::int64_t b = a;
		if (endP && db.date(r.id, endP->id, e)) b = std::max(a, dayOf(e));
		spans.push_back({ i, a, b });
	}

	ImGui::BeginChild("##calendar", ImVec2(0, 0), ImGuiChildFlags_None);
	const float fh = ImGui::GetFrameHeight();
	const float headH = fh + 4.0f;
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const float cellW = std::max(60.0f, avail.x / 7.0f);
	const float cellH = std::max(fh * 4.0f, (avail.y - headH) / 6.0f);
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();

	static const char* const kWeekdays[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
	for (int c = 0; c < 7; ++c)
		dl->AddText(ImVec2(origin.x + c * cellW + 6.0f, origin.y + 2.0f), Theme::u32(Theme::TextDim), kWeekdays[c]);

	const float chipH = fh * 0.85f;
	auto dayAtPos = [&](ImVec2 pos) {
		const int col = std::clamp(static_cast<int>(std::floor((pos.x - origin.x) / cellW)), 0, 6);
		const int wk  = std::clamp(static_cast<int>(std::floor((pos.y - origin.y - headH) / cellH)), 0, 5);
		return gridStart + wk * 7 + col;
	};
	const ImGuiIO& io = ImGui::GetIO();
	const bool noDates = db.id == tasksDatabaseId();

	// A span of days as a bar across the weeks it covers (a segment per week), square where it carries on.
	// `lane` gives each week its own row of bars; a negative lane is a ghost, drawn on top of the cells.
	auto drawSegment = [&](std::int64_t a, std::int64_t b, std::int64_t weekStart, int lane, const ImVec4& colour, float alpha, bool outline,
	                       const char* label, bool startsHere, bool endsHere, ImVec2& outA, ImVec2& outB) {
		const int wk = static_cast<int>((weekStart - gridStart) / 7);
		const std::int64_t sa = std::max(a, weekStart), sb = std::min(b, weekStart + 6);
		const float x0 = origin.x + static_cast<float>(sa - weekStart) * cellW + 3.0f;
		const float x1 = origin.x + static_cast<float>(sb - weekStart + 1) * cellW - 3.0f;
		const float y0 = origin.y + headH + static_cast<float>(wk) * cellH + fh + 2.0f + static_cast<float>(lane) * (chipH + 2.0f);
		outA = ImVec2(x0, y0);
		outB = ImVec2(x1, y0 + chipH);
		const ImDrawFlags flags = (startsHere ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersNone) |
		                          (endsHere ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone);
		dl->AddRectFilled(outA, outB, Theme::u32(Theme::alpha(colour, alpha)), 4.0f, flags);
		if (outline) dl->AddRect(outA, outB, Theme::u32(Theme::AccentBright), 4.0f, flags, 1.5f);
		if (label)
		{
			dl->PushClipRect(outA, outB, true);
			dl->AddText(ImVec2(x0 + 5.0f, y0 + (chipH - ImGui::GetTextLineHeight()) * 0.5f), Theme::u32(Theme::Ink), label);
			dl->PopClipRect();
		}
	};
	auto drawSpanGhost = [&](std::int64_t a, std::int64_t b, const ImVec4& colour) {
		for (int wk = 0; wk < 6; ++wk)
		{
			const std::int64_t ws0 = gridStart + wk * 7;
			if (b < ws0 || a > ws0 + 6) continue;
			ImVec2 pa, pb;
			drawSegment(a, b, ws0, 0, colour, 0.45f, true, nullptr, a >= ws0, b <= ws0 + 6, pa, pb);
		}
	};

	// The days: a drag across them makes an event over exactly those days, a double-click or the + one day.
	bool createGhost = false;
	std::int64_t createA = 0, createB = 0;
	for (int week = 0; week < 6; ++week)
		for (int c = 0; c < 7; ++c)
		{
			const std::int64_t day = gridStart + week * 7 + c;
			int y = 0, m = 0, dnum = 0;
			splitDay(day, y, m, dnum);
			const ImVec2 p(origin.x + c * cellW, origin.y + headH + week * cellH);
			ImGui::PushID(static_cast<int>(week * 7 + c));
			ImGui::SetCursorScreenPos(p);
			ImGui::SetNextItemAllowOverlap();   // the bars on this day sit on top of it and take the clicks
			ImGui::InvisibleButton("##day", ImVec2(cellW, cellH));
			char key[32];
			std::snprintf(key, sizeof(key), "day:%04d-%02d-%02d", y, m, dnum);
			note(key);
			const bool cellHover = ImGui::IsItemHovered();
			const ImVec2 plusA(p.x + cellW - 26.0f, p.y + 3.0f), plusB(p.x + cellW - 4.0f, p.y + 25.0f);
			s_rects[std::string("cal-add:") + key] = Rect{ plusA.x, plusA.y, plusB.x, plusB.y };
			const bool overPlus = cellHover && io.MousePos.x >= plusA.x && io.MousePos.x <= plusB.x && io.MousePos.y >= plusA.y && io.MousePos.y <= plusB.y;
			const ImVec2 plusAt((plusA.x + plusB.x) * 0.5f, (plusA.y + plusB.y) * 0.5f);
			if (!noDates && cellHover && (overPlus ? ImGui::IsMouseClicked(ImGuiMouseButton_Left) : ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
				newDated(db.id, view.id, day, day);
			if (!noDates && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f))
			{
				const std::int64_t da = dayAtPos(io.MouseClickedPos[0]), dbb = dayAtPos(io.MousePos);
				createGhost = true;
				createA = std::min(da, dbb);
				createB = std::max(da, dbb);
			}
			if (!noDates && ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
			    std::abs(io.MousePos.x - io.MouseClickedPos[0].x) + std::abs(io.MousePos.y - io.MouseClickedPos[0].y) >= 6.0f)
			{
				const std::int64_t da = dayAtPos(io.MouseClickedPos[0]), dbb = dayAtPos(io.MousePos);
				if (da != dbb) newDated(db.id, view.id, std::min(da, dbb), std::max(da, dbb));
			}

			const bool otherMonth = m != s_calMonth;
			dl->AddRectFilled(p, ImVec2(p.x + cellW, p.y + cellH),
			                  Theme::u32(day == today ? Theme::alpha(Theme::Warn, 0.12f) : Theme::warm(otherMonth ? 0.07f : 0.095f)));
			dl->AddRect(p, ImVec2(p.x + cellW, p.y + cellH), Theme::u32(Theme::warm(0.20f)));
			char num[8];
			std::snprintf(num, sizeof(num), "%d", dnum);
			dl->AddText(ImVec2(p.x + 6.0f, p.y + 3.0f),
			            Theme::u32(day == today ? Theme::Warn : otherMonth ? Theme::TextDim : Theme::Text), num);
			if (cellHover && !noDates)
				PlannerIcons::drawIcon(dl, PlannerIcons::Icon::Plus, plusAt, Theme::u32(overPlus ? Theme::AccentBright : Theme::TextDim));
			ImGui::PopID();
		}

	// The events, as bars that carry on across the days (and the weeks) they cover.
	const int fit = std::max(0, static_cast<int>((cellH - fh - 6.0f) / (chipH + 2.0f)));
	std::vector<int> hiddenOnDay(42, 0);
	for (int week = 0; week < 6; ++week)
	{
		const std::int64_t ws0 = gridStart + week * 7;
		struct Seg { const Span* sp; std::int64_t a, b; int lane; };
		std::vector<Seg> segs;
		for (const Span& sp : spans)
			if (sp.b >= ws0 && sp.a <= ws0 + 6) segs.push_back({ &sp, std::max(sp.a, ws0), std::min(sp.b, ws0 + 6), 0 });
		std::stable_sort(segs.begin(), segs.end(), [](const Seg& x, const Seg& y) { return x.a != y.a ? x.a < y.a : x.b > y.b; });
		std::vector<std::int64_t> laneEnd;
		for (Seg& sg : segs)
		{
			std::size_t l = 0;
			while (l < laneEnd.size() && laneEnd[l] >= sg.a) ++l;
			if (l == laneEnd.size()) laneEnd.push_back(sg.b);
			laneEnd[l] = sg.b;
			sg.lane = static_cast<int>(l);
		}
		for (const Seg& sg : segs)
		{
			if (sg.lane >= fit)
			{
				for (std::int64_t d = sg.a; d <= sg.b; ++d) ++hiddenOnDay[static_cast<std::size_t>(week * 7 + (d - ws0))];
				continue;
			}
			const Row& row = db.rows[sg.sp->row];
			const std::string title = row.title.empty() ? std::string("Untitled") : row.title;
			ImGui::PushID(static_cast<int>(sg.sp->row));
			ImGui::PushID(week);
			ImVec4 colour = Theme::Accent;
			if (colourP)
				if (const SelectOption* o = colourP->findOption(db.select(row.id, colourP->id))) colour = colorOf(o->color);
			const bool startsHere = sg.sp->a >= ws0, endsHere = sg.sp->b <= ws0 + 6;
			ImVec2 pa, pb;
			// Where the bar is: the item first (it is the same rectangle), then what it looks like.
			drawSegment(sg.a, sg.b, ws0, sg.lane, colour, 0.0f, false, nullptr, startsHere, endsHere, pa, pb);
			ImGui::SetCursorScreenPos(pa);
			ImGui::SetNextItemAllowOverlap();   // the right end is a handle of its own
			ImGui::InvisibleButton("##chip", ImVec2(pb.x - pa.x, pb.y - pa.y));
			if (startsHere) note("cal-chip:" + row.title);
			const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
			if (releasedWithoutDrag()) s_selRow = row.id;
			// Dragged to another day, the bar moves by that many days (also across weeks).
			if (act && !noDates && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f))
				s_calDrag = { row.id, 1, static_cast<int>(dayAtPos(io.MousePos) - dayAtPos(io.MouseClickedPos[0])) };
			if (ImGui::IsItemDeactivated() && s_calDrag.row == row.id && s_calDrag.mode == 1)
			{
				shiftDates(db.id, row.id, dateP->id, endP ? endP->id : UUID{}, 1, s_calDrag.days);
				s_calDrag = DateDrag{};
			}
			const bool selected = row.id == s_selRow;
			drawSegment(sg.a, sg.b, ws0, sg.lane, colour, hov || act ? 0.97f : 0.8f, selected, title.c_str(), startsHere, endsHere, pa, pb);
			// The right end of the last piece: pull it to give the event more days or fewer.
			if (endsHere && !noDates)
			{
				ImGui::SetCursorScreenPos(ImVec2(pb.x - 8.0f, pa.y));
				ImGui::InvisibleButton("##end", ImVec2(8.0f, pb.y - pa.y));
				note("cal-end:" + row.title);
				if (ImGui::IsItemHovered() || ImGui::IsItemActive())
				{
					ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
					dl->AddLine(ImVec2(pb.x - 4.0f, pa.y + 4.0f), ImVec2(pb.x - 4.0f, pb.y - 4.0f), Theme::u32(Theme::Ink), 2.0f);
				}
				if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f))
					s_calDrag = { row.id, 3, static_cast<int>(dayAtPos(io.MousePos) - dayAtPos(io.MouseClickedPos[0])) };
				if (ImGui::IsItemDeactivated() && s_calDrag.row == row.id && s_calDrag.mode == 3)
				{
					shiftDates(db.id, row.id, dateP->id, endP ? endP->id : UUID{}, 3, s_calDrag.days);
					s_calDrag = DateDrag{};
				}
			}
			ImGui::PopID();
			ImGui::PopID();
		}
	}
	for (int i = 0; i < 42; ++i)
		if (hiddenOnDay[static_cast<std::size_t>(i)] > 0)
		{
			char buf[24];
			std::snprintf(buf, sizeof(buf), "+%d more", hiddenOnDay[static_cast<std::size_t>(i)]);
			const ImVec2 p(origin.x + (i % 7) * cellW, origin.y + headH + (i / 7) * cellH);
			dl->AddText(ImVec2(p.x + 6.0f, p.y + cellH - ImGui::GetTextLineHeight() - 2.0f), Theme::u32(Theme::TextDim), buf);
		}

	// What a drag would do, drawn over the days.
	if (createGhost) drawSpanGhost(createA, createB, ImVec4(Theme::Accent));
	if (s_calDrag.mode != 0)
		for (const Span& sp : spans)
			if (db.rows[sp.row].id == s_calDrag.row)
			{
				std::int64_t a = sp.a, b = sp.b;
				if (s_calDrag.mode == 1) { a += s_calDrag.days; b += s_calDrag.days; }
				else b = std::max(a, b + s_calDrag.days);
				drawSpanGhost(a, b, ImVec4(Theme::Accent));
			}
	ImGui::SetCursorScreenPos(origin);
	ImGui::Dummy(ImVec2(cellW * 7.0f, headH + cellH * 6.0f));
	ImGui::EndChild();
}

// ── A whiteboard ─────────────────────────────────────────────────────────────

std::function<bool(std::string&, std::string&)> s_clipImage;   // a test's clipboard; empty = the system's
std::function<std::string()>                    s_clipText;
std::function<void(const std::string&)>         s_clipSet;

void drawBoardTab(AppContext& ctx)
{
	if (!s_ws.findBoard(s_boardId)) { s_boardId = UUID{}; return; }
	PlannerBoardView::Services services;
	services.ws   = &s_ws;
	services.ctx  = &ctx;
	services.post = [](std::function<void()> fn) { post(std::move(fn)); };
	services.note = [](const std::string& name) { note(name); };
	services.noteRect = [](const std::string& name, ImVec2 a, ImVec2 b) { s_rects[name] = { a.x, a.y, b.x, b.y }; };
	services.root = s_ws.project();
	// The system clipboard: a picture in one of the formats a screenshot tool or a browser leaves, else text.
	services.clipboardImage = s_clipImage ? s_clipImage : [](std::string& bytes, std::string& ext) {
		static const struct { const char* mime; const char* ext; } kFormats[] = {
			{ "image/png", "png" }, { "image/jpeg", "jpg" }, { "image/bmp", "bmp" }, { "image/webp", "webp" } };
		for (const auto& f : kFormats)
		{
			if (!SDL_HasClipboardData(f.mime)) continue;
			size_t size = 0;
			if (void* data = SDL_GetClipboardData(f.mime, &size))
			{
				bytes.assign(static_cast<const char*>(data), size);
				SDL_free(data);
				ext = f.ext;
				if (!bytes.empty()) return true;
			}
		}
		return false;
	};
	services.clipboardText = s_clipText ? s_clipText : []() -> std::string {
		if (!SDL_HasClipboardText()) return {};
		char* t = SDL_GetClipboardText();
		std::string out = t ? t : "";
		SDL_free(t);
		return out;
	};
	services.setClipboardText = s_clipSet ? s_clipSet : [](const std::string& t) { SDL_SetClipboardText(t.c_str()); };
	PlannerBoardView::draw(services, s_boardId);
}

// ── One database ─────────────────────────────────────────────────────────────

// A row of buttons drawn as one control, `on` lit. Each is noted "<id>:<name>". The index clicked, or -1.
int segmented(const char* id, const char* const* names, int n, int on, const char* helpKey)
{
	int hit = -1;
	const float bh = ImGui::GetFrameHeight();
	ImGui::PushID(id);
	for (int i = 0; i < n; ++i)
	{
		if (i > 0) ImGui::SameLine(0, 0);
		const float w = ImGui::CalcTextSize(names[i]).x + 22.0f;
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::PushID(i);
		if (ImGui::InvisibleButton("##seg", ImVec2(w, bh))) hit = i;
		const bool hov = ImGui::IsItemHovered();
		EditorWidgets::helpForKey(helpKey);
		note(std::string(id) + ":" + names[i]);
		ImGui::PopID();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImDrawFlags flags = i == 0 ? ImDrawFlags_RoundCornersLeft : i == n - 1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone;
		dl->AddRectFilled(p, ImVec2(p.x + w, p.y + bh),
		                  Theme::u32(i == on ? Theme::alpha(Theme::Accent, 0.30f) : Theme::warm(hov ? 0.28f : 0.18f)), 7.0f, flags);
		dl->AddText(ImVec2(p.x + 11.0f, p.y + (bh - ImGui::GetFontSize()) * 0.5f),
		            Theme::u32(i == on ? Theme::AccentBright : Theme::Text), names[i]);
	}
	ImGui::PopID();
	return hit;
}

// The dated rows a timeline or calendar shows, as all-day events in an .ics file under Export/Planner.
// `open`: hand the file to the system as well, which on macOS is Calendar: it asks which calendar to
// add the events to. Importing the file again updates them (the UID is the row's id).
void exportCalendar(const UUID& dbId, const UUID& viewId, bool open)
{
	post([=] {
		const Database* db = s_ws.find(dbId);
		const View* view = db ? db->findView(viewId) : nullptr;
		if (!view) return;
		const Property* desc = nullptr;
		for (const Property& p : db->properties)
			if (p.type == PropertyType::Text && p.name == "Description") desc = &p;
		std::vector<CalendarEvent> events;
		for (const std::size_t i : db->query(*view))
		{
			const Row& r = db->rows[i];
			std::int64_t s = 0, e = 0;
			if (!db->date(r.id, view->dateProperty, s)) continue;
			CalendarEvent ev;
			ev.id = r.id;
			ev.title = r.title;
			ev.firstDay = ev.lastDay = dayOf(s);
			if (!(view->endProperty == UUID{}) && db->date(r.id, view->endProperty, e)) ev.lastDay = std::max(ev.firstDay, dayOf(e));
			if (desc) ev.description = db->text(r.id, desc->id);
			events.push_back(std::move(ev));
		}
		const std::string name = view->name.empty() ? db->name : view->name;
		const std::filesystem::path file = exportFileFor(s_ws.project(), name, "ics", "Calendar");
		std::string err;
		if (s_ws.project().empty()) err = "no project is open";
		else if (writeExport(file, eventsToIcs(events, name, taskNowMs()), &err))
		{
			HE::Ed::notify(HE::Ed::NoteLevel::Info, std::to_string(events.size()) + " events exported", file.string());
			std::string url = "file://";
			for (const unsigned char c : file.generic_string())
			{
				if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') url += static_cast<char>(c);
				else { char hex[4]; std::snprintf(hex, sizeof hex, "%%%02X", c); url += hex; }
			}
			if (open && !SDL_OpenURL(url.c_str()))
				err = std::string("the calendar app did not open it: ") + SDL_GetError();
		}
		if (!err.empty()) HE::Ed::notify(HE::Ed::NoteLevel::Important, "Calendar not exported: " + err);
	});
}

// The right end of a view's toolbar: how its rows are grouped, filtered and shown, and the view's settings.
// The left end makes things and finds them (New, the search, the view's own navigation).
void toolbarRight(const Database& db, const View& view)
{
	const UUID dbId = db.id, viewId = view.id;
	const bool board = view.kind == ViewKind::Board;
	const bool dated = view.kind == ViewKind::Timeline || view.kind == ViewKind::Calendar;
	const Property* group = board ? db.findProperty(view.groupBy) : nullptr;
	const std::string groupLabel  = std::string("Group: ") + (group ? group->name : std::string("None"));
	const std::string filterLabel = view.filters.empty() ? std::string("Filter") : "Filter " + std::to_string(view.filters.size());
	const float sp = ImGui::GetStyle().ItemSpacing.x;
	float w = toolWidth(filterLabel.c_str()) + sp + toolWidth("Properties");
	if (board) w += toolWidth(groupLabel.c_str()) + sp;
	if (dated) w += toolWidth("Settings") + sp + toolWidth("Export") + sp;
	ImGui::SameLine();
	if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();   // a narrow window: the group gets a line of its own
	alignRight(w);
	if (board)
	{
		if (toolButton("Planner/Group By", groupLabel.c_str(), PlannerIcons::Icon::Kanban, false)) ImGui::OpenPopup("##groupby");
		note("groupby");
		ImGui::SameLine();
	}
	if (toolButton("Planner/Filter", filterLabel.c_str(), PlannerIcons::Icon::Filter, !view.filters.empty())) ImGui::OpenPopup("##filters");
	note("Filter");
	ImGui::SameLine();
	if (toolButton("Planner/Properties", "Properties", PlannerIcons::Icon::Sliders, false)) ImGui::OpenPopup("##properties");
	note("Properties");
	if (dated)
	{
		ImGui::SameLine();
		if (toolButton("Planner/Timeline Settings", "Settings", PlannerIcons::Icon::Gear, false)) ImGui::OpenPopup("##viewsettings");
		note("timeline:settings");
		ImGui::SameLine();
		if (toolButton("Planner/Export Calendar", "Export", PlannerIcons::Icon::Export, false)) ImGui::OpenPopup("##calexport");
		note("calendar:export");
	}
	if (beginPopup("##calexport"))
	{
		if (ImGui::Selectable("iCalendar file (.ics)")) exportCalendar(dbId, viewId, false);
		note("export:ics");
#ifdef __APPLE__
		if (ImGui::Selectable("Add to Calendar")) exportCalendar(dbId, viewId, true);
		note("export:calendar-app");
#endif
		ImGui::TextDisabled("Every dated row this view shows, as all-day events.");
		ImGui::EndPopup();
	}

	if (beginPopup("##groupby"))
	{
		ImGui::TextDisabled("Lanes from");
		for (const Property& p : db.properties)
			if (p.type == PropertyType::Select && ImGui::Selectable(p.name.c_str(), group == &p))
			{
				const UUID pid = p.id;
				editView(dbId, viewId, [pid](View& v) { v.groupBy = pid; return true; });
			}
		ImGui::EndPopup();
	}
	if (beginPopup("##filters")) { drawFilterPopup(db, view); ImGui::EndPopup(); }
	if (beginPopup("##properties")) { drawPropertiesPopup(db, view); ImGui::EndPopup(); }
	else s_armedDelete = UUID{};
	if (beginPopup("##viewsettings"))
	{
		const bool cal = view.kind == ViewKind::Calendar;
		dateCombo(db, dbId, viewId, cal ? "Day from" : "Starts at", "##tlstart", cal ? "Date" : "Start", view.dateProperty, false);
		note("timeline:start");
		dateCombo(db, dbId, viewId, cal ? "Until" : "Ends at", "##tlend", "End", view.endProperty, true);
		ImGui::TextUnformatted("Colour by");
		ImGui::SameLine(90);
		ImGui::SetNextItemWidth(160);
		const Property* colour = db.findProperty(view.groupBy);
		if (ImGui::BeginCombo("##tlcolour", colour ? colour->name.c_str() : "Nothing"))
		{
			if (ImGui::Selectable("(none)", colour == nullptr))
				editView(dbId, viewId, [](View& v) { v.groupBy = UUID{}; return true; });
			for (const Property& p : db.properties)
				if (p.type == PropertyType::Select && ImGui::Selectable(p.name.c_str(), colour == &p))
				{
					const UUID pid = p.id;
					editView(dbId, viewId, [pid](View& v) { v.groupBy = pid; return true; });
				}
			ImGui::EndCombo();
		}
		ImGui::EndPopup();
	}
}

void drawDatabase(AppContext& ctx, const Database& db)
{
	const UUID dbId = db.id;
	// Which view is open: the one the sidebar picked, else the first.
	const View* view = db.findView(s_viewId);
	if (!view) { view = preferredView(db); if (view) s_viewId = view->id; }

	// What you opened is a table, a timeline, a board or a calendar: its name is the title. The data it
	// shows has a name too, behind the button.
	if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
	if (view)
	{
		const UUID vid = view->id;
		textCell("##viewtitle", { vid, UUID{ 0, 10 } }, view->name, [=](const std::string& name) {
			editView(dbId, vid, [name](View& vv) {
				if (name.empty() || name == vv.name) return false;
				vv.name = name;
				return true;
			});
		}, "Untitled", 0.0f, 360.0f);
	}
	if (ctx.fontHeading) ImGui::PopFont();
	// The data behind the view, at the right end of the title line.
	const std::string dataLabel = std::string("Data: ") + (db.name.empty() ? "Untitled" : db.name);
	ImGui::SameLine();
	alignRight(toolWidth(dataLabel.c_str()));
	if (toolButton("Planner/Data", dataLabel.c_str(), PlannerIcons::Icon::Table, false))
		ImGui::OpenPopup("##datamenu");
	note("data:menu");
	if (beginPopup("##datamenu"))
	{
		ImGui::TextDisabled("The data behind this view");
		textCell("##dbname", { dbId, UUID{} }, db.name, [=](const std::string& name) {
			post([=] {
				s_ws.edit(dbId, [&](Database& d) {
					if (name.empty() || name == d.name) return false;
					d.name = name;
					return true;
				});
			});
		}, "Untitled", 0.0f, 240.0f);
		ImGui::TextDisabled("%d rows, %d views.", static_cast<int>(db.rows.size()), static_cast<int>(db.views.size()));
		ImGui::TextDisabled("Another table, timeline, board or calendar can show");
		ImGui::TextDisabled("the same rows: + in its sidebar section, \"data\" choice.");
		ImGui::EndPopup();
	}

	if (!view) return;
	const UUID viewId = view->id;

	// Toolbar: the button makes what this view shows. Tasks has no dates (the Task Board file has none), so a
	// timeline or calendar cannot add to it.
	const std::int64_t today = dayOf(taskNowMs());
	const bool noDates = dbId == tasksDatabaseId();
	if (view->kind == ViewKind::Timeline)
	{
		ImGui::BeginDisabled(noDates);
		if (EditorWidgets::primaryButton("New Step")) newDated(dbId, view->id, today, today + 2);
		ImGui::EndDisabled();
	}
	else if (view->kind == ViewKind::Calendar)
	{
		ImGui::BeginDisabled(noDates);
		if (EditorWidgets::primaryButton("New Event"))
		{
			int y = 0, m = 0, dn = 0;
			splitDay(today, y, m, dn);
			// Today when it is on the month shown, else the first day of that month.
			const std::int64_t day = (s_calMonth == 0 || (y == s_calYear && m == s_calMonth)) ? today : firstOfMonth(s_calYear, s_calMonth);
			newDated(dbId, view->id, day, day);
		}
		ImGui::EndDisabled();
	}
	else if (view->kind == ViewKind::Board)
	{
		if (EditorWidgets::primaryButton("New Item"))
		{
			const UUID groupId = view->groupBy;
			post([=] {
				UUID made;
				s_ws.edit(dbId, [&](Database& d) {
					Row* r = d.addRow("New card", s_ws.author(), taskNowMs());
					if (!r) return false;
					made = r->id;
					const Property* g = d.findProperty(groupId);
					if (g && g->type == PropertyType::Select && !g->options.empty()) d.setSelect(made, groupId, g->options.front().id, taskNowMs());
					return true;
				});
				if (!(made == UUID{})) s_selRow = made;
			});
		}
	}
	else if (EditorWidgets::primaryButton("New Row"))
		post([=] {
			s_ws.edit(dbId, [&](Database& d) { return d.addRow("", s_ws.author(), taskNowMs()) != nullptr; });
		});
	note("New Row");
	ImGui::SameLine(0, 10);
	ImGui::SetNextItemWidth(220);
	{
		// A magnifier inside the field, at its left.
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(28.0f, ImGui::GetStyle().FramePadding.y));
		ImGui::InputTextWithHint("##search", "Search titles", &s_search);
		ImGui::PopStyleVar();
		const ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
		PlannerIcons::drawIcon(ImGui::GetWindowDrawList(), PlannerIcons::Icon::Search, ImVec2(a.x + 15, (a.y + z.y) * 0.5f), Theme::u32(Theme::TextDim));
	}
	// "Assigned to me", where the data has people: who "me" is, is the team member we are signed in as
	// (TaskBoardPanel::author() follows it), else the name this editor writes under.
	const Property* people = nullptr;
	for (const Property& p : db.properties)
		if (p.type == PropertyType::Person) { people = &p; break; }
	const std::string me = TaskBoardPanel::author();
	if (people && !me.empty())
	{
		ImGui::SameLine();
		if (toolButton("Planner/Assigned to me", "Mine", PlannerIcons::Icon::Person, s_mineOnly)) s_mineOnly = !s_mineOnly;
		note("mine");
	}
	if (view->kind == ViewKind::Timeline || view->kind == ViewKind::Calendar)
	{
		// The same dated rows along a time axis or as a month: one view, shown either way.
		ImGui::SameLine(0, 10);
		const char* const modes[] = { "Timeline", "Month" };
		const int hit = segmented("mode", modes, 2, view->kind == ViewKind::Calendar ? 1 : 0, "Planner/Timeline or Month");
		if (hit >= 0)
		{
			const ViewKind kind = hit == 1 ? ViewKind::Calendar : ViewKind::Timeline;
			editView(dbId, viewId, [kind](View& v) {
				if (v.kind == kind) return false;
				v.kind = kind;
				return true;
			});
		}
	}
	if (view->kind == ViewKind::Timeline)
	{
		// How wide a day is, a way back to today, and the rarely changed settings in a popup.
		ImGui::SameLine(0, 10);
		{
			const char* const names[] = { "Days", "Weeks", "Months" };
			int on = -1;
			for (int i = 0; i < 3; ++i) if (std::abs(s_tlPpd - kZoomDayW[i]) < 0.5f) on = i;
			const int hit = segmented("zoom", names, 3, on, "Planner/Timeline Zoom");
			if (hit >= 0) s_tlPending = kZoomDayW[hit];
		}
		ImGui::SameLine();
		if (toolButton("Planner/Today", "Today", PlannerIcons::Icon::Calendar, false)) s_tlGoToday = true;
		note("timeline:today");
		{
			const float bh = ImGui::GetFrameHeight();
			ImGui::SameLine();
			if (PlannerIcons::iconButton("Timeline Zoom Out", PlannerIcons::Icon::ZoomOut, false, true, "Planner/Zoom Out", false, 0, ImVec2(bh, bh))) s_tlPending = s_tlPpd / 1.5f;
			note("timeline:zoomout");
			ImGui::SameLine(0, 2);
			if (PlannerIcons::iconButton("Timeline Zoom In", PlannerIcons::Icon::ZoomIn, false, true, "Planner/Zoom In", false, 0, ImVec2(bh, bh))) s_tlPending = s_tlPpd * 1.5f;
			note("timeline:zoomin");
			ImGui::SameLine(0, 2);
			if (PlannerIcons::iconButton("Timeline Fit", PlannerIcons::Icon::Fit, false, true, "Planner/Fit", false, 0, ImVec2(bh, bh))) s_tlFit = true;
			note("timeline:fit");
		}
	}
	if (view->kind == ViewKind::Calendar)
	{
		// The month shown with arrows either side, a way back to today, and the rarely changed settings in a popup.
		const float bh = ImGui::GetFrameHeight();
		ImGui::SameLine();
		if (PlannerIcons::iconButton("Previous", PlannerIcons::Icon::ChevronLeft, false, true, "Planner/Previous", false, 0, ImVec2(bh, bh))) post([] { stepMonth(-1); });
		note("cal:previous");
		ImGui::SameLine(0, 2);
		{
			static const char* const kMonths[] = { "January", "February", "March", "April", "May", "June", "July",
			                                       "August", "September", "October", "November", "December" };
			const std::string label = s_calMonth != 0 ? std::string(kMonths[s_calMonth - 1]) + " " + std::to_string(s_calYear) : std::string("   ");
			const float w = 150.0f;
			const ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(w, bh));
			const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
			ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (bh - ts.y) * 0.5f), Theme::u32(Theme::Text), label.c_str());
		}
		ImGui::SameLine(0, 2);
		if (PlannerIcons::iconButton("Next", PlannerIcons::Icon::ChevronRight, false, true, "Planner/Next", false, 0, ImVec2(bh, bh))) post([] { stepMonth(1); });
		note("cal:next");
		ImGui::SameLine();
		if (toolButton("Planner/Today", "Today", PlannerIcons::Icon::Calendar, false))
			post([] {
				int d = 0;
				splitDay(dayOf(taskNowMs()), s_calYear, s_calMonth, d);
			});
	}
	toolbarRight(db, *view);
	ImGui::Dummy(ImVec2(0, 2));
	// The rows this view shows: its own filters and sorts, plus the search box.
	View effective = *view;
	if (!s_search.empty())
		effective.filters.push_back({ UUID{}, "contains", nlohmann::json(s_search).dump() });
	if (s_mineOnly && people && !me.empty())
		effective.filters.push_back({ people->id, "contains", nlohmann::json(me).dump() });
	const std::vector<std::size_t> idx = db.query(effective);
	s_stats.rows = static_cast<int>(idx.size());

	// The table or the board, and on its right the details of one row.
	const Row* open = db.findRow(s_selRow);
	if (!open) s_selRow = UUID{};
	constexpr float kDrawerW = 380.0f;
	// A table keeps the details beside it; a timeline, calendar or board shows them in a flyover next to
	// where you clicked, so the plan keeps its whole width.
	const bool docked = open && view->kind == ViewKind::Table;
	ImGui::BeginChild("##plannerbody", ImVec2(docked ? -(kDrawerW + ImGui::GetStyle().ItemSpacing.x) : 0.0f, 0.0f),
	                  ImGuiChildFlags_None);
	switch (view->kind)
	{
	case ViewKind::Table: drawTable(db, effective, idx); break;
	case ViewKind::Board: drawBoard(db, effective);      break;
	case ViewKind::Timeline: drawTimeline(db, effective, idx); break;
	case ViewKind::Calendar: drawCalendar(db, effective, idx); break;
	default:
		ImGui::TextDisabled("This view type is not available in this editor yet.");
		break;
	}
	ImGui::EndChild();
	if (docked)
	{
		ImGui::SameLine();
		ImGui::BeginChild("##plannerdrawer", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
		drawDrawer(db, *open, true);
		ImGui::EndChild();
	}
	else if (open)
	{
		const ImGuiIO& io = ImGui::GetIO();
		const bool newRow = !(s_flyRow == s_selRow);
		s_flyRow = s_selRow;
		if (newRow) s_flyAt = io.MousePos;   // where the click was
		const ImVec2 wmin = ImGui::GetWindowPos(), wmax(wmin.x + ImGui::GetWindowWidth(), wmin.y + ImGui::GetWindowHeight());
		const ImVec2 at(std::clamp(s_flyAt.x + 14.0f, wmin.x + 8.0f, std::max(wmin.x + 8.0f, wmax.x - kDrawerW - 8.0f)),
		                std::clamp(s_flyAt.y + 8.0f, wmin.y + 8.0f, std::max(wmin.y + 8.0f, wmax.y - 320.0f)));
		ImGui::SetNextWindowPos(at, newRow ? ImGuiCond_Always : ImGuiCond_Appearing);
		// As tall as its content, at most the panel's height (the rest scrolls), and kept inside the panel
		// every frame: a growing history must not push it past the editor window's bottom edge.
		ImGui::SetNextWindowSizeConstraints(ImVec2(kDrawerW, 160.0f), ImVec2(kDrawerW, std::max(160.0f, wmax.y - wmin.y - 16.0f)));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::alpha(Theme::warm(0.11f), 0.985f));
		ImGui::PushStyleColor(ImGuiCol_Border, Theme::warm(0.34f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
		bool closeFly = false;
		if (ImGui::Begin("##rowflyover", nullptr,
		                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
		{
			keepInside(ImVec2(wmin.x + 8.0f, wmin.y + 8.0f), ImVec2(wmax.x - 8.0f, wmax.y - 8.0f));
			ImGui::TextDisabled("Details");
			ImGui::SameLine(ImGui::GetWindowWidth() - 36.0f);
			if (PlannerIcons::iconButton("Close Details", PlannerIcons::Icon::Close, false, true, "Planner/Close Details", false, 0, ImVec2(22, 22))) closeFly = true;
			note("flyover:close");
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)) closeFly = true;
			ImGui::Separator();
			note("flyover");
			drawDrawer(db, *open, false);
		}
		ImGui::End();
		ImGui::PopStyleVar(3);
		ImGui::PopStyleColor(2);
		if (closeFly) s_selRow = UUID{};
	}
}

// ── The sidebar ──────────────────────────────────────────────────────────────

// What the others did to tasks that concern you. Only once something has come
// in; the bell has the same lines, this list is the one that takes you to the task.
void drawTeamNotices(AppContext& ctx)
{
	HE::TaskBoardTeam* team = TaskBoardPanel::team();
	if (!team || team->recent().empty()) return;
	ImGui::Spacing();
	int fresh = 0;
	for (const auto& r : team->recent()) fresh += r.atMs > TaskBoardPanel::teamSeen();
	const bool openList = EditorWidgets::smallButton("From the Team");
	note("From the Team");
	// Under the button, not at the mouse, where it would cover it.
	const ImVec2 listAt(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 2.0f);
	if (openList)
	{
		ImGui::OpenPopup("##fromTeam");
		TaskBoardPanel::markTeamSeen(team->recent().front().atMs);
	}
	ImGui::SameLine();
	if (fresh > 0) ImGui::TextColored(Theme::AccentBright, "%d new", fresh);
	else           ImGui::TextDisabled("%d", static_cast<int>(team->recent().size()));
	ImGui::SetNextWindowPos(listAt, ImGuiCond_Appearing);
	if (!beginPopup("##fromTeam")) return;
	TaskBoard* board = TaskBoardPanel::board(ctx);
	int row = 0;
	for (const auto& r : team->recent())
	{
		const bool here = board && board->find(r.notice.task) != nullptr;
		ImGui::PushID(row++);
		const bool clicked = ImGui::Selectable(r.text.c_str(), false, here ? 0 : ImGuiSelectableFlags_Disabled);
		note("team-row:" + std::to_string(row - 1));
		if (clicked && here) openTask(r.notice.task);
		ImGui::PopID();
		if (!r.notice.text.empty())
		{
			ImGui::Indent();
			ImGui::TextDisabled("%s", r.notice.text.c_str());
			ImGui::Unindent();
		}
		// A task created on another machine is on this board after the next pull;
		// until then there is nothing to open. One from another of the team's
		// projects is on that project's board.
		if (!here)
		{
			ImGui::Indent();
			if (!r.notice.project.empty() && r.notice.project != TaskBoardPanel::projectName())
				ImGui::TextDisabled("In project %s.", r.notice.project.c_str());
			else
				ImGui::TextDisabled("Not on this board yet: pull to see it.");
			ImGui::Unindent();
		}
	}
	ImGui::EndPopup();
}

// The pages without a parent, and under each the pages that name it as theirs.
void sidebarPages(const std::vector<Page*>& all, const UUID& parent, int depth)
{
	for (const Page* p : all)
	{
		if (!(p->parent == parent)) continue;
		pushUuid(p->id);
		if (depth > 0) ImGui::Indent(static_cast<float>(depth) * 12.0f);
		const std::string label = p->title.empty() ? std::string("Untitled") : p->title;
		if (sidebarRow("##page", PlannerIcons::Icon::Page, label, p->id == s_pageId && s_pageDb == UUID{} && s_boardId == UUID{}))
		{
			s_pageId = p->id;
			s_pageDb = UUID{};
			s_boardId = UUID{};
		}
		note("page:" + p->title);
		if (beginPopupContextItem("##pagemenu"))
		{
			if (EditorWidgets::dangerMenuItem("Delete Page")) s_deletePage = p->id;
			note("menu:Delete Page");
			ImGui::EndPopup();
		}
		if (depth > 0) ImGui::Unindent(static_cast<float>(depth) * 12.0f);
		sidebarPages(all, p->id, depth + 1);
		ImGui::PopID();
	}
}

void drawSidebar(AppContext& ctx)
{
	if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
	ImGui::TextUnformatted("Planner");
	if (ctx.fontSubheading) ImGui::PopFont();
	ImGui::Spacing();

	// Tables, timelines, boards and calendars: one entry per view, each kind under its own title. Views of the
	// same data sit under different titles; the tooltip says which data an entry shows.
	{
		// A timeline and a calendar show the same dated rows, one as bars along a time axis, one as a month:
		// one section, and each such view switches between the two with its Timeline / Month toggle.
		const struct { ViewKind kind; const char* title; const char* newName; const char* key; const char* empty; } kinds[] = {
			{ ViewKind::Table, "TABLES", "New Table", "Planner/New Table", "No tables yet" },
			{ ViewKind::Timeline, "TIMELINES & CALENDARS", "New Timeline", "Planner/New Timeline", "Nothing scheduled yet" } };
		for (const auto& k : kinds)
		{
			if (sidebarSection(k.title, k.newName, k.key))
			{
				s_newKind = k.kind;
				ImGui::OpenPopup("##newdata");
			}
			int listed = 0;
			for (Database* db : s_ws.databases())
				for (const View& v : db->views)
				{
					// A board is a table shown in lanes: it lists with the tables (there are whiteboards for free-form boards).
					const bool here = k.kind == ViewKind::Table ? (v.kind == ViewKind::Table || v.kind == ViewKind::Board)
					                                            : (v.kind == ViewKind::Timeline || v.kind == ViewKind::Calendar);
					if (!here || hiddenInSidebar(*db, v)) continue;
					++listed;
					const bool isTasks = db->id == tasksDatabaseId();
					pushUuid(v.id);
					const std::string label = isTasks ? std::string("Tasks") : v.name.empty() ? std::string("Untitled") : v.name;
					const bool open = (isTasks ? db->id == s_dbId : db->id == s_dbId && v.id == s_viewId) && s_pageId == UUID{} && s_boardId == UUID{};
					if (sidebarRow("##view", viewIcon(v.kind), label, open))
					{
						s_dbId   = db->id;
						s_viewId = v.id;
						s_pageId = s_pageDb = UUID{};
						s_boardId = UUID{};
					}
					note("item:" + label);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Data: %s (%d rows, %d views)", db->name.c_str(), static_cast<int>(db->rows.size()),
						                  static_cast<int>(db->views.size()));
					if (!isTasks && beginPopupContextItem("##viewitemmenu"))
					{
						const UUID dbId = db->id, vid = v.id;
						if (db->views.size() > 1)
						{
							if (EditorWidgets::dangerMenuItem("Delete View"))
								post([=] {
									s_ws.edit(dbId, [&](Database& d) { return d.removeView(vid); });
									if (s_viewId == vid) s_viewId = UUID{};
								});
						}
						else if (EditorWidgets::dangerMenuItem("Delete Data", !(dbId == tasksDatabaseId())))
							s_deleteData = dbId;
						ImGui::EndPopup();
					}
					ImGui::PopID();
				}
			if (listed == 0) sidebarEmpty(k.empty);
		}
		if (beginPopup("##newdata"))
		{
			ImGui::TextDisabled("A %s of", kindWord(s_newKind));
			if (ImGui::Selectable("New data")) newView(s_newKind, UUID{});
			note("data:new");
			if (!s_ws.databases().empty()) ImGui::Separator();
			for (Database* db : s_ws.databases())
			{
				ImGui::PushID(db);
				// Tasks is the Task Board file, which has no dates: nothing to lay out on a timeline or calendar.
				const bool noDates = db->id == tasksDatabaseId() && (s_newKind == ViewKind::Timeline || s_newKind == ViewKind::Calendar);
				if (ImGui::Selectable((db->name.empty() ? std::string("Untitled") : db->name).c_str(), false, noDates ? ImGuiSelectableFlags_Disabled : 0))
					newView(s_newKind, db->id);
				note("data:" + db->name);
				if (noDates && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The Task Board has no dates");
				ImGui::SameLine();
				ImGui::TextDisabled("%d rows", static_cast<int>(db->rows.size()));
				ImGui::PopID();
			}
			ImGui::EndPopup();
		}
		if (!(s_deleteData == UUID{}) && !s_deleteDataOpen)
		{
			ImGui::OpenPopup("##confirmDeleteData");
			s_deleteDataOpen = true;
		}
		if (beginPopup("##confirmDeleteData"))
		{
			const Database* doomed = s_ws.find(s_deleteData);
			ImGui::Text("Delete \"%s\" and its rows?", doomed ? (doomed->name.empty() ? "Untitled" : doomed->name.c_str()) : "");
			ImGui::TextDisabled("Source control has the previous version.");
			if (EditorWidgets::dangerButton("Delete"))
			{
				const UUID gone = s_deleteData;
				s_deleteData = UUID{};
				post([=] {
					s_ws.remove(gone);
					if (s_dbId == gone) { s_dbId = UUID{}; s_viewId = UUID{}; }
				});
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (EditorWidgets::cancelButton("Cancel")) { s_deleteData = UUID{}; ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}
		else if (s_deleteDataOpen)
			s_deleteDataOpen = false;
		if (!s_deleteDataOpen) s_deleteData = UUID{};
	}

	// Pages: write-ups that are not a row of anything (the design document, a level concept).
	if (sidebarSection("PAGES", "New Page", "Planner/New Page"))
		post([] {
			if (Page* page = s_ws.createPage("Untitled"))
			{
				s_pageId = page->id;
				s_pageDb = UUID{};
			}
		});
	sidebarPages(s_ws.pages(), UUID{}, 0);
	{
		bool anyPage = false;
		for (Page* pg : s_ws.pages()) anyPage |= pg->parent == UUID{};
		if (!anyPage) sidebarEmpty("No pages yet");
	}

	// Whiteboards: cards and arrows on a free surface.
	if (sidebarSection("WHITEBOARDS", "New Board", "Planner/New Board"))
		post([] {
			if (Board* board = s_ws.createBoard("Untitled board"))
			{
				s_boardId = board->id;
				s_pageId = s_pageDb = UUID{};
			}
		});
	if (s_ws.boards().empty()) sidebarEmpty("No whiteboards yet");
	for (Board* b : s_ws.boards())
	{
		pushUuid(b->id);
		const std::string label = b->title.empty() ? std::string("Untitled board") : b->title;
		if (sidebarRow("##board", PlannerIcons::Icon::Whiteboard, label, b->id == s_boardId))
		{
			s_boardId = b->id;
			s_pageId = s_pageDb = UUID{};
		}
		note("board:" + b->title);
		if (beginPopupContextItem("##boardmenu"))
		{
			if (EditorWidgets::dangerMenuItem("Delete Board")) s_deleteBoard = b->id;
			ImGui::EndPopup();
		}
		ImGui::PopID();
	}
	// Opened once: asking again every frame would bring it back after the answer.
	if (!(s_deleteBoard == UUID{}) && !s_deleteBoardOpen)
	{
		ImGui::OpenPopup("##confirmDeleteBoard");
		s_deleteBoardOpen = true;
	}
	if (beginPopup("##confirmDeleteBoard"))
	{
		const Board* doomed = s_ws.findBoard(s_deleteBoard);
		ImGui::Text("Delete \"%s\"?", doomed ? (doomed->title.empty() ? "Untitled" : doomed->title.c_str()) : "");
		ImGui::TextDisabled("Source control has the previous version.");
		if (EditorWidgets::dangerButton("Delete"))
		{
			const UUID gone = s_deleteBoard;
			s_deleteBoard = UUID{};
			post([=] {
				s_ws.removeBoard(gone);
				PlannerBoardView::forget(gone);
				if (s_boardId == gone) s_boardId = UUID{};
			});
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (EditorWidgets::cancelButton("Cancel")) { s_deleteBoard = UUID{}; ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}
	else if (s_deleteBoardOpen)
		s_deleteBoardOpen = false;
	if (!s_deleteBoardOpen) s_deleteBoard = UUID{};
	if (s_deleteBoard == UUID{} && s_deleteBoardOpen && !ImGui::IsPopupOpen("##confirmDeleteBoard")) s_deleteBoardOpen = false;
	// Opened once: asking again every frame would bring it back after the answer.
	if (!(s_deletePage == UUID{}) && !s_deletePageOpen)
	{
		ImGui::OpenPopup("##confirmDeletePage");
		s_deletePageOpen = true;
	}
	if (beginPopup("##confirmDeletePage"))
	{
		const Page* doomed = s_ws.findPage(s_deletePage);
		ImGui::Text("Delete \"%s\"?", doomed ? (doomed->title.empty() ? "Untitled" : doomed->title.c_str()) : "");
		ImGui::TextDisabled("Its sub-pages go with it. Source control has the previous version.");
		const bool confirmed = EditorWidgets::dangerButton("Delete");
		note("confirm:Delete Page");
		if (confirmed)
		{
			const UUID gone = s_deletePage;
			s_deletePage = UUID{};
			post([=] {
				s_ws.removePage(gone);
				if (s_pageId == gone) s_pageId = s_pageDb = UUID{};
			});
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (EditorWidgets::cancelButton("Cancel")) { s_deletePage = UUID{}; ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}
	else if (s_deletePageOpen)
		s_deletePageOpen = false;
	if (!s_deletePageOpen) s_deletePage = UUID{};

	drawTeamNotices(ctx);

	for (const Workspace::Unreadable& u : s_ws.unreadableBoards())
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, Theme::Bad);
		EditorWidgets::WrapText wrap(ImGui::GetContentRegionAvail().x);
		ImGui::Text("board %s.json cannot be read: %s", uuidToHex(u.id).substr(0, 8).c_str(), u.error.c_str());
		ImGui::PopStyleColor();
	}
	for (const Workspace::Unreadable& u : s_ws.unreadable())
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, Theme::Bad);
		EditorWidgets::WrapText wrap(ImGui::GetContentRegionAvail().x);
		ImGui::Text("%s.json cannot be read: %s", uuidToHex(u.id).substr(0, 8).c_str(), u.error.c_str());
		ImGui::PopStyleColor();
	}
}

} // namespace

// ── The tab ──────────────────────────────────────────────────────────────────

// ── Open things, as tabs ──────────────────────────────────────────────────────
// Whatever is open (a table, a timeline, a board, a calendar, a page, a whiteboard) has one tab; opening
// something that is open switches to it. The tabs follow the selection: whatever is selected is open.
struct Tab { int kind = -1; UUID a, b; };   // 0 view (a: data, b: view), 1 page (a: page, b: its data or none), 2 whiteboard (a)
std::vector<Tab> s_tabs;

bool sameTab(const Tab& x, const Tab& y) { return x.kind == y.kind && x.a == y.a && x.b == y.b; }

Tab currentTab()
{
	Tab t;
	if (!(s_boardId == UUID{})) { t.kind = 2; t.a = s_boardId; }
	else if (!(s_pageId == UUID{})) { t.kind = 1; t.a = s_pageId; t.b = s_pageDb; }
	else if (const Database* db = s_ws.find(s_dbId))
		if (const View* v = db->findView(s_viewId)) { t.kind = 0; t.a = db->id; t.b = v->id; }
	return t;
}

void selectTab(const Tab& t)
{
	s_boardId = s_pageId = s_pageDb = UUID{};
	if (t.kind == 0) { s_dbId = t.a; s_viewId = t.b; }
	if (t.kind == 1) { s_pageId = t.a; s_pageDb = t.b; }
	if (t.kind == 2) s_boardId = t.a;
}

// A tab's name and glyph; false when what it stood for is gone.
bool tabInfo(const Tab& t, std::string& name, PlannerIcons::Icon& icon)
{
	if (t.kind == 0)
	{
		const Database* db = s_ws.find(t.a);
		const View* v = db ? db->findView(t.b) : nullptr;
		if (!v) return false;
		name = v->name.empty() ? std::string("Untitled") : v->name;
		icon = viewIcon(v->kind);
		return true;
	}
	if (t.kind == 1)
	{
		const Page* p = s_ws.findPage(t.a);
		if (!p) return false;
		name = p->title.empty() ? std::string("Untitled") : p->title;
		icon = PlannerIcons::Icon::Page;
		return true;
	}
	if (t.kind == 2)
	{
		const Board* b = s_ws.findBoard(t.a);
		if (!b) return false;
		name = b->title.empty() ? std::string("Untitled board") : b->title;
		icon = PlannerIcons::Icon::Whiteboard;
		return true;
	}
	return false;
}

void drawTabs()
{
	const Tab cur = currentTab();
	if (cur.kind >= 0 && std::none_of(s_tabs.begin(), s_tabs.end(), [&](const Tab& t) { return sameTab(t, cur); })) s_tabs.push_back(cur);
	{
		std::string n;
		PlannerIcons::Icon ic;
		s_tabs.erase(std::remove_if(s_tabs.begin(), s_tabs.end(), [&](const Tab& t) { return !tabInfo(t, n, ic); }), s_tabs.end());
	}

	const float avail = ImGui::GetContentRegionAvail().x - 80.0f;   // room for the + and the reload button
	std::vector<float> widths;
	float total = 0;
	for (const Tab& t : s_tabs)
	{
		std::string n;
		PlannerIcons::Icon ic;
		tabInfo(t, n, ic);
		widths.push_back(std::min(220.0f, ImGui::CalcTextSize(n.c_str()).x + 56.0f));
		total += widths.back() + 6.0f;
	}
	const float scale = total > avail && total > 0 ? avail / total : 1.0f;

	const ImVec2 row0 = ImGui::GetCursorScreenPos();
	float x = row0.x;
	int close = -1;
	for (std::size_t i = 0; i < s_tabs.size(); ++i)
	{
		const Tab& t = s_tabs[i];
		std::string name;
		PlannerIcons::Icon icon;
		tabInfo(t, name, icon);
		const float w = std::max(70.0f, widths[i] * scale);
		const bool selected = sameTab(t, cur);
		ImGui::PushID(static_cast<int>(i));
		ImGui::SetCursorScreenPos(ImVec2(x, row0.y));
		ImGui::SetNextItemAllowOverlap();
		if (ImGui::InvisibleButton("##tab", ImVec2(w, 28.0f))) selectTab(t);
		note(std::string(t.kind == 0 ? "view:" : t.kind == 1 ? "page-tab:" : "board-tab:") + name);
		const bool hover = ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		// Drawn here, within the tab's own width, so neighbours never overlap (a Selectable paints past its edges).
		if (selected || hover)
			dl->AddRectFilled(ImVec2(x, row0.y), ImVec2(x + w, row0.y + 28.0f), Theme::u32(selected ? Theme::alpha(Theme::Accent, 0.16f) : Theme::warm(0.16f)), 6.0f);
		PlannerIcons::drawIcon(dl, icon, ImVec2(x + 15, row0.y + 14), Theme::u32(selected ? Theme::AccentBright : Theme::TextDim));
		dl->PushClipRect(ImVec2(x, row0.y), ImVec2(x + w - (s_tabs.size() > 1 ? 22.0f : 6.0f), row0.y + 28), true);
		dl->AddText(ImVec2(x + 28, row0.y + (28.0f - ImGui::GetFontSize()) * 0.5f), Theme::u32(selected ? Theme::Text : Theme::TextDim), name.c_str());
		dl->PopClipRect();
		if (selected) dl->AddRectFilled(ImVec2(x + 4, row0.y + 26), ImVec2(x + w - 4, row0.y + 28), Theme::u32(Theme::Accent), 1.0f);
		if (s_tabs.size() > 1)
		{
			// The x closes the tab (not the thing: it is still in the sidebar).
			ImGui::SetCursorScreenPos(ImVec2(x + w - 22.0f, row0.y + 6.0f));
			if (ImGui::InvisibleButton("##close", ImVec2(16, 16))) close = static_cast<int>(i);
			note("close:" + name);
			const bool overX = ImGui::IsItemHovered();
			if (overX) dl->AddRectFilled(ImVec2(x + w - 22.0f, row0.y + 6.0f), ImVec2(x + w - 6.0f, row0.y + 22.0f), Theme::u32(Theme::warm(0.30f)), 4.0f);
			if (hover || selected || overX)
			{
				const ImVec2 c(x + w - 14.0f, row0.y + 14.0f);
				const ImU32 ink = Theme::u32(overX ? Theme::Text : Theme::TextDim);
				dl->AddLine(ImVec2(c.x - 3, c.y - 3), ImVec2(c.x + 3, c.y + 3), ink, 1.5f);
				dl->AddLine(ImVec2(c.x - 3, c.y + 3), ImVec2(c.x + 3, c.y - 3), ink, 1.5f);
			}
		}
		ImGui::PopID();
		x += w + 6.0f;
	}
	// The + lists everything there is to open.
	ImGui::SetCursorScreenPos(ImVec2(x + 4.0f, row0.y + 1.0f));
	if (PlannerIcons::iconButton("Open", PlannerIcons::Icon::Plus, false, true, "Planner/Open", false, 0, ImVec2(26, 26))) ImGui::OpenPopup("##openthing");
	note("Open");
	if (beginPopup("##openthing"))
	{
		auto entry = [&](const Tab& t) {
			std::string name;
			PlannerIcons::Icon icon;
			if (!tabInfo(t, name, icon)) return;
			const bool isOpen = std::any_of(s_tabs.begin(), s_tabs.end(), [&](const Tab& o) { return sameTab(o, t); });
			const ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::PushID(name.c_str());
			if (ImGui::Selectable((std::string("      ") + name).c_str(), sameTab(t, cur))) selectTab(t);
			note("open:" + name);
			PlannerIcons::drawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(p.x + 12, p.y + ImGui::GetTextLineHeight() * 0.5f + 1),
			                       Theme::u32(isOpen ? Theme::AccentBright : Theme::TextDim));
			ImGui::PopID();
		};
		const struct { ViewKind kind; const char* title; } kinds[] = { { ViewKind::Table, "Tables" }, { ViewKind::Timeline, "Timelines" },
		                                                              { ViewKind::Calendar, "Calendars" } };
		bool any = false;
		for (const auto& k : kinds)
		{
			bool head = false;
			for (Database* db : s_ws.databases())
				for (const View& v : db->views)
					if (v.kind == k.kind || (k.kind == ViewKind::Table && v.kind == ViewKind::Board))
					{
						if (!head) { ImGui::TextDisabled("%s", k.title); head = true; }
						entry(Tab{ 0, db->id, v.id });
						any = true;
					}
		}
		if (!s_ws.pages().empty()) { ImGui::TextDisabled("Pages"); any = true; }
		for (Page* pg : s_ws.pages()) entry(Tab{ 1, pg->id, UUID{} });
		if (!s_ws.boards().empty()) { ImGui::TextDisabled("Whiteboards"); any = true; }
		for (Board* b : s_ws.boards()) entry(Tab{ 2, b->id, UUID{} });
		if (!any) ImGui::TextDisabled("Nothing to open yet.");
		ImGui::EndPopup();
	}
	ImGui::SetCursorScreenPos(ImVec2(row0.x, row0.y + 32.0f));
	ImGui::Dummy(ImVec2(1, 1));
	if (close >= 0)
	{
		const bool wasCurrent = sameTab(s_tabs[static_cast<std::size_t>(close)], cur);
		s_tabs.erase(s_tabs.begin() + close);
		if (wasCurrent && !s_tabs.empty()) selectTab(s_tabs[std::min<std::size_t>(static_cast<std::size_t>(close), s_tabs.size() - 1)]);
	}
}

void render(AppContext& ctx, const ImVec2& pos, const ImVec2& size)
{
	s_ctx = &ctx;
	Workspace* ws = workspace(ctx);
	if (ws)
	{
		s_ws.setAuthor(TaskBoardPanel::author());
		TaskBoardPanel::refreshOrphans(ctx);
		s_ws.poll();
	}
	s_stats = {};
	s_rects.clear();
	HE::Ed::Help::Scope helpScope("Planner");

	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin("##PlannerTab", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoSavedSettings);

	if (!ws)
	{
		ImGui::TextDisabled("Open a project to plan in it.");
		ImGui::End();
		return;
	}

	// The selection may point at a database that is gone (a pull removed it).
	if (!s_ws.find(s_dbId))
	{
		const std::vector<Database*> all = s_ws.databases();
		s_dbId   = all.empty() ? UUID{} : all.front()->id;
		s_viewId = UUID{};
	}
	// A selected data set without a (valid) view opens on the one it prefers.
	if (s_boardId == UUID{} && s_pageId == UUID{})
		if (const Database* db = s_ws.find(s_dbId))
			if (!db->findView(s_viewId))
				if (const View* v = preferredView(*db)) s_viewId = v->id;

	ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::warm(0.075f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 12.0f));
	ImGui::BeginChild("##plannernav", ImVec2(230.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::PopStyleVar();
	drawSidebar(ctx);
	ImGui::EndChild();
	ImGui::PopStyleColor();

	ImGui::SameLine();
	ImGui::BeginChild("##plannermain", ImVec2(0, 0), ImGuiChildFlags_None);
	{
		// Reload: a small icon in the corner, where it is findable and out of the way.
		const ImVec2 back = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight(), back.y));
		if (iconButton("##reload", "Planner/Reload", refreshGlyph)) post([] { s_ws.poll(true); });
		note("Reload");
		ImGui::SetCursorPos(back);
	}
	drawTabs();
	if (!s_ws.lastError().empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, Theme::Bad);
		EditorWidgets::WrapText wrap(ImGui::GetContentRegionAvail().x);
		ImGui::TextUnformatted(s_ws.lastError().c_str());
		ImGui::PopStyleColor();
	}
	if (!TaskBoardPanel::lastError().empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, Theme::Warn);
		{
			EditorWidgets::WrapText wrap(ImGui::GetContentRegionAvail().x);
			ImGui::TextUnformatted(TaskBoardPanel::lastError().c_str());
		}
		ImGui::PopStyleColor();
		if (EditorWidgets::smallButton("Reload Tasks")) post([&ctx] { TaskBoardPanel::reloadBoard(ctx); });
		if (TaskBoardPanel::hasUnsavedEdit())
		{
			ImGui::SameLine();
			if (EditorWidgets::smallButton("Retry Save")) post([] { TaskBoardPanel::retrySave(); });
		}
	}
	if (!(s_boardId == UUID{}))
		drawBoardTab(ctx);
	else if (!(s_pageId == UUID{}))
		drawPage(ctx);
	else if (const Database* db = s_ws.find(s_dbId))
		drawDatabase(ctx, *db);
	else
	{
		ImGui::Spacing();
		ImGui::TextDisabled("Make a database with New Database or a write-up with New Page. The project's tasks are in Tasks.");
	}
	ImGui::EndChild();
	ImGui::End();

	// Edits asked for while drawing, now that nothing holds a reference into a database.
	for (int guard = 0; guard < 8 && !s_posted.empty(); ++guard)
	{
		std::vector<std::function<void()>> now;
		now.swap(s_posted);
		for (auto& fn : now) fn();
	}
	s_posted.clear();
}

Workspace* workspace(AppContext& ctx)
{
	if (!ctx.projectLoaded || !ctx.projectManager)
	{
		s_ws.setProject({});
		return nullptr;
	}
	const std::string root = ctx.projectManager->projectRoot();
	if (root.empty())
	{
		s_ws.setProject({});
		return nullptr;
	}
	s_ws.setProject(root);

	// Tasks is the Task Board seen through the Planner (PlannerTasks.h).
	AppContext* pctx = &ctx;
	Workspace::External tasks;
	tasks.id         = tasksDatabaseId();
	tasks.name       = "Tasks";
	tasks.generation = [] { return TaskBoardPanel::generation(); };
	tasks.refresh    = [pctx](Database& db) {
		if (const TaskBoard* b = TaskBoardPanel::board(*pctx)) refreshTasks(db, *b);
		else refreshTasks(db, TaskBoard{});
	};
	tasks.apply = [pctx](const Database& before, const Database& after, std::string& err) {
		bool ok = true, changed = false;
		const bool saved = TaskBoardPanel::edit(*pctx, [&](TaskBoard& board) {
			std::string why;
			if (!applyTasksEdit(before, after, board, TaskBoardPanel::author(), taskNowMs(), changed, why))
			{
				err = why;
				ok  = false;
				return false;
			}
			return changed;
		});
		if (!ok) return false;
		if (changed && !saved)
		{
			err = TaskBoardPanel::lastError();
			return false;
		}
		return true;
	};
	s_ws.setExternal(std::move(tasks));
	return &s_ws;
}

void selectDatabase(const UUID& id) { s_dbId = id; s_viewId = UUID{}; s_pageId = s_pageDb = UUID{}; s_boardId = UUID{}; }
UUID selectedDatabase() { return s_dbId; }
UUID selectedView() { return s_viewId; }
void selectView(const UUID& databaseId, const UUID& viewId) { s_dbId = databaseId; s_viewId = viewId; s_pageId = s_pageDb = UUID{}; s_boardId = UUID{}; }
DrawStats lastDrawStats() { return s_stats; }
const std::string& lastError() { return s_ws.lastError(); }

void openTask(const UUID& id)
{
	s_dbId   = tasksDatabaseId();
	s_viewId = UUID{};
	s_selRow = id;
	TaskBoardPanel::selectTask(id);
}

UUID selectedRow() { return s_selRow; }

void showCalendarMonth(int year, int month) { s_calYear = year; s_calMonth = std::clamp(month, 1, 12); }
int  calendarYear() { return s_calYear; }
int  calendarMonth() { return s_calMonth; }
void setTimelineZoom(int level) { s_tlPpd = kZoomDayW[std::clamp(level, 0, 2)]; s_tlPending = 0; }

void selectPage(const UUID& id) { s_pageId = id; s_pageDb = UUID{}; s_boardId = UUID{}; }
void setClipboardForTest(std::function<bool(std::string&, std::string&)> image, std::function<std::string()> text,
                         std::function<void(const std::string&)> setText)
{
	s_clipImage = std::move(image);
	s_clipText  = std::move(text);
	s_clipSet   = std::move(setText);
}

void selectBoard(const UUID& id) { s_boardId = id; s_pageId = s_pageDb = UUID{}; }
UUID selectedBoard() { return s_boardId; }
void openRowPage(const UUID& dbId, const UUID& rowId) { openPage(rowId, dbId); }

void openRowInView(const UUID& dbId, const UUID& rowId)
{
	const Database* db = s_ws.find(dbId);
	if (!db || !db->findRow(rowId) || db->views.empty()) { openPage(rowId, dbId); return; }
	// A view of this data that is open already, else its first.
	UUID view = db->views.front().id;
	for (const Tab& t : s_tabs)
		if (t.kind == 0 && t.a == dbId && db->findView(t.b)) { view = t.b; break; }
	s_boardId = s_pageId = s_pageDb = UUID{};
	s_dbId = dbId;
	s_viewId = view;
	s_selRow = rowId;
}
UUID selectedPage() { return s_pageId; }

Rect controlRect(const std::string& name)
{
	const auto it = s_rects.find(name);
	return it == s_rects.end() ? Rect{} : it->second;
}

void reset()
{
	s_ws = Workspace{};
	s_dbId = s_viewId = UUID{};
	s_search.clear();
	s_selRow = UUID{};
	s_comment.clear();
	s_commentFor = UUID{};
	s_ctx = nullptr;
	s_editing = false;
	s_mineOnly = false;
	s_editBuf.clear();
	s_posted.clear();
	s_rects.clear();
	s_stats = {};
	s_armedDelete = UUID{};
	s_newRelationTarget = UUID{};
	s_pageId = s_pageDb = s_focusBlock = s_deletePage = UUID{};
	s_calYear = s_calMonth = 0;
	s_boardId = s_deleteBoard = UUID{};
	s_deleteBoardOpen = false;
	PlannerBoardView::reset();
	s_tlPpd = 24.0f;
	s_tlPending = 0;
	s_tlViews.clear();
	s_tlDrag = s_calDrag = DateDrag{};
	s_deletePageOpen = false;
	s_focusAtEnd = false;
	s_blockEdits.clear();
	s_codeEdits.clear();
	s_lastCodeLang.clear();
	s_cursorAt = -1;
	s_tabs.clear();
	s_flyRow = UUID{};
}

} // namespace PlannerPanel
