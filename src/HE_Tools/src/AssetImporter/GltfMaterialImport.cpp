// glTF 2.0 materials + textures → MaterialAssets with a real PBR node graph.
//
// This replaces the "first base-colour texture in the file wins, wired to an unlit
// material" import that came before it. What a mesh import produces now:
//   • one MaterialAsset per glTF material, named after the glTF material,
//   • one TextureAsset per glTF IMAGE (an image shared by several materials, or used
//     twice inside one — Unreal packs occlusion+roughness+metallic into a single ORM
//     map — is imported once),
//   • a HE::MaterialGraph on each material wiring base colour, metallic, roughness,
//     normal, occlusion and emissive into the Output node, with the glTF factors
//     folded in as constants, plus the baked GLSL that graph generates.
//
// The mesh itself still binds exactly ONE material (StaticMeshAsset::materialPath →
// chunk MREF): the engine has no submesh/section concept, so a multi-material glTF
// gets its first primitive's material bound and the geometry of the others is shaded
// with it. The other materials are still written — they are correct assets, they
// simply have nowhere on THIS mesh to attach (see GltfMaterialImport::unbound) — and
// the import says so rather than dropping them silently.
#include "ImporterCommon.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "ContentManager/Assets.h"
#include "ContentManager/HAsset.h"
#include "Diagnostics/Logger.h"
#include "MaterialGraph/MaterialGraph.h"
#include "TextureImporter.h"

// Declarations only — CGLTF_IMPLEMENTATION is defined exactly once, in
// MeshImporter.cpp, for the whole HorizonImporters link unit.
#include "cgltf.h"

namespace Importer
{
namespace
{

void logWarn(const std::string& msg)  { HE_LOG_WARN(Tool,  "%s", ("GltfMaterials: " + msg).c_str()); }

// ─── Naming ──────────────────────────────────────────────────────────────────

// A glTF name turned into a file stem. Everything that is not alphanumeric, '_'
// or '-' becomes '_', because these names come from a DCC tool and routinely
// carry spaces, slashes and colons ("M_Rock.M_Rock", "Material #3") — a slash in
// particular would silently place the asset in a subfolder that no other code
// expects. Empty input, or input that sanitises away to nothing, yields "" so the
// caller falls back to its index-derived name.
std::string sanitizeStem(const char* raw)
{
	if (!raw) return {};
	std::string s;
	for (const char* p = raw; *p; ++p)
	{
		const unsigned char u = static_cast<unsigned char>(*p);
		s += (std::isalnum(u) || *p == '_' || *p == '-') ? *p : '_';
	}
	// Leading/trailing separators make for ugly file names and, for a leading dot
	// (already mapped to '_' above) hidden files on Unix; trim them off.
	const size_t first = s.find_first_not_of('_');
	if (first == std::string::npos) return {};
	const size_t last = s.find_last_not_of('_');
	return s.substr(first, last - first + 1);
}

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

// ─── Output naming ───────────────────────────────────────────────────────────

// The asset type recorded in a .hasset's header, or Unknown when the file is not
// one. Only the 32-byte header is read — this is asked once per candidate name.
HE::AssetType assetTypeOf(const std::filesystem::path& file)
{
	std::ifstream f(file, std::ios::binary);
	if (!f.is_open()) return HE::AssetType::Unknown;
	char     magic[4] = {};
	uint16_t version = 0, type = 0;
	f.read(magic, 4);
	f.read(reinterpret_cast<char*>(&version), sizeof(version));
	f.read(reinterpret_cast<char*>(&type),    sizeof(type));
	if (!f || std::memcmp(magic, HAsset::k_magic, 4) != 0) return HE::AssetType::Unknown;
	return static_cast<HE::AssetType>(type);
}

// Hands out output stems that collide with nothing.
//
// Every output of one import — N materials, M textures AND the mesh itself — lands
// in the same flat folder under a stem taken from an untrusted glTF string. Nothing
// stopped a material called "Wood" from being written over the .hasset a "Wood.png"
// texture had just produced: writeAsset recovers the uuid from whatever file is
// already there, so the material inherited the TEXTURE's identity and every
// reference to that texture silently resolved to a material instead — a mesh that
// renders untextured with not one line logged. The same collision hits a material
// named after the source file, because the mesh is written LAST and wins.
//
// A stem is free when this import has not already claimed it AND the file either
// does not exist or holds an asset of the SAME type. That second clause is what
// keeps re-imports idempotent: a texture landing on its own previous .hasset is
// exactly right, while a texture landing on a material is not.
class NameReserver
{
public:
	NameReserver(std::filesystem::path contentRoot, std::filesystem::path relativeOutputDir)
		: m_root(std::move(contentRoot)), m_dir(std::move(relativeOutputDir)) {}

	// Reserve a name up front for an output written by someone else (the mesh).
	void reserve(const std::string& stem) { if (!stem.empty()) m_taken.push_back(stem); }

	std::string claim(const std::string& wanted, HE::AssetType type)
	{
		if (isFree(wanted, type)) { m_taken.push_back(wanted); return wanted; }
		for (int n = 2; n < 1000; ++n)
		{
			const std::string candidate = wanted + "_" + std::to_string(n);
			if (!isFree(candidate, type)) continue;
			logWarn("output name '" + wanted + "' is already taken by another asset — '"
			        + candidate + "' used instead");
			m_taken.push_back(candidate);
			return candidate;
		}
		return wanted;   // 1000 collisions: let the caller write and be overwritten
	}

private:
	bool isFree(const std::string& stem, HE::AssetType type) const
	{
		if (stem.empty()) return false;
		if (std::find(m_taken.begin(), m_taken.end(), stem) != m_taken.end()) return false;
		const std::filesystem::path file = m_root / m_dir / (stem + ".hasset");
		std::error_code ec;
		if (!std::filesystem::is_regular_file(file, ec)) return true;
		const HE::AssetType existing = assetTypeOf(file);
		// Unknown = not a readable .hasset. Overwriting that is what an import has
		// always done, and refusing would strand the name forever.
		return existing == type || existing == HE::AssetType::Unknown;
	}

