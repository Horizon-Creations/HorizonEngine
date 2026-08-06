#include "ViewportToolbar.h"

#ifdef HE_IMGUI_ENABLED

#include "EditorApplication.h"   // AppContext, EditorConfig, EditorMode
#include "EditorCamera.h"
#include "ViewportPanel.h"       // renderSizePx() for the options popup readout
#include "EditorToolbar.h"        // palette, metrics, cell/well — shared with the SC bar

#include <imgui.h>
#include <imgui_internal.h>      // dock node: the hidden-tab-bar "unhide" corner
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ViewportToolbar
{
namespace
{

// ── Scene-only colour ────────────────────────────────────────────────────────
// The shared palette lives in EditorToolbar.h. What stays here is the one thing
// only this bar has: play mode, which tints the whole strip so "am I editing or
// playing?" is answered in peripheral vision rather than by reading a button.
using namespace EditorToolbar;

constexpr ImU32 kPlayFg   = kGood;
constexpr ImU32 kStopFg   = kBad;
constexpr ImU32 kPlayWell = kGoodWell;
constexpr ImU32 kStopWell = kBadWell;
constexpr ImU32 kPlayTint = IM_COL32(222, 92, 92, 16);   // whole-bar wash while running

// ── Icons ────────────────────────────────────────────────────────────────────
// Drawn as vectors rather than shipped as textures: the bar has to look right at
// any UI font scale and on HiDPI, and a 16 px PNG does neither. `s` is the box
// the glyph is fitted into, `c` its centre.
constexpr float kPi = 3.14159265358979323846f;   // IM_PI is in imgui_internal.h

// Mouse pointer — View mode (click to select, drag the gizmo).
void iconCursor(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.5f;
	const ImVec2 p[7] = {
		{ c.x - h * 0.52f, c.y - h },
		{ c.x - h * 0.52f, c.y + h * 0.70f },
		{ c.x - h * 0.14f, c.y + h * 0.28f },
		{ c.x + h * 0.14f, c.y + h },
		{ c.x + h * 0.44f, c.y + h * 0.85f },
		{ c.x + h * 0.14f, c.y + h * 0.18f },
		{ c.x + h * 0.58f, c.y + h * 0.12f },
	};
	dl->AddPolyline(p, 7, col, ImDrawFlags_Closed, stroke(s));
}

// Two peaks — Landscape mode.
void iconTerrain(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.5f;
	dl->AddTriangleFilled({ c.x - h,          c.y + h * 0.72f },
	                      { c.x - h * 0.18f,  c.y - h * 0.30f },
	                      { c.x + h * 0.42f,  c.y + h * 0.72f }, col);
	dl->AddTriangleFilled({ c.x - h * 0.06f,  c.y + h * 0.72f },
	                      { c.x + h * 0.40f,  c.y - h * 0.78f },
	                      { c.x + h,          c.y + h * 0.72f }, col);
}

// Four-way arrow — Move.
void iconMove(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.5f, t = stroke(s), a = s * 0.155f, b = a * 1.15f;
	dl->AddLine({ c.x - h + b, c.y }, { c.x + h - b, c.y }, col, t);
	dl->AddLine({ c.x, c.y - h + b }, { c.x, c.y + h - b }, col, t);
	dl->AddTriangleFilled({ c.x - h, c.y }, { c.x - h + b, c.y - a }, { c.x - h + b, c.y + a }, col);
	dl->AddTriangleFilled({ c.x + h, c.y }, { c.x + h - b, c.y - a }, { c.x + h - b, c.y + a }, col);
	dl->AddTriangleFilled({ c.x, c.y - h }, { c.x - a, c.y - h + b }, { c.x + a, c.y - h + b }, col);
	dl->AddTriangleFilled({ c.x, c.y + h }, { c.x - a, c.y + h - b }, { c.x + a, c.y + h - b }, col);
}

// Open circular arrow — Rotate.
void iconRotate(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float r = s * 0.40f, t = stroke(s);
	const float aEnd = kPi * 1.72f;
	dl->PathArcTo(c, r, kPi * 0.28f, aEnd, 28);
	dl->PathStroke(col, 0, t);

	const ImVec2 e(c.x + std::cos(aEnd) * r, c.y + std::sin(aEnd) * r);
	const ImVec2 tangent(-std::sin(aEnd), std::cos(aEnd));   // direction of travel
	const ImVec2 radial(std::cos(aEnd), std::sin(aEnd));
	const float  k = s * 0.20f;
	dl->AddTriangleFilled({ e.x + tangent.x * k,          e.y + tangent.y * k },
	                      { e.x - radial.x * k * 0.72f,   e.y - radial.y * k * 0.72f },
	                      { e.x + radial.x * k * 0.72f,   e.y + radial.y * k * 0.72f }, col);
}

// Small box → big box on a diagonal — Scale.
void iconScale(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.5f, t = stroke(s);
	const ImVec2 lo(c.x - h * 0.62f, c.y + h * 0.62f);
	const ImVec2 hi(c.x + h * 0.58f, c.y - h * 0.58f);
	dl->AddLine(lo, hi, col, t);
	const float a = s * 0.13f, b = s * 0.21f;
	dl->AddRectFilled({ lo.x - a, lo.y - a }, { lo.x + a, lo.y + a }, col, 1.0f);
	dl->AddRect({ hi.x - b, hi.y - b }, { hi.x + b, hi.y + b }, col, 1.5f, 0, t);
}

// Isometric cube — Local axes.
void iconCube(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float r = s * 0.47f, t = stroke(s);
	ImVec2 p[6];
	for (int i = 0; i < 6; ++i)
	{
		const float a = -kPi * 0.5f + i * (kPi / 3.0f);
		p[i] = ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
	}
	dl->AddPolyline(p, 6, col, ImDrawFlags_Closed, t);
	dl->AddLine(c, p[0], col, t);   // up
	dl->AddLine(c, p[2], col, t);   // lower right
	dl->AddLine(c, p[4], col, t);   // lower left
}

// Globe — World axes.
void iconGlobe(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float r = s * 0.47f, t = stroke(s);
	dl->AddCircle(c, r, col, 24, t);
	dl->AddLine({ c.x - r, c.y }, { c.x + r, c.y }, col, t);
	ImVec2 e[24];
	for (int i = 0; i < 24; ++i)
	{
		const float a = i * (2.0f * kPi / 24.0f);
		e[i] = ImVec2(c.x + std::cos(a) * r * 0.45f, c.y + std::sin(a) * r);
	}
	dl->AddPolyline(e, 24, col, ImDrawFlags_Closed, t);
}

// 3×3 grid — snapping.
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

// Camera body + lens — editor fly speed.
void iconCamera(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.5f, t = stroke(s);
	dl->AddRect({ c.x - h, c.y - h * 0.62f }, { c.x + h * 0.72f, c.y + h * 0.68f }, col, 2.0f, 0, t);
	dl->AddCircle({ c.x - h * 0.16f, c.y + h * 0.03f }, h * 0.32f, col, 16, t);
	dl->AddLine({ c.x - h * 0.55f, c.y - h * 0.62f }, { c.x - h * 0.30f, c.y - h },  col, t);
	dl->AddLine({ c.x - h * 0.30f, c.y - h },         { c.x + h * 0.05f, c.y - h },  col, t);
	dl->AddLine({ c.x + h * 0.05f, c.y - h },         { c.x + h * 0.22f, c.y - h * 0.62f }, col, t);
}

// Two sliders — viewport options.
void iconSliders(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.5f, t = stroke(s), knob = s * 0.15f;
	const float y0 = c.y - h * 0.42f, y1 = c.y + h * 0.42f;
	dl->AddLine({ c.x - h, y0 }, { c.x + h, y0 }, col, t);
	dl->AddLine({ c.x - h, y1 }, { c.x + h, y1 }, col, t);
	dl->AddCircleFilled({ c.x - h * 0.30f, y0 }, knob, col, 12);
	dl->AddCircleFilled({ c.x + h * 0.34f, y1 }, knob, col, 12);
}

// Fallbacks for the transport, used when Play.tga / Stop.tga failed to load.
void iconPlay(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.5f;
	dl->AddTriangleFilled({ c.x - h * 0.72f, c.y - h },
	                      { c.x - h * 0.72f, c.y + h },
	                      { c.x + h * 0.86f, c.y }, col);
}

void iconStop(ImDrawList* dl, const ImVec2& c, float s, ImU32 col)
{
	const float h = s * 0.42f;
	dl->AddRectFilled({ c.x - h, c.y - h }, { c.x + h, c.y + h }, col, 1.5f);
}

// Snap increment for the armed operation, formatted for the value cell.
void snapText(const State& st, ImGuizmo::OPERATION op, char* buf, size_t n)
{
	if (op == ImGuizmo::ROTATE)     std::snprintf(buf, n, "%g\xc2\xb0", st.snapRotate);
	else if (op == ImGuizmo::SCALE) std::snprintf(buf, n, "%g\xc3\x97", st.snapScale);
	else                            std::snprintf(buf, n, "%g m",       st.snapTranslate);
}

// The snap presets a viewport actually wants, per operation.
void snapPresets(State& st, ImGuizmo::OPERATION op)
{
	struct Preset { const char* label; float value; };
	static const Preset kMove[]   = { {"0.1 m",1e-1f}, {"0.25 m",0.25f}, {"0.5 m",0.5f},
	                                  {"1 m",1.0f},    {"5 m",5.0f},     {"10 m",10.0f} };
	static const Preset kRotate[] = { {"1\xc2\xb0",1.0f},  {"5\xc2\xb0",5.0f},  {"10\xc2\xb0",10.0f},
	                                  {"15\xc2\xb0",15.0f},{"45\xc2\xb0",45.0f},{"90\xc2\xb0",90.0f} };
	static const Preset kScale[]  = { {"0.05\xc3\x97",0.05f}, {"0.1\xc3\x97",0.1f}, {"0.25\xc3\x97",0.25f},
	                                  {"0.5\xc3\x97",0.5f},   {"1\xc3\x97",1.0f} };

	const Preset* list  = kMove;
	int           count = IM_ARRAYSIZE(kMove);
	float*        value = &st.snapTranslate;
	const char*   title = "Move snap";
	float         step  = 0.1f;
	if (op == ImGuizmo::ROTATE)
	{
		list = kRotate; count = IM_ARRAYSIZE(kRotate); value = &st.snapRotate;
		title = "Rotate snap"; step = 1.0f;
	}
	else if (op == ImGuizmo::SCALE)
	{
		list = kScale; count = IM_ARRAYSIZE(kScale); value = &st.snapScale;
		title = "Scale snap"; step = 0.05f;
	}

	ImGui::TextDisabled("%s", title);
	ImGui::Separator();
	for (int i = 0; i < count; ++i)
		if (ImGui::Selectable(list[i].label, *value == list[i].value))
		{
			*value = list[i].value;
			st.snapEnabled = true;
		}
	ImGui::Separator();
	ImGui::SetNextItemWidth(110.0f);
	if (ImGui::InputFloat("##custom", value, step, step * 4.0f, "%g"))
		*value = std::max(1e-4f, *value);
}

// Everything the bar can hide when it runs out of room, plus the settings that
// were never worth a permanent cell.
void optionsPopup(AppContext& ctx, State& st)
{
	ImGui::TextDisabled("Snapping");
	ImGui::Separator();
	ImGui::Checkbox("Snap to grid", &st.snapEnabled);
	ImGui::SetNextItemWidth(90.0f);
	ImGui::InputFloat("Move (m)",    &st.snapTranslate, 0.1f, 1.0f, "%g");
	ImGui::SetNextItemWidth(90.0f);
	ImGui::InputFloat("Rotate (\xc2\xb0)", &st.snapRotate, 1.0f, 15.0f, "%g");
	ImGui::SetNextItemWidth(90.0f);
	ImGui::InputFloat("Scale (\xc3\x97)",  &st.snapScale, 0.05f, 0.25f, "%g");
	st.snapTranslate = std::max(1e-4f, st.snapTranslate);
	st.snapRotate    = std::max(1e-4f, st.snapRotate);
	st.snapScale     = std::max(1e-4f, st.snapScale);

	ImGui::Spacing();
	ImGui::TextDisabled("Gizmo");
	ImGui::Separator();
	ImGui::Checkbox("Screen-space rotation ring", &st.rotateScreenRing);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("The rotate gizmo's outer ring, which rotates about the view axis");

	ImGui::Spacing();
	ImGui::TextDisabled("Camera");
	ImGui::Separator();
	ImGui::SetNextItemWidth(160.0f);
	if (ImGui::SliderFloat("Speed##vpCam", &ctx.editorConfig.EditorCameraSpeed,
	                       1.0f, 50.0f, "%.1f u/s") && ctx.editorCamera)
		ctx.editorCamera->setFlySpeed(ctx.editorConfig.EditorCameraSpeed);

	ImGui::Spacing();
	ImGui::TextDisabled("Viewport");
	ImGui::Separator();
	int pxW = 0, pxH = 0;
	ViewportPanel::renderSizePx(pxW, pxH);
	ImGui::Text("Render target: %d \xc3\x97 %d px", pxW, pxH);
}

} // namespace

const float* State::activeSnap() const
{
	if (!snapEnabled) return nullptr;
	const float v = (op == ImGuizmo::ROTATE) ? snapRotate
	              : (op == ImGuizmo::SCALE)  ? snapScale
	                                         : snapTranslate;
	m_snapBuf[0] = m_snapBuf[1] = m_snapBuf[2] = v;
	return m_snapBuf;
}

float height() { return EditorToolbar::height(); }

void render(AppContext& ctx, State& st)
{
	ImDrawList*  dl     = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float  barW   = ImGui::GetContentRegionAvail().x;
	const Metrics m     = metrics(origin.y);
	const bool   playing = ctx.isPlaying;

	// ── The strip itself ────────────────────────────────────────────────────
	// Its own darker band plus a hairline, so it reads as chrome rather than as
	// controls floating on the panel background. While the scene is running the
	// band is washed red and the hairline turns red with it — the "am I editing
	// or playing?" answer belongs in peripheral vision, not in a button icon.
	EditorToolbar::bar(origin, barW, m,
	                   playing ? kPlayTint : 0u,
	                   playing ? kStopFg : kBarLine,
	                   playing ? 2.0f : 1.0f);

	// The Scene panel starts with its dock tab bar hidden, and ImGui's way back —
	// a small triangle in the node's corner — is drawn during Begin(), i.e. UNDER
	// the band above. Re-draw it on top and keep the first group clear of it;
	// otherwise the only route back to the tab bar is an invisible 8 px corner.
	// Interaction is ImGui's ("#UNHIDE" already ran in Begin), this is paint only.
	float edgeL = kEdgeGap;
	{
		ImGuiWindow*   win  = ImGui::GetCurrentWindow();
		ImGuiDockNode* node = win ? win->DockNode : nullptr;
		if (win && win->DockIsActive && node && node->IsHiddenTabBar() && !node->IsNoTabBar())
		{
			const float  sz  = std::floor(ImGui::GetFontSize() * 0.70f);
			const float  hit = std::floor(ImGui::GetFontSize() * 0.55f);
			const ImVec2 p   = node->Pos;
			const bool   hov = ImGui::IsMouseHoveringRect(p, ImVec2(p.x + hit, p.y + hit), false);
			dl->AddTriangleFilled(p, ImVec2(p.x + sz, p.y), ImVec2(p.x, p.y + sz),
				ImGui::GetColorU32(hov ? ImGuiCol_ButtonHovered : ImGuiCol_Button));
			edgeL = std::max(kEdgeGap, sz + 3.0f);
		}
	}

	// ── Fit ─────────────────────────────────────────────────────────────────
	// Measure first, draw second: the transport has to be centred on the strip,
	// not on "whatever is left after the left-hand controls", and the shrink
	// steps below need the totals anyway. The orientation cell is measured with
	// the wider of its two labels so toggling it never reflows the row.
	const char* orientLabel = (st.mode == ImGuizmo::WORLD) ? "World" : "Local";
	const float orientW     = std::max(cellWidth(m, "World"), cellWidth(m, "Local"));
	const float snapValW    = std::floor(ImGui::CalcTextSize("88.88\xc2\xb0").x + kCellPadX * 2.0f);
	const float camValW     = std::floor(ImGui::CalcTextSize("88.8 u/s").x + kCellPadX * 2.0f);
	const float playW       = std::floor(m.cell * 2.0f);

	auto leftWidth = [&](bool labels, bool snap)
	{
		float w = kWellPad * 2.0f + cellWidth(m, labels ? "View" : nullptr) + kSegGap
		                          + cellWidth(m, labels ? "Landscape" : nullptr);
		w += kGroupGap + kWellPad * 2.0f + m.cell * 3.0f + kSegGap * 2.0f;
		w += kGroupGap + kWellPad * 2.0f + (labels ? orientW : m.cell);
		if (snap) w += kGroupGap + kWellPad * 2.0f + m.cell + kSegGap + snapValW;
		return w;
	};
	auto rightWidth = [&](bool camera)
	{
		float w = kWellPad * 2.0f + m.cell;                       // options button
		if (camera) w += kWellPad * 2.0f + m.cell + kSegGap + camValW + kGroupGap;
		return w;
	};

	const float centreW = playW + kWellPad * 2.0f;
	const float slack   = edgeL + kEdgeGap + kGroupGap * 2.0f;     // breathing room around the transport
	bool labels = true, showCamera = true, showSnap = true;
	auto fits = [&] { return leftWidth(labels, showSnap) + centreW + rightWidth(showCamera) + slack <= barW; };
	if (!fits()) labels     = false;
	if (!fits()) showCamera = false;
	if (!fits()) showSnap   = false;

	const float wLeft  = leftWidth(labels, showSnap);
	const float wRight = rightWidth(showCamera);

	// ── Left zone: what the mouse does in the viewport ───────────────────────
	float x = origin.x + edgeL;

	// Editor mode. Two entries today, but the row is the natural home for any
	// future one (paint, foliage), so it stays a segmented list rather than a
	// checkbox pretending to be a mode.
	{
		const float wView = cellWidth(m, labels ? "View" : nullptr);
		const float wLand = cellWidth(m, labels ? "Landscape" : nullptr);
		well(m, x, kWellPad * 2.0f + wView + kSegGap + wLand);
		float cx = x + kWellPad;
		if (cell(m, cx, wView, "##vpModeView", iconCursor, labels ? "View" : nullptr,
		         ctx.editorConfig.mode == EditorMode::View, true,
		         "View mode — select and transform entities"))
			ctx.editorConfig.mode = EditorMode::View;
		cx += wView + kSegGap;
		if (cell(m, cx, wLand, "##vpModeLand", iconTerrain, labels ? "Landscape" : nullptr,
		         ctx.editorConfig.mode == EditorMode::Landscape, true,
		         "Landscape mode — sculpt and paint terrain"))
			ctx.editorConfig.mode = EditorMode::Landscape;
		x += kWellPad * 2.0f + wView + kSegGap + wLand + kGroupGap;
	}

	// Manipulation tools. Dead in Landscape mode (the gizmo is suppressed there
	// so a stray drag can't move the terrain out from under the brush), so they
	// dim instead of vanishing — the row keeps its shape either way.
	const bool gizmoUsable = ctx.editorConfig.mode != EditorMode::Landscape;
	{
		well(m, x, kWellPad * 2.0f + m.cell * 3.0f + kSegGap * 2.0f);
		float cx = x + kWellPad;
		struct Tool { const char* id; IconFn icon; ImGuizmo::OPERATION op; const char* tip; };
		const Tool tools[3] = {
			{ "##vpMove",   iconMove,   ImGuizmo::TRANSLATE, "Move (W)"   },
			{ "##vpRotate", iconRotate, ImGuizmo::ROTATE,    "Rotate (E)" },
			{ "##vpScale",  iconScale,  ImGuizmo::SCALE,     "Scale (R)"  },
		};
		for (const Tool& t : tools)
		{
			if (cell(m, cx, m.cell, t.id, t.icon, nullptr, st.op == t.op, gizmoUsable, t.tip))
				st.op = t.op;
			cx += m.cell + kSegGap;
		}
		x += kWellPad * 2.0f + m.cell * 3.0f + kSegGap * 2.0f + kGroupGap;
	}

	// Gizmo orientation. A two-state combo is one click too many for something
	// toggled this often; the cell shows the state it is IN and flips on click.
	{
		const float w = labels ? orientW : m.cell;
		well(m, x, kWellPad * 2.0f + w);
		if (cell(m, x + kWellPad, w, "##vpOrient",
		         (st.mode == ImGuizmo::WORLD) ? iconGlobe : iconCube,
		         labels ? orientLabel : nullptr, false, gizmoUsable,
		         st.mode == ImGuizmo::WORLD
		             ? "World axes — click for Local (object) axes"
		             : "Local axes — click for World (axis-aligned) axes"))
			st.mode = (st.mode == ImGuizmo::WORLD) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
		x += kWellPad * 2.0f + w + kGroupGap;
	}

	// Snapping: toggle + increment. The increment follows the armed tool, since
	// metres, degrees and factors are three different numbers.
	if (showSnap)
	{
		well(m, x, kWellPad * 2.0f + m.cell + kSegGap + snapValW);
		if (cell(m, x + kWellPad, m.cell, "##vpSnap", iconGrid, nullptr,
		         st.snapEnabled, gizmoUsable, "Snap to grid while dragging the gizmo"))
			st.snapEnabled = !st.snapEnabled;

		char buf[32];
		snapText(st, st.op, buf, sizeof(buf));
		const float vx = x + kWellPad + m.cell + kSegGap;
		ImGui::SetCursorScreenPos(ImVec2(vx, m.y + kWellPad));
		if (!gizmoUsable) ImGui::BeginDisabled();
		const bool pressed = ImGui::InvisibleButton("##vpSnapValue", ImVec2(snapValW, m.cell));
		const bool hovered = ImGui::IsItemHovered();
		if (gizmoUsable) ImGui::SetItemTooltip("Snap increment for the active tool");
		if (!gizmoUsable) ImGui::EndDisabled();
		if (hovered)
			dl->AddRectFilled(ImVec2(vx, m.y + kWellPad),
			                  ImVec2(vx + snapValW, m.y + kWellPad + m.cell), kHoverBg, kCellRound);
		const ImU32 fg = !gizmoUsable ? kFgDim : (st.snapEnabled ? kFgOn : kFg);
		const float tw = ImGui::CalcTextSize(buf).x;
		dl->AddText(ImVec2(std::floor(vx + (snapValW - tw) * 0.5f),
		                   std::floor(m.cy - ImGui::GetFontSize() * 0.5f)), fg, buf);
		if (pressed && gizmoUsable) ImGui::OpenPopup("##vpSnapPopup");
		if (ImGui::BeginPopup("##vpSnapPopup"))
		{
			snapPresets(st, st.op);
			ImGui::EndPopup();
		}
		x += kWellPad * 2.0f + m.cell + kSegGap + snapValW + kGroupGap;
	}

	// ── Centre zone: what the editor is doing ───────────────────────────────
	// Centred on the strip, then clamped so it can never land under a group —
	// the old hand-tuned offset only looked centred at one panel width.
	{
		const float leftEnd    = origin.x + edgeL + wLeft;
		const float rightStart = origin.x + barW - kEdgeGap - wRight;
		float cx = origin.x + (barW - centreW) * 0.5f;
		cx = std::min(cx, rightStart - kGroupGap - centreW);
		cx = std::max(cx, leftEnd + kGroupGap);

		well(m, cx, centreW, playing ? kStopWell : kPlayWell);

		const ImVec2 p0(cx + kWellPad, m.y + kWellPad);
		ImGui::SetCursorScreenPos(p0);
		const bool pressed = ImGui::InvisibleButton("##vpPlay", ImVec2(playW, m.cell));
		const bool hovered = ImGui::IsItemHovered();
		const bool held    = ImGui::IsItemActive();
		ImGui::SetItemTooltip("%s", playing ? "Stop — return to the edited scene"
		                                    : "Play — run the scene in the viewport");
		if (hovered || held)
			dl->AddRectFilled(p0, ImVec2(p0.x + playW, p0.y + m.cell),
			                  held ? kDownBg : kHoverBg, kCellRound);

		const ImU32   fg   = playing ? kStopFg : kPlayFg;
		const ImTextureID tex = playing ? ctx.toolbarIcons.stop : ctx.toolbarIcons.play;
		const ImVec2  c(std::floor(p0.x + playW * 0.5f), std::floor(m.cy));
		const float   g = std::floor(m.cell * 0.62f);
		if (tex)
			dl->AddImage(tex, ImVec2(c.x - g * 0.5f, c.y - g * 0.5f),
			                  ImVec2(c.x + g * 0.5f, c.y + g * 0.5f),
			             ImVec2(0, 0), ImVec2(1, 1), fg);
		else if (playing) iconStop(dl, c, g, fg);
		else              iconPlay(dl, c, g, fg);

		if (pressed && ctx.setPlayMode) ctx.setPlayMode(!playing);
	}

	// ── Right zone: how the viewport looks ──────────────────────────────────
	{
		float rx = origin.x + barW - kEdgeGap - wRight;

		// Fly speed. It lives in Preferences, which is the wrong distance away
		// from a camera that is too fast for the room you are standing in.
		if (showCamera)
		{
			const float w = kWellPad * 2.0f + m.cell + kSegGap + camValW;
			well(m, rx, w);
			// Decorative — it labels the number next to it, so it is drawn
			// rather than submitted as an item nothing would ever click.
			iconCamera(dl, ImVec2(std::floor(rx + kWellPad + m.cell * 0.5f), std::floor(m.cy)),
			           m.icon, kFg);

			ImGui::SetCursorScreenPos(ImVec2(rx + kWellPad + m.cell + kSegGap, m.y + kWellPad));
			ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kHoverBg);
			ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  kDownBg);
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kCellRound);
			ImGui::SetNextItemWidth(camValW);
			if (ImGui::DragFloat("##vpCamSpeed", &ctx.editorConfig.EditorCameraSpeed,
			                     0.25f, 1.0f, 200.0f, "%.1f u/s") && ctx.editorCamera)
				ctx.editorCamera->setFlySpeed(ctx.editorConfig.EditorCameraSpeed);
			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);
			ImGui::SetItemTooltip("Editor fly-camera speed (units/second) — drag or double-click");
			rx += w + kGroupGap;
		}

		// Options. The overflow target, so it is the one thing never dropped.
		well(m, rx, kWellPad * 2.0f + m.cell);
		if (cell(m, rx + kWellPad, m.cell, "##vpOptions", iconSliders, nullptr, false, true,
		         "Viewport options — snapping, gizmo, camera"))
			ImGui::OpenPopup("##vpOptionsPopup");
		if (ImGui::BeginPopup("##vpOptionsPopup"))
		{
			optionsPopup(ctx, st);
			ImGui::EndPopup();
		}
	}

	// Hand the rest of the window to the viewport image.
	ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + m.bar));
}

} // namespace ViewportToolbar

#endif // HE_IMGUI_ENABLED
