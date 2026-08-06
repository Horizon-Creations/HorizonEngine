#include "EditorToolbar.h"

#ifdef HE_IMGUI_ENABLED

#include <algorithm>
#include <cmath>

namespace EditorToolbar
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;   // IM_PI is in imgui_internal.h
}

float stroke(float s) { return std::max(1.2f, s * 0.10f); }

float height()
{
	return std::floor(ImGui::GetFrameHeight() + 12.0f);
}

Metrics metrics(float originY)
{
	Metrics m;
	m.cell  = ImGui::GetFrameHeight();
	m.bar   = std::floor(m.cell + 12.0f);
	m.icon  = std::floor(m.cell * 0.60f);
	m.wellH = m.cell + kWellPad * 2.0f;
	m.y     = std::floor(originY + (m.bar - m.wellH) * 0.5f);
	m.cy    = m.y + m.wellH * 0.5f;
	return m;
}

float cellWidth(const Metrics& m, const char* label)
{
	if (!label) return m.cell;
	return std::floor(m.icon + kLabelGap + ImGui::CalcTextSize(label).x + kCellPadX * 2.0f);
}

namespace
{
// The shared body of cell()/cellTinted(): identical geometry and interaction,
// differing only in how the foreground colour is decided.
bool cellImpl(const Metrics& m, float x, float w, const char* id, IconFn icon,
              const char* label, bool on, bool enabled, const char* tooltip,
              const ImU32* forcedFg)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 p0(x, m.y + kWellPad);
	const ImVec2 p1(x + w, p0.y + m.cell);

	ImGui::SetCursorScreenPos(p0);
	if (!enabled) ImGui::BeginDisabled();
	const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, m.cell));
	const bool hovered = ImGui::IsItemHovered();
	const bool held    = ImGui::IsItemActive();
	if (tooltip && enabled) ImGui::SetItemTooltip("%s", tooltip);
	if (!enabled) ImGui::EndDisabled();

	const ImU32 bg = on      ? (hovered ? kOnBgHover : kOnBg)
	               : held    ? kDownBg
	               : hovered ? kHoverBg
	                         : 0u;
	if (bg) dl->AddRectFilled(p0, p1, bg, kCellRound);

	const ImU32 fg = !enabled  ? kFgDim
	               : forcedFg  ? *forcedFg
	               : on        ? kFgOn
	                           : kFg;

	const float labelW   = label ? ImGui::CalcTextSize(label).x : 0.0f;
	const float iconW    = icon ? m.icon : 0.0f;
	const float gap      = (icon && label) ? kLabelGap : 0.0f;
	const float contentW = iconW + gap + labelW;
	const float left     = x + (w - contentW) * 0.5f;

	if (icon) icon(dl, ImVec2(std::floor(left + m.icon * 0.5f), std::floor(m.cy)), m.icon, fg);
	if (label)
		dl->AddText(ImVec2(std::floor(left + iconW + gap),
		                   std::floor(m.cy - ImGui::GetFontSize() * 0.5f)), fg, label);
	return pressed && enabled;
}
} // namespace

bool cell(const Metrics& m, float x, float w, const char* id, IconFn icon,
          const char* label, bool on, bool enabled, const char* tooltip)
{
	return cellImpl(m, x, w, id, icon, label, on, enabled, tooltip, nullptr);
}

bool cellTinted(const Metrics& m, float x, float w, const char* id, IconFn icon,
                const char* label, ImU32 fg, bool enabled, const char* tooltip)
{
	return cellImpl(m, x, w, id, icon, label, false, enabled, tooltip, &fg);
}

void well(const Metrics& m, float x, float w, ImU32 col)
{
	ImGui::GetWindowDrawList()->AddRectFilled(
		ImVec2(x, m.y), ImVec2(x + w, m.y + m.wellH), col, kWellRound);
}

void bar(const ImVec2& origin, float width, const Metrics& m,
         ImU32 tint, ImU32 lineCol, float lineThickness)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + m.bar), kBarBg);
	if (tint) dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + m.bar), tint);
	dl->AddLine(ImVec2(origin.x,         origin.y + m.bar - 1.0f),
	            ImVec2(origin.x + width, origin.y + m.bar - 1.0f), lineCol, lineThickness);
}

// ─── Icons ───────────────────────────────────────────────────────────────────

void iconGear(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float r = s * 0.30f, tooth = s * 0.46f, t = stroke(s);
	for (int i = 0; i < 6; ++i)
	{
		const float a = i * (kPi / 3.0f);
		dl->AddLine({ c.x + std::cos(a) * r * 0.9f,   c.y + std::sin(a) * r * 0.9f },
		            { c.x + std::cos(a) * tooth,      c.y + std::sin(a) * tooth }, col, t * 1.4f);
	}
	dl->AddCircle(c, r, col, 20, t);
}

// Circular arrow, closed further round than the rotate gizmo's so the two do not
// read as the same control.
void iconRefresh(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float r = s * 0.38f, t = stroke(s);
	const float aStart = -kPi * 0.35f, aEnd = kPi * 1.30f;
	dl->PathArcTo(c, r, aStart, aEnd, 28);
	dl->PathStroke(col, 0, t);

	const ImVec2 e(c.x + std::cos(aStart) * r, c.y + std::sin(aStart) * r);
	const float  k = s * 0.20f;
	dl->AddTriangleFilled({ e.x + k * 0.9f, e.y - k * 0.2f },
	                      { e.x - k * 0.5f, e.y - k * 0.9f },
	                      { e.x - k * 0.2f, e.y + k * 0.8f }, col);
}

