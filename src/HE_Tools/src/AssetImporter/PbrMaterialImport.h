#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "ImporterCommon.h"   // Importer::OutputTargets

// The format-neutral half of the mesh importers' material import.
//
// Every mesh format the engine reads (glTF through cgltf, FBX / OBJ / COLLADA
// through Assimp) describes its materials differently, but what the engine
// writes for each of them is the same thing: one MaterialAsset per source
// material with a PBR node graph plus the GLSL that graph generates, and one
// TextureAsset per source image, imported once no matter how many materials
// share it. This header is the seam between the two: a format reader turns its
// materials into PbrMaterialDesc records over a PbrImage table, and
// importPbrMaterials() below does everything from there — output naming, the
// image dedupe, the graph, the texture-slot budget, the re-import rules, the
// write. GltfMaterialImport.cpp and AssimpMaterialImport.cpp are the two readers.
//
// Private to HorizonImporters (nothing outside the importers includes it).
namespace Importer
{
	// One image a source declares. Materials refer to it by INDEX into the table
	// their reader built, which is what lets two materials — or two channels of one
	// — that name the same image resolve to one TextureAsset.
	struct PbrImage
	{
		// Naming hint for an EMBEDDED image (glTF's image name, FBX's stored file
		// name); may be empty, then the image is named after the mesh and its index.
		std::string           name;
		// An EXTERNAL image: absolute path of the file on disk. The asset is named
		// after this file, so two meshes importing one texture land on one asset.
		std::filesystem::path file;
		// An EMBEDDED image in a compressed format stb_image decodes (PNG, JPG…).
		std::vector<uint8_t>  encoded;
		// An EMBEDDED image as raw RGBA8 pixels, rows bottom-up (the orientation
		// TextureImporter produces for files); width * height * 4 bytes.
		std::vector<uint8_t>  rgba;
		uint32_t              width  = 0;
		uint32_t              height = 0;
		// Set by the reader when the source references something it could not
		// find or decode — the reference as the file spells it, for the log line.
		// Such an image imports as nothing; the material is written without the
		// channel and says so.
		std::string           unresolved;

		bool importable() const
		{ return !file.empty() || !encoded.empty() || !rgba.empty(); }
	};

	// One material channel's texture: which image it samples, how it addresses it.
	struct PbrTextureRef
	{
		int   image     = -1;                 // index into the PbrImage table; -1 = unused
		// The UV node the sampler reads through, in the ENGINE's UV space (V up,
		// origin bottom-left): uv * tiling + offset. A reader whose format
		// transforms in another space (glTF, V down) converts before filling this.
		float tiling[2] = { 1.0f, 1.0f };
		float offset[2] = { 0.0f, 0.0f };
		// For the SCALAR channels (metallic, roughness, occlusion): which colour
		// channel of the sampled texel carries the value — SplitRGBA pin 0..3 =
		// R, G, B, A. glTF packs metallic into B and roughness into G of one image;
		// an OBJ's map_Pm / map_Pr are two grey images read from R.
		int   channel   = 0;

		bool used() const { return image >= 0; }
	};

	// Everything the PBR graph and the MaterialAsset's scalar fields are built from.
	// Defaults are DELIBERATELY not any one format's: each reader sets every field
	// it has an opinion on (glTF's metallic default is 1, a Phong material's is 0).
	struct PbrMaterialDesc
	{
		enum class AlphaMode { Opaque, Mask, Blend };

		std::string   name;                            // the source's material name; may be empty
		// The loader made this material up rather than read it (Assimp's OBJ
		// reader always prepends a "DefaultMaterial" for faces without usemtl)
		// and no geometry uses it: written as nothing, its path stays empty.
		// Keeps the table index-parallel to the source's material table.
		bool          omit              = false;
		float         baseColor[4]      = { 1.0f, 1.0f, 1.0f, 1.0f };   // rgb tint + alpha
		float         metallic          = 0.0f;
		float         roughness         = 0.5f;
		float         emissive[3]       = { 0.0f, 0.0f, 0.0f };        // multiplies the emissive texture
		// The Output node's Specular pin (engine F0 = 0.08 * pin), when the source
		// states one; left unwired otherwise so the pin keeps its own default.
		bool          hasSpecular       = false;
		float         specular          = 0.5f;
		float         normalScale       = 1.0f;         // Normal Map node strength
		float         occlusionStrength = 1.0f;         // lerp(1, sampled, strength)
		AlphaMode     alphaMode         = AlphaMode::Opaque;
		float         alphaCutoff       = 0.5f;
		bool          doubleSided       = false;

