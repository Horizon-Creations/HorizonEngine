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


// ─── Bar builder ─────────────────────────────────────────────────────────────

Bar::Bar()
{
	m_dl     = ImGui::GetWindowDrawList();
	m_origin = ImGui::GetCursorScreenPos();
	m_width  = ImGui::GetContentRegionAvail().x;
	m_m      = EditorToolbar::metrics(m_origin.y);

	EditorToolbar::bar(m_origin, m_width, m_m);

	// Channel 0 carries the wells, channel 1 the cells on top of them. That is
	// what lets a well be painted after the cells it sits behind — its width is
	// not known until the group closes, and measuring everything twice for that
	// is how the two hand-built bars ended up with their own copies of the
	// arithmetic.
	m_dl->ChannelsSplit(2);
	m_dl->ChannelsSetCurrent(1);

	m_left  = m_origin.x + kEdgeGap;
	m_right = m_origin.x + m_width - kEdgeGap;
}

Bar::~Bar()
{
	if (m_inGroup) endGroup();
	m_dl->ChannelsMerge();
	// Hand the rest of the window to the panel body. A bare SetCursorScreenPos
	// leaves ImGui's IsSetPos flag armed until the next item — if the body then
	// submits nothing (e.g. an emptied Content-Browser folder), EndChild trips
	// the "extend window boundaries" error check. The zero-size Dummy (with
	// zero spacing so the cursor stays put) commits the position as a real
	// item; the final SetCursorScreenPos restores the exact hand-off point and
	// is harmless now that CursorMaxPos already covers it.
	const ImVec2 handOff(m_origin.x, m_origin.y + m_m.bar);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
	ImGui::SetCursorScreenPos(handOff);
	ImGui::Dummy(ImVec2(0.0f, 0.0f));
	ImGui::PopStyleVar();
	ImGui::SetCursorScreenPos(handOff);
}

void Bar::tint(ImU32 wash, ImU32 line, float thickness)
{
	// Painted into the well channel so it stays under every cell.
	m_dl->ChannelsSetCurrent(0);
	if (wash)
		m_dl->AddRectFilled(m_origin, ImVec2(m_origin.x + m_width, m_origin.y + m_m.bar), wash);
	m_dl->AddLine(ImVec2(m_origin.x,            m_origin.y + m_m.bar - 1.0f),
	              ImVec2(m_origin.x + m_width,  m_origin.y + m_m.bar - 1.0f), line, thickness);
	m_dl->ChannelsSetCurrent(1);
}

void Bar::group()
{
	if (m_inGroup) endGroup();
	m_inGroup      = true;
	m_groupIsRight = false;
	m_groupX       = m_left;
	m_cursor       = kWellPad;      // opening padding; cells add themselves
	m_first        = true;
}

void Bar::rightGroup(float width)
{
	if (m_inGroup) endGroup();
	m_inGroup      = true;
	m_groupIsRight = true;
	m_groupX       = m_right - width;
	m_cursor       = kWellPad;      // cells fill in from the group's LEFT edge
	m_groupW       = width;
	m_first        = true;
	m_right        = m_groupX - kGroupGap;
}

void Bar::endGroup()
{
	if (!m_inGroup) return;
	const float w = m_groupIsRight ? m_groupW : m_cursor + kWellPad;

	m_dl->ChannelsSetCurrent(0);
	EditorToolbar::well(m_m, m_groupX, w);
	m_dl->ChannelsSetCurrent(1);

	if (!m_groupIsRight) m_left = m_groupX + w + kGroupGap;
	m_inGroup = false;
}

void Bar::gap() { if (!m_inGroup) m_left += kGroupGap; }

bool Bar::item(const char* id, IconFn icon, const char* label, bool on, bool enabled,
               const char* tooltip)
{
	const float w = EditorToolbar::cellWidth(m_m, label);
	if (!m_first) m_cursor += kSegGap;
	const float x = m_groupX + m_cursor;
	m_cursor += w;
	m_first = false;
	return EditorToolbar::cell(m_m, x, w, id, icon, label, on, enabled, tooltip);
}

bool Bar::itemTinted(const char* id, IconFn icon, const char* label, ImU32 fg,
                     bool enabled, const char* tooltip)
{
	const float w = EditorToolbar::cellWidth(m_m, label);
	if (!m_first) m_cursor += kSegGap;
	const float x = m_groupX + m_cursor;
	m_cursor += w;
	m_first = false;
	return EditorToolbar::cellTinted(m_m, x, w, id, icon, label, fg, enabled, tooltip);
}

