#include "TaskBoardFooter.h"
#include "TaskBoard.h"               // the data
#include "EditorApplication.h"       // AppContext

#ifdef HE_IMGUI_ENABLED
#include "EditorWidgets.h"           // help-aware button, helpForKey, WrapText
#include "EditorHelp.h"              // "Task Board/<label>" scope for the list's controls
#include "EditorTheme.h"
#include <imgui.h>
#include <imgui_internal.h>          // RenderTextEllipsis: a title cut with "..." at its slot's edge
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

using HE::Task;
using HE::TaskBoard;
using HE::TaskPriority;
using HE::TaskStatus;

namespace TaskBoardFooter
{

// ═════════════════════════════════════════════════════════════════════════════
// What it says
// ═════════════════════════════════════════════════════════════════════════════
namespace
{
	int priorityRank(const Task& t)
	{
		switch (t.priority)
		{
			case TaskPriority::Urgent: return 0;
			case TaskPriority::High:   return 1;
			case TaskPriority::Low:    return 3;
			default:                   return 2;   // Normal, and a word this build does not know
		}
	}

	int statusRank(const Task& t)
	{
		return t.status == TaskStatus::InProgress ? 0 : 1;
	}

	std::string titleOf(const Task& t)
	{
		return t.title.empty() ? std::string("(untitled)") : t.title;
	}
}

std::vector<HE::UUID> rankedOpen(const TaskBoard& board, std::string_view me)
{
	std::vector<const Task*> open;
	for (const Task& t : board.tasks)
		if (!t.isDone()) open.push_back(&t);

	// Stable, so what the keys do not separate stays in file order.
	std::stable_sort(open.begin(), open.end(), [me](const Task* a, const Task* b)
	{
		const bool mineA = !me.empty() && a->isAssignedTo(me);
		const bool mineB = !me.empty() && b->isAssignedTo(me);
		if (mineA != mineB) return mineA;
		const bool blockedA = a->status == TaskStatus::Blocked;
		const bool blockedB = b->status == TaskStatus::Blocked;
		if (blockedA != blockedB) return blockedB;
		if (priorityRank(*a) != priorityRank(*b)) return priorityRank(*a) < priorityRank(*b);
		return statusRank(*a) < statusRank(*b);
	});

	std::vector<HE::UUID> ids;
	ids.reserve(open.size());
	for (const Task* t : open) ids.push_back(t->id);
	return ids;
}

Summary summarize(const TaskBoard& board, std::string_view me)
{
	Summary s;
	s.open = board.openCount();
	s.mine = me.empty() ? 0 : board.openCountFor(me);
	const std::vector<HE::UUID> ids = rankedOpen(board, me);
	if (!ids.empty())
		if (const Task* t = board.find(ids.front()))
		{
			s.next      = t->id;
			s.nextTitle = titleOf(*t);
		}
	return s;
}

float budget(float cursorX, float windowWidth)
{
	// The centred status line keeps to the middle third (EditorRewards'
	// drawFooterStatus drops its counters rather than outgrow it); stop a
	// neighbour's gap short of where that third begins.
	return std::max(0.0f, windowWidth / 3.0f - 16.0f - cursorX);
}

#ifdef HE_IMGUI_ENABLED

// ═════════════════════════════════════════════════════════════════════════════
// The widget and its list
// ═════════════════════════════════════════════════════════════════════════════
namespace
{
	// Where the widget landed last frame — the list hangs off it.
	ImVec2 s_widgetMin { 0.0f, 0.0f };
	ImVec2 s_widgetMax { 0.0f, 0.0f };
	bool   s_widgetHovered = false;
	int    s_widgetFrame   = -1;

	bool   s_open = false;

	// The list's rows are taken when it opens and kept until it closes. A task
	// ticked off is no longer open, and a list rebuilt every frame would pull
	// the row out from under the mouse — with no way to untick a slip. So the
	// row stays, ticked, and remembers what the task was before, which is what
	// unticking puts back (In Progress stays In Progress, not Open).
	struct Row
	{
		HE::UUID    id;
		TaskStatus  before = TaskStatus::Open;
		std::string beforeOther;   // the file's word, when `before` is Other
	};
	std::vector<Row> s_rows;
	int              s_more = 0;   // open tasks the list had no room for
	constexpr int    kMaxRows = 8;

