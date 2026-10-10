#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class HorizonWorld;

namespace HE
{

struct CellManifest;

// ── Splitting a scene into streaming cells, and merging it back (Thema 153) ───
// The editor's side of HE::CellStreamer. A split takes the placed things of a
// scene (meshes, point and spot lights, static bodies, decals, placed prefabs,
// and the dressing that names assets only: particle systems, skeletal meshes,
// animators: top-level subtrees whose every entity carries only such
// components) out into one scene file per grid square, and leaves everything
// else in the base: sky, weather, terrain, cameras, scripts, characters,
// dynamic bodies, audio sources, directional lights. Which component goes where
// is one table, kComponentClasses in CellSplit.cpp; a component that is not in it
// stays in the base. Folders (no components, or an identity transform only) are
// looked through; one left empty is dropped. A subtree goes to the square its top
// entity stands in; positions stay absolute.
//
// Every cell file carries a "streaming" head (CellStreamer.h, kCellFormatVersion)
// and the manifest a version and, per cell, how many of its entities can own a
// physics body. The ids in a cell are the scene's own and are kept when the game
// loads it.
//
// The rules of scripts/split_scene_cells.py before Thema 164, which stays for
// batch use and still writes version-1 cells (no head, a smaller table); this is
// the reference. A merge is the way back, so a split scene stays editable as one
// piece.

struct CellSplitOptions
{
	float       cellSize     = 512.0f;   // m
	float       loadRadius   = 0.0f;     // m; 0 = 1.5 × cellSize
	float       unloadRadius = 0.0f;     // m; 0 = 1.25 × loadRadius
	float       lookaheadSec = 2.0f;     // s
	std::string dir;                     // project-relative folder of the cell files
};

struct CellSplitResult
{
	struct Cell
	{
		int            x = 0, z = 0;
		nlohmann::json scene;          // a .hescene of its own: a "Cell x,z" root + the subtrees
		uint32_t       entities = 0;   // without that root
		uint32_t       bodies = 0;     // of them, the ones that can own a physics body
	};
	nlohmann::json    base;            // the scene without them, "cells" manifest included
	std::vector<Cell> cells;           // sorted by x, then z
	size_t            moved = 0;       // entities that went into cells
	std::string       error;           // set when the scene could not be split; nothing else is then
};

// Pure: reads `scene` (a .hescene as JSON), writes nothing.
CellSplitResult splitSceneIntoCells(const nlohmann::json& scene, const CellSplitOptions& options);

// The open world, split: the cell files written through `write` (project-
// relative path, JSON text — false aborts the split), then the world rebuilt
// from the base. Returns the result for the counts; on any error the world is
// untouched and `error` says why. Positions in the world must be absolute
// (origin at 0, as in the editor).
CellSplitResult splitWorldIntoCells(HorizonWorld& world, const CellSplitOptions& options,
                                    const std::function<bool(const std::string& path,
                                                             const std::string& text)>& write);

// The open world's cells, loaded back into it as ordinary entities under the
// world root, and the manifest dropped — the scene is one piece again. `read`
// fetches a cell's file (project-relative path) as bytes, JSON or CBOR; a cell
// it cannot read stops the merge before anything changes. False and `error`
// when the world has no cells or one could not be read.
bool mergeCellsIntoWorld(HorizonWorld& world,
                         const std::function<bool(const std::string& path, std::vector<uint8_t>& out)>& read,
                         std::string* error = nullptr, size_t* mergedEntities = nullptr);

} // namespace HE