void Bar::readout(IconFn icon, const char* label, ImU32 fg)
{
	const float w = EditorToolbar::cellWidth(m_m, label);
	if (!m_first) m_cursor += kSegGap;
	const float x = m_groupX + m_cursor;
	m_cursor += w;
	m_first = false;

	const float labelW   = label ? ImGui::CalcTextSize(label).x : 0.0f;
	const float iconW    = icon ? m_m.icon : 0.0f;
	const float gapX     = (icon && label) ? kLabelGap : 0.0f;
	const float left     = x + (w - (iconW + gapX + labelW)) * 0.5f;
	if (icon) icon(m_dl, ImVec2(std::floor(left + m_m.icon * 0.5f), std::floor(m_m.cy)),
	               m_m.icon, fg);
	if (label)
		m_dl->AddText(ImVec2(std::floor(left + iconW + gapX),
		                     std::floor(m_m.cy - ImGui::GetFontSize() * 0.5f)), fg, label);
}

void Bar::divider()
{
	if (m_first) return;
	const float x = m_groupX + m_cursor + kSegGap + 1.0f;
	m_dl->AddLine(ImVec2(x, m_m.y + kWellPad * 2.0f),
	              ImVec2(x, m_m.y + m_m.wellH - kWellPad * 2.0f), kBarLine, 1.0f);
	m_cursor += kSegGap * 2.0f + 2.0f;
}

void Bar::label(const char* text, ImU32 col)
{
	if (m_inGroup) endGroup();
	const float w = ImGui::CalcTextSize(text).x;
	m_dl->AddText(ImVec2(std::floor(m_left),
	                     std::floor(m_m.cy - ImGui::GetFontSize() * 0.5f)), col, text);
	m_left += w + kGroupGap;
}

float Bar::iconGroupWidth(int n) const
{
	if (n <= 0) return 0.0f;
	return kWellPad * 2.0f + n * m_m.cell + (n - 1) * kSegGap;
}

float Bar::labelGroupWidth(std::initializer_list<const char*> labels) const
{
	float w = kWellPad * 2.0f;
	int   n = 0;
	for (const char* l : labels) { w += EditorToolbar::cellWidth(m_m, l); ++n; }
	if (n > 1) w += (n - 1) * kSegGap;
	return w;
}

float Bar::remaining() const
{
	return m_right - (m_inGroup ? m_groupX + m_cursor + kWellPad : m_left);
}

// ─── Asset-editor header ─────────────────────────────────────────────────────

void assetHeader(Bar& bar, const char* name, IconFn kindIcon, bool dirty)
{
	bar.group();
	bar.readout(kindIcon, name && *name ? name : "(unnamed)");
	// The dot is the whole message: a word would be read once and then stop
	// being noticed, a mark in the one colour the editor uses for "needs
	// attention" keeps working from the corner of the eye.
	if (dirty) bar.readout(nullptr, "\xe2\x97\x8f", kWarn);
	bar.endGroup();
}

bool saveButton(Bar& bar, bool enabled)
{
	bar.rightGroup(bar.iconGroupWidth(1));
	const bool pressed = bar.item("##save", iconSave, nullptr, false, enabled,
	                              enabled ? "Save (Cmd/Ctrl+S)" : "Nothing to save");
	bar.endGroup();
	return pressed;
}

// ─── More icons ──────────────────────────────────────────────────────────────

// Floppy: outer body, shutter at the top, label at the bottom.
void iconSave(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.46f, t = stroke(s);
	dl->AddRect({ c.x - h, c.y - h }, { c.x + h, c.y + h }, col, 2.0f, 0, t);
	dl->AddRectFilled({ c.x - h * 0.52f, c.y - h }, { c.x + h * 0.42f, c.y - h * 0.24f }, col, 1.0f);
	dl->AddRect({ c.x - h * 0.60f, c.y + h * 0.14f }, { c.x + h * 0.60f, c.y + h }, col, 1.0f, 0, t);
}

void iconPlay(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.5f;
	dl->AddTriangleFilled({ c.x - h * 0.72f, c.y - h },
	                      { c.x - h * 0.72f, c.y + h },
	                      { c.x + h * 0.86f, c.y }, col);
}

void iconPause(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.44f, w = s * 0.14f, gap = s * 0.12f;
	dl->AddRectFilled({ c.x - gap - w * 2.0f, c.y - h }, { c.x - gap, c.y + h }, col, 1.0f);
	dl->AddRectFilled({ c.x + gap, c.y - h }, { c.x + gap + w * 2.0f, c.y + h }, col, 1.0f);
}

void iconStop(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.42f;
	dl->AddRectFilled({ c.x - h, c.y - h }, { c.x + h, c.y + h }, col, 1.5f);
}