	Rect                               s_listRect;
	bool                               s_listDrawn = false;
	std::unordered_map<HE::UUID, Rect> s_tickRects, s_titleRects;

	// The ambient grey the footer's other status lines use.
	const ImVec4 kFooterText { 0.65f, 0.65f, 0.65f, 0.9f };

	Rect rectOf(const ImVec2& a, const ImVec2& z) { return Rect{ a.x, a.y, z.x, z.y }; }

	void closeList()
	{
		s_open = false;
		s_rows.clear();
		s_more = 0;
		s_listDrawn = false;
		s_tickRects.clear();
		s_titleRects.clear();
	}

	void openList(const TaskBoard& b)
	{
		closeList();
		s_open = true;
		const std::vector<HE::UUID> ids = rankedOpen(b, TaskBoardPanel::author());
		for (const HE::UUID& id : ids)
		{
			if ((int)s_rows.size() == kMaxRows) break;
			if (const Task* t = b.find(id))
				s_rows.push_back(Row{ id, t->status, t->statusOther });
		}
		s_more = (int)ids.size() - (int)s_rows.size();
	}

	// The widget's parts, measured once so the hit box and the drawing agree.
	struct Layout
	{
		float       width = 0.0f;   // 0: nothing is drawn
		float       lineH = 0.0f;
		float       box   = 0.0f;
		float       gap   = 0.0f;
		std::string counts;         // may be empty: the box alone
		float       countsW = 0.0f;
		std::string next;           // "Next: ..." — empty when it did not fit
		float       nextW = 0.0f;   // the slot it is cut to
	};

	Layout layoutFor(const Summary& s, float maxWidth)
	{
		Layout l;
		l.lineH = ImGui::GetTextLineHeight();
		l.box   = std::floor(l.lineH * 0.72f);
		l.gap   = std::floor(ImGui::GetFontSize() * 0.4f);
		if (maxWidth < l.box) return l;

		char buf[96];
		if (s.open == 0)     std::snprintf(buf, sizeof(buf), "No open tasks");
		else if (s.mine > 0) std::snprintf(buf, sizeof(buf), "%d open \xC2\xB7 %d mine", s.open, s.mine);
		else                 std::snprintf(buf, sizeof(buf), "%d open", s.open);
		auto fits = [&](const char* text) {
			return l.box + l.gap + ImGui::CalcTextSize(text).x <= maxWidth;
		};
		bool full = fits(buf);
		if (!full)
		{
			// Only the numbers: "3", or "3 (1)" with some of them mine.
			if (s.open == 0)     buf[0] = '\0';
			else if (s.mine > 0) std::snprintf(buf, sizeof(buf), "%d (%d)", s.open, s.mine);
			else                 std::snprintf(buf, sizeof(buf), "%d", s.open);
			if (buf[0] && !fits(buf)) buf[0] = '\0';
		}
		l.counts  = buf;
		l.countsW = l.counts.empty() ? 0.0f : ImGui::CalcTextSize(buf).x;
		l.width   = l.box + (l.counts.empty() ? 0.0f : l.gap + l.countsW);

		// The next task, cut to what is left — or not at all when that is too
		// little to read a word of it.
		if (full && s.open > 0 && !s.nextTitle.empty())
		{
			std::string next = "Next: " + s.nextTitle;
			const float room = maxWidth - l.width - l.gap * 2.0f;
			const float cap  = ImGui::GetFontSize() * 18.0f;
			if (room >= ImGui::GetFontSize() * 4.0f)
			{
				l.next  = std::move(next);
				l.nextW = std::min({ ImGui::CalcTextSize(l.next.c_str()).x, cap, room });
				l.width += l.gap * 2.0f + l.nextW;
			}
		}
		return l;
	}

	ImVec4 priorityColour(const Task& t)
	{
		switch (t.priority)
		{
			case TaskPriority::Low:    return HE::Ed::Theme::TextDim;
			case TaskPriority::High:   return HE::Ed::Theme::AccentBright;
			case TaskPriority::Urgent: return ImVec4(0.90f, 0.32f, 0.26f, 1.0f);
			default:                   return HE::Ed::Theme::Accent;
		}
	}