	std::filesystem::path    m_root;
	std::filesystem::path    m_dir;
	std::vector<std::string> m_taken;
};

// ─── Image import (one TextureAsset per glTF image, imported at most once) ────

// Imports each glTF image exactly once per source file and hands out its asset
// path. Two materials sharing a texture, or one material using its ORM map as both
// the metallic-roughness AND the occlusion source, must resolve to the SAME asset
// path: the material graph deduplicates its texture slots by path (see
// HE::textureSampler), so anything else burns a second slot out of the four a graph
// has — and would import the same pixels twice.
class ImageCache
{
public:
	ImageCache(const cgltf_data*            data,
	           const std::filesystem::path& sourcePath,
	           const std::filesystem::path& contentRoot,
	           const std::filesystem::path& relativeOutputDir,
	           const std::string&           meshStem,
	           NameReserver&                names)
		: m_data(data), m_source(sourcePath), m_contentRoot(contentRoot),
		  m_outDir(relativeOutputDir), m_meshStem(meshStem), m_names(names) {}

	// `srgb` marks the asset as colour data (base colour / emissive) rather than
	// data (normal, ORM). `explicitPath` pins the output onto a file that already
	// exists — a re-import of a texture the user renamed; it applies to the first
	// request for this image only, which is why it is passed per call rather than
	// held here.
	std::string get(const cgltf_texture_view& view, bool srgb, const std::string& explicitPath = {})
	{
		if (!view.texture || !view.texture->image)
			return {};
		const cgltf_image* img = view.texture->image;

		const auto it = m_done.find(img);
		if (it != m_done.end())
			return it->second;

		const std::string path = importImage(*img, srgb, explicitPath);
		m_done.emplace(img, path);   // cache failures too: retrying logs the same error N times
		return path;
	}

private:
	std::string importImage(const cgltf_image& img, bool srgb, const std::string& explicitPath)
	{
		TextureImporter::ImportSettings settings;
		settings.srgb = srgb;

		// External file referenced relative to the glTF. Its asset is named after the
		// IMAGE file, not the mesh, so two meshes importing the same texture land on
		// one asset instead of two copies under two mesh-derived names.
		if (img.uri && std::strncmp(img.uri, "data:", 5) != 0)
		{
			// cgltf_decode_uri works in place and only ever shrinks the string (%XX →
			// one byte), so a std::string buffer is safe — the fixed 1024-byte array
			// this used to use truncated any longer path into a file that is not there.
			std::string uri(img.uri);
			cgltf_decode_uri(uri.data());
			uri.resize(std::strlen(uri.c_str()));

			const std::filesystem::path texFile = m_source.parent_path() / uri;
			std::error_code ec;
			if (!std::filesystem::is_regular_file(texFile, ec))
			{
				logWarn(m_source.filename().string() + ": image '" + uri
				        + "' is not next to the glTF — texture skipped");
				return {};
			}
			// The output name is reserved here rather than left to TextureImporter's
			// sourcePath.stem(): that is flat, so two images called basecolor.png in
			// different sub-folders resolved to ONE asset and the second silently
			// replaced the first, leaving both materials sampling the same pixels.
			std::string target = explicitPath;
			if (target.empty())
				target = toAssetPath(m_outDir / (m_names.claim(texFile.stem().string(),
				                                               HE::AssetType::Texture) + ".hasset"));
			auto tex = TextureImporter::import(texFile, m_contentRoot, m_outDir, settings,
			                                   OutputTargets{ target, {}, {} });
			return tex ? tex->path : std::string{};
		}

		// Embedded: either a bufferView into the .glb's binary chunk, or a base64
		// data: URI. cgltf_load_buffers resolves data-URI BUFFERS but leaves data-URI
		// IMAGES encoded, so that case is decoded here rather than dropped (which is
		// what the previous importer did — every .gltf with inlined images imported
		// untextured, with no error).
		std::vector<uint8_t> owned;
		const uint8_t* bytes = nullptr;
		size_t         size  = 0;
		if (img.buffer_view && img.buffer_view->buffer && img.buffer_view->buffer->data)
		{
			bytes = static_cast<const uint8_t*>(img.buffer_view->buffer->data) + img.buffer_view->offset;
			size  = img.buffer_view->size;
		}
		else if (img.uri)
		{
			const char* comma = std::strchr(img.uri, ',');
			// Only base64 payloads: cgltf_load_buffer_base64 is a base64 decoder, and
			// handing it a percent-encoded or plain-text data URI decodes garbage.
			if (!comma || std::strstr(img.uri, ";base64") == nullptr || comma < img.uri + 7)
				return {};

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
				return {};

			void*         raw = nullptr;
			cgltf_options opt{};   // null alloc/free funcs → cgltf's malloc/free defaults
			if (cgltf_load_buffer_base64(&opt, decoded, b64, &raw) != cgltf_result_success || !raw)
			{
				logWarn(m_source.filename().string() + ": image "
				        + std::to_string(cgltf_image_index(m_data, &img))
				        + " has an undecodable base64 data URI");
				return {};
			}
			owned.assign(static_cast<uint8_t*>(raw), static_cast<uint8_t*>(raw) + decoded);
			std::free(raw);
			bytes = owned.data();
			size  = owned.size();
		}
		if (!bytes || size == 0)
			return {};

		auto tex = TextureImporter::decodeFromMemory(bytes, size, settings);
		if (!tex)
		{
			logWarn(m_source.filename().string() + ": embedded image "
			        + std::to_string(cgltf_image_index(m_data, &img)) + " failed to decode");
			return {};
		}

		// An embedded image has no file name to inherit. Its own `name` is used when
		// the exporter wrote one, else the image INDEX — the "<meshStem>_basecolor"
		// this used to be collided the moment a glTF embedded more than one image,
		// and every texture after the first overwrote the same asset.
		std::string stem = sanitizeStem(img.name);
		if (stem.empty())
			stem = m_meshStem + "_img" + std::to_string(cgltf_image_index(m_data, &img));

		const ResolvedOutput out =
			resolveOutput(explicitPath, m_outDir,
			              explicitPath.empty() ? m_names.claim(stem, HE::AssetType::Texture) : stem);
		tex->type = HE::AssetType::Texture;
		tex->name = out.name;
		tex->path = out.path;
		// No source file: the pixels live inside the glTF, so writeAsset keeps
		// whatever source the asset already recorded rather than pointing a
		// re-import at an image file that does not exist.
		if (!writeAsset(*tex, m_contentRoot))
			return {};
		return tex->path;
	}

