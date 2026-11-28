#pragma once
#include <string>
#include <vector>

// The picture the Stamp Heightmap window paints (StampHeightmapPanel.h): heights in 0..1, row 0 at the top, and the few strokes
// that make one. No ImGui in here, so the strokes are tested without a window. Places and radii are in pixels.
namespace HE::StampHeightmap
{
struct Map
{
	int w = 0, h = 0;
	std::vector<float> v;   // w * h, 0..1
};

void fill(Map& m, int w, int h, float height);
// A soft dab: adds `amount` at the centre (negative lowers), fading to nothing at `radius`.
void dab(Map& m, float x, float y, float radius, float amount);
// A soft dab toward the mean of each pixel's neighbours.
void smooth(Map& m, float x, float y, float radius, float strength);
// A line of `radius` from A to B whose height runs from h0 to h1: the ground is pulled to it, fully on the line and fading to the
// sides. Higher than the ground it is a ridge or a wall, lower a road or a valley; A == B is a dot.
void line(Map& m, float x0, float y0, float x1, float y1, float radius, float h0, float h1, float strength);
// A transition over the whole picture: h0 before A, h1 beyond B, eased between them along A -> B.
void gradient(Map& m, float x0, float y0, float x1, float y1, float h0, float h1, float strength);

// 16-bit PGM, the form a Landscape Stamp's heightmap file is read in at full precision (TerrainHeightmap::readGrey16).
bool writePgm16(const Map& m, const std::string& path, std::string& error);
bool read(Map& m, const std::string& path, std::string& error);
}
