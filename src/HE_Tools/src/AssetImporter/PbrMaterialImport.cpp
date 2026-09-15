// Format-neutral material import: PbrMaterialDesc records → MaterialAssets with a
// real PBR node graph, PbrImage table → TextureAssets. See PbrMaterialImport.h
// for the seam; GltfMaterialImport.cpp and AssimpMaterialImport.cpp fill it.
//
// What a mesh import produces through here:
//   • one MaterialAsset per source material, named after the source material,
//   • one TextureAsset per source IMAGE (an image shared by several materials, or
//     used twice inside one — Unreal packs occlusion+roughness+metallic into a
//     single ORM map — is imported once),
//   • a HE::MaterialGraph on each material wiring base colour, metallic, roughness,
//     normal, occlusion and emissive into the Output node, with the source's
//     factors folded in as constants, plus the baked GLSL that graph generates.
//
// The mesh binds them through its section table (MeshSection, chunk MSEC): the
// importers group the primitives by material and bind `paths[i]` to each section
// (buildMeshSections), so a multi-material file arrives with every material on
// the geometry it was authored on. The mesh-level MREF (`primary`) is section 0's
// material — what the section-unaware draw paths still resolve for the whole mesh.
#include "PbrMaterialImport.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ContentManager/Assets.h"
#include "ContentManager/HAsset.h"
#include "Diagnostics/Logger.h"
#include "MaterialGraph/MaterialGraph.h"
#include "TextureImporter.h"

namespace Importer
{

// ─── Naming ──────────────────────────────────────────────────────────────────

// A source name turned into a file stem. Everything that is not alphanumeric, '_'
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

namespace
{

// Every warning below names the reader that produced the description, so a log
// line can be traced to the file format that was being imported.
struct Log
{
	const char* prefix;
	void warn(const std::string& msg) const
	{
		HE_LOG_WARN(Tool, "%s", (std::string(prefix) + ": " + msg).c_str());
	}
};

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
// in the same flat folder under a stem taken from an untrusted source string.
// Nothing stopped a material called "Wood" from being written over the .hasset a
// "Wood.png" texture had just produced: writeAsset recovers the uuid from whatever
// file is already there, so the material inherited the TEXTURE's identity and every
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
	NameReserver(std::filesystem::path contentRoot, std::filesystem::path relativeOutputDir,
	             const Log& log)
		: m_root(std::move(contentRoot)), m_dir(std::move(relativeOutputDir)), m_log(log) {}

	// Reserve a name up front for an output written by someone else (the mesh).
	void reserve(const std::string& stem) { if (!stem.empty()) m_taken.push_back(stem); }

