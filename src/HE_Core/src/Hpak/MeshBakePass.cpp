#include <Hpak/MeshBakePass.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <Hpak/MeshTextureBake.h>
#include <MaterialGraph/MaterialGraph.h>
#include <Types/Enums.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>

namespace HE::MeshBake
{

namespace
{

namespace fs = std::filesystem;

constexpr uint16_t kTypeMesh     = static_cast<uint16_t>(HE::AssetType::StaticMesh);
constexpr uint16_t kTypeMaterial = static_cast<uint16_t>(HE::AssetType::Material);
constexpr uint16_t kTypeTexture  = static_cast<uint16_t>(HE::AssetType::Texture);

// Ids of the derived assets are a pure function of the mesh's, so an unchanged mesh packs the
// same bytes under the same ids on the next export (the incremental cache relies on it).
HE::UUID derivedId(const HE::UUID& base, uint64_t k)
{
	HE::UUID id;
	id.hi = base.hi ^ (0x9E3779B97F4A7C15ull * (k + 1));
	id.lo = base.lo + 0x100000001B3ull * (k + 1);
	return id;
}

std::string hexOf(const HE::UUID& id)
{
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(id.hi),
	              static_cast<unsigned long long>(id.lo));
	return buf;
}

bool readFile(const fs::path& p, std::vector<uint8_t>& out)
{
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

// The scratch copy of the files the pass needs, and the engine's own loader over it.
class Scratch
{
public:
	explicit Scratch(const std::string& tag)
	{
		std::error_code ec;
		m_root = fs::temp_directory_path(ec) / ("he_meshbake_" + tag);
		fs::remove_all(m_root, ec);
		fs::create_directories(m_root, ec);
		m_cm = std::make_unique<ContentManager>(m_root.string());
	}
	~Scratch()
	{
		m_cm.reset();
		std::error_code ec;
		fs::remove_all(m_root, ec);
	}
	ContentManager& cm() { return *m_cm; }
	fs::path pathOf(const std::string& rel) const { return m_root / rel; }

	// Writes an entry's bytes once, under its addressed path.
	bool stage(const PassEntry& e)
	{
		if (!e.bytes) return false;
		if (m_staged.count(e.relPath)) return true;
		std::error_code ec;
		const fs::path p = pathOf(e.relPath);
		fs::create_directories(p.parent_path(), ec);
		std::ofstream f(p, std::ios::binary);
		if (!f) return false;
		f.write(reinterpret_cast<const char*>(e.bytes->data()), static_cast<std::streamsize>(e.bytes->size()));
		m_staged.insert(e.relPath);
		return f.good();
	}

private:
	fs::path                        m_root;
	std::unique_ptr<ContentManager> m_cm;
	std::set<std::string>           m_staged;
};

// A TextureAsset as the decoded RGBA8 level 0 the bake reads; false for anything else
// (a block-compressed or float texture, an array).
bool toImage(const TextureAsset& t, Image& out)
{
	if (t.layers > 1 || t.format != ::TextureFormat::RGBA8 || t.width == 0 || t.height == 0) return false;
	const size_t px = static_cast<size_t>(t.width) * t.height;
	if (t.channels != 1 && t.channels != 3 && t.channels != 4) return false;
	if (t.data.size() < px * t.channels) return false;
	out.width = t.width; out.height = t.height; out.srgb = t.srgb;
	out.rgba.resize(px * 4);
	for (size_t i = 0; i < px; ++i)
	{
		const uint8_t* s = &t.data[i * t.channels];
		uint8_t* d = &out.rgba[i * 4];
		if (t.channels == 1)      { d[0] = d[1] = d[2] = s[0]; d[3] = 255; }
		else if (t.channels == 3) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
		else                      { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; }
	}
	return true;
}

// ── The merged material's graph ───────────────────────────────────────────────
// The same wiring the glTF importer writes (PbrMaterialImport.cpp), so the renderers see
// nothing new: a UV node into every sample, ORM split into occlusion/roughness/metallic.
struct GraphBuilder
{
	MaterialGraph g;
	int out = 0;
	int uv  = -1;

	explicit GraphBuilder(bool lit)
	{
		out = g.addNode(MatNodeType::Output);
		MatGraphNode* o = g.findNode(out);
		o->p[0] = lit ? 1.0f : 0.0f;
		o->p[1] = static_cast<float>(MatBlendMode::Opaque);
	}
	int uvNode()
	{
		if (uv < 0)
		{
			uv = g.addNode(MatNodeType::UV);
			MatGraphNode* n = g.findNode(uv);
			n->p[0] = 1.0f; n->p[1] = 1.0f; n->p[2] = 0.0f; n->p[3] = 0.0f;
		}
		return uv;
	}
	int sample(const std::string& path)
	{
		const int n = g.addNode(MatNodeType::TextureSample);
		g.findNode(n)->s = path;
		g.connect(uvNode(), 0, n, 0);
		return n;
	}
	int split(int sampleNode)
	{
		const int n = g.addNode(MatNodeType::SplitRGBA);
		g.connect(sampleNode, 0, n, 0);
		return n;
	}
	int constFloat(float v)
	{
		const int n = g.addNode(MatNodeType::ConstFloat);
		g.findNode(n)->p[0] = v;
		return n;
	}
	int constColor(const float c[3])
	{
		const int n = g.addNode(MatNodeType::ConstColor);
		MatGraphNode* node = g.findNode(n);
		node->p[0] = c[0]; node->p[1] = c[1]; node->p[2] = c[2];
		return n;
	}
	int multiply(int a, int aPin, int b)
	{
		const int n = g.addNode(MatNodeType::Multiply);
		g.connect(a, aPin, n, 0);
		g.connect(b, 0, n, 1);
		return n;
	}
	void toOutput(int src, int srcPin, int pin) { g.connect(src, srcPin, out, pin); }
};

bool isOne3(const float c[3]) { return std::fabs(c[0] - 1) < 1e-6f && std::fabs(c[1] - 1) < 1e-6f && std::fabs(c[2] - 1) < 1e-6f; }

MaterialGraph buildMergedGraph(const SimpleTextureMaterial& m, const std::string& basePath,
                               const std::string& ormPath, const std::string& normalPath,
                               const std::string& emissivePath)
{
	GraphBuilder b(m.lit);
	// Base Color
	if (!m.baseColor.tex.empty())
	{
		const int s = b.sample(basePath);
		if (isOne3(m.baseColor.factor)) b.toOutput(s, 0, kMatOutputBaseColorPin);
		else b.toOutput(b.multiply(s, 0, b.constColor(m.baseColor.factor)), 0, kMatOutputBaseColorPin);
	}
	else
		b.toOutput(b.constColor(m.baseColor.factor), 0, kMatOutputBaseColorPin);

	// Metallic / Roughness / Occlusion: channels of the ORM atlas, or constants.
	if (!m.metallic.tex.empty())
	{
		const int split = b.split(b.sample(ormPath));
		const auto scaled = [&](int channel, float factor, int pin) {
			if (std::fabs(factor - 1.0f) < 1e-6f) b.toOutput(split, channel, pin);
			else b.toOutput(b.multiply(split, channel, b.constFloat(factor)), 0, pin);
		};
		scaled(1, m.roughness.factor, kMatOutputRoughnessPin);
		scaled(2, m.metallic.factor, kMatOutputMetallicPin);
		if (std::fabs(m.ao.factor - 1.0f) < 1e-6f) b.toOutput(split, 0, kMatOutputAOPin);
		else
		{
			// ao = lerp(1, sample, strength), the importer's form.
			const int lerp = b.g.addNode(MatNodeType::Lerp);
			const float one[3] = { 1.0f, 1.0f, 1.0f };
			b.g.connect(b.constColor(one), 0, lerp, 0);
			b.g.connect(split, 0, lerp, 1);
			b.g.connect(b.constFloat(m.ao.factor), 0, lerp, 2);
			b.toOutput(lerp, 0, kMatOutputAOPin);
		}
	}
	else
	{
		b.toOutput(b.constFloat(m.metallic.factor), 0, kMatOutputMetallicPin);
		b.toOutput(b.constFloat(m.roughness.factor), 0, kMatOutputRoughnessPin);
		if (std::fabs(m.ao.factor - 1.0f) > 1e-6f) b.toOutput(b.constFloat(m.ao.factor), 0, kMatOutputAOPin);
	}
	if (std::fabs(m.specular - 0.5f) > 1e-6f) b.toOutput(b.constFloat(m.specular), 0, kMatOutputSpecularPin);

	if (!m.normal.empty())
	{
		const int n = b.g.addNode(MatNodeType::NormalMapSample);
		b.g.findNode(n)->s = normalPath;
		b.g.findNode(n)->p[0] = m.normalStrength;
		b.g.connect(b.uvNode(), 0, n, 0);
		b.toOutput(n, 0, kMatOutputNormalPin);
	}
	if (!m.emissive.tex.empty())
	{
		const int s = b.sample(emissivePath);
		if (isOne3(m.emissive.factor)) b.toOutput(s, 0, kMatOutputEmissivePin);
		else b.toOutput(b.multiply(s, 0, b.constColor(m.emissive.factor)), 0, kMatOutputEmissivePin);
	}
	else if (m.emissive.factor[0] != 0.0f || m.emissive.factor[1] != 0.0f || m.emissive.factor[2] != 0.0f)
		b.toOutput(b.constColor(m.emissive.factor), 0, kMatOutputEmissivePin);
	return b.g;
}

TextureAsset makeTexture(const Image& im, const std::string& name, const std::string& path, const HE::UUID& id)
{
	TextureAsset t;
	t.type = HE::AssetType::Texture;
	t.name = name; t.path = path; t.id = id;
	t.width = im.width; t.height = im.height; t.channels = 4;
	t.mipLevels = 1; t.format = ::TextureFormat::RGBA8; t.srgb = im.srgb;
	t.data = im.rgba;
	return t;
}

} // namespace

PassResult runPass(const std::vector<PassEntry>& entries, const PassOptions& options)
{
	PassResult result;
	std::unordered_map<std::string, const PassEntry*> byPath;
	std::vector<const PassEntry*> meshes;
	for (const PassEntry& e : entries)
	{
		byPath[e.relPath] = &e;
		if (!e.metaPath.empty()) byPath.emplace(e.metaPath, &e);
		if (e.type == kTypeMesh && e.bytes) meshes.push_back(&e);
	}
	if (meshes.empty()) return result;

	Scratch scratch(hexOf(meshes.front()->id).substr(0, 16));
	ContentManager& cm = scratch.cm();

	for (const PassEntry* meshEntry : meshes)
	{
		const auto note = [&](const std::string& text) { result.notices.push_back(meshEntry->relPath + ": " + text); };
		if (!scratch.stage(*meshEntry)) continue;
		const HE::UUID meshId = cm.loadAsset(meshEntry->relPath);
		const StaticMeshAsset* loaded = cm.getStaticMesh(meshId);
		if (!loaded) continue;
		++result.meshesSeen;
		if (loaded->cooked) { ++result.skippedCooked; continue; }
		const StaticMeshAsset mesh = *loaded;          // the next loadAsset invalidates `loaded`
		if (!mesh.bake.mergeTextures && !mesh.bake.uvTexture && !mesh.bake.vertexColors)
		{ ++result.skippedSwitchedOff; continue; }
		const std::vector<MeshSection> sections = HE::meshSectionsOf(mesh);
		if (sections.size() < 2 || mesh.uvs.size() != mesh.vertices.size() / 3 * 2)
		{ ++result.skippedOneSection; continue; }

		// Every section's material, classified.
		MeshInput in;
		in.vertices = mesh.vertices; in.normals = mesh.normals; in.uvs = mesh.uvs; in.indices = mesh.indices;
		std::set<std::string> texturePaths;
		std::vector<std::string> sectionMaterialPaths;
		std::vector<std::string> reasons;
		for (const MeshSection& s : sections)
		{
			SectionInput si;
			si.indexOffset = s.indexOffset; si.indexCount = s.indexCount;
			const std::string matPath = s.materialPath.empty() ? mesh.materialPath : s.materialPath;
			sectionMaterialPaths.push_back(matPath);
			std::string why = "it has no material";
			const auto it = matPath.empty() ? byPath.end() : byPath.find(matPath);
			if (it != byPath.end() && it->second->type == kTypeMaterial && scratch.stage(*it->second))
			{
				const MaterialAsset* mat = cm.getMaterial(cm.loadAsset(it->second->relPath));
				if (mat)
				{
					si.simple = HE::classifySimpleTextureMaterial(*mat, si.material, &why);
					if (si.simple)
						for (const std::string& t : si.material.texturePaths()) texturePaths.insert(t);
				}
				else why = "its material cannot be loaded";
			}
			if (!si.simple) reasons.push_back(matPath.empty() ? why : (matPath + " " + why));
			in.sections.push_back(std::move(si));
		}

		// The textures those materials read, decoded.
		std::map<std::string, Image> images;
		for (const std::string& tp : texturePaths)
		{
			const auto it = byPath.find(tp);
			if (it == byPath.end() || it->second->type != kTypeTexture || !scratch.stage(*it->second)) continue;
			const TextureAsset* tex = cm.getTexture(cm.loadAsset(it->second->relPath));
			Image im;
			if (tex && toImage(*tex, im)) images[tp] = std::move(im);
			if (tex) cm.unloadAsset(tex->id);
		}
		const ImageLookup lookup = [&](const std::string& p) -> const Image* {
			const auto f = images.find(p);
			return f == images.end() ? nullptr : &f->second;
		};

		Options opt;
		opt.maxSize = options.maxAtlas;
		Result r;
		std::string failure;
		if (mesh.bake.mergeTextures)
		{
			opt.mode = Mode::Merge;
			r = bake(in, lookup, opt);
			if (!r.ok) failure = "merge: " + r.why;
		}
		if (!r.ok && mesh.bake.uvTexture)
		{
			opt.mode = Mode::Resample;
			r = bake(in, lookup, opt);
			if (!r.ok) failure += (failure.empty() ? "" : "; ") + std::string("resample: ") + r.why;
		}
		if (!r.ok)
		{
			if (failure.empty()) continue;      // only vertex colours were on: a later step's business
			++result.declined;
			note("not baked (" + failure + ")");
			continue;
		}

		// ── The derived assets ───────────────────────────────────────────────────
		const std::string stem = "Baked/" + hexOf(mesh.id);
		const std::string basePath = stem + "_base.hasset", ormPath = stem + "_orm.hasset",
		                  normalPath = stem + "_normal.hasset", emissivePath = stem + "_emissive.hasset",
		                  matRel = stem + "_material.hasset";
		const auto emitTexture = [&](const Group& grp, const std::string& rel, uint64_t k, const char* what) -> bool
		{
			if (!grp.atlas) return true;
			TextureAsset t = makeTexture(grp.image, std::string(what), rel, derivedId(mesh.id, k));
			const fs::path file = scratch.pathOf(rel);
			if (!cm.writeAssetTo(t, file.string())) return false;
			NewAsset na; na.id = t.id; na.relPath = rel; na.type = kTypeTexture;
			if (!readFile(file, na.bytes)) return false;
			result.added.push_back(std::move(na));
			return true;
		};
		bool wrote = emitTexture(r.base, basePath, 1, "Baked Base") && emitTexture(r.orm, ormPath, 2, "Baked ORM")
		          && emitTexture(r.normal, normalPath, 3, "Baked Normal") && emitTexture(r.emissive, emissivePath, 4, "Baked Emissive");

		// Material: the merged graph, regenerated through the loader so the GLSL, the GBuffer
		// variant and the GI approximation are exactly what the editor would have saved.
		MaterialAsset mat;
		if (wrote)
		{
			const MaterialGraph graph = buildMergedGraph(r.material, basePath, ormPath, normalPath, emissivePath);
			const HE::MatShaderGen gen = HE::generateFragment(graph);
			mat.type = HE::AssetType::Material;
			mat.name = "Baked Material"; mat.path = matRel; mat.id = derivedId(mesh.id, 0);
			if (r.base.atlas) mat.texturePaths.push_back(basePath);
			mat.nodeGraphJson = HE::materialGraphToJson(graph);
			mat.customShaderFragGlsl = gen.glsl;
			mat.customShaderGBufGlsl = gen.glslGBuffer;
			mat.customShaderVertGlsl = gen.vertexBody;
			mat.customShaderDispGlsl = gen.displacementBody;
			mat.blendMode = gen.blendMode;
			mat.graphTexturePaths = gen.textures;
			for (int k = 0; k < 3; ++k) mat.baseColor[k] = r.material.baseColor.factor[k];
			mat.metallic = r.material.metallic.factor;
			mat.roughness = r.material.roughness.factor;
			mat.doubleSided = r.material.doubleSided;
			const fs::path file = scratch.pathOf(matRel);
			wrote = cm.writeAssetTo(mat, file.string());
			if (wrote)
			{
				// Load = regenerate from the graph; save that copy so the tail carries it all.
				const MaterialAsset* back = cm.getMaterial(cm.loadAsset(matRel));
				if (back) { MaterialAsset copy = *back; wrote = cm.writeAssetTo(copy, file.string()); }
				NewAsset na; na.id = mat.id; na.relPath = matRel; na.type = kTypeMaterial;
				wrote = wrote && readFile(file, na.bytes);
				if (wrote) result.added.push_back(std::move(na));
			}
		}

		// The mesh: the merged section first, the sections the bake left alone behind it.
		StaticMeshAsset out = mesh;
		out.cooked = false; out.interleaved.clear(); out.vertexCount = 0;
		out.vertices = r.vertices; out.normals = r.normals; out.uvs = r.uvs; out.indices = r.indices;
		out.sections.clear();
		MeshSection merged;
		merged.indexOffset = 0; merged.indexCount = r.mergedIndexCount; merged.materialPath = matRel;
		out.sections.push_back(merged);
		for (const Result::Kept& k : r.keptSections)
		{
			MeshSection s;
			s.indexOffset = k.indexOffset; s.indexCount = k.indexCount;
			s.materialPath = sectionMaterialPaths[k.inputSection];
			out.sections.push_back(s);
		}
		out.materialPath = matRel; out.materialId = {};
		for (int c = 0; c < 3; ++c) { out.boundsMin[c] = 0; out.boundsMax[c] = 0; }

		ReplacedMesh rm; rm.id = mesh.id;
		const fs::path meshFile = scratch.pathOf(meshEntry->relPath);
		if (wrote && cm.writeAssetTo(out, meshFile.string()) && readFile(meshFile, rm.bytes))
		{
			result.replaced.push_back(std::move(rm));
			++result.baked;
			note(std::string("baked ") + (r.lossless ? "lossless" : "resampled") + ", "
			     + std::to_string(r.mergedSections) + " sections into one"
			     + (r.keptSections.empty() ? std::string() : (", " + std::to_string(r.keptSections.size()) + " kept"))
			     + ", atlas " + std::to_string(r.base.atlas ? r.base.image.width : r.orm.image.width) + "px");
		}
		else
		{
			++result.declined;
			note("not baked (could not write the derived assets)");
			// Drop what this mesh already added so a half-baked mesh never ships.
			const HE::UUID first = derivedId(mesh.id, 0);
			(void)first;
			result.added.erase(std::remove_if(result.added.begin(), result.added.end(),
				[&](const NewAsset& a) { return a.relPath.rfind(stem, 0) == 0; }), result.added.end());
		}
		if (!reasons.empty() && r.keptSections.size() > 0)
		{
			std::string joined;
			for (const std::string& s : reasons) joined += (joined.empty() ? "" : "; ") + s;
			note("kept as they are: " + joined);
		}
	}
	result.notices.insert(result.notices.begin(),
		"summary: " + std::to_string(result.meshesSeen) + " mesh(es) looked at, "
		+ std::to_string(result.baked) + " baked, " + std::to_string(result.declined) + " declined, "
		+ std::to_string(result.skippedOneSection) + " with one material slot, "
		+ std::to_string(result.skippedSwitchedOff) + " switched off, "
		+ std::to_string(result.skippedCooked) + " already cooked");
	return result;
}

} // namespace HE::MeshBake