void iconGrid(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.45f, t = stroke(s), third = (h * 2.0f) / 3.0f;
	dl->AddRect({ c.x - h, c.y - h }, { c.x + h, c.y + h }, col, 1.5f, 0, t);
	for (int i = 1; i < 3; ++i)
	{
		const float o = -h + third * i;
		dl->AddLine({ c.x + o, c.y - h }, { c.x + o, c.y + h }, col, t * 0.8f);
		dl->AddLine({ c.x - h, c.y + o }, { c.x + h, c.y + o }, col, t * 0.8f);
	}
}

// Angle brackets — source text.
void iconCode(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.42f, t = stroke(s);
	dl->PathLineTo({ c.x - h * 0.30f, c.y - h });
	dl->PathLineTo({ c.x - h,         c.y });
	dl->PathLineTo({ c.x - h * 0.30f, c.y + h });
	dl->PathStroke(col, 0, t);
	dl->PathLineTo({ c.x + h * 0.30f, c.y - h });
	dl->PathLineTo({ c.x + h,         c.y });
	dl->PathLineTo({ c.x + h * 0.30f, c.y + h });
	dl->PathStroke(col, 0, t);
}

void iconEye(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.46f, t = stroke(s);
	// Two arcs meeting at the corners — a lens, not a circle.
	dl->PathLineTo({ c.x - h, c.y });
	dl->PathBezierCubicCurveTo({ c.x - h * 0.45f, c.y - h * 0.72f },
	                           { c.x + h * 0.45f, c.y - h * 0.72f },
	                           { c.x + h,         c.y }, 16);
	dl->PathBezierCubicCurveTo({ c.x + h * 0.45f, c.y + h * 0.72f },
	                           { c.x - h * 0.45f, c.y + h * 0.72f },
	                           { c.x - h,         c.y }, 16);
	dl->PathStroke(col, 0, t);
	dl->AddCircleFilled(c, h * 0.28f, col, 12);
}

// Three stacked plates.
void iconLayers(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float w = s * 0.46f, t = stroke(s);
	for (int i = 0; i < 3; ++i)
	{
		const float y = c.y - s * 0.26f + i * s * 0.26f;
		if (i == 0)
			dl->AddQuadFilled({ c.x, y - s * 0.16f }, { c.x + w, y },
			                  { c.x, y + s * 0.16f }, { c.x - w, y }, col);
		else
			dl->AddQuad({ c.x, y - s * 0.16f }, { c.x + w, y },
			            { c.x, y + s * 0.16f }, { c.x - w, y }, col, t * 0.9f);
	}
}

void iconHammer(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.46f, t = stroke(s);
	dl->AddLine({ c.x + h * 0.42f, c.y - h * 0.30f }, { c.x - h * 0.55f, c.y + h }, col, t * 1.6f);
	dl->AddQuadFilled({ c.x - h * 0.05f, c.y - h },
	                  { c.x + h,         c.y - h * 0.42f },
	                  { c.x + h * 0.72f, c.y - h * 0.06f },
	                  { c.x - h * 0.32f, c.y - h * 0.62f }, col);
}

void iconFolder(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.40f, t = stroke(s);
	dl->PathLineTo({ c.x - h * 1.15f, c.y + h });
	dl->PathLineTo({ c.x - h * 1.15f, c.y - h });
	dl->PathLineTo({ c.x - h * 0.30f, c.y - h });
	dl->PathLineTo({ c.x - h * 0.02f, c.y - h * 0.56f });
	dl->PathLineTo({ c.x + h * 1.15f, c.y - h * 0.56f });
	dl->PathLineTo({ c.x + h * 1.15f, c.y + h });
	dl->PathStroke(col, ImDrawFlags_Closed, t);
}

void iconSearch(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float r = s * 0.30f, t = stroke(s);
	const ImVec2 o(c.x - s * 0.08f, c.y - s * 0.08f);
	dl->AddCircle(o, r, col, 20, t);
	dl->AddLine({ o.x + r * 0.72f, o.y + r * 0.72f },
	            { c.x + s * 0.44f, c.y + s * 0.44f }, col, t * 1.3f);
}

// Four corner brackets — frame the content.
void iconFit(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.44f, t = stroke(s), a = s * 0.24f;
	const float xs[2] = { c.x - h, c.x + h };
	const float ys[2] = { c.y - h, c.y + h };
	for (int i = 0; i < 2; ++i)
		for (int j = 0; j < 2; ++j)
		{
			const float sx = i ? -1.0f : 1.0f, sy = j ? -1.0f : 1.0f;
			dl->AddLine({ xs[i], ys[j] }, { xs[i] + a * sx, ys[j] }, col, t);
			dl->AddLine({ xs[i], ys[j] }, { xs[i], ys[j] + a * sy }, col, t);
		}
}