	std::string claim(const std::string& wanted, HE::AssetType type)
	{
		if (isFree(wanted, type)) { m_taken.push_back(wanted); return wanted; }
		for (int n = 2; n < 1000; ++n)
		{
			const std::string candidate = wanted + "_" + std::to_string(n);
			if (!isFree(candidate, type)) continue;
			m_log.warn("output name '" + wanted + "' is already taken by another asset — '"
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
	Log                      m_log;
	std::vector<std::string> m_taken;
};

// ─── Image import (one TextureAsset per image, imported at most once) ────────

// Imports each image of the table exactly once per source file and hands out its
// asset path. Two materials sharing a texture, or one material using its ORM map
// as both the metallic-roughness AND the occlusion source, must resolve to the
// SAME asset path: the material graph deduplicates its texture slots by path (see
// HE::textureSampler), so anything else burns a second slot out of the four a
// graph has — and would import the same pixels twice.
class ImageCache
{
public:
	ImageCache(const std::vector<PbrImage>& images,
	           const std::filesystem::path& sourcePath,
	           const std::filesystem::path& contentRoot,
	           const std::filesystem::path& relativeOutputDir,
	           const std::string&           meshStem,
	           NameReserver&                names,
	           const Log&                   log)
		: m_images(images), m_source(sourcePath), m_contentRoot(contentRoot),
		  m_outDir(relativeOutputDir), m_meshStem(meshStem), m_names(names), m_log(log) {}

	// `srgb` marks the asset as colour data (base colour / emissive) rather than
	// data (normal, ORM). `explicitPath` pins the output onto a file that already
	// exists — a re-import of a texture the user renamed; it applies to the first
	// request for this image only, which is why it is passed per call rather than
	// held here.
	std::string get(const PbrTextureRef& ref, bool srgb, const std::string& explicitPath = {})
	{
		if (!ref.used() || ref.image >= static_cast<int>(m_images.size()))
			return {};

		const auto it = m_done.find(ref.image);
		if (it != m_done.end())
			return it->second;

		const std::string path = importImage(ref.image, srgb, explicitPath);
		m_done.emplace(ref.image, path);   // cache failures too: retrying logs the same error N times
		return path;
	}

private:
	std::string importImage(int index, bool srgb, const std::string& explicitPath)
	{
		const PbrImage& img = m_images[static_cast<size_t>(index)];
		TextureImporter::ImportSettings settings;
		settings.srgb = srgb;

		if (!img.unresolved.empty())
		{
			m_log.warn(m_source.filename().string() + ": image '" + img.unresolved
			           + "' could not be found or read — texture skipped");
			return {};
		}

		// External file. Its asset is named after the IMAGE file, not the mesh, so
		// two meshes importing the same texture land on one asset instead of two
		// copies under two mesh-derived names.
		if (!img.file.empty())
		{
			std::error_code ec;
			if (!std::filesystem::is_regular_file(img.file, ec))
			{
				m_log.warn(m_source.filename().string() + ": image '" + img.file.string()
				           + "' is not next to the source file — texture skipped");
				return {};
			}
			// The output name is reserved here rather than left to TextureImporter's
			// sourcePath.stem(): that is flat, so two images called basecolor.png in
			// different sub-folders resolved to ONE asset and the second silently
			// replaced the first, leaving both materials sampling the same pixels.
			std::string target = explicitPath;
			if (target.empty())
				target = toAssetPath(m_outDir / (m_names.claim(img.file.stem().string(),
				                                               HE::AssetType::Texture) + ".hasset"));
			auto tex = TextureImporter::import(img.file, m_contentRoot, m_outDir, settings,
			                                   OutputTargets{ target, {}, {} });
			return tex ? tex->path : std::string{};
		}

		// Embedded: compressed bytes stb_image decodes, or raw pixels the reader
		// already laid out the way TextureImporter would have.
		std::unique_ptr<TextureAsset> tex;
		if (!img.encoded.empty())
		{
			tex = TextureImporter::decodeFromMemory(img.encoded.data(), img.encoded.size(), settings);
			if (!tex)
			{
				m_log.warn(m_source.filename().string() + ": embedded image "
				           + std::to_string(index) + " failed to decode");
				return {};
			}
		}
		else if (!img.rgba.empty() && img.width > 0 && img.height > 0
		         && img.rgba.size() == static_cast<size_t>(img.width) * img.height * 4)
		{
			tex = std::make_unique<TextureAsset>();
			tex->width    = img.width;
			tex->height   = img.height;
			tex->channels = 4;
			tex->srgb     = srgb;
			tex->data     = img.rgba;
		}
		else
			return {};

		// An embedded image has no file name to inherit. Its own `name` is used when
		// the exporter wrote one, else the image INDEX — the "<meshStem>_basecolor"
		// this used to be collided the moment a file embedded more than one image,
		// and every texture after the first overwrote the same asset.
		std::string stem = sanitizeStem(img.name.c_str());
		if (stem.empty())
			stem = m_meshStem + "_img" + std::to_string(index);

		const ResolvedOutput out =
			resolveOutput(explicitPath, m_outDir,
			              explicitPath.empty() ? m_names.claim(stem, HE::AssetType::Texture) : stem);
		tex->type = HE::AssetType::Texture;
		tex->name = out.name;
		tex->path = out.path;
		// No source file: the pixels live inside the mesh file, so writeAsset keeps
		// whatever source the asset already recorded rather than pointing a
		// re-import at an image file that does not exist.
		if (!writeAsset(*tex, m_contentRoot))
			return {};
		return tex->path;
	}

	const std::vector<PbrImage>& m_images;
	std::filesystem::path        m_source;
	std::filesystem::path        m_contentRoot;
	std::filesystem::path        m_outDir;
	std::string                  m_meshStem;
	NameReserver&                m_names;
	Log                          m_log;
	std::map<int, std::string>   m_done;
};

// ─── Graph construction ──────────────────────────────────────────────────────

// One resolved texture channel: the asset it samples plus the UV transform it
// samples with, and — for the scalar channels — which colour channel to read.
struct TexSlot
{
	std::string path;                       // empty = the material does not use this channel
	float       tiling[2] = { 1.0f, 1.0f }; // UV node p[0..1]
	float       offset[2] = { 0.0f, 0.0f }; // UV node p[2..3]
	int         channel   = 0;              // SplitRGBA pin for scalar reads

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

// The six texture channels one material can carry, resolved to asset paths.
struct MaterialTextures
{
	TexSlot baseColor;
	TexSlot metallic;
	TexSlot roughness;
	TexSlot normal;
	TexSlot occlusion;
	TexSlot emissive;
};

TexSlot texSlot(std::string path, const PbrTextureRef& ref)
{
	TexSlot slot;
	slot.path = std::move(path);
	if (slot.empty())
		return slot;
	slot.tiling[0] = ref.tiling[0];
	slot.tiling[1] = ref.tiling[1];
	slot.offset[0] = ref.offset[0];
	slot.offset[1] = ref.offset[1];
	slot.channel   = ref.channel;
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
	// Texture Sample nodes and the SplitRGBA hanging off them, deduplicated by
	// sampling (path + UV transform): an ORM map read for metallic, roughness AND
	// occlusion is one sampler, one upload — and one graph texture slot, since the
	// graph dedupes those by path anyway.
	std::vector<std::pair<TexSlot, int>> sampleNodes;
	std::vector<std::pair<int, int>>     splitNodes;   // sample node → its split

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

	int sampleFor(const TexSlot& slot)
	{
		for (const auto& [s, id] : sampleNodes)
			if (s.sameSampling(slot)) return id;

		const int n = g.addNode(HE::MatNodeType::TextureSample, kTexX, texY);
		g.findNode(n)->s = slot.path;
		g.connect(uvNodeFor(slot), 0, n, 0);
		texY += kRow;
		sampleNodes.emplace_back(slot, n);
		return n;
	}

	int splitFor(const TexSlot& slot)
	{
		const int sample = sampleFor(slot);
		for (const auto& [from, id] : splitNodes)
			if (from == sample) return id;

		const int n = g.addNode(HE::MatNodeType::SplitRGBA, kSplitX, texY - kRow);
		g.connect(sample, 0, n, 0);
		splitNodes.emplace_back(sample, n);
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

// Builds the PBR graph for one material. Everything the source states as a
// FACTOR is folded in as a graph constant rather than exposed as a parameter: an
// imported material should look like its source out of the box, and a Param node
// per factor would put five sliders on every imported asset.
HE::MaterialGraph buildGraph(const PbrMaterialDesc& m, const MaterialTextures& tex)
{
	HE::MaterialGraph g;
	GraphBuilder      b{ g, g.addNode(HE::MatNodeType::Output, 0.0f, 0.0f) };

	const float baseRGB[3] = { m.baseColor[0], m.baseColor[1], m.baseColor[2] };
	const float baseAlpha  = m.baseColor[3];

	// ── Base colour (+ the base-colour texture's alpha, reused below) ──────────
	int baseTexNode = 0;
	if (!tex.baseColor.empty())
	{
		baseTexNode = b.sampleFor(tex.baseColor);
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

	// ── Metallic / roughness ──────────────────────────────────────────────────
	// Both pins are wired unconditionally even when there is no texture, because
	// the source's factor defaults (glTF: metallic 1, roughness 1) are NOT the
	// Output node's pin defaults (0 and 0.5) — leaving them unconnected would
	// silently re-shade every imported material. A packed map (glTF's one image,
	// B = metallic, G = roughness) and two separate grey maps (OBJ's map_Pm /
	// map_Pr) both come through here: the split is per SAMPLING, so the packed
	// case still costs one sampler.
	const auto wireScalar = [&](const TexSlot& slot, float factor, int outPin)
	{
		if (slot.empty())
		{
			b.toOutput(b.addConstFloat(factor), 0, outPin);
			return;
		}
		const int split = b.splitFor(slot);
		if (factor == 1.0f)
			b.toOutput(split, slot.channel, outPin);
		else
			b.toOutput(b.addMultiply(split, slot.channel, b.addConstFloat(factor)), 0, outPin);
	};
	wireScalar(tex.metallic,  m.metallic,  HE::kMatOutputMetallicPin);
	wireScalar(tex.roughness, m.roughness, HE::kMatOutputRoughnessPin);

	// ── Specular ──────────────────────────────────────────────────────────────
	// Only when the source states one (glTF's KHR_materials_specular): the pin's
	// own default is the dielectric 0.5, and writing that would only add a node
	// that changes nothing.
	if (m.hasSpecular)
		b.toOutput(b.addConstFloat(m.specular), 0, HE::kMatOutputSpecularPin);

	// ── Normal map ────────────────────────────────────────────────────────────
	// No vertex tangents needed: the Normal Map node builds a cotangent frame from
	// screen-space derivatives (Mikkelsen). Its frame follows the STORED UV, which
	// every mesh importer lays out with the engine's bottom-left origin — which
	// puts the bitangent along "up in the image", the direction an OpenGL-convention
	// (+Y up) normal map's green channel means. So no green flip here; see
	// tests/test_gltf_material_import.cpp, which pins that down.
	if (!tex.normal.empty())
	{
		const int n = g.addNode(HE::MatNodeType::NormalMapSample, GraphBuilder::kTexX, b.texY);
		b.texY += GraphBuilder::kRow;
		g.findNode(n)->s    = tex.normal.path;
		g.findNode(n)->p[0] = m.normalScale > 0.0f ? m.normalScale : 1.0f;
		// Normal Map falls back to vUV on its own, but it is wired anyway: a
		// transformed normal map has to tile with the channels beside it, and an
		// explicit node is where the user adjusts that afterwards.
		g.connect(b.uvNodeFor(tex.normal), 0, n, 0);
		b.toOutput(n, 0, HE::kMatOutputNormalPin);
	}

	// ── Ambient occlusion ─────────────────────────────────────────────────────
	// Same TextureSample node when the ORM map IS the metallic-roughness map,
	// sampled the same way: one sampler, one upload (splitFor dedupes). A shared
	// image with DIFFERENT texture transforms needs its own node, or occlusion
	// would be read at metallic-roughness's tiling — the extra node is free either
	// way, since the graph deduplicates its texture SLOTS by path.
	if (!tex.occlusion.empty())
	{
		const int split = b.splitFor(tex.occlusion);
		if (m.occlusionStrength >= 1.0f)
			b.toOutput(split, tex.occlusion.channel, HE::kMatOutputAOPin);
		else
		{
			// ao = 1 + strength * (sampled - 1), i.e. lerp(1, sampled, strength).
			const int lerp = g.addNode(HE::MatNodeType::Lerp, GraphBuilder::kMathX, b.mathY);
			b.mathY += GraphBuilder::kRow * 0.5f;
			const float one[3] = { 1.0f, 1.0f, 1.0f };
			g.connect(b.addConstColor(one), 0, lerp, 0);
			g.connect(split, tex.occlusion.channel, lerp, 1);
			g.connect(b.addConstFloat(m.occlusionStrength), 0, lerp, 2);
			b.toOutput(lerp, 0, HE::kMatOutputAOPin);
		}
	}

	// ── Emissive ──────────────────────────────────────────────────────────────
	if (!tex.emissive.empty())
	{
		const int t = b.sampleFor(tex.emissive);
		if (isOne(m.emissive))
			b.toOutput(t, 0, HE::kMatOutputEmissivePin);
		else
			b.toOutput(b.addMultiply(t, 0, b.addConstColor(m.emissive)), 0, HE::kMatOutputEmissivePin);
	}
	else if (m.emissive[0] != 0.0f || m.emissive[1] != 0.0f || m.emissive[2] != 0.0f)
	{
		b.toOutput(b.addConstColor(m.emissive), 0, HE::kMatOutputEmissivePin);
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
		switch (m.alphaMode)
		{
		case PbrMaterialDesc::AlphaMode::Mask:  out->p[1] = static_cast<float>(HE::MatBlendMode::Masked);      break;
		case PbrMaterialDesc::AlphaMode::Blend: out->p[1] = static_cast<float>(HE::MatBlendMode::Translucent); break;
		default:                                out->p[1] = static_cast<float>(HE::MatBlendMode::Opaque);      break;
		}
		out->p[2] = m.alphaCutoff > 0.0f ? m.alphaCutoff : 0.5f;
	}

	if (m.alphaMode != PbrMaterialDesc::AlphaMode::Opaque)
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

// Resolves the material's texture references to asset paths, importing each image
// once. Enforces the graph's texture-slot budget HERE rather than letting codegen
// discover it: HE::textureSampler allocates slots in the order the Output pins are
// emitted (base, metallic/roughness, emissive, AO, and only THEN normal), so a
// material over budget would lose its NORMAL MAP — silently, by falling back to the
// mesh texture. Occlusion is the cheapest channel to lose, so it is dropped first;
// emissive second, which only ever comes up with SEPARATE metallic and roughness
// maps (base + normal + metallic + roughness already fill the four).
MaterialTextures resolveTextures(const PbrMaterialDesc& m, ImageCache& images,
                                 const std::string& materialLabel,
                                 const std::string& baseColorOverride, const Log& log)
{
	MaterialTextures tex;
	tex.baseColor = texSlot(images.get(m.baseColorTex, true, baseColorOverride), m.baseColorTex);
	tex.metallic  = texSlot(images.get(m.metallicTex,  false), m.metallicTex);
	tex.roughness = texSlot(images.get(m.roughnessTex, false), m.roughnessTex);
	tex.normal    = texSlot(images.get(m.normalTex,    false), m.normalTex);
	tex.occlusion = texSlot(images.get(m.occlusionTex, false), m.occlusionTex);
	tex.emissive  = texSlot(images.get(m.emissiveTex,  true),  m.emissiveTex);
	for (const PbrMaterialDesc::UnlinkedImage& u : m.unlinkedImages)
	{
		PbrTextureRef ref;
		ref.image = u.image;
		images.get(ref, u.srgb);   // written, deliberately not wired
	}

	// A channel the source DECLARES but that resolved to nothing — the image file
	// is missing next to the source, undecodable, or its .hasset could not be
	// written. Each of those already logs its own cause, but none of them says
	// which MATERIAL just lost a channel, and the graph simply omits the pin: the
	// material still loads, still renders, and is quietly missing its normal map.
	// Naming it here is what turns that into something a reader can act on.
	const auto checkResolved = [&](const PbrTextureRef& ref, const TexSlot& slot, const char* what)
	{
		if (ref.used() && slot.empty())
			log.warn(materialLabel + ": its " + what + " texture could not be imported —"
			         " the material is written WITHOUT that channel");
	};
	checkResolved(m.baseColorTex, tex.baseColor, "base colour");
	checkResolved(m.metallicTex,  tex.metallic,  "metallic");
	checkResolved(m.roughnessTex, tex.roughness, "roughness");
	checkResolved(m.normalTex,    tex.normal,    "normal");
	checkResolved(m.occlusionTex, tex.occlusion, "occlusion");
	checkResolved(m.emissiveTex,  tex.emissive,  "emissive");

	// Distinct paths = distinct graph texture slots (the graph dedupes by path).
	const auto distinctCount = [&]()
	{
		std::vector<std::string> distinct;
		for (const TexSlot* p : { &tex.baseColor, &tex.metallic, &tex.roughness, &tex.normal,
		                          &tex.occlusion, &tex.emissive })
			if (!p->empty() && std::find(distinct.begin(), distinct.end(), p->path) == distinct.end())
				distinct.push_back(p->path);
		return static_cast<int>(distinct.size());
	};
	for (TexSlot* victim : { &tex.occlusion, &tex.emissive })
	{
		const int count = distinctCount();
		if (count <= HE::kMatMaxGraphTextures || victim->empty())
			continue;
		log.warn(materialLabel + ": " + std::to_string(count) + " textures exceed the "
		         + std::to_string(HE::kMatMaxGraphTextures) + " a material graph can sample — "
		         + (victim == &tex.occlusion ? "occlusion" : "emissive")
		         + " (" + victim->path + ") dropped");
		victim->path.clear();
	}
	return tex;
}

// Writes one MaterialAsset for `m`. Returns its asset path, or empty on failure.
std::string importMaterial(const PbrMaterialDesc&       m,
                           ImageCache&                  images,
                           const std::filesystem::path& contentRoot,
                           const std::filesystem::path& relativeOutputDir,
                           const std::string&           derivedStem,
                           const std::string&           explicitPath,
                           const std::string&           baseColorOverride,
                           bool                         isPrimary,
                           const Log&                   log)
{
	const ResolvedOutput out = resolveOutput(explicitPath, relativeOutputDir, derivedStem);
	const MaterialTextures tex = resolveTextures(m, images, out.name, baseColorOverride, log);

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
	// multi-material file stopped destroying work:
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
			log.warn(out.name + " already exists and was left untouched — delete it to"
			         " regenerate it from the source file");
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
	for (int k = 0; k < 3; ++k) mat.baseColor[k] = m.baseColor[k];
	mat.metallic     = m.metallic;
	mat.roughness    = m.roughness;
	mat.opacity      = m.baseColor[3];
	mat.doubleSided  = m.doubleSided;

	if (!writeAsset(mat, contentRoot))
		return {};
	return mat.path;
}

} // namespace

PbrMaterialImport importPbrMaterials(const std::vector<PbrMaterialDesc>& materials,
                                     const std::vector<PbrImage>&        images,
                                     int                                 primaryIndex,
                                     const std::filesystem::path&        sourcePath,
                                     const std::filesystem::path&        contentRoot,
                                     const std::filesystem::path&        relativeOutputDir,
                                     const std::string&                  meshStem,
                                     const OutputTargets&                outputs,
                                     const char*                         logPrefix)
{
	PbrMaterialImport result;
	if (materials.empty())
		return result;
	const Log log{ logPrefix };

	// Seeded with the names the MESH itself will take. The mesh is written AFTER the
	// materials, so without this a material named like the source file is created and
	// then destroyed by the mesh landing on top of it — leaving the mesh's own MREF
	// pointing at the mesh.
	NameReserver names(contentRoot, relativeOutputDir, log);
	names.reserve(meshStem);
	names.reserve(meshStem + "_skeletal");          // SkeletalMeshImporter's output
	if (!outputs.asset.empty())
		names.reserve(std::filesystem::path(outputs.asset).stem().string());

	ImageCache cache(images, sourcePath, contentRoot, relativeOutputDir, meshStem, names, log);

	// A re-import redirects the mesh's ONE recorded material sidecar (and its base
	// colour texture) onto the files that already exist, so the asset every scene
	// references is the one that gets refreshed. Only the MREF material can be
	// redirected — the others are named after their source material, which is
	// stable across re-imports and needs no redirect.
	const bool singleMaterial = materials.size() == 1;

	result.paths.resize(materials.size());
	std::vector<std::string> usedStems;
	for (size_t i = 0; i < materials.size(); ++i)
	{
		const PbrMaterialDesc& m = materials[i];
		const bool isPrimary = static_cast<int>(i) == primaryIndex;

		// Named after the source material so it survives a mesh rename and so two
		// meshes sharing a material converge on one asset. Unnamed materials fall
		// back to the historical "<meshStem>_mat" for the single-material case —
		// the name every existing import already wrote — and to an indexed variant
		// beyond that.
		std::string stem = sanitizeStem(m.name.c_str());
		if (stem.empty())
			stem = singleMaterial ? meshStem + "_mat"
			                      : meshStem + "_mat" + std::to_string(i);
		// Backward compatibility: before materials were named after the source, a
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
		// Two materials may carry the SAME name — a DCC tool exporting "Material"
		// twice, or two distinct names that sanitise to one. They would otherwise
		// resolve to one asset path, and the second import would overwrite the
		// first while both mesh slots point at whichever survived.
		if (std::find(usedStems.begin(), usedStems.end(), stem) != usedStems.end())
		{
			const std::string base = stem;
			for (int suffix = 2; ; ++suffix)
			{
				stem = base + "_" + std::to_string(suffix);
				if (std::find(usedStems.begin(), usedStems.end(), stem) == usedStems.end())
					break;
			}
			log.warn(sourcePath.filename().string() + ": two materials named '" + base
			         + "' — the second is written as '" + stem + "'");
		}
		usedStems.push_back(stem);
		stem = names.claim(stem, HE::AssetType::Material);

		result.paths[i] = importMaterial(
			m, cache, contentRoot, relativeOutputDir, stem,
			/*explicitPath=*/ (isPrimary && singleMaterial) ? outputs.material : std::string{},
			/*baseColorOverride=*/ (isPrimary && singleMaterial) ? outputs.texture : std::string{},
			isPrimary, log);

		if (isPrimary)
			result.primary = result.paths[i];
	}

	return result;
}

} // namespace Importer
