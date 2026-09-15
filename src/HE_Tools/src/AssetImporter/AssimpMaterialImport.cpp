// FBX / OBJ / COLLADA materials (as Assimp presents them) → PbrMaterialDesc
// records for the shared core in PbrMaterialImport.cpp.
//
// Assimp's material model is Phong with PBR keys bolted on: every loader writes
// COLOR_DIFFUSE / SHININESS / OPACITY, and the ones that know better (FBX's
// Maya-Stingray and 3dsMax-Physical materials, OBJ's map_Pr / map_Pm extension)
// add BASE_COLOR, METALLIC_FACTOR, ROUGHNESS_FACTOR and typed texture slots. This
// file decides, per channel, which of those the engine's metallic-roughness
// graph takes and what a missing one means — and that is NOT what it means in
// glTF: a Phong material without a metallic key is a dielectric (0), not glTF's
// default metal (1), and a shininess exponent is a roughness in disguise.
//
// UV space needs no conversion here, unlike glTF's: Assimp's origin is the
// engine's (bottom-left), which the OBJ fixture in test_assimpimport pins, so an
// aiUVTransform's scaling and translation go into the UV node verbatim.
//
// Only compiled when HE_HAVE_ASSIMP is defined.
#include "AssimpMeshImport.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

#include <assimp/material.h>
#include <assimp/ObjMaterial.h>   // AI_MATKEY_OBJ_BUMPMULT_*
#include <assimp/scene.h>
#include <assimp/texture.h>

#include "Diagnostics/Logger.h"

namespace Importer
{
namespace
{

void logWarn(const std::string& msg) { HE_LOG_WARN(Tool, "%s", ("AssimpMaterials: " + msg).c_str()); }
void logInfo(const std::string& msg) { HE_LOG_INFO(Tool, "%s", ("AssimpMaterials: " + msg).c_str()); }

// ─── Images ──────────────────────────────────────────────────────────────────

// Builds the PbrImage table: one entry per distinct texture reference string.
// Two materials naming the same file (or the same "*3") share one entry, which
// is what makes them share one TextureAsset downstream.
class ImageTable
{
public:
	ImageTable(const aiScene& scene, const std::filesystem::path& sourcePath,
	           std::vector<PbrImage>& images)
		: m_scene(scene), m_source(sourcePath), m_images(images) {}

	int resolve(const aiString& reference)
	{
		const std::string key = reference.C_Str();
		const auto it = m_byKey.find(key);
		if (it != m_byKey.end())
			return it->second;

		const int index = static_cast<int>(m_images.size());
		m_images.push_back(describe(key));
		m_byKey.emplace(key, index);
		return index;
	}

	// Whether a resolved reference has pixels behind it (found on disk or
	// embedded) — a reference to a file that is not there does not.
	bool importable(const PbrTextureRef& ref) const
	{
		return ref.used() && ref.image < static_cast<int>(m_images.size())
		    && m_images[static_cast<size_t>(ref.image)].importable();
	}

private:
	PbrImage describe(const std::string& key) const
	{
		PbrImage img;

		// Embedded first: catches both the "*N" index form and — with Assimp's
		// current FBX naming — a stored file name that matches an embedded
		// aiTexture, which must not be looked for on disk (it is usually not there:
		// the artist's absolute path from another machine).
		if (const aiTexture* tex = m_scene.GetEmbeddedTexture(key.c_str()))
		{
			const std::string stored = tex->mFilename.length > 0 ? tex->mFilename.C_Str()
			                         : (key.empty() || key[0] == '*') ? std::string{} : key;
			if (!stored.empty())
				img.name = std::filesystem::path(normalizeSlashes(stored)).stem().string();

			if (tex->mHeight == 0)
			{
				// Compressed: mWidth is the byte count of a PNG/JPG/… stb decodes.
				const auto* bytes = reinterpret_cast<const uint8_t*>(tex->pcData);
				img.encoded.assign(bytes, bytes + tex->mWidth);
			}
			else
			{
				// Raw aiTexel (b,g,r,a per pixel), first row at the TOP. TextureImporter
				// stores files bottom-up (stb flips on load), so the rows are turned
				// over here — the two paths must agree on where V = 0 is.
				img.width  = tex->mWidth;
				img.height = tex->mHeight;
				img.rgba.resize(static_cast<size_t>(tex->mWidth) * tex->mHeight * 4);
				for (unsigned y = 0; y < tex->mHeight; ++y)
				{
					const aiTexel* row = tex->pcData + static_cast<size_t>(tex->mHeight - 1 - y) * tex->mWidth;
					uint8_t*       dst = img.rgba.data() + static_cast<size_t>(y) * tex->mWidth * 4;
					for (unsigned x = 0; x < tex->mWidth; ++x)
					{
						dst[x * 4 + 0] = row[x].r;
						dst[x * 4 + 1] = row[x].g;
						dst[x * 4 + 2] = row[x].b;
						dst[x * 4 + 3] = row[x].a;
					}
				}
			}
			if (!img.importable())
				img.unresolved = key;
			return img;
		}

		// External. The reference is whatever the exporter wrote — relative to the
		// source, absolute on the author's machine, backslashes and all — so it is
		// tried as given, then relative to the source's folder, then by bare file
		// name next to the source (the one that rescues most FBX files that left
		// their machine).
		const std::filesystem::path ref = normalizeSlashes(key);
		std::error_code ec;
		for (const std::filesystem::path& candidate : {
		         ref.is_absolute() ? ref : std::filesystem::path{},
		         m_source.parent_path() / ref,
		         m_source.parent_path() / ref.filename() })
		{
			if (candidate.empty() || !std::filesystem::is_regular_file(candidate, ec))
				continue;
			img.file = std::filesystem::absolute(candidate, ec);
			if (img.file.empty()) img.file = candidate;
			return img;
		}
		img.unresolved = key;
		return img;
	}

