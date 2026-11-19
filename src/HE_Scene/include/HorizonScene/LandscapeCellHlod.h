#pragma once
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace HE
{

class CellStreamer;

// ── HLOD proxies for entity cells (Thema 326, step 6) ────────────────────────
// Once a region's entity cells unload, their props and buildings are gone from the
// scene; the coarse quadtree level still draws the ground under where they stood.
// An HLOD proxy is a single merged mesh, baked once (in the editor or at export)
// from a cell's STATIC content, that stands in for the real entities while the
// cell itself is not built. It is not terrain LOD (the quadtree already covers the
// ground) and it is not an impostor (billboarded trees are a separate, not yet
// built, part of this plan): just the buildings and props of one cell, flattened
// into geometry cheap enough to keep around for every cell that might need it.
//
// The draw unit stays the CELL, not the region: standing inside a loaded region
// you are usually near SOME of its cells and far from others (the region is ~8 km,
// a cell a few hundred metres), so a whole-region proxy would either double-draw
// next to a loaded cell or leave a hole where a far cell of the same region has
// none. A region only decides the OUTER bound — which cells could possibly need a
// proxy at all, because their region is resident (CellStreamer::isRegionResident)
// — and the table itself is keyed per cell, with a region named only so a pak
// writer can group the cells of one region into one chunk (not done here: see
// LandscapeHlodTable::encode/decode, which store every cell's key with its mesh
// and do not need a region grouping to round-trip correctly).
//
// What is deliberately NOT here: the bake source (resolving a cell's
// StaticMeshComponent assets into triangles — the export pipeline's job, not
// built), tree impostors (foliage is procedural per tile, not cell content, and
// needs its own billboard atlas), geometry simplification (bakeCellHlod merges
// triangles; it does not decimate them), and the drawing itself (a future
// LandscapeHlodProxies would mirror LandscapeWorldProxies.h: one hidden entity per
// visible cell, a MeshComponent built from the baked mesh, synced by the same
// hide-on-`loaded` / show-on-`unloading` CellStreamer hooks the selector below
// assumes).

// One cell's merged proxy geometry. Positions are in the SAME space the cell's own
// entities would be built in (absolute minus the world's floating origin, like a
// cell root — see CellStreamer.h's "Space" note); a renderer places the proxy
// entity at that cell's root position and uploads this as-is, no further
// transform. Empty (no vertices or no indices): nothing to draw, equivalent to no
// entry in the table at all.
struct LandscapeHlodVertex
{
	glm::vec3 position{ 0.0f };
	glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
	glm::vec2 uv{ 0.0f };
};

struct LandscapeHlodMesh
{
	std::vector<LandscapeHlodVertex> vertices;
	std::vector<uint32_t>            indices;
	bool   empty() const { return vertices.empty() || indices.empty(); }
	size_t bytes() const { return vertices.size() * sizeof(LandscapeHlodVertex) + indices.size() * sizeof(uint32_t); }
};

// One source mesh's triangles, already in the cell's local space, as the baker
// takes them (the export pipeline resolves a StaticMeshComponent's asset and its
// entity transform down to this before calling bakeCellHlod — not built here).
// `normals`/`uvs` empty: every vertex gets the flat per-triangle normal and (0, 0).
struct LandscapeHlodSourceMesh
{
	std::vector<glm::vec3> positions;
	std::vector<glm::vec3> normals;
	std::vector<glm::vec2> uvs;
	std::vector<uint32_t>  indices;
};

// Merges every source's triangles into one mesh: concatenation only (no shared-
// vertex welding, no decimation — see the file comment above). A source whose
// index count is not a multiple of 3, or that indexes past its own position
// count, is skipped (logged by the caller, not here: this is pure data shaping).
LandscapeHlodMesh bakeCellHlod(const std::vector<LandscapeHlodSourceMesh>& sources);

// A cell's key inside the table — the same (x, z) CellManifest's cells use.
struct LandscapeHlodCellKey
{
	int x = 0, z = 0;
	bool operator==(const LandscapeHlodCellKey& o) const { return x == o.x && z == o.z; }
};
struct LandscapeHlodCellKeyHash
{
	size_t operator()(const LandscapeHlodCellKey& k) const
	{
		return static_cast<size_t>(static_cast<uint32_t>(k.x)) * 73856093u ^ static_cast<size_t>(static_cast<uint32_t>(k.z)) * 19349663u;
	}
};

// Every cell's baked proxy, by (x, z). What the export pipeline fills in and the
// pak stores (not built here — see the file comment); what a runtime loads and
// hands to hlodCellsToDraw/a future LandscapeHlodProxies.
class LandscapeHlodTable
{
public:
	void                     set(int cellX, int cellZ, LandscapeHlodMesh mesh);
	const LandscapeHlodMesh* find(int cellX, int cellZ) const;
	bool                     has(int cellX, int cellZ) const { return find(cellX, cellZ) != nullptr; }
	size_t                   cellCount() const { return m_cells.size(); }
	std::vector<LandscapeHlodCellKey> cells() const;
	size_t                   bytes() const;
	void                     clear() { m_cells.clear(); }

	// Little-endian binary: u32 magic 'HLDT', u32 version (1), u32 cellCount, then
	// per cell i32 x, i32 z, u32 vertexCount, u32 indexCount, the vertices, the
	// indices, back to back. For a pak chunk or a loose file; version is for a
	// later format change, not read today beyond "must be 1".
	std::vector<uint8_t> encode() const;
	static bool          decode(const std::vector<uint8_t>& bytes, LandscapeHlodTable& out);

private:
	std::unordered_map<LandscapeHlodCellKey, LandscapeHlodMesh, LandscapeHlodCellKeyHash> m_cells;
};

// The CPU-only selector: which of the table's cells should show their HLOD proxy
// right now, given what `streamer` currently has loaded. A cell whose own content
// is fully built (CellStreamer::isLoaded) is never selected — the real entities
// draw instead. Everything else the table has baked data for IS selected,
// including a cell that is still under construction (CellStreamer::isBuilding):
// the intended wiring hides a cell's proxy entity on CellStreamer::Hooks::loaded
// and shows it again on `unloading`, never on `loadedSlice`, so a cell a few
// slices into its build keeps its proxy for exactly as many frames as it needs
// one. Region residency is not consulted here on purpose — by the time a region
// is not resident its cells are not loaded either (CellStreamer's region gate,
// above), so "not loaded" already covers it, and a selector that also asked the
// region would just be answering the same question twice.
std::vector<LandscapeHlodCellKey> hlodCellsToDraw(const LandscapeHlodTable& table, const CellStreamer& streamer);

} // namespace HE
