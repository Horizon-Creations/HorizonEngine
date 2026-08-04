#include "EditorWidgets.h"

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
	return row(label, [&]{ return ImGui::DragFloat2("##v", v, speed, min, max, fmt); });
}

bool dragFloat3(const char* label, float* v, float speed, float min, float max, const char* fmt)
{
	return row(label, [&]{ return ImGui::DragFloat3("##v", v, speed, min, max, fmt); });
}

bool dragFloat4(const char* label, float* v, float speed, float min, float max, const char* fmt)
{
	return row(label, [&]{ return ImGui::DragFloat4("##v", v, speed, min, max, fmt); });
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