	const cgltf_data*                          m_data;
	std::filesystem::path                      m_source;
	std::filesystem::path                      m_contentRoot;
	std::filesystem::path                      m_outDir;
	std::string                                m_meshStem;
	NameReserver&                              m_names;
	std::map<const cgltf_image*, std::string>  m_done;
};

// ─── Graph construction ──────────────────────────────────────────────────────

// One resolved texture channel: the asset it samples plus the UV transform it
// samples with (KHR_texture_transform, already converted into the engine's
// flipped-V space by texSlot()).
struct TexSlot
{
	std::string path;                       // empty = the material does not use this channel
	float       tiling[2] = { 1.0f, 1.0f }; // UV node p[0..1]
	float       offset[2] = { 0.0f, 0.0f }; // UV node p[2..3]

	bool empty() const { return path.empty(); }

	// Same asset AND same UV transform — i.e. the two channels can share one
	// Texture Sample node. Comparing only the path would sample the second channel
	// at the first one's tiling.
	bool sameSampling(const TexSlot& o) const
	{
		return path == o.path
		    && tiling[0] == o.tiling[0] && tiling[1] == o.tiling[1]
		    && offset[0] == o.offset[0] && offset[1] == o.offset[1];
	}
};

// The five texture channels one glTF material can carry.
struct MaterialTextures
{
	TexSlot baseColor;
	TexSlot metallicRoughness;
	TexSlot normal;
	TexSlot occlusion;
	TexSlot emissive;
	float   normalScale     = 1.0f;   // normal_texture.scale
	float   occlusionScale  = 1.0f;   // occlusion_texture.scale == "strength"
};

// Resolves one texture view into a TexSlot, converting KHR_texture_transform into
// the UV node's tiling/offset.
//
// The conversion is not the identity, because the two spaces disagree about V. The
// extension transforms in glTF UV space (v down): v_gltf' = v_gltf * sy + oy. The
// engine stores w = 1 - v_gltf and samples a vertically flipped image, so the
// coordinate the UV node must produce is t = 1 - v_gltf' = (1 - sy - oy) + sy * w.
// U is unaffected. Taking the extension's numbers verbatim would slide and mirror
// every transformed texture along V.
TexSlot texSlot(std::string path, const cgltf_texture_view& view, const std::string& label)
{
	TexSlot slot;
	slot.path = std::move(path);
	if (slot.empty() || !view.has_transform)
		return slot;

	if (view.transform.rotation != 0.0f)
		// The UV node has tiling and offset, no rotation. Silently ignoring it would
		// look like a mis-authored texture rather than a missing feature.
		logWarn(label + ": KHR_texture_transform rotation is not supported — ignored");

	slot.tiling[0] = view.transform.scale[0];
	slot.tiling[1] = view.transform.scale[1];
	slot.offset[0] = view.transform.offset[0];
	slot.offset[1] = 1.0f - view.transform.scale[1] - view.transform.offset[1];
	return slot;
}

// Lays the generated nodes out in readable columns so the graph is workable when
// the user opens the imported material in the Material Editor — codegen ignores
// x/y entirely, but a pile of nodes stacked at the origin is not something anyone
// can edit.
struct GraphBuilder
{
	HE::MaterialGraph& g;
	int                output;
	float              uvY   = 0.0f;   // next free row in the UV column
	float              texY  = 0.0f;   // next free row in the texture column
	float              mathY = 0.0f;

	static constexpr float kUvX   = -1000.0f;
	static constexpr float kTexX  = -760.0f;
	static constexpr float kSplitX = -520.0f;
	static constexpr float kMathX = -260.0f;
	static constexpr float kRow   = 150.0f;

	// UV nodes, deduplicated by their tiling/offset: an untransformed material ends
	// up with exactly one, shared by every sampler.
	std::vector<std::pair<std::array<float, 4>, int>> uvNodes;

