#pragma once
#include "EditorTheme.h"
#include "EditorWidgets.h"

#include <imgui.h>

#include <cmath>

// ── The Planner's icons ──────────────────────────────────────────────────────
// Glyphs drawn with the draw list: crisp at any scale, in the theme's colours, and no files to ship
// (the editor has no icon font). Used by the sidebar, the database toolbar and the whiteboard.
namespace PlannerIcons
{
namespace Theme = HE::Ed::Theme;

// Drawn with the draw list, so they are crisp at any scale, take the theme's colours and need no files.
enum class Icon { Note, Frame, AddCard, Undo, Redo, ZoomIn, ZoomOut, Fit, Map, Export, Duplicate, Align, Snap, Colour, Trash,
	              Plus, Table, Kanban, Timeline, Calendar, Search, Filter, Sliders, Page, Whiteboard, Gear, Close, ChevronLeft, ChevronRight };

inline void drawIcon(ImDrawList* dl, Icon icon, ImVec2 c, ImU32 col, ImU32 fill = 0)
{
	constexpr float s = 8.0f;   // half the glyph's width
	constexpr float t = 1.6f;   // stroke
	auto P = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };
	auto L = [&](float x0, float y0, float x1, float y1) { dl->AddLine(P(x0, y0), P(x1, y1), col, t); };
	switch (icon)
	{
	case Icon::Note:
		dl->AddRect(P(-1, -0.85f), P(1, 0.85f), col, 3.0f, 0, t);
		L(-0.55f, -0.25f, 0.55f, -0.25f);
		L(-0.55f, 0.25f, 0.15f, 0.25f);
		break;
	case Icon::Frame:
		for (int sx = -1; sx <= 1; sx += 2)
			for (int sy = -1; sy <= 1; sy += 2)
			{
				L(sx * 1.0f, sy * 1.0f, sx * 1.0f, sy * 0.35f);
				L(sx * 1.0f, sy * 1.0f, sx * 0.35f, sy * 1.0f);
			}
		L(-0.5f, -0.35f, 0.1f, -0.35f);
		break;
	case Icon::AddCard:
		dl->AddRect(P(-0.75f, -1), P(0.75f, 1), col, 3.0f, 0, t);
		L(-0.3f, 0.0f, 0.3f, 0.0f);
		L(0.0f, -0.3f, 0.0f, 0.3f);
		break;
	case Icon::Undo:
	case Icon::Redo:
	{
		const float f = icon == Icon::Undo ? 1.0f : -1.0f;
		const float r = 0.72f * s;
		const ImVec2 mid(c.x, c.y + 0.15f * s);
		const int steps = 14;
		for (int i = 0; i <= steps; ++i)
		{
			const float a = -1.5708f + static_cast<float>(i) * (1.25f * 3.14159f) / steps;
			dl->PathLineTo(ImVec2(mid.x + f * std::cos(a) * r, mid.y + std::sin(a) * r));
		}
		dl->PathStroke(col, 0, t);
		const ImVec2 p0(mid.x, mid.y - r);
		dl->AddTriangleFilled(ImVec2(p0.x - f * 0.5f * s, p0.y), ImVec2(p0.x + f * 0.05f * s, p0.y - 0.42f * s),
		                      ImVec2(p0.x + f * 0.05f * s, p0.y + 0.42f * s), col);
		break;
	}
	case Icon::ZoomIn:
	case Icon::ZoomOut:
		dl->AddCircle(P(-0.15f, -0.15f), 0.62f * s, col, 0, t);
		L(0.3f, 0.3f, 0.95f, 0.95f);
		L(-0.45f, -0.15f, 0.15f, -0.15f);
		if (icon == Icon::ZoomIn) L(-0.15f, -0.45f, -0.15f, 0.15f);
		break;
	case Icon::Fit:
		for (int sx = -1; sx <= 1; sx += 2)
			for (int sy = -1; sy <= 1; sy += 2)
			{
				L(sx * 1.0f, sy * 1.0f, sx * 1.0f, sy * 0.45f);
				L(sx * 1.0f, sy * 1.0f, sx * 0.45f, sy * 1.0f);
			}
		dl->AddRect(P(-0.3f, -0.3f), P(0.3f, 0.3f), col, 1.0f, 0, t);
		break;
	case Icon::Map:
		dl->AddRect(P(-1, -0.75f), P(1, 0.75f), col, 2.5f, 0, t);
		dl->AddRectFilled(P(-0.6f, -0.35f), P(-0.05f, 0.1f), col, 1.0f);
		dl->AddRect(P(0.1f, 0.0f), P(0.65f, 0.4f), col, 1.0f, 0, t);
		break;
	case Icon::Export:
		L(-1, 0.1f, -1, 0.9f);
		L(-1, 0.9f, 1, 0.9f);
		L(1, 0.9f, 1, 0.1f);
		L(0, -0.95f, 0, 0.45f);
		L(-0.45f, 0.0f, 0.0f, 0.45f);
		L(0.45f, 0.0f, 0.0f, 0.45f);
		break;
	case Icon::Duplicate:
		dl->AddRect(P(-0.5f, -1), P(1, 0.5f), col, 2.5f, 0, t);
		dl->AddRect(P(-1, -0.5f), P(0.5f, 1), col, 2.5f, 0, t);
		break;
	case Icon::Align:
		L(-1, -1, -1, 1);
		L(-0.6f, -0.55f, 0.9f, -0.55f);
		L(-0.6f, 0.0f, 0.3f, 0.0f);
		L(-0.6f, 0.55f, 0.6f, 0.55f);
		break;
	case Icon::Snap:
		for (int ix = -1; ix <= 1; ++ix)
			for (int iy = -1; iy <= 1; ++iy)
				dl->AddCircleFilled(P(ix * 0.8f, iy * 0.8f), 1.5f, col);
		break;
	case Icon::Colour:
		if (fill) dl->AddCircleFilled(c, 0.78f * s, fill);
		dl->AddCircle(c, 0.78f * s, col, 0, t);
		break;
	case Icon::Gear:
		dl->AddCircle(c, 0.36f * s, col, 0, t);
		for (int k = 0; k < 8; ++k)
		{
			const float a = static_cast<float>(k) * 0.785398f;
			L(std::cos(a) * 0.62f, std::sin(a) * 0.62f, std::cos(a) * 0.98f, std::sin(a) * 0.98f);
		}
		dl->AddCircle(c, 0.62f * s, col, 0, t);
		break;
	case Icon::Close:
		L(-0.7f, -0.7f, 0.7f, 0.7f);
		L(-0.7f, 0.7f, 0.7f, -0.7f);
		break;
	case Icon::ChevronLeft:
		L(0.4f, -0.8f, -0.4f, 0.0f);
		L(-0.4f, 0.0f, 0.4f, 0.8f);
		break;
	case Icon::ChevronRight:
		L(-0.4f, -0.8f, 0.4f, 0.0f);
		L(0.4f, 0.0f, -0.4f, 0.8f);
		break;
	case Icon::Plus:
		L(-0.8f, 0, 0.8f, 0);
		L(0, -0.8f, 0, 0.8f);
		break;
	case Icon::Table:
		dl->AddRect(P(-1, -0.8f), P(1, 0.8f), col, 2.5f, 0, t);
		L(-1, -0.2f, 1, -0.2f);
		L(-1, 0.35f, 1, 0.35f);
		L(-0.15f, -0.2f, -0.15f, 0.8f);
		break;
	case Icon::Kanban:
		dl->AddRect(P(-1, -0.85f), P(-0.4f, 0.55f), col, 1.5f, 0, t);
		dl->AddRect(P(-0.2f, -0.85f), P(0.4f, 0.1f), col, 1.5f, 0, t);
		dl->AddRect(P(0.6f, -0.85f), P(1, 0.85f), col, 1.5f, 0, t);
		break;
	case Icon::Timeline:
		dl->AddRectFilled(P(-1, -0.75f), P(0.1f, -0.35f), col, 1.5f);
		dl->AddRectFilled(P(-0.5f, -0.2f), P(0.7f, 0.2f), col, 1.5f);
		dl->AddRectFilled(P(-0.1f, 0.35f), P(1, 0.75f), col, 1.5f);
		break;
	case Icon::Calendar:
		dl->AddRect(P(-1, -0.75f), P(1, 0.95f), col, 2.5f, 0, t);
		L(-1, -0.25f, 1, -0.25f);
		L(-0.5f, -1, -0.5f, -0.55f);
		L(0.5f, -1, 0.5f, -0.55f);
		dl->AddCircleFilled(P(-0.5f, 0.2f), 1.2f, col);
		dl->AddCircleFilled(P(0.0f, 0.2f), 1.2f, col);
		dl->AddCircleFilled(P(0.5f, 0.2f), 1.2f, col);
		dl->AddCircleFilled(P(-0.5f, 0.65f), 1.2f, col);
		break;
	case Icon::Search:
		dl->AddCircle(P(-0.2f, -0.2f), 0.62f * s, col, 0, t);
		L(0.25f, 0.25f, 0.95f, 0.95f);
		break;
	case Icon::Filter:
		L(-1, -0.8f, 1, -0.8f);
		L(1, -0.8f, 0.2f, 0.1f);
		L(0.2f, 0.1f, 0.2f, 0.9f);
		L(0.2f, 0.9f, -0.2f, 0.65f);
		L(-0.2f, 0.65f, -0.2f, 0.1f);
		L(-0.2f, 0.1f, -1, -0.8f);
		break;
	case Icon::Sliders:
		L(-1, -0.6f, 1, -0.6f);
		L(-1, 0.0f, 1, 0.0f);
		L(-1, 0.6f, 1, 0.6f);
		dl->AddCircleFilled(P(-0.4f, -0.6f), 2.2f, col);
		dl->AddCircleFilled(P(0.45f, 0.0f), 2.2f, col);
		dl->AddCircleFilled(P(-0.1f, 0.6f), 2.2f, col);
		break;
	case Icon::Page:
		L(-0.7f, -1, 0.2f, -1);
		L(0.2f, -1, 0.7f, -0.5f);
		L(0.7f, -0.5f, 0.7f, 1);
		L(0.7f, 1, -0.7f, 1);
		L(-0.7f, 1, -0.7f, -1);
		L(0.2f, -1, 0.2f, -0.5f);
		L(0.2f, -0.5f, 0.7f, -0.5f);
		L(-0.4f, 0.1f, 0.4f, 0.1f);
		L(-0.4f, 0.55f, 0.2f, 0.55f);
		break;
	case Icon::Whiteboard:
		dl->AddRect(P(-1, -0.85f), P(1, 0.7f), col, 2.5f, 0, t);
		dl->AddRectFilled(P(-0.6f, -0.45f), P(-0.05f, 0.05f), col, 1.0f);
		L(0.1f, 0.0f, 0.65f, -0.4f);
		L(-0.4f, 0.7f, -0.6f, 1);
		L(0.4f, 0.7f, 0.6f, 1);
		break;
	case Icon::Trash:
		L(-1, -0.6f, 1, -0.6f);
		L(-0.3f, -0.6f, -0.3f, -0.95f);
		L(-0.3f, -0.95f, 0.3f, -0.95f);
		L(0.3f, -0.95f, 0.3f, -0.6f);
		L(-0.7f, -0.6f, -0.55f, 0.95f);
		L(-0.55f, 0.95f, 0.55f, 0.95f);
		L(0.55f, 0.95f, 0.7f, -0.6f);
		L(-0.2f, -0.2f, -0.2f, 0.6f);
		L(0.2f, -0.2f, 0.2f, 0.6f);
		break;
	}
}

