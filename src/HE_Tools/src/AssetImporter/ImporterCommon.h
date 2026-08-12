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
	//
	// `sourceFile` is the file the asset was imported FROM; it is stored
	// (absolute) on the asset so a later re-import knows what to read without
	// asking the user again. Passing none is not the same as clearing it: an
	// asset already on disk keeps the source it recorded, so a rewrite that has
	// no source of its own (an animation clip, a generated material) cannot
	// silently erase one.
	bool writeAsset(RuntimeAsset& asset, const std::filesystem::path& contentRoot,
	                const std::filesystem::path& sourceFile = {});

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

	// ─── Source routing ───────────────────────────────────────────────────────

	// True when `sourcePath`'s extension is one this namespace can import.
	// The editor asks this instead of keeping its own extension list: the menu
	// import and the Content Browser's right-click Import had drifted apart
	// (fonts were importable from one and not the other), and a list that lives
	// next to the routing below cannot drift from it at all.
	bool isImportableSource(const std::filesystem::path& sourcePath);

	// Imports one source file into <contentRoot>/<relativeOutputDir>, picking the
	// importer from the extension — including the skinned-glTF split, which is
	// not a detail a caller may re-derive: a rigged mesh sent to MeshImporter
	// imports as bind-pose geometry registered as a StaticMesh, successfully and
	// unusably. False when the extension is not importable or the import failed
	// (the importer has already logged why).
	bool importSource(const std::filesystem::path& sourcePath,
	                  const std::filesystem::path& contentRoot,
	                  const std::filesystem::path& relativeOutputDir = {});

	// ─── Re-import bookkeeping ────────────────────────────────────────────────

	// The absolute path recorded in `assetFile`'s META when it was imported, or
	// empty when it records none (authored in the editor, or written by a build
	// from before the field existed). Reads the file's header and META chunk
	// only. Callers use "empty" to disable a Reimport affordance rather than
	// offering one that cannot work.
	std::string sourceFileOf(const std::filesystem::path& assetFile);

	// Re-runs the import that produced `assetFile`, back over `assetFile` itself:
	// the output directory is the asset's CURRENT folder, not wherever the source
	// happens to live, and writeAsset recovers the existing UUID — so the asset
	// every scene already references is the one that gets updated, instead of a
	// second copy appearing at the content root.
	// False (with a log) when the asset records no source, when that source is
	// gone from disk, when the asset does not live under `contentRoot`, or when
	// the import itself failed.
	bool reimport(const std::filesystem::path& assetFile,
	              const std::filesystem::path& contentRoot);

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