	// The UV node a slot samples through. EVERY sampler gets one wired to its UV
	// input, including untransformed ones: a Texture Sample whose UV pin is
	// unconnected falls back to the pin's numeric DEFAULT, vec2(0) — codegen emits
	// `texture(heTexP0, vec2(0.0))`, so the whole surface reads one texel. (Only
	// Normal Map falls back to vUV, via uvInput().) The node doubles as the place a
	// user adjusts tiling afterwards.
	int uvNodeFor(const TexSlot& slot)
	{
		const std::array<float, 4> key{ slot.tiling[0], slot.tiling[1],
		                                slot.offset[0], slot.offset[1] };
		for (const auto& [k, id] : uvNodes)
			if (k == key) return id;

		const int n = g.addNode(HE::MatNodeType::UV, kUvX, uvY);
		uvY += kRow;
		HE::MatGraphNode* node = g.findNode(n);
		for (int k = 0; k < 4; ++k) node->p[k] = key[k];
		uvNodes.emplace_back(key, n);
		return n;
	}

	int addTextureSample(const TexSlot& slot)
	{
		const int n = g.addNode(HE::MatNodeType::TextureSample, kTexX, texY);
		g.findNode(n)->s = slot.path;
		g.connect(uvNodeFor(slot), 0, n, 0);
		texY += kRow;
		return n;
	}

	int addSplit(int fromNode, int fromPin)
	{
		const int n = g.addNode(HE::MatNodeType::SplitRGBA, kSplitX, texY - kRow);
		g.connect(fromNode, fromPin, n, 0);
		return n;
	}

	int addConstFloat(float v)
	{
		const int n = g.addNode(HE::MatNodeType::ConstFloat, kMathX, mathY);
		g.findNode(n)->p[0] = v;
		mathY += kRow * 0.5f;
		return n;
	}

	int addConstColor(const float rgb[3])
	{
		const int n = g.addNode(HE::MatNodeType::ConstColor, kMathX, mathY);
		for (int k = 0; k < 3; ++k) g.findNode(n)->p[k] = rgb[k];
		mathY += kRow * 0.5f;
		return n;
	}

	// (srcNode, srcPin) * (factorNode, 0) → the new Multiply node.
	int addMultiply(int srcNode, int srcPin, int factorNode)
	{
		const int n = g.addNode(HE::MatNodeType::Multiply, kMathX + 180.0f, mathY);
		g.connect(srcNode, srcPin, n, 0);
		g.connect(factorNode, 0, n, 1);
		mathY += kRow * 0.5f;
		return n;
	}

