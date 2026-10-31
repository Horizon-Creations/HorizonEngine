#pragma once
#include "EditorTheme.h"
#include <imgui.h>
#include <cstdio>

// ─── "Compiling shaders  3 / 8" over the Scene window ────────────────────────
// A backend that builds material pipelines off the render thread does not draw an
// object whose pipeline is still compiling (IRenderer::PumpShaderBuilds): the object
// appears when its build lands. This card is the reason the viewport is incomplete in
// the meantime. Bottom left of the viewport rect, inside the calling window; gone as
// soon as `pending` reaches 0. Header-only so the UI shot test draws the same pixels.
namespace HE::Ed
{
	inline void drawShaderBuildHint(int pending, int total, const ImVec2& rectMin, const ImVec2& rectMax)
	{
		if (pending <= 0) return;
		char buf[64];
		std::snprintf(buf, sizeof(buf), "Compiling shaders  %d / %d", total - pending, total);
		const ImVec2 ts = ImGui::CalcTextSize(buf);
		const ImVec2 p0(rectMin.x + 10.0f, rectMax.y - ts.y - 22.0f);
		if (p0.y < rectMin.y) return;   // too small a pane to hold it
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(p0, ImVec2(p0.x + ts.x + 20.0f, p0.y + ts.y + 10.0f), IM_COL32(12, 11, 10, 190), 6.0f);
		dl->AddText(ImVec2(p0.x + 10.0f, p0.y + 5.0f), ImGui::GetColorU32(HE::Ed::Theme::AccentBright), buf);
	}
}
