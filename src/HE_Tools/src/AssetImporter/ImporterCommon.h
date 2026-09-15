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
struct cgltf_material;

// Shared plumbing for all asset importers.
//
// Importers fill a RuntimeAsset-derived struct and hand it to writeAsset().
// `asset.path` must be set to the path relative to the content root
// (forward slashes) — the same string the ContentManager later uses to load
// the asset, and the same string other assets use to reference it
// (e.g. MaterialAsset::texturePaths).
namespace Importer
{
	// ─── Output placement ─────────────────────────────────────────────────────

	// Where an import must put its .hasset outputs, given as paths relative to the
	// content root. All fields empty means "name every output after the source
	// file", which is what a first import does and what every importer did
	// unconditionally before this existed.
	//
	// A RE-import fills them in, because the asset it re-runs may have been
	// RENAMED since it was imported. Deriving the name from the source stem then
	// is not a cosmetic slip: it writes a second .hasset, with a second UUID,
	// next to the one every scene references — see reimport().
	struct OutputTargets
	{
		std::string asset;      // the importer's primary output
		std::string material;   // mesh importers: the generated material sidecar
		std::string texture;    // mesh importers: its base-colour texture sidecar
	};

	// The path and the META name an importer's primary output takes.
	struct ResolvedOutput
	{
		std::string path;   // relative to the content root, forward slashes
		std::string name;   // that path's file stem
	};