namespace
{
void arrow(ImDrawList* dl, const ImVec2& c, float s, ImU32 col, float dir)
{
	const float h = s * 0.46f, t = stroke(s), head = s * 0.24f;
	dl->AddLine({ c.x, c.y - h * dir }, { c.x, c.y + h * dir }, col, t);
	dl->AddTriangleFilled({ c.x,        c.y - h * dir },
	                      { c.x - head, c.y - (h - head) * dir },
	                      { c.x + head, c.y - (h - head) * dir }, col);
}
} // namespace

void iconArrowUp  (ImDrawList* dl, const ImVec2& c, float s, ImU32 col) { arrow(dl, c, s, col,  1.0f); }
void iconArrowDown(ImDrawList* dl, const ImVec2& c, float s, ImU32 col) { arrow(dl, c, s, col, -1.0f); }

// Two nodes on a trunk with a fork — a branch, the way every git UI draws it.
void iconBranch(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.46f, t = stroke(s), r = s * 0.14f;
	const float xl = c.x - s * 0.26f, xr = c.x + s * 0.26f;
	dl->AddLine({ xl, c.y - h + r }, { xl, c.y + h - r }, col, t);
	// The fork: out of the trunk and up into the second node.
	dl->PathLineTo({ xl, c.y + h * 0.05f });
	dl->PathLineTo({ xr - r * 1.2f, c.y + h * 0.05f });
	dl->PathLineTo({ xr, c.y - h + r * 2.2f });
	dl->PathStroke(col, 0, t);
	dl->AddCircleFilled({ xl, c.y - h + r }, r, col, 12);
	dl->AddCircleFilled({ xl, c.y + h - r }, r, col, 12);
	dl->AddCircleFilled({ xr, c.y - h + r }, r, col, 12);
}

void iconCloud(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.5f, t = stroke(s);
	dl->AddCircle({ c.x - h * 0.34f, c.y + h * 0.06f }, h * 0.34f, col, 16, t);
	dl->AddCircle({ c.x + h * 0.10f, c.y - h * 0.14f }, h * 0.44f, col, 16, t);
	dl->AddCircle({ c.x + h * 0.52f, c.y + h * 0.12f }, h * 0.30f, col, 16, t);
	dl->AddLine({ c.x - h * 0.34f, c.y + h * 0.40f },
	            { c.x + h * 0.52f, c.y + h * 0.40f }, col, t);
}

void iconCheck(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.42f, t = stroke(s) * 1.3f;
	dl->PathLineTo({ c.x - h,         c.y + h * 0.06f });
	dl->PathLineTo({ c.x - h * 0.24f, c.y + h * 0.72f });
	dl->PathLineTo({ c.x + h,         c.y - h * 0.66f });
	dl->PathStroke(col, 0, t);
}

void iconWarning(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.48f, t = stroke(s);
	dl->AddTriangle({ c.x, c.y - h }, { c.x + h, c.y + h * 0.78f },
	                { c.x - h, c.y + h * 0.78f }, col, t);
	dl->AddLine({ c.x, c.y - h * 0.30f }, { c.x, c.y + h * 0.22f }, col, t);
	dl->AddCircleFilled({ c.x, c.y + h * 0.52f }, t * 0.7f, col, 8);
}

void iconList(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.40f, t = stroke(s), dot = s * 0.075f;
	for (int i = 0; i < 3; ++i)
	{
		const float y = c.y - h + h * i;
		dl->AddCircleFilled({ c.x - h - dot, y }, dot, col, 8);
		dl->AddLine({ c.x - h * 0.42f, y }, { c.x + h + dot, y }, col, t);
	}
}

void iconTree(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.42f, t = stroke(s), dot = s * 0.075f;
	const float x0 = c.x - h;
	dl->AddLine({ x0, c.y - h }, { x0, c.y + h }, col, t);
	for (int i = 0; i < 2; ++i)
	{
		const float y = c.y + h * (i == 0 ? 0.0f : 1.0f);
		dl->AddLine({ x0, y }, { x0 + h * 0.55f, y }, col, t);
		dl->AddCircleFilled({ x0 + h * 0.55f + dot, y }, dot, col, 8);
		dl->AddLine({ x0 + h * 0.55f + dot * 3.0f, y }, { c.x + h, y }, col, t);
	}
	dl->AddCircleFilled({ x0, c.y - h }, dot, col, 8);
	dl->AddLine({ x0 + dot * 2.0f, c.y - h }, { c.x + h, c.y - h }, col, t);
}

// A clock with its hands at a past hour — history.
void iconHistory(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float r = s * 0.44f, t = stroke(s);
	dl->AddCircle(c, r, col, 22, t);
	dl->AddLine(c, { c.x, c.y - r * 0.60f }, col, t);
	dl->AddLine(c, { c.x - r * 0.46f, c.y + r * 0.16f }, col, t);
}

void iconPlus(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.40f, t = stroke(s) * 1.2f;
	dl->AddLine({ c.x - h, c.y }, { c.x + h, c.y }, col, t);
	dl->AddLine({ c.x, c.y - h }, { c.x, c.y + h }, col, t);
}

} // namespace EditorToolbar

#endif // HE_IMGUI_ENABLED
