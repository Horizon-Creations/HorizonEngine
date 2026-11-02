#pragma once
#include <Types/Defines.h>
#include <Types/UUID.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  "What does the game reach?" — the filter behind PackSettings::onlyReferenced
//
//  An export used to pack every .hasset under the project's Content and the engine's
//  library, whether or not anything ever used it. This answers the question from the
//  other side: starting at the ROOTS (the scenes the game loads, the project manifest,
//  the code that runs, the engine's own implicit needs) it follows references to the
//  assets they point at, then those assets' references, and so on. What is not
//  reached is not packed.
//
//  Reachability is TRANSITIVE FROM A ROOT, not "has any referrer": a mesh that names a
//  material is not a reason to keep either when nothing reachable names the mesh.
//
//  How a reference is found (the same two spellings AssetRefScan.h documents, because
//  the engine stores both and a scan that knows one is worse than none):
//    • BY PATH — the text "Folder/Name.hasset" anywhere in a blob (a chunk string, a
//      node-graph JSON, a widget tree, a script, a scene's CBOR text string). The match
//      is by SUFFIX, so a "Content/" prefix, the engine's "Engine/" form or a stray
//      length byte in front of the string do not matter.
//    • BY UUID — an asset id: 16 raw bytes little-endian (MTLU, MRFU, ... chunks), 16
//      raw bytes big-endian or a CBOR array [hi, lo] of shortest-form ints (a CBOR
//      scene/prefab), or the decimal pair in JSON ([hi, lo] or {"hi":…,"lo":…}).
//  The scan deliberately errs toward KEEPING: a false positive ships a few bytes too
//  many, a false negative ships a game with a grey surface. Texture, audio and font
//  assets are never read (they point at nothing), and a mesh's vertex data is skipped.
//
//  What it cannot see: an asset whose path a script BUILDS at run time ("Level" + n).
//  Those are named in the profile's always-include patterns (PackSettings::
//  alwaysIncludePatterns), the same glob shape as the exclude patterns.
// ─────────────────────────────────────────────────────────────────────────────

namespace HE::reach
{

struct Node
{
    HE::UUID    id{};             // null for a node addressed by path only (a scene)
    std::string path;             // addressed path: "Meshes/Rock.hasset", "Engine/Meshes/Cube.hasset"
    std::string metaPath;         // the asset's own META path when it differs, else empty
    uint16_t    type = 0;         // HE::AssetType value, 0 = unknown (scan like a generic blob)
    const uint8_t* data = nullptr; // the bytes scanned for OUTGOING references
    size_t      size = 0;
};

struct Roots
{
    std::vector<std::string> paths;         // exact addressed paths
    std::vector<std::string> pathPrefixes;  // a folder ("Engine/Audio/Weather/") or any prefix
    std::vector<HE::UUID>    uuids;
    std::vector<std::string> globs;         // Hpak::globMatch against the addressed path
    // Whole kinds of asset (HE::AssetType values) that are roots. A few kinds are found by
    // the runtime BY TYPE, with nothing pointing at them: PlayerHost spawns every
    // PlayerController class and binds every input action and mapping context it
    // discovers, and TypeRegistry::refreshFromContent registers every struct and enum.
    std::vector<uint16_t>    types;
    // Blobs that are roots themselves (the startup scene, the manifest, scripts): scanned
    // for references like a node's data, but they are not nodes and are never "dropped".
    std::vector<std::pair<const uint8_t*, size_t>> blobs;
};

struct Result
{
    std::vector<uint8_t> reachable;   // one flag per node, same order as the input
    size_t nodesReachable = 0;
    size_t bytesScanned   = 0;        // for the export log
};

// Marks every node reachable from `roots`. Pure CPU work, no engine state, thread-safe.
HE_API Result computeReachable(const std::vector<Node>& nodes, const Roots& roots);

// What the engine pulls in on its own, without any asset naming it: the kinds the runtime
// discovers by type (see Roots::types), the weather sounds
// (WeatherAudio, DefaultAssets.h 0x500..0x504), the weather and procedural-surface material
// functions, the layer-mesh presets the landscape scatters (LayerMeshPresets.h builds
// "Engine/Meshes/<name>[LodN].hasset" from a material's group), the water surface's material
// (WaterSurface.h) and the trail clipmap's texture (TrailClipmap.h 0x41F). Kept as a short list
// of folders on purpose: the engine library's small assets cost little, and a runtime system
// added later that loads by a path it builds would otherwise ship a game without it.
HE_API Roots engineImplicitRoots();

// The text the game's code and manifest hold that can name an asset, read into `out`: loose
// scripts and HorizonCode under `contentDir`, the project manifest and GameInstance at its
// parent (the project root), and the root's Config folder. They ship with the game whatever
// they say, so what they name is kept. Export/, Saved/ and the like are NOT walked (an exported
// app bundle is hundreds of MB).
HE_API void collectProjectTextRoots(const std::filesystem::path& contentDir,
                                    std::vector<std::vector<uint8_t>>& out);

} // namespace HE::reach
