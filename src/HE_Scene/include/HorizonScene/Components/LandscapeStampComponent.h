#pragma once
#include <Types/UUID.h>

// A heightmap stamped onto the tile landscape (Thema 326): a texture whose red channel is added to the ground under the entity.
// The entity's transform is the stamp's place: position (X, Z), turn around Y, and scale (X and Z stretch the footprint, Y the
// height). Moving the entity moves the stamp; nothing is written into the landscape until an export bakes it (LandscapeStamps.h).
struct LandscapeStampComponent
{
	bool     enabled = true;
	HE::UUID textureId;           // the heightmap; none = the stamp does nothing
	float    sizeM   = 256.0f;    // edge of the footprint at scale 1
	float    heightM = 50.0f;     // what white adds at scale 1; negative digs
	float    falloff = 0.2f;      // 0..1: how far in from the edge the stamp fades out
};