	void toOutput(int srcNode, int srcPin, int outputPin)
	{
		g.connect(srcNode, srcPin, output, outputPin);
	}
};

bool isOne(const float rgb[3])
{
	return rgb[0] == 1.0f && rgb[1] == 1.0f && rgb[2] == 1.0f;
}

// Builds the PBR graph for one glTF material. Everything the glTF states as a
// FACTOR is folded in as a graph constant rather than exposed as a parameter: an
// imported material should look like its source out of the box, and a Param node
// per factor would put five sliders on every imported asset.
HE::MaterialGraph buildGraph(const cgltf_material& m, const MaterialTextures& tex)
{
	HE::MaterialGraph g;
	GraphBuilder      b{ g, g.addNode(HE::MatNodeType::Output, 0.0f, 0.0f) };

	const cgltf_pbr_metallic_roughness& pbr = m.pbr_metallic_roughness;
	// A material with no metallic-roughness block (KHR_materials_pbrSpecularGlossiness
	// only, or a bare `{}`) leaves cgltf's factors at their glTF defaults, which is
	// exactly what the spec says to fall back to — so no special case is needed.
	const float baseRGB[3] = { pbr.base_color_factor[0], pbr.base_color_factor[1],
	                           pbr.base_color_factor[2] };
	const float baseAlpha  = pbr.base_color_factor[3];

	// ── Base colour (+ the base-colour texture's alpha, reused below) ──────────
	int baseTexNode = 0;
	if (!tex.baseColor.empty())
	{
		baseTexNode = b.addTextureSample(tex.baseColor);
		if (isOne(baseRGB))
			b.toOutput(baseTexNode, 0, HE::kMatOutputBaseColorPin);
		else
			b.toOutput(b.addMultiply(baseTexNode, 0, b.addConstColor(baseRGB)), 0,
			           HE::kMatOutputBaseColorPin);
	}
	else
	{
		b.toOutput(b.addConstColor(baseRGB), 0, HE::kMatOutputBaseColorPin);
	}

	// ── Metallic / roughness (glTF: B = metallic, G = roughness of one texture) ─
	// Both pins are wired unconditionally even when there is no texture, because
	// glTF's factor defaults (metallic 1, roughness 1) are NOT the Output node's pin
	// defaults (0 and 0.5) — leaving them unconnected would silently re-shade every
	// imported material.
	int mrSplit = 0;
	if (!tex.metallicRoughness.empty())
		mrSplit = b.addSplit(b.addTextureSample(tex.metallicRoughness), 0);

	const auto wireScalar = [&](int splitNode, int splitPin, float factor, int outPin)
	{
		if (splitNode == 0)
		{
			b.toOutput(b.addConstFloat(factor), 0, outPin);
			return;
		}
		if (factor == 1.0f)
			b.toOutput(splitNode, splitPin, outPin);
		else
			b.toOutput(b.addMultiply(splitNode, splitPin, b.addConstFloat(factor)), 0, outPin);
	};
	wireScalar(mrSplit, 2 /*B*/, pbr.metallic_factor,  HE::kMatOutputMetallicPin);
	wireScalar(mrSplit, 1 /*G*/, pbr.roughness_factor, HE::kMatOutputRoughnessPin);

	// ── Specular (KHR_materials_specular) ─────────────────────────────────────
	// The two conventions line up exactly, so this is a conversion and not a
	// judgement call:
	//     engine   F0 = 0.08 * pin        (MaterialShaderLibrary's heLitP; Unreal's rule)
	//     glTF     F0 = 0.04 * specularFactor
	//     pin = 0.5 * specularFactor  ⇒  0.08 * 0.5 * s = 0.04 * s  for every s,
	// and glTF's default s = 1 lands on the pin's own default of 0.5.
	// Unreal writes this for a foliage material whose Specular the artist pulled
	// down (0.05 on bark, 0.03 on leaves); left unwired the surface would sit at the
	// dielectric 0.04 and read as wet plastic.
	//
	// The pin is left alone when the extension is absent — cgltf defaults
	// specular_factor to 1.0, which maps back onto the pin default anyway, so
	// writing it would only add a node that changes nothing.
	//
	// Only the scalar factor is imported. specularColorFactor/specularTexture would
	// need a vec3 pin, and pin 2 is a float — widening it is an on-disk graph format
	// change (kMatGraphVersion + a remap), not an importer one.
	if (m.has_specular)
		b.toOutput(b.addConstFloat(0.5f * m.specular.specular_factor), 0,
		           HE::kMatOutputSpecularPin);

	// ── Normal map ────────────────────────────────────────────────────────────
	// No vertex tangents needed: the Normal Map node builds a cotangent frame from
	// screen-space derivatives (Mikkelsen). Its frame follows the STORED UV, whose V
	// the mesh importer already flipped to the engine's bottom-left origin — which
	// puts the bitangent along "up in the image", the direction an OpenGL-convention
	// (+Y up) glTF normal map's green channel means. So no green flip here; see
	// tests/test_gltf_material_import.cpp, which pins that down.
	if (!tex.normal.empty())
	{
		const int n = g.addNode(HE::MatNodeType::NormalMapSample, GraphBuilder::kTexX, b.texY);
		b.texY += GraphBuilder::kRow;
		g.findNode(n)->s    = tex.normal.path;
		g.findNode(n)->p[0] = tex.normalScale > 0.0f ? tex.normalScale : 1.0f;
		// Normal Map falls back to vUV on its own, but it is wired anyway: a
		// transformed normal map has to tile with the channels beside it, and an
		// explicit node is where the user adjusts that afterwards.
		g.connect(b.uvNodeFor(tex.normal), 0, n, 0);
		b.toOutput(n, 0, HE::kMatOutputNormalPin);
	}

	// ── Ambient occlusion (glTF: R channel; Unreal packs it into the ORM map) ──
	if (!tex.occlusion.empty())
	{
		// Same TextureSample node when the ORM map IS the metallic-roughness map,
		// sampled the same way: one sampler, one upload. A shared image with
		// DIFFERENT texture transforms needs its own node, or occlusion would be
		// read at metallic-roughness's tiling — the extra node is free either way,
		// since the graph deduplicates its texture SLOTS by path.
		const int split = (tex.occlusion.sameSampling(tex.metallicRoughness) && mrSplit != 0)
			? mrSplit
			: b.addSplit(b.addTextureSample(tex.occlusion), 0);
		if (tex.occlusionScale >= 1.0f)
			b.toOutput(split, 0 /*R*/, HE::kMatOutputAOPin);
		else
		{
			// glTF: ao = 1 + strength * (sampled - 1), i.e. lerp(1, sampled, strength).
			const int lerp = g.addNode(HE::MatNodeType::Lerp, GraphBuilder::kMathX, b.mathY);
			b.mathY += GraphBuilder::kRow * 0.5f;
			const float one[3] = { 1.0f, 1.0f, 1.0f };
			g.connect(b.addConstColor(one), 0, lerp, 0);
			g.connect(split, 0, lerp, 1);
			g.connect(b.addConstFloat(tex.occlusionScale), 0, lerp, 2);
			b.toOutput(lerp, 0, HE::kMatOutputAOPin);
		}
	}

	// ── Emissive ──────────────────────────────────────────────────────────────
	// KHR_materials_emissive_strength scales the factor; folding it in here is what
	// keeps a glowing Unreal material glowing instead of clamping at the factor's 0..1.
	const float strength = m.has_emissive_strength ? m.emissive_strength.emissive_strength : 1.0f;
	const float emisRGB[3] = { m.emissive_factor[0] * strength,
	                           m.emissive_factor[1] * strength,
	                           m.emissive_factor[2] * strength };
	if (!tex.emissive.empty())
	{
		const int t = b.addTextureSample(tex.emissive);
		if (isOne(emisRGB))
			b.toOutput(t, 0, HE::kMatOutputEmissivePin);
		else
			b.toOutput(b.addMultiply(t, 0, b.addConstColor(emisRGB)), 0, HE::kMatOutputEmissivePin);
	}
	else if (emisRGB[0] != 0.0f || emisRGB[1] != 0.0f || emisRGB[2] != 0.0f)
	{
		b.toOutput(b.addConstColor(emisRGB), 0, HE::kMatOutputEmissivePin);
	}

	// ── Blend mode + opacity ──────────────────────────────────────────────────
	// The Opacity pin only means anything on Masked (mask) and Translucent (alpha);
	// on Opaque, codegen forces alpha to 1 and never reads the pin, so wiring it
	// there would just be a dead branch in every imported material.
	// Scoped, and taken AFTER the last addNode above: MaterialGraph::nodes is a
	// vector, so every addNode can reallocate and dangle a node pointer held across
	// it. The opacity wiring below adds more nodes — hence the block.
	{
		HE::MatGraphNode* out = g.findNode(b.output);
		out->p[0] = 1.0f;  // lit
		switch (m.alpha_mode)
		{
		case cgltf_alpha_mode_mask:  out->p[1] = static_cast<float>(HE::MatBlendMode::Masked);      break;
		case cgltf_alpha_mode_blend: out->p[1] = static_cast<float>(HE::MatBlendMode::Translucent); break;
		default:                     out->p[1] = static_cast<float>(HE::MatBlendMode::Opaque);      break;
		}
		out->p[2] = m.alpha_cutoff > 0.0f ? m.alpha_cutoff : 0.5f;
	}

	if (m.alpha_mode != cgltf_alpha_mode_opaque)
	{
		if (baseTexNode != 0)
		{
			if (baseAlpha == 1.0f)
				b.toOutput(baseTexNode, 1 /*A*/, HE::kMatOutputOpacityPin);
			else
				b.toOutput(b.addMultiply(baseTexNode, 1, b.addConstFloat(baseAlpha)), 0,
				           HE::kMatOutputOpacityPin);
		}
		else
			b.toOutput(b.addConstFloat(baseAlpha), 0, HE::kMatOutputOpacityPin);
	}

	return g;
}

// ─── One material ────────────────────────────────────────────────────────────

// Resolves the material's five texture views to asset paths, importing each image
// once. Enforces the graph's texture-slot budget HERE rather than letting codegen
// discover it: HE::textureSampler allocates slots in the order the Output pins are
// emitted (base, metallic/roughness, emissive, AO, and only THEN normal), so a
// material over budget would lose its NORMAL MAP — silently, by falling back to the
// mesh texture. Occlusion is the cheapest channel to lose, so it is dropped first.
MaterialTextures resolveTextures(const cgltf_material& m, ImageCache& images,
                                 const std::string& materialLabel,
                                 const std::string& baseColorOverride)
{
	const cgltf_pbr_metallic_roughness& pbr = m.pbr_metallic_roughness;

	MaterialTextures tex;
	tex.baseColor = texSlot(images.get(pbr.base_color_texture, true, baseColorOverride),
	                        pbr.base_color_texture, materialLabel);
	tex.metallicRoughness = texSlot(images.get(pbr.metallic_roughness_texture, false),
	                                pbr.metallic_roughness_texture, materialLabel);
	tex.normal    = texSlot(images.get(m.normal_texture,    false), m.normal_texture,    materialLabel);
	tex.occlusion = texSlot(images.get(m.occlusion_texture, false), m.occlusion_texture, materialLabel);
	tex.emissive  = texSlot(images.get(m.emissive_texture,  true),  m.emissive_texture,  materialLabel);
	tex.normalScale       = m.normal_texture.scale;
	tex.occlusionScale    = m.occlusion_texture.scale;

	// A channel the glTF DECLARES but that resolved to nothing — the image file is
	// missing next to the .gltf, undecodable, or its .hasset could not be written.
	// Each of those already logs its own cause, but none of them says which MATERIAL
	// just lost a channel, and the graph below simply omits the pin: the material
	// still loads, still renders, and is quietly missing its normal map. Naming it
	// here is what turns that into something a reader can act on.
	const auto checkResolved = [&](const cgltf_texture_view& v, const TexSlot& slot,
	                               const char* what)
	{
		if (v.texture && slot.empty())
			logWarn(materialLabel + ": its " + what + " texture could not be imported —"
			        " the material is written WITHOUT that channel");
	};
	checkResolved(pbr.base_color_texture,         tex.baseColor,         "base colour");
	checkResolved(pbr.metallic_roughness_texture, tex.metallicRoughness, "metallic-roughness");
	checkResolved(m.normal_texture,               tex.normal,            "normal");
	checkResolved(m.occlusion_texture,            tex.occlusion,         "occlusion");
	checkResolved(m.emissive_texture,             tex.emissive,          "emissive");

	// glTF multiplies the emissive texture BY emissiveFactor, whose default is
	// (0,0,0) — so a material that names an emissive texture without a factor emits
	// pure black by the spec. Wiring it anyway would spend one of the four graph
	// texture slots on a channel that evaluates to zero, and the budget below might
	// then drop a channel that does something. The image is still imported above;
	// only the graph link is skipped, so the user can wire it up if they meant it.
	const float emissiveStrength =
		m.has_emissive_strength ? m.emissive_strength.emissive_strength : 1.0f;
	const bool emissiveIsBlack = (m.emissive_factor[0] * emissiveStrength == 0.0f)
	                          && (m.emissive_factor[1] * emissiveStrength == 0.0f)
	                          && (m.emissive_factor[2] * emissiveStrength == 0.0f);
	if (emissiveIsBlack && !tex.emissive.empty())
	{
		logWarn(materialLabel + ": emissive texture ignored — emissiveFactor is 0,"
		        " so glTF says this material emits nothing");
		tex.emissive.path.clear();
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
			logWarn(materialLabel + ": " + what + " samples TEXCOORD_" + std::to_string(viewUvSet(v))
			        + " but this material is imported on TEXCOORD_" + std::to_string(matSet)
			        + " — a mesh carries one UV stream, so that channel will be wrong");
	};
	checkUV(pbr.base_color_texture,         "base colour");
	checkUV(pbr.metallic_roughness_texture, "metallic-roughness");
	checkUV(m.normal_texture,               "normal");
	checkUV(m.occlusion_texture,            "occlusion");
	checkUV(m.emissive_texture,             "emissive");

	// Distinct paths = distinct graph texture slots (the graph dedupes by path).
	std::vector<std::string> distinct;
	for (const TexSlot* p : { &tex.baseColor, &tex.metallicRoughness, &tex.normal,
	                          &tex.occlusion, &tex.emissive })
		if (!p->empty() && std::find(distinct.begin(), distinct.end(), p->path) == distinct.end())
			distinct.push_back(p->path);

	if (static_cast<int>(distinct.size()) > HE::kMatMaxGraphTextures && !tex.occlusion.empty())
	{
		logWarn(materialLabel + ": " + std::to_string(distinct.size()) + " textures exceed the "
		        + std::to_string(HE::kMatMaxGraphTextures)
		        + " a material graph can sample — occlusion (" + tex.occlusion.path + ") dropped");
		tex.occlusion.path.clear();
	}
	return tex;
}

// Writes one MaterialAsset for `m`. Returns its asset path, or empty on failure.
std::string importMaterial(const cgltf_material&        m,
                           ImageCache&                  images,
                           const std::filesystem::path& contentRoot,
                           const std::filesystem::path& relativeOutputDir,
                           const std::string&           derivedStem,
                           const std::string&           explicitPath,
                           const std::string&           baseColorOverride,
                           bool                         isPrimary)
{
	const ResolvedOutput out = resolveOutput(explicitPath, relativeOutputDir, derivedStem);
	const MaterialTextures tex = resolveTextures(m, images, out.name, baseColorOverride);

	// A material that is already on disk is left exactly as it is. Everything below
	// builds a BRAND NEW MaterialAsset, and writing that over one the artist has
	// since opened in the Material Editor destroys its content while looking
	// perfectly healthy: writeAsset keeps the file's UUID so nothing dangles, and the
	// editor does not reload an already resident MaterialAsset, so the viewport keeps
	// rendering the authored graph from memory and the loss first surfaces on the
	// NEXT project load — node graph, generated shaders, param values and their
	// name/group/tooltip tables, a material INSTANCE's parentMaterialPath, blend mode,
	// WPO body and GI approximation, all gone.
	// This is why `explicitPath` (a re-import redirect) is honoured for the FILE but
	// not for its contents. A first import is unaffected (the file does not exist
	// yet), and a sidecar the user DELETED is regenerated, the same way
	// importOutputsUpToDate treats a missing sidecar. The textures above are
	// refreshed either way: they are derived data with nothing authorable in them.
	// Two ways to reach "leave it alone", and the second one is why a re-import of a
	// multi-material glTF stopped destroying work:
	//   • explicitPath set — a Reimport, redirected onto the sidecar the mesh names.
	//   • not the bound material — nothing measures its mtime (meshSidecarAssets
	//     follows only the MREF material), so there is no rebuild-loop reason to
	//     rewrite it and rewriting it is pure downside. Before this, EVERY
	//     non-primary material was regenerated on every import, because explicitPath
	//     is empty for all of them: a leaf material the artist had opened and given a
	//     graph came back flat and generated, and the loss first showed on the next
	//     project load.
	// The BOUND material on a plain (non-Reimport) import is still rewritten, because
	// its mtime is exactly what importOutputsUpToDate measures — never refreshing it
	// makes the asset compiler re-import that mesh on every run, forever.
	// Deleting a material still regenerates it, the same way a deleted sidecar does.
	std::error_code ec;
	if ((!explicitPath.empty() || !isPrimary)
	    && std::filesystem::is_regular_file(contentRoot / out.path, ec))
	{
		if (!isPrimary)
			logWarn(out.name + " already exists and was left untouched — delete it to"
			        " regenerate it from the glTF");
		return out.path;
	}

	const HE::MaterialGraph graph = buildGraph(m, tex);
	// The graph is the source of truth, but the baked GLSL is generated HERE rather
	// than left to ContentManager's load-time regeneration: the packer reads .hasset
	// chunks raw (HpakWriter), so a material whose MTRL carries only the graph would
	// ship with no shader at all.
	const HE::MatShaderGen gen = HE::generateFragment(graph);

	MaterialAsset mat;
	mat.type = HE::AssetType::Material;
	mat.name = out.name;
	mat.path = out.path;
	// shaderPath is a dead field for graph materials — no renderer reads it and the
	// packer resolves it to a UUID, which the old "builtin/unlit" placeholder never
	// had. Empty is the value that path explicitly handles.
	mat.shaderPath = {};
	// The legacy single-texture slot (heTex0) every backend still uses for meshes
	// without a graph shader, and what the Inspector shows as the material's texture.
	if (!tex.baseColor.empty())
		mat.texturePaths.push_back(tex.baseColor.path);

	mat.nodeGraphJson        = HE::materialGraphToJson(graph);
	mat.customShaderFragGlsl = gen.glsl;
	mat.customShaderGBufGlsl = gen.glslGBuffer;
	mat.customShaderVertGlsl = gen.vertexBody;
	mat.blendMode            = gen.blendMode;
	mat.graphTexturePaths    = gen.textures;

	// The scalar PBR fields too, not just the graph: they are what the Inspector
	// edits, what PropertyAnimClip animates, and what a consumer bypassing the graph
	// shader falls back to — a material whose graph says metal and whose scalars say
	// dielectric is a bug waiting for the first such consumer.
	const cgltf_pbr_metallic_roughness& pbr = m.pbr_metallic_roughness;
	for (int k = 0; k < 3; ++k) mat.baseColor[k] = pbr.base_color_factor[k];
	mat.metallic     = pbr.metallic_factor;
	mat.roughness    = pbr.roughness_factor;
	mat.opacity      = pbr.base_color_factor[3];
	mat.doubleSided  = m.double_sided != 0;

	if (!writeAsset(mat, contentRoot))
		return {};
	return mat.path;
}

// The material of the first primitive that carries geometry, in the SAME order the
// mesh importers bake it (mesh-bearing nodes first, bare meshes only when the glTF
// has no node hierarchy). That makes "the material the mesh got bound to" the one
// belonging to the first triangles in the vertex buffer rather than an arbitrary
// index into data->materials.
const cgltf_material* firstPrimitiveMaterial(const cgltf_data* data)
{
	for (cgltf_size n = 0; n < data->nodes_count; ++n)
	{
		const cgltf_node& node = data->nodes[n];
		if (!node.mesh) continue;
		for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p)
			if (node.mesh->primitives[p].material)
				return node.mesh->primitives[p].material;
	}
	for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
		for (cgltf_size p = 0; p < data->meshes[mi].primitives_count; ++p)
			if (data->meshes[mi].primitives[p].material)
				return data->meshes[mi].primitives[p].material;
	return nullptr;
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

