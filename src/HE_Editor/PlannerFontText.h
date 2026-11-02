#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ── Text drawn from a font file, into pixels ─────────────────────────────────
// The whiteboard's PNG export draws its text here instead of through ImGui: in the editor ImGui's glyph atlas
// is a GPU texture, and a picture made on the CPU has no way to read it. This lays a string out (word wrap,
// newlines) and blends its glyphs, from the TTF/OTF data the UI font was loaded from, into an RGBA8 image.
// No ImGui in here: stb_truetype's rect-pack types clash with imgui_internal.h in one translation unit.
namespace PlannerFontText
{

struct Run
{
	float x = 0, y = 0;       // the top left of the first line, in pixels
	float size = 14;          // the font size in pixels (as ImGui's AddText takes it)
	std::uint32_t colour = 0xFFFFFFFFu;   // packed like IM_COL32: red in the low byte
	std::string text;         // UTF-8
	float wrapWidth = 0;      // 0 = no wrapping
	float clipX0 = 0, clipY0 = 0, clipX1 = 1e9f, clipY1 = 1e9f;
};

// Blends every run into `rgba` (width * height * 4). False when the font data cannot be read.
bool draw(const unsigned char* fontData, int fontNumber, std::uint8_t* rgba, int width, int height, const std::vector<Run>& runs);

} // namespace PlannerFontText
