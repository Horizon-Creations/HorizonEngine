// glTF 2.0 materials + textures → PbrMaterialDesc records, which the shared core
// in PbrMaterialImport.cpp turns into MaterialAssets with a real PBR node graph.
//
// This file holds only what is glTF's own: resolving images (external URI,
// .glb buffer view, base64 data URI), the metallic-roughness packing (B/G of one
// image), KHR_texture_transform converted into the engine's flipped-V UV space,
// KHR_materials_specular / emissive_strength, alphaMode, the UV-set choice the
// mesh importers store per primitive, and the spec rule that an emissive texture
// with a zero factor emits nothing. Naming, the image dedupe, the graph, the
// texture-slot budget, the re-import rules and the write are the core's — the
// same code the Assimp formats go through.
#include "ImporterCommon.h"
#include "PbrMaterialImport.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Diagnostics/Logger.h"

// Declarations only — CGLTF_IMPLEMENTATION is defined exactly once, in
// MeshImporter.cpp, for the whole HorizonImporters link unit.
#include "cgltf.h"

namespace Importer
{
namespace
{

void logWarn(const std::string& msg)  { HE_LOG_WARN(Tool,  "%s", ("GltfMaterials: " + msg).c_str()); }

// ─── UV set ──────────────────────────────────────────────────────────────────

// The `texCoord` a texture view samples, honouring KHR_texture_transform's own
// texcoord override (the extension may redirect a view to a different set).
int viewUvSet(const cgltf_texture_view& view)
{
	if (view.has_transform && view.transform.has_texcoord)
		return static_cast<int>(view.transform.texcoord);
	return static_cast<int>(view.texcoord);
}

} // namespace

int gltfMaterialUvSet(const cgltf_material* material)
{
	if (!material)
		return 0;
	const cgltf_pbr_metallic_roughness& pbr = material->pbr_metallic_roughness;

	// Base colour decides, because it is the channel a mismatch is most visible in
	// and the one every material has. The others only get to disagree loudly.
	int  chosen  = 0;
	bool haveAny = false;
	const auto consider = [&](const cgltf_texture_view& v, bool authoritative)
	{
		if (!v.texture) return;
		const int set = viewUvSet(v);
		if (authoritative || !haveAny) { chosen = set; haveAny = true; }
	};
	consider(pbr.base_color_texture,         true);
	consider(pbr.metallic_roughness_texture, false);
	consider(material->normal_texture,       false);
	consider(material->occlusion_texture,    false);
	consider(material->emissive_texture,     false);
	return chosen;
}

namespace
{

// ─── Images ──────────────────────────────────────────────────────────────────

// One glTF image as the core's PbrImage: an external file next to the .gltf, the
// bytes of a .glb buffer view, or a decoded base64 data URI.
PbrImage describeImage(const cgltf_data* data, const cgltf_image& img,
                       const std::filesystem::path& sourcePath)
{
	PbrImage out;
	out.name = img.name ? img.name : "";

	// External file referenced relative to the glTF.
	if (img.uri && std::strncmp(img.uri, "data:", 5) != 0)
	{
		// cgltf_decode_uri works in place and only ever shrinks the string (%XX →
		// one byte), so a std::string buffer is safe — the fixed 1024-byte array
		// this used to use truncated any longer path into a file that is not there.
		std::string uri(img.uri);
		cgltf_decode_uri(uri.data());
		uri.resize(std::strlen(uri.c_str()));

		const std::filesystem::path texFile = sourcePath.parent_path() / uri;
		std::error_code ec;
		if (!std::filesystem::is_regular_file(texFile, ec))
			out.unresolved = uri;
		else
			out.file = texFile;
		return out;
	}

	// Embedded: either a bufferView into the .glb's binary chunk, or a base64
	// data: URI. cgltf_load_buffers resolves data-URI BUFFERS but leaves data-URI
	// IMAGES encoded, so that case is decoded here rather than dropped (which is
	// what the previous importer did — every .gltf with inlined images imported
	// untextured, with no error).
	if (img.buffer_view && img.buffer_view->buffer && img.buffer_view->buffer->data)
	{
		const auto* bytes = static_cast<const uint8_t*>(img.buffer_view->buffer->data)
		                  + img.buffer_view->offset;
		out.encoded.assign(bytes, bytes + img.buffer_view->size);
		return out;
	}
	if (img.uri)
	{
		const char* comma = std::strchr(img.uri, ',');
		// Only base64 payloads: cgltf_load_buffer_base64 is a base64 decoder, and
		// handing it a percent-encoded or plain-text data URI decodes garbage.
		if (!comma || std::strstr(img.uri, ";base64") == nullptr || comma < img.uri + 7)
			return out;

		const char* b64 = comma + 1;
		// The decoded length must be EXACT: the decoder produces precisely the
		// requested byte count and fails on the first character outside the base64
		// alphabet — the '=' padding included. Over-estimating the size therefore
		// does not merely append junk, it makes every padded image fail to decode.
		size_t encoded = std::strlen(b64);
		while (encoded > 0 && b64[encoded - 1] == '=')
			--encoded;
		const cgltf_size decoded = static_cast<cgltf_size>(encoded * 3 / 4);
		if (decoded == 0)
			return out;

		void*         raw = nullptr;
		cgltf_options opt{};   // null alloc/free funcs → cgltf's malloc/free defaults
		if (cgltf_load_buffer_base64(&opt, decoded, b64, &raw) != cgltf_result_success || !raw)
		{
			logWarn(sourcePath.filename().string() + ": image "
			        + std::to_string(cgltf_image_index(data, &img))
			        + " has an undecodable base64 data URI");
			return out;
		}
		out.encoded.assign(static_cast<uint8_t*>(raw), static_cast<uint8_t*>(raw) + decoded);
		std::free(raw);
	}
	return out;
}

// ─── Texture views ───────────────────────────────────────────────────────────

// Resolves one texture view into a PbrTextureRef, converting KHR_texture_transform
// into the UV node's tiling/offset.
//
// The conversion is not the identity, because the two spaces disagree about V. The
// extension transforms in glTF UV space (v down): v_gltf' = v_gltf * sy + oy. The
// engine stores w = 1 - v_gltf and samples a vertically flipped image, so the
// coordinate the UV node must produce is t = 1 - v_gltf' = (1 - sy - oy) + sy * w.
// U is unaffected. Taking the extension's numbers verbatim would slide and mirror
// every transformed texture along V.
PbrTextureRef texRef(const cgltf_data* data, const cgltf_texture_view& view,
                     int channel, const std::string& label)
{
	PbrTextureRef ref;
	ref.channel = channel;
	if (!view.texture || !view.texture->image)
		return ref;
	ref.image = static_cast<int>(cgltf_image_index(data, view.texture->image));
	if (!view.has_transform)
		return ref;

	if (view.transform.rotation != 0.0f)
		// The UV node has tiling and offset, no rotation. Silently ignoring it would
		// look like a mis-authored texture rather than a missing feature.
		logWarn(label + ": KHR_texture_transform rotation is not supported — ignored");

	ref.tiling[0] = view.transform.scale[0];
	ref.tiling[1] = view.transform.scale[1];
	ref.offset[0] = view.transform.offset[0];
	ref.offset[1] = 1.0f - view.transform.scale[1] - view.transform.offset[1];
	return ref;
}

// ─── One material ────────────────────────────────────────────────────────────

// The core's description of one glTF material. Everything glTF states is folded
// in here; the label is the material's output name, used in the warnings.
PbrMaterialDesc describeMaterial(const cgltf_data* data, const cgltf_material& m,
                                 const std::string& label)
{
	PbrMaterialDesc d;
	d.name = m.name ? m.name : "";

	const cgltf_pbr_metallic_roughness& pbr = m.pbr_metallic_roughness;
	// A material with no metallic-roughness block (KHR_materials_pbrSpecularGlossiness
	// only, or a bare `{}`) leaves cgltf's factors at their glTF defaults, which is
	// exactly what the spec says to fall back to — so no special case is needed.
	for (int k = 0; k < 4; ++k) d.baseColor[k] = pbr.base_color_factor[k];
	d.metallic  = pbr.metallic_factor;
	d.roughness = pbr.roughness_factor;

	// KHR_materials_emissive_strength scales the factor; folding it in here is what
	// keeps a glowing Unreal material glowing instead of clamping at the factor's 0..1.
	const float strength = m.has_emissive_strength ? m.emissive_strength.emissive_strength : 1.0f;
	for (int k = 0; k < 3; ++k) d.emissive[k] = m.emissive_factor[k] * strength;

	// KHR_materials_specular. The two conventions line up exactly, so this is a
	// conversion and not a judgement call:
	//     engine   F0 = 0.08 * pin        (MaterialShaderLibrary's heLitP; Unreal's rule)
	//     glTF     F0 = 0.04 * specularFactor
	//     pin = 0.5 * specularFactor  ⇒  0.08 * 0.5 * s = 0.04 * s  for every s,
	// and glTF's default s = 1 lands on the pin's own default of 0.5.
	// Unreal writes this for a foliage material whose Specular the artist pulled
	// down (0.05 on bark, 0.03 on leaves); left unwired the surface would sit at the
	// dielectric 0.04 and read as wet plastic.
	// Only the scalar factor is imported. specularColorFactor/specularTexture would
	// need a vec3 pin, and pin 2 is a float — widening it is an on-disk graph format
	// change (kMatGraphVersion + a remap), not an importer one.
	if (m.has_specular)
	{
		d.hasSpecular = true;
		d.specular    = 0.5f * m.specular.specular_factor;
	}

	d.normalScale       = m.normal_texture.scale;
	d.occlusionStrength = m.occlusion_texture.scale;   // == "strength"
	d.doubleSided       = m.double_sided != 0;

	switch (m.alpha_mode)
	{
	case cgltf_alpha_mode_mask:  d.alphaMode = PbrMaterialDesc::AlphaMode::Mask;  break;
	case cgltf_alpha_mode_blend: d.alphaMode = PbrMaterialDesc::AlphaMode::Blend; break;
	default:                     d.alphaMode = PbrMaterialDesc::AlphaMode::Opaque; break;
	}
	d.alphaCutoff = m.alpha_cutoff;

	// glTF packs metallic into B and roughness into G of ONE image; occlusion is R
	// of its own view (which Unreal points at the same ORM image).
	d.baseColorTex = texRef(data, pbr.base_color_texture,         0, label);
	d.metallicTex  = texRef(data, pbr.metallic_roughness_texture, 2, label);
	d.roughnessTex = texRef(data, pbr.metallic_roughness_texture, 1, label);
	d.normalTex    = texRef(data, m.normal_texture,               0, label);
	d.occlusionTex = texRef(data, m.occlusion_texture,            0, label);
	d.emissiveTex  = texRef(data, m.emissive_texture,             0, label);

	// glTF multiplies the emissive texture BY emissiveFactor, whose default is
	// (0,0,0) — so a material that names an emissive texture without a factor emits
	// pure black by the spec. Wiring it anyway would spend one of the four graph
	// texture slots on a channel that evaluates to zero, and the budget might then
	// drop a channel that does something. The image is still imported; only the
	// graph link is skipped, so the user can wire it up if they meant it.
	const bool emissiveIsBlack = d.emissive[0] == 0.0f && d.emissive[1] == 0.0f && d.emissive[2] == 0.0f;
	if (emissiveIsBlack && d.emissiveTex.used())
	{
		logWarn(label + ": emissive texture ignored — emissiveFactor is 0,"
		        " so glTF says this material emits nothing");
		d.unlinkedImages.push_back({ d.emissiveTex.image, /*srgb=*/true });
		d.emissiveTex = PbrTextureRef{};
	}

	// The mesh importers store the UV set THIS material samples (gltfMaterialUvSet),
	// so a material on TEXCOORD_1 — which is what an Unreal bake produces — is
	// imported correctly and needs no warning. What a single UV stream genuinely
	// cannot serve is a material whose own channels disagree about the set: base
	// colour wins there, and the rest sample the wrong coordinates.
	const int matSet = gltfMaterialUvSet(&m);
	const auto checkUV = [&](const cgltf_texture_view& v, const char* what)
	{
		if (v.texture && viewUvSet(v) != matSet)
			logWarn(label + ": " + what + " samples TEXCOORD_" + std::to_string(viewUvSet(v))
			        + " but this material is imported on TEXCOORD_" + std::to_string(matSet)
			        + " — a mesh carries one UV stream, so that channel will be wrong");
	};
	checkUV(pbr.base_color_texture,         "base colour");
	checkUV(pbr.metallic_roughness_texture, "metallic-roughness");
	checkUV(m.normal_texture,               "normal");
	checkUV(m.occlusion_texture,            "occlusion");
	checkUV(m.emissive_texture,             "emissive");

	return d;
}

} // namespace

GltfMaterialImport importGltfMaterials(const cgltf_data*            data,
                                       const std::filesystem::path& sourcePath,
                                       const std::filesystem::path& contentRoot,
                                       const std::filesystem::path& relativeOutputDir,
                                       const std::string&           meshStem,
                                       const OutputTargets&         outputs)
{
	GltfMaterialImport result;
	if (!data || data->materials_count == 0)
		return result;

	std::vector<PbrImage> images;
	images.reserve(data->images_count);
	for (cgltf_size i = 0; i < data->images_count; ++i)
		images.push_back(describeImage(data, data->images[i], sourcePath));

	// The same rule the importers bind section 0 with — never null here, since the
	// glTF declares at least one material.
	const cgltf_material* primary = gltfPrimaryMaterial(data);
	const int primaryIndex = primary ? static_cast<int>(primary - data->materials)
	                                 : BakedRange::kNoMaterial;

	std::vector<PbrMaterialDesc> materials;
	materials.reserve(data->materials_count);
	for (cgltf_size i = 0; i < data->materials_count; ++i)
	{
		const cgltf_material& m = data->materials[i];
		// The warnings name the material the way its asset will be called; the
		// core derives the same stem, so the two agree.
		std::string label = sanitizeStem(m.name);
		if (label.empty())
			label = data->materials_count == 1 ? meshStem + "_mat"
			                                   : meshStem + "_mat" + std::to_string(i);
		materials.push_back(describeMaterial(data, m, label));
	}

	const PbrMaterialImport imported = importPbrMaterials(
		materials, images, primaryIndex, sourcePath, contentRoot, relativeOutputDir,
		meshStem, outputs, "GltfMaterials");
	result.primary = imported.primary;
	result.paths   = imported.paths;
	return result;
}

} // namespace Importer
