#include "EditorWidgets.h"
#include "EditorHelp.h"       // the tooltip table every labelled row is looked up in
#include "EditorTheme.h"      // the brand accent behind primaryButton

#include <misc/cpp/imgui_stdlib.h>   // the std::string rows below
#include <imgui_internal.h>   // PushMultiItemsWidths (the splitter DragFloatN uses)
                              // + the window's WorkRect, which is what moves a
                              // SeparatorText's rule short of a trailing button

#include <cfloat>
#include <cstdarg>
#include <cstring>
#include <string>

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
	// Styled the same as an unstyled one for the purpose of help: the colour says
	// what KIND of thing this is, the entry says what it does. Queued after the
	// button so the last submitted item is still the caller's.
	helpForLabel(label);
	return pressed;
}
} // namespace

bool primaryButton(const char* label, const ImVec2& size)
{
	// The brand amber: "this is the thing that happens" is the same statement
	// whether it is a tool or a dialog's confirm.
	using namespace HE::Ed::Theme;
	return filledButton(label, size,
	                    Accent,
	                    AccentHi,
	                    mix(warm(0.10f), Accent, 0.80f));
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

bool sectionHeaderAdd(const char* label, const char* id, const char* tooltip)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (window->SkipItems) return false;

	const ImGuiStyle& style = ImGui::GetStyle();
	const float       btn   = ImGui::GetFrameHeight();   // addButton draws a btn×btn square

	// ── Making room in the RULE, not just on the line ────────────────────────
	// Checked in the vendored ImGui (1.92.9 WIP) rather than assumed:
	// SeparatorTextEx builds its bounding box as
	//     ImRect bb(pos, ImVec2(window->WorkRect.Max.x, pos.y + min_size.y))
	// and draws the right-hand rule out to bb.Max.x. So the work rect is the
	// only lever that moves the LINE — SameLine() alone would leave it running
	// straight through the button. Narrowing it for the duration of the call
	// also shortens the label's ellipsis budget (bb.Max.x is what
	// RenderTextEllipsis clips to), so a heading too long for the panel now
	// truncates BEFORE the button instead of underneath it.
	//
	// Truncated to whole pixels first so "line ends exactly one ItemSpacing.x
	// left of the button" holds without a sub-pixel seam between the two.
	// Both clamped to the row's left edge, so a panel dragged narrower than one
	// button still keeps the "+" inside itself instead of pushing it out over
	// whatever is docked to the left.
	const float workMaxX = window->WorkRect.Max.x;
	const float btnX     = ImMax(ImTrunc(workMaxX - btn), window->DC.CursorPos.x);
	window->WorkRect.Max.x = ImMax(btnX - style.ItemSpacing.x, window->DC.CursorPos.x);

	// The row's own metrics, computed the way SeparatorTextEx computes them, so
	// the button can be placed without having to ask ImGui afterwards.
	const ImVec2 labelSize = ImGui::CalcTextSize(label, ImGui::FindRenderedTextEnd(label), false);
	const float  rowH      = ImMax(labelSize.y + style.SeparatorTextPadding.y * 2.0f,
	                               style.SeparatorTextBorderSize);
	const float  rowTopY   = window->DC.CursorPos.y;

	ImGui::SeparatorText(label);
	window->WorkRect.Max.x = workMaxX;

	// ── The button, leaving no trace in the layout ───────────────────────────
	// The two are not the same height: a "+" is GetFrameHeight() tall, the
	// heading row is the text plus 2 × SeparatorTextPadding.y. So the button is
	// CENTRED on the row rather than aligned to it, and the row keeps the height
	// it would have had — otherwise everything the caller draws below shifts.
	//
	// Submitting an item moves the cursor and rewrites the line bookkeeping, so
	// all of it is put back afterwards. Written through window->DC rather than
	// SetCursorScreenPos()/GetCursorScreenPos(): those set DC.IsSetPos, and End()
	// then runs ErrorCheckUsingSetCursorPosToExtendParentBoundaries(), which
	// asserts whenever the restored cursor sits past CursorMaxPos — which it
	// always does, by exactly one ItemSpacing.y. CursorMaxPos itself is
	// deliberately left grown by the button: the window must still size and
	// scroll to include it.
	const ImVec2 cursor       = window->DC.CursorPos;
	const ImVec2 cursorPrev   = window->DC.CursorPosPrevLine;
	const ImVec2 prevLineSize = window->DC.PrevLineSize;
	const ImVec2 currLineSize = window->DC.CurrLineSize;
	const float  prevBaseline = window->DC.PrevLineTextBaseOffset;
	const float  currBaseline = window->DC.CurrLineTextBaseOffset;
	const bool   isSameLine   = window->DC.IsSameLine;

	window->DC.CursorPos = ImVec2(btnX, ImTrunc(rowTopY + (rowH - btn) * 0.5f));
	const bool pressed   = addButton(id, tooltip);

	window->DC.CursorPos              = cursor;
	window->DC.CursorPosPrevLine      = cursorPrev;
	window->DC.PrevLineSize           = prevLineSize;
	window->DC.CurrLineSize           = currLineSize;
	window->DC.PrevLineTextBaseOffset = prevBaseline;
	window->DC.CurrLineTextBaseOffset = currBaseline;
	window->DC.IsSameLine             = isSameLine;
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
	helpForLabel(label);
	return pressed;
}