	static std::string normalizeSlashes(std::string s)
	{
		std::replace(s.begin(), s.end(), '\\', '/');
		return s;
	}

	const aiScene&               m_scene;
	std::filesystem::path        m_source;
	std::vector<PbrImage>&       m_images;
	std::map<std::string, int>   m_byKey;
};

// ─── One texture slot ────────────────────────────────────────────────────────

// The first texture of `type` on the material as a PbrTextureRef, or an unused
// one. `channel` is the SplitRGBA pin the scalar channels read.
PbrTextureRef texRef(const aiMaterial& m, aiTextureType type, int channel,
                     ImageTable& images, const std::string& label, const char* what)
{
	PbrTextureRef ref;
	ref.channel = channel;
	if (m.GetTextureCount(type) == 0)
		return ref;

	aiString path;
	unsigned uvIndex = 0;   // left alone when the loader wrote no UVWSRC
	if (m.GetTexture(type, 0, &path, nullptr, &uvIndex) != AI_SUCCESS || path.length == 0)
		return ref;
	ref.image = images.resolve(path);

	// The bake reads UV channel 0 only (AssimpMeshImport::appendMesh). A material
	// on another set is imported on the wrong coordinates — say so rather than
	// let it look like a mis-authored texture.
	if (uvIndex != 0)
		logWarn(label + ": " + what + " samples UV set " + std::to_string(uvIndex)
		        + " but only set 0 is imported — that channel will be misplaced");

	aiUVTransform transform;
	if (m.Get(AI_MATKEY_UVTRANSFORM(type, 0), transform) == AI_SUCCESS)
	{
		if (transform.mRotation != 0.0f)
			// The UV node has tiling and offset, no rotation.
			logWarn(label + ": " + what + " has a UV rotation, which is not supported — ignored");
		ref.tiling[0] = transform.mScaling.x;
		ref.tiling[1] = transform.mScaling.y;
		ref.offset[0] = transform.mTranslation.x;
		ref.offset[1] = transform.mTranslation.y;
	}
	return ref;
}

// The first of `types` the material carries, in that order of preference.
template <size_t N>
PbrTextureRef firstOf(const aiMaterial& m, const aiTextureType (&types)[N], int channel,
                      ImageTable& images, const std::string& label, const char* what)
{
	for (aiTextureType t : types)
	{
		PbrTextureRef ref = texRef(m, t, channel, images, label, what);
		if (ref.used())
			return ref;
	}
	PbrTextureRef none;
	none.channel = channel;
	return none;
}

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// ─── One material ────────────────────────────────────────────────────────────

PbrMaterialDesc describeMaterial(const aiMaterial& m, ImageTable& images, const std::string& label)
{
	PbrMaterialDesc d;

	aiString name;
	if (m.Get(AI_MATKEY_NAME, name) == AI_SUCCESS)
		d.name = name.C_Str();

	// ── Textures ──────────────────────────────────────────────────────────────
	// Preference per channel: the typed PBR slot the FBX PBR materials and OBJ's
	// extension fill, then the classic Phong slot every loader fills.
	d.baseColorTex = firstOf(m, { aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE }, 0,
	                         images, label, "base colour");
	d.emissiveTex  = firstOf(m, { aiTextureType_EMISSION_COLOR, aiTextureType_EMISSIVE }, 0,
	                         images, label, "emissive");
	d.occlusionTex = firstOf(m, { aiTextureType_AMBIENT_OCCLUSION, aiTextureType_LIGHTMAP }, 0,
	                         images, label, "occlusion");

	// Normal: the typed slots first. HEIGHT is where OBJ's `map_bump` / `bump` and
	// FBX's `Bump` connection land — in practice almost always a tangent-space
	// normal map (Blender's and Substance's OBJ exports write map_Bump for it), so
	// it is taken as one when nothing better exists, and the log says so: a real
	// height map read as normals renders as flat noise, which is then explainable.
	d.normalTex = firstOf(m, { aiTextureType_NORMALS, aiTextureType_NORMAL_CAMERA }, 0,
	                      images, label, "normal");
	if (!d.normalTex.used())
	{
		d.normalTex = texRef(m, aiTextureType_HEIGHT, 0, images, label, "normal");
		if (d.normalTex.used())
			logInfo(label + ": its bump map (map_bump / Bump) is imported as a tangent-space"
			        " normal map — the file does not say which it is");
	}

	// Metallic / roughness: two separate grey maps (OBJ map_Pm / map_Pr, Maya's
	// metalness / diffuseRoughness, 3dsMax's metalness_map / roughness_map),
	// each read from R. Failing both, one packed map: glTF's own packing
	// (B = metallic, G = roughness) if a loader tagged it as such, or OBJ's
	// map_RMA (R = roughness, G = metallic, B = occlusion), which Assimp files
	// under UNKNOWN — the only thing that reaches UNKNOWN from the three loaders
	// this build has.
	d.metallicTex  = texRef(m, aiTextureType_METALNESS,         0, images, label, "metallic");
	d.roughnessTex = texRef(m, aiTextureType_DIFFUSE_ROUGHNESS, 0, images, label, "roughness");
	if (!d.metallicTex.used() && !d.roughnessTex.used())
	{
		if (m.GetTextureCount(aiTextureType_GLTF_METALLIC_ROUGHNESS) > 0)
		{
			d.metallicTex  = texRef(m, aiTextureType_GLTF_METALLIC_ROUGHNESS, 2, images, label, "metallic");
			d.roughnessTex = texRef(m, aiTextureType_GLTF_METALLIC_ROUGHNESS, 1, images, label, "roughness");
		}
		else if (m.GetTextureCount(aiTextureType_UNKNOWN) > 0)
		{
			d.roughnessTex = texRef(m, aiTextureType_UNKNOWN, 0, images, label, "roughness");
			d.metallicTex  = texRef(m, aiTextureType_UNKNOWN, 1, images, label, "metallic");
			if (!d.occlusionTex.used())
				d.occlusionTex = texRef(m, aiTextureType_UNKNOWN, 2, images, label, "occlusion");
			logInfo(label + ": its packed map is read as OBJ's map_RMA"
			        " (R = roughness, G = metallic, B = occlusion)");
		}
	}

	// Channels the metallic-roughness graph has no pin for. Named once so a
	// material that looks wrong can be traced to what was left out.
	if (m.GetTextureCount(aiTextureType_SHININESS) > 0 && !d.roughnessTex.used())
		logWarn(label + ": has a shininess / glossiness map, which the metallic-roughness"
		        " model cannot take as it is — not imported (invert it into a roughness map)");
	if (m.GetTextureCount(aiTextureType_SPECULAR) > 0)
		logWarn(label + ": has a specular colour map — not imported (no vec3 Specular pin)");

	// ── Factors ───────────────────────────────────────────────────────────────
	// Base colour: the PBR key when a PBR material wrote one, else Phong's diffuse.
	// With a base-colour TEXTURE the colour is not applied as a tint: Phong
	// exporters write Kd / DiffuseColor as a lighting coefficient (Blender's OBJ:
	// Kd 0.8 next to map_Kd), and multiplying it in darkens every textured
	// material — Unreal's and Unity's FBX importers drop it for the same reason.
	// glTF's baseColorFactor IS a tint by spec, which is why the glTF reader keeps it.
	// A map that is NOT THERE (the file did not travel with the mesh) keeps the
	// colour: the material then shows the Phong colour rather than plain white,
	// and the missing-texture warning says what to fix.
	aiColor4D base(1.0f, 1.0f, 1.0f, 1.0f);
	aiColor3D diffuse;
	if (m.Get(AI_MATKEY_BASE_COLOR, base) != AI_SUCCESS
	    && m.Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS)
		base = aiColor4D(diffuse.r, diffuse.g, diffuse.b, 1.0f);
	if (images.importable(d.baseColorTex))
		base.r = base.g = base.b = 1.0f;
	d.baseColor[0] = clamp01(base.r);
	d.baseColor[1] = clamp01(base.g);
	d.baseColor[2] = clamp01(base.b);

	// Opacity: the explicit key (OBJ's d / Tr, FBX's Opacity, COLLADA's
	// transparency), else the base colour's alpha when a PBR key carried one.
	float opacity = 1.0f;
	if (m.Get(AI_MATKEY_OPACITY, opacity) != AI_SUCCESS)
		opacity = base.a;
	d.baseColor[3] = clamp01(opacity);

	// Metallic: a dielectric unless the file says otherwise. Roughness: the PBR
	// key, else Phong's specular exponent turned around the way Assimp's own FBX
	// loader (and Blender) do it, else the middle of the range. An exponent of 0
	// is "not set" for OBJ (Assimp's default), not a mirror.
	if (m.Get(AI_MATKEY_METALLIC_FACTOR, d.metallic) != AI_SUCCESS)
		d.metallic = 0.0f;
	d.metallic = clamp01(d.metallic);
	float shininess = 0.0f;
	if (m.Get(AI_MATKEY_ROUGHNESS_FACTOR, d.roughness) == AI_SUCCESS)
		d.roughness = clamp01(d.roughness);
	else if (m.Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess > 0.0f)
		d.roughness = clamp01(1.0f - std::sqrt(shininess) / 10.0f);
	else
		d.roughness = 0.5f;
	// A metallic / roughness TEXTURE multiplies by the factor in the graph, so a
	// factor that only existed as a Phong fallback must not darken the map: with a
	// map, the factor is 1 unless a PBR key explicitly set it.
	float explicitMetallic = 0.0f, explicitRoughness = 0.0f;
	if (d.metallicTex.used() && m.Get(AI_MATKEY_METALLIC_FACTOR, explicitMetallic) != AI_SUCCESS)
		d.metallic = 1.0f;
	if (d.roughnessTex.used() && m.Get(AI_MATKEY_ROUGHNESS_FACTOR, explicitRoughness) != AI_SUCCESS)
		d.roughness = 1.0f;

	// Emissive: colour × intensity. An emissive TEXTURE under a black (or absent)
	// colour is taken at full strength — unlike glTF, no format here defines the
	// colour as the texture's multiplier, and FBX's EmissiveColor defaults to
	// black while a texture hangs off it.
	aiColor3D emissive(0.0f, 0.0f, 0.0f);
	m.Get(AI_MATKEY_COLOR_EMISSIVE, emissive);
	float intensity = 1.0f;
	m.Get(AI_MATKEY_EMISSIVE_INTENSITY, intensity);
	d.emissive[0] = emissive.r * intensity;
	d.emissive[1] = emissive.g * intensity;
	d.emissive[2] = emissive.b * intensity;
	if (d.emissiveTex.used() && d.emissive[0] == 0.0f && d.emissive[1] == 0.0f && d.emissive[2] == 0.0f)
		d.emissive[0] = d.emissive[1] = d.emissive[2] = 1.0f;

	// Normal map strength: FBX's BumpFactor, OBJ's `-bm` multiplier.
	float bump = 0.0f;
	if (m.Get(AI_MATKEY_BUMPSCALING, bump) == AI_SUCCESS && bump > 0.0f)
		d.normalScale = bump;
	else if ((m.Get(AI_MATKEY_OBJ_BUMPMULT_NORMALS(0), bump) == AI_SUCCESS
	          || m.Get(AI_MATKEY_OBJ_BUMPMULT_HEIGHT(0), bump) == AI_SUCCESS) && bump > 0.0f)
		d.normalScale = bump;

	int twoSided = 0;
	if (m.Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS)
		d.doubleSided = twoSided != 0;

	// ── Blend mode ────────────────────────────────────────────────────────────
	// None of the three formats has glTF's alphaMode. A constant opacity below 1
	// is a translucent surface; an OPACITY map (OBJ map_d, FBX TransparentColor
	// texture) that is the base-colour image itself is the usual cut-out foliage
	// — masked on the base colour's alpha, which the graph wires on its own. An
	// opacity map that is a DIFFERENT image would need a fifth sampler and its
	// own pin; it is imported so the user can wire it, and the material stays
	// opaque rather than half-right.
	const PbrTextureRef opacityTex = texRef(m, aiTextureType_OPACITY, 3, images, label, "opacity");
	if (d.baseColor[3] < 1.0f)
		d.alphaMode = PbrMaterialDesc::AlphaMode::Blend;
	else if (opacityTex.used() && d.baseColorTex.used() && opacityTex.image == d.baseColorTex.image)
		d.alphaMode = PbrMaterialDesc::AlphaMode::Mask;
	else if (opacityTex.used())
	{
		logWarn(label + ": its opacity map is a separate image, which the graph cannot"
		        " take as a mask — imported unwired, the material is written opaque");
		d.unlinkedImages.push_back({ opacityTex.image, /*srgb=*/false });
	}
	d.alphaCutoff = 0.5f;

	return d;
}

} // namespace

void AssimpScene::describeMaterials(const std::filesystem::path&  sourcePath,
                                    std::vector<PbrMaterialDesc>& materials,
                                    std::vector<PbrImage>&        images) const
{
	materials.clear();
	images.clear();
	const aiScene* scene = this->scene();
	if (!scene)
		return;

	// Which materials any mesh actually binds. Assimp's OBJ reader always puts a
	// "DefaultMaterial" at index 0 for faces without usemtl, whether or not the
	// file has any; written out, that is one stray asset per OBJ import. A default
	// that geometry DOES use (an OBJ with no mtllib at all) is a real material and
	// is kept — the mesh needs something to bind.
	std::vector<bool> referenced(scene->mNumMaterials, false);
	for (unsigned i = 0; i < scene->mNumMeshes; ++i)
		if (scene->mMeshes[i]->mMaterialIndex < scene->mNumMaterials)
			referenced[scene->mMeshes[i]->mMaterialIndex] = true;

	ImageTable table(*scene, sourcePath, images);
	materials.reserve(scene->mNumMaterials);
	for (unsigned i = 0; i < scene->mNumMaterials; ++i)
	{
		const aiMaterial& m = *scene->mMaterials[i];
		aiString    probe;
		const bool  loaderDefault = m.Get(AI_MATKEY_NAME, probe) == AI_SUCCESS
		                         && std::strcmp(probe.C_Str(), AI_DEFAULT_MATERIAL_NAME) == 0;
		if (loaderDefault && !referenced[i])
		{
			PbrMaterialDesc omitted;
			omitted.name = probe.C_Str();
			omitted.omit = true;
			materials.push_back(omitted);
			continue;
		}
		// The warnings name the material the way its asset will be called; the
		// core derives the same stem from the same name.
		aiString    name;
		std::string label;
		if (m.Get(AI_MATKEY_NAME, name) == AI_SUCCESS)
			label = sanitizeStem(name.C_Str());
		if (label.empty())
			label = sourcePath.stem().string() + "_mat" + (scene->mNumMaterials == 1 ? "" : std::to_string(i));
		materials.push_back(describeMaterial(m, table, label));
	}
}

PbrMaterialImport importAssimpMaterials(const AssimpScene&           scene,
                                        int                          primaryIndex,
                                        const std::filesystem::path& sourcePath,
                                        const std::filesystem::path& contentRoot,
                                        const std::filesystem::path& relativeOutputDir,
                                        const std::string&           meshStem,
                                        const OutputTargets&         outputs)
{
	std::vector<PbrMaterialDesc> materials;
	std::vector<PbrImage>        images;
	scene.describeMaterials(sourcePath, materials, images);
	return importPbrMaterials(materials, images, primaryIndex, sourcePath, contentRoot,
	                          relativeOutputDir, meshStem, outputs, "AssimpMaterials");
}

} // namespace Importer