		PbrTextureRef baseColorTex;   // rgb = colour, a = opacity (Mask / Blend)
		PbrTextureRef metallicTex;    // scalar, see PbrTextureRef::channel
		PbrTextureRef roughnessTex;   // scalar
		PbrTextureRef normalTex;      // tangent-space, OpenGL convention (+Y up)
		PbrTextureRef occlusionTex;   // scalar
		PbrTextureRef emissiveTex;    // rgb

		// Images the material names but does NOT wire — imported all the same, so
		// the user can connect them by hand (glTF's emissive texture under a zero
		// factor: by the spec it emits nothing, and wiring it would spend a graph
		// texture slot on a channel that evaluates to black). `srgb` says whether
		// the image is colour data.
		struct UnlinkedImage { int image = -1; bool srgb = false; };
		std::vector<UnlinkedImage> unlinkedImages;
	};

	// The result of importing every material a source declares — what the mesh
	// importers bind: `primary` into the mesh's own materialPath (chunk MREF) and
	// `paths` section by section through buildMeshSections.
	struct PbrMaterialImport
	{
		// Content-relative path of the material at `primaryIndex` — section 0's,
		// which the mesh binds at mesh level. Empty when the source declares no
		// materials, or when writing that one failed.
		std::string              primary;
		// Every material, index-parallel to the descriptions handed in. An entry is
		// empty when that material failed to write.
		std::vector<std::string> paths;
	};

	// Writes every material in `materials` as its own MaterialAsset (PBR graph +
	// generated GLSL) and every image they reference as a TextureAsset, at most
	// once per image. `primaryIndex` is the material the mesh binds at mesh level
	// (the one a re-import may redirect; BakedRange::kNoMaterial when the source
	// declares none — then nothing is written).
	//
	// Naming: materials after their source name (sanitised), unnamed ones
	// "<meshStem>_mat" / "<meshStem>_mat<i>"; external images after their file,
	// embedded ones after PbrImage::name or "<meshStem>_img<i>". Every output of
	// one import lands in one flat folder, so names are handed out through a
	// reserver that never lets one asset be written over another of a different
	// type — including the mesh itself, whose stems are reserved up front.
	//
	// `outputs.material` / `outputs.texture` redirect the bound material and its
	// base-colour texture onto files that already exist (a re-import of a mesh
	// whose sidecars were renamed) — only for a single-material source, since
	// there is exactly one recorded sidecar to redirect. A material that is
	// already on disk is left untouched when it is not the bound one, or when it
	// is redirected: the generated graph would overwrite one the artist has since
	// authored. Textures are refreshed either way.
	//
	// `logPrefix` names the reader in every warning ("GltfMaterials",
	// "AssimpMaterials") so a log line can be traced to the file format.
	PbrMaterialImport importPbrMaterials(const std::vector<PbrMaterialDesc>& materials,
	                                     const std::vector<PbrImage>&        images,
	                                     int                                 primaryIndex,
	                                     const std::filesystem::path&        sourcePath,
	                                     const std::filesystem::path&        contentRoot,
	                                     const std::filesystem::path&        relativeOutputDir,
	                                     const std::string&                  meshStem,
	                                     const OutputTargets&                outputs,
	                                     const char*                         logPrefix);

	// A source-file name turned into an output stem: everything that is not
	// alphanumeric, '_' or '-' becomes '_', leading/trailing '_' are trimmed.
	// Empty when nothing survives, so the caller falls back to an indexed name.
	// Shared with the readers, which use it for their own naming hints.
	std::string sanitizeStem(const char* raw);
}
