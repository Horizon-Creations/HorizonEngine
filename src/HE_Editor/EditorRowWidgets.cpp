#include "EditorWidgets.h"

#include <imgui_internal.h>   // PushMultiItemsWidths — the same splitter DragFloatN uses

#include <cfloat>
#include <cstdarg>
#include <cstring>

// ─── Labelled rows + wrapped hints ───────────────────────────────────────────
// Split out of EditorWidgets.cpp on purpose: everything here depends on ImGui
// and nothing else — no AppContext, no ContentManager, no renderer. That is
// what lets the test target compile it against ImGui's core alone and assert
// the layout invariant these exist for (a control never reaches past the
// panel's right edge, a hint wraps instead of being clipped), which is
// otherwise invisible to a headless build.

namespace EditorWidgets
{

// ── Confirm / cancel buttons (see the header for the vocabulary) ─────────────
namespace
{
bool filledButton(const char* label, const ImVec2& size,
                  ImVec4 base, ImVec4 hovered, ImVec4 active)
{
	ImGui::PushStyleColor(ImGuiCol_Button,        base);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,  active);
	ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
	const bool pressed = ImGui::Button(label, size);
	ImGui::PopStyleColor(4);
	return pressed;
}
} // namespace

bool primaryButton(const char* label, const ImVec2& size)
{
	// The toolbar's armed-blue: "this is the thing that happens" is the same
	// statement whether it is a tool or a dialog's confirm.
	return filledButton(label, size,
	                    ImVec4(0.22f, 0.42f, 0.70f, 1.0f),
	                    ImVec4(0.28f, 0.51f, 0.80f, 1.0f),
	                    ImVec4(0.18f, 0.35f, 0.60f, 1.0f));
}

bool dangerButton(const char* label, const ImVec2& size)
{
	return filledButton(label, size,
	                    ImVec4(0.60f, 0.20f, 0.20f, 1.0f),
	                    ImVec4(0.72f, 0.26f, 0.26f, 1.0f),
	                    ImVec4(0.52f, 0.16f, 0.16f, 1.0f));
}

bool addButton(const char* id, const char* tooltip)
{
	// "+##id": the visible label is always just the glyph, the identity is the
	// caller's. Frame-height square, so it lines up with any input row.
	char label[64];
	std::snprintf(label, sizeof(label), "+%s", id);
	const float sz = ImGui::GetFrameHeight();
	const bool pressed = ImGui::Button(label, ImVec2(sz, sz));
	if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
	return pressed;
}

bool dangerSmallButton(const char* label)
{
	ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.95f, 0.48f, 0.45f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.60f, 0.20f, 0.20f, 0.20f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.66f, 0.23f, 0.23f, 0.75f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.52f, 0.16f, 0.16f, 1.0f));
	const bool pressed = ImGui::SmallButton(label);
	ImGui::PopStyleColor(4);
	return pressed;
}

bool dangerMenuItem(const char* label, bool enabled)
{
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.48f, 0.45f, 1.0f));
	const bool pressed = ImGui::MenuItem(label, nullptr, false, enabled);
	ImGui::PopStyleColor();
	return pressed;
}

bool cancelButton(const char* label, const ImVec2& size)
{
	// A ghost: border and text, no fill until hovered. The way out should be
	// findable, not competitive.
	ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.14f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
	const bool pressed = ImGui::Button(label, size);
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(3);
	return pressed;
}