	// Seeded with the names the MESH itself will take. The mesh is written AFTER the
	// materials, so without this a material named like the source file is created and
	// then destroyed by the mesh landing on top of it — leaving the mesh's own MREF
	// pointing at the mesh.
	NameReserver names(contentRoot, relativeOutputDir);
	names.reserve(meshStem);
	names.reserve(meshStem + "_skeletal");          // SkeletalMeshImporter's output
	if (!outputs.asset.empty())
		names.reserve(std::filesystem::path(outputs.asset).stem().string());

	ImageCache images(data, sourcePath, contentRoot, relativeOutputDir, meshStem, names);

	const cgltf_material* primary = firstPrimitiveMaterial(data);
	// Primitives without a material still get the file's first material bound rather
	// than nothing: a glTF that declares materials but leaves a primitive unassigned
	// would otherwise import a mesh with no material reference at all, which blanks
	// the MREF every scene resolves through.
	if (!primary) primary = &data->materials[0];

	// A re-import redirects the mesh's ONE recorded material sidecar (and its base
	// colour texture) onto the files that already exist, so the asset every scene
	// references is the one that gets refreshed. Only the bound material can be
	// redirected — the others are named after their glTF material, which is stable
	// across re-imports and needs no redirect.
	const bool singleMaterial = data->materials_count == 1;

