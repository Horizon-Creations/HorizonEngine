#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The picture the Stamp Heightmap editor works on (StampHeightmapPanel.h): heights in 0..1, row 0 at the top, and everything that
// makes one: brush strokes, generated landforms, the pass that turns a rough sketch into ground that looks grown, erosion and the
// usual filters. No ImGui in here, so all of it is tested without a window. Places and radii are in pixels.
//
// A `mask` (a Map of the same size, 0..1; null = everywhere) limits a generator or a filter to a region: the result is blended in
// by it, so a feathered mask gives a seamless patch. That is how one region of a picture is made again without touching the rest.
namespace HE::StampHeightmap
{
struct Map
{
	int w = 0, h = 0;
	std::vector<float> v;   // w * h, 0..1
};

void fill(Map& m, int w, int h, float height);

// ── Strokes ──
// A soft dab: adds `amount` at the centre (negative lowers), fading to nothing at `radius`.
void dab(Map& m, float x, float y, float radius, float amount);
// A soft dab toward the mean of each pixel's neighbours.
void smooth(Map& m, float x, float y, float radius, float strength);
// A soft dab toward one height.
void flatten(Map& m, float x, float y, float radius, float height, float strength);
// A line of `radius` from A to B whose height runs from h0 to h1: the ground is pulled to it, fully on the line and fading to the
// sides. Higher than the ground it is a ridge or a wall, lower a road or a valley; A == B is a dot.
void line(Map& m, float x0, float y0, float x1, float y1, float radius, float h0, float h1, float strength);
// A transition over the whole picture: h0 before A, h1 beyond B, eased between them along A -> B.
void gradient(Map& m, float x0, float y0, float x1, float y1, float h0, float h1, float strength);

// ── Generated landforms ──
enum class Shape { Hills, Mountain, MountainRange, Plateau, Crater, Volcano, Dunes, Island, Count };
enum class Blend { Replace, Add, Subtract, Max, Min, Count };
struct GenParams
{
	Shape    shape = Shape::Mountain;
	Blend    blend = Blend::Replace;
	uint32_t seed = 1;          // the same seed gives the same landform; another one a sibling of it
	float    scale = 0.35f;     // size of the largest features, as a share of the picture
	int      detail = 6;        // octaves: how far down the features repeat at half the size
	float    roughness = 0.5f;  // how much each finer octave keeps (0.3 soft, 0.7 craggy)
	float    warp = 0.3f;       // bends the form, so nothing in it is straight or round
	float    height = 1.0f;     // the peak
};
void generate(Map& m, const GenParams& p, const Map* mask = nullptr);

// ── From a sketch to ground ──
// Takes what is in the picture as a rough statement of where the ground is high: blurs the brush marks away, bends the outlines,
// lays ridged detail over it that grows with the height, and lets water and gravity work on the result.
struct RefineParams
{
	uint32_t seed = 1;
	float smoothing = 0.04f;   // blur radius as a share of the picture: how much of the hand is taken out
	float detail = 0.35f;      // how much relief is added
	float warp = 0.25f;        // how far outlines are bent
	float erosion = 0.5f;      // 0 none .. 1 heavy
};
void refine(Map& m, const RefineParams& p, const Map* mask = nullptr);

// ── Filters ──
// Water: `amount` 0..1 sets how many drops run downhill, carving channels and filling hollows. Gravity: `thermalPasses` let
// slopes steeper than the angle of repose slide. Deterministic for a seed.
void erode(Map& m, float amount, int thermalPasses, uint32_t seed, const Map* mask = nullptr);
void blur(Map& m, float radius, const Map* mask = nullptr);
// Steps: `steps` levels, `sharpness` 0 (untouched) .. 1 (stairs).
void terrace(Map& m, int steps, float sharpness, const Map* mask = nullptr);
// Stretches the heights so the lowest is 0 and the highest 1.
void normalize(Map& m);
void invert(Map& m, const Map* mask = nullptr);
// Fades the picture to 0 toward its border over `width` (a share of the half edge), so the stamp has no visible rim.
void fadeEdges(Map& m, float width);

// ── A look ──
// The picture as ground colours (RGBA8, sRGB, w * h): grass low, soil above it, rock where it is steep, snow on top. For the test
// landscape's material, so its relief reads as terrain and not as a white sheet. `sizeM` and `heightM` are the landscape's edge and
// what white is, which make a slope a slope. `coloured` off gives one even grey, to judge the form alone.
void groundColours(const Map& m, float sizeM, float heightM, bool coloured, std::vector<uint8_t>& rgba);

// ── File ──
// 16-bit PGM, the form a Landscape Stamp's heightmap file is read in at full precision (TerrainHeightmap::readGrey16).
bool writePgm16(const Map& m, const std::string& path, std::string& error);
bool read(Map& m, const std::string& path, std::string& error);

// ── Opening the editor from elsewhere ──
// The Inspector and the Landscape mode ask for the editor tab with a stamp's heightmap file ("" = a new picture); the editor's
// frame takes the request and brings the tab forward. Kept here, without ImGui, so the askers do not link the panel.
void requestEdit(const std::string& file);
bool takeEditRequest(std::string& file);
}
