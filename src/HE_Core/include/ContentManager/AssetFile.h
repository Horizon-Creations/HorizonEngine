#pragma once
#include "Types/Defines.h"
#include <string>

struct RuntimeAsset;

namespace HE
{
// Writes `asset` to <contentRoot>/<asset.path> — exactly the file and the bytes a
// freshly constructed ContentManager(contentRoot).saveAsset(asset) writes, without
// constructing one. For offline writers (the importers, asset_compiler) that only
// ever need the save: the ContentManager constructor builds every default asset
// first (meshes, the grid texture, five editor icons rasterised and one material
// graph generated per icon), and an import writing a mesh with N materials and M
// textures used to pay that N+M+1 times (TLS-101).
//
// Same rules as saveAsset: refused while an export reads the project
// (ProjectWriteGate), an asset without a UUID gets one minted, missing parent
// folders are created, the same log line. What it leaves out is per-manager state
// a throwaway manager never had anyone to tell: the content epoch, the hot-reload
// mtime stamp and the onAssetSaved callback. No "Engine/" redirection either — a
// fresh manager has no engine content root, so saveAsset would not do one.
HE_API bool saveAssetFile(RuntimeAsset& asset, const std::string& contentRoot);
} // namespace HE