// A square button with a drawn glyph; its tooltip is the help entry `helpKey`. Returns true on a click
// (never while disabled). `active` is a toggle that is on.
inline bool iconButton(const char* id, Icon icon, bool active, bool enabled, const char* helpKey, bool danger = false,
                ImU32 fill = 0, ImVec2 size = ImVec2(28, 28))
{
	ImGui::PushID(id);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton("##icon", size);
	const bool hov = ImGui::IsItemHovered() && enabled, held = ImGui::IsItemActive() && enabled;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	if (active) dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), Theme::u32(Theme::alpha(Theme::Accent, 0.28f)), 7.0f);
	else if (hov) dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), Theme::u32(Theme::warm(held ? 0.36f : 0.28f)), 7.0f);
	ImVec4 ink = !enabled ? Theme::alpha(Theme::TextDim, 0.45f) : (danger && hov) ? Theme::Bad : active ? Theme::AccentBright : Theme::Text;
	drawIcon(dl, icon, ImVec2(p.x + size.x * 0.5f, p.y + size.y * 0.5f), Theme::u32(ink), fill);
	EditorWidgets::helpForKey(helpKey);
	ImGui::PopID();
	return pressed && enabled;
}

// A thin line between groups of buttons in a row.
inline void iconSeparator()
{
	ImGui::SameLine(0, 6);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 5), ImVec2(p.x, p.y + 23), Theme::u32(Theme::warm(0.30f)));
	ImGui::Dummy(ImVec2(1, 28));
	ImGui::SameLine(0, 6);
}

} // namespace PlannerIcons
