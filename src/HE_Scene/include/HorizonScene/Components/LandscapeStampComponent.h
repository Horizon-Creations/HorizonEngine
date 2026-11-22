#pragma once
#include <Types/UUID.h>
#include <string>

// A heightmap stamped onto the tile landscape (Thema 326): a texture whose red channel is added to the ground under the entity.
// The entity's transform is the stamp's place: position (X, Z), turn around Y, and scale (X and Z stretch the footprint, Y the
// height). Moving the entity moves the stamp; nothing is written into the landscape until an export bakes it (LandscapeStamps.h).
struct LandscapeStampComponent
{
	bool     enabled = true;
	HE::UUID textureId;           // the heightmap as a texture asset: its red channel, 8 bits (256 height steps)
	// The heightmap as a file (absolute, or relative to the project's folder): PNG / PGM at 8 or 16 bits, .r16 / .raw, read at
	// 16 bits (TerrainHeightmap::readGrey16). Wins over the texture when both are set. Only the editor and a project run without
	// an export read it: an export bakes the stamps into the pack, the file does not ship.
	std::string heightmapFile;
	// Which channel of the picture is the height: 0 red, 1 green, 2 blue, 3 alpha, 4 the grey value (luminance). A packed
	// texture can so carry several stamps. A grey picture answers with its grey for red, green and blue.
	int      channel = 0;
	float    sizeM   = 256.0f;    // edge of the footprint at scale 1
	float    heightM = 50.0f;     // what white adds at scale 1; negative digs
	float    falloff = 0.2f;      // 0..1: how far in from the edge the stamp fades out
};
