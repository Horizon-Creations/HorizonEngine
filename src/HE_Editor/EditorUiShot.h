#pragma once

#include <string>

// ── The running editor's own UI, as pixels ───────────────────────────────────
// HE_DUMP_LIVE and the MCP screenshot tools picture the SCENE (the viewport's
// offscreen target); neither has the ImGui UI on top. This is the UI half:
// this frame's draw data, rasterised on the CPU by the test harness's
// rasteriser (tests/ImGuiSoftwareRaster), so it works in a hidden editor.
namespace HE::Ed::UiShot
{
	// Writes this frame's ImGui output as a BMP. Call after ImGui::Render().
	// Textures that are not ImGui's own (the Scene viewport's GPU image) come
	// out flat grey. False without draw data or when the file cannot be written.
	bool captureDrawData(const std::string& path);

	// Live witness for an outside driver (scripts/he_team_presence_shots.py):
	//
	//   HE_DUMP_UI=<dir>  HE_DUMP_UI_TRIGGER=<file>
	//
	// When the trigger file appears, its one line "<name> [frames] [x y]" is
	// read: the next `frames` frames (default 1) go to <dir>/<name>-000.bmp,
	// -001 ..., each logged with its frame number and ImGui time; with x y the
	// mouse is put there for the burst (a hover in a window nobody can point
	// at). The trigger is removed after the last write, so "trigger gone" means
	// "burst complete" to the driver. Call right after ImGui::Render(); one
	// stat() per frame, and only when both variables are set.
	void liveAfterRender();
}