// ── Labelled setting rows (see the header for why these exist) ───────────────
namespace Row
{
namespace {

// Every row is the same three steps: scope the ids under the FULL label (so the
// control itself can be spelled "##v" everywhere and "Speed##an" still differs
// from "Speed##ab"), print only the part before "##", stretch the control to the
// panel. `body` submits the control and is the LAST item, which is what the
// callers' undo tracking keys off.
template <typename F>
bool row(const char* label, F&& body)
{
	ImGui::PushID(label);
	if (const char* hash = std::strstr(label, "##"))
		ImGui::TextUnformatted(label, hash);
	else
		ImGui::TextUnformatted(label);
	ImGui::SetNextItemWidth(-FLT_MIN);
	const bool changed = body();
	ImGui::PopID();
	return changed;
}

// A multi-component drag whose components carry their axis colour: a red X, a
// green Y, a blue Z as a strip inside each field's left edge. DragFloat3 shows
// three identical grey boxes, and which of them is Z is something the user has
// to count — the whole reason every other DCC colours its axes. The strip sits
// INSIDE the frame rather than being a separate label so the row costs no extra
// width, which the Details panel does not have.
bool axisDragN(const char* id, float* v, int n, float speed, float min, float max,
               const char* fmt)
{
	// X red, Y green, Z blue — the gizmo's colours, because these fields and
	// that gizmo edit the same three numbers. W stays neutral.
	static const ImU32 kAxis[4] = {
		IM_COL32(214,  88,  88, 255),
		IM_COL32(118, 190,  96, 255),
		IM_COL32( 92, 140, 228, 255),
		IM_COL32(160, 160, 170, 255),
	};

	bool changed = false;
	ImGui::BeginGroup();
	ImGui::PushID(id);
	ImGui::PushMultiItemsWidths(n, ImGui::CalcItemWidth());
	for (int i = 0; i < n; ++i)
	{
		if (i > 0) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
		ImGui::PushID(i);
		changed |= ImGui::DragFloat("##c", &v[i], speed, min, max, fmt);
		// Painted after the item so it sits on top of the frame background —
		// clipped to the frame's rounding so the corner stays clean.
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 mn = ImGui::GetItemRectMin();
		const ImVec2 mx = ImGui::GetItemRectMax();
		dl->AddRectFilled(mn, ImVec2(mn.x + 3.0f, mx.y), kAxis[i < 4 ? i : 3],
		                  ImGui::GetStyle().FrameRounding, ImDrawFlags_RoundCornersLeft);
		ImGui::PopID();
		ImGui::PopItemWidth();
	}
	ImGui::PopID();
	ImGui::EndGroup();
	return changed;
}

} // namespace

bool sliderFloat(const char* label, float* v, float min, float max,
                 const char* fmt, ImGuiSliderFlags flags)
{
	return row(label, [&]{ return ImGui::SliderFloat("##v", v, min, max, fmt, flags); });
}

bool sliderInt(const char* label, int* v, int min, int max, const char* fmt)
{
	return row(label, [&]{ return ImGui::SliderInt("##v", v, min, max, fmt); });
}

bool dragFloat(const char* label, float* v, float speed, float min, float max, const char* fmt)
{
	return row(label, [&]{ return ImGui::DragFloat("##v", v, speed, min, max, fmt); });
}

bool dragFloat2(const char* label, float* v, float speed, float min, float max, const char* fmt)
{
	return row(label, [&]{ return axisDragN("##v", v, 2, speed, min, max, fmt); });
}

bool dragFloat3(const char* label, float* v, float speed, float min, float max, const char* fmt)
{
	return row(label, [&]{ return axisDragN("##v", v, 3, speed, min, max, fmt); });
}

bool dragFloat4(const char* label, float* v, float speed, float min, float max, const char* fmt)
{
	return row(label, [&]{ return axisDragN("##v", v, 4, speed, min, max, fmt); });
}

bool dragInt(const char* label, int* v, float speed, int min, int max)
{
	return row(label, [&]{ return ImGui::DragInt("##v", v, speed, min, max); });
}

bool inputInt(const char* label, int* v)
{
	return row(label, [&]{ return ImGui::InputInt("##v", v, 0, 0); });
}

bool combo(const char* label, int* v, const char* const items[], int count)
{
	return row(label, [&]{ return ImGui::Combo("##v", v, items, count); });
}

bool comboZ(const char* label, int* v, const char* itemsSeparatedByZeros)
{
	return row(label, [&]{ return ImGui::Combo("##v", v, itemsSeparatedByZeros); });
}

bool colorEdit3(const char* label, float* rgb, ImGuiColorEditFlags flags)
{
	return row(label, [&]{ return ImGui::ColorEdit3("##v", rgb, flags); });
}

bool colorEdit4(const char* label, float* rgba, ImGuiColorEditFlags flags)
{
	return row(label, [&]{ return ImGui::ColorEdit4("##v", rgba, flags); });
}

bool inputText(const char* label, char* buf, size_t bufSize, ImGuiInputTextFlags flags)
{
	return row(label, [&]{ return ImGui::InputText("##v", buf, bufSize, flags); });
}

void labelText(const char* label, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	row(label, [&]{ ImGui::TextV(fmt, args); return false; });
	va_end(args);
}

} // namespace Row

void hint(const char* fmt, ...)
{
	// Wrapped at the panel's right edge, not at ImGui's default (which is the
	// window width minus a fixed inset and ignores the current indent, so an
	// indented hint still ran off the side).
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
	va_list args;
	va_start(args, fmt);
	ImGui::TextV(fmt, args);
	va_end(args);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
}

void subHeading(const char* text)
{
	ImGui::Spacing();
	ImGui::SeparatorText(text);
}

} // namespace EditorWidgets
