#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <glm/fwd.hpp>
#include "ContentManager/Assets.h"

// cgltf.h lives in HE_Tools/vendor, a PRIVATE include directory of
// HorizonImporters — consumers of this header (editor, tests) never see it, so
// the glTF types used below are forward-declared instead of included.
struct cgltf_data;
struct cgltf_accessor;
struct cgltf_primitive;

// Shared plumbing for all asset importers.
//
// Importers fill a RuntimeAsset-derived struct and hand it to writeAsset().
// `asset.path` must be set to the path relative to the content root
// (forward slashes) — the same string the ContentManager later uses to load
// the asset, and the same string other assets use to reference it
// (e.g. MaterialAsset::texturePaths).
namespace Importer
{
	// Writes <contentRoot>/<asset.path> as a .hasset file, creating parent
	// directories as needed. If the target file already exists, its META UUID
	// is reused so scene/asset references survive a re-import.
	bool writeAsset(RuntimeAsset& asset, const std::filesystem::path& contentRoot);

	// Normalises a relative path to forward slashes (asset reference form).
	std::string toAssetPath(const std::filesystem::path& relativePath);

	// True when the glTF/GLB at `sourcePath` declares at least one skin, i.e. it
	// must be routed to SkeletalMeshImporter — MeshImporter ignores JOINTS_0 /
	// WEIGHTS_0 and would silently produce bind-pose geometry registered as a
	// StaticMesh. Every import entry point (asset_compiler and both editor import
	// paths) asks this one function so they cannot drift apart again.
	// Only the JSON is parsed, no buffers are loaded; anything that fails to parse
	// answers false so the caller falls back to the static path and reports that
	// importer's error instead of a second, redundant one here.
	bool gltfHasSkin(const std::filesystem::path& sourcePath);

	// ─── Shared glTF geometry path (MeshImporter + SkeletalMeshImporter) ──────
	// Both mesh importers read the same POSITION / NORMAL / TEXCOORD_0 streams in
	// the same way — including the V flip, which used to live in two copies.

	// The vertex attributes of one primitive. `joints` / `weights` stay null on
	// the static path, which does not read them.
	struct GltfPrimitiveAttributes
	{
		const cgltf_accessor* position = nullptr;   // null → primitive was skipped
		const cgltf_accessor* normal   = nullptr;
		const cgltf_accessor* uv       = nullptr;   // TEXCOORD_0
		const cgltf_accessor* joints   = nullptr;   // JOINTS_0
		const cgltf_accessor* weights  = nullptr;   // WEIGHTS_0
	};

	// The three parallel per-vertex arrays both mesh assets carry. StaticMeshAsset
	// and SkeletalMeshAsset declare them separately (they share no base class), so
	// they are bound here by reference.
	struct MeshVertexStreams
	{
		std::vector<float>& positions;
		std::vector<float>& normals;
		std::vector<float>& uvs;
	};

	// Appends one primitive's positions/normals/UVs to `out` (transformed by
	// `world`, scaled by `uniformScale`) and its indices to `indices`, rebased
	// onto the vertices just appended. Non-triangle primitives and primitives
	// without POSITION are skipped; the returned attributes then carry a null
	// `position` and the caller must not append anything else for them.
	GltfPrimitiveAttributes appendPrimitive(const cgltf_primitive& prim,
	                                        const glm::mat4&       world,
	                                        float                  uniformScale,
	                                        MeshVertexStreams      out,
	                                        std::vector<uint32_t>& indices);

	// Appends the 4 joint indices + 4 weights per vertex of the primitive that
	// appendPrimitive() just consumed, keeping both arrays index-parallel to the
	// vertices. A vertex without JOINTS_0/WEIGHTS_0 gets joint 0 at full weight.
	void appendSkinning(const GltfPrimitiveAttributes& attrs,
	                    std::vector<uint32_t>&         boneIDs,
	                    std::vector<float>&            boneWeights);

	// Imports the first base-color texture found in the glTF plus a MaterialAsset
	// referencing it, both written next to the mesh. Returns the MATERIAL's asset
	// path — the value that belongs in StaticMeshAsset/SkeletalMeshAsset's
	// `materialPath` (chunk MREF), which every renderer resolves as a material
	// reference. Empty when the glTF has no base-color texture, or when writing
	// the texture or the material failed.
	std::string importBaseColorMaterial(const cgltf_data*            data,
	                                    const std::filesystem::path& sourcePath,
	                                    const std::filesystem::path& contentRoot,
	                                    const std::filesystem::path& relativeOutputDir,
	                                    const std::string&           meshStem);

	// ─── Re-import bookkeeping ────────────────────────────────────────────────

	// The sidecar assets that were written alongside an already-imported mesh: the
	// material its MREF chunk names, plus that material's textures. Paths are
	// relative to the content root, material first.
	// They are read back off the asset instead of guessed, so a mesh whose glTF
	// carried no base-color texture yields an empty list rather than looking
	// permanently out of date. Returns {} for every asset type without an MREF
	// chunk, and for anything that cannot be opened.
	std::vector<std::string> meshSidecarAssets(const std::filesystem::path& meshAsset,
	                                           const std::filesystem::path& contentRoot);

	// True when an import of `source` has nothing left to do: `primaryOutput`, the
	// sidecars it names (meshSidecarAssets) and every path in `extraOutputs` (given
	// relative to `contentRoot` — the asset compiler passes the animation clips a
	// rigged glTF produces) all exist and are at least as new as the source.
	// A DELETED sidecar makes this false, so the next run regenerates it without
	// needing --force.
	bool importOutputsUpToDate(const std::filesystem::path&    source,
	                           const std::filesystem::path&    contentRoot,
	                           const std::filesystem::path&    primaryOutput,
	                           const std::vector<std::string>& extraOutputs = {});
}