	// Back to what a row's task was before it was ticked. A status word this
	// build does not know is put back as that word — setStatus only takes the
	// four it knows — so a tick and an untick leave a newer editor's status as
	// it was.
	bool restore(TaskBoard& b, const Row& r)
	{
		if (r.before != TaskStatus::Other)
			return b.setStatus(r.id, r.before, TaskBoardPanel::author(), HE::taskNowMs());
		Task* t = b.find(r.id);
		if (!t) return false;
		const std::int64_t now = HE::taskNowMs();
		t->status      = TaskStatus::Other;
		t->statusOther = r.beforeOther;
		t->modifiedMs  = now;
		HE::TaskUpdate u;
		u.id     = HE::UUID::generate();
		u.kind   = HE::TaskUpdateKind::Status;
		u.author = TaskBoardPanel::author();
		u.timeMs = now;
		u.status = r.beforeOther;
		t->updates.push_back(std::move(u));
		return true;
	}

	// The list's rows. Returns the task whose title was clicked (zero for
	// none); a tick is handed back in `tick`/`tickDone` and applied by the
	// caller after the loop — an edit can reload the board under it.
	HE::UUID drawRows(const TaskBoard& b, HE::UUID& tick, bool& tickDone)
	{
		HE::Ed::Help::Scope helpScope("Task Board");
		HE::UUID clicked;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		for (const Row& r : s_rows)
		{
			const Task* t = b.find(r.id);
			if (!t) continue;   // gone with a pull since the list opened

			ImGui::PushID(HE::uuidToHex(r.id).c_str());
			bool done = t->isDone();
			if (ImGui::Checkbox("##tick", &done)) { tick = r.id; tickDone = done; }
			EditorWidgets::helpForKey("Task Board/Tick Off");
			s_tickRects[r.id] = rectOf(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

			ImGui::SameLine();
			const ImVec2 a = ImGui::GetCursorScreenPos();
			const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
			const float  h = ImGui::GetFrameHeight();
			if (ImGui::InvisibleButton("##title", ImVec2(w, h))) clicked = r.id;
			EditorWidgets::helpForKey("Task Board/Footer Task");
			const ImVec2 z(a.x + w, a.y + h);
			s_titleRects[r.id] = rectOf(a, z);
			if (ImGui::IsItemHovered())
				dl->AddRectFilled(a, z, ImGui::GetColorU32(ImGuiCol_HeaderHovered), 3.0f);

			// Priority as a stripe, like the card's on the board.
			dl->AddRectFilled(ImVec2(a.x, a.y + 3.0f), ImVec2(a.x + 3.0f, z.y - 3.0f),
			                  ImGui::GetColorU32(priorityColour(*t)), 1.5f);

			const std::string title = titleOf(*t);
			const char* tag = t->isDone() ? nullptr
			                : t->status == TaskStatus::InProgress ? "In Progress"
			                : t->status == TaskStatus::Blocked    ? "Blocked" : nullptr;
			const float tagW = tag ? ImGui::CalcTextSize(tag).x + 8.0f : 0.0f;
			const float tx0 = a.x + 9.0f, tx1 = std::max(tx0, z.x - 4.0f - tagW);
			const float ty  = a.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(t->isDone() ? ImGuiCol_TextDisabled : ImGuiCol_Text));
			ImGui::RenderTextEllipsis(dl, ImVec2(tx0, ty), ImVec2(tx1, ty + ImGui::GetTextLineHeight()), tx1,
			                          title.c_str(), nullptr, nullptr);
			ImGui::PopStyleColor();
			if (t->isDone())
			{
				// Struck through: done, and still here to be unticked.
				const float tw = std::min(ImGui::CalcTextSize(title.c_str()).x, tx1 - tx0);
				const float my = std::floor(ty + ImGui::GetTextLineHeight() * 0.55f) + 0.5f;
				dl->AddLine(ImVec2(tx0, my), ImVec2(tx0 + tw, my), ImGui::GetColorU32(ImGuiCol_TextDisabled), 1.0f);
			}
			if (tag)
				dl->AddText(ImVec2(z.x - 4.0f - tagW + 8.0f, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), tag);
			ImGui::PopID();
		}
		return clicked;
	}
}

float DrawFooter(AppContext& ctx, float maxWidth)
{
	TaskBoard* b = TaskBoardPanel::board(ctx);
	if (!b) { if (s_open) closeList(); return 0.0f; }

	const Summary s = summarize(*b, TaskBoardPanel::author());
	const Layout  l = layoutFor(s, maxWidth);
	if (l.width <= 0.0f) return 0.0f;   // the list, if open, closes: no anchor this frame

	// One hit box for the whole widget, one text line tall.
	const ImVec2 o = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##taskBoardFooter", ImVec2(l.width, l.lineH));
	const bool hovered = ImGui::IsItemHovered();
	if (ImGui::IsItemClicked())
	{
		if (s_open) closeList();
		else        openList(*b);
	}
	EditorWidgets::helpForKey("Task Board/Footer");

	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec4 base = s.open == 0 ? HE::Ed::Theme::TextDim : kFooterText;
	const ImU32  col  = ImGui::GetColorU32(hovered || s_open ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : base);
	const ImU32  mine = ImGui::GetColorU32(HE::Ed::Theme::Accent);

	// The box: empty with work left, ticked with everything done.
	const float by = std::floor(o.y + (l.lineH - l.box) * 0.5f);
	const ImVec2 ba(o.x + 0.5f, by + 0.5f), bz(o.x + l.box - 0.5f, by + l.box - 0.5f);
	dl->AddRect(ba, bz, s.mine > 0 ? mine : col, 2.0f, 0, 1.2f);
	if (s.open == 0 && !b->tasks.empty())
	{
		// Brighter than the dimmed box around it, or at this size it reads as
		// a smudge rather than a tick.
		const ImU32  tickCol = ImGui::GetColorU32(kFooterText);
		const ImVec2 p0(ba.x + l.box * 0.20f, ba.y + l.box * 0.50f);
		const ImVec2 p1(ba.x + l.box * 0.42f, ba.y + l.box * 0.74f);
		const ImVec2 p2(ba.x + l.box * 0.82f, ba.y + l.box * 0.24f);
		dl->AddLine(p0, p1, tickCol, 1.8f);
		dl->AddLine(p1, p2, tickCol, 1.8f);
	}

	float x = o.x + l.box + l.gap;
	if (!l.counts.empty())
	{
		dl->AddText(ImVec2(x, o.y), s.mine > 0 ? mine : col, l.counts.c_str());
		x += l.countsW;
	}
	if (!l.next.empty())
	{
		// A dot between the count and the title, like the "·" of the status
		// line, then the title cut at its slot: a title is whatever somebody
		// typed, of any length.
		dl->AddCircleFilled(ImVec2(x + l.gap, o.y + l.lineH * 0.55f), 1.5f,
		                    ImGui::GetColorU32(HE::Ed::Theme::TextDim));
		x += l.gap * 2.0f;
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
		ImGui::RenderTextEllipsis(dl, ImVec2(x, o.y), ImVec2(x + l.nextW, o.y + l.lineH), x + l.nextW,
		                          l.next.c_str(), nullptr, nullptr);
		ImGui::PopStyleColor();
	}

	s_widgetMin     = o;
	s_widgetMax     = ImVec2(o.x + l.width, o.y + l.lineH);
	s_widgetHovered = hovered;
	s_widgetFrame   = ImGui::GetFrameCount();
	return l.width;
}

bool DrawOverlay(AppContext& ctx)
{
	// Every control here is looked up as "Task Board/<its label>".
	HE::Ed::Help::Scope helpScope("Task Board");
	if (!s_open) return false;
	// The widget did not draw this frame — no project, no room, or the footer
	// was skipped (the project hub) — so there is nothing to hang the list off.
	if (s_widgetFrame != ImGui::GetFrameCount()) { closeList(); return false; }
	TaskBoard* b = TaskBoardPanel::board(ctx);
	if (!b) { closeList(); return false; }

	// Anchored bottom-left onto the widget with no gap, like the bell's
	// flyout, and kept inside the viewport.
	const float width = ImGui::GetFontSize() * 24.0f;
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	const float x = std::max(vp->WorkPos.x + 4.0f,
	                         std::min(s_widgetMin.x, vp->WorkPos.x + vp->WorkSize.x - width - 4.0f));
	ImGui::SetNextWindowPos(ImVec2(x, s_widgetMin.y), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
	ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
	ImGui::SetNextWindowViewport(vp->ID);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));