bool dangerMenuItem(const char* label, bool enabled)
{
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.48f, 0.45f, 1.0f));
	const bool pressed = ImGui::MenuItem(label, nullptr, false, enabled);
	ImGui::PopStyleColor();
	helpForLabel(label);
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
	// This one does not go through filledButton, so it needs its own lookup —
	// the header promised all three of these look their label up, and until now
	// this was the one that did not.
	helpForLabel(label);
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
	// The label counts as part of the control for help: it is the wider target,
	// and it is what the eye is on when the question "what IS this?" arises.
	const bool labelHelp = helpForLabel(label);
	ImGui::SetNextItemWidth(-FLT_MIN);
	const bool changed = body();
	// After the control, so the LAST item is still the caller's — nothing here
	// submits an item, and the tooltip itself is drawn at the end of the frame
	// (see drawQueuedHelp).
	if (!labelHelp) helpForLabel(label);
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

bool inputText(const char* label, std::string* s, ImGuiInputTextFlags flags)
{
	return row(label, [&]{ return ImGui::InputText("##v", s, flags); });
}

bool inputTextMultiline(const char* label, std::string* s, float height)
{
	// The width is what `row` already stretched; only the height is ours. A
	// multi-line box that took its width from the caller would be the one row on
	// the panel that reaches past the edge.
	return row(label, [&]{
		return ImGui::InputTextMultiline("##v", s, ImVec2(-FLT_MIN, height));
	});
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

// ── Help tooltips (see the header for why they are drawn late) ───────────────
namespace
{
// What the mouse is over, decided fresh every frame. A raw pointer into the
// help table is safe to keep: the table is `constexpr` static storage.
const HE::Ed::Help::Entry* s_queued     = nullptr;
const HE::Ed::Help::Entry* s_lastQueued = nullptr;   // for the F1 press

bool queueIfHovered(const HE::Ed::Help::Entry* entry)
{
	if (!entry) return false;
	// ForTooltip is the flag set that carries ImGui's own tooltip manners:
	// a short delay, and Stationary — the tooltip does not fire while the
	// pointer is merely sweeping across the panel on its way somewhere else.
	// That is half of what separates a useful tooltip from a nuisance.
	// AllowWhenDisabled is in there too, which is exactly right here: a greyed
	// control is the one people most want an explanation for.
	if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return false;
	s_queued = entry;
	return true;
}
} // namespace

bool helpForLabel(const char* label)
{
	return queueIfHovered(HE::Ed::Help::find(label ? label : ""));
}

bool helpForKey(const char* key)
{
	return queueIfHovered(HE::Ed::Help::findKey(key ? key : ""));
}

void helpMarker(const char* key)
{
	const HE::Ed::Help::Entry* e = HE::Ed::Help::findKey(key ? key : "");
	if (!e) return;
	ImGui::SameLine(0.0f, 4.0f);
	ImGui::TextDisabled("(?)");
	queueIfHovered(e);
}

bool checkbox(const char* label, bool* v)
{
	const bool changed = ImGui::Checkbox(label, v);
	helpForLabel(label);
	return changed;
}

bool menuItem(const char* label, const char* shortcut, bool selected, bool enabled)
{
	const bool pressed = ImGui::MenuItem(label, shortcut, selected, enabled);
	// AFTER the item, and the lookup is hover-gated, so a disabled entry still
	// explains itself — "why can I not press this" is the question a greyed
	// menu row raises, and the answer is usually in the sentence.
	helpForLabel(label);
	return pressed;
}

bool button(const char* label, const ImVec2& size)
{
	const bool pressed = ImGui::Button(label, size);
	helpForLabel(label);
	return pressed;
}

bool selectable(const char* label, bool selected, ImGuiSelectableFlags flags,
                const ImVec2& size)
{
	const bool pressed = ImGui::Selectable(label, selected, flags, size);
	helpForLabel(label);
	return pressed;
}

bool smallButton(const char* label)
{
	const bool pressed = ImGui::SmallButton(label);
	helpForLabel(label);
	return pressed;
}

const char* drawQueuedHelp()
{
	const HE::Ed::Help::Entry* e = s_queued;
	s_queued = nullptr;
	s_lastQueued = e;
	if (!e) return nullptr;

	// Wrapped at a fixed width rather than the window's: a tooltip window sizes
	// itself to its widest line, so wrapping at its own edge feeds back into the
	// measurement and the box shrinks a little more every frame.
	const float wrapW = 380.0f * ImGui::GetFontSize() / 16.0f;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
	if (ImGui::BeginTooltip())
	{
		if (e->title[0])
		{
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
			ImGui::TextUnformatted(e->title);
			ImGui::PopStyleColor();
		}
		ImGui::PushTextWrapPos(wrapW);
		ImGui::TextUnformatted(e->body);
		ImGui::PopTextWrapPos();

		// Where F1 goes: this control's OWN entry in the generated reference,
		// not the chapter its concept belongs to. The chapter is a link inside
		// that entry — see EditorHelp.h, and the audit that made the difference
		// measurable (320 entries pointed at 47 chapters).
		static std::string s_topic;
		s_topic = HE::Ed::Help::referenceTopic(e->key);

		if (e->shortcut[0] || !s_topic.empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
			if (e->shortcut[0])
			{
				const std::string sc = HE::Ed::Help::shortcutLabel(e->shortcut);
				ImGui::TextUnformatted(sc.c_str());
				if (!s_topic.empty()) ImGui::SameLine(0.0f, 12.0f);
			}
			// Only advertised where there is a page to open. A hint that does
			// nothing on a third of the controls teaches people to ignore it.
			if (!s_topic.empty()) ImGui::TextUnformatted("F1  documentation");
			ImGui::PopStyleColor();
		}
		ImGui::EndTooltip();

		// F1 belongs to whatever is under the mouse right now, which is what the
		// tooltip is showing. Checked here rather than in a global shortcut
		// block for exactly that reason.
		if (!s_topic.empty() && ImGui::IsKeyPressed(ImGuiKey_F1, false))
		{
			ImGui::PopStyleVar();
			return s_topic.c_str();
		}
	}
	ImGui::PopStyleVar();
	return nullptr;
}

} // namespace EditorWidgets