void iconTrash(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.44f, t = stroke(s), w = s * 0.30f;
	dl->AddLine({ c.x - w * 1.35f, c.y - h * 0.55f }, { c.x + w * 1.35f, c.y - h * 0.55f }, col, t);
	dl->AddLine({ c.x - w * 0.45f, c.y - h }, { c.x + w * 0.45f, c.y - h }, col, t);
	dl->AddRect({ c.x - w, c.y - h * 0.40f }, { c.x + w, c.y + h }, col, 1.5f, 0, t);
	dl->AddLine({ c.x, c.y - h * 0.10f }, { c.x, c.y + h * 0.66f }, col, t * 0.8f);
}

// A long bone with two knuckles — skeletal data.
void iconBone(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.40f, t = stroke(s), r = s * 0.13f;
	const ImVec2 a(c.x - h * 0.78f, c.y + h * 0.78f);
	const ImVec2 b(c.x + h * 0.78f, c.y - h * 0.78f);
	dl->AddLine(a, b, col, t * 1.5f);
	dl->AddCircle({ a.x - r * 0.5f, a.y + r * 0.5f }, r, col, 12, t);
	dl->AddCircle({ b.x + r * 0.5f, b.y - r * 0.5f }, r, col, 12, t);
}

void iconBrush(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.44f, t = stroke(s);
	dl->AddLine({ c.x + h * 0.72f, c.y - h }, { c.x - h * 0.16f, c.y + h * 0.14f }, col, t * 1.7f);
	dl->AddTriangleFilled({ c.x - h * 0.44f, c.y - h * 0.10f },
	                      { c.x + h * 0.10f, c.y + h * 0.42f },
	                      { c.x - h,         c.y + h }, col);
}

// Horizontal mirror: an arrow each way across a dashed axis.
void iconFlip(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.44f, t = stroke(s);
	for (int i = 0; i < 3; ++i)
	{
		const float y = c.y - h + i * h;
		dl->AddLine({ c.x, y }, { c.x, y + h * 0.55f }, col, t * 0.8f);
	}
	dl->AddLine({ c.x - h, c.y }, { c.x - h * 0.30f, c.y }, col, t);
	dl->AddLine({ c.x + h * 0.30f, c.y }, { c.x + h, c.y }, col, t);
	const float k = s * 0.16f;
	dl->AddTriangleFilled({ c.x - h, c.y }, { c.x - h + k, c.y - k }, { c.x - h + k, c.y + k }, col);
	dl->AddTriangleFilled({ c.x + h, c.y }, { c.x + h - k, c.y - k }, { c.x + h - k, c.y + k }, col);
}

// A four-pointed star with a small companion — particles.
void iconSparkle(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	auto star = [&](ImVec2 o, float r)
	{
		dl->AddQuadFilled({ o.x, o.y - r }, { o.x + r * 0.30f, o.y },
		                  { o.x, o.y + r }, { o.x - r * 0.30f, o.y }, col);
		dl->AddQuadFilled({ o.x - r, o.y }, { o.x, o.y - r * 0.30f },
		                  { o.x + r, o.y }, { o.x, o.y + r * 0.30f }, col);
	};
	star({ c.x - s * 0.10f, c.y - s * 0.06f }, s * 0.34f);
	star({ c.x + s * 0.30f, c.y + s * 0.28f }, s * 0.17f);
}

// A frame with a title bar — a widget/panel.
void iconWidget(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.44f, t = stroke(s);
	dl->AddRect({ c.x - h, c.y - h }, { c.x + h, c.y + h }, col, 2.0f, 0, t);
	dl->AddRectFilled({ c.x - h, c.y - h }, { c.x + h, c.y - h * 0.42f }, col, 2.0f,
	                  ImDrawFlags_RoundCornersTop);
	dl->AddRect({ c.x - h * 0.55f, c.y - h * 0.10f }, { c.x + h * 0.30f, c.y + h * 0.62f },
	            col, 1.0f, 0, t * 0.8f);
}

// A sound wave: a centre line with a symmetric envelope around it.
void iconWave(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.42f, t = stroke(s);
	// Four bars of rising then falling height — the shape of a waveform view,
	// which is what the audio tab actually shows.
	const float amp[4] = { 0.34f, 0.92f, 0.55f, 0.78f };
	for (int i = 0; i < 4; ++i)
	{
		const float x = c.x + (static_cast<float>(i) - 1.5f) * (h * 0.52f);
		const float a = h * amp[i];
		dl->AddLine({ x, c.y - a }, { x, c.y + a }, col, t * 1.2f);
	}
}

} // namespace EditorToolbar

#endif // HE_IMGUI_ENABLED