	bool     hovered   = false;
	bool     openPanel = false;
	HE::UUID tick, titled;
	bool     tickDone  = false;
	s_tickRects.clear();
	s_titleRects.clear();
	s_listDrawn = false;
	if (ImGui::Begin("##taskBoardFooterList", nullptr,
	                 ImGuiWindowFlags_NoTitleBar         |
	                 ImGuiWindowFlags_NoResize           |
	                 ImGuiWindowFlags_NoMove             |
	                 ImGuiWindowFlags_NoScrollbar        |
	                 ImGuiWindowFlags_NoSavedSettings    |
	                 ImGuiWindowFlags_NoDocking          |
	                 ImGuiWindowFlags_NoFocusOnAppearing |
	                 ImGuiWindowFlags_AlwaysAutoResize))
	{
		hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup |
		                                 ImGuiHoveredFlags_ChildWindows);
		EditorWidgets::WrapText wrap;

		const Summary s = summarize(*b, TaskBoardPanel::author());
		ImGui::TextUnformatted("Tasks");
		ImGui::SameLine();
		if (s.open == 0)     ImGui::TextDisabled("nothing open");
		else if (s.mine > 0) ImGui::TextDisabled("%d open, %d assigned to you", s.open, s.mine);
		else                 ImGui::TextDisabled("%d open", s.open);
		ImGui::Separator();