	// `explicitPath` when the caller named one, else
	// <relativeOutputDir>/<derivedStem>.hasset. The name follows whichever FILE
	// won, never the source: re-importing an asset the user renamed must not put
	// the source's stem back into its META, where it would show up as the asset's
	// name everywhere the file name is not what is displayed.
	ResolvedOutput resolveOutput(const std::string&           explicitPath,
	                             const std::filesystem::path& relativeOutputDir,
	                             const std::string&           derivedStem);

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
	// Anything that is not a .gltf/.glb answers false WITHOUT opening it: the
	// other mesh sources (FBX/OBJ/DAE via Assimp) would otherwise be read whole
	// by cgltf just to fail — twice per re-import.
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
		const cgltf_accessor* uv       = nullptr;   // TEXCOORD_<uvSet>
		const cgltf_accessor* joints   = nullptr;   // JOINTS_0
		const cgltf_accessor* weights  = nullptr;   // WEIGHTS_0
		// The TEXCOORD set `uv` actually came from. Lower than the requested set
		// means the primitive does not carry that set and 0 was used instead — the
		// caller reports it, because the material then samples the wrong UVs.
		int                   uvSet    = 0;
	};

	// Which glTF UV set a material's textures sample: the `texCoord` on its texture
	// views (and KHR_texture_transform's own texcoord override, which wins where it
	// is present). 0 for a material with no textures, and for a null material.
	//
	// This is NOT always 0 in practice. Unreal's glTF exporter BAKES a material into
	// textures addressed by the mesh's second, non-overlapping UV set — baking needs
	// a set with no overlap, which is the lightmap UV — and then declares
	// "texCoord": 1 on every texture view. Reading TEXCOORD_0 for such an export
	// samples the baked atlas with the original tiling UVs: not subtly off, but
	// unrecognisable.
	//
	// A material whose channels disagree about the set cannot be satisfied by a mesh
	// with ONE UV stream; the base-colour channel's set wins and the caller is warned.
	int gltfMaterialUvSet(const cgltf_material* material);

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
	//
	// `uvSet` selects which TEXCOORD_n to read — normally gltfMaterialUvSet() of the
	// primitive's own material, so each primitive contributes the UVs ITS material
	// samples. That is exact rather than a compromise: a primitive's vertices are
	// appended contiguously, so the choice is per vertex range, not per mesh. A
	// primitive lacking the requested set falls back to TEXCOORD_0 and says so
	// through the returned `uvSet`.
	GltfPrimitiveAttributes appendPrimitive(const cgltf_primitive& prim,
	                                        const glm::mat4&       world,
	                                        float                  uniformScale,
	                                        MeshVertexStreams      out,
	                                        std::vector<uint32_t>& indices,
	                                        int                    uvSet = 0);

	// Appends the 4 joint indices + 4 weights per vertex of the primitive that
	// appendPrimitive() just consumed, keeping both arrays index-parallel to the
	// vertices. A vertex without JOINTS_0/WEIGHTS_0 gets joint 0 at full weight.
	void appendSkinning(const GltfPrimitiveAttributes& attrs,
	                    std::vector<uint32_t>&         boneIDs,
	                    std::vector<float>&            boneWeights);

	// ─── Material sections ────────────────────────────────────────────────────

	// Where one baked primitive landed in the merged index buffer, as recorded by
	// the mesh importers around each appendPrimitive() call (indices.size() before
	// and after), with the glTF material it was authored with.
	struct BakedPrimitive
	{
		uint32_t              indexStart = 0;
		uint32_t              indexCount = 0;
		const cgltf_material* material   = nullptr;  // null = the primitive has none
	};

	// The material a mesh binds at mesh level (chunk MREF) — which is also its
	// section 0: the material of the first primitive that carries one, in the SAME
	// order the importers bake geometry (mesh-bearing nodes first, bare meshes only
	// when the glTF has no node hierarchy), so "the material the mesh got" is the
	// one belonging to the first triangles in the buffer rather than an arbitrary
	// index into data->materials. A glTF whose primitives are all unassigned still
	// gets its first declared material; null only for a glTF that declares none.
	const cgltf_material* gltfPrimaryMaterial(const cgltf_data* data);

	// The format-neutral form of BakedPrimitive: the same index range, with the
	// material as an INDEX into whatever material table the source declares
	// (cgltf_data::materials, aiScene::mMaterials). kNoMaterial = the range has
	// none. This is what the section core below works on, so the Assimp path
	// (AssimpMeshImport) and the glTF path share one grouping rule.
	struct BakedRange
	{
		static constexpr int kNoMaterial = -1;
		uint32_t indexStart    = 0;
		uint32_t indexCount    = 0;
		int      materialIndex = kNoMaterial;
	};

	// Turns the baked primitives into the mesh's section table (MeshSection, chunk
	// MSEC) and regroups `indices` to match: primitives sharing a glTF material
	// become ONE section, in the order their material first appears in the bake,
	// each a contiguous run of the index buffer. Only whole primitive ranges move
	// and only inside `indices` — the vertex streams stay as baked, so every
	// per-vertex array (positions, UVs, joints…) remains index-parallel and the
	// per-primitive UV-set choice keeps its exact meaning.
	//
	// A primitive without a material joins `primary`'s section (the rule every
	// import has always applied: unassigned geometry takes the first material), so
	// section 0 is `primary`'s and the mesh-level MREF stays equal to slot 0. Each
	// section's materialPath is `materialPaths[index of its material]` — EMPTY when
	// materials were not imported or that one failed to write, which the engine
	// reads as "the mesh's own material" (see MeshSection). A single-material glTF
	// therefore yields one section over the whole buffer, indices untouched.
	std::vector<MeshSection> buildMeshSections(const cgltf_data*                  data,
	                                           const std::vector<BakedPrimitive>& baked,
	                                           const cgltf_material*              primary,
	                                           const std::vector<std::string>&    materialPaths,
	                                           std::vector<uint32_t>&             indices);

	// The core the glTF overload above wraps: identical rules, with materials as
	// indices. `primaryIndex` is the material a range without one joins
	// (BakedRange::kNoMaterial when the source declares none — such ranges then
	// form one section with an empty path). `materialPaths` may be shorter than
	// the material table, or empty altogether: a missing entry is an empty path.
	// Returns {} when a range does not describe `indices` (see the .cpp).
	std::vector<MeshSection> buildMeshSections(const std::vector<BakedRange>&  baked,
	                                           int                             primaryIndex,
	                                           const std::vector<std::string>& materialPaths,
	                                           std::vector<uint32_t>&          indices);

	// The result of importing every material a glTF declares.
	struct GltfMaterialImport
	{
		// Content-relative path of the material the MESH binds (chunk MREF) — that
		// of gltfPrimaryMaterial(), which is also section 0's. Empty when the glTF
		// declares no materials, or when writing that one failed.
		std::string              primary;
		// Every material, index-parallel to `cgltf_data::materials`. An entry is
		// empty when that material failed to write. This is what buildMeshSections
		// binds section by section, so a multi-material glTF imports with every one
		// of its materials assigned to the geometry it was authored on.
		std::vector<std::string> paths;
	};

	// Imports EVERY material of the glTF as its own MaterialAsset — each with a PBR
	// node graph (base colour, metallic, roughness, normal, occlusion, emissive, the
	// glTF factors folded in as constants, alphaMode as the blend mode) plus the GLSL
	// that graph generates — and every IMAGE it references as a TextureAsset, imported
	// once no matter how many materials or channels share it.
	//
	// The caller puts `primary` into StaticMeshAsset/SkeletalMeshAsset's `materialPath`
	// (chunk MREF), which every renderer resolves as a material reference, and
	// `paths` into the section table via buildMeshSections.
	//
	// `outputs.material` / `outputs.texture` redirect the bound material and its
	// base-colour texture onto files that already exist (a re-import of a mesh whose
	// sidecars were renamed) — only for a single-material glTF, since there is exactly
	// one such recorded sidecar to redirect; every other material is named after the
	// glTF material, which is stable across re-imports. `outputs.asset` is not read
	// here — it belongs to the mesh itself.
	// A redirected MATERIAL that exists is returned untouched rather than rewritten:
	// the generated material would overwrite a graph the artist has since authored in
	// the Material Editor. Textures are refreshed either way.
	GltfMaterialImport importGltfMaterials(const cgltf_data*            data,
	                                       const std::filesystem::path& sourcePath,
	                                       const std::filesystem::path& contentRoot,
	                                       const std::filesystem::path& relativeOutputDir,
	                                       const std::string&           meshStem,
	                                       const OutputTargets&         outputs = {});

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
	// `outputs` is empty for an import and filled in by reimport(), which has to
	// land on files that exist rather than on the source's stem.
	bool importSource(const std::filesystem::path& sourcePath,
	                  const std::filesystem::path& contentRoot,
	                  const std::filesystem::path& relativeOutputDir = {},
	                  const OutputTargets&         outputs = {});

	// ─── Re-import bookkeeping ────────────────────────────────────────────────

	// The absolute path recorded in `assetFile`'s META when it was imported, or
	// empty when it records none (authored in the editor, or written by a build
	// from before the field existed). STREAMS the file: the header and the META
	// chunk are read, every other chunk payload is seeked past. Callers use
	// "empty" to disable a Reimport affordance rather than offering one that
	// cannot work — which the Content Browser asks once per frame for as long as
	// a context menu is open, so this may not read the asset's payload at all.
	std::string sourceFileOf(const std::filesystem::path& assetFile);

	// Re-runs the import that produced `assetFile`, back onto `assetFile` ITSELF:
	// not just into the asset's current folder but under its current FILE NAME,
	// so the asset every scene already references is the one that gets updated
	// (writeAsset then recovers its UUID from it). Deriving either from the source
	// — the folder the source sits in, or the source's stem — produces a second
	// asset with a second UUID while every reference keeps pointing at the first.
	// A mesh's sidecars are redirected onto the ones it already names.
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
