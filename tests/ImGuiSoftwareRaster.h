#pragma once
#include <imgui.h>

#include <cstdint>
#include <string>
#include <vector>

// ── Looking at editor UI without a GPU, a window or a person ─────────────────
// ImGui needs no backend to RUN — a context and a display size are enough, which
// is what the existing headless tests use. What it has never had here is a way
// to SEE the result: the tests can assert that an item lands inside the panel,
// but not that the panel reads as anything.
//
// This is that missing half. ImGui's output is already a plain triangle list
// with a texture and a colour per vertex, so rendering it on the CPU is a small
// rasteriser rather than a graphics port — and once there are pixels, a docked
// panel, a tooltip or a documentation page can be dumped to a file and looked at
// on any machine, in CI, from a script.
//
// What it is NOT: a match for the GPU backends down to the pixel. Blending is
// straight source-over in 8-bit sRGB, there is no gamma correction and no
// multisampling. ImGui's own anti-aliasing comes through untouched (it is baked
// into the vertex alpha), so text and rounded corners look right; what this
// cannot answer is whether Metal and OpenGL agree on a blend.

namespace he_ui
{

// One rendered frame, straight RGBA8, row-major from the top.
struct Image
{
	int                       width  = 0;
	int                       height = 0;
	std::vector<std::uint8_t> rgba;   // width * height * 4

	bool valid() const { return width > 0 && height > 0 &&
	                            rgba.size() == static_cast<std::size_t>(width) * height * 4; }
	// Straight pixel read, for the assertions that ask about colour rather than
	// about layout ("is anything drawn here at all", "did the accent land").
	void pixel(int x, int y, std::uint8_t& r, std::uint8_t& g,
	           std::uint8_t& b, std::uint8_t& a) const;
	// How many pixels differ from the background — the cheapest possible "did
	// this draw anything" check, and the one that catches an empty panel.
	int inkedPixels(std::uint8_t bgR, std::uint8_t bgG, std::uint8_t bgB) const;
};

// Rasterise one frame of ImGui output. Call after ImGui::Render(), with
// ImGui::GetDrawData(). Textures are taken from the draw data itself (ImGui
// 1.92 owns its atlas and hands it over as ImTextureData), so nothing needs to
// have been uploaded anywhere first.
// The default clear is the editor's warm near-black, packed the way IM_COL32
// packs — red in the LOW byte. Spelling it as a hex literal in the obvious
// left-to-right order is how it ends up blue.
Image rasterize(const ImDrawData* drawData, int width, int height,
                std::uint32_t clearColor = 0xFF0F1214U);   // IM_COL32(20, 18, 15, 255)

// Write an image as a 32-bit BMP. Uncompressed and dependency-free on purpose:
// the engine ships no PNG encoder, and the existing headless dumps (the splash
// screen, the frame dump he_shot.py drives) already write BMP and convert
// outside. scripts/he_uishot.py does the same for these.
bool writeBmp(const Image& img, const std::string& path);

} // namespace he_ui