	result.paths.resize(data->materials_count);
	std::vector<std::string> usedStems;
	for (cgltf_size i = 0; i < data->materials_count; ++i)
	{
		const cgltf_material& m = data->materials[i];
		const bool isPrimary = (&m == primary);

		// Named after the glTF material so it survives a mesh rename and so two
		// meshes sharing a material converge on one asset. Unnamed materials fall
		// back to the historical "<meshStem>_mat" for the single-material case —
		// the name every existing import already wrote — and to an indexed variant
		// beyond that.
		std::string stem = sanitizeStem(m.name);
		if (stem.empty())
			stem = singleMaterial ? meshStem + "_mat"
			                      : meshStem + "_mat" + std::to_string(i);
		// Backward compatibility: before materials were named after the glTF, a
		// single-material import always wrote "<meshStem>_mat". A project that
		// already holds that file keeps it, so re-importing an asset imported by an
		// older build refreshes the material the mesh already references instead of
		// writing a second one beside it and orphaning the artist's edits.
		if (singleMaterial && isPrimary && outputs.material.empty())
		{
			std::error_code legacyEc;
			if (std::filesystem::is_regular_file(
			        contentRoot / relativeOutputDir / (meshStem + "_mat.hasset"), legacyEc))
				stem = meshStem + "_mat";
		}
		// Two glTF materials may carry the SAME name — a DCC tool exporting
		// "Material" twice, or two distinct names that sanitise to one. They would
		// otherwise resolve to one asset path, and the second import would overwrite
		// the first while both mesh slots point at whichever survived.
		if (std::find(usedStems.begin(), usedStems.end(), stem) != usedStems.end())
		{
			const std::string base = stem;
			for (int suffix = 2; ; ++suffix)
			{
				stem = base + "_" + std::to_string(suffix);
				if (std::find(usedStems.begin(), usedStems.end(), stem) == usedStems.end())
					break;
			}
			logWarn(sourcePath.filename().string() + ": two materials named '" + base
			        + "' — the second is written as '" + stem + "'");
		}
		usedStems.push_back(stem);
		stem = names.claim(stem, HE::AssetType::Material);

		result.paths[i] = importMaterial(
			m, images, contentRoot, relativeOutputDir, stem,
			/*explicitPath=*/ (isPrimary && singleMaterial) ? outputs.material : std::string{},
			/*baseColorOverride=*/ (isPrimary && singleMaterial) ? outputs.texture : std::string{},
			isPrimary);

		if (isPrimary)
			result.primary = result.paths[i];
		else if (!result.paths[i].empty())
			result.unbound.push_back(result.paths[i]);
	}

	if (!result.unbound.empty())
	{
		// The assets are written and complete — but saying "assign them by hand" would
		// be a lie. All primitives are baked into ONE vertex/index buffer with ONE
		// material reference, and an entity carries ONE MaterialComponent, so there is
		// no assignment anywhere in the editor that puts a second material on part of
		// this mesh. Until meshes carry material SECTIONS, the only thing that
		// actually works is re-exporting one mesh per material from the DCC.
		// Naming the materials still matters: it is the difference between a user who
		// knows what happened and one who thinks the import ate them.
		std::string list;
		for (const std::string& p : result.unbound)
			list += (list.empty() ? "" : ", ") + p;
		logWarn(sourcePath.filename().string() + ": glTF has "
		        + std::to_string(data->materials_count)
		        + " materials but a mesh holds only one — '" + result.primary
		        + "' is bound and the geometry of the others is shaded with it. Written but"
		        " UNUSABLE on this mesh: " + list
		        + ". To get them, export one mesh per material from the DCC.");
	}

	return result;
}

} // namespace Importer
