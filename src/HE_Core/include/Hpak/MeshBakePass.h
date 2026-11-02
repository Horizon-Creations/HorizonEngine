#pragma once
#include <Types/Defines.h>
#include <Types/UUID.h>
#include <cstdint>
#include <string>
#include <vector>

// The export pass that applies HE::MeshBake (MeshTextureBake.h) to a set of loose assets:
// for every static mesh whose bake switches allow it and whose materials are plain PBR
// texture sets, the mesh is rewritten onto ONE merged material with atlas textures, and the
// pass hands back the derived assets. HpakWriter calls it between the reachability filter and
// the packing pass, then filters again so the textures nothing reaches any more stay out.
//
// The pass works on a scratch copy of the files it needs (a private ContentManager over a
// temporary directory) so it reads and writes assets exactly as the engine does, and never
// touches the project.
namespace HE::MeshBake
{

struct PassEntry
{
	HE::UUID                    id;
	std::string                 relPath;     // the addressed path ("Imported/M_Rock.hasset")
	std::string                 metaPath;    // the path inside the asset's own META, when it differs
	uint16_t                    type = 0;    // HE::AssetType
	const std::vector<uint8_t>* bytes = nullptr;
};

struct NewAsset
{
	HE::UUID             id;
	std::string          relPath;
	uint16_t             type = 0;
	std::vector<uint8_t> bytes;
};

struct ReplacedMesh
{
	HE::UUID             id;
	std::vector<uint8_t> bytes;              // the rewritten .hasset
};

struct PassOptions
{
	uint32_t maxAtlas = 4096;
};

struct PassResult
{
	std::vector<NewAsset>     added;         // derived material and textures
	std::vector<ReplacedMesh> replaced;      // meshes rewritten onto the merged material
	std::vector<std::string>  notices;       // one line per mesh baked or declined, for the export log
	uint32_t                  baked = 0;
	uint32_t                  declined = 0;  // switched on and eligible by type, but not bakeable
	// Why the others were not touched, so an export that bakes nothing can say so.
	uint32_t                  meshesSeen = 0;
	uint32_t                  skippedSwitchedOff = 0;   // every bake switch of the mesh is off
	uint32_t                  skippedOneSection = 0;    // one material slot: nothing to merge
	uint32_t                  skippedCooked = 0;        // already in the runtime form (no loose geometry)
};

HE_API PassResult runPass(const std::vector<PassEntry>& entries, const PassOptions& options);

} // namespace HE::MeshBake