		if (s_rows.empty())
			ImGui::TextDisabled(b->tasks.empty()
				? "No tasks yet. Open the Task Board to add one."
				: "Everything is done.");
		else
			titled = drawRows(*b, tick, tickDone);
		if (s_more > 0)
			ImGui::TextDisabled("+%d more on the board", s_more);

		ImGui::Separator();
		if (EditorWidgets::button("Open Task Board")) openPanel = true;

		s_listRect  = rectOf(ImGui::GetWindowPos(),
		                     ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
		                            ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));
		s_listDrawn = true;
	}
	ImGui::End();
	ImGui::PopStyleVar();

	// The tick, now that nothing points into the task list any more. By id:
	// edit() may reload the board (a pull) and run this a second time.
	if (tick != HE::UUID{})
	{
		const auto it = std::find_if(s_rows.begin(), s_rows.end(), [&](const Row& r) { return r.id == tick; });
		if (it != s_rows.end())
		{
			const Row row = *it;
			TaskBoardPanel::edit(ctx, [row, tickDone](TaskBoard& bb) {
				return tickDone ? bb.setStatus(row.id, TaskStatus::Done, TaskBoardPanel::author(), HE::taskNowMs())
				                : restore(bb, row);
			});
		}
	}
	if (titled != HE::UUID{})
	{
		TaskBoardPanel::selectTask(titled);
		openPanel = true;
	}

	if (openPanel ||
	    ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
	    (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered && !s_widgetHovered))
		closeList();
	return openPanel;
}

#else // !HE_IMGUI_ENABLED

float DrawFooter(AppContext&, float) { return 0.0f; }
bool  DrawOverlay(AppContext&) { return false; }

#endif

bool widgetRect(Rect& out)
{
#ifdef HE_IMGUI_ENABLED
	if (s_widgetFrame < 0) return false;
	out = rectOf(s_widgetMin, s_widgetMax);
	return true;
#else
	(void)out;
	return false;
#endif
}

bool isOpen()
{
#ifdef HE_IMGUI_ENABLED
	return s_open;
#else
	return false;
#endif
}

bool listRect(Rect& out)
{
#ifdef HE_IMGUI_ENABLED
	if (!s_open || !s_listDrawn) return false;
	out = s_listRect;
	return true;
#else
	(void)out;
	return false;
#endif
}

bool tickRect(const HE::UUID& task, Rect& out)
{
#ifdef HE_IMGUI_ENABLED
	const auto it = s_tickRects.find(task);
	if (!s_open || it == s_tickRects.end()) return false;
	out = it->second;
	return true;
#else
	(void)task; (void)out;
	return false;
#endif
}

bool titleRect(const HE::UUID& task, Rect& out)
{
#ifdef HE_IMGUI_ENABLED
	const auto it = s_titleRects.find(task);
	if (!s_open || it == s_titleRects.end()) return false;
	out = it->second;
	return true;
#else
	(void)task; (void)out;
	return false;
#endif
}

void reset()
{
#ifdef HE_IMGUI_ENABLED
	closeList();
	s_widgetFrame   = -1;
	s_widgetHovered = false;
#endif
}

} // namespace TaskBoardFooter
